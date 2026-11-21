#!/usr/bin/env python3
"""Offline helpers for unsigned S31 / 16-MiB / DIO-80MHz development images.

Uses only Python 3.10+ standard library. Never installs software or touches a
serial port. Structural checks are not hardware or runtime certification.
"""
from __future__ import annotations

import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import struct
import subprocess
import sys

FLASH_BYTES = 16 * 1024 * 1024
TARGET = "metalio-eink4-plus"
LAYOUT = "metalio16m-plus-pocket2"
CAPACITY = 0xD00000


def require(condition, message):
    if not condition:
        raise ValueError(message)


def unique_object(pairs):
    result = {}
    for key, value in pairs:
        require(key not in result, f"Duplicate JSON key: {key}")
        result[key] = value
    return result


def read_json(path):
    return json.loads(Path(path).read_text(encoding="utf-8-sig"),
                      object_pairs_hook=unique_object)


def sha(data):
    return hashlib.sha256(data).hexdigest()


def image_bytes(path):
    path = Path(path).resolve()
    require(path.is_file(), f"Missing file: {path}")
    require(0 < path.stat().st_size <= FLASH_BYTES, "Image file size out of range")
    return path.read_bytes()


def cstring(data, empty=False):
    require(b"\0" in data, "Descriptor string lacks NUL terminator")
    content, padding = data.split(b"\0", 1)
    require(not any(padding), "Descriptor string has nonzero padding")
    text = content.decode("utf-8", errors="strict")
    require(empty or bool(text), "Required descriptor string is empty")
    require(not any(ord(c) < 32 or 127 <= ord(c) <= 159 for c in text),
            "Control character in descriptor")
    return text


def metadata(raw, profile="public-plus-pocket2"):
    require(profile in ("public-abi1", "public-plus-pocket2"), "Unknown protocol/layout profile")
    require(len(raw) == 512, "Truncated pocket metadata")
    require(raw[:16] == b"FPOCKET-ABI-0001", "Missing pocket ABI magic")
    major, minor, length, flags = struct.unpack_from("<HHII", raw, 16)
    require((major, minor, length) == (1, 0, 512), "Unsupported pocket ABI")
    storage, exit_method, tf_required, reserved = raw[28:32]
    state_schema = struct.unpack_from("<I", raw, 32)[0]
    require(flags in (1, 2) and storage in (0, 1, 2), "Invalid role/storage mode")
    require(tf_required in (0, 1) and reserved == 0 and not any(raw[420:]),
            "Unknown metadata flags or reserved bytes")
    require((storage == 0 and state_schema == 0) or
            (storage != 0 and 1 <= state_schema <= 0x7FFFFFFF), "Invalid state schema")
    fields = {name: cstring(raw[start:start + size], empty)
              for name, start, size, empty in (
                  ("target", 36, 32, False), ("layout_profile", 68, 32, False),
                  ("app_id", 100, 32, False), ("version", 132, 32, False),
                  ("name", 164, 64, False), ("description", 228, 96, True),
                  ("exit_hint", 324, 96, flags == 1))}
    for name in ("target", "layout_profile"):
        require(re.fullmatch(r"[a-z][a-z0-9_-]{0,30}", fields[name]), f"Invalid {name}")
    require(re.fullmatch(r"[a-z][a-z0-9_]{0,30}", fields["app_id"]), "Invalid app_id")
    require(re.fullmatch(r"[A-Za-z0-9_.+-]{1,31}", fields["version"]), "Invalid version")
    if flags == 1:
        require(fields["app_id"] == "pocket_manager" and storage == 0 and
                exit_method == 0 and not fields["exit_hint"],
                "Invalid manager declaration")
    else:
        require(fields["app_id"] != "pocket_manager", "Application uses manager identity")
        require(1 <= exit_method <= 5, "Unsupported public exit method")
    require(fields["target"] == TARGET and fields["layout_profile"] == LAYOUT,
            "Metadata is not this Plus layout")
    return dict(fields, flags=flags, storage_mode=storage, exit_method=exit_method,
                tf_required_standalone=bool(tf_required), state_schema=state_schema)


