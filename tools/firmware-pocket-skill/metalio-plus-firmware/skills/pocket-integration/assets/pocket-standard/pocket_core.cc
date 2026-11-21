#include "pocket_core.h"

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <limits>
#include <map>
#include <memory>
#include <new>
#include <sys/stat.h>
#include <unistd.h>
#include <utility>

namespace pocket {
namespace {

constexpr std::size_t kHeaderSize = 32;
constexpr unsigned char kMagic[8] = {'P', 'K', 'R', 'E', 'C', '0', '1', 0};

bool Fail(std::string *error, const char *message) {
    if (error != nullptr)
        *error = message;
    return false;
}

bool Version(const std::string &value) {
    if (value.empty() || value.size() > 31)
        return false;
    for (char c : value) {
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '.' ||
              c == '-' || c == '_' || c == '+'))
            return false;
    }
    return true;
}

std::uint32_t UpdateCrc(std::uint32_t crc, const unsigned char *data, std::size_t size) {
    for (std::size_t i = 0; i < size; ++i) {
        crc ^= data[i];
        for (unsigned bit = 0; bit < 8; ++bit) {
            crc = (crc >> 1) ^ (0xedb88320U & (0U - (crc & 1U)));
        }
    }
    return crc;
}

std::uint32_t Get32(const unsigned char *p) {
    return static_cast<std::uint32_t>(p[0]) | (static_cast<std::uint32_t>(p[1]) << 8) |
           (static_cast<std::uint32_t>(p[2]) << 16) | (static_cast<std::uint32_t>(p[3]) << 24);
}

std::uint64_t Get64(const unsigned char *p) {
    return Get32(p) | (static_cast<std::uint64_t>(Get32(p + 4)) << 32);
}

void Put32(unsigned char *p, std::uint32_t value) {
    for (unsigned i = 0; i < 4; ++i)
        p[i] = static_cast<unsigned char>(value >> (8 * i));
}

void Put64(unsigned char *p, std::uint64_t value) {
    Put32(p, static_cast<std::uint32_t>(value));
    Put32(p + 4, static_cast<std::uint32_t>(value >> 32));
}

RecordResult Result(RecordStatus status, const char *error) {
    RecordResult result;
    result.status = status;
    result.error = error;
    return result;
}

std::string SlotPath(const std::string &root, const std::string &key, int slot) {
    return root + "/" + key + (slot == 0 ? ".a.pkr" : ".b.pkr");
}

bool ValidRoot(const std::string &root) {
    return !root.empty() && root.size() <= 512 && root.find('\0') == std::string::npos &&
           root.find('\n') == std::string::npos && root.find('\r') == std::string::npos;
}

