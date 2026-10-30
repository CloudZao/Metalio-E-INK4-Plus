#pragma once

#include <cstdint>
#include <algorithm>
#include <atomic>
#include <cstdio>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "ebook_document.h"
#include "book_disk_cache.h"
#include "epub_document.h"
#include "lvgl.h"
#include "reader_types.h"
#include "book_bookmarks.h"
#include "txt_chapter.h"

namespace reader {

// 统一阅读会话：TXT / EPUB / EBOOK，分页、翻页、封面与插图解码。
//
// TXT 大文件策略（真流式）：
// - 打开先显示当前页；现有排版任务单趟建立 SD 行索引和页索引
// - 不把全文（无论 GB18030/UTF-8）留在 RAM
// - CurrentPageData() 按行 seek+读+转码，只物化当前页
// - 按 \\n 分段：段首两字宽 pad 缩进；源文空行仅分段，不渲染占位行
// - 顶格章节 TOC（第N章/第N回/数字短章名）与分页同趟扫描，写入 .idx
// - 换参：当前页即时预览 + 后台全量建索引；预览态可邻页流式翻页（短历史回退）
// - 索引可缓存到 SD：与 txt 同目录 `<书名>.txt.idx`（书库在 /sdcard/metalio/e-ink/books）
// - 阅读状态：同目录 <书名>.*.pos（POS3 及可选 TXT 字节锚点），下次打开续读
//   兼容读 POS1/POS2；写出始终 POS3
//
// EBOOK：按章 fseek + 分块 zlib；章内图文以 ContentBlock 分页（与 EPUB 同路径）。
// 段首缩进：去掉源文前导空白，渲染侧 pad_left 两字宽（与 TXT 同策略）。
// 打开时以索引 flags/uncompressed_total 轻量过滤空章，不再全量解压扫章。
// 超大章：按物理块/HTML 流解析，分页正文落 SD；内存只保留固定窗口和当前页。
// 全书页数：打开时只读 .cpg；未命中则先估算，后台只计页扫各章写入缓存；换参后同样后台重算。
// 后台计页与章节读取串行；UI 导航先取消扫描，再由独立游标异步准备页面。
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
    BookSession() = default;
    ~BookSession();

    BookSession(const BookSession&) = delete;
    BookSession& operator=(const BookSession&) = delete;

    void SetFont(const lv_font_t* font) { font_ = font; }
    void SetVerticalWriting(bool vertical) {
        vertical_writing_ = vertical && !page_images_mode_;
        SetViewport(physical_w_, physical_h_);
    }
    bool VerticalWriting() const { return vertical_writing_; }
    void SetViewport(lv_coord_t width, lv_coord_t height) {
        if (physical_w_ != width || physical_h_ != height) ClearPageImageCache();
        physical_w_ = width;
        physical_h_ = height;
        // Each vertical column is paginated as a line in a transposed viewport.
        viewport_w_ = vertical_writing_ ? height : width;
        viewport_h_ = vertical_writing_ ? width : height;
    }
    lv_coord_t ViewportWidth() const { return physical_w_; }
    lv_coord_t ViewportHeight() const { return physical_h_; }
    /** @brief 设置字体行框的相对疏密百分比及额外段距，影响分页指纹。 */
    void SetLineSpacing(int spacing_percent, lv_coord_t para_gap) {
        line_spacing_percent_ = page_images_mode_ ? 0 : std::clamp(spacing_percent, 0, 100);
        para_gap_ = page_images_mode_ ? 0 : std::max<lv_coord_t>(0, para_gap);
    }
    int LineSpacingPercent() const { return line_spacing_percent_; }
    lv_coord_t ParaGap() const { return para_gap_; }

    // 打开过程进度 0–100；可在非 LVGL 线程回调，调用方自行投递 UI。
    using OpenProgressFn = void (*)(int percent, void* user);
    void SetOpenProgress(OpenProgressFn fn, void* user) {
        progress_fn_ = fn;
        progress_user_ = user;
    }

