#include "home_icon_source.h"
#include <lvgl.h>
#include "src/draw/lv_image_decoder_private.h"
#include "esp_heap_caps.h"
#include <algorithm>
#include <memory>
#include <new>
#include <string>
#include <zlib.h>

namespace {
struct Session {
    const lv_image_dsc_t* source; // Flash 中的压缩图标
    int32_t next_y; // 下次返回的起始行
    int32_t tile_index; // 当前已解压分块，-1 表示尚未解压
    uint8_t* pixels; // 会话像素缓冲：ARGB8888 为 16 行，A8/AL88 为完整图像
    lv_draw_buf_t tile; // 当前裁剪区域的像素视图
};
bool InflateTile(const lv_image_dsc_t* source, int index, uint8_t* pixels) {
    const auto* packed = reinterpret_cast<const HomeIconCompressed*>(source);
    const uint32_t begin = packed->tile_offsets[index];
    const uint32_t limit = packed->tile_offsets[index + 1];
    const uLongf expected = size_t(packed->color_format == LV_COLOR_FORMAT_AL88 ? source->header.h :
        std::min<int>(16, source->header.h - index * 16)) * source->header.stride;
    uLongf size = expected;
    return begin < limit && limit <= source->data_size &&
        uncompress(pixels, &size, source->data + begin, limit - begin) == Z_OK && size == expected;
}
lv_result_t Info(lv_image_decoder_t*, lv_image_decoder_dsc_t* dsc, lv_image_header_t* header) {
    if(dsc->src_type != LV_IMAGE_SRC_VARIABLE) return LV_RESULT_INVALID;
    const auto* source = static_cast<const lv_image_dsc_t*>(dsc->src);
    if(source->header.cf != LV_COLOR_FORMAT_RAW ||
       source->header.flags != (LV_IMAGE_FLAGS_USER8 | LV_IMAGE_FLAGS_USER7) ||
       !source->header.w || !source->header.h || !source->data || !source->data_size) return LV_RESULT_INVALID;
    const auto* packed = reinterpret_cast<const HomeIconCompressed*>(source);
    if(!packed->tile_offsets || (packed->color_format != LV_COLOR_FORMAT_A8 &&
       packed->color_format != LV_COLOR_FORMAT_AL88 && packed->color_format != LV_COLOR_FORMAT_ARGB8888)) return LV_RESULT_INVALID;
    const int bytes = packed->color_format == LV_COLOR_FORMAT_A8 ? 1 :
        packed->color_format == LV_COLOR_FORMAT_AL88 ? 2 : 4;
    if(source->header.stride != source->header.w * bytes) return LV_RESULT_INVALID;
    *header = source->header;
    header->cf = packed->color_format;
    header->flags = 0;
    return LV_RESULT_OK;
}
lv_result_t Open(lv_image_decoder_t*, lv_image_decoder_dsc_t* dsc) {
    auto* session = static_cast<Session*>(heap_caps_calloc(1, sizeof(Session), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if(!session) return LV_RESULT_INVALID;
    session->source = static_cast<const lv_image_dsc_t*>(dsc->src);
    const auto* source = session->source;
    const bool full_image = dsc->header.cf == LV_COLOR_FORMAT_A8 || dsc->header.cf == LV_COLOR_FORMAT_AL88;
    session->tile_index = -1;
    session->pixels = static_cast<uint8_t*>(heap_caps_malloc(
        size_t(source->header.stride) * (full_image ? source->header.h : std::min<int>(16, source->header.h)),
        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if(!session->pixels) { heap_caps_free(session); return LV_RESULT_INVALID; }
    dsc->user_data = session;
    dsc->args.no_cache = true;
    dsc->decoded = nullptr;
    if(full_image) {
        // 灰度透明图保持整图缩放，避免分块改变插值边界。
        for(int y = 0; y < source->header.h; y += (dsc->header.cf == LV_COLOR_FORMAT_AL88 ? source->header.h : 16)) {
            if(!InflateTile(source, y / 16, session->pixels + y * source->header.stride)) {
                heap_caps_free(session->pixels);
                heap_caps_free(session);
                dsc->user_data = nullptr;
                return LV_RESULT_INVALID;
            }
        }
        session->tile.header = dsc->header;
        session->tile.data = session->pixels;
        session->tile.data_size = size_t(source->header.stride) * source->header.h;
        session->tile.unaligned_data = session->pixels;
        session->tile.handlers = lv_draw_buf_get_image_handlers();
        dsc->decoded = &session->tile;
    }
    return LV_RESULT_OK;
}
lv_result_t Area(lv_image_decoder_t*, lv_image_decoder_dsc_t* dsc, const lv_area_t* full, lv_area_t* decoded) {
    auto* session = static_cast<Session*>(dsc->user_data);
    if(!session || full->x1 > full->x2 || full->y1 > full->y2) return LV_RESULT_INVALID;
    const auto* source = session->source;
    const int32_t x1 = std::max<int32_t>(full->x1, 0);
    const int32_t x2 = std::min<int32_t>(full->x2, source->header.w - 1);
    const int32_t y = std::max<int32_t>(session->next_y,
        decoded->y1 == LV_COORD_MIN ? full->y1 : decoded->y2 + 1);
    if(x1 > x2 || y < 0 || y > full->y2 || y >= source->header.h) return LV_RESULT_INVALID;
    // 与 SPNG 保持相同的 16 行边界和步幅，避免缩放结果变化。
    const int32_t end = std::min({((y / 16) + 1) * 16, int32_t(source->header.h), full->y2 + 1});
    const auto* packed = reinterpret_cast<const HomeIconCompressed*>(source);
    const int32_t index = y / 16;
    if(session->tile_index != index) {
        if(!InflateTile(source, index, session->pixels)) return LV_RESULT_INVALID;
        session->tile_index = index;
    }
    const int bytes = packed->color_format == LV_COLOR_FORMAT_A8 ? 1 :
        packed->color_format == LV_COLOR_FORMAT_AL88 ? 2 : 4;
    auto& tile = session->tile;
    tile.header = dsc->header;
    tile.header.w = x2 - x1 + 1;
    tile.header.h = end - y;
    tile.data = session->pixels + (y % 16) * source->header.stride + x1 * bytes;
    tile.data_size = (tile.header.h - 1) * source->header.stride + tile.header.w * bytes;
    tile.unaligned_data = tile.data;
    tile.handlers = lv_draw_buf_get_image_handlers();
    *decoded = {full->x1, y, full->x2, end - 1};
    session->next_y = end;
    dsc->decoded = &tile;
    return LV_RESULT_OK;
}
void Close(lv_image_decoder_t*, lv_image_decoder_dsc_t* dsc) {
    auto* session = static_cast<Session*>(dsc->user_data);
    if(session) heap_caps_free(session->pixels);
    heap_caps_free(dsc->user_data);
    dsc->user_data = nullptr;
}
}
bool HomeIconDecoderReady() {
    static lv_image_decoder_t* decoder = nullptr;
    if(decoder) return true;
    decoder = lv_image_decoder_create();
    if(!decoder) return false;
    lv_image_decoder_set_info_cb(decoder, Info);
    lv_image_decoder_set_open_cb(decoder, Open);
    lv_image_decoder_set_get_area_cb(decoder, Area);
    lv_image_decoder_set_close_cb(decoder, Close);
    return true;
}

namespace {
struct FullIcon {
    std::string path; // 当前资源路径，相同天气不重复解码
    lv_image_dsc_t image{}; // 控件借用的完整 A8 图像
    uint8_t* pixels = nullptr; // 当前控件持有的透明度数据
    ~FullIcon() {
        lv_image_cache_drop(&image);
        heap_caps_free(pixels);
    }
};

bool DecodeFullIcon(const char* path, FullIcon& icon) {
    lv_image_decoder_dsc_t decoder{};
    lv_image_decoder_args_t args{};
    args.no_cache = true;
    if (lv_image_decoder_open(&decoder, path, &args) != LV_RESULT_OK) return false;
    struct CloseDecoder {
        lv_image_decoder_dsc_t* decoder;
        ~CloseDecoder() { lv_image_decoder_close(decoder); }
    } close{&decoder};
    const int w = decoder.header.w, h = decoder.header.h;
    if (w <= 0 || h <= 0 || decoder.header.cf != LV_COLOR_FORMAT_ARGB8888) return false;
    icon.pixels = static_cast<uint8_t*>(heap_caps_malloc(size_t(w) * h, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (!icon.pixels) return false;
    const lv_area_t full{0, 0, w - 1, h - 1};
    lv_area_t area{LV_COORD_MIN, LV_COORD_MIN, LV_COORD_MIN, LV_COORD_MIN};
    int next_y = 0;
    const bool complete = decoder.decoded != nullptr;
    while (next_y < h) {
        if (complete) area = full;
        else if (lv_image_decoder_get_area(&decoder, &full, &area) != LV_RESULT_OK) return false;
        const auto* tile = decoder.decoded;
        if (!tile || !tile->data || tile->header.cf != LV_COLOR_FORMAT_ARGB8888 ||
            tile->header.stride < w * 4 || area.x1 != 0 || area.x2 != w - 1 ||
            area.y1 != next_y || area.y2 < area.y1 || area.y2 >= h) return false;
        for (int y = area.y1; y <= area.y2; ++y) {
            const uint8_t* row = tile->data + size_t(y - area.y1) * tile->header.stride;
            for (int x = 0; x < w; ++x) {
                const uint8_t* pixel = row + x * 4;
                if (pixel[3] && (pixel[0] || pixel[1] || pixel[2])) return false;
                icon.pixels[size_t(y) * w + x] = pixel[3];
            }
        }
        next_y = area.y2 + 1;
    }
    icon.image.header.magic = LV_IMAGE_HEADER_MAGIC;
    icon.image.header.cf = LV_COLOR_FORMAT_A8;
    icon.image.header.w = w;
    icon.image.header.h = h;
    icon.image.header.stride = w;
    icon.image.data = icon.pixels;
    icon.image.data_size = size_t(w) * h;
    icon.path = path;
    return true;
}
}

bool HomeIconSetFullSource(lv_obj_t* image, const char* path) try {
    auto* previous = static_cast<FullIcon*>(lv_obj_get_user_data(image));
    if (previous && previous->path == path) return true;
    auto next = std::make_unique<FullIcon>();
    if (!DecodeFullIcon(path, *next)) return false;
    lv_image_set_src(image, &next->image);
    if (!previous) {
        lv_obj_add_event_cb(image, [](lv_event_t* event) {
            auto* object = static_cast<lv_obj_t*>(lv_event_get_target(event));
            delete static_cast<FullIcon*>(lv_obj_get_user_data(object));
        }, LV_EVENT_DELETE, nullptr);
    }
    lv_obj_set_user_data(image, next.release());
    delete previous;
    return true;
} catch (const std::bad_alloc&) {
    return false;
}
