// Copyright 2025 Terrence
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <atomic>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include "driver/gpio.h"
#include "driver/uart.h"
#include "uart_uhci.h"
#include "esp_err.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "iot_eth.h"
#include "iot_eth_netif_glue.h"
#include "iot_eth_types.h"

/**
 * @brief 管理 4G 模组的 UART 以太网通信、AT 命令与网络状态
 * @note MRDY/SRDY 握手与帧格式由本驱动维护，配置中的引脚由板级装配传入。
 */
class UartEthModem {
public:
    enum class WorkingState {
        Idle,           // 双方空闲，DMA 已停止
        PendingActive,  // 等待模组唤醒并拉低 SRDY
        Active,         // 正在通信，DMA 运行中
        PendingIdle,    // 等待模组空闲并拉高 SRDY
    };

    enum class MrdyLevel {
        Low,   // 主机忙碌
        High,  // 主机空闲，允许模组休眠
    };

    enum class EventType : uint8_t {
        None = 0,   // 无事件
        TxRequest,  // 请求发送
        SrdyLow,    // 模组进入忙碌状态
        SrdyHigh,   // 模组进入空闲状态
        RxData,     // 接收到数据
        Stop,       // 请求停止
    };

    struct Event {
        EventType type;                 // 主任务事件类型
        UartUhci::RxBuffer* rx_buffer;  // 仅接收事件携带的 DMA 缓冲
    };

    enum class UartEthModemEvent {
        Connecting,               // 正在搜索或连接网络
        Connected,                // 已取得网络地址
        Disconnected,             // 网络链路已断开
        ErrorNoSim,               // 未检测到 SIM 卡
        ErrorRegistrationDenied,  // 网络注册被拒绝
        ErrorInitFailed,          // 模组初始化失败
    };

    struct CellInfo {
        int stat = 0;     // 网络注册状态
        std::string tac;  // 跟踪区代码
        std::string ci;   // 小区标识
        int act = 0;      // 无线接入技术
    };

    struct Config {
        uart_port_t uart_num = UART_NUM_1;  // UART 控制器
        int baud_rate = 3000000;            // UART 波特率
        gpio_num_t tx_pin = GPIO_NUM_NC;    // 主机发送引脚
        gpio_num_t rx_pin = GPIO_NUM_NC;    // 主机接收引脚
        gpio_num_t mrdy_pin = GPIO_NUM_NC;  // 主机握手引脚
        gpio_num_t srdy_pin = GPIO_NUM_NC;  // 模组握手引脚
        size_t rx_buffer_count = 4;         // DMA 接收缓冲数量
        size_t rx_buffer_size = 1600;       // 单个 DMA 接收缓冲字节数
    };

    // 回调中的 detail 为可选原因说明，可能为空；不得未经检查直接记录上游响应。
    using UartEthModemEventCallback = std::function<void(UartEthModemEvent event, const std::string& detail)>;

    /** @brief 使用板级配置创建模组管理对象，尚不启动通信 */
    explicit UartEthModem(const Config& config);

    /** @brief 停止模组并释放通信资源 */
    ~UartEthModem();
    UartEthModem(const UartEthModem&) = delete;
    UartEthModem& operator=(const UartEthModem&) = delete;

    /**
     * @brief 初始化通信、启动工作任务并注册以太网接口
     * @return 成功返回 ESP_OK，失败返回初始化阶段的错误码
     */
    esp_err_t Start();

    /** @brief 停止工作任务并释放通信资源，成功返回 ESP_OK */
    esp_err_t Stop();

    /**
     * @brief 同步发送 AT 命令，内部串行化并发请求
     * @param cmd 不带末尾回车的 AT 命令
     * @param response 接收命令响应
     * @param timeout_ms 超时时间，单位毫秒
     * @return 成功返回 ESP_OK，超时返回 ESP_ERR_TIMEOUT
     */
    esp_err_t SendAt(const std::string& cmd, std::string& response, uint32_t timeout_ms = 1000);

