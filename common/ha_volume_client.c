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

// Rotation writes are accumulated and flushed as one HA call per burst
// rather than one call per encoder dispatch, matching the owner's wiki
// (§44.6/§47.4's Audio - Harmony Volume: accumulate, then flush on
// inactivity). A physical knob should feel closer to instant than a
// remote's held-button repeat, so this uses a much shorter quiet window
// than that automation's 500ms - tune on real hardware once flashed.
#define HA_VOLUME_DEBOUNCE_MS 90
#define HA_VOLUME_FLUSH_POLL_MS 30
#define HA_VOLUME_FLUSH_TASK_STACK 4096

// common/controller_input.c's resolve_volume_ticks now passes the true
// accumulated encoder tick count straight through as the step count
// (see that file), uncapped. Whether one raw tick equals one physical
// detent hasn't been confirmed on real hardware yet - this scales
// firmware ticks to the "clicks" (0.5 dB each) audio_voice_volume
// expects. Start at 1:1 and correct after checking on hardware (e.g. a
// temporary log of raw ticks against a known number of manual clicks).
#define HA_VOLUME_TICKS_PER_CLICK 1

static os_mutex_t s_lock = OS_MUTEX_INITIALIZER;
static rk_ha_cfg_t s_cfg;
static bool s_configured;
/* Gates every HTTP call, mirroring bridge_client_set_network_ready. lwIP's
 * TCPIP task doesn't exist yet this early in boot - firing a GET/POST
 * before esp_netif/WiFi have come up at all (not just "connected", but
 * initialized) crashes with "assert failed: tcpip_send_msg_wait_sem ...
 * Invalid mbox" (hit live on hardware: the poll task's very first,
 * undelayed loop iteration raced ahead of network init). Set from the
 * same RK_NET_EVT_GOT_IP/FAIL/AP_STARTED handling in main_idf.c that
 * already drives bridge_client_set_network_ready. */
static bool s_network_ready;
static int s_position;       // last-known volume, 0-255 position scale
static bool s_have_position;  // true once a poll has succeeded at least once

/* input_select.audio_input's current value ("Music"/"TV"/"Vinyl"), polled
 * in the same cycle as volume below rather than via a second task+stack -
 * same host/token/network-ready gate either way, and this is a single
 * tiny GET. Lets the dial reflect a source switch made from anywhere
 * (Harmony, the HA dashboard, voice), not just its own picker - needed
 * for the picker's own highlight now, and for choosing which screen to
 * show once the TV/Vinyl screens exist (a later slice). */
#define HA_CURRENT_SOURCE_MAX 16
static char s_current_source[HA_CURRENT_SOURCE_MAX];
static bool s_have_source;

static int32_t s_pending_ticks;    // accumulated, not-yet-sent raw ticks
static uint64_t s_last_tick_ms;    // when a tick last landed in the burst

static void flush_task(void *arg);

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
    bool ready = s_configured && s_network_ready;
    if (ready && out) {
        *out = s_cfg;
    }
    os_mutex_unlock(&s_lock);
    return ready;
}

