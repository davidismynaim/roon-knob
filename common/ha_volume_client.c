#include "ha_volume_client.h"

#include "controller_presentation.h"
#include "os_mutex.h"
#include "platform/platform_http.h"
#include "platform/platform_log.h"
#include "platform/platform_storage.h"
#include "platform/platform_task.h"
#include "platform/platform_time.h"

#include <cJSON.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>

// Poll cadence for number.hifi_volume. Independent of the UHC/Roon poll
// loop in bridge_client.c on purpose - this must keep working even when
// Roon/UHC is unreachable.
#define HA_VOLUME_POLL_INTERVAL_MS 2000
#define HA_VOLUME_POLL_TASK_STACK 4096

static os_mutex_t s_lock = OS_MUTEX_INITIALIZER;
static rk_ha_cfg_t s_cfg;
static bool s_configured;
static int s_position;       // last-known volume, 0-255 position scale
static bool s_have_position;  // true once a poll has succeeded at least once

static float clamp_position(float value) {
    if (value < 0.0f) {
        return 0.0f;
    }
    if (value > 255.0f) {
        return 255.0f;
    }
    return value;
}

static int db_to_position(float db) {
    float position = roundf((db + 127.5f) * 2.0f);
    return (int)clamp_position(position);
}

static bool snapshot_cfg(rk_ha_cfg_t *out) {
    os_mutex_lock(&s_lock);
    bool configured = s_configured;
    if (configured && out) {
        *out = s_cfg;
    }
    os_mutex_unlock(&s_lock);
    return configured;
}

static void set_cached_position(int position) {
    os_mutex_lock(&s_lock);
    s_position = position;
    s_have_position = true;
    os_mutex_unlock(&s_lock);
}

static int get_cached_position(void) {
    os_mutex_lock(&s_lock);
    int position = s_position;
    os_mutex_unlock(&s_lock);
    return position;
}

static bool poll_once(const rk_ha_cfg_t *cfg) {
    char url[128];
    snprintf(url, sizeof(url), "http://%s/api/states/number.hifi_volume",
             cfg->host);

    char *resp = NULL;
    size_t resp_len = 0;
    int ret = platform_http_get_auth(url, cfg->token, &resp, &resp_len);
    if (ret != 0 || !resp) {
        platform_http_free(resp);
        return false;
    }

    cJSON *root = cJSON_Parse(resp);
    platform_http_free(resp);
    if (!root) {
        return false;
    }

    cJSON *state = cJSON_GetObjectItemCaseSensitive(root, "state");
    bool ok = false;
    if (cJSON_IsString(state) && state->valuestring) {
        char *end = NULL;
        float db = strtof(state->valuestring, &end);
        if (end != state->valuestring) {
            int position = db_to_position(db);
            set_cached_position(position);
            controller_presentation_set_volume_range((float)position, 0.0f,
                                                      255.0f, 1.0f);
            ok = true;
        }
    }
    cJSON_Delete(root);
    return ok;
}

static void poll_task(void *arg) {
    (void)arg;
    while (true) {
        rk_ha_cfg_t cfg;
        if (snapshot_cfg(&cfg)) {
            if (!poll_once(&cfg)) {
                LOGW("HA volume poll failed (host='%s')", cfg.host);
            }
        }
        platform_sleep_ms(HA_VOLUME_POLL_INTERVAL_MS);
    }
}

void ha_volume_client_init(void) {
    rk_ha_cfg_t cfg;
    if (!platform_storage_read_ha(&cfg) || !rk_ha_cfg_is_valid(&cfg)) {
        LOGI("HA volume control not configured (set host/token in the "
             "device's config page)");
        return;
    }

    os_mutex_lock(&s_lock);
    s_cfg = cfg;
    s_configured = true;
    os_mutex_unlock(&s_lock);

    if (platform_task_start_configured("ha_vol_poll",
                                       HA_VOLUME_POLL_TASK_STACK, poll_task,
                                       NULL) != 0) {
        LOGE("Failed to start HA volume poll task");
    } else {
        LOGI("HA volume control active (host='%s')", cfg.host);
    }
}

bool ha_volume_client_is_active(void) {
    os_mutex_lock(&s_lock);
    bool configured = s_configured;
    os_mutex_unlock(&s_lock);
    return configured;
}

bool ha_volume_client_adjust(int32_t steps) {
    if (steps == 0) {
        return false;
    }
    rk_ha_cfg_t cfg;
    if (!snapshot_cfg(&cfg)) {
        return false;
    }

    int32_t magnitude = steps < 0 ? -steps : steps;

    int new_position = get_cached_position() + steps;
    if (new_position < 0) {
        new_position = 0;
    } else if (new_position > 255) {
        new_position = 255;
    }
    set_cached_position(new_position);
    controller_presentation_show_volume_change((float)new_position, 1.0f);

    /* One call to script.audio_voice_volume per dispatch, not one call
     * per step: that script resolves the currently-selected input's
     * helper itself, clamps 0-255, and writes the new target in a single
     * input_number.set_value. Audio - Volume Helper Changed then fires
     * exactly one remote.send_command with num_repeats = the resulting
     * delta - the same native-repeat batching already proven for
     * Harmony's held-button case (wiki §44.9, §47.2/§47.4), rather than
     * this firmware looping individual nexus_volume_up/down calls and
     * producing one IR transaction per step. */
    char url[128];
    snprintf(url, sizeof(url),
             "http://%s/api/services/script/audio_voice_volume", cfg.host);
    char body[96];
    snprintf(body, sizeof(body),
             "{\"action\":\"%s\",\"unit\":\"clicks\",\"amount\":%ld}",
             steps > 0 ? "increase" : "decrease", (long)magnitude);

    char *resp = NULL;
    size_t resp_len = 0;
    int ret = platform_http_post_auth(url, cfg.token, body, &resp, &resp_len);
    platform_http_free(resp);
    if (ret != 0) {
        LOGW("HA volume adjust: audio_voice_volume call failed");
        return false;
    }
    return true;
}

void ha_volume_client_get_display(float *volume, float *volume_min,
                                  float *volume_max, float *volume_step) {
    int position = s_have_position ? get_cached_position() : 0;
    if (volume) {
        *volume = (float)position;
    }
    if (volume_min) {
        *volume_min = 0.0f;
    }
    if (volume_max) {
        *volume_max = 255.0f;
    }
    if (volume_step) {
        *volume_step = 1.0f;
    }
}
