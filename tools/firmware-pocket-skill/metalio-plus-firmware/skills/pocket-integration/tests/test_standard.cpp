#include "pocket_core.h"
#include "pocket_handoff.h"
#include "pocket_hash.h"
#include "pocket_image.h"
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <vector>
namespace fs = std::filesystem;
int checks = 0;
#define CHECK(c)                                                                                             \
    do {                                                                                                     \
        ++checks;                                                                                            \
        if (!(c)) {                                                                                          \
            std::cerr << "FAIL " << __LINE__ << " " << #c << "\n";                                           \
            return 1;                                                                                        \
        }                                                                                                    \
    } while (0)
std::string Read(const fs::path &p) {
    std::ifstream f(p, std::ios::binary);
    return {std::istreambuf_iterator<char>(f), {}};
}
void Write(const fs::path &p, const std::string &s) {
    std::ofstream f(p, std::ios::binary);
    f.write(s.data(), s.size());
}
void Put32(std::vector<unsigned char> &b, size_t at, uint32_t n) {
    for (int i = 0; i < 4; ++i)
        b[at + i] = static_cast<unsigned char>(n >> (8 * i));
}
void Seal(std::vector<unsigned char> &image) {
    unsigned char checksum = 0xef;
    for (size_t i = 32; i < 800; ++i)
        checksum ^= image[i];
    image[815] = checksum;
    pocket::Sha256 hash;
    hash.Add(image.data(), 816);
    hash.Finish(image.data() + 816);
}
std::vector<unsigned char> Image(bool manager = false) {
    std::vector<unsigned char> b(848);
    b[0] = 0xe9;
    b[1] = 1;
    b[2] = pocket::kImageFlashMode;
    b[3] = pocket::kImageFlashSizeSpeed;
    b[12] = pocket::kImageChipId & 255;
    b[13] = pocket::kImageChipId >> 8;
    b[23] = 1;
    Put32(b, 28, 768);
    Put32(b, 32, 0xabcd5432);
    std::strcpy(reinterpret_cast<char *>(b.data() + 48), "9.8.7");
    std::strcpy(reinterpret_cast<char *>(b.data() + 80), manager ? "pocket_manager" : "test_game");
    std::strcpy(reinterpret_cast<char *>(b.data() + 144), "idf-custom");
    pocket::CompactMetadata m{};
    std::memcpy(m.magic, pocket::kPocketAbiMagic, 16);
    m.abi_major = 1;
    m.header_bytes = 512;
    m.flags = manager ? 1 : 2;
    m.exit_method = manager ? 0 : 1;
    std::strcpy(m.target, pocket::kTargetId);
    std::strcpy(m.layout_profile, pocket::kLayoutId);
    std::strcpy(m.app_id, manager ? "pocket_manager" : "test_game");
    std::strcpy(m.version, "9.8.7");
    std::strcpy(m.name, "Test");
    if (!manager)
        std::strcpy(m.exit_hint, "UI return");
    std::memcpy(b.data() + 288, &m, 512);
    Seal(b);
    return b;
}
bool Reader(void *context, uint32_t offset, void *out, size_t n) {
    const auto &b = *static_cast<std::vector<unsigned char> *>(context);
    if (offset > b.size() || n > b.size() - offset)
        return false;
    std::memcpy(out, b.data() + offset, n);
    return true;
}
#ifndef POCKET_TEST_FIXTURES_ONLY
int main(int argc, char **argv) {
    CHECK(argc == 2);
    fs::path dir = argv[1];
    CHECK(!fs::exists(dir));
    fs::create_directories(dir);
    using namespace pocket;
    std::string error, id = "unchanged";
    for (auto suffix : {"\n", "\n0\n", "\n1\n"}) {
        CHECK(DecodeIdentity(std::string("0legacy.user-") + suffix, &id));
        CHECK(id == "0legacy.user-");
    }
    for (auto text : {"u\n2\n", "u.\n", "../x\n", "U\n", "u\n0\nx", "u"}) {
        id = "old";
        CHECK(!DecodeIdentity(text, &id));
        CHECK(id == "old");
    }
    CHECK(IsAppIdentifier("a_1"));
    CHECK(!IsAppIdentifier("0abc"));
    CHECK(!IsAppIdentifier("a-b"));
    CHECK(!IsAppIdentifier("a.b"));
    RunContext run;
    run.run_id = "0123456789abcdef0123456789abcdef";
    run.user_id = "u0123456789abcdef01234567";
    run.app_id = "test_game";
    run.version = "1.0.0";
    run.image_sha256 = std::string(64, 'a');
    run.phase = "active";
    run.image_bytes = 123456;
    run.storage_mode = 2;
    run.state_schema = 9;
    auto text = EncodeRun(run);
    RunContext decoded;
    CHECK(text.find("image_bytes") == std::string::npos);
    CHECK(DecodeRun(text, &decoded));
    CHECK(decoded.image_bytes == 0 && decoded.storage_mode == 0);
    CHECK(EncodeRun(decoded) == text);
    auto reordered = "{ \"phase\":\"active\",\"image_sha256\":\"" + run.image_sha256 +
                     "\",\"version\":\"1.0.0\",\"app_id\":\"test_game\",\"user_id\":\"" + run.user_id +
                     "\",\"run_id\":\"" + run.run_id + "\" }\n";
    CHECK(DecodeRun(reordered, &decoded));
    CHECK(EncodeRun(decoded) == text);
    auto escaped = text;
    escaped.replace(escaped.find("run_id"), 6, "run_\\u0069d");
    CHECK(DecodeRun(escaped, &decoded));
    std::vector<std::string> invalid = {text + "x",
                                        std::string("\xef\xbb\xbf") + text,
                                        text.substr(0, text.size() - 1) + ",\"extra\":\"x\"}",
                                        text.substr(0, text.size() - 1) + ",\"run_\\u0069d\":\"" +
                                            run.run_id + "\"}",
                                        text.substr(0, text.size() - 1) + ",}",
                                        "{\"run_id\":null}",
                                        text.substr(0, text.size() - 1)};
    for (auto bad : invalid) {
        decoded.app_id = "unchanged";
        CHECK(!DecodeRun(bad, &decoded));
        CHECK(decoded.app_id == "unchanged");
    }
    for (auto phase : {"prepared", "closed"}) {
        run.phase = phase;
        CHECK(DecodeRun(EncodeRun(run), &decoded));
    }
    run.phase = "active";
    RecordStore store(dir.string());
    CHECK(store.Load("run", 1).status == RecordStatus::Missing);
    CHECK(store.Save("run", 1, reordered).ok());
    CHECK(WriteHandoffReceipt(dir.string(), run, false, &error));
    CHECK(InspectHandoff(dir.string(), run.user_id).state == HandoffState::Unfinished);
    CHECK(WriteHandoffReceipt(dir.string(), run, true, &error));
    CHECK(InspectHandoff(dir.string(), run.user_id).state == HandoffState::Returned);
    CHECK(WriteHandoffReceipt(dir.string(), run, false, &error));
    CHECK(InspectHandoff(dir.string(), run.user_id).state == HandoffState::Unfinished);
    run.phase = "closed";
    CHECK(SaveRun(dir.string(), run, &error));
    CHECK(!InspectHandoff(dir.string(), run.user_id).may_replace());
    run.phase = "active";
    CHECK(!WriteHandoffReceipt(dir.string(), run, true, &error));
    CHECK(SaveRun(dir.string(), run, &error));
    CHECK(!fs::exists(dir / "run107.a.pkr"));
    CHECK(!fs::exists(dir / "receipt107.a.pkr"));
    CHECK(store.Save("pairs", 1, "first").ok());
    CHECK(store.Save("pairs", 1, "second").ok());
    CHECK(store.Load("pairs", 1).payload == "second");
    Write(dir / "pairs.b.pkr", "broken");
    auto fallback = store.Load("pairs", 1);
    CHECK(fallback.ok() && fallback.degraded && fallback.payload == "first");
    auto raw = Read(dir / "pairs.a.pkr");
    CHECK(raw.substr(0, 8) == std::string("PKREC01\0", 8));
    CHECK(raw.size() == 37);
    CHECK(store.Save("future", 2, "new").ok());
    CHECK(store.Load("future", 1).status == RecordStatus::UnsupportedSchema);
    CHECK(!store.Save("future", 1, "old").ok());
    CHECK(store.Load("future", 2).payload == "new");
    CHECK(store.Save("conflict", 1, "aaaa").ok());
    auto a = Read(dir / "conflict.a.pkr"), b = a;
    b.replace(32, 4, "bbbb");
    auto crc = Crc32((b.substr(0, 28) + b.substr(32)).data(), b.size() - 4);
    for (int i = 0; i < 4; ++i)
        b[28 + i] = char(crc >> (8 * i));
    Write(dir / "conflict.b.pkr", b);
    CHECK(store.Load("conflict", 1).status == RecordStatus::Corrupt);
    CHECK(!store.Save("conflict", 1, "overwrite").ok());
    CHECK(store.Load("absent", 1).status == RecordStatus::Missing);
    CHECK(RecordStore((dir / "no-mount").string()).Load("absent", 1).status == RecordStatus::IoError);
    CHECK(store.Save("limit", 1, std::string(4096, 'x')).ok());
    CHECK(!store.Save("limit", 1, std::string(4097, 'x')).ok());
    CHECK(store.Save("overflow", 1, "data").ok());
    auto exhausted = Read(dir / "overflow.a.pkr");
    for (int i = 16; i < 24; ++i)
        exhausted[i] = char(-1);
    auto crc_bytes = exhausted.substr(0, 28) + exhausted.substr(32);
    crc = Crc32(crc_bytes.data(), crc_bytes.size());
    for (int i = 0; i < 4; ++i)
        exhausted[28 + i] = char(crc >> (8 * i));
    Write(dir / "overflow.a.pkr", exhausted);
    CHECK(store.Load("overflow", 1).generation == std::numeric_limits<uint64_t>::max());
    CHECK(store.Save("overflow", 1, "next").status == RecordStatus::GenerationOverflow);
    CHECK(Read(dir / "overflow.a.pkr") == exhausted);
    auto unknown = raw;
    unknown[8] = 2;
    Write(dir / "pairs.b.pkr", unknown);
    CHECK(store.Load("pairs", 1).status == RecordStatus::UnsupportedSchema);
    CHECK(!store.Save("pairs", 1, "next").ok());
    WifiOffer offer{run.run_id, std::string("\xff raw ", 6), "", 1};
    auto encoded = EncodeWifiOffer(offer);
    CHECK(encoded.size() == 24 && static_cast<unsigned char>(encoded[0]) == 1 &&
          static_cast<unsigned char>(encoded[15]) == 0xef);
    RecordResult rr;
    rr.status = RecordStatus::Ok;
    rr.schema = 1;
    rr.generation = 1;
    rr.payload = encoded;
    WifiOffer back;
    CHECK(DecodeWifiOffer(rr, &back));
    CHECK(back.ssid == offer.ssid && back.run_id == run.run_id && back.password.empty());
    rr.payload.push_back(0);
    CHECK(!DecodeWifiOffer(rr, &back));
    rr.payload = encoded;
    rr.payload[16] = 0;
    CHECK(!DecodeWifiOffer(rr, &back));
    CHECK(WriteWifiOffer(dir.string(), offer, &error));
    CHECK(store.Load("wifi_offer", 1).payload == encoded);
    for (auto result : {"processing", "duplicate", "accepted", "failed", "cancelled"}) {
        CHECK(WriteWifiResult(dir.string(), offer, result, &error));
        std::string actual;
        CHECK(ReadWifiResult(dir.string(), offer, &actual, &error));
        CHECK(actual == result);
    }
    for (auto generation : {"0", "01", "18446744073709551616", "-1"}) {
        CHECK(store
                  .Save("wifi_result", 1,
                        "{\"run_id\":\"" + run.run_id + "\",\"offer_generation\":\"" + generation +
                            "\",\"result\":\"accepted\"}")
                  .ok());
        std::string value = "untouched";
        CHECK(!ReadWifiResult(dir.string(), offer, &value, &error));
        CHECK(value == "untouched");
    }
    offer.generation = std::numeric_limits<uint64_t>::max();
    CHECK(WriteWifiResult(dir.string(), offer, "accepted", &error));
    std::string result;
    CHECK(ReadWifiResult(dir.string(), offer, &result, &error) && result == "accepted");
    auto img = Image();
    CompactImageInfo info;
    CHECK(ValidateRunningImage(Reader, &img, kAppSize, &info, &error));
    CHECK(info.manifest.size == 848 && !info.manager);
    for (int exit = 1; exit <= 5; ++exit) {
        img = Image();
        img[288 + 29] = static_cast<unsigned char>(exit);
        Seal(img);
        CHECK(ValidateRunningImage(Reader, &img, kAppSize, &info, &error));
    }
    img = Image();
    img[288 + 29] = 6;
    Seal(img);
    CHECK(!ValidateRunningImage(Reader, &img, kAppSize, &info, &error));
    img = Image(true);
    CHECK(ValidateRunningImage(Reader, &img, kManagerSize, &info, &error));
    CHECK(info.manager && info.manifest.version == "9.8.7");
    for (size_t pos : {size_t(23), size_t(28), size_t(80), size_t(288 + 31), size_t(288 + 420), size_t(815),
                       size_t(847)}) {
        img = Image();
        img[pos] ^= 1;
        CHECK(!ValidateRunningImage(Reader, &img, kAppSize, &info, &error));
    }
    img = Image();
    img[80] = 'x';
    Seal(img);
    CHECK(!ValidateRunningImage(Reader, &img, kAppSize, &info, &error));
    img = Image();
    img.pop_back();
    CHECK(!ValidateRunningImage(Reader, &img, kAppSize, &info, &error));
    CHECK(!ValidateRunningImage(nullptr, nullptr, kAppSize, &info, &error));
    std::cout << "PASS " << checks << " standard control/record/image checks\n";
    return 0;
}
#endif
