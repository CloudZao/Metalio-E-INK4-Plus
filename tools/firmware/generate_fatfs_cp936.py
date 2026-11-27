"""从当前 ESP-IDF FatFS 表生成 CP936 只读分块映射，不修改 SDK。"""
import argparse
from pathlib import Path
import re
import struct


def compact_table(source, name):
    body = re.search(r"\b" + name + r"\[\]\s*=\s*\{(.*?)\};", source, re.S)[1]
    body = re.sub(r"/\*.*?\*/", "", body, flags=re.S)
    values = [int(value.strip(), 0) for value in body.split(",") if value.strip()]
    pairs = dict(zip(values[::2], values[1::2]))
    pairs.pop(0, None)
    offsets = []
    blocks = bytearray()
    for base in range(0, 65536, 64):
        mask = sum(1 << bit for bit in range(64) if base + bit in pairs)
        if mask == 0:
            offsets.append(0xffff)
            continue
        offsets.append(len(blocks) // 2)
        blocks.extend(struct.pack("<Q", mask))
        for bit in range(64):
            if mask & (1 << bit):
                blocks.extend(struct.pack("<H", pairs[base + bit]))
    if len(blocks) // 2 > 65535:
        raise ValueError("CP936 table exceeds 16-bit block offset")
    return offsets, blocks


def c_array(name, c_type, values, width):
    lines = [f"static const {c_type} {name}[] = {{"]
    lines.extend("    " + ", ".join(f"0x{v:0{width}x}" for v in values[i:i + 16]) + ","
                 for i in range(0, len(values), 16))
    return "\n".join(lines + ["};", ""])


def generate(source):
    # 保留上游数据的授权声明；大小写转换仍由 SDK 原模块提供。
    output = source[:source.index('#include "ff.h"')]
    output += '#include <stdint.h>\n#include <string.h>\n#include "ff.h"\n\n'
    output += '#if FF_CODE_PAGE != 936\n#error "Compact FatFS table requires CP936"\n#endif\n\n'
    for source_name, name in (("uni2oem936", "kCp936Unicode"), ("oem2uni936", "kCp936Oem")):
        offsets, blocks = compact_table(source, source_name)
        output += c_array(name + "Offsets", "uint16_t", offsets, 4)
        output += c_array(name + "Blocks", "uint8_t", blocks, 2)
    output += r'''
static WCHAR Cp936Lookup(uint16_t code, const uint16_t *offsets, const uint8_t *blocks)
{
    uint16_t offset = offsets[code >> 6];
    if (offset == UINT16_MAX) {
        return 0;
    }
    const uint8_t *block = blocks + (uint32_t)offset * 2;
    uint64_t mask;
    memcpy(&mask, block, sizeof(mask));
    uint64_t bit = UINT64_C(1) << (code & 63);
    if ((mask & bit) == 0) {
        return 0;
    }
    unsigned index = __builtin_popcountll(mask & (bit - 1));
    uint16_t value;
    memcpy(&value, block + sizeof(mask) + index * sizeof(value), sizeof(value));
    return value;
}

/** @brief 保持 FatFS 的 Unicode 转 CP936 接口及错误返回值。 */
WCHAR __wrap_ff_uni2oem(DWORD unicode, WORD codepage)
{
    if (unicode < 0x80) {
        return (WCHAR)unicode;
    }
    if (unicode >= 0x10000 || codepage != 936) {
        return 0;
    }
    return Cp936Lookup((uint16_t)unicode, kCp936UnicodeOffsets, kCp936UnicodeBlocks);
}

/** @brief 保持 FatFS 的 CP936 转 Unicode 接口及错误返回值。 */
WCHAR __wrap_ff_oem2uni(WCHAR oem, WORD codepage)
{
    if (oem < 0x80) {
        return oem;
    }
    if (codepage != 936) {
        return 0;
    }
    return Cp936Lookup(oem, kCp936OemOffsets, kCp936OemBlocks);
}
'''
    return output


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--input", required=True, type=Path)
    parser.add_argument("--output", required=True, type=Path)
    args = parser.parse_args()
    args.output.write_text(generate(args.input.read_text(encoding="utf-8")), encoding="utf-8", newline="\n")
