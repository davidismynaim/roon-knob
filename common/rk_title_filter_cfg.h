#pragma once

// Track-title cleanup config (strips streaming-service noise like
// "(Remastered 2011)" or "(Album Version)" from the Now Playing track
// title before it's displayed). Dial-only feature - see track_title_filter.c
// for the matcher and common/ui.c's ui_set_track() for where it's applied.
//
// Deliberately its own small NVS blob (namespace "rk_titlef", key "cfg"),
// same reasoning as rk_ha_cfg.h: unrelated to rk_cfg_t's V1/V2/V3
// migration machinery, so this can't accidentally disturb that contract.

#include "rk_cfg.h"  // rk_strlcpy

#include <stdbool.h>
#include <stdint.h>

#define RK_TITLE_FILTER_CFG_CURRENT_VER 1

// One pattern per line - just the inner phrase, e.g. "Remastered YYYY", not
// "(Remastered YYYY)". track_title_filter.c's matcher strips a whole
// (), [], or {} segment if it contains ANY configured pattern anywhere
// within it (not necessarily the segment's entire content - real tags
// often stack several fragments in one bracket, e.g.
// "(2009 Remaster; Remastered LP Version)", which no single phrase would
// ever match end-to-end). A "- phrase" dash suffix has no closing
// delimiter to bound a segment, so that form still requires an exact
// literal match.
//
// Two wildcard tokens, both crude on purpose (no real regex support):
//   YYYY  matches exactly 4 consecutive digits (e.g. "2011")
//   YY    matches exactly 2 consecutive digits (e.g. "11")
//   NUM   matches 1 or more consecutive digits (e.g. "24", "96", "44")
// Matching is otherwise a plain case-insensitive substring search.
#define RK_TITLE_FILTER_PATTERNS_MAX 4096

typedef struct {
    char patterns[RK_TITLE_FILTER_PATTERNS_MAX];
    uint8_t cfg_ver;
} rk_title_filter_cfg_t;

