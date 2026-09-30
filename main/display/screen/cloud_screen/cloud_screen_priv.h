#pragma once

#include <atomic>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <freertos/task.h>
#include <lvgl.h>

#include "cloud_screen/push/push_resources_library.h"
#include "reader/download_gate.h"
#include "reader/reader_types.h"
#include "screen_common.h"
#include "ui_scale.h"

constexpr const char* TAG = "CloudScreen";
constexpr const char* kScreenId = "cloud";

// 源值对齐 397 cloud_screen；经 UiSx/UiSy 同比例到 470
constexpr lv_coord_t kPad = UiSx(12);
constexpr lv_coord_t kFooterH = UiSy(36);
constexpr lv_coord_t kCloudRowH = UiSy(80);
constexpr lv_coord_t kCloudRowLineGap = UiSy(6);
constexpr lv_coord_t kRowGap = UiSy(8);
constexpr lv_coord_t kThumbW = UiSx(48);
constexpr lv_coord_t kThumbH = UiSy(64);
constexpr lv_coord_t kThumbGap = UiSx(10);
constexpr int kThumbDecodeW = UiSx(48);
constexpr int kThumbDecodeH = UiSy(64);
constexpr size_t kThumbMaxDownloadBytes = 512 * 1024;
constexpr lv_coord_t kRowPad = UiSx(10);
constexpr lv_coord_t kRowBorderW = UiSx(2);
constexpr lv_coord_t kRowRadius = UiSx(8);
constexpr lv_coord_t kActionH = UiSy(56);
constexpr int kPreviewMaxW = UiSx(240);
constexpr int kPreviewMaxH = UiSy(400);
constexpr size_t kPreviewMaxDownloadBytes = 2 * 1024 * 1024;
constexpr uint32_t kCloudFetchStack = 8 * 1024;
constexpr uint32_t kWallpaperPreviewStack = 10 * 1024;
constexpr uint32_t kCoverThumbStack = 8 * 1024;
constexpr uint32_t kCloudDownloadStack = 8 * 1024;  // 内 DRAM：写 SD 关 cache 时禁 PSRAM 栈
constexpr uint32_t kCloudDeleteStack = 8 * 1024;
constexpr int64_t kSyncThrottleUs = 3000000;  // 手动刷新最短间隔 3s
constexpr lv_coord_t kSyncBtnH = UiSy(44);
constexpr lv_coord_t kSyncDividerW = 2;    // 底栏顶部分割线
constexpr lv_coord_t kSyncUnderlineH = 2;  // 「刷新」/多选动作下划线
constexpr lv_coord_t kSyncUnderlinePadHor = UiSx(5); // 下划线两端各外延；共比字宽加长 10px
constexpr lv_coord_t kSyncUnderlinePadBottom = UiSy(2);
constexpr lv_coord_t kMultiDotSize = UiSx(6);        // 分隔圆点直径（约 · 的 2 倍）
constexpr lv_coord_t kMultiBarGap = UiSx(10);
constexpr lv_coord_t kMultiActionPadHor = UiSx(4);
constexpr lv_coord_t kFooterBlockH = kSyncBtnH;  // 框内刷新/多选底栏
constexpr lv_coord_t kListFrameBorderW = UiSx(2);  // 列表+刷新细外框（不含页码）
constexpr lv_coord_t kListFrameRadius = UiSx(10);
constexpr lv_coord_t kListFramePad = UiSx(10);
constexpr lv_coord_t kTabH = UiSy(48);
constexpr lv_coord_t kTabBorderW = UiSx(3);
constexpr lv_coord_t kTabInset = UiSx(3);
constexpr lv_coord_t kHeaderH = UiSy(36);
constexpr lv_coord_t kHeaderIcon = UiSx(32);
constexpr lv_coord_t kHeaderGap = UiSx(8);
constexpr lv_coord_t kStatusGap = UiSx(12);  // 标题与拉取提示间距（同每日清单）
constexpr int kTabCount = 4;
constexpr lv_coord_t kChipRadius = 999;  // 预览元信息胶囊
constexpr lv_coord_t kChipH = UiSy(36);        // 略高于字高，避免贴边
constexpr lv_coord_t kChipPadH = UiSx(16);     // 左右内边距，边框大于文字
constexpr lv_coord_t kPreviewGap = UiSy(12);
constexpr lv_coord_t kPreviewImgMaxH = UiSy(420);  // 预览图框上限，避免占满整屏
constexpr lv_coord_t kPreviewBodyPadTop = UiSy(8);
constexpr lv_coord_t kPreviewBodyPadBot = UiSy(12);
constexpr lv_coord_t kPreviewImgHostRadius = UiSx(16);
constexpr lv_coord_t kPreviewMetaH = UiSy(40);
constexpr lv_coord_t kPreviewMetaGap = UiSx(8);
constexpr lv_coord_t kPageLblMargin = UiSx(16);
constexpr lv_coord_t kPageLblBottom = UiSy(4);
constexpr lv_coord_t kFooterOutsideH = kFooterH;
constexpr uint32_t kStatusClearMs = 1200;
constexpr lv_coord_t kCheckSize = UiSx(28);  // 多选行尾勾选（同清单）
constexpr lv_coord_t kCheckGap = UiSx(10);
constexpr lv_coord_t kDialogCardMargin = UiSx(48);
constexpr lv_coord_t kDialogLabelMargin = UiSx(80);
constexpr lv_coord_t kDialogCardPad = UiSx(18);
constexpr lv_coord_t kDialogCardPadSmall = UiSx(14);
constexpr lv_coord_t kDialogRowGap = UiSy(14);
constexpr lv_coord_t kDialogRowGapSmall = UiSy(12);
constexpr lv_coord_t kDlBarW = UiSx(320);
constexpr lv_coord_t kDlBarH = UiSy(28);
constexpr lv_coord_t kDlBarRadius = UiSx(6);
constexpr lv_coord_t kDlBarIndRadius = UiSx(4);
constexpr lv_coord_t kCancelBtnW = UiSx(160);
constexpr lv_coord_t kCancelBtnH = UiSy(44);
constexpr lv_coord_t kActionDialogCardH = UiSy(220);
constexpr lv_coord_t kActionBtnRowH = UiSy(48);
constexpr lv_coord_t kActionBtnW = UiSx(120);
constexpr lv_coord_t kActionBtnH = UiSy(44);
constexpr lv_coord_t kActionDialogPadInset = UiSx(28); // pad_all 14×2
constexpr int64_t kSuppressRowClickUs = 400000;  // 只挡长按行松手假点击，不挡其它行连点
constexpr int64_t kDlProgUiIntervalUs = 3000 * 1000;  // 进度页约 3s 刷一次