    bool Open(const BookInfo& info);
    void Close();
    /** @brief 停钟并提交最终进度快照，避免延迟析构覆盖新会话的进度。 */
    bool PrepareClose();
    bool IsOpen() const { return open_; }
    /** @brief 最近一次打开或正文加载失败的具体原因。 */
    BookReadError Error() const { return error_; }

    const BookInfo& Info() const { return info_; }
    const std::string& Title() const { return display_title_; }
    const std::string& Author() const { return display_author_; }

    bool EnsureCover(int max_w, int max_h);
    const RasterImage& Cover() const { return cover_; }

    int PageCount() const;
    int CurrentPage() const;
    /** 全书页数（TXT=PageCount；章节书=各章页数之和，未就绪时用估算） */
    int BookPageCount() const;
    /** 全书当前页 0-based（TXT=CurrentPage；章节书=前章页数之和+章内页） */
    int BookCurrentPage() const;
    /** @brief 跳到章内指定页；导航成功后由调用方保存进度。 */
    bool GoToPage(int page);
    /** @brief 按千分比跳到全书位置；导航成功后由调用方保存进度。 */
    bool GoToReadingProgress(int progress_x10);
    /** @brief 向后翻页，必要时加载下一章；不自动保存进度。 */
    bool NextPage();
    /** @brief 向前翻页，必要时加载上一章；不自动保存进度。 */
    bool PrevPage();
    enum class NavigationKind {
        kRefresh, // 重新物化并绘制当前位置
        kDelta, // 相对页数增量
        kToc, // 目录项序号
        kProgress, // 全书进度，千分比
        kBookmark, // 书签 ID
        kChapterDelta, // 相对目录项增量
    };
    /** @brief 在 UI 创建独立导航游标，不读盘；源会话和字体须保留到游标销毁。 */
    std::unique_ptr<BookSession> CreateNavigationCursor(NavigationKind kind, int& value) const;
    /** @brief 在工作任务中加载游标索引、执行导航并物化目标页，不修改源会话。 */
    bool PrepareNavigation(NavigationKind kind, int value);
    /**
     * @brief 在 UI 发布游标位置及读取缓存，旧资源交回游标由后台清理。
     * @return 会话与缓存匹配且发布成功时返回 true。
     */
    bool CommitNavigation(BookSession& cursor);
    /** 是否还能向后翻（含跨章），不改状态 */
    bool HasNextPage() const;
    /** 是否还能向前翻（含跨章），不改状态 */
    bool HasPrevPage() const;

    const Page* CurrentPageData() const;
    const BookBookmarkStore& BookmarkStore() const { return bookmarks_; }
    bool CaptureBookmarkRange(BookSourcePosition& first,BookSourcePosition& end) const;
    uint32_t CurrentPageBookmarkId() const;
    bool AddCurrentPageBookmark(std::string& error);
    bool RemoveBookmarks(const std::vector<uint32_t>& ids,std::string& error) { return bookmarks_.Remove(ids,error); }
    bool GoToBookmark(uint32_t id);

    bool LoadPageImage(const std::string& href, int max_w, int max_h, RasterImage& out);

    /** @brief 按资源路径和目标框匹配页图缓存，命中后拷贝到 out，不读盘 */
    bool TryCopyCachedPageImage(const std::string& href, int max_w, int max_h, RasterImage& out) const;

    /** @brief 查询页图缓存并更新使用顺序，不拷贝像素；允许解码任务调用 */
    bool HasCachedPageImage(const std::string& href, int max_w, int max_h) const;
    /** @brief 按当前可用 PSRAM 返回单侧预取张数，上限 10；内存紧张时减少或停取 */
    int PageImagePrefetchRadius() const;
    /** @brief 释放页图缓存，供解码内存不足时回退及资源收尾使用 */
    void ClearPageImageCache();
    /** @brief 写入原始页图缓存，按内存预算淘汰旧图；允许解码任务调用 */
    void SetCachedPageImage(const std::string& href, int max_w, int max_h, RasterImage&& img);

    int ChapterIndex() const { return chapter_index_; }
    int ChapterCount() const { return chapter_count_; }

