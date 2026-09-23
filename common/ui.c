// HiPhi Dial UI - Clean design based on smart-knob approach
// Uses LVGL default theme + minimal manual styling

#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include <math.h>

#include "os_mutex.h"
#include "platform/platform_task.h"
#include "platform/platform_time.h"
#include "platform/platform_http.h"
#include "controller_input.h"
#include "lvgl.h"
#include "ui.h"
#include "bridge_client.h"
#include "ha_volume_client.h"
#include "ha_mute_client.h"
#include "track_title_filter.h"

#ifdef ESP_PLATFORM
#include "esp_log.h"
#include "esp_heap_caps.h"  // PSRAM allocation for the volume ring's canvas buffer
#include "battery.h"
#include "haptic_driver.h"
#include "ui_jpeg.h"  // JPEG decoder helper
#define UI_TAG "ui"
#else
#define UI_TAG "ui"
#define ESP_LOGI(tag, fmt, ...) printf("[I] " tag ": " fmt "\n", ##__VA_ARGS__)
#define ESP_LOGW(tag, fmt, ...) printf("[W] " tag ": " fmt "\n", ##__VA_ARGS__)
#define ESP_LOGE(tag, fmt, ...) printf("[E] " tag ": " fmt "\n", ##__VA_ARGS__)
#endif

#ifdef ESP_PLATFORM
#define SCREEN_SIZE 360
#else
#define SCREEN_SIZE 240
#endif

// Smart-knob inspired color palette - using hex for cleaner code
#define COLOR_WHITE         lv_color_hex(0xffffff)
#define COLOR_GREY          lv_color_hex(0x5a5a5a)
#define COLOR_DARK_GREY     lv_color_hex(0x3c3c3c)

struct ui_state {
    char line1[128];
    char line2[128];
    char line3[128];  // Album - previously received (bridge_client.c) and
                       // discarded (controller_presentation_dial.c); now
                       // shown on the detail info screen (build_detail_overlay).
    char zone_name[64];
    bool playing;
    float volume;
    float volume_min;
    float volume_max;
    bool online;
    float volume_step;

    int seek_position;
    int length;

    // Detail screen enrichment, sourced independently of the rest of this
    // struct (a separate, "fragile" HA-wired service, not Roon/the bridge -
    // Roon's own API has no structured field for any of these, confirmed
    // against the actual Transport/Browse API source rather than assumed).
    // Each one is absent-by-default and shown only when actually set, never
    // blocking or blanking the always-visible title/artist/album/progress
    // group above it if the source is down or a given track has no data -
    // see ui_set_album_year()/ui_set_bit_info()/ui_set_next_track().
    int album_year;       // 0 = unknown/absent
    char bit_info[64];    // e.g. "16-bit / 44.1kHz"; empty = absent
    char next_line1[128]; // Next track title (radio "up next"); empty = absent
    char next_line2[128]; // Next track artist
    bool next_track_none; // Positively known: nothing is coming next (shows "Nothing"); false = unknown
};

// UI widgets - Blue Knob inspired design
static lv_obj_t *s_track_label;        // Main track name
static lv_obj_t *s_artist_label;       // Artist/album
static lv_obj_t *s_volume_canvas;      // Outer volume ring - 256 dots, one per HA click (0.5dB),
                                        // hand-drawn to a cached canvas (see redraw_volume_ring)
                                        // rather than a live lv_scale, so it doesn't get swept into
                                        // every redraw the scrolling title's animation triggers
static void *s_volume_canvas_buf;      // PSRAM pixel buffer backing s_volume_canvas
static int s_volume_canvas_lit_ticks = -1;  // Last-drawn lit-tick count; -1 forces the first draw
static lv_obj_t *s_progress_arc;       // Inner arc for track progress
// 1000 rather than a plain 0-100 percent range - at 0-100, a single arc
// step is (track_length / 100), which for a typical 3-5 minute track is
// 2-3 real seconds regardless of how often the interpolation timer below
// ticks. That's what "still steps every few seconds" turned out to be:
// not a timing problem, a resolution one. 1000 steps make each step a
// fraction of a second on any normal track length.
#define PROGRESS_ARC_MAX 1000
static lv_timer_t *s_progress_interp_timer;  // Advances the arc between polls - see progress_interp_timer_cb
static int s_progress_base_ms = -1;    // Last known real seek position (-1 = no data yet)
static int s_progress_length_ms;       // Track length at the time s_progress_base_ms was recorded
static uint64_t s_progress_base_uptime_ms;  // platform_millis() at the moment s_progress_base_ms arrived
static bool s_progress_is_playing;     // Whether to keep advancing the local estimate
static int s_progress_last_reported_ms = -1;  // Previous poll's raw reported position (-1 = none) - see EXTERNAL_BACKWARD_SEEK_MIN_MS
// A same-track poll reporting a position at least this far BEHIND the
// previous poll's own reported position is a real backward move (restart via
// "previous track", or a seek from another controller), not poll lag: Roon's
// raw seek_position never goes backward during normal playback, so this
// compares raw reports with raw reports rather than with the local estimate
// (which is what the forward-only clamp guards, and is legitimately ahead of
// a lagging poll). Positions are whole seconds, so 2s clears any rounding.
#define EXTERNAL_BACKWARD_SEEK_MIN_MS 2000
static char s_progress_last_line1[128] = "";  // Track identity, to tell a real track change
                                               // apart from ordinary same-track poll noise

// Detail screen's seek-jog (ui_seek_adjust): the encoder owns this arc
// instead of volume there, so it grows to where the volume ring used to be
// (SCREEN_SIZE-30 -> SCREEN_SIZE-10, still leaving a gap to the true edge)
// and thickens 3x (4px -> 12px) for the whole detail-screen visit, not just
// while actively seeking. PROGRESS_ARC_COLOR_NORMAL matches the indicator
// color set at creation time below.
#define PROGRESS_ARC_SIZE_NORMAL (SCREEN_SIZE - 30)
#define PROGRESS_ARC_SIZE_SEEK (SCREEN_SIZE - 10)
#define PROGRESS_ARC_WIDTH_NORMAL 4
#define PROGRESS_ARC_WIDTH_SEEK 12
#define PROGRESS_ARC_COLOR_NORMAL 0x7bb9e8
#define PROGRESS_ARC_COLOR_SEEK 0xffc107
// Idle time after the last encoder tick before a seek preview commits to a
// single CONTROLLER_COMMAND_SEEK_TO_SECONDS call - keeps a fast spin of the
// knob from firing a network seek per detent.
#define SEEK_COMMIT_DEBOUNCE_MS 300
// How long after a seek commit to distrust a poll's reported seek_position
// in favor of our own just-committed value - see apply_state()'s use of
// s_seek_commit_uptime_ms for why. Sized above the bridge's fastest poll
// interval (2s, see wait_for_poll_interval()) so a poll response that was
// already in flight when the seek landed - and therefore still reflects
// Roon's pre-seek zone state - gets one full cycle to be superseded by a
// poll dispatched after the seek, rather than being trusted as fresh.
#define SEEK_RESYNC_GRACE_MS 3000
static bool s_seek_active = false;       // Previewing a seek - polls/interpolation are frozen meanwhile
static int s_seek_start_seconds = 0;     // Where the preview started - s_progress_arc stays frozen here
static int s_seek_preview_seconds = 0;   // Position the preview is currently showing
static int s_seek_step_seconds = 1;      // Seconds per raw encoder tick, sized to the current track's length
static lv_timer_t *s_seek_commit_timer;  // One-shot SEEK_COMMIT_DEBOUNCE_MS after the last tick - see ui_seek_adjust
static uint64_t s_seek_commit_uptime_ms; // platform_millis() at the last seek commit (0 = none yet this boot)
// Amber overlay spanning only the [start, preview] range being scrubbed -
// s_progress_arc itself stays blue and frozen at s_seek_start_seconds for
// the whole preview, so only the actual movement highlights, not the
// entire played portion of the track (owner feedback: the first pass
// recolored the whole ring, which read as "the whole thing is pending"
// rather than "this much is moving"). Same size/rotation/bg_angles as
// s_progress_arc so its indicator angles land on the same ring; driven
// directly via lv_arc_set_start_angle/end_angle rather than
// lv_arc_set_value, since value-based indicators always start from zero.
static lv_obj_t *s_seek_delta_arc;
static lv_obj_t *s_volume_label_large; // Volume display (large, prominent) - primary display
static lv_obj_t *s_volume_label_halo[8]; // 8-directional legibility halo behind s_volume_label_large
static lv_obj_t *s_volume_db_label;    // dB-equivalent readout, at volume's old position
static lv_obj_t *s_volume_db_label_halo[8]; // Same halo technique as s_volume_label_halo, 1px offset
static lv_timer_t *s_volume_emphasis_timer;  // Timer to reset volume emphasis after adjustment
static lv_obj_t *s_status_dot;         // Online/offline indicator
static lv_obj_t *s_battery_icon;       // Battery icon (Material Symbols)
static lv_obj_t *s_battery_badge;      // Small dark tint behind the battery icon for legibility
static lv_obj_t *s_lower_tint;         // Lower-third darkening tint for text legibility
static lv_obj_t *s_btn_prev;           // Previous track button
static lv_obj_t *s_btn_play;           // Play/pause button (center, large)
static lv_obj_t *s_btn_next;           // Next track button
static lv_obj_t *s_play_icon;          // Play/pause icon label
static lv_obj_t *s_background;         // Light background container

// Artwork layers
static lv_obj_t *s_artwork_container;  // Container for artwork layers
static lv_obj_t *s_artwork_image;      // Album art image
static lv_obj_t *s_ui_container;       // Container for all UI widgets (Music/Now Playing)

// TV/Vinyl screens (ADR: docs/meta/decisions/2026-09-14_DESIGN_HYBRID_DIAL_UI.md,
// Screen 2). Sibling of s_ui_container rather than reusing it - no track/
// timeline concept, no transport controls, different volume treatment
// entirely (bigger, centered, no halo - the background photo is already
// dark, unlike variable-brightness album art). s_volume_canvas (the outer
// ring) is shared between all three screens - see its own comment below
// for why it moved out of s_ui_container to make that possible.
static lv_obj_t *s_tv_vinyl_bg;             // Background photo (see tv_background/vinyl_background below)
static lv_obj_t *s_tv_vinyl_container;      // Everything else: labels, gesture regions
static lv_obj_t *s_tv_vinyl_volume_label;   // Hero volume number - 2x xlarge, no halo
static lv_obj_t *s_tv_vinyl_db_label;       // dB-equivalent, above the hero number - 2x font_small, no halo
typedef enum {
    DIAL_SCREEN_MUSIC = 0,
    DIAL_SCREEN_TV,
    DIAL_SCREEN_VINYL,
} dial_screen_t;
static dial_screen_t s_current_screen = DIAL_SCREEN_MUSIC;

// Full-screen mute state (owner direction: roll into the same slice as
// TV/Vinyl rather than as its own later one; shown regardless of which of
// the three screens above is active underneath, since
// input_boolean.audio_mute is a single global state, not per-input).
static lv_obj_t *s_mute_overlay;
static bool s_mute_overlay_visible = false;  // Avoid redundant show/hide calls every poll cycle

// Detail info screen (build_detail_overlay) - a third content state, entered
// by a second swipe-down while controls are already showing, exited by
// swiping up. Full-screen child of s_artwork_container (same parent mute
// overlay uses elsewhere) so it sits above whichever of Music/TV/Vinyl is
// underneath regardless of which is active - moved to the foreground
// explicitly on show rather than relying on creation order, since sibling
// creation order is exactly what caused the volume-ring z-order regression
// earlier this project.
static lv_obj_t *s_detail_overlay;
static lv_obj_t *s_detail_text_group;  // Flex column: stacks title/artist/album/progress without overlap
static lv_obj_t *s_detail_thumbnail_mask;  // Circular clip container - see s_detail_thumbnail comment below
static lv_obj_t *s_detail_thumbnail;   // Album art (shares s_artwork_img's pixel data), child of the mask above
static lv_obj_t *s_detail_title_label;
static lv_obj_t *s_detail_artist_label;
static lv_obj_t *s_detail_album_label;
static lv_obj_t *s_detail_progress_label;  // "2:12 / 4:15" - shares the progress interpolation timer
static bool s_detail_mode_visible = false;
static char s_detail_album_raw[128] = "";  // Album text alone, before any (year) suffix - see apply_detail_album_label()
static int s_detail_album_year = 0;        // 0 = unknown/absent

// Enrichment fields - bit info / "Coming up" / next track. Plain additional
// rows in s_detail_text_group's flex column, same as title/artist/album/
// progress above them: single-line, horizontal marquee-scroll for long
// text (LV_LABEL_LONG_SCROLL_CIRCULAR, matching the Music screen's own
// track/artist labels) rather than wrapping, so every row has a fixed,
// predictable height and nothing here can push the group tall enough to
// need any vertical scrolling. Each hidden when absent - see
// ui_set_bit_info()/ui_set_next_track()'s own comments for why.
static lv_obj_t *s_detail_bitinfo_label;     // e.g. "16-bit / 44.1kHz"
static lv_obj_t *s_detail_next_label;        // "Coming up..." - hidden together with the two below
static lv_obj_t *s_detail_next_title_label;
static lv_obj_t *s_detail_next_artist_label;
// Battery (right of the text block) and volume (left), both at the
// progress row's height - see build_detail_overlay(). No dark tint behind
// the icon like s_battery_badge has on Music: this screen's background is
// already plain black, so the tint would add nothing (owner direction).
static lv_obj_t *s_detail_battery_icon;
static lv_obj_t *s_detail_battery_pct_label;  // "72%" beneath the icon
static lv_obj_t *s_detail_volume_label;       // Numeric volume (format_volume_text)
static lv_obj_t *s_detail_db_label;           // dB-equivalent (derive_volume_db_equivalent)

// Large semi-transparent play/pause confirmation icon - shown briefly
// (ui_show_playback_feedback) whenever playback is toggled, whether from
// the transport button on the main screen or a swipe gesture on the
// detail screen. A child of s_artwork_container (like the mute/detail
// overlays) so it can show above either one.
static lv_obj_t *s_playback_icon_overlay;
static lv_timer_t *s_playback_icon_timer;  // Auto-hides it again - see playback_icon_timer_cb

// Reusable styles - smart-knob inspired
static lv_style_t style_button_primary;    // Center play/pause button
static lv_style_t style_button_secondary;  // Prev/next buttons
static lv_style_t style_button_label;      // Button text labels

// Status bar at bottom
static lv_obj_t *s_status_bar;         // Status bar label at bottom
static lv_timer_t *s_status_timer;     // Timer to clear status messages

// Zone picker - using LVGL list widget (supports per-item icons)
static lv_obj_t *s_zone_picker_overlay;    // Dark background overlay
static lv_obj_t *s_zone_list;              // List widget for zone selection
static bool s_zone_picker_visible = false;
#define MAX_ZONE_PICKER_ZONES 64
#define MAX_ZONE_ID_LEN 48
static char s_zone_picker_ids[MAX_ZONE_PICKER_ZONES][MAX_ZONE_ID_LEN];  // Store zone IDs
static int s_zone_picker_count = 0;
static int s_zone_picker_selected = 0;     // Currently highlighted item
static int s_zone_picker_current = -1;     // Currently active zone (no-op if selected)

// OTA update notification
static lv_obj_t *s_update_btn;             // Update notification button
static char s_update_version[32] = "";     // Available update version
static int s_update_progress = -1;         // Update download progress (-1 = not updating)

// State management
static os_mutex_t s_state_lock = OS_MUTEX_INITIALIZER;
static struct ui_state s_pending = {
    .line1 = "Starting...",
    .line2 = "",
    .line3 = "",
    .zone_name = "",
    .playing = false,
    .volume = 0.0f,
    .volume_min = -80.0f,
    .volume_max = 0.0f,
    .volume_step = 1.0f,
    .online = false,
    .seek_position = 0,
    .length = 0,
    .album_year = 0,
    .bit_info = "",
    .next_line1 = "",
    .next_line2 = "",
    .next_track_none = false,
};
static bool s_dirty = true;
static char s_pending_message[128] = "";
static bool s_message_dirty = false;
static bool s_zone_name_dirty = false;
static char s_network_status[128] = "";   // Persistent network status (doesn't auto-clear)
static bool s_network_status_dirty = false;
static char s_last_image_key[128] = "";  // Track last loaded artwork
static float s_last_predicted_volume = -9999.0f;  // Track user's predicted volume for emphasis suppression
static float s_volume_label_resting_db = -9999.0f;  // dB behind the label's current non-flash color - see reset_volume_emphasis_timer_cb
#ifdef ESP_PLATFORM
static ui_jpeg_image_t s_artwork_img;  // Decoded RGB565 image for artwork (ESP32)
#else
static char *s_artwork_data = NULL;  // Raw JPEG data for PC simulator
#endif

// Fonts - use pre-rendered bitmap fonts on ESP32, built-in on PC
// Two font families: text (Charis SIL) and icons (Material Symbols)
#if !TARGET_PC
#include "font_manager.h"
// TV/Vinyl background photos - see idf_app/main/images/*.c, generated by
// scripts/convert_tv_vinyl_images.py.
extern const lv_image_dsc_t tv_background;
extern const lv_image_dsc_t vinyl_background;
// Text fonts for music metadata
static inline const lv_font_t *font_small(void) { return font_manager_get_small(); }
static inline const lv_font_t *font_normal(void) { return font_manager_get_normal(); }
// Built-in Montserrat 14 (CONFIG_LV_FONT_MONTSERRAT_14, already compiled in):
// ASCII only, so reserve it for text we control or that is ASCII by
// construction (bit info, "Coming up..."), never track/artist names.
static inline const lv_font_t *font_tiny(void) { return &lv_font_montserrat_14; }
static inline const lv_font_t *font_large(void) { return font_manager_get_large(); }
static inline const lv_font_t *font_xlarge(void) { return font_manager_get_xlarge(); }
static inline const lv_font_t *font_xxlarge(void) { return font_manager_get_xxlarge(); }
static inline const lv_font_t *font_db_large(void) { return font_manager_get_db_large(); }
// Icon fonts for UI controls
static inline const lv_font_t *font_icon_small(void) { return font_manager_get_icon_small(); }
static inline const lv_font_t *font_icon_normal(void) { return font_manager_get_icon_normal(); }
static inline const lv_font_t *font_icon_large(void) { return font_manager_get_icon_large(); }
static inline const lv_font_t *font_large_icon(void) { return font_manager_get_large_icon(); }
// Icon aliases (Material Symbols on ESP32)
#define UI_ICON_DOWNLOAD  ICON_DOWNLOAD
#else
// PC fallback - use built-in Montserrat (has LVGL symbols)
static inline const lv_font_t *font_small(void) { return &lv_font_montserrat_20; }
static inline const lv_font_t *font_normal(void) { return &lv_font_montserrat_28; }
static inline const lv_font_t *font_tiny(void) { return &lv_font_montserrat_14; }
static inline const lv_font_t *font_large(void) { return &lv_font_montserrat_48; }
static inline const lv_font_t *font_xlarge(void) { return &lv_font_montserrat_48; }  // PC sim has no 56px asset
static inline const lv_font_t *font_xxlarge(void) { return &lv_font_montserrat_48; }  // PC sim has no 112px asset
static inline const lv_font_t *font_db_large(void) { return &lv_font_montserrat_48; }  // PC sim has no 44px asset
static inline const lv_font_t *font_icon_small(void) { return &lv_font_montserrat_20; }
static inline const lv_font_t *font_icon_normal(void) { return &lv_font_montserrat_28; }
static inline const lv_font_t *font_icon_large(void) { return &lv_font_montserrat_48; }
static inline const lv_font_t *font_large_icon(void) { return &lv_font_montserrat_48; }  // PC sim has no 140px asset
// Icon aliases (LVGL symbols on PC)
#define UI_ICON_DOWNLOAD  LV_SYMBOL_DOWNLOAD
#endif

// Forward declarations
static void apply_state(const struct ui_state *state);
static void build_layout(void);
static void build_detail_overlay(void);
static void poll_pending(lv_timer_t *timer);
static void set_status_dot(bool online);
static void mute_region_long_press_cb(lv_event_t *e);
static void source_region_long_press_cb(lv_event_t *e);
static void build_tv_vinyl_layout(void);
static void build_mute_overlay(void);
static void apply_current_screen(void);
static void apply_mute_overlay(void);
static void btn_prev_event_cb(lv_event_t *e);
static void btn_play_event_cb(lv_event_t *e);
static void btn_next_event_cb(lv_event_t *e);
static void zone_list_item_event_cb(lv_event_t *e);
static void show_status_message(const char *message);
static void clear_status_message_timer_cb(lv_timer_t *timer);
static void update_battery_display(void);
static void battery_poll_timer_cb(lv_timer_t *timer);
static void battery_flash_timer_cb(lv_timer_t *timer);
static void reset_volume_emphasis_timer_cb(lv_timer_t *timer);
static void build_playback_icon_overlay(void);
static void playback_icon_timer_cb(lv_timer_t *timer);
static void seek_commit_timer_cb(lv_timer_t *timer);
static void seek_commit(void);
static void emphasize_volume_label(void);

// ============================================================================
// Progress Time Formatting Helper
// ============================================================================

// "M:SS" (or "MM:SS" past 9:59) - total_ms is whole milliseconds, already
// converted from the bridge's whole-seconds seek_position/length (see
// apply_state()'s comment on that unit mismatch).
static inline void format_mmss(char *buf, size_t len, int total_ms) {
    if (total_ms < 0) {
        total_ms = 0;
    }
    int total_seconds = total_ms / 1000;
    snprintf(buf, len, "%d:%02d", total_seconds / 60, total_seconds % 60);
}

// ============================================================================
// Volume Formatting Helper
// ============================================================================

static inline void format_volume_text(char *buf, size_t len, float volume, float volume_min, float volume_step) {
    float step_abs = volume_step < 0.0f ? -volume_step : volume_step;
    int step_is_fractional = (step_abs - (int)step_abs) > 0.01f;

    if (volume_min < 0.0f) {
        if (step_is_fractional) {
            snprintf(buf, len, "%.1f dB", volume);
        } else {
            snprintf(buf, len, "%.0f dB", volume);
        }
    } else {
        if (step_is_fractional) {
            snprintf(buf, len, "%.1f", volume);
        } else {
            snprintf(buf, len, "%.0f", volume);
        }
    }
}

// The big on-screen number is "volume" as reported by whichever backend is
// active: on Dial's direct-to-HA path that's a 0-255 position, not dB (see
// ha_volume_client.c's db_to_position() / controller_presentation_set_volume_range(
// position, 0, 255, 1) call) - everywhere else (Roon-relative volume on
// Frame/RLCD, or Dial's own Roon fallback) "volume" already *is* dB.
// Detecting the Dial position-scale case by its exact 0..255 range (rather
// than adding a target-specific #ifdef to this shared file) and inverting
// db_to_position's formula gets us the dB-equivalent without new plumbing
// across the controller-boundary layers for a display-only value.
static inline float derive_volume_db_equivalent(float volume, float volume_min, float volume_max) {
    if (volume_min == 0.0f && volume_max == 255.0f) {
        return volume / 2.0f - 127.5f;
    }
    return volume;
}

