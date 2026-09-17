#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    CONTROLLER_COMMAND_NONE = 0,
    CONTROLLER_COMMAND_TOGGLE_PLAYBACK,
    CONTROLLER_COMMAND_NEXT_TRACK,
    CONTROLLER_COMMAND_PREVIOUS_TRACK,
    CONTROLLER_COMMAND_ADJUST_VOLUME_STEPS,
    CONTROLLER_COMMAND_SEEK_TO_SECONDS,
} controller_command_kind_t;

typedef struct {
    controller_command_kind_t kind;
    int32_t volume_steps;
    /* Absolute track position in seconds, valid only for
     * CONTROLLER_COMMAND_SEEK_TO_SECONDS - see common/ui.c's detail-screen
     * seek preview (encoder jog, committed after a short idle debounce). A
     * separate field rather than reusing volume_steps: the two commands
     * are never active at once, but a position in seconds stored under a
     * field named "volume_steps" would mislead the next reader. */
    int32_t seek_seconds;
} controller_command_t;

static inline controller_command_t controller_command_make(
    controller_command_kind_t kind) {
    controller_command_t command = {
        .kind = kind,
        .volume_steps = 0,
        .seek_seconds = 0,
    };
    return command;
}

static inline controller_command_t controller_command_adjust_volume(
    int32_t signed_steps) {
    controller_command_t command = {
        .kind = CONTROLLER_COMMAND_ADJUST_VOLUME_STEPS,
        .volume_steps = signed_steps,
        .seek_seconds = 0,
    };
    return command;
}

static inline controller_command_t controller_command_seek_to(
    int32_t position_seconds) {
    controller_command_t command = {
        .kind = CONTROLLER_COMMAND_SEEK_TO_SECONDS,
        .volume_steps = 0,
        .seek_seconds = position_seconds,
    };
    return command;
}

#if defined(__cplusplus)
static_assert(sizeof(controller_command_t) <= 12,
              "controller command exceeds its ESP32 budget");
#else
_Static_assert(sizeof(controller_command_t) <= 12,
               "controller command exceeds its ESP32 budget");
#endif

#ifdef __cplusplus
}
#endif
