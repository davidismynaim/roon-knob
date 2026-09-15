#pragma once

// Dial-only replacement for the dynamic Roon zone picker: a fixed
// Music/TV/Vinyl/Settings list, registered via
// controller_action_router_set_source_picker_override. Reuses the
// existing zone-picker UI widget and interaction pattern as-is (same
// controller_presentation_*_zone_picker_* calls the Roon zone picker
// uses) - only what populates it and what selecting an entry does are
// different. See
// docs/meta/decisions/2026-09-14_DESIGN_HYBRID_DIAL_UI.md.

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

bool source_picker_open(void);
bool source_picker_select(void);

#ifdef __cplusplus
}
#endif
