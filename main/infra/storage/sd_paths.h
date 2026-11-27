#pragma once

/**
 * @file sd_paths.h
 * @brief 统一设备在 /sdcard/metalio/e-ink/ 下的 SD 数据布局
 *
 * 挂载点为 `/sdcard`（SdCardManager），应用数据按功能放在
 * `/sdcard/metalio/e-ink/<feature>/` 下。
 *
 * | 路径 | 用途 |
 * |------|-----|
 * | .../文件上传前必读说明.txt | 上传前必读（中文；无则开机生成） |
 * | .../Read_Before_Upload.txt | 上传前必读（英文；无则开机生成） |
 * | .../settings/wificonfig.json | Wi-Fi 配置，与 NVS 同步保存 |
 * | .../books | 电子书 .epub/.txt/.ebook；新建辅助文件另存，兼容保留旧旁路文件 |
 * | .../fonts | 阅读 .ef 及扩展字库 .fontpack |
 * | .../fonts_ime/zh-CN | 中文输入法词库及许可 |
 * | .../fonts_ttf | 系统与阅读 .ttf/.otf（支持直接加载或转换为 .ef） |
 * | .../apps | 商店应用安装清单、ELF 与恢复备份 |
 * | .../settings/apps | 按应用名称和 ID 隔离的应用持久状态 |
 * | .../temp/app_store | 按应用名称和 ID 归档的下载与安装工作文件 |
 * | .../temp/bwchat/images | A2UI 图片会话缓存（开机清空） |
 * | .../temp/bwchat/messages | 百问AI 本机会话 JSON（开机清空；无卡不写） |
 * | .../wallpaper | 壁纸库（关机/待机启用仅 NVS 记文件名；关机无/坏图则用固件内置 bg_shutdown.a2i1） |
 */

#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <sys/stat.h>
#include <unistd.h>
#include <esp_log.h>

