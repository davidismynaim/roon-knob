// HTTP config server - runs when connected to WiFi for remote configuration
// Access at http://<knob-ip>/ to set bridge URL

#include "config_server.h"
#include "controller_config.h"
#include "display_sleep.h"
#include "haptic_driver.h"
#include "http_server_lifecycle.h"
#include "platform/platform_mdns.h"
#include "platform/platform_storage.h"
#include "bridge_client.h"
#include "rk_ble_hid_host.h"
#include "room_cfg.h"
#include "wifi_manager.h"

#include <stdlib.h>
#include <string.h>
#include <esp_err.h>
#include <esp_heap_caps.h>
#include <esp_log.h>
#include <esp_http_server.h>
#include <esp_system.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

static const char *TAG = "config_server";

static httpd_handle_t s_server = NULL;

static esp_err_t send_unverified_settings(httpd_req_t *req) {
    httpd_resp_set_status(req, "503 Service Unavailable");
    httpd_resp_set_type(req, "text/plain");
    httpd_resp_sendstr(req,
        "Settings saved but could not be verified. No restart was performed; please try again.");
    return ESP_FAIL;
}

static void apply_committed_wifi(bool reconnect) {
    controller_config_wifi_snapshot_t wifi = {0};
    if (controller_config_wifi_snapshot(&wifi)) {
        wifi_mgr_apply_wifi(&wifi, reconnect);
    }
}

static esp_err_t send_conflict(httpd_req_t *req, const char *message) {
    httpd_resp_set_status(req, "409 Conflict");
    httpd_resp_set_type(req, "text/plain");
    httpd_resp_sendstr(req,
                       message ? message : "Request conflicts with BLE state");
    return ESP_FAIL;
}

// HTML page for config
// Format args: current_bridge, status_class, status_text, wifi_html,
// bridge_value, ha_host, ha_token_placeholder, zone_options,
// escaped_title_patterns, haptic_checked, haptic_effect_options,
// haptic_cal_status, room_lounge_selected, room_dining_selected, boot_reason
static const char *HTML_CONFIG =
    "<!DOCTYPE html>"
    "<html><head>"
    "<meta name='viewport' content='width=device-width,initial-scale=1'>"
    "<title>HiPhi Dial Config</title>"
    "<style>"
    "body{font-family:sans-serif;margin:20px;background:#1a1a2e;color:#eee;}"
    "h1{color:#4fc3f7;margin-bottom:5px;}"
    "h2{color:#aaa;font-size:16px;margin-top:20px;}"
    ".info{color:#888;margin:10px 0;}"
    "form{background:#16213e;padding:20px;border-radius:10px;max-width:400px;}"
    "label{display:block;margin:15px 0 5px;color:#aaa;}"
    "input[type=text],input[type=url],input[type=password],select{width:100%%;padding:10px;border:1px solid #333;border-radius:5px;background:#0f0f1a;color:#fff;box-sizing:border-box;}"
    "input[type=submit]{padding:12px 24px;margin-top:20px;background:#4fc3f7;color:#000;border:none;border-radius:5px;font-weight:bold;cursor:pointer;}"
    "input[type=submit]:hover{background:#29b6f6;}"
    ".btn-clear{background:#ff7043;}"
    ".btn-clear:hover{background:#ff5722;}"
    ".btn-sm{padding:6px 12px;margin:0 0 0 10px;font-size:12px;}"
    ".current{background:#0f0f1a;padding:10px;border-radius:5px;margin:10px 0;font-family:monospace;}"
    ".status{padding:10px;border-radius:5px;margin:10px 0;}"
    ".status-ok{background:#1b5e20;}"
    ".status-warn{background:#e65100;}"
    ".status-err{background:#b71c1c;}"
    ".hint{font-size:12px;color:#666;margin-top:4px;}"
    ".success{background:#2e7d32;padding:15px;border-radius:5px;margin:15px 0;}"
    ".wifi-entry{background:#0f0f1a;padding:8px 12px;border-radius:5px;margin:4px 0;display:flex;justify-content:space-between;align-items:center;max-width:400px;}"
    ".section{max-width:400px;}"
    "a{color:#4fc3f7;}"
    ".device{background:#0f0f1a;padding:10px;border-radius:5px;margin:8px 0;display:flex;justify-content:space-between;align-items:center;}"
    "</style></head><body>"
    "<h1>HiPhi Dial</h1>"
    "<p class='info'>Configure your HiPhi Dial settings</p>"
    "<p><a href='/ble'>BLE Media Remote settings</a></p>"
    "<div class='current'>"
    "<strong>Current Unified Hi-Fi Control:</strong> %s"
    "</div>"
    "<div class='status %s'>"
    "<strong>Status:</strong> %s"
    "</div>"
        "<h2>Saved WiFi Networks</h2>"
        "<div class='section'>%s</div>"
        "<p class='hint'>Saved-network changes take effect after restart.</p>"
    "<form method='POST' action='/wifi-add'>"
    "<h2>Add WiFi Network</h2>"
    "<label>SSID</label>"
    "<input type='text' name='ssid' maxlength='32' placeholder='Network name' required>"
    "<label>Password</label>"
        "<input type='password' name='pass' maxlength='64' placeholder='Password (optional)'>"
        "<p class='hint'>Up to two networks. Remove one before replacing it.</p>"
    "<input type='submit' value='Add Network'>"
    "</form>"
    "<form method='POST' action='/config'>"
    "<h2>Unified Hi-Fi Control Override</h2>"
    "<label>Unified Hi-Fi Control URL</label>"
    "<input type='url' name='bridge' maxlength='128' placeholder='http://192.168.1.x:8088' value='%s'>"
    "<p class='hint'>Leave empty for mDNS auto-discovery. Check the HiPhi Dial display for connection progress.</p>"
    "<input type='submit' value='Save'>"
    "<input type='submit' name='action' value='Clear' class='btn-clear' formnovalidate>"
    "</form>"
    "<form method='POST' action='/ha-config'>"
    "<h2>Home Assistant (Nexus Volume)</h2>"
    "<label>HA Host:Port</label>"
    "<input type='text' name='ha_host' maxlength='63' placeholder='192.168.1.x:8123' value='%s'>"
    "<label>Long-Lived Access Token</label>"
    "<input type='password' name='ha_token' maxlength='255' placeholder='%s'>"
    "<p class='hint'>Controls Nexus volume directly via Home Assistant, bypassing Roon. Leave the token blank to keep the one already saved.</p>"
    "<input type='submit' value='Save'>"
    "</form>"
    "<form method='POST' action='/zone-config'>"
    "<h2>Roon Zone</h2>"
    "<label>Zone</label>"
    "<select name='zone_id'>%s</select>"
    "<p class='hint'>Locked to this one zone &mdash; there's no on-device zone picker. Pick a different zone here if the dial is ever connected to the wrong one. If your zone isn't listed, the bridge may be unreachable right now &mdash; reload this page once it's back.</p>"
    "<input type='submit' value='Save'>"
    "</form>"
    "<form method='POST' action='/title-filter-config'>"
    "<h2>Track Title Cleanup</h2>"
    "<label>Patterns to strip (one per line)</label>"
    "<textarea name='patterns' rows='10' style='width:100%%;padding:10px;border:1px solid #333;border-radius:5px;background:#0f0f1a;color:#fff;box-sizing:border-box;font-family:monospace;font-size:12px;'>%s</textarea>"
    "<p class='hint'>One phrase per line, e.g. \"Remastered YYYY\" or \"Album Version\" &mdash; no need to add the brackets, dashes, or quotes yourself, matching handles those automatically. Write YYYY for a 4-digit year, YY for 2 digits, or NUM for any run of digits (e.g. bit depth/sample rate). Matching is plain text otherwise and not case-sensitive. Leave blank to disable cleanup entirely.</p>"
    "<input type='submit' value='Save'>"
    "</form>"
    "<form method='POST' action='/haptic-config'>"
    "<h2>Haptic Feedback</h2>"
    "<label><input type='checkbox' name='enabled' value='1' style='width:auto;display:inline;margin-right:8px;'%s>Enable haptic feedback</label>"
    "<label>Effect</label>"
    "<select name='effect_id'>%s</select>"
    "<p class='hint'>Vibrates briefly on play/pause/skip taps, the mute/source-picker long-press gestures, and picking an input from the source list. Not applied to volume changes &mdash; the encoder's own mechanical detents already give a good feel there. Effect names are the DRV2605 chip's own built-in library names, not ours &mdash; try a few and keep whichever feels best.</p>"
    "<p class='hint'>%s</p>"
    "<input type='submit' value='Save'>"
    "</form>"
    "<form method='POST' action='/room-config'>"
    "<h2>Installation</h2>"
    "<label>Room</label>"
    "<select name='room'>"
    "<option value='lounge'%s>Lounge</option>"
    "<option value='dining'%s>Dining Room</option>"
    "</select>"
    "<p class='hint'>Which installation this dial talks to &mdash; Lounge (Roon-driven volume/source/mute) or Dining Room (dbx DriveRack VENU360). Changes which Home Assistant entities the dial's volume knob, mute, and source picker use, and hides the TV source in Dining. Saving reboots the device.</p>"
    "<input type='submit' value='Save'>"
    "</form>"
    "<p class='hint'>Last boot: %s</p>"
    "</body></html>";

