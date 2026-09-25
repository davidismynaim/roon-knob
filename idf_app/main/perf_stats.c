#include "perf_stats.h"

#if CONFIG_RK_PERF_LOG

#include <stdio.h>
#include "esp_attr.h"
#include "esp_log.h"
#include "esp_pm.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "perf";

static const char *const NAMES[PERF_COUNTER_COUNT] = {
    "ui_loop", "encoder_timer", "lvgl_tick", "touch_read",
    "notify_post", "notify_timer", "notify_input", "notify_other", "encoder_isr",
    "invalidate", "render", "flush", "wait_signalled",
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
static bool s_mid_dumped;
#define PERF_MID_SLEEP_US (90LL * 1000000LL)

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


#if CONFIG_PM_LIGHT_SLEEP_CALLBACKS
// ESP-IDF calls this each time it considers light sleep on a core, BEFORE it checks whether the
// gap is long enough (>= 30 ms here), passing the gap it calculated. The histogram therefore shows
// which core keeps finding gaps that are too short, and how short.
#define GAP_BUCKETS 7
static const char *const GAP_LABELS[GAP_BUCKETS] = {"<5ms", "5-10", "10-20", "20-30", "30-50", "50-100", ">=100"};
static volatile uint32_t s_gap_hist[2][GAP_BUCKETS];
static bool s_gap_cb_registered;

static esp_err_t IRAM_ATTR perf_light_sleep_enter_cb(int64_t sleep_time_us, void *arg) {
    (void)arg;
    int core = xPortGetCoreID() & 1;
    int64_t ms = sleep_time_us / 1000;
    int b = ms < 5 ? 0 : ms < 10 ? 1 : ms < 20 ? 2 : ms < 30 ? 3 : ms < 50 ? 4 : ms < 100 ? 5 : 6;
    s_gap_hist[core][b]++;
    return ESP_OK;
}

static void register_gap_cb(void) {
    if (s_gap_cb_registered) return;
    esp_pm_sleep_cbs_register_config_t cfg = {
        .enter_cb = perf_light_sleep_enter_cb,
        .enter_cb_user_arg = NULL,
        .exit_cb = NULL,
        .exit_cb_user_arg = NULL,
        .enter_cb_prior = 0,
        .exit_cb_prior = 0,
    };
    if (esp_pm_light_sleep_register_cbs(&cfg) == ESP_OK) s_gap_cb_registered = true;
}

static void dump_gap_hist(const char *when) {
    printf("[perf] --- light-sleep gap seen by ESP-IDF per core, %s (cumulative) ---\n", when);
    for (int core = 0; core < 2; core++) {
        printf("core%d:", core);
        for (int b = 0; b < GAP_BUCKETS; b++) printf(" %s=%lu", GAP_LABELS[b], (unsigned long)s_gap_hist[core][b]);
        printf("\n");
    }
    printf("[perf] --- end ---\n");
}
#else
static void register_gap_cb(void) {}
static void dump_gap_hist(const char *when) { (void)when; }
#endif

static void dump_pm(const char *when) {
#if CONFIG_PM_ENABLE
    printf("[perf] --- power-manager statistics %s ---\n", when);
    esp_pm_dump_locks(stdout);
    printf("[perf] --- end ---\n");
#else
    (void)when;
#endif
    dump_gap_hist(when);
#if CONFIG_FREERTOS_GENERATE_RUN_TIME_STATS && CONFIG_FREERTOS_USE_TRACE_FACILITY
    static char stats[2048];
    printf("[perf] --- task CPU time %s (cumulative since boot) ---\n", when);
    vTaskGetRunTimeStats(stats);
    printf("%s\n[perf] --- end ---\n", stats);
#endif
#if CONFIG_ESP_TIMER_PROFILING
    printf("[perf] --- esp_timer callbacks %s (cumulative) ---\n", when);
    esp_timer_dump(stdout);
    printf("[perf] --- end ---\n");
#endif
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
    // 90 s into a sleep, print the same statistics as at wake, so a session can be measured
    // without waking the dial (the tap that ends a session also changes what is being measured).
    if (s_in_session && !s_mid_dumped && now - s_begin_us >= PERF_MID_SLEEP_US) {
        s_mid_dumped = true;
        double secs = (double)(now - s_begin_us) / 1e6;
        char line[520];
        int n = snprintf(line, sizeof(line), "sleep snapshot %.1fs (still asleep):", secs);
        format_window(line + n, sizeof(line) - n, s_at_begin, s_acc_at_begin, secs);
        ESP_LOGI(TAG, "%s", line);
        dump_pm("mid-sleep snapshot");
    }
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


void perf_sleep_begin(void) {
    if (s_in_session) return;
    register_gap_cb();
    for (int i = 0; i < PERF_COUNTER_COUNT; i++) s_at_begin[i] = s_counts[i];
    for (int i = 0; i < PERF_ACC_COUNT; i++) s_acc_at_begin[i] = s_accs[i];
    s_begin_us = esp_timer_get_time();
    s_in_session = true;
    s_mid_dumped = false;
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
