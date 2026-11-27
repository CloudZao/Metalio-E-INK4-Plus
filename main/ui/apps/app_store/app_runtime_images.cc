#include <algorithm>
#include <atomic>
#include <cstring>
#include <mutex>
#include <new>
#include <utility>
#include <vector>
#include <esp_log.h>
#include <src/misc/cache/instance/lv_image_cache.h>
#include "app_runtime_images.h"
#include "boards/common/storage/sd_access.h"
#include "config.h"
#include "infra/diagnostics/firmware_memory.h"
#include "infra/image/image_decode.h"
#include "infra/image/image_process.h"
#include "sysmgmt/tasks/task_executor.h"
#include "ui/common/drawing/rendering/epd_content_policy.h"

struct AppRuntimeImage {
    lv_obj_t* object = nullptr; // 仅 UI 线程访问
    int width = 0, height = 0; // 创建时确定的解码尺寸上限
    std::atomic<bool> closed{false};
    std::atomic<uint32_t> generation{0}; // 取消旧请求和迟到的解码结果
    std::mutex mutex; // 保护后台结果交接
    uint32_t completed_generation = 0;
    bool completed = false;
    int pending_result = XZ_APP_OK;
    std::shared_ptr<RasterImage> pending;
    std::shared_ptr<RasterImage> original; // 原始 L8，不累计转换误差
    RasterImage display; // LVGL 借用的量化显示副本
    xz_app_image_effects_t effects{sizeof(xz_app_image_effects_t), XZ_APP_IMAGE_GRAY16, 0, -1, 0, 0,
                                  0, 0, 100, 0, 0, 0, 0, 0, 0, 0, 10000, 10000};
    int result = XZ_APP_OK;
    bool loading = false;
    uint32_t revision = 0; // UI 线程完成显示像素更新后递增
};

struct AppRuntimeImages {
    lv_obj_t* root; // 所属应用内容区
    AppStoreItem item; // 后台任务持有资源文件身份，不借用 ELF
    uint32_t asset_size = 0; // 启动时已校验的资源节大小
    bool closed = false;
    std::vector<std::shared_ptr<AppRuntimeImage>> images; // 最多四个图片区域
};

static std::shared_ptr<AppRuntimeImage> FindImage(const std::shared_ptr<AppRuntimeImages>& state, void* widget) {
    if (!state || state->closed) {
        return {};
    }
    for (const auto& image : state->images) {
        if (image->object == widget && !image->closed.load()) {
            return image;
        }
    }
    return {};
}

static ImageProcessingOptions ProcessingOptions(const xz_app_image_effects_t& effects) {
    ImageProcessingOptions options;
    options.levels = effects.mode == XZ_APP_IMAGE_GRAY16 ? 16 : effects.mode == XZ_APP_IMAGE_GRAY8 ? 8 : 2;
    options.dither = effects.mode == XZ_APP_IMAGE_DITHER;
    options.contrast = effects.contrast;
    options.brightness = effects.brightness;
    options.exposure = effects.exposure;
    options.shadows = effects.shadows;
    options.highlights = effects.highlights;
    options.gamma = effects.gamma;
    options.sharpness = effects.sharpness;
    options.invert = effects.invert;
    options.mirror_x = effects.mirror_x;
    options.mirror_y = effects.mirror_y;
    options.rotation = effects.rotation;
    options.crop_x = effects.crop_x;
    options.crop_y = effects.crop_y;
    options.crop_width = effects.crop_width;
    options.crop_height = effects.crop_height;
    options.threshold = effects.threshold < 0 ? 128 : effects.threshold;
    return options;
}

static int RenderImage(AppRuntimeImage& image) {
    if (!image.original || image.original->empty()) {
        return XZ_APP_OK;
    }
    try {
        const auto options = ProcessingOptions(image.effects);
        ImageProcessingLayout layout;
        if (!GetImageProcessingLayout(image.original->width, image.original->height, options, layout)) {
            ESP_LOGE("AppImage", "operation=render stage=layout result=invalid");
            return XZ_APP_ERR_INVALID;
        }
        const size_t bytes = size_t(layout.output_width) * layout.output_height;
        RasterImage rendered;
        rendered.width = layout.output_width;
        rendered.height = layout.output_height;
        if (!FirmwarePsramCanAllocate(bytes)) {
            if (image.result != XZ_APP_ERR_MEMORY) {
                ESP_LOGE("AppImage", "operation=render stage=budget result=no_memory");
            }
            return XZ_APP_ERR_MEMORY;
        }
        rendered.pixels.resize(bytes);
        if (!ProcessImageL8(image.original->pixels.data(), image.original->pixels.size(), image.original->width,
                            image.original->height, options, rendered.pixels.data(), bytes)) {
            ESP_LOGE("AppImage", "operation=render stage=process result=invalid");
            return XZ_APP_ERR_INVALID;
        }
        lv_image_cache_drop(&image.display.dsc);
        image.display = std::move(rendered);
        lv_image_set_src(image.object, &image.display.dsc);
        EpdSetContentRole(image.object, EpdContentRole::ApplicationImage);
        ++image.revision;
        return XZ_APP_OK;
    } catch (const std::bad_alloc&) {
        if (image.result != XZ_APP_ERR_MEMORY) {
            ESP_LOGE("AppImage", "operation=render stage=allocate result=no_memory");
        }
        return XZ_APP_ERR_MEMORY;
    }
}

