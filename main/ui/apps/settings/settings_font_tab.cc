#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <memory>
#include <new>
#include <string>
#include <strings.h>
#include <esp_heap_caps.h>
#include <esp_log.h>
#include "assets/lang_config.h"
#include "boards/common/storage/sd_access.h"
#include "boards/common/storage/sd_directory_page.h"
#include "infra/storage/sd_paths.h"
#include "infra/storage/settings.h"
#include "infra/diagnostics/firmware_memory.h"
#include "sysmgmt/tasks/task_executor.h"
#include "ui/apps/settings/settings_common.h"
#include "ui/apps/settings/settings_font_tab.h"
#include "ui/apps/settings/settings_screen.h"
#include "ui/common/drawing/font/ui_fonts.h"
#include "ui/common/drawing/ui_profile.h"
#include "ui/common/drawing/ui_theme.h"
#include "ui/common/runtime/page_lifecycle.h"
#include "ui/common/widgets/screen_common.h"
#include "ui/common/widgets/screen_scrollbar.h"

static constexpr const char* TAG = "SettingsFont";
static unsigned s_mount_generation = UINT32_MAX; // 当前字体目录所属介质
static int s_font_batch = 1; // 页面完成布局后按视口更新

struct SettingsFontLoad {
    std::string file;  // 后台作业独占的文件名
    bool apply = false;  // 成功后保存选择并接入系统字体
    UiFontResourcePtr resource;  // 预览与应用共用已加载字体
    bool ok = false;  // 完成标记发布前写入
    std::atomic<bool> done{false};  // 后台结果已发布
};

static UiPageLifecycle s_page;
static SdDirectoryPage s_directory;
static lv_obj_t* s_root = nullptr;
static lv_obj_t* s_list = nullptr;
static lv_obj_t* s_list_section = nullptr;
static lv_obj_t* s_preview_page = nullptr;
static int s_list_scroll = 0;
static unsigned s_view_generation = UINT32_MAX;
static int s_requested_page = 0;
static int s_page_direction = 0;
static bool s_has_more = false;
static bool s_scan_loading = false;
static bool s_locate_current = false; // 打开列表时定位当前字体，翻页与预览返回保留用户位置
static bool s_memory_wait = false;
static int s_directory_page = 0;
static bool s_preview = false;
static std::string s_selected;
static std::string s_shown_file;
static UiFontResourcePtr s_preview_resource;

static std::shared_ptr<SettingsFontLoad> s_load;

static void BuildFontList();
static void RequestFontPage(int page, int direction, bool locate_current = false);
static void ShowFontList();
static void ApplyFontPageResult(const SdDirectoryPage::Result& result);

static const char* FontText(const char* zh, const char* en) {
    return std::strcmp(LangCode, "zh-CN") == 0 ? zh : en;
}

static bool FontFilter(const char* path, bool directory) {
    const char* extension = std::strrchr(path, '.');
    return !directory && extension &&
           (strcasecmp(extension, ".ttf") == 0 || strcasecmp(extension, ".otf") == 0);
}

static bool StartFontLoad(const std::string& file, bool apply) try {
    if (s_load && !s_load->done.load(std::memory_order_acquire)) {
        return false;
    }
    auto job = std::make_shared<SettingsFontLoad>();
    job->file = file;
    job->apply = apply;
    if (apply) {
        job->resource = s_preview_resource;
    }
    const bool scheduled = TaskExecutor::Schedule([job]() {
        try {
            if (!job->resource) {
                job->resource = UiFontLoadFile(job->file.c_str());
            }
            job->ok = UiFontPreview(job->resource) != nullptr;
            if (job->ok && job->apply) {
                Settings settings("display", true);
                const auto previous = settings.GetString("system_font");
                job->ok = settings.SetString("system_font", job->file) && settings.Commit();
                if (job->ok) {
                    job->ok = UiFontSetFile(job->resource);
                    if (!job->ok && (!settings.SetString("system_font", previous) || !settings.Commit())) {
                        ESP_LOGE(TAG, "operation=apply stage=restore_preference result=failed");
                    }
                }
            }
        } catch (const std::bad_alloc&) {
            job->ok = false;
            ESP_LOGE(TAG, "operation=load stage=execute result=no_memory");
        }
        job->done.store(true, std::memory_order_release);
    }, TaskDomain::Storage);
    if (!scheduled) {
        return false;
    }
    s_load = std::move(job);
    return true;
} catch (const std::bad_alloc&) {
    ESP_LOGE(TAG, "operation=load stage=prepare result=no_memory");
    return false;
}

