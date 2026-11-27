#include "resource_budget.h"
#include "plus_hal.h"
#include <string>
namespace plus_examples {
// Narrow example: verifies a conservative RX-only budget and coordinates P14.
// It does NOT announce microphone-ready or create UART/I2S. Actual Bluetooth chip
// handshake and sample arrival must succeed before recording is reported.
bool ResetSharedAudioForRx(std::size_t reserve_bytes, std::string* error) {
    const auto before = plus_services::ReadMemorySnapshot();
    std::size_t rx_bytes = 0;
    if (!plus_services::DmaBytes(6, 256, 2, 4, &rx_bytes) ||
        !plus_services::HasDmaBudget(before, rx_bytes, 2048, reserve_bytes)) {
        if (error) *error = "AUDIO_DMA_BUDGET";
        return false;
    }
    // Base HAL owns P14, panel/aux gates and settling. Do not hold its Flash
    // lease here, do not duplicate the 3s sleep while holding an I2C mutex.
    return plus_hal::ResetAudioPower(error);
}
}

