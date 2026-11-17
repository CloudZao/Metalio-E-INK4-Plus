#pragma once
#include <array>
#include <cstdio>
#include <memory>
#include <lvgl.h>

namespace AppStore { struct Item; }
namespace AppStoreIcons {
struct Icon {
    alignas(4) std::array<unsigned char, 2056> pixels{}; // LVGL 调色板及单色像素
    lv_image_dsc_t image{}; // 引用同一对象中的像素数据
};
/** @brief 返回应用商店内嵌图标或通用图标 */
const lv_image_dsc_t* Source(const char* app_id);
/** @brief 返回应用 ELF 自带图标，缺失时使用通用图标 */
const lv_image_dsc_t* Source(const AppStore::Item& item);
/** @brief 设置图标源；将 128 像素单色图标预缩放至 1–128 像素，随控件释放 */
void SetSource(lv_obj_t* image, const void* source, int edge);
/** @brief 解码版本 1 的 XZIC 单色图标资源 */
std::shared_ptr<const Icon> Decode(const unsigned char* data, size_t size);
/** @brief 从 ELF 的非执行资源段读取图标，不加载或运行应用 */
std::shared_ptr<const Icon> ReadElf(FILE* file, size_t size);
}
