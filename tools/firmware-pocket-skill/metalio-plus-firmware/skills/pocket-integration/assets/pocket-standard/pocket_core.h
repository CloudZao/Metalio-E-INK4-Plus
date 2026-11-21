#pragma once

#include "pocket_layout.h"
#include <cstddef>
#include <cstdint>
#include <string>

namespace pocket {

constexpr std::uint32_t kMaximumImageBytes = kAppSize;
constexpr std::size_t kMaximumRecordBytes = 4096;

// IDs are stable single path components, never display names or filenames.
bool IsSafeIdentifier(const std::string &value, std::size_t maximum = 31);
bool IsAppIdentifier(const std::string &value);
bool IsAppVersion(const std::string &value);

struct Manifest {
    std::string id;
    std::string name;
    // Optional package information, at most 95 UTF-8 bytes in FPOCKET ABI 1.0.
    std::string description;
    std::string version;
    std::string target;
    std::string layout;
    std::string image;
    std::string sha256;
    std::string resources;
    std::string resource_font_sha256;
    std::string resource_ui_sha256;
    std::uint32_t size = 0;
    std::uint32_t state_version = 0;
    // 0=NONE, 1=CHECKPOINT, 2=DIRECT_TF. The application owns its save format.
    std::uint8_t storage_mode = 0;
    // 1=UI, 2=AI_DOUBLE, 3=AI_TRIPLE, 4=AI_HOLD, 5=CUSTOM; manager uses 0.
    std::uint8_t exit_method = 0;
    bool tf_required_standalone = false;
    std::string exit_hint;
};

enum class RecordStatus {
    Ok,
    Missing,
    InvalidArgument,
    IoError,
    Corrupt,
    UnsupportedSchema,
    GenerationOverflow,
};

struct RecordResult {
    RecordStatus status = RecordStatus::InvalidArgument;
    std::string payload;
    std::string error;
    std::uint32_t schema = 0;
    std::uint64_t generation = 0;
    bool degraded = false;
    int slot = -1;
    bool ok() const { return status == RecordStatus::Ok; }
};

// root_directory is an existing, trusted, app/user-specific directory provided
// by the platform adapter. This class never creates, renames or removes it.
// Each key uses key.a.pkr and key.b.pkr. No global or implicit fallback storage.
// Serialize access and storage mount changes in the caller's storage worker.
// A failed first write may remove only its own exclusively created partial slot,
// in the same call, after checking its bytes. Unknown corrupt files are preserved.
// fsync success + readback is not a claim of power-loss immunity for all cards.
class RecordStore {
  public:
    explicit RecordStore(std::string root_directory);
    RecordResult Load(const std::string &key, std::uint32_t max_schema) const;
    RecordResult Save(const std::string &key, std::uint32_t schema, const std::string &payload) const;
    // Optional application-private stronger policy only. Standard protocol keys
    // use ordinary Load/Save; they do NOT require two identical payloads.
    RecordResult LoadConfirmed(const std::string &key, std::uint32_t max_schema) const;
    RecordResult SaveConfirmed(const std::string &key, std::uint32_t schema,
                               const std::string &payload) const;

  private:
    std::string root_;
};

std::uint32_t Crc32(const void *data, std::size_t size);
const char *RecordStatusName(RecordStatus status);

} // namespace pocket
