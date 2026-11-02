# uart-uhci

基于 ESP32 UHCI + GDMA 的 UART DMA 接收控制器组件。使用预分配缓冲区池和 GDMA owner 机制实现持续接收，支持按需启动/停止 DMA 以配合低功耗（如释放 PM 锁）。

## 特性

- **UHCI + GDMA**：通过 UHCI 控制器将 UART 与 GDMA 连接，实现 DMA 接收
- **缓冲区池**：预分配多块 DMA 缓冲区，每块填满或 UART 空闲时触发回调
- **Idle EOF**：使用 UHCI 空闲 EOF 模式，在 UART 线空闲时结束当前 DMA 传输
- **Owner 机制**：通过 GDMA 描述符 owner 管理缓冲区归属，用户处理完后调用 `ReturnBuffer` 归还，DMA 自动继续使用
- **溢出恢复**：当所有缓冲区被 CPU 占用时 DMA 暂停并触发 `OverflowCallback`，全部归还后自动清空 UART RX FIFO 并重新挂载缓冲区恢复接收
- **PM 锁**：在 `StartReceive` 与 `Transmit` 期间持有 PM 锁（若启用 `CONFIG_PM_ENABLE`），`StopReceive` 与 `Transmit` 完成后释放，便于配合 light sleep
- **发送**：TX 使用标准 UART FIFO 同步写入（不占用额外 GDMA 通道）

## 依赖

- ESP-IDF >= 5.5.2
- 依赖组件：`esp_pm`、`esp_mm`、`esp_driver_uart`

## 配置结构

```c
// 缓冲区池配置
BufferPoolConfig rx_pool;
rx_pool.buffer_count = 4;   // 至少 2，建议 4 及以上
rx_pool.buffer_size  = 256; // 每块缓冲区字节数

// 总配置
UartUhci::Config config = {};
config.uart_port      = UART_NUM_1;
config.dma_burst_size = 16;  // 或 0 使用默认
config.rx_pool        = rx_pool;
```

## 基本用法

1. **创建并初始化**

```cpp
UartUhci uhci;
UartUhci::Config config = {};
config.uart_port      = UART_NUM_1;
config.dma_burst_size = 16;
config.rx_pool        = { .buffer_count = 4, .buffer_size = 256 };

esp_err_t err = uhci.Init(config);
```

2. **注册回调（必须在 StartReceive 之前）**

```cpp
// 每块缓冲区接收完成时调用（在 ISR 上下文，需尽快返回）
bool on_rx(const UartUhci::RxEventData& data, void* user_data) {
    // 使用 data.buffer->data, data.recv_size 处理数据
    // 处理完后必须归还：
    uhci.ReturnBuffer(data.buffer);
    return false; // 返回 true 表示需要 yield
}
uhci.SetRxCallback(on_rx, nullptr);

// 可选：缓冲区耗尽导致 DMA 暂停时调用（在 ISR 上下文）
bool on_overflow(void* user_data) {
    // 尽快处理并归还缓冲区，或仅记录溢出事件
    return false; // 返回 true 表示需要 yield
}
uhci.SetOverflowCallback(on_overflow, nullptr);
```

3. **启动/停止接收**

```cpp
uhci.StartReceive();  // 开始 DMA 接收，持有 PM 锁
// ...
uhci.StopReceive();   // 停止 DMA，释放 PM 锁
```

4. **发送数据**

```cpp
uint8_t msg[] = "hello";
uhci.Transmit(msg, sizeof(msg) - 1);
```

5. **状态查询**

```cpp
bool running   = uhci.IsReceiving();           // 是否正在接收
bool overflow  = uhci.HasOverflow();           // 是否处于溢出状态（不清除标志）
bool was_over  = uhci.CheckAndClearOverflow(); // 读取并清除溢出标志
```

6. **反初始化**

```cpp
uhci.Deinit();
```

## 注意事项

- **缓冲区数量**：`buffer_count` 至少为 2；过小在数据处理稍慢时容易触发溢出（所有缓冲区被 CPU 占用，DMA 暂停）。
- **回调上下文**：`RxCallback` 与 `OverflowCallback` 都在 ISR 中执行，应避免阻塞和复杂逻辑，处理完后尽快 `ReturnBuffer`。
- **必须归还**：每块通过 `RxCallback` 拿到的缓冲区都必须调用 `ReturnBuffer(buffer)`，否则会导致缓冲区耗尽和 DMA 暂停。
- **溢出恢复**：发生溢出后，当所有缓冲区都被归还时，组件会清空 UART RX FIFO 并重新挂载 DMA，无需额外调用。
- **UART 初始化**：波特率、引脚等需在外部用 `uart_driver_install` / 驱动接口先配置好；本组件只负责 UHCI/GDMA 接收与发送数据到已有 UART。

## 许可证

Apache-2.0
