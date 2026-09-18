#pragma once

#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

void ui_init(void);
void ui_loop_iter(void);
void ui_update(const char *line1, const char *line2, bool playing, float volume, float volume_min, float volume_max, float volume_step, int seek_position, int length);
void ui_set_volume_with_range(float vol, float vol_min, float vol_max, float vol_step);  // Update volume ring/label without touching track/progress
void ui_set_status(bool online);
void ui_set_message(const char *msg);
void ui_set_zone_name(const char *zone_name);
void ui_show_zone_picker(const char **zone_names, const char **zone_ids, int zone_count, int selected_idx);
void ui_hide_zone_picker(void);
bool ui_is_zone_picker_visible(void);
int ui_zone_picker_get_selected(void);
void ui_zone_picker_get_selected_id(char *out, size_t len);
void ui_zone_picker_scroll(int delta);
bool ui_zone_picker_is_current_selection(void);  // Returns true if selected zone == current zone
void ui_set_artwork(const char *image_key);  // Set album artwork (placeholder for now)
void ui_set_album(const char *album);  // Album name (line3) - previously received and discarded
void ui_show_volume_change(float vol, float vol_step);  // Show volume overlay when adjusting
void ui_test_pattern(void);  // Debug: Show RGB test pattern to verify color format

// Settings UI (platform-specific implementation)
void ui_show_settings(void);  // Show settings panel (long-press zone label)
void ui_hide_settings(void);  // Hide settings panel
bool ui_is_settings_visible(void);  // Check if settings panel is visible

// OTA update UI
void ui_set_update_available(const char *version);  // Show update notification (NULL to hide)
void ui_set_update_progress(int percent);  // Show update progress (-1 to hide)
void ui_trigger_update(void);  // Called when user taps update notification

// Display state control
void ui_set_controls_visible(bool visible);  // Show/hide UI controls for art mode

// Detail info screen - a third content state (distinct from art mode's
// power/backlight handling in display_sleep.c) showing a thumbnail, title,
// artist, album, and live progress numerics. Entered/exited by
// idf_app/main/platform_display_idf.c's swipe-down/up handling; not gated
// on the ART_MODE display_state_t at all, since this stays fully awake.
void ui_set_detail_mode(bool active);

// True if the last known playback state was "playing" - used by the
// platform layer's swipe gesture handling to decide whether a detail-
// screen swipe down/up should mean "pause"/"play" (see
// platform_display_idf.c's swipe-down/up branching).
bool ui_is_playing(void);

// Large semi-transparent play/pause icon shown briefly (3s) as visual
// confirmation whenever playback is toggled - see common/ui.c's
// ui_show_playback_feedback for exactly what it shows.
void ui_show_playback_feedback(bool now_playing);

// Same large semi-transparent icon overlay, showing skip-next/skip-previous
// instead - shown whenever a next/previous track action fires, from either
// a transport button or a swipe gesture (main, art mode, or detail screen).
void ui_show_track_feedback(bool next);

// True only on the plain Music/Now-Playing screen, false on the TV/Vinyl
// hero-volume screens (see common/ui.c's apply_current_screen). Swipe
// gestures (art mode, detail screen, next/previous track) are Music-only -
// TV/Vinyl have no track/timeline concept for them to act on, and art
// mode's only visible effect there would be hiding the battery/status
// indicators for no reason. See platform_display_idf.c's swipe handling.
bool ui_is_music_screen(void);

// Detail screen's seek-jog: called with the raw accelerated encoder tick
// delta while CONTROLLER_INTERACTION_CONTEXT_SEEK is active (see
// controller_presentation_seek_adjust). Updates a local preview only (arc
// turns amber, numeric label shows the pending position) - the actual
// CONTROLLER_COMMAND_SEEK_TO_SECONDS network call fires once rotation has
// been idle for a short debounce. See common/ui.c for the full state
// machine.
void ui_seek_adjust(int32_t ticks);

// Network status banner (persistent, doesn't auto-clear)
void ui_set_network_status(const char *status);  // Show persistent network status (NULL to clear)

// Battery indicator
void ui_update_battery(void);  // Force battery display refresh (call on USB connect/disconnect)

#ifdef __cplusplus
}
#endif
