#pragma once

#include <lvgl.h>

/** @brief emoji 集合 stub（墨水屏不接彩色表情图） */
class EmojiCollection {
public:
    void AddEmoji(const char* /*name*/, const lv_image_dsc_t* /*image*/) {}
    const void* GetEmoji(const char* /*name*/) const {
        return nullptr;
    }
};
