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
    gpio_num_t dtr_pin_; // mrdy_pin
    gpio_num_t ri_pin_;  // srdy_pin

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
    Nt26Board(gpio_num_t tx_pin, gpio_num_t rx_pin, gpio_num_t dtr_pin, gpio_num_t ri_pin);
    virtual ~Nt26Board();
    virtual std::string GetBoardType() override;
    virtual void StartNetwork() override;
    virtual bool EnsureNetworkReady(int timeout_ms = 30000) override;
    virtual bool IsNetworkReady() override;
    virtual void SetNetworkEventCallback(NetworkEventCallback callback) override;
    virtual NetworkInterface* GetNetwork() override;
    virtual void SetPowerSaveMode(bool enabled) override;
    virtual const char* GetNetworkStateIcon() override;
    const char* GetNetworkBusyStatusText() override;
    virtual AudioCodec* GetAudioCodec() override { return nullptr; }
    virtual std::string GetDeviceStatusJson() override;

    // AT+CSQ：0–31 有效，99/-1 未知；modem 未就绪时返回 99。
    int GetSignalStrength();

    // 读取开机联网已缓存的 ICCID，不发 AT；未就绪或未缓存时返回空。
    std::string GetIccid();

    /**
     * P14 掉电前调用（供电仍在）：干净 Stop + 释放 modem，避免掉电后
     * 软件仍以为已初始化、或 UART/DMA 任务空转。
     */
    void PreparePowerOff();
    void OnPowerRestored();

    /**
     * P14 上升沿后异步恢复：未初始化则重新走 Start 流程。
     * 单飞：重复上升沿不叠任务；与 EnsureNetworkReady 共用 lifecycle_mu_。
     */
    void RecoverAfterPowerOnAsync();

    // 转发到 UartEthModem::SendAt。线程安全（modem 内部用 mutex 串行化）。
    // 在 modem 还没初始化、或 4G 网络未就绪时返回 ESP_ERR_INVALID_STATE。
    // bypass_init_check=true: 跳过本地的 IsInitialized() 检查，只要 modem
    // 实例存在就直接转发到底层 SendAt（UartEthModem 内部仍然有它自己的
    // handshake / stop_flag 保护）。专门给 SIM 卡切换之类的场景用：外置卡
    // 没插的时候 modem 不会进入 “initialized” 状态，但 AT 通道本身是通的，
    // 拒绝下发会让用户永远卡死在没卡的槽位上。
    esp_err_t SendAtCommand(const std::string& cmd, std::string& response,
                            uint32_t timeout_ms = 5000,
                            bool bypass_init_check = false);

    // 发送 AT 并持续收集 URC，直到出现 done_marker（如 "+ECPING: DONE"）。
    esp_err_t SendAtCommandCollectUntil(const std::string& cmd,
                                        std::string& response,
                                        uint32_t timeout_ms,
                                        const char* done_marker,
                                        bool bypass_init_check = false);
};

#endif // NT26_BOARD_H
