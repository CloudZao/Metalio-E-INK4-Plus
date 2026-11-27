#!/usr/bin/env python3
"""Build an indexed SD pinyin/English dictionary from pinned sources and font coverage."""
import argparse
import gzip
import hashlib
import json
import re
import shutil
import struct
import sys
import zipfile
from collections import defaultdict
from pathlib import Path

ROOT = Path(__file__).resolve().parents[3]
SOURCE = ROOT / 'tools/firmware-pocket-skill/metalio-plus-firmware/skills/text-input'
RECORD = struct.Struct('<64sII')
HEADER = struct.Struct('<8s6I')


def font_characters(path):
    with path.open('rb') as f:
        header = f.read(64)
        if header.startswith(b'EPDFONT\0'):
            h = struct.unpack_from('<8sHHHHhhIIIII', header)
            if h[1] != 1:
                raise ValueError('Unsupported EF version')
            f.seek(h[9])
            result = set()
            for _ in range(h[7]):
                first, last, _ = struct.unpack('<III', f.read(12))
                result.update(range(first, last + 1))
            return result
        magic, version, _, count, index, _, _ = struct.unpack_from('<4sHHIQQI', header)
        if magic != b'EFNT' or version != 1:
            raise ValueError('Unsupported fontpack')
        f.seek(index)
        result = set()
        for _ in range(count):
            key, _, _, _ = struct.unpack('<QIHH', f.read(16))
            if (key >> 16) & 0xffff == 25 and key & 0xffff == 2:
                result.add(key >> 32)
        return result


