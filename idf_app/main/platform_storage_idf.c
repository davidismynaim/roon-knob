#include "platform/platform_storage.h"

#include <esp_err.h>
#include <esp_heap_caps.h>
#include <esp_log.h>
#include <nvs_flash.h>
#include <nvs.h>
#include <stdlib.h>
#include <string.h>

#include "sdkconfig.h"

static const char *TAG = "platform_storage";
static const char *NAMESPACE = "rk_cfg";
static const char *KEY = "cfg";

static esp_err_t open_ns(nvs_handle_t *handle, nvs_open_mode_t mode) {
    return nvs_open(NAMESPACE, mode, handle);
}

static void apply_storage_defaults(rk_cfg_t *out) {
    rk_cfg_set_storage_defaults(out);
}

bool platform_storage_read(rk_cfg_t *out,
                           platform_storage_read_result_t *out_result) {
    if (out_result) {
        *out_result = (platform_storage_read_result_t){
            .state = PLATFORM_STORAGE_READ_ERROR,
            .needs_persist = false,
        };
    }
    if (!out) {
        return false;
    }

    memset(out, 0, sizeof(*out));
    esp_err_t err;
    nvs_handle_t handle;
    err = open_ns(&handle, NVS_READONLY);
    if (err != ESP_OK) {
        if (err == ESP_ERR_NVS_NOT_FOUND) {
            if (out_result) {
                out_result->state = PLATFORM_STORAGE_READ_EMPTY;
            }
            return true;
        }
        ESP_LOGW(TAG, "nvs open failed: %s", esp_err_to_name(err));
        return false;
    }

    size_t stored_len = 0;
    err = nvs_get_blob(handle, KEY, NULL, &stored_len);
    if (err != ESP_OK) {
        nvs_close(handle);
        if (err == ESP_ERR_NVS_NOT_FOUND) {
            if (out_result) {
                out_result->state = PLATFORM_STORAGE_READ_EMPTY;
            }
            return true;
        }
        ESP_LOGW(TAG, "nvs size query failed: %s", esp_err_to_name(err));
        return false;
    }

    size_t read_len = sizeof(*out);
    err = nvs_get_blob(handle, KEY, out, &read_len);
    nvs_close(handle);

    if (err != ESP_OK) {
        if (err == ESP_ERR_NVS_INVALID_LENGTH) {
            ESP_LOGW(TAG, "Config blob too large (stored=%d, max=%d)",
                     (int)stored_len, (int)sizeof(*out));
            apply_storage_defaults(out);
            if (out_result) {
                out_result->state = PLATFORM_STORAGE_READ_RECOVERED;
                out_result->needs_persist = true;
            }
            return true;
        } else {
            ESP_LOGW(TAG, "nvs read failed: %s", esp_err_to_name(err));
        }
        memset(out, 0, sizeof(*out));
        return false;
    }

    if (stored_len == RK_CFG_V1_SIZE && out->cfg_ver == 1) {
        ESP_LOGI(TAG, "Upgrading this device's config from v1 to v3");
    } else if (stored_len == RK_CFG_V2_SIZE && out->cfg_ver == 2) {
        ESP_LOGI(TAG, "Upgrading this device's config from v2 to v3");
    } else if (stored_len != sizeof(*out)) {
        ESP_LOGW(TAG, "Config size mismatch (stored=%d, expected=%d), applying defaults",
                 (int)stored_len, (int)sizeof(*out));
    }

    bool needs_persist = rk_cfg_prepare_storage_read(out, stored_len);

    ESP_LOGI(TAG, "Loaded config: ssid='%s' bridge='%s' zone='%s' ver=%d rot=%d/%d",
             out->ssid[0] ? out->ssid : "(empty)",
             out->bridge_base[0] ? out->bridge_base : "(empty)",
             out->zone_id[0] ? out->zone_id : "(empty)",
             out->cfg_ver,
             out->rotation_charging,
             out->rotation_not_charging);

    if (out_result) {
        out_result->state = needs_persist ? PLATFORM_STORAGE_READ_RECOVERED
                                          : PLATFORM_STORAGE_READ_VALID;
        out_result->needs_persist = needs_persist;
    }
    return true;
}