static const char *HTML_SUCCESS =
    "<!DOCTYPE html>"
    "<html><head>"
    "<meta name='viewport' content='width=device-width,initial-scale=1'>"
    "<title>Saved</title>"
    "<style>"
    "body{font-family:sans-serif;margin:20px;background:#1a1a2e;color:#eee;text-align:center;}"
    "h1{color:#4fc3f7;}"
    ".success{background:#2e7d32;padding:20px;border-radius:10px;max-width:300px;margin:20px auto;}"
    ".info{background:#16213e;padding:15px;border-radius:10px;max-width:300px;margin:20px auto;}"
    "</style></head><body>"
    "<h1>HiPhi Dial</h1>"
    "<div class='success'>%s</div>"
    "<div class='info'>Device will reboot automatically to apply changes...</div>"
    "</body></html>";

// URL decode a string in place
static void url_decode(char *str) {
    char *src = str;
    char *dst = str;
    while (*src) {
        if (*src == '+') {
            *dst++ = ' ';
            src++;
        } else if (*src == '%' && src[1] && src[2]) {
            char hex[3] = {src[1], src[2], 0};
            *dst++ = (char)strtol(hex, NULL, 16);
            src += 3;
        } else {
            *dst++ = *src++;
        }
    }
    *dst = '\0';
}

// Parse form data to extract a field value
static bool get_form_field(const char *data, const char *field, char *out, size_t out_len) {
    char search[64];
    snprintf(search, sizeof(search), "%s=", field);

    const char *start = data;
    while ((start = strstr(start, search)) != NULL) {
        if (start == data || *(start - 1) == '&') {
            break;
        }
        start++;
    }
    if (!start) {
        return false;
    }
    start += strlen(search);

    const char *end = strchr(start, '&');
    size_t len = end ? (size_t)(end - start) : strlen(start);

    // URL-encoded data can be up to 3x the decoded length (e.g. ! -> %21).
    // Decode in a temporary buffer first, then truncate to fit the output.
    char encoded[256];
    if (len >= sizeof(encoded)) {
        len = sizeof(encoded) - 1;
    }

    memcpy(encoded, start, len);
    encoded[len] = '\0';
    url_decode(encoded);

    size_t decoded_len = strlen(encoded);
    if (decoded_len >= out_len) {
        decoded_len = out_len - 1;
    }
    memcpy(out, encoded, decoded_len);
    out[decoded_len] = '\0';
    return true;
}

// Like get_form_field, but for values that can be much larger than a
// typical form field (e.g. a multi-line textarea) - heap-allocates its
// decode scratch buffer sized exactly to the raw field length instead of
// truncating at get_form_field's fixed 256-byte cap. URL-decoding only
// ever shrinks or keeps text the same length, so `len` bytes is always
// enough room for the decoded result too.
static bool get_form_field_big(const char *data, const char *field, char *out, size_t out_len) {
    char search[64];
    snprintf(search, sizeof(search), "%s=", field);

    const char *start = data;
    while ((start = strstr(start, search)) != NULL) {
        if (start == data || *(start - 1) == '&') {
            break;
        }
        start++;
    }
    if (!start) {
        return false;
    }
    start += strlen(search);

    const char *end = strchr(start, '&');
    size_t len = end ? (size_t)(end - start) : strlen(start);

    char *encoded = heap_caps_malloc(len + 1, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!encoded) {
        return false;
    }
    memcpy(encoded, start, len);
    encoded[len] = '\0';
    url_decode(encoded);

    size_t decoded_len = strlen(encoded);
    if (decoded_len >= out_len) {
        decoded_len = out_len - 1;
    }
    memcpy(out, encoded, decoded_len);
    out[decoded_len] = '\0';
    free(encoded);
    return true;
}

static void html_escape(const char *src, char *dst, size_t dst_len) {
    size_t pos = 0;
    for (size_t i = 0; src && src[i] && pos + 1 < dst_len; i++) {
        const char *escaped = NULL;
        switch (src[i]) {
            case '&': escaped = "&amp;"; break;
            case '<': escaped = "&lt;"; break;
            case '>': escaped = "&gt;"; break;
            case '"': escaped = "&quot;"; break;
            case '\'': escaped = "&#39;"; break;
            default: break;
        }
        if (escaped) {
            size_t len = strlen(escaped);
            if (pos + len >= dst_len) break;
            memcpy(dst + pos, escaped, len);
            pos += len;
        } else {
            dst[pos++] = src[i];
        }
    }
    dst[pos] = '\0';
}

// Resolve .local hostname in URL to IP address via mDNS
// Modifies url in place if resolution succeeds
static void resolve_local_in_url(char *url, size_t url_len) {
    if (!url || !url[0]) return;

    // Check if URL contains .local
    char *local_pos = strstr(url, ".local");
    if (!local_pos) return;

    // Make sure it's actually the hostname suffix (followed by : or / or end)
    char after = local_pos[6];
    if (after != ':' && after != '/' && after != '\0') return;

    // Extract hostname: skip http://
    const char *host_start = strstr(url, "://");
    if (!host_start) return;
    host_start += 3;

    // Find end of hostname
    const char *host_end = host_start;
    while (*host_end && *host_end != ':' && *host_end != '/') host_end++;

    // Extract hostname
    size_t host_len = host_end - host_start;
    if (host_len == 0 || host_len >= 64) return;

    char hostname[64];
    memcpy(hostname, host_start, host_len);
    hostname[host_len] = '\0';

    // Resolve via mDNS
    char ip[16];
    if (!platform_mdns_resolve_local(hostname, ip, sizeof(ip))) {
        ESP_LOGW(TAG, "Could not resolve %s via mDNS", hostname);
        return;
    }

    // Build new URL with IP instead of hostname
    char new_url[128];
    size_t scheme_len = host_start - url;
    snprintf(new_url, sizeof(new_url), "%.*s%s%s", (int)scheme_len, url, ip, host_end);

    // Copy back if it fits
    if (strlen(new_url) < url_len) {
        strcpy(url, new_url);
        ESP_LOGI(TAG, "Resolved .local URL to: %s", url);
    }
}

