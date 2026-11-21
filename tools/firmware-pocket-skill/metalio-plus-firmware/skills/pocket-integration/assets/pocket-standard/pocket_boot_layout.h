#pragma once
#include "pocket_layout.h"
#include <cstdint>

namespace pocket {
// Source-container positions never change the integrated target partition.
inline constexpr std::uint32_t kStandaloneAppAddress = kCompactStandaloneOffset;
inline constexpr std::uint32_t kStandaloneCapability = 2;
inline constexpr std::uint8_t kFactorySubtype = 0;
inline constexpr std::uint8_t kOta1Subtype = 0x11;

struct BootPartition {
    std::uint32_t address = 0;
    std::uint32_t size = 0;
};
struct AppBootPartitions {
    BootPartition nvs, otadata, phy, factory, manager, app;
};
enum class AppBootLayout { Invalid, CompactFactory, PocketSlots };

constexpr bool PartitionMatches(BootPartition p, std::uint32_t address, std::uint32_t size) {
    return p.address == address && p.size == size;
}
constexpr bool PartitionAbsent(BootPartition p) { return p.address == 0 && p.size == 0; }

// A factory app can occupy the same physical address as a manager, but is not
// one. Require both the partition type and complete expected address/size set.
constexpr AppBootLayout ClassifyAppBootLayout(const AppBootPartitions& p,
                                              std::uint8_t running_subtype,
                                              std::uint32_t running_address) {
    if (!PartitionMatches(p.nvs, kNvsAddress, kNvsSize) ||
        !PartitionMatches(p.otadata, kOtaDataAddress, kOtaDataSize) ||
        !PartitionMatches(p.phy, kPhyAddress, kPhySize)) return AppBootLayout::Invalid;
    if (PartitionMatches(p.factory, kStandaloneAppAddress, kAppSize) &&
        PartitionAbsent(p.manager) && PartitionAbsent(p.app) &&
        running_subtype == kFactorySubtype && running_address == kStandaloneAppAddress)
        return AppBootLayout::CompactFactory;
    if (PartitionAbsent(p.factory) &&
        PartitionMatches(p.manager, kManagerAddress, kManagerSize) &&
        PartitionMatches(p.app, kAppAddress, kAppSize) &&
        running_subtype == kOta1Subtype && running_address == kAppAddress)
        return AppBootLayout::PocketSlots;
    return AppBootLayout::Invalid;
}
} // namespace pocket
