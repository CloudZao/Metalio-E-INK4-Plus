#pragma once
#include "pocket_core.h"

namespace pocket {

// FPOCKET ABI 1.0 metadata is inside the SHA-protected application image,
// first segment, immediately following esp_app_desc_t, not an external TXT.
constexpr std::uint32_t kCompactMetadataOffset = 288;
// The compact standalone container boots a factory image at 64 KiB. This is
// a FILE/source layout; the manager still installs it at kAppAddress.
inline constexpr char kPocketAbiMagic[] = "FPOCKET-ABI-0001";
constexpr std::uint32_t kPocketManagerFlag = 1;
constexpr std::uint32_t kPocketApplicationFlag = 2;
// The application implements its own trigger. The pocket displays exit_hint;
// it does not capture another firmware's keys. UI means an obvious UI entry.
// Shortcut timing and confirmation are application responsibilities and must
// agree with exit_hint. These values follow public guide 1.1, section 3.
enum class ExitMethod : std::uint8_t {
    Manager = 0,
    Ui = 1,
    AiDouble = 2,
    AiTriple = 3,
    AiHold = 4,
    Custom = 5,
};
#pragma pack(push, 1)
struct CompactMetadata {
    char magic[16];
    std::uint16_t abi_major, abi_minor;
    std::uint32_t header_bytes, flags;
    std::uint8_t storage_mode, exit_method, tf_required_standalone, reserved0;
    std::uint32_t state_schema;
    char target[32], layout_profile[32], app_id[32], version[32];
    char name[64], description[96], exit_hint[96];
    std::uint8_t reserved[92];
};
#pragma pack(pop)
static_assert(sizeof(CompactMetadata) == 512, "FPOCKET descriptor ABI");
static_assert(offsetof(CompactMetadata, storage_mode) == 28);
static_assert(offsetof(CompactMetadata, target) == 36);
static_assert(offsetof(CompactMetadata, app_id) == 100);
static_assert(offsetof(CompactMetadata, exit_hint) == 324);

struct CompactImageInfo {
    Manifest manifest;
    bool manager = false;
    // File location of the application; Flash destination stays kAppAddress.
    std::uint32_t image_offset = 0;
};

// Accept pure applications and compatible standalone 0x0 full images. Probe
// checks descriptors and the full-image partition/OTA envelope, not payload
// integrity. Validate also checks boot/app hashes and empty reserved regions.
// Full containers may use the OTA1 layout or the compact factory layout.
// Both require the new ABI's dual-mode application identity; PCKTB104 is rejected.
// manifest.size and sha256 always describe ONLY the application bytes starting
// at image_offset; the importer must never write the full container to OTA1.
// Probe leaves manifest.sha256 empty.
// Neither function changes output on failure or performs any Flash writes.
bool ProbeCompactImage(const std::string &path, CompactImageInfo *output, std::string *error);
bool ValidateCompactImage(const std::string &path, CompactImageInfo *output, std::string *error);
// Read actual Flash bytes, derive exact ESP image length, verify checksum and
// appended SHA, metadata and descriptor consistency. available is partition
// capacity, never a length supplied by run JSON. No writes; output unchanged
// on failure. Driver read failures can be distinguished by the callback.
using ImageRead = bool (*)(void *context, std::uint32_t offset, void *out, std::size_t size);
bool ValidateRunningImage(ImageRead read, void *context, std::uint32_t available, CompactImageInfo *output,
                          std::string *error);

} // namespace pocket
