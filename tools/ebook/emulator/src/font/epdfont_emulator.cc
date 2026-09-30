#include "font_emulator.h"

#include <string>

static std::string s_epdfont_path;

extern "C" void epdfont_emulator_set_path(const char* path) {
    s_epdfont_path = path != nullptr ? path : "";
}
