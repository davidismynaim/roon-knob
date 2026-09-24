#include "vinyl_client.h"

#include "controller_presentation.h"
#include "controller_view_compat.h"
#include "os_mutex.h"
#include "platform/platform_http.h"
#include "platform/platform_log.h"
#include "platform/platform_task.h"
#include "room_cfg.h"

#include <cJSON.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define VINYL_SERVICE_PORT 8099
#define VINYL_TEXT_MAX 128
#define VINYL_KEY_MAX 40
// A failed request tolerates a couple of misses before falling back to the
// static Vinyl picture, so one dropped packet doesn't flicker the screen.
#define VINYL_FAIL_TOLERANCE 3

typedef struct {
    char title[VINYL_TEXT_MAX];
    char artist[VINYL_TEXT_MAX];
    char album[VINYL_TEXT_MAX];
    char next_title[VINYL_TEXT_MAX];
    char next_artist[VINYL_TEXT_MAX];
    char image_key[VINYL_KEY_MAX];
    int seek;
    int length;
    int year;
    bool next_none;
} vinyl_track_t;

static os_mutex_t s_lock = OS_MUTEX_INITIALIZER;
static bool s_showing;        // service has a recognised track
static bool s_content_ready;  // ...and it has been handed to the UI
static bool s_owns;           // Vinyl is the source (Lounge): Roon media is suppressed
static int s_fail_count;
static char s_host[64];

bool vinyl_client_host_from_ha(const char *ha_host, char *out, size_t len) {
    if (!ha_host || !out || len == 0) {
        return false;
    }
    size_t n = 0;
    while (ha_host[n] && ha_host[n] != ':' && ha_host[n] != '/') {
        n++;
    }
    if (n == 0 || n >= len) {
        return false;
    }
    memcpy(out, ha_host, n);
    out[n] = '\0';
    return true;
}

bool vinyl_client_showing(void) {
    os_mutex_lock(&s_lock);
    bool showing = s_showing;
    os_mutex_unlock(&s_lock);
    return showing;
}

bool vinyl_client_content_ready(void) {
    os_mutex_lock(&s_lock);
    bool ready = s_showing && s_content_ready;
    os_mutex_unlock(&s_lock);
    return ready;
}

bool vinyl_client_owns_media(void) {
    os_mutex_lock(&s_lock);
    bool owns = s_owns;
    os_mutex_unlock(&s_lock);
    return owns;
}

// Blank the Music-layout widgets when the vinyl feed goes away (UI task), so
// nothing stale - and, being Vinyl, never Roon's track - can show through
// (the detail view, for instance, is independent of which screen is showing).
static void clear_content_on_ui(void *arg) {
    (void)arg;
    controller_presentation_set_artwork("");
    controller_presentation_update("", "", "", false, 0.0f, 0.0f, 255.0f, 1.0f, 0, 0);
    controller_presentation_set_media_enrichment("", "", 0, "", false);
}

// Hand the media display back to Roon on leaving the Vinyl source (UI task).
// The Roon path only reloads artwork when Roon's own artwork reference changes,
// so without this the vinyl cover stayed up next to Roon's text.
static void hand_back_to_roon(void *arg) {
    (void)arg;
    controller_presentation_set_artwork("");
    controller_view_compat_reset();
}

static void set_showing(bool showing) {
    os_mutex_lock(&s_lock);
    bool was_showing = s_showing;
    s_showing = showing;
    if (!showing) {
        s_content_ready = false;
    } else {
        s_fail_count = 0;
    }
    os_mutex_unlock(&s_lock);
    if (was_showing != showing) {
        LOGI("Vinyl feed %s", showing ? "on (track recognised)" : "off");
    }
    if (was_showing && !showing) {
        (void)platform_task_post_to_ui(clear_content_on_ui, NULL);
    }
}

bool vinyl_client_artwork_url(char *url, size_t len, int width, int height) {
    if (!url || len == 0) {
        return false;
    }
    os_mutex_lock(&s_lock);
    bool have_host = s_host[0] != '\0';
    char host[sizeof(s_host)];
    memcpy(host, s_host, sizeof(host));
    os_mutex_unlock(&s_lock);
    if (!have_host) {
        return false;
    }
    snprintf(url, len,
             "http://%s:%d/now_playing/image?scale=fit&width=%d&height=%d&format=rgb565",
             host, VINYL_SERVICE_PORT, width, height);
    return true;
}

bool vinyl_client_swallow_command(const controller_command_t *command) {
    if (!command || command->kind == CONTROLLER_COMMAND_ADJUST_VOLUME_STEPS) {
        return false;
    }
    return vinyl_client_owns_media();
}

