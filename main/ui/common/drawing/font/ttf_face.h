#pragma once

#include <atomic>
#include <ft2build.h>
#include FT_FREETYPE_H

/** @brief 从 SD 打开 FreeType 字体；同源字号共享内存快照或读窗口，transient 模式不长期占用文件句柄。 */
bool TtfOpenFace(FT_Library library, const char* path, FT_Face* face, std::atomic<bool>* io_failed = nullptr,
                 FT_Error* error = nullptr, bool transient = false, bool* memory_backed = nullptr);

/** @brief 后台逐块核对完整内存快照与当前 SD 文件，不修改字体或阻塞字形缓存。 */
bool TtfFaceSnapshotMatchesFile(FT_Face face);
