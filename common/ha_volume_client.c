#include "ha_volume_client.h"

#include "controller_presentation.h"
#include "ha_mute_client.h"
#include "os_mutex.h"
#include "platform/platform_display.h"
#include "platform/platform_http.h"
#include "platform/platform_log.h"
#include "platform/platform_storage.h"
#include "platform/platform_task.h"
#include "platform/platform_time.h"
#include "room_cfg.h"
#include "vinyl_client.h"
#include "voice_client.h"

#include <cJSON.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// Poll cadence for number.hifi_volume/input_select.audio_input/
// input_boolean.audio_mute. Independent of the UHC/Roon poll loop in
// bridge_client.c on purpose - this must keep working even when Roon/UHC
// is unreachable - but mirrors that same loop's adaptive-interval pattern
// (see its wait_for_poll_interval()): fast while awake and charging, since
// nothing else is limiting battery life then; slower on battery, since
// there's no local reason to check three HA entities 30x/minute if nobody's
// looking at the screen; slower still once the display is asleep, when the
// only thing polling still buys is a mute/source/volume change from
// somewhere else (Harmony, the HA dashboard) showing up promptly on next
// wake rather than instantly.
#define HA_VOLUME_POLL_INTERVAL_AWAKE_CHARGING_MS 2000
#define HA_VOLUME_POLL_INTERVAL_AWAKE_BATTERY_MS 5000
#define HA_VOLUME_POLL_INTERVAL_SLEEPING_MS 30000
#define HA_VOLUME_POLL_INTERVAL_VOICE_MS 1000
#define HA_VOLUME_POLL_TASK_STACK 4096

static uint32_t poll_interval_ms(void) {
    if (voice_client_wants_fast_poll()) {
        return HA_VOLUME_POLL_INTERVAL_VOICE_MS;  // clear the red mic promptly
    }
    if (platform_display_is_sleeping()) {
        return HA_VOLUME_POLL_INTERVAL_SLEEPING_MS;
    }
    if (platform_battery_is_charging()) {
        return HA_VOLUME_POLL_INTERVAL_AWAKE_CHARGING_MS;
    }
    return HA_VOLUME_POLL_INTERVAL_AWAKE_BATTERY_MS;
}

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

/* input_boolean.audio_mute's current value, polled in the same cycle as
 * volume/source above - see ha_volume_client_get_muted's header comment.
 * ha_mute_client.c sets s_muted optimistically the instant it fires a
 * toggle (see ha_volume_client_set_muted_optimistic), so the mute screen
 * appears immediately rather than lagging behind by up to one poll
 * interval; poll_mute_once() below then reconciles it with the real
 * state every cycle regardless of who changed it (this dial, Harmony,
 * the HA dashboard). */
static bool s_muted;

static int32_t s_pending_ticks;    // accumulated, not-yet-sent raw ticks
static uint64_t s_last_tick_ms;    // when a tick last landed in the burst

static void flush_task(void *arg);

static float clamp_position(float value, float max) {
    if (value < 0.0f) {
        return 0.0f;
    }
    if (value > max) {
        return max;
    }
    return value;
}

static int db_to_position(float db) {
    float position = roundf((db + 127.5f) * 2.0f);
    return (int)clamp_position(position, 255.0f);
}