def esp_image(data, offset, limit):
    require(offset + 24 <= limit <= len(data), "Truncated ESP header")
    header = data[offset:offset + 24]
    require(header[0] == 0xE9 and 1 <= header[1] <= 16, "Invalid ESP image header")
    require(struct.unpack_from("<H", header, 12)[0] == 32, "Expected ESP32-S31 chip ID 32")
    require(header[2:4] == bytes((2, 0x4F)), "Expected DIO / 16-MiB / 80-MHz image header")
    require(header[23] == 1, "Image must contain its SHA256 digest")
    position, checksum = 24, 0xEF
    first_length = None
    for _ in range(header[1]):
        require(offset + position + 8 <= limit, "Truncated segment header")
        size = struct.unpack_from("<I", data, offset + position + 4)[0]
        if first_length is None:
            first_length = size
        position += 8
        require(size % 4 == 0 and offset + position + size <= limit, "Invalid segment size")
        for byte in data[offset + position:offset + position + size]:
            checksum ^= byte
        position += size
    digest_offset = (position + 16) & ~15
    end = offset + digest_offset + 32
    require(end <= limit, "Truncated checksum/digest")
    require(data[offset + digest_offset - 1] == checksum, "ESP segment checksum mismatch")
    require(hashlib.sha256(data[offset:offset + digest_offset]).digest() ==
            data[offset + digest_offset:end], "ESP SHA256 mismatch")
    return {"offset": offset, "bytes": end - offset,
            "sha256": sha(data[offset:end]), "first_segment_bytes": first_length}


def partitions(raw):
    require(len(raw) >= 64, "Missing partition table")
    result, md5_ok = [], False
    for pos in range(0, len(raw) - 31, 32):
        magic = struct.unpack_from("<H", raw, pos)[0]
        if magic == 0xEBEB:
            require(raw[pos + 2:pos + 16] == b"\xff" * 14 and
                    raw[pos + 32:] == b"\xff" * (len(raw) - pos - 32),
                    "Invalid partition MD5 marker or nonempty table tail")
            require(raw[pos + 16:pos + 32] == hashlib.md5(raw[:pos]).digest(),
                    "Partition MD5 mismatch")
            md5_ok = True
            break
        require(magic == 0x50AA, "Invalid partition table before MD5")
        _, kind, subtype, address, size, label, flags = struct.unpack_from("<HBBII16sI", raw, pos)
        name = cstring(label)
        require(name.isascii(), "Non-ASCII partition label")
        require(name and size and flags == 0, "Invalid/encrypted partition unsupported by this checker")
        require(address % 0x1000 == 0 and (kind != 0 or address % 0x10000 == 0),
                "Partition alignment error")
        result.append(dict(name=name, type=kind, subtype=subtype, offset=address,
                           size=size, flags=flags))
    require(md5_ok and result, "Missing partition MD5 or entries")
    require(len({p["name"] for p in result}) == len(result), "Duplicate partition name")
    previous = 0x9000
    for part in sorted(result, key=lambda p: p["offset"]):
        require(part["offset"] >= previous, "Overlapping partition ranges")
        previous = part["offset"] + part["size"]
        require(previous <= FLASH_BYTES, "Partition exceeds 16-MiB flash")
    return result


