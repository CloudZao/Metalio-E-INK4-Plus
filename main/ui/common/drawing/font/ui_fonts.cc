#include <algorithm>
#include <array>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <new>
#include <string>
#include <strings.h>
#include <utility>
#include <esp_log.h>
#include <esp_timer.h>
#include "ui_fonts.h"
#include "fontpack_lvgl.h"
#include "ttf_runtime.h"
#include "assets/lang_config.h"
#include "boards/common/storage/sd_access.h"
#include "infra/storage/settings.h"
#include "infra/storage/sd_paths.h"
#include "sysmgmt/tasks/task_executor.h"
#include "ui/common/drawing/adapter/lv_adapter_display.h"
#include "ui/common/drawing/display.h"
#include "ui/common/drawing/ui_profile.h"
#include "ui/common/widgets/screen_common.h"

struct UiFontSpec {
    int size; // 字号，像素
    bool emphasis; // 使用强调字重
};

static constexpr UiFontSpec kUiFontSpecs[] = {
    {GetUiResourceProfile().ui_font_size, false}, {GetUiResourceProfile().ui_font_size, false},
    {GetUiResourceProfile().ui_font_size, true}, {GetUiResourceProfile().ui_font_size, true},
    {GetUiResourceProfile().ui_large_font_size, false}, {GetUiResourceProfile().ui_large_font_size, true},
    {GetUiResourceProfile().ui_caption_font_size, true}, {GetUiResourceProfile().ui_caption_font_size, false},
};
static constexpr unsigned kUiFontCount = sizeof(kUiFontSpecs) / sizeof(kUiFontSpecs[0]);
static_assert(kUiFontCount == static_cast<unsigned>(UiFontRole::Caption) + 1);

struct UiFontResource {
    char file[SD_FILE_NAME_CAPACITY] = {}; // fonts_ttf 内的文件名
    std::atomic<unsigned> generation{0}; // 加载或后台校验完成的 SD 挂载代次
    TtfRuntime* fonts[2] = {}; // 正文及辅助字号，字号相同则共用实例
    lv_font_t preview{}; // 预览控件引用的稳定字体对象

    ~UiFontResource() {
        if (fonts[1] != fonts[0]) {
            TtfRuntimeClose(fonts[1]);
        }
        TtfRuntimeClose(fonts[0]);
    }
};

static std::mutex s_font_mutex;
static UiFontResourcePtr s_current;
static char s_system_font[SD_FILE_NAME_CAPACITY] = {};
static char s_requested_font[SD_FILE_NAME_CAPACITY] = {};
static uint32_t s_revision = 0;
static bool s_use_builtin = false; // 流式字体恢复前使用内置字形，保留原资源及当前选择
static bool s_restore_failed = false;
static uint32_t s_restore_failure_revision = 0;
static std::atomic<bool> s_restore_enabled{false};
static std::atomic<bool> s_restore_busy{false};
static std::atomic<bool> s_restore_interrupted{false};
static int64_t s_restore_retry_at = 0;

static const std::array<const lv_font_t*, kUiFontCount>& BuiltinFonts() {
    static const auto fonts = [] {
        std::array<const lv_font_t*, kUiFontCount> result{};
        for (unsigned i = 0; i < result.size(); ++i) {
            const auto& spec = kUiFontSpecs[i];
            result[i] = spec.emphasis ? fontpack_lv_font_emphasis(spec.size, GetUiResourceProfile().emphasis_native_bpp,
                                                                  GetUiResourceProfile().emphasis_expand_px)
                                      : fontpack_lv_font_get(spec.size, 2);
            if (!result[i]) {
                result[i] = fontpack_lv_font_ui();
            }
        }
        return result;
    }();
    return fonts;
}

static const lv_font_t* FontSource(unsigned index, UiFontResourcePtr& owner) {
    {
        std::lock_guard<std::mutex> lock(s_font_mutex);
        owner = s_use_builtin ? UiFontResourcePtr{} : s_current;
    }
    const bool caption = kUiFontSpecs[index].size == GetUiResourceProfile().ui_caption_font_size;
    const auto* selected = owner ? TtfRuntimeFont(owner->fonts[caption ? 1 : 0]) : nullptr;
    return selected ? selected : BuiltinFonts()[index];
}

