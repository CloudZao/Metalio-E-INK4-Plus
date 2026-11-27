#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <memory>
#include <new>
#include <optional>
#include <esp_log.h>
#include "ui/common/runtime/page_lifecycle.h"
#include "ui/common/widgets/fixed_footer.h"
#include "ui/apps/app_store/app_store_screen.h"
#include "assets/lang_app_store.h"
#include "assets/lang_config.h"
#include "infra/storage/sd_paths.h"
#include "business/app_store/app_store_icons.h"
#include "infra/network/endpoints/api_endpoints.h"
#include "infra/storage/settings.h"
#include "sysmgmt/power/policy/power_policy.h"
#include "ui/common/drawing/adapter/epd_refresh.h"
#include "ui/common/drawing/font/ui_fonts.h"
#include "ui/common/widgets/screen_common.h"
#include "ui/common/runtime/modal.h"
#include "ui/common/widgets/screen_options.h"
#include "ui/common/widgets/screen_text.h"
#include "ui/common/runtime/input/vk_key_handler.h"
#include "ui/common/drawing/ui_theme.h"
#include "ui/common/runtime/screen_common_nav.h"

enum class AppStoreScreenFilter { All, Uninstalled, Installed, Downloading };

struct AppStoreScreenPage {
    ScreenFixedFooter detail_frame;  // 详情滚动内容与固定操作区

    std::optional<PowerNeedHold> network_hold;                // 前台页面软保网，离页或待机时释放
    std::optional<PowerNeedHold> install_hold;                // 安装队列存续期间禁止待机
    std::vector<AppStoreItem> queue;                          // 按用户选择顺序串行安装，最多为目录应用数
    AppStoreItem installing;                                  // 当前操作目标，与详情选择独立
    uint32_t progress_tick = 0;                               // 墨水屏进度刷新限频
    unsigned progress_percent = 101;                          // 最近显示的百分比
    bool progress_verifying = false;                          // 最近显示的校验阶段
    bool active = false;                                      // 当前页面已加载
    bool standby = false;                                     // 待机暂停轮询和网络请求
    bool reload_on_wake = false;                              // 有在途操作时，唤醒后重新读取实际安装状态
    lv_obj_t* root = nullptr;                                 // 页面根对象
    lv_obj_t* list_frame = nullptr;                           // 列表外框
    lv_obj_t* count = nullptr;                                // 当前筛选及应用数量
    lv_obj_t* list = nullptr;                                 // 横向应用行列表
    lv_obj_t* title = nullptr;                                // 商店标题
    lv_obj_t* detail_title = nullptr;                         // 详情标题
    lv_obj_t* refresh_button = nullptr;                       // 刷新入口
    lv_obj_t* notice = nullptr;                               // 仅覆盖标题区域的通知
    lv_obj_t* filter_button = nullptr;                        // 工具栏左侧筛选入口
    lv_obj_t* filter_overlay = nullptr;                       // 点击外部关闭的悬浮筛选层
    lv_obj_t* filters = nullptr;                              // 带边框的筛选面板
    lv_obj_t* choices[4] = {};                                // 筛选选项按钮
    lv_obj_t* status = nullptr;                               // 仅显示空状态
    lv_obj_t* detail = nullptr;                               // 应用详情页面层
    bool detail_expanded[2] = {true, true};                   // 应用描述与更新说明分别保存展开状态
    lv_obj_t* detail_buttons[3] = {};                         // 详情底部操作按钮
    lv_obj_t* confirmation = nullptr;                         // 删除确认框
    AppStoreScreenFilter filter = AppStoreScreenFilter::All;  // 当前生效条件
    AppStoreOperation pending_operation = AppStoreOperation::Scan;  // 当前工作类型
    bool list_dirty = false;  // 下载筛选成员变化后延后到轮询重建，保留事件中的控件
    bool expanded = false;    // 筛选是否展开
    bool empty = true;        // 当前筛选没有应用
    uint32_t retry_tick = 0;  // 跨页面请求收尾后的有限频率重试
    bool loading = false;     // 本地扫描与远端获取组成的一轮刷新
    AppStoreOperation next_load = AppStoreOperation::Scan;  // 等待启动的加载阶段
    bool catalog_failed = false;                                        // 最近一次目录请求失败
    bool catalog_stale = false;                                         // 队列处理完后刷新不可用版本所在的目录
    bool notice_sticky = false;                                         // 进度通知保留到阶段结束
    uint32_t notice_tick = 0;                                           // 结果通知开始时间
    AppStoreItem update_candidate;                                      // 已检查并等待用户确认的版本快照
    AppStoreItem selected;                                              // 详情或操作目标快照
    std::string server;                                                 // 已配置的商店 API 基址
    std::string scan_error;                                             // 本轮本地扫描错误，保留到加载结果提示
    AppStoreCatalogPage pagination;                                     // 当前在线页，列表仅保留单页
    size_t local_page = 1;                                              // 本地筛选页
    std::vector<AppStoreItem> catalog;                                  // UI 线程持有的服务器应用
    std::vector<AppStoreItem> installed;                                // UI 线程持有的已安装应用
    std::shared_ptr<AppStoreResult> pending;                      // 在途操作
};

struct AppStoreScreenRow {
    AppStoreScreenPage* page;    // 所属页面，仅在 UI 线程访问
    AppStoreItem item;           // 行内应用快照
    bool installed;              // 操作按钮是否打开应用
    lv_obj_t* action = nullptr;  // 安装、打开或取消按钮
};

static UiPageLifecycle s_page_lifecycle;

static constexpr lv_obj_flag_t kAppStoreClosingLayer = LV_OBJ_FLAG_USER_3;  // 根容器持有的待清理页面层

static AppStoreScreenPage* s_current = nullptr;
static AppStoreItem s_return_item;
static AppStoreScreenFilter s_return_filter = AppStoreScreenFilter::All;

static const AppStoreItem* Find(const std::vector<AppStoreItem>& items, const std::string& id) {
    for (const auto& item : items) {
        if (item.app_id == id) {
            return &item;
        }
    }
    return nullptr;
}

static UiModal s_window_modal;

