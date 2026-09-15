#include "source_picker_client.h"

#include "controller_input.h"
#include "controller_presentation.h"
#include "ha_source_client.h"

#include <string.h>

#define SOURCE_ID_BACK "__back__"
#define SOURCE_ID_SETTINGS "__settings__"

static const char *s_names[] = {"Back", "Music", "TV", "Vinyl", "Settings"};
static const char *s_ids[] = {SOURCE_ID_BACK, "Music", "TV", "Vinyl",
                              SOURCE_ID_SETTINGS};
#define SOURCE_COUNT 5

bool source_picker_open(void) {
    /* Not yet tracking which input is actually active (that would need
     * its own HA poll, like ha_volume_client's for volume) - always
     * highlights Back rather than guessing at Music/TV/Vinyl. Revisit if
     * that's confusing in practice once the TV/Vinyl screens land. */
    controller_presentation_show_zone_picker(s_names, s_ids, SOURCE_COUNT,
                                             0);
    return controller_input_set_context(
        CONTROLLER_INTERACTION_CONTEXT_ZONE_PICKER);
}

bool source_picker_select(void) {
    char selected_id[32] = {0};
    controller_presentation_zone_picker_get_selected_id(selected_id,
                                                         sizeof(selected_id));

    if (selected_id[0] == '\0' ||
        strcmp(selected_id, SOURCE_ID_BACK) == 0) {
        controller_presentation_hide_zone_picker();
        return controller_input_set_context(
            CONTROLLER_INTERACTION_CONTEXT_MEDIA);
    }

    if (strcmp(selected_id, SOURCE_ID_SETTINGS) == 0) {
        controller_presentation_hide_zone_picker();
        controller_presentation_show_settings();
        return controller_input_set_context(
            CONTROLLER_INTERACTION_CONTEXT_SETTINGS_RECOVERY);
    }

    /* Music/TV/Vinyl. Stays on the current (Now Playing) layout either
     * way for now - the distinct TV/Vinyl screens are a later slice. */
    bool ok = ha_source_client_select(selected_id);
    controller_presentation_hide_zone_picker();
    (void)controller_input_set_context(CONTROLLER_INTERACTION_CONTEXT_MEDIA);
    return ok;
}
