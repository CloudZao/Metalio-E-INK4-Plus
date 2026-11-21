#!/usr/bin/env python3
"""Validate the frozen pinyin table and optionally emit a byte-exact C++ header."""
from __future__ import annotations

import argparse
import hashlib
import json
import re
import shutil
from pathlib import Path

TABLE_VERSION = "pinyin-4262-wordfreq311-v2"
TABLE_SHA256 = "0169c1a2f0c39094f24b066d572f26485f2c01f63f42b237bf21d8d9ff70db5a"
MODULE_ROOT = Path(__file__).resolve().parents[1]
SKILL_ROOT = Path(__file__).resolve().parents[3]


def load_table(path: Path) -> list[tuple[str, str]]:
    raw = path.read_bytes()
    if len(raw) != 15348 or hashlib.sha256(raw).hexdigest() != TABLE_SHA256:
        raise ValueError("Frozen " + TABLE_VERSION + " size/SHA mismatch; do not silently regenerate the dictionary")
    if raw.startswith(b"\xef\xbb\xbf") or b"\r" in raw or not raw.endswith(b"\n"):
        raise ValueError("Expected UTF-8 without BOM, LF, final newline")
    rows = []
    for line in raw.decode("utf-8").splitlines():
        fields = line.split("\t")
        if len(fields) != 2:
            raise ValueError("Each row must have exactly two fields")
        key, chars = fields
        if not re.fullmatch(r"[a-z]{1,6}", key) or not chars:
            raise ValueError("Invalid key or empty candidates")
        if len(set(chars)) != len(chars) or any(not 0x4E00 <= ord(c) <= 0x9FFF for c in chars):
            raise ValueError("Invalid or duplicate character within one syllable")
        rows.append((key, chars))
    keys = [key for key, _ in rows]
    all_chars = "".join(chars for _, chars in rows)
    if keys != sorted(set(keys)) or len(rows) != 402 or len(all_chars) != 4415 or len(set(all_chars)) != 4262:
        raise ValueError("Frozen table structural counts do not match")
    return rows


def make_header(rows: list[tuple[str, str]]) -> bytes:
    lines = [
        "// Generated from " + TABLE_VERSION + "; preserve syllables, characters and ordering.",
        "// Frequency-derived data: CC-BY-SA-4.0; wordfreq 3.1.1, Robyn Speer et al.",
        "// Pinyin mapping: pypinyin/pinyin-data (MIT). Ship the module licenses/ with derived data.",
        "// SHA256 " + TABLE_SHA256,
        "#pragma once",
        "#include <cstddef>",
        "namespace metalio_plus_ime {",
        "struct Entry { const char* syllable; const char* candidates_utf8; };",
        "static constexpr Entry kPinyin[] = {",
    ]
    for key, chars in rows:
        escaped = "".join(f"\\x{byte:02x}" for byte in chars.encode("utf-8"))
        lines.append(f'    {{"{key}", "{escaped}"}},')
    lines += [
        "};",
        "static constexpr std::size_t kPinyinCount = sizeof(kPinyin) / sizeof(kPinyin[0]);",
        "} // namespace metalio_plus_ime",
        "",
    ]
    return "\n".join(lines).encode("ascii")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--table", type=Path, default=MODULE_ROOT / "assets" / "pinyin-candidates.tsv")
    parser.add_argument("--header", type=Path, help="Optional C++ header, outside the installed Skill")
    parser.add_argument("--force", action="store_true", help="Replace the generated header")
    parser.add_argument("--notices-dir", type=Path, help="Export license texts and the corresponding data outside the Skill")
    args = parser.parse_args()
    try:
        source = args.table.resolve(strict=True)
        rows = load_table(source)
        header_hash = None
        if args.header:
            output = args.header.resolve()
            if output == source or output.is_relative_to(SKILL_ROOT):
                raise ValueError("Generated header must be outside the installed Skill and input")
            if output.exists() and (not args.force or not output.is_file()):
                raise ValueError("Header exists; choose a fresh path or use --force")
            header = make_header(rows)
            output.parent.mkdir(parents=True, exist_ok=True)
            output.write_bytes(header)
            header_hash = hashlib.sha256(header).hexdigest()
        if args.notices_dir:
            notices = args.notices_dir.resolve()
            if notices.is_relative_to(SKILL_ROOT):
                raise ValueError("Notices must be exported outside the installed Skill")
            files = [(p, p.name) for p in (MODULE_ROOT / "licenses").iterdir() if p.is_file()]
            files += [(MODULE_ROOT / "assets/pinyin-candidates.tsv", "pinyin-candidates.tsv")]
            files += [(MODULE_ROOT / "generation" / name, name) for name in
                      ("frequency-weights.tsv", "generation-report.json", "sources.json")]
            # Preflight conflicts before writing any exported file.
            for original, name in files:
                target = notices / name
                if target.exists() and (not target.is_file() or target.read_bytes() != original.read_bytes()):
                    raise ValueError("Different notice already exists: " + str(target))
            notices.mkdir(parents=True, exist_ok=True)
            for original, name in files:
                shutil.copyfile(original, notices / name)
        print(json.dumps({"version": TABLE_VERSION, "sha256": TABLE_SHA256,
                          "syllables": len(rows), "candidate_occurrences": 4415,
                          "unique_characters": 4262, "header_sha256": header_hash}))
        return 0
    except (OSError, ValueError) as exc:
        parser.exit(2, f"ERROR: {exc}\n")


if __name__ == "__main__":
    raise SystemExit(main())
