#pragma once

#include <cstddef>
#include <cstdint>

#include <esp_err.h>

// USB UVC 静拍：页面级 Host 会话，Host 仅装一次并常驻复用。
// 调用顺序为：切 MUX -> StartSession -> CaptureOne -> StopSession -> 关闭 OTG。
// 不要与虚拟 U 盘并发；Start/Capture/Detect/Stop 不得在 LVGL 线程调用。
// 关闭 free_all 后也不能再切 MUX 或关电，否则会触发 IDF hub abort。
class UsbUvcStill {
public:
    struct JpegBuffer {
        uint8_t* data = nullptr;
        size_t len = 0;

        void Release();
        bool empty() const { return data == nullptr || len == 0; }
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

    static void SelectUsbMux(bool camera);
    static bool IsBusy();
    static bool IsSessionActive();

    /** 安装 Host+UVC；Host 已装则只恢复 UVC。enum_wait_ms 内等待 DEVICE_CONNECTED。 */
    static esp_err_t StartSession(uint32_t enum_wait_ms = 5000);

    /**
     * 安全停用：关流 + 切回 flash + 等 hub disconnect + 卸 UVC + 等 ALL_FREE。
     * Host/PHY/usb_lib 常驻（避免 uninstall abort）。
     */
    static void StopSession();

    /**
     * 若会话已开：读已连接设备的 format 列表填 out。
     * 若未开：临时 StartSession 探测后保持会话（调用方仍须 StopSession）。
     */
    static esp_err_t Detect(DetectInfo& out, uint32_t timeout_ms = 8000);

    /** 阻塞抓取一帧 JPEG（须已 StartSession；假定 MUX 已切到摄像头）。 */
    static esp_err_t CaptureOne(JpegBuffer& out, uint32_t timeout_ms = 20000);

    static const char* ErrMessage(esp_err_t err);
};
