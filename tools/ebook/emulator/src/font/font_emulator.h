#pragma once

#ifdef __cplusplus
extern "C" {
#endif

/** 设置 fontpack 文件路径（.fontpack）；切换后需重新 fontpack_lv_ensure_ready。 */
void fontpack_emulator_set_path(const char* path);

/** 设置 epdfont 文件路径（.ef）；切换后需重新 epdfont_emulator_open。 */
void epdfont_emulator_set_path(const char* path);

#ifdef __cplusplus
}
#endif
