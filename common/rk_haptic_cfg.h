#pragma once

// Haptic feedback preferences (idf_app/main/haptic_driver.c, Dial-only).
// Deliberately tiny (3 bytes) and deliberately its own independent NVS
// blob, same reasoning as rk_ha_cfg.h - but worth calling out explicitly
// here: this is exactly the kind of small, fixed-size struct that's safe
// to keep as a plain stack local everywhere. Contrast with
// rk_title_filter_cfg_t (4KB+), which caused two failed flashes by
// sitting on the stack in a couple of places before that got fixed -
// there's no equivalent risk here.

#include <stdbool.h>
#include <stdint.h>

#define RK_HAPTIC_CFG_CURRENT_VER 2  // v1 had no effect_id field

typedef struct {
    uint8_t enabled;
    uint8_t effect_id;  // DRV2605 library effect ID - see haptic_driver.h
    uint8_t cfg_ver;
} rk_haptic_cfg_t;

static inline bool rk_haptic_cfg_is_valid(const rk_haptic_cfg_t *cfg) {
    return cfg && cfg->cfg_ver != 0;
}

static inline void rk_haptic_cfg_set_defaults(rk_haptic_cfg_t *cfg) {
    if (!cfg) {
        return;
    }
    *cfg = (rk_haptic_cfg_t){0};
    cfg->cfg_ver = RK_HAPTIC_CFG_CURRENT_VER;
    // Default OFF, not on: the DRV2605 has never actually been driven by
    // this firmware before this slice (docs/esp/hw-reference/drv2605.md's
    // notes aside, nothing ever wrote to it - see haptic_driver.h), so
    // this is genuinely first-time, unverified-on-real-hardware behavior.
    // Same reasoning the wifi_power_save_enabled/cpu_freq_scaling_enabled
    // settings used when those were introduced ("disabled by default
    // until proven stable in real-world use").
    cfg->enabled = 0;
    cfg->effect_id = 1;  // "Strong Click - 100%" - see haptic_driver.h
}
