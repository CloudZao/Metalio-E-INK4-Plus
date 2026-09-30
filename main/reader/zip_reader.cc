#include "zip_reader.h"

#include <cstdio>
#include <cstring>

#include <esp_heap_caps.h>
#include <esp_log.h>
#include <zlib.h>

namespace reader {
namespace {

constexpr const char* TAG = "ZipReader";
constexpr uint32_t kSigLocal = 0x04034b50;
constexpr uint32_t kSigCentral = 0x02014b50;
constexpr uint32_t kSigEnd = 0x06054b50;
constexpr uint16_t kMethodStore = 0;
constexpr uint16_t kMethodDeflate = 8;
constexpr size_t kStreamInBuf = 4096;

bool ReadExact(FILE* fp, void* buf, size_t n) {
    return std::fread(buf, 1, n, fp) == n;
}

bool ReadU16(FILE* fp, uint16_t* v) {
    uint8_t b[2];
    if (!ReadExact(fp, b, 2)) {
        return false;
    }
    *v = static_cast<uint16_t>(b[0] | (b[1] << 8));
    return true;
}

bool ReadU32(FILE* fp, uint32_t* v) {
    uint8_t b[4];
    if (!ReadExact(fp, b, 4)) {
        return false;
    }
    *v = static_cast<uint32_t>(b[0] | (b[1] << 8) | (b[2] << 16) | (b[3] << 24));
    return true;
}

bool SeekCur(FILE* fp, long off) {
    return std::fseek(fp, off, SEEK_CUR) == 0;
}

uint8_t* AllocPsram(size_t n) {
    uint8_t* p = static_cast<uint8_t*>(heap_caps_malloc(n, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (p == nullptr) {
        p = static_cast<uint8_t*>(heap_caps_malloc(n, MALLOC_CAP_8BIT));
    }
    return p;
}

}  // namespace

bool ZipReader::Open(const char* path) {
    Close();
    if (path == nullptr || path[0] == '\0') {
        return false;
    }
    fp_ = std::fopen(path, "rb");
    if (fp_ == nullptr) {
        ESP_LOGE(TAG, "open failed: %s", path);
        return false;
    }
    path_ = path;
    cd_loaded_ = false;
    entries_.clear();
    if (!LoadCentralDirectory()) {
        Close();
        return false;
    }
    return true;
}

void ZipReader::Close() {
    CloseEntryStream();
    if (fp_ != nullptr) {
        std::fclose(fp_);
        fp_ = nullptr;
    }
    entries_.clear();
    cd_loaded_ = false;
    path_.clear();
}

void ZipReader::DetachFile() {
    CloseEntryStream();
    if (fp_ != nullptr) {
        std::fclose(fp_);
        fp_ = nullptr;
    }
}

bool ZipReader::ReattachFile() {
    if (fp_ != nullptr) {
        return true;
    }
    if (path_.empty() || !cd_loaded_) {
        return false;
    }
    fp_ = std::fopen(path_.c_str(), "rb");
    if (fp_ == nullptr) {
        ESP_LOGW(TAG, "reattach fail: %s", path_.c_str());
        return false;
    }
    return true;
}

bool ZipReader::LoadCentralDirectory() {
    if (fp_ == nullptr) {
        return false;
    }
    if (std::fseek(fp_, 0, SEEK_END) != 0) {
        return false;
    }
    const long file_size = std::ftell(fp_);
    if (file_size < 22) {
        return false;
    }

    // EOCD 在末尾最多 64KB comment
    const long scan = file_size < 65557 ? file_size : 65557L;
    std::vector<uint8_t> tail(static_cast<size_t>(scan));
    if (std::fseek(fp_, file_size - scan, SEEK_SET) != 0) {
        return false;
    }
    if (!ReadExact(fp_, tail.data(), tail.size())) {
        return false;
    }

    long eocd = -1;
    for (long i = static_cast<long>(tail.size()) - 22; i >= 0; --i) {
        const uint32_t sig = static_cast<uint32_t>(tail[i] | (tail[i + 1] << 8) | (tail[i + 2] << 16) |
                                                   (tail[i + 3] << 24));
        if (sig == kSigEnd) {
            eocd = i;
            break;
        }
    }
    if (eocd < 0) {
        ESP_LOGE(TAG, "EOCD not found: %s", path_.c_str());
        return false;
    }

    const uint8_t* p = tail.data() + eocd;
    const uint16_t total_entries = static_cast<uint16_t>(p[10] | (p[11] << 8));
    const uint32_t cd_size = static_cast<uint32_t>(p[12] | (p[13] << 8) | (p[14] << 16) | (p[15] << 24));
    const uint32_t cd_off = static_cast<uint32_t>(p[16] | (p[17] << 8) | (p[18] << 16) | (p[19] << 24));
    (void)cd_size;

    if (std::fseek(fp_, static_cast<long>(cd_off), SEEK_SET) != 0) {
        return false;
    }

    entries_.clear();
    entries_.reserve(total_entries);
    for (uint16_t i = 0; i < total_entries; ++i) {
        uint32_t sig = 0;
        if (!ReadU32(fp_, &sig) || sig != kSigCentral) {
            break;
        }
        Entry e;
        if (!SeekCur(fp_, 6)) {
            return false;
        }
        if (!ReadU16(fp_, &e.method)) {
            return false;
        }
        if (!SeekCur(fp_, 8)) {
            return false;
        }
        if (!ReadU32(fp_, &e.comp_size) || !ReadU32(fp_, &e.uncomp_size)) {
            return false;
        }
        uint16_t name_len = 0, extra_len = 0, comment_len = 0;
        if (!ReadU16(fp_, &name_len) || !ReadU16(fp_, &extra_len) || !ReadU16(fp_, &comment_len)) {
            return false;
        }
        if (!SeekCur(fp_, 8)) {
            return false;
        }
        if (!ReadU32(fp_, &e.local_header_off)) {
            return false;
        }
        std::string name(name_len, '\0');
        if (name_len > 0 && !ReadExact(fp_, name.data(), name_len)) {
            return false;
        }
        if (!SeekCur(fp_, static_cast<long>(extra_len) + comment_len)) {
            return false;
        }
        if (!name.empty() && name.back() == '/') {
            continue;  // 目录
        }
        entries_.emplace(std::move(name), e);
    }

    cd_loaded_ = true;
    ESP_LOGI(TAG, "loaded %u entries from %s", static_cast<unsigned>(entries_.size()), path_.c_str());
    return !entries_.empty();
}

bool ZipReader::FindEntry(const char* name, Entry* out) {
    if (!cd_loaded_ || name == nullptr || out == nullptr) {
        return false;
    }
    auto it = entries_.find(name);
    if (it == entries_.end()) {
        return false;
    }
    *out = it->second;
    return true;
}

bool ZipReader::EntryExists(const char* name) {
    Entry e;
    return FindEntry(name, &e);
}

bool ZipReader::GetUncompressedSize(const char* name, size_t* size) {
    Entry e;
    if (!FindEntry(name, &e) || size == nullptr) {
        return false;
    }
    *size = e.uncomp_size;
    return true;
}

void ZipReader::ListEntries(std::vector<std::string>& names) {
    names.clear();
    names.reserve(entries_.size());
    for (const auto& kv : entries_) {
        names.push_back(kv.first);
    }
}

bool ZipReader::InflateEntry(const Entry& e, std::vector<uint8_t>& out, size_t max_bytes) {
    out.clear();
    if (fp_ == nullptr) {
        return false;
    }
    if (e.uncomp_size > max_bytes) {
        ESP_LOGW(TAG, "entry too large: %u > %u", e.uncomp_size, static_cast<unsigned>(max_bytes));
        return false;
    }

    if (std::fseek(fp_, static_cast<long>(e.local_header_off), SEEK_SET) != 0) {
        return false;
    }
    uint32_t sig = 0;
    if (!ReadU32(fp_, &sig) || sig != kSigLocal) {
        ESP_LOGE(TAG, "bad local header");
        return false;
    }
    if (!SeekCur(fp_, 22)) {
        return false;
    }
    uint16_t name_len = 0, extra_len = 0;
    if (!ReadU16(fp_, &name_len) || !ReadU16(fp_, &extra_len)) {
        return false;
    }
    if (!SeekCur(fp_, static_cast<long>(name_len) + extra_len)) {
        return false;
    }

    if (e.method == kMethodStore) {
        out.resize(e.uncomp_size);
        if (e.uncomp_size > 0 && !ReadExact(fp_, out.data(), e.uncomp_size)) {
            out.clear();
            return false;
        }
        return true;
    }

    if (e.method != kMethodDeflate) {
        ESP_LOGE(TAG, "unsupported method %u", e.method);
        return false;
    }

    std::vector<uint8_t> comp(e.comp_size);
    if (e.comp_size > 0 && !ReadExact(fp_, comp.data(), e.comp_size)) {
        return false;
    }

    out.resize(e.uncomp_size);
    z_stream strm{};
    strm.next_in = comp.data();
    strm.avail_in = e.comp_size;
    strm.next_out = out.data();
    strm.avail_out = e.uncomp_size;

    // ZIP 使用 raw deflate
    if (inflateInit2(&strm, -MAX_WBITS) != Z_OK) {
        out.clear();
        return false;
    }
    const int zret = inflate(&strm, Z_FINISH);
    inflateEnd(&strm);
    if (zret != Z_STREAM_END && zret != Z_OK) {
        ESP_LOGE(TAG, "inflate failed: %d", zret);
        out.clear();
        return false;
    }
    out.resize(strm.total_out);
    return true;
}

bool ZipReader::ReadEntry(const char* name, std::vector<uint8_t>& out, size_t max_bytes) {
    std::lock_guard<std::mutex> lock(io_mutex_);
    Entry e;
    if (!FindEntry(name, &e)) {
        ESP_LOGW(TAG, "missing entry: %s", name);
        out.clear();
        return false;
    }
    return InflateEntry(e, out, max_bytes);
}

bool ZipReader::SeekEntryPayload(const Entry& e, uint32_t* comp_remain) {
    if (fp_ == nullptr || comp_remain == nullptr) {
        return false;
    }
    if (std::fseek(fp_, static_cast<long>(e.local_header_off), SEEK_SET) != 0) {
        return false;
    }
    uint32_t sig = 0;
    if (!ReadU32(fp_, &sig) || sig != kSigLocal) {
        ESP_LOGE(TAG, "bad local header");
        return false;
    }
    if (!SeekCur(fp_, 22)) {
        return false;
    }
    uint16_t name_len = 0, extra_len = 0;
    if (!ReadU16(fp_, &name_len) || !ReadU16(fp_, &extra_len)) {
        return false;
    }
    if (!SeekCur(fp_, static_cast<long>(name_len) + extra_len)) {
        return false;
    }
    *comp_remain = e.comp_size;
    return true;
}

void ZipReader::ReleaseStreamLock() {
    if (stream_holding_mutex_) {
        stream_holding_mutex_ = false;
        io_mutex_.unlock();
    }
}

void ZipReader::CloseEntryStream() {
    if (stream_z_init_) {
        inflateEnd(&stream_z_);
        stream_z_init_ = false;
        std::memset(&stream_z_, 0, sizeof(stream_z_));
    }
    if (stream_inbuf_ != nullptr) {
        heap_caps_free(stream_inbuf_);
        stream_inbuf_ = nullptr;
    }
    stream_inbuf_cap_ = 0;
    stream_inbuf_len_ = 0;
    stream_inbuf_pos_ = 0;
    stream_comp_remain_ = 0;
    stream_uncomp_size_ = 0;
    stream_store_ = false;
    stream_eof_ = false;
    stream_error_ = false;
    stream_open_ = false;
    ReleaseStreamLock();
}

bool ZipReader::OpenEntryStream(const char* name) {
    CloseEntryStream();
    io_mutex_.lock();
    stream_holding_mutex_ = true;

    Entry e;
    if (!FindEntry(name, &e)) {
        ESP_LOGW(TAG, "stream missing entry: %s", name);
        CloseEntryStream();
        return false;
    }
    if (e.method != kMethodStore && e.method != kMethodDeflate) {
        ESP_LOGE(TAG, "stream unsupported method %u", e.method);
        CloseEntryStream();
        return false;
    }
    uint32_t remain = 0;
    if (!SeekEntryPayload(e, &remain)) {
        CloseEntryStream();
        return false;
    }

    stream_store_ = (e.method == kMethodStore);
    stream_comp_remain_ = remain;
    stream_uncomp_size_ = e.uncomp_size;
    stream_eof_ = (remain == 0 && e.uncomp_size == 0);
    stream_error_ = false;

    if (!stream_store_) {
        stream_inbuf_ = AllocPsram(kStreamInBuf);
        if (stream_inbuf_ == nullptr) {
            ESP_LOGE(TAG, "stream inbuf alloc fail");
            CloseEntryStream();
            return false;
        }
        stream_inbuf_cap_ = kStreamInBuf;
        stream_inbuf_len_ = 0;
        stream_inbuf_pos_ = 0;
        stream_z_ = {};
        if (inflateInit2(&stream_z_, -MAX_WBITS) != Z_OK) {
            CloseEntryStream();
            return false;
        }
        stream_z_init_ = true;
    }

    stream_open_ = true;
    ESP_LOGI(TAG, "stream open %s uncomp=%u method=%u", name, static_cast<unsigned>(e.uncomp_size),
             e.method);
    return true;
}

size_t ZipReader::EntryStreamRead(uint8_t* dst, size_t n) {
    if (!stream_open_ || stream_error_ || dst == nullptr || n == 0) {
        return 0;
    }
    if (stream_eof_) {
        return 0;
    }

    if (stream_store_) {
        const size_t want = n < stream_comp_remain_ ? n : stream_comp_remain_;
        if (want == 0) {
            stream_eof_ = true;
            return 0;
        }
        const size_t got = std::fread(dst, 1, want, fp_);
        stream_comp_remain_ -= static_cast<uint32_t>(got);
        if (got < want) {
            stream_error_ = true;
        }
        if (stream_comp_remain_ == 0) {
            stream_eof_ = true;
        }
        return got;
    }

    size_t produced = 0;
    while (produced < n && !stream_eof_ && !stream_error_) {
        if (stream_inbuf_pos_ >= stream_inbuf_len_ && stream_comp_remain_ > 0) {
            const size_t want =
                stream_comp_remain_ < stream_inbuf_cap_ ? stream_comp_remain_ : stream_inbuf_cap_;
            const size_t got = std::fread(stream_inbuf_, 1, want, fp_);
            if (got == 0) {
                stream_error_ = true;
                break;
            }
            stream_comp_remain_ -= static_cast<uint32_t>(got);
            stream_inbuf_len_ = got;
            stream_inbuf_pos_ = 0;
        }

        stream_z_.next_in = stream_inbuf_ + stream_inbuf_pos_;
        stream_z_.avail_in = static_cast<uInt>(stream_inbuf_len_ - stream_inbuf_pos_);
        stream_z_.next_out = dst + produced;
        stream_z_.avail_out = static_cast<uInt>(n - produced);

        const int zret = inflate(&stream_z_, stream_comp_remain_ == 0 ? Z_FINISH : Z_NO_FLUSH);
        const size_t took_in = (stream_inbuf_len_ - stream_inbuf_pos_) - stream_z_.avail_in;
        stream_inbuf_pos_ += took_in;
        const size_t got_out = (n - produced) - stream_z_.avail_out;
        produced += got_out;

        if (zret == Z_STREAM_END) {
            stream_eof_ = true;
            break;
        }
        if (zret == Z_BUF_ERROR && got_out == 0 && stream_comp_remain_ == 0 &&
            stream_inbuf_pos_ >= stream_inbuf_len_) {
            stream_eof_ = true;
            break;
        }
        if (zret != Z_OK && zret != Z_BUF_ERROR) {
            ESP_LOGE(TAG, "stream inflate err %d", zret);
            stream_error_ = true;
            break;
        }
        if (got_out == 0 && stream_z_.avail_in == 0 && stream_comp_remain_ == 0) {
            stream_eof_ = true;
            break;
        }
    }
    return produced;
}

}  // namespace reader
