#include "book_reader_emulator.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <memory>
#include <string>

#include "font_emulator.h"
#include "epdfont.h"
#include "book_session.h"
#include "ebook_document.h"
#include "reader_page_render.h"
#include "reader_types.h"

namespace {

constexpr lv_coord_t kListPad = 12;
constexpr lv_coord_t kFooterH = 36;
constexpr lv_coord_t kStatusPadV = 8;

struct ReaderUi {
    std::unique_ptr<reader::BookSession> session;
    epdfont_t* epdfont = nullptr;
    std::string epdfont_path;
    std::string last_error;
    std::string temp_ebook_path;
    std::string open_ebook_path;

    lv_obj_t* scr = nullptr;
    lv_obj_t* content = nullptr;
    lv_obj_t* footer = nullptr;
    lv_coord_t status_h = 0;
};

static bool s_ui_ready = false;

ReaderUi& Ui() {
    static ReaderUi s;
    return s;
}

void DisableScroll(lv_obj_t* obj) {
    if (obj == nullptr) {
        return;
    }
    lv_obj_remove_flag(obj, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scrollbar_mode(obj, LV_SCROLLBAR_MODE_OFF);
}

void EnsureEpdfont() {
    auto& ui = Ui();
    if (ui.epdfont != nullptr || ui.epdfont_path.empty()) {
        return;
    }
    ui.epdfont = epdfont_open(ui.epdfont_path.c_str());
}

void ReleaseEpdfont() {
    auto& ui = Ui();
    if (ui.epdfont == nullptr) {
        return;
    }
    epdfont_close(ui.epdfont);
    ui.epdfont = nullptr;
}

const lv_font_t* UiFont() {
    EnsureEpdfont();
    return epdfont_get_lv_font(Ui().epdfont);
}

const lv_font_t* ListFont() {
    return UiFont();
}

const lv_font_t* BookFont() {
    return UiFont();
}

void PrewarmPageFont(const reader::Page* page) {
    auto& ui = Ui();
    if (ui.epdfont == nullptr || page == nullptr) {
        return;
    }
    std::string blob;
    blob.reserve(2048);
    for (const auto& item : page->items) {
        if (item.kind == reader::ContentKind::kText) {
            blob.append(item.text);
            blob.push_back('\n');
        }
    }
    if (!blob.empty()) {
        epdfont_prewarm_utf8(ui.epdfont, blob.c_str(), blob.size());
    }
}

lv_coord_t ContentWidth() { return EPD_HOR_RES - kListPad * 2; }

lv_coord_t StatusBarHeight() {
    const lv_font_t* ui_font = UiFont();
    const lv_coord_t line_h = ui_font != nullptr ? ui_font->line_height : 30;
    return line_h + kStatusPadV * 2;
}

lv_coord_t MeasureTextWidth(const lv_font_t* font, const char* text) {
    if (font == nullptr || text == nullptr) {
        return 0;
    }
    lv_coord_t w = 0;
    for (const unsigned char* p = reinterpret_cast<const unsigned char*>(text); *p; ) {
        uint32_t cp = 0;
        if ((*p & 0x80) == 0) {
            cp = *p++;
        } else if ((*p & 0xE0) == 0xC0 && p[1]) {
            cp = ((p[0] & 0x1F) << 6) | (p[1] & 0x3F);
            p += 2;
        } else if ((*p & 0xF0) == 0xE0 && p[1] && p[2]) {
            cp = ((p[0] & 0x0F) << 12) | ((p[1] & 0x3F) << 6) | (p[2] & 0x3F);
            p += 3;
        } else {
            cp = *p++;
        }
        w += lv_font_get_glyph_width(font, cp, 0);
    }
    return w;
}

std::string TruncateTextToWidth(std::string text, const lv_font_t* font, lv_coord_t max_w) {
    if (max_w <= 0 || MeasureTextWidth(font, text.c_str()) <= max_w) {
        return text;
    }
    while (!text.empty()) {
        text.pop_back();
        if (MeasureTextWidth(font, (text + "…").c_str()) <= max_w) {
            return text + "…";
        }
    }
    return "…";
}

void UpdateFooter() {
    auto& ui = Ui();
    if (ui.footer == nullptr || !ui.session || !ui.session->IsOpen()) {
        return;
    }
    char buf[96];
    const lv_font_t* footer_font = ListFont();
    const lv_coord_t footer_w = EPD_HOR_RES - 16;
    const int pct_x10 = ui.session->ReadingProgressX10();
    char suffix[24];
    std::snprintf(suffix, sizeof(suffix), " · %d.%d%%", pct_x10 / 10, pct_x10 % 10);
    const lv_coord_t suffix_w = MeasureTextWidth(footer_font, suffix);
    const lv_coord_t title_budget = std::max<lv_coord_t>(0, footer_w - suffix_w);

    std::string title = ui.session->CurrentTocTitle();
    if (title.empty()) {
        title = "正文";
    }
    title = TruncateTextToWidth(title, footer_font, title_budget);
    std::snprintf(buf, sizeof(buf), "%s%s", title.c_str(), suffix);
    lv_label_set_text(ui.footer, buf);
}

void RenderPage() {
    auto& ui = Ui();
    if (ui.content == nullptr || !ui.session) {
        return;
    }
    lv_obj_clean(ui.content);
    DisableScroll(ui.content);

    const reader::Page* page = ui.session->CurrentPageData();
    if (page == nullptr) {
        lv_obj_t* msg = lv_label_create(ui.content);
        lv_label_set_text(msg, "无法加载页面");
        return;
    }

    PrewarmPageFont(page);

    const bool page_images = ui.session->IsPageImagesMode();
    const lv_coord_t max_w = page_images ? EPD_HOR_RES : ContentWidth();
    const lv_coord_t content_h = lv_obj_get_height(ui.content);
    lv_obj_t* col = lv_obj_create(ui.content);
    lv_obj_remove_style_all(col);
    lv_obj_set_size(col, max_w, page_images ? content_h : LV_SIZE_CONTENT);
    lv_obj_set_style_pad_all(col, 0, 0);
    lv_obj_set_flex_flow(col, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(col, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    lv_obj_set_style_pad_row(col, page_images ? 0 : reader::kReaderLineGapDefault, 0);
    lv_obj_align(col, page_images ? LV_ALIGN_TOP_LEFT : LV_ALIGN_TOP_MID, 0, 0);
    lv_obj_clear_flag(col, LV_OBJ_FLAG_CLICKABLE);
    DisableScroll(col);

    for (const auto& item : page->items) {
        if (item.kind == reader::ContentKind::kText) {
            const lv_font_t* book_font = BookFont();
            if (item.text.empty()) {
                continue;
            }
            reader::CreatePageTextLabel(col, item, max_w, book_font, reader::kReaderLineGapDefault,
                                       reader::kReaderParaGapDefault);
        } else {
            reader::RasterImage img;
            const lv_coord_t max_h = page_images ? content_h : (content_h * 3 / 4);
            if (ui.session->LoadPageImage(item.image_href, max_w, max_h > 40 ? max_h : 200, img) &&
                !img.empty()) {
                auto* heap_img = new reader::RasterImage(std::move(img));
                heap_img->BindDsc();
                lv_obj_t* image = lv_image_create(col);
                lv_image_set_src(image, &heap_img->dsc);
                lv_obj_set_user_data(image, heap_img);
                lv_obj_add_event_cb(
                    image,
                    [](lv_event_t* ev) {
                        auto* p = static_cast<reader::RasterImage*>(lv_obj_get_user_data(
                            static_cast<lv_obj_t*>(lv_event_get_target(ev))));
                        delete p;
                    },
                    LV_EVENT_DELETE, nullptr);
                lv_obj_clear_flag(image, LV_OBJ_FLAG_CLICKABLE);
                if (page_images) {
                    lv_obj_align(image, LV_ALIGN_TOP_LEFT, 0, 0);
                }
            } else {
                lv_obj_t* label = lv_label_create(col);
                lv_label_set_text(label, "[图片]");
                lv_obj_set_style_text_font(label, BookFont(), 0);
            }
        }
    }
    UpdateFooter();
}

void OnContentClicked(lv_event_t* e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) {
        return;
    }
    auto& ui = Ui();
    if (!ui.session || !ui.session->IsOpen()) {
        return;
    }
    if (ui.session->NextPage()) {
        RenderPage();
    }
}

void BuildScreen() {
    auto& ui = Ui();
    if (ui.scr != nullptr && lv_obj_is_valid(ui.scr)) {
        lv_obj_delete(ui.scr);
    }
    ui.scr = lv_obj_create(nullptr);
    lv_obj_set_style_bg_color(ui.scr, lv_color_hex(0xEFEBE0), 0);
    lv_obj_set_style_bg_opa(ui.scr, LV_OPA_COVER, 0);
    lv_obj_set_style_text_font(ui.scr, ListFont(), 0);
    lv_obj_set_style_text_color(ui.scr, lv_color_black(), 0);
    DisableScroll(ui.scr);

    ui.status_h = StatusBarHeight();
    lv_obj_t* status = lv_obj_create(ui.scr);
    lv_obj_set_size(status, EPD_HOR_RES, ui.status_h);
    lv_obj_set_style_bg_opa(status, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(status, 0, 0);
    lv_obj_align(status, LV_ALIGN_TOP_MID, 0, 0);
    DisableScroll(status);

    const lv_coord_t body_h = EPD_VER_RES - ui.status_h - kFooterH;
    const bool page_images = ui.session && ui.session->IsPageImagesMode();
    lv_obj_t* body = lv_obj_create(ui.scr);
    lv_obj_remove_style_all(body);
    lv_obj_set_size(body, EPD_HOR_RES, body_h);
    lv_obj_align(body, LV_ALIGN_TOP_MID, 0, ui.status_h);
    lv_obj_set_style_pad_hor(body, page_images ? 0 : kListPad, 0);
    lv_obj_set_style_bg_opa(body, LV_OPA_TRANSP, 0);
    DisableScroll(body);
    lv_obj_add_event_cb(body, OnContentClicked, LV_EVENT_CLICKED, nullptr);
    ui.content = body;

    ui.footer = lv_label_create(ui.scr);
    lv_obj_set_width(ui.footer, EPD_HOR_RES - 16);
    lv_obj_set_style_text_align(ui.footer, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_font(ui.footer, ListFont(), 0);
    lv_obj_set_style_text_color(ui.footer, lv_color_black(), 0);
    lv_label_set_long_mode(ui.footer, LV_LABEL_LONG_CLIP);
    lv_obj_align(ui.footer, LV_ALIGN_BOTTOM_MID, 0, -4);
    DisableScroll(ui.footer);

    lv_screen_load(ui.scr);
}

void ShowIdleScreen() {
    auto& ui = Ui();
    if (ui.session && ui.session->IsOpen()) {
        return;
    }
    if (ui.scr != nullptr && lv_obj_is_valid(ui.scr)) {
        lv_obj_delete(ui.scr);
    }
    ui.content = nullptr;
    ui.footer = nullptr;
    ui.scr = lv_obj_create(nullptr);
    lv_obj_set_style_bg_color(ui.scr, lv_color_hex(0xEFEBE0), 0);
    lv_obj_set_style_bg_opa(ui.scr, LV_OPA_COVER, 0);
    DisableScroll(ui.scr);

    const lv_font_t* font = UiFont();
    lv_obj_t* lbl = lv_label_create(ui.scr);
    lv_label_set_text(lbl, "墨水屏模拟器\n480 × 800\n\n从书库打开 .ebook 预览");
    if (font != nullptr) {
        lv_obj_set_style_text_font(lbl, font, 0);
    }
    lv_obj_set_style_text_color(lbl, lv_color_hex(0x111111), 0);
    lv_obj_set_style_text_align(lbl, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_center(lbl);
    lv_screen_load(ui.scr);
}

bool OpenSessionPath(const char* path) {
    auto& ui = Ui();
    ui.last_error.clear();
    if (path == nullptr || path[0] == '\0') {
        ui.last_error = "空路径";
        return false;
    }
    ui.open_ebook_path = path;

    EnsureEpdfont();
    ui.session = std::make_unique<reader::BookSession>();
    ui.session->SetFont(BookFont());
    ui.session->SetGaps(reader::kReaderLineGapDefault, reader::kReaderParaGapDefault);

    const lv_coord_t body_h = EPD_VER_RES - StatusBarHeight() - kFooterH;
    const bool page_images = reader::EbookDocument::PeekPageImages(path);
    if (page_images) {
        ui.session->SetViewport(EPD_HOR_RES, body_h);
    } else {
        ui.session->SetViewport(ContentWidth(), body_h - 8);
    }

    reader::BookInfo info;
    info.path = path;
    info.format = reader::BookFormat::kEbook;
    info.title = path;

    if (!ui.session->Open(info)) {
        ui.last_error = "打开 .ebook 失败";
        ui.session.reset();
        return false;
    }

    BuildScreen();
    RenderPage();
    return true;
}

}  // namespace

extern "C" {

void book_reader_init(void) {
    s_ui_ready = true;
    EnsureEpdfont();
    ShowIdleScreen();
}

void book_reader_set_fontpack_path(const char* /*path*/) {
    /* 模拟器/Web 仅使用 .ef，不再加载 fontpack */
}

void book_reader_set_epdfont_path(const char* path) {
    auto& ui = Ui();
    const bool had_book = ui.session && ui.session->IsOpen();
    int saved_toc = -1;
    int saved_page = 0;
    std::string reopen_path;
    if (had_book) {
        saved_toc = ui.session->CurrentTocIndex();
        saved_page = ui.session->CurrentPage();
        reopen_path = ui.open_ebook_path;
    }

    ReleaseEpdfont();
    ui.epdfont_path = path != nullptr ? path : "";
    epdfont_emulator_set_path(path);
    if (!s_ui_ready) {
        return;
    }

    // 换字体须重新打开并分页，否则 viewport/页表仍是旧字模，正文会压住底栏
    if (had_book && !reopen_path.empty()) {
        EnsureEpdfont();
        if (!OpenSessionPath(reopen_path.c_str())) {
            ShowIdleScreen();
            return;
        }
        if (saved_toc >= 0) {
            ui.session->GoToToc(saved_toc);
        }
        if (saved_page > 0) {
            ui.session->GoToPage(saved_page);
        }
        RenderPage();
        return;
    }

    if (ui.session && ui.session->IsOpen()) {
        EnsureEpdfont();
        ui.session->SetFont(BookFont());
        BuildScreen();
        RenderPage();
    } else {
        ShowIdleScreen();
    }
}

int book_reader_open_ebook_path(const char* path) {
    return OpenSessionPath(path) ? 1 : 0;
}

int book_reader_open_ebook_mem(const uint8_t* data, size_t len) {
    auto& ui = Ui();
    if (data == nullptr || len == 0) {
        ui.last_error = "空数据";
        return 0;
    }
    char tmp_path[64];
    std::snprintf(tmp_path, sizeof(tmp_path), "/tmp/ebook_%p.ebook", static_cast<const void*>(data));
#ifdef __EMSCRIPTEN__
    std::snprintf(tmp_path, sizeof(tmp_path), "/ebook_open.ebook");
#endif
    FILE* fp = fopen(tmp_path, "wb");
    if (fp == nullptr) {
        ui.last_error = "无法写入临时文件";
        return 0;
    }
    if (fwrite(data, 1, len, fp) != len) {
        fclose(fp);
        ui.last_error = "写入临时文件失败";
        return 0;
    }
    fclose(fp);
    ui.temp_ebook_path = tmp_path;
    return OpenSessionPath(tmp_path) ? 1 : 0;
}

void book_reader_next_page(void) {
    auto& ui = Ui();
    if (ui.session && ui.session->NextPage()) {
        RenderPage();
    }
}

void book_reader_prev_page(void) {
    auto& ui = Ui();
    if (ui.session && ui.session->PrevPage()) {
        RenderPage();
    }
}

void book_reader_go_toc(int index) {
    auto& ui = Ui();
    if (ui.session && ui.session->GoToToc(index)) {
        RenderPage();
    }
}

int book_reader_toc_count(void) {
    auto& ui = Ui();
    return ui.session ? ui.session->TocCount() : 0;
}

const char* book_reader_toc_title(int index) {
    static std::string s_toc_title;
    auto& ui = Ui();
    s_toc_title.clear();
    if (!ui.session || index < 0) {
        return "";
    }
    const reader::TocEntry* ent = ui.session->TocAt(index);
    if (ent == nullptr) {
        return "";
    }
    s_toc_title = ent->title;
    return s_toc_title.c_str();
}

int book_reader_current_toc_index(void) {
    auto& ui = Ui();
    return ui.session ? ui.session->CurrentTocIndex() : -1;
}

const char* book_reader_last_error(void) {
    return Ui().last_error.c_str();
}

}  // extern "C"
