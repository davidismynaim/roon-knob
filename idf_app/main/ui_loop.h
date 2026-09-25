#pragma once

// Wake the UI loop task from another task (or an esp_timer callback). The loop blocks on a task
// notification instead of a fixed 10 ms delay while the display is asleep (issue #45), so anything
// that sets a flag the loop acts on must call this.
void ui_loop_wake(void);

// Same, from an interrupt handler.
void ui_loop_wake_from_isr(void);
