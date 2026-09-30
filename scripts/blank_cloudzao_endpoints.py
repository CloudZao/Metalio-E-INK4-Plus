#!/usr/bin/env python3
"""Blank all string literals in main/cloudzao_endpoints.c (CI open release)."""
from __future__ import annotations

import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
PATH = ROOT / "main" / "cloudzao_endpoints.c"


def main() -> int:
    if not PATH.is_file():
        print(f"missing {PATH}", file=sys.stderr)
        return 1
    text = PATH.read_text(encoding="utf-8")
    new, n = re.subn(r'=\s*"[^"]*"', '= ""', text)
    PATH.write_text(new, encoding="utf-8", newline="\n")
    print(f"blanked {n} string literal(s) in {PATH.relative_to(ROOT)}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
