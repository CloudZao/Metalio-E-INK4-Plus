#pragma once

#include <atomic>
#include <ft2build.h>
#include FT_FREETYPE_H

/** @brief 从 SD 文件流打开 FreeType 字体，使用有界读取缓存并可记录读写错误。 */
bool TtfOpenFace(FT_Library library, const char* path, FT_Face* face,
                 std::atomic<bool>* io_failed = nullptr, FT_Error* error = nullptr);