// Handler for GET / - serve the config form
static esp_err_t config_get_handler(httpd_req_t *req) {
    ESP_LOGI(TAG, "Serving config page");

    controller_config_snapshot_t snapshot = {0};
    if (!controller_config_snapshot(&snapshot)) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR,
                            "Settings are unavailable");
        return ESP_FAIL;
    }
    const rk_cfg_t *cfg = &snapshot.value;

    const char *current = cfg->bridge_base[0] ? cfg->bridge_base : "(mDNS auto-discovery)";

    // Get bridge connection status
    const char *status_class;
    char status_text[64];
    bool bridge_connected = bridge_client_is_bridge_connected();
    int retry_count = bridge_client_get_bridge_retry_count();
    int retry_max = bridge_client_get_bridge_retry_max();

    if (bridge_connected) {
        status_class = "status-ok";
        snprintf(status_text, sizeof(status_text), "Connected");
    } else if (!cfg->bridge_base[0]) {
        status_class = "status-warn";
        snprintf(status_text, sizeof(status_text), "Searching via mDNS...");
    } else if (retry_count >= retry_max) {
        status_class = "status-err";
        snprintf(status_text, sizeof(status_text),
                 "Unreachable - check Unified Hi-Fi Control");
    } else if (retry_count > 0) {
        status_class = "status-warn";
        snprintf(status_text, sizeof(status_text), "Connecting... (%d/%d)", retry_count, retry_max);
    } else {
        status_class = "status-warn";
        snprintf(status_text, sizeof(status_text), "Connecting...");
    }

    // The HTTP server task's stack is only 8KB (see config.stack_size
    // below) - these used to be stack arrays, and large ones (the 16-entry
    // zone list plus its rendered <option> HTML) pushed this handler over
    // that budget, causing a stack-overflow panic (full chip reboot,
    // dropping WiFi) on every page load once the zone dropdown was added.
    // PSRAM-backed heap allocations instead, matching the existing `html`
    // buffer below.
    char *wifi_html = heap_caps_malloc(1024, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    bridge_zone_t *zones =
        heap_caps_malloc(16 * sizeof(bridge_zone_t),
                         MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    char *zone_options = heap_caps_malloc(3072, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!wifi_html || !zones || !zone_options) {
        free(wifi_html);
        free(zones);
        free(zone_options);
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Out of memory");
        return ESP_FAIL;
    }
    wifi_html[0] = '\0';
    zone_options[0] = '\0';

    size_t wifi_pos = 0;
    for (int i = 0; i < cfg->wifi_count && i < RK_MAX_WIFI; i++) {
        char escaped_ssid[192];
        html_escape(cfg->wifi[i].ssid, escaped_ssid, sizeof(escaped_ssid));
        int written = snprintf(
            wifi_html + wifi_pos, 1024 - wifi_pos,
            "<div class='wifi-entry'><span>%d. %s</span>"
            "<form method='POST' action='/wifi-remove' style='display:inline;margin:0;padding:0;'>"
            "<input type='hidden' name='idx' value='%d'>"
            "<input type='submit' value='Remove' class='btn-sm btn-clear'>"
            "</form></div>",
            i + 1, escaped_ssid, i);
        if (written < 0 || (size_t)written >= 1024 - wifi_pos) {
            break;
        }
        wifi_pos += (size_t)written;
    }
    if (wifi_pos == 0) {
        snprintf(wifi_html, 1024,
                 "<div class='wifi-entry'><em>No saved networks</em></div>");
    }

    rk_ha_cfg_t ha_cfg = {0};
    platform_storage_read_ha(&ha_cfg);
    const char *ha_token_placeholder =
        ha_cfg.token[0] ? "(unchanged)" : "Paste token here";

    // rk_title_filter_cfg_t is 4KB+ (RK_TITLE_FILTER_PATTERNS_MAX patterns
    // buffer) - same PSRAM-heap treatment as wifi_html/zones/zone_options
    // above, not a stack local; this handler's task stack is only 8KB.
    rk_title_filter_cfg_t *title_cfg =
        heap_caps_malloc(sizeof(*title_cfg), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    char *escaped_patterns = heap_caps_malloc(6144, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!title_cfg || !escaped_patterns) {
        free(wifi_html);
        free(zones);
        free(zone_options);
        free(title_cfg);
        free(escaped_patterns);
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Out of memory");
        return ESP_FAIL;
    }
    platform_storage_read_title_filters(title_cfg);
    html_escape(title_cfg->patterns, escaped_patterns, 6144);
    free(title_cfg);

    // Build the zone <option> list from the bridge's live zone list - the
    // same target-neutral zone API frame_app/main/captive_portal.c already
    // uses for its own zone UI. The currently-configured zone_id always
    // gets an option first, even if the live list didn't return it (e.g.
    // the bridge is briefly unreachable), so the current selection is
    // never silently lost or left unselected.
    int zone_count = bridge_client_get_zones(zones, 16);
    size_t zone_opt_pos = 0;
    {
        char escaped_id[192];
        char escaped_name[128];
        html_escape(cfg->zone_id, escaped_id, sizeof(escaped_id));
        const char *current_name = cfg->zone_id[0] ? cfg->zone_id
                                                    : "(not connected yet)";
        for (int i = 0; i < zone_count; i++) {
            if (strcmp(zones[i].id, cfg->zone_id) == 0) {
                current_name = zones[i].name;
                break;
            }
        }
        html_escape(current_name, escaped_name, sizeof(escaped_name));
        int written = snprintf(zone_options + zone_opt_pos,
                               3072 - zone_opt_pos,
                               "<option value='%s' selected>%s</option>",
                               escaped_id, escaped_name);
        if (written > 0 && (size_t)written < 3072 - zone_opt_pos) {
            zone_opt_pos += (size_t)written;
        }

        for (int i = 0; i < zone_count; i++) {
            if (strcmp(zones[i].id, cfg->zone_id) == 0) {
                continue;  // already listed above as the current selection
            }
            html_escape(zones[i].id, escaped_id, sizeof(escaped_id));
            html_escape(zones[i].name, escaped_name, sizeof(escaped_name));
            written = snprintf(zone_options + zone_opt_pos,
                               3072 - zone_opt_pos,
                               "<option value='%s'>%s</option>", escaped_id,
                               escaped_name);
            if (written < 0 || (size_t)written >= 3072 - zone_opt_pos) {
                break;
            }
            zone_opt_pos += (size_t)written;
        }
    }

    // rk_haptic_cfg_t is 3 bytes - unlike title_cfg above, genuinely safe
    // as a plain stack local (see rk_haptic_cfg.h).
    rk_haptic_cfg_t haptic_cfg = {0};
    platform_storage_read_haptic(&haptic_cfg);
    const char *haptic_checked = haptic_cfg.enabled ? " checked" : "";

    // Small, fixed-size list - a stack buffer is fine here, unlike the
    // zone/pattern lists above which can be arbitrarily large.
    char haptic_effect_options[1024] = "";
    size_t haptic_opt_pos = 0;
    size_t effect_count = 0;
    const haptic_effect_option_t *effects = haptic_driver_get_effect_options(&effect_count);
    for (size_t i = 0; i < effect_count; i++) {
        int written = snprintf(haptic_effect_options + haptic_opt_pos,
                               sizeof(haptic_effect_options) - haptic_opt_pos,
                               "<option value='%d'%s>%s</option>",
                               effects[i].effect_id,
                               effects[i].effect_id == haptic_cfg.effect_id ? " selected" : "",
                               effects[i].name);
        if (written < 0 || (size_t)written >= sizeof(haptic_effect_options) - haptic_opt_pos) {
            break;
        }
        haptic_opt_pos += (size_t)written;
    }
    // Calibration trigger (sentinel 210, see haptic_config_post_handler) -
    // also never pre-selected, a one-shot action like the diagnostics above.
    snprintf(haptic_effect_options + haptic_opt_pos,
             sizeof(haptic_effect_options) - haptic_opt_pos,
             "<option value='210'>RUN AUTO-CALIBRATION (felt, ~1s)</option>");

    char haptic_cal_status[160];
    if (haptic_cfg.calibrated) {
        snprintf(haptic_cal_status, sizeof(haptic_cal_status),
                 "Calibrated: yes (feedback=0x%02x compensation=0x%02x back_emf=0x%02x) &mdash; running closed-loop.",
                 haptic_cfg.cal_feedback, haptic_cfg.cal_compensation, haptic_cfg.cal_back_emf);
    } else {
        snprintf(haptic_cal_status, sizeof(haptic_cal_status),
                 "Calibrated: no &mdash; running open-loop. Pick \"RUN AUTO-CALIBRATION\" above and Save to calibrate (felt, ~1s, check serial log for the result).");
    }

    // Build HTML with current values, saved networks, and bridge status.
    char *html = heap_caps_malloc(16384,
                                  MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!html) {
        free(wifi_html);
        free(zones);
        free(zone_options);
        free(escaped_patterns);
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Out of memory");
        return ESP_FAIL;
    }

    bool dining = room_cfg_get_current() == RK_ROOM_DINING;
    const char *room_lounge_selected = dining ? "" : " selected";
    const char *room_dining_selected = dining ? " selected" : "";

    snprintf(html, 16384, HTML_CONFIG, current, status_class, status_text,
             wifi_html, cfg->bridge_base, ha_cfg.host, ha_token_placeholder,
             zone_options, escaped_patterns, haptic_checked, haptic_effect_options,
             haptic_cal_status, room_lounge_selected, room_dining_selected,
             display_boot_reason());

    httpd_resp_set_type(req, "text/html; charset=utf-8");
    httpd_resp_send(req, html, strlen(html));
    free(html);
    free(wifi_html);
    free(zones);
    free(zone_options);
    free(escaped_patterns);
    return ESP_OK;
}

// Handler for POST /config - save settings
static esp_err_t config_post_handler(httpd_req_t *req) {
    char buf[256] = {0};
    int received = httpd_req_recv(req, buf, sizeof(buf) - 1);

    if (received <= 0) {
        ESP_LOGE(TAG, "Failed to receive POST data");
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "No data received");
        return ESP_FAIL;
    }
    buf[received] = '\0';
    ESP_LOGI(TAG, "Received config: %s", buf);

    // Check if Clear button was pressed
    char action[16] = {0};
    get_form_field(buf, "action", action, sizeof(action));

    const char *message;
    char bridge_base[129] = {0};
    if (strcmp(action, "Clear") == 0) {
        message = "Unified Hi-Fi Control cleared! Will use mDNS.";
        ESP_LOGI(TAG, "Bridge URL cleared");
    } else {
        char bridge[129] = {0};
        get_form_field(buf, "bridge", bridge, sizeof(bridge));

        // Validate bridge URL format if provided
        if (bridge[0] && strncmp(bridge, "http://", 7) != 0) {
            httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST,
                "Invalid URL. Must start with http://");
            return ESP_FAIL;
        }

        rk_strlcpy(bridge_base, bridge, sizeof(bridge_base));

        // Resolve .local hostnames to IPs (ESP32 lwIP has issues with .local DNS)
        if (bridge[0]) {
            resolve_local_in_url(bridge_base, sizeof(bridge_base));
        }

        message = bridge_base[0] ? "Unified Hi-Fi Control URL saved!"
                                 : "Unified Hi-Fi Control cleared! Will use mDNS.";
        ESP_LOGI(TAG, "Bridge URL set to: %s", bridge_base[0] ? bridge_base : "(mDNS)");
    }

    controller_config_write_result_t result =
        controller_config_set_endpoint(bridge_base, false, NULL);
    if (result == CONTROLLER_CONFIG_NOT_COMMITTED) {
        ESP_LOGE(TAG, "Failed to save config");
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Failed to save");
        return ESP_FAIL;
    }
    if (result == CONTROLLER_CONFIG_COMMITTED_UNVERIFIED) {
        return send_unverified_settings(req);
    }

    // Send success response
    char *html = heap_caps_malloc(1024,
                                  MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!html) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Out of memory");
        return ESP_FAIL;
    }

    snprintf(html, 1024, HTML_SUCCESS, message);
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    httpd_resp_send(req, html, strlen(html));
    free(html);

    // Reboot to apply new config
    ESP_LOGI(TAG, "Config saved, rebooting in 1 second...");
    vTaskDelay(pdMS_TO_TICKS(1000));
    esp_restart();

    return ESP_OK;
}

