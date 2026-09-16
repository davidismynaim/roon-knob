#include "haptic_driver.h"

#include "i2c_bsp.h"
#include "platform/platform_storage.h"

#include <esp_log.h>

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
static uint8_t s_effect_id = DRV2605_DEFAULT_EFFECT_ID;

static bool write_reg(uint8_t reg, uint8_t value) {
    return i2c_write_buff(drv2605_dev_handle, reg, &value, 1) == 0;
}

static bool read_reg(uint8_t reg, uint8_t *out) {
    return i2c_read_buff(drv2605_dev_handle, reg, out, 1) == 0;
}

void haptic_driver_init(void) {
    rk_haptic_cfg_t cfg;
    if (platform_storage_read_haptic(&cfg)) {
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

    // Open-loop ERM (no auto-calibration in this pass - see haptic_driver.h -
    // closed-loop relies on calibrated back-EMF feedback parameters we're
    // not measuring). Read-modify-write so unrelated CONTROL3 bits are
    // left at their power-on-reset values rather than guessed at.
    uint8_t control3 = 0;
    if (!read_reg(DRV2605_REG_CONTROL3, &control3) ||
        !write_reg(DRV2605_REG_CONTROL3, control3 | DRV2605_CONTROL3_ERM_OPEN_LOOP_BIT)) {
        ESP_LOGW(TAG, "DRV2605 CONTROL3 write failed - haptic feedback unavailable");
        return;
    }

    if (!write_reg(DRV2605_REG_LIBRARY, DRV2605_LIBRARY_ERM_A)) {
        ESP_LOGW(TAG, "DRV2605 LIBRARY write failed - haptic feedback unavailable");
        return;
    }

    s_chip_ready = true;
    ESP_LOGI(TAG, "DRV2605 initialized (open-loop ERM, no calibration), enabled=%d effect_id=%d",
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
