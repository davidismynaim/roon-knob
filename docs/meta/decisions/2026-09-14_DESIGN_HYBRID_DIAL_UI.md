# Design: Hybrid Dial UI + Direct Home Assistant Volume Control

**Status:** Accepted (maintainer-approved, this is a personal fork)
**Date:** 2026-09-14
**Issue:** [#1](https://github.com/davidismynaim/roon-knob/issues/1) (tracking), [#2](https://github.com/davidismynaim/roon-knob/issues/2) (first slice: HA volume backend)

> Consolidates and supersedes two prior spec documents kept only in the
> owner's wiki/notes (`dedicated-single-zone-dial-ui-spec.md`, the original
> full custom redesign; `dial-minimal-stock-changes-spec.md`, the
> minimal-change alternative written after running stock firmware). Neither
> is checked into this repo. **This document is what gets built.**

## Context

This is a personal fork of upstream HiPhi Dial, run against a single fixed
physical hi-fi setup (one Roon zone, one DSP volume controller called Nexus,
one Home Assistant instance already acting as the automation hub for that
gear). The owner wants the on-device volume ring to control Nexus directly
through Home Assistant rather than through Roon/UHC's relative-volume
mechanism, plus a reworked Now Playing layout and two new fixed-input
screens (TV, Vinyl) with a repurposed source picker.

## Decision: bypass the controller-boundary rule for volume/source, deliberately

