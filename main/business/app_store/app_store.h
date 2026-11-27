#pragma once
#include <atomic>
#include <memory>
#include <string>
#include <vector>
#include <cJSON.h>
#include <esp_elf.h>

#if CONFIG_IDF_TARGET_ARCH_XTENSA
inline constexpr uint16_t kAppStoreElfMachine = 94; // Xtensa ELF 机器标识，与固件架构一致
#elif CONFIG_IDF_TARGET_ARCH_RISCV
inline constexpr uint16_t kAppStoreElfMachine = 243; // RISC-V ELF 机器标识，与固件架构一致
#else
#error "Unsupported ELF app architecture"
#endif

struct AppStoreIcon;

struct AppStoreItem {
    std::string id;                            // 服务器包标识
    std::string app_id;                        // 稳定应用标识
    std::string installed_name;                // 本地安装目录及 ELF 文件名前缀
    std::string name;                          // 中文名称
    std::string name_en;                       // 英文名称
    std::string summary;                       // 中文简述
    std::string summary_en;                    // 英文简述
    std::string category;                      // 中文类别
    std::string category_en;                   // 英文类别
    std::string description;                   // 中文说明
    std::string description_en;                // 英文说明
    std::string languages;                     // 逗号分隔的支持语言代码
    std::string provider;                      // 应用提供者名称
    std::string release_notes;                 // 中文更新说明
    std::string release_notes_en;              // 英文更新说明
    std::string uploaded_at;                   // 此版本上传时间，RFC 3339
    std::string version;                       // 三段数字版本
    std::string sha256;                        // ELF 内容摘要
    std::shared_ptr<const AppStoreIcon> icon;  // ELF 中的图标，副本共享不可变数据
    size_t size = 0;                           // ELF 字节数
};

/** @brief 读取并恢复已安装应用清单，不请求网络 */
bool AppStoreScan(std::vector<AppStoreItem>& items, std::string& error);

struct AppStoreCatalogPage {
    size_t number = 1;     // 当前页，从一开始
    size_t total = 0;      // 在线兼容应用总数
    bool expired = false;  // 请求快照过期，需重新刷新第一页
    bool more = false;     // 是否还有下一页
    std::string revision;  // 稳定目录快照
};

/** @brief 获取服务器单页兼容应用清单 */
bool AppStoreFetch(const std::string& server, std::vector<AppStoreItem>& items, std::string& error,
                   const std::atomic<bool>& cancelled, AppStoreCatalogPage* page = nullptr);

/** @brief 查询指定应用的兼容新版本，不修改本机安装状态 */
bool AppStoreCheckUpdate(const std::string& server, const AppStoreItem& current, AppStoreItem& candidate,
                         bool& available, std::string& error, const std::atomic<bool>& cancelled);

/** @brief 获取指定版本详情，离线失败保留本机信息 */
bool AppStoreDetail(const std::string& server, AppStoreItem& item, std::string& error,
                    const std::atomic<bool>& cancelled);

struct AppStoreProgress {
    std::atomic<size_t> bytes{0};        // 已写入 SD 的字节数
    std::atomic<size_t> total{0};        // 当前包的总字节数
    std::atomic<bool> verifying{false};  // 正在校验或提交安装
};

/** @brief 下载、严格校验并切换已安装版本 */
bool AppStoreInstall(const std::string& server, const AppStoreItem& item, std::string& error,
                     const std::atomic<bool>& cancelled, AppStoreProgress* progress = nullptr);

/** @brief 删除本机应用及其安装文件 */
bool AppStoreRemove(const AppStoreItem& item, std::string& error);

/** @brief 校验本机 ELF 并按需读取、重定位，仅加载执行所需内容 */
bool AppStoreLoad(const AppStoreItem& item, esp_elf_t& elf, std::string& error);

/** @brief 从 ELF 的非加载资源节按需读取，返回读取字节数，失败返回零 */
size_t AppStoreReadAsset(const AppStoreItem& item, size_t offset, void* buffer, size_t length);

/** @brief 查询 ELF 非加载资源节的字节数，无资源返回零 */
size_t AppStoreAssetSize(const AppStoreItem& item);

/** @brief 解析应用资源节的宿主文件与有界区域，不向 ELF 暴露文件句柄 */
bool AppStoreLocateAsset(const AppStoreItem& item, std::string& path, size_t& offset, size_t& length);

/** @brief 比较三段数字版本 */
bool AppStoreNewer(const std::string& candidate, const std::string& current);

/** @brief 检查并规范化本地服务器基址 */
bool AppStoreNormalizeServer(std::string& server);

/** @brief 按当前语言返回应用名称 */
const char* AppStoreName(const AppStoreItem& item);

enum class AppStoreOperation { Scan, Refresh, PageFetch, Detail, CheckUpdate, Install, Update, Remove };

struct AppStoreResult {
    std::atomic<bool> done{false};        // 工作结果发布屏障
    std::atomic<bool> cancelled{false};   // 离页取消后续网络操作
    AppStoreProgress progress;            // 后台发布下载进度
    bool ok = false;                      // 操作结果
    bool fetched = false;                 // 服务器清单是否有效
    bool scanned = false;                 // 本机清单是否有效
    AppStoreCatalogPage pagination;       // 本次获取的页及目录快照
    AppStoreItem candidate;               // 更新或详情响应
    bool latest = false;                  // 已安装版本无需更新
    std::vector<AppStoreItem> catalog;    // 网络清单快照
    std::vector<AppStoreItem> installed;  // 本机清单快照
    std::string error;                    // 操作失败说明
};

/** @brief 提交商店请求；作业持有请求、取消、进度和扫描收尾，不借用页面。 */
std::shared_ptr<AppStoreResult> AppStoreSubmit(AppStoreOperation operation, const std::string& server,
                                            const AppStoreItem& item, AppStoreCatalogPage pagination);

/** @brief 查询商店是否有尚未释放资源的后台作业。 */
bool AppStoreBusy();
