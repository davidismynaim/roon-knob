#pragma once

#include <stdbool.h>

// Encoder handling while the display sleeps (issue #45).
//
// Awake, a 3 ms timer polls the two encoder pins in software. That timer wakes the CPU 333 times a
// second, which keeps light sleep from lasting. While the display is asleep the timer is stopped
// and both pins instead wake the CPU from light sleep (and interrupt) on their next change. The
// first movement only wakes the display: like the deep-sleep wake, it does not change the volume.
// Leaving sleep mode restarts the polling timer and re-reads the pins so the pause is not decoded
// as rotation.
void platform_input_sleep_mode(bool sleeping);