platform_storage_write_result_t platform_storage_write(const rk_cfg_t *in) {
    if (!in) {
        return PLATFORM_STORAGE_NOT_COMMITTED;
    }
    rk_cfg_t copy;
    if (!rk_cfg_canonicalize_v3(&copy, in)) {
        return PLATFORM_STORAGE_NOT_COMMITTED;
    }

    ESP_LOGI(TAG, "Saving config: ssid='%s' bridge='%s' zone='%s' ver=%d",
             copy.ssid[0] ? copy.ssid : "(empty)",
             copy.bridge_base[0] ? copy.bridge_base : "(empty)",
             copy.zone_id[0] ? copy.zone_id : "(empty)",
             copy.cfg_ver);

    esp_err_t err;
    nvs_handle_t handle;
    err = open_ns(&handle, NVS_READWRITE);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "nvs open rw failed: %s", esp_err_to_name(err));
        return PLATFORM_STORAGE_NOT_COMMITTED;
    }
    err = nvs_set_blob(handle, KEY, &copy, sizeof(copy));
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "nvs_set_blob failed: %s", esp_err_to_name(err));
        nvs_close(handle);
        return PLATFORM_STORAGE_NOT_COMMITTED;
    }
    ESP_LOGI(TAG, "nvs_set_blob OK, committing...");

    err = nvs_commit(handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "nvs_commit failed: %s", esp_err_to_name(err));
        nvs_close(handle);
        return PLATFORM_STORAGE_NOT_COMMITTED;
    }
    ESP_LOGI(TAG, "nvs_commit OK");
    nvs_close(handle);

    rk_cfg_t verify = {0};
    platform_storage_read_result_t verify_result;
    if (!platform_storage_read(&verify, &verify_result) ||
        verify_result.state != PLATFORM_STORAGE_READ_VALID ||
        verify_result.needs_persist) {
        ESP_LOGE(TAG, "VERIFY FAILED: Could not read back saved config!");
        return PLATFORM_STORAGE_COMMITTED_UNVERIFIED;
    }

    /* Canonicalize only the readback. copy is already canonical, so this
     * preserves padding-independent comparison without the old two-canonical
     * helper's additional stack peak. */
    if (!rk_cfg_v3_equal_to_canonical(&verify, &copy)) {
        ESP_LOGE(TAG, "VERIFY FAILED: full V3 candidate mismatch");
        return PLATFORM_STORAGE_COMMITTED_UNVERIFIED;
    }

    ESP_LOGI(TAG, "VERIFY OK: Config saved and verified successfully");
    return PLATFORM_STORAGE_COMMITTED_VERIFIED;
}

void platform_storage_defaults(rk_cfg_t *out) {
    if (!out) {
        return;
    }
    apply_storage_defaults(out);
    // Leave bridge_base empty - mDNS discovery is the primary method
    // wifi_manager will fill SSID/pass from Kconfig defaults
    // zone_id is left empty - user will select from available zones
    ESP_LOGI(TAG, "Applied defaults (bridge will be discovered via mDNS)");
}

static const char *HA_NAMESPACE = "rk_ha";
static const char *HA_KEY = "cfg";

bool platform_storage_read_ha(rk_ha_cfg_t *out) {
    if (!out) {
        return false;
    }
    rk_ha_cfg_set_defaults(out);

    nvs_handle_t handle;
    esp_err_t err = nvs_open(HA_NAMESPACE, NVS_READONLY, &handle);
    if (err != ESP_OK) {
        if (err != ESP_ERR_NVS_NOT_FOUND) {
            ESP_LOGW(TAG, "ha nvs open failed: %s", esp_err_to_name(err));
        }
        return true;
    }

    rk_ha_cfg_t stored = {0};
    size_t len = sizeof(stored);
    err = nvs_get_blob(handle, HA_KEY, &stored, &len);
    nvs_close(handle);

    if (err != ESP_OK) {
        if (err != ESP_ERR_NVS_NOT_FOUND) {
            ESP_LOGW(TAG, "ha nvs read failed: %s", esp_err_to_name(err));
        }
        return true;
    }
    if (len != sizeof(stored) || stored.cfg_ver != RK_HA_CFG_CURRENT_VER) {
        ESP_LOGW(TAG, "ha config size/version mismatch, using defaults");
        return true;
    }

    stored.host[sizeof(stored.host) - 1] = '\0';
    stored.token[sizeof(stored.token) - 1] = '\0';
    *out = stored;
    return true;
}

bool platform_storage_write_ha(const rk_ha_cfg_t *in) {
    if (!in) {
        return false;
    }
    rk_ha_cfg_t copy = *in;
    copy.host[sizeof(copy.host) - 1] = '\0';
    copy.token[sizeof(copy.token) - 1] = '\0';
    copy.cfg_ver = RK_HA_CFG_CURRENT_VER;

    nvs_handle_t handle;
    esp_err_t err = nvs_open(HA_NAMESPACE, NVS_READWRITE, &handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "ha nvs open rw failed: %s", esp_err_to_name(err));
        return false;
    }
    err = nvs_set_blob(handle, HA_KEY, &copy, sizeof(copy));
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "ha nvs_set_blob failed: %s", esp_err_to_name(err));
        nvs_close(handle);
        return false;
    }
    err = nvs_commit(handle);
    nvs_close(handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "ha nvs_commit failed: %s", esp_err_to_name(err));
        return false;
    }
    ESP_LOGI(TAG, "Saved HA config: host='%s' token=%s",
             copy.host, copy.token[0] ? "(set)" : "(empty)");
    return true;
}

