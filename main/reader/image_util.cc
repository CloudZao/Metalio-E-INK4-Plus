#include "image_util.h"
#include "image_stream.h"
#include "image_geometry.h"
#include "sd_access.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <memory>
#include <new>
#include <unistd.h>

#include <esp_heap_caps.h>
#include <esp_log.h>

#ifndef CONFIG_IDF_TARGET_ESP32
#include "jpg/jpeg_to_image.h"
#endif

namespace reader {
namespace {

constexpr const char* TAG = "ReaderImg";

bool LooksLikeJpeg(const uint8_t* data, size_t len) {
    return len >= 3 && data[0] == 0xFF && data[1] == 0xD8 && data[2] == 0xFF;
}

bool LooksLikePng(const uint8_t* data, size_t len) {
    static const uint8_t kSig[] = {0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A};
    return len >= 8 && std::memcmp(data, kSig, 8) == 0;
}

bool LooksLikeA2i1(const uint8_t* data, size_t len) {
    return len >= 20 && data[0] == 'A' && data[1] == '2' && data[2] == 'I' && data[3] == '1';
}

bool LooksLikeEbgr(const uint8_t* data, size_t len) {
    return len >= 8 && data[0] == 'E' && data[1] == 'B' && data[2] == 'G' && data[3] == 'R';
}

bool DecodeA2i1ToL8(const uint8_t* data, size_t len, int max_w, int max_h, RasterImage& out) {
    // A2I1: magic + w/h/stride/res(2) + 8B palette + bitmap (1=白 0=黑, MSB left)
    if (!LooksLikeA2i1(data, len)) {
        return false;
    }
    const uint16_t src_w = static_cast<uint16_t>(data[4] | (data[5] << 8));
    const uint16_t src_h = static_cast<uint16_t>(data[6] | (data[7] << 8));
    const uint16_t stride = static_cast<uint16_t>(data[8] | (data[9] << 8));
    if (src_w == 0 || src_h == 0 || stride == 0) {
        return false;
    }
    const size_t need = 20u + static_cast<size_t>(stride) * src_h;
    if (len < need) {
        return false;
    }
    const uint8_t* bits = data + 20;

    int dst_w = src_w;
    int dst_h = src_h;
    if (dst_w > max_w || dst_h > max_h) {
        const float sx = static_cast<float>(max_w) / static_cast<float>(dst_w);
        const float sy = static_cast<float>(max_h) / static_cast<float>(dst_h);
        const float s = sx < sy ? sx : sy;
        dst_w = std::max(1, static_cast<int>(dst_w * s));
        dst_h = std::max(1, static_cast<int>(dst_h * s));
    }
    out.pixels.assign(static_cast<size_t>(dst_w) * dst_h, 0xFF);
    out.width = static_cast<uint16_t>(dst_w);
    out.height = static_cast<uint16_t>(dst_h);
    for (int y = 0; y < dst_h; ++y) {
        const int sy = int64_t(y) * src_h / dst_h;
        const uint8_t* row = bits + static_cast<size_t>(sy) * stride;
        for (int x = 0; x < dst_w; ++x) {
            const int sx = int64_t(x) * src_w / dst_w;
            const uint8_t byte = row[sx >> 3];
            const bool white = (byte & (0x80 >> (sx & 7))) != 0;
            out.pixels[static_cast<size_t>(y) * dst_w + x] = white ? 0xFF : 0x00;
        }
    }
    out.BindDsc();
    return true;
}

bool DecodeEbgrToL8(const uint8_t* data, size_t len, int max_w, int max_h, RasterImage& out) {
    if (!LooksLikeEbgr(data, len)) {
        return false;
    }
    const int src_w = data[4] | (data[5] << 8);
    const int src_h = data[6] | (data[7] << 8);
    if (src_w <= 0 || src_h <= 0) {
        return false;
    }
    const size_t need = 8u + static_cast<size_t>(src_w) * src_h;
    if (len < need) {
        return false;
    }
    int dst_w = src_w;
    int dst_h = src_h;
    if (dst_w > max_w || dst_h > max_h) {
        const float sx = static_cast<float>(max_w) / static_cast<float>(dst_w);
        const float sy = static_cast<float>(max_h) / static_cast<float>(dst_h);
        const float s = sx < sy ? sx : sy;
        dst_w = std::max(1, static_cast<int>(dst_w * s));
        dst_h = std::max(1, static_cast<int>(dst_h * s));
    }
    out.pixels.assign(static_cast<size_t>(dst_w) * dst_h, 0xFF);
    out.width = static_cast<uint16_t>(dst_w);
    out.height = static_cast<uint16_t>(dst_h);
    const uint8_t* gray = data + 8;
    for (int y = 0; y < dst_h; ++y) {
        const int sy = int64_t(y) * src_h / dst_h;
        for (int x = 0; x < dst_w; ++x) {
            const int sx = int64_t(x) * src_w / dst_w;
            const uint8_t g = gray[static_cast<size_t>(sy) * src_w + sx];
            out.pixels[static_cast<size_t>(y) * dst_w + x] = g;
        }
    }
    out.BindDsc();
    return true;
}

void Rgb565ToL8Scaled(const uint8_t* rgb565, int src_w, int src_h, size_t stride, int max_w, int max_h,
                      RasterImage& out) {
    if (src_w <= 0 || src_h <= 0) {
        out.Reset();
        return;
    }
    int dst_w = src_w;
    int dst_h = src_h;
    if (dst_w > max_w || dst_h > max_h) {
        const float sx = static_cast<float>(max_w) / static_cast<float>(dst_w);
        const float sy = static_cast<float>(max_h) / static_cast<float>(dst_h);
        const float s = sx < sy ? sx : sy;
        dst_w = std::max(1, static_cast<int>(dst_w * s));
        dst_h = std::max(1, static_cast<int>(dst_h * s));
    }

    out.pixels.assign(static_cast<size_t>(dst_w) * dst_h, 0xFF);
    out.width = static_cast<uint16_t>(dst_w);
    out.height = static_cast<uint16_t>(dst_h);

    for (int y = 0; y < dst_h; ++y) {
        const int sy = int64_t(y) * src_h / dst_h;
        const uint8_t* row = rgb565 + static_cast<size_t>(sy) * stride;
        for (int x = 0; x < dst_w; ++x) {
            const int sx = int64_t(x) * src_w / dst_w;
            const uint16_t pix = static_cast<uint16_t>(row[sx * 2] | (row[sx * 2 + 1] << 8));
            const uint8_t r = static_cast<uint8_t>(((pix >> 11) & 0x1F) * 255 / 31);
            const uint8_t g = static_cast<uint8_t>(((pix >> 5) & 0x3F) * 255 / 63);
            const uint8_t b = static_cast<uint8_t>((pix & 0x1F) * 255 / 31);
            const uint8_t gray = static_cast<uint8_t>((r * 30 + g * 59 + b * 11) / 100);
            out.pixels[static_cast<size_t>(y) * dst_w + x] = gray;
        }
    }
    out.BindDsc();
}

}  // namespace

bool DecodeImageToL8(const uint8_t* data, size_t len, int max_w, int max_h, RasterImage& out,
                     ImageDecodeCancelled cancelled, void* context, ImageDecodeError* error) try {
    out.Reset();
    if (error) *error = ImageDecodeError::kInvalidData;
    auto finish = [error](bool ok) {
        if (ok && error) *error = ImageDecodeError::kNone;
        return ok;
    };
    if (cancelled != nullptr && cancelled(context)) {
        if (error) *error = ImageDecodeError::kCancelled;
        return false;
    }
    const bool original_size = max_w == 0 && max_h == 0;
    if (data == nullptr || len == 0 || (!original_size && (max_w <= 0 || max_h <= 0))) {
        if (error) *error = ImageDecodeError::kInvalidArgument;
        return false;
    }
    if (LooksLikePng(data, len)) {
        return DecodePngMemoryToL8(data, len, max_w, max_h, out, cancelled, context, error);
    }
    const bool a2l8 = len >= 4 && std::memcmp(data, "A2L8", 4) == 0;
    const bool known = a2l8 || LooksLikeJpeg(data, len) ||
        (len >= 4 && (!std::memcmp(data, "A2I1", 4) || !std::memcmp(data, "EBGR", 4)));
    if (!known) {
        if (error) *error = ImageDecodeError::kUnsupported;
        ESP_LOGW(TAG, "unknown image format (%u bytes)", static_cast<unsigned>(len));
        return false;
    }
    if (original_size && !PeekRasterSize(data, len, &max_w, &max_h)) return false;

    if (a2l8) {
        if (!ValidateA2l8Bytes(data, len)) return false;
        const int width=data[4]|(data[5]<<8),height=data[6]|(data[7]<<8);
        const double scale=std::min({1.0,double(max_w)/width,double(max_h)/height});
        out.width=std::max(1,int(width*scale)); out.height=std::max(1,int(height*scale));
        out.pixels.resize(size_t(out.width)*out.height);
        for(int y=0;y<out.height;++y) for(int x=0;x<out.width;++x)
            out.pixels[size_t(y)*out.width+x]=data[8+size_t(int64_t(y)*height/out.height)*width+int64_t(x)*width/out.width];
        out.BindDsc(); return finish(true);
    }
    if (LooksLikeA2i1(data, len)) {
        return finish(DecodeA2i1ToL8(data, len, max_w, max_h, out));
    }
    if (LooksLikeEbgr(data, len)) {
        return finish(DecodeEbgrToL8(data, len, max_w, max_h, out));
    }

    if (LooksLikeJpeg(data, len)) {
        if (IsProgressiveJpeg(data, len)) {
            return DecodeProgressiveJpeg(data, len, max_w, max_h, out, cancelled, context, error);
        }
#ifndef CONFIG_IDF_TARGET_ESP32
        uint8_t* rgb565 = nullptr;
        size_t out_len = 0, w = 0, h = 0, stride = 0;
        // 解码阶段按 max 缩小（最多约 1/8）+ PSRAM，避免大封面全尺寸 RGB565 OOM
        const esp_err_t status = jpeg_to_image_fit(data, len, max_w, max_h, &rgb565, &out_len, &w, &h, &stride);
        std::unique_ptr<uint8_t, decltype(&heap_caps_free)> decoded(rgb565, heap_caps_free);
        if (status != ESP_OK || !decoded) {
            if (error) *error = status == ESP_ERR_NO_MEM ? ImageDecodeError::kNoMemory :
                status == ESP_ERR_NOT_SUPPORTED ? ImageDecodeError::kUnsupported : ImageDecodeError::kFailed;
            ESP_LOGW(TAG, "jpeg decode failed");
            return false;
        }
        Rgb565ToL8Scaled(decoded.get(), static_cast<int>(w), static_cast<int>(h), stride, max_w, max_h, out);
        return finish(!out.empty());
#else
        if (error) *error = ImageDecodeError::kUnsupported;
        ESP_LOGW(TAG, "jpeg not supported on esp32");
        return false;
#endif
    }

    return false;
} catch (const std::bad_alloc&) {
    if (error) *error = ImageDecodeError::kNoMemory;
    out.Reset();
    ESP_LOGW(TAG, "image decode allocation failed");
    return false;
}

bool DecodeImageFileToL8(const char* path, int max_w, int max_h, RasterImage& out,
                     ImageDecodeCancelled cancelled, void* context, ImageDecodeError* error) {
    out.Reset();
    if (error) *error = ImageDecodeError::kInvalidData;
    if (cancelled != nullptr && cancelled(context)) {
        if (error) *error = ImageDecodeError::kCancelled;
        return false;
    }
    const bool original_size = max_w == 0 && max_h == 0;
    if (!path || !path[0] || (!original_size && (max_w <= 0 || max_h <= 0))) {
        if (error) *error = ImageDecodeError::kInvalidArgument;
        return false;
    }
    std::unique_ptr<FILE, decltype(&std::fclose)> file(std::fopen(path, "rb"), std::fclose);
    if (!file) {
        if (error) *error = ImageDecodeError::kFileIo;
        return false;
    }
    FILE* fp = file.get();
    uint8_t signature[8];
    const size_t signature_size = std::fread(signature, 1, sizeof(signature), fp);
    if (LooksLikePng(signature, signature_size)) {
        file.reset();
        return DecodePngFileToL8(path, max_w, max_h, out, cancelled, context, nullptr, error);
    }
    if (std::ferror(fp) || std::fseek(fp, 0, SEEK_END) != 0) {
        if (error) *error = ImageDecodeError::kFileIo;
        return false;
    }
    const long sz = std::ftell(fp);
    if (sz <= 0 || (!original_size && sz > 4 * 1024 * 1024)) {
        if (error && sz != 0) *error = sz < 0 ? ImageDecodeError::kFileIo : ImageDecodeError::kUnsupported;
        return false;
    }
    std::rewind(fp);
    std::unique_ptr<uint8_t, decltype(&heap_caps_free)> buf(
        static_cast<uint8_t*>(heap_caps_malloc(static_cast<size_t>(sz), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)),
        heap_caps_free);
    if (!buf || (cancelled != nullptr && cancelled(context))) {
        if (error) *error = cancelled && cancelled(context) ? ImageDecodeError::kCancelled : ImageDecodeError::kNoMemory;
        return false;
    }
    size_t read = 0;
    while (read < static_cast<size_t>(sz)) {
        if (cancelled != nullptr && cancelled(context)) break;
        const size_t count = std::min<size_t>(16 * 1024, static_cast<size_t>(sz) - read);
        const size_t n = std::fread(buf.get() + read, 1, count, fp);
        read += n;
        if (n != count) break;
    }
    if (read != static_cast<size_t>(sz)) {
        if (error) *error = cancelled && cancelled(context) ? ImageDecodeError::kCancelled : ImageDecodeError::kFileIo;
        return false;
    }
    file.reset();
    const bool ok = DecodeImageToL8(buf.get(), static_cast<size_t>(sz), max_w, max_h, out, cancelled, context, error);
    if (!ok && (cancelled == nullptr || !cancelled(context))) {
        ESP_LOGW(TAG, "image decode failed: %s (%ld bytes)", path, sz);
    }
    return ok;
}

bool PeekA2i1Size(const uint8_t* data, size_t len, int* w, int* h) {
    if (w == nullptr || h == nullptr || !LooksLikeA2i1(data, len) || len < 10) {
        return false;
    }
    const int sw = data[4] | (data[5] << 8);
    const int sh = data[6] | (data[7] << 8);
    if (sw <= 0 || sh <= 0) {
        return false;
    }
    *w = sw;
    *h = sh;
    return true;
}

bool ScaleRasterToFit(const RasterImage& src, int max_w, int max_h, RasterImage& out) try {
    int dst_w = 0, dst_h = 0;
    if (src.empty() || !FitImageSize(src.width, src.height, max_w, max_h, &dst_w, &dst_h)) {
        out.Reset();
        return false;
    }
    if (dst_w == src.width && dst_h == src.height) {
        if (&out != &src) {
            out = src;
        }
        out.BindDsc();
        return true;
    }

    RasterImage tmp;
    tmp.pixels.assign(static_cast<size_t>(dst_w) * static_cast<size_t>(dst_h), 0xFF);
    tmp.width = static_cast<uint16_t>(dst_w);
    tmp.height = static_cast<uint16_t>(dst_h);
    for (int y = 0; y < dst_h; ++y) {
        const int sy = int64_t(y) * src.height / dst_h;
        for (int x = 0; x < dst_w; ++x) {
            const int sx = int64_t(x) * src.width / dst_w;
            tmp.pixels[static_cast<size_t>(y) * dst_w + x] =
                src.pixels[static_cast<size_t>(sy) * src.width + sx];
        }
    }
    out = std::move(tmp);
    out.BindDsc();
    return !out.empty();
} catch (const std::bad_alloc&) {
    out.Reset();
    ESP_LOGW(TAG, "image scaling allocation failed");
    return false;
}

bool ValidateA2i1Bytes(const uint8_t* data, size_t len) {
    if (!LooksLikeA2i1(data, len)) {
        return false;
    }
    const uint16_t src_w = static_cast<uint16_t>(data[4] | (data[5] << 8));
    const uint16_t src_h = static_cast<uint16_t>(data[6] | (data[7] << 8));
    const uint16_t stride = static_cast<uint16_t>(data[8] | (data[9] << 8));
    if (src_w == 0 || src_h == 0 || stride == 0) {
        return false;
    }
    const size_t min_stride = (static_cast<size_t>(src_w) + 7u) / 8u;
    if (stride < min_stride) {
        return false;
    }
    const size_t need = 20u + static_cast<size_t>(stride) * static_cast<size_t>(src_h);
    return len >= need;
}

bool WriteFileAtomic(const char* path, const uint8_t* data, size_t len) {
    sd_access::Transaction transaction;
    if (!transaction) return false;
    if (path == nullptr || path[0] == '\0' || data == nullptr || len == 0) {
        return false;
    }
    std::string tmp(path);
    tmp.append(".tmp");
    FILE* f = std::fopen(tmp.c_str(), "wb");
    if (f == nullptr) {
        return false;
    }
    bool ok = std::fwrite(data, 1, len, f) == len;
    if (ok && std::fflush(f) != 0) {
        ok = false;
    }
    if (std::fclose(f) != 0) {
        ok = false;
    }
    if (!ok) {
        unlink(tmp.c_str());
        return false;
    }
    // FatFS：目标存在时 rename 常失败 → 先删旧。若随后 rename 仍失败，保留 .tmp
    // 供调用方/LoadPos 回退；切勿再 unlink(tmp)，否则唯一完好副本被毁掉。
    unlink(path);
    if (rename(tmp.c_str(), path) != 0) {
        return false;
    }
    return true;
}

bool ValidateA2l8Bytes(const uint8_t* data,size_t len) {
    if(!data || len<8 || std::memcmp(data,"A2L8",4)) return false;
    const unsigned width=data[4]|(unsigned(data[5])<<8),height=data[6]|(unsigned(data[7])<<8);
    return width && height && size_t(width)*height<=len-8;
}
bool EncodeL8ToA2l8(const RasterImage& image,std::vector<uint8_t>& out) {
    out.clear();
    const size_t pixels=size_t(image.width)*image.height;
    if(!pixels || image.pixels.size()<pixels) return false;
    out.resize(8+pixels);
    std::memcpy(out.data(),"A2L8",4);
    out[4]=image.width&255;out[5]=image.width>>8;
    out[6]=image.height&255;out[7]=image.height>>8;
    std::memcpy(out.data()+8,image.pixels.data(),pixels);
    return true;
}
bool PeekRasterSize(const uint8_t* data,size_t len,int* width,int* height) {
    if(!data || !width || !height) return false;
    if(len>=8 && (!std::memcmp(data,"A2L8",4) || !std::memcmp(data,"EBGR",4))) {
        *width=data[4]|(data[5]<<8);*height=data[6]|(data[7]<<8);
        return *width>0 && *height>0;
    }
    if(PeekA2i1Size(data,len,width,height)) return true;
    if(len>=24 && LooksLikePng(data,len)) {
        const auto be32=[](const uint8_t* p) { return unsigned(p[0])<<24|unsigned(p[1])<<16|unsigned(p[2])<<8|p[3]; };
        const unsigned w=be32(data+16),h=be32(data+20);
        if(!w || !h || w>65535 || h>65535) return false;
        *width=w;*height=h;return true;
    }
    if(LooksLikeJpeg(data,len)) {
        size_t pos=2;
        while(pos+3<len) {
            if(data[pos++]!=255) return false;
            while(pos<len && data[pos]==255) ++pos;
            if(pos>=len) return false;
            const unsigned marker=data[pos++];
            if(marker==0xd9 || marker==0xda) return false;
            if(marker==0x01 || (marker>=0xd0 && marker<=0xd7)) continue;
            if(pos+2>len) return false;
            const unsigned size=(unsigned(data[pos])<<8)|data[pos+1];
            if(size<2 || size>len-pos) return false;
            if((marker>=0xc0 && marker<=0xc3) || (marker>=0xc5 && marker<=0xc7) ||
               (marker>=0xc9 && marker<=0xcb) || (marker>=0xcd && marker<=0xcf)) {
                if(size<8) return false;
                *height=(data[pos+3]<<8)|data[pos+4];*width=(data[pos+5]<<8)|data[pos+6];
                return *width>0 && *height>0;
            }
            pos+=size;
        }
    }
    return false;
}
bool PeekRasterFileSize(const char* path,int* width,int* height) {
    if(!path) return false;
    FILE* file=std::fopen(path,"rb"); if(!file) return false;
    // Pixel envelopes and PNG keep dimensions in the first 24 bytes.
    uint8_t header[24];const size_t count=std::fread(header,1,sizeof(header),file);
    if(PeekRasterSize(header,count,width,height)) { std::fclose(file);return true; }
    if(std::fseek(file,0,SEEK_END)) { std::fclose(file);return false; }
    const long size=std::ftell(file);
    if(size<=0 || size>4*1024*1024) { std::fclose(file);return false; }
    std::rewind(file);std::vector<uint8_t> data(size);
    const bool read=std::fread(data.data(),1,data.size(),file)==data.size();
    std::fclose(file);return read && PeekRasterSize(data.data(),data.size(),width,height);
}
}
