#pragma once

#include "controller_command.h"

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    CONTROLLER_SYSTEM_ACTION_NONE = 0,
    CONTROLLER_SYSTEM_ACTION_START_PROVISIONING,
    CONTROLLER_SYSTEM_ACTION_RESTART,
} controller_system_action_t;

typedef enum {
    CONTROLLER_ACTION_NONE = 0,
    CONTROLLER_ACTION_COMMAND,
    CONTROLLER_ACTION_OPEN_ZONE_PICKER,
    CONTROLLER_ACTION_CLOSE_ZONE_PICKER,
    CONTROLLER_ACTION_SCROLL_ZONE_PICKER,
    CONTROLLER_ACTION_SELECT_ZONE_PICKER,
    CONTROLLER_ACTION_SHOW_SETTINGS,
    CONTROLLER_ACTION_TOGGLE_MUTE,
    /* Rotation-driven, preview-only - like CONTROLLER_ACTION_SCROLL_ZONE_PICKER,
     * this never touches the network by itself. The dial's detail-screen seek
     * preview (common/ui.c's ui_seek_adjust) accumulates these locally and
     * only fires a CONTROLLER_COMMAND_SEEK_TO_SECONDS command once the user
     * stops turning. Only bound in CONTROLLER_INTERACTION_CONTEXT_SEEK. */
    CONTROLLER_ACTION_ADJUST_SEEK,
    CONTROLLER_ACTION_SYSTEM,
    /*
     * Reserved for #170/#194 Slice C. Slice A has no adaptive action table,
     * so the router must reject this kind.
     */
    CONTROLLER_ACTION_ADAPTIVE_REF,
} controller_action_kind_t;

typedef struct {
    controller_action_kind_t kind;
    union {
        controller_command_t command;
        int32_t picker_delta;
        int32_t seek_ticks;
        controller_system_action_t system;
        uint32_t adaptive_ref;
    } value;
} controller_action_t;

static inline controller_action_t controller_action_none(void) {
    controller_action_t action = {
        .kind = CONTROLLER_ACTION_NONE,
        .value.command = controller_command_make(CONTROLLER_COMMAND_NONE),
    };
    return action;
}

static inline controller_action_t controller_action_command(
    controller_command_t command) {
    controller_action_t action = {
        .kind = CONTROLLER_ACTION_COMMAND,
        .value.command = command,
    };
    return action;
}

static inline controller_action_t controller_action_simple(
    controller_action_kind_t kind) {
    controller_action_t action = {
        .kind = kind,
        .value.picker_delta = 0,
    };
    return action;
}

static inline controller_action_t controller_action_picker_scroll(int delta) {
    controller_action_t action = {
        .kind = CONTROLLER_ACTION_SCROLL_ZONE_PICKER,
        .value.picker_delta = delta,
    };
    return action;
}

static inline controller_action_t controller_action_adjust_seek(int32_t ticks) {
    controller_action_t action = {
        .kind = CONTROLLER_ACTION_ADJUST_SEEK,
        .value.seek_ticks = ticks,
    };
    return action;
}

static inline controller_action_t controller_action_system(
    controller_system_action_t system) {
    controller_action_t action = {
        .kind = CONTROLLER_ACTION_SYSTEM,
        .value.system = system,
    };
    return action;
}

#if defined(__cplusplus)
static_assert(sizeof(controller_action_t) <= 16,
              "controller action exceeds its ESP32 budget");
#else
_Static_assert(sizeof(controller_action_t) <= 16,
               "controller action exceeds its ESP32 budget");
#endif

#ifdef __cplusplus
}
#endif