// Handler for POST /ha-config - save Home Assistant volume-backend settings
// (direct Nexus volume control; see
// docs/meta/decisions/2026-09-14_DESIGN_HYBRID_DIAL_UI.md).
static esp_err_t ha_config_post_handler(httpd_req_t *req) {
    char buf[512] = {0};
    int received = httpd_req_recv(req, buf, sizeof(buf) - 1);
    if (received <= 0) {
        ESP_LOGE(TAG, "Failed to receive POST data");
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "No data received");
        return ESP_FAIL;
    }
    buf[received] = '\0';

    rk_ha_cfg_t cfg = {0};
    platform_storage_read_ha(&cfg);  // start from saved token so a blank field keeps it

    char host[64] = {0};
    get_form_field(buf, "ha_host", host, sizeof(host));
    const char *host_value = host;
    if (strncmp(host_value, "http://", 7) == 0) {
        host_value += 7;
    } else if (strncmp(host_value, "https://", 8) == 0) {
        host_value += 8;
    }
    rk_strlcpy(cfg.host, host_value, sizeof(cfg.host));

    char token[256] = {0};
    get_form_field(buf, "ha_token", token, sizeof(token));
    if (token[0]) {
        rk_strlcpy(cfg.token, token, sizeof(cfg.token));
    }
    cfg.cfg_ver = RK_HA_CFG_CURRENT_VER;

    if (!platform_storage_write_ha(&cfg)) {
        ESP_LOGE(TAG, "Failed to save HA config");
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Failed to save");
        return ESP_FAIL;
    }

    char *html = heap_caps_malloc(1024,
                                  MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!html) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Out of memory");
        return ESP_FAIL;
    }
    snprintf(html, 1024, HTML_SUCCESS, "Home Assistant settings saved!");
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    httpd_resp_send(req, html, strlen(html));
    free(html);

    ESP_LOGI(TAG, "HA config saved, rebooting in 1 second...");
    vTaskDelay(pdMS_TO_TICKS(1000));
    esp_restart();

    return ESP_OK;
}

// Handler for POST /zone-config - override the locked Roon zone (recovery
// path now that there's no on-device zone picker; see
// docs/meta/decisions/2026-09-14_DESIGN_HYBRID_DIAL_UI.md and
// CONFIG_RK_DEFAULT_ZONE_ID / refresh_zone_label() in bridge_client.c).
static esp_err_t zone_config_post_handler(httpd_req_t *req) {
    char buf[256] = {0};
    int received = httpd_req_recv(req, buf, sizeof(buf) - 1);
    if (received <= 0) {
        ESP_LOGE(TAG, "Failed to receive POST data");
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "No data received");
        return ESP_FAIL;
    }
    buf[received] = '\0';

    char zone_id[64] = {0};
    get_form_field(buf, "zone_id", zone_id, sizeof(zone_id));

    controller_config_write_result_t result =
        controller_config_set_zone(zone_id, NULL);
    if (result == CONTROLLER_CONFIG_NOT_COMMITTED) {
        ESP_LOGE(TAG, "Failed to save zone");
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Failed to save");
        return ESP_FAIL;
    }
    if (result == CONTROLLER_CONFIG_COMMITTED_UNVERIFIED) {
        return send_unverified_settings(req);
    }

    char *html = heap_caps_malloc(1024,
                                  MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!html) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Out of memory");
        return ESP_FAIL;
    }
    snprintf(html, 1024, HTML_SUCCESS, "Zone saved!");
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    httpd_resp_send(req, html, strlen(html));
    free(html);

    ESP_LOGI(TAG, "Zone config saved, rebooting in 1 second...");
    vTaskDelay(pdMS_TO_TICKS(1000));
    esp_restart();

    return ESP_OK;
}

