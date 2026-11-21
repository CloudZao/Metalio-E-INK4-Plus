"""Create/check image-bound evidence records. This checks claims, not hardware.

Record observed results and evidence files in WORK. A changed BIN requires a
new record: historical PASS claims are never automatically migrated.
"""
from __future__ import annotations

import argparse
from datetime import datetime, timezone
import hashlib
import json
from pathlib import Path
import sys

import firmware_tools

SKILL_ROOT = Path(__file__).resolve().parents[3]
CHECKS = {
    "build": ("host", "Target build succeeds with preserved command and exit code", {"host-test"}),
    "image_structure": ("host", "Full BIN structure and source segments match", {"host-test"}),
    "integration_review": ("host", "Initialization, resource budget, mode, startup confirmation and return wiring reviewed", {"source-review", "host-test"}),
    "business_host": ("host", "Applicable layout, state and persistence tests executed", {"host-test"}),
    "protocol_contract": ("host", "Production adapter agrees with guide 1.1 records, errors and callbacks; no product version whitelist", {"source-review", "host-test"}),
    "standalone_boot": ("standalone", "Expected application boots in standalone layout", {"serial", "hardware-test"}),
    "standalone_business": ("standalone", "User's core use cases work", {"user-observation", "hardware-test"}),
    "standalone_ui_input_power": ("standalone", "Display, touch, lock/unlock and deep sleep/wake work", {"user-observation", "hardware-test"}),
    "standalone_persistence": ("standalone", "Business state survives required save/restart path", {"user-observation", "hardware-test"}),
    "pocket_boot_confirm": ("pocket", "Manager launches expected app; platform confirmation and started receipt succeed", {"serial", "hardware-test"}),
    "pocket_business": ("pocket", "User's core use cases work in pocket mode", {"user-observation", "hardware-test"}),
    "pocket_ui_input_power": ("pocket", "Display, touch, lock/unlock and deep sleep/wake work", {"user-observation", "hardware-test"}),
    "pocket_return_recovery": ("pocket", "Return and reopen follow the declared data strategy", {"user-observation", "hardware-test"}),
}
WIFI_CHECKS = {
    "wifi_production_host": ("host", "Production Wi-Fi service covers SDK stop/drain, stale events, new IP and error reporting", {"host-test"}),
    "wifi_standalone": ("standalone", "Device setup reaches IP without shared credentials; wrong password can be corrected", {"serial", "hardware-test"}),
    "wifi_shared": ("pocket", "App reads standard shared credentials and establishes its own connection; private/manual setup remains available", {"serial", "hardware-test"}),
    "wifi_errors": ("pocket", "Save failure, busy queue, authentication/no-IP and HTTP failure remain distinguishable; no credentials in logs", {"user-observation", "hardware-test"}),
}


def fingerprint(path: Path) -> dict:
    h = hashlib.sha256()
    count = 0
    with path.open("rb") as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b""):
            h.update(block)
            count += len(block)
    return {"bytes": count, "sha256": h.hexdigest()}


def initialize(binary: Path, output: Path, skill_version: str, material_version: str, wifi: bool = False) -> dict:
    binary = binary.resolve(strict=True)
    structure = firmware_tools.inspect(firmware_tools.image_bytes(binary), True, "public-plus-pocket2")
    output = output.resolve()
    if output.exists() or output == SKILL_ROOT or SKILL_ROOT in output.parents:
        raise ValueError("Choose a new evidence record in WORK, outside installed Skill")
    app = structure["applications"][0]
    storage_mode = app["pocket"]["storage_mode"]
    selected_checks = {**CHECKS, **(WIFI_CHECKS if wifi else {})}
    checks = {name: {"mode": mode, "requirement": requirement, "status": "pending",
                     "observed": "", "evidence": []}
              for name, (mode, requirement, _) in selected_checks.items()}
    if storage_mode == 0:
        checks["standalone_persistence"]["requirement"] = (
            "NONE: no business persistence required; verify no unexpected business writes "
            "or pocket identity/run/receipt records in standalone mode")
        checks["pocket_return_recovery"]["requirement"] = (
            "NONE: return/reopen follow the declared stateless strategy; no placeholder "
            "business archive, while required pocket handoff receipts still succeed")
    else:
        strategy = {1: "CHECKPOINT", 2: "DIRECT_TF"}[storage_mode]
        checks["standalone_persistence"]["requirement"] = (
            f"{strategy}: required business state survives save/restart according to the declared data strategy")
        checks["pocket_return_recovery"]["requirement"] = (
            f"{strategy}: save, return and reopen follow the declared data strategy")
    record = {
        "schema": "plus-validation-v2", "created_utc": datetime.now(timezone.utc).isoformat(),
        "binary": {"path": str(binary), **fingerprint(binary)},
        "application": {"app_id": app["pocket"]["app_id"], "version": app["pocket"]["version"],
                        "app_sha256": app["sha256"], "storage_mode": storage_mode},
        "skill_version": skill_version, "material_version": material_version,
        "device": "metalio-eink4-plus", "device_identity": None,
        "protocol": {"guide_revision": "1.1", "abi": "1.0", "layout_profile": firmware_tools.LAYOUT},
        "integration_host": {"name": None, "version": None},
        "capabilities": {"wifi": wifi},
        "checks": checks,
        "note": "Fill observed results from actual commands/user reports; this record is not a hardware test runner",
    }
    output.parent.mkdir(parents=True, exist_ok=True)
    with output.open("x", encoding="utf-8", newline="\n") as stream:
        json.dump(record, stream, ensure_ascii=False, indent=2)
        stream.write("\n")
    return record