void ha_volume_client_set_network_ready(bool ready) {
    os_mutex_lock(&s_lock);
    s_network_ready = ready;
    os_mutex_unlock(&s_lock);
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

static void set_cached_source(const char *source) {
    os_mutex_lock(&s_lock);
    rk_strlcpy(s_current_source, source, sizeof(s_current_source));
    s_have_source = true;
    os_mutex_unlock(&s_lock);
}

bool ha_volume_client_get_current_source(char *out, size_t len) {
    if (!out || len == 0) {
        return false;
    }
    os_mutex_lock(&s_lock);
    bool have = s_have_source;
    if (have) {
        rk_strlcpy(out, s_current_source, len);
    }
    os_mutex_unlock(&s_lock);
    return have;
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

static bool poll_source_once(const rk_ha_cfg_t *cfg) {
    char url[128];
    snprintf(url, sizeof(url),
             "http://%s/api/states/input_select.audio_input", cfg->host);

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
        set_cached_source(state->valuestring);
        ok = true;
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
            if (!poll_source_once(&cfg)) {
                LOGW("HA source poll failed (host='%s')", cfg.host);
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

    if (platform_task_start_configured("ha_vol_flush",
                                       HA_VOLUME_FLUSH_TASK_STACK, flush_task,
                                       NULL) != 0) {
        LOGE("Failed to start HA volume flush task");
    }
}

bool ha_volume_client_is_active(void) {
    os_mutex_lock(&s_lock);
    bool configured = s_configured;
    os_mutex_unlock(&s_lock);
    return configured;
}

/* One call to script.audio_voice_volume per burst, not per dispatch and
 * not per step: that script resolves the currently-selected input's
 * helper itself, clamps 0-255, and writes the new target in a single
 * input_number.set_value. Audio - Volume Helper Changed then fires
 * exactly one remote.send_command with num_repeats = the resulting
 * delta - the same native-repeat batching already proven for Harmony's
 * held-button case (wiki §44.9, §47.2/§47.4). */
static bool send_clicks(const rk_ha_cfg_t *cfg, int32_t clicks) {
    if (clicks == 0) {
        return true;
    }
    int32_t magnitude = clicks < 0 ? -clicks : clicks;

    char url[128];
    snprintf(url, sizeof(url),
             "http://%s/api/services/script/audio_voice_volume", cfg->host);
    char body[96];
    snprintf(body, sizeof(body),
             "{\"action\":\"%s\",\"unit\":\"clicks\",\"amount\":%ld}",
             clicks > 0 ? "increase" : "decrease", (long)magnitude);

    char *resp = NULL;
    size_t resp_len = 0;
    int ret = platform_http_post_auth(url, cfg->token, body, &resp, &resp_len);
    platform_http_free(resp);
    if (ret != 0) {
        LOGW("HA volume adjust: audio_voice_volume call failed");
        return false;
    }
    return true;
}

static void flush_pending(void) {
    rk_ha_cfg_t cfg;
    os_mutex_lock(&s_lock);
    /* Only actually drain s_pending_ticks once the network is up - if we
     * cleared it while not ready, a burst that happened before WiFi
     * connected would be silently lost instead of sent once it does. */
    bool ready = s_configured && s_network_ready;
    int32_t ticks = s_pending_ticks;
    if (ready) {
        cfg = s_cfg;
        s_pending_ticks = 0;
    }
    os_mutex_unlock(&s_lock);

    if (!ready || ticks == 0) {
        return;
    }
    int32_t clicks = ticks / HA_VOLUME_TICKS_PER_CLICK;
    if (clicks != 0) {
        (void)send_clicks(&cfg, clicks);
    }
}

static void flush_task(void *arg) {
    (void)arg;
    while (true) {
        platform_sleep_ms(HA_VOLUME_FLUSH_POLL_MS);
        os_mutex_lock(&s_lock);
        bool due = s_pending_ticks != 0 &&
                   (platform_millis() - s_last_tick_ms) >=
                       HA_VOLUME_DEBOUNCE_MS;
        os_mutex_unlock(&s_lock);
        if (due) {
            flush_pending();
        }
    }
}

bool ha_volume_client_adjust(int32_t ticks) {
    if (ticks == 0) {
        return false;
    }
    if (!ha_volume_client_is_active()) {
        return false;
    }

    /* Optimistic display update happens immediately, using the full raw
     * tick count (matching the eventual click count 1:1 while
     * HA_VOLUME_TICKS_PER_CLICK stays 1) - only the actual HA call is
     * debounced, so the dial still feels instantly responsive even
     * though the network write lags slightly behind a fast spin. */
    int new_position = get_cached_position() + ticks / HA_VOLUME_TICKS_PER_CLICK;
    if (new_position < 0) {
        new_position = 0;
    } else if (new_position > 255) {
        new_position = 255;
    }
    set_cached_position(new_position);
    controller_presentation_show_volume_change((float)new_position, 1.0f);

    os_mutex_lock(&s_lock);
    s_pending_ticks += ticks;
    s_last_tick_ms = platform_millis();
    os_mutex_unlock(&s_lock);
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
