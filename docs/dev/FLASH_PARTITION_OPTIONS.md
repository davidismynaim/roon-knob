# Flash Partition Options (parked)

Nothing here is implemented. It records the current headroom and two ways to
get more, for when a feature needs it.

## Where things stand (measured 2026-09-24, Dial build)

Flash is 16 MB (`CONFIG_ESPTOOLPY_FLASHSIZE="16MB"`). The real layout is
`idf_app/partitions.csv` (the layout table in
[OTA_UPDATES.md](../usage/OTA_UPDATES.md) is out of date):

| Partition | Offset | Size | Used by |
|-----------|--------|------|---------|
| nvs | 0x9000 | 16 KB | Settings blobs |
| otadata | 0xd000 | 8 KB | Which app slot boots |
| phy_init | 0xf000 | 4 KB | WiFi PHY calibration |
| factory | 0x10000 | 2.5 MiB | **Where every USB flash writes the app** |
| ota_0 | 0x290000 | 2.5 MiB | OTA only - empty if you only flash by USB |
| ota_1 | 0x510000 | 2.5 MiB | OTA only - empty if you only flash by USB |

- App image: ~2.19 MiB of a 2.5 MiB slot, so **~318 KiB (12%) free**.
- Partitions end at 0x790000, so **~8.4 MiB of flash is unallocated**, and the
  two OTA slots (5 MiB) are idle in a USB-only workflow.
- Code executes from flash, so app size is a flash question, not a RAM one.
- Internal RAM (DIRAM): ~59% used, ~138 KiB free, fixed by the chip and the
  tighter constraint at runtime (WiFi/TLS/HTTP need it, and it is fragmented).
  PSRAM: 8 MB, ~7.4 MB free - put large buffers (artwork, JSON) there.
- `CONFIG_ESP_WIFI_IRAM_OPT` and `CONFIG_ESP_WIFI_RX_IRAM_OPT` are on; turning
  them off should return some internal RAM at the cost of WiFi speed. Not
  measured.

Small features fit as-is. Anything that embeds images/fonts, or adds a large
client, should move data out of the app image or grow the partitions first.

## Why three app slots

`factory` is what USB flashing writes. `ota_0`/`ota_1` are the pair OTA
alternates between (write the inactive one, switch, keep the old as a
fallback). `factory` is the permanent known-good image OTA never overwrites.
Each USB flash also writes `ota_data_initial.bin` to 0xd000, which resets boot
selection to `factory`.

## Option A - single large app slot, OTA disabled

Only sensible if OTA is never used (dev builds are `-dev`, which skips the
auto-check; see `ota_check_for_update()`).

- `partitions.csv`: keep nvs/otadata/phy_init; make `factory` large (e.g.
  8 MiB); drop `ota_0`/`ota_1`; optionally add a data partition for assets in
  the remaining ~7 MiB so images/fonts can leave the app image.
- Headroom: ~318 KiB -> roughly 5+ MiB.
- OTA needs no code to fail safely: `esp_ota_get_next_update_partition()` returns
  NULL and the updater reports "No OTA partition". Hide the update UI/auto-check
  for a cleaner result.
- No brick risk: ESP32-S3 ROM USB download mode always allows a reflash.
- Cost: diverges from the upstream layout (fine for this fork), and no OTA.

## Option B - keep three slots, grow each to 4 MiB

Stays compatible with the upstream OTA design.

- `partitions.csv`: `factory`/`ota_0`/`ota_1` at 4 MiB each (offsets
  0x10000, 0x410000, 0x810000; ends 0xC10000).
- Headroom: ~318 KiB -> ~1.8 MiB (5 MiB slots would give ~2.8 MiB and is the
  practical maximum for three slots).

## Applying either option

- The partition table cannot be changed over the air: **each dial needs one
  USB flash** of bootloader + partition table + otadata + app (the same
  `esptool` offsets `docker.yml` and the web flasher already use).
- CI and the web flasher package whatever `partitions.csv` says, so no workflow
  change is needed.
- Delete `sdkconfig` when changing anything in `sdkconfig.defaults` (see
  [KCONFIG.md](KCONFIG.md)); the partition CSV itself is read at build time.
- Update `docs/usage/OTA_UPDATES.md`'s layout table at the same time.
- Every dial (Lounge, Dining, any future one) must be reflashed with the same
  layout.
