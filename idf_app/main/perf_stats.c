#include "perf_stats.h"

#if CONFIG_RK_PERF_LOG

#include <stdio.h>
#include "esp_attr.h"
#include "esp_log.h"
#include "esp_pm.h"
#include "esp_timer.h"

static const char *TAG = "perf";

static const char *const NAMES[PERF_COUNTER_COUNT] = {
    "ui_loop", "encoder_timer", "lvgl_tick", "touch_read",
    "notify_post", "notify_timer", "notify_input", "notify_other", "encoder_isr",
    "invalidate", "render", "flush",
};
static const char *const ACC_NAMES[PERF_ACC_COUNT] = {
    "invalid_px", "flush_px", "render_ms", "flush_ms",
};

static volatile uint32_t s_counts[PERF_COUNTER_COUNT];
static volatile uint32_t s_accs[PERF_ACC_COUNT];   // wraps at 4e9; a 30 s window stays far below
static uint32_t s_at_begin[PERF_COUNTER_COUNT];
static uint32_t s_acc_at_begin[PERF_ACC_COUNT];
static int64_t s_begin_us;
static volatile bool s_in_session;

static uint32_t s_win_counts[PERF_COUNTER_COUNT];
static uint32_t s_win_accs[PERF_ACC_COUNT];
static int64_t s_win_start_us;
#define PERF_WINDOW_US (30LL * 1000000LL)

// Safe from interrupt handlers and from any task: a relaxed atomic increment into DRAM.
void IRAM_ATTR perf_count(perf_counter_t counter) {
    if (counter < PERF_COUNTER_COUNT) {
        __atomic_fetch_add(&s_counts[counter], 1, __ATOMIC_RELAXED);
    }
}

void IRAM_ATTR perf_add(perf_acc_t acc, uint32_t value) {
    if (acc < PERF_ACC_COUNT) {
        __atomic_fetch_add(&s_accs[acc], value, __ATOMIC_RELAXED);
    }
}

static int format_window(char *line, size_t cap, const uint32_t *counts0, const uint32_t *accs0, double secs) {
    int n = 0;
    for (int i = 0; i < PERF_COUNTER_COUNT && n < (int)cap; i++) {
        uint32_t d = s_counts[i] - counts0[i];
        n += snprintf(line + n, cap - n, " %s=%lu(%.1f/s)", NAMES[i], (unsigned long)d, d / secs);
    }
    for (int i = 0; i < PERF_ACC_COUNT && n < (int)cap; i++) {
        uint32_t d = s_accs[i] - accs0[i];
        // render/flush time is kept in microseconds and reported in milliseconds
        double v = (i == PERF_ACC_RENDER_US || i == PERF_ACC_FLUSH_US) ? d / 1000.0 : (double)d;
        n += snprintf(line + n, cap - n, " %s=%.0f", ACC_NAMES[i], v);
    }
    return n;
}

void perf_periodic(const char *state_name) {
    int64_t now = esp_timer_get_time();
    if (s_win_start_us == 0) {
        s_win_start_us = now;
        for (int i = 0; i < PERF_COUNTER_COUNT; i++) s_win_counts[i] = s_counts[i];
        for (int i = 0; i < PERF_ACC_COUNT; i++) s_win_accs[i] = s_accs[i];
        return;
    }
    if (now - s_win_start_us < PERF_WINDOW_US) return;
    double secs = (double)(now - s_win_start_us) / 1e6;
    char line[520];
    int n = snprintf(line, sizeof(line), "%.0fs window, display=%s:", secs, state_name ? state_name : "?");
    format_window(line + n, sizeof(line) - n, s_win_counts, s_win_accs, secs);
    ESP_LOGI(TAG, "%s", line);
    s_win_start_us = now;
    for (int i = 0; i < PERF_COUNTER_COUNT; i++) s_win_counts[i] = s_counts[i];
    for (int i = 0; i < PERF_ACC_COUNT; i++) s_win_accs[i] = s_accs[i];
}

static void dump_pm(const char *when) {
#if CONFIG_PM_ENABLE
    printf("[perf] --- power-manager statistics %s ---\n", when);
    esp_pm_dump_locks(stdout);
    printf("[perf] --- end ---\n");
#else
    (void)when;
#endif
}

void perf_sleep_begin(void) {
    if (s_in_session) return;
    for (int i = 0; i < PERF_COUNTER_COUNT; i++) s_at_begin[i] = s_counts[i];
    for (int i = 0; i < PERF_ACC_COUNT; i++) s_acc_at_begin[i] = s_accs[i];
    s_begin_us = esp_timer_get_time();
    s_in_session = true;
    ESP_LOGI(TAG, "sleep session begins");
    dump_pm("at sleep start");
}

void perf_sleep_end(const char *cause) {
    if (!s_in_session) return;
    s_in_session = false;
    double secs = (double)(esp_timer_get_time() - s_begin_us) / 1e6;
    if (secs < 0.001) secs = 0.001;
    char line[520];
    int n = snprintf(line, sizeof(line), "sleep session %.1fs (woken by %s):", secs, cause ? cause : "?");
    format_window(line + n, sizeof(line) - n, s_at_begin, s_acc_at_begin, secs);
    ESP_LOGI(TAG, "%s", line);
    dump_pm("at wake");
}

#endif  // CONFIG_RK_PERF_LOG
