#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Presentation seam consumed by shared controller code (app_main,
// bridge_client). Exactly one target implementation is link-time selected,
// matching the existing platform_* adapter pattern; shared code carries no
// renderer conditionals and no LVGL/target types.

void controller_presentation_update(const char *line1, const char *line2, const char *line3,
                                     bool playing, float volume, float volume_min,
                                     float volume_max, float volume_step,
                                     int seek_position, int length);
void controller_presentation_set_status(bool online);
void controller_presentation_set_message(const char *msg);
void controller_presentation_set_zone_name(const char *zone_name);
void controller_presentation_set_network_status(const char *status);  // NULL clears the banner
void controller_presentation_set_artwork(const char *image_key);
void controller_presentation_show_volume_change(float volume, float volume_step);
/* Independent volume-only refresh (min/max included), for a backend that
 * updates volume on its own cadence rather than as part of a full
 * controller_presentation_update. Unused unless a target registers such a
 * backend (see controller_action_router_set_volume_override). */
void controller_presentation_set_volume_range(float volume, float volume_min,
                                              float volume_max,
                                              float volume_step);
void controller_presentation_update_battery(void);

void controller_presentation_show_zone_picker(const char **zone_names, const char **zone_ids,
                                               int zone_count, int selected_idx);
void controller_presentation_hide_zone_picker(void);
bool controller_presentation_is_zone_picker_visible(void);
void controller_presentation_zone_picker_scroll(int delta);
bool controller_presentation_zone_picker_is_current_selection(void);
void controller_presentation_zone_picker_get_selected_id(char *out, size_t len);

void controller_presentation_show_settings(void);

/* Dial-only: encoder rotation while the detail screen owns the dial for
 * seek-jog instead of volume (CONTROLLER_INTERACTION_CONTEXT_SEEK). ticks is
 * the raw accumulated encoder delta, magnitude preserved - see
 * common/ui.c's ui_seek_adjust(). A no-op on targets with no detail screen. */
void controller_presentation_seek_adjust(int32_t ticks);

/* Dial-only: detail-screen enrichment (next track / album year / bit info),
 * a separate call from controller_presentation_update() above rather than
 * extra params on it - see controller_media_enrichment_view_t's own comment
 * for why this is a distinct patch. Each field is independently absent-able:
 * empty next_track_title means no next track (next_track_artist is ignored
 * in that case), album_year 0 means unknown, empty bit_info means absent. A
 * no-op on targets with no detail screen. */
void controller_presentation_set_media_enrichment(
    const char *next_track_title, const char *next_track_artist,
    int32_t album_year, const char *bit_info);

#ifdef __cplusplus
}
#endif
