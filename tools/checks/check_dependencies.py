#!/usr/bin/env python3
"""Check main's project includes without changing the ESP-IDF component layout."""

import argparse
import json
from pathlib import Path
import re


SOURCE_SUFFIXES = {".c", ".cc", ".cpp", ".cxx", ".h", ".hpp", ".hh", ".hxx"}
INCLUDE = re.compile(r'^[\t ]*#[\t ]*include[\t ]+([^\n]+)', re.MULTILINE)
LAYERS = {"boards", "infra", "business", "sysmgmt", "ui", "assets"}
SDK_MACROS = {
    "FT_FREETYPE_H": "freetype/freetype.h",
    "FT_ADVANCES_H": "freetype/ftadvanc.h",
    "FT_MODULE_H": "freetype/ftmodapi.h",
}


def compiler_includes(root, databases):
    """Read include search order, including generated headers, from existing builds."""
    sources = {}
    all_dirs = {}
    configurations = {}
    for database in databases:
        entries = json.loads(database.read_text(encoding="utf-8"))
        for entry in entries:
            source = Path(entry["file"])
            directory = Path(entry["directory"])
            if not source.is_absolute():
                source = directory / source
            source = source.resolve()
            if not source.is_relative_to(root / "main"):
                continue
            command = entry.get("command", "")
            if not command:
                command = " ".join('"' + arg + '"' if " " in arg else arg for arg in entry["arguments"])
            names = tuple(match[1] or match[2] for match in re.finditer(
                r'(?:^|\s)(?:-I|-isystem)\s*(?:"([^"]+)"|([^\s]+))', command))
            key = directory, names
            if key not in configurations:
                dirs = {}
                for name in names:
                    path = Path(name)
                    if not path.is_absolute():
                        path = directory / path
                    path = path.resolve()
                    dirs[path] = None
                    all_dirs[path] = None
                configurations[key] = tuple(dirs)
            sources[source] = configurations[key]
    return sources, tuple(all_dirs)


def boundary(source, target):
    """Return a rule name only for dependencies needing explicit ownership."""
    origin, dependency = source.parts[0], target.parts[0]
    if origin == "infra" and dependency in {"business", "sysmgmt", "ui"}:
        return "infra_upper_layer"
    if origin == "business" and dependency == "ui":
        return "business_ui"
    if origin == "boards" and dependency in {"business", "sysmgmt", "ui"}:
        return "board_assembly"
    if origin == "sysmgmt" and dependency == "ui":
        return "system_presentation"
    if source.parts[:2] == ("ui", "common") and target.parts[:2] in {
        ("ui", "apps"), ("ui", "home")
    }:
        return "common_application"
    if source.parts[:3] == ("ui", "common", "runtime") and target == Path("ui/application_ui.h"):
        return "runtime_registration"
    return None


