#!/usr/bin/env python3
"""Native Windows/MSVC checks. All generated files go to --output (never this Skill)."""
import argparse
import json
import os
from pathlib import Path
import subprocess
import time

def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--output", required=True, type=Path)
    parser.add_argument("--vsdev", type=Path, default=Path(
        "C:/Program Files (x86)/Microsoft Visual Studio/2022/BuildTools/Common7/Tools/VsDevCmd.bat"))
    args = parser.parse_args()
    if os.name != "nt":
        raise SystemExit("This runner verifies native Windows/MSVC; use the Windows route.")
    root = Path(__file__).resolve().parents[1]
    pocket = root.parent / "pocket-integration" / "assets"
    output = args.output.resolve()
    installed_skill = root.parent.parent
    if installed_skill == output or installed_skill in output.parents:
        raise SystemExit("--output must be outside the installed Skill.")
    if not args.vsdev.is_file():
        raise SystemExit("MSVC VsDevCmd.bat not found; supply --vsdev.")
    for path in (root, pocket, output, args.vsdev):
        if any(c in str(path) for c in '\r\n%!"&|<>^'):
            raise SystemExit("For this CMD-based runner use paths without shell metacharacters.")
    output.mkdir(parents=True, exist_ok=True)
    run = output / time.strftime("host-%Y%m%d-%H%M%S")
    run.mkdir()
    def quote(path):
        return '"' + str(path) + '"'
    includes = [root/"assets"/"plus_services", root/"tests"/"compat",
                pocket/"pocket-standard", pocket/"wifi-standard"]
    common = ("cl /nologo /EHsc /std:c++17 /utf-8 /W4 /WX /wd4267 /wd4244 /wd4996 /wd4127 "
              "/D_CRT_SECURE_NO_WARNINGS /D_CRT_NONSTDC_NO_DEPRECATE " +
              " ".join("/I" + quote(p) for p in includes) +
              " /FI" + quote(root/"tests"/"compat"/"unistd.h"))
    core = pocket/"pocket-standard"/"pocket_core.cc"
    commands = [
        "@echo off", "chcp 65001 >nul", "call " + quote(args.vsdev) + " -arch=x64 >nul",
        "if errorlevel 1 exit /b 1",
        common + " " + quote(root/"tests"/"test_services.cpp") + " " + quote(core) + " /Fe:services.exe",
        "if errorlevel 1 exit /b 1", "services.exe fixture-services", "if errorlevel 1 exit /b 1",
        common + " " + quote(root/"tests"/"test_application_wifi.cpp") + " " + quote(core) + " " +
            quote(pocket/"wifi-standard"/"pocket_wifi.cc") + " /Fe:wifi.exe",
        "if errorlevel 1 exit /b 1", "wifi.exe fixture-wifi", "if errorlevel 1 exit /b 1",
        "exit /b 0"
    ]
    batch = run/"run.cmd"
    batch.write_text("\n".join(commands)+"\n", encoding="utf-8", newline="\r\n")
    result = subprocess.run(["cmd.exe", "/d", "/c", str(batch)], cwd=run,
                            stdout=subprocess.PIPE, stderr=subprocess.STDOUT, timeout=180)
    log = result.stdout.decode("utf-8", errors="replace")
    (run/"host.log").write_text(log, encoding="utf-8")
    report = {"exit_code": result.returncode, "platform": "Windows native MSVC C++17",
              "scope": "real RecordStore/credentials + portable service helpers",
              "not_tested": ["ESP radio execution", "true TF power loss", "USB", "device playback",
                             "STT", "full new firmware dual-mode acceptance"],
              "host_adapter": "MSVC POSIX calls; binary mode; no symlink/reparse-point equivalence claim"}
    (run/"result.json").write_text(json.dumps(report, ensure_ascii=False, indent=2), encoding="utf-8")
    print(log)
    print("Evidence:", run)
    raise SystemExit(result.returncode)
if __name__ == "__main__":
    main()

