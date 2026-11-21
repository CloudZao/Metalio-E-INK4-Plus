"""Copy a verified full BIN as bytes and recheck the actual receiving file.

No download, serial access, text conversion, ZIP requirement, or input mutation.
"""
from __future__ import annotations

import argparse
import hashlib
import json
from pathlib import Path
import sys

import firmware_tools
import verify_merged_image

SKILL_ROOT = Path(__file__).resolve().parents[3]


def output_path(path: Path) -> Path:
    path = path.resolve()
    if path == SKILL_ROOT or SKILL_ROOT in path.parents:
        raise ValueError("Write results to WORK, not the installed Skill")
    if path.exists():
        raise ValueError(f"Output already exists; use a new versioned path: {path}")
    return path


def deliver(source: Path, build: Path, destination: Path, report: Path) -> dict:
    source = source.resolve(strict=True)
    destination, report = output_path(destination), output_path(report)
    if len({source, destination, report}) != 3:
        raise ValueError("BIN source, destination and report must be different files")
    original = firmware_tools.image_bytes(source)
    before = firmware_tools.inspect(original, True, "public-plus-pocket2")
    verify_merged_image.verify(build, source)
    destination.parent.mkdir(parents=True, exist_ok=True)
    with destination.open("xb") as stream:
        stream.write(original)
    copied = destination.read_bytes()
    if copied != original:
        raise ValueError("Receiving file differs from original bytes; do not deliver it")
    if source.read_bytes() != original:
        raise ValueError("Source changed during copying; rebuild evidence before delivery")
    structure = firmware_tools.inspect(copied, True, "public-plus-pocket2")
    segments = verify_merged_image.verify(build, destination)
    result = {
        "status": "DELIVERY_COPY_VERIFIED", "source": str(source),
        "binary": str(destination), "bytes": len(copied),
        "sha256": hashlib.sha256(copied).hexdigest(),
        "source_sha256": before["sha256"], "binary_copy_equal": True,
        "structure": structure, "segments": segments,
        "acceptance_status": "pending_evidence", "validation_record": None,
        "hardware_verified": False,
        "scope": "Receiving bytes and build segments only; use image-bound runtime evidence separately",
    }
    report.parent.mkdir(parents=True, exist_ok=True)
    with report.open("x", encoding="utf-8", newline="\n") as stream:
        json.dump(result, stream, ensure_ascii=False, indent=2)
        stream.write("\n")
    return result


def main() -> int:
    for stream in (sys.stdout, sys.stderr):
        if hasattr(stream, "reconfigure"):
            stream.reconfigure(encoding="utf-8")
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--source", type=Path, required=True)
    parser.add_argument("--build", type=Path, required=True)
    parser.add_argument("--destination", type=Path, required=True)
    parser.add_argument("--report", type=Path, required=True)
    args = parser.parse_args()
    try:
        result = deliver(args.source, args.build, args.destination, args.report)
        print(json.dumps(result, ensure_ascii=False, indent=2))
        return 0
    except (ValueError, OSError, KeyError, TypeError) as error:
        print(json.dumps({"status": "FAIL", "error": str(error)}, ensure_ascii=False), file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