RecordResult ReadSlot(const std::string &path, int slot) {
    int flags = O_RDONLY;
#if defined(O_NOFOLLOW) && !defined(ESP_PLATFORM)
    // ESP-IDF FATFS has no symbolic links and ignores O_NOFOLLOW. On hosts that
    // do support links, refuse them instead of following paths out of the root.
    flags |= O_NOFOLLOW;
#endif
    const int descriptor = open(path.c_str(), flags);
    if (descriptor < 0) {
        return Result(errno == ENOENT ? RecordStatus::Missing : RecordStatus::IoError,
                      errno == ENOENT ? "record absent" : "cannot open record");
    }
    struct stat info{};
    if (fstat(descriptor, &info) != 0 || !S_ISREG(info.st_mode)) {
        close(descriptor);
        return Result(RecordStatus::IoError, "record is not a readable regular file");
    }
    if (info.st_size < static_cast<off_t>(kHeaderSize) ||
        info.st_size > static_cast<off_t>(kHeaderSize + kMaximumRecordBytes)) {
        close(descriptor);
        return Result(RecordStatus::Corrupt, "invalid record length");
    }
    const auto size = static_cast<std::size_t>(info.st_size);
    std::unique_ptr<unsigned char[]> bytes(new (std::nothrow) unsigned char[size]{});
    if (!bytes) {
        close(descriptor);
        return Result(RecordStatus::IoError, "record buffer allocation failed");
    }
    std::size_t received = 0;
    while (received < size) {
        const auto count = read(descriptor, bytes.get() + received, size - received);
        if (count < 0 && errno == EINTR)
            continue;
        if (count < 0) {
            close(descriptor);
            return Result(RecordStatus::IoError, "record read failed");
        }
        if (count == 0) {
            close(descriptor);
            return Result(RecordStatus::Corrupt, "record truncated while reading");
        }
        received += static_cast<std::size_t>(count);
    }
    unsigned char extra = 0;
    ssize_t trailing;
    do {
        trailing = read(descriptor, &extra, 1);
    } while (trailing < 0 && errno == EINTR);
    const int close_result = close(descriptor);
    if (trailing < 0 || close_result != 0)
        return Result(RecordStatus::IoError, "record read/close failed");
    if (trailing != 0)
        return Result(RecordStatus::Corrupt, "record changed while reading");
    if (std::memcmp(bytes.get(), kMagic, sizeof(kMagic)) != 0) {
        if (std::memcmp(bytes.get(), "PKREC", 5) == 0)
            return Result(RecordStatus::UnsupportedSchema,
                          "unrecognized record envelope; originals preserved");
        return Result(RecordStatus::Corrupt, "damaged record envelope");
    }
    if (bytes[8] != 1 || bytes[9] != 0 || bytes[10] != 0 || bytes[11] != 0)
        return Result(RecordStatus::UnsupportedSchema,
                      "unrecognized record envelope version; originals preserved");
    const auto schema = Get32(bytes.get() + 12);
    const auto generation = Get64(bytes.get() + 16);
    const auto payload_size = Get32(bytes.get() + 24);
    if (schema == 0 || generation == 0 || payload_size > kMaximumRecordBytes ||
        size != kHeaderSize + payload_size)
        return Result(RecordStatus::Corrupt, "invalid record fields");
    auto crc = UpdateCrc(0xffffffffU, bytes.get(), 28);
    crc = UpdateCrc(crc, bytes.get() + kHeaderSize, payload_size) ^ 0xffffffffU;
    if (crc != Get32(bytes.get() + 28))
        return Result(RecordStatus::Corrupt, "record checksum mismatch");
    RecordResult result;
    result.status = RecordStatus::Ok;
    result.schema = schema;
    result.generation = generation;
    result.slot = slot;
    result.payload.assign(reinterpret_cast<const char *>(bytes.get() + kHeaderSize), payload_size);
    return result;
}

// Only the caller that successfully used O_EXCL while both slots were absent
// may invoke this, before returning from that write attempt. This is not a
// general corruption repair: after reboot there is no ownership evidence.
bool RemoveOwnedPartial(const std::string &path, const std::string &other_path, const struct stat &created,
                        const std::array<unsigned char, kHeaderSize> &header, const std::string &payload) {
    if (ReadSlot(other_path, 1).status != RecordStatus::Missing)
        return false;
    if (ReadSlot(path, 0).status != RecordStatus::Corrupt)
        return false;
    int flags = O_RDONLY;
#if defined(O_NOFOLLOW) && !defined(ESP_PLATFORM)
    flags |= O_NOFOLLOW;
#endif
    const int descriptor = open(path.c_str(), flags);
    if (descriptor < 0)
        return false;
    struct stat current{};
    bool owned = fstat(descriptor, &current) == 0 && S_ISREG(current.st_mode) && current.st_size >= 0 &&
                 current.st_size < static_cast<off_t>(kHeaderSize + payload.size());
#ifndef ESP_PLATFORM
    // FATFS reports no inode identity; its safety evidence is exclusive creation,
    // serialized lifecycle/access, exact byte prefix and absence of the other slot.
    owned = owned && current.st_dev == created.st_dev && current.st_ino == created.st_ino;
#else
    (void)created;
#endif
    std::array<unsigned char, 256> bytes{};
    std::size_t offset = 0;
    while (owned && offset < static_cast<std::size_t>(current.st_size)) {
        const auto count = read(descriptor, bytes.data(),
                                std::min(bytes.size(), static_cast<std::size_t>(current.st_size) - offset));
        if (count < 0 && errno == EINTR)
            continue;
        if (count <= 0) {
            owned = false;
            break;
        }
        for (ssize_t i = 0; i < count; ++i) {
            const auto at = offset + static_cast<std::size_t>(i);
            const unsigned char expected =
                at < kHeaderSize ? header[at] : static_cast<unsigned char>(payload[at - kHeaderSize]);
            if (bytes[static_cast<std::size_t>(i)] != expected) {
                owned = false;
                break;
            }
        }
        offset += static_cast<std::size_t>(count);
    }
    if (close(descriptor) != 0)
        owned = false;
#ifndef ESP_PLATFORM
    struct stat final_identity{};
    owned = owned && lstat(path.c_str(), &final_identity) == 0 && S_ISREG(final_identity.st_mode) &&
            final_identity.st_dev == created.st_dev && final_identity.st_ino == created.st_ino &&
            final_identity.st_size == current.st_size;
#endif
    return owned && unlink(path.c_str()) == 0;
}

