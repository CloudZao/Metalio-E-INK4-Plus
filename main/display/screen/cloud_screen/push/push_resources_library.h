#pragma once

#include "book_screen/book_cloud_library.h"

#include <cstdint>
#include <string>
#include <vector>

namespace reader {

enum class PushResourceType : uint8_t {
    kUnknown = 0,
    kBook = 1,
    kBadge = 2,
    kFont = 3,
};

struct CloudPushResource {
    PushResourceType type = PushResourceType::kUnknown;
    std::string task_id;
    std::string resource_id;
    std::string name; // 列表展示名（接口 name）
    std::string resource_name; // 人类可读名（可选）
    std::string download_url; // 下载 URL
    std::string cover_image_url; // 封面 URL
    uint64_t file_size = 0; // 文件大小
    std::string sha256; // 校验
    std::string format; // 格式
    int page_count = 0; // 页数（书籍）
    std::string font_style; // 字体样式
    int font_weight = 0; // 字重

    /** @brief 类型中文标签 */
    const char* TypeLabel() const;
    /** @brief 缩略图 URL（仅 coverImageUrl） */
    const std::string& CoverThumbUrl() const;
    /** @brief 是否有封面缩略 URL */
    bool HasCoverThumb() const;
    /** @brief 本地落盘路径 */
    std::string LocalPath() const;
    /** @brief 本地是否已存在对应文件 */
    bool IsDownloaded() const;
};

/** @brief 拉取推送资源列表 */
bool FetchPushResources(std::vector<CloudPushResource>& out, std::string& err_out,
                        DownloadGate* gate = nullptr);
/** @brief 下载单条推送资源 */
bool DownloadPushResource(const CloudPushResource& item, std::string& err_out,
                          CloudDownloadProgressFn on_progress = nullptr,
                          void* progress_user = nullptr, DownloadGate* gate = nullptr,
                          const uint8_t* cover_bytes = nullptr, size_t cover_len = 0);
/** @brief DELETE push-resources（下载回执/用户删除共用） */
bool DeletePushResourceRemote(const CloudPushResource& item, std::string& err_out);
/** @brief 下载成功回执（等同远端删除任务） */
inline bool AckPushResourceDownloaded(const CloudPushResource& item, std::string& err_out) {
    return DeletePushResourceRemote(item, err_out);
}

}  // namespace reader
