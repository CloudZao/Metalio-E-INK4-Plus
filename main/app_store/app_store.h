#pragma once
#include <atomic>
#include <memory>
#include <string>
#include <vector>
#include <cJSON.h>
#include <esp_elf.h>

namespace AppStoreIcons { struct Icon; }

namespace AppStore {
struct Item {
    std::string id; // 服务器包标识
    std::string app_id; // 稳定应用标识
    std::string installed_name; // 本地安装目录及 ELF 文件名前缀
    std::string name; // 中文名称
    std::string name_en; // 英文名称
    std::string summary; // 中文简述
    std::string summary_en; // 英文简述
    std::string category; // 中文类别
    std::string category_en; // 英文类别
    std::string description; // 中文说明
    std::string description_en; // 英文说明
    std::string languages; // 逗号分隔的支持语言代码
    std::string provider; // 应用提供者名称
    std::string release_notes; // 中文更新说明
    std::string release_notes_en; // 英文更新说明
    std::string uploaded_at; // 此版本上传时间，RFC 3339
    std::string version; // 三段数字版本
    std::string sha256; // ELF 内容摘要
    std::shared_ptr<const AppStoreIcons::Icon> icon; // ELF 中的图标，副本共享不可变数据
    size_t size = 0; // ELF 字节数
};
/** @brief 读取并恢复已安装应用清单，不请求网络 */
bool Scan(std::vector<Item>& items, std::string& error);
struct CatalogPage {
    size_t number = 1; // 当前页，从一开始
    size_t total = 0; // 在线兼容应用总数
    bool expired = false; // 请求快照过期，需重新刷新第一页
    bool more = false; // 是否还有下一页
    std::string revision; // 稳定目录快照
};
/** @brief 获取服务器单页兼容应用清单 */
bool Fetch(const std::string& server, std::vector<Item>& items, std::string& error, const std::atomic<bool>& cancelled, CatalogPage* page = nullptr);
/** @brief 查询指定应用的兼容新版本，不修改本机安装状态 */
bool CheckUpdate(const std::string& server, const Item& current, Item& candidate, bool& available, std::string& error, const std::atomic<bool>& cancelled);
/** @brief 获取指定版本详情，离线失败保留本机信息 */
bool Detail(const std::string& server, Item& item, std::string& error, const std::atomic<bool>& cancelled);
struct Progress {
    std::atomic<size_t> bytes{0}; // 已写入 SD 的字节数
    std::atomic<size_t> total{0}; // 当前包的总字节数
    std::atomic<bool> verifying{false}; // 正在校验或提交安装
};
/** @brief 下载、严格校验并切换已安装版本 */
bool Install(const std::string& server, const Item& item, std::string& error, const std::atomic<bool>& cancelled, Progress* progress = nullptr);
/** @brief 删除本机应用及其安装文件 */
bool Remove(const Item& item, std::string& error);
/** @brief 校验本机 ELF 并按需读取、重定位，仅加载执行所需内容 */
bool Load(const Item& item, esp_elf_t& elf, std::string& error);
/** @brief 从 ELF 的非加载资源节按需读取，返回读取字节数，失败返回零 */
size_t ReadAsset(const Item& item, size_t offset, void* buffer, size_t length);
/** @brief 查询 ELF 非加载资源节的字节数，无资源返回零 */
size_t AssetSize(const Item& item);
/** @brief 解析应用资源节的宿主文件与有界区域，不向 ELF 暴露文件句柄 */
bool LocateAsset(const Item& item, std::string& path, size_t& offset, size_t& length);
/** @brief 比较三段数字版本 */
bool Newer(const std::string& candidate, const std::string& current);
/** @brief 检查并规范化本地服务器基址 */
bool NormalizeServer(std::string& server);
/** @brief 按当前语言返回应用名称 */
const char* Name(const Item& item);
}