    /**
     * @brief 同步发送 AT 命令并收集后续通知，直到出现完成标记
     * @param done_marker 标记操作完成的响应子串
     * @note 适用于先返回 OK、再异步上报执行结果的命令。
     */
    esp_err_t SendAtCollectUntil(const std::string& cmd, std::string& response, uint32_t timeout_ms,
                                 const char* done_marker);

    /** @brief 查询网络初始化是否完成 */
    bool IsInitialized() const {
        return initialized_.load();
    }

    /** @brief 设置网络状态回调，接收初始化后的连接变化与错误原因 */
    void SetNetworkEventCallback(UartEthModemEventCallback callback);

    /** @brief 查询模组设备标识 */
    std::string GetImei();

    /** @brief 查询移动用户标识 */
    std::string GetImsi();

    /** @brief 查询 SIM 卡标识 */
    std::string GetIccid();

    /** @brief 返回开机联网已缓存的 ICCID，不发送 AT */
    std::string CachedIccid() const {
        std::lock_guard<std::mutex> lock(iccid_mutex_);
        return iccid_;
    }

    /** @brief 查询运营商名称 */
    std::string GetCarrierName();

    /** @brief 查询模组固件版本 */
    std::string GetModuleRevision();

    /** @brief 查询 CSQ 信号强度，0–31 为有效值，99 表示未知 */
    int GetSignalStrength();

    /** @brief 查询网络注册状态及小区信息 */
    CellInfo GetCellInfo();

    /** @brief 获取网络事件的固定名称，用于诊断 */
    static const char* GetNetworkEventName(UartEthModemEvent event);

private:
    // Frame type
    enum class FrameType : uint8_t { kEthernet = 0, kAtCommand = 1 };

    // Frame header structure (4 bytes, compatible with original protocol)
    // Layout:
    //   Byte 0: payload_length[7:0]
    //   Byte 1: seq_no[7:4], payload_length[11:8]
    //   Byte 2: reserved[7:4], type[3:2], continue[1], flow_control[0]
    //   Byte 3: checksum
    struct FrameHeader {
        uint8_t raw[4];

        uint16_t GetPayloadLength() const {
            return raw[0] | ((raw[1] & 0x0F) << 8);
        }
        void SetPayloadLength(uint16_t len) {
            raw[0] = len & 0xFF;
            raw[1] = (raw[1] & 0xF0) | ((len >> 8) & 0x0F);
        }

        void SetSequence(uint8_t seq) {
            raw[1] = (raw[1] & 0x0F) | ((seq & 0x0F) << 4);
        }

        // Flow control: 0 = XON (permit to send), 1 = XOFF (shall not send)
        void SetFlowControl(bool xoff) {
            raw[2] = (raw[2] & 0xFE) | (xoff ? 1 : 0);
        }

        FrameType GetType() const {
            return static_cast<FrameType>((raw[2] >> 2) & 0x03);
        }
        void SetType(FrameType type) {
            raw[2] = (raw[2] & 0xF3) | ((static_cast<uint8_t>(type) & 0x03) << 2);
        }

        uint8_t GetChecksum() const {
            return raw[3];
        }
        void SetChecksum(uint8_t sum) {
            raw[3] = sum;
        }

        uint8_t CalculateChecksum() const {
            uint32_t sum = raw[0] + raw[1] + raw[2];
            return static_cast<uint8_t>((sum >> 8) ^ sum ^ 0x03);
        }

        bool ValidateChecksum() const {
            return GetChecksum() == CalculateChecksum();
        }
        void UpdateChecksum() {
            SetChecksum(CalculateChecksum());
        }
    } __attribute__((packed));

    static_assert(sizeof(FrameHeader) == 4, "FrameHeader must be 4 bytes");

