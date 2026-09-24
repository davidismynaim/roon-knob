#pragma once

// Vinyl-recognizer feed for the Dial's Vinyl screen (Lounge and Dining Room). A service on
// the same host as Home Assistant identifies what the record player is playing
// and serves it in the same shape UHC serves Roon's now-playing, so the Music
// screen can show it. While Vinyl is the selected source and the service has a
// track, that track replaces Roon's on the Music screen; with no track (or no
// service) the static Vinyl picture stays, exactly as before.
//
// Dial-only: compiled into idf_app alone.

#include "controller_command.h"

#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

// Called once per Home Assistant poll cycle (ha_volume_client.c's poll task,
// so no extra task or stack). `ha_host` is the configured "host:port" of Home
// Assistant - the service is assumed to run on the same host: port 8099 for the
// Lounge, 8100 for the Dining Room (each with its own room's microphone).
void vinyl_client_poll(const char *ha_host, bool source_is_vinyl);

// True while a recognised track is available from the service.
bool vinyl_client_showing(void);

// True while the vinyl track's text/artwork have been handed to the UI, i.e. it
// is safe to switch the screen to the Music layout without a flash of stale
// content. False again the moment the feed goes off.
bool vinyl_client_content_ready(void);

// True whenever this dial is on the Vinyl source (either room), with or without
// a recognised track. Roon's media data must never reach the screen then: with
// no track the static picture is shown, not whatever Roon last had.
bool vinyl_client_owns_media(void);

// Artwork request for the current track, same query shape UHC's
// /now_playing/image takes. False if no service host is known.
bool vinyl_client_artwork_url(char *url, size_t len, int width, int height);

// Command filter for controller_action_router: transport commands (play/pause,
// next/previous, seek) have nothing to act on for vinyl, so they are consumed
// while Vinyl is the source. Volume is untouched.
bool vinyl_client_swallow_command(const controller_command_t *command);

// Host part of "host:port" (no scheme), for building the service address.
// False if `ha_host` is empty or does not fit.
bool vinyl_client_host_from_ha(const char *ha_host, char *out, size_t len);

#ifdef __cplusplus
}
#endif
