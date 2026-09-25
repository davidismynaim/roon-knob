#pragma once

// Wake-up accounting for the display-sleep power work (issue #45).
//
// While the screen is off, every periodic timer and every task that wakes on a schedule stops the
// CPU from staying in light sleep. These counters say how often each one fired during a sleep
// session, and perf_sleep_end() prints them as rates together with ESP-IDF's power-manager
// statistics (time spent in each power mode, light-sleep entries and rejections). Nothing here
// exists in a normal build: it is compiled in only with CONFIG_RK_PERF_LOG, so production firmware
// carries no counters, no logging, and no PM profiling overhead.

#include <stdint.h>
#include "sdkconfig.h"

typedef enum {
    PERF_UI_LOOP,            // one pass of the UI loop
    PERF_ENCODER_TIMER,      // the 3 ms encoder polling timer
    PERF_LVGL_TICK,          // the periodic LVGL tick timer (only present in builds that still have it)
    PERF_TOUCH_READ,         // one LVGL touch-controller read (I2C)
    PERF_UI_NOTIFY_POST,     // UI task woken by a callback posted from another task
    PERF_UI_NOTIFY_TIMER,    // ... by a display-state timer (dim/sleep/deep sleep)
    PERF_UI_NOTIFY_INPUT,    // ... by the encoder
    PERF_UI_NOTIFY_OTHER,    // ... by WiFi/OTA/BLE housekeeping flags
    PERF_ENCODER_WAKE_ISR,   // encoder pin interrupt while asleep
    PERF_LV_INVALIDATE,      // an area was marked for redraw (LV_EVENT_INVALIDATE_AREA)
    PERF_LV_RENDER,          // one LVGL render pass (LV_EVENT_RENDER_START)
    PERF_LV_FLUSH,           // one flush of pixels toward the panel
    PERF_UI_WAIT_SIGNALLED,  // the UI loop's wait ended because it was signalled (not a timeout)
    PERF_COUNTER_COUNT
} perf_counter_t;

// Running totals that are not simple event counts.
typedef enum {
    PERF_ACC_INVALID_PX,     // sum of the pixel areas marked for redraw (overlaps counted twice)
    PERF_ACC_FLUSH_PX,       // pixels handed to the panel
    PERF_ACC_RENDER_US,      // microseconds spent rendering
    PERF_ACC_FLUSH_US,       // microseconds spent inside the flush callback
    PERF_ACC_COUNT
} perf_acc_t;

#if CONFIG_RK_PERF_LOG
void perf_count(perf_counter_t counter);
void perf_add(perf_acc_t acc, uint32_t value);
void perf_sleep_begin(void);
void perf_sleep_end(const char *cause);
// Call from the UI loop: every 30 s logs the counters for that window with the display state
// name, so each mode (normal, art, dim, sleep) gets its own numbers as the dial moves through it.
void perf_periodic(const char *state_name);
#else
static inline void perf_count(perf_counter_t counter) { (void)counter; }
static inline void perf_add(perf_acc_t acc, uint32_t value) { (void)acc; (void)value; }
static inline void perf_periodic(const char *state_name) { (void)state_name; }
static inline void perf_sleep_begin(void) {}
static inline void perf_sleep_end(const char *cause) { (void)cause; }
#endif
