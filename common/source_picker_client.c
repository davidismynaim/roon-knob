#include "source_picker_client.h"

#include "controller_input.h"
#include "controller_presentation.h"
#include "ha_source_client.h"
#include "ha_volume_client.h"

#include <string.h>

#define SOURCE_ID_BACK "__back__"
#define SOURCE_ID_SETTINGS "__settings__"

static const char *s_names[] = {"Back", "Music", "TV", "Vinyl", "Settings"};
static const char *s_ids[] = {SOURCE_ID_BACK, "Music", "TV", "Vinyl",
                              SOURCE_ID_SETTINGS};
#define SOURCE_COUNT 5

bool source_picker_open(void) {
    /* Reflects whatever ha_volume_client last polled for
     * input_select.audio_input - which may have been changed from
     * Harmony, the HA dashboard, or voice, not just this picker - so the
     * highlight is only ever stale by up to one poll interval, not
     * permanently wrong. Falls back to Back (index 0) if never
     * successfully polled yet or the value doesn't match one of the
     * three known options. */
    int selected = 0;
    char current[16] = {0};
    if (ha_volume_client_get_current_source(current, sizeof(current))) {
        for (int i = 1; i < SOURCE_COUNT - 1; ++i) {
            if (strcmp(s_ids[i], current) == 0) {
                selected = i;
                break;
            }
        }
    }
    controller_presentation_show_zone_picker(s_names, s_ids, SOURCE_COUNT,
                                             selected);
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

    /* Music/TV/Vinyl. common/ui.c's apply_current_screen() switches to the
     * matching screen based on ha_volume_client_get_current_source() - set
     * it optimistically here so picking from this dial's own picker feels
     * instant instead of waiting up to one poll interval (2-30s,
     * depending on charging/sleep state - see ha_volume_client.c's
     * poll_interval_ms()) for confirmation; the next poll reconciles it
     * either way. */
    bool ok = ha_source_client_select(selected_id);
    if (ok) {
        ha_volume_client_set_current_source_optimistic(selected_id);
    }
    controller_presentation_hide_zone_picker();
    (void)controller_input_set_context(CONTROLLER_INTERACTION_CONTEXT_MEDIA);
    return ok;
}
