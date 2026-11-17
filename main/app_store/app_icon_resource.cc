#include "app_store_icons.h"
#include <cstring>
#include <new>
#include <esp_heap_caps.h>

namespace {
uint16_t U16(const unsigned char* p) { return p[0] | (uint16_t(p[1]) << 8); }
uint32_t U32(const unsigned char* p) { return U16(p) | (uint32_t(U16(p + 2)) << 16); }
}

std::shared_ptr<const AppStoreIcons::Icon> AppStoreIcons::Decode(const unsigned char* data, size_t size) {
    if (!data || size != 2064 || memcmp(data, "XZIC", 4) || U16(data + 4) != 1 ||
        U16(data + 6) != 128 || U16(data + 8) != 128 || U16(data + 10) != 1 || U32(data + 12) != 2048) return {};
    void* memory = heap_caps_malloc(sizeof(Icon), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!memory) return {};
    auto icon = std::shared_ptr<Icon>(new(memory) Icon, [](Icon* p) { p->~Icon(); heap_caps_free(p); });
    const unsigned char palette[] = {0, 0, 0, 255, 255, 255, 255, 0};
    memcpy(icon->pixels.data(), palette, sizeof(palette));
    for (size_t i = 0; i < 2048; ++i) icon->pixels[i + 8] = data[i + 16] ^ 255;
    icon->image.header.magic = LV_IMAGE_HEADER_MAGIC;
    icon->image.header.cf = LV_COLOR_FORMAT_I1;
    icon->image.header.w = 128;
    icon->image.header.h = 128;
    icon->image.header.stride = 16;
    icon->image.data_size = icon->pixels.size();
    icon->image.data = icon->pixels.data();
    return icon;
}

std::shared_ptr<const AppStoreIcons::Icon> AppStoreIcons::ReadElf(FILE* file, size_t size) {
    auto read = [file, size](uint32_t offset, void* data, size_t length) {
        return offset <= size && length <= size - offset && fseek(file, offset, SEEK_SET) == 0 &&
               fread(data, 1, length, file) == length;
    };
    unsigned char header[52];
    if (!file || !read(0, header, sizeof(header)) || memcmp(header, "\177ELF\1\1", 6) ||
        U16(header + 16) != 3 || U16(header + 18) != 243 || U16(header + 46) != 40) return {};
    const uint32_t table = U32(header + 32);
    const uint16_t count = U16(header + 48), names_index = U16(header + 50);
    if (!count || count > 512 || names_index >= count || table > size || uint32_t(count) * 40 > size - table) return {};
    unsigned char section[40];
    if (!read(table + uint32_t(names_index) * 40, section, sizeof(section)) || U32(section + 4) != 3) return {};
    const uint32_t names = U32(section + 16), names_size = U32(section + 20);
    if (names > size || names_size > size - names) return {};
    uint32_t icon_offset = 0;
    bool found = false;
    for (uint16_t i = 0; i < count; ++i) {
        if (!read(table + uint32_t(i) * 40, section, sizeof(section))) return {};
        const uint32_t name_offset = U32(section);
        char name[13];
        if (name_offset > names_size || sizeof(name) > names_size - name_offset) continue;
        if (!read(names + name_offset, name, sizeof(name))) return {};
        if (memcmp(name, ".xz.app.icon", sizeof(name))) continue;
        if (found || U32(section + 4) != 1 || (U32(section + 8) & (2 | 0x800)) || U32(section + 20) != 2064) return {};
        found = true;
        icon_offset = U32(section + 16);
    }
    unsigned char data[2064];
    return found && read(icon_offset, data, sizeof(data)) ? Decode(data, sizeof(data)) : nullptr;
}

void AppStoreIcons::SetSource(lv_obj_t* image, const void* source, int edge) {
    if (edge > 0 && edge <= 128 && lv_image_src_get_type(source) == LV_IMAGE_SRC_VARIABLE) {
        const auto* original = static_cast<const lv_image_dsc_t*>(source);
        if (original->header.cf == LV_COLOR_FORMAT_I1 && original->header.w == 128 && original->header.h == 128 &&
            original->header.stride >= 16 && original->data_size >= 8 + original->header.stride * 128 && edge != 128) {
            void* memory = heap_caps_malloc(sizeof(Icon), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
            if (memory) {
                auto* fitted = new(memory) Icon;
                memcpy(fitted->pixels.data(), original->data, 8);
                memset(fitted->pixels.data() + 8, 255, fitted->pixels.size() - 8);
                const int stride = (edge + 7) / 8;
                for (int y = 0; y < edge; ++y) for (int x = 0; x < edge; ++x) {
                    const int sx = x * 128 / edge, sy = y * 128 / edge;
                    if (!(original->data[8 + sy * original->header.stride + sx / 8] & (0x80 >> (sx & 7)))) {
                        fitted->pixels[8 + y * stride + x / 8] &= ~(0x80 >> (x & 7));
                    }
                }
                fitted->image.header = original->header;
                fitted->image.header.w = edge;
                fitted->image.header.h = edge;
                fitted->image.header.stride = stride;
                fitted->image.data = fitted->pixels.data();
                fitted->image.data_size = 8 + stride * edge;
                lv_image_set_src(image, &fitted->image);
                lv_obj_add_event_cb(image, [](lv_event_t* event) {
                    auto* icon = static_cast<Icon*>(lv_event_get_user_data(event));
                    icon->~Icon();
                    heap_caps_free(icon);
                }, LV_EVENT_DELETE, fitted);
                return;
            }
        }
    }
    lv_image_set_src(image, source);
}