struct UiState {
    struct Chrome {
        lv_obj_t* screen = nullptr;
        lv_obj_t* main_body = nullptr;  // 列表区；预览时隐藏（页码贴底，不在此列）
        lv_obj_t* list_body = nullptr;
        lv_obj_t* footer = nullptr;  // 框内刷新/多选底栏
        lv_obj_t* sync_btn = nullptr;
        lv_obj_t* sync_lbl = nullptr;
        lv_obj_t* multi_bar = nullptr;  // 取消 / 全选 / 删除 / 保存
        lv_obj_t* page_lbl = nullptr;  // 贴底，对齐壁纸页码
        lv_obj_t* tab_btns[kTabCount] = {};
        lv_obj_t* tab_lbls[kTabCount] = {};
        lv_obj_t* section_title = nullptr;
        lv_obj_t* status_lbl = nullptr;
    } chrome;

    struct Preview {
        lv_obj_t* body = nullptr;
        lv_obj_t* title = nullptr;  // 文件名，双行省略，在胶囊上方
        lv_obj_t* meta = nullptr;   // 类型/大小胶囊行
        lv_obj_t* status = nullptr;
        lv_obj_t* img = nullptr;
        lv_obj_t* img_host = nullptr;
        lv_obj_t* download_btn = nullptr;
        lv_obj_t* download_lbl = nullptr;
        uint32_t epoch = 0;
        int idx = -1;
        bool open = false;
        reader::RasterImage* raster = nullptr;
    } preview;

    struct Data {
        std::vector<reader::CloudPushResource> items;
        std::vector<int> filtered;  // items 下标，按 Tab 筛选
        std::vector<uint8_t> selected;  // 与 items 等长；1=多选选中
        std::vector<std::string> batch_save_ids;  // 批量保存队列（taskId）
        std::vector<std::unique_ptr<reader::RasterImage>> thumbs;
        std::vector<std::vector<uint8_t>> cover_bytes;  // 与 items 等长：coverImageUrl 原始字节
        uint32_t thumb_epoch = 0;
        int filter_tab = 0;  // 0全部 1壁纸 2书籍 3字体
        int page = 0;
        int page_size = 6;
        bool loading = false;
        bool waiting_net = false;
        bool fetch_done = false;
        bool covers_loading = false;  // 预览图全入 PSRAM 前不渲染列表行
        bool multi = false;
        bool batch_busy = false;
        bool delete_busy = false;  // 多选 DELETE push-resources 进行中
        int64_t suppress_row_click_until_us = 0;
        int suppress_row_click_idx = -1; // 与 until 配对：只吞该行松手 CLICKED
        std::string error;
        char status_text[80] = {};
    } data;

    struct Xfer {
        bool download_busy = false;
        bool dl_cancel_pending = false;  // 点行取消当前下载（断 HTTP）
        std::string dl_cancel_key;
        reader::PushResourceType download_type = reader::PushResourceType::kUnknown;
        std::string download_key;       // 当前下载 taskId
        uint32_t op_generation = 1;
        uint32_t download_gen = 0;
        int pending_index = -1;
        int action_index = -1;
        // 下载独占进度页（网点底）：单文件/批量共用
        lv_obj_t* dl_page = nullptr;
        lv_obj_t* dl_bar = nullptr;
        lv_obj_t* dl_pct_lbl = nullptr;
        lv_obj_t* dl_count_lbl = nullptr;
        lv_obj_t* dl_title_lbl = nullptr;
        bool dl_page_open = false;
        bool dl_user_abort = false;  // 进度页取消 / 返回：整单放弃，不再续下
        int dl_job_total = 0;
        int dl_job_done = 0;
        int dl_shown_percent = -1;
    } xfer;

