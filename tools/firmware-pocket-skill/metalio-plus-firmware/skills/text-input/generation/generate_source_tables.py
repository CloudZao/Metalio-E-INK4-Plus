"""Maintainer-only, offline rebuild from two hash-pinned wheels. Not a device dependency."""
from __future__ import annotations

import argparse
import gzip
import hashlib
import json
from pathlib import Path
import re
import sys
import tempfile
import zipfile

HERE = Path(__file__).resolve().parent
MODULE = HERE.parent
VERSION = "pinyin-4262-wordfreq311-v2"
WHEELS = {
    "wordfreq": "4b1c6ecffc6198be3396d5cf871c4423ca71c907c231348d352dd54d62b97473",
    "pypinyin": "d53b1e8ad2cdb815fb2cb604ed3123372f5a28c6f447571244aca36fc62a286f",
}


def sha(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def build(wordfreq_wheel: Path, pypinyin_wheel: Path, output: Path) -> dict:
    if output.resolve().is_relative_to(MODULE.resolve()):
        raise ValueError("Rebuild outside the installed Skill; compare before deliberate publication")
    for key, path in (("wordfreq", wordfreq_wheel), ("pypinyin", pypinyin_wheel)):
        if sha(path.read_bytes()) != WHEELS[key]:
            raise ValueError(f"{key} wheel SHA mismatch")
    import msgpack  # Authoring-only reader, not bundled or imported on device.
    if tuple(msgpack.version) != (1, 1, 2):
        raise ValueError("Rebuild reader is pinned to msgpack 1.1.2")
    chars = (HERE / "compat-characters.txt").read_text(encoding="utf-8").strip()
    if len(chars) != 4262 or chars != "".join(sorted(set(chars))):
        raise ValueError("Expected 4262 unique compatibility characters in codepoint order")
    overrides = json.loads((HERE / "common-readings.json").read_text(encoding="utf-8"))
    with zipfile.ZipFile(wordfreq_wheel) as archive:
        compressed = archive.read("wordfreq/data/large_zh.msgpack.gz")
        pack = msgpack.unpackb(gzip.decompress(compressed), raw=False)
    if pack[0] != {"format": "cB", "version": 1}:
        raise ValueError("Unexpected wordfreq cBpack format")
    # Exact single-codepoint entries only. No tokenizer or simplified-character
    # normalization; no multi-character aggregation or inherited old ranking.
    weights = {}
    wanted = set(chars)
    for bucket, terms in enumerate(pack[1:]):
        for term in terms:
            if term in wanted:
                if term in weights:
                    raise ValueError("Duplicate frequency token")
                weights[term] = bucket
    with tempfile.TemporaryDirectory(prefix="plus-pinyin-") as temp:
        with zipfile.ZipFile(pypinyin_wheel) as archive:
            for info in archive.infolist():
                if not info.filename.startswith("pypinyin/") or info.is_dir():
                    continue
                path = Path(temp, info.filename).resolve()
                if not path.is_relative_to(Path(temp).resolve()):
                    raise ValueError("Unexpected wheel member")
                path.parent.mkdir(parents=True, exist_ok=True)
                path.write_bytes(archive.read(info))
        if "pypinyin" in sys.modules:
            raise ValueError("Run generation in a fresh Python process")
        sys.path.insert(0, temp)
        import pypinyin
        if pypinyin.__version__ != "0.55.0":
            raise ValueError("Unexpected pypinyin version")
        table = {}
        for char in chars:
            default = pypinyin.lazy_pinyin(char, style=pypinyin.Style.NORMAL, strict=True)[0].replace("ü", "v")
            for key in sorted(set(overrides.get(char, default).split())):
                if not re.fullmatch(r"[a-z]{1,6}", key):
                    raise ValueError(f"Invalid reading {char}: {key}")
                table.setdefault(key, []).append(char)
        sys.path.pop(0)
    for candidates in table.values():
        candidates.sort(key=lambda c: (0, weights[c], ord(c)) if c in weights else (1, 0, ord(c)))
    data = "".join(f"{key}\t{''.join(table[key])}\n" for key in sorted(table)).encode("utf-8")
    weight_data = "character\twordfreq_negative_centibels\n" + "".join(
        f"{c}\t{-weights[c] if c in weights else 'missing'}\n" for c in chars)
    report = {
        "version": VERSION, "table_sha256": sha(data), "table_bytes": len(data),
        "syllables": len(table), "candidate_occurrences": sum(map(len, table.values())),
        "unique_characters": len(chars), "weighted_characters": len(weights),
        "missing_weight_characters": "".join(c for c in chars if c not in weights),
        "missing_policy": "Keep all characters; missing weights follow available entries, codepoint ascending",
        "ranking": "wordfreq 3.1.1 large_zh exact single-codepoint token centibel buckets, then codepoint",
        "not_claimed": "Not per-pronunciation frequency, not a pure character-count corpus, not mobile IME ranking",
        "wordfreq_data_member": "wordfreq/data/large_zh.msgpack.gz",
        "wordfreq_data_sha256": sha(compressed), "wordfreq_wheel_sha256": WHEELS["wordfreq"],
        "pypinyin_wheel_sha256": WHEELS["pypinyin"], "msgpack_reader_version": "1.1.2",
        "compat_characters_sha256": sha((HERE / "compat-characters.txt").read_bytes()),
        "common_readings_sha256": sha((HERE / "common-readings.json").read_bytes()),
        "weights_sha256": sha(weight_data.encode("utf-8")),
        "samples": {key: "".join(table[key]) for key in ("si", "shi", "ai", "an", "chi", "lv", "nv", "lve", "chong", "zhong", "hang", "xing")},
    }
    output.mkdir(parents=True, exist_ok=True)
    (output / "pinyin-candidates.tsv").write_bytes(data)
    (output / "frequency-weights.tsv").write_text(weight_data, encoding="utf-8", newline="\n")
    (output / "generation-report.json").write_text(json.dumps(report, ensure_ascii=False, indent=2) + "\n", encoding="utf-8", newline="\n")
    return report


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--wordfreq-wheel", required=True, type=Path)
    parser.add_argument("--pypinyin-wheel", required=True, type=Path)
    parser.add_argument("--output-dir", required=True, type=Path)
    args = parser.parse_args()
    print(json.dumps(build(args.wordfreq_wheel, args.pypinyin_wheel, args.output_dir), ensure_ascii=False))
