#pragma once

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <mutex>
#include <string>
#include <utility>
#include <vector>
#include "lvgl.h"
#include "book_bookmarks.h"
#include "book_disk_cache.h"
#include "book_toc.h"
#include "ebook_document.h"
#include "epub_document.h"
#include "reader_types.h"
#include "txt_chapter.h"
#ifndef EBOOK_EMULATOR
#include "infra/diagnostics/firmware_memory.h"
#endif

// TXT / EPUB / EBOOK 阅读会话：先按源锚点生成一页预览，后台补齐结构缓存和当前章节分页。
// TXT 目录按原始字节扫描；无可用目录时保留逐页流式阅读，不估算全书总页数。
// EPUB 目录范围可细分同一个正文文件或跨正文文件；EBOOK 保留原始章节编号。
// 正文分页存于 SD，内存只保留固定窗口及当前页；独立导航游标准备成功后由 UI 发布。
// POS4 保存正文锚点、文件身份及阅读时长；保留 POS1/2/3 和 BKM1 的必要读取兼容。
//
// 阅读时长（单书）：
// - 终身累计 reading_seconds_：跨日不清零；仅 Close/Open 边界重置内存，Open 后从 .pos 恢复
// - 当日 daily_seconds_：按本地日历日（YYYYMMDD）懒换日清零；时钟无效（未对时）不累计当日、不误清盘上日戳
// - 仅在 UI 判定「可读正文已就绪」后 StartReadingClock；退出前 StopReadingClock / Close 落盘
// - 进待机由 UI 停钟，退出待机再 Start（避免浅睡墙钟计入阅读时长）
// - 用 esp_timer 墙钟累加（非每秒 +1），SaveProgress 会先 fold 再写，支持周期性 checkpoint
// - 换日检查在 Open / fold / Save / Peek 当日路径；不依赖开机扫库
class BookSession {
public:
    /** @brief 创建尚未打开书籍的阅读会话 */
    BookSession() = default;

    /** @brief 关闭阅读会话并释放文件、分页和图像资源 */
    ~BookSession();

    BookSession(const BookSession&) = delete;
    BookSession& operator=(const BookSession&) = delete;

    /**
     * @brief 设置分页步进字体及用于安全字距判断的绘制字体
     * @note 调用方在打开会话前提供字体；未提供时沿用无字体的估算度量
     * @param glyph_font 完整字形字体；为空时使用分页字体，调用方保证字体在会话使用期间有效
     */
    void SetFont(const lv_font_t* font, const lv_font_t* glyph_font = nullptr) {
        font_ = font;
        glyph_font_ = glyph_font ? glyph_font : font;
    }

    /** @brief 设置竖排方向并同步逻辑视口，整页图片模式始终使用横排坐标 */
    void SetVerticalWriting(bool vertical) {
        vertical_writing_ = vertical && !page_images_mode_;
        SetViewport(physical_w_, physical_h_);
    }

    /** @brief 查询当前会话是否按竖排列布局 */
    bool VerticalWriting() const {
        return vertical_writing_;
    }

    /** @brief 设置物理阅读区域尺寸，尺寸改变时释放旧页图缓存 */
    void SetViewport(lv_coord_t width, lv_coord_t height) {
        if (physical_w_ != width || physical_h_ != height) {
            ClearPageImageCache();
        }
        physical_w_ = width;
        physical_h_ = height;
        // 竖排交换逻辑视口的宽高，将每列作为一行参与分页。
        viewport_w_ = vertical_writing_ ? height : width;
        viewport_h_ = vertical_writing_ ? width : height;
    }

    /** @brief 获取物理阅读区域宽度，不受竖排坐标交换影响 */
    lv_coord_t ViewportWidth() const {
        return physical_w_;
    }

    /** @brief 获取物理阅读区域高度，不受竖排坐标交换影响 */
    lv_coord_t ViewportHeight() const {
        return physical_h_;
    }

    /** @brief 设置字体行框的相对疏密百分比及额外段距，影响分页指纹。 */
    void SetLineSpacing(int spacing_percent, lv_coord_t para_gap) {
        line_spacing_percent_ = page_images_mode_ ? 0 : std::clamp(spacing_percent, 0, 100);
        para_gap_ = page_images_mode_ ? 0 : std::max<lv_coord_t>(0, para_gap);
    }

    /** @brief 设置分页和绘制共享的连续排版参数。 */
    void SetTextStyle(const TextStyle& style) {
        text_style_ = style;
    }

