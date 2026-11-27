#pragma once
#include <cstddef>
#include <cstdint>

struct ImageProcessingOptions {
    unsigned levels = 16; // 输出灰阶数，2、8 或 16
    bool dither = false; // 仅两阶输出使用 4×4 有序抖点
    int contrast = 0; // 相对对比度，-100—100%
    int brightness = 0; // 满量程亮度偏移，-100—100%
    int exposure = 0; // 软件曝光，-200—200，单位 0.01 EV
    int shadows = 0; // 暗部调节，-100—100%
    int highlights = 0; // 亮部调节，-100—100%
    int gamma = 100; // Gamma，25—400，单位 0.01
    int sharpness = 0; // -100—0 为柔化，0—100 为锐化
    bool invert = false; // 灰度反相
    bool mirror_x = false; // 裁剪后水平镜像
    bool mirror_y = false; // 裁剪后垂直镜像
    unsigned rotation = 0; // 顺时针角度，0、90、180 或 270
    unsigned crop_x = 0, crop_y = 0; // 裁剪起点，原图宽高的万分比
    unsigned crop_width = 10000, crop_height = 10000; // 裁剪宽高，原图宽高的万分比
    unsigned threshold = 128; // 无抖点的两阶输出阈值
};

struct ImageProcessingLayout {
    int x = 0, y = 0; // 原图中的裁剪起点
    int width = 0, height = 0; // 像素对齐后的裁剪尺寸
    int output_width = 0, output_height = 0; // 旋转后的输出尺寸
};

/** @brief 校验显示处理参数并计算输出尺寸；输入边长不超过 1600，面积不超过 1 Mi 像素 */
bool GetImageProcessingLayout(int width, int height, const ImageProcessingOptions& options,
                              ImageProcessingLayout& layout);

/** @brief 将紧排 L8 原图处理到独立输出缓冲，内部仅使用固定大小查表，不分配整图缓存 */
bool ProcessImageL8(const uint8_t* source, size_t source_size, int width, int height,
                     const ImageProcessingOptions& options, uint8_t* target, size_t target_size);