static bool ViewGlyphMetrics(const lv_font_t* view, lv_font_glyph_dsc_t* output, uint32_t letter, uint32_t next) {
    const auto index = static_cast<const UiFontSpec*>(view->dsc) - kUiFontSpecs;
    UiFontResourcePtr owner;
    const auto* source = FontSource(index, owner);
    if (!source->get_glyph_dsc(source, output, letter, next)) {
        return false;
    }
    if (output->box_h > view->line_height) {
        return false;
    }
    output->ofs_y = std::clamp<int>(output->ofs_y, -view->base_line,
                                    view->line_height - view->base_line - output->box_h);
    if (owner && owner->file[0] && kUiFontSpecs[index].emphasis && output->box_w > 0) {
        if (output->box_w == UINT16_MAX) {
            return false;
        }
        ++output->box_w;
        output->format = LV_FONT_GLYPH_FORMAT_A8;
    }
    return true;
}

static const void* ViewGlyphBitmap(lv_font_glyph_dsc_t* glyph, lv_draw_buf_t* output) {
    const auto index = static_cast<const UiFontSpec*>(glyph->resolved_font->dsc) - kUiFontSpecs;
    UiFontResourcePtr owner;
    const auto* source = FontSource(index, owner);
    auto original = *glyph;
    original.resolved_font = source;
    const bool expand = owner && owner->file[0] && kUiFontSpecs[index].emphasis && original.box_w > 0;
    if (expand) {
        --original.box_w;
    }
    const auto* bitmap = source->get_glyph_bitmap(&original, output);
    if (!bitmap || !expand || !output || bitmap != output || output->header.cf != LV_COLOR_FORMAT_A8) {
        return bitmap;
    }
    const unsigned width = output->header.w;
    const unsigned old_stride = output->header.stride;
    const unsigned stride = lv_draw_buf_width_to_stride(width + 1, LV_COLOR_FORMAT_A8);
    if (static_cast<size_t>(stride) * output->header.h > output->data_size) {
        return nullptr;
    }
    // 从末行向前扩展，避免对齐后的新步长覆盖尚未处理的像素。
    for (int y = output->header.h - 1; y >= 0; --y) {
        auto* row = output->data + y * stride;
        std::memmove(row, output->data + y * old_stride, width);
        row[width] = row[width - 1];
        for (unsigned x = width - 1; x > 0; --x) {
            row[x] = std::max(row[x], row[x - 1]);
        }
    }
    output->header.w = width + 1;
    output->header.stride = stride;
    lv_draw_buf_flush_cache(output, nullptr);
    return output;
}

static std::array<lv_font_t, kUiFontCount>& FontViews() {
    static auto views = [] {
        std::array<lv_font_t, kUiFontCount> result{};
        for (unsigned i = 0; i < result.size(); ++i) {
            result[i] = *BuiltinFonts()[i];
            result[i].dsc = &kUiFontSpecs[i];
            result[i].get_glyph_dsc = ViewGlyphMetrics;
            result[i].get_glyph_bitmap = ViewGlyphBitmap;
            result[i].static_bitmap = 0;
        }
        return result;
    }();
    return views;
}

static bool ValidFontFile(const char* file) {
    if (!file || std::strlen(file) >= sizeof(s_system_font) || std::strchr(file, '/') || std::strchr(file, '\\')) {
        return false;
    }
    const char* extension = std::strrchr(file, '.');
    return !file[0] || (extension && (strcasecmp(extension, ".ttf") == 0 || strcasecmp(extension, ".otf") == 0));
}

