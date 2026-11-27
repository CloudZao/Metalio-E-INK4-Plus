#!/usr/bin/env python3
"""Build a board in an isolated build/configuration directory."""

import argparse
import os
import re
from pathlib import Path
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[2]
ALIASES = {"470": "metalio-e-ink4-plus", "397": "metalio-e-ink-4"}


def board_target(defaults):
    match = re.search(r'^CONFIG_IDF_TARGET="([a-z0-9]+)"$',
                      defaults.read_text(encoding="utf-8"), re.MULTILINE)
    if not match:
        raise ValueError("Board defaults must declare CONFIG_IDF_TARGET")
    return match.group(1)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("board", help="470, 397, or a board directory name; append -global for Global")
    parser.add_argument("action", choices=["build", "reconfigure", "menuconfig"], nargs="?", default="build")
    parser.add_argument("--build-dir", type=Path)
    args = parser.parse_args()
    idf = os.environ.get("IDF_PATH")
    if not idf or not (Path(idf) / "tools/idf.py").is_file():
        parser.error("Activate ESP-IDF 6.1 before running this command")
    international = args.board.endswith("-global")
    name = args.board[:-7] if international else args.board
    directory = ALIASES.get(name, name)
    if not re.fullmatch(r"[a-z0-9][a-z0-9-]*", directory):
        parser.error("Invalid board directory name")
    board_dir = ROOT / "main/boards" / directory
    try:
        target = board_target(board_dir / "sdkconfig.defaults")
    except (OSError, ValueError) as error:
        parser.error(str(error))
    build = (args.build_dir or ROOT / "build" / args.board).resolve()
    defaults = [ROOT / "sdkconfig.defaults"]
    # ESP-IDF 自动在每个 defaults 文件后加载对应的 .<target> 文件。
    defaults.append(board_dir / "sdkconfig.defaults")
    build.mkdir(parents=True, exist_ok=True)
    marker = build / "board-profile.txt"
    if marker.exists() and marker.read_text(encoding="utf-8").strip() != args.board:
        parser.error("Build directory belongs to another board; choose a separate directory")
    config = build / "sdkconfig"
    selection = "CONFIG_BOARD_TYPE_" + directory.replace("-", "_").upper()
    choices = re.findall(r"^\s*config (BOARD_TYPE_\w+)",
                         (ROOT / "main/Kconfig.projbuild").read_text(encoding="utf-8"), re.MULTILINE)
    choices = [choice for choice in choices if not choice.endswith("_GLOBAL")]
    if selection.removeprefix("CONFIG_") not in choices:
        parser.error("Board/product selection is not registered in Kconfig.projbuild")
    if config.exists() and not any(key + "=y" in config.read_text(encoding="utf-8").splitlines()
                                   for key in (selection, selection + "_GLOBAL")):
        parser.error("Existing sdkconfig selects a different board or product variant")
    marker.write_text(args.board + "\n", encoding="utf-8")
    selected_defaults = build / "board-selection.defaults"
    selected_text = "".join("CONFIG_" + choice + "=y\n" if "CONFIG_" + choice == selection
                            else "# CONFIG_" + choice + " is not set\n" for choice in choices)
    selected_text += ("CONFIG_PRODUCT_GLOBAL=y\n# CONFIG_PRODUCT_DOMESTIC is not set\n" if international
                      else "CONFIG_PRODUCT_DOMESTIC=y\n# CONFIG_PRODUCT_GLOBAL is not set\n")
    if not selected_defaults.exists() or selected_defaults.read_text(encoding="utf-8") != selected_text:
        selected_defaults.write_text(selected_text, encoding="utf-8")
    defaults.append(selected_defaults)
    command = [sys.executable, str(Path(idf) / "tools/idf.py"), "-B", str(build),
               "-D", "IDF_TARGET=" + target,
               "-D", "SDKCONFIG=" + str(build / "sdkconfig"),
               "-D", "SDKCONFIG_DEFAULTS=" + ";".join(str(p) for p in defaults), args.action]
    return subprocess.call(command, cwd=ROOT)


if __name__ == "__main__":
    sys.exit(main())