static int ReplaceOriginalImage(AppRuntimeImage& image, std::shared_ptr<RasterImage> pixels) {
    auto previous = std::exchange(image.original, std::move(pixels));
    image.result = RenderImage(image);
    if (image.result != XZ_APP_OK) {
        image.original = std::move(previous);
    }
    return image.result;
}

std::shared_ptr<AppRuntimeImages> AppImagesCreate(lv_obj_t* root, const AppStoreItem& item, uint32_t asset_size) {
    auto state = std::make_shared<AppRuntimeImages>();
    state->root = root;
    state->item = item;
    state->asset_size = asset_size;
    return state;
}

void* AppImagesWidget(const std::shared_ptr<AppRuntimeImages>& state, void* parent, int x, int y, int width, int height) {
    if (!state || state->closed || !parent || !lv_obj_is_valid(static_cast<lv_obj_t*>(parent)) ||
        width < 1 || height < 1 || width > 1600 || height > 1600 || uint64_t(width) * height > 1024 * 1024) {
        return nullptr;
    }
    auto* ancestor = static_cast<lv_obj_t*>(parent);
    while (ancestor && ancestor != state->root) {
        ancestor = lv_obj_get_parent(ancestor);
    }
    if (!ancestor) {
        return nullptr;
    }
    std::erase_if(state->images, [](const auto& image) { return image->closed.load(); });
    if (state->images.size() >= 4) {
        return nullptr;
    }
    try {
        auto image = std::make_shared<AppRuntimeImage>();
        if (BOARD_DISPLAY_GRAY_LEVELS < 8) {
            image->effects.mode = XZ_APP_IMAGE_DITHER;
        }
        auto owner = std::make_unique<std::shared_ptr<AppRuntimeImage>>(image);
        image->width = width;
        image->height = height;
        state->images.push_back(image);
        auto* object = lv_image_create(static_cast<lv_obj_t*>(parent));
        if (!object) {
            state->images.pop_back();
            ESP_LOGE("AppImage", "operation=create stage=widget result=no_memory");
            return nullptr;
        }
        image->object = object;
        lv_obj_set_pos(object, x, y);
        lv_obj_set_size(object, width, height);
        lv_image_set_inner_align(object, LV_IMAGE_ALIGN_CONTAIN);
        lv_image_set_antialias(object, false);
        lv_obj_remove_flag(object, static_cast<lv_obj_flag_t>(LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE));
        lv_obj_add_event_cb(object, [](lv_event_t* event) {
            auto* held = static_cast<std::shared_ptr<AppRuntimeImage>*>(lv_event_get_user_data(event));
            auto& image = **held;
            image.closed.store(true);
            ++image.generation;
            image.object = nullptr;
            lv_image_cache_drop(&image.display.dsc);
            image.original.reset();
            image.display.Reset();
            delete held;
        }, LV_EVENT_DELETE, owner.release());
        return object;
    } catch (const std::bad_alloc&) {
        ESP_LOGE("AppImage", "operation=create stage=allocate result=no_memory");
        return nullptr;
    }
}