    struct Dialogs {
        lv_obj_t* dialog_mask = nullptr;
        lv_obj_t* action_mask = nullptr;
    } dialogs;
};


struct CloudFetchDoneMsg {
    uint32_t op_gen = 0;
    bool ok = false;
    std::vector<reader::CloudPushResource> items;
    char error[64] = {};
};

struct CloudFetchWork {
    uint32_t op_gen = 0;
};

struct CloudDownloadWork {
    uint32_t op_gen = 0;
    reader::CloudPushResource item;
    std::vector<uint8_t> cover_bytes;  // 列表已缓存的 coverImageUrl，供旁路免再 HTTP
};

struct CloudDownloadDoneMsg {
    uint32_t op_gen = 0;
    reader::PushResourceType type = reader::PushResourceType::kUnknown;
    bool ok = false;
    bool acked = false;
    bool cancelled = false;
    char key[160] = {};  // taskId
    char error[64] = {};
};

struct CloudDownloadProgressMsg {
    int percent = 0;
};

struct CloudDeleteWork {
    uint32_t op_gen = 0;
    std::vector<reader::CloudPushResource> items;
};

struct CloudDeleteDoneMsg {
    uint32_t op_gen = 0;
    int ok_n = 0;
    int fail_n = 0;
    std::vector<std::string> ok_ids;
    char error[96] = {};
};

struct SyncStoppedMsg {
    uint32_t op_gen = 0;
};

struct NetPrepDoneMsg {
    bool ok = false;
    uint32_t op_gen = 0;
};

struct WallpaperPreviewWork {
    int index = -1;
    uint32_t epoch = 0;
    char local_path[256] = {};
    char download_url[768] = {};
    bool use_local = false;
    std::vector<uint8_t> cached_bytes;  // 非空则本地按详情尺寸解码，不再 HTTP
};

struct WallpaperPreviewResultMsg {
    int index = -1;
    uint32_t epoch = 0;
    bool ok = false;
    char err[80] = {};
    reader::RasterImage* img = nullptr;
};

struct CoverThumbWorkItem {
    int index = -1;
    char url[768] = {};
};

struct CoverThumbOneMsg {
    uint32_t epoch = 0;
    int index = -1;
    reader::RasterImage* img = nullptr;
    std::vector<uint8_t> bytes;  // 原始封面字节，与小图一并入库
    bool done = false;  // 本批最后一项（或失败收尾）后清 busy
};

struct CoverThumbFillWork {
    uint32_t epoch = 0;
    std::vector<CoverThumbWorkItem> items;
};

/** @brief 云传输 UI 单例状态 */
UiState& Cloud_State();

extern lv_timer_t* s_status_clear_timer;
extern DownloadGate s_dl_gate;
extern DownloadGate s_sync_gate;
extern int64_t s_last_user_sync_us;
extern std::atomic<bool> s_ui_net_held;
extern std::atomic<bool> s_thumb_busy;
extern std::atomic<bool> s_thumb_abort;
extern std::atomic<bool> s_dl_owns_http;
extern std::atomic<bool> s_cloud_fetch_alive;
extern TaskHandle_t s_net_prep_task;
extern std::atomic<uint32_t> s_net_prep_attach_gen;
extern TaskHandle_t s_thumb_task;
extern std::atomic<bool> s_wallpaper_preview_busy;
extern TaskHandle_t s_wallpaper_preview_task;
extern TaskHandle_t s_delete_task;
extern std::atomic<int64_t> s_dl_prog_last_ui_us;
extern TaskHandle_t s_dl_task;
extern QueueHandle_t s_dl_q;
extern ScreenPaintCoalesce s_cloud_paint;
extern ScreenPaintCoalesce s_cloud_check_paint;

/** @brief 拆掉云端保网预备 */
void DetachCloudNetPrep(const char* why);
/** @brief 在非 LVGL 路径中止下载门闩 */
void AbortCloudGatesOffLvgl();
/** @brief 作废当前云传输会话 */
void InvalidateSession();
/** @brief 停止传输并收起进度 UI */
void ApplyStopTransferUi();
/** @brief 请求停止传输 */
void RequestStopTransfer(const char* why);
/** @brief 云传输页盖板键短按 */
bool Cloud_OnVkKey(const char* key);
/** @brief 云传输页盖板键长按 */
bool Cloud_OnVkKeyLongPress(const char* key);
/** @brief 云传输页盖板键抬起 */
bool Cloud_OnVkKeyPressUp(const char* key);
/** @brief 云传输页删除事件 */
void Cloud_OnScreenDeleted(lv_event_t* e);
