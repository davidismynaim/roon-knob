#pragma once

// Installation/room selector - thin cached accessor over
// platform_storage_read_room/write_room (see rk_room_cfg.h for why this is
// its own tiny NVS blob rather than folded into rk_ha_cfg_t or rk_cfg_t).
// Read frequently (every HA poll cycle, every source-picker open, every
// screen switch) across ha_volume_client.c/ha_source_client.c/
// ha_mute_client.c/source_picker_client.c/ui.c - cached in memory at init
// rather than re-reading NVS on every call, same pattern as
// haptic_driver.c's s_enabled.

#include "rk_room_cfg.h"

// Load the persisted room selection into the in-memory cache. Call once
// during startup, alongside haptic_driver_init() (both dial-only,
// idf_app-side init).
void room_cfg_init(void);

// Fast - returns the in-memory cache, no NVS access.
rk_room_t room_cfg_get_current(void);

// Updates the in-memory cache and persists. Returns false if the NVS write
// failed (the in-memory cache is still updated regardless, so the running
// device behaves correctly even if the write didn't stick - it just won't
// survive a reboot).
bool room_cfg_set_current(rk_room_t room);
