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
    return true;
}