// Inverse of derive_volume_db_equivalent() - the raw "volume" that would
// display as the given dB. Used to translate the volume-ring's fixed
// amber/red dB thresholds into raw-volume terms once, so the tick index
// each threshold first lights at can be computed with the exact same
// calculate_volume_lit_ticks() rounding the live tick count itself uses -
// see that function's own comment for why the two need to agree exactly.
static inline float volume_for_db_equivalent(float db, float volume_min, float volume_max) {
    if (volume_min == 0.0f && volume_max == 255.0f) {
        return (db + 127.5f) * 2.0f;
    }
    return db;
}

// Loud-volume warning coloring for the volume ring and its numeric label -
// amber above -7.5dB, red above -2.5dB (owner-specified thresholds; on
// Dial's 0-255 position scale via derive_volume_db_equivalent above, that's
// 240 and 250 respectively). Same red/amber hex as the battery warning
// colors elsewhere in this file (update_battery_display), for one
// consistent "entering warning territory" language across the UI rather
// than a second unrelated color pairing.
#define VOLUME_HOT_DB_AMBER -7.5f
#define VOLUME_HOT_DB_RED -2.5f

static inline lv_color_t volume_hot_color_for_db(float db, lv_color_t normal_color) {
    if (db >= VOLUME_HOT_DB_RED) return lv_color_hex(0xff0000);    // Red
    if (db >= VOLUME_HOT_DB_AMBER) return lv_color_hex(0xffaa00);  // Amber
    return normal_color;
}

// Earlier versions of this tried a dark offset duplicate-label shadow
// (visible gap between the number and its shifted ghost - no blur to sell
// the effect), then a black/60%-opacity backdrop panel behind the text
// (owner feedback: read as a big panel, not a shadow - not the intent).
// Currently plain, undecorated text - the bold 56px font on its own
// (font_manager_get_xlarge()) reads fine without either.
//
// NOTE: an even earlier version applied a style-transform scale (2x/1.5x)
// to fake a bigger font (no lv_font_conv toolchain available in this
// build environment at the time to generate a real larger bitmap font).
// That crashed on hardware on the very first frame - Guru Meditation
// Error, LoadProhibited (EXCVADDR 0x0), inside
// lv_draw_sw_blend_color_to_rgb565 <- draw_letter_cb <- lv_draw_label,
// i.e. a null pointer during glyph blending for exactly one of those
// scaled labels. LVGL's software renderer here doesn't safely handle a
// transform-scaled, auto-sized (LV_SIZE_CONTENT) label - most likely the
// widget's logical layout box doesn't grow to match the transform, so the
// draw/layer code sizes its buffer for the small untransformed box and
// then blends into it using the larger transformed coordinates. A real
// bitmap font generated at the target size (see font_manager_get_xlarge())
// is what actually fixed that.
static lv_obj_t *create_number_label(lv_obj_t *parent, const lv_font_t *font,
                                      lv_color_t color, int32_t top_y) {
    lv_obj_t *label = lv_label_create(parent);
    lv_obj_set_style_text_font(label, font, 0);
    lv_obj_set_style_text_color(label, color, 0);
    lv_label_set_text(label, "--");
    lv_obj_align(label, LV_ALIGN_TOP_MID, 0, top_y);

    return label;
}

// Halo for the big volume number specifically (busy album art can still
// wash it out even bold/white) - 8 dark copies of the same text at small
// symmetric offsets all the way around the glyphs, in the exact tint
// already used for the lower-third darkening (black @ 60% opacity), drawn
// before (so it renders behind) the real label. Two earlier attempts at
// this didn't land: a single offset dark copy read as a shifted ghost
// with a visible gap, not a shadow; a solid tint panel behind the text
// read as a big rectangle, not a shadow either. Surrounding the glyphs
// symmetrically on all sides, instead of to one side or as a filled box,
// is what actually reads as a halo.
//
// Offset is a parameter (rather than a fixed macro) so the same technique
// can give the big volume number its established 2px halo and the smaller
// dB label under it a finer 1px halo - a 2px halo on 22px text read too
// thick (owner feedback), same fix applied to s_volume_db_label after it
// was found washing out on pale album art the same way the volume number
// used to.
static const int32_t VOLUME_HALO_UNIT_OFFSETS[8][2] = {
    {-1, 0}, {1, 0},
    {0, -1}, {0, 1},
    {-1, -1}, {1, -1},
    {-1, 1}, {1, 1},
};

static lv_obj_t *create_haloed_number_label(lv_obj_t *parent, const lv_font_t *font,
                                             lv_color_t color, int32_t top_y,
                                             int32_t halo_offset_px,
                                             lv_obj_t *out_halo[8]) {
    for (int i = 0; i < 8; i++) {
        lv_obj_t *halo = lv_label_create(parent);
        lv_obj_set_style_text_font(halo, font, 0);
        lv_obj_set_style_text_color(halo, lv_color_hex(0x000000), 0);
        lv_obj_set_style_text_opa(halo, LV_OPA_60, 0);
        lv_label_set_text(halo, "--");
        lv_obj_align(halo, LV_ALIGN_TOP_MID,
                     VOLUME_HALO_UNIT_OFFSETS[i][0] * halo_offset_px,
                     top_y + VOLUME_HALO_UNIT_OFFSETS[i][1] * halo_offset_px);
        out_halo[i] = halo;
    }

    return create_number_label(parent, font, color, top_y);
}

static inline void set_haloed_label_text(lv_obj_t *label, lv_obj_t *halo[8], const char *text) {
    if (label) lv_label_set_text(label, text);
    for (int i = 0; i < 8; i++) {
        if (halo[i]) lv_label_set_text(halo[i], text);
    }
}

// ============================================================================
// Volume Ring - hand-drawn 256-dot ring, cached to a canvas
// ============================================================================
//
// This used to be a live lv_scale widget (built-in tick rendering). That
// looked right but its bounding box is a near-full-screen square (its
// visible ring is thin, but LVGL invalidates the whole declared widget
// size, not the painted pixels), and the scrolling title sits inside that
// square - so every scroll-animation frame's invalidated area overlapped
// the ring's, and LVGL redrew all 256 ticks (plus a baseline arc) on
// every one of those frames, not just when the volume changed. That's a
// real chunk of extra per-frame work landing right on top of the
// scrolling animation, and is the likely cause of the tearing seen on
// hardware (this display flushes in ~36-row strips - see main_idf.c's
// draw buffer allocation - so a frame that takes too long can leave the
// strips visibly out of sync with each other).
//
// Fix: draw the ticks into an off-screen canvas once, only when the
// volume actually changes (redraw_volume_ring below, gated on
// s_volume_canvas_lit_ticks actually changing) rather than on every LVGL
// refresh. A scroll-animation frame that happens to overlap the canvas
// still has to composite it, but that's now a plain image blit - the
// same cost as the album art already redrawing behind everything - not
// dozens of fresh line-draw calls.
// Full screen diameter, not inset - owner feedback wanted ticks reaching
// the physical edge of the display, not stopping short of it.
#define VOLUME_RING_SIZE SCREEN_SIZE
// 1dB per tick (2 clicks), not 0.5dB/1 click - owner feedback that 256
// ticks was past this display's usable resolution. Dial's native range is
// -127.5..0dB, so 128 ticks over that range is ~1dB/tick.
#define VOLUME_RING_TICK_COUNT 128
#define VOLUME_RING_TICK_WIDTH 4              // Was 2 - owner feedback that fewer (128) ticks needed to be thicker
// Outer end stays at the true screen edge (radius_edge, from the previous
// pass). Inner end is back to 169 - what radius_edge - 6 worked out to
// before that pass enlarged VOLUME_RING_SIZE from SCREEN_SIZE-10 to
// SCREEN_SIZE - because the longer ticks were now reaching inward past
// the progress ring's own radius (165, SCREEN_SIZE-30 sized) and visibly
// crossing it. 180 - 169 = 11.
#define VOLUME_RING_TICK_LEN 11
#define VOLUME_RING_ANGLE_RANGE 359           // Nearly full circle, matches the old arc/scale
#define VOLUME_RING_ROTATION 270              // Start at top (12 o'clock)

// Fraction-of-range -> lit tick count, computed directly from the raw
// volume/min/max rather than through calculate_volume_percentage()'s 0-100
// integer percentage. That 0-100 rounding was an earlier "two ticks per
// click" bug: at VOLUME_RING_TICK_COUNT=256 (one tick per click), an
// integer 0-100 range meant each whole percentage-point step covered
// ~2.56 ticks, so crossing one integer percent (what one volume click
// did, most of the time) lit 2-3 ticks at once. Going straight from the
// native range to a float fraction of VOLUME_RING_TICK_COUNT avoids that
// quantization regardless of what the tick count is currently set to.
static inline int calculate_volume_lit_ticks(float volume, float volume_min, float volume_max) {
    float range = volume_max - volume_min;
    if (range < 0.01f) return 0;
    float frac = (volume - volume_min) / range;
    if (frac < 0.0f) frac = 0.0f;
    if (frac > 1.0f) frac = 1.0f;
    int ticks = (int)lroundf(frac * VOLUME_RING_TICK_COUNT);
    if (ticks < 0) ticks = 0;
    if (ticks > VOLUME_RING_TICK_COUNT) ticks = VOLUME_RING_TICK_COUNT;
    return ticks;
}

static void redraw_volume_ring(float volume, float volume_min, float volume_max) {
    if (!s_volume_canvas) return;

    int lit_ticks = calculate_volume_lit_ticks(volume, volume_min, volume_max);
    if (lit_ticks == s_volume_canvas_lit_ticks) return;  // No visible change - skip the redraw
    s_volume_canvas_lit_ticks = lit_ticks;

    lv_canvas_fill_bg(s_volume_canvas, lv_color_black(), LV_OPA_TRANSP);

    lv_layer_t layer;
    lv_canvas_init_layer(s_volume_canvas, &layer);

    lv_draw_line_dsc_t dsc;
    lv_draw_line_dsc_init(&dsc);

    const int32_t radius_edge = VOLUME_RING_SIZE / 2;
    const lv_point_t center = { radius_edge, radius_edge };

    // Bright blue (owner-confirmed: size/weight/color are good as of this
    // pass), no glow - a glow pass was tried here and reverted (owner
    // feedback: "just looks blurred"). Overridden per-tick below for ticks
    // at/past the amber/red warning thresholds.
    dsc.opa = LV_OPA_COVER;
    dsc.width = VOLUME_RING_TICK_WIDTH;
    dsc.round_start = false;
    dsc.round_end = false;

    // Tick index each threshold first lights at, computed by running the
    // threshold's own dB back through calculate_volume_lit_ticks() - the
    // exact same round() the live tick count above went through. Matching
    // rounding functions is the point: an earlier version compared each
    // tick's own quantized value (floor-based) against these dB thresholds
    // directly, which put it a fraction of a tick out of step with the
    // live count (round-based), so as volume rose one raw unit at a time
    // the *previously lit* leading tick would sometimes flip color when a
    // new tick lit above it - e.g. hardware testing showed 250/251/253/255
    // reading 3/4/5/5 amber and 1/1/1/2 red, an already-colored tick
    // silently changing rather than a new one simply joining. Precomputing
    // these once here means every tick's color is a fixed function of its
    // own index - lit once, colored once, never revisited - so raising the
    // volume can only ever light additional ticks in whatever color that
    // index already had, never recolor one that's already lit.
    const int amber_start_tick = calculate_volume_lit_ticks(
        volume_for_db_equivalent(VOLUME_HOT_DB_AMBER, volume_min, volume_max), volume_min, volume_max);
    const int red_start_tick = calculate_volume_lit_ticks(
        volume_for_db_equivalent(VOLUME_HOT_DB_RED, volume_min, volume_max), volume_min, volume_max);

    // Unlit ticks aren't drawn at all (owner feedback: the dark tint
    // there was "distracting and adds no value") - only the lit 0..
    // lit_ticks range gets a line.
    for (int tick_idx = 0; tick_idx < lit_ticks; tick_idx++) {
        if (tick_idx >= red_start_tick) {
            dsc.color = lv_color_hex(0xff0000);    // Red
        } else if (tick_idx >= amber_start_tick) {
            dsc.color = lv_color_hex(0xffaa00);    // Amber
        } else {
            dsc.color = lv_color_hex(0x4dabff);    // Normal blue
        }

        // Same angle math as lv_scale's own ROUND_INNER tick placement
        // (lv_scale.c's scale_get_tick_points) - tenths of a degree,
        // tick 0 at VOLUME_RING_ROTATION, tick (count-1) at
        // ROTATION+ANGLE_RANGE, then lv_point_transform rotates a point
        // starting due "east" of center by that angle.
        int32_t angle_tenths = (int32_t)(((int64_t)tick_idx * VOLUME_RING_ANGLE_RANGE * 10) /
                                          (VOLUME_RING_TICK_COUNT - 1)) +
                                VOLUME_RING_ROTATION * 10;

        lv_point_t pa = { center.x + radius_edge, center.y };
        lv_point_transform(&pa, angle_tenths, LV_SCALE_NONE, LV_SCALE_NONE, &center, false);

        lv_point_t pb = { center.x + (radius_edge - VOLUME_RING_TICK_LEN), center.y };
        lv_point_transform(&pb, angle_tenths, LV_SCALE_NONE, LV_SCALE_NONE, &center, false);

        dsc.p1 = lv_point_to_precise(&pa);
        dsc.p2 = lv_point_to_precise(&pb);
        lv_draw_line(&layer, &dsc);
    }

    lv_canvas_finish_layer(s_volume_canvas, &layer);
}

// Advances the progress arc's visual position between polls, decoupling
// smoothness from the bridge's battery-conscious poll interval (2-60s
// depending on charging/sleep state - see bridge_client.c's
// wait_for_poll_interval()). Purely a local estimate: last known position
// plus elapsed device time since it arrived, re-synced to the authoritative
// value on every real poll in apply_state() above, so any drift from a
// seek/skip/pause that happened between ticks self-corrects within one poll
// cycle rather than compounding. Cheap even at this cadence -
// lv_arc_set_value() already no-ops when the computed percentage hasn't
// actually moved (checked against LVGL's own source, see apply_state()'s
// comment on the same thing).
static void progress_interp_timer_cb(lv_timer_t *timer) {
    (void)timer;
    if (s_seek_active) {
        return;  // ui_seek_adjust owns the arc/label while a seek is previewing
    }
    if (!s_progress_arc || !s_progress_is_playing || s_progress_base_ms < 0 ||
        s_progress_length_ms <= 0) {
        return;
    }
    uint64_t elapsed_ms = platform_millis() - s_progress_base_uptime_ms;
    int estimated_ms = s_progress_base_ms + (int)elapsed_ms;
    if (estimated_ms > s_progress_length_ms) {
        estimated_ms = s_progress_length_ms;
    }
    int progress_scaled = (estimated_ms * PROGRESS_ARC_MAX) / s_progress_length_ms;
    if (progress_scaled > PROGRESS_ARC_MAX) progress_scaled = PROGRESS_ARC_MAX;
    if (progress_scaled < 0) progress_scaled = 0;
    lv_arc_set_value(s_progress_arc, progress_scaled);

    // Detail screen's "2:12 / 4:15" - only while actually visible, same
    // reasoning as everywhere else in this file that skips work for hidden
    // widgets.
    if (s_detail_mode_visible && s_detail_progress_label) {
        char elapsed_text[16];
        char total_text[16];
        format_mmss(elapsed_text, sizeof(elapsed_text), estimated_ms);
        format_mmss(total_text, sizeof(total_text), s_progress_length_ms);
        char combined[40];
        snprintf(combined, sizeof(combined), "%s / %s", elapsed_text, total_text);
        lv_label_set_text(s_detail_progress_label, combined);
    }
}

// Raw indicator angle (0..359, before s_progress_arc's own 270 rotation is
// added by LVGL) for a position in seconds - mirrors lv_arc's own internal
// value-to-angle mapping (see lv_arc.c's value_update(), LV_ARC_MODE_NORMAL
// case: lv_map(value, min, max, bg_angle_start, bg_angle_end)) so
// s_seek_delta_arc's angle-driven boundary lines up exactly with where
// s_progress_arc's value-driven indicator would end at the same position.
static int32_t seek_arc_angle_for_seconds(int seconds) {
    if (s_progress_length_ms <= 0) {
        return 0;
    }
    int32_t angle = (int32_t)(((int64_t)seconds * 1000 * 359) / s_progress_length_ms);
    if (angle < 0) angle = 0;
    if (angle > 359) angle = 359;
    return angle;
}

// Detail screen's seek-jog - see ui.h for the overall contract. First tick
// of a gesture seeds the preview from wherever the (possibly still-
// interpolating) progress arc currently sits and sizes the per-tick step
// to the track's own length; every tick after that just moves the preview
// and (re)arms the commit debounce. s_progress_arc itself never moves
// during a preview - only s_seek_delta_arc, an amber overlay spanning just
// [s_seek_start_seconds, s_seek_preview_seconds], so the ring shows how far
// the knob has moved rather than recoloring the whole played portion.
void ui_seek_adjust(int32_t ticks) {
    if (ticks == 0 || !s_progress_arc || !s_seek_delta_arc) {
        return;
    }

    // Same auto-unmute as ha_volume_client_adjust(): turning the knob is
    // clearly not an attempt to stay muted. Without this, scrubbing the
    // seek ring while muted moved the ring but left audio silent, with no
    // indication why - unlike volume mode, which already un-mutes here.
    if (ha_volume_client_get_muted()) {
        (void)ha_mute_client_toggle();
    }

    if (!s_seek_active) {
        if (s_progress_length_ms <= 0 || s_progress_base_ms < 0) {
            return;  // No known track/duration yet - nothing to seek within
        }
        int start_ms = s_progress_base_ms;
        if (s_progress_is_playing) {
            start_ms += (int)(platform_millis() - s_progress_base_uptime_ms);
        }
        if (start_ms > s_progress_length_ms) start_ms = s_progress_length_ms;
        if (start_ms < 0) start_ms = 0;
        s_seek_start_seconds = start_ms / 1000;
        s_seek_preview_seconds = s_seek_start_seconds;

        // One tick's worth of seconds, scaled to the track's own length so
        // a full encoder rotation (~20-30 detents) can span a meaningful
        // chunk of any track: 1s/tick floor for short tracks, 30s/tick
        // ceiling so a single detent never jumps further than that on a
        // long podcast/audiobook.
        int length_seconds = s_progress_length_ms / 1000;
        s_seek_step_seconds = length_seconds / 60;
        if (s_seek_step_seconds < 1) s_seek_step_seconds = 1;
        if (s_seek_step_seconds > 30) s_seek_step_seconds = 30;

        s_seek_active = true;
        lv_obj_move_foreground(s_seek_delta_arc);
        lv_obj_remove_flag(s_seek_delta_arc, LV_OBJ_FLAG_HIDDEN);
    }

    int length_seconds = s_progress_length_ms / 1000;
    s_seek_preview_seconds += (int)ticks * s_seek_step_seconds;
    if (s_seek_preview_seconds < 0) s_seek_preview_seconds = 0;
    if (s_seek_preview_seconds > length_seconds) s_seek_preview_seconds = length_seconds;

    int32_t start_angle = seek_arc_angle_for_seconds(s_seek_start_seconds);
    int32_t preview_angle = seek_arc_angle_for_seconds(s_seek_preview_seconds);
    if (start_angle <= preview_angle) {
        lv_arc_set_angles(s_seek_delta_arc, start_angle, preview_angle);
    } else {
        lv_arc_set_angles(s_seek_delta_arc, preview_angle, start_angle);
    }

    if (s_detail_progress_label) {
        char elapsed_text[16];
        char total_text[16];
        format_mmss(elapsed_text, sizeof(elapsed_text), s_seek_preview_seconds * 1000);
        format_mmss(total_text, sizeof(total_text), s_progress_length_ms);
        char combined[40];
        snprintf(combined, sizeof(combined), "%s / %s", elapsed_text, total_text);
        lv_label_set_text(s_detail_progress_label, combined);
    }

    if (s_seek_commit_timer) {
        lv_timer_reset(s_seek_commit_timer);
    } else {
        s_seek_commit_timer = lv_timer_create(seek_commit_timer_cb, SEEK_COMMIT_DEBOUNCE_MS, NULL);
        if (s_seek_commit_timer) {
            lv_timer_set_repeat_count(s_seek_commit_timer, 1);
        }
    }
}

static void seek_commit_timer_cb(lv_timer_t *timer) {
    (void)timer;
    s_seek_commit_timer = NULL;  // One-shot - LVGL already freed it
    seek_commit();
}

// Fires the actual network seek and ends the preview - called either by
// the debounce timer above (rotation went idle) or directly by
// ui_set_detail_mode() if the screen is exited mid-scrub, so an in-flight
// jog is committed rather than silently dropped.
static void seek_commit(void) {
    if (!s_seek_active) {
        return;
    }
    s_seek_active = false;
    if (s_seek_delta_arc) {
        lv_obj_add_flag(s_seek_delta_arc, LV_OBJ_FLAG_HIDDEN);
    }
    if (s_progress_arc && s_progress_length_ms > 0) {
        int progress_scaled = (int)(((int64_t)s_seek_preview_seconds * 1000 * PROGRESS_ARC_MAX) /
                                     s_progress_length_ms);
        if (progress_scaled > PROGRESS_ARC_MAX) progress_scaled = PROGRESS_ARC_MAX;
        if (progress_scaled < 0) progress_scaled = 0;
        lv_arc_set_value(s_progress_arc, progress_scaled);
    }
    controller_action_t action = controller_action_command(
        controller_command_seek_to(s_seek_preview_seconds));
    (void)controller_input_dispatch_action(&action);
    // Optimistic re-sync, matching the play/pause toggle's pattern
    // elsewhere in this file: the next poll still reports the pre-seek
    // position for one cycle, and apply_state()'s "same track, still
    // playing" forward-only clamp would otherwise hold the arc at the OLD
    // position until a later poll catches up. Backward seeks need more
    // than that: the clamp always prefers whichever of {reported, locally
    // estimated} is LATER, which is correct when a stale poll under-reports
    // (normal forward drift) but exactly backwards when a stale poll still
    // shows the pre-seek position after a seek moved us earlier - the clamp
    // would keep the stale, later value forever. s_seek_commit_uptime_ms
    // marks a grace window (see SEEK_RESYNC_GRACE_MS) during which
    // apply_state() trusts this optimistic position outright instead of
    // running that comparison.
    s_progress_base_ms = s_seek_preview_seconds * 1000;
    s_progress_base_uptime_ms = platform_millis();
    s_seek_commit_uptime_ms = s_progress_base_uptime_ms;
}

// ============================================================================
// UI Initialization
// ============================================================================

void ui_init(void) {
    // Don't use theme - it causes ugly color overrides
    // We'll style everything manually for full control

    ESP_LOGI(UI_TAG, "Using ESP_NEW_JPEG software decoder for artwork");

    build_layout();

    // Poll for state updates every 50ms
    lv_timer_t *poll_timer = lv_timer_create(poll_pending, 50, NULL);
    if (poll_timer) {
        lv_timer_set_repeat_count(poll_timer, -1);
    } else {
        ESP_LOGE(UI_TAG, "FAILED to create poll_pending timer!");
    }

    // Periodic battery check for percentage drift (charging state changes trigger immediate updates)
    lv_timer_t *battery_timer = lv_timer_create(battery_poll_timer_cb, 30000, NULL);
    if (battery_timer) {
        lv_timer_set_repeat_count(battery_timer, -1);
    } else {
        ESP_LOGE(UI_TAG, "FAILED to create battery poll timer!");
    }

    // Smooths the progress arc between the much slower bridge polls (see
    // progress_interp_timer_cb's own comment). 250ms, paired with the
    // 1000-step arc range above, keeps steps small and frequent enough to
    // read as continuous motion rather than being wasteful -
    // lv_arc_set_value() no-ops on repeat calls with an unchanged value.
    s_progress_interp_timer = lv_timer_create(progress_interp_timer_cb, 250, NULL);
    if (s_progress_interp_timer) {
        lv_timer_set_repeat_count(s_progress_interp_timer, -1);
    } else {
        ESP_LOGE(UI_TAG, "FAILED to create progress interpolation timer!");
    }
}

