#pragma once

// Direct-to-Home-Assistant volume backend for Nexus, bypassing Roon/UHC
// entirely. See docs/meta/decisions/2026-09-14_DESIGN_HYBRID_DIAL_UI.md for
// why. Dial-only: compiled into idf_app alone (see idf_app/main/CMakeLists.txt),
// not part of the shared controller/backend boundary Frame and RLCD share.

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Loads HA host/token from NVS (see platform_storage_read_ha) and, if
// configured, starts a background poll task that keeps the cached display
// value in sync with number.hifi_volume. Safe to call even when
// unconfigured: logs and no-ops so the device behaves exactly as before
// this feature existed until the owner sets host/token via the device's
// config web page.
void ha_volume_client_init(void);

// True once a valid host/token pair has been loaded (independent of
// whether HA is currently reachable).
bool ha_volume_client_is_active(void);

// Adjusts Nexus volume by `steps` 0.5 dB clicks (positive = up, negative =
// down) by calling the nexus_volume_up/down HA scripts once per step,
// updating the cached display value optimistically first. Matches
// controller_command_t.volume_steps. Returns false if not configured or if
// any of the HA calls failed.
bool ha_volume_client_adjust(int32_t steps);

// Fills the last-known HA volume state on the 0-255 position scale
// (volume_min=0, volume_max=255, volume_step=1). Returns the cached value
// even if the most recent poll failed (self-corrects on the next
// successful poll); all zero/defaults if never configured or never
// successfully read.
void ha_volume_client_get_display(float *volume, float *volume_min,
                                  float *volume_max, float *volume_step);

#ifdef __cplusplus
}
#endif