// Handler for POST /title-filter-config - save the track-title cleanup
// pattern list (see common/track_title_filter.c / rk_title_filter_cfg.h).
static esp_err_t title_filter_config_post_handler(httpd_req_t *req) {
    // The textarea's raw + URL-encoded body can run well past a typical
    // form POST (patterns up to 4096 bytes, plus encoding overhead) - heap
    // buffer rather than a stack array, same reasoning as config_get_handler's
    // comment on the 8KB HTTP task stack. Also unlike this file's other POST
    // handlers, a body this size can easily span more than one TCP segment,
    // so read in a loop until the whole declared content length has arrived
    // instead of assuming a single httpd_req_recv() call covers it.
    const size_t buf_cap = 8192;
    char *buf = heap_caps_malloc(buf_cap, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!buf) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Out of memory");
        return ESP_FAIL;
    }
    size_t want = (req->content_len > 0 && (size_t)req->content_len < buf_cap - 1)
                      ? (size_t)req->content_len
                      : buf_cap - 1;
    size_t received = 0;
    while (received < want) {
        int r = httpd_req_recv(req, buf + received, want - received);
        if (r == HTTPD_SOCK_ERR_TIMEOUT) {
            continue;
        }
        if (r <= 0) {
            free(buf);
            ESP_LOGE(TAG, "Failed to receive POST data");
            httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "No data received");
            return ESP_FAIL;
        }
        received += (size_t)r;
    }
    buf[received] = '\0';

    // Heap, not a stack local - same 4KB+ sizing concern as
    // config_get_handler's title_cfg above.
    rk_title_filter_cfg_t *cfg =
        heap_caps_malloc(sizeof(*cfg), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!cfg) {
        free(buf);
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Out of memory");
        return ESP_FAIL;
    }
    memset(cfg, 0, sizeof(*cfg));
    cfg->cfg_ver = RK_TITLE_FILTER_CFG_CURRENT_VER;
    if (!get_form_field_big(buf, "patterns", cfg->patterns, sizeof(cfg->patterns))) {
        cfg->patterns[0] = '\0';
    }
    free(buf);

    bool saved = platform_storage_write_title_filters(cfg);
    free(cfg);
    if (!saved) {
        ESP_LOGE(TAG, "Failed to save title filter config");
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Failed to save");
        return ESP_FAIL;
    }

    char *html = heap_caps_malloc(1024,
                                  MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!html) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Out of memory");
        return ESP_FAIL;
    }
    snprintf(html, 1024, HTML_SUCCESS, "Track title cleanup patterns saved!");
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    httpd_resp_send(req, html, strlen(html));
    free(html);

    ESP_LOGI(TAG, "Title filter config saved, rebooting in 1 second...");
    vTaskDelay(pdMS_TO_TICKS(1000));
    esp_restart();

    return ESP_OK;
}

// Handler for POST /haptic-config - save the haptic feedback on/off
// preference and selected effect (see idf_app/main/haptic_driver.c).
// rk_haptic_cfg_t is 3 bytes, so unlike the title-filter handler above, a
// plain stack local is completely safe here - no heap allocation needed.
static esp_err_t haptic_config_post_handler(httpd_req_t *req) {
    char buf[128] = {0};
    int received = httpd_req_recv(req, buf, sizeof(buf) - 1);
    if (received <= 0) {
        ESP_LOGE(TAG, "Failed to receive POST data");
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "No data received");
        return ESP_FAIL;
    }
    buf[received] = '\0';

    // Unchecked HTML checkboxes aren't submitted at all - presence of the
    // field (regardless of value) means the box was checked.
    char unused[8] = {0};
    bool enabled = get_form_field(buf, "enabled", unused, sizeof(unused));
    if (!haptic_driver_set_enabled(enabled)) {
        ESP_LOGE(TAG, "Failed to save haptic config");
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Failed to save");
        return ESP_FAIL;
    }

    char effect_id_text[8] = {0};
    if (get_form_field(buf, "effect_id", effect_id_text, sizeof(effect_id_text))) {
        int effect_id = atoi(effect_id_text);
        if (effect_id == 210) {
            // Calibration sentinel (see config_get_handler's dropdown and
            // haptic_driver_run_calibration()) - a one-shot action, not a
            // setting to persist as-is (a pass persists its own result).
            haptic_driver_run_calibration();
        } else if (effect_id > 0 && effect_id < 200) {
            haptic_driver_set_effect((uint8_t)effect_id);
        }
    }

    char *html = heap_caps_malloc(1024,
                                  MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!html) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Out of memory");
        return ESP_FAIL;
    }
    snprintf(html, 1024, HTML_SUCCESS, "Haptic feedback setting saved!");
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    httpd_resp_send(req, html, strlen(html));
    free(html);

    ESP_LOGI(TAG, "Haptic config saved, rebooting in 1 second...");
    vTaskDelay(pdMS_TO_TICKS(1000));
    esp_restart();

    return ESP_OK;
}

// Handler for POST /room-config - save which installation this dial talks
// to (see common/rk_room_cfg.h). Reboots on save like haptic-config above,
// rather than trying to hot-swap every already-running poll/flush task's
// notion of which entities to hit.
static esp_err_t room_config_post_handler(httpd_req_t *req) {
    char buf[64] = {0};
    int received = httpd_req_recv(req, buf, sizeof(buf) - 1);
    if (received <= 0) {
        ESP_LOGE(TAG, "Failed to receive POST data");
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "No data received");
        return ESP_FAIL;
    }
    buf[received] = '\0';

    char room_text[16] = {0};
    get_form_field(buf, "room", room_text, sizeof(room_text));
    rk_room_t room = (strcmp(room_text, "dining") == 0) ? RK_ROOM_DINING
                                                         : RK_ROOM_LOUNGE;
    if (!room_cfg_set_current(room)) {
        ESP_LOGE(TAG, "Failed to save room config");
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Failed to save");
        return ESP_FAIL;
    }

    char *html = heap_caps_malloc(1024,
                                  MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!html) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Out of memory");
        return ESP_FAIL;
    }
    snprintf(html, 1024, HTML_SUCCESS, "Room setting saved!");
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    httpd_resp_send(req, html, strlen(html));
    free(html);

    ESP_LOGI(TAG, "Room config saved, rebooting in 1 second...");
    vTaskDelay(pdMS_TO_TICKS(1000));
    esp_restart();

    return ESP_OK;
}

