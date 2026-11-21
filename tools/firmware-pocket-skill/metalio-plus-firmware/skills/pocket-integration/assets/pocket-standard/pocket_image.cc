#include "pocket_image.h"
#include "pocket_hash.h"
#include "pocket_layout.h"
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <sys/stat.h>
#include <utility>
#ifdef ESP_PLATFORM
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#endif

namespace pocket {
namespace {
bool Fail(std::string *error, const char *message) {
    if (error)
        *error = message;
    return false;
}
uint32_t U32(const unsigned char *p) {
    return uint32_t(p[0]) | uint32_t(p[1]) << 8 | uint32_t(p[2]) << 16 | uint32_t(p[3]) << 24;
}
bool String(const char *input, size_t capacity, std::string *output, bool empty = false) {
    size_t length = 0;
    while (length < capacity && input[length])
        ++length;
    if (length == capacity || (!empty && !length))
        return false;
    for (size_t i = length; i < capacity; ++i)
        if (input[i])
            return false;
    // Reject malformed/overlong UTF-8, surrogates and controls before LVGL sees it.
    for (size_t i = 0; i < length;) {
        unsigned char c = input[i++];
        if (c < 32 || c == 127)
            return false;
        if (c < 128)
            continue;
        unsigned count = 0;
        uint32_t value = 0, minimum = 0;
        if (c >= 0xc2 && c <= 0xdf) {
            count = 1;
            value = c & 31;
            minimum = 0x80;
        } else if (c >= 0xe0 && c <= 0xef) {
            count = 2;
            value = c & 15;
            minimum = 0x800;
        } else if (c >= 0xf0 && c <= 0xf4) {
            count = 3;
            value = c & 7;
            minimum = 0x10000;
        } else
            return false;
        if (count > length - i)
            return false;
        while (count--) {
            c = input[i++];
            if ((c & 0xc0) != 0x80)
                return false;
            value = (value << 6) | (c & 63);
        }
        if (value < minimum || value > 0x10ffff || (value >= 0xd800 && value <= 0xdfff) ||
            (value >= 0x80 && value <= 0x9f))
            return false;
    }
    output->assign(input, length);
    return true;
}
bool Identifier(const std::string &text) {
    if (text.empty() || text.size() > 31 || text[0] < 'a' || text[0] > 'z')
        return false;
    for (char c : text)
        if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_'))
            return false;
    return true;
}
bool Version(const std::string &text) {
    if (text.empty() || text.size() > 31)
        return false;
    for (char c : text)
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' ||
              c == '.' || c == '+' || c == '-'))
            return false;
    return true;
}
void YieldImageReader() {
#ifdef ESP_PLATFORM
    vTaskDelay(1);
#endif
}
FILE *Open(const std::string &path, uint32_t *size) {
    struct stat st{};
    if (stat(path.c_str(), &st) || !S_ISREG(st.st_mode) || st.st_size < 0 ||
        static_cast<uint64_t>(st.st_size) > kAppAddress + kAppSize)
        return nullptr;
    *size = static_cast<uint32_t>(st.st_size);
    return fopen(path.c_str(), "rb");
}
bool ProbeMetadata(const unsigned char *prefix, const CompactMetadata &meta, uint32_t offset, uint32_t size,
                   const std::string &path, CompactImageInfo *output, std::string *error) {
    if (prefix[0] != 0xe9 || prefix[1] < 1 || prefix[1] > 16 || prefix[2] != kImageFlashMode ||
        prefix[3] != kImageFlashSizeSpeed ||
        (uint16_t(prefix[12]) | uint16_t(prefix[13]) << 8) != kImageChipId || prefix[23] != 1 ||
        U32(prefix + 28) < 256 + sizeof(meta) || U32(prefix + 32) != 0xabcd5432)
        return Fail(error, "不是兼容的 ESP32-S31 Plus 应用 BIN");
    if (!memcmp(meta.magic, "PCKTB104", 8))
        return Fail(error, "旧版 BIN 尚未适配本版接入协议，请使用新版部件");
    if (memcmp(meta.magic, kPocketAbiMagic, sizeof(meta.magic)) || meta.abi_major != 1 ||
        meta.abi_minor != 0 || meta.header_bytes != sizeof(meta) ||
        (meta.flags != kPocketManagerFlag && meta.flags != kPocketApplicationFlag))
        return Fail(error, "BIN 未适配本版接入协议");
    if (meta.reserved0 || meta.tf_required_standalone > 1 || meta.storage_mode > 2 ||
        meta.state_schema > 0x7fffffffU ||
        (meta.storage_mode == 0 ? meta.state_schema != 0 : meta.state_schema == 0))
        return Fail(error, "BIN 存档方式声明不合法");
    for (auto c : meta.reserved)
        if (c)
            return Fail(error, "BIN 包含未知扩展信息");
    CompactImageInfo result;
    result.image_offset = offset;
    auto &m = result.manifest;
    if (!String(meta.target, sizeof(meta.target), &m.target) || m.target != kTargetId ||
        !String(meta.layout_profile, sizeof(meta.layout_profile), &m.layout) || m.layout != kLayoutId ||
        !String(meta.app_id, sizeof(meta.app_id), &m.id) || !Identifier(m.id) ||
        !String(meta.version, sizeof(meta.version), &m.version) || !Version(m.version) ||
        !String(meta.name, sizeof(meta.name), &m.name) ||
        !String(meta.description, sizeof(meta.description), &m.description, true) ||
        !String(meta.exit_hint, sizeof(meta.exit_hint), &m.exit_hint, meta.flags == kPocketManagerFlag))
        return Fail(error, "BIN 名称、身份或简介不合法");
    std::string id, version;
    if (!String(reinterpret_cast<const char *>(prefix + 48), 32, &version) ||
        !String(reinterpret_cast<const char *>(prefix + 80), 32, &id) || id != m.id || version != m.version)
        return Fail(error, "BIN 内嵌信息与实际编译信息不符");
    result.manager = meta.flags == kPocketManagerFlag;
    if (result.manager != (m.id == "pocket_manager") || size > (result.manager ? kManagerSize : kAppSize))
        return Fail(error, "BIN 角色或容量不符合本版布局");
    if (result.manager ? (meta.storage_mode != 0 || meta.exit_method != 0 || !m.exit_hint.empty())
                       : (meta.exit_method < 1 || meta.exit_method > 5))
        return Fail(error, "BIN 退出方式声明不合法");
    m.resources = "self-contained";
    m.state_version = meta.state_schema;
    m.storage_mode = meta.storage_mode;
    m.exit_method = meta.exit_method;
    m.tf_required_standalone = meta.tf_required_standalone != 0;
    m.size = size;
    const auto slash = path.find_last_of("/\\");
    m.image = path.substr(slash == std::string::npos ? 0 : slash + 1);
    *output = std::move(result);
    if (error)
        error->clear();
    return true;
}
bool ProbeApp(FILE *file, uint32_t offset, uint32_t size, const std::string &path, CompactImageInfo *output,
              std::string *error) {
    unsigned char prefix[kCompactMetadataOffset]{};
    CompactMetadata meta{};
    if (size < sizeof(prefix) + sizeof(meta) + 32 || fseek(file, offset, SEEK_SET) ||
        fread(prefix, 1, sizeof(prefix), file) != sizeof(prefix) ||
        fread(&meta, 1, sizeof(meta), file) != sizeof(meta))
        return Fail(error, "cannot read image metadata");
    return ProbeMetadata(prefix, meta, offset, size, path, output, error);
}
bool EspHeader(const unsigned char *h) {
    return h[0] == 0xe9 && h[1] >= 1 && h[1] <= 16 && h[2] == kImageFlashMode &&
           h[3] == kImageFlashSizeSpeed && (uint16_t(h[12]) | uint16_t(h[13]) << 8) == kImageChipId &&
           h[23] == 1;
}
bool Verify(FILE *file, uint32_t offset, uint32_t available, bool exact, uint32_t *image_bytes,
            std::string *full_hash, std::string *error) {
    unsigned char header[24], bytes[4096], segment[8];
    if (available < 64 || fseek(file, offset, SEEK_SET) ||
        fread(header, 1, sizeof(header), file) != sizeof(header) || !EspHeader(header))
        return Fail(error, "不是兼容的 ESP32-S31 Plus 镜像");
    uint32_t position = sizeof(header);
    unsigned char checksum = 0xef;
    for (unsigned i = 0; i < header[1]; ++i) {
        if (position > available - 32 - sizeof(segment) ||
            fread(segment, 1, sizeof(segment), file) != sizeof(segment))
            return Fail(error, "BIN 数据段被截断");
        position += sizeof(segment);
        uint32_t remaining = U32(segment + 4);
        if ((remaining & 3) || remaining > available - 32 - position)
            return Fail(error, "BIN 数据段长度错误");
        while (remaining) {
            size_t n = std::min<size_t>(remaining, sizeof(bytes));
            if (fread(bytes, 1, n, file) != n)
                return Fail(error, "读取 BIN 数据失败");
            for (size_t j = 0; j < n; ++j)
                checksum ^= bytes[j];
            remaining -= n;
            position += n;
            if ((position & 0xffff) < n)
                YieldImageReader();
        }
    }
    const uint32_t digest_offset = (position + 16) & ~15U;
    if (digest_offset > available - 32 || (exact && digest_offset + 32 != available) ||
        fseek(file, offset + digest_offset - 1, SEEK_SET) || fgetc(file) != checksum)
        return Fail(error, "BIN 校验和或总长度错误");
    unsigned char expected[32], actual[32], whole[32];
    if (fread(expected, 1, sizeof(expected), file) != sizeof(expected) || fseek(file, offset, SEEK_SET))
        return Fail(error, "读取 BIN 摘要失败");
    Sha256 hash, full;
    int result = 0;
    uint32_t remaining = digest_offset;
    while (remaining && !result) {
        size_t n = std::min<size_t>(remaining, sizeof(bytes));
        if (fread(bytes, 1, n, file) != n) {
            result = -1;
            break;
        }
        result = hash.Add(bytes, n) && full.Add(bytes, n) ? 0 : -1;
        remaining -= n;
        if ((remaining & 0xffff) < n)
            YieldImageReader();
    }
    if (!result)
        result = hash.Finish(actual) ? 0 : -1;
    if (!result)
        result = full.Add(expected, sizeof(expected)) ? 0 : -1;
    if (!result)
        result = full.Finish(whole) ? 0 : -1;
    if (result || memcmp(expected, actual, 32))
        return Fail(error, "BIN SHA256 校验失败");
    char text[65];
    for (unsigned i = 0; i < 32; ++i)
        snprintf(text + i * 2, 3, "%02x", whole[i]);
    *full_hash = text;
    *image_bytes = digest_offset + 32;
    return true;
}
bool AllFF(const unsigned char *data, size_t size) {
    for (size_t i = 0; i < size; ++i)
        if (data[i] != 0xff)
            return false;
    return true;
}
bool EmptyRegion(FILE *file, uint32_t begin, uint32_t end, std::string *error) {
    if (begin > end || fseek(file, begin, SEEK_SET))
        return Fail(error, "无法检查整机 BIN 空白区");
    unsigned char bytes[4096];
    while (begin < end) {
        const size_t n = std::min<uint32_t>(end - begin, sizeof(bytes));
        if (fread(bytes, 1, n, file) != n || !AllFF(bytes, n))
            return Fail(error, "整机 BIN 含公共配置或管理器内容，拒绝导入");
        begin += n;
        if ((begin & 0xffff) < n)
            YieldImageReader();
    }
    return true;
}
bool FullEnvelope(FILE *file, uint32_t *app_offset, std::string *error) {
    // Two fixed public layouts: table entries and MD5 are generated by ESP-IDF.
    // Strict labels/types/offsets prevent untrusted containers choosing writes.
    struct Expected {
        const char *label;
        unsigned char type, subtype;
        uint32_t offset, size;
    };
    Expected expected[] = {{"nvs", 1, 2, kNvsAddress, kNvsSize},
                           {"otadata", 1, 0, kOtaDataAddress, kOtaDataSize},
                           {"phy_init", 1, 1, kPhyAddress, kPhySize},
                           {"ota_0", 0, 0x10, kManagerAddress, kManagerSize},
                           {"ota_1", 0, 0x11, kAppAddress, kAppSize}};
    unsigned char table[4096];
    if (fseek(file, kPartitionTableOffset, SEEK_SET) || fread(table, 1, sizeof(table), file) != sizeof(table))
        return Fail(error, "无法读取整机 BIN 分区表");
    const bool compact = table[98] == 0 && table[99] == 0;
    const size_t count = compact ? 4 : 5;
    if (compact)
        expected[3] = {"factory", 0, 0, kCompactStandaloneOffset, kAppSize};
    for (size_t i = 0; i < count; ++i) {
        const auto *e = table + 32 * i;
        std::string label;
        if (e[0] != 0xaa || e[1] != 0x50 || e[2] != expected[i].type || e[3] != expected[i].subtype ||
            U32(e + 4) != expected[i].offset || U32(e + 8) != expected[i].size || U32(e + 28) != 0 ||
            !String(reinterpret_cast<const char *>(e + 12), 16, &label) || label != expected[i].label)
            return Fail(error, "整机 BIN 分区与口袋不兼容");
    }
    const size_t md5_offset = count * 32;
    unsigned char digest[16];
    if (table[md5_offset] != 0xeb || table[md5_offset + 1] != 0xeb || !AllFF(table + md5_offset + 2, 14) ||
        !PartitionMd5(table, md5_offset, digest) || memcmp(digest, table + md5_offset + 16, 16) ||
        !AllFF(table + md5_offset + 32, sizeof(table) - md5_offset - 32))
        return Fail(error, "整机 BIN 分区表校验失败");
    if (compact) {
        // An erased OTA selection boots the only factory application. No
        // configuration or old OTA selection is carried into a public package.
        if (!EmptyRegion(file, kOtaDataAddress, kPhyAddress, error))
            return false;
        *app_offset = kCompactStandaloneOffset;
        return true;
    }
    unsigned char ota[32];
    // Canonical ESP-IDF NEW record: seq=2 selects OTA1. CRC is
    // zlib.crc32(pack('<I',2),0xffffffff), identical to esp_rom_crc32_le.
    if (fseek(file, kOtaDataAddress, SEEK_SET) || fread(ota, 1, sizeof(ota), file) != sizeof(ota) ||
        U32(ota) != 2 || !AllFF(ota + 4, 20) || U32(ota + 24) != 0 || U32(ota + 28) != 0x55f63774U)
        return Fail(error, "整机 BIN 未设置正确的独立启动入口");
    *app_offset = kAppAddress;
    return true;
}
bool Read(const std::string &path, CompactImageInfo *output, std::string *error, bool verify) {
    if (!output)
        return Fail(error, "缺少 BIN 输出参数");
    uint32_t size = 0;
    FILE *file = Open(path, &size);
    if (!file)
        return Fail(error, "无法打开 BIN，或文件超出 16 MiB");
    CompactImageInfo result;
    unsigned char prefix[36]{};
    bool ok = fread(prefix, 1, sizeof(prefix), file) == sizeof(prefix);
    if (!ok)
        Fail(error, "BIN 文件被截断");
    const bool pure = ok && U32(prefix + 32) == 0xabcd5432;
    if (ok && pure) {
        ok = ProbeApp(file, 0, size, path, &result, error);
    } else if (ok) {
        // The ESP32-S31 boot header is at 0x2000 within an independent 0x0 BIN.
        // Never accept an S3 header at zero, or arbitrary data in the reserved prefix.
        ok = size > kCompactStandaloneOffset && EmptyRegion(file, 0, kBootloaderOffset, error) &&
             fseek(file, kBootloaderOffset, SEEK_SET) == 0 &&
             fread(prefix, 1, sizeof(prefix), file) == sizeof(prefix) && EspHeader(prefix);
        if (!ok)
            Fail(error, "不是兼容的独立整机 BIN");
        uint32_t app_offset = 0;
        if (ok)
            ok = FullEnvelope(file, &app_offset, error);
        if (ok && (size <= app_offset || size - app_offset > kAppSize))
            ok = Fail(error, "整机 BIN 应用段超出 13 MiB 或被截断");
        if (ok)
            ok = ProbeApp(file, app_offset, size - app_offset, path, &result, error);
        if (ok && result.manager)
            ok = Fail(error, "整机管理器不能作为部件导入");
    }
    if (ok && verify) {
        uint32_t actual = 0;
        std::string boot_hash;
        if (result.image_offset) {
            ok = Verify(file, kBootloaderOffset, kPartitionTableOffset - kBootloaderOffset, false, &actual,
                        &boot_hash, error) &&
                 EmptyRegion(file, kBootloaderOffset + actual, kPartitionTableOffset, error) &&
                 EmptyRegion(file, kNvsAddress, kOtaDataAddress, error) &&
                 EmptyRegion(file,
                             result.image_offset == kCompactStandaloneOffset ? kOtaDataAddress
                                                                             : kOtaDataAddress + 32,
                             result.image_offset, error);
        }
        if (ok)
            ok = Verify(file, result.image_offset, result.manifest.size, true, &actual,
                        &result.manifest.sha256, error);
    }
    if (fclose(file) != 0 && ok)
        ok = Fail(error, "关闭 BIN 文件失败");
    if (ok) {
        *output = std::move(result);
        if (error)
            error->clear();
    }
    return ok;
}
} // namespace
bool ProbeCompactImage(const std::string &path, CompactImageInfo *output, std::string *error) {
    return Read(path, output, error, false);
}
bool ValidateCompactImage(const std::string &path, CompactImageInfo *output, std::string *error) {
    return Read(path, output, error, true);
}

