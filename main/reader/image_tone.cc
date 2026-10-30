#include "image_tone.h"

namespace reader {

void ApplyImageDarkening(uint8_t* pixels, size_t size) {
    if (!pixels) return;
    for (size_t i = 0; i < size; ++i) {
        pixels[i] = DarkenImagePixel(pixels[i]);
    }
}

} // namespace reader
