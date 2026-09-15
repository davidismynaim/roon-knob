#pragma once

// Direct-to-Home-Assistant volume backend for Nexus, bypassing Roon/UHC
// entirely. See docs/meta/decisions/2026-09-14_DESIGN_HYBRID_DIAL_UI.md for
// why. Dial-only: compiled into idf_app alone (see idf_app/main/CMakeLists.txt),
// not part of the shared controller/backend boundary Frame and RLCD share.
// Also tracks input_select.audio_input's current value (see
// ha_volume_client_get_current_source below) - piggybacking on this
// module's existing poll task/network-ready gate rather than standing up
// a second one for one more tiny GET.

#include <stdbool.h>
#include <stddef.h>
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

// Gates every HTTP call this module makes. Call with true only once the
// device actually has an IP (mirrors bridge_client_set_network_ready) -
// firing a request before esp_netif/WiFi have initialized at all crashes
// the whole chip (lwIP's TCPIP task mailbox doesn't exist yet). Call with
// false on disconnect/AP-mode-fallback; queued rotation writes are kept,
// not dropped, until this goes true again.
void ha_volume_client_set_network_ready(bool ready);

// Called once per rotation dispatch with the true accumulated encoder
// tick count (positive = up, negative = down) - matches
// controller_command_t.volume_steps, which common/controller_input.c's
// resolve_volume_ticks now passes through uncapped rather than bucketing.
// Updates the cached display value optimistically and immediately, but
// only accumulates the actual Home Assistant write; a separate internal
// task debounces and flushes accumulated bursts as one call to
// script.audio_voice_volume, so a fast continuous spin doesn't fire one
// HA/IR round trip per dispatch. Returns false only if not configured or
// the tick count was zero - a true send failure inside the debounce
// window isn't reported back to the caller (nothing consumes this return
// value beyond ignoring it today; see controller_action_router.h).
bool ha_volume_client_adjust(int32_t ticks);

// Fills the last-known HA volume state on the 0-255 position scale
// (volume_min=0, volume_max=255, volume_step=1). Returns the cached value
// even if the most recent poll failed (self-corrects on the next
// successful poll); all zero/defaults if never configured or never
// successfully read.
void ha_volume_client_get_display(float *volume, float *volume_min,
                                  float *volume_max, float *volume_step);

// Fills `out` with the last-polled value of input_select.audio_input
// ("Music"/"TV"/"Vinyl"), reflecting a change made from anywhere (this
// dial's own picker, Harmony, the HA dashboard, voice), not just writes
// made through ha_source_client. Returns false if never successfully
// polled yet (e.g. still booting, or HA unreachable since boot) - `out`
// is left untouched in that case.
bool ha_volume_client_get_current_source(char *out, size_t len);

#ifdef __cplusplus
}
#endif
