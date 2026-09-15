#pragma once

// Home Assistant volume-backend config (see
// docs/meta/decisions/2026-09-14_DESIGN_HYBRID_DIAL_UI.md). Deliberately
// stored as its own small NVS blob (namespace "rk_ha", key "cfg") rather
// than folded into rk_cfg_t: it's unrelated to the device-local
// connectivity fields that struct's V1/V2/V3 migration machinery exists to
// protect, and keeping it separate means this feature can't accidentally
// disturb that compatibility contract.

#include <stdbool.h>
#include <stdint.h>

#define RK_HA_CFG_CURRENT_VER 1

typedef struct {
    char host[64];    // "host:port", no scheme (e.g. "192.168.4.55:8123")
    char token[256];  // HA long-lived access token
    uint8_t cfg_ver;
} rk_ha_cfg_t;

static inline bool rk_ha_cfg_is_valid(const rk_ha_cfg_t *cfg) {
    return cfg && cfg->cfg_ver != 0 && cfg->host[0] != '\0' &&
           cfg->token[0] != '\0';
}

static inline void rk_ha_cfg_set_defaults(rk_ha_cfg_t *cfg) {
    if (!cfg) {
        return;
    }
    *cfg = (rk_ha_cfg_t){0};
    cfg->cfg_ver = RK_HA_CFG_CURRENT_VER;
}