def inspect(data, require_pocket=False, profile="public-plus-pocket2"):
    require(profile in ("public-abi1", "public-plus-pocket2"), "Unknown protocol/layout profile")
    require(len(data) > 0x9000, "Expected full BIN from address 0x0, not a raw app")
    require(data[:0x2000] == b"\xff" * 0x2000, "First 8 KiB must be S31 reserved FF padding")
    boot = esp_image(data, 0x2000, 0x8000)
    require(not any(x != 0xFF for x in data[0x2000 + boot["bytes"]:0x8000]),
            "Unexpected data after bootloader; signed/encrypted layouts need another checker")
    table = partitions(data[0x8000:0x9000])
    apps = []
    for part in table:
        offset = part["offset"]
        if part["type"] != 0 or offset >= len(data):
            continue
        if data[offset:min(offset + 24, len(data))] == b"\xff" * min(24, len(data) - offset):
            continue
        app = esp_image(data, offset, min(offset + part["size"], len(data)))
        app["partition"] = part["name"]
        if data[offset + 0x120:offset + 0x130] == b"FPOCKET-ABI-0001":
            require(app["first_segment_bytes"] >= 256 + 512, "Metadata outside first segment")
            require(struct.unpack_from("<I", data, offset + 32)[0] == 0xABCD5432,
                    "Missing standard app descriptor")
            meta = metadata(data[offset + 0x120:offset + 0x320], profile)
            require(cstring(data[offset + 48:offset + 80]) == meta["version"] and
                    cstring(data[offset + 80:offset + 112]) == meta["app_id"],
                    "ESP project/version and pocket metadata disagree")
            app["pocket"] = meta
        apps.append(app)
    require(apps, "No populated application image")
    if require_pocket:
        require(len(apps) == 1 and apps[0].get("pocket", {}).get("flags") == 2,
                "Need exactly one dual-mode application, not a manager")
        app = apps[0]
        require(app["bytes"] <= CAPACITY, "Application exceeds Plus pocket2 13-MiB slot")
        if profile == "public-plus-pocket2":
            expected = [("nvs", 1, 2, 0x9000, 0x4000), ("otadata", 1, 0, 0xD000, 0x2000),
                        ("phy_init", 1, 1, 0xF000, 0x1000), ("factory", 0, 0, 0x10000, CAPACITY)]
            actual = [(p["name"], p["type"], p["subtype"], p["offset"], p["size"]) for p in table]
            require(actual == expected, "Plus pocket2 binding needs the compact factory partition layout")
            require(app["offset"] == 0x10000 and len(data) == 0x10000 + app["bytes"],
                    "Plus compact BIN must contain only 64-KiB prefix plus app")
            require(data[0x9000:0x10000] == b"\xff" * 0x7000,
                    "Compact prefix contains nonempty NVS/OTA/PHY personal or initialization data")
    return {"status": "STRUCTURE_OK", "chip": "esp32s31", "flash_address": "0x0",
            "bytes": len(data), "sha256": sha(data), "profile": profile,
            "pocket_required": require_pocket, "bootloader": boot,
            "partitions": table, "applications": apps, "hardware_verified": False,
            "resource_completeness_verified": False,
            "scope": "ESP integrity and selected layout only; runtime/resources need separate verification"}