    /** @brief 获取当前会话的排版与绘制参数快照。 */
    const TextStyle& Style() const {
        return text_style_;
    }

    /** @brief 获取当前字体行框的相对疏密百分比 */
    int LineSpacingPercent() const {
        return line_spacing_percent_;
    }

    /** @brief 获取普通行距之外增加的段间像素数 */
    lv_coord_t ParaGap() const {
        return para_gap_;
    }

    // 打开过程进度 0–100；可在非 LVGL 线程回调，调用方自行投递 UI。
    using OpenProgressFn = void (*)(int percent, void* user);

    /** @brief 登记打开进度回调，调用方负责将非界面线程的通知转交界面 */
    void SetOpenProgress(OpenProgressFn fn, void* user) {
        progress_fn_ = fn;
        progress_user_ = user;
    }

    /** @brief 关闭旧会话并打开指定书籍，准备当前阅读位置；失败原因通过 Error 获取 */
    bool Open(const BookInfo& info);
#ifndef EBOOK_EMULATOR
    /** @brief 删除书籍及关联数据并更新阅读统计；正在打开或阅读时拒绝删除。 */
    static bool DeleteFile(const char* book);
#endif

    /** @brief 停止阅读计时、提交尚未接纳的最终进度并释放会话资源 */
    void Close();

    /** @brief 停钟并提交最终进度快照，避免延迟析构覆盖新会话的进度。 */
    bool PrepareClose();

    /** @brief 查询会话是否已成功打开书籍 */
    bool IsOpen() const {
        return open_;
    }

    /** @brief 最近一次打开或正文加载失败的具体原因。 */
    BookReadError Error() const {
        return error_;
    }

    /** @brief 借用当前书籍的路径、格式和文件信息 */
    const BookInfo& Info() const {
        return info_;
    }

    /** @brief 获取解析元数据和默认标题后用于展示的书名 */
    const std::string& Title() const {
        return display_title_;
    }

    /** @brief 获取供界面展示的作者信息 */
    const std::string& Author() const {
        return display_author_;
    }

    /** @brief 首次按目标框解码并缓存封面，无封面或解码失败时返回 false */
    bool EnsureCover(int max_w, int max_h);

    /** @brief 借用已准备的封面位图，尚未准备或失败时为空 */
    const RasterImage& Cover() const {
        return cover_;
    }

    /** @brief 获取当前阅读章节的页数，无目录 TXT 流式模式不提供总页数 */
    int PageCount() const;

    /** @brief 获取当前阅读章节内从零开始的页码 */
    int CurrentPage() const;

    /** @brief 跳到章内指定页；导航成功后由调用方保存进度。 */
    bool GoToPage(int page);

    /** @brief 按千分比跳到全书位置；导航成功后由调用方保存进度。 */
    bool GoToReadingProgress(int progress_x10);

    /** @brief 向后翻页，必要时加载下一章；不自动保存进度。 */
    bool NextPage();

    /** @brief 向前翻页，必要时加载上一章；不自动保存进度。 */
    bool PrevPage();
    enum class NavigationKind {
        kRefresh,       // 重新物化并绘制当前位置
        kDelta,         // 相对页数增量
        kToc,           // 目录项序号
        kProgress,      // 全书进度，千分比
        kBookmark,      // 书签 ID
        kChapterDelta,  // 相对目录项增量
    };

    /** @brief 在 UI 创建独立导航游标，不读盘；源会话和字体须保留到游标销毁。 */
    std::unique_ptr<BookSession> CreateNavigationCursor(NavigationKind kind, int& value) const;

    /**
     * @brief 在工作任务中加载游标索引、执行导航并物化目标页，不修改源会话
     * @param loading_chapter 可选的加载状态，调用方须保持到本次准备结束
     */
    bool PrepareNavigation(NavigationKind kind, int value, const std::atomic<bool>* cancelled = nullptr,
                           std::atomic<bool>* loading_chapter = nullptr);

    /**
     * @brief 在 UI 发布游标位置及读取缓存，旧资源交回游标由后台清理。
     * @return 会话与缓存匹配且发布成功时返回 true。
     */
    bool CommitNavigation(BookSession& cursor);

    /** @brief 是否还能向后翻（含跨章），不改状态 */
    bool HasNextPage() const;

    /** @brief 是否还能向前翻（含跨章），不改状态 */
    bool HasPrevPage() const;

