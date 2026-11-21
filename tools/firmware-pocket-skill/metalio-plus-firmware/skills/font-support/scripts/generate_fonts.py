#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Optional reproducible font generation; normal users only need prepare_font.py."""
import argparse
import json
from pathlib import Path
import sys
import zlib

from font_format import (SOURCE_SHA, SOURCE_URL, SIZES, WEIGHTS, THRESHOLD,
                         PLUS_METRICS, chars_from_text, default_characters, encode,
                         sha256)


def csv_ints(text):
    try:
        values = sorted(set(int(part) for part in text.split(',')))
    except ValueError as exc:
        raise argparse.ArgumentTypeError('Expected comma-separated integers') from exc
    if not values:
        raise argparse.ArgumentTypeError('Empty selection')
    return values


def generate(args):
    try:
        from PIL import Image, ImageFont, features
        import fontTools
        from fontTools.ttLib import TTFont
    except ImportError as exc:
        raise ValueError('Optional generator requires Pillow and fonttools; prepare_font.py does not.') from exc
    source = args.source.read_bytes()
    if sha256(source) != SOURCE_SHA:
        raise ValueError('Source font SHA256 differs from the accepted, pinned Noto Sans SC file')
    if any(px < 16 or px > 64 for px in args.sizes) or any(w not in WEIGHTS for w in args.weights):
        raise ValueError('Sizes must be 16..64 px, weights must be 400, 500 or 600')
    codes = chars_from_text(args.chars_file.read_text('utf-8-sig')) if args.chars_file else default_characters()
    table = TTFont(args.source)
    cmap = table.getBestCmap()
    missing = [cp for cp in codes if cp not in cmap]
    if missing:
        raise ValueError('Source lacks: ' + ' '.join('U+%04X' % cp for cp in missing))
    args.output.mkdir(parents=True, exist_ok=True)
    charset = ''.join(map(chr, codes))
    (args.output / 'characters.txt').write_text(charset, encoding='utf-8', newline='\n')
    manifest = {
        'schema': 'plus-noto-font-assets-v1', 'derivative_name': 'Plus Mono Sans',
        'source_url': SOURCE_URL, 'source_sha256': SOURCE_SHA, 'license': 'OFL-1.1',
        'bpp': 1, 'threshold': THRESHOLD, 'bit_order': 'continuous-msb-first-one-is-ink',
        'charset_file': 'characters.txt', 'charset_sha256': sha256(charset.encode('utf-8')),
        'character_count': len(codes), 'han_count': sum(0x4e00 <= cp <= 0x9fff for cp in codes),
        'tools': {'python': sys.version.split()[0], 'pillow': Image.__version__,
                  'fonttools': fontTools.version, 'freetype': features.version('freetype2'),
                  'zlib': zlib.ZLIB_RUNTIME_VERSION},
        'metric_policy': 'Accepted Plus line metrics at 24/28/32/36/40/48; natural SC metrics otherwise; expand only if glyph needs it.',
        'generation': 'ls baseline; natural bearings; advance=int(getlength+0.5); coverage>=112; no resize, no fallback.',
        'fonts': []}
    for px in args.sizes:
        for weight in args.weights:
            face = ImageFont.truetype(str(args.source), px)
            face.set_variation_by_axes([weight if axis.axisTag == 'wght' else axis.defaultValue
                                        for axis in table['fvar'].axes])
            ascent, descent = face.getmetrics()
            old_ascent, old_descent = PLUS_METRICS.get(px, (ascent, descent))
            ascent, descent = max(ascent, old_ascent), max(descent, old_descent)
            glyphs = []
            for cp in codes:
                mask, (left, top) = face.getmask2(chr(cp), mode='L', anchor='ls')
                width, height = mask.size
                advance = int(face.getlength(chr(cp)) + 0.5)
                bitmap = bytearray((width * height + 7) // 8)
                for i, coverage in enumerate(mask):
                    if coverage >= THRESHOLD:
                        bitmap[i // 8] |= 0x80 >> (i & 7)
                # Pixel-equivalence validation is independent of byte alignment.
                if any(bool(bitmap[i // 8] & (0x80 >> (i & 7))) != (v >= THRESHOLD)
                       for i, v in enumerate(mask)):
                    raise ValueError('Pixel encoding mismatch')
                ascent, descent = max(ascent, -top), max(descent, top + height)
                glyphs.append((cp, width, height, left, top, advance, bytes(bitmap)))
            spec = (px, weight, THRESHOLD, ascent, descent, ascent + descent + 2)
            data = encode(spec, glyphs)
            compressed = zlib.compress(data, 9)
            name = 'plus-mono-%d-%d-112.ntf.zlib' % (px, weight)
            (args.output / name).write_bytes(compressed)
            manifest['fonts'].append(dict(file=name, pixels=px, weight=weight,
                threshold=THRESHOLD, ascent=ascent, descent=descent, line_height=spec[5],
                glyphs=len(glyphs), bytes=len(data), sha256=sha256(data),
                compressed_bytes=len(compressed), compressed_sha256=sha256(compressed)))
            print('%s: %d bytes, compressed %d' % (name, len(data), len(compressed)), flush=True)
    (args.output / 'fonts-manifest.json').write_text(json.dumps(manifest, ensure_ascii=False, indent=2) + '\n', encoding='utf-8', newline='\n')
    print('Generated %d fonts; all source coverage pixels and NTF1 records verified.' % len(manifest['fonts']))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--source', required=True, type=Path, help='Pinned NotoSansSC-wght.ttf, SHA checked; no automatic download')
    parser.add_argument('--output', required=True, type=Path, help='Output assets/fonts directory')
    parser.add_argument('--sizes', type=csv_ints, default=list(SIZES), help='Comma-separated pixel sizes (default 24,28,32,36,40,48)')
    parser.add_argument('--weights', type=csv_ints, default=list(WEIGHTS), help='Comma-separated weights (default 400,500,600)')
    parser.add_argument('--chars-file', type=Path, help='UTF-8 character/text file; otherwise 6763 GB2312 Han plus common symbols')
    args = parser.parse_args()
    try:
        generate(args)
    except (ValueError, OSError) as exc:
        parser.exit(2, 'error: %s\n' % exc)


if __name__ == '__main__':
    main()