static void ShowPreview() {
    s_list_scroll = lv_obj_get_scroll_y(s_root);
    const auto* font = UiFontPreview(s_preview_resource);
    if (!font) {
        s_preview = false;
        ShowFontList();
        return;
    }
    const auto name = s_selected.empty() ? std::string(FontText("默认字体", "Default font")) : s_selected.substr(0, s_selected.find_last_of('.'));
    SettingsScreen::SetTitle(name.c_str());
    lv_obj_add_flag(s_list_section, LV_OBJ_FLAG_HIDDEN);
    s_preview_page = SettingsCreateSection(s_root);
    lv_obj_set_height(s_preview_page, lv_obj_get_content_height(s_root));
    lv_obj_scroll_to_y(s_root, 0, LV_ANIM_OFF);
    auto* sample = lv_obj_create(s_preview_page);
    lv_obj_remove_style_all(sample);
    lv_obj_set_size(sample, lv_pct(100), LV_SIZE_CONTENT);
    lv_obj_set_style_border_side(sample, static_cast<lv_border_side_t>(LV_BORDER_SIDE_TOP | LV_BORDER_SIDE_BOTTOM), 0);
    lv_obj_set_style_border_width(sample, 1, 0);
    lv_obj_set_style_border_color(sample, UiColorForeground(), 0);
    lv_obj_set_style_pad_hor(sample, UiSx(8), 0);
    lv_obj_set_style_pad_ver(sample, UiSy(21), 0);
    lv_obj_set_flex_flow(sample, LV_FLEX_FLOW_COLUMN);
    lv_obj_clear_flag(sample, static_cast<lv_obj_flag_t>(LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE));
    auto* preview = lv_label_create(sample);
    lv_obj_set_width(preview, lv_pct(100));
    lv_label_set_long_mode(preview, LV_LABEL_LONG_WRAP);
    lv_label_set_text(preview, FontText("山川湖海，万物有声。\n阅读，让每一刻更从容。\n0123456789  ABCDEFG\n设置 · 首页 · 返回", "The world is full of stories.\nMake time for a little reading.\n0123456789  ABCDEFG\nSettings · Home · Back"));
    lv_obj_set_style_text_font(preview, font, 0);
    auto* space = lv_obj_create(s_preview_page);
    lv_obj_remove_style_all(space);
    lv_obj_set_size(space, 1, 0);
    lv_obj_set_flex_grow(space, 1);
    lv_obj_clear_flag(space, static_cast<lv_obj_flag_t>(LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE));
    auto* apply = ScreenCreateSelectableOption(s_preview_page, FontText("应用", "Apply"), [](lv_event_t*) {
        if (!StartFontLoad(s_selected, true)) {
            ScreenShowToast(FontText("暂时无法应用，请重试", "Unable to apply. Try again."));
        }
    }, 0);
    ScreenStyleSelectable(apply, lv_obj_get_child(apply, 0), true);
}