static esp_err_t wifi_add_handler(httpd_req_t *req) {
    char buf[384] = {0};
    int received = httpd_req_recv(req, buf, sizeof(buf) - 1);
    if (received <= 0) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "No data received");
        return ESP_FAIL;
    }
    buf[received] = '\0';

    char ssid[33] = {0};
    char pass[65] = {0};
    if (!get_form_field(buf, "ssid", ssid, sizeof(ssid)) || !ssid[0]) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Missing SSID");
        return ESP_FAIL;
    }
    get_form_field(buf, "pass", pass, sizeof(pass));

    controller_config_wifi_snapshot_t wifi = {0};
    if (!controller_config_wifi_snapshot(&wifi)) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR,
                            "Settings are unavailable");
        return ESP_FAIL;
    }
    bool updating_saved_network = false;
    for (size_t i = 0; i < wifi.count && i < RK_MAX_WIFI; ++i) {
        if (strcmp(wifi.entries[i].ssid, ssid) == 0) {
            updating_saved_network = true;
            break;
        }
    }
    if (wifi.count >= RK_MAX_WIFI && !updating_saved_network) {
        httpd_resp_set_status(req, "409 Conflict");
        httpd_resp_set_type(req, "text/plain");
        httpd_resp_sendstr(req,
                          "Two networks are already saved; remove one first");
        return ESP_FAIL;
    }
    controller_config_write_result_t result =
        controller_config_upsert_wifi(ssid, pass, false, NULL);
    if (result == CONTROLLER_CONFIG_NOT_COMMITTED) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Failed to save");
        return ESP_FAIL;
    }
    if (result == CONTROLLER_CONFIG_COMMITTED_UNVERIFIED) {
        apply_committed_wifi(false);
        return send_unverified_settings(req);
    }
    apply_committed_wifi(false);

    ESP_LOGI(TAG, "Added WiFi '%s' to this Dial", ssid);
    httpd_resp_set_status(req, "303 See Other");
    httpd_resp_set_hdr(req, "Location", "/");
    httpd_resp_sendstr(req, "Redirecting...");
    return ESP_OK;
}

static esp_err_t wifi_remove_handler(httpd_req_t *req) {
    char buf[64] = {0};
    int received = httpd_req_recv(req, buf, sizeof(buf) - 1);
    if (received <= 0) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "No data received");
        return ESP_FAIL;
    }
    buf[received] = '\0';

    char idx_text[8] = {0};
    if (!get_form_field(buf, "idx", idx_text, sizeof(idx_text))) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Missing index");
        return ESP_FAIL;
    }
    char *end = NULL;
    long parsed = strtol(idx_text, &end, 10);
    if (end == idx_text || *end != '\0' || parsed < 0 || parsed >= RK_MAX_WIFI) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid index");
        return ESP_FAIL;
    }

    int idx = (int)parsed;
    controller_config_snapshot_t snapshot = {0};
    if (!controller_config_snapshot(&snapshot)) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR,
                            "Settings are unavailable");
        return ESP_FAIL;
    }
    if (idx < snapshot.value.wifi_count) {
        ESP_LOGI(TAG, "Removing WiFi '%s' from this Dial",
                 snapshot.value.wifi[idx].ssid);
        controller_config_write_result_t result =
            controller_config_remove_wifi((size_t)idx, NULL);
        if (result == CONTROLLER_CONFIG_NOT_COMMITTED) {
            httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR,
                                "Failed to save");
            return ESP_FAIL;
        }
        if (result == CONTROLLER_CONFIG_COMMITTED_UNVERIFIED) {
            apply_committed_wifi(false);
            return send_unverified_settings(req);
        }
        apply_committed_wifi(false);
    }

    httpd_resp_set_status(req, "303 See Other");
    httpd_resp_set_hdr(req, "Location", "/");
    httpd_resp_sendstr(req, "Redirecting...");
    return ESP_OK;
}

static esp_err_t redirect_to_ble(httpd_req_t *req) {
    httpd_resp_set_status(req, "303 See Other");
    /* Force one follow-up refresh even if the owner task has not consumed the
     * command before the browser follows this redirect. */
    httpd_resp_set_hdr(req, "Location", "/ble?watch=1");
    httpd_resp_sendstr(req, "Redirecting...");
    return ESP_OK;
}

static bool ble_watch_requested(httpd_req_t *req) {
    char query[32];
    char watch[4];
    return httpd_req_get_url_query_str(req, query, sizeof(query)) == ESP_OK &&
           httpd_query_key_value(query, "watch", watch, sizeof(watch)) == ESP_OK &&
           watch[0] == '1';
}

static bool ble_state_auto_updates(rk_ble_hid_host_state_t state) {
    return state == RK_BLE_HID_HOST_STATE_STARTING ||
           state == RK_BLE_HID_HOST_STATE_SCANNING ||
           state == RK_BLE_HID_HOST_STATE_CONNECTING ||
           state == RK_BLE_HID_HOST_STATE_STOPPING;
}

