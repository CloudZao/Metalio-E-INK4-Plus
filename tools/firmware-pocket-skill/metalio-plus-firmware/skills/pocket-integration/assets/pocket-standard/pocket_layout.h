#pragma once
#include <cstdint>
#define POCKET_TARGET_ID "metalio-eink4-plus"
#define POCKET_LAYOUT_ID "metalio16m-plus-pocket2"

namespace pocket {
// Metalio E-INK4 Plus binding. Source-file offsets and Flash destinations are
// deliberately separate. The ABI record layout remains FPOCKET ABI 1.0.
inline constexpr char kTargetId[] = POCKET_TARGET_ID;
// ESP-IDF v6.1 esp_app_format.h: ESP_CHIP_ID_ESP32S31 = 0x0020.
inline constexpr std::uint16_t kImageChipId = 0x0020;
inline constexpr std::uint8_t kImageFlashMode = 2; // DIO, verified in real S31 boot/app images
inline constexpr std::uint8_t kImageFlashSizeSpeed = 0x4f; // 16 MiB / 80 MHz
// ESP32-S31 ROM reserves the first 8 KiB. An independent 0x0 BIN carries FF
// padding here and its bootloader at 0x2000, not at zero as the old S3 binding.
inline constexpr std::uint32_t kBootloaderOffset = 0x2000;
inline constexpr std::uint32_t kPartitionTableOffset = 0x8000;
inline constexpr std::uint32_t kNvsAddress = 0x9000;
inline constexpr std::uint32_t kNvsSize = 0x4000;
inline constexpr std::uint32_t kOtaDataAddress = 0xd000;
inline constexpr std::uint32_t kOtaDataSize = 0x2000;
inline constexpr std::uint32_t kPhyAddress = 0xf000;
inline constexpr std::uint32_t kPhySize = 0x1000;
inline constexpr std::uint32_t kCompactStandaloneOffset = 0x10000;
// Fixed manager layout v2. Application fonts/resources belong to each BIN.
inline constexpr std::uint32_t kManagerAddress = 0x10000;
inline constexpr std::uint32_t kManagerSize = 0x2f0000;
inline constexpr std::uint32_t kAppAddress = 0x300000;
inline constexpr std::uint32_t kAppSize = 0xd00000;
inline constexpr char kLayoutId[] = POCKET_LAYOUT_ID;
static_assert(kManagerAddress + kManagerSize == kAppAddress);
static_assert(kAppAddress + kAppSize == 16U * 1024U * 1024U);
}
