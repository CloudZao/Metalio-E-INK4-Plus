#pragma once

#include <cstddef>
#include <cstdint>

/**
 * @brief 在后台有界探测图片头，估计缩略图解码的相对成本
 * @note 权重仅用于初始排序，不表示预计等待时间；探测失败或取消时按后缀与文件长度保守回退，不代替正式解码校验。
 */
uint64_t Wallpaper_EstimateThumbCost(const char* path, size_t bytes,
                                     bool (*cancelled)(void*), void* user);