static const char *TITLE_FILTER_NAMESPACE = "rk_titlef";
static const char *TITLE_FILTER_KEY = "cfg";

// Note: unlike platform_storage_read_ha/write_ha above, these read/write
// directly into/out of the caller's rk_title_filter_cfg_t rather than via
// a local stack copy - that struct is 4KB+ (RK_TITLE_FILTER_PATTERNS_MAX),
// and a local that size was enough to overflow an 8KB task stack in
// practice (see the comment on this same struct in track_title_filter.c).
bool platform_storage_read_title_filters(rk_title_filter_cfg_t *out) {
    if (!out) {
        return false;
    }
    rk_title_filter_cfg_set_defaults(out);

    nvs_handle_t handle;
    esp_err_t err = nvs_open(TITLE_FILTER_NAMESPACE, NVS_READONLY, &handle);
    if (err != ESP_OK) {
        if (err != ESP_ERR_NVS_NOT_FOUND) {
            ESP_LOGW(TAG, "title filter nvs open failed: %s", esp_err_to_name(err));
        }
        return true;
    }

    // Buffer capacity is the full struct size, but the actual stored blob
    // may be much shorter - see platform_storage_write_title_filters for
    // why. nvs_get_blob() only requires the buffer be >= the stored size,
    // and reports the real size back through len.
    size_t len = sizeof(*out);
    err = nvs_get_blob(handle, TITLE_FILTER_KEY, out, &len);
    nvs_close(handle);

    if (err != ESP_OK) {
        if (err != ESP_ERR_NVS_NOT_FOUND) {
            ESP_LOGW(TAG, "title filter nvs read failed: %s", esp_err_to_name(err));
        }
        rk_title_filter_cfg_set_defaults(out);  // out may be partially written
        return true;
    }
    // cfg_ver is always the blob's trailing byte, whether it's an old
    // fixed-4097-byte blob (from before this fix) or a new variable-length
    // one - both lay patterns out first with cfg_ver immediately after, so
    // this stays compatible with blobs already on a device's flash.
    if (len < 1 || len > sizeof(out->patterns) + 1) {
        ESP_LOGW(TAG, "title filter config size mismatch (%d bytes), using defaults",
                 (int)len);
        rk_title_filter_cfg_set_defaults(out);
        return true;
    }
    uint8_t stored_ver = ((uint8_t *)out)[len - 1];
    if (stored_ver != RK_TITLE_FILTER_CFG_CURRENT_VER) {
        ESP_LOGW(TAG, "title filter config version mismatch, using defaults");
        rk_title_filter_cfg_set_defaults(out);
        return true;
    }
    out->cfg_ver = stored_ver;
    if (len - 1 < sizeof(out->patterns)) {
        out->patterns[len - 1] = '\0';
    }
    out->patterns[sizeof(out->patterns) - 1] = '\0';
    return true;
}

bool platform_storage_write_title_filters(const rk_title_filter_cfg_t *in) {
    if (!in) {
        return false;
    }

    // Only the patterns actually in use get written to NVS, not the full
    // 4KB+ fixed buffer - the nvs partition is only 16KB total
    // (idf_app/partitions.csv), and always paying for the worst case on
    // every save (even an empty list) was eating enough space to make
    // OTHER config saves (haptic, HA) start failing too. cfg_ver stays the
    // blob's trailing byte either way, so platform_storage_read_title_filters
    // reads old full-size blobs and these new short ones the same way.
    size_t patterns_len = strnlen(in->patterns, sizeof(in->patterns) - 1) + 1;
    size_t blob_len = patterns_len + 1;
    uint8_t *tmp = heap_caps_malloc(blob_len, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!tmp) {
        ESP_LOGE(TAG, "title filter save: out of memory");
        return false;
    }
    memcpy(tmp, in->patterns, patterns_len);
    tmp[patterns_len] = in->cfg_ver;

    nvs_handle_t handle;
    esp_err_t err = nvs_open(TITLE_FILTER_NAMESPACE, NVS_READWRITE, &handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "title filter nvs open rw failed: %s", esp_err_to_name(err));
        free(tmp);
        return false;
    }
    err = nvs_set_blob(handle, TITLE_FILTER_KEY, tmp, blob_len);
    free(tmp);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "title filter nvs_set_blob failed: %s", esp_err_to_name(err));
        nvs_close(handle);
        return false;
    }
    err = nvs_commit(handle);
    nvs_close(handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "title filter nvs_commit failed: %s", esp_err_to_name(err));
        return false;
    }
    ESP_LOGI(TAG, "Saved title filter config (%d bytes of patterns)",
             (int)(patterns_len - 1));
    return true;
}

