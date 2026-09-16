#include "haptic_driver.h"

#include "i2c_bsp.h"
#include "platform/platform_storage.h"

#include <driver/gpio.h>
#include <esp_log.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

static const char *TAG = "haptic";

// DRV2605 register map (TI datasheet; matches the widely-used Adafruit
// DRV2605 library's register addresses, which this was cross-checked
// against rather than derived from scratch).
#define DRV2605_REG_STATUS   0x00
#define DRV2605_REG_MODE     0x01
#define DRV2605_REG_LIBRARY  0x03
#define DRV2605_REG_WAVESEQ1 0x04
#define DRV2605_REG_WAVESEQ2 0x05
#define DRV2605_REG_GO       0x0C
#define DRV2605_REG_CONTROL3 0x1D

#define DRV2605_MODE_INTERNAL_TRIGGER 0x00  // STANDBY=0, MODE=Internal Trigger
#define DRV2605_LIBRARY_ERM_A 0x01
#define DRV2605_CONTROL3_ERM_OPEN_LOOP_BIT 0x20  // bit5

#define DRV2605_DEFAULT_EFFECT_ID 0x01  // Strong Click - 100%

// See haptic_driver.h's diagnostic-section comment.
#define DRV2605_REG_FEEDBACK 0x1A
#define DRV2605_FEEDBACK_LRA_BIT 0x80  // bit7: 1 = LRA, 0 = ERM
#define DRV2605_MODE_DIAGNOSTICS 0x06
#define DRV2605_LIBRARY_LRA 0x06
#define HAPTIC_EN_GPIO GPIO_NUM_38  // confirmed NOT a real enable pin on this
                                    // board - see haptic_driver.h

// Auto-calibration (see haptic_driver.h and haptic_driver_run_calibration()).
#define DRV2605_REG_CAL_COMP 0x18
#define DRV2605_REG_CAL_BEMF 0x19
#define DRV2605_REG_CONTROL4 0x1E
#define DRV2605_MODE_AUTO_CALIBRATION 0x07
#define DRV2605_CONTROL4_CAL_TIME_MASK 0x30
#define DRV2605_CONTROL4_CAL_TIME_LONGEST 0x30  // chip's own default for an
                                                 // unknown actuator
#define DRV2605_STATUS_DIAG_RESULT_BIT 0x08  // set = calibration/diagnostic
                                              // saw an open or shorted
                                              // actuator, i.e. failed
#define DRV2605_FEEDBACK_CAL_SEED 0x36  // datasheet's own starting brake
                                         // factor/loop gain for an unknown
                                         // actuator, ORed onto the existing
                                         // actuator-type bit (bit7)

#define HAPTIC_DIAG_EN_LOW  200
#define HAPTIC_DIAG_EN_HIGH 201
#define HAPTIC_DIAG_LRA     202

static const haptic_diagnostic_option_t s_diagnostic_options[] = {
    {HAPTIC_DIAG_EN_LOW,  "DIAGNOSTIC: GPIO38 LOW + Strong Click (baseline)"},
    {HAPTIC_DIAG_EN_HIGH, "DIAGNOSTIC: GPIO38 HIGH + Strong Click"},
    {HAPTIC_DIAG_LRA,     "DIAGNOSTIC: GPIO38 HIGH + LRA mode + Strong Click"},
};

const haptic_diagnostic_option_t *haptic_driver_get_diagnostic_options(size_t *count) {
    if (count) {
        *count = sizeof(s_diagnostic_options) / sizeof(s_diagnostic_options[0]);
    }
    return s_diagnostic_options;
}

// See haptic_driver.h's comment on why the list stops at 14.
static const haptic_effect_option_t s_effect_options[] = {
    {1,  "Strong Click - 100%"},
    {4,  "Sharp Click - 100%"},
    {7,  "Soft Bump - 100%"},
    {10, "Double Click - 100%"},
    {12, "Triple Click - 100%"},
    {14, "Strong Buzz - 100%"},
};

const haptic_effect_option_t *haptic_driver_get_effect_options(size_t *count) {
    if (count) {
        *count = sizeof(s_effect_options) / sizeof(s_effect_options[0]);
    }
    return s_effect_options;
}

static bool effect_id_is_valid(uint8_t effect_id) {
    size_t count = sizeof(s_effect_options) / sizeof(s_effect_options[0]);
    for (size_t i = 0; i < count; i++) {
        if (s_effect_options[i].effect_id == effect_id) {
            return true;
        }
    }
    return false;
}

