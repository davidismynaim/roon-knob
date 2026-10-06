#!/usr/bin/env python3
"""Converts the streaming-platform icons (dial#57) to embedded
RGB565A8 C arrays - RGB565 color plane followed by an 8bpp alpha plane,
LVGL's standard format for a color image that needs real transparency
(CONFIG_LV_DRAW_SW_SUPPORT_RGB565A8=y, already enabled in sdkconfig).
Plain RGB565 (the TV/Vinyl background photos' format - see
convert_tv_vinyl_images.py) has no alpha channel at all, which doesn't
work here: these icons are circular badges that need to show through to
whatever's behind the button outside that circle, not sit on a faked-in
square backdrop.

Source icons live in idf_app/source_images/streaming/ (netflix.png,
prime.png, appletv.png, disney.png, iplayer.png, itvx.png, channel4.png,
channel5.png) - already circular with transparent corners (owner-sourced
from each service's own app-icon artwork - the first 4 from each
service's own app-icon/press image, the UK broadcaster 4 from their
Google Play Store listings - cropped/masked to a matching size; see
dial#57's PR for provenance of each). Replace them there and rerun this
script to update the on-device assets.

Run from idf_app directory:
    python3 scripts/convert_streaming_icons.py

Stored at 64x64 (not the 80px button size itself - 8 buttons share one
ring now, shrunk from 88px/72px icon to fit all 8 at 45 degrees apart
without touching) so the icon reads as a badge inset within the button's
own border, matching the directional ring's visual convention (dial#55)
of icon-smaller-than-button rather than edge-to-edge. 64x64 RGB565A8 is
3 bytes/pixel * 4096px = 12KB per icon, ~96KB for all 8 - a deliberate
size/flash tradeoff, not the source images' own native resolution.
"""
from pathlib import Path

from PIL import Image

SRC_DIR = Path(__file__).resolve().parent.parent / "source_images" / "streaming"
OUT_DIR = Path(__file__).resolve().parent.parent / "main" / "images"
SIZE = 64

ICONS = {
    "netflix.png": "icon_netflix",
    "prime.png": "icon_prime",
    "appletv.png": "icon_appletv",
    "disney.png": "icon_disney",
    "iplayer.png": "icon_iplayer",
    "itvx.png": "icon_itvx",
    "channel4.png": "icon_channel4",
    "channel5.png": "icon_channel5",
}


def convert(src_path: Path, var_name: str) -> None:
    img = Image.open(src_path).convert("RGBA")
    if img.size != (SIZE, SIZE):
        img = img.resize((SIZE, SIZE), Image.LANCZOS)

    pixels = img.load()
    color_data = bytearray()
    alpha_data = bytearray()
    for y in range(SIZE):
        for x in range(SIZE):
            r, g, b, a = pixels[x, y]
            rgb565 = ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3)
            # Little-endian (low byte first) - matches LVGL's
            # LV_COLOR_FORMAT_RGB565 convention on little-endian MCUs,
            # same as convert_tv_vinyl_images.py.
            color_data.append(rgb565 & 0xFF)
            color_data.append((rgb565 >> 8) & 0xFF)
            alpha_data.append(a)

    data = color_data + alpha_data

    OUT_DIR.mkdir(parents=True, exist_ok=True)
    out_path = OUT_DIR / f"{var_name}.c"
    with open(out_path, "w") as f:
        f.write('#include "lvgl.h"\n\n')
        f.write(f"static const uint8_t {var_name}_map[] = {{\n")
        for i in range(0, len(data), 16):
            chunk = data[i:i + 16]
            f.write("    " + ",".join(f"0x{b:02x}" for b in chunk) + ",\n")
        f.write("};\n\n")
        f.write(f"const lv_image_dsc_t {var_name} = {{\n")
        f.write("    .header = {\n")
        f.write("        .magic = LV_IMAGE_HEADER_MAGIC,\n")
        f.write("        .cf = LV_COLOR_FORMAT_RGB565A8,\n")
        f.write(f"        .w = {SIZE},\n")
        f.write(f"        .h = {SIZE},\n")
        f.write("    },\n")
        f.write(f"    .data_size = sizeof({var_name}_map),\n")
        f.write(f"    .data = {var_name}_map,\n")
        f.write("};\n")

    print(f"Wrote {out_path} ({len(data)} bytes of pixel data)")


def main() -> None:
    for fname, var_name in ICONS.items():
        convert(SRC_DIR / fname, var_name)


if __name__ == "__main__":
    main()
