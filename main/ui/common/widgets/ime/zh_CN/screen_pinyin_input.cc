#include <algorithm>
#include <atomic>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <iterator>
#include <memory>
#include <mutex>
#include <new>
#include <string_view>
#include <vector>
#include <esp_heap_caps.h>
#include <esp_log.h>
#include "assets/lang_config.h"
#include "assets/lang_screen.h"
#include "boards/common/storage/sd_access.h"
#include "infra/diagnostics/firmware_memory.h"
#include "infra/storage/sd_paths.h"
#include "infra/ime/zh_CN/pinyin_composition.h"
#include "sysmgmt/tasks/task_executor.h"
#include "ui/common/drawing/adapter/epd_refresh.h"
#include "ui/common/drawing/font/ui_fonts.h"
#include "ui/common/drawing/ui_scale.h"
#include "ui/common/drawing/ui_theme.h"
#include "ui/common/runtime/haptic_feedback.h"
#include "ui/common/widgets/screen_common.h"
#include "ui/common/widgets/screen_keyboard.h"
#include "ui/common/widgets/ime/zh_CN/screen_pinyin_input.h"
#include "ui/common/widgets/screen_text.h"

static constexpr const char* TAG = "PinyinInput";
static constexpr size_t kVisibleCandidates = 16;  // 一页最多控件数，实际数量按完整词条宽度决定

struct PinyinLookupSession {
    std::mutex mutex;  // 保护请求串及已发布的候选块
    std::atomic<unsigned> generation{0};  // 快速输入合并为最新请求
    std::atomic<bool> cancelled{false};  // 页面关闭时撤销查询
    char key[PinyinDictionary::kMaxKeyBytes + 1] = {};  // 后台使用的小写查询键
    bool working = false;  // 同一会话最多一个后台任务
    unsigned completed = 0;  // 已发布的请求代次
    PinyinDictionary::Result status = PinyinDictionary::Result::Ready;  // 最近查询结果
    std::shared_ptr<char> payload;  // 有界 PSRAM 候选块，不加载整个字典
    size_t bytes = 0;  // 候选块有效字节数
    unsigned mount = SdAccessMountGeneration();  // 防止查询跨 SD 挂载代次
};

struct PinyinLookupCancel {
    PinyinLookupSession* session;  // 后台任务持有会话寿命
    unsigned generation;  // 本次查询代次
};

static bool PinyinLookupCancelled(void* context) {
    const auto* request = static_cast<PinyinLookupCancel*>(context);
    return request->session->cancelled.load() || request->generation != request->session->generation.load() ||
           SdAccessIsQuiescing();
}