static bool s_chip_ready = false;
static bool s_enabled = false;
static bool s_calibrated = false;
static uint8_t s_effect_id = DRV2605_DEFAULT_EFFECT_ID;

static bool write_reg(uint8_t reg, uint8_t value) {
    return i2c_write_buff(drv2605_dev_handle, reg, &value, 1) == 0;
}

static bool read_reg(uint8_t reg, uint8_t *out) {
    return i2c_read_buff(drv2605_dev_handle, reg, out, 1) == 0;
}

void haptic_driver_init(void) {
    rk_haptic_cfg_t cfg;
    bool have_cfg = platform_storage_read_haptic(&cfg);
    if (have_cfg) {
        s_enabled = cfg.enabled != 0;
        s_effect_id = effect_id_is_valid(cfg.effect_id) ? cfg.effect_id
                                                        : DRV2605_DEFAULT_EFFECT_ID;
    }

    uint8_t status = 0;
    if (!read_reg(DRV2605_REG_STATUS, &status)) {
        ESP_LOGW(TAG, "DRV2605 not responding on I2C - haptic feedback unavailable");
        s_chip_ready = false;
        return;
    }

    // Exit standby, select Internal Trigger mode (single register covers
    // both - see the MODE bit layout above).
    if (!write_reg(DRV2605_REG_MODE, DRV2605_MODE_INTERNAL_TRIGGER)) {
        ESP_LOGW(TAG, "DRV2605 MODE write failed - haptic feedback unavailable");
        return;
    }

    // ERM confirmed for this board (see haptic_driver.h) - always select
    // that library regardless of calibration state.
    if (!write_reg(DRV2605_REG_LIBRARY, DRV2605_LIBRARY_ERM_A)) {
        ESP_LOGW(TAG, "DRV2605 LIBRARY write failed - haptic feedback unavailable");
        return;
    }

    if (have_cfg && cfg.calibrated) {
        // Closed-loop: write back exactly what a passed calibration left
        // behind (see haptic_driver_run_calibration()) rather than
        // re-running it - calibration is felt, a boot should stay quiet.
        write_reg(DRV2605_REG_FEEDBACK, cfg.cal_feedback);
        write_reg(DRV2605_REG_CAL_COMP, cfg.cal_compensation);
        write_reg(DRV2605_REG_CAL_BEMF, cfg.cal_back_emf);
        s_calibrated = true;
    } else {
        // Open-loop fallback (no calibration data yet). Read-modify-write
        // so unrelated CONTROL3 bits are left at their power-on-reset
        // values rather than guessed at.
        uint8_t control3 = 0;
        if (!read_reg(DRV2605_REG_CONTROL3, &control3) ||
            !write_reg(DRV2605_REG_CONTROL3, control3 | DRV2605_CONTROL3_ERM_OPEN_LOOP_BIT)) {
            ESP_LOGW(TAG, "DRV2605 CONTROL3 write failed - haptic feedback unavailable");
            return;
        }
    }

    s_chip_ready = true;
    ESP_LOGI(TAG, "DRV2605 initialized (%s ERM), enabled=%d effect_id=%d",
             s_calibrated ? "closed-loop, calibrated" : "open-loop, no calibration",
             s_enabled, s_effect_id);
}

bool haptic_driver_is_enabled(void) {
    return s_enabled;
}

bool haptic_driver_set_enabled(bool enabled) {
    s_enabled = enabled;
    // Read-modify-write so this doesn't clobber a previously-selected
    // effect back to the default - rk_haptic_cfg_set_defaults() only
    // fills in a fallback for a config that's never been saved before.
    rk_haptic_cfg_t cfg;
    if (!platform_storage_read_haptic(&cfg)) {
        rk_haptic_cfg_set_defaults(&cfg);
    }
    cfg.enabled = enabled ? 1 : 0;
    cfg.effect_id = s_effect_id;
    bool ok = platform_storage_write_haptic(&cfg);
    if (!ok) {
        ESP_LOGW(TAG, "Failed to persist haptic enabled setting");
    }
    return ok;
}

uint8_t haptic_driver_get_effect(void) {
    return s_effect_id;
}

