#pragma once

/** FatFS 最小桩：模拟器仅 .ebook 路径；TXT GBK 转码降级为 ASCII 直通。 */

typedef unsigned short WCHAR;

static inline WCHAR ff_oem2uni(WCHAR oem, int cp) {
    (void)cp;
    if (oem < 0x80) {
        return oem;
    }
    return 0;
}