    // The sender and TX queue each own one reference; timeout must not leave
    // a queue entry pointing at a caller's stack or a deleted semaphore.
    struct TxCompletion {
        std::atomic<unsigned> refs{2};
        SemaphoreHandle_t done = nullptr;
        esp_err_t result = ESP_OK;
    };
    static void ReleaseTxCompletion(TxCompletion* completion);
    static void CompleteTxFrame(TxCompletion* completion, esp_err_t result);

    // TX frame structure (for queue)
    struct TxFrame {
        uint8_t* data;             // Header + payload, allocated with malloc
        size_t length;             // Total length including header
        TxCompletion* completion;  // nullptr for asynchronous Ethernet frames
    };

    // Initialization and cleanup
    esp_err_t InitUart();
    esp_err_t InitGpio();
    esp_err_t InitIotEth();
    void DeinitUart();
    void DeinitGpio();
    void DeinitIotEth();

    // Task implementation (single task with UHCI DMA)
    void MainTaskRun();
    void InitTaskRun();
    void TxTaskRun();  // Dedicated TX task for non-blocking transmit

    // Event handling
    void HandleEvent(const Event& event);
    void HandleSrdyLow();
    void HandleSrdyHigh();
    void HandleRxData(UartUhci::RxBuffer* buffer);
    void HandleIdleTimeout();

    // Working state machine
    void EnterPendingActiveState();  // Master initiates wakeup, wait for slave
    void EnterActiveState();
    void EnterPendingIdleState();
    void EnterIdleState();
    TickType_t CalculateNextTimeout();

    // UHCI DMA callbacks (called from ISR context, must be in IRAM)
    static bool IRAM_ATTR UhciRxCallbackStatic(const UartUhci::RxEventData& data, void* user_data);

    // Frame processing
    esp_err_t SendFrame(const uint8_t* data, size_t length, FrameType type);
    esp_err_t EnqueueTxFrame(const uint8_t* buf, size_t len);
    void ProcessReceivedFrame(uint8_t* data, size_t size);
    void HandleEthFrame(uint8_t* data, size_t length);
    void HandleAtResponse(const char* data, size_t length);

    // AT command helpers
    esp_err_t SendAtWithRetry(const std::string& cmd, std::string& response, uint32_t timeout_ms, int max_retries);
    void ParseAtResponse(const std::string& response);

    // Initialization sequence
    esp_err_t RunNormalModeInitSequence();
    bool CheckSimCard();
    bool WaitForRegistration(uint32_t timeout_ms);
    void QueryModemInfo();
    esp_err_t AtDetect();

    // GPIO control (low level = busy)
    void SetMrdy(MrdyLevel level);
    bool IsSrdyLow();
    void SendAckPulse();
    void ConfigureSrdyInterrupt(bool for_wakeup);
    static void IRAM_ATTR SrdyIsrHandler(void* arg);

    // State management
    // detail is an optional human-readable reason forwarded to the event
    // callback (mainly for error events). Empty for normal state changes.
    void SetNetworkEvent(UartEthModemEvent event, const std::string& detail = "");

    // Resource cleanup
    void CleanupResources(bool cleanup_iot_eth = true);

    // IP event handler
    static void IpEventHandler(void* arg, esp_event_base_t event_base, int32_t event_id, void* event_data);

    // Configuration
    Config config_;
    // ISR 只读此副本（对象在堆/DRAM）；勿在 ISR 里读 config_
    gpio_num_t srdy_pin_isr_ = GPIO_NUM_NC;
    int detect_baud_rate_ = 0;

    // Network initialization flag (atomic for thread safety)
    std::atomic<bool> initialized_{false};

    // Network event (atomic for thread safety)
    std::atomic<UartEthModemEvent> network_event_{UartEthModemEvent::Disconnected};

    // Synchronization primitives
    QueueHandle_t event_queue_ = nullptr;
    std::mutex at_mutex_;
    EventGroupHandle_t event_group_ = nullptr;

    // UHCI DMA
    UartUhci uart_uhci_;