static void UpdateFontViews(const UiFontResourcePtr& resource) {
    auto& views = FontViews();
    for (unsigned i = 0; i < views.size(); ++i) {
        const bool caption = kUiFontSpecs[i].size == GetUiResourceProfile().ui_caption_font_size;
        const auto* selected = resource ? TtfRuntimeFont(resource->fonts[caption ? 1 : 0]) : nullptr;
        views[i].fallback = selected ? BuiltinFonts()[i] : BuiltinFonts()[i]->fallback;
    }
}

static bool PublishFont(const UiFontResourcePtr& resource, uint32_t revision, bool restore) {
    if (!resource) {
        return false;
    }
    auto* adapter = LVAdapterDisplay::Instance();
    if (!adapter) {
        ESP_LOGE("UiFont", "operation=publish result=ui_unavailable");
        return false;
    }
    DisplayLockGuard ui_lock(adapter);
    if (!ui_lock) {
        return false;
    }
    UiFontResourcePtr previous;
    bool applied = false;
    bool reused = false;
    bool changed = false;
    {
        std::lock_guard<std::mutex> lock(s_font_mutex);
        const bool available = !resource->file[0] ||
            (resource->generation == SdAccessMountGeneration() && !SdAccessIsQuiescing());
        if (available && (!restore || revision == s_revision)) {
            reused = s_current == resource;
            changed = !reused || s_use_builtin;
            if (!reused) {
                previous = std::move(s_current);
                s_current = resource;
                std::snprintf(s_system_font, sizeof(s_system_font), "%s", resource->file);
                std::snprintf(s_requested_font, sizeof(s_requested_font), "%s", resource->file);
                ++s_revision;
            }
            s_use_builtin = false;
            s_restore_failed = false;
            UpdateFontViews(resource);
            applied = true;
        }
    }
    if (applied) {
        s_restore_enabled.store(true, std::memory_order_release);
        // 字体视图地址保持稳定，显式通知原有控件重新测量文字及布局。
        if (changed) {
            lv_obj_report_style_change(nullptr);
            lv_obj_invalidate(lv_screen_active());
        }
        ESP_LOGI("UiFont", "operation=%s result=ready source=%s reused=%d", restore ? "restore" : "apply",
                 resource->file[0] ? "sd_ttf" : "builtin", reused ? 1 : 0);
        ScreenShowToast(std::strcmp(LangCode, "zh-CN") == 0 ? "系统字体应用成功"
                                                         : "System font applied successfully");
    }
    return applied;
}

static bool DropUnavailableFont(uint32_t revision, unsigned generation) {
    const auto status = SdAccessStatus();
    if ((status && status.stage != StorageStage::Mount) || SdAccessIsQuiescing() ||
        generation != SdAccessMountGeneration()) {
        return false;
    }
    auto* adapter = LVAdapterDisplay::Instance();
    if (!adapter) {
        return false;
    }
    DisplayLockGuard ui_lock(adapter);
    if (!ui_lock) {
        return false;
    }
    UiFontResourcePtr previous;
    {
        std::lock_guard<std::mutex> lock(s_font_mutex);
        if (revision != s_revision || !s_requested_font[0] ||
            (s_current && s_current->generation == generation)) {
            return false;
        }
        previous = std::move(s_current);
        s_system_font[0] = '\0';
        s_use_builtin = false;
        s_restore_failed = true;
        s_restore_failure_revision = revision;
        UpdateFontViews({});
    }
    lv_obj_report_style_change(nullptr);
    lv_obj_invalidate(lv_screen_active());
    ESP_LOGW("UiFont", "operation=restore result=builtin_fallback");
    return true;
}

