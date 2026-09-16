#!/usr/bin/env python3
"""Converts the TV/Vinyl background photos to embedded RGB565 C arrays.

Source photos live in idf_app/source_images/ (tv.jpg, vinyl.jpg) - replace
them there and rerun this script to update the on-device backgrounds.

Run from idf_app directory:
    python3 scripts/convert_tv_vinyl_images.py source_images/tv.jpg source_images/vinyl.jpg

Stored at half resolution (180x180, not the display's native 360x360) and
scaled up 2x at runtime via lv_image_set_scale() - full-res RGB565 for two
360x360 images is ~518KB, which doesn't fit the available flash headroom.
At 180x180 each image is ~65KB (130KB combined), comfortably within
budget. This is a placeholder-quality tradeoff (a linear 2x upscale is
visibly softer than native res); revisit with on-device JPEG decode if
that turns out to matter more than the flash budget once seen on
hardware.
"""
import sys
from pathlib import Path

from PIL import Image

OUT_DIR = Path(__file__).resolve().parent.parent / "main" / "images"
SIZE = 180


def convert(src_path: str, var_name: str) -> None:
    img = Image.open(src_path).convert("RGB")
    # Both source photos are already 360x360 (square); center-crop to
    # square defensively in case a future replacement photo isn't.
    w, h = img.size
    if w != h:
        side = min(w, h)
        left = (w - side) // 2
        top = (h - side) // 2
        img = img.crop((left, top, left + side, top + side))
    img = img.resize((SIZE, SIZE), Image.LANCZOS)

    pixels = img.load()
    data = bytearray()
    for y in range(SIZE):
        for x in range(SIZE):
            r, g, b = pixels[x, y]
            rgb565 = ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3)
            # Little-endian (low byte first) - matches LVGL's
            # LV_COLOR_FORMAT_RGB565 convention on little-endian MCUs.
            data.append(rgb565 & 0xFF)
            data.append((rgb565 >> 8) & 0xFF)

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
        f.write("        .cf = LV_COLOR_FORMAT_RGB565,\n")
        f.write(f"        .w = {SIZE},\n")
        f.write(f"        .h = {SIZE},\n")
        f.write("    },\n")
        f.write(f"    .data_size = sizeof({var_name}_map),\n")
        f.write(f"    .data = {var_name}_map,\n")
        f.write("};\n")

    print(f"Wrote {out_path} ({len(data)} bytes of pixel data)")


def main() -> None:
    if len(sys.argv) != 3:
        print(f"Usage: {sys.argv[0]} <tv.jpg> <vinyl.jpg>", file=sys.stderr)
        sys.exit(1)
    convert(sys.argv[1], "tv_background")
    convert(sys.argv[2], "vinyl_background")


if __name__ == "__main__":
    main()
