#include "sdkconfig.h"
#include "epd_reader_aa.h"

#if CONFIG_DISPLAY_BACKEND_SSD1677
#include "boards/common/display/ssd1677/epd_i1_glyph_thin.h"
#endif

static bool s_enabled = false;

bool EpdReaderAaEnabled() {
    return s_enabled;
}

void EpdSetReaderAaEnabled(bool enabled) {
    s_enabled = enabled;
    #if CONFIG_DISPLAY_BACKEND_SSD1677
    epd_i1_glyph_set_aa(enabled);
    #endif
}
