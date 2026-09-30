#pragma once

/**
 * @file sd_paths.h
 * @brief Unified on-device SD layout under /sdcard/metalio/e-ink/
 *
 * Mount point stays `/sdcard` (SdCardManager). All app data lives under
 * `/sdcard/metalio/e-ink/<feature>/` so the card root stays clean.
 *
 * | Path | Use |
 * |------|-----|
 * | .../文件上传前必读说明.txt | 上传前必读（中文；无则开机生成） |
 * | .../Read_Before_Upload.txt | 上传前必读（英文；无则开机生成） |
 * | .../recordings | 录音 Ogg Opus |
 * | .../books | 电子书 .epub/.txt/.ebook；同目录 .txt.idx 为分页索引；<stem>.a2i1 为旁路封面 |
 * | .../fonts | 阅读用 .ef 字体（云推送 .ef 亦落此） |
 * | .../fonts_ttf | 待转换 .ttf/.otf（云推送 TTF/OTF + 设置卡「导入 TTF」扫描） |
 * | .../a2ui_cache | A2UI 图片会话缓存（开机清空） |
 * | .../chat_log | 百问AI 本机会话 JSON（开机清空；无卡不写） |
 * | .../wallpaper | 壁纸库（关机/待机启用仅 NVS 记文件名；关机无/坏图则用固件内置 bg_shutdown.a2i1） |
 */

#include <sys/stat.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include <esp_log.h>

#include "cloudzao_endpoints.h"

