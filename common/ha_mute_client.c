#include "ha_mute_client.h"

#include "platform/platform_http.h"
#include "platform/platform_log.h"
#include "platform/platform_storage.h"

#include <stdio.h>

bool ha_mute_client_toggle(void) {
    rk_ha_cfg_t cfg;
    if (!platform_storage_read_ha(&cfg) || !rk_ha_cfg_is_valid(&cfg)) {
        LOGW("Mute toggle: HA not configured (set host/token in the "
             "device's config page)");
        return false;
    }

    char url[128];
    snprintf(url, sizeof(url),
             "http://%s/api/services/input_boolean/toggle", cfg.host);

    char *resp = NULL;
    size_t resp_len = 0;
    int ret = platform_http_post_auth(
        url, cfg.token, "{\"entity_id\":\"input_boolean.audio_mute\"}",
        &resp, &resp_len);
    platform_http_free(resp);
    if (ret != 0) {
        LOGW("Mute toggle: HA call failed");
        return false;
    }
    return true;
}
