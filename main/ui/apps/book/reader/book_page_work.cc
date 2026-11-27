#include <atomic>
#include <new>
#include <esp_log.h>
#include "ui/apps/book/reader/book_page_work.h"
#include "business/reader/book_reader_prefs.h"
#include "ui/apps/book/book_font_runtime.h"
#include "ui/apps/book/book_geom.h"
#include "ui/apps/book/book_nav.h"
#include "ui/apps/book/book_screen_priv.h"
#include "ui/common/drawing/ui_theme.h"

static constexpr const char* kTag = "ReaderPageWork";
static std::atomic<ReaderPageResult*> completed{nullptr};
static std::atomic<bool> loading_chapter{false};

bool ReaderChapterLoadPending() {
    return loading_chapter.load();
}

bool StartReaderPageWork(std::unique_ptr<ReaderPageResult> result) {
    auto work = std::unique_ptr<ReaderPageJob>(new (std::nothrow) ReaderPageJob{});
    if (!work) {
        ESP_LOGE(kTag, "page request allocation failed");
        return false;
    }
    loading_chapter.store(false);
    work->loading_chapter = &loading_chapter;
    work->result = std::move(result);
    work->result->dark = UiThemeIsDark();
    work->prewarm = PrewarmPageFont;
    work->completed = [](std::unique_ptr<ReaderPageResult> result) {
        completed.store(result.release(), std::memory_order_release);
        BookRequestUiWork();
    };
    const auto& st = Book_State();
    work->font = BookFont();
    work->style = work->result->cursor->Style();
    work->style.underline_offset = BookReaderPrefsTextValue(BookTextOption::UnderlineOffset);
    work->style.aa_strength = BookReaderPrefsTextValue(BookTextOption::AaStrength);
    work->width = st.reader.viewport_w;
    work->height = st.reader.viewport_h;
    work->vertical = st.reader.session->VerticalWriting();
    work->rtl = BookReaderPrefsTextDirection() == kBookReaderTextRightToLeft;
    work->underline = BookReaderPrefsUnderlineMode();
    work->antialias = BookReaderPrefsAntialias() != 0;
    work->background = st.reader.session->IsPageImagesMode() ? 0 : BookReaderPrefsBackground();
    const auto background_area = ReadBackgroundArea();
    work->screen_width = lv_area_get_width(&background_area);
    work->screen_height = lv_area_get_height(&background_area);
    const auto area = ReadContentArea();
    work->text_left = area.x1;
    work->text_top = area.y1;
    if (work->background && st.reader.background_mode == work->background &&
        st.reader.background_dark == work->result->dark && st.reader.background_image &&
        st.reader.background_image->width == work->screen_width &&
        st.reader.background_image->height == work->screen_height) {
        work->background_image = st.reader.background_image;
    }
    return ReaderPreparePage(std::move(work));
}

std::unique_ptr<ReaderPageResult> TakeCompletedReaderPageWork() {
    return std::unique_ptr<ReaderPageResult>(completed.exchange(nullptr, std::memory_order_acq_rel));
}
