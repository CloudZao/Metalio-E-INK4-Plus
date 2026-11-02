# uart-uhci

A UART DMA receive controller component for ESP32, built on UHCI + GDMA. It uses a preallocated buffer pool together with the GDMA owner mechanism to provide continuous reception, and supports starting/stopping DMA on demand so the system can release its PM lock and enter low-power modes.

## Features

- **UHCI + GDMA**: bridges UART with GDMA through the UHCI controller for DMA-based reception.
- **Buffer pool**: preallocates multiple DMA buffers; a callback is invoked whenever a buffer is filled or the UART line goes idle.
- **Idle EOF**: uses UHCI's idle EOF mode to terminate the current DMA transfer when the UART line becomes idle.
- **Owner mechanism**: the GDMA descriptor owner field is used to track buffer ownership. After the user processes a buffer, calling `ReturnBuffer` hands it back to the DMA, which keeps consuming the rest of the pool automatically.
- **Overflow recovery**: when every buffer is held by the CPU, the DMA pauses and `OverflowCallback` is fired. Once all buffers have been returned, the component flushes the UART RX FIFO, re-mounts the buffers and resumes reception.
- **PM lock**: a PM lock is held during `StartReceive` and `Transmit` (when `CONFIG_PM_ENABLE` is on) and released by `StopReceive` and at the end of `Transmit`, which plays well with light sleep.
- **Transmit**: TX is done with synchronous UART FIFO writes, so no extra GDMA channel is needed for sending.

## Requirements

- ESP-IDF >= 5.5.2
- Component dependencies: `esp_pm`, `esp_mm`, `esp_driver_uart`

## Configuration

```c
// Buffer pool configuration
BufferPoolConfig rx_pool;
rx_pool.buffer_count = 4;   // At least 2; 4 or more is recommended
rx_pool.buffer_size  = 256; // Bytes per buffer

// Top-level configuration
UartUhci::Config config = {};
config.uart_port      = UART_NUM_1;
config.dma_burst_size = 16;  // Or 0 to use the default
config.rx_pool        = rx_pool;
```

## Basic usage

1. **Create and initialize**

```cpp
UartUhci uhci;
UartUhci::Config config = {};
config.uart_port      = UART_NUM_1;
config.dma_burst_size = 16;
config.rx_pool        = { .buffer_count = 4, .buffer_size = 256 };

esp_err_t err = uhci.Init(config);
```

2. **Register callbacks (must happen before `StartReceive`)**

```cpp
// Invoked whenever a buffer is completed (ISR context, return quickly)
bool on_rx(const UartUhci::RxEventData& data, void* user_data) {
    // Process the data via data.buffer->data and data.recv_size.
    // The buffer MUST be returned to the pool when you are done with it:
    uhci.ReturnBuffer(data.buffer);
    return false; // Return true to request a yield
}
uhci.SetRxCallback(on_rx, nullptr);

// Optional: invoked when DMA pauses because the buffer pool is exhausted (ISR context)
bool on_overflow(void* user_data) {
    // Return outstanding buffers as quickly as possible, or simply record the event
    return false; // Return true to request a yield
}
uhci.SetOverflowCallback(on_overflow, nullptr);
```

3. **Start / stop reception**

```cpp
uhci.StartReceive();  // Starts DMA reception and acquires the PM lock
// ...
uhci.StopReceive();   // Stops DMA and releases the PM lock
```

4. **Transmit data**

```cpp
uint8_t msg[] = "hello";
uhci.Transmit(msg, sizeof(msg) - 1);
```

5. **Status queries**

```cpp
bool running   = uhci.IsReceiving();           // Whether reception is active
bool overflow  = uhci.HasOverflow();           // Whether an overflow is currently latched (does not clear)
bool was_over  = uhci.CheckAndClearOverflow(); // Reads and clears the overflow flag
```

6. **Deinitialize**

```cpp
uhci.Deinit();
```

## Notes

- **Pool size**: `buffer_count` must be at least 2. A small pool makes overflow easy to hit when the consumer is slightly slow (all buffers are held by the CPU and the DMA pauses).
- **Callback context**: both `RxCallback` and `OverflowCallback` run inside an ISR, so avoid blocking or heavy logic and return the buffer via `ReturnBuffer` as soon as possible.
- **Always return buffers**: every buffer delivered via `RxCallback` must be released with `ReturnBuffer(buffer)`, otherwise the pool will eventually be exhausted and the DMA will stop.
- **Overflow recovery**: after an overflow, once every buffer has been returned, the component flushes the UART RX FIFO and re-mounts the DMA buffers automatically; no extra call is required.
- **UART setup**: the baud rate, pin assignment, and other UART parameters must be configured beforehand through `uart_driver_install` or the equivalent driver API. This component only handles UHCI/GDMA reception and writes TX data into an already configured UART.

## License

Apache-2.0
