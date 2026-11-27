#pragma once
#include <atomic>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>
#include "infra/storage/sd_paths.h"

enum class WallpaperOperation : uint8_t { None, Select, Delete, Save };
enum class WallpaperTarget : uint8_t { Shutdown, Standby, Reading };

struct WallpaperSaveOptions {
    std::string path; // 壁纸根目录内的源文件
    uint8_t rotation = 0; // 顺时针旋转次数
    bool mirror = false;
    bool save_as = false;
    int width = 0; // 当前板型的目标尺寸
    int height = 0;
};

struct WallpaperResult {
    WallpaperOperation operation = WallpaperOperation::None;
    WallpaperTarget target = WallpaperTarget::Shutdown;
    bool batch = false;
    bool clear = false;
    bool save_as = false;
    std::atomic<bool> completed{false}; // 发布后可读取以下结果
    mutable std::atomic<bool> cancelled{false}; // 保存提交前或批量删除条目之间检查
    int succeeded = 0;
    int failed = 0;
    size_t size_bytes = 0;
    char path[SD_FILE_PATH_CAPACITY] = {}; // 已提交图片的路径
    char error[80] = {};
};

using WallpaperRequest = std::shared_ptr<const WallpaperResult>;

/** @brief 提交壁纸选择；已接纳后独立完成持久化及阅读背景设置，失败返回空。 */
WallpaperRequest WallpaperSelect(WallpaperTarget target, std::string path, bool clear, bool overlay);

/** @brief 提交单项或批量删除；批量取消在当前条目清理完成后生效，失败返回空。 */
WallpaperRequest WallpaperDelete(std::vector<std::string> names, bool batch = false);

/** @brief 提交图片变换保存；取消仅在文件提交前生效，失败返回空。 */
WallpaperRequest WallpaperSave(WallpaperSaveOptions options);

/** @brief 查询能力拥有的在途操作，不依赖页面或结果接收。 */
bool WallpaperIsBusy(WallpaperOperation operation = WallpaperOperation::None);

/** @brief 查询已完成操作的版本，用于重新进入或恢复页面后更新呈现。 */
uint32_t WallpaperRevision();
