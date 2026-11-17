#pragma once

#include <driver/uart.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include <cstring>
#include <functional>
#include <string>
#include <vector>
#include <mutex>

/**
 * @brief 简易 UART 单例（BT AT；对齐 397 SimpleUart，适配 IDF 6.1）
 */
class SimpleUart {
public:
    /** @brief 获取蓝牙串口实例 */
    static SimpleUart& getInstance() {
        static SimpleUart instance;
        return instance;
    }

    SimpleUart(const SimpleUart&) = delete;
    SimpleUart& operator=(const SimpleUart&) = delete;

    /** @brief 初始化串口驱动和接收任务，失败时释放已申请资源 */
    bool begin(int txPin, int rxPin, int baudRate, uart_port_t uartNum = UART_NUM_1) {
        if (m_initialized) {
            return false;
        }

        m_uartNum = uartNum;
        m_txPin = txPin;
        m_rxPin = rxPin;
        m_baudRate = baudRate;

        uart_config_t uartConfig = {};
        uartConfig.baud_rate = m_baudRate;
        uartConfig.data_bits = UART_DATA_8_BITS;
        uartConfig.parity = UART_PARITY_DISABLE;
        uartConfig.stop_bits = UART_STOP_BITS_1;
        uartConfig.flow_ctrl = UART_HW_FLOWCTRL_DISABLE;
        uartConfig.rx_flow_ctrl_thresh = 0;
        uartConfig.rx_glitch_filt_thresh = 0;
        uartConfig.source_clk = UART_SCLK_DEFAULT;
        uartConfig.flags.allow_pd = 0;

        if (uart_driver_install(m_uartNum, RX_BUFFER_SIZE, TX_BUFFER_SIZE, 10, &m_uartQueue, 0) !=
            ESP_OK) {
            return false;
        }
        if (uart_param_config(m_uartNum, &uartConfig) != ESP_OK ||
            uart_set_pin(m_uartNum, m_txPin, m_rxPin, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE) != ESP_OK) {
            uart_driver_delete(m_uartNum);
            m_uartQueue = nullptr;
            return false;
        }

        if (xTaskCreate(uartReceiveTask, "uart_rx_task", 4096, this, 10, &m_rxTaskHandle) != pdPASS) {
            uart_driver_delete(m_uartNum);
            m_uartQueue = nullptr;
            return false;
        }
        m_initialized = true;
        return true;
    }

    /** @brief 设置可替换的页面数据回调，传入空回调可取消 */
    void registerCallback(std::function<void(const std::vector<uint8_t>&)> callback) {
        std::lock_guard<std::mutex> lock(m_callbackMutex);
        m_callback = callback;
    }

    /** @brief 设置独立于页面回调的协议应答观察者，由串口接收任务调用 */
    void registerObserver(std::function<void(const std::vector<uint8_t>&)> observer) {
        std::lock_guard<std::mutex> lock(m_callbackMutex);
        m_observer = observer;
    }

    /** @brief 发送指定字节数的数据，返回是否完整写入串口 */
    bool sendData(const uint8_t* data, size_t length) {
        std::lock_guard<std::recursive_timed_mutex> lock(m_commandMutex);
        if (!m_initialized) {
            return false;
        }
        return uart_write_bytes(m_uartNum, reinterpret_cast<const char*>(data), length) ==
               static_cast<int>(length);
    }

    /** @brief 发送字符串内容 */
    bool sendString(const std::string& data) {
        return sendData(reinterpret_cast<const uint8_t*>(data.c_str()), data.length());
    }

    /** @brief 发送以空字符结尾的字符串 */
    bool sendString(const char* data) {
        if (data == nullptr) {
            return false;
        }
        return sendData(reinterpret_cast<const uint8_t*>(data), strlen(data));
    }

    /** @brief 查询串口驱动和接收任务是否已初始化 */
    bool isInitialized() const { return m_initialized; }

    /** @brief 获取多指令事务与共享电源复位使用的串口互斥锁 */
    std::recursive_timed_mutex& commandMutex() { return m_commandMutex; }

private:
    SimpleUart() = default;

    ~SimpleUart() {
        if (m_rxTaskHandle != nullptr) {
            vTaskDelete(m_rxTaskHandle);
            m_rxTaskHandle = nullptr;
        }
        if (m_initialized) {
            uart_driver_delete(m_uartNum);
            m_initialized = false;
        }
    }

    static void uartReceiveTask(void* arg) {
        auto* uart = static_cast<SimpleUart*>(arg);
        uint8_t buffer[RX_BUFFER_SIZE];
        while (true) {
            int length =
                uart_read_bytes(uart->m_uartNum, buffer, RX_BUFFER_SIZE - 1, pdMS_TO_TICKS(100));
            if (length > 0) {
                std::vector<uint8_t> data(buffer, buffer + length);
                std::function<void(const std::vector<uint8_t>&)> callback, observer;
                {
                    std::lock_guard<std::mutex> lock(uart->m_callbackMutex);
                    callback = uart->m_callback;
                    observer = uart->m_observer;
                }
                if (observer) observer(data);
                if (callback) callback(data);
            }
        }
    }

    static constexpr size_t RX_BUFFER_SIZE = 1024;
    static constexpr size_t TX_BUFFER_SIZE = 1024;

    uart_port_t m_uartNum = UART_NUM_1;
    int m_txPin = 0;
    int m_rxPin = 0;
    int m_baudRate = 115200;
    bool m_initialized = false;
    TaskHandle_t m_rxTaskHandle = nullptr;
    QueueHandle_t m_uartQueue = nullptr;
    std::function<void(const std::vector<uint8_t>&)> m_callback; // 页面可替换的数据回调
    std::function<void(const std::vector<uint8_t>&)> m_observer; // 持续处理协议应答的观察者
    std::mutex m_callbackMutex; // 保护回调注册与接收任务快照
    std::recursive_timed_mutex m_commandMutex; // 保证存储事务期间不插入其他命令或复位
};