def merge(build, output, require_pocket=False, profile="public-plus-pocket2"):
    build, output = Path(build).resolve(), Path(output).resolve()
    flash = read_json(build / "flasher_args.json")
    require(flash.get("extra_esptool_args", {}).get("chip") == "esp32s31",
            "Build target is not esp32s31")
    settings = flash.get("flash_settings", {})
    require((settings.get("flash_mode"), settings.get("flash_size"), settings.get("flash_freq"))
            == ("dio", "16MB", "80m"), "Raw merge requires DIO/16MB/80m settings")
    files = flash.get("flash_files")
    require(isinstance(files, dict) and files, "Missing flash_files")
    segments = []
    seen = set()
    for address, relative in files.items():
        offset = int(address, 0)
        require(offset not in seen, "Duplicate numerical flash address")
        seen.add(offset)
        require(isinstance(relative, str) and relative, "Invalid segment path")
        source = (build / relative).resolve()
        require(source != output, "Output would overwrite a source segment")
        payload = image_bytes(source)
        require(not payload.startswith(b"version https://git-lfs.github.com/spec"), "Unresolved LFS pointer")
        segments.append((offset, source, payload))
    segments.sort(key=lambda item: item[0])
    by_address = {offset: payload for offset, _, payload in segments}
    require(0x2000 in by_address and 0x8000 in by_address, "Missing S31 bootloader or partition table")
    table = partitions(by_address[0x8000])
    previous = 0x2000
    for offset, source, payload in segments:
        require(offset >= previous and offset + len(payload) <= FLASH_BYTES, "Overlapping/out-of-range segment")
        if offset == 0x2000:
            require(len(payload) <= 0x6000, "Bootloader overlaps partition table")
        elif offset == 0x8000:
            require(len(payload) <= 0x1000, "Partition table overlaps data")
        else:
            part = next((p for p in table if p["offset"] == offset), None)
            require(part is not None and len(payload) <= part["size"],
                    f"Segment not within declared partition: {source}")
        previous = offset + len(payload)
    font = next((p for p in table if p["name"] == "font_data"), None)
    require(font is None or font["offset"] in by_address,
            "font_data has no flash segment; restore verified font/CMake registration before merging")
    result = bytearray(b"\xff" * previous)
    for offset, _, payload in segments:
        result[offset:offset + len(payload)] = payload
    report = inspect(bytes(result), require_pocket, profile)
    require(not output.exists(), "Output exists; choose a new versioned output filename")
    output.parent.mkdir(parents=True, exist_ok=True)
    with output.open("xb") as stream:
        stream.write(result)
    report["output"] = str(output)
    report["segments"] = [{"offset": offset, "file": str(source), "bytes": len(payload),
                           "sha256": sha(payload)} for offset, source, payload in segments]
    report["merge_method"] = "raw bytes from build manifest; headers checked, never patched"
    return report


def generate_metadata(source, output):
    declaration = read_json(source)
    required = {"app_id", "version", "name", "description", "storage_mode", "state_schema",
                "exit_hint", "tf_required_standalone"}
    require(set(declaration) in (required, required | {"exit_method"}), "Application declaration has missing or unknown fields")
    exits = {"UI": 1, "AI_DOUBLE": 2, "AI_TRIPLE": 3, "AI_HOLD": 4, "CUSTOM": 5}
    method = declaration.get("exit_method", "UI")
    require(isinstance(method, str) and method in exits, "Unknown public exit method")
    modes = {"NONE": 0, "CHECKPOINT": 1, "DIRECT_TF": 2}
    require(declaration["storage_mode"] in modes, "Unknown storage mode")
    require(type(declaration["state_schema"]) is int and
            type(declaration["tf_required_standalone"]) is bool, "Incorrect declaration value types")
    raw = bytearray(512)
    raw[:16] = b"FPOCKET-ABI-0001"
    struct.pack_into("<HHIIBBBBI", raw, 16, 1, 0, 512, 2,
                     modes[declaration["storage_mode"]], exits[method],
                     int(declaration["tf_required_standalone"]), 0, declaration["state_schema"])
    values = dict(declaration, target=TARGET, layout_profile=LAYOUT)
    for name, offset, size in (("target", 36, 32), ("layout_profile", 68, 32),
                               ("app_id", 100, 32), ("version", 132, 32), ("name", 164, 64),
                               ("description", 228, 96), ("exit_hint", 324, 96)):
        require(isinstance(values[name], str), f"{name} must be a string")
        encoded = values[name].encode("utf-8")
        require(len(encoded) < size and b"\0" not in encoded, f"{name} too long or contains NUL")
        raw[offset:offset + len(encoded)] = encoded
    info = metadata(bytes(raw), "public-plus-pocket2")
    output = Path(output).resolve()
    require(not output.exists() and output != Path(source).resolve(), "Metadata output already exists")
    lines = ["/* Generated FPOCKET descriptor; link with -u pocket_compact_metadata. */",
             "#include <stdint.h>",
             '__attribute__((section(".rodata_custom_desc"), used))',
             "const uint8_t pocket_compact_metadata[512] = {"]
    lines.extend("    " + ", ".join(str(x) for x in raw[i:i + 16]) + "," for i in range(0, 512, 16))
    lines.append("};\n")
    output.parent.mkdir(parents=True, exist_ok=True)
    with output.open("x", encoding="utf-8", newline="\n") as stream:
        stream.write("\n".join(lines))
    return {"status": "METADATA_SOURCE_CREATED", "output": str(output), "declaration": info,
            "note": "Compile as C, link once, then verify actual BIN offset and runtime return implementation"}