// ============================================================================
// Styles - Smart-knob inspired reusable styles
// ============================================================================

static void create_styles(void) {
    // Primary button style (center play/pause) - override ALL theme colors
    lv_style_init(&style_button_primary);
    lv_style_set_radius(&style_button_primary, LV_RADIUS_CIRCLE);
    lv_style_set_bg_color(&style_button_primary, lv_color_hex(0x2c2c2c));  // Dark grey
    lv_style_set_bg_opa(&style_button_primary, LV_OPA_COVER);
    lv_style_set_border_width(&style_button_primary, 3);
    lv_style_set_border_color(&style_button_primary, lv_color_hex(0x5a9fd4));  // Light blue
    lv_style_set_border_opa(&style_button_primary, LV_OPA_COVER);
    lv_style_set_shadow_width(&style_button_primary, 0);

    // Secondary button style (prev/next) - override ALL theme colors
    lv_style_init(&style_button_secondary);
    lv_style_set_radius(&style_button_secondary, LV_RADIUS_CIRCLE);
    lv_style_set_bg_color(&style_button_secondary, lv_color_hex(0x1a1a1a));  // Darker grey
    lv_style_set_bg_opa(&style_button_secondary, LV_OPA_COVER);
    lv_style_set_border_width(&style_button_secondary, 2);
    lv_style_set_border_color(&style_button_secondary, COLOR_GREY);
    lv_style_set_border_opa(&style_button_secondary, LV_OPA_COVER);
    lv_style_set_shadow_width(&style_button_secondary, 0);

    // Button label style
    lv_style_init(&style_button_label);
    lv_style_set_text_color(&style_button_label, lv_color_hex(0xfafafa));  // Off-white
}

// ============================================================================
// Layout - Blue Knob inspired design
// ============================================================================

