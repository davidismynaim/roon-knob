#pragma once

// Vinyl-recognizer feed for the Dial's Vinyl screen (Lounge only). A service on
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
// Assistant - the service is assumed to run on the same host, port 8099.
void vinyl_client_poll(const char *ha_host, bool source_is_vinyl);

// True while a recognised track is being shown in place of Roon's.
bool vinyl_client_showing(void);

// Artwork request for the current track, same query shape UHC's
// /now_playing/image takes. False if no service host is known.
bool vinyl_client_artwork_url(char *url, size_t len, int width, int height);

// Command filter for controller_action_router: transport commands (play/pause,
// next/previous, seek) have nothing to act on for vinyl, so they are consumed
// while a track is shown. Volume is untouched.
bool vinyl_client_swallow_command(const controller_command_t *command);

// Host part of "host:port" (no scheme), for building the service address.
// False if `ha_host` is empty or does not fit.
bool vinyl_client_host_from_ha(const char *ha_host, char *out, size_t len);

#ifdef __cplusplus
}
#endif
