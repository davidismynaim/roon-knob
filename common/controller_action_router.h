#pragma once

#include "controller_action.h"

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

void controller_action_router_init(void);
bool controller_action_router_handle(const controller_action_t *action);

/*
 * Target-registered override for ADJUST_VOLUME_STEPS: when set, volume
 * commands route here instead of bridge_client_execute_command (Roon/UHC).
 * Unset on every target except idf_app/Dial, where main_idf.c registers
 * ha_volume_client_adjust — see
 * docs/meta/decisions/2026-09-14_DESIGN_HYBRID_DIAL_UI.md. `steps` matches
 * controller_command_t.volume_steps (signed, positive = up).
 */
typedef bool (*controller_volume_override_fn_t)(int32_t steps);
void controller_action_router_set_volume_override(
    controller_volume_override_fn_t fn);

/*
 * Target-registered override for the zone-picker OPEN/SELECT actions: when
 * both are set, they replace the default dynamic Roon zone list
 * (open_picker/select_picker_entry, bridge_client_visit_zones /
 * bridge_client_select_zone_value) entirely. Unset on every target except
 * idf_app/Dial, where main_idf.c registers a fixed Music/TV/Vinyl picker
 * writing input_select.audio_input via Home Assistant instead of a Roon
 * zone — see docs/meta/decisions/2026-09-14_DESIGN_HYBRID_DIAL_UI.md.
 * Registering only one of the two is treated as not registering either.
 */
typedef bool (*controller_source_picker_open_fn_t)(void);
typedef bool (*controller_source_picker_select_fn_t)(void);
void controller_action_router_set_source_picker_override(
    controller_source_picker_open_fn_t open_fn,
    controller_source_picker_select_fn_t select_fn);

#ifdef __cplusplus
}
#endif
