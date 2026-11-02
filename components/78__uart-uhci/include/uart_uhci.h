/*
 * UART UHCI DMA Controller
 * 
 * 自定义 UHCI DMA 控制器，提供启动/停止 DMA 接收的功能，
 * 以支持低功耗模式下释放 PM 锁。
 * 
 * 使用 GDMA owner 机制实现动态缓冲区管理：
 * - 每个缓冲区接收满时触发回调
 * - 用户返还缓冲区后自动加回 DMA 链表
 */

#pragma once

#include <atomic>
#include <cstdint>
#include <cstddef>

#include "esp_err.h"
#include "esp_pm.h"
#include "esp_attr.h"
#include "hal/uart_types.h"
#include "freertos/FreeRTOS.h"

// Forward declarations for ESP-IDF internal types
typedef struct gdma_channel_t *gdma_channel_handle_t;
typedef struct gdma_link_list_t *gdma_link_list_handle_t;
struct uhci_dev_t;

class UartUhci {
public:
    // RX buffer descriptor for buffer pool mode
    // 缓冲区池模式的 RX 缓冲区描述符
    struct RxBuffer {
        uint8_t* data;          // Pointer to buffer data / 缓冲区数据指针
        size_t capacity;        // Buffer capacity / 缓冲区容量
        size_t size;            // Actual received data size / 实际接收数据大小
        uint32_t index;         // Buffer index in pool / 缓冲区在池中的索引
    };

    // RX event data passed to callback
    // 传递给回调的 RX 事件数据
    struct RxEventData {
        RxBuffer* buffer;       // Buffer containing received data / 包含接收数据的缓冲区
        size_t recv_size;       // Number of bytes received / 接收字节数
    };

    // Callback types (return true to yield, called from ISR context)
    // 回调类型（返回 true 表示需要调度，在 ISR 上下文中调用）
    // Note: These are function pointers, not std::function, for ISR safety
    using RxCallback = bool(*)(const RxEventData& data, void* user_data);
    
    // Overflow callback - called when DMA stops due to buffer exhaustion
    // 溢出回调 - 当 DMA 因缓冲区耗尽而停止时调用
    using OverflowCallback = bool(*)(void* user_data);

    // Buffer pool configuration
    // 缓冲区池配置
    struct BufferPoolConfig {
        size_t buffer_count;        // Number of buffers in pool / 池中缓冲区数量
        size_t buffer_size;         // Size of each buffer / 每个缓冲区大小
    };

    // Configuration structure
    // 配置结构
    struct Config {
        uart_port_t uart_port;          // UART port number / UART 端口号
        size_t dma_burst_size;          // DMA burst size (0 to disable, or power of 2) / DMA 突发大小
        BufferPoolConfig rx_pool;       // RX buffer pool config / RX 缓冲区池配置
    };

    UartUhci();
    ~UartUhci();

    // Initialize the UHCI controller
    // 初始化 UHCI 控制器
    esp_err_t Init(const Config& config);

    // Deinitialize and release resources
    // 反初始化并释放资源
    void Deinit();

    // Register callbacks (must be called before StartReceive)
    // 注册回调（必须在 StartReceive 之前调用）
    void SetRxCallback(RxCallback callback, void* user_data);
    
    // Register overflow callback (called when DMA stops due to buffer exhaustion)
    // 注册溢出回调（当 DMA 因缓冲区耗尽而停止时调用）
    void SetOverflowCallback(OverflowCallback callback, void* user_data);

    // Start continuous DMA receive using buffer pool
    // Acquires PM lock to prevent light sleep
    // Buffers are delivered via RxCallback, caller must return them via ReturnBuffer
    // 使用缓冲区池启动持续 DMA 接收
    // 获取 PM 锁以阻止 light sleep
    // 缓冲区通过 RxCallback 传递，调用者必须通过 ReturnBuffer 归还
    esp_err_t StartReceive();

