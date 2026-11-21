"""Check installed Skill files against its release manifest (Python stdlib only)."""
from __future__ import annotations

import argparse
import hashlib
import json
from pathlib import Path, PurePosixPath


def inspect(root: Path) -> dict:
    root = root.resolve()
    manifest = json.loads((root / "manifest.json").read_text(encoding="utf-8-sig"))
    entries = manifest.get("files")
    if not isinstance(entries, list) or not entries:
        raise ValueError("manifest.files must contain the release inventory")
    expected = set()
    failures = []
    total = 0
    for entry in entries:
        name = entry["path"]
        pure = PurePosixPath(name)
        if (not name or pure.is_absolute() or ".." in pure.parts or "\\" in name
                or ":" in name or name == "manifest.json" or name in expected):
            raise ValueError(f"Invalid or duplicate inventory path: {name!r}")
        expected.add(name)
        path = root.joinpath(*pure.parts)
        if not path.resolve().is_relative_to(root):
            failures.append({"path": name, "error": "outside_package"})
            continue
        if not path.is_file():
            failures.append({"path": name, "error": "missing"})
            continue
        payload = path.read_bytes()
        total += len(payload)
        actual = hashlib.sha256(payload).hexdigest()
        if len(payload) != entry["bytes"] or actual != entry["sha256"]:
            failures.append({"path": name, "error": "content_mismatch"})
    actual_names = {p.relative_to(root).as_posix() for p in root.rglob("*") if p.is_file()}
    extra = sorted(actual_names - expected - {"manifest.json"})
    return {"ok": not failures and not extra, "name": manifest["name"],
            "version": manifest["version"], "revision": manifest.get("revision"),
            "checked_files": len(expected),
            "checked_bytes": total, "failures": failures, "unexpected_files": extra,
            "scope": "package_integrity_only_not_firmware_or_hardware_validation"}


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--root", type=Path, default=Path(__file__).resolve().parents[1])
    args = parser.parse_args()
    try:
        result = inspect(args.root)
    except (OSError, ValueError, KeyError, TypeError) as exc:
        print(json.dumps({"ok": False, "error": str(exc)}, ensure_ascii=True))
        return 1
    print(json.dumps(result, ensure_ascii=True, indent=2))
    return 0 if result["ok"] else 1


if __name__ == "__main__":
    raise SystemExit(main())
