#pragma once

#include <driver/uart.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include <cstring>
#include <functional>
#include <string>
#include <vector>

/**
 * @brief 简易 UART 单例（BT AT；对齐 397 SimpleUart，适配 IDF 6.1）
 */
class SimpleUart {
public:
    static SimpleUart& getInstance() {
        static SimpleUart instance;
        return instance;
    }

    SimpleUart(const SimpleUart&) = delete;
    SimpleUart& operator=(const SimpleUart&) = delete;

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
        if (uart_param_config(m_uartNum, &uartConfig) != ESP_OK) {
            return false;
        }
        if (uart_set_pin(m_uartNum, m_txPin, m_rxPin, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE) !=
            ESP_OK) {
            return false;
        }

        m_initialized = true;
        xTaskCreate(uartReceiveTask, "uart_rx_task", 4096, this, 10, &m_rxTaskHandle);
        return true;
    }

    void registerCallback(std::function<void(const std::vector<uint8_t>&)> callback) {
        m_callback = callback;
    }

    bool sendData(const uint8_t* data, size_t length) {
        if (!m_initialized) {
            return false;
        }
        return uart_write_bytes(m_uartNum, reinterpret_cast<const char*>(data), length) ==
               static_cast<int>(length);
    }

    bool sendString(const std::string& data) {
        return sendData(reinterpret_cast<const uint8_t*>(data.c_str()), data.length());
    }

    bool sendString(const char* data) {
        if (data == nullptr) {
            return false;
        }
        return sendData(reinterpret_cast<const uint8_t*>(data), strlen(data));
    }

    bool isInitialized() const { return m_initialized; }

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
            if (length > 0 && uart->m_callback) {
                std::vector<uint8_t> data(buffer, buffer + length);
                uart->m_callback(data);
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
    std::function<void(const std::vector<uint8_t>&)> m_callback;
};
