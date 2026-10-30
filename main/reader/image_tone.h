#pragma once

#include <cstddef>
#include <cstdint>

namespace reader {

/** @brief 将 L8 图片灰度按固定 5 级（gamma 2.0）压暗，保持黑白端点 */
inline uint8_t DarkenImagePixel(uint8_t value) {
    return static_cast<uint8_t>((unsigned(value) * value + 127) / 255);
}

/** @brief 对显示副本应用固定 5 级压暗（gamma 2.0），保持黑白端点 */
void ApplyImageDarkening(uint8_t* pixels, size_t size);

} // namespace reader