    /** @brief 按需物化并借用当前页内容，读取或分配失败时返回空指针 */
    const Page* CurrentPageData() const;

    /** @brief 借用当前书籍的书签及其加载错误状态 */
    const BookBookmarkStore& BookmarkStore() const {
        return bookmarks_;
    }

    /** @brief 取得当前页的源位置半开区间，尚无可定位正文时返回 false */
    bool CaptureBookmarkRange(BookSourcePosition& first, BookSourcePosition& end) const;

    /** @brief 返回起点落在当前页内的首个书签编号，无匹配时返回零 */
    uint32_t CurrentPageBookmarkId() const;

    /** @brief 提取当前页书签及摘要快照，不执行文件保存。 */
    bool CaptureCurrentPageBookmark(BookBookmark& entry, std::string& error);

    /** @brief 接入已完成原子保存的书签快照，不访问存储。 */
    void AdoptBookmarks(BookBookmarkStore store) { bookmarks_ = std::move(store); }

    /** @brief 按书签的稳定源位置导航，书签不可用或导航失败时返回 false */
    bool GoToBookmark(uint32_t id);

    /** @brief 加载正文引用的图片并适配目标框，优先复用页图缓存 */
    bool LoadPageImage(const std::string& href, int max_w, int max_h, RasterImage& out);

    /** @brief 按资源路径和目标框匹配页图缓存，命中后拷贝到 out，不读盘 */
    bool TryCopyCachedPageImage(const std::string& href, int max_w, int max_h, RasterImage& out) const;

    /** @brief 查询页图缓存并更新使用顺序，不拷贝像素；允许解码任务调用 */
    bool HasCachedPageImage(const std::string& href, int max_w, int max_h) const;

    /** @brief 按当前可用 PSRAM 粗估能否继续预取一张页图 */
    bool CanPrefetchPageImage() const;

    /** @brief 阅读锚点变化时淘汰最旧缓存，为继续逐项预取留出空间。 */
    void PreparePageImagePrefetch(const std::string& anchor);

    /** @brief 释放页图缓存，供公共内存回收及资源收尾使用 */
    void ClearPageImageCache();

    /** @brief 写入原始页图缓存，按内存预算淘汰旧图；允许解码任务调用 */
    void SetCachedPageImage(const std::string& href, int max_w, int max_h, RasterImage&& img,
                            uint32_t reclaim_generation);

    /** @brief 获取当前阅读章节的从零开始序号 */
    int ChapterIndex() const {
        return chapter_index_;
    }

    /** @brief 获取当前会话的阅读章节数，独立于原生目录项数 */
    int ChapterCount() const {
        return chapter_count_;
    }

    /** @brief 获取统一目录项数，无可用目录时返回零 */
    int TocCount() const;

    /** @brief 借用指定目录项，序号无效时返回空指针 */
    const TocEntry* TocAt(int index) const;

    /** @brief 判断首个目录目标是否位于全书第一页之后 */
    bool TocStartsAfterFirstPage() const;

    /** @brief 按书籍语言获取目录首页入口标题，缺少语言声明时按目录文字判断 */
    const char* TocHomeTitle() const;

    /** @brief 获取当前阅读位置对应的目录项序号，无匹配时返回 -1 */
    int CurrentTocIndex() const;

    /** @brief 导航至目录项对应的 TXT 起始页或章节位置 */
    bool GoToToc(int index);

    /** @brief 返回底栏使用的当前章节标题，无可用标题时为空 */
    std::string CurrentTocTitle() const;

    /** @brief 获取全书阅读进度的千分比，范围为 0 至 1000 */
    int ReadingProgressX10() const;

    /** @brief 查询是否为整页图片书，此模式适配阅读区域而不按文字参数排版 */
    bool IsPageImagesMode() const {
        return page_images_mode_;
    }

    /** @brief 读取最新进度的千分比，无有效百分比字段时返回 -1 */
    static int PeekReadingProgressX10(const char* book_path);

    /** @brief 读取最新进度中的累计阅读秒数，无时长字段时返回零 */
    static uint32_t PeekReadingSeconds(const char* book_path);

    /** @brief 读取单书今日阅读秒数，时钟无效或记录日期非今日时返回零，不改写文件 */
    static uint32_t PeekReadingDailySeconds(const char* book_path);

