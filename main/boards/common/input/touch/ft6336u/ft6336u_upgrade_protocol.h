#pragma once
#include <algorithm>
#include <cstddef>
#include <cstdint>

// 升级协议依据供应商 FT6336U 驱动；应用失效回退及重试依据 Qualcomm V3.4。
// IO 提供 Write(bytes, size)、Read(command, size, output, size)、Delay(ms)。

constexpr size_t kFt6336uUpgradeFileSize = 47176;
constexpr size_t kFt6336uUpgradeAppSize = 47096;
constexpr uint8_t kFt6336uUpgradeVersion = 0x04;
constexpr uint8_t kFt6336uUpgradeAppXor = 0xfa;
enum class Ft6336uUpgradeResult {
    Updated,
    Current,
    InvalidImage,
    WrongChip,
    IoError,
    BootIdError,
    EraseError,
    PacketError,
    EccError,
    VersionError
};

inline bool Ft6336uUpgradeValidate(const uint8_t* image, size_t size) {
    if (!image || size != kFt6336uUpgradeFileSize) {
        return false;
    }
    const size_t length = (size_t(image[0x100]) << 8) | image[0x101];
    const size_t inverse = (size_t(image[0x102]) << 8) | image[0x103];
    if (length != kFt6336uUpgradeAppSize || (length ^ inverse) != 0xffff || image[0x10a] != kFt6336uUpgradeVersion) {
        return false;
    }
    uint8_t ecc = 0;
    for (size_t i = 0; i < length; ++i) {
        ecc ^= image[i];
    }
    uint32_t crc = 0xffffffff;
    for (size_t i = 0; i < size; ++i) {
        crc ^= image[i];
        for (int bit = 0; bit < 8; ++bit) {
            crc = (crc >> 1) ^ ((crc & 1) ? 0xedb88320u : 0);
        }
    }
    return ecc == kFt6336uUpgradeAppXor && (crc ^ 0xffffffff) == 0xaff8e72b;
}

template <class IO>
bool Ft6336uUpgradeStatus(IO& io, uint16_t expected, int retries, int delay) {
    const uint8_t command[] = {0x6a, 0, 0, 0};
    for (int i = 0; i < retries; ++i) {
        uint8_t status[2] = {};
        if (io.Read(command, sizeof(command), status, sizeof(status)) &&
            ((uint16_t(status[0]) << 8) | status[1]) == expected) {
            return true;
        }
        io.Delay(delay);
    }
    return false;
}