def is_han(cp):
    return any(first <= cp <= last for first, last in [(0x3400, 0x4dbf), (0x4e00, 0x9fff),
                                                       (0xf900, 0xfaff), (0x20000, 0x2fa1f),
                                                       (0x30000, 0x323af)])


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--sources', type=Path, required=True)
    parser.add_argument('--font', type=Path, action='append', required=True)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    sys.path.insert(0, str((args.sources / 'lib').resolve()))
    import msgpack
    from pypinyin import Style, lazy_pinyin, pinyin
    from pypinyin.pinyin_dict import pinyin_dict
    from pypinyin.phrases_dict import phrases_dict
    manifest = json.loads((SOURCE / 'generation/sources.json').read_text(encoding='utf-8'))
    for item in manifest['inputs']:
        raw = (args.sources / item['filename']).read_bytes()
        if hashlib.sha256(raw).hexdigest() != item['sha256']:
            raise ValueError('Source checksum mismatch: ' + item['name'])
    coverage = set()
    for path in args.font:
        coverage.update(font_characters(path))
    print('Font codepoints:', len(coverage), flush=True)
    with zipfile.ZipFile(args.sources / 'wordfreq-3.1.1-py3-none-any.whl') as wheel:
        zh = msgpack.unpackb(gzip.decompress(wheel.read('wordfreq/data/large_zh.msgpack.gz')), raw=False)
        en = msgpack.unpackb(gzip.decompress(wheel.read('wordfreq/data/large_en.msgpack.gz')), raw=False)
    frequencies = {word: rank for rank, bucket in enumerate(zh[1:]) for word in bucket}
    exact = defaultdict(dict)
    predicted = defaultdict(dict)
    english = defaultdict(dict)
    character_count = 0
    for cp in sorted(pinyin_dict):
        if cp not in coverage:
            continue
        char = chr(cp)
        readings = pinyin(char, style=Style.NORMAL, heteronym=True, errors='ignore')
        if not readings:
            continue
        valid = [r for r in readings[0] if re.fullmatch('[a-z]{1,6}', r)]
        if valid:
            character_count += 1
        for key in valid:
            exact[key][char] = (frequencies.get(char, 1000), char)
    phrases = [word for bucket in zh[1:] for word in bucket if 2 <= len(word) <= 8][:150000]
    phrases.extend(phrases_dict)
    seen = set()
    for word in phrases:
        if word in seen or not 2 <= len(word) <= 8 or any(ord(c) not in coverage for c in word):
            continue
        syllables = lazy_pinyin(word, style=Style.NORMAL, errors='ignore')
        key = ''.join(syllables)
        if not re.fullmatch('[a-z]{2,63}', key) or len(syllables) != len(word) or any(not is_han(ord(c)) for c in word):
            continue
        seen.add(word)
        rank = frequencies.get(word, 1100)
        exact[key][word] = (rank, word)
        for end in range(2, len(word)):
            prefix = word[:end]
            prefix_key = ''.join(syllables[:end])
            prefix_rank = frequencies.get(prefix, rank + 50)
            old = exact[prefix_key].get(prefix)
            if old is None or prefix_rank < old[0]:
                exact[prefix_key][prefix] = (prefix_rank, prefix)
        for length in range(2, len(key)):
            group = predicted[key[:length]]
            if len(group) < 8 or word in group:
                group[word] = (rank, word)
    words = []
    for bucket in en[1:]:
        for word in bucket:
            if re.fullmatch('[a-z]{2,24}', word) and all(ord(c) in coverage for c in word):
                words.append(word)
                if len(words) >= 60000:
                    break
        if len(words) >= 60000:
            break
    for rank, word in enumerate(words):
        for length in range(1, len(word) + 1):
            group = english[word[:length]]
            if len(group) < 12:
                group[word] = rank
    keys = sorted(set(exact) | set(predicted) | set(english))
    args.output.parent.mkdir(parents=True, exist_ok=True)
    index_size = len(keys) * RECORD.size
    payload_start = HEADER.size + index_size
    with args.output.open('wb') as output:
        output.seek(payload_start)
        entries = []
        for key in keys:
            ranked = sorted(exact.get(key, {}).items(), key=lambda item: item[1])
            candidates = [word for word, _ in ranked]
            candidates += [word for word, _ in sorted(predicted.get(key, {}).items(), key=lambda item: item[1])
                           if word not in candidates]
            candidates += [word for word, _ in sorted(english.get(key, {}).items(), key=lambda item: item[1])
                           if word not in candidates]
            encoded = ' '.join(candidates).encode('utf-8')
            if len(encoded) > 32768:
                raise ValueError('Candidate payload exceeds runtime limit: ' + key)
            position = output.tell()
            output.write(encoded)
            entries.append((key.encode('ascii'), position, len(encoded)))
        size = output.tell()
        output.seek(0)
        output.write(HEADER.pack(b'PINYIN2\0', 2, len(keys), HEADER.size, payload_start, size, 0))
        for entry in entries:
            output.write(RECORD.pack(*entry))
    notices = args.output.parent / 'pinyin-licenses'
    notices.mkdir(exist_ok=True)
    for file in (SOURCE / 'licenses').iterdir():
        if file.is_file():
            shutil.copyfile(file, notices / file.name)
    phrase_count = len({word for group in exact.values() for word in group if len(word) > 1})
    report = dict(version=2, characters=character_count, phrases=phrase_count, english_words=len(words),
                  lookup_keys=len(keys), bytes=size, sha256=hashlib.sha256(args.output.read_bytes()).hexdigest(),
                  mapping='pypinyin 0.55.0, MIT', ranking='wordfreq 3.1.1, CC BY-SA 4.0',
                  changes=('Expanded to supplied font coverage; all supported character readings; '
                           'phrase-boundary and English prefix candidates; indexed binary layout. '
                           'No font glyphs redistributed.'))
    (notices / 'ATTRIBUTION.md').write_text(
        "# Pinyin dictionary v2 / 拼音词库 v2\n\n"
        "Mapping and phrase pronunciations: pypinyin 0.55.0 and upstream pinyin-data / phrase-pinyin-data (MIT).\n"
        "Frequency-derived ranking: wordfreq 3.1.1, Robyn Speer and its data contributors (CC BY-SA 4.0). "
        "SUBTLEX and its authors are credited in wordfreq-3.1.1-data-notice.txt; SUBTLEX remains freely available.\n\n"
        "Changes: expanded character coverage using supplied font inventories, retained all supported readings, "
        "added Chinese phrases and phrase-boundary prefixes, and English word completions. "
        "The derived data is distributed under CC BY-SA 4.0 with the mapping's MIT notices retained. "
        "No font glyphs are redistributed. This does not relicense the firmware.\n\n"
        "改动：按所提供字库的字符覆盖扩充可用汉字，保留支持的多音读法，加入中文词组、词组边界前缀和英文补全。"
        "词频派生数据按 CC BY-SA 4.0 分发，并保留映射部分的 MIT 声明；不分发字体字形，不改变固件代码许可。\n"
        "\nSee dictionary-report.json and sources.json, plus the full licenses in this directory.\n",
        encoding='utf-8')
    source_report = dict(inputs=manifest['inputs'], license_sources=manifest['license_sources'])
    (notices / 'sources.json').write_text(json.dumps(source_report, indent=2), encoding='utf-8')
    (notices / 'dictionary-report.json').write_text(json.dumps(report, ensure_ascii=False, indent=2), encoding='utf-8')
    print(json.dumps(report, ensure_ascii=False), flush=True)


if __name__ == '__main__':
    main()