def environment(project=None):
    def capture(args):
        try:
            done = subprocess.run(args, capture_output=True, text=True, timeout=15,
                                  encoding="utf-8", errors="replace", check=False)
            return {"exit_code": done.returncode, "output": (done.stdout + done.stderr).strip()[:1800]}
        except (OSError, subprocess.TimeoutExpired) as error:
            return {"error": str(error)}
    idf = os.environ.get("IDF_PATH")
    result = {"python": sys.executable, "python_version": sys.version.split()[0],
              "platform": sys.platform, "git": shutil.which("git"),
              "cmake": shutil.which("cmake"), "ninja": shutil.which("ninja"),
              "idf_path": idf, "idf_target_env": os.environ.get("IDF_TARGET"),
              "esptool": capture([sys.executable, "-m", "esptool", "version"])}
    if idf and (Path(idf) / "tools/idf.py").is_file():
        result["idf_version"] = capture([sys.executable, str(Path(idf) / "tools/idf.py"), "--version"])
        if result["git"]:
            result["idf_commit"] = capture([result["git"], "-C", idf, "rev-parse", "HEAD"])
    if project:
        config = Path(project) / "sdkconfig"
        if config.is_file():
            lines = config.read_text(encoding="utf-8").splitlines()
            result["target_config"] = [line for line in lines if line.startswith("CONFIG_IDF_TARGET=")]
    return result


def main():
    for stream in (sys.stdout, sys.stderr):
        if hasattr(stream, "reconfigure"):
            stream.reconfigure(encoding="utf-8")
    parser = argparse.ArgumentParser(description=__doc__)
    commands = parser.add_subparsers(dest="command", required=True)
    env = commands.add_parser("environment")
    env.add_argument("--project")
    for name in ("merge", "inspect"):
        sub = commands.add_parser(name)
        sub.add_argument("--report", help="Write a new UTF-8 JSON report after successful verification")
        sub.add_argument("--require-pocket", action="store_true")
        sub.add_argument("--profile", choices=("public-abi1", "public-plus-pocket2"), default="public-plus-pocket2")
        if name == "merge":
            sub.add_argument("--build", required=True)
            sub.add_argument("--output", required=True)
        else:
            sub.add_argument("--bin", required=True)
    meta = commands.add_parser("metadata")
    meta.add_argument("--declaration", required=True)
    meta.add_argument("--output", required=True)
    args = parser.parse_args()
    try:
        report_path = Path(args.report).resolve() if getattr(args, "report", None) else None
        if report_path:
            require(not report_path.exists(), "Report exists; choose a new evidence filename")
            for input_name in ("bin", "output"):
                if getattr(args, input_name, None):
                    require(report_path != Path(getattr(args, input_name)).resolve(), "Report and BIN paths must differ")
        if args.command == "environment":
            result = environment(args.project)
        elif args.command == "merge":
            result = merge(args.build, args.output, args.require_pocket, args.profile)
        elif args.command == "inspect":
            result = inspect(image_bytes(args.bin), args.require_pocket, args.profile)
        else:
            result = generate_metadata(args.declaration, args.output)
        if report_path:
            report_path.parent.mkdir(parents=True, exist_ok=True)
            with report_path.open("x", encoding="utf-8", newline="\n") as stream:
                stream.write(json.dumps(result, ensure_ascii=False, indent=2) + "\n")
        print(json.dumps(result, ensure_ascii=False, indent=2))
        return 0
    except (ValueError, OSError, struct.error, KeyError, TypeError) as error:
        print(json.dumps({"status": "ERROR", "error": str(error)}, ensure_ascii=False), file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
