#pragma once

// Sets input_select.audio_input via Home Assistant. Dial-only, same as
// ha_volume_client.h - compiled into idf_app alone, not part of the
// shared controller/backend boundary Frame and RLCD share. See
// docs/meta/decisions/2026-09-14_DESIGN_HYBRID_DIAL_UI.md.

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

// Sets input_select.audio_input to `option` (e.g. "Music", "TV", "Vinyl")
// via Home Assistant's input_select.select_option service. This is a
// rare, user-initiated action (picking a source), not a hot path, so
// unlike ha_volume_client it reads HA config fresh on each call rather
// than caching it. Returns false if HA isn't configured (host/token
// unset) or the call failed.
bool ha_source_client_select(const char *option);

// Powers off the Lounge system by setting select.x2_hub_activity to
// "Powered Off" directly (roon-knob#63) - a different entity/service than
// ha_source_client_select above, since "Powered Off" isn't a valid
// input_select.audio_input option at all (see .storage/input_select's own
// configured options - just Music/TV/Vinyl). Lounge only, same as the
// Power Off picker entry itself; never call this for Dining. Returns
// false if HA isn't configured or the call failed.
bool ha_source_client_power_off(void);

#ifdef __cplusplus
}
#endif