    // 统一目录（TXT 解析 TOC / EPUB·EBOOK 章表）。无目录时 Count=0。
    int TocCount() const;
    const TocEntry* TocAt(int index) const;
    // 当前阅读位置对应的目录项；无则 -1。
    int CurrentTocIndex() const;
    // 跳到目录项起始页（TXT）或 spine/ebook 章。
    bool GoToToc(int index);
    // 底栏用：当前章名（可能截断）；无目录时返回空串。
    std::string CurrentTocTitle() const;
    // 全书阅读进度 ×10（0–1000，一位小数）。
    int ReadingProgressX10() const;

    // 单图页面 EPUB / .ebook 整页转图书：适配阅读区域，文字设置不参与排版。
    bool IsPageImagesMode() const { return page_images_mode_; }

    // 窥探 SD 上的 .pos；无有效进度（或旧版无百分比字段）返回 -1；否则 0–1000。
    static int PeekReadingProgressX10(const char* book_path);
    // 窥探终身累计阅读秒；无 .pos / 旧 POS1 无时长字段返回 0。
    static uint32_t PeekReadingSeconds(const char* book_path);
    // 窥探单书当日阅读秒；非 POS3、时钟无效、或日戳非今日 → 0（Peek 不改盘）。
    static uint32_t PeekReadingDailySeconds(const char* book_path);

    // 一次打开 .pos 取进度/终身/当日（供开机上报等批量路径，避免连开三次）。
    struct ProgressPeek {
        int progress_x10 = -1;       // -1=无有效百分比
        uint32_t reading_seconds = 0;
        uint32_t daily_seconds = 0;  // 已按「是否今日」过滤
    };
    static ProgressPeek PeekProgress(const char* book_path);

    // 内存中的终身累计秒（含尚未 fold 的活跃时钟段需先 Stop/Save 才完整）。
    uint32_t ReadingSeconds() const { return reading_seconds_; }
    // 内存中的当日秒（同上；未 fold 段不计入；换日由 EnsureDailyBucket 处理）。
    uint32_t ReadingDailySeconds() const { return daily_seconds_; }

    // 开始/停止本段阅读墙钟（幂等）。Stop 会 fold 进终身累计与当日（时钟有效时）。
    void StartReadingClock();
    void StopReadingClock();
    /** @brief 本段阅读墙钟是否正在累计 */
    bool IsReadingClockActive() const { return reading_clock_active_; }

    /**
     * @brief 提交 POS3 进度快照；合并同书待执行写入，由存储任务落盘。
     * @return 快照被接纳时返回 true；模拟器直接返回写盘结果。
     */
    bool SaveProgress();

    /** @brief 浅睡卸 SD 前关书/字体相关 FILE*（保留分页与元数据） */
    void DetachSdHandles();
    /** @brief SD 重挂后重新 fopen 书文件句柄 */
    bool ReattachSdHandles();

    /** 排版锚点：TXT=页首源字节；章节书=章+页比例 */
    struct LayoutAnchor {
        bool valid = false;
        bool txt = false;
        uint32_t txt_byte_off = 0;
        bool txt_at_para_start = true;  // 锚点行是否为段首（决定首行缩进）
        int chapter = 0;
        int page = 0;
        int page_count = 1;
    };

    LayoutAnchor CaptureLayoutAnchor() const;
    /** @brief 是否仍需后台完成 TXT 索引或当前章节重排 */
    bool LiveRelayoutPending() const { return live_preview_active_ || chapter_relayout_pending_; }
    void ClearLivePreview();
    /** @brief TXT 换参后由已有排版任务重建 SD 页表 */
    bool NeedsTxtIndexRebuild() const { return txt_mode_ && !txt_empty_; }
    /** @brief 章节书换参后先重排当前章，再由已有计页任务更新全书页数 */
    bool NeedsChapterPageRebuild() const;
    /** @brief 中止全书计页，保留同一排版下已经完成的章节结果。 */
    void AbortChapterPageTable();
    /** @brief 排版参数改变时作废全书页表及待发布结果。 */
    void InvalidateChapterPageTable();
    /** @brief 请求 TXT 扫描在安全点暂停，返回是否已确认暂停；仅供前台预览导航。 */
    bool PauseTxtPaginate();
    /** @brief 恢复同一次 TXT 扫描，保留文件位置、索引和折行状态。 */
    void ResumeTxtPaginate();
    /** @brief 参数改变或离页时取消 TXT 扫描，使后台任务退出。 */
    void AbortTxtPaginate();
    /** @brief 待机时取消并阻止后台编排；恢复前须等待旧任务结束。 */
    void SetBackgroundPaused(bool paused);