// Seeded as the default pattern list on first boot (before the user has
// customized it via the config page). Deliberately limited to pure
// reissue/format/edition noise - anything that names a genuinely different
// performance, arrangement, or mix (Live, Acoustic, Demo, Instrumental,
// Remix, Session, Alternate Take, etc.) or an attribution (feat./with/
// Remix by <Artist>) is left out on purpose, since that's real information
// about what's about to play, not clutter. Add more via the config page if
// you disagree with where this line was drawn for a specific tag.
#define RK_TITLE_FILTER_DEFAULT_PATTERNS \
    "Remastered\n" \
    "Remaster\n" \
    "Digitally Remastered\n" \
    "Digital Remaster\n" \
    "Newly Remastered\n" \
    "Newly Remastered Version\n" \
    "Remastered Version\n" \
    "Remaster Version\n" \
    "Remastered YY\n" \
    "Remastered YYYY\n" \
    "Remaster YY\n" \
    "Remaster YYYY\n" \
    "Digital Remaster YY\n" \
    "Digital Remaster YYYY\n" \
    "Digitally Remastered YY\n" \
    "Digitally Remastered YYYY\n" \
    "YY Remaster\n" \
    "YY Remastered\n" \
    "YY Digital Remaster\n" \
    "YY Digitally Remastered\n" \
    "YY Remaster Version\n" \
    "YY Remastered Version\n" \
    "YYYY Remaster\n" \
    "YYYY Remastered\n" \
    "YYYY Digital Remaster\n" \
    "YYYY Digitally Remastered\n" \
    "YYYY Remaster Version\n" \
    "YYYY Remastered Version\n" \
    "Stereo\n" \
    "Stereo Version\n" \
    "Mono\n" \
    "Mono Version\n" \
    "Hi-Res\n" \
    "Hi-Res Audio\n" \
    "High Resolution\n" \
    "High-Resolution\n" \
    "High Resolution Audio\n" \
    "HD\n" \
    "HD Audio\n" \
    "High Definition\n" \
    "High-Definition\n" \
    "Mastered for iTunes\n" \
    "Apple Digital Master\n" \
    "MFiT\n" \
    "Spatial Audio\n" \
    "Apple Spatial Audio\n" \
    "Dolby Atmos\n" \
    "Atmos\n" \
    "Immersive Audio\n" \
    "Binaural\n" \
    "360 Reality Audio\n" \
    "NUM-bit\n" \
    "NUM bit\n" \
    "NUMbit\n" \
    "NUM Bit Audio\n" \
    "NUMkHz\n" \
    "NUM kHz\n" \
    "NUMkHz Audio\n" \
    "NUM-bit/NUMkHz\n" \
    "NUM bit/NUM kHz\n" \
    "NUM bit / NUM kHz\n" \
    "Version\n" \
    "Version 1\n" \
    "Version 2\n" \
    "Original Version\n" \
    "Original Album Version\n" \
    "Original\n" \
    "Album Version\n" \
    "Album Edit\n" \
    "LP Version\n" \
    "Single Version\n" \
    "Single Edit\n" \
    "Radio Version\n" \
    "Radio Edit\n" \
    "Edit\n" \
    "Edited Version\n" \
    "Edited\n" \
    "Extended Version\n" \
    "Extended Edit\n" \
    "Long Version\n" \
    "Long Edit\n" \
    "Short Version\n" \
    "Short Edit\n" \
    "Full Version\n" \
    "New Version\n" \
    "Revised Version\n" \
    "YY Version\n" \
    "YY Edit\n" \
    "YYYY Version\n" \
    "YYYY Edit\n" \
    "Deluxe Edition\n" \
    "Deluxe Version\n" \
    "Expanded Edition\n" \
    "Expanded Version\n" \
    "Special Edition\n" \
    "Limited Edition\n" \
    "Collector's Edition\n" \
    "Collectors Edition\n" \
    "Super Deluxe Edition\n" \
    "Legacy Edition\n" \
    "Anniversary Edition\n" \
    "YY Anniversary Edition\n" \
    "YYYY Anniversary Edition\n" \
    "Explicit\n" \
    "Explicit Version\n" \
    "Explicit Edit\n" \
    "Clean\n" \
    "Clean Version\n" \
    "Clean Edit\n" \
    "Censored\n" \
    "Censored Version\n" \
    "Censored Edit\n" \
    "iTunes Version\n" \
    "iTunes Edit\n" \
    "Spotify Version\n" \
    "Spotify Edit\n" \
    "Tidal Version\n" \
    "Tidal Edit\n" \
    "YouTube Version\n" \
    "YouTube Edit\n" \
    "Amazon Version\n" \
    "Amazon Edit\n" \
    "Digital Version\n" \
    "Digital Edit\n" \
    "Streaming Version\n" \
    "Streaming Edit\n" \
    "Bonus\n" \
    "Bonus Track\n" \
    "Bonus Version\n" \
    "Hidden Track\n" \
    "Digital Bonus\n" \
    "Digital Bonus Track\n" \
    "iTunes Bonus\n" \
    "iTunes Bonus Track\n" \
    "Amazon Bonus\n" \
    "Amazon Bonus Track\n" \
    "Previously Unreleased\n" \
    "Previously Unreleased Track\n" \
    "Unreleased\n" \
    "New Song"

static inline bool rk_title_filter_cfg_is_valid(const rk_title_filter_cfg_t *cfg) {
    return cfg && cfg->cfg_ver != 0;
}

static inline void rk_title_filter_cfg_set_defaults(rk_title_filter_cfg_t *cfg) {
    if (!cfg) {
        return;
    }
    *cfg = (rk_title_filter_cfg_t){0};
    cfg->cfg_ver = RK_TITLE_FILTER_CFG_CURRENT_VER;
    rk_strlcpy(cfg->patterns, RK_TITLE_FILTER_DEFAULT_PATTERNS,
               sizeof(cfg->patterns));
}
