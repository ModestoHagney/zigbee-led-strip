#!/usr/bin/env python3
"""Write index.json for a folder of .ota files, in the format ZHA reads.

    python tools/make_index.py FOLDER BASE_URL

BASE_URL is where the folder will be reachable on the web. Home
Assistant downloads index.json from there, and each file listed in it.
"""

import hashlib
import json
import struct
import sys
from pathlib import Path


def entry(path: Path, base_url: str) -> dict:
    data = path.read_bytes()
    # start of the Zigbee OTA header, see make_ota.py
    magic, _, _, _, manufacturer, image_type, version = struct.unpack_from("<IHHHHHI", data)
    if magic != 0x0BEEF11E:
        sys.exit(f"{path.name} is not a Zigbee update file")
    return {
        "fileName": path.name,
        "fileVersion": version,
        "fileSize": len(data),
        "manufacturerCode": manufacturer,
        "imageType": image_type,
        "sha512": hashlib.sha512(data).hexdigest(),
        "url": f"{base_url.rstrip('/')}/{path.name}",
    }


if __name__ == "__main__":
    if len(sys.argv) != 3:
        sys.exit(__doc__)
    folder = Path(sys.argv[1])
    index = [entry(path, sys.argv[2]) for path in sorted(folder.glob("*.ota"))]
    (folder / "index.json").write_text(json.dumps(index, indent=2) + "\n")
    for item in index:
        print(f"{item['fileName']}: image type {item['imageType']}, version {item['fileVersion']}")
