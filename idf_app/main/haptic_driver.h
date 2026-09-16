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

#include <stddef.h>
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Selectable built-in effect (TI's own DRV2605 waveform-library names, not
// ours - owner direction: let the user pick which one feels best rather
// than guessing). All are the "100%" strength variant of their family -
// picking a different *character* of effect, not a percentage tier of the
// same one, since the underlying drive-voltage ceiling (RATED_VOLTAGE/
// OVERDRIVE_CLAMP_VOLTAGE) is left at the chip's own conservative
// power-on-reset default in this pass (see the no-calibration note
// above) and is shared by every effect regardless of which is selected.
//
// IDs 1-14 only: this is the subset of TI's waveform-library effect table
// reproduced consistently enough across independent reference
// implementations that picking them without the datasheet PDF in hand
// felt safe to commit to; higher IDs (alert tones, numbered click/tick
// variants) exist in the real library but aren't offered here since
// their exact naming/behavior wasn't something this session could verify
// against a primary source.
typedef struct {
    uint8_t effect_id;
    const char *name;
} haptic_effect_option_t;

// Returns the selectable effect list and its length. Order matches what
// the config page's dropdown should show.
const haptic_effect_option_t *haptic_driver_get_effect_options(size_t *count);

// Initializes the DRV2605 (exits standby, selects open-loop ERM mode) and
// loads the enabled/disabled preference and selected effect from NVS.
// Call once at startup, after i2c_master_Init(). Safe to call even if the
// chip doesn't respond (logs a warning, haptic_driver_pulse() then just
// no-ops).
void haptic_driver_init(void);

// True if haptic feedback is currently enabled (persisted setting).
bool haptic_driver_is_enabled(void);

// Updates and persists the enabled/disabled setting. Returns false if the
// NVS write failed (the in-memory setting is still updated either way).
bool haptic_driver_set_enabled(bool enabled);

// The currently-selected effect's DRV2605 library ID.
uint8_t haptic_driver_get_effect(void);

// Updates and persists which effect haptic_driver_pulse() plays. Silently
// ignored if effect_id isn't one of haptic_driver_get_effect_options()'s
// entries. Returns false if the NVS write failed (the in-memory setting
// is still updated either way).
bool haptic_driver_set_effect(uint8_t effect_id);

// Fires the currently-selected effect if enabled and the chip initialized
// successfully; otherwise a no-op. Drops (does not queue) a request that
// arrives while the previous pulse is still playing - effects are short
// enough, and triggers infrequent enough (touch/long-press, not volume
// clicks), that this is expected to be rare in practice.
void haptic_driver_pulse(void);

#ifdef __cplusplus
}
#endif