def check(root, databases=()):
    root = root.resolve()
    main = root / "main"
    files = sorted(p for p in main.rglob("*") if p.suffix in SOURCE_SUFFIXES)
    by_name = {}
    for path in files:
        by_name.setdefault(path.name, []).append(path)
    source_dirs, all_dirs = compiler_includes(root, databases)
    search_cache = {}
    unresolved = []
    macro_count = 0
    policy = json.loads((root / "tools/checks/dependency_boundaries.json").read_text(encoding="utf-8"))
    allowed = {}
    for rule, group in policy["exceptions"].items():
        for source, targets in group["includes"].items():
            for target in targets:
                allowed[source, target] = {"rule": rule, "reason": group.get("reason")}
    used = set()
    errors = []
    edges = 0
    for path in files:
        source = path.relative_to(main)
        text = path.read_text(encoding="utf-8-sig")
        # Comments cannot introduce dependencies; keep line counts for diagnostics.
        text = re.sub(r'"(?:\\.|[^"\\])*"|\'(?:\\.|[^\'\\])*\'|/\*.*?\*/|//[^\n]*',
                      lambda m: "\n" * m[0].count("\n") if m[0].startswith("/") else m[0],
                      text, flags=re.DOTALL)
        # ELF networking reports actual allocation failures instead of applying firmware debug headroom.
        if source.as_posix() not in {"infra/tasks/task_create.cc", "business/app_store/app_network.cc"}:
            for match in re.finditer(r'\bxTaskCreate(?:PinnedToCoreWithCaps|PinnedToCore|WithCaps)?\s*\(', text):
                line = text.count("\n", 0, match.start()) + 1
                errors.append(f"main/{source.as_posix()}:{line}: dynamic tasks must use FirmwareCreateTask")
        for match in INCLUDE.finditer(text):
            operand = match[1].strip()
            location = f"main/{source.as_posix()}:{text.count(chr(10), 0, match.start()) + 1}"
            literal = re.fullmatch(r'([<"])([^>"\n]+)[>"]', operand)
            if literal:
                quoted, name = literal[1] == '"', literal[2]
            elif operand in SDK_MACROS:
                quoted, name = False, SDK_MACROS[operand]
                macro_count += 1
            else:
                errors.append(f"{location}: unsupported include expression: {operand}")
                continue
            target = None
            if quoted and (path.parent / name).is_file():
                target = (path.parent / name).resolve()
            if target is None and (main / name).is_file() and Path(name).parts[0] in LAYERS:
                target = (main / name).resolve()
            if target is None:
                dirs = source_dirs.get(path, all_dirs)
                key = dirs, name
                if key not in search_cache:
                    search_cache[key] = next(((directory / name).resolve() for directory in dirs
                                              if (directory / name).is_file()), None)
                target = search_cache[key]
            if target is None:
                candidates = [p for p in by_name.get(Path(name).name, []) if p.as_posix().endswith("/" + name)]
                if len(candidates) == 1:
                    target = candidates[0]
                elif len(candidates) > 1:
                    errors.append(f"{location}: ambiguous project include: {name}; supply compile_commands.json")
                    continue
            if target is None:
                if Path(name).parts[0] in LAYERS or (quoted and name.startswith(("./", "../"))):
                    errors.append(f"{location}: unresolved project include: {name}")
                else:
                    unresolved.append(f"{location}: {name}")
                continue
            if not target.is_relative_to(main):
                continue
            edges += 1
            relative = target.relative_to(main)
            if target.name.endswith("_priv.h") and not path.is_relative_to(target.parent):
                errors.append(f"{location}: private header outside its owner: {relative.as_posix()}")
            rule = boundary(source, relative)
            if rule:
                key = source.as_posix(), relative.as_posix()
                exception = allowed.get(key)
                if rule in {"infra_upper_layer", "business_ui", "runtime_registration"} or exception is None:
                    errors.append(f"{location}: {rule}: {relative.as_posix()}")
                elif exception.get("rule") != rule or not exception.get("reason"):
                    errors.append(f"{location}: exception must identify its rule and owner")
                else:
                    used.add(key)
    for source, target in sorted(set(allowed) - used):
        errors.append(f"unused dependency exception: {source} -> {target}; remove the stale entry")
    return errors, len(files), edges, len(used), unresolved, macro_count


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--root", type=Path, default=Path(__file__).resolve().parents[2])
    parser.add_argument("--compile-commands", type=Path, action="append", default=[])
    parser.add_argument("--show-unresolved", action="store_true", help="list unresolved external/toolchain headers")
    args = parser.parse_args()
    root = args.root.resolve()
    databases = args.compile_commands or [path for board in ("470", "397")
                                         if (path := root / "build" / board / "compile_commands.json").is_file()]
    errors, files, edges, exceptions, unresolved, macros = check(root, databases)
    if errors:
        print("\n".join(errors))
        return 1
    print(f"Dependency boundaries: PASS ({files} files, {edges} project includes, "
          f"{exceptions} assembly exceptions, {macros} known SDK macro includes)")
    print(f"Unresolved external/toolchain includes: {len(unresolved)} (outside boundary coverage)")
    if args.show_unresolved:
        print("\n".join(unresolved))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
