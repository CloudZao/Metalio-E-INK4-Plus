#pragma once
#include <memory>
#include <lvgl.h>
#include "business/app_store/app_api.h"
#include "business/app_store/app_store.h"
#include "infra/image/raster_image.h"

struct AppRuntimeImages;

/** @brief 创建应用图片服务，不分配像素缓冲 */
std::shared_ptr<AppRuntimeImages> AppImagesCreate(lv_obj_t* root, const AppStoreItem& item, uint32_t asset_size);

/** @brief 创建等比居中的托管图片区域 */
void* AppImagesWidget(const std::shared_ptr<AppRuntimeImages>& state, void* parent, int x, int y, int width, int height);

/** @brief 异步读取并解码应用资源中的独立图片区域 */
int AppImagesLoad(const std::shared_ptr<AppRuntimeImages>& state, void* widget, uint32_t offset, uint32_t length);

/** @brief 对保存的原始灰度图重新量化，不修改源图片 */
int AppImagesEffects(const std::shared_ptr<AppRuntimeImages>& state, void* widget, const xz_app_image_effects_t* effects);

/** @brief 查询图片请求结果和板型灰阶能力 */
int AppImagesStatus(const std::shared_ptr<AppRuntimeImages>& state, void* widget, xz_app_image_status_t* result);

/** @brief 查询当前显示处理参数 */
int AppImagesGetEffects(const std::shared_ptr<AppRuntimeImages>& state, void* widget, xz_app_image_effects_t* result);

/** @brief 分块复制有界原始或显示 L8 像素，每次最多 64 KiB */
int AppImagesReadPixels(const std::shared_ptr<AppRuntimeImages>& state, void* widget, uint32_t processed,
                         uint32_t offset, void* buffer, uint32_t length);

/** @brief 复制应用提供的紧排 L8 像素作为新原图并应用当前处理参数 */
int AppImagesSetPixels(const std::shared_ptr<AppRuntimeImages>& state, void* widget, uint32_t width, uint32_t height,
                        const void* pixels, uint32_t length);

/** @brief 查询仍属于本应用的图片区域尺寸 */
bool AppImagesSize(const std::shared_ptr<AppRuntimeImages>& state, void* widget, int& width, int& height);

/** @brief 在 UI 线程交付摄像头图像；空图仅取消该区域尚未完成的资源解码 */
void AppImagesPresent(const std::shared_ptr<AppRuntimeImages>& state, void* widget, std::shared_ptr<RasterImage> image);

/** @brief 在 UI 线程应用已经完成的后台解码结果 */
void AppImagesPoll(const std::shared_ptr<AppRuntimeImages>& state);

/** @brief 取消所有后台图片请求，控件删除时归还显示缓冲 */
void AppImagesClose(const std::shared_ptr<AppRuntimeImages>& state);