    // 一次打开 .pos 取进度/终身/当日（供开机上报等批量路径，避免连开三次）。
    struct ProgressPeek {
        int progress_x10 = -1;  // -1=无有效百分比
        uint32_t reading_seconds = 0;
        uint32_t daily_seconds = 0;  // 已按「是否今日」过滤
    };

    /** @brief 一次读取进度及累计、今日秒数，优先使用待执行快照，其次读取正式或可恢复记录 */
    static ProgressPeek PeekProgress(const char* book_path);

    /** @brief 获取内存中已结算的累计阅读秒数，活动计时段需先停止或保存才能计入 */
    uint32_t ReadingSeconds() const {
        return reading_seconds_;
    }

    /** @brief 获取内存中已结算的当日秒数，不包含尚未结算的活动计时段 */
    uint32_t ReadingDailySeconds() const {
        return daily_seconds_;
    }

    /** @brief 开始本段阅读计时，重复调用不会重置正在累计的时间 */
    void StartReadingClock();

    /** @brief 结束本段阅读计时并结算累计及有效日期内的秒数，允许重复停止 */
    void StopReadingClock();

    /** @brief 本段阅读墙钟是否正在累计 */
    bool IsReadingClockActive() const {
        return reading_clock_active_;
    }

    /**
     * @brief 提交 POS4 正文锚点与进度快照；合并同书待执行写入，由存储任务落盘。
     * @return 快照被接纳时返回 true；模拟器直接返回写盘结果。
     */
    bool SaveProgress();

    /** @brief 浅睡卸 SD 前关书/字体相关 FILE*（保留分页与元数据） */
    void DetachSdHandles();

    /** @brief SD 重挂后重新 fopen 书文件句柄 */
    bool ReattachSdHandles();

    // 排版锚点：TXT 使用源字节；章节书优先使用内容块和块内偏移。
    struct LayoutAnchor {
        bool valid = false;
        bool txt = false;
        uint32_t txt_byte_off = 0;
        bool txt_at_para_start = true;  // 锚点行是否为段首（决定首行缩进）
        int chapter = 0;
        int page = 0;
        int page_count = 1;
        uint32_t source_chapter = 0; // 原始正文编号，独立于阅读章节
        uint32_t source_block = UINT32_MAX, source_offset = 0;  // 章节页首的稳定源位置
        bool source_para_start = true;                          // 页首是否需要段首缩进
    };

    /** @brief 捕获当前页的稳定源位置供换参重排，未打开会话时返回无效锚点 */
    LayoutAnchor CaptureLayoutAnchor() const;

    /** @brief 查询当前阅读章节是否仍需后台重排 */
    bool LiveRelayoutPending() const {
        return (structure_pending_ && !structure_failed_.load()) || chapter_relayout_pending_;
    }

    /** @brief 查询目录结构是否尚未完成；期间仅依赖源锚点阅读 */
    bool StructurePending() const {
        return structure_pending_;
    }

    /** @brief 当前章节是否等待按新参数准备首个可读页。 */
    bool NeedsCurrentPagePreview() const {
        return chapter_preview_pending_;
    }

    /** @brief 当前章节已有新参数预览页，完整分页仍可在后台继续。 */
    bool HasCurrentPagePreview() const {
        return !structure_pending_ && !txt_mode_ && live_preview_active_ && !chapter_preview_pending_;
    }

    /** @brief 清理即时预览页面、源位置及短历史，结束预览状态 */
    void ClearLivePreview();

    /**
     * @brief 中止当前章节的后台排版，使过期结果无法发布
     * @return 中止后的排版代次，供后续任务检查取消
     */
    uint32_t AbortChapterLayout();

    /** @brief 待机时取消并阻止后台编排；恢复前须等待旧任务结束。 */
    void SetBackgroundPaused(bool paused);

    /**
     * @brief 在 UI 记录当前内容位置，交由页面任务准备新参数预览
     * @param anchor_inout 传入可空；成功时写出稳定正文锚点供后台对齐
     * @return 先保留当前页，后续由现有排版任务调用 FinishLiveRelayout 完成当前章节重排
     */
    bool BeginLiveRelayout(LayoutAnchor* anchor_inout);

    /**
     * @brief 在既有排版任务中重建当前阅读章节，并对齐正文锚点
     * @param generation 任务提交时取得的排版代次
     * @note 完成结果经 UI 提交后替换旧章节页面。
     */
    bool FinishLiveRelayout(const LayoutAnchor& anchor, uint32_t generation);

