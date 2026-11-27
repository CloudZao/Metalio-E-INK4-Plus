#ifndef NT26_BOARD_H
#define NT26_BOARD_H

#include <atomic>
#include <memory>
#include <mutex>
#include <uart_eth_modem.h>
#include "freertos/event_groups.h"
#include "board.h"

// P14 supplies the modem. Stop before power-off and rebuild after waking.
class Nt26Board : public Board {
protected:
    // shared_ptr：SendAt 与 TearDown 并发时延长 lifetime，避免 Stop 后 UAF
    std::shared_ptr<UartEthModem> modem_;
    gpio_num_t tx_pin_;
    gpio_num_t rx_pin_;
    gpio_num_t dtr_pin_;  // mrdy_pin
    gpio_num_t ri_pin_;   // srdy_pin

    NetworkEventCallback network_event_callback_;
    EventGroupHandle_t network_wait_event_ = nullptr;

    // 生命周期：PreparePowerOff / StartOrRestart / Ensure 互斥
    mutable std::mutex lifecycle_mu_;
    /** 串行化 StartOrRestart 整段（含 wait），避免 Ensure 与上升沿恢复互相 TearDown */
    std::timed_mutex start_mu_;
    std::atomic<bool> retry_needed_{false};
    std::atomic<int64_t> last_start_us_{0};
    std::mutex modem_operation_mu_;
    std::atomic<bool> powered_{true};
    std::atomic<int64_t> power_restored_us_{0};
    std::atomic<int> cached_csq_{99};
    std::atomic<bool> signal_busy_{false};
    std::atomic<int64_t> signal_sample_us_{-5000000};
    static void SignalSampleTask(void* arg);
    std::atomic<uint32_t> lifecycle_epoch_{0};
    std::atomic<bool> network_ready_{false};
    std::atomic<bool> network_busy_{false};
    std::atomic<bool> recover_busy_{false};

    virtual std::string GetBoardJson() override;

    void OnNetworkEvent(NetworkEvent event, const std::string& data = "", uint32_t expected_epoch = 0);

    /** 停模组、唤醒等待者、清 ready；递增 epoch。Stop 在锁外，避免与回调死锁 */
    void TearDownModem();
    /** 安全唤醒 StartOrRestart 的 WaitBits（短持锁取 handle） */
    void SignalWaitBits(uint32_t bits);
    /** 启动（或重建）模组并等待 Connected；可与 Ensure / 上升沿恢复共用 */
    bool StartOrRestartModem(int timeout_ms);
    /** P14 上升沿恢复任务入口（需访问 recover_busy_） */
    static void CellularRecoverTask(void* arg);

public:
    /** @brief 绑定蜂窝握手引脚并准备网络基础设施，尚不启动模组连接 */
    Nt26Board(gpio_num_t tx_pin, gpio_num_t rx_pin, gpio_num_t dtr_pin, gpio_num_t ri_pin);
    virtual ~Nt26Board();
    virtual std::string GetBoardType() override;
    virtual void StartNetwork() override;
    virtual bool EnsureNetworkReady(int timeout_ms = 30000) override;
    virtual bool IsNetworkReady() override;
    virtual void SetNetworkEventCallback(NetworkEventCallback callback) override;
    virtual NetworkInterface* GetNetwork() override;
    virtual void SetPowerSaveMode(bool enabled) override;

    /** @brief 返回当前联网状态与原始信号强度。 */
    NetworkStatus GetNetworkStatus() override;
    virtual AudioCodec* GetAudioCodec() override {
        return nullptr;
    }
    virtual std::string GetDeviceStatusJson() override;

    /** @brief 查询 CSQ 信号强度，0–31 为有效值，未就绪时返回 99 */
    int GetSignalStrength();

    /**
     * @brief 读取开机联网已缓存的 ICCID，不发送 AT
     * @return 未就绪或未缓存时返回空字符串
     */
    std::string GetIccid();

    /**
     * @brief 在板级蜂窝供电关闭前停止模组、唤醒等待方并释放通信资源
     */
    void PreparePowerOff();

    /** @brief 记录蜂窝供电恢复时间并允许后续重建模组 */
    void OnPowerRestored();

    /**
     * @brief 蜂窝供电恢复后异步重建网络，已有恢复任务时不重复创建
     */
    void RecoverAfterPowerOnAsync();

    /**
     * @brief 通过模组串行发送 AT 命令并收集响应
     * @param bypass_init_check 是否跳过本层初始化检查，仍保留模组底层握手和停止保护
     * @return 模组不存在或未通过初始化检查时返回 ESP_ERR_INVALID_STATE，否则返回底层结果
     */
    esp_err_t SendAtCommand(const std::string& cmd, std::string& response, uint32_t timeout_ms = 5000,
                            bool bypass_init_check = false);

    /** @brief 发送 AT 命令并收集后续通知，直到完成标记出现、超时或底层失败 */
    esp_err_t SendAtCommandCollectUntil(const std::string& cmd, std::string& response, uint32_t timeout_ms,
                                        const char* done_marker, bool bypass_init_check = false);
};

#endif  // NT26_BOARD_H
