#pragma once

// Sends a TV nav/transport command via Home Assistant's script.tv_dial_command
// (dial#54, renamed from script.fire_tv_dial_command/ha_firetv_client - the
// Fire Stick it originally targeted is fully gone from the household now;
// this has driven the LG TV directly over webOS since 10 October 2026, see
// that script's own header comment). Dial-only, same as ha_source_client.h -
// compiled into idf_app alone, not part of the shared controller/backend
// boundary Frame and RLCD share. See
// docs/meta/decisions/2026-09-14_DESIGN_HYBRID_DIAL_UI.md.

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

// Sends `command` (one of "OK", "UP", "DOWN", "LEFT", "RIGHT", "HOME",
// "MENU", "BACK", "FAST_FORWARD", "REWIND", "PLAY" (dial#57's scrub-speed
// ladder resuming normal playback - see main_idf.c's s_tv_seek_level) -
// see scripts.yaml's tv_dial_command) to the TV via Home Assistant. A
// rare, user-initiated
// action like ha_source_client_select, not a hot path, so unlike
// ha_volume_client it reads HA config fresh on each call rather than
// caching it. Returns false if HA isn't configured (host/token unset) or
// the call failed.
bool ha_tv_remote_client_send(const char *command);

#ifdef __cplusplus
}
#endif