#ifdef __cplusplus
extern "C" {
#endif

/** VFS 挂载点；改动须同步 SdCardManager */
#ifndef SD_MOUNT_POINT
#define SD_MOUNT_POINT "/sdcard"
#endif

#define SD_FILE_NAME_MAX_BYTES 246                            // 用户文件名的 UTF-8 字节上限，包含扩展名
#define SD_FILE_NAME_CAPACITY (SD_FILE_NAME_MAX_BYTES + 1)    // 文件名缓冲区，包含终止符
#define SD_FILE_PATH_CAPACITY (512 + sizeof(SD_MOUNT_POINT))  // 与传输服务一致的完整路径容量

/** @brief 检查用户文件名长度及 FAT 不允许的名称字符 */
static inline int SdIsValidFileName(const char* name) {
    if (name == NULL || name[0] == '\0') {
        return 0;
    }
    const size_t len = strlen(name);
    if (len > SD_FILE_NAME_MAX_BYTES || name[len - 1] == '.' || name[len - 1] == ' ') {
        return 0;
    }
    for (const unsigned char* p = (const unsigned char*)name; *p; ++p) {
        if (*p < 32 || *p == 127 || strchr("/\\:*?\"<>|", *p) != NULL) {
            return 0;
        }
    }
    return 1;
}

/** @brief 获取不截断 UTF-8 字符且不超过字节预算的前缀长度 */
static inline size_t SdUtf8PrefixLength(const char* text, size_t max_bytes) {
    size_t len = strlen(text);
    if (len <= max_bytes) {
        return len;
    }
    len = max_bytes;
    while (len > 0 && ((unsigned char)text[len] & 0xc0) == 0x80) {
        --len;
    }
    return len;
}

/** 产品数据根目录 */
#ifndef SD_APP_ROOT
#define SD_APP_ROOT SD_MOUNT_POINT "/metalio/e-ink"
#endif

#define SD_VENDOR_ROOT SD_MOUNT_POINT "/metalio"                        // 产品目录父级
#define SD_PATH_OTA_FIRMWARE SD_MOUNT_POINT "/Metalio-E-Ink4-Plus.bin"  // 兼容本地升级文件
#define SD_PATH_WALLPAPER_BACKUP SD_PATH_WALLPAPER_TEMP "/backup"       // 覆盖保存中断后的原图
#define SD_PATH_IMAGE_WORK SD_PATH_TEMP "/image"                        // 公共图像解码工作文件

#define SD_PATH_SETTINGS SD_APP_ROOT "/settings"                 // 用户配置
#define SD_PATH_KEY_BINDINGS SD_PATH_SETTINGS "/key_bindings.json"  // 按键绑定配置
#define SD_PATH_WIFI_CONFIG SD_PATH_SETTINGS "/wificonfig.json"  // 无线网络配置

#define SD_PATH_SYSTEM SD_PATH_SETTINGS "/system"  // 系统持久数据
#define SD_PATH_LOGS SD_PATH_SYSTEM "/logs"
#define SD_PATH_RUNTIME_LOG SD_PATH_LOGS "/metalio_logs.txt"
#define SD_PATH_COREDUMP SD_PATH_LOGS "/metalio_coredump.elf"

#define SD_PATH_BOOKS SD_APP_ROOT "/books"                         // 电子书
#define SD_PATH_FONTS SD_APP_ROOT "/fonts"                         // 阅读 .ef 与扩展 .fontpack 字体
#define SD_PATH_FONTS_IME SD_APP_ROOT "/fonts_ime"                 // 输入法语言资源
#define SD_PATH_IME_ZH_CN SD_PATH_FONTS_IME "/zh-CN"              // 中文输入法资源
#define SD_PATH_PINYIN_DICT SD_PATH_IME_ZH_CN "/pinyin-dictionary.bin"  // 中文拼音词库
#define SD_PATH_FONT_EXT SD_PATH_FONTS "/extended-fonts.fontpack"  // UI 与阅读缺字拓展字库
#define SD_PATH_FONT_SRC SD_APP_ROOT "/fonts_ttf"                  // 系统与阅读 TTF/OTF 源字体
#define SD_PATH_A2UI_CACHE SD_PATH_BWCHAT_TEMP "/images"           // A2UI 图片缓存
#define SD_PATH_CHAT_LOG SD_PATH_BWCHAT_TEMP "/messages"           // 百问会话 JSON
#define SD_PATH_WALLPAPER SD_APP_ROOT "/wallpaper"                 // 壁纸库
#define SD_PATH_TEMP SD_APP_ROOT "/temp"                           // 临时工作文件根目录
#define SD_PATH_APPS SD_APP_ROOT "/apps"                           // 商店安装的应用
#define SD_PATH_APP_STATE SD_PATH_SETTINGS "/apps"                 // 应用私有持久数据
#define SD_PATH_APP_STORE_TEMP SD_PATH_TEMP "/app_store"           // 商店下载与安装工作文件
#define SD_PATH_BOOK_STATE SD_PATH_SETTINGS "/books"               // 进度、时长与书签
#define SD_PATH_BOOK_TEMP SD_PATH_TEMP "/books"                    // 阅读应用缓存
#define SD_PATH_BOOK_WORK SD_PATH_BOOK_TEMP "/work"                // 阅读会话工作文件
#define SD_PATH_BOOK_CACHE SD_PATH_BOOK_TEMP "/cache"              // 可重建的封面和索引
#define SD_PATH_BWCHAT_TEMP SD_PATH_TEMP "/bwchat"                 // 百问 AI 会话缓存
#define SD_PATH_WALLPAPER_APP_TEMP SD_PATH_TEMP "/wallpaper"       // 壁纸应用缓存
#define SD_PATH_WALLPAPER_TEMP SD_PATH_WALLPAPER_APP_TEMP "/work"  // 图片解码系数后备文件
#define SD_PATH_WALLPAPER_META SD_PATH_WALLPAPER_APP_TEMP "/meta"  // 壁纸名称元数据
#define SD_PATH_TRANSFER_TEMP SD_PATH_TEMP "/transfer"             // 传输应用缓存
#define SD_PATH_TRANSFER_WORK SD_PATH_TRANSFER_TEMP "/work"        // 下载和预览工作文件

#define SD_PATH_BOOK_FONT SD_PATH_FONTS "/misans_40_2.ef"                   // 默认阅读字体
#define SD_PATH_BOOK_FONT_ALT SD_PATH_FONTS "/font_misans_regular_25_2.ef"  // 备用阅读字体

#define SD_PATH_BG_SHUTDOWN SD_PATH_WALLPAPER "/bg_shutdown.a2i1"  // 关机壁纸文件名

#define SD_PATH_UPLOAD_README_ZH SD_APP_ROOT "/文件上传前必读说明.txt"  // 上传说明（中）
#define SD_PATH_UPLOAD_README_EN SD_APP_ROOT "/Read_Before_Upload.txt"  // 上传说明（英）

/**
 * @brief 移除 VFS 挂载点前缀，使界面路径与 USB MSC 卡根目录一致
 * @note 例如显示 `metalio/e-ink/books`，避免用户额外创建 `/sdcard` 目录。
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

/** @brief 准备已登记目录及其父目录；任意用户目录仍使用 SdEnsureDir。 */
int SdEnsureDirectory(const char* path);

/** @brief 创建产品目录布局，根目录可用返回 1 */
int SdEnsureAppLayout(void);

/** @brief 补齐中英文上传说明文件，全部就绪返回 1 */
int SdEnsureUploadReadme(void);

#ifdef __cplusplus
}
#endif