static void AddFontRow(const char* file) {
    const size_t file_size = std::strlen(file);
    char* owned = nullptr;
    if (file_size != 0) {
        owned = static_cast<char*>(heap_caps_malloc(file_size + 1, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
        if (!owned) {
            ESP_LOGE(TAG, "operation=list stage=row_name result=no_memory bytes=%u", unsigned(file_size + 1));
            return;
        }
        std::memcpy(owned, file, file_size + 1);
    }
    char name[SD_FILE_NAME_CAPACITY];
    std::snprintf(name, sizeof(name), "%s", file_size ? file : FontText("默认字体", "Default font"));
    if (file_size) {
        if (char* extension = std::strrchr(name, '.')) {
            *extension = '\0';
        }
    }
    auto* row = SettingsCreateListRow(s_list, name,
        "", [](lv_event_t* event) {
            if (s_scan_loading) {
                return;
            }
            auto* row = lv_event_get_current_target_obj(event);
            const char* file = lv_obj_get_user_data(row) ? static_cast<const char*>(lv_obj_get_user_data(row)) : "";
            s_selected = file;
            s_list_scroll = lv_obj_get_scroll_y(s_root);
            s_preview = true;
            if (!StartFontLoad(s_selected, false)) {
                s_preview = false;
                ScreenShowToast(FontText("暂时无法预览，请重试", "Unable to preview. Try again."));
            }
        });
    SettingsSetListRowCurrent(row, std::strcmp(file, UiFontFile()) == 0);
    if (owned) {
        lv_obj_set_user_data(row, owned);
        lv_obj_add_event_cb(row, [](lv_event_t* event) {
            heap_caps_free(lv_event_get_user_data(event));
        }, LV_EVENT_DELETE, owned);
    }
}

static void UpdateFontRows() {
    if (!s_list) {
        return;
    }
    for (uint32_t i = 0; i < lv_obj_get_child_count(s_list); ++i) {
        auto* row = lv_obj_get_child(s_list, i);
        const auto* file = static_cast<const char*>(lv_obj_get_user_data(row));
        SettingsSetListRowCurrent(row, std::strcmp(UiFontFile(), file ? file : "") == 0);
    }
    s_shown_file = UiFontFile();
}

static void BeginFontRows(int page) {
    if (s_list_section) {
        lv_obj_delete(s_list_section);
    }
    s_list_section = SettingsCreateSection(s_root, FontText("可用字体", "Available fonts"));
    s_list = SettingsCreateListGroup(s_list_section);
    if (page == 0) {
        AddFontRow("");
    }
}

static void FinishFontRows(int page, bool more, int direction) {
    s_directory_page = page;
    s_has_more = more;
    s_scan_loading = false;
    s_view_generation = SdAccessMountGeneration();
    s_shown_file = UiFontFile();
    lv_obj_update_layout(s_root);
    lv_obj_scroll_to_y(s_root, direction < 0 ? std::max<int>(0, lv_obj_get_scroll_bottom(s_root)) : 0, LV_ANIM_OFF);
    if (s_locate_current) {
        for (uint32_t i = 0; i < lv_obj_get_child_count(s_list); ++i) {
            auto* row = lv_obj_get_child(s_list, i);
            const auto* file = static_cast<const char*>(lv_obj_get_user_data(row));
            if (std::strcmp(UiFontFile(), file ? file : "") == 0) {
                // 分页容器关闭原生滚动，按可视区直接调整偏移。
                lv_area_t viewport;
                lv_area_t area;
                lv_obj_get_content_coords(s_root, &viewport);
                lv_obj_get_coords(row, &area);
                int target = lv_obj_get_scroll_y(s_root);
                if (area.y1 < viewport.y1) {
                    target += area.y1 - viewport.y1;
                } else if (area.y2 > viewport.y2) {
                    target += std::min(area.y1 - viewport.y1, area.y2 - viewport.y2);
                }
                lv_obj_scroll_to_y(s_root, target, LV_ANIM_OFF);
                break;
            }
        }
        s_locate_current = false;
    }
    ScreenScrollbarShow(s_root);
}

static bool FontListWorkMemoryAvailable() {
    if (FirmwarePsramCanAllocate(0)) {
        s_memory_wait = false;
        return true;
    }
    if (!s_memory_wait) {
        ScreenShowToast(FontText("内存暂不足，请稍后重试", "Not enough memory. Try again later."));
    }
    s_memory_wait = true;
    return false;
}

static void RequestFontPage(int page, int direction, bool locate_current) {
    FirmwareMemoryDemand demand(0);
    s_requested_page = page;
    s_page_direction = direction;
    s_locate_current = locate_current;
    if (!FontListWorkMemoryAvailable()) {
        return;
    }
    s_scan_loading = true;
    std::string locate_path;
    if (locate_current && *UiFontFile()) {
        locate_path = std::string(SD_PATH_FONT_SRC) + "/" + UiFontFile();
    }
    s_directory.Request({SD_PATH_FONT_SRC}, FontFilter, page, s_font_batch, false, nullptr, true,
                        {}, false, locate_path);
    SdDirectoryPage::Result result;
    if (s_directory.Poll(result)) {
        ApplyFontPageResult(result);
    }
}

static bool MoveFontPage(int direction) {
    if (s_scan_loading) {
        return true;
    }
    if ((direction < 0 && s_directory_page == 0) || (direction > 0 && !s_has_more)) {
        return false;
    }
    RequestFontPage(s_directory_page + (direction > 0 ? 1 : -1), direction);
    return true;
}

static void FontBoundaryGesture(lv_event_t* event) {
    if (s_preview || !s_root) {
        return;
    }
    auto* input = lv_indev_active();
    if (!input) {
        return;
    }
    const auto direction = lv_indev_get_gesture_dir(input);
    if ((direction == LV_DIR_TOP || direction == LV_DIR_BOTTOM) &&
        MoveFontPage(direction == LV_DIR_TOP ? 1 : -1)) {
        lv_event_stop_bubbling(event);
        lv_indev_wait_release(input);
    }
}

static void ShowFontList() {
    if (s_preview_page) {
        lv_obj_delete(s_preview_page);
        s_preview_page = nullptr;
    }
    s_preview_resource.reset();
    SettingsScreen::SetTitle(FontText("系统字体", "System font"));
    if (s_list_section) {
        lv_obj_clear_flag(s_list_section, LV_OBJ_FLAG_HIDDEN);
        UpdateFontRows();
        lv_obj_update_layout(s_root);
        lv_obj_scroll_to_y(s_root, s_list_scroll, LV_ANIM_OFF);
        ScreenScrollbarShow(s_root);
        if (s_view_generation != SdAccessMountGeneration()) {
            RequestFontPage(0, 0, true);
        }
    } else {
        BuildFontList();
    }
}

static bool FontRowsMatch(const SdDirectoryPage::Result& result) {
    const uint32_t first = result.page == 0 ? 1 : 0;
    if (!s_list || s_directory_page != result.page ||
        lv_obj_get_child_count(s_list) != result.entries.size() + first) {
        return false;
    }
    for (size_t i = 0; i < result.entries.size(); ++i) {
        const auto& path = result.entries[i].path;
        const auto slash = path.find_last_of('/');
        const char* file = path.c_str() + (slash == std::string::npos ? 0 : slash + 1);
        auto* row = lv_obj_get_child(s_list, i + first);
        const auto* shown = static_cast<const char*>(lv_obj_get_user_data(row));
        if (!shown || std::strcmp(shown, file) != 0) {
            return false;
        }
    }
    return true;
}

static void PollFontPage() {
    const unsigned generation = SdAccessMountGeneration();
    if (s_mount_generation != generation) {
        s_mount_generation = generation;
        s_directory.Cancel();
        s_scan_loading = false;
        if (!s_preview) {
            RequestFontPage(0, 0, true);
        }
    }
    if (s_load && s_load->done.load(std::memory_order_acquire)) {
        const auto job = std::move(s_load);
        if (!job->ok) {
            s_preview = false;
            ShowFontList();
            ScreenShowToast(job->apply ? FontText("无法应用字体，请重试", "Unable to apply the font. Try again.")
                                      : FontText("字体不可用，请检查字体文件", "Font unavailable. Check the font file."));
        } else if (job->apply) {
            s_preview = false;
            ShowFontList();
            return;
        } else if (s_preview) {
            s_preview_resource = job->resource;
            ShowPreview();
        }
    }
    if (s_preview) {
        if (s_preview_resource && !UiFontPreview(s_preview_resource)) {
            s_preview = false;
            ShowFontList();
        }
        return;
    }
    if (s_shown_file != UiFontFile()) {
        UpdateFontRows();
    }
    if (s_memory_wait && FirmwarePsramCanAllocate(0)) {
        RequestFontPage(s_requested_page, s_page_direction, s_locate_current);
    }
    SdDirectoryPage::Result result;
    if (!s_scan_loading || !s_directory.Poll(result)) {
        return;
    }
    ApplyFontPageResult(result);
}

static void ApplyFontPageResult(const SdDirectoryPage::Result& result) {
    FirmwareMemoryDemand demand(0);
    s_scan_loading = false;
    if (!result.ok) {
        ScreenShowToast(FontText("暂时无法读取字体列表，请稍后重试", "Unable to read the font list. Try again later."));
        return;
    }
    if (result.page > 0 && result.entries.empty()) {
        s_has_more = false;
        return;
    }
    const bool more = result.more && (!s_directory.Counted() || result.page + 1 < s_directory.Pages());
    if (FontRowsMatch(result)) {
        s_has_more = more;
        s_view_generation = SdAccessMountGeneration();
        UpdateFontRows();
        if (s_locate_current) {
            FinishFontRows(result.page, more, s_page_direction);
        }
        return;
    }
    if (!FontListWorkMemoryAvailable()) {
        return;
    }
    BeginFontRows(result.page);
    for (const auto& entry : result.entries) {
        const auto slash = entry.path.find_last_of('/');
        AddFontRow(entry.path.c_str() + (slash == std::string::npos ? 0 : slash + 1));
    }
    FinishFontRows(result.page, more, s_page_direction);
}

static void BuildFontList() {
    SettingsScreen::SetTitle(FontText("系统字体", "System font"));
    s_preview_resource.reset();
    s_directory_page = 0;
    s_has_more = false;
    s_list_scroll = 0;
    BeginFontRows(0);
    RequestFontPage(0, 0, true);
}

void SettingsFontTab_Build(lv_obj_t* page) {
    SettingsFontTab_Reset();
    s_root = page;
    lv_obj_update_layout(page);
    const int viewport_rows = std::max<int>(1, lv_obj_get_content_height(page) / std::max<int>(1, UiSy(51)));
    s_font_batch = viewport_rows * 2;
    s_page.Bind(page, {PollFontPage, nullptr, SettingsFontTab_Reset, 100}, true);
    s_mount_generation = SdAccessMountGeneration();
    lv_obj_add_event_cb(page, FontBoundaryGesture, LV_EVENT_GESTURE, nullptr);
    BuildFontList();
}

void SettingsFontTab_Reset() {
    s_page.Unbind();
    if (s_root && lv_obj_is_valid(s_root)) {
        lv_obj_remove_event_cb(s_root, FontBoundaryGesture);
    }
    s_directory.Cancel();
    s_load.reset();
    s_root = s_list = s_list_section = s_preview_page = nullptr;
    s_has_more = s_scan_loading = s_memory_wait = s_locate_current = false;
    s_preview = false;
    s_selected.clear();
    s_shown_file.clear();
    s_preview_resource.reset();
}

bool SettingsFontTab_Back() {
    if (!s_root || !s_preview) {
        return false;
    }
    s_preview = false;
    s_load.reset();
    ShowFontList();
    return true;
}

bool SettingsFontTab_Scroll(int direction) {
    if (!s_root || s_preview) {
        return false;
    }
    return SettingsScrollPage(s_root, direction) || MoveFontPage(direction);
}
