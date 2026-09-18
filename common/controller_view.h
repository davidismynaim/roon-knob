#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define CONTROLLER_MEDIA_TEXT_CAPACITY 128
#define CONTROLLER_ARTWORK_REF_CAPACITY 128
#define CONTROLLER_CONNECTIVITY_TEXT_CAPACITY 96

/*
 * Owned media patch. All strings are always NUL-terminated. The value may be
 * queued by ownership transfer and consumed synchronously on the UI task.
 */
typedef struct {
    char primary[CONTROLLER_MEDIA_TEXT_CAPACITY];
    char secondary[CONTROLLER_MEDIA_TEXT_CAPACITY];
    char tertiary[CONTROLLER_MEDIA_TEXT_CAPACITY];
    char artwork_ref[CONTROLLER_ARTWORK_REF_CAPACITY];
    bool playing;
    float volume;
    float volume_min;
    float volume_max;
    float volume_step;
    int32_t progress_position;
    int32_t progress_duration;
    uint32_t artwork_generation;
} controller_media_view_t;

/*
 * Recovery/connectivity is a separate patch so posting it does not allocate a
 * media-sized value or imply that it replaces durable media state.
 */
typedef struct {
    char headline[CONTROLLER_CONNECTIVITY_TEXT_CAPACITY];
    char detail[CONTROLLER_CONNECTIVITY_TEXT_CAPACITY];
} controller_connectivity_view_t;

#define CONTROLLER_ENRICHMENT_TEXT_CAPACITY 128
#define CONTROLLER_ENRICHMENT_BIT_INFO_CAPACITY 64

/*
 * Detail-screen enrichment (next track / album year / bit info) - a separate
 * patch for the same reason connectivity is: these are Dial-only, optional,
 * and absent far more often than present (no live source yet feeds them at
 * all - see common/ui.h's ui_set_next_track()/ui_set_album_year()/
 * ui_set_bit_info()), so folding them into controller_media_view_t would
 * mean every target - including Frame/RLCD, which have no use for any of
 * this - pays for the extra ~324 bytes on every single media update just to
 * stay under that struct's own tight 576-byte budget. album_year 0 means
 * unknown; empty strings mean absent, matching those setters' own contract.
 */
typedef struct {
    char next_track_title[CONTROLLER_ENRICHMENT_TEXT_CAPACITY];
    char next_track_artist[CONTROLLER_ENRICHMENT_TEXT_CAPACITY];
    int32_t album_year;
    char bit_info[CONTROLLER_ENRICHMENT_BIT_INFO_CAPACITY];
} controller_media_enrichment_view_t;

void controller_media_view_init(
    controller_media_view_t *view,
    const char *primary,
    const char *secondary,
    const char *tertiary,
    bool playing,
    float volume,
    float volume_min,
    float volume_max,
    float volume_step,
    int32_t progress_position,
    int32_t progress_duration,
    const char *artwork_ref,
    uint32_t artwork_generation);

void controller_connectivity_view_init(
    controller_connectivity_view_t *view,
    const char *headline,
    const char *detail);

void controller_media_enrichment_view_init(
    controller_media_enrichment_view_t *view,
    const char *next_track_title,
    const char *next_track_artist,
    int32_t album_year,
    const char *bit_info);

#if defined(__cplusplus)
static_assert(sizeof(controller_media_view_t) <= 576,
              "controller media patch exceeds its ESP32 budget");
static_assert(sizeof(controller_connectivity_view_t) <= 192,
              "controller connectivity patch exceeds its ESP32 budget");
static_assert(sizeof(controller_media_enrichment_view_t) <= 384,
              "controller media enrichment patch exceeds its ESP32 budget");
#else
_Static_assert(sizeof(controller_media_view_t) <= 576,
               "controller media patch exceeds its ESP32 budget");
_Static_assert(sizeof(controller_connectivity_view_t) <= 192,
               "controller connectivity patch exceeds its ESP32 budget");
_Static_assert(sizeof(controller_media_enrichment_view_t) <= 384,
               "controller media enrichment patch exceeds its ESP32 budget");
#endif

#ifdef __cplusplus
}
#endif