static esp_err_t ble_get_handler(httpd_req_t *req) {
    rk_ble_hid_host_status_t status = {0};
    if (rk_ble_hid_host_status_copy(&status) != RK_BLE_HID_HOST_OK) {
        httpd_resp_set_status(req, "503 Service Unavailable");
        httpd_resp_set_type(req, "text/plain");
        httpd_resp_sendstr(req, "BLE media remote is unavailable");
        return ESP_FAIL;
    }

    rk_ble_hid_host_device_t results[RK_BLE_HID_HOST_MAX_RESULTS];
    uint32_t scan_generation = 0;
    size_t result_count = rk_ble_hid_host_scan_results_copy(
        results, RK_BLE_HID_HOST_MAX_RESULTS, &scan_generation);

    const size_t html_size = 12288;
    char *html = heap_caps_malloc(html_size,
                                  MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!html) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR,
                            "Out of memory");
        return ESP_FAIL;
    }

    const char *device_name =
        status.active_name[0] ? status.active_name : status.bonded_name;
    char escaped_name[RK_BLE_HID_HOST_NAME_MAX_LEN * 2] = "";
    html_escape(device_name, escaped_name, sizeof(escaped_name));

    const char *state_class = "idle";
    const char *state_title = "Ready to pair";
    const char *state_detail = "No remote is paired yet.";
    if (!status.enabled || status.state == RK_BLE_HID_HOST_STATE_DISABLED) {
        state_title = "BLE is off";
        state_detail = "Turn it on when you want to connect a media remote.";
    } else if (status.state == RK_BLE_HID_HOST_STATE_ERROR) {
        state_class = "error";
        state_title = "Bluetooth needs attention";
        state_detail = "Restart the Dial, then try again.";
    } else if (status.connected) {
        state_class = "connected";
        state_title = "Connected";
        state_detail = escaped_name[0] ? escaped_name : "Paired media remote";
    } else if (status.state == RK_BLE_HID_HOST_STATE_CONNECTING ||
               status.bonded) {
        state_class = "working";
        state_title = "Reconnecting";
        state_detail = escaped_name[0] ? escaped_name : "Paired media remote";
    } else if (status.state == RK_BLE_HID_HOST_STATE_SCANNING) {
        state_class = "working";
        state_title = "Looking for remotes";
        state_detail = "Keep your remote in pairing mode. This takes about five seconds.";
    } else if (status.state == RK_BLE_HID_HOST_STATE_STARTING) {
        state_class = "working";
        state_title = "Starting Bluetooth";
        state_detail = "The remote service will be ready shortly.";
    } else if (status.state == RK_BLE_HID_HOST_STATE_STOPPING) {
        state_class = "working";
        state_title = "Turning off Bluetooth";
        state_detail = "The remote service is shutting down.";
    }
    const bool auto_updates = ble_watch_requested(req) ||
                              ble_state_auto_updates(status.state);

    int pos = snprintf(
        html, html_size,
        "<!DOCTYPE html><html><head>"
        "<meta charset='utf-8'>"
        "<meta name='viewport' content='width=device-width,initial-scale=1'>"
        "<title>BLE Media Remote - HiPhi Dial</title>"
        "<style>"
        "*{box-sizing:border-box}body{font-family:sans-serif;margin:0;padding:24px;"
        "background:#1a1a2e;color:#eee;line-height:1.45}main{max-width:480px;margin:0 auto}"
        "a{color:#70d6ff}h1{color:#4fc3f7;margin:24px 0 6px;font-size:28px}"
        "h2{font-size:18px;margin:28px 0 8px}.lede{color:#b9c3d8;margin:0 0 20px}"
        ".connection{display:flex;gap:12px;align-items:flex-start;background:#0f0f1a;"
        "padding:16px;border-radius:14px;margin:18px 0}.connection strong,.connection span{display:block}"
        ".connection span:last-child{color:#b9c3d8;margin-top:2px;overflow-wrap:anywhere}"
        ".dot{width:10px;height:10px;border-radius:50%%;background:#8a94a8;margin-top:6px;flex:none}"
        ".connected .dot{background:#55d98b}.working .dot{background:#ffd166;animation:pulse 1.4s ease-in-out infinite}"
        ".error .dot{background:#ff7043}.live{font-size:12px;color:#91a0bb;margin-top:8px}"
        ".actions{display:flex;flex-wrap:wrap;gap:8px;margin:16px 0}.actions form{margin:0}"
        "button{padding:10px 16px;background:#4fc3f7;color:#07111a;border:0;border-radius:8px;"
        "font-weight:700;cursor:pointer}button:hover{background:#70d6ff}button:focus-visible,a:focus-visible{outline:3px solid #fff;outline-offset:3px}"
        "button:disabled{background:#596275;color:#c7ccda;cursor:wait}.danger{background:#ff8a65}"
        ".device{background:#0f0f1a;padding:12px 14px;border-radius:12px;margin:8px 0;"
        "display:flex;gap:12px;justify-content:space-between;align-items:center}.device span{overflow-wrap:anywhere}"
        ".empty,.hint{color:#b9c3d8}.technical{margin-top:28px;color:#91a0bb;font-size:13px}"
        ".technical summary{cursor:pointer;color:#b9c3d8}@keyframes pulse{50%%{opacity:.35;transform:scale(.75)}}"
        "@media(prefers-reduced-motion:reduce){.working .dot{animation:none}}"
        "</style>%s</head><body><main>"
        "<a href='/'>← Back to Dial settings</a>"
        "<h1>BLE Media Remote</h1>"
        "<p class='lede'>Connect one physical Bluetooth remote to control media on this Dial.</p>"
        "<div class='connection %s' role='status' aria-live='polite'>"
        "<span class='dot' aria-hidden='true'></span><div><strong>%s</strong><span>%s</span>"
        "%s</div></div>"
        "<div class='actions'>"
        "<form method='POST' action='/ble-enable'>"
        "<input type='hidden' name='enabled' value='%d'>"
        "<button type='submit' class='%s'>%s</button></form>",
        auto_updates
            ? "<script>if(location.search)history.replaceState(null,'','/ble');"
              "setTimeout(function(){if(!document.hidden)location.reload()},1000);"
              "document.addEventListener('visibilitychange',function(){if(!document.hidden)location.reload()});</script>"
            : "",
        state_class, state_title, state_detail,
        auto_updates ? "<span class='live'>Updates automatically</span>" : "",
        status.enabled ? 0 : 1,
        status.enabled ? "danger" : "",
        status.enabled ? "Turn off BLE" : "Turn on BLE");
    if (pos < 0 || pos >= (int)html_size) {
        free(html);
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR,
                            "Page generation failed");
        return ESP_FAIL;
    }

    if (status.enabled) {
        if (status.bonded) {
            pos += snprintf(
                html + pos, html_size - (size_t)pos,
                " <form method='POST' action='/ble-forget'>"
                "<button type='submit' class='danger'>Forget remote</button></form>");
        }
        pos += snprintf(html + pos, html_size - (size_t)pos, "</div>");

        if (status.bonded) {
            pos += snprintf(
                html + pos, html_size - (size_t)pos,
                "<p class='hint'>To pair a different remote, forget this one first.</p>");
        } else {
            pos += snprintf(
                html + pos, html_size - (size_t)pos,
                "<h2>Pair a remote</h2>"
                "<p class='hint'>Put the remote into pairing mode, then start a scan.</p>"
                "<form method='POST' action='/ble-scan'>"
                "<button type='submit'%s>%s</button></form>",
                status.state == RK_BLE_HID_HOST_STATE_SCANNING ? " disabled" : "",
                status.state == RK_BLE_HID_HOST_STATE_SCANNING
                    ? "Scanning…" : "Scan for remotes");

            if (status.state != RK_BLE_HID_HOST_STATE_SCANNING) {
            for (size_t i = 0; i < result_count && pos < (int)html_size; i++) {
                char escaped_result[RK_BLE_HID_HOST_NAME_MAX_LEN * 2];
                html_escape(results[i].name, escaped_result,
                            sizeof(escaped_result));
                pos += snprintf(
                    html + pos, html_size - (size_t)pos,
                    "<div class='device'><span>%s</span>"
                    "<form method='POST' action='/ble-pair'>"
                    "<input type='hidden' name='idx' value='%u'>"
                    "<input type='hidden' name='generation' value='%lu'>"
                    "<button type='submit'>Pair this remote</button></form></div>",
                    escaped_result, (unsigned)i,
                    (unsigned long)scan_generation);
            }
                if (result_count == 0 && scan_generation > 0) {
                    pos += snprintf(
                        html + pos, html_size - (size_t)pos,
                        "<p class='empty'>No remotes found. Keep the remote in pairing mode and scan again.</p>");
                }
            }
        }
    } else {
        pos += snprintf(
            html + pos, html_size - (size_t)pos,
            "</div><p class='hint'>BLE stays off across restarts until you turn it on here.</p>");
    }

    if (pos < 0 || pos >= (int)html_size) {
        pos = (int)html_size - 1;
    }
    pos += snprintf(
        html + pos, html_size - (size_t)pos,
        "<details class='technical'><summary>About this setting</summary>"
        "<p>The Dial connects to a separate Bluetooth media remote. The Dial itself "
        "does not appear as a remote to phones or computers.</p>%s%s%s</details>"
        "</main></body></html>",
        status.last_error != RK_BLE_HID_HOST_ERROR_NONE ? "<p>Technical error: " : "",
        status.last_error != RK_BLE_HID_HOST_ERROR_NONE
            ? rk_ble_hid_host_error_name(status.last_error) : "",
        status.last_error != RK_BLE_HID_HOST_ERROR_NONE ? "</p>" : "");
    if (pos < 0 || pos >= (int)html_size) {
        pos = (int)html_size - 1;
    }

    httpd_resp_set_type(req, "text/html; charset=utf-8");
    httpd_resp_send(req, html, pos);
    free(html);
    return ESP_OK;
}

static esp_err_t ble_enable_handler(httpd_req_t *req) {
    char buf[32] = {0};
    int received = httpd_req_recv(req, buf, sizeof(buf) - 1);
    if (received <= 0) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "No data");
        return ESP_FAIL;
    }
    buf[received] = '\0';

    char enabled_text[4] = {0};
    if (!get_form_field(buf, "enabled", enabled_text, sizeof(enabled_text)) ||
        (strcmp(enabled_text, "0") != 0 && strcmp(enabled_text, "1") != 0)) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid enabled value");
        return ESP_FAIL;
    }
    rk_ble_hid_host_result_t result =
        rk_ble_hid_host_set_enabled(enabled_text[0] == '1');
    if (result != RK_BLE_HID_HOST_OK) {
        return send_conflict(req, rk_ble_hid_host_result_name(result));
    }
    return redirect_to_ble(req);
}

