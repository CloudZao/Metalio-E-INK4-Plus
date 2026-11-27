#pragma once

#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>
#include <vector>
#include <dirent.h>
#include "infra/storage/storage_error.h"

// 页面模块静态持有；目录页按内存预算缓存，计数不打开文件内容。
class SdDirectoryPage {
public:
    using Filter = bool (*)(const char* path, bool directory);
    struct Entry {
        std::string path;           // 当前页条目的绝对路径
        bool directory = false;     // 目录由文件系统类型字段识别
        bool prefetch = false;      // 后台相邻页预取，可按需准备缩略图
        size_t bytes = 0;           // 当前页文件大小
        int64_t mtime = 0;          // 当前页修改时间
        StorageError error;          // 条目属性读取降级，不使整个目录页失败
        int metric = 0;             // 当前页字体字号等展示属性
        std::string title;          // 当前页附加标题
    };
    struct Result {
        int page = 0;                // 实际页码
        bool ok = false;             // 目录读取成功
        StorageError error;           // 首次失败原因，随异步结果交付
        bool more = false;           // 当前页后仍有条目
        std::vector<Entry> entries;  // 当前页条目
    };

    /**
     * @brief 按页请求目录条目，后续 Poll 推进后台任务
     * @param name_query 文件名关键词，英文字母忽略大小写；空值不筛选
     * @param recursive 是否递归访问子目录，默认仅列出指定根目录
     * @param locate_path 非空时先按文件名定位所在页；未找到时使用传入页码
     */
    void Request(std::vector<std::string> roots, Filter filter, int page, int size, bool refresh = false,
                 void (*enrich)(Entry&) = nullptr, bool names_only = false,
                 const std::string& name_query = {}, bool recursive = false, const std::string& locate_path = {});

    /** @brief 推进请求并接收当前页结果；由界面线程周期调用 */
    bool Poll(Result& result);

    /** @brief 取消请求并释放空闲目录句柄，在途任务逐项退出 */
    void Cancel();

    /** @brief 返回已知页数；计数未完成时包含已发现的下一页 */
    int Pages() const;

    /** @brief 查询总数是否已完整计得 */
    bool Counted() const {
        return total_.load() >= 0;
    }

    /** @brief 查询当前工作批次是否已取消，供预取解码检查 */
    bool Cancelled() const;

    /** @brief 查询指定页是否已缓存；仅由界面线程调用 */
    bool Cached(int page);

private:
    struct CachedPage {
        Result result;      // 不持有控件和图片像素
        size_t bytes = 0;   // 目录页内存估算
        uint64_t used = 0;  // 最近使用次序
    };
    static void ReclaimCaches(void*);
    std::atomic<bool> reclaim_pending_{false}; // 回收不改变正在请求的目录页
    void Remember(const Result& result, uint32_t reclaim_generation);
    bool Recall(int page, Result& result);
    static void Worker(void* arg);
    bool Matches(const char* path, bool directory) const;
    bool Next(Entry& entry, StorageError& error);
    bool NextDirectoryEntry(bool counting, Entry& entry, StorageError& error);
    void LocatePage(const std::string& path, int size, int& page, StorageError& error);
    void Close();
    void CloseCount();
    void Run(unsigned generation);
    std::mutex mutex_;                       // 保护 UI 请求、结果及目录页缓存
    std::atomic<unsigned> generation_{0};    // 翻页、刷新或取消时作废在途批次
    std::atomic<bool> busy_{false};          // 当前是否存在目录工作任务
    std::atomic<int> total_{-1};             // 有效条目总数，负值表示尚未完成计数
    std::atomic<int> known_pages_{1};        // 计数完成前已确认的页数下界
    bool pending_ = false;                   // 最近请求尚未交给工作任务
    bool ready_ = false;                     // 当前请求结果可由 UI 领取
    std::vector<std::string> roots_;         // 按枚举顺序访问的根目录
    std::string name_query_;                 // 请求的文件名关键词，空值不过滤
    std::string locate_path_;                // 本次请求优先定位的文件，后台仅扫描名称
    std::string cursor_name_query_;          // 工作任务持有的关键词快照
    Filter filter_ = nullptr;                // 最近请求的条目过滤器
    void (*enrich_)(Entry&) = nullptr;       // 可选展示属性读取，仅在非名称模式执行
    int page_ = 0;                           // 最近请求的零起始页码
    std::atomic<int> size_{1};               // 每页最多条目数，至少为一
    Result result_;                          // 待 UI 领取的当前页结果
    std::vector<CachedPage> cache_;          // 有内存上限的目录页缓存
    uint64_t cache_used_ = 0;                // 缓存访问序号，用于淘汰最久未使用页
    std::vector<std::string> cursor_roots_;  // 工作批次持有的根目录快照
    Filter cursor_filter_ = nullptr;         // 与工作游标一致的过滤器
    DIR* cursor_ = nullptr;                  // 仅工作任务访问；空闲取消时在 mutex_ 内关闭
    size_t root_ = 0;                        // 当前枚举根目录的下标
    size_t position_ = 0;                    // 下一个有效条目的序号
    DIR* count_cursor_ = nullptr;            // 翻页打断计数时保留位置，离页时关闭
    size_t count_root_ = 0;                  // 正在计数的根目录
    int count_so_far_ = 0;                   // 已计得的有效条目数
    bool cursor_recursive_ = false;         // 当前工作代次的递归快照
    bool recursive_ = false;                // 仅按请求递归枚举子目录
    std::string cursor_path_;              // 当前目录路径，不保存完整目录树
    std::string count_path_;               // 独立计数目录路径
    std::vector<long> cursor_parents_;     // 父目录位置，进入子目录前关闭父句柄
    std::vector<long> count_parents_;      // 计数用父目录位置
    bool names_only_ = false;                // 名称页不读取 stat 或附加元数据
    bool reset_ = true;                      // 工作批次开始时重置目录及计数游标
    unsigned visited_ = 0;                   // 限制连续目录枚举批次
    unsigned active_generation_ = 0;         // 工作任务的取消代次
};

/** @brief 待机或介质切换前取消所有目录分页并释放句柄 */
void SdCancelDirectoryPages();
