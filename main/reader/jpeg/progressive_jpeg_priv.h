#pragma once

#include "progressive_jpeg.h"
#include "sd_access.h"

#include <cstdio>
#include <optional>

namespace reader::jpeg {
constexpr size_t kPsramReserve = 2 * 1024 * 1024; // 保留给显示和其他业务的 PSRAM
constexpr size_t kDecodeBudget = 8 * 1024 * 1024; // 解码工作区与结果的单次上限

struct DecodeContext {
    const uint8_t* data = nullptr; // 借用内存输入，与 input 二选一
    FILE* input = nullptr; // 借用文件输入，不接管关闭
    size_t input_size = 0; // 输入字节数，已检查文件偏移范围
    int max_width = 0; // 输出宽度上限
    int max_height = 0; // 输出高度上限
    ImageDecodeCancelled cancelled = nullptr; // 调用方取消条件
    void* user = nullptr; // 取消条件上下文
    ImageDecodeError error = ImageDecodeError::kNone; // 首次错误
    RasterImage* output = nullptr; // 借用最终 L8 容器
    uint8_t* arena = nullptr; // 格式工作区，强制 PSRAM
    size_t arena_size = 0; // 工作区容量
    size_t used = 0; // 工作区已用字节数
    uint8_t* work_cache = nullptr; // 扫描快照与中间系数条带，强制 PSRAM
    size_t work_cache_size = 0; // 条带或分页缓存容量
    size_t output_bytes = 0; // 最终 L8 预留字节数
    FILE* work = nullptr; // 按需创建的系数后备文件
    uint32_t work_size = 0; // 已初始化的工作区末尾
    char work_path[128] = {}; // 本次请求独占的工作文件
    std::optional<sd_access::Transaction> transaction; // 覆盖创建、关闭和清理
    uint32_t last_yield = 0; // 周期性让出 CPU 的时刻

    ~DecodeContext();
    bool Cancelled();
    void Fail(ImageDecodeError failure) { if (error == ImageDecodeError::kNone) error = failure; }
    uint32_t InputSize() const { return static_cast<uint32_t>(input_size); }
    bool Read(uint32_t offset, void* bytes, size_t size);
    void* Allocate(size_t size);
    void CacheWork(uint32_t bytes);
    bool Begin(uint16_t width, uint16_t height);
    bool Output(uint32_t offset, const void* bytes, size_t size);
    bool WorkRead(uint32_t offset, void* bytes, size_t size);
    bool WorkWrite(uint32_t offset, const void* bytes, size_t size);
};

/** @brief 获取有界渐进式格式工作区大小 */
size_t ProgressiveWorkspaceSize();
/** @brief 使用扫描条带或系数分页完成渐进式解码 */
bool DecodePagedProgressive(DecodeContext& context);
/** @brief 在现有 L8 容器中生成有界解码结果，失败时释放工作资源 */
bool DecodeProgressiveFallback(const uint8_t* data, FILE* input, size_t size, int max_w, int max_h,
                               RasterImage& out, ImageDecodeCancelled cancelled, void* user,
                               ImageDecodeError* error);
} // namespace reader::jpeg
