"""将快捷控制原始 PNG 无损转换为 Flash 中的 LVGL 灰度透明图像。"""

import argparse
import zlib
from pathlib import Path

from PIL import Image


def emit(lines, symbol, pixels, width, height, mask):
    stride = width if mask else width * 2
    rows = 16 if mask else height
    offsets = [0]
    packed = b''
    for y in range(0, height, rows):
        tile = zlib.compress(pixels[y * stride:min(y + rows, height) * stride], 9)
        assert zlib.decompress(tile) == pixels[y * stride:min(y + rows, height) * stride]
        packed += tile
        offsets.append(len(packed))
    lines.append(f'alignas(4) static const uint8_t {symbol}Pixels[] = {{')
    lines.extend(','.join(map(str, packed[i:i+32])) + ',' for i in range(0, len(packed), 32))
    lines.append('};')
    lines.append(f'static const uint32_t {symbol}Offsets[] = {{' + ','.join(map(str, offsets)) + '};')
    cf = 'LV_COLOR_FORMAT_A8' if mask else 'LV_COLOR_FORMAT_AL88'
    lines.append(f'const HomeIconCompressed {symbol} = {{ .image = {{ .header = {{'
                 f'.magic=LV_IMAGE_HEADER_MAGIC, .cf=LV_COLOR_FORMAT_RAW, '
                 f'.flags=LV_IMAGE_FLAGS_USER8 | LV_IMAGE_FLAGS_USER7, '
                 f'.w={width}, .h={height}, .stride={stride}, .reserved_2=0}}, '
                 f'.data_size=sizeof({symbol}Pixels), .data={symbol}Pixels, '
                 '.reserved=nullptr, .reserved_2=nullptr }, '
                 f'.tile_offsets={symbol}Offsets, .color_format={cf} }};')

def generate(assets, output):
    lines = ['// 由 tools/generate_quick_control_icons.py 生成；保留原始尺寸及透明留白。',
             '#include "quick_control_icons.h"']
    for filename, symbol in [('cat_paw.png', 'kQuickCatPawImage'),
                             ('light.png', 'kQuickBrightnessImage'),
                             ('color_temp.png', 'kQuickColorTempMask')]:
        with Image.open(assets / filename) as source:
            image = source.convert('RGBA')
            rgba = image.tobytes()
            if any(rgba[i] != rgba[i + 1] or rgba[i] != rgba[i + 2]
                   for i in range(0, len(rgba), 4)):
                raise ValueError(f'{filename} 必须为灰度图像')
            is_mask = filename != 'light.png'
            if filename == 'cat_paw.png' and any(rgba[i + 3] and rgba[i] != 255 for i in range(0, len(rgba), 4)):
                raise ValueError('猫爪必须为白色透明图像')
            pixels = bytes(rgba[3::4]) if is_mask else bytes(
                value for i in range(0, len(rgba), 4) for value in (rgba[i], rgba[i + 3]))
        emit(lines, symbol, pixels, image.width, image.height, is_mask)
        if not is_mask:
            emit(lines, symbol.replace('Image', 'Mask'), rgba[3::4], image.width, image.height, True)
    output.parent.mkdir(parents=True, exist_ok=True)
    output.write_text('\n'.join(lines) + '\n', encoding='utf-8')


if __name__ == '__main__':
    parser = argparse.ArgumentParser()
    parser.add_argument('--assets', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    generate(args.assets, args.output)
