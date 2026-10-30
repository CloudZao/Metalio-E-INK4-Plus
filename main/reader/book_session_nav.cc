#include "book_session.h"

#include <new>

namespace reader {

std::unique_ptr<BookSession> BookSession::CreateNavigationCursor(NavigationKind kind, int& value) const try {
    if (!open_) return {};
    auto cursor = std::make_unique<BookSession>();
    cursor->navigation_source_ = this;
    cursor->open_ = true;
    cursor->close_progress_queued_ = true;
    cursor->info_ = info_;
    cursor->font_ = font_;
    cursor->vertical_writing_ = vertical_writing_;
    cursor->physical_w_ = physical_w_; cursor->physical_h_ = physical_h_;
    cursor->viewport_w_ = viewport_w_; cursor->viewport_h_ = viewport_h_;
    cursor->line_spacing_percent_ = line_spacing_percent_; cursor->para_gap_ = para_gap_;
    cursor->page_images_mode_ = page_images_mode_;
    cursor->chapter_count_ = chapter_count_;
    cursor->chapter_index_ = chapter_index_; cursor->page_index_ = page_index_;
    cursor->chapter_page_counts_ = chapter_page_counts_;
    cursor->chapter_pages_ready_.store(chapter_pages_ready_.load());
    cursor->ebook_spine_ = ebook_spine_;
    cursor->txt_mode_ = txt_mode_; cursor->txt_empty_ = txt_empty_;
    cursor->txt_as_gb18030_ = txt_as_gb18030_;
    cursor->live_preview_active_ = live_preview_active_;
    cursor->txt_preview_relayout_pending_ = txt_preview_relayout_pending_;
    if (live_preview_active_) {
        cursor->live_preview_page_ = live_preview_page_;
        cursor->live_preview_byte_off_ = live_preview_byte_off_;
        cursor->live_preview_at_para_start_ = live_preview_at_para_start_;
        cursor->live_preview_next_off_ = live_preview_next_off_;
        cursor->live_preview_next_at_para_start_ = live_preview_next_at_para_start_;
        cursor->live_preview_has_next_ = live_preview_has_next_;
        cursor->live_preview_hist_ = live_preview_hist_;
    } else if (txt_mode_) {
        cursor->txt_lines_len_ = txt_lines_len_;
        cursor->txt_page_count_ = txt_page_count_;
    }
    if (kind == NavigationKind::kChapterDelta) {
        if (TocCount() <= 0) return {};
        value = std::clamp(CurrentTocIndex() + value, 0, TocCount() - 1);
    }
    if ((kind == NavigationKind::kToc || kind == NavigationKind::kChapterDelta) && txt_mode_) {
        if (value < 0 || static_cast<size_t>(value) >= toc_.size()) return {};
        cursor->toc_.push_back(toc_[value]);
        value = 0;
    }
    if (kind == NavigationKind::kBookmark) {
        const auto& entries = bookmarks_.Entries();
        const auto found = std::find_if(entries.begin(), entries.end(), [value](const BookBookmark& entry) {
            return entry.id == static_cast<uint32_t>(value);
        });
        if (found == entries.end() || !bookmarks_.Error().empty()) return {};
        cursor->navigation_bookmark_ = found->start;
    }
    return cursor;
} catch (const std::bad_alloc&) {
    return {};
}

bool BookSession::PrepareNavigation(NavigationKind kind, int value) {
    const auto prepare = [&]() -> bool {
        if (!navigation_source_ || navigation_source_->background_paused_.load()) { error_ = BookReadError::kCancelled; return false; }
        if (txt_preview_relayout_pending_) {
            if (!FillTxtLivePreview(live_preview_byte_off_, live_preview_at_para_start_)) return false;
            txt_preview_relayout_pending_ = false;
        }
        if (txt_mode_ && !live_preview_active_) {
            if (!navigation_source_->txt_lines_.CloneReadOnly(txt_lines_, txt_lines_len_)) {
                error_ = txt_lines_.Error();
                return false;
            }
            if (!navigation_source_->txt_page_first_line_.CloneReadOnly(txt_page_first_line_, txt_page_count_ + 1)) {
                error_ = txt_page_first_line_.Error();
                return false;
            }
        } else if (!txt_mode_ && !navigation_source_->pages_.CloneReadOnly(pages_)) {
            error_ = pages_.Error();
            return false;
        }
        bool ok = true;
        switch (kind) {
            case NavigationKind::kRefresh: break;
            case NavigationKind::kDelta: {
                // UI 已将连点合并为有界增量，只读取最终目标页。
                const int direction = value > 0 ? 1 : -1;
                while (value != 0 && !navigation_source_->background_paused_.load()) {
                    const bool moved = direction > 0 ? NextPage() : PrevPage();
                    if (!moved) {
                        if (Error() != BookReadError::kNone) ok = false;
                        break;
                    }
                    value -= direction;
                }
                break;
            }
            case NavigationKind::kToc: ok = GoToToc(value); break;
            case NavigationKind::kChapterDelta: ok = GoToToc(value); break;
            case NavigationKind::kProgress: ok = GoToReadingProgress(value); break;
            case NavigationKind::kBookmark: ok = GoToBookmarkPosition(navigation_bookmark_); break;
        }
        if (navigation_source_->background_paused_.load()) { error_ = BookReadError::kCancelled; return false; }
        if (!ok) return false;
        if (!CurrentPageData()) {
            if (error_ == BookReadError::kNone) error_ = txt_mode_ ? BookReadError::kCacheIo : pages_.Error();
            return false;
        }
        navigation_anchor_ = CaptureLayoutAnchor();
        return true;
    };
    bool prepared = false;
    try {
        prepared = prepare();
    } catch (const std::bad_alloc&) {
        error_ = BookReadError::kNoMemory;
    }
    // 仅保留已物化页面；延迟回收游标不能持续占用 SD 的有限文件句柄。
    const bool closed = CloseSdHandles();
    return prepared && closed;
}

bool BookSession::CommitNavigation(BookSession& cursor) {
    if (cursor.navigation_source_ != this) return false;
    if (cursor.navigation_replaced_pages_) {
        pages_.swap(cursor.pages_);
        NoteChapterPageCount(cursor.chapter_index_, std::max(1, static_cast<int>(pages_.size())));
    } else if (!txt_mode_ && !pages_.TakeReadCache(cursor.pages_)) {
        return false;
    }
    chapter_index_ = cursor.chapter_index_;
    page_index_ = cursor.page_index_;
    live_preview_active_ = cursor.live_preview_active_;
    txt_preview_relayout_pending_ = false;
    live_preview_byte_off_ = cursor.live_preview_byte_off_;
    live_preview_at_para_start_ = cursor.live_preview_at_para_start_;
    live_preview_next_off_ = cursor.live_preview_next_off_;
    live_preview_next_at_para_start_ = cursor.live_preview_next_at_para_start_;
    live_preview_has_next_ = cursor.live_preview_has_next_;
    live_preview_hist_.swap(cursor.live_preview_hist_);
    live_preview_page_.items.swap(cursor.live_preview_page_.items);
    txt_preview_anchor_.store(live_preview_byte_off_);
    txt_view_page_.items.swap(cursor.txt_view_page_.items);
    txt_view_index_ = cursor.txt_view_index_;
    navigation_anchor_ = cursor.navigation_anchor_;
    error_ = BookReadError::kNone;
    return true;
}

} // namespace reader
