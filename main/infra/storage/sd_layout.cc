#include <fcntl.h>
#include <esp_random.h>
#include "infra/storage/sd_paths.h"
#include "sd_work_file.h"
#include "infra/network/endpoints/cloudzao_endpoints.h"

struct SdLayoutDirectory {
    const char* path;    // 已登记目录
    const char* parent;  // 父目录，空指针表示挂载根
};

static const SdLayoutDirectory kSdDirectories[] = {
    {SD_VENDOR_ROOT, nullptr},
    {SD_APP_ROOT, SD_VENDOR_ROOT},
    {SD_PATH_SETTINGS, SD_APP_ROOT},
    {SD_PATH_SYSTEM, SD_PATH_SETTINGS},
    {SD_PATH_LOGS, SD_PATH_SYSTEM},
    {SD_PATH_BOOK_STATE, SD_PATH_SETTINGS},
    {SD_PATH_APP_STATE, SD_PATH_SETTINGS},
    {SD_PATH_BOOKS, SD_APP_ROOT},
    {SD_PATH_FONTS, SD_APP_ROOT},
    {SD_PATH_FONT_SRC, SD_APP_ROOT},
    {SD_PATH_FONTS_IME, SD_APP_ROOT},
    {SD_PATH_IME_ZH_CN, SD_PATH_FONTS_IME},
    {SD_PATH_APPS, SD_APP_ROOT},
    {SD_PATH_WALLPAPER, SD_APP_ROOT},
    {SD_PATH_TEMP, SD_APP_ROOT},
    {SD_PATH_BOOK_TEMP, SD_PATH_TEMP},
    {SD_PATH_BOOK_WORK, SD_PATH_BOOK_TEMP},
    {SD_PATH_BOOK_CACHE, SD_PATH_BOOK_TEMP},
    {SD_PATH_BWCHAT_TEMP, SD_PATH_TEMP},
    {SD_PATH_A2UI_CACHE, SD_PATH_BWCHAT_TEMP},
    {SD_PATH_CHAT_LOG, SD_PATH_BWCHAT_TEMP},
    {SD_PATH_WALLPAPER_APP_TEMP, SD_PATH_TEMP},
    {SD_PATH_WALLPAPER_TEMP, SD_PATH_WALLPAPER_APP_TEMP},
    {SD_PATH_WALLPAPER_META, SD_PATH_WALLPAPER_APP_TEMP},
    {SD_PATH_WALLPAPER_BACKUP, SD_PATH_WALLPAPER_TEMP},
    {SD_PATH_TRANSFER_TEMP, SD_PATH_TEMP},
    {SD_PATH_TRANSFER_WORK, SD_PATH_TRANSFER_TEMP},
    {SD_PATH_APP_STORE_TEMP, SD_PATH_TEMP},
    {SD_PATH_IMAGE_WORK, SD_PATH_TEMP},
};

int SdEnsureDirectory(const char* path) {
    if (path) {
        for (const auto& directory : kSdDirectories) {
            if (strcmp(path, directory.path) != 0) {
                continue;
            }
            if (directory.parent && !SdEnsureDirectory(directory.parent)) {
                return 0;
            }
            if (SdEnsureDir(directory.path)) {
                return 1;
            }
            const int error = errno;
            ESP_LOGE("SdLayout", "operation=ensure_directory index=%u errno=%d",
                     static_cast<unsigned>(&directory - kSdDirectories), error);
            errno = error;
            return 0;
        }
    }
    errno = EINVAL;
    ESP_LOGE("SdLayout", "operation=ensure_directory result=unregistered");
    return 0;
}

int SdEnsureAppLayout(void) {
    if (!SdEnsureDirectory(SD_APP_ROOT)) {
        return 0;
    }
    for (const auto& directory : kSdDirectories) {
        if (!SdEnsureDirectory(directory.path)) {
            return 0;
        }
    }
    return 1;
}

/**
 * @brief 若 path 不存在则写入 body（UTF-8）；已存在普通文件则跳过
 * @return 1 已存在或写入成功，0 失败
 */
static int SdWriteTextIfAbsent(const char* path, const char* body, size_t body_len) {
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
    const int write_error = errno;
    const int closed = fclose(f);
    const int error = w != body_len ? write_error : errno;
    if (w != body_len || closed != 0) {
        ESP_LOGE(kTag, "operation=write_readme bytes=%u expected=%u close=%d errno=%d", (unsigned)w, (unsigned)body_len,
                 closed, error);
        if (unlink(path) != 0 && errno != ENOENT) {
            ESP_LOGE(kTag, "operation=write_readme stage=cleanup errno=%d", errno);
        }
        return 0;
    }
    ESP_LOGI(kTag, "created %s", path);
    return 1;
}

/**
 * @brief metalio/e-ink 下补齐中/英「上传前必读」；各文件独立，有则跳过
 * @return 1 两份都已就绪（或本轮写成功），0 目录不可用或任一份写失败
 */
int SdEnsureUploadReadme(void) {
    static const char* kTag = "sd_paths";
    if (!SdEnsureDir(SD_MOUNT_POINT "/metalio") || !SdEnsureDir(SD_APP_ROOT)) {
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

const char* SdWorkDirectory(SdWorkArea area) {
    switch (area) {
        case SdWorkArea::Transfer:
            return SD_PATH_TRANSFER_WORK;
        case SdWorkArea::Books:
            return SD_PATH_BOOK_WORK;
        case SdWorkArea::Wallpaper:
            return SD_PATH_WALLPAPER_TEMP;
        case SdWorkArea::Image:
            return SD_PATH_IMAGE_WORK;
    }
    return nullptr;
}

int SdCreateWorkFile(SdWorkArea area, const char* prefix, char* path, size_t capacity) {
    if (path && capacity) {
        path[0] = '\0';
    }
    const char* directory = SdWorkDirectory(area);
    bool valid = prefix && *prefix;
    if (valid) {
        for (const unsigned char* ch = reinterpret_cast<const unsigned char*>(prefix); *ch; ++ch) {
            if (!((*ch >= 'a' && *ch <= 'z') || (*ch >= 'A' && *ch <= 'Z') || (*ch >= '0' && *ch <= '9') ||
                  *ch == '-' || *ch == '_')) {
                valid = false;
                break;
            }
        }
    }
    if (!directory || !valid || !path || !capacity) {
        errno = EINVAL;
        return -1;
    }
    if (!SdEnsureDirectory(directory)) {
        return -1;
    }
    for (unsigned attempt = 0; attempt < 8; ++attempt) {
        const int count =
            snprintf(path, capacity, "%s/%s%08lx.tmp", directory, prefix, static_cast<unsigned long>(esp_random()));
        if (count < 0 || static_cast<size_t>(count) >= capacity) {
            path[0] = '\0';
            errno = ENAMETOOLONG;
            return -1;
        }
        const int fd = open(path, O_CREAT | O_EXCL | O_RDWR, 0600);
        if (fd >= 0) {
            return fd;
        }
        if (errno != EEXIST) {
            break;
        }
    }
    const int error = errno;
    path[0] = '\0';
    ESP_LOGE("SdLayout", "operation=create_work area=%u errno=%d", static_cast<unsigned>(area), error);
    errno = error;
    return -1;
}
