#pragma once

// DRV2605 haptic motor driver (docs/esp/hw-reference/drv2605.md). The chip
// was already wired up on the I2C bus in i2c_bsp.c (drv2605_dev_handle) but
// never actually driven - this is the first code that talks to it.
//
// Deliberately touch-only, not volume: the encoder's mechanical detents
// already give a good tactile feel for volume changes (owner direction),
// so this only fires on the transport buttons (play/pause/prev/next) and
// the long-press gestures (mute, source picker).
//
// No auto-calibration in this first pass (owner direction) - runs the
// DRV2605 open-loop with the chip's own power-on-reset default drive
// levels rather than measuring the actual motor and tuning to it. Safer
// (can't overdrive an unknown actuator with an unverified calibration)
// and simpler, at the cost of a less precisely-tuned feel; revisit with
// real auto-calibration only if that turns out to matter once felt on
// hardware.

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

// Initializes the DRV2605 (exits standby, selects open-loop ERM mode) and
// loads the enabled/disabled preference from NVS. Call once at startup,
// after i2c_master_Init(). Safe to call even if the chip doesn't respond
// (logs a warning, haptic_driver_pulse() then just no-ops).
void haptic_driver_init(void);

// True if haptic feedback is currently enabled (persisted setting).
bool haptic_driver_is_enabled(void);

// Updates and persists the enabled/disabled setting. Returns false if the
// NVS write failed (the in-memory setting is still updated either way).
bool haptic_driver_set_enabled(bool enabled);

// Fires a short "click" pulse if enabled and the chip initialized
// successfully; otherwise a no-op. Drops (does not queue) a request that
// arrives while the previous pulse is still playing - effects are short
// enough, and triggers infrequent enough (touch/long-press, not volume
// clicks), that this is expected to be rare in practice.
void haptic_driver_pulse(void);

#ifdef __cplusplus
}
#endif