bool ValidateRunningImage(ImageRead read, void *ctx, uint32_t available, CompactImageInfo *output,
                          std::string *error) {
    if (!read || !output || available < 832 || available > kAppSize)
        return Fail(error, "invalid image reader/capacity");
    unsigned char header[24], segment[8], bytes[2048];
    auto get = [&](uint32_t at, void *out, size_t n) {
        return at <= available && n <= available - at && read(ctx, at, out, n);
    };
    if (!get(0, header, sizeof(header)) || !EspHeader(header))
        return Fail(error, "invalid running ESP image header");
    uint32_t position = 24;
    unsigned char checksum = 0xef;
    for (unsigned i = 0; i < header[1]; ++i) {
        if (!get(position, segment, 8))
            return Fail(error, "truncated running segment");
        position += 8;
        uint32_t count = U32(segment + 4);
        if ((count & 3) || count > available - position)
            return Fail(error, "invalid running segment length");
        while (count) {
            size_t n = std::min<size_t>(count, sizeof(bytes));
            if (!get(position, bytes, n))
                return Fail(error, "running image read failed");
            for (size_t j = 0; j < n; ++j)
                checksum ^= bytes[j];
            position += n;
            count -= n;
            if ((position & 0xffff) < n)
                YieldImageReader();
        }
    }
    const uint32_t digest_offset = (position + 16) & ~15U;
    unsigned char tail[16], expected[32], actual[32], whole[32];
    if (digest_offset > available || available - digest_offset < 32 ||
        !get(position, tail, digest_offset - position) || tail[digest_offset - position - 1] != checksum)
        return Fail(error, "running image checksum/length mismatch");
    for (uint32_t i = 0; i + 1 < digest_offset - position; ++i)
        if (tail[i])
            return Fail(error, "nonzero image padding");
    if (!get(digest_offset, expected, 32))
        return Fail(error, "running image digest read failed");
    Sha256 hash, full;
    for (uint32_t at = 0; at < digest_offset;) {
        size_t n = std::min<size_t>(digest_offset - at, sizeof(bytes));
        if (!get(at, bytes, n) || !hash.Add(bytes, n) || !full.Add(bytes, n))
            return Fail(error, "running image hash failed");
        at += n;
        if ((at & 0xffff) < n)
            YieldImageReader();
    }
    if (!hash.Finish(actual) || memcmp(expected, actual, 32) || !full.Add(expected, 32) ||
        !full.Finish(whole))
        return Fail(error, "running image SHA mismatch");
    unsigned char prefix[kCompactMetadataOffset];
    CompactMetadata meta{};
    if (!get(0, prefix, sizeof(prefix)) || !get(kCompactMetadataOffset, &meta, sizeof(meta)))
        return Fail(error, "running metadata read failed");
    CompactImageInfo result;
    if (!ProbeMetadata(prefix, meta, 0, digest_offset + 32, "", &result, error))
        return false;
    char text[65];
    for (unsigned i = 0; i < 32; ++i)
        snprintf(text + i * 2, 3, "%02x", whole[i]);
    result.manifest.sha256 = text;
    *output = std::move(result);
    if (error)
        error->clear();
    return true;
}
} // namespace pocket
