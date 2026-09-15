#pragma once

// Strips streaming-service title noise (e.g. "(Remastered 2011)",
// "(Album Version)") from the Now Playing track title before display.
// Dial-only - see common/ui.c's ui_set_track() for where this is applied.

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

// Loads the configured pattern list from NVS (see rk_title_filter_cfg.h).
// Call once at startup, before the first ui_set_track().
void track_title_filter_init(void);

// Strips every configured pattern found in `title`, in place. Safe to call
// with an empty pattern list (no-op) or an empty title.
void track_title_filter_apply(char *title, size_t title_buf_len);

#ifdef __cplusplus
}
#endif