`.oh/controller-boundaries.md` (enforced by issue #190's CI checks) states
the physical-input/device layer must contain no Roon/HQPlayer/Home
Assistant names, entity IDs, or backend action strings — that traffic is
supposed to go through the bridge service (UHC), with a proper adaptive
HA integration path already planned (`.oh/input-bindings.md` "Slice C",
tracked upstream as issue #170 + UHC #333/#335).

This design **knowingly violates that rule**: `idf_app` will call Home
Assistant's REST API directly, with entity IDs (`number.hifi_volume`,
`input_select.audio_input`) and a bearer token embedded in firmware
config, bypassing UHC and Roon entirely for volume and source switching.

**Rationale:** this is a single-owner fork of a single fixed installation,
not the general multi-user product upstream is building toward. Waiting
for the upstream Slice C protocol isn't worth it here. Direct investigation
(owner's wiki §50.3) found two independent reliability problems with the
Roon-mediated path — a Roon Core volume-event bug (since fixed by a Core
restart) and a separately unreliable dial/UHC interaction with Roon's
relative-volume API even with Core healthy — while the HA script-based
mechanism is already proven reliable for this same hardware via the
dashboard, Harmony remote, and voice control. Reusing that proven path is
lower-risk than fixing the Roon-mediated one for a setup of one.

This exception is scoped to volume and source-selection only. It does not
change the boundary rule for anyone building on upstream; it is recorded
here so the deviation is explicit and owned, not accidental drift.

## Volume control backend

**Corrected 2026-09-14 (later same day) after the owner shared the full HA
wiki.** The original version of this section (below, in the implementation
notes) assumed `script.nexus_volume_up`/`nexus_volume_down` send Nexus IR
directly and that the dial should call one of them once per volume step.
That's wrong: per the wiki (§44.9, §45–§47), those two scripts are
**logical-only** — they just increment/decrement the currently-selected
input's `input_number.*` helper by one 0.5 dB position and do not touch
Nexus at all. A separate automation, `Audio - Volume Helper Changed`,
watches those helpers and is the **only** thing that ever sends Nexus IR:
it diffs old vs. new helper value and sends exactly that many
`DirectionUp`/`DirectionDown` pulses in **one** `remote.send_command` call
using Harmony's native `num_repeats` — the same batching mechanism already
proven for Harmony's own held-button case, not something to reimplement.

Looping `nexus_volume_up` N times per rotation dispatch (the earlier plan)
would have produced N separate helper writes and therefore N separate IR
transactions — exactly the "many rapid calls" problem flagged in this
section's original rate-limiting note, self-inflicted rather than
avoided.

- **Read** (for display): `GET http://<ha-host>:8123/api/states/number.hifi_volume`,
  `Authorization: Bearer <long-lived access token>`. Convert from
  `number.hifi_volume`'s native dB scale (-127.5 to 0) to the 0-255
  position scale for display: `position = round((db_value + 127.5) * 2)`.
  Unchanged from the original plan — `number.hifi_volume` already tracks
  whichever input (Music/TV/Vinyl) is currently selected.
- **Write** (for control): `POST http://<ha-host>:8123/api/services/script/audio_voice_volume`,
  body `{"action": "increase"|"decrease", "unit": "clicks", "amount":
  <magnitude>}` (wiki §27.4/§47.2 — "one click is 0.5 dB"). That script
  resolves the selected input's helper itself, clamps 0-255, and writes
  the target in one `input_number.set_value` — which
  `Audio - Volume Helper Changed` then turns into one batched IR send, no
  matter how large `amount` is. The dial does not call
  `nexus_volume_up`/`down` at all, and does not need to loop.
- Roon's Lounge zone is on **Fixed Volume at unity gain** — Roon no longer
  offers any volume interface for this zone, which is fine since nothing
  here depends on Roon's own volume buttons.
- **Resolved 2026-09-15** (was an open follow-up above): the firmware's
  encoder acceleration (`resolve_volume_ticks` in
  `common/controller_input.c`) used to bucket true rotation magnitude
  down to a capped 1/3/5 steps before it ever reached this backend. Per
  the owner's explicit direction, that capping is now removed —
  `resolve_volume_ticks` passes the true accumulated tick count straight
  through uncapped. Since the resulting HA write is one call per rotation
  *dispatch*, and a fast continuous spin can still produce many dispatches
  in quick succession, `ha_volume_client_adjust` now only accumulates
  ticks and updates the display optimistically; a separate debounce task
  flushes the accumulated burst as one `audio_voice_volume` call once
  ~90ms of quiet passes (constant: `HA_VOLUME_DEBOUNCE_MS` in
  `common/ha_volume_client.c`) — the same accumulate-then-flush shape as
  the owner's `Audio - Harmony Volume` automation, tuned much shorter
  since a physical knob should feel closer to instant than a remote's
  held-button repeat. Changing `resolve_volume_ticks` directly (rather
  than behind another opt-in hook) was safe because
  `CONTROLLER_INPUT_TRANSFORM_ROTATION_ACCELERATED` has no live caller
  besides `idf_app/main/controller_input_profile_dial.c` — Frame and RLCD
  bind nothing to it.
  **Confirmed on hardware 2026-09-15:** one physical click moves the
  volume exactly 0.5 dB — `HA_VOLUME_TICKS_PER_CLICK = 1` is correct as
  shipped, no change needed. Also confirmed on hardware: a full slow
  rotation, a fast spin (batches into one smooth jump via the debounce
  flush, no stutter), display staying in sync with HA after repeated
  up/down turns, and clean reboot with no repeat of the netif-timing
  crash below.

- **Crash found and fixed on hardware 2026-09-15:** the first flash with
  HA configured crash-looped on every boot —
  `assert failed: tcpip_send_msg_wait_sem ... Invalid mbox`. Root cause:
  `ha_volume_client_init()` started its poll task unconditionally at
  boot, and that task's first loop iteration fired an HTTP GET (via
  `platform_http_get_auth` → `esp_http_client_open` → `getaddrinfo`)
  before `esp_netif`/lwIP's TCPIP task had been initialized at all, let
  alone connected. Fixed by adding `ha_volume_client_set_network_ready()`,
  mirroring `bridge_client_set_network_ready`'s existing pattern exactly
  and wired into the same `RK_NET_EVT_GOT_IP`/`FAIL`/`AP_STARTED`
  handling in `main_idf.c`. Both the poll task and the debounce-flush
  task gate on it now; a rotation burst that lands before the network is
  ready is kept, not dropped, and sent once ready becomes true.

### Implementation notes (from source investigation)

- `common/bridge_client.c` currently reads volume by scanning UHC's
  `/now_playing` JSON (`bridge_client.c:732-763`) and writes it via
  `platform_http_post_json()` to `<bridge>/control` (`bridge_client.c:1011-1016`,
  `controller_presentation_show_volume_change` at `:1262-1263`). This new
  path runs alongside/replaces that flow for volume specifically — it does
  not need to reuse `bridge_client.c`'s JSON scanning, since HA's response
  shape is different.
- **No existing HTTP call in this codebase sends an `Authorization` header.**
  `idf_app/main/platform_http_idf.c:60-70` sets `Accept`, `Content-Type`,
  `X-Knob-Id`/`X-Knob-Version` only. A bearer-auth GET/POST helper is new
  code, following the existing `esp_http_client_set_header` pattern — small,
  but not a rewire of something that already exists.

## Screen 1: Now Playing (Music input)

### Visual layout

- Concentric rings retained exactly as stock: outer ring = volume position,
  inner ring = playback progress. No change to this visualization.
- Album art fills the full circular display, edge-to-edge, as the
  background for the whole screen.
- No full-screen darkening mask (stock's is removed).
- A darkening tint applies only to the **lower third**, just enough to
  keep track/artist text legible over the artwork — the upper two-thirds
  stay fully undimmed.
- **Volume number**: top third, 0-255 position scale (not dB), large —
  the primary at-a-glance readout.
- **Track/artist details**: lower third, on top of the darkening tint.
- **Transport controls** (previous / play-pause / next): middle third,
  all three the same size, touch targets (touch is reliable per stock
  testing — these stay touch, not rotate/click).
- **Zone selector and current-zone display: removed entirely** — not
  hidden, not defaulted, not present.

### Gestures

- Long-press, top third → activate mute (full-screen red mute icon).
- Long-press, middle third → no action (disabled so it doesn't compete
  with the three transport touch targets in this region).
- Long-press, lower third → open source selection.

## Screen 2: TV and Vinyl inputs

Shared layout, differing only in background image and label text.

### Visual layout

- No inner progress ring (no track/timeline concept for these inputs).
- Outer volume ring retained, same behavior as Music.
- **Volume number**: 0-255 scale, large, top **two-thirds** (more room
  than Music's top-third since there's no now-playing content).
- **Input label** ("TV" or "Vinyl"): small text, bottom third.
- **Background**: full-screen wallpaper (turntable image for Vinyl,
  TV/screen image for TV). Owner supplies real photography later — build
  against placeholders now, but assume a full-bleed photo, not an icon.
- No darkening tint or overlay at all in this mode.

### Gestures

- Long-press, top two-thirds → activate mute (same full-screen treatment).
- Long-press, bottom third → re-open source selection.

## Source selection UI

- Reuse the existing zone-picker interaction pattern as-is:
  `ui_show_zone_picker()` (`common/ui.c:856`, an LVGL `lv_list`) is already
  cleanly separated from the data feeding it — `controller_action_router.c:60-96`
  builds the `names`/`ids` arrays before calling it. Feeding it a static
  3-item Music/TV/Vinyl list instead of live Roon zones is a small,
  low-risk change; the picker's rendering/interaction code needs no
  changes.
- The three fixed options: **Music, TV, Vinyl**, matching
  `input_select.audio_input`'s three states already used elsewhere in this
  setup. No dynamic Roon zone list — this dial is locked to one physical
  setup.
- Selecting an option sets `input_select.audio_input` in HA (reusing the
  existing HA-side input-switching logic) and returns to the appropriate
  screen (Now Playing for Music, the TV/Vinyl screen otherwise).
- `select_picker_entry()` (`controller_action_router.c:113-150`) currently
  mutates Roon zone selection on pick — this needs a parallel code path for
  the HA input-select case rather than reuse of that function's body.

**Implemented and tested on hardware 2026-09-15.** Built as
`controller_action_router_set_source_picker_override()` — an opt-in
open/select hook pair mirroring the volume-override pattern exactly, so
Frame/RLCD's dynamic Roon zone picker is untouched (NULL on those
targets). `common/source_picker_client.c` (Dial-only) implements it: a
static Back/Music/TV/Vinyl/Settings list through the existing
`controller_presentation_*_zone_picker_*` calls, `common/ha_source_client.c`
does the one `input_select.select_option` call per pick. Confirmed on
hardware: all three inputs switch `input_select.audio_input` correctly,
Back/Settings both still work, and the picker highlights the actually-
active input (not just whatever was last picked from the dial itself) —
see the input-tracking note below. TV/Vinyl icons (Material Icons album
U+E019, tv U+E333 — verified by rendering the actual vendored TTF before
committing to the codepoints, not guessed) replace the shared music-note
icon for those two entries; regenerating the bitmap icon font needed
`lv_font_conv` (installed locally, not system-wide) and the real
`MaterialIcons-Regular.ttf` already vendored in `idf_app/spiffs_data/`.

Settings sentinel kept in the reused picker list (owner's direction),
in addition to the existing direct long-press-zone-label-to-Settings
gesture. Selecting TV/Vinyl returns to the current Now Playing layout
unchanged for now (owner's direction) — the distinct TV/Vinyl screens
are their own later slice.

**Input-state tracking (owner-flagged gap, closed same slice).** The
picker only *wrote* `input_select.audio_input` at first — a source
switch made from Harmony, the HA dashboard, or voice would have been
invisible to the dial, mattering once the TV/Vinyl screens need to know
which to show. `ha_volume_client`'s existing poll task (already polling
`number.hifi_volume` every 2s with the network-ready gate) now also
fetches `input_select.audio_input` each cycle — one more small GET on an
existing task rather than a second task+stack — cached behind
`ha_volume_client_get_current_source()`. Confirmed on hardware: switching
input from the dial, then changing it again from the HA dashboard,
correctly updates the picker's highlight within one poll cycle.
Volume itself never had this problem — `number.hifi_volume` already
derives from whichever input's helper is selected, so the existing poll
reflects a cross-device input change automatically.

## Long-press-by-region gesture system (new, not a reuse)

Investigation confirmed **no existing per-region touch long-press
mechanism exists today.** The Dial has no physical buttons
(`platform_input_idf.c:35-37`); `common/controller_button_gesture.c` only
handles tap/double-tap on a physical encoder button, which doesn't apply
here. The only long-press anywhere today is one hardcoded LVGL
`LV_EVENT_LONG_PRESSED` handler on the zone-name label
(`common/ui.c:390,554-556`), which opens Settings.

This design's three-region long-press (top/middle/bottom on Music,
top-two-thirds/bottom-third on TV/Vinyl) is **new interaction code**:
straightforward in LVGL (stack invisible full-width objects per region,
attach `LV_EVENT_LONG_PRESSED` to each), but budget it as new work, not a
rewire.

LVGL's default long-press threshold (~400ms) is not overridden anywhere
in this repo today and `lv_conf.h` isn't vendored locally to confirm the
exact value at build time. Use the existing zone-label long-press as the
feel reference; don't introduce a different threshold without checking
`lv_conf.h` at build time first.

## Mute behavior

- Full-screen red mute icon, matching the original design document's
  treatment as an unambiguous, unmissable full-screen state.
- Confirmed **net-new** — no existing mute mechanism anywhere in the repo
  (`input_boolean.audio_mute` or otherwise).
- Exact mechanism (which HA entity/service) carries forward from the
  project's standard mute toggle (`input_boolean.audio_mute`) unless
  building it surfaces a reason to deviate.

## Idle timeout / sleep behavior

**No change.** `display_activity_detected()`
(`idf_app/main/display_sleep.c:516`) is invoked generically from the touch
and encoder input paths (`platform_display_idf.c:369,387`,
`platform_input_idf.c:260`), not gated on which screen is active. New
screens built from normal LVGL touch objects will keep tripping this path
unchanged — no special-casing needed for Music vs. TV/Vinyl. Default: TV
and Vinyl screens get the same idle/sleep behavior as Music, for free,
since the mechanism is screen-agnostic. Revisit only if that feels wrong
once it's running on hardware.

## Open questions carried into implementation

1. ~~Locate existing Roon-volume read/write code~~ — answered above.
2. ~~Confirm zone-selection UI mechanism~~ — answered above
   (`ui_show_zone_picker`, LVGL list).
3. Exact long-press duration threshold — LVGL default, not confirmed
   in-repo; check `lv_conf.h` at build time before tuning.
4. ~~Does stock have idle/sleep for TV/Vinyl~~ — moot; mechanism is
   screen-agnostic, so TV/Vinyl inherit it automatically. Default decided
   above.
5. Exact pixel boundaries for thirds/two-thirds against the round display's
   circular usable area — **no existing helper for this.** `common/ui.c`
   lays out everything with fixed, eyeballed pixel offsets against
   `SCREEN_SIZE` (360 for Dial). Thirds boundaries will need to be tuned
   by eye on real hardware, same as the rest of this file — not derived
   from a formula. Budget an iteration pass on-device.
6. Confirm HA long-lived access token storage/provisioning mechanism on
   the device (NVS alongside existing WiFi/bridge config, presumably) —
   not yet designed.

## Implementation order

Smallest independently-testable slices first, each shippable on its own:

1. **HA volume backend** — bearer-auth HTTP helper, GET/POST to the two
   endpoints above, dB↔position conversion, wired into the existing
   volume ring display and encoder rotation handler. Testable in isolation
   against real HA before touching any screen layout.
2. **Source picker reuse** — static 3-item list into
   `ui_show_zone_picker()`, writes `input_select.audio_input`.
3. **Long-press-by-region gesture system** — generic enough to serve both
   mute-trigger and source-select-open on both screen layouts.
4. **Mute** — full-screen icon + HA toggle, using the gesture system from
   step 3.
5. **Now Playing layout rework** — tint, thirds, zone-selector removal.
6. **TV/Vinyl screens** — new layout, placeholder art, wired to steps 1-4.

Each slice gets its own branch/PR per the project's git workflow, kept
draft until tested on hardware.
