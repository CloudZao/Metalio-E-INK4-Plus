/**
 * @brief RISC-V：原 Xtensa SIMD（diff.S / lut.S）的等价 C 实现（收紧热路径）
 */
#include <esp_attr.h>
#include <stdint.h>
#include <string.h>

/**
 * @brief 将 to/from 两行 4bpp 交织为差分行，并更新列脏标记
 * @param to 目标帧一行（2 像素/字节）
 * @param from 当前帧一行（2 像素/字节）
 * @param interlaced 输出差分（1 像素/字节）
 * @param col_dirtyness 列脏字节（与 to 同行宽）
 * @param fb_width 像素宽度
 * @return 非 0 表示本行有差异
 */
uint32_t epd_interlace_4bpp_line_VE(
    const uint8_t* to,
    const uint8_t* from,
    uint8_t* interlaced,
    uint8_t* col_dirtyness,
    int fb_width
) {
    uint32_t dirty = 0;
    if (to == NULL || from == NULL || interlaced == NULL || col_dirtyness == NULL || fb_width <= 0) {
        return 0;
    }
    // 两像素一字节：成对处理，少一次 x/2、x%2
    const int pairs = fb_width >> 1;
    for (int p = 0; p < pairs; p++) {
        const uint8_t tb = to[p];
        const uint8_t fb = from[p];
        const uint8_t t0 = (uint8_t)(tb & 0x0f);
        const uint8_t t1 = (uint8_t)(tb >> 4);
        const uint8_t f0 = (uint8_t)(fb & 0x0f);
        const uint8_t f1 = (uint8_t)(fb >> 4);
        const uint8_t d0 = (uint8_t)(t0 ^ f0);
        const uint8_t d1 = (uint8_t)(t1 ^ f1);
        col_dirtyness[p] |= (uint8_t)(d0 | (d1 << 4));
        dirty |= (uint32_t)(d0 | d1);
        interlaced[p * 2] = (uint8_t)((t0 << 4) | f0);
        interlaced[p * 2 + 1] = (uint8_t)((t1 << 4) | f1);
    }
    if (fb_width & 1) {
        const int x = fb_width - 1;
        uint8_t t = to[x / 2];
        uint8_t f = from[x / 2];
        t = (uint8_t)((x % 2) ? (t >> 4) : (t & 0x0f));
        f = (uint8_t)((x % 2) ? (f >> 4) : (f & 0x0f));
        col_dirtyness[x / 2] |= (uint8_t)((t ^ f) << (4 * (x % 2)));
        dirty |= (uint32_t)(t ^ f);
        interlaced[x] = (uint8_t)((t << 4) | f);
    }
    return dirty;
}

__attribute__((optimize("O3"))) void IRAM_ATTR
epd_apply_line_mask_VE(uint8_t* line, const uint8_t* mask, int mask_len) {
    if (line == NULL || mask == NULL || mask_len <= 0) {
        return;
    }
    // 与 epd_apply_line_mask 同：按字与；队列/mask 均 16B 对齐
    uint32_t* l32 = (uint32_t*)line;
    const uint32_t* m32 = (const uint32_t*)mask;
    const int words = mask_len >> 2;
    int i = 0;
    for (; i + 4 <= words; i += 4) {
        l32[i] &= m32[i];
        l32[i + 1] &= m32[i + 1];
        l32[i + 2] &= m32[i + 2];
        l32[i + 3] &= m32[i + 3];
    }
    for (; i < words; i++) {
        l32[i] &= m32[i];
    }
    for (i = words << 2; i < mask_len; i++) {
        line[i] &= mask[i];
    }
}

/**
 * @brief 1ppB / 1K VE 格式 LUT 查表（与 lut.c lookup_pixels_in_VE_LUT 一致）
 * @note S31 与 S3 字节内 4px 高低位对调：板级/LCD 沿 X 先吃高位 2bit，非 CPU 端序差异。
 *       VE LUT 每项占 4 字节，值在 LE 低字节；按字节取比整字 load 更省带宽。
 */
__attribute__((optimize("O3"))) void IRAM_ATTR
calc_epd_input_1ppB_1k_S3_VE_aligned(
    const uint32_t* ld, uint8_t* epd_input, const uint8_t* conversion_lut, uint32_t epd_width
) {
    if (ld == NULL || epd_input == NULL || conversion_lut == NULL || epd_width < 4) {
        return;
    }

    const uint8_t* lut = conversion_lut;
    const uint32_t n = epd_width >> 2; // 输出字节数 = 像素/4
    uint32_t j = 0;

#if defined(CONFIG_IDF_TARGET_ESP32S31)
#define LUT4(in)                                                                                   \
    ((uint8_t)(lut[((in) & 0xFFu) << 2] << 6 | lut[(((in) >> 8) & 0xFFu) << 2] << 4                \
               | lut[(((in) >> 16) & 0xFFu) << 2] << 2 | lut[((in) >> 24) << 2]))
#else
#define LUT4(in)                                                                                   \
    ((uint8_t)(lut[((in) >> 24) << 2] << 6 | lut[(((in) >> 16) & 0xFFu) << 2] << 4                 \
               | lut[(((in) >> 8) & 0xFFu) << 2] << 2 | lut[((in) & 0xFFu) << 2]))
#endif

    for (; j + 4 <= n; j += 4) {
        const uint32_t in0 = ld[j];
        const uint32_t in1 = ld[j + 1];
        const uint32_t in2 = ld[j + 2];
        const uint32_t in3 = ld[j + 3];
        epd_input[j] = LUT4(in0);
        epd_input[j + 1] = LUT4(in1);
        epd_input[j + 2] = LUT4(in2);
        epd_input[j + 3] = LUT4(in3);
    }
    for (; j < n; j++) {
        epd_input[j] = LUT4(ld[j]);
    }
#undef LUT4
}
