#include "cloud_screen/preview/cloud_wallpaper_preview.h"
#include "cloud_screen/cloud_screen_priv.h"
#include "cloud_screen/download/cloud_download.h"
#include "cloud_screen/cloud_list.h"
#include "cloud_screen/cloud_ui_helpers.h"
#include "cloud_screen/push/push_resources_library.h"

#include <freertos/FreeRTOS.h>
#include <lvgl.h>
#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>
#include <esp_log.h>
#include <freertos/task.h>

#include "assets/lang_config.h"
#include "power_policy.h"
#include "reader/reader.h"
#include "reader/image_util.h"
#include "screen_common.h"

void StyleDownloadBtn(bool enabled) {
    auto& st = Cloud_State();
    if (st.preview.download_btn == nullptr || st.preview.download_lbl == nullptr) {
        return;
    }
    lv_obj_set_style_bg_color(st.preview.download_btn, lv_color_white(), 0);
    lv_obj_set_style_bg_opa(st.preview.download_btn, enabled ? LV_OPA_COVER : LV_OPA_50, 0);
    lv_obj_set_style_border_color(st.preview.download_btn, lv_color_black(), 0);
    lv_obj_set_style_border_width(st.preview.download_btn, kRowBorderW, 0);
    lv_obj_set_style_radius(st.preview.download_btn, kRowRadius, 0);
    lv_obj_set_style_text_color(st.preview.download_lbl, lv_color_black(), 0);
    if (enabled) {
        lv_obj_add_flag(st.preview.download_btn, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_clear_state(st.preview.download_btn, LV_STATE_DISABLED);
    } else {
        lv_obj_clear_flag(st.preview.download_btn, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_state(st.preview.download_btn, LV_STATE_DISABLED);
    }
}

void ClearWallpaperPreviewImage() {
    auto& st = Cloud_State();
    if (st.preview.img != nullptr) {
        lv_image_set_src(st.preview.img, nullptr);
        lv_obj_add_flag(st.preview.img, LV_OBJ_FLAG_HIDDEN);
    }
    if (st.preview.raster != nullptr) {
        delete st.preview.raster;
        st.preview.raster = nullptr;
    }
}

void ShowWallpaperPreviewMode(bool preview) {
    auto& st = Cloud_State();
    st.preview.open = preview;
    if (st.chrome.main_body != nullptr) {
        if (preview) {
            lv_obj_add_flag(st.chrome.main_body, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_clear_flag(st.chrome.main_body, LV_OBJ_FLAG_HIDDEN);
        }
    }
    if (st.chrome.page_lbl != nullptr) {
        if (preview) {
            lv_obj_add_flag(st.chrome.page_lbl, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_clear_flag(st.chrome.page_lbl, LV_OBJ_FLAG_HIDDEN);
        }
    }
    if (st.preview.body != nullptr) {
        if (preview) {
            lv_obj_clear_flag(st.preview.body, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_add_flag(st.preview.body, LV_OBJ_FLAG_HIDDEN);
        }
    }
}

void CloseWallpaperPreview() {
    auto& st = Cloud_State();
    ++st.preview.epoch;
    st.preview.idx = -1;
    ClearWallpaperPreviewImage();
    ShowWallpaperPreviewMode(false);
    RenderListPage();
}

void ApplyWallpaperPreviewAsync(void* p) {
    auto* msg = static_cast<WallpaperPreviewResultMsg*>(p);
    s_wallpaper_preview_busy.store(false);
    s_wallpaper_preview_task = nullptr;
    if (msg == nullptr) {
        return;
    }
    auto& st = Cloud_State();
    if (!ScreenAlive() || msg->epoch != st.preview.epoch ||
        !st.preview.open || msg->index != st.preview.idx) {
        delete msg->img;
        delete msg;
        if (ScreenAlive() && st.preview.open && st.preview.idx >= 0) {
            if (CloudDlTasksActive() || s_dl_owns_http.load(std::memory_order_acquire)) {
                ShowPreviewWaitDlTip();
            } else {
                ScheduleLoadWallpaperPreview(st.preview.idx);
            }
        }
        return;
    }

    ClearWallpaperPreviewImage();
    if (!msg->ok || msg->img == nullptr || msg->img->empty()) {
        if (st.preview.status != nullptr) {
            lv_label_set_text(st.preview.status,
                              msg->err[0] != '\0' ? msg->err : Lang::Strings::CLOUD_PREVIEW_FAIL);
            lv_obj_clear_flag(st.preview.status, LV_OBJ_FLAG_HIDDEN);
        }
        StyleDownloadBtn(true);
        delete msg->img;
        delete msg;
        return;
    }

    st.preview.raster = msg->img;
    msg->img = nullptr;
    st.preview.raster->BindDsc();

    if (st.preview.img != nullptr) {
        lv_image_set_scale(st.preview.img, LV_SCALE_NONE);
        lv_image_set_src(st.preview.img, &st.preview.raster->dsc);
        lv_obj_set_size(st.preview.img, st.preview.raster->width,
                        st.preview.raster->height);
        lv_obj_clear_flag(st.preview.img, LV_OBJ_FLAG_HIDDEN);
        lv_obj_center(st.preview.img);
        lv_obj_invalidate(st.preview.img);
        if (st.preview.img_host != nullptr) {
            lv_obj_invalidate(st.preview.img_host);
        }
    }
    if (st.preview.status != nullptr) {
        lv_obj_add_flag(st.preview.status, LV_OBJ_FLAG_HIDDEN);
    }
    StyleDownloadBtn(true);
    delete msg;
}

void WallpaperPreviewLoadTask(void* arg) {
    auto* work = static_cast<WallpaperPreviewWork*>(arg);
    auto* msg = new WallpaperPreviewResultMsg{};
    {
        if (work == nullptr) {
            std::snprintf(msg->err, sizeof(msg->err), "%s", Lang::Strings::CLOUD_FILE_INVALID);
        } else {
            msg->index = work->index;
            msg->epoch = work->epoch;
            auto* img = new reader::RasterImage();
            bool ok = false;
            if (Cloud_State().preview.epoch != work->epoch) {
                delete img;
                img = nullptr;
                std::snprintf(msg->err, sizeof(msg->err), "%s", Lang::Strings::CLOUD_CANCELLED);
            } else if (s_dl_owns_http.load(std::memory_order_acquire)) {
                delete img;
                img = nullptr;
                std::snprintf(msg->err, sizeof(msg->err), "%s", Lang::Strings::CLOUD_WAIT_DOWNLOAD);
            } else if (!work->cached_bytes.empty()) {
                // 列表已缓存 coverImageUrl 字节：按详情尺寸解码，不再 HTTP
                ok = reader::DecodeImageToL8(work->cached_bytes.data(), work->cached_bytes.size(),
                                             kPreviewMaxW, kPreviewMaxH, *img) &&
                     !img->empty();
                if (!ok) {
                    std::snprintf(msg->err, sizeof(msg->err), "%s", Lang::Strings::CLOUD_DECODE_FAIL);
                }
            } else {
                PowerNeedHold hold_net(PowerNeed::OtaDownload);
                if (work->use_local && work->local_path[0] != '\0') {
                    ok = reader::DecodeImageFileToL8(work->local_path, kPreviewMaxW, kPreviewMaxH,
                                                     *img) &&
                         !img->empty();
                } else if (work->download_url[0] != '\0') {
                    std::vector<uint8_t> bytes;
                    if (Cloud_DownloadUrlToBuffer(work->download_url, bytes, kPreviewMaxDownloadBytes) &&
                        Cloud_State().preview.epoch == work->epoch) {
                        ok = reader::DecodeImageToL8(bytes.data(), bytes.size(), kPreviewMaxW,
                                                     kPreviewMaxH, *img) &&
                             !img->empty();
                    } else {
                        std::snprintf(msg->err, sizeof(msg->err), "%s", Lang::Strings::CLOUD_LOAD_FAIL);
                    }
                }
            }
            if (ok) {
                msg->ok = true;
                msg->img = img;
            } else {
                delete img;
                if (msg->err[0] == '\0') {
                    std::snprintf(msg->err, sizeof(msg->err), "%s", Lang::Strings::CLOUD_DECODE_FAIL);
                }
            }
            delete work;
        }
    }

    if (!ScreenLvAsync(ApplyWallpaperPreviewAsync, msg)) {
        delete msg->img;
        delete msg;
        s_wallpaper_preview_busy.store(false);
        s_wallpaper_preview_task = nullptr;
    }
    vTaskDelete(nullptr);
}

void ScheduleLoadWallpaperPreview(int index) {
    auto& st = Cloud_State();
    if (index < 0 || index >= static_cast<int>(st.data.items.size())) {
        return;
    }
    // 下载独占 HTTP：详情可开，但不加载封面（含缓存解码），统一提示；全结束后续 Resume
    if (CloudDlTasksActive() || s_dl_owns_http.load(std::memory_order_acquire)) {
        ShowPreviewWaitDlTip();
        return;
    }
    if (s_wallpaper_preview_busy.exchange(true)) {
        return;
    }
    const reader::CloudPushResource& item = st.data.items[static_cast<size_t>(index)];
    auto* work = new WallpaperPreviewWork{};
    work->index = index;
    work->epoch = st.preview.epoch;
    // 预览只用 coverImageUrl（列表缓存或 HTTP）；已落盘壁纸可走本地文件；绝不拉完整 downloadUrl
    const bool is_badge = item.type == reader::PushResourceType::kBadge;
    work->use_local = is_badge && item.IsDownloaded() &&
                      (index >= static_cast<int>(st.data.cover_bytes.size()) ||
                       st.data.cover_bytes[static_cast<size_t>(index)].empty()) &&
                      item.CoverThumbUrl().empty();
    if (work->use_local) {
        std::snprintf(work->local_path, sizeof(work->local_path), "%s", item.LocalPath().c_str());
    }
    if (index < static_cast<int>(st.data.cover_bytes.size()) &&
        !st.data.cover_bytes[static_cast<size_t>(index)].empty()) {
        work->cached_bytes = st.data.cover_bytes[static_cast<size_t>(index)];
        ESP_LOGI(TAG, "preview cover from list cache idx=%d bytes=%u", index,
                 static_cast<unsigned>(work->cached_bytes.size()));
    } else if (!work->use_local) {
        const std::string& url = item.CoverThumbUrl();
        if (url.empty()) {
            delete work;
            s_wallpaper_preview_busy.store(false);
            s_wallpaper_preview_task = nullptr;
            if (st.preview.status != nullptr) {
                lv_label_set_text(st.preview.status, Lang::Strings::CLOUD_NO_COVER);
                lv_obj_clear_flag(st.preview.status, LV_OBJ_FLAG_HIDDEN);
            }
            StyleDownloadBtn(true);
            return;
        }
        if (url.size() >= sizeof(work->download_url)) {
            delete work;
            s_wallpaper_preview_busy.store(false);
            s_wallpaper_preview_task = nullptr;
            if (st.preview.status != nullptr) {
                lv_label_set_text(st.preview.status, Lang::Strings::CLOUD_PREVIEW_URL_LONG);
                lv_obj_clear_flag(st.preview.status, LV_OBJ_FLAG_HIDDEN);
            }
            StyleDownloadBtn(true);
            return;
        }
        std::snprintf(work->download_url, sizeof(work->download_url), "%s", url.c_str());
        ESP_LOGI(TAG, "preview cover HTTP fallback idx=%d (no list cache)", index);
    }
    // 预览加载任务
    if (xTaskCreatePinnedToCore(WallpaperPreviewLoadTask, "cloud_wp_prev",
                                kWallpaperPreviewStack, work, 5, &s_wallpaper_preview_task,
                                0) != pdPASS) {
        delete work;
        s_wallpaper_preview_busy.store(false);
        s_wallpaper_preview_task = nullptr;
        if (st.preview.status != nullptr) {
            lv_label_set_text(st.preview.status, Lang::Strings::CLOUD_PREVIEW_START_FAIL);
            lv_obj_clear_flag(st.preview.status, LV_OBJ_FLAG_HIDDEN);
        }
        StyleDownloadBtn(true);
    }
}

void OpenWallpaperPreview(int index) {
    auto& st = Cloud_State();
    if (index < 0 || index >= static_cast<int>(st.data.items.size())) {
        return;
    }
    if (st.dialogs.dialog_mask != nullptr || st.dialogs.action_mask != nullptr) {
        return;
    }
    const reader::CloudPushResource& item = st.data.items[static_cast<size_t>(index)];
    ++st.preview.epoch;
    st.preview.idx = index;
    ClearWallpaperPreviewImage();
    ShowWallpaperPreviewMode(true);
    if (st.preview.title != nullptr) {
        // 优先可读名；展示去扩展名（同壁纸/书库标题）
        const char* raw =
            !item.resource_name.empty() ? item.resource_name.c_str() : item.name.c_str();
        const std::string stem = reader::TitleFromPath(raw);
        const std::string shown =
            Cloud_LayoutTitleTwoLines(stem.empty() ? raw : stem.c_str(), Cloud_UiFont(), Cloud_ContentWidth());
        lv_label_set_text(st.preview.title, shown.c_str());
    }
    FillPreviewMeta(&item);
    if (st.preview.download_lbl != nullptr) {
        lv_label_set_text(st.preview.download_lbl, Lang::Strings::CLOUD_SAVE_LOCAL);
    }
    StyleDownloadBtn(true);
    if (CloudDlTasksActive() || s_dl_owns_http.load(std::memory_order_acquire)) {
        ShowPreviewWaitDlTip();
        return;
    }
    if (st.preview.status != nullptr) {
        lv_label_set_text(st.preview.status, Lang::Strings::CLOUD_LOADING);
        lv_obj_clear_flag(st.preview.status, LV_OBJ_FLAG_HIDDEN);
    }
    ScheduleLoadWallpaperPreview(index);
}

void OnWallpaperDownloadClicked(lv_event_t* /*e*/) {
    auto& st = Cloud_State();
    if (!st.preview.open || st.preview.idx < 0) {
        return;
    }
    StartWallpaperDownload(st.preview.idx);
}

void StartWallpaperDownload(int index) {
    auto& st = Cloud_State();
    if (SyncBusy()) {
        SetStatusTip(Lang::Strings::CLOUD_PLEASE_WAIT, true);
        return;
    }
    if (index < 0 || index >= static_cast<int>(st.data.items.size())) {
        return;
    }
    const std::string& id = st.data.items[static_cast<size_t>(index)].task_id;
    auto close_preview = [&]() {
        if (st.preview.open) {
            ++st.preview.epoch;
            st.preview.idx = -1;
            ClearWallpaperPreviewImage();
            ShowWallpaperPreviewMode(false);
        }
    };

    // 正在下这一条：仅提示
    if (st.xfer.download_busy && st.xfer.download_key == id) {
        SetStatusTip(Lang::Strings::CLOUD_DOWNLOADING, true);
        close_preview();
        return;
    }
    // 已有下载/批量：加入串行队列，当前完成后 ContinueBatchSaveIfNeeded 会接着下
    if (st.xfer.download_busy || st.data.batch_busy || st.data.delete_busy) {
        if (st.data.delete_busy) {
            SetStatusTip(Lang::Strings::CLOUD_PLEASE_WAIT, true);
            close_preview();
            return;
        }
        if (std::find(st.data.batch_save_ids.begin(), st.data.batch_save_ids.end(), id) !=
            st.data.batch_save_ids.end()) {
            SetStatusTip(Lang::Strings::CLOUD_IN_QUEUE, true);
            close_preview();
            return;
        }
        st.data.batch_save_ids.push_back(id);
        st.data.batch_busy = true;
        ++st.xfer.dl_job_total;
        close_preview();
        if (st.xfer.dl_page_open) {
            RefreshDlProgressPage(st.xfer.dl_shown_percent < 0 ? 0 : st.xfer.dl_shown_percent);
        }
        return;
    }

    close_preview();
    StartResourceDownload(index);
}

