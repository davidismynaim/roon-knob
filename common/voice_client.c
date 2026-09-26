#include "voice_client.h"

#include "ha_volume_client.h"
#include "os_mutex.h"
#include "platform/platform_display.h"
#include "platform/platform_http.h"
#include "platform/platform_log.h"
#include "platform/platform_task.h"
#include "platform/platform_time.h"
#include "room_cfg.h"
#include "ui.h"

#include <cJSON.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

// Per room: a script that starts the room's satellite listening, and a sensor
// mirrored from that satellite's listening/idle events.
static const char *voice_script(void) {
    return room_cfg_get_current() == RK_ROOM_DINING ? "script.dining_voice_listen"
                                                    : "script.lounge_voice_listen";
}

static const char *voice_state_entity(void) {
    return room_cfg_get_current() == RK_ROOM_DINING ? "binary_sensor.dining_voice_active"
                                                    : "binary_sensor.lounge_voice_active";
}
// The satellite goes listening a moment after the request (it plays its prompt
// first). If it never does, stop showing the mic.
#define VOICE_REQUEST_GRACE_MS 12000
// Safety net if the "ended" event is ever missed: never show the mic forever.
#define VOICE_ACTIVE_MAX_MS 120000

typedef enum { VOICE_IDLE, VOICE_REQUESTED, VOICE_ACTIVE } voice_state_t;

static os_mutex_t s_lock = OS_MUTEX_INITIALIZER;
static voice_state_t s_state = VOICE_IDLE;
static uint64_t s_since_ms;
static bool s_shown;  // what the UI was last told

bool voice_client_active(void) {
    os_mutex_lock(&s_lock);
    bool active = s_state != VOICE_IDLE;
    os_mutex_unlock(&s_lock);
    return active;
}

bool voice_client_wants_fast_poll(void) {
    return voice_client_active();
}

static void apply_on_ui(void *arg) {
    ui_set_voice_active((bool)(intptr_t)arg);
}

// Tell the UI only when the shown state actually changes.
static void sync_ui(bool wanted) {
    os_mutex_lock(&s_lock);
    bool changed = wanted != s_shown;
    s_shown = wanted;
    os_mutex_unlock(&s_lock);
    if (changed) {
        (void)platform_task_post_to_ui(apply_on_ui, (void *)(intptr_t)wanted);
    }
}

static void request_done(bool ok) {
    if (ok) {
        return;
    }
    LOGW("Voice: HA script call failed");
    os_mutex_lock(&s_lock);
    if (s_state == VOICE_REQUESTED) {
        s_state = VOICE_IDLE;
    }
    bool wanted = s_state != VOICE_IDLE;
    os_mutex_unlock(&s_lock);
    sync_ui(wanted);
}

bool voice_client_request_listen(void) {
    if (!ha_volume_client_is_active()) {
        return false;
    }
    os_mutex_lock(&s_lock);
    if (s_state != VOICE_IDLE) {
        os_mutex_unlock(&s_lock);
        return false;
    }
    s_state = VOICE_REQUESTED;
    s_since_ms = platform_millis();
    s_shown = true;
    os_mutex_unlock(&s_lock);

    ui_set_voice_active(true);  // already on the UI thread: instant feedback
    if (!ha_volume_client_call_script_async(voice_script(), request_done)) {
        request_done(false);
        return false;
    }
    return true;
}

// `known`: whether the sensor could be read this cycle; `on`: its value.
static void reconcile(bool known, bool on) {
    uint64_t now = platform_millis();
    os_mutex_lock(&s_lock);
    if (known && on) {
        if (s_state != VOICE_ACTIVE) {
            s_state = VOICE_ACTIVE;
            s_since_ms = now;
        }
    } else if (known && s_state == VOICE_ACTIVE) {
        s_state = VOICE_IDLE;  // conversation ended: HA says idle again
    }
    if (s_state == VOICE_REQUESTED && now - s_since_ms > VOICE_REQUEST_GRACE_MS) {
        s_state = VOICE_IDLE;
    }
    if (s_state == VOICE_ACTIVE && now - s_since_ms > VOICE_ACTIVE_MAX_MS) {
        s_state = VOICE_IDLE;
    }
    bool wanted = s_state != VOICE_IDLE;
    os_mutex_unlock(&s_lock);
    sync_ui(wanted);
}

void voice_client_poll(const rk_ha_cfg_t *cfg) {
    if (!cfg) {
        return;
    }
    // The red mic is only visible with the screen on; skip the extra GET
    // while it sleeps (the state is read again on the first poll after wake).
    if (platform_display_is_sleeping()) {
        return;
    }

    char url[128];
    snprintf(url, sizeof(url), "http://%s/api/states/%s", cfg->host, voice_state_entity());
    char *resp = NULL;
    size_t resp_len = 0;
    if (platform_http_get_auth(url, cfg->token, &resp, &resp_len) != 0 || !resp) {
        platform_http_free(resp);
        reconcile(false, false);
        return;
    }
    cJSON *root = cJSON_Parse(resp);
    platform_http_free(resp);
    cJSON *state = root ? cJSON_GetObjectItemCaseSensitive(root, "state") : NULL;
    bool known = cJSON_IsString(state) && state->valuestring;
    bool on = known && strcmp(state->valuestring, "on") == 0;
    // "unknown"/"unavailable" (e.g. before the automation has ever fired) count
    // as not listening rather than as an unreadable sensor.
    reconcile(true, on);
    cJSON_Delete(root);
}