int AppImagesLoad(const std::shared_ptr<AppRuntimeImages>& state, void* widget, uint32_t offset, uint32_t length) try {
    auto image = FindImage(state, widget);
    if (!image || !length || offset > state->asset_size || length > state->asset_size - offset) {
        return XZ_APP_ERR_INVALID;
    }
    if (length > 4 * 1024 * 1024) {
        return XZ_APP_ERR_LIMIT;
    }
    const uint32_t generation = ++image->generation;
    const AppStoreItem item = state->item;
    image->loading = true;
    const bool accepted = TaskExecutor::Schedule([image, item, offset, length, generation] {
        struct Request {
            AppRuntimeImage& image;
            uint32_t generation;
        } request{*image, generation};
        const auto cancelled = [](void* context) {
            auto& request = *static_cast<Request*>(context);
            return request.image.closed.load() || request.image.generation.load() != request.generation;
        };
        int result = XZ_APP_OK;
        ImageDecodeError error = ImageDecodeError::kNone;
        auto decoded = std::shared_ptr<RasterImage>();
        try {
            if (!cancelled(&request)) {
                const size_t working = size_t(image->width) * image->height * 3 + 256 * 1024;
                FirmwareMemoryDemand demand(length + working);
                if (!FirmwarePsramCanAllocate(length, working)) {
                    result = XZ_APP_ERR_MEMORY;
                } else {
                    SdAccessTransaction transaction;
                    if (!transaction) {
                        result = XZ_APP_ERR_NOT_READY;
                    } else {
                        std::vector<uint8_t> bytes(length);
                        decoded = std::make_shared<RasterImage>();
                        if (AppStoreReadAsset(item, offset, bytes.data(), length) != length) {
                            result = XZ_APP_ERR_IO;
                        } else if (!DecodeImageToL8(bytes.data(), length, image->width, image->height, *decoded,
                                                   cancelled, &request, &error)) {
                            switch (error) {
                                case ImageDecodeError::kNoMemory:
                                    result = XZ_APP_ERR_MEMORY;
                                    break;
                                case ImageDecodeError::kUnsupported:
                                    result = XZ_APP_ERR_UNSUPPORTED;
                                    break;
                                case ImageDecodeError::kInvalidArgument:
                                case ImageDecodeError::kInvalidData:
                                    result = XZ_APP_ERR_INVALID;
                                    break;
                                default:
                                    result = XZ_APP_ERR_IO;
                                    break;
                            }
                        }
                    }
                }
            }
        } catch (const std::bad_alloc&) {
            result = XZ_APP_ERR_MEMORY;
        } catch (...) {
            result = XZ_APP_ERR_IO;
        }
        if (cancelled(&request)) {
            return;
        }
        if (result != XZ_APP_OK) {
            ESP_LOGE("AppImage", "operation=decode stage=resource result=%d cause=%d bytes=%lu", result, int(error),
                     static_cast<unsigned long>(length));
        }
        std::lock_guard<std::mutex> lock(image->mutex);
        image->pending = result == XZ_APP_OK ? std::move(decoded) : nullptr;
        image->pending_result = result;
        image->completed_generation = generation;
        image->completed = true;
    }, TaskDomain::Image);
    if (!accepted) {
        image->loading = false;
        image->result = XZ_APP_ERR_BUSY;
        ESP_LOGW("AppImage", "operation=decode stage=queue result=busy");
        return XZ_APP_ERR_BUSY;
    }
    return XZ_APP_OK;
} catch (const std::bad_alloc&) {
    auto image = FindImage(state, widget);
    if (image) {
        ++image->generation;
        image->loading = false;
        image->result = XZ_APP_ERR_MEMORY;
    }
    ESP_LOGE("AppImage", "operation=decode stage=request result=no_memory");
    return XZ_APP_ERR_MEMORY;
}

int AppImagesEffects(const std::shared_ptr<AppRuntimeImages>& state, void* widget, const xz_app_image_effects_t* effects) {
    auto image = FindImage(state, widget);
    if (!image || !effects || effects->struct_size < sizeof(*effects) || effects->mode > XZ_APP_IMAGE_MONO ||
        effects->threshold < -1 || effects->threshold > 255 || effects->invert > 1 ||
        effects->mirror_x > 1 || effects->mirror_y > 1) {
        return XZ_APP_ERR_INVALID;
    }
    ImageProcessingLayout layout;
    if (!GetImageProcessingLayout(1, 1, ProcessingOptions(*effects), layout)) {
        return XZ_APP_ERR_INVALID;
    }
    if ((effects->mode == XZ_APP_IMAGE_GRAY16 && BOARD_DISPLAY_GRAY_LEVELS < 16) ||
        (effects->mode == XZ_APP_IMAGE_GRAY8 && BOARD_DISPLAY_GRAY_LEVELS < 8)) {
        return XZ_APP_ERR_UNSUPPORTED;
    }
    const auto previous = image->effects;
    image->effects = *effects;
    image->effects.struct_size = sizeof(image->effects);
    const int result = RenderImage(*image);
    if (result != XZ_APP_OK) {
        image->effects = previous;
    }
    image->result = result;
    return result;
}

int AppImagesStatus(const std::shared_ptr<AppRuntimeImages>& state, void* widget, xz_app_image_status_t* result) {
    auto image = FindImage(state, widget);
    if (!image || !result || result->struct_size < sizeof(uint32_t)) {
        return XZ_APP_ERR_INVALID;
    }
    const xz_app_image_status_t status{sizeof(status), image->loading, image->result,
                                      image->display.width, image->display.height, BOARD_DISPLAY_GRAY_LEVELS,
                                      image->effects.threshold < 0 ? 128u : uint32_t(image->effects.threshold),
                                      image->original ? image->original->width : 0u,
                                      image->original ? image->original->height : 0u, image->revision};
    std::memcpy(result, &status, std::min<size_t>(result->struct_size, sizeof(status)));
    return XZ_APP_OK;
}