    /** @brief 在 UI 任务发布已完成的排版结果，避免后台替换正在显示的页。 */
    void CommitLiveRelayout(const LayoutAnchor& anchor);

    /** @brief 查询当前章节页数是否仍待重排完成。 */
    bool PageCountsEstimated() const {
        return LiveRelayoutPending();
    }

private:
    static constexpr size_t kMaxTxtTocEntries = 8000; // TXT 目录容量上限
    static constexpr uint32_t kSourcePositionVersion = 1; // 原始正文锚点解释版本
    bool CloseSdHandles();
    const BookSession* navigation_source_ = nullptr;           // 后台只读游标的源会话
    const std::atomic<bool>* navigation_cancelled_ = nullptr;  // 仅页面准备期间有效的取消标记
    std::atomic<bool>* navigation_loading_chapter_ = nullptr; // 工作任务持有的章节加载状态
    bool txt_preview_relayout_pending_ = false;                // 换参后的预览交给前台工作任务计算
    bool navigation_replaced_pages_ = false;                   // 游标已构建新章节
    LayoutAnchor navigation_anchor_;                           // 已发布页面的源位置锚点
    BookSourcePosition navigation_bookmark_;                   // 导航请求对应的单个书签位置
    BookBookmarkStore bookmarks_;

    // 与永久索引的 6 字节行记录保持一致，可直接窗口读取。
    struct __attribute__((packed)) TxtLineRef {
        uint32_t off = 0;
        uint16_t len = 0;
    };

    // .pos 解析快照（堆外；Peek/Open 共用，避免三套 fread 分叉）
    struct PosFileData {
        char magic[4] = {};
        uint32_t chapter = 0;
        uint32_t page = 0;
        uint32_t pct_x10 = 0;
        bool has_pct = false;
        uint32_t reading_seconds = 0;
        bool has_seconds = false;
        uint32_t day_id = 0;
        uint32_t daily_seconds = 0;
        bool has_daily = false;
        bool has_source_anchor = false; // POS4 的稳定正文锚点
        BookSourcePosition source_anchor;
        uint64_t file_size = 0; // POS4 书籍身份
        int64_t mtime = 0;
        uint32_t format = 0, parser_version = 0;
        bool has_txt_anchor = false;  // 是否有可用的 TXT 源字节锚点
        uint32_t txt_anchor = 0;      // TXT 页首源字节
        bool txt_para_start = true;   // 锚点是否为段落起始
    };
    enum class PosLoadStatus : int8_t { kMissing = 0, kInvalid = -1, kOk = 1 };

