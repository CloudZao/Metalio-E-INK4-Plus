#!/usr/bin/env python3
"""Append resources.bin to the app firmware used as the OTA image.

The app image stays a valid ESP image. The resources partition image follows it:

  magic MRES | version u16=1 | header_size u16=16 | payload_size u32 | crc32 u32 | payload

Device boot copies that payload into the resources partition before pictures are loaded.
"""

import argparse
import os
import struct
import sys
import zlib

MAGIC = b"MRES"
HEADER = struct.Struct("<4sHHII")
OTA_LIMIT = 5 * 1024 * 1024
REQUIRED = (
    b"ic_px_book",
    b"ic_px_wallpaper",
    b"ic_px_ai",
    b"ic_px_task",
    b"ic_px_cloud",
    b"ic_px_setting",
)


def image_end(blob: bytes) -> int:
    if len(blob) < 24 or blob[0] != 0xE9:
        raise SystemExit("app image magic is not 0xE9")
    segments = blob[1]
    if segments == 0 or segments > 16:
        raise SystemExit(f"unexpected segment count {segments}")
    off = 24
    for _ in range(segments):
        if off + 8 > len(blob):
            raise SystemExit("truncated app image")
        _load, data_len = struct.unpack_from("<II", blob, off)
        off += 8 + data_len
        if off > len(blob):
            raise SystemExit("segment exceeds app image")
    end = (off + 1 + 15) & ~15
    if blob[23]:
        end += 32
    if end > len(blob):
        raise SystemExit("image end exceeds file")
    return end


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--app", required=True)
    parser.add_argument("--resources", required=True)
    args = parser.parse_args()

    if not os.path.isfile(args.app):
        raise SystemExit(f"missing app image: {args.app}")
    if not os.path.isfile(args.resources):
        raise SystemExit(f"missing resources image: {args.resources}")

    app = open(args.app, "rb").read()
    end = image_end(app)
    if len(app) > end:
        if app[end : end + 4] != MAGIC:
            raise SystemExit(f"{args.app} has {len(app) - end} extra bytes that are not an MRES trailer")
        app = app[:end]
    elif len(app) != end:
        raise SystemExit("app image shorter than its header")

    payload = open(args.resources, "rb").read()
    missing = [name.decode() for name in REQUIRED if name not in payload]
    if missing:
        raise SystemExit("resources.bin missing pixel icons: " + ", ".join(missing))

    crc = zlib.crc32(payload) & 0xFFFFFFFF
    header = HEADER.pack(MAGIC, 1, HEADER.size, len(payload), crc)
    image = app + header + payload
    if len(image) > OTA_LIMIT:
        raise SystemExit(f"OTA image {len(image)} bytes exceeds {OTA_LIMIT}")

    tmp = args.app + ".ota-tmp"
    with open(tmp, "wb") as out:
        out.write(image)
    os.replace(tmp, args.app)

    check = open(args.app, "rb").read()
    check_end = image_end(check)
    if check[check_end : check_end + 4] != MAGIC:
        raise SystemExit("trailer missing after write")
    print(
        f"OTA firmware {args.app}: app {end} + resources {len(payload)} = {len(image)} bytes"
    )


if __name__ == "__main__":
    main()
