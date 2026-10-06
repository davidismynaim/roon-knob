#include "ha_firetv_client.h"

#include "platform/platform_http.h"
#include "platform/platform_log.h"
#include "platform/platform_storage.h"

#include <stdio.h>

bool ha_firetv_client_send(const char *command) {
    if (!command || !command[0]) {
        return false;
    }

    rk_ha_cfg_t cfg;
    if (!platform_storage_read_ha(&cfg) || !rk_ha_cfg_is_valid(&cfg)) {
        LOGW("Fire TV command: HA not configured (set host/token in the "
             "device's config page)");
        return false;
    }

    char url[128];
    snprintf(url, sizeof(url),
             "http://%s/api/services/script/fire_tv_dial_command", cfg.host);
    char body[48];
    snprintf(body, sizeof(body), "{\"command\":\"%s\"}", command);

    char *resp = NULL;
    size_t resp_len = 0;
    int ret = platform_http_post_auth(url, cfg.token, body, &resp, &resp_len);
    platform_http_free(resp);
    if (ret != 0) {
        LOGW("Fire TV command: HA call failed for command '%s'", command);
        return false;
    }
    return true;
}