    bool OpenImpl(const BookInfo& info);
    bool OpenTxt(const char* path, size_t known_size, const PosFileData& position);
    bool OpenEpub(const char* path);
    bool OpenEbook(const char* path);
    bool PaginateChapter(int chapter, BookPageStore& built, uint32_t generation, const LayoutAnchor* preview = nullptr,
                         const std::atomic<bool>* cancelled = nullptr) const;
    struct ReadingChapter {
        BookSourcePosition first, end; // 原始正文位置的半开区间
        uint64_t content_begin = 0, content_end = 0; // 全书正文累计位置
        int toc_index = -1; // 无目录前言为负数
    };
    std::shared_ptr<const std::vector<ReadingChapter>> reading_chapters_; // 导航游标共享只读目录范围
    std::shared_ptr<const std::vector<uint64_t>> source_sizes_; // 各原始正文累计内容字节数
    uint32_t txt_range_end_ = UINT32_MAX; // 章节预览的原始字节上界
    int source_chapter_count_ = 0; // 排版分组前的正文数量
    const std::vector<TocEntry>& TocEntries() const;
    struct TxtStructureScan;
    std::shared_ptr<TxtStructureScan> txt_structure_scan_; // 后台 TXT 扫描的可恢复状态
    std::unique_ptr<BookSession> pending_structure_; // 后台独占；完成后在 UI 发布
    std::vector<uint64_t> structure_scan_sizes_, structure_scan_toc_content_; // 已完成源文件的扫描结果
    int structure_scan_source_ = 0; // 仅在完整扫描一个原始正文后推进
    bool structure_staging_ = false; // 暂存对象使用自己的目录，文档句柄借用源会话
    bool structure_cache_checked_ = false; // 缓存只在后台首次尝试读取
    bool structure_pending_ = false; // 首屏不等待全书结构
    std::atomic<bool> structure_failed_{false}; // 失败后保留预览，不自动反复扫描
    int restored_progress_x10_ = 0; // 全书长度未就绪时保留的旧进度
    int preview_toc_index_ = -1; // 仅对已解析的目录目标提供当前章节名
    LayoutAnchor source_preview_anchor_, source_preview_next_;
    std::vector<LayoutAnchor> source_preview_history_; // 结构未就绪时的有界回退历史
    bool BuildReadingChapters(uint32_t generation);
    bool ScanTxtChapters(uint32_t generation);
    bool StructureScanInterrupted(uint32_t generation) const;
    bool FinishStructureLayout(const LayoutAnchor& anchor, uint32_t generation);
    bool PrepareSourcePreview(const LayoutAnchor& anchor);
    bool ResolveTocPreview(int index);
    uint32_t StructureFingerprint() const;
    bool LoadStructureCache(uint32_t generation);
    bool SaveStructureCache(uint32_t generation) const;
    int ReadingChapterForPosition(const BookSourcePosition& position) const;
    bool PaginateSourceChapter(int chapter, BookPageStore& built, uint32_t generation,
                                const LayoutAnchor* preview, const std::atomic<bool>* cancelled,
                                const ReadingChapter* range = nullptr, bool* preview_has_more = nullptr,
                                size_t page_limit = SIZE_MAX) const;
    bool LoadChapter(int chapter);
    bool GoToBookmarkPosition(const BookSourcePosition& position);
    void ClearTxtIndex();
    bool EnsureTxtFile() const;
    mutable bool sd_handles_detached_ = false;  // 浅睡卸 SD 期间禁止再 fopen，避免句柄在 unmount 前被意外拉回
    static std::string TxtIndexPath(const char* txt_path);
    static std::string ProgressPath(const char* book_path);
    static PosLoadStatus LoadPosFile(const char* pos_path, PosFileData& out);
    static PosLoadStatus LoadPosFileOnce(const char* pos_path, PosFileData& out);
    // 本地日历日 YYYYMMDD；时钟不可信返回 0。
    static uint32_t LocalCalendarDayId();
    // 若 day_id 对应今日秒数（时钟有效且日戳匹配）；否则 0。
    static uint32_t DailySecondsIfToday(const PosFileData& data);
    uint32_t FontFingerprint() const;
    bool TryLoadLegacyTxtIndex(const char* txt_path, size_t file_size);
    void FoldReadingClock();
    void EnsureDailyBucket();
    void AddReadingSecondsSaturated(uint32_t delta_sec);
    void AddDailySecondsSaturated(uint32_t delta_sec);
    void ReportOpenProgress(int percent);
    void ClearToc();
    bool IsChapteredBook() const;
    bool FillTxtLivePreview(uint32_t byte_off, bool at_para_start);
    bool SeekTxtPreview(uint32_t byte_off);

    std::atomic<bool> background_paused_{false}; // 待机期间持续阻止正文准备
    mutable std::atomic<BookReadError> error_{BookReadError::kNone};  // 打开及正文加载错误
    bool open_ = false;
    bool close_progress_queued_ = false;  // UI 已提交退出快照，后台析构不再保存
    OpenProgressFn progress_fn_ = nullptr;
    void* progress_user_ = nullptr;
    int last_progress_pct_ = -1;
    BookInfo info_{};
    std::string display_title_;
    std::string display_author_;
    const lv_font_t* font_ = nullptr;
    const lv_font_t* glyph_font_ = nullptr;  // 与绘制一致的完整字形边界，步进仍使用分页字体
    bool vertical_writing_ = false;
    lv_coord_t physical_w_ = 440;
    lv_coord_t physical_h_ = 700;
    lv_coord_t LayoutLineHeight() const;
    TextLineLayout LineLayout() const;
    lv_coord_t LayoutGlyphWidth(uint32_t cp) const;
    lv_coord_t LayoutIndentWidth() const;
    lv_coord_t viewport_w_ = 440;
    lv_coord_t viewport_h_ = 700;
    int line_spacing_percent_ = kReaderLineSpacingPercentDefault;
    lv_coord_t para_gap_ = kReaderParaGapDefault;
    TextStyle text_style_;  // 导航游标复制的排版快照

