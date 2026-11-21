#!/usr/bin/env python3
"""Create a self-contained Plus/S31 application starter, never a final user app.

No SDK, network, historical project path or author machine is consulted.
The default page is an explicit hardware smoke probe; implement the confirmed
business and validate both real-device modes before final delivery.
"""
import argparse
import importlib.util
import json
import re
import shutil
import sys
import subprocess
import zlib
from pathlib import Path


def safe_text(value, limit):
    if not value or len(value.encode("utf-8")) >= limit or any(c in value for c in '\\"$;\r\n\x00'):
        raise ValueError("Invalid CMake/metadata string or UTF-8 byte limit exceeded")
    return value



def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--output", required=True, type=Path)
    p.add_argument("--app-id", required=True)
    p.add_argument("--name", required=True)
    p.add_argument("--version", default="1.0.0", help="Application version, independent of the installed Skill version")
    p.add_argument("--storage", choices=["none", "checkpoint", "direct-tf"], default="none")
    p.add_argument("--schema", type=int)
    p.add_argument("--wifi", action="store_true", help="Copy/compile the optional Wi-Fi backend; application must wire its UI")
    p.add_argument("--pinyin", action="store_true", help="Prepare checked pinyin data/core and required notices")
    a = p.parse_args()
    if not re.fullmatch(r"[a-z][a-z0-9_]{0,30}", a.app_id) or a.app_id == "pocket_manager":
        raise ValueError("app-id must be a non-manager lower-case CMake-safe identifier, <=31 bytes")
    safe_text(a.name, 64); safe_text(a.version, 32)
    if not re.fullmatch(r"[A-Za-z0-9._+\-]{1,31}", a.version):
        raise ValueError("version must match the public guide ASCII version contract")
    root = Path(__file__).resolve().parents[3]
    out = a.output.resolve()
    if out == root or root in out.parents or any("02_测试预置_原始材料" == x for x in out.parts):
        raise ValueError("Create project in independent WORK, not Skill or raw materials")
    if out.exists() and (not out.is_dir() or any(out.iterdir())):
        raise ValueError("Output must be an absent or empty directory; existing projects are never overwritten")
    mode = {"none": 0, "checkpoint": 1, "direct-tf": 2}[a.storage]
    schema = a.schema if a.schema is not None else (0 if mode == 0 else 1)
    if not 0 <= schema <= 0x7fffffff or (mode == 0) != (schema == 0):
        raise ValueError("NONE requires schema=0; persistent modes require a positive schema")
    base = root / "skills/firmware-build/assets/plus-app-base"
    pocket = root / "skills/pocket-integration/assets"
    shutil.copytree(base, out, dirs_exist_ok=True)
    shutil.copytree(pocket / "pocket-standard", out / "main/pocket")
    for name in ["plus_pocket_bridge.h", "plus_pocket_bridge.cc"]:
        shutil.copy2(pocket / name, out / "main" / name)
    shutil.copy2(pocket / "standalone-partitions.csv", out / "partitions.csv")
    definitions = {
        "PROJECT_VER": a.version, "POCKET_APP_ID": a.app_id,
        "PLUS_APP_WAKE_MAGIC": zlib.crc32((a.app_id + "@" + a.version).encode("utf-8")),
        "POCKET_ROLE_MANAGER": 0, "POCKET_STATE_SCHEMA": schema,
        "POCKET_STORAGE_MODE": mode, "POCKET_EXIT_METHOD": 1,
        "POCKET_TF_REQUIRED_STANDALONE": int(mode == 2 or a.wifi),
        "POCKET_DISPLAY_NAME": a.name,
        "POCKET_DESCRIPTION": "硬件验证起点，尚未实现用户业务",
        "POCKET_EXIT_HINT": "点击顶栏返回口袋，保存成功后返回",
    }
    # Quotes/expansion characters were rejected before writing any project file.
    lines = [f'set({key} "{value}")' for key, value in definitions.items()]
    (out / "app-config.cmake").write_text("\n".join(lines) + "\n", encoding="utf-8")
    sys.dont_write_bytecode = True
    font = root / "skills/font-support"
    shutil.copytree(font / "components/plus_font", out / "components/plus_font")
    subprocess.run([sys.executable, "-B", str(font / "scripts/prepare_font.py"),
                    "--size", "32", "--weight", "500", "--output", str(out / "main/generated/body")], check=True)
    from prepare_posters import prepare
    prepare(root, out)
    if a.wifi:
        shutil.copytree(pocket / "wifi-standard", out / "main/wifi")
        cmake = out / "main/CMakeLists.txt"
        content = cmake.read_text("utf-8").replace('SRCS "main.cc"', 'SRCS "wifi/pocket_wifi.cc" "wifi/pocket_wifi_connection.cc" "main.cc"')
        content = content.replace('INCLUDE_DIRS "."', 'INCLUDE_DIRS "." "wifi"')
        content = content.replace('REQUIRES plus_hal', 'REQUIRES esp_wifi esp_netif esp_event heap plus_hal')
        cmake.write_text(content, "utf-8", newline="\n")
    if a.pinyin:
        dest = out / "main/input"; dest.mkdir()
        shutil.copy2(root / "skills/text-input/assets/ime_core.h", dest / "ime_core.h")
        subprocess.run([sys.executable, "-B", str(root / "skills/text-input/scripts/table_tool.py"),
            "--header", str(dest / "pinyin_table.h"), "--notices-dir", str(out / "pinyin-notices")], check=True)
    license_script = root / "skills/license-compliance/scripts/emit_licenses.py"
    subprocess.run([sys.executable, "-B", str(license_script), "--project", str(out)]+(["--pinyin"] if a.pinyin else []), check=True)
    report = {"schema": 1, "device": "metalio-eink4-plus", "source": "bundled-plus-app-base",
              "sdk_commit": "fff9895c82d744c7237be8847347bdd1b07c6643", "app_id": a.app_id,
              "version": a.version, "storage_mode": mode, "state_schema": schema,
              "integration_contract": "public-guide-1.1", "layout_profile": "metalio16m-plus-pocket2",
              "capabilities_prepared": {"wifi_backend": a.wifi, "pinyin_core": a.pinyin},
              "business_ui_wired": False, "scaffold_only": True, "hardware_verified": False, "user_function_verified": False,
              "persistent_hooks_implemented": mode == 0,
              "next": "Build and verify hardware, then implement confirmed application; not final delivery"}
    (out / "scaffold-status.json").write_text(json.dumps(report, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
    print(json.dumps({"status": "PLUS_STARTER_CREATED", "output": str(out), "scaffold_only": True}, ensure_ascii=False))


if __name__ == "__main__":
    main()