RecordResult SelectRecord(const RecordResult &a, const RecordResult &b, std::uint32_t maximum_schema) {
    // An unreadable slot might contain a newer record; never silently overwrite it.
    if (a.status == RecordStatus::IoError || b.status == RecordStatus::IoError)
        return Result(RecordStatus::IoError, "at least one record slot is unreadable");
    // A newer envelope cannot be ordered reliably by fields we do not understand.
    if (a.status == RecordStatus::UnsupportedSchema || b.status == RecordStatus::UnsupportedSchema)
        return Result(RecordStatus::UnsupportedSchema, "unrecognized record copy; originals preserved");
    if (!a.ok() && !b.ok()) {
        if (a.status == RecordStatus::Missing && b.status == RecordStatus::Missing)
            return Result(RecordStatus::Missing, "record absent");
        return Result(RecordStatus::Corrupt, "no valid record copy; originals preserved");
    }
    if (a.ok() && b.ok() && a.generation == b.generation && (a.schema != b.schema || a.payload != b.payload))
        return Result(RecordStatus::Corrupt, "conflicting copies with the same generation");
    RecordResult result = a.ok() && (!b.ok() || a.generation >= b.generation) ? a : b;
    result.degraded = a.status == RecordStatus::Corrupt || b.status == RecordStatus::Corrupt;
    if (result.schema > maximum_schema) {
        result.status = RecordStatus::UnsupportedSchema;
        result.error = "newest record schema is newer than this application; originals preserved";
        result.payload.clear();
    }
    return result;
}

} // namespace