    std::unique_ptr<EpubDocument> epub_;
    std::unique_ptr<EbookDocument> ebook_;
    // .ebook 可见章 → 文件章下标（跳过空章）
    std::vector<int> ebook_spine_;
    bool page_images_mode_ = false;

    // TXT：无目录时流式阅读；旧索引仅用于旧进度的页码到字节位置转换。
    bool txt_mode_ = false; // 仅无目录 TXT 使用逐页流式模式
    bool txt_as_gb18030_ = false;
    bool txt_empty_ = false;
    mutable FILE* txt_fp_ = nullptr;
    BookDiskArray<TxtLineRef> txt_lines_;  // SD 行索引，仅缓存固定窗口
    size_t txt_lines_len_ = 0;
    BookDiskArray<uint32_t> txt_page_first_line_;  // SD 页索引，仅缓存固定窗口
    size_t txt_page_count_ = 0;

    // 换参预览先替换当前页；TXT 可流式翻页，章节书随后发布完整分页。
    bool live_preview_active_ = false;
    bool chapter_preview_pending_ = false;   // 章节首屏由页面任务准备，完整章节随后重建
    LayoutAnchor chapter_preview_anchor_{};  // 新字号首屏的内容锚点
    int pending_chapter_anchor_page_ = 0;    // 后台完整章节中包含锚点的页码
    uint32_t live_preview_byte_off_ = 0;
    bool live_preview_at_para_start_ = true;
    uint32_t live_preview_next_off_ = 0;  // 下一页字节锚点（页满未消费处）
    bool live_preview_next_at_para_start_ = true;
    bool live_preview_has_next_ = false;
    Page live_preview_page_;
    // ponytail: 预览上一页靠短历史；上限内可回退，超出只能停在栈底
    static constexpr size_t kLivePreviewHistMax = 48;

    struct LivePreviewHistEnt {
        uint32_t byte_off = 0;
        bool at_para_start = true;
    };

    std::vector<LivePreviewHistEnt> live_preview_hist_;

    BookPageStore pending_pages_;  // 后台构建完成后由 UI 发布
    BookPageStore pages_;          // 分页正文落 SD，内存保留当前及相邻页
    int page_index_ = 0;
    int chapter_index_ = 0;
    int chapter_count_ = 1;
    bool chapter_relayout_pending_ = false;       // 当前章待现有排版任务重排
    std::atomic<uint32_t> chapter_layout_generation_{0};  // 中止或重排递增；后台写回时拒绝过期结果
    // 后台重排与章节装载串行，避免复用文档句柄时发生竞态
    mutable std::mutex chapter_io_mu_;

    // TXT 解析目录；EPUB 保存原生目录，首项目标页用于首页入口判断；EBOOK 保存章标题。
    std::vector<TocEntry> toc_;
    std::vector<BookSourcePosition> epub_toc_positions_;  // 原生目录映射到稳定正文块，随导航游标发布

    RasterImage cover_;
    bool cover_tried_ = false;

    struct CachedPageImage {
        std::string href;          // 书内资源路径
        int max_w = 0, max_h = 0;  // 解码目标框
        RasterImage image;         // 保留原始 L8，不写入显示压暗结果
    };

    std::string page_image_anchor_; // 最近一次扩展的阅读锚点
    size_t PageImageCacheBytesLocked() const;
    size_t PageImageCacheBudgetLocked(size_t used) const;
    mutable std::atomic<bool> page_image_reclaim_pending_{false}; // 忙碌缓存在下一安全点清理
    void ReclaimPageImagesLocked() const;
    mutable std::mutex page_image_cache_mu_;                   // 仅保护图片缓存，不持锁读盘或解码
    mutable std::vector<CachedPageImage> cached_page_images_;  // 从久未使用到最近使用排列
    #ifndef EBOOK_EMULATOR
    static void ReclaimOptionalMemory(void* context);
    FirmwareCacheReclaimer memory_reclaimer_{ReclaimOptionalMemory, this}; // 先注销回调，再销毁缓存
    #endif

    // 终身累计阅读秒；仅 Close/Open 边界重置内存，Open 后从 .pos 恢复
    uint32_t reading_seconds_ = 0;
    // 单书当日秒 + 对应本地日戳（YYYYMMDD；0=未知/未对时）
    uint32_t daily_seconds_ = 0;
    uint32_t reading_day_id_ = 0;
    bool reading_clock_active_ = false;
    int64_t reading_clock_since_us_ = 0;  // esp_timer_get_time()，仅 active 时有效
};
