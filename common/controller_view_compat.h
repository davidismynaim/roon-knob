#pragma once

#include "controller_view.h"

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Translate owned controller patches to the existing link-time-selected
 * presentation API. Call on the existing UI task; the target adapter consumes
 * the supplied value synchronously.
 */
void controller_view_compat_apply_media(const controller_media_view_t *view);
void controller_view_compat_apply_media_enrichment(
    const controller_media_enrichment_view_t *view);
void controller_view_compat_apply_connectivity(
    const controller_connectivity_view_t *view);

/*
 * Optional target hook: while it returns true, bridge media and enrichment
 * patches are dropped because another source (the Dial's vinyl feed) owns the
 * media display. Unset on every target that has no such source.
 */
typedef bool (*controller_view_compat_suppress_fn_t)(void);
void controller_view_compat_set_suppress_fn(controller_view_compat_suppress_fn_t fn);

/* Reset only compatibility bookkeeping; no renderer call is made. */
void controller_view_compat_reset(void);

#ifdef __cplusplus
}
#endif
