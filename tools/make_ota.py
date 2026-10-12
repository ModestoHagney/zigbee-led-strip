#!/usr/bin/env python3
"""Wrap a built firmware as a Zigbee update file for Home Assistant (ZHA).

    python tools/make_ota.py                  # uses build/zigbee_led_strip.bin
    python tools/make_ota.py path/to/app.bin

The version and manufacturer come from main/config.h and the image type
from the chip the firmware was built for, so the file always matches the
build. Raise FW_VERSION in main/config.h and rebuild before running this.
"""

import re
import struct
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent

# chip id in the firmware header -> (OTA_IMAGE_TYPE in main/config.h, name)
CHIPS = {0x0010: (0x0001, "h2"), 0x000D: (0x0002, "c6")}


def config(name: str) -> int:
    text = (ROOT / "main/config.h").read_text()
    return int(re.search(rf"^#define\s+{name}\s+(\w+)", text, re.M).group(1), 0)


def make_ota(firmware: bytes, version: int, manufacturer: int) -> tuple[bytes, str]:
    if len(firmware) < 24 or firmware[0] != 0xE9:
        sys.exit("that is not an ESP firmware .bin")
    chip_id = struct.unpack_from("<H", firmware, 12)[0]
    if chip_id not in CHIPS:
        sys.exit(f"firmware is for a chip this project has no board for (id {chip_id:#x})")
    image_type, chip = CHIPS[chip_id]

    # one element: tag 0 (upgrade image), length, data
    element = struct.pack("<HI", 0, len(firmware)) + firmware
    header = struct.pack(
        "<IHHHHHIH32sI",
        0x0BEEF11E,  # Zigbee OTA file identifier
        0x0100,  # header version
        56,  # header length
        0,  # field control: no optional fields
        manufacturer,
        image_type,
        version,
        2,  # Zigbee stack version: Zigbee Pro
        b"NiftyFish LED strip",
        56 + len(element),
    )
    return header + element, chip


if __name__ == "__main__":
    source = Path(sys.argv[1]) if len(sys.argv) > 1 else ROOT / "build/zigbee_led_strip.bin"
    version = config("FW_VERSION")
    data, chip = make_ota(source.read_bytes(), version, config("OTA_MANUFACTURER"))
    target = source.with_name(f"niftyfish-{chip}-led-v{version}.ota")
    target.write_bytes(data)
    print(f"{target}  ({chip.upper()} board, version {version}, {len(data)} bytes)")