// Lounge's number.hifi_volume is a dB value converted to a 0-255 position
// (db_to_position above). Dining's sensor.venu360_main_gain_dial_2 is
// already the position itself, 0-120 (121 discrete 0.5dB steps over
// -60..0dB - see script.venu360_gain_controller server-side; the firmware
// doesn't need to know that mapping, only the position range it reports).
static float room_volume_max(void) {
    return room_cfg_get_current() == RK_ROOM_DINING ? 120.0f : 255.0f;
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
    bool changed = !s_have_source || strcmp(s_current_source, source) != 0;
    rk_strlcpy(s_current_source, source, sizeof(s_current_source));
    s_have_source = true;
    os_mutex_unlock(&s_lock);
    if (changed) {
        LOGI("Source is now '%s'", source);
    }
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

void ha_volume_client_set_current_source_optimistic(const char *source) {
    if (!source) {
        return;
    }
    set_cached_source(source);
}

bool ha_volume_client_get_muted(void) {
    os_mutex_lock(&s_lock);
    bool muted = s_muted;
    os_mutex_unlock(&s_lock);
    return muted;
}

void ha_volume_client_set_muted_optimistic(bool muted) {
    os_mutex_lock(&s_lock);
    s_muted = muted;
    os_mutex_unlock(&s_lock);
}

static bool poll_once(const rk_ha_cfg_t *cfg) {
    bool dining = room_cfg_get_current() == RK_ROOM_DINING;
    char url[128];
    snprintf(url, sizeof(url), "http://%s/api/states/%s", cfg->host,
             dining ? "sensor.venu360_main_gain_dial_2" : "number.hifi_volume");

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
        float raw = strtof(state->valuestring, &end);
        if (end != state->valuestring) {
            float max = room_volume_max();
            int position = dining ? (int)clamp_position(roundf(raw), max)
                                   : db_to_position(raw);
            set_cached_position(position);
            controller_presentation_set_volume_range((float)position, 0.0f,
                                                      max, 1.0f);
            ok = true;
        }
    }
    cJSON_Delete(root);
    return ok;
}

static bool poll_source_once(const rk_ha_cfg_t *cfg) {
    bool dining = room_cfg_get_current() == RK_ROOM_DINING;
    char url[128];
    snprintf(url, sizeof(url), "http://%s/api/states/%s", cfg->host,
             dining ? "sensor.venu360_inputs" : "input_select.audio_input");

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

static bool poll_mute_once(const rk_ha_cfg_t *cfg) {
    bool dining = room_cfg_get_current() == RK_ROOM_DINING;
    char url[128];
    snprintf(url, sizeof(url), "http://%s/api/states/%s", cfg->host,
             dining ? "switch.venu360_main_mute" : "input_boolean.audio_mute");

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
        os_mutex_lock(&s_lock);
        s_muted = (strcmp(state->valuestring, "on") == 0);
        os_mutex_unlock(&s_lock);
        ok = true;
    }
    cJSON_Delete(root);
    return ok;
}

static volatile bool s_poll_now;

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
            if (!poll_mute_once(&cfg)) {
                LOGW("HA mute poll failed (host='%s')", cfg.host);
            }
            {
                char src[HA_CURRENT_SOURCE_MAX];
                bool on_vinyl = ha_volume_client_get_current_source(src, sizeof(src)) &&
                                strcmp(src, "Vinyl") == 0;
                vinyl_client_poll(cfg.host, on_vinyl);
            }
            voice_client_poll(&cfg);
        }
        // Sleep in short slices so ha_volume_client_poll_now() can cut it short.
        uint32_t remaining = poll_interval_ms();
        while (remaining > 0 && !s_poll_now) {
            uint32_t step = remaining < 100 ? remaining : 100;
            platform_sleep_ms(step);
            remaining -= step;
        }
        s_poll_now = false;
    }
}

