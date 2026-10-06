#pragma once

// Sends a Fire TV nav/transport command via Home Assistant's
// script.fire_tv_dial_command (roon-knob dial#54). Dial-only, same as
// ha_source_client.h - compiled into idf_app alone, not part of the
// shared controller/backend boundary Frame and RLCD share. See
// docs/meta/decisions/2026-09-14_DESIGN_HYBRID_DIAL_UI.md.

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

// Sends `command` (one of "OK", "UP", "DOWN", "LEFT", "RIGHT", "HOME",
// "MENU", "BACK", "FAST_FORWARD", "REWIND" - see scripts.yaml's
// fire_tv_dial_command) to the Fire TV via Home Assistant. A rare,
// user-initiated action like ha_source_client_select, not a hot path, so
// unlike ha_volume_client it reads HA config fresh on each call rather
// than caching it. Returns false if HA isn't configured (host/token
// unset) or the call failed.
bool ha_firetv_client_send(const char *command);

#ifdef __cplusplus
}
#endif