    // Stop DMA receive and release PM lock
    // 停止 DMA 接收并释放 PM 锁
    esp_err_t StopReceive();

    // Return a buffer back to the pool after processing
    // Must be called for each buffer received via RxCallback
    // 处理完成后将缓冲区归还到池中
    // 必须对每个通过 RxCallback 收到的缓冲区调用
    void ReturnBuffer(RxBuffer* buffer);

    // Check if RX is currently running
    // 检查 RX 是否正在运行
    bool IsReceiving() const { return rx_running_.load(); }

    // Check and clear buffer overflow flag (DMA stopped due to buffer exhaustion)
    // 检查并清除缓冲区溢出标志（DMA 因缓冲区耗尽而停止）
    bool CheckAndClearOverflow() {
        return buffer_overflow_.exchange(false);
    }
    
    // Check if overflow occurred (without clearing)
    // 检查是否发生溢出（不清除）
    bool HasOverflow() const { return buffer_overflow_.load(); }

    // Transmit data (blocking until FIFO is written)
    // 通过 FIFO 发送数据（同步阻塞）
    esp_err_t Transmit(const uint8_t* buffer, size_t size);

private:
    // Initialize GDMA channels
    esp_err_t InitGdma(const Config& config);
    void DeinitGdma();

    // Initialize RX buffer pool
    esp_err_t InitRxBufferPool(const BufferPoolConfig& config);
    void DeinitRxBufferPool();
    
    // Re-mount all buffers and restart DMA (used for initial start and overflow recovery)
    // flush_uart_fifo: if true, flush UART RX FIFO before restarting (for overflow recovery)
    void RemountAndRestartDma(bool flush_uart_fifo = false);

public:
    // Handle GDMA RX done callback (called from ISR)
    // desc_addr: completed descriptor address, is_normal_eof: normal EOF flag
    bool IRAM_ATTR HandleGdmaRxDone(uintptr_t desc_addr, bool is_normal_eof);
    
    // Handle GDMA descriptor error callback (called from ISR)
    // Triggered when DMA encounters descriptor error, e.g., owner check failed (buffer exhaustion)
    bool IRAM_ATTR HandleGdmaDescrErr();

private:

    // Member variables
    int uhci_num_{-1};
    uart_port_t uart_port_{UART_NUM_MAX};
    uhci_dev_t* uhci_dev_{nullptr};

    // TX direction (Synchronous FIFO mode)
    // No queues needed as it's now blocking and called from a dedicated task

    // RX direction - uses GDMA owner mechanism for dynamic buffer management
    gdma_channel_handle_t rx_dma_chan_{nullptr};
    gdma_link_list_handle_t rx_dma_link_{nullptr};
    RxBuffer* rx_buffer_pool_{nullptr};     // Array of buffer descriptors
    size_t rx_pool_size_{0};                // Number of buffers in pool
    size_t rx_buffer_size_{0};              // Size of each buffer
    uintptr_t rx_dma_link_head_addr_{0};    // DMA link list head address
    size_t rx_cache_line_{0};
    size_t rx_int_mem_align_{0};
    size_t rx_ext_mem_align_{0};
    std::atomic<bool> rx_running_{false};
    std::atomic<bool> buffer_overflow_{false};  // Set when DMA stops due to buffer exhaustion

    // PM lock
    esp_pm_lock_handle_t pm_lock_{nullptr};

    // Cache line sizes
    size_t int_mem_cache_line_{0};
    size_t ext_mem_cache_line_{0};

    // Callbacks
    RxCallback rx_callback_{nullptr};
    void* rx_callback_user_data_{nullptr};
    OverflowCallback overflow_callback_{nullptr};
    void* overflow_callback_user_data_{nullptr};
    
    // Last processed buffer index for in-order processing
    int last_rx_buf_idx_{-1};

    // Delete copy/move
    UartUhci(const UartUhci&) = delete;
    UartUhci& operator=(const UartUhci&) = delete;
};