static const char *HAPTIC_NAMESPACE = "rk_haptic";
static const char *HAPTIC_KEY = "cfg";

bool platform_storage_read_haptic(rk_haptic_cfg_t *out) {
    if (!out) {
        return false;
    }
    rk_haptic_cfg_set_defaults(out);

    nvs_handle_t handle;
    esp_err_t err = nvs_open(HAPTIC_NAMESPACE, NVS_READONLY, &handle);
    if (err != ESP_OK) {
        if (err != ESP_ERR_NVS_NOT_FOUND) {
            ESP_LOGW(TAG, "haptic nvs open failed: %s", esp_err_to_name(err));
        }
        return true;
    }

    rk_haptic_cfg_t stored = {0};
    size_t len = sizeof(stored);
    err = nvs_get_blob(handle, HAPTIC_KEY, &stored, &len);
    nvs_close(handle);

    if (err != ESP_OK) {
        if (err != ESP_ERR_NVS_NOT_FOUND) {
            ESP_LOGW(TAG, "haptic nvs read failed: %s", esp_err_to_name(err));
        }
        return true;
    }
    if (len != sizeof(stored) || stored.cfg_ver != RK_HAPTIC_CFG_CURRENT_VER) {
        ESP_LOGW(TAG, "haptic config size/version mismatch, using defaults");
        return true;
    }

    *out = stored;
    return true;
}

bool platform_storage_write_haptic(const rk_haptic_cfg_t *in) {
    if (!in) {
        return false;
    }
    rk_haptic_cfg_t copy = *in;
    copy.cfg_ver = RK_HAPTIC_CFG_CURRENT_VER;

    nvs_handle_t handle;
    esp_err_t err = nvs_open(HAPTIC_NAMESPACE, NVS_READWRITE, &handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "haptic nvs open rw failed: %s", esp_err_to_name(err));
        return false;
    }
    err = nvs_set_blob(handle, HAPTIC_KEY, &copy, sizeof(copy));
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "haptic nvs_set_blob failed: %s", esp_err_to_name(err));
        nvs_close(handle);
        return false;
    }
    err = nvs_commit(handle);
    nvs_close(handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "haptic nvs_commit failed: %s", esp_err_to_name(err));
        return false;
    }
    ESP_LOGI(TAG, "Saved haptic config: enabled=%d", copy.enabled);
    return true;
}

static const char *ROOM_NAMESPACE = "rk_room";
static const char *ROOM_KEY = "cfg";

bool platform_storage_read_room(rk_room_cfg_t *out) {
    if (!out) {
        return false;
    }
    rk_room_cfg_set_defaults(out);

    nvs_handle_t handle;
    esp_err_t err = nvs_open(ROOM_NAMESPACE, NVS_READONLY, &handle);
    if (err != ESP_OK) {
        if (err != ESP_ERR_NVS_NOT_FOUND) {
            ESP_LOGW(TAG, "room nvs open failed: %s", esp_err_to_name(err));
        }
        return true;
    }

    rk_room_cfg_t stored = {0};
    size_t len = sizeof(stored);
    err = nvs_get_blob(handle, ROOM_KEY, &stored, &len);
    nvs_close(handle);

    if (err != ESP_OK) {
        if (err != ESP_ERR_NVS_NOT_FOUND) {
            ESP_LOGW(TAG, "room nvs read failed: %s", esp_err_to_name(err));
        }
        return true;
    }
    if (len != sizeof(stored) || stored.cfg_ver != RK_ROOM_CFG_CURRENT_VER) {
        ESP_LOGW(TAG, "room config size/version mismatch, using defaults");
        return true;
    }

    *out = stored;
    return true;
}

bool platform_storage_write_room(const rk_room_cfg_t *in) {
    if (!in) {
        return false;
    }
    rk_room_cfg_t copy = *in;
    copy.cfg_ver = RK_ROOM_CFG_CURRENT_VER;

    nvs_handle_t handle;
    esp_err_t err = nvs_open(ROOM_NAMESPACE, NVS_READWRITE, &handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "room nvs open rw failed: %s", esp_err_to_name(err));
        return false;
    }
    err = nvs_set_blob(handle, ROOM_KEY, &copy, sizeof(copy));
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "room nvs_set_blob failed: %s", esp_err_to_name(err));
        nvs_close(handle);
        return false;
    }
    err = nvs_commit(handle);
    nvs_close(handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "room nvs_commit failed: %s", esp_err_to_name(err));
        return false;
    }
    ESP_LOGI(TAG, "Saved room config: room=%d", copy.room);
    return true;
}