static void Fixed(lv_obj_t* object) {
    lv_obj_remove_flag(object, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_color(object, UiColorBackground(), 0);
    lv_obj_set_style_text_color(object, UiColorForeground(), 0);
    lv_obj_set_style_border_color(object, UiColorForeground(), 0);
    lv_obj_set_style_shadow_width(object, 0, 0);
}

static lv_obj_t* Container(lv_obj_t* parent) {
    auto* object = lv_obj_create(parent);
    Fixed(object);
    lv_obj_set_style_border_width(object, 0, 0);
    lv_obj_set_style_pad_all(object, 0, 0);
    lv_obj_set_style_radius(object, 0, 0);
    lv_obj_remove_flag(object, LV_OBJ_FLAG_CLICKABLE);
    return object;
}

static void Frame(lv_obj_t* object, int border, int offset) {
    lv_obj_set_style_border_width(object, border, 0);
    lv_obj_set_style_border_post(object, true, 0);
    lv_obj_set_style_radius(object, 12, 0);
    lv_obj_set_style_shadow_color(object, UiColorForeground(), 0);
    lv_obj_set_style_shadow_opa(object, LV_OPA_COVER, 0);
    lv_obj_set_style_shadow_width(object, 1, 0);
    lv_obj_set_style_shadow_offset_x(object, offset, 0);
    lv_obj_set_style_shadow_offset_y(object, offset, 0);
}

static void ScrollStyle(lv_obj_t* object) {
    lv_obj_set_style_pad_right(object, 24, 0);
    lv_obj_set_style_width(object, 4, LV_PART_SCROLLBAR);
    lv_obj_set_style_pad_right(object, 0, LV_PART_SCROLLBAR);
    lv_obj_set_style_bg_color(object, UiColorForeground(), LV_PART_SCROLLBAR);
    lv_obj_set_style_bg_opa(object, LV_OPA_COVER, LV_PART_SCROLLBAR);
    lv_obj_set_style_border_width(object, 0, LV_PART_SCROLLBAR);
}

static lv_obj_t* Label(lv_obj_t* parent, const char* text, int width, UiFontRole role = UiFontRole::Body) {
    auto* label = lv_label_create(parent);
    lv_obj_set_width(label, width);
    lv_label_set_long_mode(label, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_font(label, UiFont(role), 0);
    lv_label_set_text(label, text);
    return label;
}

static lv_obj_t* Title(lv_obj_t* parent, const char* text, int top) {
    auto* title = Label(parent, text, LV_HOR_RES - 48, UiFontRole::Title);
    const auto* bold = UiFont(UiFontRole::TitleEmphasis);
    lv_obj_set_style_text_font(title, bold ? bold : UiFont(UiFontRole::Title), 0);
    lv_obj_set_style_text_align(title, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_pos(title, 24, top + 24);
    return title;
}

static lv_obj_t* Button(lv_obj_t* parent, const char* text, int width, int height, lv_event_cb_t callback,
                        void* context, bool primary = false) {
    auto* button = lv_button_create(parent);
    lv_obj_remove_style_all(button);
    lv_obj_set_style_bg_opa(button, LV_OPA_COVER, 0);
    lv_obj_set_style_opa(button, LV_OPA_50, LV_STATE_DISABLED);
    Fixed(button);
    lv_obj_set_size(button, width, height);
    lv_obj_set_style_border_width(button, 2, 0);
    lv_obj_set_style_radius(button, 10, 0);
    lv_obj_set_style_pad_all(button, 6, 0);
    lv_obj_remove_flag(button, LV_OBJ_FLAG_EVENT_BUBBLE);
    if (primary) {
        lv_obj_set_style_bg_color(button, UiColorForeground(), 0);
        lv_obj_set_style_text_color(button, UiColorBackground(), 0);
    }
    auto* label = Label(button, text, width - 16, UiFontRole::Body);
    lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_CENTER, 0);
    ScreenLayoutTextButton(button, label, height);
    lv_obj_add_event_cb(button, callback, LV_EVENT_CLICKED, context);
    return button;
}

static void Icon(lv_obj_t* parent, const AppStoreItem& item, int edge) {
    auto* icon = lv_image_create(parent);
    AppStoreIconsSetSource(icon, AppStoreIconsSource(item), edge);
    lv_obj_set_size(icon, edge, edge);
    lv_image_set_inner_align(icon, LV_IMAGE_ALIGN_CONTAIN);
    lv_image_set_antialias(icon, false);
    lv_obj_remove_flag(icon, LV_OBJ_FLAG_CLICKABLE);
}

static void Render(AppStoreScreenPage* page, bool reset_scroll = false);
static void Details(AppStoreScreenPage* page);

static bool CanOpen(const AppStoreScreenPage* page) {
    return !page->pending || page->pending_operation == AppStoreOperation::Scan ||
           page->pending_operation == AppStoreOperation::Refresh;
}

static bool Working(const AppStoreScreenPage* page) {
    return page->loading || page->pending || !page->queue.empty();
}

static bool CurrentInstall(const AppStoreScreenPage* page, const std::string& id) {
    return page->pending &&
           (page->pending_operation == AppStoreOperation::Install ||
            page->pending_operation == AppStoreOperation::Update) &&
           page->installing.app_id == id;
}

static bool HasDownloads(const AppStoreScreenPage* page) {
    return !page->queue.empty() || CurrentInstall(page, page->installing.app_id);
}

static bool Queued(const AppStoreScreenPage* page, const std::string& id) {
    return Find(page->queue, id) || CurrentInstall(page, id);
}

static std::string InstallStatus(const AppStoreScreenPage* page, const std::string& id) {
    if (!CurrentInstall(page, id)) {
        return Find(page->queue, id) ? LangString_STORE_QUEUED : "";
    }
    if (page->pending->cancelled.load()) {
        return LangString_STORE_CANCELLING;
    }
    const auto& progress = page->pending->progress;
    const size_t total = progress.total.load(), bytes = progress.bytes.load();
    if (!total) {
        return LangString_STORE_INSTALLING;
    }
    if (progress.verifying.load()) {
        return LangString_STORE_VERIFYING;
    }
    char text[8];
    std::snprintf(text, sizeof(text), "%u%%", unsigned(bytes * 100 / total));
    return text;
}

static void ActionText(lv_obj_t* button, const AppStoreScreenPage* page, const std::string& id, bool installed) {
    const bool queued = Queued(page, id);
    auto* label = lv_obj_get_child(button, 0);
    const auto status = InstallStatus(page, id);
    lv_label_set_text(label, queued ? status.c_str() : installed ? LangString_STORE_RUN : LangString_STORE_INSTALL);
    lv_obj_set_style_text_font(label, UiFont(queued ? UiFontRole::Caption : UiFontRole::Title), 0);
}

static bool CanInstall(const AppStoreScreenPage* page) {
    return !page->standby && !page->loading &&
           (!page->pending || page->pending_operation == AppStoreOperation::Install ||
            page->pending_operation == AppStoreOperation::Update);
}

static void PumpQueue(AppStoreScreenPage* page);
static void QueueInstall(AppStoreScreenPage* page, const AppStoreItem& item);

static void CancelInstall(AppStoreScreenPage* page, const std::string& id) {
    if (CurrentInstall(page, id)) {
        page->pending->cancelled.store(true);
        page->progress_tick = 0;
        page->notice_sticky = false;
        lv_obj_add_flag(page->notice, LV_OBJ_FLAG_HIDDEN);
    } else {
        if (page->filter == AppStoreScreenFilter::Downloading) {
            page->list_dirty = true;
        }
        page->queue.erase(std::remove_if(page->queue.begin(), page->queue.end(),
                                         [&](const AppStoreItem& item) { return item.app_id == id; }),
                          page->queue.end());
        if (!Working(page)) {
            page->install_hold.reset();
        }
    }
}

static void Layout(AppStoreScreenPage* page) {
    const int top = ScreenStatusBarHeight() + 168;
    lv_obj_set_pos(page->list_frame, 20, top);
    lv_obj_set_size(page->list_frame, LV_HOR_RES - 40, LV_VER_RES - top - 16);
    lv_obj_move_foreground(page->status);
    if (page->expanded) {
        lv_obj_remove_flag(page->filter_overlay, LV_OBJ_FLAG_HIDDEN);
        lv_obj_move_foreground(page->filter_overlay);
    } else {
        lv_obj_add_flag(page->filter_overlay, LV_OBJ_FLAG_HIDDEN);
    }
    const bool no_installed =
        page->filter == AppStoreScreenFilter::Installed ||
        (page->filter == AppStoreScreenFilter::All && page->catalog_failed && page->catalog.empty());
    lv_label_set_text(page->status, no_installed                                ? LangString_STORE_NO_INSTALLED
                                    : page->filter == AppStoreScreenFilter::All ? LangString_STORE_EMPTY
                                                                                : LangString_STORE_NO_MATCH);
    lv_obj_set_style_text_align(page->status, LV_TEXT_ALIGN_CENTER, 0);
    if (page->detail || !page->empty || Working(page)) {
        lv_obj_add_flag(page->status, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_remove_flag(page->status, LV_OBJ_FLAG_HIDDEN);
        lv_obj_align(page->status, LV_ALIGN_CENTER, 0, top / 2);
    }
    const bool notice_visible = !lv_obj_has_flag(page->notice, LV_OBJ_FLAG_HIDDEN);
    for (auto* title : {page->title, page->detail_title}) {
        if (!title) {
            continue;
        }
        if (notice_visible) {
            lv_obj_add_flag(title, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_remove_flag(title, LV_OBJ_FLAG_HIDDEN);
        }
    }
    if (notice_visible) {
        lv_obj_move_foreground(page->notice);
    }
}

static void Notice(AppStoreScreenPage* page, const char* text, bool sticky = false) {
    page->notice_sticky = sticky;
    page->notice_tick = lv_tick_get();
    lv_label_set_text(page->notice, text);
    lv_obj_align(page->notice, LV_ALIGN_TOP_MID, 0, ScreenStatusBarHeight() + 8);
    lv_obj_remove_flag(page->notice, LV_OBJ_FLAG_HIDDEN);
    Layout(page);
}

static void SetBlocked(lv_obj_t* button, bool blocked) {
    if (blocked) {
        lv_obj_add_state(button, LV_STATE_DISABLED);
    } else {
        lv_obj_remove_state(button, LV_STATE_DISABLED);
    }
}

static void Feedback(AppStoreScreenPage* page) {
    const char* labels[] = {LangString_STORE_ALL_APPS, LangString_STORE_UNINSTALLED, LangString_STORE_INSTALLED,
                            LangString_STORE_DOWNLOADING};
    unsigned counts[4] = {};
    auto count_item = [&](const AppStoreItem& item) {
        const bool installed = Find(page->installed, item.app_id) != nullptr;
        ++counts[0];
        ++counts[installed ? 2 : 1];
        if (Queued(page, item.app_id)) {
            ++counts[3];
        }
    };
    for (const auto& item : page->catalog) {
        count_item(item);
    }
    for (const auto& item : page->installed) {
        if (!Find(page->catalog, item.app_id)) {
            count_item(item);
        }
    }
    if (page->filter == AppStoreScreenFilter::All && !page->catalog.empty()) {
        lv_label_set_text_fmt(page->count, "%s %zu", labels[0], page->pagination.total);
    } else if (page->filter == AppStoreScreenFilter::Uninstalled) {
        lv_label_set_text(page->count, LangString_STORE_PAGE_RESULTS);
    } else {
        lv_label_set_text_fmt(page->count, "%s %u", labels[static_cast<int>(page->filter)],
                              counts[static_cast<int>(page->filter)]);
    }
    for (int i = 0; i < 4; ++i) {
        lv_label_set_text_fmt(lv_obj_get_child(page->choices[i], 0), "%s %u", labels[i], counts[i]);
    }
    Layout(page);
    SetBlocked(page->refresh_button, Working(page));
    for (unsigned i = 0; i < lv_obj_get_child_count(page->list); ++i) {
        auto* object = lv_obj_get_child(page->list, i);
        auto* row = static_cast<AppStoreScreenRow*>(lv_obj_get_user_data(object));
        if (row) {
            auto* action = row->action;
            const bool queued = Queued(page, row->item.app_id);
            ActionText(action, page, row->item.app_id, row->installed);
            SetBlocked(action, queued ? (CurrentInstall(page, row->item.app_id) && page->pending->cancelled.load())
                               : row->installed ? !CanOpen(page)
                                                : !CanInstall(page));
        }
    }
    for (int i = 0; i < 3; ++i) {
        auto* button = page->detail_buttons[i];
        if (button) {
            if (i == 1) {
                SetBlocked(button, Working(page) && !HasDownloads(page));
                continue;
            }
            const bool installed = Find(page->installed, page->selected.app_id);
            const bool queued = Queued(page, page->selected.app_id);
            if (i == 0) {
                ActionText(button, page, page->selected.app_id, installed);
            }
            SetBlocked(button,
                       i == 0
                           ? (queued ? (CurrentInstall(page, page->selected.app_id) && page->pending->cancelled.load())
                              : installed ? !CanOpen(page)
                                          : !CanInstall(page))
                           : Working(page));
        }
    }
}

static bool Start(AppStoreScreenPage* page, AppStoreOperation operation, const AppStoreItem& item = {},
                  size_t number = 1) {
    if (page->pending || page->standby) {
        return false;
    }
    if (operation != AppStoreOperation::Scan && operation != AppStoreOperation::Remove &&
        !AppStoreNormalizeServer(page->server)) {
        ESP_LOGW("AppStore", "operation=start stage=server_config result=invalid");
        Notice(page, LangString_STORE_SERVER_ERROR);
        return false;
    }
    if (AppStoreBusy()) {
        Notice(page, LangString_STORE_BUSY);
        return false;
    }
    auto pagination = page->pagination;
    pagination.number = number;
    if (operation == AppStoreOperation::Refresh) {
        pagination = AppStoreCatalogPage{};
    }
    auto result = AppStoreSubmit(operation, page->server, item, std::move(pagination));
    if (!result) {
        Notice(page, LangString_STORE_MEMORY_ERROR);
        return false;
    }
    page->installing = item;
    page->progress_percent = 101;
    page->progress_verifying = false;
    page->progress_tick = 0;
    if (page->filter == AppStoreScreenFilter::Downloading) {
        page->list_dirty = true;
    }
    page->pending = result;
    page->pending_operation = operation;
    const char* progress =
        operation == AppStoreOperation::Scan     ? LangString_STORE_READING_INSTALLED
        : operation == AppStoreOperation::Detail ? LangString_STORE_FETCHING_DETAIL
        : (operation == AppStoreOperation::Refresh || operation == AppStoreOperation::PageFetch)
            ? LangString_STORE_FETCHING
        : operation == AppStoreOperation::CheckUpdate ? LangString_STORE_CHECKING
        : operation == AppStoreOperation::Remove      ? LangString_STORE_REMOVING
                                                            : LangString_STORE_INSTALLING;
    // 事件回调内保留被点击按钮，只更新通知与可用状态。
    if (operation == AppStoreOperation::Install || operation == AppStoreOperation::Update) {
        page->notice_sticky = false;
        lv_obj_add_flag(page->notice, LV_OBJ_FLAG_HIDDEN);
    } else {
        Notice(page, progress, true);
    }
    Feedback(page);
    return true;
}

static void PumpQueue(AppStoreScreenPage* page) {
    if (page->pending || page->standby || page->queue.empty() || AppStoreBusy()) {
        return;
    }
    const AppStoreItem item = page->queue.front();
    page->queue.erase(page->queue.begin());
    if (!Start(page, AppStoreOperation::Install, item)) {
        page->queue.clear();
        page->install_hold.reset();
    }
    Feedback(page);
}

static void QueueInstall(AppStoreScreenPage* page, const AppStoreItem& item) {
    if (Queued(page, item.app_id)) {
        CancelInstall(page, item.app_id);
        Feedback(page);
        return;
    }
    if (!CanInstall(page) || page->queue.size() >= 32) {
        return;
    }
    page->queue.push_back(item);
    if (page->filter == AppStoreScreenFilter::Downloading) {
        page->list_dirty = true;
    }
    if (!page->install_hold) {
        page->install_hold.emplace(PowerNeed::OtaDownload);
    }
    PumpQueue(page);
    Feedback(page);
}

static void TryLoad(AppStoreScreenPage* page) {
    if (!page->loading || page->pending || AppStoreBusy()) {
        return;
    }
    page->retry_tick = lv_tick_get();
    if (!Start(page, page->next_load)) {
        if (page->next_load == AppStoreOperation::Refresh) {
            page->catalog_failed = true;
        }
        page->loading = false;
        Feedback(page);
    }
}

static void BeginRefresh(AppStoreScreenPage* page) {
    if (Working(page) || page->standby) {
        return;
    }
    page->loading = true;
    page->next_load = AppStoreOperation::Scan;
    page->scan_error.clear();
    page->expanded = false;
    Notice(page, LangString_STORE_READING_INSTALLED, true);
    Feedback(page);
    TryLoad(page);
}

static void Refresh(lv_event_t* event) {
    BeginRefresh(static_cast<AppStoreScreenPage*>(lv_event_get_user_data(event)));
}

static void DeferLayerDeletion(lv_obj_t* layer) {
    if (layer != nullptr) {
        // 事件中的控件保留到下次页面轮询；根容器继续持有它，不依赖异步队列接纳。
        lv_obj_add_flag(layer, static_cast<lv_obj_flag_t>(LV_OBJ_FLAG_HIDDEN | kAppStoreClosingLayer));
    }
}

static void DeleteClosedLayers(AppStoreScreenPage* page) {
    uint32_t index = 0;
    while (index < lv_obj_get_child_count(page->root)) {
        auto* layer = lv_obj_get_child(page->root, index);
        if (lv_obj_has_flag(layer, kAppStoreClosingLayer)) {
            lv_obj_delete(layer);
        } else {
            ++index;
        }
    }
}

static void CloseConfirmation(AppStoreScreenPage* page) {
    DeferLayerDeletion(page->confirmation);
    page->confirmation = nullptr;
    page->update_candidate = {};
}

static void CloseDetails(AppStoreScreenPage* page) {
    CloseConfirmation(page);
    if (page->detail) {
        EpdRequestScreenChange();
        DeferLayerDeletion(page->detail);
        lv_obj_invalidate(page->root);
    }
    page->detail = nullptr;
    page->detail_title = nullptr;
    std::fill(std::begin(page->detail_buttons), std::end(page->detail_buttons), nullptr);
    Layout(page);
}

static void SetFilter(AppStoreScreenPage* page, AppStoreScreenFilter filter);

static lv_obj_t* ResumeStore() {
    auto* root = AppStoreScreen::Create();
    SetFilter(s_current, s_return_filter);

    if (!s_return_item.app_id.empty()) {
        s_current->selected = s_return_item;
        s_current->installed.push_back(s_return_item);

        Details(s_current);
    }
    Layout(s_current);
    return root;
}

static void Launch(AppStoreScreenPage* page, const AppStoreItem& item) {
    s_return_filter = page->filter;
    s_return_item = page->detail ? item : AppStoreItem{};
    AppStoreScreen::Launch(item);
}

static void Run(lv_event_t* event) {
    auto* page = static_cast<AppStoreScreenPage*>(lv_event_get_user_data(event));
    if (Queued(page, page->selected.app_id)) {
        CancelInstall(page, page->selected.app_id);
        Feedback(page);
    } else if (CanOpen(page)) {
        Launch(page, page->selected);
    }
}

static void Install(lv_event_t* event) {
    auto* page = static_cast<AppStoreScreenPage*>(lv_event_get_user_data(event));
    if (const AppStoreItem* item = Find(page->catalog, page->selected.app_id)) {
        QueueInstall(page, *item);
    }
}

static void Update(lv_event_t* event) {
    auto* page = static_cast<AppStoreScreenPage*>(lv_event_get_user_data(event));
    if (HasDownloads(page)) {
        Notice(page, LangString_STORE_DOWNLOAD_BUSY);
        return;
    }
    Start(page, AppStoreOperation::CheckUpdate, page->selected);
}

static void ConfirmRemove(lv_event_t* event) {
    auto* page = static_cast<AppStoreScreenPage*>(lv_event_get_user_data(event));
    CloseConfirmation(page);
    Start(page, AppStoreOperation::Remove, page->selected);
}

static void CancelRemove(lv_event_t* event) {
    CloseConfirmation(static_cast<AppStoreScreenPage*>(lv_event_get_user_data(event)));
}

static void Confirmation(AppStoreScreenPage* page, const char* text, const char* confirm, lv_event_cb_t callback) {
    if (!page->detail || page->confirmation) {
        return;
    }
    auto* overlay = Container(page->root);
    lv_obj_set_size(overlay, LV_PCT(100), LV_PCT(100));
    lv_obj_set_style_bg_opa(overlay, LV_OPA_TRANSP, 0);
    lv_obj_add_flag(overlay, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(overlay, CancelRemove, LV_EVENT_CLICKED, page);
    page->confirmation = overlay;
    s_window_modal.Bind(overlay, [](void* context) { CloseConfirmation(static_cast<AppStoreScreenPage*>(context)); }, page);
    auto* dialog = Container(overlay);
    lv_obj_set_size(dialog, LV_HOR_RES - 80, LV_SIZE_CONTENT);
    ScreenStyleDialogCard(dialog, 22);
    lv_obj_add_flag(dialog, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(dialog, LV_OBJ_FLAG_EVENT_BUBBLE);
    lv_obj_set_flex_flow(dialog, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(dialog, 22, 0);
    auto* message = Label(dialog, text, LV_PCT(100), UiFontRole::Body);
    lv_obj_set_style_text_align(message, LV_TEXT_ALIGN_CENTER, 0);
    auto* row = Container(dialog);
    lv_obj_set_size(row, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_column(row, 16, 0);
    lv_obj_update_layout(dialog);
    const int width = (lv_obj_get_content_width(row) - 16) / 2;
    Button(row, LangString_STORE_CANCEL, width, 66, CancelRemove, page);
    Button(row, confirm, width, 66, callback, page, true);
    lv_obj_center(dialog);
}

static void ConfirmUpdate(lv_event_t* event) {
    auto* page = static_cast<AppStoreScreenPage*>(lv_event_get_user_data(event));
    const AppStoreItem candidate = page->update_candidate;
    CloseConfirmation(page);
    if (candidate.app_id.empty() || !CanInstall(page)) {
        return;
    }
    page->install_hold.emplace(PowerNeed::OtaDownload);
    if (!Start(page, AppStoreOperation::Update, candidate)) {
        page->install_hold.reset();
    }
}

static void UpdateAvailable(AppStoreScreenPage* page, const AppStoreItem& candidate) {
    if (!page->detail || page->selected.app_id != candidate.app_id) {
        return;
    }
    page->update_candidate = candidate;
    char text[192];
    std::snprintf(text, sizeof(text), LangString_STORE_UPDATE_CONFIRM_FMT, candidate.version.c_str());
    Confirmation(page, text, LangString_STORE_CONFIRM, ConfirmUpdate);
}

static void Remove(lv_event_t* event) {
    auto* page = static_cast<AppStoreScreenPage*>(lv_event_get_user_data(event));
    if (Working(page)) {
        return;
    }
    Confirmation(page, LangString_STORE_REMOVE_CONFIRM, LangString_STORE_REMOVE, ConfirmRemove);
}

static std::string SizeText(size_t bytes) {
    char text[32];
    std::snprintf(text, sizeof(text), bytes >= 1048576 ? "%.1f MiB" : "%.1f KiB",
                  bytes / (bytes >= 1048576 ? 1048576.0 : 1024.0));
    return text;
}

static std::string LanguageText(const std::string& codes) {
    if (codes.empty()) {
        return LangString_STORE_NOT_PROVIDED;
    }
    std::string result;
    for (size_t begin = 0; begin < codes.size();) {
        const auto end = codes.find(',', begin);
        const auto code = codes.substr(begin, end == std::string::npos ? end : end - begin);
        if (!result.empty()) {
            result += ", ";
        }
        result += code == "zh-CN"   ? LangString_STORE_LANGUAGE_ZH
                  : code == "en-US" ? LangString_STORE_LANGUAGE_EN
                                    : code.c_str();
        if (end == std::string::npos) {
            break;
        }
        begin = end + 1;
    }
    return result;
}

static std::string UploadTimeText(const std::string& value) {
    if (value.empty() || value == "0001-01-01T00:00:00Z") {
        return LangString_STORE_NOT_PROVIDED;
    }
    return value.substr(0, 10);
}

static void SectionArrow(lv_obj_t* arrow, bool expanded) {
    static const lv_point_precise_t down[] = {{0, 0}, {8, 8}, {16, 0}};
    static const lv_point_precise_t right[] = {{0, 0}, {8, 8}, {0, 16}};
    lv_line_set_points(arrow, expanded ? down : right, 3);
}

static void ToggleDescription(lv_event_t* event) {
    auto* heading = static_cast<lv_obj_t*>(lv_event_get_current_target(event));
    auto* expanded = static_cast<bool*>(lv_obj_get_user_data(heading));
    auto* body = static_cast<lv_obj_t*>(lv_event_get_user_data(event));
    *expanded = !*expanded;
    if (*expanded) {
        lv_obj_remove_flag(body, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(body, LV_OBJ_FLAG_HIDDEN);
    }
    SectionArrow(lv_obj_get_child(heading, -1), *expanded);
}

static void Description(AppStoreScreenPage* page, lv_obj_t* parent, const char* title, const std::string& value,
                        unsigned index) {
    auto* section = Container(parent);
    lv_obj_set_size(section, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(section, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(section, 12, 0);
    lv_obj_set_style_pad_bottom(section, 14, 0);
    lv_obj_set_style_border_width(section, index == 0 ? 1 : 0, 0);
    lv_obj_set_style_border_color(section, UiColorForeground(), 0);
    lv_obj_set_style_border_side(section, LV_BORDER_SIDE_BOTTOM, 0);
    auto* heading = Container(section);
    lv_obj_set_size(heading, LV_PCT(100), 64);
    lv_obj_set_style_pad_ver(heading, 14, 0);
    lv_obj_add_flag(heading, LV_OBJ_FLAG_CLICKABLE);
    auto* label = Label(heading, title, LV_PCT(85), UiFontRole::Large);
    lv_obj_align(label, LV_ALIGN_LEFT_MID, 0, 0);
    auto* arrow = lv_line_create(heading);
    SectionArrow(arrow, page->detail_expanded[index]);
    lv_obj_set_style_line_color(arrow, UiColorForeground(), 0);
    lv_obj_set_style_line_width(arrow, 2, 0);
    lv_obj_remove_flag(arrow, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_align(arrow, LV_ALIGN_RIGHT_MID, -4, 0);
    auto* body =
        Label(section, value.empty() ? LangString_STORE_NOT_PROVIDED : value.c_str(), LV_PCT(100), UiFontRole::Body);
    if (!page->detail_expanded[index]) {
        lv_obj_add_flag(body, LV_OBJ_FLAG_HIDDEN);
    }
    lv_obj_set_user_data(heading, &page->detail_expanded[index]);
    lv_obj_add_event_cb(heading, ToggleDescription, LV_EVENT_CLICKED, body);
}

static void Metadata(lv_obj_t* parent, const char* title, const std::string& value) {
    auto* row = Container(parent);
    lv_obj_set_size(row, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_style_min_height(row, 64, 0);
    lv_obj_set_style_pad_ver(row, 14, 0);
    lv_obj_set_style_border_width(row, 1, 0);
    lv_obj_set_style_border_side(row, LV_BORDER_SIDE_BOTTOM, 0);
    lv_obj_set_style_border_color(row, UiColorForeground(), 0);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    auto* label = Label(row, title, LV_PCT(42), UiFontRole::Caption);
    lv_obj_set_flex_grow(label, 1);
    auto* data = Label(row, value.c_str(), LV_PCT(54), UiFontRole::Body);
    lv_obj_set_style_text_align(data, LV_TEXT_ALIGN_RIGHT, 0);
}

static void Details(AppStoreScreenPage* page) {
    if (page->confirmation) {
        return;
    }
    const bool entering = page->detail == nullptr;
    if (page->detail) {
        lv_obj_delete(page->detail);
    }
    std::fill(std::begin(page->detail_buttons), std::end(page->detail_buttons), nullptr);
    page->detail = Container(page->root);
    lv_obj_set_pos(page->detail, 0, ScreenStatusBarHeight());
    lv_obj_set_size(page->detail, LV_HOR_RES, LV_VER_RES - ScreenStatusBarHeight());
    lv_obj_add_flag(page->detail, LV_OBJ_FLAG_CLICKABLE);
    page->detail_title = Title(page->detail, LangString_STORE_DETAILS, 0);
    const bool installed = Find(page->installed, page->selected.app_id) != nullptr;
    const int actions_height = installed ? 200 : 112;
    const int shell_height = LV_VER_RES - ScreenStatusBarHeight() - 100;
    page->detail_frame.Create(page->detail, LV_HOR_RES - 40, shell_height, actions_height, UiColorForeground());
    auto* shell = page->detail_frame.Root();
    lv_obj_set_pos(shell, 20, 84);
    Frame(shell, 1, 2);
    auto* content = page->detail_frame.Content();
    lv_obj_add_flag(content, static_cast<lv_obj_flag_t>(LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE));
    lv_obj_set_scroll_dir(content, LV_DIR_VER);
    lv_obj_remove_flag(content, static_cast<lv_obj_flag_t>(LV_OBJ_FLAG_SCROLL_ELASTIC | LV_OBJ_FLAG_SCROLL_MOMENTUM));
    lv_obj_set_style_pad_top(content, 16, 0);
    lv_obj_set_style_pad_left(content, 16, 0);
    lv_obj_set_style_pad_bottom(content, 16, 0);
    ScrollStyle(content);
    lv_obj_set_style_pad_right(content, 40, 0);
    lv_obj_set_style_pad_right(content, 8, LV_PART_SCROLLBAR);
    auto* info = Container(content);
    lv_obj_set_size(info, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(info, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(info, 12, 0);
    auto* heading = Container(info);
    lv_obj_set_size(heading, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_style_min_height(heading, 152, 0);
    lv_obj_set_flex_flow(heading, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(heading, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(heading, 28, 0);
    Icon(heading, page->selected, 128);
    auto* name = Label(heading, AppStoreName(page->selected), LV_HOR_RES - 240, UiFontRole::Large);
    lv_obj_set_flex_grow(name, 1);
    Metadata(info, LangString_STORE_VERSION, page->selected.version);
    Metadata(info, LangString_STORE_SIZE, SizeText(page->selected.size));
    const auto& category =
        std::strcmp(LangCurrent(), "zh-CN") == 0 ? page->selected.category : page->selected.category_en;
    Metadata(info, LangString_STORE_CATEGORY, category.empty() ? LangString_STORE_NOT_PROVIDED : category);
    Metadata(info, LangString_STORE_LANGUAGE, LanguageText(page->selected.languages));
    Metadata(info, LangString_STORE_PROVIDER,
             page->selected.provider.empty() ? LangString_STORE_NOT_PROVIDED : page->selected.provider);
    Metadata(info, LangString_STORE_LAST_UPDATED, UploadTimeText(page->selected.uploaded_at));
    if (installed) {
        const AppStoreItem* local = Find(page->installed, page->selected.app_id);
        const std::string path = std::string(SdUserPath(SD_PATH_APPS)) + "/" + local->installed_name;
        Metadata(info, LangString_STORE_LOCATION, path);
    }
    const auto& description =
        std::strcmp(LangCurrent(), "zh-CN") == 0 ? page->selected.description : page->selected.description_en;
    Description(page, info, LangString_STORE_DESCRIPTION, description, 0);
    const auto& notes =
        std::strcmp(LangCurrent(), "zh-CN") == 0 ? page->selected.release_notes : page->selected.release_notes_en;
    Description(page, info, LangString_STORE_RELEASE_NOTES, notes, 1);
    auto* actions = page->detail_frame.Footer();
    lv_obj_add_flag(actions, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_height(actions, LV_SIZE_CONTENT);
    lv_obj_set_style_min_height(actions, actions_height, 0);
    lv_obj_set_style_pad_top(actions, 16, 0);
    lv_obj_set_style_pad_hor(actions, 16, 0);
    lv_obj_set_style_border_width(actions, 1, 0);
    lv_obj_set_flex_flow(actions, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(actions, 14, 0);
    lv_obj_update_layout(actions);
    const int content_width = lv_obj_get_content_width(actions);
    auto* primary = Button(actions, installed ? LangString_STORE_RUN : LangString_STORE_INSTALL, content_width, 78,
                           installed ? Run : Install, page, true);
    page->detail_buttons[0] = primary;
    if (installed ? !CanOpen(page) : !CanInstall(page)) {
        lv_obj_add_state(primary, LV_STATE_DISABLED);
    }
    if (installed) {
        auto* row = Container(actions);
        lv_obj_set_size(row, LV_PCT(100), LV_SIZE_CONTENT);
        lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
        lv_obj_set_style_pad_column(row, 16, 0);
        auto* update = Button(row, LangString_STORE_CHECK_UPDATE, (content_width - 16) / 2, 72, Update, page);
        auto* remove = Button(row, LangString_STORE_REMOVE, (content_width - 16) / 2, 72, Remove, page);
        page->detail_buttons[1] = update;
        page->detail_buttons[2] = remove;
        if (Working(page)) {
            lv_obj_add_state(update, LV_STATE_DISABLED);
            lv_obj_add_state(remove, LV_STATE_DISABLED);
        }
    }
    lv_obj_update_layout(actions);
    Feedback(page);
    if (entering) {
        EpdRequestScreenChange();
        lv_obj_invalidate(page->root);
    }
}

static void RowEvent(lv_event_t* event) {
    auto* row = static_cast<AppStoreScreenRow*>(lv_event_get_user_data(event));
    if (lv_event_get_code(event) == LV_EVENT_DELETE) {
        delete row;
        return;
    }
    if (lv_event_get_code(event) != LV_EVENT_CLICKED) {
        return;
    }
    row->page->selected = row->item;
    std::fill(std::begin(row->page->detail_expanded), std::end(row->page->detail_expanded), true);
    Details(row->page);
    if (!Working(row->page)) {
        Start(row->page, AppStoreOperation::Detail, row->item);
    }
}

static void RowAction(lv_event_t* event) {
    auto* row = static_cast<AppStoreScreenRow*>(lv_event_get_user_data(event));
    if (Queued(row->page, row->item.app_id)) {
        CancelInstall(row->page, row->item.app_id);
        Feedback(row->page);
        return;
    }
    if (row->installed ? !CanOpen(row->page) : !CanInstall(row->page)) {
        return;
    }
    row->page->selected = row->item;
    if (row->installed) {
        Launch(row->page, row->item);
    } else {
        QueueInstall(row->page, row->item);
    }
}

static void PreviousPage(lv_event_t* event) {
    auto* page = static_cast<AppStoreScreenPage*>(lv_event_get_user_data(event));
    if (Working(page)) {
        return;
    }
    const bool local = page->filter == AppStoreScreenFilter::Installed ||
                       page->filter == AppStoreScreenFilter::Downloading ||
                       (page->catalog.empty() && page->filter != AppStoreScreenFilter::Uninstalled);
    if (local) {
        if (page->local_page > 1) {
            --page->local_page;
            Render(page, true);
        }
    } else if (page->pagination.number > 1) {
        Start(page, AppStoreOperation::PageFetch, {}, page->pagination.number - 1);
    }
}

static void NextPage(lv_event_t* event) {
    auto* page = static_cast<AppStoreScreenPage*>(lv_event_get_user_data(event));
    if (Working(page)) {
        return;
    }
    const bool local = page->filter == AppStoreScreenFilter::Installed ||
                       page->filter == AppStoreScreenFilter::Downloading ||
                       (page->catalog.empty() && page->filter != AppStoreScreenFilter::Uninstalled);
    if (local) {
        ++page->local_page;
        Render(page, true);
    } else if (page->pagination.more) {
        Start(page, AppStoreOperation::PageFetch, {}, page->pagination.number + 1);
    }
}

static void Render(AppStoreScreenPage* page, bool reset_scroll) {
    page->list_dirty = false;
    const int scroll_y = reset_scroll ? 0 : lv_obj_get_scroll_y(page->list);
    lv_obj_clean(page->list);
    std::vector<AppStoreItem> items;
    const bool local_only = page->filter == AppStoreScreenFilter::Installed ||
                            (page->catalog.empty() && page->filter != AppStoreScreenFilter::Uninstalled);
    if (local_only) {
        const size_t first = (page->local_page - 1) * 16;
        for (size_t i = first; i < page->installed.size() && i - first < 16; ++i) {
            items.push_back(page->installed[i]);
        }
    } else if (page->filter == AppStoreScreenFilter::Downloading) {
        const size_t first = (page->local_page - 1) * 16;
        for (size_t i = first; i < page->queue.size() && i - first < 16; ++i) {
            items.push_back(page->queue[i]);
        }
        if (first == 0 && CurrentInstall(page, page->installing.app_id) && !Find(items, page->installing.app_id)) {
            items.push_back(page->installing);
        }
    } else {
        items = page->catalog;
    }
    for (auto& item : items) {
        if (const AppStoreItem* local = Find(page->installed, item.app_id)) {
            item = *local;
        }
    }
    std::sort(items.begin(), items.end(),
              [](const AppStoreItem& a, const AppStoreItem& b) { return a.app_id < b.app_id; });
    size_t count = 0;
    for (const auto& item : items) {
        const bool installed = Find(page->installed, item.app_id) != nullptr;
        if ((page->filter == AppStoreScreenFilter::Installed && !installed) ||
            (page->filter == AppStoreScreenFilter::Uninstalled && installed) ||
            (page->filter == AppStoreScreenFilter::Downloading && !Queued(page, item.app_id))) {
            continue;
        }
        if (count > 0) {
            auto* divider = Container(page->list);
            lv_obj_set_size(divider, LV_PCT(100), 1);
            lv_obj_set_style_bg_color(divider, UiColorForeground(), 0);
        }
        ++count;
        auto* object = lv_button_create(page->list);
        lv_obj_remove_style_all(object);
        lv_obj_set_style_bg_opa(object, LV_OPA_COVER, 0);
        Fixed(object);
        lv_obj_set_size(object, LV_PCT(100), LV_SIZE_CONTENT);
        lv_obj_set_style_min_height(object, 160, 0);
        lv_obj_set_style_radius(object, 0, 0);
        lv_obj_set_style_border_width(object, 0, 0);
        lv_obj_set_style_pad_ver(object, 24, 0);
        lv_obj_set_style_pad_hor(object, 8, 0);
        lv_obj_set_style_pad_column(object, 24, 0);
        lv_obj_set_flex_flow(object, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(object, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        Icon(object, item, 128);
        auto* text = Container(object);
        lv_obj_set_size(text, 0, LV_SIZE_CONTENT);
        lv_obj_set_flex_grow(text, 1);
        lv_obj_set_flex_flow(text, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_style_pad_row(text, 12, 0);
        Label(text, AppStoreName(item), LV_PCT(100), UiFontRole::Large);
        const auto& summary = std::strcmp(LangCurrent(), "zh-CN") == 0 ? item.summary : item.summary_en;
        if (!summary.empty()) {
            auto* brief = Label(text, summary.c_str(), LV_PCT(100), UiFontRole::Caption);
            lv_label_set_long_mode(brief, LV_LABEL_LONG_DOT);
            lv_obj_set_height(brief, UiFont(UiFontRole::Caption)->line_height);
        }
        Label(text, ("v" + item.version + "  " + SizeText(item.size)).c_str(), LV_PCT(100), UiFontRole::Caption);
        auto* row = new AppStoreScreenRow{page, item, installed};
        row->action =
            Button(object, installed ? LangString_STORE_RUN : LangString_STORE_INSTALL, 142, 68, RowAction, row);
        lv_obj_set_user_data(object, row);
        lv_obj_add_event_cb(object, RowEvent, LV_EVENT_ALL, row);
    }
    page->empty = count == 0;
    const bool local = local_only || page->filter == AppStoreScreenFilter::Downloading;
    const size_t number = local ? page->local_page : page->pagination.number;
    const size_t local_total = page->filter == AppStoreScreenFilter::Downloading
                                   ? page->queue.size() + (CurrentInstall(page, page->installing.app_id) ? 1 : 0)
                                   : page->installed.size();
    const bool more = local ? number < ((local_total + 15) / 16) : page->pagination.more;
    if (number > 1 || more) {
        auto* navigation = Container(page->list);
        lv_obj_set_size(navigation, LV_PCT(100), 76);
        lv_obj_set_flex_flow(navigation, LV_FLEX_FLOW_ROW);
        lv_obj_set_style_pad_column(navigation, 16, 0);
        auto* previous = Button(navigation, LangString_STORE_PREVIOUS_PAGE, 180, 68, PreviousPage, page);
        auto* next = Button(navigation, LangString_STORE_NEXT_PAGE, 180, 68, NextPage, page);
        SetBlocked(previous, number <= 1 || Working(page));
        SetBlocked(next, !more || Working(page));
    }
    Feedback(page);
    lv_obj_update_layout(page->list);
    const int max_scroll = std::max<int>(0, lv_obj_get_scroll_y(page->list) + lv_obj_get_scroll_bottom(page->list));
    lv_obj_scroll_to_y(page->list, std::min(scroll_y, max_scroll), LV_ANIM_OFF);
}

static void SetFilter(AppStoreScreenPage* page, AppStoreScreenFilter filter) {
    page->local_page = 1;
    page->filter = filter;
    page->expanded = false;
    for (int i = 0; i < 4; ++i) {
        const bool selected = i == static_cast<int>(filter);
        lv_obj_set_style_bg_color(page->choices[i], selected ? UiColorForeground() : UiColorBackground(), 0);
        lv_obj_set_style_text_color(page->choices[i], selected ? UiColorBackground() : UiColorForeground(), 0);
    }
    Layout(page);
    Render(page, true);
}

static void All(lv_event_t* event) {
    SetFilter(static_cast<AppStoreScreenPage*>(lv_event_get_user_data(event)), AppStoreScreenFilter::All);
}

static void Uninstalled(lv_event_t* event) {
    SetFilter(static_cast<AppStoreScreenPage*>(lv_event_get_user_data(event)), AppStoreScreenFilter::Uninstalled);
}

static void Installed(lv_event_t* event) {
    SetFilter(static_cast<AppStoreScreenPage*>(lv_event_get_user_data(event)), AppStoreScreenFilter::Installed);
}

static void Downloading(lv_event_t* event) {
    SetFilter(static_cast<AppStoreScreenPage*>(lv_event_get_user_data(event)), AppStoreScreenFilter::Downloading);
}

static void DismissFilter(lv_event_t* event) {
    auto* page = static_cast<AppStoreScreenPage*>(lv_event_get_user_data(event));
    page->expanded = false;
    Layout(page);
}

static void ToggleFilter(lv_event_t* event) {
    auto* page = static_cast<AppStoreScreenPage*>(lv_event_get_user_data(event));
    page->expanded = !page->expanded;
    Layout(page);
}

static bool Key(const char* key) {
    auto* page = s_current;
    if (!page || (std::strcmp(key, "vk_home") != 0 && std::strcmp(key, "vk_prev") != 0)) {
        return false;
    }

    if (page->detail) {
        CloseDetails(page);
        return true;
    }
    if (page->expanded) {
        page->expanded = false;
        Layout(page);
        return true;
    }
    return false;
}

static void Tick(AppStoreScreenPage* page) {
    DeleteClosedLayers(page);
    if (page->list_dirty) {
        Render(page);
    }
    if (!page->notice_sticky && !lv_obj_has_flag(page->notice, LV_OBJ_FLAG_HIDDEN) &&
        lv_tick_elaps(page->notice_tick) >= 3000) {
        lv_obj_add_flag(page->notice, LV_OBJ_FLAG_HIDDEN);
        Layout(page);
    }
    if (!page->pending) {
        if (page->loading && lv_tick_elaps(page->retry_tick) >= 1000) {
            TryLoad(page);
        } else if (lv_tick_elaps(page->retry_tick) >= 1500) {
            PumpQueue(page);
        }
        return;
    }
    if (!page->pending->done.load()) {
        auto& progress = page->pending->progress;
        const size_t total = progress.total.load(), bytes = progress.bytes.load();
        const bool verifying = progress.verifying.load();
        const unsigned percent = total ? static_cast<unsigned>(bytes * 100 / total) : 0;
        if (total && (percent != page->progress_percent || verifying != page->progress_verifying) &&
            lv_tick_elaps(page->progress_tick) >= 1000) {
            page->progress_tick = lv_tick_get();
            page->progress_percent = percent;
            page->progress_verifying = verifying;
            Feedback(page);
        }
        return;
    }
    const auto operation = page->pending_operation;
    auto result = std::move(page->pending);
    if (result->scanned) {
        page->installed = std::move(result->installed);
    }
    if (result->fetched) {
        page->catalog = std::move(result->catalog);
        page->pagination = std::move(result->pagination);
        page->catalog_failed = false;
        page->catalog_stale = false;
    }
    for (auto& local : page->installed) {
        const auto* remote = Find(page->catalog, local.app_id);
        if (!remote) {
            continue;
        }
        local.summary = remote->summary;
        local.summary_en = remote->summary_en;
        local.category = remote->category;
        local.category_en = remote->category_en;
        if (remote->version != local.version || remote->sha256 != local.sha256) {
            continue;
        }
        local.languages = remote->languages;
        local.provider = remote->provider;
        local.release_notes = remote->release_notes;
        local.release_notes_en = remote->release_notes_en;
        local.uploaded_at = remote->uploaded_at;
    }
    if (operation == AppStoreOperation::Scan) {
        page->scan_error = result->error;
        page->next_load = AppStoreOperation::Refresh;
    } else {
        if (operation == AppStoreOperation::Refresh) {
            page->loading = false;
            page->catalog_failed = !result->fetched;
        }
        if (!result->ok) {
            const bool offline_installed = operation == AppStoreOperation::Refresh && page->scan_error.empty() &&
                                           !page->installed.empty() && result->error == LangString_STORE_NETWORK_ERROR;
            Notice(page, offline_installed ? LangString_STORE_OFFLINE_INSTALLED
                         : operation == AppStoreOperation::Refresh && !page->scan_error.empty()
                             ? page->scan_error.c_str()
                             : result->error.c_str());
        } else if (result->latest) {
            Notice(page, LangString_STORE_CURRENT);
        } else if (operation == AppStoreOperation::Refresh || operation == AppStoreOperation::PageFetch) {
            Notice(page, page->scan_error.empty() ? LangString_STORE_REFRESHED : page->scan_error.c_str());
        } else if (operation == AppStoreOperation::CheckUpdate || operation == AppStoreOperation::Detail) {
            page->notice_sticky = false;
            lv_obj_add_flag(page->notice, LV_OBJ_FLAG_HIDDEN);
        } else {
            Notice(page, operation == AppStoreOperation::Remove   ? LangString_STORE_REMOVED
                         : operation == AppStoreOperation::Update ? LangString_STORE_UPDATED
                                                                        : LangString_STORE_INSTALLED_DONE);
        }
    }
    if (operation == AppStoreOperation::Detail && result->ok) {
        page->selected = std::move(result->candidate);
    }
    if (page->detail) {
        if (result->ok && operation == AppStoreOperation::Remove) {
            CloseDetails(page);
        } else {
            if (operation != AppStoreOperation::Detail) {
                if (const AppStoreItem* item = Find(page->installed, page->selected.app_id)) {
                    page->selected = *item;
                }
            }
            Details(page);
        }
    }
    Render(page);
    if (operation == AppStoreOperation::CheckUpdate && result->ok && !result->latest &&
        !result->cancelled.load()) {
        UpdateAvailable(page, result->candidate);
    }
    if (operation == AppStoreOperation::PageFetch && result->pagination.expired) {
        BeginRefresh(page);
        return;
    }
    if (!result->ok && result->error == LangString_STORE_UNAVAILABLE && operation != AppStoreOperation::Refresh &&
        operation != AppStoreOperation::PageFetch) {
        page->catalog_stale = true;
    }
    if (page->catalog_stale && page->queue.empty()) {
        page->catalog_stale = false;
        BeginRefresh(page);
        return;
    }
    if (operation == AppStoreOperation::Scan) {
        TryLoad(page);
    } else if (!page->queue.empty()) {
        page->retry_tick = lv_tick_get();
    } else {
        page->install_hold.reset();
    }
}

static void Lifecycle(lv_event_t* event) {
    auto* page = static_cast<AppStoreScreenPage*>(lv_event_get_user_data(event));
    if (lv_event_get_code(event) == LV_EVENT_SCREEN_LOADED) {
        page->active = true;
        if (!page->network_hold) {
            page->network_hold.emplace(PowerNeed::ForegroundNetwork);
        }
    } else if (lv_event_get_code(event) == LV_EVENT_SCREEN_UNLOADED) {
        page->active = false;
        page->queue.clear();
        page->install_hold.reset();
        page->network_hold.reset();
        if (page->pending) {
            page->pending->cancelled.store(true);
        }
    }
}

static void Delete(lv_event_t* event) {
    auto* page = static_cast<AppStoreScreenPage*>(lv_event_get_user_data(event));
    if (page->pending) {
        page->pending->cancelled.store(true);
    }
    if (s_current == page) {
        s_current = nullptr;
    }
    delete page;
}

lv_obj_t* AppStoreScreen::Create() {
    auto* page = new AppStoreScreenPage;
    s_current = page;
    ScreenSetIsHome(false);
    page->root = Container(nullptr);
    lv_obj_set_style_text_font(page->root, UiFont(), 0);
    const auto bar = ScreenCreateStatusBar(page->root);
    page->title = Title(page->root, LangString_STORE_TITLE, bar.height);
    auto* toolbar = Container(page->root);
    lv_obj_set_pos(toolbar, 20, bar.height + 68);
    lv_obj_set_size(toolbar, LV_HOR_RES - 40, 84);
    Frame(toolbar, 2, 3);
    page->count = Label(toolbar, "", LV_HOR_RES - 220, UiFontRole::Body);
    lv_obj_align(page->count, LV_ALIGN_LEFT_MID, 86, 0);
    auto icon_button = [page, toolbar](int x, lv_event_cb_t callback, const lv_point_precise_t* points, size_t count) {
        auto* button = Button(toolbar, "", 60, 60, callback, page);
        lv_obj_set_style_border_width(button, 1, 0);
        lv_obj_clean(button);
        lv_obj_align(button, LV_ALIGN_LEFT_MID, x, 0);
        auto* line = lv_line_create(button);
        lv_line_set_points(line, points, count);
        lv_obj_set_style_line_color(line, UiColorForeground(), 0);
        lv_obj_set_style_line_width(line, 2, 0);
        lv_obj_center(line);
        lv_obj_remove_flag(line, LV_OBJ_FLAG_CLICKABLE);
        return button;
    };
    static const lv_point_precise_t funnel[] = {{0, 0}, {28, 0}, {17, 13}, {17, 28}, {10, 23}, {10, 13}, {0, 0}};
    static const lv_point_precise_t refresh[] = {{18, 9}, {27, 9}, {27, 0}, {27, 9},  {23, 3},  {16, 0}, {8, 2},
                                                 {2, 8},  {0, 16}, {3, 23}, {10, 28}, {18, 28}, {25, 23}};
    page->filter_button = icon_button(10, ToggleFilter, funnel, 7);
    page->refresh_button = icon_button(LV_HOR_RES - 114, Refresh, refresh, 13);
    page->filter_overlay = Container(page->root);
    lv_obj_set_pos(page->filter_overlay, 0, 0);
    lv_obj_set_size(page->filter_overlay, LV_HOR_RES, LV_VER_RES);
    lv_obj_set_style_bg_opa(page->filter_overlay, LV_OPA_TRANSP, 0);
    lv_obj_add_flag(page->filter_overlay, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(page->filter_overlay, DismissFilter, LV_EVENT_CLICKED, page);
    page->filters = Container(page->filter_overlay);
    lv_obj_set_pos(page->filters, 20, bar.height + 168);
    lv_obj_set_size(page->filters, LV_HOR_RES - 40, LV_SIZE_CONTENT);
    lv_obj_set_style_min_height(page->filters, 248, 0);
    lv_obj_set_flex_flow(page->filters, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(page->filters, 16, 0);
    lv_obj_set_style_pad_all(page->filters, 16, 0);
    lv_obj_set_style_border_width(page->filters, 2, 0);
    lv_obj_set_style_radius(page->filters, 12, 0);
    lv_obj_add_flag(page->filters, LV_OBJ_FLAG_CLICKABLE);
    Label(page->filters, LangString_STORE_INSTALL_STATE, LV_PCT(100), UiFontRole::Body);
    auto* choices = ScreenCreateOptionGrid(page->filters, 2);
    const char* labels[] = {LangString_STORE_ALL, LangString_STORE_UNINSTALLED, LangString_STORE_INSTALLED,
                            LangString_STORE_DOWNLOADING};
    lv_event_cb_t callbacks[] = {All, Uninstalled, Installed, Downloading};
    const int width = (LV_HOR_RES - 92) / 2;
    for (int i = 0; i < 4; ++i) {
        page->choices[i] = Button(choices, labels[i], width, 68, callbacks[i], page, i == 0);
    }
    page->status = Label(page->root, "", LV_HOR_RES - 48, UiFontRole::Body);
    page->list_frame = Container(page->root);
    Frame(page->list_frame, 1, 2);
    lv_obj_set_style_pad_all(page->list_frame, 8, 0);
    page->list = Container(page->list_frame);
    lv_obj_set_size(page->list, LV_PCT(100), LV_PCT(100));
    ScrollStyle(page->list);
    lv_obj_add_flag(page->list, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scroll_dir(page->list, LV_DIR_VER);
    lv_obj_remove_flag(page->list,
                       static_cast<lv_obj_flag_t>(LV_OBJ_FLAG_SCROLL_ELASTIC | LV_OBJ_FLAG_SCROLL_MOMENTUM));
    lv_obj_set_flex_flow(page->list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(page->list, 0, 0);
    page->notice = ScreenCreateToast(page->root, "");
    lv_obj_add_flag(page->notice, LV_OBJ_FLAG_HIDDEN);
    Settings settings("app_store", false);
    page->server = kApiDefaultAppStoreUrl;
    const std::string saved_server = settings.GetString("server");
    if (!saved_server.empty()) {
        page->server = saved_server;
    }
    ESP_LOGI("AppStore", "operation=configure source=%s", !saved_server.empty() ? "saved" : "default");
    Layout(page);
    lv_obj_add_event_cb(page->root, Delete, LV_EVENT_DELETE, page);
    lv_obj_add_event_cb(page->root, Lifecycle, LV_EVENT_SCREEN_LOADED, page);
    lv_obj_add_event_cb(page->root, Lifecycle, LV_EVENT_SCREEN_UNLOADED, page);
    VkKeyScreenDesc keys;
    keys.factory = ResumeStore;
    keys.on_key = Key;
    VkKey_AttachScreen(page->root, "AppStoreScreen", keys);
    BeginRefresh(page);
    s_page_lifecycle.Bind(page->root, {[] {
        if (s_current) {
            Tick(s_current);
        }
    }, [](UiPagePowerEvent event) {
        if (event == UiPagePowerEvent::PauseApplications || event == UiPagePowerEvent::ResumeApplications) {
            AppStoreScreen::SetStandby(event == UiPagePowerEvent::PauseApplications);
        }
    }, nullptr, 150});
    return page->root;
}

void AppStoreScreen::SetStandby(bool standby) {
    auto* page = s_current;
    if (!page || !page->active || page->standby == standby) {
        return;
    }
    page->standby = standby;
    if (standby) {
        page->reload_on_wake = Working(page);
        if (page->pending) {
            page->pending->cancelled.store(true);
        }
        page->pending.reset();
        page->queue.clear();
        page->install_hold.reset();
        page->loading = false;
        page->network_hold.reset();
        lv_obj_add_flag(page->notice, LV_OBJ_FLAG_HIDDEN);
        page->notice_sticky = false;
    } else {
        page->network_hold.emplace(PowerNeed::ForegroundNetwork);
        if (page->reload_on_wake) {
            page->reload_on_wake = false;
            BeginRefresh(page);
        } else {
            Feedback(page);
        }
    }
}