    /**
     * @brief 按当前 font/gaps/viewport 即时重排当前位置（LVGL 任务）
     * @param anchor_inout 传入可空；成功时写出锚点供 TXT 后台对齐
     * @return 先保留当前页，后续由现有排版任务调用 FinishLiveRelayout 完成索引或章节重排
     */
    bool BeginLiveRelayout(LayoutAnchor* anchor_inout);

    /**
     * @brief 在既有排版任务中重建 TXT 索引或当前章，并对齐锚点
     * @note 期间使用独立 FILE*；允许为前台预览暂停，完成结果经 UI 提交后清除预览。
     */
    bool FinishLiveRelayout(const LayoutAnchor& anchor);
    /** @brief 在 UI 任务发布已完成的排版结果，避免后台替换正在显示的页。 */
    void CommitLiveRelayout(const LayoutAnchor& anchor);
    /** @brief 在 UI 任务取出有界批次的目录和计页结果，不等待全书编排完成。 */
    bool PollLayoutProgress();
    /** @brief 页数仍为扫描前缀推算值，显示时应标记暂估。 */
    bool PageCountsEstimated() const { return LiveRelayoutPending() || NeedsChapterPageRebuild(); }
    /**
     * @brief 在工作任务补齐未知章节页数，中止后保留已完成章节供下次续扫。
     * @note 不改当前 pages_/阅读位置；前台请求中止后异步等待任务结束。
     */
    bool FinishChapterPageTable();
    /** 打开时：仅尝试 .cpg；未命中则 false（交给后台 FinishChapterPageTable） */
    bool WarmChapterPageTableFromCache();

private:
    bool CloseSdHandles();
    const BookSession* navigation_source_ = nullptr; // 后台只读游标的源会话
    bool txt_preview_relayout_pending_ = false; // 换参后的预览交给前台工作任务计算
    bool navigation_replaced_pages_ = false; // 游标已构建新章节
    LayoutAnchor navigation_anchor_; // 已发布的 TXT 源锚点
    BookSourcePosition navigation_bookmark_; // 导航请求对应的单个书签位置
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
        bool has_txt_anchor = false; // 是否有可用的 TXT 源字节锚点
        uint32_t txt_anchor = 0; // TXT 页首源字节
        bool txt_para_start = true; // 锚点是否为段落起始
    };
    enum class PosLoadStatus : int8_t { kMissing = 0, kInvalid = -1, kOk = 1 };

    bool OpenImpl(const BookInfo& info);
    bool OpenTxt(const char* path, size_t known_size = 0);
    bool OpenEpub(const char* path);
    bool OpenEbook(const char* path);
    bool PaginateChapter(int chapter, BookPageStore& built) const;
    bool CountChapterPages(int chapter, int& out_pages) const;
    bool RebuildChapterPageTable();
    static std::string ChapterPageCachePath(const char* book_path);
    bool TryLoadChapterPageCache();
    bool SaveChapterPageCache(const std::vector<uint32_t>& counts) const;
    void NoteChapterPageCount(int chapter, int pages);
    bool LoadChapter(int chapter);
    bool GoToBookmarkPosition(const BookSourcePosition& position);
    bool StreamPaginateTxt(FILE* fp, size_t file_size);
    bool WaitTxtPaginate(uint32_t generation) const;
    void MaterializeTxtPage(int page) const;
    void ClearTxtIndex();
    bool GrowTxtLines(size_t need);
    bool GrowTxtPageStarts(size_t need);
    bool EnsureTxtFile() const;
    /** @brief 浅睡卸 SD 期间禁止再 fopen，避免句柄在 unmount 前被意外拉回 */
    mutable bool sd_handles_detached_ = false;
    static std::string TxtIndexPath(const char* txt_path);
    static std::string ProgressPath(const char* book_path);
    static PosLoadStatus LoadPosFile(const char* pos_path, PosFileData& out);
    static PosLoadStatus LoadPosFileOnce(const char* pos_path, PosFileData& out);
    // 本地日历日 YYYYMMDD；时钟不可信返回 0。
    static uint32_t LocalCalendarDayId();
    // 若 day_id 对应今日秒数（时钟有效且日戳匹配）；否则 0。
    static uint32_t DailySecondsIfToday(const PosFileData& data);
    uint32_t FontFingerprint() const;
    bool TryLoadTxtIndex(const char* txt_path, size_t file_size);
    bool SaveTxtIndex(const char* txt_path, size_t file_size, const std::vector<TocEntry>& entries,
                      uint32_t generation) const;
    // 读 .pos：POS1/2/3；seconds/daily 按版本填充（缺省 0）。
    bool ReadProgressPos(uint32_t& chapter, uint32_t& page, uint32_t* seconds_out = nullptr,
                         uint32_t* day_id_out = nullptr, uint32_t* daily_out = nullptr);
    void FoldReadingClock();
    void EnsureDailyBucket();
    void AddReadingSecondsSaturated(uint32_t delta_sec);
    void AddDailySecondsSaturated(uint32_t delta_sec);
    void ReportOpenProgress(int percent);
    void ClearToc();
    void FinalizeTocPages();
    int PageIndexForLine(uint32_t line) const;
    int PageIndexForTxtByteOff(uint32_t byte_off) const;
    bool TryPushTxtChapter(std::string title, uint32_t first_line);
    bool IsChapteredBook() const;
    bool FillTxtLivePreview(uint32_t byte_off, bool at_para_start);
    bool SeekTxtPreview(uint32_t byte_off);

    // StreamPaginateTxt 建新目录时指向临时 vector，避免 ClearToc 导致编排中章节标题乱跳
    std::vector<TocEntry>* toc_build_ = nullptr;
    std::atomic<bool> background_paused_{false}; // 待机期间持续阻止后台编排
    mutable std::mutex layout_progress_mu_; // 只保护小批次交接，不持锁排版或操作 UI
    std::vector<TocEntry> pending_toc_; // 至多 32 条增量；完成时接管最终目录
    bool pending_toc_complete_ = false; // 待发布目录为完整快照
    uint32_t pending_txt_bytes_ = 0, pending_txt_pages_ = 0; // 扫描前缀
    uint32_t txt_indexed_bytes_ = 0, txt_indexed_pages_ = 0; // UI 已发布的扫描前缀
    std::atomic<uint32_t> txt_preview_anchor_{0}; // UI 当前页源位置，供扫描任务定位
    uint32_t pending_anchor_ = 0, pending_anchor_page_ = 0; // 扫描任务定位的当前位置
    uint32_t indexed_anchor_ = 0, indexed_anchor_page_ = 0; // UI 当前可用的精确页位置
    std::vector<std::pair<int, uint32_t>> pending_chapter_counts_; // 小批次章节计页结果
    std::vector<uint32_t> completed_chapter_counts_; // 扫描完成后的全书计页结果

    mutable std::atomic<BookReadError> error_{BookReadError::kNone}; // 打开及正文加载错误
    bool open_ = false;
    bool close_progress_queued_ = false; // UI 已提交退出快照，后台析构不再保存
    OpenProgressFn progress_fn_ = nullptr;
    void* progress_user_ = nullptr;
    int last_progress_pct_ = -1;
    BookInfo info_{};
    std::string display_title_;
    std::string display_author_;
    const lv_font_t* font_ = nullptr;
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

    std::unique_ptr<EpubDocument> epub_;
    std::unique_ptr<EbookDocument> ebook_;
    // .ebook 可见章 → 文件章下标（跳过空章）
    std::vector<int> ebook_spine_;
    bool page_images_mode_ = false;

    // TXT：源文件偏移索引 + 按需读盘
    bool txt_mode_ = false;
    bool txt_as_gb18030_ = false;
    bool txt_empty_ = false;
    uint32_t txt_work_id_ = 0; // 会话私有索引文件编号，旧会话回收不能删除新索引
    mutable FILE* txt_fp_ = nullptr;
    BookDiskArray<TxtLineRef> txt_lines_; // SD 行索引，仅缓存固定窗口
    size_t txt_lines_len_ = 0;
    size_t txt_lines_cap_ = 0;
    BookDiskArray<uint32_t> txt_page_first_line_; // SD 页索引，仅缓存固定窗口
    size_t txt_page_count_ = 0;
    size_t txt_page_starts_cap_ = 0;
    std::atomic<uint32_t> txt_paginate_gen_{0}; // AbortTxtPaginate 递增；StreamPaginateTxt 轮询
    enum class TxtScanState : uint8_t {
        kRunning, // 扫描可继续
        kPauseRequested, // 前台请求在安全点暂停
        kPaused, // 后台已让出，保持到前台完成
    };
    mutable std::atomic<TxtScanState> txt_scan_state_{TxtScanState::kRunning}; // 暂停确认与恢复使用同一状态，避免旧确认被复用
    mutable Page txt_view_page_;
    mutable int txt_view_index_ = -1;

    // 换参即时预览：TXT 后台建索引前用此页顶替 CurrentPageData；可流式邻页翻页
    bool live_preview_active_ = false;
    uint32_t live_preview_byte_off_ = 0;
    bool live_preview_at_para_start_ = true;
    uint32_t live_preview_next_off_ = 0; // 下一页字节锚点（页满未消费处）
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

    BookPageStore pending_pages_; // 后台构建完成后由 UI 发布
    BookPageStore pages_; // 分页正文落 SD，内存保留当前及相邻页
    int page_index_ = 0;
    int chapter_index_ = 0;
    int chapter_count_ = 1;
    // 各章页数（与 PageCount 同口径）；ready 后 BookPageCount 为真实全书页
    std::vector<uint32_t> chapter_page_counts_;
    std::vector<uint32_t> chapter_scan_counts_; // 计页任务独占的可恢复进度，完成后转交 UI
    bool chapter_scan_reset_ = true; // 由 layout_progress_mu_ 保护，下次扫描清空旧排版进度
    std::atomic<bool> chapter_pages_ready_{false};
    bool chapter_relayout_pending_ = false; // 当前章待现有排版任务重排
    std::atomic<uint32_t> chapter_pages_gen_{0}; // 中止或重排递增；后台写回时拒绝过期结果
    // 后台计页与翻页 LoadChapter 串行，避免 ebook/epub 文件句柄竞态
    mutable std::mutex chapter_io_mu_;

    // TXT 解析目录；EPUB/EBOOK 打开时填章标题（first_page 未用）
    std::vector<TocEntry> toc_;

    RasterImage cover_;
    bool cover_tried_ = false;

    struct CachedPageImage {
        std::string href; // 书内资源路径
        int max_w = 0, max_h = 0; // 解码目标框
        RasterImage image; // 保留原始 L8，不写入显示压暗结果
    };
    size_t PageImageCacheBudgetLocked() const;
    mutable std::mutex page_image_cache_mu_; // 仅保护图片缓存，不持锁读盘或解码
    mutable std::vector<CachedPageImage> cached_page_images_; // 从久未使用到最近使用排列

    // 终身累计阅读秒；仅 Close/Open 边界重置内存，Open 后从 .pos 恢复
    uint32_t reading_seconds_ = 0;
    // 单书当日秒 + 对应本地日戳（YYYYMMDD；0=未知/未对时）
    uint32_t daily_seconds_ = 0;
    uint32_t reading_day_id_ = 0;
    bool reading_clock_active_ = false;
    int64_t reading_clock_since_us_ = 0;  // esp_timer_get_time()，仅 active 时有效
};

}  // namespace reader