bool IsSafeIdentifier(const std::string &value, std::size_t maximum) {
    if (value.empty() || value.size() > maximum || value.back() == '.')
        return false;
    const auto alphanumeric = [](char c) { return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9'); };
    if (!alphanumeric(value.front()))
        return false;
    for (char c : value) {
        if (!alphanumeric(c) && c != '.' && c != '_' && c != '-')
            return false;
    }
    return true;
}

bool IsAppIdentifier(const std::string &value) {
    if (value.empty() || value.size() > 31 || value[0] < 'a' || value[0] > 'z')
        return false;
    for (char c : value)
        if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_'))
            return false;
    return true;
}
bool IsAppVersion(const std::string &value) { return Version(value); }

RecordStore::RecordStore(std::string root_directory) : root_(std::move(root_directory)) {}

RecordResult RecordStore::Load(const std::string &key, std::uint32_t max_schema) const {
    if (!ValidRoot(root_) || !IsSafeIdentifier(key) || max_schema == 0)
        return Result(RecordStatus::InvalidArgument, "invalid storage root, key or schema");
    // A missing mount/directory is storage failure, never a new user's empty state.
    struct stat info{};
    if (stat(root_.c_str(), &info) != 0 || !S_ISDIR(info.st_mode))
        return Result(RecordStatus::IoError, "record directory unavailable");
    return SelectRecord(ReadSlot(SlotPath(root_, key, 0), 0), ReadSlot(SlotPath(root_, key, 1), 1),
                        max_schema);
}

RecordResult RecordStore::Save(const std::string &key, std::uint32_t schema,
                               const std::string &payload) const {
    if (payload.size() > kMaximumRecordBytes)
        return Result(RecordStatus::InvalidArgument, "record payload exceeds 4096 bytes");
    const auto previous = Load(key, schema);
    if (!previous.ok() && previous.status != RecordStatus::Missing)
        return previous;
    if (previous.generation == std::numeric_limits<std::uint64_t>::max())
        return Result(RecordStatus::GenerationOverflow, "record generation exhausted");
    const int target = previous.ok() ? 1 - previous.slot : 0;
    const std::uint64_t generation = previous.generation + 1;
    std::array<unsigned char, kHeaderSize> header{};
    std::memcpy(header.data(), kMagic, sizeof(kMagic));
    header[8] = 1;
    Put32(header.data() + 12, schema);
    Put64(header.data() + 16, generation);
    Put32(header.data() + 24, static_cast<std::uint32_t>(payload.size()));
    auto crc = UpdateCrc(0xffffffffU, header.data(), 28);
    crc =
        UpdateCrc(crc, reinterpret_cast<const unsigned char *>(payload.data()), payload.size()) ^ 0xffffffffU;
    Put32(header.data() + 28, crc);
    const auto path = SlotPath(root_, key, target);
    const bool first_write = previous.status == RecordStatus::Missing;
    int flags = O_WRONLY | O_CREAT | (first_write ? O_EXCL : O_TRUNC);
#if defined(O_NOFOLLOW) && !defined(ESP_PLATFORM)
    flags |= O_NOFOLLOW;
#endif
    const int descriptor = open(path.c_str(), flags, 0600);
    if (descriptor < 0)
        return Result(RecordStatus::IoError, "cannot open alternate record slot");
    struct stat created{};
    const bool owned_new_file =
        first_write && fstat(descriptor, &created) == 0 && S_ISREG(created.st_mode) && created.st_size == 0;
    const auto failed_write = [&]() {
        const bool cleaned = owned_new_file && RemoveOwnedPartial(path, SlotPath(root_, key, 1 - target),
                                                                  created, header, payload);
        return Result(RecordStatus::IoError,
                      cleaned ? "write failed; own new partial slot removed, retry after restoring storage"
                              : "record write, flush, sync or close failed; existing files preserved");
    };
    FILE *file = fdopen(descriptor, "wb");
    if (file == nullptr) {
        close(descriptor);
        return failed_write();
    }
    bool success = fwrite(header.data(), 1, header.size(), file) == header.size();
    if (success && !payload.empty())
        success = fwrite(payload.data(), 1, payload.size(), file) == payload.size();
    if (success)
        success = fflush(file) == 0;
    if (success)
        success = fsync(fileno(file)) == 0;
    // Always close, including error paths. A close failure cannot be reported as saved.
    if (fclose(file) != 0)
        success = false;
    if (!success)
        return failed_write();
    auto verified = ReadSlot(path, target);
    if (!verified.ok() || verified.schema != schema || verified.generation != generation ||
        verified.payload != payload)
        return Result(RecordStatus::IoError, "written record failed readback verification");
    return verified;
}

RecordResult RecordStore::LoadConfirmed(const std::string &key, std::uint32_t max_schema) const {
    auto ordinary = Load(key, max_schema);
    if (!ordinary.ok())
        return ordinary;
    const auto a = ReadSlot(SlotPath(root_, key, 0), 0);
    const auto b = ReadSlot(SlotPath(root_, key, 1), 1);
    if (!a.ok() || !b.ok() || a.schema != b.schema || a.payload != b.payload)
        return Result(RecordStatus::Corrupt, "control copies disagree; overwrite is blocked");
    ordinary.degraded = false;
    return ordinary;
}

RecordResult RecordStore::SaveConfirmed(const std::string &key, std::uint32_t schema,
                                        const std::string &payload) const {
    // While either commit is incomplete the reader blocks, rather than using
    // the previous acknowledgement. Callers do not resume business until both
    // copies agree. General business snapshots keep ordinary fallback semantics.
    auto result = Save(key, schema, payload);
    if (!result.ok())
        return result;
    result = Save(key, schema, payload);
    if (!result.ok())
        return result;
    return LoadConfirmed(key, schema);
}

std::uint32_t Crc32(const void *data, std::size_t size) {
    return UpdateCrc(0xffffffffU, static_cast<const unsigned char *>(data), size) ^ 0xffffffffU;
}

const char *RecordStatusName(RecordStatus status) {
    switch (status) {
    case RecordStatus::Ok:
        return "ok";
    case RecordStatus::Missing:
        return "missing";
    case RecordStatus::InvalidArgument:
        return "invalid_argument";
    case RecordStatus::IoError:
        return "io_error";
    case RecordStatus::Corrupt:
        return "corrupt";
    case RecordStatus::UnsupportedSchema:
        return "unsupported_schema";
    case RecordStatus::GenerationOverflow:
        return "generation_overflow";
    }
    return "unknown";
}

} // namespace pocket