bool haptic_driver_set_effect(uint8_t effect_id) {
    if (!effect_id_is_valid(effect_id)) {
        ESP_LOGW(TAG, "Ignoring unknown haptic effect id %d", effect_id);
        return false;
    }
    s_effect_id = effect_id;
    rk_haptic_cfg_t cfg;
    if (!platform_storage_read_haptic(&cfg)) {
        rk_haptic_cfg_set_defaults(&cfg);
    }
    cfg.enabled = s_enabled ? 1 : 0;
    cfg.effect_id = effect_id;
    bool ok = platform_storage_write_haptic(&cfg);
    if (!ok) {
        ESP_LOGW(TAG, "Failed to persist haptic effect setting");
    }
    return ok;
}

void haptic_driver_pulse(void) {
    if (!s_chip_ready || !s_enabled) {
        return;
    }

    // Drop rather than queue if a previous pulse is still playing (GO
    // hasn't self-cleared yet) - see haptic_driver.h's header comment on
    // why that's an acceptable simplification for how infrequently this
    // fires (touch/long-press, not every encoder tick).
    uint8_t go = 0;
    if (!read_reg(DRV2605_REG_GO, &go) || go != 0) {
        return;
    }

    if (!write_reg(DRV2605_REG_WAVESEQ1, s_effect_id) ||
        !write_reg(DRV2605_REG_WAVESEQ2, 0) ||  // Terminates the sequence after slot 1
        !write_reg(DRV2605_REG_GO, 0x01)) {
        ESP_LOGW(TAG, "Haptic pulse trigger failed");
    }
}

static bool s_en_gpio_configured = false;

static void ensure_en_gpio_configured(void) {
    if (s_en_gpio_configured) {
        return;
    }
    gpio_config_t cfg = {
        .pin_bit_mask = 1ULL << HAPTIC_EN_GPIO,
        .mode = GPIO_MODE_OUTPUT,
    };
    gpio_config(&cfg);
    s_en_gpio_configured = true;
}

// Runs the DRV2605's own built-in actuator diagnostic and logs the result -
// bit3 of STATUS set means the chip's diagnostic saw an open or shorted
// actuator (i.e. nothing usable connected, from its point of view).
static void log_actuator_diagnostic(const char *label) {
    write_reg(DRV2605_REG_MODE, DRV2605_MODE_DIAGNOSTICS);
    write_reg(DRV2605_REG_GO, 0x01);
    uint8_t go = 1;
    for (int i = 0; i < 50 && go != 0; i++) {
        vTaskDelay(pdMS_TO_TICKS(20));
        if (!read_reg(DRV2605_REG_GO, &go)) {
            break;
        }
    }
    uint8_t status = 0;
    read_reg(DRV2605_REG_STATUS, &status);
    ESP_LOGI(TAG, "%s: actuator diagnostic status=0x%02x (%s)", label, status,
             (status & 0x08) ? "OPEN/SHORTED - chip sees nothing usable connected"
                              : "connected");
    write_reg(DRV2605_REG_MODE, DRV2605_MODE_INTERNAL_TRIGGER);
}

void haptic_driver_run_diagnostic(uint8_t test_id) {
    if (!s_chip_ready) {
        ESP_LOGW(TAG, "Haptic diagnostic requested but DRV2605 never initialized");
        return;
    }
    ensure_en_gpio_configured();

    switch (test_id) {
    case HAPTIC_DIAG_EN_LOW:
        gpio_set_level(HAPTIC_EN_GPIO, 0);
        vTaskDelay(pdMS_TO_TICKS(2));
        log_actuator_diagnostic("Diagnostic (GPIO38 LOW, ERM as configured)");
        break;
    case HAPTIC_DIAG_EN_HIGH:
        gpio_set_level(HAPTIC_EN_GPIO, 1);
        vTaskDelay(pdMS_TO_TICKS(2));
        log_actuator_diagnostic("Diagnostic (GPIO38 HIGH, ERM as configured)");
        break;
    case HAPTIC_DIAG_LRA: {
        gpio_set_level(HAPTIC_EN_GPIO, 1);
        vTaskDelay(pdMS_TO_TICKS(2));
        uint8_t feedback = 0;
        read_reg(DRV2605_REG_FEEDBACK, &feedback);
        write_reg(DRV2605_REG_FEEDBACK, feedback | DRV2605_FEEDBACK_LRA_BIT);
        write_reg(DRV2605_REG_LIBRARY, DRV2605_LIBRARY_LRA);
        log_actuator_diagnostic("Diagnostic (GPIO38 HIGH, LRA mode)");
        break;
    }
    default:
        ESP_LOGW(TAG, "Unknown haptic diagnostic test id %d", test_id);
        return;
    }

    // Effect id 1 is "Strong Click" in every DRV2605 library, ERM or LRA -
    // felt confirmation to go with the logged diagnostic above.
    write_reg(DRV2605_REG_WAVESEQ1, 1);
    write_reg(DRV2605_REG_WAVESEQ2, 0);
    write_reg(DRV2605_REG_GO, 0x01);
}

