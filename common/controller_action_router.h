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

#ifdef __cplusplus
}
#endif
