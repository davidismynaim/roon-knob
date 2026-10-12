#include "ha_source_client.h"

#include "platform/platform_http.h"
#include "platform/platform_log.h"
#include "platform/platform_storage.h"
#include "room_cfg.h"

#include <stdio.h>

bool ha_source_client_select(const char *option) {
    if (!option || !option[0]) {
        return false;
    }

    rk_ha_cfg_t cfg;
    if (!platform_storage_read_ha(&cfg) || !rk_ha_cfg_is_valid(&cfg)) {
        LOGW("Source select: HA not configured (set host/token in the "
             "device's config page)");
        return false;
    }

    char url[128];
    char body[96];
    if (room_cfg_get_current() == RK_ROOM_DINING) {
        // Not instantaneous server-side like Lounge's plain state write -
        // script.venu360_select_input runs a real break-before-make
        // sequence through the DBX bridge (mute the inactive source,
        // verify, unmute the new one). Confirmation only arrives on the
        // next sensor.venu360_inputs poll - same optimistic-then-reconcile
        // handling source_picker_client.c already applies to Lounge covers
        // this fine, nothing extra needed here.
        snprintf(url, sizeof(url),
                 "http://%s/api/services/script/venu360_select_input",
                 cfg.host);
        snprintf(body, sizeof(body), "{\"source\":\"%s\"}", option);
    } else {
        snprintf(url, sizeof(url),
                 "http://%s/api/services/input_select/select_option",
                 cfg.host);
        snprintf(
            body, sizeof(body),
            "{\"entity_id\":\"input_select.audio_input\",\"option\":\"%s\"}",
            option);
    }

    char *resp = NULL;
    size_t resp_len = 0;
    int ret = platform_http_post_auth(url, cfg.token, body, &resp, &resp_len);
    platform_http_free(resp);
    if (ret != 0) {
        LOGW("Source select: HA call failed for option '%s'", option);
        return false;
    }

    // roon-knob#63: also tell the X2 hub directly, Lounge only. The write
    // above is a no-op in HA's eyes (no state-changed event at all) when
    // `option` already matches input_select.audio_input's current value -
    // which is exactly what happens re-picking the activity you were on
    // BEFORE powering off, since powering off never touches that entity.
    // audio_x2_activity_display_sync only runs the hub's real wake/switch
    // macro on an actual state CHANGE of input_select.audio_input, so that
    // no-op left the hub stuck off even though the dial itself moved on -
    // confirmed live, 12 October 2026. A direct select.select_option call
    // runs the integration's handler unconditionally on every call, same
    // as ha_source_client_power_off's own write, so it reliably wakes the
    // hub regardless of whether the value "changed".
    if (room_cfg_get_current() != RK_ROOM_DINING) {
        snprintf(url, sizeof(url), "http://%s/api/services/select/select_option",
                 cfg.host);
        snprintf(body, sizeof(body),
                 "{\"entity_id\":\"select.x2_hub_activity\",\"option\":\"%s\"}",
                 option);
        char *resp2 = NULL;
        size_t resp2_len = 0;
        int ret2 = platform_http_post_auth(url, cfg.token, body, &resp2, &resp2_len);
        platform_http_free(resp2);
        if (ret2 != 0) {
            LOGW("Source select: X2 hub activity call failed for option '%s'", option);
            // Don't fail the whole call - input_select.audio_input was
            // already written successfully above, and this second write
            // is purely to guarantee the hub wakes on a same-value
            // reselect; a transient failure here self-corrects on the
            // next deliberate activity pick.
        }
    }
    return true;
}

bool ha_source_client_power_off(void) {
    rk_ha_cfg_t cfg;
    if (!platform_storage_read_ha(&cfg) || !rk_ha_cfg_is_valid(&cfg)) {
        LOGW("Power off: HA not configured (set host/token in the device's "
             "config page)");
        return false;
    }

    char url[128];
    snprintf(url, sizeof(url), "http://%s/api/services/select/select_option",
             cfg.host);
    static const char *const body =
        "{\"entity_id\":\"select.x2_hub_activity\",\"option\":\"Powered Off\"}";

    char *resp = NULL;
    size_t resp_len = 0;
    int ret = platform_http_post_auth(url, cfg.token, body, &resp, &resp_len);
    platform_http_free(resp);
    if (ret != 0) {
        LOGW("Power off: HA call failed");
        return false;
    }
    return true;
}