template <class IO>
Ft6336uUpgradeResult Ft6336uUpgradeAttempt(IO& io, const uint8_t* image, size_t size, bool force_update) {
    if (!Ft6336uUpgradeValidate(image, size)) {
        return Ft6336uUpgradeResult::InvalidImage;
    }
    uint8_t chip = 0, chip2 = 0, version = 0;
    const uint8_t id_reg = 0xa3, id2_reg = 0x9f, version_reg = 0xa6;
    bool app_valid = false;
    for (int i = 0; i < 5; ++i) {
        const bool id_ok = io.Read(&id_reg, 1, &chip, 1);
        const bool id2_ok = io.Read(&id2_reg, 1, &chip2, 1);
        const bool version_ok = io.Read(&version_reg, 1, &version, 1);
        if (id_ok && id2_ok && version_ok && chip == 0x64 && chip2 == 0x26 && version != 0xff) {
            app_valid = true;
            break;
        }
        io.Delay(200);
    }
    if (app_valid && !force_update && version == kFt6336uUpgradeVersion) {
        return Ft6336uUpgradeResult::Current;
    }

    // 进入升级流程后，无论成功失败都尝试复位退出 Bootloader。
    auto finish = [&](Ft6336uUpgradeResult result) {
        const uint8_t reset = 0x07;
        const bool reset_ok = io.Write(&reset, 1);
        io.Delay(300);
        return result == Ft6336uUpgradeResult::Updated && !reset_ok ? Ft6336uUpgradeResult::IoError : result;
    };
    if (app_valid) {
        const uint8_t aa[] = {0xbc, 0xaa}, bb[] = {0xbc, 0x55};
        if (!io.Write(aa, 2)) {
            return finish(Ft6336uUpgradeResult::IoError);
        }
        io.Delay(10);
        if (!io.Write(bb, 2)) {
            return finish(Ft6336uUpgradeResult::IoError);
        }
        io.Delay(80);
    } else {
        // 应用无效时不能依赖应用寄存器复位，改用 Boot 复位命令。
        const uint8_t reset = 0x07;
        if (!io.Write(&reset, 1)) {
            return finish(Ft6336uUpgradeResult::IoError);
        }
        io.Delay(80);
    }
    const uint8_t start[] = {0x55, 0xaa};
    const uint8_t boot_command[] = {0x90, 0, 0, 0};
    uint8_t boot[2] = {};
    bool boot_valid = false;
    for (int i = 0; i < 30; ++i) {
        if (io.Write(start, 2)) {
            io.Delay(12);
            if (io.Read(boot_command, 4, boot, 2) && boot[0] == 0x79 && boot[1] == 0x1c) {
                boot_valid = true;
                break;
            }
        }
        io.Delay(20);
    }
    if (!boot_valid) {
        return finish(Ft6336uUpgradeResult::BootIdError);
    }

    const uint8_t erase = 0x61;
    if (!io.Write(&erase, 1)) {
        return finish(Ft6336uUpgradeResult::IoError);
    }
    io.Delay(500);
    if (!Ft6336uUpgradeStatus(io, 0xb002, 50, 100)) {
        return finish(Ft6336uUpgradeResult::EraseError);
    }
    uint8_t packet[6 + 32] = {};
    for (size_t offset = 0, index = 0; offset < kFt6336uUpgradeAppSize; offset += 32, ++index) {
        const size_t length = std::min<size_t>(32, kFt6336uUpgradeAppSize - offset);
        packet[0] = 0xbf;
        packet[1] = uint8_t(offset >> 16);
        packet[2] = uint8_t(offset >> 8);
        packet[3] = uint8_t(offset);
        packet[4] = 0;
        packet[5] = uint8_t(length);
        std::copy(image + offset, image + offset + length, packet + 6);
        if (!io.Write(packet, 6 + length)) {
            return finish(Ft6336uUpgradeResult::IoError);
        }
        io.Delay(1);
        if (!Ft6336uUpgradeStatus(io, uint16_t(0xb003 + index), 100, 1)) {
            return finish(Ft6336uUpgradeResult::PacketError);
        }
    }
    const uint8_t ecc_command = 0xcc;
    uint8_t ecc = 0;
    if (!io.Read(&ecc_command, 1, &ecc, 1)) {
        return finish(Ft6336uUpgradeResult::IoError);
    }
    if (ecc != kFt6336uUpgradeAppXor) {
        return finish(Ft6336uUpgradeResult::EccError);
    }
    Ft6336uUpgradeResult result = finish(Ft6336uUpgradeResult::Updated);
    if (result != Ft6336uUpgradeResult::Updated) {
        return result;
    }
    if (!io.Read(&version_reg, 1, &version, 1) || !io.Read(&id_reg, 1, &chip, 1) || !io.Read(&id2_reg, 1, &chip2, 1)) {
        return Ft6336uUpgradeResult::IoError;
    }
    return chip == 0x64 && chip2 == 0x26 && version == kFt6336uUpgradeVersion ? Ft6336uUpgradeResult::Updated
                                                                              : Ft6336uUpgradeResult::VersionError;
}

template <class IO>
Ft6336uUpgradeResult Ft6336uUpgradeRun(IO& io, const uint8_t* image, size_t size, int* attempts = nullptr) {
    if (attempts) {
        *attempts = 1;
    }
    Ft6336uUpgradeResult result = Ft6336uUpgradeAttempt(io, image, size, false);
    if (result == Ft6336uUpgradeResult::Updated || result == Ft6336uUpgradeResult::Current ||
        result == Ft6336uUpgradeResult::InvalidImage) {
        return result;
    }
    // 首次写入可能已改变应用头部；重试不能仅凭版本号跳过完整写入和校验。
    if (attempts) {
        *attempts = 2;
    }
    return Ft6336uUpgradeAttempt(io, image, size, true);
}