void ha_volume_client_poll_now(void) {
    s_poll_now = true;
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

// script.venu360_gain_controller has no "batch of N clicks" call shape like
// audio_voice_volume - it only accepts one {"event":"step","delta":±1} per
// invocation (real hardware step sequence, not a value write). So a burst
// of N clicks means N individual HTTP round trips here, not one - this
// only runs from flush_task's background task, so the extra latency isn't
// on the input path.
static bool send_steps(const rk_ha_cfg_t *cfg, int32_t clicks) {
    if (clicks == 0) {
        return true;
    }
    int32_t magnitude = clicks < 0 ? -clicks : clicks;
    int delta = clicks > 0 ? 1 : -1;

    char url[128];
    snprintf(url, sizeof(url),
             "http://%s/api/services/script/venu360_gain_controller",
             cfg->host);
    char body[48];
    snprintf(body, sizeof(body), "{\"event\":\"step\",\"delta\":%d}", delta);

    bool ok = true;
    for (int32_t i = 0; i < magnitude; i++) {
        char *resp = NULL;
        size_t resp_len = 0;
        int ret =
            platform_http_post_auth(url, cfg->token, body, &resp, &resp_len);
        platform_http_free(resp);
        if (ret != 0) {
            LOGW("HA volume adjust: venu360_gain_controller step call "
                 "failed");
            ok = false;
        }
    }
    return ok;
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
        if (room_cfg_get_current() == RK_ROOM_DINING) {
            (void)send_steps(&cfg, clicks);
        } else {
            (void)send_clicks(&cfg, clicks);
        }
    }
}

static char s_pending_script[64];
static ha_script_done_fn_t s_pending_script_done;

bool ha_volume_client_call_script_async(const char *script_entity_id,
                                        ha_script_done_fn_t done) {
    if (!script_entity_id || !script_entity_id[0] ||
        strlen(script_entity_id) >= sizeof(s_pending_script)) {
        return false;
    }
    os_mutex_lock(&s_lock);
    bool ok = s_configured && s_network_ready && s_pending_script[0] == '\0';
    if (ok) {
        memcpy(s_pending_script, script_entity_id, strlen(script_entity_id) + 1);
        s_pending_script_done = done;
    }
    os_mutex_unlock(&s_lock);
    return ok;
}

static void run_pending_script(void) {
    char script[sizeof(s_pending_script)];
    ha_script_done_fn_t done;
    rk_ha_cfg_t cfg;
    os_mutex_lock(&s_lock);
    if (s_pending_script[0] == '\0') {
        os_mutex_unlock(&s_lock);
        return;
    }
    memcpy(script, s_pending_script, sizeof(script));
    done = s_pending_script_done;
    s_pending_script[0] = '\0';
    s_pending_script_done = NULL;
    cfg = s_cfg;
    os_mutex_unlock(&s_lock);

    char url[128];
    snprintf(url, sizeof(url), "http://%s/api/services/script/turn_on", cfg.host);
    char body[96];
    snprintf(body, sizeof(body), "{\"entity_id\":\"%s\"}", script);
    char *resp = NULL;
    size_t resp_len = 0;
    int ret = platform_http_post_auth(url, cfg.token, body, &resp, &resp_len);
    platform_http_free(resp);
    if (done) {
        done(ret == 0);
    }
}

static void flush_task(void *arg) {
    (void)arg;
    while (true) {
        platform_sleep_ms(HA_VOLUME_FLUSH_POLL_MS);
        run_pending_script();
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

    /* Turning the knob while muted un-mutes first, same as most physical
     * amps/receivers - the user turning it is clearly not trying to stay
     * muted. ha_mute_client_toggle() reads the current cached mute state
     * itself and flips it, so this only ever un-mutes here (never
     * re-mutes: get_muted() is false immediately after). Best-effort -
     * ignore failure and adjust the volume anyway rather than leaving the
     * knob unresponsive because the unmute call happened to fail. */
    if (ha_volume_client_get_muted()) {
        (void)ha_mute_client_toggle();
    }

    /* Optimistic display update happens immediately, using the full raw
     * tick count (matching the eventual click count 1:1 while
     * HA_VOLUME_TICKS_PER_CLICK stays 1) - only the actual HA call is
     * debounced, so the dial still feels instantly responsive even
     * though the network write lags slightly behind a fast spin. */
    int new_position = get_cached_position() + ticks / HA_VOLUME_TICKS_PER_CLICK;
    int max_position = (int)room_volume_max();
    if (new_position < 0) {
        new_position = 0;
    } else if (new_position > max_position) {
        new_position = max_position;
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
        *volume_max = room_volume_max();
    }
    if (volume_step) {
        *volume_step = 1.0f;
    }
}
