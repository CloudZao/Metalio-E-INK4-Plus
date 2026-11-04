#pragma once

#include <atomic>
#include <ft2build.h>
#include FT_FREETYPE_H

/** @brief 从 SD 打开 FreeType 字体，同一源文件的不同字号共享有界读取缓存。 */
bool TtfOpenFace(FT_Library library, const char* path, FT_Face* face,
                 std::atomic<bool>* io_failed = nullptr, FT_Error* error = nullptr);