static void RunPinyinLookup(const std::shared_ptr<PinyinLookupSession>& session) try {
    for (;;) {
        char key[PinyinDictionary::kMaxKeyBytes + 1];
        unsigned generation;
        {
            std::lock_guard<std::mutex> lock(session->mutex);
            if (session->cancelled.load()) {
                session->working = false;
                return;
            }
            std::strcpy(key, session->key);
            generation = session->generation.load();
        }
        auto status = PinyinDictionary::Result::Ready;
        const char* stage = "storage";
        int system_error = 0;
        int driver_error = 0;
        std::shared_ptr<char> payload;
        size_t bytes = 0;
        {
            SdAccessTransaction transaction;
            if (!transaction || session->mount != SdAccessMountGeneration()) {
                status = PinyinDictionary::Result::IoError;
                if (!transaction) {
                    system_error = transaction.Error().system_error;
                    driver_error = transaction.Error().platform_error;
                }
            } else {
                stage = "open";
                const std::unique_ptr<FILE, decltype(&std::fclose)> file(std::fopen(SD_PATH_PINYIN_DICT, "rb"), &std::fclose);
                if (!file) {
                    system_error = errno;
                    status = errno == ENOENT ? PinyinDictionary::Result::Missing : PinyinDictionary::Result::IoError;
                } else {
                    stage = "allocate";
                    char* buffer = static_cast<char*>(heap_caps_malloc(
                        !key[0] ? 1 : PinyinDictionary::kMaxCandidateBytes + 1, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
                    if (!buffer) {
                        status = PinyinDictionary::Result::IoError;
                    } else {
                        payload = std::shared_ptr<char>(buffer, &heap_caps_free);
                        PinyinLookupCancel cancel{session.get(), generation};
                        stage = "lookup";
                        errno = 0;
                        status = PinyinDictionary::Lookup(file.get(), key, buffer, bytes,
                                                          PinyinLookupCancelled, &cancel);
                        system_error = errno;
                    }
                }
            }
        }
        {
            std::lock_guard<std::mutex> lock(session->mutex);
            if (session->cancelled.load()) {
                session->working = false;
                return;
            }
            if (generation != session->generation.load()) {
                continue;
            }
            if (status != PinyinDictionary::Result::Ready && status != PinyinDictionary::Result::Cancelled &&
                status != session->status && !SdAccessIsQuiescing()) {
                ESP_LOGW(TAG, "operation=lookup_dictionary stage=%s result=%d errno=%d driver_error=%d; literal input retained",
                         stage, static_cast<int>(status), system_error, driver_error);
            }
            if (status == PinyinDictionary::Result::Ready && session->status != PinyinDictionary::Result::Ready) {
                ESP_LOGI(TAG, "operation=lookup_dictionary result=recovered");
            }
            session->payload = std::move(payload);
            session->bytes = bytes;
            session->status = status;
            session->completed = generation;
            session->working = false;
            return;
        }
    }
} catch (const std::bad_alloc&) {
    std::lock_guard<std::mutex> lock(session->mutex);
    session->completed = session->generation.load();
    session->status = PinyinDictionary::Result::IoError;
    session->working = false;
    ESP_LOGE(TAG, "operation=lookup_dictionary stage=allocate result=no_memory");
}

struct ScreenPinyinInput {
    lv_obj_t* root = nullptr;  // 包含候选顶部部件和原四行键盘
    lv_obj_t* textarea = nullptr;  // 原页面的输入框
    lv_obj_t* keyboard = nullptr;  // 系统键盘
    lv_obj_t* candidates = nullptr;  // 按需出现的键盘顶部部件
    lv_obj_t* words = nullptr;  // 不截断词条的候选行
    lv_obj_t* previous = nullptr;  // 上一组完整词条
    lv_obj_t* next = nullptr;  // 下一组完整词条
    lv_obj_t* buttons[kVisibleCandidates] = {};  // 重用候选触摸控件
    std::string_view visible[kVisibleCandidates];  // 当前已显示的完整词条
    std::vector<size_t> pages{0};  // 每组起始字节位置，返回上一组时保持边界
    size_t next_cursor = 0;  // 下一组起点
    lv_timer_t* poll = nullptr;  // 仅在有请求时轮询后台结果
    std::shared_ptr<PinyinLookupSession> lookup;  // 与后台任务共享取消状态
    std::shared_ptr<char> candidate_data;  // 持有当前界面引用的只读候选
    PinyinComposition composition;  // 与输入框内待选片段对应
    int32_t composition_start = 0;  // 待选片段起始字符位置
    bool chinese = true;  // 中英文候选模式
    bool uppercase = false;  // 英文大写状态
    bool backspace_composition = false;  // 长按退格不误删组合之前的文字
    bool ready = false;  // 候选对应当前组合串
    bool dictionary_warning = false;  // 同一输入会话只提示一次词库不可用
    unsigned received = 0;  // 最近消费的查询代次
    uint32_t changed_after_submit = 0;  // 拒绝尚未落墨的新候选被旧画面选中
    void (*confirm)() = nullptr;  // 执行搜索并允许关闭输入

    void Query();
    void Refresh();
    void Resize(lv_coord_t candidate_height);
    void UpdateLanguageKey();
    void SwitchLanguage();
    void Select(size_t index);
    bool Insert(const char* text);
    bool ReplaceComposition(const char* text);
    void Keyboard(ScreenKeyboardEvent event, const char* text);
    bool ViewReady() const;
    void CheckCursor();
    void FormatCandidate(std::string_view word, char (&text)[257]) const;
};

static void PinyinTextareaEvent(lv_event_t* event) {
    auto* input = static_cast<ScreenPinyinInput*>(lv_event_get_user_data(event));
    if (lv_event_get_code(event) == LV_EVENT_DELETE) {
        input->textarea = nullptr;
    } else if (lv_event_get_code(event) == LV_EVENT_CLICKED) {
        input->CheckCursor();
    }
}

static void PinyinContainer(lv_obj_t* object) {
    lv_obj_remove_style_all(object);
    lv_obj_remove_flag(object, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_width(object, lv_pct(100));
}

static lv_obj_t* PinyinButton(lv_obj_t* parent, const char* text, lv_event_cb_t callback, ScreenPinyinInput* input) {
    auto* button = lv_obj_create(parent);
    PinyinContainer(button);
    lv_obj_add_flag(button, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_width(button, LV_SIZE_CONTENT);
    lv_obj_set_height(button, lv_pct(100));
    auto* label = lv_label_create(button);
    lv_label_set_long_mode(label, LV_LABEL_LONG_WRAP);
    lv_label_set_text(label, text);
    lv_obj_remove_flag(label, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_flex_flow(button, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(button, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    HapticAttachClick(button);
    lv_obj_add_event_cb(button, callback, LV_EVENT_CLICKED, input);
    return button;
}

void ScreenPinyinInput::Resize(lv_coord_t candidate_height) {
    const lv_coord_t height = ScreenPinyinInputPreferredHeight() + candidate_height;
    if (lv_obj_get_height(root) != height) {
        lv_obj_set_height(root, height);
        lv_obj_align(root, LV_ALIGN_BOTTOM_MID, 0, 0);
    }
}

void ScreenPinyinInput::FormatCandidate(std::string_view word, char (&text)[257]) const {
    const size_t length = std::min(word.size(), sizeof(text) - 1);
    for (size_t i = 0; i < length; ++i) {
        const char c = word[i];
        text[i] = uppercase && c >= 'a' && c <= 'z' ? c - ('a' - 'A') : c;
    }
    text[length] = '\0';
}

void ScreenPinyinInput::Refresh() {
    std::fill(std::begin(visible), std::end(visible), std::string_view{});
    if (!ready || !composition.Letters()[0]) {
        lv_obj_add_flag(candidates, LV_OBJ_FLAG_HIDDEN);
        Resize(0);
        changed_after_submit = EpdGetStats().submitted;
        return;
    }
    bool changed = lv_obj_has_flag(candidates, LV_OBJ_FLAG_HIDDEN);
    lv_obj_remove_flag(candidates, LV_OBJ_FLAG_HIDDEN);
    const lv_font_t* font = UiFont();
    const lv_coord_t border = lv_obj_get_style_border_width(candidates, LV_PART_MAIN);
    lv_coord_t row_height = font->line_height + UiSy(12);
    if (changed) {
        lv_obj_set_height(candidates, row_height + border);
        Resize(row_height + border);
    }
    lv_obj_update_layout(root);
    const lv_coord_t budget = lv_obj_get_content_width(words);
    const lv_coord_t gap = ScreenMeasureTextWidth(font, "  ");
    if (lv_obj_get_style_pad_column(words, LV_PART_MAIN) != gap) {
        lv_obj_set_style_pad_column(words, gap, 0);
    }
    lv_coord_t used = 0;
    size_t visible_count = 0;
    size_t cursor = pages.back();
    for (size_t i = 0; i < kVisibleCandidates; ++i) {
        const size_t start = cursor;
        const auto word = composition.NextCandidate(cursor, chinese);
        if (word.empty()) {
            break;
        }
        char text[257];
        FormatCandidate(word, text);
        const lv_font_t* item_font = font;
        lv_coord_t width = ScreenMeasureTextWidth(item_font, text);
        if (i == 0 && width > budget) {
            item_font = UiFont(UiFontRole::Caption);
            width = ScreenMeasureTextWidth(item_font, text);
        }
        if (i > 0 && used + gap + width > budget) {
            cursor = start;
            break;
        }
        width = std::max<lv_coord_t>(1, std::min(width, budget));
        lv_point_t size;
        lv_text_get_size(&size, text, item_font, 0, 0, width, LV_TEXT_FLAG_NONE);
        const lv_coord_t height = size.y + UiSy(12);
        row_height = std::max(row_height, height);
        auto* label = lv_obj_get_child(buttons[i], 0);
        const bool text_changed = std::strcmp(lv_label_get_text(label), text) != 0;
        changed |= text_changed || lv_obj_has_flag(buttons[i], LV_OBJ_FLAG_HIDDEN) ||
                   lv_obj_get_style_text_font(label, LV_PART_MAIN) != item_font || lv_obj_get_width(buttons[i]) != width ||
                   lv_obj_get_height(buttons[i]) != height;
        if (lv_obj_get_style_text_font(label, LV_PART_MAIN) != item_font) {
            lv_obj_set_style_text_font(label, item_font, 0);
        }
        lv_obj_set_width(label, width);
        if (text_changed) {
            lv_label_set_text(label, text);
        }
        lv_obj_set_size(buttons[i], width, height);
        lv_obj_remove_flag(buttons[i], LV_OBJ_FLAG_HIDDEN);
        visible[i] = word;
        ++visible_count;
        used += (i ? gap : 0) + width;
    }
    for (size_t i = visible_count; i < kVisibleCandidates; ++i) {
        changed |= !lv_obj_has_flag(buttons[i], LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(buttons[i], LV_OBJ_FLAG_HIDDEN);
    }
    next_cursor = cursor;
    size_t test = cursor;
    const bool disable_previous = pages.size() <= 1;
    const bool disable_next = composition.NextCandidate(test, chinese).empty();
    changed |= lv_obj_has_state(previous, LV_STATE_DISABLED) != disable_previous ||
               lv_obj_has_state(next, LV_STATE_DISABLED) != disable_next ||
               lv_obj_get_height(candidates) != row_height + border;
    lv_obj_set_state(previous, LV_STATE_DISABLED, disable_previous);
    lv_obj_set_state(next, LV_STATE_DISABLED, disable_next);
    lv_obj_set_height(candidates, row_height + border);
    Resize(row_height + border);
    if (changed) {
        changed_after_submit = EpdGetStats().submitted;
    }
}

void ScreenPinyinInput::Query() {
    ready = false;
    pages.assign(1, 0);
    if (!composition.Letters()[0]) {
        Refresh();
    }
    {
        std::lock_guard<std::mutex> lock(lookup->mutex);
        std::strcpy(lookup->key, composition.Letters());
        for (char* c = lookup->key; *c; ++c) {
            if (*c >= 'A' && *c <= 'Z') {
                *c += 'a' - 'A';
            }
        }
        ++lookup->generation;
        if (lookup->working) {
            lv_timer_resume(poll);
            return;
        }
        lookup->working = true;
    }
    const auto session = lookup;
    bool queued = false;
    try {
        queued = TaskExecutor::Schedule([session]() { RunPinyinLookup(session); }, TaskDomain::Storage);
    } catch (const std::bad_alloc&) {
        queued = false;
    }
    if (!queued) {
        std::lock_guard<std::mutex> lock(lookup->mutex);
        lookup->working = false;
        lookup->status = PinyinDictionary::Result::IoError;
        lookup->completed = lookup->generation.load();
        ESP_LOGE(TAG, "operation=lookup_dictionary stage=schedule result=unavailable");
    }
    lv_timer_resume(poll);
}

bool ScreenPinyinInput::ViewReady() const {
    return ready && static_cast<int32_t>(EpdGetStats().completed - changed_after_submit) > 0;
}

void ScreenPinyinInput::CheckCursor() {
    if (composition.Letters()[0] && lv_textarea_get_cursor_pos(textarea) !=
        composition_start + static_cast<int32_t>(std::strlen(composition.Letters()))) {
        composition.Clear();
        Query();
    }
}

static uint32_t PinyinTextLength(const char* text) {
    uint32_t length = 0;
    for (const auto* p = reinterpret_cast<const unsigned char*>(text); *p; ++p) {
        length += (*p & 0xc0) != 0x80;
    }
    return length;
}

bool ScreenPinyinInput::Insert(const char* text) {
    const uint32_t count = PinyinTextLength(lv_textarea_get_text(textarea));
    const uint32_t added = PinyinTextLength(text);
    const uint32_t limit = lv_textarea_get_max_length(textarea);
    if (limit && (count > limit || added > limit - count)) {
        ScreenShowToast(LangString_PINYIN_INPUT_FULL);
        return false;
    }
    const size_t size = std::strlen(lv_textarea_get_text(textarea));
    lv_textarea_add_text(textarea, text);
    if (std::strlen(lv_textarea_get_text(textarea)) != size + std::strlen(text)) {
        ESP_LOGE(TAG, "operation=insert stage=textarea result=rejected");
        ScreenShowToast(LangString_COMMON_MEMORY_RETRY);
        return false;
    }
    return true;
}

bool ScreenPinyinInput::ReplaceComposition(const char* text) {
    const char* current = lv_textarea_get_text(textarea);
    const uint32_t letters = std::strlen(composition.Letters());
    const uint32_t count = PinyinTextLength(current);
    const uint32_t limit = lv_textarea_get_max_length(textarea);
    if (count < letters || (limit && count - letters + PinyinTextLength(text) > limit)) {
        ScreenShowToast(LangString_PINYIN_INPUT_FULL);
        return false;
    }
    char backup[321];
    if (std::strlen(current) >= sizeof(backup)) {
        ESP_LOGE(TAG, "operation=replace_composition result=input_bound_exceeded");
        return false;
    }
    std::memcpy(backup, current, std::strlen(current) + 1);
    const int32_t cursor = lv_textarea_get_cursor_pos(textarea);
    for (uint32_t i = 0; i < letters; ++i) {
        lv_textarea_delete_char(textarea);
    }
    if (!Insert(text)) {
        lv_textarea_set_text(textarea, backup);
        lv_textarea_set_cursor_pos(textarea, cursor);
        return false;
    }
    composition.Clear();
    ready = false;
    Refresh();
    return true;
}

void ScreenPinyinInput::Select(size_t index) {
    if (!ViewReady() || index >= kVisibleCandidates || visible[index].empty()) {
        return;
    }
    CheckCursor();
    if (!composition.Letters()[0]) {
        return;
    }
    char text[257];
    FormatCandidate(visible[index], text);
    ReplaceComposition(text);
}

void ScreenPinyinInput::UpdateLanguageKey() {
    ScreenKeyboardSetLanguageKey(keyboard, chinese ? LangString_PINYIN_MODE_KEY : (uppercase ? "ABC" : "abc"),
                                 uppercase, [](void* context) {
        static_cast<ScreenPinyinInput*>(context)->SwitchLanguage();
    });
}

void ScreenPinyinInput::SwitchLanguage() {
    if (static_cast<int32_t>(EpdGetStats().completed - changed_after_submit) <= 0) {
        return;
    }
    composition.Clear();
    if (chinese) {
        chinese = false;
        uppercase = false;
    } else if (!uppercase) {
        uppercase = true;
    } else {
        chinese = true;
        uppercase = false;
    }
    Query();
    UpdateLanguageKey();
}

void ScreenPinyinInput::Keyboard(ScreenKeyboardEvent event, const char* text) {
    if (event == ScreenKeyboardEvent::kLayout) {
        return;
    }
    CheckCursor();
    const bool pending = composition.Letters()[0] != '\0';
    if (event == ScreenKeyboardEvent::kBackspace) {
        backspace_composition = pending;
        lv_textarea_delete_char(textarea);
        if (pending) {
            composition.Backspace();
            Query();
        }
    } else if (event == ScreenKeyboardEvent::kClear) {
        if (pending || backspace_composition) {
            for (size_t i = 0; i < std::strlen(composition.Letters()); ++i) {
                lv_textarea_delete_char(textarea);
            }
        } else {
            lv_textarea_set_text(textarea, "");
        }
        composition.Clear();
        Query();
    } else if (event == ScreenKeyboardEvent::kConfirm) {
        if (pending && !ViewReady()) {
            return;
        }
        if (pending && chinese && !visible[0].empty()) {
            char candidate[257];
            FormatCandidate(visible[0], candidate);
            if (!ReplaceComposition(candidate)) {
                return;
            }
        }
        confirm();
    } else if (event == ScreenKeyboardEvent::kInsert) {
        const bool letter = text[0] && !text[1] &&
            ((text[0] >= 'a' && text[0] <= 'z') || (text[0] >= 'A' && text[0] <= 'Z'));
        if (letter) {
            if (std::strlen(composition.Letters()) >= PinyinComposition::kMaxLetters) {
                ScreenShowToast(LangString_PINYIN_TOO_LONG);
                return;
            }
            const int32_t start = lv_textarea_get_cursor_pos(textarea);
            if (!Insert(text)) {
                return;
            }
            if (!pending) {
                composition_start = start;
            }
            composition.TypeLetter(text[0]);
            Query();
        } else if (pending && chinese && std::strcmp(text, " ") == 0) {
            Select(0);
        } else {
            composition.Clear();
            Refresh();
            Insert(text);
        }
    }
}

lv_obj_t* ScreenPinyinInputCreate(lv_obj_t* parent, lv_obj_t* textarea, void (*confirm)()) try {
    const size_t state_bytes = sizeof(ScreenPinyinInput) + sizeof(PinyinLookupSession) + sizeof(size_t);
    if (heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT) < kFirmwareInternalFloorBytes + state_bytes) {
        ESP_LOGW(TAG, "operation=create stage=budget result=unavailable");
        return nullptr;
    }
    auto state = std::make_unique<ScreenPinyinInput>();
    state->lookup = std::make_shared<PinyinLookupSession>();
    state->textarea = textarea;
    state->confirm = confirm;
    auto* root = lv_obj_create(parent);
    std::unique_ptr<lv_obj_t, decltype(&lv_obj_delete)> guard(root, &lv_obj_delete);
    state->root = root;
    PinyinContainer(root);
    lv_obj_set_flex_flow(root, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_column(root, 0, 0);
    lv_obj_set_style_pad_row(root, 0, 0);
    lv_obj_set_style_pad_left(root, UiSx(12), 0);
    lv_obj_set_style_pad_right(root, UiSx(12), 0);
    lv_obj_set_style_pad_top(root, UiSy(8), 0);
    lv_obj_set_style_pad_bottom(root, UiSy(12), 0);
    lv_obj_set_style_text_font(root, UiFont(), 0);
    lv_obj_set_style_text_color(root, UiColorForeground(), 0);
    lv_obj_set_style_bg_color(root, UiColorBackground(), 0);
    lv_obj_set_style_bg_opa(root, LV_OPA_COVER, 0);
    auto* input = state.release();
    lv_obj_set_user_data(root, input);
    lv_obj_add_event_cb(root, [](lv_event_t* event) {
        auto* input = static_cast<ScreenPinyinInput*>(lv_event_get_user_data(event));
        if (lv_event_get_target_obj(event) != input->root) {
            return;
        }
        input->lookup->cancelled.store(true);
        ++input->lookup->generation;
        if (input->textarea) {
            lv_obj_remove_event_cb_with_user_data(input->textarea, PinyinTextareaEvent, input);
        }
        if (input->poll) {
            lv_timer_delete(input->poll);
        }
        delete input;
    }, LV_EVENT_DELETE, input);
    lv_obj_add_event_cb(textarea, PinyinTextareaEvent, LV_EVENT_ALL, input);
    input->candidates = lv_obj_create(root);
    PinyinContainer(input->candidates);
    lv_obj_set_style_border_side(input->candidates, LV_BORDER_SIDE_TOP, 0);
    lv_obj_set_style_border_width(input->candidates, 1, 0);
    lv_obj_set_style_border_color(input->candidates, UiColorForeground(), 0);
    lv_obj_set_flex_flow(input->candidates, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(input->candidates, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(input->candidates, UiSx(10), 0);
    const auto page = [](lv_event_t* event) {
        auto* input = static_cast<ScreenPinyinInput*>(lv_event_get_user_data(event));
        if (!input->ViewReady()) {
            return;
        }
        if (lv_event_get_target(event) == input->previous) {
            if (input->pages.size() > 1) {
                input->pages.pop_back();
            }
        } else {
            if (input->pages.size() == input->pages.capacity() &&
                heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT) <
                    kFirmwareInternalFloorBytes + input->pages.capacity() * sizeof(size_t) * 2) {
                ESP_LOGW(TAG, "operation=candidate_page stage=budget result=unavailable");
                ScreenShowToast(LangString_COMMON_MEMORY_RETRY);
                return;
            }
            try {
                input->pages.push_back(input->next_cursor);
            } catch (const std::bad_alloc&) {
                ESP_LOGW(TAG, "operation=candidate_page stage=allocate result=no_memory");
                ScreenShowToast(LangString_COMMON_MEMORY_RETRY);
                return;
            }
        }
        input->Refresh();
    };
    input->previous = PinyinButton(input->candidates, "<", page, input);
    lv_obj_set_width(input->previous, UiSx(28));
    input->words = lv_obj_create(input->candidates);
    PinyinContainer(input->words);
    lv_obj_set_width(input->words, 0);
    lv_obj_set_flex_grow(input->words, 1);
    lv_obj_set_height(input->words, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(input->words, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(input->words, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    for (size_t i = 0; i < kVisibleCandidates; ++i) {
        input->buttons[i] = PinyinButton(input->words, "", [](lv_event_t* event) {
            auto* input = static_cast<ScreenPinyinInput*>(lv_event_get_user_data(event));
            input->Select(reinterpret_cast<uintptr_t>(lv_obj_get_user_data(lv_event_get_target_obj(event))));
        }, input);
        lv_obj_set_user_data(input->buttons[i], reinterpret_cast<void*>(i));
    }
    input->next = PinyinButton(input->candidates, ">", page, input);
    lv_obj_set_width(input->next, UiSx(28));
    input->keyboard = ScreenKeyboardCreate(root, [](ScreenKeyboardEvent event, const char* text, void* context) {
        static_cast<ScreenPinyinInput*>(context)->Keyboard(event, text);
    }, input);
    if (!input->keyboard) {
        ESP_LOGE(TAG, "operation=create stage=keyboard result=failed");
        return nullptr;
    }
    input->UpdateLanguageKey();
    input->poll = lv_timer_create([](lv_timer_t* timer) {
        auto* input = static_cast<ScreenPinyinInput*>(lv_timer_get_user_data(timer));
        std::shared_ptr<char> data;
        size_t bytes;
        PinyinDictionary::Result status;
        {
            std::lock_guard<std::mutex> lock(input->lookup->mutex);
            const unsigned generation = input->lookup->generation.load();
            if (input->lookup->completed != generation || input->received == generation) {
                return;
            }
            input->received = generation;
            data = input->lookup->payload;
            bytes = input->lookup->bytes;
            status = input->lookup->status;
        }
        const bool valid = status == PinyinDictionary::Result::Ready &&
                           input->composition.SetCandidates({data ? data.get() : "", bytes});
        if (!valid) {
            if (status == PinyinDictionary::Result::Ready && !input->dictionary_warning) {
                ESP_LOGE(TAG, "operation=lookup_dictionary stage=validate result=invalid");
            }
            input->composition.SetCandidates({});
            if (!input->dictionary_warning && !SdAccessIsQuiescing()) {
                input->dictionary_warning = true;
                ScreenShowToast(LangString_PINYIN_DICT_UNAVAILABLE);
            }
        }
        input->candidate_data = std::move(data);
        input->ready = true;
        input->Refresh();
        lv_timer_pause(timer);
    }, 50, input);
    if (!input->poll) {
        ESP_LOGE(TAG, "operation=create stage=timer result=failed");
        return nullptr;
    }
    input->Query();
    guard.release();
    return root;
} catch (const std::bad_alloc&) {
    ESP_LOGE(TAG, "operation=create stage=allocate result=no_memory");
    return nullptr;
}

lv_coord_t ScreenPinyinInputPreferredHeight() {
    return ScreenKeyboardPreferredHeight() + UiSy(8) + UiSy(12);
}

void ScreenPinyinInputClear(lv_obj_t* keyboard) {
    auto* input = static_cast<ScreenPinyinInput*>(lv_obj_get_user_data(keyboard));
    input->composition.Clear();
    input->backspace_composition = false;
    lv_textarea_set_text(input->textarea, "");
    input->Query();
}
