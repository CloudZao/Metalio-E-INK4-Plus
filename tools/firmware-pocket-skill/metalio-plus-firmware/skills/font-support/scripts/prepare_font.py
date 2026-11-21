#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Select a prebuilt Plus font offline, optionally subset it, emit C++ and NTF1.

Only Python's standard library is required. No font download/rasterizer/device access.
"""
import argparse
import json
from pathlib import Path
import re
import sys
import zlib

from font_format import chars_from_text, decode, encode, sha256

ROOT = Path(__file__).resolve().parents[1]
CPP_KEYWORDS = set(('alignas alignof and and_eq asm auto bitand bitor bool break case catch '
    'char char8_t char16_t char32_t class compl concept const consteval constexpr constinit '
    'const_cast continue co_await co_return co_yield decltype default delete do double '
    'dynamic_cast else enum explicit export extern false float for friend goto if inline '
    'int long mutable namespace new noexcept not not_eq nullptr operator or or_eq private '
    'protected public register reinterpret_cast requires return short signed sizeof static '
    'static_assert static_cast struct switch template this thread_local throw true try '
    'typedef typeid typename union unsigned using virtual void volatile wchar_t while xor xor_eq').split())


def read_font(assets, pixels, weight):
    manifest = json.loads((assets / 'fonts-manifest.json').read_text('utf-8'))
    if manifest.get('schema') != 'plus-noto-font-assets-v1':
        raise ValueError('Unsupported asset manifest')
    matches = [f for f in manifest['fonts'] if (f['pixels'], f['weight'], f['threshold']) == (pixels, weight, 112)]
    if len(matches) != 1:
        raise ValueError('No unique prebuilt font for %d px / %d; check assets manifest' % (pixels, weight))
    entry = matches[0]
    path = (assets / entry['file']).resolve()
    if path.parent != assets.resolve():
        raise ValueError('Font path must stay inside the selected assets directory')
    compressed = path.read_bytes()
    if len(compressed) != entry['compressed_bytes'] or sha256(compressed) != entry['compressed_sha256']:
        raise ValueError('Compressed font checksum mismatch')
    expected = entry['bytes']
    if not 64 <= expected <= 32 * 1024 * 1024:
        raise ValueError('Font size is outside supported bounds')
    decoder = zlib.decompressobj()
    data = decoder.decompress(compressed, expected + 1)
    if len(data) != expected or not decoder.eof or decoder.unused_data or decoder.unconsumed_tail or sha256(data) != entry['sha256']:
        raise ValueError('Font decompression/checksum mismatch')
    spec, glyphs = decode(data)
    if spec[:3] != (pixels, weight, 112) or len(glyphs) != entry['glyphs']:
        raise ValueError('Font metadata mismatch')
    return data, spec, glyphs, manifest, entry


def c_literal(text):
    # Encode every non-ASCII byte using octal to avoid C++ hex escape ambiguity.
    return '"' + ''.join(('\\%03o' % b) if b < 32 or b >= 127 or b in (34, 92) else chr(b)
                         for b in text.encode('utf-8')) + '"'


def prepare(args):
    if (not re.fullmatch(r'[A-Za-z][A-Za-z0-9_]*', args.symbol_prefix)
            or '__' in args.symbol_prefix or args.symbol_prefix in CPP_KEYWORDS):
        raise ValueError('symbol-prefix must be a non-reserved C++ identifier')
    data, spec, glyphs, source_manifest, entry = read_font(args.assets, args.size, args.weight)
    if args.chars_file:
        requested = set(chars_from_text(args.chars_file.read_text('utf-8-sig')))
        present = {g[0] for g in glyphs}
        missing = sorted(requested - present)
        if missing:
            raise ValueError('Prebuilt font lacks requested characters: ' + ' '.join('U+%04X' % cp for cp in missing))
        glyphs = [g for g in glyphs if g[0] in requested]
        data = encode(spec, glyphs)
    licenses = ROOT / 'licenses'
    ofl = (licenses / 'OFL.txt').read_text('utf-8')
    notice = (licenses / 'FONT-NOTICE.txt').read_text('utf-8')
    if 'SIL OPEN FONT LICENSE' not in ofl or 'Copyright 2014-2021 Adobe' not in ofl:
        raise ValueError('Required original font license/copyright is missing')
    args.output.mkdir(parents=True, exist_ok=True)
    if args.binary_only and any((args.output / name).exists() for name in ('font.h', 'font.cc')):
        raise ValueError('Binary-only output needs a folder without old font.h/font.cc; choose a new folder')
    outputs = ['font.ntf', 'font-manifest.json', 'OFL.txt', 'FONT-NOTICE.txt']
    if not args.binary_only:
        outputs += ['font.h', 'font.cc']
    for name in outputs:
        if (args.output / name).exists() and not args.overwrite:
            raise ValueError('Output exists: %s; choose a new folder or use --overwrite' % (args.output / name))
    (args.output / 'font.ntf').write_bytes(data)
    (args.output / 'OFL.txt').write_text(ofl, encoding='utf-8', newline='\n')
    (args.output / 'FONT-NOTICE.txt').write_text(notice, encoding='utf-8', newline='\n')
    prefix = args.symbol_prefix
    if not args.binary_only:
        header = ('// Font data: OFL-1.1. See OFL.txt and FONT-NOTICE.txt.\n#pragma once\n'
                  '#include <cstddef>\n#include <cstdint>\n'
                  'extern const uint8_t %s[];\nextern const std::size_t %s_size;\n'
                  'extern const char %s_license[];\n' % (prefix, prefix, prefix))
        (args.output / 'font.h').write_text(header, encoding='utf-8', newline='\n')
        with (args.output / 'font.cc').open('w', encoding='utf-8', newline='\n') as out:
            out.write('// Font data: OFL-1.1. See font.h and the original license.\n#include "font.h"\n')
            out.write('alignas(4) const uint8_t %s[] = {\n' % prefix)
            for start in range(0, len(data), 16):
                out.write('    ' + ','.join('0x%02x' % b for b in data[start:start + 16]) + ',\n')
            out.write('};\nconst std::size_t %s_size = sizeof(%s);\n' % (prefix, prefix))
            out.write('const char %s_license[] =\n' % prefix)
            for line in (notice + '\n' + ofl).splitlines(keepends=True):
                out.write('    ' + c_literal(line) + '\n')
            out.write('    ;\n')
    report = {'schema': 'plus-noto-font-prepared-v1', 'derivative_name': 'Plus Mono Sans',
              'pixels': spec[0], 'weight': spec[1], 'threshold': spec[2],
              'ascent': spec[3], 'descent': spec[4], 'line_height': spec[5],
              'glyphs': len(glyphs), 'charset': ['U+%04X' % g[0] for g in glyphs],
              'bytes': len(data), 'sha256': sha256(data), 'source_sha256': source_manifest['source_sha256'],
              'prebuilt_font_sha256': entry['sha256'], 'missing_requested_characters': [],
              'subset': bool(args.chars_file), 'symbol_prefix': prefix,
              'license': 'OFL-1.1', 'font_license_files': ['OFL.txt', 'FONT-NOTICE.txt'],
              'binary_only': args.binary_only,
              'embedded_license_symbol': None if args.binary_only else prefix + '_license',
              'files': outputs}
    (args.output / 'font-manifest.json').write_text(json.dumps(report, ensure_ascii=False, indent=2) + '\n', encoding='utf-8', newline='\n')
    print(json.dumps({key: report[key] for key in ('pixels', 'weight', 'threshold', 'glyphs', 'bytes', 'sha256', 'subset', 'font_license_files')}, ensure_ascii=False))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--size', type=int, default=32, help='Prebuilt native pixel size (default 32)')
    parser.add_argument('--weight', type=int, default=500, help='400 regular / 500 medium / 600 semibold (default 500)')
    parser.add_argument('--output', required=True, type=Path, help='Destination directory; not a firmware BIN')
    parser.add_argument('--chars-file', type=Path, help='Optional UTF-8 fixed UI text/character list; ASCII is always included')
    parser.add_argument('--symbol-prefix', default='plus_font_data', help='C++ symbol prefix (default plus_font_data)')
    parser.add_argument('--assets', type=Path, default=ROOT / 'assets' / 'fonts', help='Prebuilt fonts directory (normally inferred from this package)')
    parser.add_argument('--binary-only', action='store_true', help='Emit NTF and licensing files, omit C++ arrays; caller must expose license')
    parser.add_argument('--overwrite', action='store_true', help='Replace only the named generated output files')
    try:
        prepare(parser.parse_args())
    except (ValueError, OSError, KeyError, zlib.error) as exc:
        parser.exit(2, 'error: %s\n' % exc)


if __name__ == '__main__':
    main()
