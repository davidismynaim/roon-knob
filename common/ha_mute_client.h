#pragma once

// Toggles input_boolean.audio_mute via Home Assistant - the project's
// standard central mute toggle (wiki §44.3/§47.4: Harmony, voice, and the
// HA dashboard all operate this same helper; a separate HA-side automation
// is the one place that ever sends the actual Nexus mute IR). Dial-only,
// same as ha_volume_client.h/ha_source_client.h - compiled into idf_app
// alone. See docs/meta/decisions/2026-09-14_DESIGN_HYBRID_DIAL_UI.md.
//
// This is the functional toggle only. The full-screen red mute icon the
// design doc calls for is a separate, later slice (dedicated-single-zone
// gesture wiring here does not draw it).

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

// Toggles input_boolean.audio_mute via Home Assistant's input_boolean.toggle
// service. Reads HA config fresh each call (not cached), matching
// ha_source_client - a rare, user-initiated action, not a hot path.
// Returns false if HA isn't configured or the call failed.
bool ha_mute_client_toggle(void);

#ifdef __cplusplus
}
#endif
