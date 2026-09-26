#pragma once

// Installation/room selector (common/source_picker_client.c,
// common/ha_volume_client.c, common/ha_source_client.c,
// common/ha_mute_client.c, common/ui.c). Deliberately tiny (2 bytes) and
// deliberately its own independent NVS blob, same reasoning as
// rk_haptic_cfg.h - keeping it out of rk_ha_cfg_t specifically avoids
// resetting the existing HA host/token on upgrade (that struct's read path
// treats any size/version mismatch as "unconfigured", by design - see its
// own header comment), and keeping it out of rk_cfg_t avoids growing that
// struct's V1/V2/V3 migration surface for something unrelated to
// device-local connectivity. One firmware image, one enum flag - not a
// per-install configurable entity-id text field and not a generalized
// n-room abstraction (owner direction: hardcode both rooms' behavior
// behind this flag; there are no plans for a third installation).

#include <stdbool.h>
#include <stdint.h>

#define RK_ROOM_CFG_CURRENT_VER 1

typedef enum {
    RK_ROOM_LOUNGE = 0,
    RK_ROOM_DINING = 1,
} rk_room_t;

typedef struct {
    uint8_t room;  // rk_room_t
    uint8_t cfg_ver;
} rk_room_cfg_t;

static inline bool rk_room_cfg_is_valid(const rk_room_cfg_t *cfg) {
    return cfg && cfg->cfg_ver != 0;
}

static inline void rk_room_cfg_set_defaults(rk_room_cfg_t *cfg) {
    if (!cfg) {
        return;
    }
    *cfg = (rk_room_cfg_t){0};
    cfg->cfg_ver = RK_ROOM_CFG_CURRENT_VER;
    cfg->room = RK_ROOM_LOUNGE;  // Existing installs stay Lounge by default
}