def check_record(binary: Path, record_path: Path) -> dict:
    record_path = record_path.resolve(strict=True)
    record = firmware_tools.read_json(record_path)
    if record.get("schema") != "plus-validation-v2":
        raise ValueError("Unsupported validation record schema")
    current = fingerprint(binary.resolve(strict=True))
    if any(record.get("binary", {}).get(key) != value for key, value in current.items()):
        raise ValueError("BIN identity changed: create a new record; prior PASS claims do not apply")
    protocol = record.get("protocol", {})
    if protocol != {"guide_revision": "1.1", "abi": "1.0", "layout_profile": firmware_tools.LAYOUT}:
        raise ValueError("Protocol/binding record differs from this adapter")
    capabilities = record.get("capabilities", {})
    if not isinstance(capabilities, dict) or type(capabilities.get("wifi")) is not bool:
        raise ValueError("Explicit Wi-Fi capability selection missing")
    selected_checks = {**CHECKS, **(WIFI_CHECKS if capabilities["wifi"] else {})}
    checks = record.get("checks")
    if not isinstance(checks, dict) or not set(selected_checks).issubset(checks):
        raise ValueError("Required verification categories missing")
    pending, failed, passed = [], [], []
    for name, entry in checks.items():
        if not isinstance(entry, dict) or entry.get("status") not in ("pending", "pass", "fail"):
            raise ValueError(f"Invalid check/status: {name}")
        if not isinstance(entry.get("requirement"), str) or not entry["requirement"].strip():
            raise ValueError(f"Missing requirement: {name}")
        if name in selected_checks:
            expected_mode, _, allowed_kinds = selected_checks[name]
            if entry.get("mode") != expected_mode:
                raise ValueError(f"Wrong evidence mode: {name}")
        else:
            if entry.get("mode") not in ("host", "standalone", "pocket"):
                raise ValueError(f"Invalid custom requirement mode: {name}")
            allowed_kinds = {"host-test"} if entry["mode"] == "host" else {"serial", "user-observation", "hardware-test"}
        status = entry["status"]
        evidence = entry.get("evidence", [])
        if not isinstance(evidence, list):
            raise ValueError(f"Evidence must be a list: {name}")
        if status == "pass":
            if not isinstance(entry.get("observed"), str) or not entry["observed"].strip():
                raise ValueError(f"PASS needs an actual observed result: {name}")
            if entry["mode"] != "host" and not record.get("device_identity"):
                raise ValueError("Hardware PASS needs a recorded device identity")
            if entry["mode"] == "pocket":
                host = record.get("integration_host", {})
                if not isinstance(host, dict) or any(not isinstance(host.get(key), str) or not host[key].strip()
                                                    for key in ("name", "version")):
                    raise ValueError("Integrated PASS needs the actual host identity/version, not a predefined whitelist")
            if not evidence or not any(isinstance(item, dict) and item.get("kind") in allowed_kinds for item in evidence):
                raise ValueError(f"PASS lacks evidence of appropriate type: {name}")
        for item in evidence:
            if not isinstance(item, dict) or item.get("kind") not in {
                "source-review", "host-test", "serial", "user-observation", "hardware-test"}:
                raise ValueError(f"Invalid evidence kind: {name}")
            if not isinstance(item.get("path"), str) or not item["path"]:
                raise ValueError(f"Evidence path missing: {name}")
            path = (record_path.parent / item["path"]).resolve(strict=True)
            actual = fingerprint(path)
            if item.get("sha256") != actual["sha256"]:
                raise ValueError(f"Evidence file changed or hash absent: {name}: {path}")
        {"pending": pending, "fail": failed, "pass": passed}[status].append(name)
    return {"status": "EVIDENCE_RECORD_VALID", "binary": current, "passed": passed,
            "failed": failed, "pending": pending, "claims_complete": not pending and not failed,
            "scope": "Image binding, evidence types and file integrity; reported observations require human/agent review"}


def main() -> int:
    for stream in (sys.stdout, sys.stderr):
        if hasattr(stream, "reconfigure"):
            stream.reconfigure(encoding="utf-8")
    parser = argparse.ArgumentParser(description=__doc__)
    commands = parser.add_subparsers(dest="command", required=True)
    init = commands.add_parser("init")
    init.add_argument("--bin", dest="binary", type=Path, required=True)
    init.add_argument("--output", type=Path, required=True)
    init.add_argument("--skill-version", required=True)
    init.add_argument("--material-version", required=True)
    init.add_argument("--wifi", action="store_true", help="Include production Wi-Fi and both-mode network acceptance")
    check = commands.add_parser("check")
    check.add_argument("--bin", dest="binary", type=Path, required=True)
    check.add_argument("--record", type=Path, required=True)
    check.add_argument("--require-complete", action="store_true")
    args = parser.parse_args()
    try:
        if args.command == "init":
            result = initialize(args.binary, args.output, args.skill_version, args.material_version, args.wifi)
        else:
            result = check_record(args.binary, args.record)
        print(json.dumps(result, ensure_ascii=False, indent=2))
        return 2 if args.command == "check" and args.require_complete and not result["claims_complete"] else 0
    except (OSError, ValueError, TypeError, KeyError) as error:
        print(json.dumps({"status": "FAIL", "error": str(error)}, ensure_ascii=False), file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