bool haptic_driver_is_calibrated(void) {
    return s_calibrated;
}

void haptic_driver_run_calibration(void) {
    if (!s_chip_ready) {
        ESP_LOGW(TAG, "Calibration requested but DRV2605 never initialized");
        return;
    }

    // Datasheet's own starting point for an unknown actuator: brake factor
    // and loop gain in the middle of their ranges, ORed onto the existing
    // actuator-type bit (bit7 - left alone; ERM is confirmed for this board,
    // see haptic_driver.h, so this should already read 0).
    uint8_t feedback = 0;
    read_reg(DRV2605_REG_FEEDBACK, &feedback);
    write_reg(DRV2605_REG_FEEDBACK, (feedback & 0x80) | DRV2605_FEEDBACK_CAL_SEED);

    uint8_t control4 = 0;
    read_reg(DRV2605_REG_CONTROL4, &control4);
    write_reg(DRV2605_REG_CONTROL4,
              (control4 & ~DRV2605_CONTROL4_CAL_TIME_MASK) | DRV2605_CONTROL4_CAL_TIME_LONGEST);

    write_reg(DRV2605_REG_MODE, DRV2605_MODE_AUTO_CALIBRATION);
    write_reg(DRV2605_REG_GO, 0x01);

    // The chip's own longest calibration window is up to ~1.2s - poll
    // generously past that rather than assume.
    uint8_t go = 1;
    for (int i = 0; i < 100 && go != 0; i++) {
        vTaskDelay(pdMS_TO_TICKS(20));
        if (!read_reg(DRV2605_REG_GO, &go)) {
            break;
        }
    }

    uint8_t status = 0;
    read_reg(DRV2605_REG_STATUS, &status);
    bool passed = (status & DRV2605_STATUS_DIAG_RESULT_BIT) == 0;

    uint8_t comp = 0, back_emf = 0, result_feedback = 0;
    read_reg(DRV2605_REG_CAL_COMP, &comp);
    read_reg(DRV2605_REG_CAL_BEMF, &back_emf);
    read_reg(DRV2605_REG_FEEDBACK, &result_feedback);

    write_reg(DRV2605_REG_MODE, DRV2605_MODE_INTERNAL_TRIGGER);

    ESP_LOGI(TAG,
             "Auto-calibration %s: status=0x%02x feedback=0x%02x compensation=0x%02x back_emf=0x%02x",
             passed ? "PASSED" : "FAILED", status, result_feedback, comp, back_emf);

    if (!passed) {
        ESP_LOGW(TAG, "Calibration failed - leaving existing open-loop configuration in place");
        return;
    }

    rk_haptic_cfg_t cfg;
    if (!platform_storage_read_haptic(&cfg)) {
        rk_haptic_cfg_set_defaults(&cfg);
    }
    cfg.enabled = s_enabled ? 1 : 0;
    cfg.effect_id = s_effect_id;
    cfg.calibrated = 1;
    cfg.cal_feedback = result_feedback;
    cfg.cal_compensation = comp;
    cfg.cal_back_emf = back_emf;
    if (!platform_storage_write_haptic(&cfg)) {
        ESP_LOGW(TAG, "Failed to persist calibration result - staying open-loop until retried");
        return;
    }

    // Apply immediately rather than waiting for the next boot: turn off the
    // open-loop bit now that real closed-loop compensation data exists.
    uint8_t control3 = 0;
    read_reg(DRV2605_REG_CONTROL3, &control3);
    write_reg(DRV2605_REG_CONTROL3, control3 & ~DRV2605_CONTROL3_ERM_OPEN_LOOP_BIT);
    s_calibrated = true;

    ESP_LOGI(TAG, "Calibration saved - now running closed-loop");
}