static esp_err_t ble_scan_handler(httpd_req_t *req) {
    rk_ble_hid_host_result_t result = rk_ble_hid_host_scan_start();
    if (result != RK_BLE_HID_HOST_OK) {
        return send_conflict(req, rk_ble_hid_host_result_name(result));
    }
    return redirect_to_ble(req);
}

static esp_err_t ble_pair_handler(httpd_req_t *req) {
    char buf[96] = {0};
    int received = httpd_req_recv(req, buf, sizeof(buf) - 1);
    if (received <= 0) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "No data");
        return ESP_FAIL;
    }
    buf[received] = '\0';

    char idx_text[8] = {0};
    char generation_text[16] = {0};
    if (!get_form_field(buf, "idx", idx_text, sizeof(idx_text)) ||
        !get_form_field(buf, "generation", generation_text,
                        sizeof(generation_text))) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST,
                            "Missing scan identity");
        return ESP_FAIL;
    }

    char *end = NULL;
    long idx = strtol(idx_text, &end, 10);
    if (end == idx_text || *end != '\0' || idx < 0 ||
        idx >= RK_BLE_HID_HOST_MAX_RESULTS) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid index");
        return ESP_FAIL;
    }
    end = NULL;
    unsigned long requested_generation = strtoul(generation_text, &end, 10);
    if (end == generation_text || *end != '\0') {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid generation");
        return ESP_FAIL;
    }

    rk_ble_hid_host_device_t results[RK_BLE_HID_HOST_MAX_RESULTS];
    uint32_t current_generation = 0;
    size_t count = rk_ble_hid_host_scan_results_copy(
        results, RK_BLE_HID_HOST_MAX_RESULTS, &current_generation);
    if ((size_t)idx >= count || requested_generation != current_generation) {
        return send_conflict(req, "Scan results changed; scan again");
    }

    rk_ble_hid_host_result_t result = rk_ble_hid_host_pair(&results[idx]);
    if (result != RK_BLE_HID_HOST_OK) {
        return send_conflict(req, rk_ble_hid_host_result_name(result));
    }
    return redirect_to_ble(req);
}

static esp_err_t ble_forget_handler(httpd_req_t *req) {
    rk_ble_hid_host_result_t result = rk_ble_hid_host_forget();
    if (result != RK_BLE_HID_HOST_OK) {
        return send_conflict(req, rk_ble_hid_host_result_name(result));
    }
    return redirect_to_ble(req);
}

void config_server_start(void) {
    if (!http_server_lifecycle_lock()) {
        ESP_LOGE(TAG, "Could not acquire HTTP lifecycle lock");
        return;
    }
    if (http_server_lifecycle_owner_locked() ==
        HTTP_SERVER_OWNER_CAPTIVE_PORTAL) {
        ESP_LOGW(TAG, "Config server start suppressed while AP owns port 80");
        http_server_lifecycle_unlock();
        return;
    }
    if (s_server) {
        ESP_LOGW(TAG, "Config server already running");
        http_server_lifecycle_claim_locked(HTTP_SERVER_OWNER_CONFIG);
        http_server_lifecycle_unlock();
        return;
    }

    http_server_lifecycle_claim_locked(HTTP_SERVER_OWNER_CONFIG);

    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.server_port = 80;
    config.max_uri_handlers = 15;  // root, config, ha-config, zone-config, title-filter-config, haptic-config, room-config, 2 wifi, 5 ble
    config.stack_size = 8192;  // Increased for mDNS resolution during config save
    // Note: max_req_hdr_len set via CONFIG_HTTPD_MAX_REQ_HDR_LEN in sdkconfig

    ESP_LOGI(TAG, "Starting config server on port %d", config.server_port);

    if (httpd_start(&s_server, &config) != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start HTTP server");
        http_server_lifecycle_release_locked(HTTP_SERVER_OWNER_CONFIG);
        http_server_lifecycle_unlock();
        return;
    }

    // Register URI handlers
    httpd_uri_t root = {
        .uri = "/",
        .method = HTTP_GET,
        .handler = config_get_handler,
    };
    httpd_register_uri_handler(s_server, &root);

    httpd_uri_t config_post = {
        .uri = "/config",
        .method = HTTP_POST,
        .handler = config_post_handler,
    };
    httpd_register_uri_handler(s_server, &config_post);

    httpd_uri_t ha_config_post = {
        .uri = "/ha-config",
        .method = HTTP_POST,
        .handler = ha_config_post_handler,
    };
    httpd_register_uri_handler(s_server, &ha_config_post);

    httpd_uri_t zone_config_post = {
        .uri = "/zone-config",
        .method = HTTP_POST,
        .handler = zone_config_post_handler,
    };
    httpd_register_uri_handler(s_server, &zone_config_post);

    httpd_uri_t title_filter_config_post = {
        .uri = "/title-filter-config",
        .method = HTTP_POST,
        .handler = title_filter_config_post_handler,
    };
    httpd_register_uri_handler(s_server, &title_filter_config_post);

    httpd_uri_t haptic_config_post = {
        .uri = "/haptic-config",
        .method = HTTP_POST,
        .handler = haptic_config_post_handler,
    };
    httpd_register_uri_handler(s_server, &haptic_config_post);

    httpd_uri_t room_config_post = {
        .uri = "/room-config",
        .method = HTTP_POST,
        .handler = room_config_post_handler,
    };
    httpd_register_uri_handler(s_server, &room_config_post);

    httpd_uri_t wifi_add = {
        .uri = "/wifi-add",
        .method = HTTP_POST,
        .handler = wifi_add_handler,
    };
    httpd_register_uri_handler(s_server, &wifi_add);

    httpd_uri_t wifi_remove = {
        .uri = "/wifi-remove",
        .method = HTTP_POST,
        .handler = wifi_remove_handler,
    };
    httpd_register_uri_handler(s_server, &wifi_remove);

    httpd_uri_t ble_get = {
        .uri = "/ble",
        .method = HTTP_GET,
        .handler = ble_get_handler,
    };
    httpd_register_uri_handler(s_server, &ble_get);

    httpd_uri_t ble_enable = {
        .uri = "/ble-enable",
        .method = HTTP_POST,
        .handler = ble_enable_handler,
    };
    httpd_register_uri_handler(s_server, &ble_enable);

    httpd_uri_t ble_scan = {
        .uri = "/ble-scan",
        .method = HTTP_POST,
        .handler = ble_scan_handler,
    };
    httpd_register_uri_handler(s_server, &ble_scan);

    httpd_uri_t ble_pair = {
        .uri = "/ble-pair",
        .method = HTTP_POST,
        .handler = ble_pair_handler,
    };
    httpd_register_uri_handler(s_server, &ble_pair);

    httpd_uri_t ble_forget = {
        .uri = "/ble-forget",
        .method = HTTP_POST,
        .handler = ble_forget_handler,
    };
    httpd_register_uri_handler(s_server, &ble_forget);

    ESP_LOGI(TAG, "Config server started");
    http_server_lifecycle_unlock();
}

void config_server_stop_locked(void) {
    if (!s_server) {
        http_server_lifecycle_release_locked(HTTP_SERVER_OWNER_CONFIG);
        return;
    }

    ESP_LOGI(TAG, "Stopping config server");
    httpd_stop(s_server);
    s_server = NULL;
    http_server_lifecycle_release_locked(HTTP_SERVER_OWNER_CONFIG);
}

void config_server_stop(void) {
    if (!http_server_lifecycle_lock()) {
        ESP_LOGE(TAG, "Could not acquire HTTP lifecycle lock");
        return;
    }
    config_server_stop_locked();
    http_server_lifecycle_unlock();
}

bool config_server_is_running(void) {
    if (!http_server_lifecycle_lock()) {
        return false;
    }
    bool running = s_server != NULL;
    http_server_lifecycle_unlock();
    return running;
}
