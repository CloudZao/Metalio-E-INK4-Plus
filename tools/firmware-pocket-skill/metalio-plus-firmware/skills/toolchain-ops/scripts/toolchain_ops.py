"""Install a fixed Windows toolchain from byte-identical public raw downloads.

All extraction, wheel building and metadata selection occur under --destination;
the raw mirror is read-only input. This script does not create a Plus application
or apply any device-specific source/dependency/build-order repairs.
"""
from __future__ import annotations

import argparse
import datetime as dt
import hashlib
import json
import os
from pathlib import Path, PurePosixPath
import shutil
import subprocess
import sys
import tarfile
import time
import urllib.parse
import urllib.request
import zipfile

SKILL = Path(__file__).resolve().parents[1]
PROFILE_PATH = SKILL / "assets/environment-profile.json"
REGISTRY_PATH = SKILL / "references/registry.json"
REGISTRY_SHA256 = "dc38a81481b934ccfc7ccc246f1a6408396d39cfeabe365a9d123354b736e354"

ACQUISITION_PATH = SKILL / "assets/acquisition-profiles.json"


def acquisition_profile(name):
    catalog = load(ACQUISITION_PATH)
    if catalog["registry_sha256"] != REGISTRY_SHA256:
        raise RuntimeError("Acquisition profiles and registry disagree")
    return catalog["profiles"][name]


def selected_items(registry, name):
    profile = acquisition_profile(name)
    items = [i for i in registry.items if i["id"] in profile["item_ids"]]
    if len(items) != len(profile["item_ids"]) or len(items) != profile["file_count"] or sum(i["bytes"] for i in items) != profile["bytes"]:
        raise RuntimeError("Material profile selection is inconsistent")
    return items


def digest(path):
    with path.open("rb") as stream:
        return hashlib.file_digest(stream, "sha256").hexdigest()


def load(path):
    return json.loads(path.read_text(encoding="utf-8-sig"))