static void RefreshFontSource(unsigned generation) {
    bool changed = false;
    bool failed = false;
    const bool paused = SdAccessIsQuiescing();
    {
        std::lock_guard<std::mutex> lock(s_font_mutex);
        const bool use_builtin = s_current && s_current->file[0] &&
            (paused || s_current->generation != generation) &&
            (!TtfRuntimeIsMemoryBacked(s_current->fonts[0]) ||
             !TtfRuntimeIsMemoryBacked(s_current->fonts[1]));
        changed = s_use_builtin != use_builtin;
        if (changed) {
            s_use_builtin = use_builtin;
            UpdateFontViews(use_builtin ? UiFontResourcePtr{} : s_current);
        }
        failed = s_restore_failed && s_restore_failure_revision == s_revision;
    }
    if (changed) {
        // 在本帧测量和绘制前统一切换，避免旧字体量得出字宽却读不到位图。
        lv_obj_report_style_change(nullptr);
        lv_obj_invalidate(lv_screen_active());
    }
    if (failed && !paused) {
        const bool chinese = std::strcmp(LangCode, "zh-CN") == 0;
        ScreenDialogAction actions[] = {{chinese ? "确定" : "OK", {}}};
        if (ScreenShowChoiceDialog(lv_screen_active(), chinese
                ? "系统字体恢复失败，已使用默认字体。请检查 SD 卡和字体文件。"
                : "Could not restore the system font. Using the default font. Check the SD card and font file.",
                actions, 1)) {
            std::lock_guard<std::mutex> lock(s_font_mutex);
            s_restore_failed = false;
        }
    }
}

const lv_font_t* UiFont(UiFontRole role) {
    const auto index = static_cast<unsigned>(role);
    return &FontViews()[index < kUiFontCount ? index : 0];
}

const lv_font_t* UiFontCompact(const lv_font_t* preferred) {
    for (auto role : {UiFontRole::Emphasis, UiFontRole::TitleEmphasis, UiFontRole::LargeEmphasis,
                     UiFontRole::CaptionEmphasis}) {
        if (preferred == UiFont(role)) {
            return UiFont(UiFontRole::CaptionEmphasis);
        }
    }
    return UiFont(UiFontRole::Caption);
}

UiFontResourcePtr UiFontLoadFile(const char* file) try {
    if (!ValidFontFile(file)) {
        return {};
    }
    UiFontResourcePtr candidate;
    const unsigned generation = SdAccessMountGeneration();
    {
        std::lock_guard<std::mutex> lock(s_font_mutex);
        if (s_current && std::strcmp(s_current->file, file) == 0) {
            if (!file[0] || s_current->generation == generation) {
                return s_current;
            }
            candidate = s_current;
        }
    }
    // 仅后台核对源字节；旧快照继续服务界面，不持有字体锁或 UI 锁等待 SD。
    if (candidate && TtfRuntimeIsMemoryBacked(candidate->fonts[0]) &&
        TtfRuntimeIsMemoryBacked(candidate->fonts[1]) &&
        TtfRuntimeSnapshotMatchesFile(candidate->fonts[0]) &&
        (candidate->fonts[1] == candidate->fonts[0] || TtfRuntimeSnapshotMatchesFile(candidate->fonts[1]))) {
        candidate->generation.store(generation, std::memory_order_release);
        return candidate;
    }
    auto resource = std::make_shared<UiFontResource>();
    std::snprintf(resource->file, sizeof(resource->file), "%s", file);
    if (!file[0]) {
        resource->preview = *BuiltinFonts()[0];
        return resource;
    }
    SdAccessTransaction transaction;
    if (!transaction) {
        return {};
    }
    resource->generation = SdAccessMountGeneration();
    char path[SD_FILE_PATH_CAPACITY];
    std::snprintf(path, sizeof(path), "%s/%s", SD_PATH_FONT_SRC, file);
    resource->fonts[0] = TtfRuntimeOpen(path, GetUiResourceProfile().ui_font_size, TtfRuntimeUse::SystemUi);
    if (!resource->fonts[0]) {
        return {};
    }
    resource->fonts[1] = GetUiResourceProfile().ui_font_size == GetUiResourceProfile().ui_caption_font_size
        ? resource->fonts[0] : TtfRuntimeOpen(path, GetUiResourceProfile().ui_caption_font_size, TtfRuntimeUse::SystemUi);
    if (!resource->fonts[1] || SdAccessIsQuiescing() || resource->generation != SdAccessMountGeneration()) {
        return {};
    }
    resource->preview = *TtfRuntimeFont(resource->fonts[0]);
    resource->preview.line_height = BuiltinFonts()[0]->line_height;
    resource->preview.base_line = BuiltinFonts()[0]->base_line;
    resource->preview.fallback = BuiltinFonts()[0];
    return resource;
} catch (const std::bad_alloc&) {
    ESP_LOGE("UiFont", "operation=load result=no_memory");
    return {};
}