int AppImagesGetEffects(const std::shared_ptr<AppRuntimeImages>& state, void* widget, xz_app_image_effects_t* result) {
    auto image = FindImage(state, widget);
    if (!image || !result || result->struct_size < sizeof(uint32_t)) {
        return XZ_APP_ERR_INVALID;
    }
    std::memcpy(result, &image->effects, std::min<size_t>(result->struct_size, sizeof(image->effects)));
    return XZ_APP_OK;
}

int AppImagesReadPixels(const std::shared_ptr<AppRuntimeImages>& state, void* widget, uint32_t processed,
                         uint32_t offset, void* buffer, uint32_t length) {
    auto image = FindImage(state, widget);
    if (!image || processed > 1 || (!buffer && length)) {
        return XZ_APP_ERR_INVALID;
    }
    if (length > 64 * 1024) {
        return XZ_APP_ERR_LIMIT;
    }
    const RasterImage* raster = processed ? &image->display : image->original.get();
    if (!raster || raster->empty()) {
        return XZ_APP_ERR_NOT_READY;
    }
    if (offset > raster->pixels.size()) {
        return XZ_APP_ERR_INVALID;
    }
    const size_t bytes = std::min<size_t>(length, raster->pixels.size() - offset);
    if (bytes) {
        std::memcpy(buffer, raster->pixels.data() + offset, bytes);
    }
    return bytes;
}

int AppImagesSetPixels(const std::shared_ptr<AppRuntimeImages>& state, void* widget, uint32_t width, uint32_t height,
                        const void* pixels, uint32_t length) {
    auto image = FindImage(state, widget);
    if (!image || !pixels || !width || !height || width > 1600 || height > 1600 ||
        uint64_t(width) * height > 1024 * 1024 || uint64_t(width) * height != length) {
        return XZ_APP_ERR_INVALID;
    }
    try {
        FirmwareMemoryDemand demand(size_t(length) * 2);
        if (!FirmwarePsramCanAllocate(size_t(length) * 2)) {
            ESP_LOGE("AppImage", "operation=set_pixels stage=budget result=no_memory bytes=%lu",
                     static_cast<unsigned long>(length));
            image->result = XZ_APP_ERR_MEMORY;
            return XZ_APP_ERR_MEMORY;
        }
        auto raster = std::make_shared<RasterImage>();
        raster->width = width;
        raster->height = height;
        raster->pixels.resize(length);
        std::memcpy(raster->pixels.data(), pixels, length);
        raster->BindDsc();
        ++image->generation;
        image->loading = false;
        return ReplaceOriginalImage(*image, std::move(raster));
    } catch (const std::bad_alloc&) {
        ESP_LOGE("AppImage", "operation=set_pixels stage=allocate result=no_memory bytes=%lu",
                 static_cast<unsigned long>(length));
        image->result = XZ_APP_ERR_MEMORY;
        return XZ_APP_ERR_MEMORY;
    }
}

bool AppImagesSize(const std::shared_ptr<AppRuntimeImages>& state, void* widget, int& width, int& height) {
    auto image = FindImage(state, widget);
    if (!image) {
        return false;
    }
    width = image->width;
    height = image->height;
    return true;
}

void AppImagesPresent(const std::shared_ptr<AppRuntimeImages>& state, void* widget, std::shared_ptr<RasterImage> pixels) {
    auto image = FindImage(state, widget);
    if (!image) {
        return;
    }
    ++image->generation;
    image->loading = false;
    if (!pixels) {
        return;
    }
    ReplaceOriginalImage(*image, std::move(pixels));
}

void AppImagesPoll(const std::shared_ptr<AppRuntimeImages>& state) {
    if (!state || state->closed) {
        return;
    }
    std::erase_if(state->images, [](const auto& image) { return image->closed.load(); });
    for (const auto& image : state->images) {
        std::shared_ptr<RasterImage> pixels;
        {
            std::lock_guard<std::mutex> lock(image->mutex);
            if (!image->completed) {
                continue;
            }
            image->completed = false;
            if (image->closed.load() || image->generation.load() != image->completed_generation) {
                image->pending.reset();
                continue;
            }
            image->loading = false;
            image->result = image->pending_result;
            pixels = std::move(image->pending);
        }
        if (pixels) {
            ReplaceOriginalImage(*image, std::move(pixels));
        }
    }
}

void AppImagesClose(const std::shared_ptr<AppRuntimeImages>& state) {
    if (state) {
        state->closed = true;
        for (const auto& image : state->images) {
            image->closed.store(true);
            ++image->generation;
            image->original.reset();
        }
    }
}