static void build_layout(void) {
    // Initialize reusable styles first
    create_styles();

    lv_obj_t *screen = lv_screen_active();
    if (!screen) {
        ESP_LOGE(UI_TAG, "lv_screen_active returned NULL!");
        return;
    }

    // Set screen background to pure black
    lv_obj_set_style_bg_color(screen, lv_color_hex(0x000000), 0);
    lv_obj_set_style_bg_opa(screen, LV_OPA_COVER, 0);

    // Create artwork layers - SIMPLIFIED to avoid memory exhaustion
    // No circular clipping (display is already circular)
    // No semi-transparent overlay (would require layer buffering)

    s_artwork_container = lv_obj_create(screen);
    lv_obj_set_size(s_artwork_container, SCREEN_SIZE, SCREEN_SIZE);
    lv_obj_center(s_artwork_container);
    lv_obj_set_style_bg_opa(s_artwork_container, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(s_artwork_container, 0, 0);
    lv_obj_set_style_pad_all(s_artwork_container, 0, 0);

    // Create artwork image (hidden initially, shown when loaded)
    s_artwork_image = lv_img_create(s_artwork_container);
    lv_obj_set_size(s_artwork_image, SCREEN_SIZE, SCREEN_SIZE);
    lv_obj_center(s_artwork_image);
    lv_obj_add_flag(s_artwork_image, LV_OBJ_FLAG_HIDDEN);  // Hidden until artwork loads
    // Full brightness, edge-to-edge (ADR: no full-screen darkening mask -
    // only the lower third gets a tint, added below, just for text
    // legibility over whatever's there).
    lv_obj_set_style_img_opa(s_artwork_image, LV_OPA_COVER, 0);

    // TV/Vinyl background - full-screen wallpaper (ADR Screen 2), the
    // owner's own photos. Stored at 180x180 (see scripts/
    // convert_tv_vinyl_images.py) and scaled up 2x here rather than at
    // native 360x360 - full-res RGB565 for both images doesn't fit the
    // available flash budget; this is a visible-softness/flash-size
    // tradeoff, not a hard technical limit, if that ever needs revisiting.
    // Hidden whenever Music is the active screen - see
    // apply_current_screen(), which also picks TV vs. Vinyl's image.
#if !TARGET_PC
    s_tv_vinyl_bg = lv_img_create(s_artwork_container);
    lv_obj_set_size(s_tv_vinyl_bg, SCREEN_SIZE, SCREEN_SIZE);
    lv_obj_center(s_tv_vinyl_bg);
    lv_image_set_scale(s_tv_vinyl_bg, 512);  // 512/256 = 2x: 180px source -> 360px screen
    lv_obj_remove_flag(s_tv_vinyl_bg, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(s_tv_vinyl_bg, LV_OBJ_FLAG_HIDDEN);
#endif

    // Create UI container directly (no intermediate overlay layer)
    s_ui_container = lv_obj_create(s_artwork_container);
    lv_obj_set_size(s_ui_container, SCREEN_SIZE, SCREEN_SIZE);
    lv_obj_center(s_ui_container);
    lv_obj_set_style_bg_opa(s_ui_container, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(s_ui_container, 0, 0);
    lv_obj_set_style_pad_all(s_ui_container, 0, 0);

    // Update background pointer to ui_container for widget creation
    s_background = s_ui_container;

    // Long-press-by-region gesture regions (top third: mute, lower third:
    // source picker). Created first, so every widget created after this
    // point sits above them in z-order and keeps first claim on its own
    // taps/long-presses - see mute_region_long_press_cb's comment below
    // for why that matters right now. The middle third is deliberately
    // left uncovered by any widget: "no action" is simply what happens
    // when nothing captures the touch there.
    lv_obj_t *mute_region = lv_obj_create(s_ui_container);
    lv_obj_set_size(mute_region, SCREEN_SIZE, SCREEN_SIZE / 3);
    lv_obj_align(mute_region, LV_ALIGN_TOP_LEFT, 0, 0);
    lv_obj_set_style_bg_opa(mute_region, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(mute_region, 0, 0);
    lv_obj_set_style_pad_all(mute_region, 0, 0);
    lv_obj_add_flag(mute_region, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(mute_region, mute_region_long_press_cb,
                        LV_EVENT_LONG_PRESSED, NULL);

    lv_obj_t *source_region = lv_obj_create(s_ui_container);
    lv_obj_set_size(source_region, SCREEN_SIZE, SCREEN_SIZE / 3);
    lv_obj_align(source_region, LV_ALIGN_BOTTOM_LEFT, 0, 0);
    lv_obj_set_style_bg_opa(source_region, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(source_region, 0, 0);
    lv_obj_set_style_pad_all(source_region, 0, 0);
    lv_obj_add_flag(source_region, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(source_region, source_region_long_press_cb,
                        LV_EVENT_LONG_PRESSED, NULL);

    // Lower-third darkening tint - just enough for track/artist text
    // legibility over the artwork; the upper two-thirds stay fully
    // undimmed (unlike the removed full-screen mask). Created before the
    // volume ring/progress arc below (moved here from just before the
    // track/artist labels, owner feedback) so those render on top of the
    // tint rather than under it - the portion of each that crosses the
    // lower third now stays at full brightness instead of getting dimmed
    // along with the artwork. Track/artist labels still render after
    // everything else, so they stay on top as before.
    s_lower_tint = lv_obj_create(s_ui_container);
    lv_obj_set_size(s_lower_tint, SCREEN_SIZE, SCREEN_SIZE / 3);
    lv_obj_align(s_lower_tint, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_set_style_bg_color(s_lower_tint, lv_color_hex(0x000000), 0);
    lv_obj_set_style_bg_opa(s_lower_tint, LV_OPA_60, 0);
    lv_obj_set_style_border_width(s_lower_tint, 0, 0);
    lv_obj_set_style_radius(s_lower_tint, 0, 0);
    lv_obj_remove_flag(s_lower_tint, LV_OBJ_FLAG_CLICKABLE);  // Let long-press reach source_region beneath it

    // Outer volume ring and inner progress arc are built further down, as
    // direct children of s_artwork_container rather than s_ui_container -
    // see that comment for why.

    // Status dot - top right (on the outer ring)
    s_status_dot = lv_obj_create(s_ui_container);
    lv_obj_set_size(s_status_dot, 10, 10);
    lv_obj_set_style_radius(s_status_dot, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_border_width(s_status_dot, 0, 0);
    lv_obj_align(s_status_dot, LV_ALIGN_TOP_RIGHT, -35, 35);

    // Battery icon - top left, mirroring the status dot. A child of
    // s_artwork_container (not s_ui_container) so it stays visible on
    // TV/Vinyl too, same reasoning as the volume ring above - owner
    // feedback that it was missing there (originally missed since the
    // ADR only discussed it in the context of the Music screen's now-
    // removed zone header).
#if !TARGET_PC
    // Small dark badge sitting behind the icon (owner feedback: near-white/
    // amber/red icon color could get lost against light or busy artwork
    // directly behind it - same legibility problem s_lower_tint already
    // solves for track/artist text, just sized tight to this one icon
    // instead of a full-width bar). The icon is a CHILD of this badge
    // (not a sibling positioned on top) specifically so z-order is
    // guaranteed by parent/child nesting rather than sibling creation
    // order - the latter is what caused the volume-ring z-order
    // regression earlier, and is easy to break again in a future edit.
    s_battery_badge = lv_obj_create(s_artwork_container);
    // Sized tight to the icon glyph itself (owner feedback after seeing the
    // first pass on hardware: height -50%, width -30% from the original
    // 40x32 guess) rather than the generous padding a first pass assumed.
    lv_obj_set_size(s_battery_badge, 28, 16);
    lv_obj_align(s_battery_badge, LV_ALIGN_TOP_MID, 0, 25);
    lv_obj_set_style_bg_color(s_battery_badge, lv_color_hex(0x000000), 0);
    lv_obj_set_style_bg_opa(s_battery_badge, LV_OPA_60, 0);
    lv_obj_set_style_border_width(s_battery_badge, 0, 0);
    lv_obj_set_style_radius(s_battery_badge, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_pad_all(s_battery_badge, 0, 0);
    lv_obj_remove_flag(s_battery_badge, LV_OBJ_FLAG_CLICKABLE);  // Let long-press reach source_region beneath it

    s_battery_icon = lv_label_create(s_battery_badge);
    lv_label_set_text(s_battery_icon, ICON_BATTERY_FULL);
    lv_obj_set_style_text_font(s_battery_icon, font_manager_get_lucide_battery(), 0);
    // Near-white (matches the track title, common/ui.c's s_track_label) -
    // the old grey read fine against dark artwork but got lost against
    // light artwork; near-white plus the dark badge behind it now holds up
    // against any artwork color, same as amber/red/flashing-red below.
    lv_obj_set_style_text_color(s_battery_icon, lv_color_hex(0xfafafa), 0);
    lv_obj_center(s_battery_icon);
#endif

    // ========================================================================
    // Now Playing layout (ADR: docs/meta/decisions/
    // 2026-09-14_DESIGN_HYBRID_DIAL_UI.md) - volume/controls/track-info
    // independently positioned in their own thirds (matching the
    // long-press gesture regions' exact boundaries) rather than clustered
    // in one centered flex column. Y-offsets below are a first pass, not
    // pixel-verified against the round display's usable width at each
    // row - expect to tune these by eye on hardware.
    // ========================================================================

    // Volume position - a real bold 56px bitmap font now (idf_app/main/fonts/
    // notosans_bold_56.c, generated via lv_font_conv from a fonttools-
    // instanced static Bold weight - see font_manager_get_xlarge()), not
    // the render-time scale trick that crashed on hardware (see
    // create_number_label's comment above). Bold rather than
    // Regular per owner feedback that the regular weight read too thin at
    // this size. Bottom edge sits ~3mm above the transport buttons (88px
    // tall post owner-requested enlargement, centered on screen -
    // TRANSPORT_BTN_SIZE is defined just below, not yet in scope here),
    // plus a further 1mm (10px) down nudge - owner feedback that it was
    // overlapping the dB label above it. 3mm/1mm assume this is the
    // common 360x360 SH8601 1.43" round AMOLED (~10px/mm) - re-tune
    // PX_PER_MM if that's wrong for this exact panel.
#define PX_PER_MM 10
    {
        const int32_t transport_top_y = SCREEN_SIZE / 2 - 44;
        const int32_t gap_px = 3 * PX_PER_MM;
        const int32_t visual_height = 43;  // notosans_bold_56 line_height (real metric, not scaled/estimated)
        const int32_t overlap_fix_px = 1 * PX_PER_MM;
        const int32_t top_y = transport_top_y - gap_px - visual_height + overlap_fix_px;
        s_volume_label_large = create_haloed_number_label(s_ui_container, font_xlarge(),
                                                            lv_color_hex(0xfafafa), top_y,
                                                            2, s_volume_label_halo);
    }

    // dB-equivalent readout, at the volume number's old position minus a
    // 1.5mm (15px) upward nudge - owner feedback that it was overlapping
    // the (now bigger) volume number below it - in the smaller of the two
    // text fonts this file already uses (font_small(), 22px vs. the
    // volume number's 56px) so it still reads as secondary in size, though
    // not in color/legibility treatment - see VOLUME_HALO_UNIT_OFFSETS
    // above: same halo as the volume number (1px rather than 2px, this
    // text being smaller), same dynamic warning color, both applied at
    // this label's two update sites (polled-state and live-adjustment)
    // right alongside s_volume_label_large's own - owner feedback that it
    // was washing out on pale album art the same way the volume number
    // used to before it got a halo.
    s_volume_db_label = create_haloed_number_label(s_ui_container, font_small(),
                                                     lv_color_hex(0xfafafa),
                                                     55 - (PX_PER_MM + PX_PER_MM / 2),
                                                     1, s_volume_db_label_halo);
#undef PX_PER_MM

    // Controls row - middle third, transport buttons (touch remains
    // reliable per stock testing, so these stay touch targets rather than
    // moving to rotate/click).
    // Positioned manually (not LV_FLEX_ALIGN_SPACE_EVENLY, used in an
    // earlier pass) because the owner wants prev/next pulled in 1mm
    // toward play specifically, not all four gaps shrinking together the
    // way space-evenly would. TRANSPORT_CENTER_OFFSET_PX is the resulting
    // play-to-prev / play-to-next center distance: it started as
    // space-evenly's own math - (SCREEN_SIZE - 3*TRANSPORT_BTN_SIZE)/4 gap
    // between edge and button, so button-center-to-button-center is
    // TRANSPORT_BTN_SIZE + gap - then had the 1mm (10px) inward nudge
    // subtracted.
#define TRANSPORT_BTN_SIZE 88  // +20px (2mm @ ~10px/mm) diameter over the original 68px, per owner feedback
#define TRANSPORT_CENTER_OFFSET_PX 102  // was 112 (88 + 24 gap), minus 10px (1mm) inward nudge

    // Previous button
    s_btn_prev = lv_btn_create(s_ui_container);
    lv_obj_set_size(s_btn_prev, TRANSPORT_BTN_SIZE, TRANSPORT_BTN_SIZE);
    lv_obj_add_style(s_btn_prev, &style_button_secondary, 0);
    lv_obj_add_event_cb(s_btn_prev, btn_prev_event_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_align(s_btn_prev, LV_ALIGN_CENTER, -TRANSPORT_CENTER_OFFSET_PX, 0);
    // Same tint as the lower-third darkening (black, 60% opa) rather than
    // an opaque dark grey, so the buttons read as part of the artwork
    // dimming rather than solid discs sitting on top of it.
    lv_obj_set_style_bg_color(s_btn_prev, lv_color_hex(0x000000), LV_STATE_DEFAULT);
    lv_obj_set_style_bg_opa(s_btn_prev, LV_OPA_60, LV_STATE_DEFAULT);
    lv_obj_set_style_bg_color(s_btn_prev, lv_color_hex(0x3c3c3c), LV_STATE_PRESSED);
    lv_obj_set_style_border_color(s_btn_prev, COLOR_GREY, LV_STATE_DEFAULT);
    lv_obj_set_style_border_color(s_btn_prev, lv_color_hex(0x5a9fd4), LV_STATE_PRESSED);

    lv_obj_t *prev_label = lv_label_create(s_btn_prev);
#if !TARGET_PC
    lv_label_set_text(prev_label, ICON_SKIP_PREV);
    lv_obj_set_style_text_font(prev_label, font_icon_normal(), 0);
#else
    lv_label_set_text(prev_label, LV_SYMBOL_PREV);
    lv_obj_set_style_text_font(prev_label, &lv_font_montserrat_28, 0);
#endif
    lv_obj_add_style(prev_label, &style_button_label, 0);
    lv_obj_center(prev_label);

    // Play/Pause button (center) - same size as prev/next now, kept
    // visually distinguished as the primary action via style_button_primary
    // (accent border) rather than by being physically bigger.
    s_btn_play = lv_btn_create(s_ui_container);
    lv_obj_set_size(s_btn_play, TRANSPORT_BTN_SIZE, TRANSPORT_BTN_SIZE);
    lv_obj_add_style(s_btn_play, &style_button_primary, 0);
    lv_obj_add_event_cb(s_btn_play, btn_play_event_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_align(s_btn_play, LV_ALIGN_CENTER, 0, 0);
    lv_obj_set_style_bg_color(s_btn_play, lv_color_hex(0x000000), LV_STATE_DEFAULT);
    lv_obj_set_style_bg_opa(s_btn_play, LV_OPA_60, LV_STATE_DEFAULT);
    lv_obj_set_style_bg_color(s_btn_play, lv_color_hex(0x3c3c3c), LV_STATE_PRESSED);
    lv_obj_set_style_border_color(s_btn_play, lv_color_hex(0x5a9fd4), LV_STATE_DEFAULT);
    lv_obj_set_style_border_color(s_btn_play, lv_color_hex(0x7bb9e8), LV_STATE_PRESSED);

    s_play_icon = lv_label_create(s_btn_play);
#if !TARGET_PC
    lv_label_set_text(s_play_icon, ICON_PLAY);
    lv_obj_set_style_text_font(s_play_icon, font_icon_normal(), 0);
#else
    lv_label_set_text(s_play_icon, LV_SYMBOL_PLAY);
    lv_obj_set_style_text_font(s_play_icon, &lv_font_montserrat_28, 0);
#endif
    lv_obj_add_style(s_play_icon, &style_button_label, 0);
    lv_obj_center(s_play_icon);

    // Next button
    s_btn_next = lv_btn_create(s_ui_container);
    lv_obj_set_size(s_btn_next, TRANSPORT_BTN_SIZE, TRANSPORT_BTN_SIZE);
    lv_obj_add_style(s_btn_next, &style_button_secondary, 0);
    lv_obj_add_event_cb(s_btn_next, btn_next_event_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_align(s_btn_next, LV_ALIGN_CENTER, TRANSPORT_CENTER_OFFSET_PX, 0);
    lv_obj_set_style_bg_color(s_btn_next, lv_color_hex(0x000000), LV_STATE_DEFAULT);
    lv_obj_set_style_bg_opa(s_btn_next, LV_OPA_60, LV_STATE_DEFAULT);
    lv_obj_set_style_bg_color(s_btn_next, lv_color_hex(0x3c3c3c), LV_STATE_PRESSED);
    lv_obj_set_style_border_color(s_btn_next, COLOR_GREY, LV_STATE_DEFAULT);
    lv_obj_set_style_border_color(s_btn_next, lv_color_hex(0x5a9fd4), LV_STATE_PRESSED);

    lv_obj_t *next_label = lv_label_create(s_btn_next);
#if !TARGET_PC
    lv_label_set_text(next_label, ICON_SKIP_NEXT);
    lv_obj_set_style_text_font(next_label, font_icon_normal(), 0);
#else
    lv_label_set_text(next_label, LV_SYMBOL_NEXT);
    lv_obj_set_style_text_font(next_label, &lv_font_montserrat_28, 0);
#endif
    lv_obj_add_style(next_label, &style_button_label, 0);
    lv_obj_center(next_label);

#undef TRANSPORT_BTN_SIZE
#undef TRANSPORT_CENTER_OFFSET_PX

    // Track/title label - upper row of the lower-third pair (owner
    // feedback: title above artist, not below). Wider than the artist
    // row below: on a round display the chord width available at this
    // height (closer to the screen's vertical center) is noticeably more
    // than at the row below it (closer to the bottom edge, where the
    // circle narrows) - SCREEN_SIZE-100 under-uses it here even though
    // it's the right width one row down. Offset -67: owner feedback said
    // the first pass (-87, 5px clearance) was still too tight against
    // s_lower_tint's top edge, so the whole title/artist pair moved down
    // 2mm (20px @ ~10px/mm) together, preserving their relative spacing.
    s_track_label = lv_label_create(s_ui_container);
    lv_obj_set_width(s_track_label, SCREEN_SIZE - 60);
    lv_obj_set_style_text_font(s_track_label, font_normal(), 0);
    lv_obj_set_style_text_align(s_track_label, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_color(s_track_label, lv_color_hex(0xfafafa), 0);
    lv_label_set_long_mode(s_track_label, LV_LABEL_LONG_SCROLL_CIRCULAR);
    lv_obj_set_style_anim_time(s_track_label, 25000, LV_PART_MAIN);
    lv_label_set_text(s_track_label, s_pending.line1);
    lv_obj_align(s_track_label, LV_ALIGN_BOTTOM_MID, 0, -67);

    // Artist label - lower row of the pair, closer to the bottom edge
    // where the circle narrows; SCREEN_SIZE-100 matched that available
    // width correctly at its original -58 offset (owner-confirmed on
    // hardware). Moved down the same 2mm as the title above, to -38 -
    // note this does mean the visible chord at this new, lower height is
    // narrower than SCREEN_SIZE-100 (~221px vs. 260px), so a
    // near-maximum-length artist name may now clip its outermost few
    // pixels against the round bezel before its own scroll animation
    // kicks in. Only worth revisiting if that's actually visible on
    // hardware with a long real artist name.
    s_artist_label = lv_label_create(s_ui_container);
    lv_obj_set_width(s_artist_label, SCREEN_SIZE - 100);
    lv_obj_set_style_text_font(s_artist_label, font_small(), 0);
    lv_obj_set_style_text_align(s_artist_label, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_color(s_artist_label, lv_color_hex(0xc0c0c0), 0);
    lv_label_set_long_mode(s_artist_label, LV_LABEL_LONG_SCROLL_CIRCULAR);
    lv_obj_set_style_anim_time(s_artist_label, 25000, LV_PART_MAIN);
    lv_label_set_text(s_artist_label, s_pending.line2);
    lv_obj_align(s_artist_label, LV_ALIGN_BOTTOM_MID, 0, -38);

    // Status bar at bottom - for transient messages like "Hi-Fi Control: Connected"
    s_status_bar = lv_label_create(s_ui_container);
    lv_label_set_text(s_status_bar, "");
    lv_obj_set_width(s_status_bar, SCREEN_SIZE - 60);
    lv_obj_set_style_text_font(s_status_bar, font_small(), 0);
    lv_obj_set_style_text_align(s_status_bar, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_color(s_status_bar, lv_color_hex(0x000000), 0);  // Black text
    lv_label_set_long_mode(s_status_bar, LV_LABEL_LONG_DOT);
    // Background styling (hidden by default, shown when message appears)
    lv_obj_set_style_bg_color(s_status_bar, lv_color_hex(0xfafafa), 0);  // Off-white
    lv_obj_set_style_bg_opa(s_status_bar, LV_OPA_TRANSP, 0);  // Hidden initially
    lv_obj_set_style_pad_ver(s_status_bar, 4, 0);
    lv_obj_set_style_pad_hor(s_status_bar, 12, 0);
    lv_obj_set_style_radius(s_status_bar, 8, 0);
    lv_obj_align(s_status_bar, LV_ALIGN_BOTTOM_MID, 0, -25);  // Higher up from edge

    build_tv_vinyl_layout();

    // Outer volume ring and inner progress arc - siblings of
    // s_ui_container/s_tv_vinyl_container (children of s_artwork_container
    // directly), not nested inside either, so both stay visible across all
    // three screens without needing copies, AND can be explicitly raised
    // above s_detail_overlay when the detail info screen is shown
    // (ui_set_detail_mode) - owner feedback that they should stay visible
    // there too rather than getting covered by that screen's opaque
    // background like everything else does. Created here, after both
    // screen containers, so they already draw on top of them by default
    // even outside detail mode (regression fixed once before when the
    // ring briefly sat before s_ui_container instead).
    {
        uint32_t stride = lv_draw_buf_width_to_stride(VOLUME_RING_SIZE, LV_COLOR_FORMAT_ARGB8888);
        size_t buf_size = (size_t)stride * VOLUME_RING_SIZE;
#ifdef ESP_PLATFORM
        s_volume_canvas_buf = heap_caps_malloc(buf_size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
#else
        s_volume_canvas_buf = malloc(buf_size);
#endif
        if (!s_volume_canvas_buf) {
            ESP_LOGE(UI_TAG, "Failed to allocate %u-byte volume ring canvas buffer",
                     (unsigned)buf_size);
        } else {
            s_volume_canvas = lv_canvas_create(s_artwork_container);
            lv_canvas_set_buffer(s_volume_canvas, s_volume_canvas_buf, VOLUME_RING_SIZE,
                                 VOLUME_RING_SIZE, LV_COLOR_FORMAT_ARGB8888);
            lv_obj_center(s_volume_canvas);
            lv_obj_remove_flag(s_volume_canvas, LV_OBJ_FLAG_CLICKABLE);
            redraw_volume_ring(0.0f, 0.0f, 0.0f);  // Forces the canvas's first real draw (transparent/empty until real data arrives)
        }
    }

    s_progress_arc = lv_arc_create(s_artwork_container);
    lv_obj_set_size(s_progress_arc, PROGRESS_ARC_SIZE_NORMAL, PROGRESS_ARC_SIZE_NORMAL);
    lv_obj_center(s_progress_arc);
    lv_arc_set_range(s_progress_arc, 0, PROGRESS_ARC_MAX);
    lv_arc_set_value(s_progress_arc, 0);
    lv_arc_set_bg_angles(s_progress_arc, 0, 359);  // Nearly full circle
    lv_arc_set_rotation(s_progress_arc, 270);  // Start at top (12 o'clock)
    lv_arc_set_mode(s_progress_arc, LV_ARC_MODE_NORMAL);
    lv_obj_set_style_arc_width(s_progress_arc, PROGRESS_ARC_WIDTH_NORMAL, LV_PART_MAIN);
    lv_obj_set_style_arc_width(s_progress_arc, PROGRESS_ARC_WIDTH_NORMAL, LV_PART_INDICATOR);
    lv_obj_remove_flag(s_progress_arc, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_bg_opa(s_progress_arc, LV_OPA_TRANSP, LV_PART_KNOB);
    lv_obj_set_style_pad_all(s_progress_arc, 0, LV_PART_KNOB);

    // Progress arc colors - unplayed track isn't drawn at all (owner
    // feedback: the dark tint there, tried in an earlier pass, was "just
    // distracting and adds no value") - only the played/blue indicator
    // shows. Stays this color even while seek-previewing - see
    // s_seek_delta_arc below for the amber movement indicator.
    lv_obj_set_style_arc_opa(s_progress_arc, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_arc_color(s_progress_arc, lv_color_hex(PROGRESS_ARC_COLOR_NORMAL), LV_PART_INDICATOR);
    lv_obj_set_style_arc_opa(s_progress_arc, LV_OPA_COVER, LV_PART_INDICATOR);

    // Amber seek-delta overlay - see its own declaration comment. Same
    // geometry as s_progress_arc; driven via start/end angle, not value,
    // so it can show an arbitrary [start, preview] span instead of always
    // starting from zero. Created after s_progress_arc so it draws on top.
    s_seek_delta_arc = lv_arc_create(s_artwork_container);
    lv_obj_set_size(s_seek_delta_arc, PROGRESS_ARC_SIZE_SEEK, PROGRESS_ARC_SIZE_SEEK);
    lv_obj_center(s_seek_delta_arc);
    lv_arc_set_bg_angles(s_seek_delta_arc, 0, 359);
    lv_arc_set_rotation(s_seek_delta_arc, 270);
    lv_arc_set_mode(s_seek_delta_arc, LV_ARC_MODE_NORMAL);
    lv_arc_set_angles(s_seek_delta_arc, 0, 0);
    lv_obj_set_style_arc_width(s_seek_delta_arc, PROGRESS_ARC_WIDTH_SEEK, LV_PART_MAIN);
    lv_obj_set_style_arc_width(s_seek_delta_arc, PROGRESS_ARC_WIDTH_SEEK, LV_PART_INDICATOR);
    lv_obj_remove_flag(s_seek_delta_arc, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_bg_opa(s_seek_delta_arc, LV_OPA_TRANSP, LV_PART_KNOB);
    lv_obj_set_style_pad_all(s_seek_delta_arc, 0, LV_PART_KNOB);
    lv_obj_set_style_arc_opa(s_seek_delta_arc, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_arc_color(s_seek_delta_arc, lv_color_hex(PROGRESS_ARC_COLOR_SEEK), LV_PART_INDICATOR);
    lv_obj_set_style_arc_opa(s_seek_delta_arc, LV_OPA_COVER, LV_PART_INDICATOR);
    lv_obj_add_flag(s_seek_delta_arc, LV_OBJ_FLAG_HIDDEN);

    build_mute_overlay();
    build_detail_overlay();
    build_playback_icon_overlay();
}

// Detail screen artwork: a circle rather than the old square, per owner
// request. Top edge at y=0 - the topmost pixel the round display has at its
// horizontal center (a circle inscribed in a SCREEN_SIZE square touches
// row 0 only exactly at the center column) - so it starts as high as the
// glass allows. Bottom edge 5px past the old square's own bottom (100+5=105,
// then +5=110), so the circle is a little bigger than the square was
// overall, at the cost of the square's corners (now outside the circle).
#define DETAIL_ARTWORK_TOP_Y 0
#define DETAIL_ARTWORK_SIZE 110  // old square was 100; bottom edge = 0 + 110 = 105(old bottom) + 5

// Detail screen text geometry. The round panel is SCREEN_SIZE across, and in
// detail mode the progress arc is PROGRESS_ARC_SIZE_SEEK wide with
// PROGRESS_ARC_WIDTH_SEEK thick, drawn inside its own bounds. Text has to
// stay inside the arc's inner edge plus a small margin, so the usable radius
// is the arc's inner radius less DETAIL_TEXT_MARGIN.
#define DETAIL_TEXT_TOP 113  // 3px below the (now circular, larger) thumbnail - ends y=110
#define DETAIL_TEXT_PAD_ROW 1
#define DETAIL_TEXT_MAX_WIDTH (SCREEN_SIZE - 20)  // chord width governs; this only bounds it
#define DETAIL_TEXT_MARGIN 2
#define DETAIL_TEXT_MIN_WIDTH 60
#define DETAIL_TEXT_RADIUS \
    (PROGRESS_ARC_SIZE_SEEK / 2 - PROGRESS_ARC_WIDTH_SEEK - DETAIL_TEXT_MARGIN)
// The "Coming up" title/artist rows ignore the progress arc and use the
// physical panel edge instead (owner direction: the arc may be overdrawn
// there, they just must not clip on the glass). The battery/volume badges
// (see build_detail_overlay) use this same radius for the same reason.
#define DETAIL_TEXT_RADIUS_FULL (SCREEN_SIZE / 2 - 4)

static int32_t detail_isqrt(int32_t v) {
    int32_t r = 0;
    while ((r + 1) * (r + 1) <= v) {
        r++;
    }
    return r;
}

// Widest a single text row may be, given where it sits vertically: the chord
// of the usable circle at whichever of the row's top/bottom edges is farthest
// from the panel's centre (the narrowest point the row's ink can reach).
// Rows below centre are limited by their bottom edge, rows above by their top.
static int32_t detail_row_width_r(int32_t row_top, int32_t row_height, int32_t r) {
    const int32_t centre = SCREEN_SIZE / 2;
    int32_t dy_top = row_top - centre;
    int32_t dy_bottom = row_top + row_height - centre;
    if (dy_top < 0) dy_top = -dy_top;
    if (dy_bottom < 0) dy_bottom = -dy_bottom;
    int32_t dy = dy_top > dy_bottom ? dy_top : dy_bottom;
    int32_t width = dy >= r ? 0 : 2 * detail_isqrt(r * r - dy * dy);
    if (width > DETAIL_TEXT_MAX_WIDTH) width = DETAIL_TEXT_MAX_WIDTH;
    if (width < DETAIL_TEXT_MIN_WIDTH) width = DETAIL_TEXT_MIN_WIDTH;
    return width;
}

static int32_t detail_row_width(int32_t row_top, int32_t row_height) {
    return detail_row_width_r(row_top, row_height, DETAIL_TEXT_RADIUS);
}

// Detail info screen (see ui_set_detail_mode) - thumbnail in the top third,
// title/artist/album/progress stacked around center, matching the same
// "first pass, tune on hardware by eye" spirit as the rest of this layout
// rather than pixel-verified boundaries. No lowest-third "coming up next" -
// deliberately left out, not even as a placeholder, since neither the
// bridge nor Home Assistant currently exposes queue/next-track data (see
// the design discussion this was built from); revisit once that data
// exists rather than shipping an empty promise.
static void build_detail_overlay(void) {
    s_detail_overlay = lv_obj_create(s_artwork_container);
    lv_obj_set_size(s_detail_overlay, SCREEN_SIZE, SCREEN_SIZE);
    lv_obj_center(s_detail_overlay);
    lv_obj_set_style_bg_color(s_detail_overlay, lv_color_hex(0x000000), 0);
    lv_obj_set_style_bg_opa(s_detail_overlay, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(s_detail_overlay, 0, 0);
    lv_obj_set_style_radius(s_detail_overlay, 0, 0);
    lv_obj_set_style_pad_all(s_detail_overlay, 0, 0);
    // Swallow touches like the mute overlay does, rather than letting them
    // fall through to whatever long-press regions sit on the screen
    // underneath - swipe up (handled at the platform layer, not as an
    // LVGL event) is the only way out. A long-press anywhere on this
    // screen mutes, same action and callback as the top-third region on
    // Music/TV/Vinyl - no top/bottom split here since there's nothing
    // else to long-press for on this screen (owner direction).
    lv_obj_add_flag(s_detail_overlay, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(s_detail_overlay, mute_region_long_press_cb,
                        LV_EVENT_LONG_PRESSED, NULL);
    lv_obj_add_flag(s_detail_overlay, LV_OBJ_FLAG_HIDDEN);
    // Never scrollable - this used to be implicitly scrollable (default
    // LVGL object behavior when children overflow it), which is what let a
    // long wrapped title push the thumbnail out of view in the first
    // place. Now moot in practice too: every text row below switched from
    // wrapping to single-line marquee scroll (LV_LABEL_LONG_SCROLL_CIRCULAR),
    // so nothing here grows tall enough to overflow regardless of text
    // length - kept as a defensive belt-and-suspenders, not load-bearing.
    lv_obj_remove_flag(s_detail_overlay, LV_OBJ_FLAG_SCROLLABLE);

    // Thumbnail - shares s_artwork_img's pixel buffer via lv_image_set_src,
    // scaled down (see ui_set_artwork()); not its own decode/copy. Circular
    // rather than the old square - see DETAIL_ARTWORK_TOP_Y/SIZE above for
    // the geometry reasoning.
    //
    // Radius+clip_corner set directly on an lv_img/lv_image object does NOT
    // clip that widget's own image content on real hardware (confirmed:
    // stayed square even with correct size/position and DRAW_SW_COMPLEX
    // enabled) - clip_corner reliably clips CHILDREN of a plain container,
    // not necessarily a specialized widget's own main draw. So the circle
    // is a separate mask container, and the image is its child, filling it
    // completely - the well-supported "circular avatar" pattern.
    s_detail_thumbnail_mask = lv_obj_create(s_detail_overlay);
    lv_obj_set_size(s_detail_thumbnail_mask, DETAIL_ARTWORK_SIZE, DETAIL_ARTWORK_SIZE);
    lv_obj_align(s_detail_thumbnail_mask, LV_ALIGN_TOP_MID, 0, DETAIL_ARTWORK_TOP_Y);
    lv_obj_set_style_radius(s_detail_thumbnail_mask, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_clip_corner(s_detail_thumbnail_mask, true, 0);
    lv_obj_set_style_bg_opa(s_detail_thumbnail_mask, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(s_detail_thumbnail_mask, 0, 0);
    lv_obj_set_style_pad_all(s_detail_thumbnail_mask, 0, 0);
    lv_obj_remove_flag(s_detail_thumbnail_mask, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(s_detail_thumbnail_mask, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(s_detail_thumbnail_mask, LV_OBJ_FLAG_HIDDEN);  // Hidden until artwork loads, same as s_artwork_image

    s_detail_thumbnail = lv_img_create(s_detail_thumbnail_mask);
    lv_obj_set_size(s_detail_thumbnail, DETAIL_ARTWORK_SIZE, DETAIL_ARTWORK_SIZE);
    lv_obj_set_pos(s_detail_thumbnail, 0, 0);

    // Title/artist/album/progress stack in a flex column rather than at
    // fixed offsets from each other - a long title or album name wraps to
    // a second line (LV_LABEL_LONG_DOT was set here originally, but that
    // mode only truncates with "..." when the label's HEIGHT is also
    // constrained; these only had a width limit, so long text just wrapped
    // instead, and the fixed offsets below made the next element overlap
    // it). A flex column with each label's height left at its natural
    // content size means a wrapped 2-line title pushes the artist/album/
    // progress lines below it down instead of colliding with them.
    // Anchored below the thumbnail and growing downward, rather than kept
    // centered as a block, specifically so it can only ever grow away
    // from the thumbnail, never into it, regardless of how many lines
    // wrap on a given track.
    s_detail_text_group = lv_obj_create(s_detail_overlay);
    lv_obj_set_size(s_detail_text_group, DETAIL_TEXT_MAX_WIDTH, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_opa(s_detail_text_group, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(s_detail_text_group, 0, 0);
    lv_obj_set_style_pad_all(s_detail_text_group, 0, 0);
    lv_obj_set_style_pad_row(s_detail_text_group, DETAIL_TEXT_PAD_ROW, 0);
    lv_obj_set_layout(s_detail_text_group, LV_LAYOUT_FLEX);
    lv_obj_set_flex_flow(s_detail_text_group, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(s_detail_text_group, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_remove_flag(s_detail_text_group, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_align(s_detail_text_group, LV_ALIGN_TOP_MID, 0, DETAIL_TEXT_TOP);
    int32_t row_y = DETAIL_TEXT_TOP;  // running top of the next row, for detail_row_width()

    // LV_LABEL_LONG_SCROLL_CIRCULAR (horizontal marquee for text wider than
    // the label, matching the Music screen's own s_track_label/s_artist_label
    // - see their own comments) rather than LV_LABEL_LONG_WRAP, which is
    // what let a long title/artist/album grow past one line each and push
    // this whole group tall enough to need scrolling in the first place
    // (owner example: a long YES title left the next track's view scrolled
    // down with the thumbnail out of view). Single-line + marquee keeps
    // every row here a fixed height, so title/artist/album/progress/
    // enrichment together never exceed the screen regardless of text
    // length - nothing on this screen needs to scroll vertically anymore.
    s_detail_title_label = lv_label_create(s_detail_text_group);
    lv_obj_set_width(s_detail_title_label, detail_row_width(row_y, lv_font_get_line_height(font_normal())));
    row_y += lv_font_get_line_height(font_normal()) + DETAIL_TEXT_PAD_ROW;
    lv_obj_set_style_text_font(s_detail_title_label, font_normal(), 0);
    lv_obj_set_style_text_align(s_detail_title_label, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_color(s_detail_title_label, lv_color_hex(0xfafafa), 0);
    lv_label_set_long_mode(s_detail_title_label, LV_LABEL_LONG_SCROLL_CIRCULAR);
    lv_obj_set_style_anim_time(s_detail_title_label, 25000, LV_PART_MAIN);

    s_detail_artist_label = lv_label_create(s_detail_text_group);
    lv_obj_set_width(s_detail_artist_label, detail_row_width(row_y, lv_font_get_line_height(font_small())));
    row_y += lv_font_get_line_height(font_small()) + DETAIL_TEXT_PAD_ROW;
    lv_obj_set_style_text_font(s_detail_artist_label, font_small(), 0);
    lv_obj_set_style_text_align(s_detail_artist_label, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_color(s_detail_artist_label, lv_color_hex(0xc0c0c0), 0);
    lv_label_set_long_mode(s_detail_artist_label, LV_LABEL_LONG_SCROLL_CIRCULAR);
    lv_obj_set_style_anim_time(s_detail_artist_label, 25000, LV_PART_MAIN);

    // Album (+ year, once available - see apply_detail_album_label()) is
    // the longest-running text on this row (title (year) can be long), so
    // it's the one most likely to actually need its marquee in practice.
    s_detail_album_label = lv_label_create(s_detail_text_group);
    lv_obj_set_width(s_detail_album_label, detail_row_width(row_y, lv_font_get_line_height(font_small())));
    row_y += lv_font_get_line_height(font_small()) + DETAIL_TEXT_PAD_ROW;
    lv_obj_set_style_text_font(s_detail_album_label, font_small(), 0);
    lv_obj_set_style_text_align(s_detail_album_label, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_color(s_detail_album_label, lv_color_hex(0x888888), 0);
    lv_label_set_long_mode(s_detail_album_label, LV_LABEL_LONG_SCROLL_CIRCULAR);
    lv_obj_set_style_anim_time(s_detail_album_label, 25000, LV_PART_MAIN);

    // Captured before s_detail_progress_label's own row_y advances below -
    // used to place the battery/volume badges (further down) at this exact
    // row's height, computed from the real font metrics rather than a
    // guessed constant, same as every other row on this screen.
    int32_t progress_row_top = row_y;
    int32_t progress_row_height = lv_font_get_line_height(font_small());

    s_detail_progress_label = lv_label_create(s_detail_text_group);
    lv_obj_set_width(s_detail_progress_label, detail_row_width(row_y, lv_font_get_line_height(font_small())));
    row_y += lv_font_get_line_height(font_small()) + DETAIL_TEXT_PAD_ROW;
    lv_obj_set_style_text_font(s_detail_progress_label, font_small(), 0);
    lv_obj_set_style_text_align(s_detail_progress_label, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_color(s_detail_progress_label, lv_color_hex(0x7bb9e8), 0);
    lv_label_set_text(s_detail_progress_label, "");

    // Battery (right of the text block) and volume (left), vertically
    // centered on the progress row - the one row of this block that's
    // always present once something is playing (bit info/next-track can
    // both be hidden). Sit in the margin between the (narrower,
    // arc-constrained) text column and the physical glass edge, using the
    // same DETAIL_TEXT_RADIUS_FULL "safe to the glass" radius the Coming-up
    // rows already use (owner direction there: the arc may be overdrawn,
    // just don't clip on the glass). No dark tint behind the battery icon
    // like s_battery_badge has on Music - this screen's background is
    // already plain black, so the tint would add nothing (owner direction).
#if !TARGET_PC
    {
#define DETAIL_BADGE_WIDTH 40
#define DETAIL_BADGE_MARGIN 4
// The volume/dB text ("-63.5 dB" etc.) needs more room than the battery
// badge does ("100%"). Widening just the label and not its parent had no
// effect - LVGL clips a child to its parent's own box by default, so the
// container (volume_group) has to be this wide too, not only the label
// inside it. Generous on purpose (was 64, still clipped "-"/"B" on
// hardware) rather than re-guessing pixel-tight again.
#define DETAIL_DB_LABEL_WIDTH 88
        int32_t row_centre_y = progress_row_top + progress_row_height / 2;
        row_centre_y += 10;  // 1mm down, away from the scrolling text above - owner hardware feedback
        int32_t half_span = detail_row_width_r(progress_row_top, progress_row_height,
                                                 DETAIL_TEXT_RADIUS_FULL) / 2;
        int32_t inset = half_span - DETAIL_BADGE_MARGIN - DETAIL_BADGE_WIDTH;
        inset -= 30;  // 3mm closer to centre (PX_PER_MM=10 elsewhere in this file) - owner hardware feedback
        if (inset < 4) inset = 4;  // defensive floor - see this screen's own "tune by eye" norm
        // Centre-x of the volume badge, held fixed at what the narrow
        // DETAIL_BADGE_WIDTH box would have used, so widening it to fit the
        // dB text grows outward from the same visual centre rather than
        // shifting the whole thing sideways.
        int32_t volume_centre_x = SCREEN_SIZE / 2 - inset - DETAIL_BADGE_WIDTH / 2;

        int32_t icon_h = lv_font_get_line_height(font_manager_get_lucide_battery());
        int32_t tiny_h = lv_font_get_line_height(font_tiny());
        int32_t battery_h = icon_h + 2 + tiny_h;
        int32_t volume_h = tiny_h + 2 + tiny_h;

        lv_obj_t *battery_group = lv_obj_create(s_detail_overlay);
        lv_obj_set_size(battery_group, DETAIL_BADGE_WIDTH, battery_h);
        lv_obj_set_pos(battery_group, SCREEN_SIZE / 2 + inset, row_centre_y - battery_h / 2);
        lv_obj_set_style_bg_opa(battery_group, LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_width(battery_group, 0, 0);
        lv_obj_set_style_pad_all(battery_group, 0, 0);
        lv_obj_remove_flag(battery_group, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_remove_flag(battery_group, LV_OBJ_FLAG_SCROLLABLE);

        s_detail_battery_icon = lv_label_create(battery_group);
        lv_label_set_text(s_detail_battery_icon, ICON_BATTERY_FULL);
        lv_obj_set_style_text_font(s_detail_battery_icon, font_manager_get_lucide_battery(), 0);
        lv_obj_set_style_text_color(s_detail_battery_icon, lv_color_hex(0xfafafa), 0);
        lv_obj_set_width(s_detail_battery_icon, DETAIL_BADGE_WIDTH);
        lv_obj_set_style_text_align(s_detail_battery_icon, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_align(s_detail_battery_icon, LV_ALIGN_TOP_MID, 0, 0);

        s_detail_battery_pct_label = lv_label_create(battery_group);
        lv_obj_set_style_text_font(s_detail_battery_pct_label, font_tiny(), 0);
        lv_obj_set_style_text_align(s_detail_battery_pct_label, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_set_style_text_color(s_detail_battery_pct_label, lv_color_hex(0x888888), 0);
        lv_obj_set_width(s_detail_battery_pct_label, DETAIL_BADGE_WIDTH);
        // Long mode defaults to WRAP; at this badge width that can wrap to a
        // second line and grow the label taller than battery_h assumes,
        // pushing this bottom-aligned label up into the icon above it -
        // force single-line instead (this file always sets long_mode
        // explicitly elsewhere; these badge labels were the exception).
        lv_label_set_long_mode(s_detail_battery_pct_label, LV_LABEL_LONG_CLIP);
        lv_label_set_text(s_detail_battery_pct_label, "");
        lv_obj_align(s_detail_battery_pct_label, LV_ALIGN_BOTTOM_MID, 0, 0);

        // Volume - left side, mirrored. Numeric on top (primary weight),
        // dB below (secondary) - same relative roles format_volume_text()/
        // derive_volume_db_equivalent() already play on the Music screen's
        // big volume readout (dB is the smaller, dimmer of that pair too).
        lv_obj_t *volume_group = lv_obj_create(s_detail_overlay);
        lv_obj_set_size(volume_group, DETAIL_DB_LABEL_WIDTH, volume_h);
        lv_obj_set_pos(volume_group, volume_centre_x - DETAIL_DB_LABEL_WIDTH / 2, row_centre_y - volume_h / 2);
        lv_obj_set_style_bg_opa(volume_group, LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_width(volume_group, 0, 0);
        lv_obj_set_style_pad_all(volume_group, 0, 0);
        lv_obj_remove_flag(volume_group, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_remove_flag(volume_group, LV_OBJ_FLAG_SCROLLABLE);

        s_detail_volume_label = lv_label_create(volume_group);
        lv_obj_set_style_text_font(s_detail_volume_label, font_tiny(), 0);
        lv_obj_set_style_text_align(s_detail_volume_label, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_set_style_text_color(s_detail_volume_label, lv_color_hex(0xfafafa), 0);
        // Wider than the notional badge column - format_volume_text() can
        // itself emit a "-XX.X dB"-style string (Roon-relative volume
        // scale), same width risk as s_detail_db_label below.
        lv_obj_set_width(s_detail_volume_label, DETAIL_DB_LABEL_WIDTH);
        // See s_detail_battery_pct_label above - same wrap-causes-overlap fix.
        lv_label_set_long_mode(s_detail_volume_label, LV_LABEL_LONG_CLIP);
        lv_label_set_text(s_detail_volume_label, "");
        lv_obj_align(s_detail_volume_label, LV_ALIGN_TOP_MID, 0, 0);

        s_detail_db_label = lv_label_create(volume_group);
        lv_obj_set_style_text_font(s_detail_db_label, font_tiny(), 0);
        lv_obj_set_style_text_align(s_detail_db_label, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_set_style_text_color(s_detail_db_label, lv_color_hex(0x888888), 0);
        // "-XX.X dB" clipped at the narrower DETAIL_BADGE_WIDTH lost its
        // leading "-" and trailing "B" (centered text, box too narrow) -
        // owner hardware feedback.
        lv_obj_set_width(s_detail_db_label, DETAIL_DB_LABEL_WIDTH);
        lv_label_set_long_mode(s_detail_db_label, LV_LABEL_LONG_CLIP);
        lv_label_set_text(s_detail_db_label, "");
        lv_obj_align(s_detail_db_label, LV_ALIGN_BOTTOM_MID, 0, 0);
#undef DETAIL_BADGE_WIDTH
#undef DETAIL_BADGE_MARGIN
#undef DETAIL_DB_LABEL_WIDTH
    }
#endif

    // Enrichment rows - bit info / "Coming up" / next track. Plain further
    // children of the same s_detail_text_group flex column above (not a
    // separate container) - now that every row is single-line, they just
    // stack after progress with no special positioning needed. Every label
    // here starts hidden: there's no live data source for any of them yet
    // (Roon's own API has no year/next-track/bit-depth field, confirmed
    // directly against its source rather than assumed), so this is the
    // display layer built ahead of that landing, not wired to anything
    // that populates it yet - see ui_set_bit_info()/ui_set_next_track().
    s_detail_bitinfo_label = lv_label_create(s_detail_text_group);
    lv_obj_set_width(s_detail_bitinfo_label, detail_row_width(row_y, lv_font_get_line_height(font_tiny())));
    row_y += lv_font_get_line_height(font_tiny()) + DETAIL_TEXT_PAD_ROW;
    lv_obj_set_style_text_font(s_detail_bitinfo_label, font_tiny(), 0);
    lv_obj_set_style_text_align(s_detail_bitinfo_label, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_color(s_detail_bitinfo_label, lv_color_hex(0x888888), 0);
    lv_label_set_text(s_detail_bitinfo_label, "");
    lv_obj_add_flag(s_detail_bitinfo_label, LV_OBJ_FLAG_HIDDEN);

    s_detail_next_label = lv_label_create(s_detail_text_group);
    lv_obj_set_width(s_detail_next_label, detail_row_width(row_y, lv_font_get_line_height(font_tiny())));
    row_y += lv_font_get_line_height(font_tiny()) + DETAIL_TEXT_PAD_ROW;
    lv_obj_set_style_text_font(s_detail_next_label, font_tiny(), 0);
    lv_obj_set_style_text_align(s_detail_next_label, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_color(s_detail_next_label, lv_color_hex(0x888888), 0);
    lv_label_set_text(s_detail_next_label, "Coming up...");
    lv_obj_add_flag(s_detail_next_label, LV_OBJ_FLAG_HIDDEN);

    // Next-track title/artist stay font_small() (Lato, full Unicode coverage:
    // names like "Bjork"/"Beyonce" with accents must render) - only the
    // ASCII-only "Coming up..." caption and bit info drop to font_tiny().
    // Each row's width comes from detail_row_width(): narrower the closer
    // it sits to the bottom of the round panel, so the marquee's clip
    // region stays inside the circle rather than the arc.
    s_detail_next_title_label = lv_label_create(s_detail_text_group);
    lv_obj_set_width(s_detail_next_title_label, detail_row_width_r(row_y, lv_font_get_line_height(font_small()),
                                                      DETAIL_TEXT_RADIUS_FULL));
    row_y += lv_font_get_line_height(font_small()) + DETAIL_TEXT_PAD_ROW;
    lv_obj_set_style_text_font(s_detail_next_title_label, font_small(), 0);
    lv_obj_set_style_text_align(s_detail_next_title_label, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_color(s_detail_next_title_label, lv_color_hex(0xc0c0c0), 0);
    lv_label_set_long_mode(s_detail_next_title_label, LV_LABEL_LONG_SCROLL_CIRCULAR);
    lv_obj_set_style_anim_time(s_detail_next_title_label, 25000, LV_PART_MAIN);
    lv_label_set_text(s_detail_next_title_label, "");
    lv_obj_add_flag(s_detail_next_title_label, LV_OBJ_FLAG_HIDDEN);

    s_detail_next_artist_label = lv_label_create(s_detail_text_group);
    lv_obj_set_width(s_detail_next_artist_label, detail_row_width_r(row_y, lv_font_get_line_height(font_small()),
                                                      DETAIL_TEXT_RADIUS_FULL));
    row_y += lv_font_get_line_height(font_small()) + DETAIL_TEXT_PAD_ROW;
    lv_obj_set_style_text_font(s_detail_next_artist_label, font_small(), 0);
    lv_obj_set_style_text_align(s_detail_next_artist_label, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_color(s_detail_next_artist_label, lv_color_hex(0x888888), 0);
    lv_label_set_long_mode(s_detail_next_artist_label, LV_LABEL_LONG_SCROLL_CIRCULAR);
    lv_obj_set_style_anim_time(s_detail_next_artist_label, 25000, LV_PART_MAIN);
    lv_label_set_text(s_detail_next_artist_label, "");
    lv_obj_add_flag(s_detail_next_artist_label, LV_OBJ_FLAG_HIDDEN);
}

// Combines s_detail_album_raw with s_detail_album_year (if known) into
// s_detail_album_label - "Crime of the Century (1974)" - or just the plain
// album text if the year is unknown, gracefully dropping the parentheses
// entirely rather than showing them empty. Called both when the album text
// itself changes (apply_state(), on a poll) and when the year arrives
// separately (ui_set_album_year(), from whatever eventually calls it), so
// either arriving first still ends up correct without waiting on the other.
static void apply_detail_album_label(void) {
    if (!s_detail_album_label) {
        return;
    }
    if (s_detail_album_year > 0 && s_detail_album_raw[0] != '\0') {
        char combined[160];
        snprintf(combined, sizeof(combined), "%s (%d)", s_detail_album_raw, s_detail_album_year);
        lv_label_set_text(s_detail_album_label, combined);
    } else {
        lv_label_set_text(s_detail_album_label, s_detail_album_raw);
    }
}

// Large semi-transparent play/pause confirmation icon (see
// ui_show_playback_feedback) - a plain label, not an opaque overlay like
// mute/detail: it's meant to sit visibly on top of whatever's already
// showing, not replace it. Text opacity carries the "semi-transparent"
// look rather than the object's own bg (which stays fully transparent).
static void build_playback_icon_overlay(void) {
    s_playback_icon_overlay = lv_label_create(s_artwork_container);
    lv_obj_set_style_text_font(s_playback_icon_overlay, font_large_icon(), 0);
    lv_obj_set_style_text_color(s_playback_icon_overlay, lv_color_hex(0xffffff), 0);
    lv_obj_set_style_text_opa(s_playback_icon_overlay, LV_OPA_50, 0);
    lv_obj_set_style_bg_opa(s_playback_icon_overlay, LV_OPA_TRANSP, 0);
    // Reverted: doubling this via lv_obj_set_style_transform_scale_x/y
    // crashed WiFi on hardware - LVGL 9's transform rendering composites a
    // scaled widget through an intermediate layer buffer sized to the
    // scaled bounding box (roughly 280x280 here), and that allocation
    // competes with the same DMA-capable heap region WiFi/BLE need (see
    // the LVGL_BUF_HEIGHT comment earlier in this codebase for the same
    // memory pressure showing up elsewhere). A real size increase needs a
    // bigger font asset (flash, not runtime heap) instead - see
    // idf_app/scripts/generate_fonts.sh's material_icons_140 block.
    lv_obj_center(s_playback_icon_overlay);
    lv_obj_remove_flag(s_playback_icon_overlay, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(s_playback_icon_overlay, LV_OBJ_FLAG_HIDDEN);
}

// TV/Vinyl screen (ADR Screen 2) - sibling of s_ui_container, built after
// it so it draws on top (irrelevant while hidden, matters the instant
// apply_current_screen() shows it). No progress ring, no transport
// controls, no track/artist text, no input label (owner direction: "let
// the photos do the talking") - just the background, the volume readout,
// and the same two long-press gestures at different proportions (top
// two-thirds/bottom-third instead of Music's thirds, since there's no
// middle-third content to protect from an accidental long-press here).
static void build_tv_vinyl_layout(void) {
    s_tv_vinyl_container = lv_obj_create(s_artwork_container);
    lv_obj_set_size(s_tv_vinyl_container, SCREEN_SIZE, SCREEN_SIZE);
    lv_obj_center(s_tv_vinyl_container);
    lv_obj_set_style_bg_opa(s_tv_vinyl_container, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(s_tv_vinyl_container, 0, 0);
    lv_obj_set_style_pad_all(s_tv_vinyl_container, 0, 0);
    lv_obj_add_flag(s_tv_vinyl_container, LV_OBJ_FLAG_HIDDEN);

    // Gesture regions first, same reasoning as Music's: everything created
    // after sits above them in z-order and keeps first claim on taps.
    lv_obj_t *mute_region = lv_obj_create(s_tv_vinyl_container);
    lv_obj_set_size(mute_region, SCREEN_SIZE, SCREEN_SIZE * 2 / 3);
    lv_obj_align(mute_region, LV_ALIGN_TOP_LEFT, 0, 0);
    lv_obj_set_style_bg_opa(mute_region, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(mute_region, 0, 0);
    lv_obj_set_style_pad_all(mute_region, 0, 0);
    lv_obj_add_flag(mute_region, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(mute_region, mute_region_long_press_cb,
                        LV_EVENT_LONG_PRESSED, NULL);

    lv_obj_t *source_region = lv_obj_create(s_tv_vinyl_container);
    lv_obj_set_size(source_region, SCREEN_SIZE, SCREEN_SIZE / 3);
    lv_obj_align(source_region, LV_ALIGN_BOTTOM_LEFT, 0, 0);
    lv_obj_set_style_bg_opa(source_region, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(source_region, 0, 0);
    lv_obj_set_style_pad_all(source_region, 0, 0);
    lv_obj_add_flag(source_region, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(source_region, source_region_long_press_cb,
                        LV_EVENT_LONG_PRESSED, NULL);

    // dB label + hero volume number, grouped in a content-sized flex
    // column so the *pair* is centered as one block (owner feedback: with
    // each positioned independently, the hero number alone was centered
    // and the dB label above it pushed the combined block off-center).
    lv_obj_t *volume_group = lv_obj_create(s_tv_vinyl_container);
    lv_obj_set_size(volume_group, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_opa(volume_group, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(volume_group, 0, 0);
    lv_obj_set_style_pad_all(volume_group, 0, 0);
    lv_obj_set_layout(volume_group, LV_LAYOUT_FLEX);
    lv_obj_set_flex_flow(volume_group, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(volume_group, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_remove_flag(volume_group, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_center(volume_group);

    // dB-equivalent, above the hero number - double font_small's size,
    // same relative "secondary" role as Music's s_volume_db_label.
    s_tv_vinyl_db_label = lv_label_create(volume_group);
    lv_obj_set_style_text_font(s_tv_vinyl_db_label, font_db_large(), 0);
    lv_obj_set_style_text_color(s_tv_vinyl_db_label, lv_color_hex(0xcccccc), 0);
    lv_label_set_text(s_tv_vinyl_db_label, "-- dB");

    // Hero volume number - double Music's xlarge size. No halo: the
    // background photo is dark by design (owner direction), unlike
    // variable-brightness album art, so the legibility problem the halo
    // solves for Music doesn't exist here.
    s_tv_vinyl_volume_label = lv_label_create(volume_group);
    lv_obj_set_style_text_font(s_tv_vinyl_volume_label, font_xxlarge(), 0);
    lv_obj_set_style_text_color(s_tv_vinyl_volume_label, lv_color_hex(0xfafafa), 0);
    lv_label_set_text(s_tv_vinyl_volume_label, "--");
}

// Full-screen mute state (owner direction: same slice as TV/Vinyl, shown
// regardless of which screen is active underneath - mute is a single
// global HA state, not per-input). Black background rather than a solid
// color fill - this panel is AMOLED (docs/esp/DISPLAY.md), where a
// large bright/saturated fill costs real, measurable extra current over
// a mostly-black one with the same information conveyed by a smaller lit
// area (the icon/text alone); unmissable comes from the icon+text being
// large and red against black, not from lighting every pixel on screen.
// Topmost object in s_artwork_container, so it covers whichever of
// Music/TV/Vinyl is currently showing without needing to know which.
static void build_mute_overlay(void) {
    s_mute_overlay = lv_obj_create(s_artwork_container);
    lv_obj_set_size(s_mute_overlay, SCREEN_SIZE, SCREEN_SIZE);
    lv_obj_center(s_mute_overlay);
    lv_obj_set_style_bg_color(s_mute_overlay, lv_color_hex(0x000000), 0);
    lv_obj_set_style_bg_opa(s_mute_overlay, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(s_mute_overlay, 0, 0);
    lv_obj_set_style_radius(s_mute_overlay, 0, 0);
    // The whole screen is one long-press target while muted - owner
    // feedback: no top/bottom split here like the screen underneath has,
    // since there's only one thing to do on this screen (unmute).
    // CLICKABLE (unlike s_lower_tint's pass-through) so it captures the
    // touch itself: the mute/source gesture regions on whichever of
    // Music/TV/Vinyl is showing underneath aren't reachable at all while
    // this is up. Unmuting just hides this overlay again - the correct
    // screen is already there underneath, unchanged, so "return to
    // whichever screen activated the mute" needs no extra logic.
    lv_obj_add_flag(s_mute_overlay, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(s_mute_overlay, mute_region_long_press_cb,
                        LV_EVENT_LONG_PRESSED, NULL);
    lv_obj_add_flag(s_mute_overlay, LV_OBJ_FLAG_HIDDEN);

#if !TARGET_PC
    lv_obj_t *icon = lv_label_create(s_mute_overlay);
    lv_label_set_text(icon, ICON_VOLUME_OFF);
    lv_obj_set_style_text_font(icon, font_large_icon(), 0);
    lv_obj_set_style_text_color(icon, lv_color_hex(0xff3333), 0);
    lv_obj_center(icon);  // True center - owner feedback: was offset high, and "MUTED" text (removed) isn't needed
#endif
}

// ============================================================================
// Event Handlers
// ============================================================================

// Long-press-by-region gestures (ADR: docs/meta/decisions/
// 2026-09-14_DESIGN_HYBRID_DIAL_UI.md). These regions are invisible,
// full-width bands created early (right after s_ui_container) so every
// other widget sits above them in z-order and keeps handling its own
// taps/long-presses exactly as before; these only ever see a long-press
// that lands on otherwise-empty background. Now that the Now Playing
// layout rework has removed the old header entirely, these regions are
// the only long-press handlers left covering the top/bottom thirds.
static void mute_region_long_press_cb(lv_event_t *e) {
    (void)e;
#if !TARGET_PC
    haptic_driver_pulse();
#endif
    controller_action_t action =
        controller_action_simple(CONTROLLER_ACTION_TOGGLE_MUTE);
    (void)controller_input_dispatch_action(&action);
}

static void source_region_long_press_cb(lv_event_t *e) {
    (void)e;
#if !TARGET_PC
    haptic_driver_pulse();
#endif
    controller_action_t action =
        controller_action_simple(CONTROLLER_ACTION_OPEN_ZONE_PICKER);
    (void)controller_input_dispatch_action(&action);
}

static void btn_prev_event_cb(lv_event_t *e) {
    (void)e;
    ESP_LOGI(UI_TAG, "btn_prev_event_cb triggered");
#if !TARGET_PC
    haptic_driver_pulse();
#endif
    ui_show_track_feedback(false);
    controller_action_t action = controller_action_command(
        controller_command_make(CONTROLLER_COMMAND_PREVIOUS_TRACK));
    (void)controller_input_dispatch_action(&action);
}

static void btn_play_event_cb(lv_event_t *e) {
    (void)e;
    ESP_LOGI(UI_TAG, "btn_play_event_cb triggered");
#if !TARGET_PC
    haptic_driver_pulse();
#endif
    // Shown optimistically (what this toggle is about to make it become)
    // rather than waiting for the next poll to confirm it - unlike the
    // button's own small icon, which only updates once state->playing
    // actually changes in apply_state(). A multi-second wait for a
    // confirmation icon would defeat the point of it.
    ui_show_playback_feedback(!ui_is_playing());
    controller_action_t action = controller_action_command(
        controller_command_make(CONTROLLER_COMMAND_TOGGLE_PLAYBACK));
    (void)controller_input_dispatch_action(&action);
}

static void btn_next_event_cb(lv_event_t *e) {
    (void)e;
    ESP_LOGI(UI_TAG, "btn_next_event_cb triggered");
#if !TARGET_PC
    haptic_driver_pulse();
#endif
    ui_show_track_feedback(true);
    controller_action_t action = controller_action_command(
        controller_command_make(CONTROLLER_COMMAND_NEXT_TRACK));
    (void)controller_input_dispatch_action(&action);
}

static void zone_list_item_event_cb(lv_event_t *e) {
    lv_obj_t *btn = lv_event_get_target(e);
    int index = (int)(intptr_t)lv_obj_get_user_data(btn);
    s_zone_picker_selected = index;
#if !TARGET_PC
    haptic_driver_pulse();
#endif
    controller_action_t action = controller_action_simple(
        CONTROLLER_ACTION_SELECT_ZONE_PICKER);
    (void)controller_input_dispatch_action(&action);
}

// ============================================================================
// State Management
// ============================================================================

static void apply_state(const struct ui_state *state) {
    // Update track/artist labels - but only if the text actually changed.
    // apply_state() runs on every dirty flush (as often as every 2s during
    // a poll, for volume/progress/playing changes that have nothing to do
    // with the track), while the track/artist themselves only change once
    // every few minutes. lv_label_set_text() doesn't check for this itself
    // (checked LVGL's own source: it unconditionally frees the label's
    // internal buffer, reallocates, copies the new text in, and
    // re-measures pixel width character-by-character to reconfigure the
    // scroll animation) - so without this guard, both labels were doing
    // that full round trip on every single poll regardless of whether
    // anything in them changed. (LVGL's circular-scroll code does already
    // preserve the animation's act_time across a same-text set_text call,
    // so this was wasted CPU/heap churn, not a visible scroll glitch.)
    static char s_last_line1[128] = "";
    static char s_last_line2[128] = "";
    if (s_track_label && s_artist_label) {
        if (strcmp(state->line1, s_last_line1) != 0) {
            lv_label_set_text(s_track_label, state->line1);
            lv_obj_invalidate(s_track_label);
            strncpy(s_last_line1, state->line1, sizeof(s_last_line1) - 1);
            s_last_line1[sizeof(s_last_line1) - 1] = '\0';
        }
        if (strcmp(state->line2, s_last_line2) != 0) {
            lv_label_set_text(s_artist_label, state->line2);
            lv_obj_invalidate(s_artist_label);
            strncpy(s_last_line2, state->line2, sizeof(s_last_line2) - 1);
            s_last_line2[sizeof(s_last_line2) - 1] = '\0';
        }
    } else {
        ESP_LOGE(UI_TAG, "Label pointers are NULL! track=%p artist=%p", s_track_label, s_artist_label);
    }

    // Detail screen's own title/artist/album labels - separate widgets from
    // the ones above (different position/size), same source data. Updated
    // unconditionally like the main labels above rather than gated on
    // s_detail_mode_visible - cheap text-set calls, and it means the screen
    // shows current info immediately on entry rather than stale text from
    // whenever it was last visible.
    if (s_detail_title_label) {
        lv_label_set_text(s_detail_title_label, state->line1);
    }
    if (s_detail_artist_label) {
        lv_label_set_text(s_detail_artist_label, state->line2);
    }
    strncpy(s_detail_album_raw, state->line3, sizeof(s_detail_album_raw) - 1);
    s_detail_album_raw[sizeof(s_detail_album_raw) - 1] = '\0';
    s_detail_album_year = state->album_year;
    apply_detail_album_label();

    // Enrichment region - hide-if-absent per field, independent of each
    // other and of title/artist/album above (see ui_set_bit_info()/
    // ui_set_next_track()'s own comments for why). Cheap enough to run
    // unconditionally every poll rather than change-guard like the
    // track/artist labels above - a handful of short label/flag calls, not
    // the 9-widget halo update volume/track text has to guard against.
    if (s_detail_bitinfo_label) {
        bool has_bit_info = state->bit_info[0] != '\0';
        lv_label_set_text(s_detail_bitinfo_label, has_bit_info ? state->bit_info : "");
        if (has_bit_info) {
            lv_obj_remove_flag(s_detail_bitinfo_label, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_add_flag(s_detail_bitinfo_label, LV_OBJ_FLAG_HIDDEN);
        }
    }
    // Three states: a known next track, a positively-known "Nothing"
    // (next_track_none - UHC only sends it with positive evidence, never
    // for merely-unknown), or unknown, which hides the whole group. A
    // title always wins over the none flag.
    bool has_next_track = state->next_line1[0] != '\0';
    bool show_next_none = !has_next_track && state->next_track_none;
    bool show_next_group = has_next_track || show_next_none;
    if (s_detail_next_label) {
        if (show_next_group) {
            lv_obj_remove_flag(s_detail_next_label, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_add_flag(s_detail_next_label, LV_OBJ_FLAG_HIDDEN);
        }
    }
    if (s_detail_next_title_label) {
        lv_label_set_text(s_detail_next_title_label,
                          has_next_track ? state->next_line1 : (show_next_none ? "Nothing" : ""));
        if (show_next_group) {
            lv_obj_remove_flag(s_detail_next_title_label, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_add_flag(s_detail_next_title_label, LV_OBJ_FLAG_HIDDEN);
        }
    }
    if (s_detail_next_artist_label) {
        bool has_next_artist = has_next_track && state->next_line2[0] != '\0';
        lv_label_set_text(s_detail_next_artist_label, has_next_artist ? state->next_line2 : "");
        if (has_next_artist) {
            lv_obj_remove_flag(s_detail_next_artist_label, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_add_flag(s_detail_next_artist_label, LV_OBJ_FLAG_HIDDEN);
        }
    }


    // Update volume arc and label, emphasize if volume changed
    // Volume is in dB with zone-specific min/max range
    static float last_volume = -9999.0f;  // Sentinel value (unlikely real volume)
    static bool volume_initialized = false;
    float vol_diff = state->volume < last_volume ? last_volume - state->volume : state->volume - last_volume;
    bool should_emphasize = false;
    if (volume_initialized && vol_diff > 0.01f) {
        // Only emphasize if value differs from last user prediction
        // (suppresses redundant emphasis when poll confirms user's change)
        float pred_diff = state->volume < s_last_predicted_volume ? s_last_predicted_volume - state->volume : state->volume - s_last_predicted_volume;
        if (pred_diff > 0.01f) {
            should_emphasize = true;
        }
    }
    volume_initialized = true;
    last_volume = state->volume;

    redraw_volume_ring(state->volume, state->volume_min, state->volume_max);

    // Display volume (format matches zone's step precision). set_haloed_label_text
    // touches 9 labels (the number plus its 8-directional halo) - guarded
    // the same way as track/artist above, since volume is unchanged on
    // most polls (only progress/playing changed) but this ran every time
    // regardless.
    static char s_last_vol_text[16] = "";
    char vol_text[16];
    // Note: volume_min is atomic float read; no lock needed (self-corrects on next poll if stale)
    format_volume_text(vol_text, sizeof(vol_text), state->volume, state->volume_min, state->volume_step);
    if (strcmp(vol_text, s_last_vol_text) != 0) {
        set_haloed_label_text(s_volume_label_large, s_volume_label_halo, vol_text);
        if (s_tv_vinyl_volume_label) lv_label_set_text(s_tv_vinyl_volume_label, vol_text);
        if (s_detail_volume_label) lv_label_set_text(s_detail_volume_label, vol_text);
        strncpy(s_last_vol_text, vol_text, sizeof(s_last_vol_text) - 1);
        s_last_vol_text[sizeof(s_last_vol_text) - 1] = '\0';
    }

    // Loud-volume warning color for the big numeric label(s) - see
    // volume_hot_color_for_db(). Set every poll (not gated on the text
    // change above) since it's a single cheap style call, and applied
    // before emphasize_volume_label() below so a real change's blue flash
    // still wins temporarily on the Music-screen label, settling back to
    // this color once it resets. The TV/Vinyl hero label has no such
    // flash, so it just goes straight to whichever color is current.
    s_volume_label_resting_db = derive_volume_db_equivalent(state->volume, state->volume_min, state->volume_max);
    lv_color_t volume_color = volume_hot_color_for_db(s_volume_label_resting_db, lv_color_hex(0xfafafa));
    if (s_volume_label_large) {
        lv_obj_set_style_text_color(s_volume_label_large, volume_color, 0);
    }
    if (s_volume_db_label) {
        lv_obj_set_style_text_color(s_volume_db_label, volume_color, 0);
    }
    if (s_tv_vinyl_volume_label) {
        lv_obj_set_style_text_color(s_tv_vinyl_volume_label, volume_color, 0);
    }
    if (should_emphasize) {
        emphasize_volume_label();
    }

    static char s_last_db_text[16] = "";
    char db_text[16];
    snprintf(db_text, sizeof(db_text), "%.1f dB",
             derive_volume_db_equivalent(state->volume, state->volume_min, state->volume_max));
    if (strcmp(db_text, s_last_db_text) != 0) {
        set_haloed_label_text(s_volume_db_label, s_volume_db_label_halo, db_text);
        if (s_tv_vinyl_db_label) lv_label_set_text(s_tv_vinyl_db_label, db_text);
        if (s_detail_db_label) lv_label_set_text(s_detail_db_label, db_text);
        strncpy(s_last_db_text, db_text, sizeof(s_last_db_text) - 1);
        s_last_db_text[sizeof(s_last_db_text) - 1] = '\0';
    }

    // Update progress arc based on seek position and track length.
    // lv_arc_set_value() already no-ops (checked LVGL's own source: it
    // returns immediately if the value is unchanged, before touching
    // anything else) - the lv_obj_invalidate() calls that used to follow
    // it unconditionally were forcing a redraw LVGL had already decided
    // was unnecessary, every single poll. Also record this as the new
    // interpolation baseline - progress_interp_timer_cb advances a local
    // estimate between polls (see its own comment for why), so every fresh
    // poll needs to reset that baseline or it'd keep extrapolating from
    // stale data.
    //
    // state->seek_position/state->length are whole SECONDS, not
    // milliseconds - confirmed against the bridge's own source
    // (unified-hifi-control passes Roon's seek_position/duration through
    // unconverted; a 645-second test fixture there is a ~10:45 track, not
    // 645ms). The "_ms" naming on ui_set_progress()'s parameters is a
    // misleading holdover. Converted to milliseconds here, once, so the
    // interpolation timer below can add real elapsed device time to it
    // directly - getting this wrong made the arc peg at 100% within a
    // fraction of a second of every poll (real elapsed ms is ~1000x the
    // actual per-tick progress in seconds), holding there until the next
    // poll corrected it and the cycle repeated.
    //
    // Skipped entirely while a seek preview is active: a poll landing
    // mid-scrub still reports the pre-seek position (the bridge doesn't
    // know about the pending seek yet), and applying it here would fight
    // the preview the user is actively watching. ui_seek_adjust re-syncs
    // s_progress_base_ms itself once the seek commits, so the next poll
    // after that lands on solid ground again.
    if (s_progress_arc && state->length > 0 && !s_seek_active) {
        int reported_ms = state->seek_position * 1000;
        uint64_t now = platform_millis();

        // A first attempt rejected small (<=2s) backward corrections,
        // guessing that Roon's own integer-second truncation plus request
        // latency wouldn't exceed that. Wrong in practice - on hardware
        // the arc still snapped backward at every single poll, meaning
        // the real lag isn't reliably "small". Rather than chase a bigger
        // magic number, don't compare a gap size at all: while the same
        // track keeps playing, the displayed position can only move
        // forward, full stop. Take whichever of "what this poll reports"
        // and "where local interpolation already estimates we are" is
        // further along - a real seek/skip is caught separately (below,
        // via the track-identity/pause checks) and still takes effect
        // immediately.
        bool same_track = strcmp(state->line1, s_progress_last_line1) == 0;
        strncpy(s_progress_last_line1, state->line1, sizeof(s_progress_last_line1) - 1);
        s_progress_last_line1[sizeof(s_progress_last_line1) - 1] = '\0';

        int effective_ms = reported_ms;
        bool reported_went_backward =
            same_track && s_progress_last_reported_ms >= 0 &&
            reported_ms + EXTERNAL_BACKWARD_SEEK_MIN_MS <= s_progress_last_reported_ms;
        s_progress_last_reported_ms = reported_ms;
        bool in_seek_resync_grace = same_track && s_seek_commit_uptime_ms != 0 &&
                                     (now - s_seek_commit_uptime_ms) < SEEK_RESYNC_GRACE_MS;
        if (in_seek_resync_grace && s_progress_base_ms >= 0) {
            // A poll landing in this window may have been dispatched before
            // the seek reached Roon and still be reporting the old
            // position - trust our own just-committed value rather than
            // comparing against it (see seek_commit()'s comment). Only
            // advance it if actually playing, same as the interpolation
            // timer, so a seek made while paused doesn't drift forward.
            effective_ms = s_progress_base_ms;
            if (state->playing) {
                effective_ms += (int)(now - s_progress_base_uptime_ms);
            }
        } else if (same_track && s_progress_is_playing && state->playing && s_progress_base_ms >= 0 &&
                   !reported_went_backward) {
            // (A genuine external backward move - e.g. Harmony's "previous"
            // restarting the same track - skips this and takes the reported
            // position as-is; without it the forward-only rule below kept
            // the dial at the pre-restart position.)
            int estimated_now_ms = s_progress_base_ms + (int)(now - s_progress_base_uptime_ms);
            if (estimated_now_ms > effective_ms) {
                effective_ms = estimated_now_ms;
            }
        }

        int progress_scaled = (effective_ms * PROGRESS_ARC_MAX) / (state->length * 1000);
        if (progress_scaled > PROGRESS_ARC_MAX) progress_scaled = PROGRESS_ARC_MAX;
        if (progress_scaled < 0) progress_scaled = 0;
        lv_arc_set_value(s_progress_arc, progress_scaled);
        s_progress_base_ms = effective_ms;
        s_progress_base_uptime_ms = now;
        s_progress_length_ms = state->length * 1000;
        s_progress_is_playing = state->playing;
        if (s_detail_progress_label) {
            char elapsed_text[16];
            char total_text[16];
            format_mmss(elapsed_text, sizeof(elapsed_text), s_progress_base_ms);
            format_mmss(total_text, sizeof(total_text), s_progress_length_ms);
            char combined[40];
            snprintf(combined, sizeof(combined), "%s / %s", elapsed_text, total_text);
            lv_label_set_text(s_detail_progress_label, combined);
        }
    } else if (s_progress_arc && !s_seek_active) {
        lv_arc_set_value(s_progress_arc, 0);
        s_progress_base_ms = -1;  // No track/length - nothing to interpolate
        s_progress_last_reported_ms = -1;
        s_progress_last_line1[0] = '\0';  // Force "different track" on whatever plays next
        if (s_detail_progress_label) {
            lv_label_set_text(s_detail_progress_label, "");
        }
    }

    // Update play/pause icon - only on an actual state change.
    static int s_last_playing = -1;  // -1 = never applied
    if (s_play_icon && (int)state->playing != s_last_playing) {
        s_last_playing = state->playing ? 1 : 0;
#if !TARGET_PC
        lv_label_set_text(s_play_icon, state->playing ? ICON_PAUSE : ICON_PLAY);
#else
        lv_label_set_text(s_play_icon, state->playing ? LV_SYMBOL_PAUSE : LV_SYMBOL_PLAY);
#endif
    }

    // Update online status
    set_status_dot(state->online);

    // Update battery
    update_battery_display();
}

static void set_status_dot(bool online) {
    if (online) {
        lv_obj_set_style_bg_color(s_status_dot, lv_color_hex(0x00ff00), 0);  // Green
    } else {
        lv_obj_set_style_bg_color(s_status_dot, COLOR_GREY, 0);
    }
}

// Switches between the Music/TV/Vinyl screens based on the polled
// input_select.audio_input value (ha_volume_client's existing poll cycle -
// see its header comment on why this lives there rather than a second
// task). Cheap to call every poll_pending() tick: just a cached-value
// read plus, only on an actual change, a handful of show/hide calls.
//
// Doesn't touch s_artwork_image's own HIDDEN flag - ui_set_artwork() owns
// that independently (based on whether artwork is currently loaded, not
// which screen is active), and s_tv_vinyl_bg already sits above it in
// z-order and is fully opaque, so showing s_tv_vinyl_bg is enough to
// visually cover the artwork without the two pieces of code fighting
// over the same flag.
static void apply_current_screen(void) {
    char source[32];
    dial_screen_t new_screen = s_current_screen;
    if (ha_volume_client_get_current_source(source, sizeof(source))) {
        if (strcmp(source, "TV") == 0) {
            new_screen = DIAL_SCREEN_TV;
        } else if (strcmp(source, "Vinyl") == 0) {
            new_screen = DIAL_SCREEN_VINYL;
        } else {
            new_screen = DIAL_SCREEN_MUSIC;
        }
    }
    if (new_screen == s_current_screen) {
        return;
    }
    s_current_screen = new_screen;
    bool music = (new_screen == DIAL_SCREEN_MUSIC);

    if (s_ui_container) {
        if (music) {
            lv_obj_remove_flag(s_ui_container, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_add_flag(s_ui_container, LV_OBJ_FLAG_HIDDEN);
        }
    }
    if (s_tv_vinyl_container) {
        if (music) {
            lv_obj_add_flag(s_tv_vinyl_container, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_remove_flag(s_tv_vinyl_container, LV_OBJ_FLAG_HIDDEN);
        }
    }
    if (s_tv_vinyl_bg) {
        if (music) {
            lv_obj_add_flag(s_tv_vinyl_bg, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_remove_flag(s_tv_vinyl_bg, LV_OBJ_FLAG_HIDDEN);
#if !TARGET_PC
            lv_image_set_src(s_tv_vinyl_bg, new_screen == DIAL_SCREEN_TV
                                                 ? &tv_background
                                                 : &vinyl_background);
#endif
        }
    }
    // The progress arc moved out of s_ui_container (see its own creation
    // comment) so it - like the volume ring - can stay visible across
    // detail/mute overlays regardless of which underlying screen is
    // active. That also meant apply_current_screen()'s hide/show of
    // s_ui_container stopped affecting it, so it needs its own explicit
    // toggle here: unlike the volume ring, TV/Vinyl have no track/timeline
    // concept for it to represent - owner feedback that it had reappeared
    // there.
    if (s_progress_arc) {
        if (music) {
            lv_obj_remove_flag(s_progress_arc, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_add_flag(s_progress_arc, LV_OBJ_FLAG_HIDDEN);
        }
    }
}

bool ui_is_music_screen(void) {
    return s_current_screen == DIAL_SCREEN_MUSIC;
}

// Shows/hides the full-screen mute state based on the polled
// input_boolean.audio_mute value (ha_mute_client.c's optimistic update
// makes this feel instant on this dial's own toggle; the poll reconciles
// it for a mute/unmute from anywhere else within one interval).
static void apply_mute_overlay(void) {
    bool muted = ha_volume_client_get_muted();
    if (muted == s_mute_overlay_visible || !s_mute_overlay) {
        return;
    }
    s_mute_overlay_visible = muted;
    if (muted) {
        // Explicit foreground move, not just relying on creation order -
        // ui_set_detail_mode() promotes the volume ring/progress arc above
        // whatever they were originally stacked under so they stay visible
        // on the detail screen, and that promotion persists afterward (it's
        // their normal resting z-order, not something detail-mode-only).
        // Without reasserting mute's own position here, muting after ever
        // having shown the detail screen would leave those rings visible
        // on top of what's supposed to be an "everything else hidden" mute
        // screen.
        lv_obj_move_foreground(s_mute_overlay);
        lv_obj_remove_flag(s_mute_overlay, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(s_mute_overlay, LV_OBJ_FLAG_HIDDEN);
    }
}

static void poll_pending(lv_timer_t *timer) {
    (void)timer;

    os_mutex_lock(&s_state_lock);
    bool dirty = s_dirty;
    struct ui_state local_state = s_pending;
    s_dirty = false;

    bool show_message = s_message_dirty;
    char message[128];
    if (show_message) {
        strncpy(message, s_pending_message, sizeof(message) - 1);
        message[sizeof(message) - 1] = '\0';
        s_message_dirty = false;
    }

    // Zone name is still tracked in s_pending (ui_set_zone_name remains a
    // public API used by controller_presentation_dial.c and the WiFi-setup
    // path in main_idf.c), but the Now Playing screen no longer renders it
    // per the ADR, so there is nothing left here to apply to a widget.
    s_zone_name_dirty = false;

    bool network_status_changed = s_network_status_dirty;
    char net_status[128];
    if (network_status_changed) {
        strncpy(net_status, s_network_status, sizeof(net_status) - 1);
        net_status[sizeof(net_status) - 1] = '\0';
        s_network_status_dirty = false;
    }
    os_mutex_unlock(&s_state_lock);

    if (dirty) {
        apply_state(&local_state);
    }
    if (show_message) {
        show_status_message(message);
    }
    if (network_status_changed && s_status_bar) {
        // Set network status directly without auto-clear timer
        lv_label_set_text(s_status_bar, net_status);
        // Show/hide background based on content
        lv_obj_set_style_bg_opa(s_status_bar, net_status[0] ? LV_OPA_COVER : LV_OPA_TRANSP, 0);
        // Cancel any pending auto-clear from show_status_message
        if (s_status_timer) {
            lv_timer_del(s_status_timer);
            s_status_timer = NULL;
        }
    }

    apply_current_screen();
    apply_mute_overlay();
}

static int s_last_battery_level = -1;  // 0-3 levels for hysteresis
static bool s_last_battery_charging = false;  // Track charging state changes
static lv_timer_t *s_battery_flash_timer = NULL;  // Only exists while flashing (<=5%, not charging)
static bool s_battery_flash_on = true;  // Current phase of the flash

static void update_battery_display(void) {
#ifdef ESP_PLATFORM
    if (!s_battery_icon) return;

    int percent = battery_get_percentage();
    bool charging = battery_is_charging();

    // Convert to 4 discrete levels for stability (precision matches fidelity)
    // Critical: ≤10%, Low: 11-25%, Medium: 26-60%, High: ≥61%
    int level;
    if (percent <= 10) level = 0;       // Critical
    else if (percent <= 25) level = 1;  // Low
    else if (percent <= 60) level = 2;  // Medium
    else level = 3;                     // High

    // Only update the icon glyph if level or charging state changed
    // (prevents flicker from redundant lv_label_set_text calls) - but
    // color/flashing below still need the exact percent, not just this
    // coarse level, since both thresholds (<=10%, <=5%) fall inside the
    // same "Critical" bucket and would otherwise never be re-evaluated
    // while percent drifts from 10% down to 5% without level changing.
    if (level != s_last_battery_level || charging != s_last_battery_charging) {
        s_last_battery_level = level;
        s_last_battery_charging = charging;

        // Update battery icon based on state (Lucide horizontal icons) -
        // mirrored onto the detail screen's own icon (s_detail_battery_icon),
        // same glyph selection, if that screen's been built.
        lv_obj_clear_flag(s_battery_icon, LV_OBJ_FLAG_HIDDEN);
        const char *glyph;
        if (charging) {
            glyph = ICON_BATTERY_CHARGING;
        } else {
            switch (level) {
                case 0:  glyph = ICON_BATTERY_WARNING; break;  // Critical
                case 1:  glyph = ICON_BATTERY_LOW; break;      // Low
                case 2:  glyph = ICON_BATTERY_MEDIUM; break;   // Medium
                default: glyph = ICON_BATTERY_FULL; break;     // High
            }
        }
        lv_label_set_text(s_battery_icon, glyph);
        if (s_detail_battery_icon) {
            lv_label_set_text(s_detail_battery_icon, glyph);
        }
    }

    // 4-stage color progression, matching the level thresholds above:
    // near-white (fine) -> amber at 25% or less (Low) -> red at 10% or less
    // (Critical) -> flashing red at 5% or less (below, unchanged). Near-white
    // instead of grey (owner feedback) so the "fine" state reads clearly
    // against the dark badge behind it, same as the amber/red states do.
    lv_color_t battery_color;
    if (percent <= 10 && !charging) {
        battery_color = lv_color_hex(0xff0000);  // Red
    } else if (percent <= 25 && !charging) {
        battery_color = lv_color_hex(0xffaa00);  // Amber
    } else {
        battery_color = lv_color_hex(0xfafafa);  // Near-white
    }
    lv_obj_set_style_text_color(s_battery_icon, battery_color, 0);
    if (s_detail_battery_icon) {
        lv_obj_set_style_text_color(s_detail_battery_icon, battery_color, 0);
    }

    // Flash (blink) at 5% or less, not charging - a timer only exists
    // while this condition holds, started/stopped here rather than left
    // running (and just skipped) the rest of the time so it isn't
    // silently ticking for the ~99% of battery life it never applies to.
    // battery_flash_timer_cb toggles both icons' opacity together.
    bool should_flash = percent <= 5 && !charging;
    if (should_flash && !s_battery_flash_timer) {
        s_battery_flash_on = true;
        lv_obj_set_style_text_opa(s_battery_icon, LV_OPA_COVER, 0);
        if (s_detail_battery_icon) lv_obj_set_style_text_opa(s_detail_battery_icon, LV_OPA_COVER, 0);
        s_battery_flash_timer = lv_timer_create(battery_flash_timer_cb, 500, NULL);
    } else if (!should_flash && s_battery_flash_timer) {
        lv_timer_del(s_battery_flash_timer);
        s_battery_flash_timer = NULL;
        s_battery_flash_on = true;
        lv_obj_set_style_text_opa(s_battery_icon, LV_OPA_COVER, 0);  // Leave it visible, not mid-blink-off
        if (s_detail_battery_icon) lv_obj_set_style_text_opa(s_detail_battery_icon, LV_OPA_COVER, 0);
    }

    // Actual percentage, small font below the detail screen's icon (owner
    // request) - only this screen shows the number; Music's badge stays
    // icon-only, unchanged. Gated on the percent actually changing, same
    // flicker-avoidance reasoning as the icon glyph above, but keyed on the
    // raw percent (not the coarser 4-level bucket) since the number is
    // exact.
    static int s_last_battery_percent_shown = -1;
    if (s_detail_battery_pct_label && percent != s_last_battery_percent_shown) {
        s_last_battery_percent_shown = percent;
        char pct_text[8];
        snprintf(pct_text, sizeof(pct_text), "%d%%", percent);
        lv_label_set_text(s_detail_battery_pct_label, pct_text);
    }
#endif
}

static void battery_flash_timer_cb(lv_timer_t *timer) {
    (void)timer;
    if (!s_battery_icon) return;
    s_battery_flash_on = !s_battery_flash_on;
    lv_opa_t opa = s_battery_flash_on ? LV_OPA_COVER : LV_OPA_TRANSP;
    lv_obj_set_style_text_opa(s_battery_icon, opa, 0);
    if (s_detail_battery_icon) {
        lv_obj_set_style_text_opa(s_detail_battery_icon, opa, 0);
    }
}

static void battery_poll_timer_cb(lv_timer_t *timer) {
    (void)timer;
    update_battery_display();
}

// Public API for immediate battery refresh (called on USB connect/disconnect)
void ui_update_battery(void) {
    update_battery_display();
}

// ============================================================================
// Status Bar
// ============================================================================

static void show_status_message(const char *message) {
    if (!s_status_bar) {
        ESP_LOGW(UI_TAG, "Status bar not initialized!");
        return;
    }

    lv_label_set_text(s_status_bar, message);
    // Show background when message is visible
    lv_obj_set_style_bg_opa(s_status_bar, LV_OPA_COVER, 0);

    // Auto-clear after 3 seconds
    if (s_status_timer) {
        lv_timer_reset(s_status_timer);
    } else {
        s_status_timer = lv_timer_create(clear_status_message_timer_cb, 3000, NULL);
        lv_timer_set_repeat_count(s_status_timer, 1);
    }
}

static void clear_status_message_timer_cb(lv_timer_t *timer) {
    (void)timer;
    if (s_status_bar) {
        lv_label_set_text(s_status_bar, "");
        // Hide background when empty
        lv_obj_set_style_bg_opa(s_status_bar, LV_OPA_TRANSP, 0);
    }
    s_status_timer = NULL;
}

// ============================================================================
// Volume Emphasis - Highlights volume display when adjusting
// ============================================================================

static void reset_volume_emphasis_timer_cb(lv_timer_t *timer) {
    (void)timer;
    if (s_volume_label_large) {
        // Reset to whichever color the current volume actually calls for
        // (white/amber/red - see volume_hot_color_for_db), not always
        // white, so the "just changed" blue flash settles back into the
        // loud-volume warning color instead of masking it for 1.5s.
        lv_obj_set_style_text_color(
            s_volume_label_large,
            volume_hot_color_for_db(s_volume_label_resting_db, lv_color_hex(0xfafafa)), 0);
    }
    s_volume_emphasis_timer = NULL;
}

static void emphasize_volume_label(void) {
    if (!s_volume_label_large) {
        return;
    }

    // At loud volumes (amber/red territory), the warning color itself is
    // the signal - a blue "just changed" flash on top of it, however
    // briefly, reads as "back to normal" and undersells how loud this is
    // (owner feedback). Skip the flash there entirely; the caller already
    // set the correct resting color on this label before calling here, so
    // there's nothing to do. A timer already running from before the
    // volume entered this zone is left alone - it re-reads
    // s_volume_label_resting_db fresh when it fires, so it still resolves
    // to the current color rather than a stale one.
    if (s_volume_label_resting_db >= VOLUME_HOT_DB_AMBER) {
        return;
    }

    // Emphasize with bright blue
    lv_obj_set_style_text_color(s_volume_label_large, lv_color_hex(0x7bb9e8), 0);

    // Reset/create timer to remove emphasis after 1.5 seconds
    if (s_volume_emphasis_timer) {
        lv_timer_reset(s_volume_emphasis_timer);
    } else {
        s_volume_emphasis_timer = lv_timer_create(reset_volume_emphasis_timer_cb, 1500, NULL);
        lv_timer_set_repeat_count(s_volume_emphasis_timer, 1);
    }
}

static void playback_icon_timer_cb(lv_timer_t *timer) {
    (void)timer;
    if (s_playback_icon_overlay) {
        lv_obj_add_flag(s_playback_icon_overlay, LV_OBJ_FLAG_HIDDEN);
    }
    s_playback_icon_timer = NULL;  // One-shot - LVGL already freed it
}

// Shared by ui_show_playback_feedback/ui_show_track_feedback below - shows
// icon_text on the overlay, brings it to the front (creation order alone
// isn't enough since this needs to show above either the main screen or
// the detail overlay, whichever is currently up), and (re)arms the 3s
// auto-hide timer.
static void show_icon_feedback(const char *icon_text) {
    if (!s_playback_icon_overlay) {
        return;
    }
    lv_label_set_text(s_playback_icon_overlay, icon_text);
    lv_obj_move_foreground(s_playback_icon_overlay);
    lv_obj_remove_flag(s_playback_icon_overlay, LV_OBJ_FLAG_HIDDEN);
    if (s_playback_icon_timer) {
        lv_timer_reset(s_playback_icon_timer);
    } else {
        s_playback_icon_timer = lv_timer_create(playback_icon_timer_cb, 3000, NULL);
        if (s_playback_icon_timer) {
            lv_timer_set_repeat_count(s_playback_icon_timer, 1);
        }
    }
}

// Shown briefly as visual confirmation whenever playback is toggled -
// from the main screen's transport button, or a swipe gesture on the
// detail screen. now_playing is what the action is COMMANDING playback
// to become (shown immediately/optimistically), not necessarily the
// latest polled state.
void ui_show_playback_feedback(bool now_playing) {
#if !TARGET_PC
    show_icon_feedback(now_playing ? ICON_PLAY : ICON_PAUSE);
#else
    show_icon_feedback(now_playing ? LV_SYMBOL_PLAY : LV_SYMBOL_PAUSE);
#endif
}

// Same overlay, skip-next/skip-previous glyph instead - shown whenever a
// next/previous track action fires (transport buttons or a swipe, on any
// screen that supports the gesture).
void ui_show_track_feedback(bool next) {
#if !TARGET_PC
    show_icon_feedback(next ? ICON_SKIP_NEXT : ICON_SKIP_PREV);
#else
    show_icon_feedback(next ? LV_SYMBOL_NEXT : LV_SYMBOL_PREV);
#endif
}

// ============================================================================
// Zone Picker - LVGL List Widget (supports per-item icons)
// ============================================================================

// Special zone IDs (must match bridge_client.c)
#define ZONE_ID_BACK "__back__"
#define ZONE_ID_SETTINGS "__settings__"

void ui_show_zone_picker(const char **zone_names, const char **zone_ids, int count, int selected) {
    if (s_zone_picker_visible) {
        return;
    }

    // Store zone IDs for later retrieval
    s_zone_picker_count = (count > MAX_ZONE_PICKER_ZONES) ? MAX_ZONE_PICKER_ZONES : count;
    s_zone_picker_selected = selected;
    s_zone_picker_current = selected;  // Remember current zone for no-op detection
    for (int i = 0; i < s_zone_picker_count; i++) {
        strncpy(s_zone_picker_ids[i], zone_ids[i], MAX_ZONE_ID_LEN - 1);
        s_zone_picker_ids[i][MAX_ZONE_ID_LEN - 1] = '\0';
    }

    // Create fullscreen dark overlay
    s_zone_picker_overlay = lv_obj_create(lv_screen_active());
    lv_obj_set_size(s_zone_picker_overlay, SCREEN_SIZE, SCREEN_SIZE);
    lv_obj_center(s_zone_picker_overlay);
    lv_obj_set_style_bg_color(s_zone_picker_overlay, lv_color_hex(0x000000), 0);
    lv_obj_set_style_bg_opa(s_zone_picker_overlay, LV_OPA_90, 0);
    lv_obj_set_style_border_width(s_zone_picker_overlay, 0, 0);
    lv_obj_set_style_radius(s_zone_picker_overlay, 0, 0);
    lv_obj_set_style_pad_all(s_zone_picker_overlay, 0, 0);

    // Title at top
    lv_obj_t *title = lv_label_create(s_zone_picker_overlay);
    lv_label_set_text(title, "INPUT SOURCE");
    lv_obj_set_style_text_font(title, font_normal(), 0);
    lv_obj_set_style_text_color(title, lv_color_hex(0xfafafa), 0);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 30);

    // Create list widget - allows per-item icons
    s_zone_list = lv_list_create(s_zone_picker_overlay);
    lv_obj_set_size(s_zone_list, SCREEN_SIZE - 60, SCREEN_SIZE - 120);
    lv_obj_align(s_zone_list, LV_ALIGN_CENTER, 0, 10);
    lv_obj_set_style_bg_color(s_zone_list, lv_color_hex(0x0a0a0a), 0);
    lv_obj_set_style_bg_opa(s_zone_list, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(s_zone_list, 0, 0);
    lv_obj_set_style_pad_all(s_zone_list, 0, 0);
    lv_obj_set_style_radius(s_zone_list, 10, 0);

    // Add items to list with appropriate icons
    for (int i = 0; i < s_zone_picker_count; i++) {
        lv_obj_t *btn = lv_list_add_btn(s_zone_list, NULL, NULL);
        lv_obj_set_style_bg_color(btn, lv_color_hex(0x1a1a1a), 0);
        lv_obj_set_style_bg_color(btn, lv_color_hex(0x3a3a3a), LV_STATE_PRESSED);
        lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, 0);
        lv_obj_set_style_pad_ver(btn, 18, 0);   // Larger vertical padding for easier tapping
        lv_obj_set_style_pad_hor(btn, 16, 0);
        lv_obj_set_height(btn, LV_SIZE_CONTENT);
        lv_obj_set_style_min_height(btn, 56, 0);  // Minimum 56px tap target (Material Design guideline)

        // Use flex layout for proper vertical alignment of icon and text
        lv_obj_set_flex_flow(btn, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(btn, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_set_style_pad_column(btn, 12, 0);  // Gap between icon and text

        // Highlight current selection
        if (i == selected) {
            lv_obj_set_style_bg_color(btn, lv_color_hex(0x2a4a6a), 0);
        }

        // Create icon label (using icon font)
        lv_obj_t *icon_label = lv_label_create(btn);
#if !TARGET_PC
        lv_obj_set_style_text_font(icon_label, font_icon_small(), 0);
        // Icon font glyphs sit high in their bounding box - add top padding to center visually
        lv_obj_set_style_pad_top(icon_label, 4, 0);
#else
        lv_obj_set_style_text_font(icon_label, font_normal(), 0);
#endif
        lv_obj_set_style_text_color(icon_label, lv_color_hex(0xaaaaaa), 0);

        // Set icon based on zone type
        const char *zone_id = zone_ids[i];
        if (strcmp(zone_id, ZONE_ID_BACK) == 0) {
#if !TARGET_PC
            lv_label_set_text(icon_label, ICON_ARROW_BACK);
#else
            lv_label_set_text(icon_label, LV_SYMBOL_LEFT);
#endif
        } else if (strcmp(zone_id, ZONE_ID_SETTINGS) == 0) {
#if !TARGET_PC
            lv_label_set_text(icon_label, ICON_SETTINGS);
#else
            lv_label_set_text(icon_label, LV_SYMBOL_SETTINGS);
#endif
        } else if (strcmp(zone_id, "TV") == 0) {
#if !TARGET_PC
            lv_label_set_text(icon_label, ICON_TV);
#else
            lv_label_set_text(icon_label, LV_SYMBOL_AUDIO);
#endif
        } else if (strcmp(zone_id, "Vinyl") == 0) {
#if !TARGET_PC
            lv_label_set_text(icon_label, ICON_ALBUM);
#else
            lv_label_set_text(icon_label, LV_SYMBOL_AUDIO);
#endif
        } else {
            // Music (the only other non-sentinel entry the source
            // picker ever sends here) - music note icon.
#if !TARGET_PC
            lv_label_set_text(icon_label, ICON_MUSIC_NOTE);
#else
            lv_label_set_text(icon_label, LV_SYMBOL_AUDIO);
#endif
        }

        // Create text label (using text font)
        lv_obj_t *text_label = lv_label_create(btn);
        lv_obj_set_style_text_font(text_label, font_normal(), 0);
        lv_obj_set_style_text_color(text_label, lv_color_hex(0xfafafa), 0);
        lv_label_set_text(text_label, zone_names[i]);

        // Store index in button user data
        lv_obj_set_user_data(btn, (void *)(intptr_t)i);
        lv_obj_add_event_cb(btn, zone_list_item_event_cb, LV_EVENT_CLICKED, NULL);
    }

    // Scroll to selected item
    if (selected > 0 && selected < s_zone_picker_count) {
        lv_obj_t *selected_btn = lv_obj_get_child(s_zone_list, selected);
        if (selected_btn) {
            lv_obj_scroll_to_view(selected_btn, LV_ANIM_OFF);
        }
    }

    // Hint text at bottom
    lv_obj_t *hint = lv_label_create(s_zone_picker_overlay);
    lv_label_set_text(hint, "Tap to select");
    lv_obj_set_style_text_font(hint, font_small(), 0);
    lv_obj_set_style_text_color(hint, lv_color_hex(0x888888), 0);
    lv_obj_set_style_text_align(hint, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(hint, LV_ALIGN_BOTTOM_MID, 0, -20);

    s_zone_picker_visible = true;
}

void ui_hide_zone_picker(void) {
    if (!s_zone_picker_visible) {
        return;
    }

    if (s_zone_picker_overlay) {
        lv_obj_delete(s_zone_picker_overlay);
        s_zone_picker_overlay = NULL;
        s_zone_list = NULL;
    }
    s_zone_picker_visible = false;
}

int ui_get_zone_picker_selected(void) {
    if (!s_zone_picker_visible) {
        return -1;
    }
    return s_zone_picker_selected;
}

void ui_zone_picker_get_selected_id(char *out, size_t len) {
    if (!out || len == 0) {
        return;
    }
    out[0] = '\0';
    if (!s_zone_picker_visible) {
        return;
    }
    int selected = s_zone_picker_selected;
    if (selected >= 0 && selected < s_zone_picker_count) {
        strncpy(out, s_zone_picker_ids[selected], len - 1);
        out[len - 1] = '\0';
    }
}

bool ui_zone_picker_is_current_selection(void) {
    // Returns true if user selected the same zone they started with (no-op)
    return s_zone_picker_selected == s_zone_picker_current;
}

// ============================================================================
// Public API
// ============================================================================

void ui_loop_iter(void) {
    lv_task_handler();
    lv_timer_handler();

    platform_task_run_pending();  // Process callbacks from bridge_client thread

    // Check for pending UI updates (poll_pending inline - no timer needed)
    poll_pending(NULL);
}

void ui_set_track(const char *line1, const char *line2) {
    os_mutex_lock(&s_state_lock);
    strncpy(s_pending.line1, line1, sizeof(s_pending.line1) - 1);
    strncpy(s_pending.line2, line2, sizeof(s_pending.line2) - 1);
    s_pending.line1[sizeof(s_pending.line1) - 1] = '\0';
    s_pending.line2[sizeof(s_pending.line2) - 1] = '\0';
    // Strip streaming-service title noise ("(Remastered 2011)", "(Album
    // Version)", etc.) - see track_title_filter.c. Track title only
    // (line1); artist/album (line2) is left untouched.
    track_title_filter_apply(s_pending.line1, sizeof(s_pending.line1));
    s_dirty = true;
    os_mutex_unlock(&s_state_lock);
}

void ui_set_album(const char *album) {
    os_mutex_lock(&s_state_lock);
    if (album) {
        strncpy(s_pending.line3, album, sizeof(s_pending.line3) - 1);
        s_pending.line3[sizeof(s_pending.line3) - 1] = '\0';
        // Same streaming-service noise ("(Remastered 2011)", "[24-bit/96kHz]",
        // etc.) shows up in album names as often as track titles - owner
        // request to strip it here too, using the same pattern list
        // (ui_set_track applies it to line1 the same way).
        track_title_filter_apply(s_pending.line3, sizeof(s_pending.line3));
    } else {
        s_pending.line3[0] = '\0';
    }
    s_dirty = true;
    os_mutex_unlock(&s_state_lock);
}

void ui_set_album_year(int year) {
    os_mutex_lock(&s_state_lock);
    s_pending.album_year = year;
    s_dirty = true;
    os_mutex_unlock(&s_state_lock);
}

void ui_set_bit_info(const char *text) {
    os_mutex_lock(&s_state_lock);
    if (text) {
        strncpy(s_pending.bit_info, text, sizeof(s_pending.bit_info) - 1);
        s_pending.bit_info[sizeof(s_pending.bit_info) - 1] = '\0';
    } else {
        s_pending.bit_info[0] = '\0';
    }
    s_dirty = true;
    os_mutex_unlock(&s_state_lock);
}

void ui_set_next_track(const char *title, const char *artist) {
    os_mutex_lock(&s_state_lock);
    if (title && title[0] != '\0') {
        strncpy(s_pending.next_line1, title, sizeof(s_pending.next_line1) - 1);
        s_pending.next_line1[sizeof(s_pending.next_line1) - 1] = '\0';
        // Same streaming-service noise cleanup the current title (ui_set_track)
        // and album (ui_set_album) already get - without it "Coming up" could
        // still show "(Remastered 2011)" beside an already-cleaned Now Playing.
        // If the filter strips the whole title, next_line1 ends up empty and
        // the group hides, which is the existing "no next track" behavior.
        track_title_filter_apply(s_pending.next_line1, sizeof(s_pending.next_line1));
        if (artist) {
            strncpy(s_pending.next_line2, artist, sizeof(s_pending.next_line2) - 1);
            s_pending.next_line2[sizeof(s_pending.next_line2) - 1] = '\0';
        } else {
            s_pending.next_line2[0] = '\0';
        }
    } else {
        // Empty title hides the whole "Coming up" group (see apply_state()) -
        // clear the artist too so a later title-only call can't show a
        // stale artist left over from a previous, unrelated next-track.
        s_pending.next_line1[0] = '\0';
        s_pending.next_line2[0] = '\0';
    }
    s_dirty = true;
    os_mutex_unlock(&s_state_lock);
}

void ui_set_next_track_none(bool none) {
    os_mutex_lock(&s_state_lock);
    s_pending.next_track_none = none;
    s_dirty = true;
    os_mutex_unlock(&s_state_lock);
}

void ui_set_volume(int vol) {
    os_mutex_lock(&s_state_lock);
    s_pending.volume = vol;
    s_dirty = true;
    os_mutex_unlock(&s_state_lock);
}

void ui_set_volume_with_range(float vol, float vol_min, float vol_max, float vol_step) {
    os_mutex_lock(&s_state_lock);
    s_pending.volume = vol;
    s_pending.volume_min = vol_min;
    s_pending.volume_max = vol_max;
    s_pending.volume_step = vol_step;
    s_dirty = true;
    os_mutex_unlock(&s_state_lock);
}

void ui_show_volume_change(float vol, float vol_step) {
    s_last_predicted_volume = vol;  // Track prediction for emphasis suppression

    // Note: Reading volume_min/volume_max without lock (atomic float reads, self-correct on next poll if stale)

    // Update volume ring immediately (optimistic)
    redraw_volume_ring(vol, s_pending.volume_min, s_pending.volume_max);

    // Update volume label immediately (optimistic)
    char vol_text[16];
    format_volume_text(vol_text, sizeof(vol_text), vol, s_pending.volume_min, vol_step);

    // Loud-volume warning color - see volume_hot_color_for_db(). Set before
    // emphasize_volume_label() below so the "just changed" blue flash still
    // wins temporarily on the Music-screen label (unless already in
    // warning territory - see that function); the flash's own reset timer
    // settles back to this color via s_volume_label_resting_db rather than
    // always white. The TV/Vinyl hero label has no flash to protect, so it
    // goes straight to whichever color is current.
    s_volume_label_resting_db = derive_volume_db_equivalent(vol, s_pending.volume_min, s_pending.volume_max);
    lv_color_t volume_color = volume_hot_color_for_db(s_volume_label_resting_db, lv_color_hex(0xfafafa));
    if (s_volume_label_large) {
        lv_obj_set_style_text_color(s_volume_label_large, volume_color, 0);
        set_haloed_label_text(s_volume_label_large, s_volume_label_halo, vol_text);
        emphasize_volume_label();
    }
    if (s_tv_vinyl_volume_label) {
        lv_obj_set_style_text_color(s_tv_vinyl_volume_label, volume_color, 0);
        lv_label_set_text(s_tv_vinyl_volume_label, vol_text);
    }
    if (s_detail_volume_label) {
        lv_label_set_text(s_detail_volume_label, vol_text);
    }

    char db_text[16];
    snprintf(db_text, sizeof(db_text), "%.1f dB",
             derive_volume_db_equivalent(vol, s_pending.volume_min, s_pending.volume_max));
    if (s_volume_db_label) {
        lv_obj_set_style_text_color(s_volume_db_label, volume_color, 0);
    }
    set_haloed_label_text(s_volume_db_label, s_volume_db_label_halo, db_text);
    if (s_tv_vinyl_db_label) {
        lv_label_set_text(s_tv_vinyl_db_label, db_text);
    }
    if (s_detail_db_label) {
        lv_label_set_text(s_detail_db_label, db_text);
    }
}

void ui_set_playing(bool playing) {
    os_mutex_lock(&s_state_lock);
    s_pending.playing = playing;
    s_dirty = true;
    os_mutex_unlock(&s_state_lock);
}

void ui_set_online(bool online) {
    os_mutex_lock(&s_state_lock);
    s_pending.online = online;
    s_dirty = true;
    os_mutex_unlock(&s_state_lock);
}

void ui_set_zone_name(const char *zone_name) {
    os_mutex_lock(&s_state_lock);
    if (zone_name) {
        strncpy(s_pending.zone_name, zone_name, sizeof(s_pending.zone_name) - 1);
        s_pending.zone_name[sizeof(s_pending.zone_name) - 1] = '\0';
        s_zone_name_dirty = true;
    }
    os_mutex_unlock(&s_state_lock);
}

void ui_set_message(const char *message) {
    os_mutex_lock(&s_state_lock);
    strncpy(s_pending_message, message, sizeof(s_pending_message) - 1);
    s_pending_message[sizeof(s_pending_message) - 1] = '\0';
    s_message_dirty = true;
    os_mutex_unlock(&s_state_lock);
}

void ui_set_network_status(const char *status) {
    os_mutex_lock(&s_state_lock);
    if (status) {
        strncpy(s_network_status, status, sizeof(s_network_status) - 1);
        s_network_status[sizeof(s_network_status) - 1] = '\0';
    } else {
        s_network_status[0] = '\0';  // Clear status
    }
    s_network_status_dirty = true;
    os_mutex_unlock(&s_state_lock);
}

void ui_set_progress(int seek_ms, int length_ms) {
    os_mutex_lock(&s_state_lock);
    s_pending.seek_position = seek_ms;
    s_pending.length = length_ms;
    s_dirty = true;
    os_mutex_unlock(&s_state_lock);
}

// ============================================================================
// Backward Compatibility API Wrappers
// ============================================================================

void ui_update(const char *line1, const char *line2, bool playing, float volume, float volume_min, float volume_max, float volume_step, int seek_position, int length) {
    ui_set_track(line1, line2);
    ui_set_playing(playing);
    ui_set_volume_with_range(volume, volume_min, volume_max, volume_step);
    ui_set_progress(seek_position, length);
}

void ui_set_status(bool online) {
    ui_set_online(online);
}

// Debug: Test pattern to verify LVGL -> panel color format
void ui_test_pattern(void) {
#ifdef ESP_PLATFORM
    // Log LVGL's actual color values to understand byte order
    lv_color_t red = lv_color_make(0xFF, 0x00, 0x00);
    lv_color_t green = lv_color_make(0x00, 0xFF, 0x00);
    lv_color_t blue = lv_color_make(0x00, 0x00, 0xFF);
    ESP_LOGI(UI_TAG, "LVGL color values:");
    ESP_LOGI(UI_TAG, "  RED   (255,0,0)   = 0x%04X", lv_color_to_u16(red));
    ESP_LOGI(UI_TAG, "  GREEN (0,255,0)   = 0x%04X", lv_color_to_u16(green));
    ESP_LOGI(UI_TAG, "  BLUE  (0,0,255)   = 0x%04X", lv_color_to_u16(blue));

    static uint8_t *test_buf = NULL;
    int w = 360;
    int h = 360;
    size_t sz = w * h * 2;

    if (!test_buf) {
        test_buf = heap_caps_aligned_calloc(16, 1, sz,
                                            MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    }
    if (!test_buf) {
        ESP_LOGE(UI_TAG, "Failed to allocate test pattern buffer");
        return;
    }

    // Simple solid color blocks - easier to diagnose than gradient
    // Top to bottom: Red, Green, Blue, White
    uint16_t *p = (uint16_t *)test_buf;
    for (int y = 0; y < h; y++) {
        uint16_t c;
        if (y < h / 4) {
            c = lv_color_to_u16(red);  // LVGL Red
        } else if (y < h / 2) {
            c = lv_color_to_u16(green);  // LVGL Green
        } else if (y < 3 * h / 4) {
            c = lv_color_to_u16(blue);  // LVGL Blue
        } else {
            c = 0xFFFF;  // White (same in all formats)
        }
        for (int x = 0; x < w; x++) {
            *p++ = c;
        }
    }

    static lv_image_dsc_t img_dsc;
    memset(&img_dsc, 0, sizeof(img_dsc));
    img_dsc.header.magic = LV_IMAGE_HEADER_MAGIC;
    img_dsc.header.cf = LV_COLOR_FORMAT_RGB565;
    img_dsc.header.w = w;
    img_dsc.header.h = h;
    img_dsc.data = test_buf;
    img_dsc.data_size = sz;

    lv_image_set_src(s_artwork_image, &img_dsc);
    lv_obj_clear_flag(s_artwork_image, LV_OBJ_FLAG_HIDDEN);
    lv_obj_set_size(s_artwork_image, w, h);
    lv_obj_center(s_artwork_image);

    ESP_LOGI(UI_TAG, "Test pattern: 4 solid bars (red/green/blue/white)");
#endif
}

void ui_set_artwork(const char *image_key) {
    // Check if image_key changed
    if (!image_key || !image_key[0]) {
        // No artwork - hide image
        if (s_last_image_key[0]) {
            lv_obj_add_flag(s_artwork_image, LV_OBJ_FLAG_HIDDEN);
            if (s_detail_thumbnail_mask) lv_obj_add_flag(s_detail_thumbnail_mask, LV_OBJ_FLAG_HIDDEN);
            s_last_image_key[0] = '\0';
        }
        return;
    }

    // Skip if same image
    if (strcmp(image_key, s_last_image_key) == 0) {
        return;
    }

    // Build artwork URL (request 360x360 to match display - no scaling needed)
    // With PSRAM enabled, we can handle the full display resolution
    char url[512];
    if (!bridge_client_get_artwork_url(url, sizeof(url), SCREEN_SIZE, SCREEN_SIZE)) {
        ESP_LOGW(UI_TAG, "Failed to build artwork URL");
        return;
    }

    ESP_LOGI(UI_TAG, "Fetching artwork: %s", url);

    // Fetch image data (JPEG)
    char *img_data = NULL;
    size_t img_len = 0;
    int ret = platform_http_get_image(url, &img_data, &img_len);

    if (ret != 0 || !img_data || img_len == 0) {
        ESP_LOGW(UI_TAG, "Failed to fetch artwork (ret=%d, len=%zu)", ret, img_len);
        platform_http_free(img_data);
        lv_obj_add_flag(s_artwork_image, LV_OBJ_FLAG_HIDDEN);
        return;
    }

    ESP_LOGI(UI_TAG, "Artwork fetched: %zu bytes", img_len);

#ifdef ESP_PLATFORM
    // Expect raw RGB565 data from bridge (format=rgb565)
    // Expected size: width * height * 2 bytes
    const size_t expected_rgb565_size = SCREEN_SIZE * SCREEN_SIZE * 2;
    if (img_len != expected_rgb565_size) {
        ESP_LOGW(UI_TAG, "Unexpected image size: %zu bytes (expected %zu for %dx%d RGB565)",
                 img_len, expected_rgb565_size, SCREEN_SIZE, SCREEN_SIZE);
        platform_http_free(img_data);
        lv_obj_add_flag(s_artwork_image, LV_OBJ_FLAG_HIDDEN);
        return;
    }

    ESP_LOGI(UI_TAG, "Processing raw RGB565 format (%zu bytes)", img_len);

    // Copy to global buffer (maintains ownership model)
    ui_jpeg_image_t new_img;
    bool ok = ui_rgb565_from_buffer((const uint8_t *)img_data,
                                    SCREEN_SIZE, SCREEN_SIZE, &new_img);

    // HTTP buffer no longer needed after copy
    platform_http_free(img_data);

    if (!ok) {
        ESP_LOGW(UI_TAG, "Failed to process RGB565 data");
        lv_obj_add_flag(s_artwork_image, LV_OBJ_FLAG_HIDDEN);
        return;
    }

    // Free previous artwork pixels
    ui_jpeg_free(&s_artwork_img);

    // Take ownership of new pixels and descriptor
    s_artwork_img = new_img;

    // Show it in LVGL
    lv_image_set_src(s_artwork_image, &s_artwork_img.dsc);
    lv_obj_clear_flag(s_artwork_image, LV_OBJ_FLAG_HIDDEN);
    lv_obj_set_size(s_artwork_image,
                    s_artwork_img.dsc.header.w,
                    s_artwork_img.dsc.header.h);
    lv_obj_center(s_artwork_image);
    lv_obj_invalidate(s_artwork_image);

    // Detail screen's thumbnail - shares the same decoded pixel buffer via
    // its own lv_img object rather than a separate decode/resize, scaled
    // down with lv_image_set_scale() (256 = 1:1). Kept in sync here so it's
    // never stale even though it's usually hidden (ui_set_detail_mode).
    if (s_detail_thumbnail && s_artwork_img.dsc.header.w > 0) {
        lv_image_set_src(s_detail_thumbnail, &s_artwork_img.dsc);
        lv_image_set_scale(s_detail_thumbnail, (DETAIL_ARTWORK_SIZE * 256) / s_artwork_img.dsc.header.w);
        // Size/position are fixed at creation (filling s_detail_thumbnail_mask,
        // which is what's actually positioned/sized against s_detail_overlay
        // and does the circular clipping) - nothing to redo here per-track.
        // Nested inside s_detail_overlay, whose own HIDDEN flag already
        // governs whether any of this actually renders - clearing this
        // one unconditionally (like s_artwork_image does) is harmless
        // either way, not gated on whether detail mode happens to be
        // active right now.
        lv_obj_clear_flag(s_detail_thumbnail_mask, LV_OBJ_FLAG_HIDDEN);
        lv_obj_invalidate(s_detail_thumbnail);
    }

    strncpy(s_last_image_key, image_key, sizeof(s_last_image_key) - 1);
    s_last_image_key[sizeof(s_last_image_key) - 1] = '\0';

    ESP_LOGI(UI_TAG, "Artwork displayed");
#else
    // PC simulator - still use raw JPEG (TJPGD or similar)
    if (s_artwork_data) {
        platform_http_free(s_artwork_data);
    }
    s_artwork_data = img_data;

    static lv_image_dsc_t img_dsc;
    img_dsc.header.cf = LV_COLOR_FORMAT_RAW;  // Let LVGL detect format
    img_dsc.header.w = 0;
    img_dsc.header.h = 0;
    img_dsc.data = (const uint8_t *)s_artwork_data;
    img_dsc.data_size = img_len;

    lv_image_set_src(s_artwork_image, &img_dsc);
    lv_obj_clear_flag(s_artwork_image, LV_OBJ_FLAG_HIDDEN);

    strncpy(s_last_image_key, image_key, sizeof(s_last_image_key) - 1);
    s_last_image_key[sizeof(s_last_image_key) - 1] = '\0';

    ESP_LOGI(UI_TAG, "Artwork displayed (PC sim)");
#endif
}

bool ui_is_zone_picker_visible(void) {
    // Don't log every call - too noisy
    return s_zone_picker_visible;
}

bool ui_is_playing(void) {
    os_mutex_lock(&s_state_lock);
    bool playing = s_pending.playing;
    os_mutex_unlock(&s_state_lock);
    return playing;
}

int ui_zone_picker_get_selected(void) {
    return ui_get_zone_picker_selected();
}

void ui_zone_picker_scroll(int delta) {
    if (!s_zone_picker_visible || !s_zone_list || s_zone_picker_count == 0) {
        return;
    }

    // Calculate new position with wraparound
    int new_pos = s_zone_picker_selected + delta;
    if (new_pos < 0) new_pos = s_zone_picker_count - 1;
    if (new_pos >= s_zone_picker_count) new_pos = 0;

    // Update visual selection
    if (new_pos != s_zone_picker_selected) {
        // Remove highlight from old selection
        lv_obj_t *old_btn = lv_obj_get_child(s_zone_list, s_zone_picker_selected);
        if (old_btn) {
            lv_obj_set_style_bg_color(old_btn, lv_color_hex(0x1a1a1a), 0);
        }

        // Add highlight to new selection
        lv_obj_t *new_btn = lv_obj_get_child(s_zone_list, new_pos);
        if (new_btn) {
            lv_obj_set_style_bg_color(new_btn, lv_color_hex(0x2a4a6a), 0);
            lv_obj_scroll_to_view(new_btn, LV_ANIM_ON);
        }

        s_zone_picker_selected = new_pos;
    }
}

// ============================================================================
// OTA Update UI
// ============================================================================

#ifdef ESP_PLATFORM
#include "ota_update.h"
#endif

static void update_btn_clicked(lv_event_t *e) {
    (void)e;
    ESP_LOGI(UI_TAG, "Update button clicked");
    ui_trigger_update();
}

void ui_set_update_available(const char *version) {
    if (version && version[0]) {
        strncpy(s_update_version, version, sizeof(s_update_version) - 1);
        s_update_version[sizeof(s_update_version) - 1] = '\0';
        ESP_LOGI(UI_TAG, "Update available: %s", s_update_version);

        // Hide settings panel so update button is visible
        ui_hide_settings();

        // Create update button if it doesn't exist
        if (!s_update_btn && s_ui_container) {
            s_update_btn = lv_btn_create(s_ui_container);
            lv_obj_set_size(s_update_btn, 200, 40);
            lv_obj_align(s_update_btn, LV_ALIGN_TOP_MID, 0, 60);
            lv_obj_set_style_bg_color(s_update_btn, lv_color_hex(0x4CAF50), 0);  // Green
            lv_obj_set_style_radius(s_update_btn, 20, 0);

            lv_obj_t *label = lv_label_create(s_update_btn);
            lv_obj_set_style_text_font(label, font_small(), 0);
            lv_obj_center(label);
            lv_obj_set_user_data(s_update_btn, label);  // Store label reference

            lv_obj_add_event_cb(s_update_btn, update_btn_clicked, LV_EVENT_CLICKED, NULL);
        }

        if (s_update_btn) {
            lv_obj_t *label = lv_obj_get_user_data(s_update_btn);
            if (label) {
                char text[64];
                snprintf(text, sizeof(text), UI_ICON_DOWNLOAD " Update to %s", s_update_version);
                lv_label_set_text(label, text);
            }
            lv_obj_clear_flag(s_update_btn, LV_OBJ_FLAG_HIDDEN);
        }
    } else {
        s_update_version[0] = '\0';
        if (s_update_btn) {
            lv_obj_add_flag(s_update_btn, LV_OBJ_FLAG_HIDDEN);
        }
    }
}

void ui_set_update_progress(int percent) {
    s_update_progress = percent;

    if (s_update_btn) {
        lv_obj_t *label = lv_obj_get_user_data(s_update_btn);
        if (label) {
            if (percent >= 0 && percent <= 100) {
                char text[64];
                snprintf(text, sizeof(text), "Updating... %d%%", percent);
                lv_label_set_text(label, text);
                lv_obj_set_style_bg_color(s_update_btn, lv_color_hex(0x2196F3), 0);  // Blue during update
                lv_obj_clear_flag(s_update_btn, LV_OBJ_FLAG_CLICKABLE);  // Disable clicking during update
            } else if (percent < 0) {
                // Hide or reset
                lv_obj_add_flag(s_update_btn, LV_OBJ_FLAG_HIDDEN);
                lv_obj_add_flag(s_update_btn, LV_OBJ_FLAG_CLICKABLE);
            }
        }
    }
}

void ui_trigger_update(void) {
#ifdef ESP_PLATFORM
    ESP_LOGI(UI_TAG, "Triggering OTA update");
    ota_start_update();
#else
    ESP_LOGI(UI_TAG, "OTA update not available on PC simulator");
#endif
}

// ============================================================================
// Display State Control - Art Mode
// ============================================================================

void ui_set_controls_visible(bool visible) {
    if (visible) {
        // Show all controls
        if (s_btn_prev) lv_obj_clear_flag(s_btn_prev, LV_OBJ_FLAG_HIDDEN);
        if (s_btn_play) lv_obj_clear_flag(s_btn_play, LV_OBJ_FLAG_HIDDEN);
        if (s_btn_next) lv_obj_clear_flag(s_btn_next, LV_OBJ_FLAG_HIDDEN);
        // Track/artist/tint are never hidden by this function at all (see
        // the else branch) - no need to show them here either.
        if (s_volume_label_large) lv_obj_clear_flag(s_volume_label_large, LV_OBJ_FLAG_HIDDEN);
        for (int i = 0; i < 8; i++) {
            if (s_volume_label_halo[i]) lv_obj_clear_flag(s_volume_label_halo[i], LV_OBJ_FLAG_HIDDEN);
        }
        if (s_volume_db_label) lv_obj_clear_flag(s_volume_db_label, LV_OBJ_FLAG_HIDDEN);
        for (int i = 0; i < 8; i++) {
            if (s_volume_db_label_halo[i]) lv_obj_clear_flag(s_volume_db_label_halo[i], LV_OBJ_FLAG_HIDDEN);
        }
        // Badge, not the icon directly - hiding/showing the parent already
        // covers the icon (its child), and the badge is what needs to
        // disappear too rather than leaving an empty dark tint floating
        // on screen in art mode.
        if (s_battery_badge) lv_obj_clear_flag(s_battery_badge, LV_OBJ_FLAG_HIDDEN);
        if (s_status_dot) lv_obj_clear_flag(s_status_dot, LV_OBJ_FLAG_HIDDEN);
        if (s_status_bar) lv_obj_clear_flag(s_status_bar, LV_OBJ_FLAG_HIDDEN);
        // Artwork stays full brightness (ADR: no full-screen darkening mask) -
        // legibility comes from the lower-third tint object, not image opacity.
        if (s_artwork_image) lv_obj_set_style_img_opa(s_artwork_image, LV_OPA_COVER, 0);
        ESP_LOGI(UI_TAG, "Controls shown");
        // Force battery display update after showing controls (GH-86)
        // Without this, hysteresis in update_battery_display() prevents the icon from reappearing
        s_last_battery_level = -1;
        update_battery_display();
    } else {
        // Hide controls for art mode - owner feedback: only the volume
        // display (number + dB), battery icon, and transport buttons
        // should disappear here. Track/artist text and the lower-third
        // tint behind them stay exactly as in normal mode.
        if (s_btn_prev) lv_obj_add_flag(s_btn_prev, LV_OBJ_FLAG_HIDDEN);
        if (s_btn_play) lv_obj_add_flag(s_btn_play, LV_OBJ_FLAG_HIDDEN);
        if (s_btn_next) lv_obj_add_flag(s_btn_next, LV_OBJ_FLAG_HIDDEN);
        if (s_volume_label_large) lv_obj_add_flag(s_volume_label_large, LV_OBJ_FLAG_HIDDEN);
        for (int i = 0; i < 8; i++) {
            if (s_volume_label_halo[i]) lv_obj_add_flag(s_volume_label_halo[i], LV_OBJ_FLAG_HIDDEN);
        }
        if (s_volume_db_label) lv_obj_add_flag(s_volume_db_label, LV_OBJ_FLAG_HIDDEN);
        for (int i = 0; i < 8; i++) {
            if (s_volume_db_label_halo[i]) lv_obj_add_flag(s_volume_db_label_halo[i], LV_OBJ_FLAG_HIDDEN);
        }
        if (s_battery_badge) lv_obj_add_flag(s_battery_badge, LV_OBJ_FLAG_HIDDEN);
        if (s_status_dot) lv_obj_add_flag(s_status_dot, LV_OBJ_FLAG_HIDDEN);
        if (s_status_bar) lv_obj_add_flag(s_status_bar, LV_OBJ_FLAG_HIDDEN);
        // Make artwork fully visible in art mode
        if (s_artwork_image) lv_obj_set_style_img_opa(s_artwork_image, LV_OPA_COVER, 0);
        ESP_LOGI(UI_TAG, "Controls hidden (art mode)");
    }
}

// Battery/perf: the progress-arc interpolation timer and every marquee
// label keep doing per-frame work even with the panel powered off and
// nothing visible - undercuts whatever power display_sleep.c saves
// elsewhere. Marquee labels can't be "paused" through LVGL's public API
// (the scroll animation is internal to LV_LABEL_LONG_SCROLL_CIRCULAR, not
// a separate lv_anim_t callers can touch) - dropping to LV_LABEL_LONG_CLIP
// for the duration is the safe, public-API way to stop the animation
// without disturbing anything else about the label; restoring
// SCROLL_CIRCULAR on wake just restarts the marquee from the beginning,
// which is unnoticeable since the screen was off anyway.
void ui_set_background_animation_paused(bool paused) {
    if (s_progress_interp_timer) {
        if (paused) {
            lv_timer_pause(s_progress_interp_timer);
        } else {
            // Freeze the interpolation baseline at "now" rather than letting
            // the next tick compute elapsed time across the whole sleep
            // duration (which would show progress jumping far ahead for one
            // frame) - the next real poll corrects s_progress_base_ms/
            // s_progress_base_uptime_ms properly, same self-correcting
            // pattern this file already uses elsewhere for seek/skip/pause
            // gaps between poll cycles.
            s_progress_base_uptime_ms = platform_millis();
            lv_timer_resume(s_progress_interp_timer);
        }
    }

    lv_label_long_mode_t mode = paused ? LV_LABEL_LONG_CLIP : LV_LABEL_LONG_SCROLL_CIRCULAR;
    if (s_track_label) lv_label_set_long_mode(s_track_label, mode);
    if (s_artist_label) lv_label_set_long_mode(s_artist_label, mode);
    if (s_detail_title_label) lv_label_set_long_mode(s_detail_title_label, mode);
    if (s_detail_artist_label) lv_label_set_long_mode(s_detail_artist_label, mode);
    if (s_detail_album_label) lv_label_set_long_mode(s_detail_album_label, mode);
    if (s_detail_next_title_label) lv_label_set_long_mode(s_detail_next_title_label, mode);
    if (s_detail_next_artist_label) lv_label_set_long_mode(s_detail_next_artist_label, mode);

    if (!paused) {
        // lvgl_flush_cb() now skips pushing pixels to the panel entirely
        // while display_is_sleeping() (separate fix, same battery
        // motivation) - LVGL's widgets still got every real content
        // update that happened during sleep (track/artist/artwork/etc.,
        // never gated on sleep state - apply_state() has to keep them
        // correct regardless), but none of those flushed to the actual
        // panel. Without this, the panel would show whatever was last
        // physically flushed before sleep began until something else
        // happened to trigger a redraw after waking. Force one now so
        // what's on screen matches LVGL's actual state the moment the
        // panel comes back on.
        lv_obj_invalidate(lv_screen_active());
    }
}

// ============================================================================
// Display State Control - Detail Info Screen
// ============================================================================

void ui_set_detail_mode(bool active) {
    if (!s_detail_overlay || active == s_detail_mode_visible) {
        return;
    }
    s_detail_mode_visible = active;
    if (active) {
        // Explicit foreground move rather than relying on creation order -
        // see build_detail_overlay()'s comment on why.
        lv_obj_move_foreground(s_detail_overlay);
        lv_obj_remove_flag(s_detail_overlay, LV_OBJ_FLAG_HIDDEN);
        // Re-raise the rings above the overlay we just moved in front of
        // them - owner feedback that they should stay visible on this
        // screen rather than getting covered like everything else. Safe to
        // leave them promoted after exiting detail mode too: this is their
        // already-correct resting z-order (on top of s_ui_container), not
        // something special to detail mode.
        if (s_progress_arc) lv_obj_move_foreground(s_progress_arc);
        // The encoder now jogs seek position instead of volume on this
        // screen (see ui_seek_adjust/CONTROLLER_INTERACTION_CONTEXT_SEEK) -
        // the volume ring has no role here, so hide it and let the
        // progress arc grow into the space it leaves behind: 3x thicker
        // and out closer to the true screen edge (still short of it) now
        // that it's the only ring on this screen.
        if (s_volume_canvas) lv_obj_add_flag(s_volume_canvas, LV_OBJ_FLAG_HIDDEN);
        if (s_progress_arc) {
            lv_obj_set_size(s_progress_arc, PROGRESS_ARC_SIZE_SEEK, PROGRESS_ARC_SIZE_SEEK);
            lv_obj_center(s_progress_arc);
            lv_obj_set_style_arc_width(s_progress_arc, PROGRESS_ARC_WIDTH_SEEK, LV_PART_MAIN);
            lv_obj_set_style_arc_width(s_progress_arc, PROGRESS_ARC_WIDTH_SEEK, LV_PART_INDICATOR);
        }
        (void)controller_input_set_context(CONTROLLER_INTERACTION_CONTEXT_SEEK);
        // Paint current values immediately rather than waiting for the next
        // poll or interpolation tick - title/artist/album already get
        // updated unconditionally in apply_state(), but the progress label
        // only updates when this flag is already true (see
        // progress_interp_timer_cb), so force one now.
        if (s_detail_progress_label && s_progress_base_ms >= 0 && s_progress_length_ms > 0) {
            char elapsed_text[16];
            char total_text[16];
            format_mmss(elapsed_text, sizeof(elapsed_text), s_progress_base_ms);
            format_mmss(total_text, sizeof(total_text), s_progress_length_ms);
            char combined[40];
            snprintf(combined, sizeof(combined), "%s / %s", elapsed_text, total_text);
            lv_label_set_text(s_detail_progress_label, combined);
        }
        ESP_LOGI(UI_TAG, "Detail info screen shown");
    } else {
        lv_obj_add_flag(s_detail_overlay, LV_OBJ_FLAG_HIDDEN);
        // Commit rather than drop an in-flight scrub - the user still
        // meant that seek even though they then swiped away before the
        // debounce fired.
        if (s_seek_active) {
            if (s_seek_commit_timer) {
                lv_timer_del(s_seek_commit_timer);
                s_seek_commit_timer = NULL;
            }
            seek_commit();
        }
        if (s_volume_canvas) lv_obj_remove_flag(s_volume_canvas, LV_OBJ_FLAG_HIDDEN);
        if (s_progress_arc) {
            lv_obj_set_size(s_progress_arc, PROGRESS_ARC_SIZE_NORMAL, PROGRESS_ARC_SIZE_NORMAL);
            lv_obj_center(s_progress_arc);
            lv_obj_set_style_arc_width(s_progress_arc, PROGRESS_ARC_WIDTH_NORMAL, LV_PART_MAIN);
            lv_obj_set_style_arc_width(s_progress_arc, PROGRESS_ARC_WIDTH_NORMAL, LV_PART_INDICATOR);
        }
        (void)controller_input_set_context(CONTROLLER_INTERACTION_CONTEXT_MEDIA);
        ESP_LOGI(UI_TAG, "Detail info screen hidden");
    }
}
