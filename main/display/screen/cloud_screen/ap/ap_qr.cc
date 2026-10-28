#include "ap_qr.h"
#include "ui_qrcode.h"

#include "qrcodegen.h"

#include <lvgl.h>

#include <cstring>

namespace {

constexpr int kQrSide = kApQrSide;
constexpr int kQuietPx = 8; // 画布内静区像素，另有外框留白
constexpr int kMaxVer = 8;

void FreeDrawBufAsync(void* buf) {
    lv_draw_buf_destroy(static_cast<lv_draw_buf_t*>(buf));
}

void OnCanvasDelete(lv_event_t* e) {
    // 析构还要读 draw_buf，等这次删除返回后再释放
    auto* buf = static_cast<lv_draw_buf_t*>(lv_event_get_user_data(e));
    if (buf != nullptr) {
        lv_async_call(FreeDrawBufAsync, buf);
    }
}

lv_draw_buf_t* BufFor(lv_obj_t* canvas) {
    auto* existing = static_cast<lv_draw_buf_t*>(lv_obj_get_user_data(canvas));
    if (existing != nullptr) {
        return existing;
    }
    lv_draw_buf_t* created = lv_draw_buf_create(kQrSide, kQrSide, LV_COLOR_FORMAT_I1, 0);
    if (created == nullptr) {
        return nullptr;
    }
    lv_obj_set_user_data(canvas, created);
    lv_obj_add_event_cb(canvas, OnCanvasDelete, LV_EVENT_DELETE, created);
    lv_canvas_set_draw_buf(canvas, created);
    // I1 的 set_px 用颜色 blue 低位当色号：黑是 0，白是 1
    lv_canvas_set_palette(canvas, 0, lv_color_to_32(lv_color_black(), LV_OPA_COVER));
    lv_canvas_set_palette(canvas, 1, lv_color_to_32(lv_color_white(), LV_OPA_COVER));
    return created;
}

void SetBlack(uint8_t* row, int x) {
    row[x >> 3] &= static_cast<uint8_t>(~(1u << (7 - (x & 7))));
}

}  // namespace

lv_obj_t* ApQrCreate(lv_obj_t* parent) {
    lv_obj_t* box = UiCreateQrFrame(parent, kApQrSide);
    lv_obj_t* qr = lv_canvas_create(box);
    lv_obj_set_size(qr, kApQrSide, kApQrSide);
    lv_obj_set_style_bg_color(qr, lv_color_white(), 0);
    lv_obj_set_style_bg_opa(qr, LV_OPA_COVER, 0);
    lv_obj_clear_flag(qr, LV_OBJ_FLAG_CLICKABLE);
    return qr;
}

void ApQrPaint(lv_obj_t* canvas, const char* text) {
    if (canvas == nullptr || text == nullptr || text[0] == '\0') {
        return;
    }
    lv_draw_buf_t* buf = BufFor(canvas);
    if (buf == nullptr) {
        return;
    }
    uint8_t temp[qrcodegen_BUFFER_LEN_FOR_VERSION(kMaxVer)];
    uint8_t qr[qrcodegen_BUFFER_LEN_FOR_VERSION(kMaxVer)];
    const bool encoded = qrcodegen_encodeText(text, temp, qr, qrcodegen_Ecc_LOW, 1, kMaxVer,
                                               qrcodegen_Mask_AUTO, true);
    // 直接写色号。fill_bg / set_px 会按像素 invalidate，两张码能把界面线程卡住数秒
    auto* px = static_cast<uint8_t*>(lv_draw_buf_goto_xy(buf, 0, 0));
    const uint32_t stride = buf->header.stride;
    for (int y = 0; y < kQrSide; ++y) {
        std::memset(px + static_cast<uint32_t>(y) * stride, 0xFF, stride);
    }
    if (encoded) {
        const int modules = qrcodegen_getSize(qr);
        const int span = kQrSide - kQuietPx * 2;
        if (modules > 0 && modules <= span) {
            for (int y = 0; y < modules; ++y) {
                for (int x = 0; x < modules; ++x) {
                    if (!qrcodegen_getModule(qr, x, y)) {
                        continue;
                    }
                    const int x0 = kQuietPx + x * span / modules;
                    const int y0 = kQuietPx + y * span / modules;
                    const int x1 = kQuietPx + (x + 1) * span / modules;
                    const int y1 = kQuietPx + (y + 1) * span / modules;
                    for (int py = y0; py < y1; ++py) {
                        uint8_t* row = px + static_cast<uint32_t>(py) * stride;
                        for (int dx = x0; dx < x1; ++dx) {
                            SetBlack(row, dx);
                        }
                    }
                }
            }
        }
    }
    lv_draw_buf_flush_cache(buf, nullptr);
    lv_obj_invalidate(canvas);
}