#ifdef __cplusplus
extern "C" {
#endif

/** VFS 挂载点；改动须同步 SdCardManager */
#ifndef SD_MOUNT_POINT
#define SD_MOUNT_POINT "/sdcard"
#endif

/** 产品数据根目录 */
#ifndef SD_APP_ROOT
#define SD_APP_ROOT "/sdcard/metalio/e-ink"
#endif

#define SD_PATH_RECORDINGS  SD_APP_ROOT "/recordings"   // 录音
#define SD_PATH_BOOKS       SD_APP_ROOT "/books"        // 电子书
#define SD_PATH_FONTS       SD_APP_ROOT "/fonts"        // 阅读 .ef 字体
#define SD_PATH_FONT_SRC    SD_APP_ROOT "/fonts_ttf"    // 待转换 TTF/OTF
#define SD_PATH_A2UI_CACHE  SD_APP_ROOT "/a2ui_cache"   // A2UI 图片缓存
#define SD_PATH_CHAT_LOG    SD_APP_ROOT "/chat_log"     // 百问会话 JSON
#define SD_PATH_WALLPAPER    SD_APP_ROOT "/wallpaper"    // 壁纸库

#define SD_PATH_BOOK_FONT      SD_PATH_FONTS "/misans_25_2.ef"              // 默认阅读字体
#define SD_PATH_BOOK_FONT_ALT  SD_PATH_FONTS "/font_misans_regular_25_2.ef" // 备用阅读字体

#define SD_PATH_BG_SHUTDOWN    SD_PATH_WALLPAPER "/bg_shutdown.a2i1" // 关机壁纸文件名

#define SD_PATH_UPLOAD_README_ZH  SD_APP_ROOT "/文件上传前必读说明.txt" // 上传说明（中）
#define SD_PATH_UPLOAD_README_EN  SD_APP_ROOT "/Read_Before_Upload.txt" // 上传说明（英）

/**
 * On-screen / user-facing path: strip VFS mount (`/sdcard`) so hints match USB MSC
 * card root (e.g. `metalio/e-ink/books`). Avoids users creating a literal `/sdcard` folder.
 */
static inline const char* SdUserPath(const char* posix_abs) {
    if (posix_abs == NULL || posix_abs[0] == '\0') {
        return "";
    }
    const size_t n = sizeof(SD_MOUNT_POINT) - 1;
    if (strncmp(posix_abs, SD_MOUNT_POINT, n) == 0) {
        if (posix_abs[n] == '/') {
            return posix_abs + n + 1;
        }
        if (posix_abs[n] == '\0') {
            return ".";
        }
    }
    return posix_abs;
}

static inline int SdEnsureDir(const char* path) {
    struct stat st;
    if (path == NULL || path[0] == '\0') {
        return 0;
    }
    if (stat(path, &st) == 0) {
        return S_ISDIR(st.st_mode) ? 1 : 0;
    }
    if (mkdir(path, 0755) == 0) {
        return 1;
    }
    if (stat(path, &st) == 0) {
        return S_ISDIR(st.st_mode) ? 1 : 0;
    }
    return 0;
}

/** Create /sdcard/metalio/e-ink and common feature dirs (best-effort). Returns 1 on ok. */
static inline int SdEnsureAppLayout(void) {
    static const char* kTag = "sd_paths";
    if (!SdEnsureDir("/sdcard/metalio") || !SdEnsureDir(SD_APP_ROOT)) {
        ESP_LOGW(kTag, "ensure app root %s failed errno=%d", SD_APP_ROOT, errno);
        return 0;
    }
    SdEnsureDir(SD_PATH_RECORDINGS);
    SdEnsureDir(SD_PATH_BOOKS);
    SdEnsureDir(SD_PATH_FONTS);
    SdEnsureDir(SD_PATH_FONT_SRC);
    SdEnsureDir(SD_PATH_A2UI_CACHE);
    SdEnsureDir(SD_PATH_CHAT_LOG);
    SdEnsureDir(SD_PATH_WALLPAPER);
    return 1;
}

/**
 * @brief 若 path 不存在则写入 body（UTF-8）；已存在普通文件则跳过
 * @return 1 已存在或写入成功，0 失败
 */
static inline int SdWriteTextIfAbsent(const char* path, const char* body, size_t body_len) {
    static const char* kTag = "sd_paths";
    struct stat st;
    if (path == NULL || body == NULL) {
        return 0;
    }
    if (stat(path, &st) == 0) {
        return S_ISREG(st.st_mode) ? 1 : 0;
    }
    FILE* f = fopen(path, "wb");
    if (f == NULL) {
        ESP_LOGW(kTag, "create %s failed errno=%d", path, errno);
        return 0;
    }
    const size_t w = fwrite(body, 1, body_len, f);
    fclose(f);
    if (w != body_len) {
        ESP_LOGW(kTag, "write %s short %u/%u", path, (unsigned)w, (unsigned)body_len);
        return 0;
    }
    ESP_LOGI(kTag, "created %s", path);
    return 1;
}

/**
 * @brief metalio/e-ink 下补齐中/英「上传前必读」；各文件独立，有则跳过
 * @return 1 两份都已就绪（或本轮写成功），0 目录不可用或任一份写失败
 */
static inline int SdEnsureUploadReadme(void) {
    static const char* kTag = "sd_paths";
    if (!SdEnsureDir("/sdcard/metalio") || !SdEnsureDir(SD_APP_ROOT)) {
        ESP_LOGW(kTag, "ensure %s for upload readme failed errno=%d", SD_APP_ROOT, errno);
        return 0;
    }
    // 清掉旧版误放路径/旧文件名
    unlink(SD_MOUNT_POINT "/README_UPLOAD.txt");
    unlink(SD_APP_ROOT "/上传前必读说明.txt");

    // UTF-8 BOM；工具站 URL 来自 cloudzao_endpoints.c（开源版为空则写无链接说明）
    char zh[512];
    char en[640];
    if (cloudzao_book_tools_url[0] != '\0') {
        snprintf(zh, sizeof(zh),
                 "\xEF\xBB\xBF"
                 "书籍和字体在上传前建议经过此网站 %s "
                 "的工具转换后再上传到SD对应目录中，可大幅提升书籍打开效率和阅读体验。\r\n",
                 cloudzao_book_tools_url);
        snprintf(en, sizeof(en),
                 "\xEF\xBB\xBF"
                 "Before uploading books and fonts, convert them with the tools at "
                 "%s, then copy them into the corresponding "
                 "SD folders. This can greatly improve book open efficiency and reading experience.\r\n",
                 cloudzao_book_tools_url);
    } else {
        snprintf(zh, sizeof(zh),
                 "\xEF\xBB\xBF"
                 "书籍和字体在上传前建议经过转换工具处理后再上传到SD对应目录中，"
                 "可大幅提升书籍打开效率和阅读体验。\r\n");
        snprintf(en, sizeof(en),
                 "\xEF\xBB\xBF"
                 "Before uploading books and fonts, convert them with the appropriate tools, "
                 "then copy them into the corresponding SD folders. This can greatly improve "
                 "book open efficiency and reading experience.\r\n");
    }

    const int zh_ok = SdWriteTextIfAbsent(SD_PATH_UPLOAD_README_ZH, zh, strlen(zh));
    const int en_ok = SdWriteTextIfAbsent(SD_PATH_UPLOAD_README_EN, en, strlen(en));
    return (zh_ok && en_ok) ? 1 : 0;
}

#ifdef __cplusplus
}
#endif
