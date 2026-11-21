"""Apply authored development preparation to a verified, unchanged upstream archive."""
from pathlib import Path, PurePosixPath
import argparse
import hashlib
import json
import shutil
import subprocess
import sys
import zipfile

COMMIT = "ad8edf4150a688737c0d7c00cae4413079b262ef"
SKILL = Path(__file__).resolve().parents[1]
PACKAGE_ROOT = Path(__file__).resolve().parents[3]


def digest(path):
    with path.open("rb") as stream:
        return hashlib.file_digest(stream, "sha256").hexdigest()


def relative(name):
    p = PurePosixPath(name)
    if p.is_absolute() or ".." in p.parts or "\\" in name or ":" in name:
        raise ValueError("Unsafe relative path: " + name)
    return p


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--mirror", required=True, type=Path)
    parser.add_argument("--path", required=True, type=Path)
    args = parser.parse_args()
    target = args.path.resolve()
    mirror = args.mirror.resolve()
    if target.is_relative_to(mirror) or target.is_relative_to(PACKAGE_ROOT):
        raise ValueError("Project must be outside the complete Skill package and raw mirror")
    if any(ord(c) > 126 or ord(c) < 33 for c in str(target)) or len(str(target)) > 50:
        raise ValueError("Use a fresh short ASCII project path without spaces, e.g. C:/PM/plus")
    if target.exists():
        raise FileExistsError("Project must not already exist: " + str(target))
    registry = json.loads((mirror / "registry.json").read_text(encoding="utf-8-sig"))
    choices = [item for item in registry["items"]
               if item["category"] == "plus-source" and
               (item.get("commit") == COMMIT or item.get("version") == COMMIT)]
    if len(choices) != 1:
        raise ValueError("Exactly one fixed official Plus source archive is required")
    item = choices[0]
    archive_path = mirror.joinpath(*relative(item["path"]).parts)
    if not archive_path.resolve().is_relative_to(mirror):
        raise ValueError("Archive path escapes mirror")
    if archive_path.stat().st_size != item["bytes"] or digest(archive_path) != item["sha256"]:
        raise ValueError("Upstream archive SHA256 mismatch")
    selection = json.loads((SKILL / "assets/official-build-fixes/source-selection.json").read_text(encoding="utf-8"))
    if selection["upstream_commit"] != COMMIT:
        raise ValueError("Source selection commit mismatch")
    # Validate every selected upstream byte before creating any project files.
    with zipfile.ZipFile(archive_path) as archive:
        names = archive.namelist()
        if len(names) != len(set(names)):
            raise ValueError("Duplicate archive members")
        roots = {relative(name).parts[0] for name in names if name}
        if len(roots) != 1:
            raise ValueError("Expected one upstream archive root")
        prefix = next(iter(roots)) + "/"
        for source in selection["files"]:
            name = source["path"]
            relative(name)
            data = archive.read(prefix + name)
            if len(data) != source["bytes"] or hashlib.sha256(data).hexdigest() != source["sha256"]:
                raise ValueError("Selected original source mismatch: " + name)
        target.mkdir(parents=True)
        for source in selection["files"]:
            path = target.joinpath(*relative(source["path"]).parts)
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_bytes(archive.read(prefix + source["path"]))
    fixes = SKILL / "assets/official-build-fixes"
    destination = target / "components/p02_build_dependencies"
    if destination.exists():
        raise FileExistsError("Public dependency overlay would overwrite source")
    shutil.copytree(fixes / "p02_build_dependencies", destination)
    if (target / "dependencies.lock").exists():
        raise FileExistsError("Refuse to replace an upstream dependency lock")
    shutil.copy2(fixes / "dependencies.lock", target / "dependencies.lock")
    subprocess.run([sys.executable, str(SKILL / "scripts/generate_language_header.py"),
                    "--project", str(target), "--language", "zh-CN"], check=True)
    provenance = {
        "upstream_commit": COMMIT, "upstream_url": item["upstream_url"],
        "upstream_archive_sha256": item["sha256"],
        "selected_original_files": len(selection["files"]),
        "authored_preparation_owner": "metalio-plus-firmware@1.0.0",
        "generated_or_added": ["main/assets/lang_config.h", "components/p02_build_dependencies", "dependencies.lock"],
        "raw_mirror_files_modified": False,
        "scope": "Development preparation only; target build, BIN checks and device execution are separate."
    }
    (target / "skill-development-provenance.json").write_text(json.dumps(provenance, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
    print(json.dumps({"status": "OFFICIAL_PROJECT_PREPARED", "project": str(target), **provenance}, ensure_ascii=False))


if __name__ == "__main__":
    main()