const lv_font_t* UiFontPreview(const UiFontResourcePtr& resource) {
    return resource && (!resource->file[0] || resource->generation == SdAccessMountGeneration())
        ? &resource->preview : nullptr;
}

bool UiFontSetFile(const UiFontResourcePtr& resource) {
    return PublishFont(resource, 0, false);
}

const char* UiFontFile() {
    return s_system_font;
}

void UiFontRestore() try {
    Settings settings("display");
    const auto file = settings.GetString("system_font");
    if (!ValidFontFile(file.c_str())) {
        return;
    }
    {
        std::lock_guard<std::mutex> lock(s_font_mutex);
        std::snprintf(s_requested_font, sizeof(s_requested_font), "%s", file.c_str());
        ++s_revision;
    }
    s_restore_enabled.store(true, std::memory_order_release);
} catch (const std::bad_alloc&) {
    ESP_LOGE("UiFont", "operation=restore stage=preferences result=no_memory");
}

void UiFontPoll() try {
    if (!s_restore_enabled.load(std::memory_order_acquire)) {
        return;
    }
    static unsigned attempted_generation = UINT32_MAX;
    static uint32_t attempted_revision = UINT32_MAX;
    const unsigned generation = SdAccessMountGeneration();
    RefreshFontSource(generation);
    if (s_restore_interrupted.exchange(false, std::memory_order_acq_rel)) {
        attempted_generation = UINT32_MAX;
    }
    char file[SD_FILE_NAME_CAPACITY];
    uint32_t revision;
    {
        std::lock_guard<std::mutex> lock(s_font_mutex);
        if (!s_requested_font[0] || (s_current && std::strcmp(s_current->file, s_requested_font) == 0 &&
                                     s_current->generation == generation)) {
            return;
        }
        std::snprintf(file, sizeof(file), "%s", s_requested_font);
        revision = s_revision;
    }
    if (SdAccessIsQuiescing()) {
        return;
    }
    if (s_restore_busy.load(std::memory_order_acquire) || esp_timer_get_time() < s_restore_retry_at ||
        (attempted_generation == generation && attempted_revision == revision)) {
        return;
    }
    const auto status = SdAccessStatus();
    if (status) {
        if (status.stage == StorageStage::Mount) {
            if (DropUnavailableFont(revision, generation)) {
                attempted_generation = generation;
                attempted_revision = revision;
            }
        }
        return;
    }
    s_restore_busy.store(true, std::memory_order_release);
    if (!TaskExecutor::Schedule([file = std::string(file), revision, generation] {
            bool completed = false;
            try {
                auto resource = UiFontLoadFile(file.c_str());
                if (resource) {
                    completed = PublishFont(resource, revision, true);
                } else {
                    completed = DropUnavailableFont(revision, generation);
                }
            } catch (const std::bad_alloc&) {
                ESP_LOGE("UiFont", "operation=restore stage=publish result=no_memory");
                completed = DropUnavailableFont(revision, generation);
            }
            s_restore_interrupted.store(!completed, std::memory_order_release);
            s_restore_busy.store(false, std::memory_order_release);
        }, TaskDomain::Storage)) {
        s_restore_busy.store(false, std::memory_order_release);
        s_restore_retry_at = esp_timer_get_time() + 5000000;
        return;
    }
    attempted_generation = generation;
    attempted_revision = revision;
} catch (const std::bad_alloc&) {
    s_restore_busy.store(false, std::memory_order_release);
    s_restore_retry_at = esp_timer_get_time() + 5000000;
    ESP_LOGE("UiFont", "operation=restore stage=schedule result=no_memory");
}
