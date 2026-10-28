"""将快捷控制原始 PNG 无损转换为 Flash 中的 LVGL 灰度透明图像。"""

import argparse
from pathlib import Path

from PIL import Image


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
        lines.append(f'alignas(4) static const uint8_t {symbol}Pixels[] = {{')
        lines.extend(','.join(str(value) for value in pixels[i:i + 32]) + ','
                     for i in range(0, len(pixels), 32))
        lines.append('};')
        color_format = 'LV_COLOR_FORMAT_A8' if is_mask else 'LV_COLOR_FORMAT_AL88'
        stride = image.width if is_mask else image.width * 2
        lines.append(f'const lv_image_dsc_t {symbol} = {{ .header = {{'
                     f'.magic=LV_IMAGE_HEADER_MAGIC, .cf={color_format}, .flags=0, '
                     f'.w={image.width}, .h={image.height}, .stride={stride}, .reserved_2=0}}, '
                     f'.data_size=sizeof({symbol}Pixels), .data={symbol}Pixels, '
                     '.reserved=nullptr, .reserved_2=nullptr };')
        if not is_mask:
            mask = symbol.replace('Image', 'Mask')
            alpha = rgba[3::4]
            lines.append(f'alignas(4) static const uint8_t {mask}Pixels[] = {{')
            lines.extend(','.join(str(value) for value in alpha[i:i + 32]) + ','
                         for i in range(0, len(alpha), 32))
            lines.append('};')
            lines.append(f'const lv_image_dsc_t {mask} = {{ .header = {{'
                         f'.magic=LV_IMAGE_HEADER_MAGIC, .cf=LV_COLOR_FORMAT_A8, .flags=0, '
                         f'.w={image.width}, .h={image.height}, .stride={image.width}, .reserved_2=0}}, '
                         f'.data_size=sizeof({mask}Pixels), .data={mask}Pixels, '
                         '.reserved=nullptr, .reserved_2=nullptr };')
    output.parent.mkdir(parents=True, exist_ok=True)
    output.write_text('\n'.join(lines) + '\n', encoding='utf-8')


if __name__ == '__main__':
    parser = argparse.ArgumentParser()
    parser.add_argument('--assets', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    generate(args.assets, args.output)
