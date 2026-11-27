"""从仓库已有 Latin Modern Math 生成像素视界数字子集。"""
from pathlib import Path
import argparse
from PIL import ImageFont
from compress_lvgl_font import encode_pixels

SOURCE = Path(__file__).resolve().parents[1] / "ttf-fonts/LatinModernMath/latinmodern-math.otf"

def font_source(name, size, chars, compressed=False):
    font = ImageFont.truetype(str(SOURCE), size)
    bitmap = bytearray()
    glyphs = ["{.bitmap_index=0,.adv_w=0,.box_w=0,.box_h=0,.ofs_x=0,.ofs_y=0}"]
    ascent = descent = 0
    for char in chars:
        mask, (left, top) = font.getmask2(char, mode="L", anchor="ls")
        width, height = mask.size
        ascent = max(ascent, -top)
        descent = max(descent, top + height)
        start = len(bitmap)
        values = bytes(mask)
        if compressed:
            packed = encode_pixels([3 if value >= 128 else 0 for value in values], width, 2)
        else:
            packed = bytearray((len(values) + 7) // 8)
            for i, value in enumerate(values):
                if value >= 128:
                    packed[i // 8] |= 0x80 >> (i % 8)
        bitmap.extend(packed)
        glyphs.append("{.bitmap_index=%d,.adv_w=%d,.box_w=%d,.box_h=%d,.ofs_x=%d,.ofs_y=%d}" %
                      (start, round(font.getlength(char)*16), width, height, left, -top-height))
    data = ["alignas(4) static const uint8_t %s_bitmap[] = {" % name]
    data += [",".join("0x%02x" % n for n in bitmap[i:i+24])+"," for i in range(0,len(bitmap),24)]
    data += ["};", "static const lv_font_fmt_txt_glyph_dsc_t %s_glyphs[] = {" % name,
             ",\n".join(glyphs), "};"]
    offsets = ",".join(str(ord(c)-ord(chars[0])) for c in chars)
    data += ["static const uint16_t %s_unicode[] = {%s};" % (name, offsets),
             "static const lv_font_fmt_txt_cmap_t %s_cmap[] = {{" % name,
             ".range_start=%d,.range_length=%d,.glyph_id_start=1,.unicode_list=%s_unicode," %
             (ord(chars[0]), ord(chars[-1])-ord(chars[0])+1, name),
             ".glyph_id_ofs_list=nullptr,.list_length=%d,.type=LV_FONT_FMT_TXT_CMAP_SPARSE_TINY}};" % len(chars),
             "static const lv_font_fmt_txt_dsc_t %s_dsc = {" % name,
             ".glyph_bitmap=%s_bitmap,.glyph_dsc=%s_glyphs,.cmaps=%s_cmap," % (name,name,name),
             ".kern_dsc=nullptr,.kern_scale=0,.cmap_num=1,.bpp=%d,.kern_classes=0,.bitmap_format=%d};" %
             ((2, 1) if compressed else (1, 0)),
             "static const lv_font_t %s = {" % name,
             ".get_glyph_dsc=lv_font_get_glyph_dsc_fmt_txt,.get_glyph_bitmap=lv_font_get_bitmap_fmt_txt,",
             ".line_height=%d,.base_line=%d,.subpx=LV_FONT_SUBPX_NONE," % (ascent+descent+2, descent+1),
             ".underline_position=-1,.underline_thickness=1,.dsc=&%s_dsc," % name,
             ".fallback=nullptr,.user_data=nullptr};"]
    return "\n".join(data), len(bitmap)

def generate(output):
    clock, clock_bytes = font_source("kPixelClockFont", 240, "0123456789:", compressed=True)
    counter, counter_bytes = font_source("kPixelCounterFont", 48, "%-0123456789")
    header = ("// 由 tools/assets/generate_pixel_theme_font.py 生成；源字体许可证：tools/ttf-fonts/LatinModernMath/GUST-FONT-LICENSE.txt\n"
              "#pragma once\n#include <lvgl.h>\n" + clock + "\n" + counter + "\n")
    Path(output).write_text(header, encoding="utf-8", newline="")
    print("Pixel numeric font bitmaps:", clock_bytes+counter_bytes, "bytes")

if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("--output", required=True)
    generate(parser.parse_args().output)