def save(path, value):
    path.parent.mkdir(parents=True, exist_ok=True)
    temporary = path.with_suffix(path.suffix + ".new")
    temporary.write_text(json.dumps(value, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
    temporary.replace(path)


def safe_relative(value, allow_dot=False):
    if value == "." and allow_dot:
        return Path()
    p = PurePosixPath(value)
    if not value or p.is_absolute() or ".." in p.parts or "\\" in value or ":" in value:
        raise RuntimeError("Unsafe relative path: " + value)
    return Path(*p.parts)


def under(root, relative):
    relative_path = safe_relative(relative)
    # Validate existing path components directly. Resolving a nonexistent deep
    # SUBST path can return mixed short/long prefixes on Windows; comparing those
    # strings falsely rejects safe extraction. Never follow a reparse descendant.
    path = root
    for part in (None, *relative_path.parts):
        if part is not None:
            path = path / part
        try:
            metadata = path.lstat()
        except FileNotFoundError:
            continue
        if path.is_symlink() or (getattr(metadata, "st_file_attributes", 0) & 0x400):
            raise RuntimeError("Reparse point inside material/extraction root: " + str(path))
    return path


class Registry:
    def __init__(self, root):
        self.root = root.resolve()
        self.path = self.root / "registry.json"
        if digest(self.path) != REGISTRY_SHA256:
            raise RuntimeError("Registry differs from the frozen r2 material identity")
        self.data = load(self.path)
        self.items = self.data["items"]
        paths = set()
        for item in self.items:
            relative = item["path"]
            safe_relative(relative)
            if relative.casefold() in paths:
                raise RuntimeError("Duplicate/case-colliding registry path: " + relative)
            paths.add(relative.casefold())
            if len(item["sha256"]) != 64 or item["bytes"] <= 0:
                raise RuntimeError("Missing original size or SHA256: " + relative)

    def local(self, item):
        return under(self.root, item["path"])

    def verify(self, item):
        path = self.local(item)
        if path.is_symlink() or not path.is_file():
            raise RuntimeError("Missing raw original: " + item["path"])
        if path.stat().st_size != item["bytes"] or digest(path) != item["sha256"]:
            raise RuntimeError("Raw original integrity mismatch: " + item["path"])
        return path

    def verify_all(self, profile="plus-base"):
        items = selected_items(self, profile)
        for i, item in enumerate(items, 1):
            self.verify(item)
            if i % 25 == 0:
                print(f"Verified selected raw originals {i}/{len(items)}", flush=True)

    def named(self, filename, sha256=None):
        found = [i for i in self.items if i["filename"] == filename and
                 (sha256 is None or i["sha256"] == sha256)]
        if len(found) != 1:
            raise RuntimeError(f"Expected one registry original: {filename}; found {len(found)}")
        return found[0]


def extract_source(archive, destination, strip_root=True):
    """Extract regular files; preserve Git-for-Windows symlink-false semantics.

    Codeload ZIP symlinks are written as their original link text, not followed.
    Tar link targets are likewise link-text files. No OS symlinks are created.
    """
    destination.mkdir(parents=True, exist_ok=True)
    if zipfile.is_zipfile(archive):
        with zipfile.ZipFile(archive) as z:
            members = z.infolist()
            if strip_root and len({PurePosixPath(m.filename).parts[0] for m in members if m.filename}) != 1:
                raise RuntimeError("Expected a single upstream archive root: " + str(archive))
            for member in members:
                pure = PurePosixPath(member.filename)
                safe_relative(member.filename.rstrip("/"))
                parts = pure.parts[1:] if strip_root else pure.parts
                if not parts:
                    continue
                target = under(destination, PurePosixPath(*parts).as_posix())
                if member.is_dir():
                    target.mkdir(parents=True, exist_ok=True)
                else:
                    target.parent.mkdir(parents=True, exist_ok=True)
                    with z.open(member) as incoming, target.open("wb") as outgoing:
                        shutil.copyfileobj(incoming, outgoing, 1024 * 1024)
    else:
        with tarfile.open(archive, "r:*") as tar:
            members = tar.getmembers()
            if strip_root and len({PurePosixPath(m.name).parts[0] for m in members if m.name}) != 1:
                raise RuntimeError("Expected a single upstream archive root: " + str(archive))
            for member in members:
                safe_relative(member.name.rstrip("/"))
                parts = PurePosixPath(member.name).parts[1:] if strip_root else PurePosixPath(member.name).parts
                if not parts:
                    continue
                target = under(destination, PurePosixPath(*parts).as_posix())
                if member.isdir():
                    target.mkdir(parents=True, exist_ok=True)
                elif member.isfile():
                    target.parent.mkdir(parents=True, exist_ok=True)
                    with tar.extractfile(member) as incoming, target.open("wb") as outgoing:
                        shutil.copyfileobj(incoming, outgoing, 1024 * 1024)
                elif member.issym() or member.islnk():
                    target.parent.mkdir(parents=True, exist_ok=True)
                    target.write_text(member.linkname, encoding="utf-8")
                else:
                    raise RuntimeError("Unsupported special archive member: " + member.name)


def run(arguments, env, cwd=None):
    print("+ " + subprocess.list2cmdline([str(x) for x in arguments]), flush=True)
    subprocess.run([str(x) for x in arguments], cwd=cwd, env=env, check=True)


def source_items(registry, profile):
    items = [i for i in registry.items if i["category"] == "sdk-sources"]
    if len(items) != profile["sdk_source_archive_count"]:
        raise RuntimeError("The frozen SDK source set is incomplete")
    paths = [i["checkout_path"] for i in items]
    if len(set(paths)) != len(paths) or paths.count(".") != 1:
        raise RuntimeError("Invalid SDK checkout paths")
    main = next(i for i in items if i["checkout_path"] == ".")
    if main["commit"] != profile["idf_commit"]:
        raise RuntimeError("Unexpected main SDK commit")
    for item in items:
        safe_relative(item["checkout_path"], allow_dot=True)
        if len(item["commit"]) != 40:
            raise RuntimeError("Unpinned SDK source")
    return sorted(items, key=lambda i: (i["checkout_path"] != ".", i["checkout_path"].count("/"), i["checkout_path"]))


def apply_tool_policy(metadata, selected_tools):
    for tool in metadata["tools"]:
        if tool["name"] in selected_tools:
            continue
        if tool["install"] == "always":
            tool["install"] = "on_request"
        for override in tool.get("platform_overrides", []):
            if override.get("install") == "always":
                override["install"] = "on_request"
    return metadata


def assemble_sdk(registry, profile, destination):
    sdk = destination / "sdk"
    receipt = destination / "sdk-source-receipt.json"
    items = source_items(registry, profile)
    sources = [{"checkout_path": i["checkout_path"], "commit": i["commit"], "sha256": i["sha256"]} for i in items]
    critical = ("tools/tools.json", "tools/cmake/version.cmake", "version.txt")
    if sdk.exists():
        if not receipt.is_file():
            raise RuntimeError("Existing SDK has no operations receipt; use a new destination")
        prior = load(receipt)
        if prior.get("material_profile") != profile["material_profile"] or prior["sources"] != sources or any(digest(sdk / p) != prior["critical_files"][p] for p in critical):
            raise RuntimeError("Existing SDK differs from its source receipt; use a new destination")
        if prior.get("tools_policy_version", 1) != 2:
            metadata = apply_tool_policy(load(sdk / "tools/tools.json"), profile["tools"])
            save(sdk / "tools/tools.json", metadata)
            prior["critical_files"]["tools/tools.json"] = digest(sdk / "tools/tools.json")
            prior["tools_policy_version"] = 2
            prior["policy_update"] = "Verified installed SDK policy now also covers platform_overrides; no tool versions or hashes changed"
            save(receipt, prior)
        return sdk
    temporary = destination / "sdk-assembling"
    if temporary.exists():
        retained = temporary
        temporary = destination / ("sdk-assembling-" + str(time.time_ns()))
        print("Prior incomplete SDK retained: " + str(retained), flush=True)
    temporary.mkdir()
    for item in items:
        extract_source(registry.local(item), temporary if item["checkout_path"] == "." else under(temporary, item["checkout_path"]))
    # A generated installation identity; never copied back into the raw mirror.
    version = temporary / "version.txt"
    if version.exists():
        if version.read_text(encoding="utf-8").strip() not in ("v6.1.0", "6.1.0"):
            raise RuntimeError("Unexpected upstream version.txt")
        addition = []
    else:
        version.write_text("v6.1.0\n", encoding="utf-8")
        addition = [{"path": "sdk/version.txt", "reason": "Generated installed-SDK version identity; not an upstream raw original"}]
    # Installation-only policy: debugger/editor tools are opt-in for this profile.
    # Keep every upstream version/URL/hash; do not mutate raw sources.
    tool_metadata = load(temporary / "tools/tools.json")
    upstream_tools_sha = digest(temporary / "tools/tools.json")
    apply_tool_policy(tool_metadata, profile["tools"])
    save(temporary / "tools/tools.json", tool_metadata)
    addition.append({"path": "sdk/tools/tools.json", "reason": "Installed profile tool policy; unselected tools become on_request; versions and hashes unchanged",
                     "upstream_sha256": upstream_tools_sha})
    temporary.rename(sdk)
    save(receipt, {"tools_policy_version": 2, "material_profile": profile["material_profile"], "sources": sources, "generated_files": addition,
                   "critical_files": {p: digest(sdk / p) for p in critical},
                   "symlink_policy": "Link text as regular files, matching git core.symlinks=false on Windows"})
    return sdk


def install_git(registry, profile, destination):
    item = registry.named(profile["git_filename"], profile["git_sha256"])
    git = destination / "git"
    receipt = destination / "git-source-receipt.json"
    if git.exists():
        if not receipt.is_file() or load(receipt)["archive_sha256"] != item["sha256"] or not (git / "cmd/git.exe").is_file():
            raise RuntimeError("Existing Git installation has no matching receipt; choose another destination")
    else:
        extract_source(registry.local(item), git, strip_root=False)
        save(receipt, {"archive_sha256": item["sha256"]})
    return git


def requirements_line(package, sha256):
    return package["name"] + "==" + package["version"] + " --hash=sha256:" + sha256


def install_python_pool(registry, profile, destination, env, sdk):
    tools = destination / "tools"
    venv = tools / "python_env/idf6.1_py3.13_env"
    python = venv / "Scripts/python.exe"
    if not python.is_file():
        run([sys.executable, "-m", "venv", venv], env)
    original_wheels = destination / "original-wheel-links"
    original_wheels.mkdir(exist_ok=True)
    original_requirements = []
    sdists = []
    for package in profile["python_packages"]:
        item = registry.named(package["filename"], package["sha256"])
        if package["kind"] == "wheel":
            target = original_wheels / package["filename"]
            if not target.is_file() or digest(target) != item["sha256"]:
                shutil.copy2(registry.local(item), target)
            original_requirements.append(requirements_line(package, item["sha256"]))
        else:
            sdists.append((package, item))
    original_lock = destination / "original-wheels.requirements.txt"
    original_lock.write_text("\n".join(original_requirements) + "\n", encoding="utf-8")
    # Transitional installation of the known complete wheel set. Dependency
    # consistency is checked after both source-only packages have been built.
    run([python, "-m", "pip", "install", "--no-index", "--find-links", original_wheels,
         "--no-deps", "--require-hashes", "-r", original_lock], env)
    built = destination / "locally-built-wheels"
    built.mkdir(exist_ok=True)
    generated = []
    final_requirements = list(original_requirements)
    for package, item in sdists:
        before = set(built.glob("*.whl"))
        run([python, "-m", "pip", "wheel", "--no-index", "--find-links", original_wheels,
             "--no-deps", "--no-build-isolation", "--wheel-dir", built, registry.local(item)], env)
        normalized = package["name"].replace("-", "_").replace(".", "_").lower()
        wheels = [p for p in built.glob("*.whl") if p.name.lower().startswith(normalized + "-" + package["version"] + "-")]
        if len(wheels) != 1:
            raise RuntimeError("Unexpected local wheel output for " + package["name"])
        wheel = wheels[0]
        local_hash = digest(wheel)
        generated.append({"name": package["name"], "version": package["version"],
                          "source_sha256": item["sha256"], "path": wheel.relative_to(destination).as_posix(),
                          "sha256": local_hash, "origin": "Locally built during installation; not a raw upstream wheel"})
        final_requirements.append(requirements_line(package, local_hash))
    final_lock = destination / "installed.requirements.txt"
    final_lock.write_text("\n".join(final_requirements) + "\n", encoding="utf-8")
    run([python, "-m", "pip", "install", "--no-index", "--find-links", original_wheels,
         "--find-links", built, "--no-deps", "--require-hashes", "-r", final_lock], env)
    run([python, "-m", "pip", "check"], env)
    (venv / "idf_version.txt").write_text("6.1\n", encoding="utf-8")
    constraint = registry.named(profile["constraints_filename"], profile["constraints_sha256"])
    shutil.copy2(registry.local(constraint), tools / profile["constraints_filename"])
    env["IDF_PYTHON_ENV_PATH"] = str(venv)
    run([python, sdk / "tools/idf_tools.py", "check-python-dependencies"], env)
    save(destination / "python-install-receipt.json", {"package_count": len(profile["python_packages"]),
        "original_wheel_count": len(original_requirements), "source_built_packages": generated,
        "requirements_sha256": digest(final_lock), "pip_check": "PASS", "idf_dependencies": "PASS"})
    return venv, original_wheels, built


def make_component_mirror(registry, profile, destination):
    mirror = destination / "component-storage"
    mirror.mkdir(parents=True, exist_ok=True)
    copied = []
    for name, version in sorted(profile["components"].items()):
        items = [i for i in registry.items if i["category"] == "components" and i.get("component_name") == name]
        indexes = [i for i in items if i.get("artifact_role") == "index"]
        if len(indexes) != 1:
            raise RuntimeError("Missing original component index: " + name)
        original = load(registry.local(indexes[0]))
        selected = [v for v in original["versions"] if v["version"] == version]
        if len(selected) != 1:
            raise RuntimeError("Original component index lacks frozen version: " + name + " " + version)
        current = dict(original)
        current["versions"] = selected
        save(under(mirror, "components/" + name + ".json"), current)
        for role, field in (("archive", "url"), ("checksums", "checksums")):
            found = [i for i in items if i.get("artifact_role") == role and i["version"] == version]
            if len(found) != 1:
                raise RuntimeError("Missing original component " + role + ": " + name)
            relative = selected[0][field]
            if urllib.parse.urlparse(relative).scheme:
                relative = urllib.parse.urlparse(relative).path.lstrip("/")
            target = under(mirror, relative)
            target.parent.mkdir(parents=True, exist_ok=True)
            shutil.copy2(registry.local(found[0]), target)
            copied.append({"component": name, "version": version, "role": role,
                           "path": target.relative_to(destination).as_posix(), "sha256": found[0]["sha256"]})
    save(destination / "component-storage-receipt.json", {"component_count": len(profile["components"]),
        "generated_indexes": "Runtime single-version selections from original full upstream indexes",
        "raw_mirror_modified": False, "original_copies": copied})
    return mirror


def prepare(registry, args, profile):
    if os.name != "nt" or sys.version_info[:3] != (3, 13, 17) or sys.maxsize <= 2**32:
        raise RuntimeError("Preparation requires standard Windows x64 CPython 3.13.17")
    destination = Path(os.path.abspath(args.destination))
    actual_destination = destination.resolve()
    text = str(destination)
    if len(text) > 40 or not text.isascii() or any(c.isspace() for c in text):
        raise RuntimeError("Use a short ASCII destination, at most 40 characters, without spaces")
    if actual_destination.is_relative_to(registry.root) or registry.root.is_relative_to(actual_destination):
        raise RuntimeError("Raw input and installed environment must be separate directories")
    if actual_destination.is_relative_to(SKILL.parents[1]) or SKILL.parents[1].is_relative_to(actual_destination):
        raise RuntimeError("Installed environment must be outside the Skill package")
    registry.verify_all(args.profile)
    destination.mkdir(parents=True, exist_ok=True)
    save(destination / "installation-start.json", {"profile": profile["name"], "destination_alias": str(destination), "destination_resolved": str(actual_destination), "material_profile": args.profile, "acquisition_profile_sha256": digest(ACQUISITION_PATH), "profile_sha256": digest(PROFILE_PATH),
         "registry_sha256": digest(registry.path), "started_utc": dt.datetime.now(dt.timezone.utc).isoformat()})
    git = install_git(registry, profile, destination)
    sdk = assemble_sdk(registry, profile, destination)
    tools = destination / "tools"
    dist = tools / "dist"
    dist.mkdir(parents=True, exist_ok=True)
    metadata = load(sdk / "tools/tools.json")
    selected = []
    for tool in metadata["tools"]:
        if tool["name"] not in profile["tools"]:
            continue
        version = next(v for v in tool["versions"] if v["status"] == "recommended")
        download = version.get("win64", version.get("any"))
        filename = download.get("rename_dist") or download["url"].rsplit("/", 1)[-1]
        item = registry.named(filename, download["sha256"])
        if item["bytes"] != download["size"]:
            raise RuntimeError("SDK tool size and registry disagree: " + filename)
        target = dist / filename
        if not target.is_file() or digest(target) != item["sha256"]:
            shutil.copy2(registry.local(item), target)
        selected.append(tool["name"] + "@" + version["name"])
    if len(selected) != len(profile["tools"]):
        raise RuntimeError("Unexpected SDK Windows tool selection")
    env = os.environ.copy()
    install_temp = under(destination, "install-temp")
    pip_cache = under(destination, "pip-cache")
    install_temp.mkdir(exist_ok=True)
    pip_cache.mkdir(exist_ok=True)
    # Child-process-only caches: never depend on or mutate the user's pip cache.
    env.update(TEMP=str(install_temp), TMP=str(install_temp), PIP_CACHE_DIR=str(pip_cache))
    # tools/activate.py normally exports this before idf.py loads extensions.
    # This preparation-time smoke check runs before shell activation, so supply
    # the same truthful version identity, checked against the assembled SDK.
    sdk_version = (sdk / "version.txt").read_text(encoding="utf-8").strip().removeprefix("v")
    if sdk_version != profile["idf_version"]:
        raise RuntimeError("Installed SDK version and frozen operations profile disagree")
    env.update(IDF_PATH=str(sdk), IDF_TOOLS_PATH=str(tools), IDF_TARGET=profile["target"],
               ESP_IDF_VERSION=sdk_version, PYTHONUTF8="1", PYTHONIOENCODING="utf-8",
               PIP_NO_INDEX="1", PIP_CONFIG_FILE="NUL", PIP_DISABLE_PIP_VERSION_CHECK="1", PYTHONNOUSERSITE="1")
    env["PATH"] = str(git / "cmd") + os.pathsep + str(Path(sys.executable).parent) + os.pathsep + env["PATH"]
    run([sys.executable, sdk / "tools/idf_tools.py", "--non-interactive", "install",
         *selected, "--targets=esp32s31"], env)
    venv, wheels, built = install_python_pool(registry, profile, destination, env, sdk)
    mirror = make_component_mirror(registry, profile, destination)
    values = {"IDF_PATH": str(sdk), "IDF_TOOLS_PATH": str(tools), "IDF_TARGET": profile["target"],
              "IDF_PYTHON_ENV_PATH": str(venv), "PLUS_RAW_MIRROR": str(registry.root),
              "PLUS_OPS_DESTINATION": str(destination),
              "IDF_COMPONENT_LOCAL_STORAGE_URL": mirror.as_uri(),
              "IDF_COMPONENT_CACHE_PATH": str(destination / "component-download-cache"),
              "PIP_DISABLE_PIP_VERSION_CHECK": "1", "PYTHONNOUSERSITE": "1",
              "PYTHONUTF8": "1", "PYTHONIOENCODING": "utf-8"}
    # The frozen base is installed with explicit --no-index flags and a private
    # child-process environment. Do not impose that policy on later business
    # dependencies or silently change the user's pip/network configuration.
    activation = ["$ErrorActionPreference = 'Stop'"]
    for key, value in values.items():
        activation.append("$env:" + key + " = '" + value.replace("'", "''") + "'")
    paths = os.pathsep.join((str(venv / "Scripts"), str(Path(sys.executable).parent), str(git / "cmd")))
    activation.append("$env:PATH = '" + paths.replace("'", "''") + ";' + $env:PATH")
    activation.append(". '" + str(sdk / "export.ps1").replace("'", "''") + "'")
    # Upstream export resolves SUBST aliases. Restore the already validated short
    # runtime paths after activation so downstream compilers keep the short paths.
    for key, value in values.items():
        activation.append("$env:" + key + " = '" + value.replace("'", "''") + "'")
    (destination / "Activate.ps1").write_text("\n".join(activation) + "\n", encoding="utf-8-sig")
    run([venv / "Scripts/python.exe", sdk / "tools/idf.py", "--version"], env)
    save(destination / "environment.json", {"status": "ENVIRONMENT_PREPARED", "profile": profile["name"],
        "destination_alias": str(destination), "destination_resolved": str(actual_destination),
        "material_profile": args.profile, "acquisition_profile_sha256": digest(ACQUISITION_PATH),
        "sdk_commit": profile["idf_commit"], "values": values, "sdk_tools": selected,
        "raw_registry_sha256": digest(registry.path), "operations_profile_sha256": digest(PROFILE_PATH),
        "raw_mirror_modified": False, "plus_application_created": False,
        "device_source_fixes_applied": False, "target_firmware_built": False,
        "hardware_verified": False, "completed_utc": dt.datetime.now(dt.timezone.utc).isoformat()})
    print("ENVIRONMENT_PREPARED", flush=True)


def fetch(registry, args):
    """All acquisition uses the same native Windows engine as Get-Materials.

    Python remains useful for verification/installation, but no independent
    source-priority or transport implementation can drift from PowerShell.
    """
    command = ["powershell.exe", "-NoProfile", "-ExecutionPolicy", "Bypass", "-File",
               str(SKILL / "scripts/Get-Materials.ps1"), "-Channel", args.channel,
               "-Cache", str(registry.root), "-Profile", args.profile,
               "-Attempts", str(args.attempts), "-TimeoutSeconds", str(args.timeout),
               "-DeadlineSeconds", str(args.deadline)]
    if not args.plan_file:
        command += ["-Stage", args.stage or "profile"]
    if args.mirror_base:
        command += ["-MirrorBase", args.mirror_base]
    if args.source:
        command += ["-Source", str(args.source)]
    for name, flag in (("plan_file", "-PlanFile"), ("mirror_map", "-MirrorMap"),
                       ("report_path", "-ReportPath"), ("device_root", "-DeviceRoot"),
                       ("context_file", "-ContextFile")):
        value = getattr(args, name)
        if value:
            command += [flag, str(value)]
    if args.offline:
        command.append("-Offline")
    child_env = os.environ.copy()
    # A Python child of PS7 otherwise passes incompatible module paths to PS5.1.
    child_env.pop("PSModulePath", None)
    result = subprocess.run(command, check=False, env=child_env)
    if result.returncode:
        # The PowerShell report identifies the failure; do not echo command-line URLs or secrets.
        raise SystemExit(result.returncode)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    sub = parser.add_subparsers(dest="command", required=True)
    for name in ("init", "verify", "fetch", "prepare"):
        command = sub.add_parser(name, aliases=["download"] if name == "fetch" else [])
        command.add_argument("--mirror", required=True, type=Path)
        command.add_argument("--profile", choices=("plus-base", "official", "debug", "full"), default="plus-base")
        if name == "prepare":
            command.add_argument("--destination", required=True, type=Path)
        if name == "fetch":
            command.add_argument("--mirror-base")
            command.add_argument("--channel", choices=("Auto", "Local", "Official", "Mirror"), default="Auto")
            command.add_argument("--source", type=Path)
            scope = command.add_mutually_exclusive_group()
            scope.add_argument("--stage", choices=("install", "profile", "python-bootstrap", "git-bootstrap", "sdk", "tools", "python-core"))
            scope.add_argument("--plan-file", type=Path, help="Frozen plan; use Plan-Materials.ps1 for explicit extra IDs")
            command.add_argument("--mirror-map", type=Path)
            command.add_argument("--report-path", type=Path)
            command.add_argument("--device-root", type=Path)
            command.add_argument("--context-file", type=Path)
            command.add_argument("--offline", action="store_true")
            command.add_argument("--attempts", type=int, choices=(1, 2), default=2)
            command.add_argument("--timeout", type=int, default=30)
            command.add_argument("--deadline", type=int, default=900)
    args = parser.parse_args()
    if args.command == "init":
        if digest(REGISTRY_PATH) != REGISTRY_SHA256:
            raise RuntimeError("Bundled registry integrity mismatch")
        target = args.mirror.resolve()
        if target.is_relative_to(SKILL.parents[1]) or SKILL.parents[1].is_relative_to(target):
            raise RuntimeError("Raw cache must be outside the Skill package")
        target.mkdir(parents=True, exist_ok=True)
        registry_path = target / "registry.json"
        if registry_path.exists() and digest(registry_path) != REGISTRY_SHA256:
            raise RuntimeError("Existing registry differs; use a fresh cache")
        if not registry_path.exists():
            shutil.copyfile(REGISTRY_PATH, registry_path)
        print(json.dumps({"status": "RAW_CACHE_INITIALIZED", "path": str(target)}))
        return
    registry = Registry(args.mirror)
    if args.command == "verify":
        registry.verify_all(args.profile)
        print(json.dumps({"status": "RAW_INPUTS_VERIFIED", "profile": args.profile, "file_count": len(selected_items(registry, args.profile)),
                          "registry_sha256": digest(registry.path)}, ensure_ascii=False))
    elif args.command in ("fetch", "download"):
        fetch(registry, args)
    else:
        profile = load(PROFILE_PATH)
        selected = acquisition_profile(args.profile)
        profile["material_profile"] = args.profile
        profile["tools"] = selected["tools"]
        if selected["components"] == "none":
            profile["components"] = {}
        prepare(registry, args, profile)


if __name__ == "__main__":
    main()