    // Frame reassembly buffer for incomplete frames
    // 用于不完整帧的重组缓冲区
    static constexpr size_t kMaxFrameSize = 1600;
    uint8_t* reassembly_buffer_ = nullptr;
    size_t reassembly_size_ = 0;
    size_t reassembly_expected_ = 0;  // Expected total frame size (header + payload)

    // Task handles
    TaskHandle_t main_task_ = nullptr;
    TaskHandle_t init_task_ = nullptr;
    TaskHandle_t tx_task_ = nullptr;

    // TX queue for non-blocking transmit from LWIP
    QueueHandle_t tx_queue_ = nullptr;
    static constexpr size_t kTxQueueDepth = 32;  // Max pending TX frames

    // State flags (atomic for thread safety)
    std::atomic<bool> stop_flag_{false};
    std::atomic<bool> handshake_done_{false};
    std::atomic<bool> initializing_{false};
    std::atomic<uint8_t> seq_no_{0};

    // Working state machine
    std::atomic<WorkingState> working_state_{WorkingState::Idle};
    int64_t last_activity_time_us_{0};
    static constexpr int64_t kIdleTimeoutMs = 500;
    static constexpr int64_t kAckTimeoutMs = 100;
    static constexpr int64_t kAckPulseUs = 50;

    // Modem information (cached)
    std::string imei_;
    mutable std::mutex iccid_mutex_; // 保护联网任务与界面共享的 ICCID 缓存
    std::string iccid_;
    std::string imsi_;
    std::string carrier_name_;
    std::string module_revision_;
    int signal_strength_ = 99;
    CellInfo cell_info_;
    uint8_t mac_addr_[6] = {0};

    // AT response handling
    std::string at_command_response_;
    bool waiting_for_at_response_ = false;
    bool at_collect_until_done_ = false;
    std::string at_until_marker_;

    // Callback
    UartEthModemEventCallback network_event_callback_;

    // iot_eth related
    iot_eth_driver_t driver_{};
    iot_eth_handle_t eth_handle_ = nullptr;
    iot_eth_mediator_t* mediator_ = nullptr;
    iot_eth_netif_glue_handle_t glue_ = nullptr;
    esp_netif_t* eth_netif_ = nullptr;
    esp_event_handler_instance_t ip_event_handler_instance_ = nullptr;

    // Event bits
    static constexpr uint32_t kEventStart = (1 << 0);
    static constexpr uint32_t kEventHandshakeDone = (1 << 1);
    static constexpr uint32_t kEventStop = (1 << 2);
    static constexpr uint32_t kEventAtResponse = (1 << 4);
    static constexpr uint32_t kEventSrdyHigh = (1 << 7);
    static constexpr uint32_t kEventActiveState = (1 << 11);  // Set when entered active state with DMA ready

    static constexpr uint32_t kEventMainTaskDone = (1 << 8);
    static constexpr uint32_t kEventInitTaskDone = (1 << 10);
    static constexpr uint32_t kEventTxTaskDone = (1 << 12);

    static constexpr uint32_t kEventAllTasksDone = kEventMainTaskDone | kEventInitTaskDone | kEventTxTaskDone;

    // Timing constants
    static constexpr uint32_t kHandshakeTimeoutMs = 5000;
    static constexpr uint32_t kModemSleepTimeoutS = 3;

    // SRDY interrupt configuration constants
    static constexpr bool kSrdyInterruptForWakeup = true;  // Low level trigger for light sleep wakeup
    static constexpr bool kSrdyInterruptForAck = false;    // Edge detection for SRDY high (ACK)

    // Handshake magic bytes
    static constexpr uint8_t kHandshakeRequest[] = {0x53, 0x50, 0x49, 0x43, 0x02, 0x04,
                                                    0x0C, 0x00, 0x00, 0x00, 0x00, 0x00};
    static constexpr uint8_t kHandshakeAck[] = {0x53, 0x50, 0x49, 0x43, 0x01, 0x80};

    static constexpr const char* kTag = "UartEthModem";
};
