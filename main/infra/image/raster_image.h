#pragma once

#include <cstdint>
#include <vector>
#include <utility>
#include <lvgl.h>

// 拥有L8灰度像素的共享图像值；LVGL描述符由BindDsc绑定。
struct RasterImage {
    lv_image_dsc_t dsc{};         // 借用当前对象像素的界面描述符
    std::vector<uint8_t> pixels;  // 拥有像素；dsc.data 指向此处
    uint16_t width = 0;           // 像素宽度
    uint16_t height = 0;          // 像素高度

    RasterImage() = default;
    RasterImage(const RasterImage& other) : pixels(other.pixels), width(other.width), height(other.height) {
        BindDsc();
    }
    RasterImage(RasterImage&& other) noexcept
        : pixels(std::move(other.pixels)), width(other.width), height(other.height) {
        BindDsc();
        other.Reset();
    }
    RasterImage& operator=(const RasterImage& other) {
        if (this != &other) {
            pixels = other.pixels;
            width = other.width;
            height = other.height;
            BindDsc();
        }
        return *this;
    }
    RasterImage& operator=(RasterImage&& other) noexcept {
        if (this != &other) {
            pixels = std::move(other.pixels);
            width = other.width;
            height = other.height;
            BindDsc();
            other.Reset();
        }
        return *this;
    }

    bool empty() const {
        return pixels.empty() || width == 0 || height == 0;
    }

    void Reset() {
        pixels.clear();
        width = 0;
        height = 0;
        dsc = {};
    }

    void BindDsc() {
        if (empty()) {
            dsc = {};
            return;
        }
        dsc.header.magic = LV_IMAGE_HEADER_MAGIC;
        dsc.header.cf = LV_COLOR_FORMAT_L8;
        dsc.header.flags = 0;
        dsc.header.w = width;
        dsc.header.h = height;
        dsc.header.stride = width;
        dsc.data_size = pixels.size();
        dsc.data = pixels.data();
    }
};
