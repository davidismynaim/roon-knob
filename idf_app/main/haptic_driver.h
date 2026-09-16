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
// No auto-calibration in the first pass (owner direction) - ran the DRV2605
// open-loop with the chip's own power-on-reset default drive levels rather
// than measuring the actual motor and tuning to it. haptic_driver_run_
// calibration() below adds that as an explicit, one-shot, opt-in action once
// hardware testing confirmed effects were felt but subtle.
//
// Actuator type (ERM vs LRA) and an undocumented enable-pin theory (GPIO38)
// were both open questions when calibration was being considered - resolved
// via the temporary diagnostic further down before writing this: on this
// specific board, GPIO38 makes no measurable difference to the DRV2605's own
// actuator diagnostic (0xe0 "connected" either way), and the vendor's own
// demo (docs/esp/hw-reference/drv2605.md) treats it as ERM with no enable
// pin - both now corroborated by direct hardware measurement, not just the
// vendor's word. Calibration below assumes ERM and does not touch GPIO38.
//
// Calibration deliberately does NOT raise RATED_VOLTAGE/OVERDRIVE_CLAMP_
// VOLTAGE - it only tunes closed-loop compensation/back-EMF gain for
// whatever drive ceiling is already configured (the chip's own power-on
// default, since those registers are still never written). That means this
// is expected to improve consistency/crispness (closed-loop actively
// compensates for friction/binding instead of just driving blind), not
// necessarily raw perceived strength - raising the actual voltage ceiling is
// a separate, distinct decision, not bundled into this.

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

// TEMPORARY hardware diagnostic - not a real feature, easy to remove once
// this board's actuator question is settled. See docs/esp/hw-reference/
// drv2605.md and the PR/issue that added this: two external sources
// disagree about this exact board (Waveshare ESP32-S3-Knob-Touch-LCD-1.8) -
// the vendor's own demo drives it as ERM with no enable pin at all, while an
// independent reverse-engineering project measured it as an LRA behind an
// undocumented enable pin on GPIO38. Owner's own hardware already contradicts
// the second source (effects ARE felt, subtly, without ever touching
// GPIO38), so rather than guess, these let the owner check on the actual
// device: each drives GPIO38 as instructed, optionally reconfigures the
// actuator-type register, reads back the DRV2605's own built-in actuator
// diagnostic (logged, not returned - check the serial monitor), and fires a
// felt "Strong Click" pulse (effect id 1, which is Strong Click in every
// library per the datasheet's effect table) so the difference can be felt
// as well as read. Deliberately NOT part of haptic_driver_get_effect_options()
// and NOT persisted - a one-shot action, not a setting.
typedef struct {
    uint8_t test_id;
    const char *name;
} haptic_diagnostic_option_t;

const haptic_diagnostic_option_t *haptic_driver_get_diagnostic_options(size_t *count);

// Runs one of haptic_driver_get_diagnostic_options()'s test_id values. No-op
// (with a warning logged) if the chip never initialized or test_id is
// unrecognized. The device reboots after every config-page save regardless
// (existing pattern), which restores normal ERM configuration from NVS
// afterward - no cleanup needed here.
void haptic_driver_run_diagnostic(uint8_t test_id);

// Runs the DRV2605's own auto-calibration against the actuator as currently
// configured (ERM, unchanged drive-voltage ceiling - see the header comment
// above). This IS felt: calibration is the one thing that moves the motor
// while it runs (a few hundred ms up to ~1.2s). Logs the pass/fail result
// and the compensation/back-EMF/feedback values either way.
//
// On a pass, persists those three values and switches immediately from
// open-loop to closed-loop ERM drive - no reboot needed to take effect. On a
// failure (the chip's own diagnostic bit, not a guess), logs a warning and
// leaves the existing open-loop configuration completely untouched; nothing
// is persisted, so a failed run can't leave a device worse off than before.
// No-op if the chip never initialized.
void haptic_driver_run_calibration(void);

// True once a calibration has been run, passed, and its result loaded
// (either just now, or from NVS at boot).
bool haptic_driver_is_calibrated(void);

#ifdef __cplusplus
}
#endif
