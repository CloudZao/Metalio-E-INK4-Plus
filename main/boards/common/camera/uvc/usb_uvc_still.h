#pragma once

#include <cstddef>
#include <cstdint>
#include <esp_err.h>
#include "boards/common/camera/camera_capture.h"

// USB UVC 静拍：一次采集持有完整 Host 会话，结束后恢复 USB Serial/JTAG。
// 调用顺序为：切 MUX -> StartSession -> CaptureOne -> StopSession -> 关闭 OTG。
// 不要与虚拟 U 盘并发；Start/Capture/Detect/Stop 不得在 LVGL 线程调用。
// 停止会话按根端口、路由、UVC、设备、事件任务、Host 与 PHY 的顺序释放。
class UsbUvcStill {
public:
    struct Request {
        bool (*cancelled)(void*) = nullptr;  // 复用470的取消回调语义
        void* context = nullptr;             // 回调上下文，仅操作期间借用
    };
    struct JpegBuffer {
        uint8_t* data = nullptr;
        size_t len = 0;

        /** @brief 释放持有的 JPEG 缓冲并清空指针和长度 */
        void Release();
        bool empty() const {
            return data == nullptr || len == 0;
        }
    };

    /** 探测结果：UVC 地址 + 首个分辨率作 ID。 */
    struct DetectInfo {
        bool ok = false;
        uint8_t dev_addr = 0;
        uint8_t stream_index = 0;
        uint16_t h_res = 0;
        uint16_t v_res = 0;
        size_t format_count = 0;
        char id_str[40]{};
    };

    /** @brief 切换共用 USB 数据路由，true 选择摄像头，false 恢复存储侧 */
    static void SelectUsbMux(bool camera);

    /** @brief 查询是否有正在执行的摄像头会话操作 */
    static bool IsBusy();

    /** @brief 查询摄像头会话是否已启动 */
    static bool IsSessionActive();

    /** @brief 申请一次完整拍照操作，覆盖供电、路由和会话释放 */
    static bool TryAcquireCapture(bool keep_awake = true);

    /** @brief 硬件收尾完成后归还拍照操作 */
    static void ReleaseCapture();

    /** @brief 完整执行探测或采集并归还供电、路由和会话，jpeg 为空时仅探测 */
    static esp_err_t CaptureManaged(JpegBuffer* jpeg, uint32_t timeout_ms, const Request* request = nullptr,
                                    uint32_t enum_wait_ms = 8000, CameraJpegFrameHandler handler = nullptr,
                                    void* context = nullptr);

    /** @brief 持有完整拍照操作后安装Host和UVC，并在指定时间内等待设备连接 */
    static esp_err_t StartSession(uint32_t enum_wait_ms = 5000, const Request* request = nullptr);

    /**
     * @brief 停止视频流并释放设备、Host 与 PHY，恢复数据路由和 USB Serial/JTAG
     */
    static void StopSession();

    /**
     * @brief 探测连接设备及视频格式，尚未启动时先建立会话
     * @note 返回后保留会话，调用方仍须调用 StopSession。
     */
    static esp_err_t Detect(DetectInfo& out, uint32_t timeout_ms = 8000, const Request* request = nullptr);

    /** @brief 阻塞抓取一帧 JPEG，调用前须建立会话并完成摄像头路由切换 */
    static esp_err_t CaptureOne(JpegBuffer& out, uint32_t timeout_ms = 20000, const Request* request = nullptr,
                                bool keep_stream = false);

    /** @brief 将错误码转换为当前语言的操作提示 */
    static const char* ErrMessage(esp_err_t err);
};
