"""Verify an SDK-produced raw ESP32-S31 flash image against flasher_args.json.

This checker never builds, merges, patches, flashes, or downloads anything.
"""
from __future__ import annotations

import argparse
import hashlib
import json
from pathlib import Path
import sys

FLASH_LIMIT = 16 * 1024 * 1024
BOOT_OFFSET = 0x2000
S31_CHIP_ID = 32


def unique_object(items: list[tuple[str, object]]) -> dict:
    result = {}
    for key, value in items:
        if key in result:
            raise ValueError(f"Duplicate JSON key: {key}")
        result[key] = value
    return result


def digest(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def verify(build: Path, binary: Path) -> dict:
    build = build.resolve(strict=True)
    binary = binary.resolve(strict=True)
    manifest_path = build / "flasher_args.json"
    manifest_bytes = manifest_path.read_bytes()
    manifest = json.loads(manifest_bytes, object_pairs_hook=unique_object)
    if not isinstance(manifest, dict):
        raise ValueError("flasher_args.json must be a JSON object")
    extra = manifest.get("extra_esptool_args")
    if not isinstance(extra, dict) or extra.get("chip") != "esp32s31":
        raise ValueError("flasher_args.json does not declare esp32s31")
    declared = manifest.get("flash_files")
    if not isinstance(declared, dict) or not declared:
        raise ValueError("flasher_args.json has no flash_files mapping")
    image = binary.read_bytes()
    if len(image) <= BOOT_OFFSET + 24 or len(image) > FLASH_LIMIT:
        raise ValueError("Full image must contain the S31 bootloader and fit in 16 MiB")

    segments = []
    seen_offsets = set()
    for offset_text, source_text in declared.items():
        try:
            offset = int(offset_text, 0)
        except (TypeError, ValueError) as exc:
            raise ValueError(f"Invalid flash offset: {offset_text!r}") from exc
        if offset < 0 or offset in seen_offsets:
            raise ValueError(f"Negative or duplicate numeric offset: {offset_text}")
        seen_offsets.add(offset)
        if not isinstance(source_text, str) or not source_text:
            raise ValueError(f"Invalid source filename at {offset_text}")
        source = Path(source_text)
        if not source.is_absolute():
            source = build / source
        source = source.resolve(strict=True)
        if source == binary:
            raise ValueError("Merged output is also declared as an input segment")
        data = source.read_bytes()
        if not data:
            raise ValueError(f"Empty declared segment: {source}")
        if data.startswith(b"version https://git-lfs.github.com/spec/v1"):
            raise ValueError(f"Unresolved LFS pointer: {source}")
        end = offset + len(data)
        if end > FLASH_LIMIT or end > len(image):
            raise ValueError(f"Segment exceeds flash or merged image: {source}")
        if image[offset:end] != data:
            raise ValueError(f"Merged bytes differ from declared source at {offset_text}: {source}")
        segments.append({"offset": offset, "offset_hex": hex(offset), "end": end,
                         "source": str(source), "bytes": len(data), "sha256": digest(data)})

    segments.sort(key=lambda segment: segment["offset"])
    if segments[0]["offset"] != BOOT_OFFSET:
        raise ValueError("First declared S31 segment is not the bootloader at 0x2000")
    cursor = 0
    for segment in segments:
        if segment["offset"] < cursor:
            raise ValueError(f"Overlapping flash segments at {segment['offset_hex']}")
        if image[cursor:segment["offset"]].strip(b"\xff"):
            raise ValueError(f"Undeclared non-FF bytes before {segment['offset_hex']}")
        cursor = segment["end"]
    if image[cursor:].strip(b"\xff"):
        raise ValueError("Undeclared non-FF bytes after the last segment")
    if image[BOOT_OFFSET] != 0xE9:
        raise ValueError("Bootloader image magic at 0x2000 is invalid")
    chip_id = int.from_bytes(image[BOOT_OFFSET + 12:BOOT_OFFSET + 14], "little")
    if chip_id != S31_CHIP_ID:
        raise ValueError(f"Bootloader chip ID is {chip_id}, expected S31 ID 32")

    return {"status": "MERGED_IMAGE_VERIFIED", "chip": "esp32s31",
            "binary": str(binary), "bytes": len(image), "sha256": digest(image),
            "flash_base_address": 0, "flash_limit_bytes": FLASH_LIMIT,
            "bootloader_offset": BOOT_OFFSET, "bootloader_chip_id": chip_id,
            "flasher_args_sha256": digest(manifest_bytes), "segment_count": len(segments),
            "segments": segments, "all_declared_bytes_match": True,
            "all_undeclared_bytes_are_ff": True, "hardware_verified": False,
            "scope": "Raw SDK merge, segment bytes, bounds, FF gaps, and S31 boot header; no device execution"}


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build", type=Path, required=True)
    parser.add_argument("--bin", dest="binary", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True,
                        help="New JSON report path; existing files are never overwritten")
    args = parser.parse_args()
    if args.output.exists():
        parser.error("Choose a new output report path; overwriting an existing file is refused")
    try:
        result = verify(args.build, args.binary)
        code = 0
    except (OSError, ValueError, TypeError) as exc:
        result = {"status": "FAIL", "error": str(exc), "hardware_verified": False}
        code = 1
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(json.dumps(result, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
    print(json.dumps(result, ensure_ascii=False))
    return code


if __name__ == "__main__":
    sys.exit(main())
