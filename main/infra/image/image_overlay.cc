#include <cerrno>
#include <cstdio>
#include <cstring>
#include <memory>
#include <new>
#include <esp_heap_caps.h>
#include <esp_memory_utils.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include "infra/image/image_geometry.h"
#include "image_stream.h"
#include "image_decode.h"
#include "infra/diagnostics/firmware_memory.h"

bool DecodeImageFileOverlay(const char* path, int max_w, int max_h, RasterImage& out, std::vector<uint8_t>& alpha,
                            ImageDecodeCancelled cancelled, void* context, ImageDecodeError* error, StorageError* storage_error) try {
    StorageError local_error;
    const bool report_error = storage_error == nullptr;
    if (!storage_error) {
        storage_error = &local_error;
    }
    *storage_error = {};
    struct StorageReporter {
        StorageError* error;
        bool enabled;
        ImageDecodeCancelled cancelled;
        void* context;
        ~StorageReporter() {
            if (enabled && error->code != StorageErrorCode::NotFound && !(cancelled && cancelled(context))) {
                LogStorageError("ImageFile", *error);
            }
        }
    } reporter{storage_error, report_error, cancelled, context};
    out.Reset();
    alpha.clear();
    bool success = false;

    struct FailureCleanup {
        RasterImage& image;           // 当前调用的结果容器
        std::vector<uint8_t>& alpha;  // 当前调用的透明度
        bool& success;                // 已完成有效结果

        ~FailureCleanup() {
            if (!success) {
                image.Reset();
                std::vector<uint8_t>().swap(image.pixels);
                std::vector<uint8_t>().swap(alpha);
            }
        }
    } cleanup{out, alpha, success};

    if (error) {
        *error = ImageDecodeError::kInvalidData;
    }
    if (cancelled && cancelled(context)) {
        if (error) {
            *error = ImageDecodeError::kCancelled;
        }
        return false;
    }
    if (!path || max_w <= 0 || max_h <= 0) {
        if (error) {
            *error = ImageDecodeError::kInvalidArgument;
        }
        return false;
    }
    FirmwareMemoryDemand demand(size_t(max_w) * max_h * 3 + 256 * 1024);
    std::unique_ptr<FILE, decltype(&std::fclose)> file(std::fopen(path, "rb"), std::fclose);
    if (!file) {
        CaptureStorageError(storage_error, StorageStage::Open, errno);
        if (error) {
            *error = ImageDecodeError::kFileIo;
        }
        return false;
    }
    uint8_t header[20] = {};
    const size_t count = std::fread(header, 1, sizeof(header), file.get());
    if (std::ferror(file.get())) {
        CaptureStorageError(storage_error, StorageStage::Read, errno);
        if (error) {
            *error = ImageDecodeError::kFileIo;
        }
        return false;
    }
    if (count >= 8 && std::memcmp(header, "\x89PNG\r\n\x1a\n", 8) == 0) {
        file.reset();
        success = DecodePngFileToL8(path, max_w, max_h, out, cancelled, context, &alpha, error, storage_error);
        return success;
    }
    if (count >= 4 && !std::memcmp(header, "A2I1", 4) && count < 20) {
        return false;
    }
    const bool i1 = count == 20 && std::memcmp(header, "A2I1", 4) == 0;
    const bool l8 = count >= 8 && (!std::memcmp(header, "A2L8", 4) || !std::memcmp(header, "EBGR", 4));
    if (i1 || l8) {
        const int width = header[4] | (unsigned(header[5]) << 8);
        const int height = header[6] | (unsigned(header[7]) << 8);
        const int stride = i1 ? header[8] | (unsigned(header[9]) << 8) : width;
        const long offset = i1 ? 20 : 8;
        if (width <= 0 || height <= 0 || stride < (i1 ? (width + 7) / 8 : width)) {
            return false;
        }
        if (std::fseek(file.get(), 0, SEEK_END) != 0) {
            CaptureStorageError(storage_error, StorageStage::Seek, errno);
            if (error) {
                *error = ImageDecodeError::kFileIo;
            }
            return false;
        }
        const long length = std::ftell(file.get());
        if (length < 0) {
            CaptureStorageError(storage_error, StorageStage::Seek, errno);
            if (error) {
                *error = ImageDecodeError::kFileIo;
            }
            return false;
        }
        if (uint64_t(length) < uint64_t(offset) + uint64_t(stride) * height) {
            return false;
        }
        int fitted_w = 0, fitted_h = 0;
        if (!FitImageSize(width, height, max_w, max_h, &fitted_w, &fitted_h)) {
            return false;
        }
        out.width = fitted_w;
        out.height = fitted_h;
        const size_t bytes = size_t(out.width) * out.height;
        constexpr size_t reserve = 2 * 1024 * 1024;
        const size_t free = heap_caps_get_free_size(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        const bool transparent = i1 && (header[12] != 255 || header[16] != 255);
        const size_t needed = bytes * (transparent ? 2 : 1) + width;
        if (needed > 8 * 1024 * 1024 || free < reserve || needed > free - reserve) {
            if (error) {
                *error = ImageDecodeError::kNoMemory;
            }
            return false;
        }
        out.pixels.resize(bytes);
        if (bytes > 4096 && !esp_ptr_external_ram(out.pixels.data())) {
            if (error) {
                *error = ImageDecodeError::kNoMemory;
            }
            return false;
        }
        if (transparent) {
            alpha.resize(out.pixels.size());
        }
        if (alpha.size() > 4096 && !esp_ptr_external_ram(alpha.data())) {
            if (error) {
                *error = ImageDecodeError::kNoMemory;
            }
            return false;
        }
        const int row_bytes = i1 ? (width + 7) / 8 : width;
        std::unique_ptr<uint8_t, decltype(&heap_caps_free)> row_storage(
            static_cast<uint8_t*>(heap_caps_malloc(row_bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)), heap_caps_free);
        if (!row_storage) {
            if (error) {
                *error = ImageDecodeError::kNoMemory;
            }
            return false;
        }
        uint8_t* row = row_storage.get();
        for (int y = 0; y < out.height; ++y) {
            if (cancelled && cancelled(context)) {
                if (error) {
                    *error = ImageDecodeError::kCancelled;
                }
                return false;
            }
            const int sy = int64_t(y) * height / out.height;
            if (std::fseek(file.get(), offset + long(sy) * stride, SEEK_SET) != 0) {
                CaptureStorageError(storage_error, StorageStage::Seek, errno);
                if (error) {
                    *error = ImageDecodeError::kFileIo;
                }
                return false;
            }
            if (std::fread(row, 1, row_bytes, file.get()) != size_t(row_bytes)) {
                if (std::ferror(file.get())) {
                    CaptureStorageError(storage_error, StorageStage::Read, errno);
                    if (error) {
                        *error = ImageDecodeError::kFileIo;
                    }
                }
                return false;
            }
            for (int x = 0; x < out.width; ++x) {
                const size_t index = size_t(y) * out.width + x;
                const int sx = int64_t(x) * width / out.width;
                if (!i1) {
                    out.pixels[index] = row[sx];
                    continue;
                }
                const unsigned color = (row[sx / 8] >> (7 - sx % 8)) & 1;
                const uint8_t* palette = header + 12 + color * 4;
                out.pixels[index] = (palette[1] * 30 + palette[2] * 59 + palette[3] * 11) / 100;
                if (!alpha.empty()) {
                    alpha[index] = palette[0];
                }
            }
            if ((y & 31) == 31) {
                vTaskDelay(1);
            }
        }
        out.BindDsc();
        success = true;
        if (error) {
            *error = ImageDecodeError::kNone;
        }
        return true;
    }
    file.reset();
    success = DecodeImageFileToL8(path, max_w, max_h, out, cancelled, context, error, storage_error);
    return success;
} catch (const std::bad_alloc&) {
    if (error) {
        *error = ImageDecodeError::kNoMemory;
    }
    return false;
}
