# Light sleep while the screen is off

Issue #45 (with #46 and #21). While the display sleeps the Dial should spend its time in ESP-IDF
automatic light sleep, and redraw only what changed while it is awake.

## What it achieves (Dial, measured)

| | Before | After |
|---|---|---|
| CPU wake-ups per second, screen off | ~950 | ~20 |
| Time actually in light sleep, screen off | 0 | about half |
| LVGL tick interrupts, any mode | 500/s | 0 |
| Pixels invalidated per 30 s, idle Music screen | ~1.8 M | ~0.19 M |

Current draw was not measured (no inline meter); expect roughly a 30-50% cut in screen-off current,
confirm with a USB power meter before quoting a number.

## How auto light sleep is reached

ESP-IDF's tickless idle asks the power manager to sleep only when **every** task and timer is due at
least `CONFIG_FREERTOS_IDLE_TIME_BEFORE_SLEEP` ticks away (2 ticks = 20 ms here, default is 3) and no
`ESP_PM_NO_LIGHT_SLEEP` lock is held. `display_sleep()` releases the display's no-light-sleep lock;
every other display state holds it, so none of this affects the awake path.

That means **any task that wakes more often than every ~20 ms while asleep defeats light sleep
entirely**. What had to change:

- **Timers off while asleep.** The 3 ms encoder poll timer is stopped and both encoder pins become GPIO
  wake sources (`platform_input_sleep_mode()`); the LVGL refresh timer and the redundant 50 ms
  `poll_pending` timer are paused; the touch controller is read every 50 ms (tap-to-wake latency).
- **No periodic LVGL tick timer.** `lv_tick_set_cb()` reads `esp_timer` instead of a 2 ms timer.
- **The UI loop blocks.** While asleep it waits on a semaphore for the time LVGL says its next timer is due
  (`ui_loop_iter()` returns it); posts from other tasks and the sleep timers wake it early.
- **Poll loops became event waits** (`include/os_event.h`): the bridge poll, the HA volume poll and the HA
  volume flush task no longer sleep 30-100 ms in a loop.
- **The BLE service task** slept in a 25 ms `vTaskDelay` loop even when idle; it now waits for a
  notification until its next real deadline.
- **Display GPIOs stay driven.** ESP-IDF floats every GPIO on each light sleep (forced on by
  `CONFIG_ESP_SLEEP_GPIO_RESET_WORKAROUND`, which protects against ESD resets). The QSPI panel then
  decoded the glitches and woke as noise, so `gpio_sleep_sel_dis()` keeps CS/CLK/data/reset/backlight
  driven. Any new peripheral that must survive sleep needs the same.

Rule for new code: while `display_is_sleeping()`, nothing may poll faster than ~50 ms. Block on an
`os_event_t` or a timeout instead of sleeping in a loop.

## Redraws

An LVGL style/text setter with an unchanged value still invalidates the object. Poll-driven update paths
(`apply_state()`, `update_battery_display()`) must skip unchanged values; see
`set_text_color_if_changed()` in `common/ui.c`. The remaining ~4 renders/s while a track plays is the
progress ring stepping about one pixel per 250 ms.

## Profiling

Profiling is compiled in only with `CONFIG_RK_PERF_LOG` (never in release builds):

```bash
cd idf_app && rm -f sdkconfig
idf.py -DSDKCONFIG_DEFAULTS="sdkconfig.defaults;sdkconfig.profiling" set-target esp32s3
idf.py build
```

- Every 30 s the log shows counters per display state (UI passes, touch reads, invalidations, renders).
- Each sleep session prints ESP-IDF's power-mode times, light-sleep counts, a histogram of the gap it saw,
  time actually slept, per-task CPU time and per-timer firing counts.
- **The USB serial link drops while the chip light-sleeps and may need a cable reseat afterwards**, so
  the same report is kept in RAM and served at `http://<dial>/perf`.
- Large redraw requests are logged with a call stack (`inval:` lines); decode with
  `xtensa-esp32s3-elf-addr2line -pfiaC -e build/hiphi_dial.elf <addresses>`.

## Not done / next levers

- The touch poll (20/s) and the mDNS timer (10/s) are now the biggest sleeping wake-ups. The CST816D INT pin
  is not documented for this board; reading it as a wake source would remove the touch poll.
- Wi-Fi keeps its own timers (DTIM/beacons); tuning the listen interval is untested.
