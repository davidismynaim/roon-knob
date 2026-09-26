#pragma once

// Voice-listen trigger for the Dial (Lounge and Dining Room): a long-press on the
// play button starts this room's voice satellite listening (no wake word needed) and
// swaps the button to a red mic until the conversation ends.
//
// Starting is delegated to the Home Assistant script script.<room>_voice_listen,
// so how listening starts can change in HA without reflashing. The "conversation
// ended" signal comes from binary_sensor.<room>_voice_active, which an HA
// automation mirrors from the satellite's listening/idle events - the same
// events the LUMIN ducking automation uses.
//
// Dial-only: compiled into idf_app alone.

#include "rk_ha_cfg.h"

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

// UI thread, from the long-press handler. Shows the red mic straight away and
// fires the HA script off-thread. False if not applicable (HA
// not configured) or a voice interaction is already showing.
bool voice_client_request_listen(void);

// True from the long-press until the conversation ends (or the request fails
// or times out).
bool voice_client_active(void);

// True while the HA poll should run fast so the red mic clears promptly.
bool voice_client_wants_fast_poll(void);

// Once per HA poll cycle (ha_volume_client.c's poll task): reads the mirrored
// satellite state and reconciles the indicator with it.
void voice_client_poll(const rk_ha_cfg_t *cfg);

#ifdef __cplusplus
}
#endif
