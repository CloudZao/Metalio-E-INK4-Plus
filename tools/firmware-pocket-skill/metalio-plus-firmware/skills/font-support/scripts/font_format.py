# SPDX-License-Identifier: MIT
"""Portable, one-spec NTF1 codec shared by the preparation tools (stdlib only)."""
import hashlib
import struct

SOURCE_SHA = 'a3041811a78c361b1de50f953c805e0244951c21c5bd412f7232ef0d899af0da'
SOURCE_URL = ('https://raw.githubusercontent.com/google/fonts/'
              '2894aab31764f10f29c421bdfd2340d3b382d384/ofl/notosanssc/NotoSansSC%5Bwght%5D.ttf')
SIZES = (24, 28, 32, 36, 40, 48)
WEIGHTS = (400, 500, 600)
THRESHOLD = 112
# Preserve the accepted test's shared line metrics, including its unused
# fallback's larger descent at 24 and 48 px. Glyphs still come from SC only.
PLUS_METRICS = {24: (28, 8), 28: (33, 9), 32: (38, 10), 36: (42, 11),
                40: (47, 12), 48: (56, 15)}


def sha256(data):
    return hashlib.sha256(data).hexdigest()


def scalar(cp):
    return 0 <= cp <= 0x10ffff and not 0xd800 <= cp <= 0xdfff


def chars_from_text(text):
    """Keep visible characters, auto-include printable ASCII for safe UI use."""
    chars = set(range(32, 127))
    for ch in text:
        cp = ord(ch)
        if ch in '\r\n\t' or cp == 0xfeff:
            continue
        if cp < 32 or 0x7f <= cp < 0xa0 or not scalar(cp):
            raise ValueError('Unsupported control/scalar U+%04X in character file' % cp)
        chars.add(cp)
    return sorted(chars)


def default_characters():
    chars = set(range(32, 127))
    han = set()
    for first in range(0xb0, 0xf8):
        for second in range(0xa1, 0xff):
            try:
                ch = bytes((first, second)).decode('gb2312')
            except UnicodeDecodeError:
                continue
            if len(ch) == 1 and 0x4e00 <= ord(ch) <= 0x9fff:
                han.add(ord(ch))
    if len(han) != 6763:
        raise ValueError('Unexpected GB2312 codec/catalog')
    chars.update(han)
    extra = ('\u00a0。，、；：？！“”‘’（）［］【】《》〈〉「」『』〔〕'
             '…—·～￥℃°‰％＋－＝／＼＃＆＠＊＄￡！＂＇，．：；？'
             '€£¥¢©®™×÷≠≤≥−±∞√∑∏∫≈≡∈∩∪∅∧∨¬∠⊥∥'
             '①②③④⑤⑥⑦⑧⑨⑩ⅠⅡⅢⅣⅤⅥⅦⅧⅨⅩ'
             'āáǎàēéěèīíǐìōóǒòūúǔùǖǘǚǜüńňǹ'
             'ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz'
             'αβγδεζηθικλμνξοπρστυφχψωΑΒΓΔΕΖΗΘΙΚΛΜΝΞΟΠΡΣΤΥΦΧΨΩ'
             '←↑→↓↔↕●○◎■□◆◇▲△▼▽★☆✓')
    chars.update(map(ord, extra))
    return sorted(chars)


def encode(spec, glyphs):
    """spec=(px,weight,threshold,ascent,descent,line_height).

    glyph tuple=(cp,width,height,left,top,advance,continuous-msb-bitmap).
    """
    px, weight, threshold, ascent, descent, line_height = spec
    count = len(glyphs)
    data_offset = 64 + 20 * count
    entries = bytearray()
    bitmaps = bytearray()
    for cp, width, height, left, top, advance, bitmap in glyphs:
        entries.extend(struct.pack('<IIHHhhhH', cp, data_offset + len(bitmaps),
                                   width, height, left, top, advance, 0))
        bitmaps.extend(bitmap)
    result = (struct.pack('<4sHHIIIIIIII', b'NTF1', 1, 40, 1, count, 0,
                          40, 64, data_offset, data_offset, data_offset + len(bitmaps)) +
              struct.pack('<HHHHhhhhII', px, weight, threshold, 0, ascent, descent,
                          line_height, 0, 0, count) + entries + bitmaps)
    decode(result)
    return result


def decode(data):
    if len(data) < 64:
        raise ValueError('Truncated font')
    magic, version, hs, ns, ng, nc, so, go, co, do, total = struct.unpack_from('<4sHHIIIIIIII', data)
    if (magic, version, hs, ns, nc, so, go) != (b'NTF1', 1, 40, 1, 0, 40, 64):
        raise ValueError('Not the one-spec, catalog-free NTF1 profile')
    if not 1 <= ng <= 200000 or co != 64 + ng * 20 or do != co or total != len(data) or do > total:
        raise ValueError('Invalid font ranges')
    px, weight, threshold, reserved, ascent, descent, line_height, reserved2, first, count = struct.unpack_from('<HHHHhhhhII', data, 40)
    if not (1 <= px <= 256 and 1 <= weight <= 1000 and 1 <= threshold <= 255 and
            ascent > 0 and descent >= 0 and line_height >= ascent + descent and
            reserved == reserved2 == first == 0 and count == ng):
        raise ValueError('Invalid font metrics')
    glyphs = []
    end = do
    previous = -1
    for i in range(ng):
        cp, off, width, height, left, top, advance, padding = struct.unpack_from('<IIHHhhhH', data, 64 + i * 20)
        size = (width * height + 7) // 8
        if not (scalar(cp) and cp > previous and off == end and off + size <= len(data)
                and width <= 512 and height <= 512 and advance >= 0 and padding == 0
                and -top <= ascent and top + height <= descent):
            raise ValueError('Invalid glyph record U+%04X' % cp)
        bitmap = data[off:off + size]
        remainder = width * height % 8
        if remainder and bitmap and bitmap[-1] & ((1 << (8 - remainder)) - 1):
            raise ValueError('Nonzero trailing pixel bits')
        glyphs.append((cp, width, height, left, top, advance, bitmap))
        end += size
        previous = cp
    if end != len(data) or not {32, 63}.issubset({g[0] for g in glyphs}):
        raise ValueError('Trailing bytes or missing space/question mark')
    return (px, weight, threshold, ascent, descent, line_height), glyphs