static void copy_string(cJSON *root, const char *key, char *out, size_t len) {
    out[0] = '\0';
    cJSON *item = cJSON_GetObjectItemCaseSensitive(root, key);
    if (cJSON_IsString(item) && item->valuestring) {
        snprintf(out, len, "%s", item->valuestring);
    }
}

static int number_or_zero(cJSON *root, const char *key) {
    cJSON *item = cJSON_GetObjectItemCaseSensitive(root, key);
    return cJSON_IsNumber(item) ? (int)item->valuedouble : 0;
}

// Returns true when the response describes a playing track; each field beyond
// the title is optional and simply left empty when absent.
static bool parse_now_playing(const char *json, vinyl_track_t *out) {
    memset(out, 0, sizeof(*out));
    cJSON *root = cJSON_Parse(json);
    if (!root) {
        return false;
    }
    copy_string(root, "line1", out->title, sizeof(out->title));
    copy_string(root, "line2", out->artist, sizeof(out->artist));
    copy_string(root, "line3", out->album, sizeof(out->album));
    copy_string(root, "image_key", out->image_key, sizeof(out->image_key));
    copy_string(root, "next_track_title", out->next_title, sizeof(out->next_title));
    copy_string(root, "next_track_artist", out->next_artist, sizeof(out->next_artist));
    out->seek = number_or_zero(root, "seek_position");
    out->length = number_or_zero(root, "length");
    out->year = number_or_zero(root, "album_year");
    cJSON *none = cJSON_GetObjectItemCaseSensitive(root, "next_none");
    out->next_none = cJSON_IsTrue(none);
    cJSON *playing = cJSON_GetObjectItemCaseSensitive(root, "is_playing");
    bool is_playing = cJSON_IsTrue(playing);
    cJSON_Delete(root);
    return is_playing && out->title[0] != '\0';
}

// Runs on the UI task, like every other presentation update: the same calls
// the bridge path makes for a Roon track, fed from the recognised one.
static void apply_on_ui(void *arg) {
    vinyl_track_t *track = arg;
    if (track && vinyl_client_showing()) {
        // Volume args are replaced by Home Assistant's own value inside the
        // Dial presentation, exactly as for the Roon path.
        controller_presentation_update(track->title, track->artist, track->album,
                                       true, 0.0f, 0.0f, 255.0f, 1.0f,
                                       track->seek, track->length);
        controller_presentation_set_artwork(track->image_key);
        controller_presentation_set_media_enrichment(
            track->next_title, track->next_artist, track->year, "Analogue",
            track->next_none);
        os_mutex_lock(&s_lock);
        s_content_ready = true;
        os_mutex_unlock(&s_lock);
    }
    free(track);
}

void vinyl_client_poll(const char *ha_host, bool source_is_vinyl) {
    bool owns = source_is_vinyl && room_cfg_get_current() == RK_ROOM_LOUNGE;
    os_mutex_lock(&s_lock);
    bool was_owns = s_owns;
    s_owns = owns;
    os_mutex_unlock(&s_lock);
    if (!owns) {
        if (vinyl_client_showing()) {
            set_showing(false);
        }
        if (was_owns) {
            (void)platform_task_post_to_ui(hand_back_to_roon, NULL);
        }
        return;
    }

    char host[sizeof(s_host)];
    if (!vinyl_client_host_from_ha(ha_host, host, sizeof(host))) {
        return;
    }
    os_mutex_lock(&s_lock);
    memcpy(s_host, host, sizeof(s_host));
    os_mutex_unlock(&s_lock);

    char url[96];
    snprintf(url, sizeof(url), "http://%s:%d/now_playing", host, VINYL_SERVICE_PORT);
    char *resp = NULL;
    size_t resp_len = 0;
    // Plain GET on purpose: nothing here may carry the Home Assistant token.
    int ret = platform_http_get(url, &resp, &resp_len);
    if (ret != 0 || !resp) {
        platform_http_free(resp);
        os_mutex_lock(&s_lock);
        bool drop = s_showing && ++s_fail_count >= VINYL_FAIL_TOLERANCE;
        os_mutex_unlock(&s_lock);
        if (drop) {
            LOGW("Vinyl service unreachable; back to the static Vinyl screen");
            set_showing(false);
        }
        return;
    }

    vinyl_track_t *track = malloc(sizeof(*track));
    if (!track) {
        platform_http_free(resp);
        return;
    }
    bool has_track = parse_now_playing(resp, track);
    platform_http_free(resp);
    if (!has_track) {
        free(track);
        if (vinyl_client_showing()) {
            set_showing(false);  // service says nothing is playing
        }
        return;
    }
    set_showing(true);
    if (!platform_task_post_to_ui(apply_on_ui, track)) {
        free(track);
    }
}
