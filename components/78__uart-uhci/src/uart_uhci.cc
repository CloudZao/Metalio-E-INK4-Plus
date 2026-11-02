/*
 * UART UHCI DMA Controller Implementation
 * 
 * 使用预分配缓冲区池的持续接收模式
 */

#include "uart_uhci.h"

#include <cstring>
#include <vector>
#include "esp_log.h"
#include "esp_idf_version.h"
#include "esp_check.h"
#include "esp_attr.h"
#include "esp_cache.h"
#include "esp_rom_sys.h"
#include "esp_heap_caps.h"
#include "esp_private/gdma.h"
#include "esp_private/gdma_link.h"
#include "esp_private/periph_ctrl.h"
#include "esp_private/esp_cache_private.h"
#include "hal/uhci_ll.h"
#include "hal/uart_ll.h"
#include "soc/soc_caps.h"

static const char* kTag = "UartUhci";

// Alignment helper macros
#define ALIGN_UP(num, align) (((num) + ((align) - 1)) & ~((align) - 1))
#define MAX_OF(a, b) (((a) > (b)) ? (a) : (b))

// C-style GDMA callback wrappers (must match gdma_event_callback_t signature)
static bool IRAM_ATTR gdma_rx_callback_wrapper(gdma_channel_handle_t dma_chan, gdma_event_data_t* event_data, void* user_data) {
    (void)dma_chan;
    auto* self = static_cast<UartUhci*>(user_data);
    return self->HandleGdmaRxDone(event_data->rx_eof_desc_addr, event_data->flags.normal_eof);
}

static bool IRAM_ATTR gdma_rx_descr_err_wrapper(gdma_channel_handle_t dma_chan, gdma_event_data_t* event_data, void* user_data) {
    (void)dma_chan;
    (void)event_data;
    auto* self = static_cast<UartUhci*>(user_data);
    return self->HandleGdmaDescrErr();
}

// Platform singleton for UHCI controller management
static struct {
    _lock_t mutex;
    void* controllers[UHCI_LL_NUM];
} s_platform = {};

UartUhci::UartUhci() = default;

UartUhci::~UartUhci() {
    Deinit();
}

esp_err_t UartUhci::Init(const Config& config) {
    esp_err_t ret = ESP_OK;

    // Validate buffer pool config
    ESP_RETURN_ON_FALSE(config.rx_pool.buffer_count >= 2, ESP_ERR_INVALID_ARG, kTag, 
                        "buffer pool needs at least 2 buffers");
    ESP_RETURN_ON_FALSE(config.rx_pool.buffer_size > 0, ESP_ERR_INVALID_ARG, kTag, 
                        "buffer size must be > 0");

    uart_port_ = config.uart_port;

    // Find a free UHCI controller
    bool found = false;
    _lock_acquire(&s_platform.mutex);
    for (int i = 0; i < UHCI_LL_NUM; i++) {
        if (s_platform.controllers[i] == nullptr) {
            s_platform.controllers[i] = this;
            uhci_num_ = i;
            found = true;
            break;
        }
    }
    _lock_release(&s_platform.mutex);
    ESP_RETURN_ON_FALSE(found, ESP_ERR_NOT_FOUND, kTag, "no free UHCI controller");

    // Enable UHCI bus clock
    PERIPH_RCC_ATOMIC() {
        uhci_ll_enable_bus_clock(uhci_num_, true);
        uhci_ll_reset_register(uhci_num_);
    }

    // Get UHCI hardware device
    uhci_dev_ = UHCI_LL_GET_HW(uhci_num_);
    ESP_GOTO_ON_FALSE(uhci_dev_, ESP_ERR_INVALID_STATE, err, kTag, "failed to get UHCI device");

    // Initialize UHCI hardware
    uhci_ll_init(uhci_dev_);
    uhci_ll_attach_uart_port(uhci_dev_, config.uart_port);

    // Disable separator character (otherwise UHCI may lose data)
    {
        uhci_seper_chr_t seper_chr = {};
        seper_chr.sub_chr_en = 0;
        uhci_ll_set_seper_chr(uhci_dev_, &seper_chr);
    }

    // Enable idle EOF mode - triggers callback when UART line becomes idle
    uhci_ll_rx_set_eof_mode(uhci_dev_, UHCI_RX_IDLE_EOF);

    // Create PM lock
#if CONFIG_PM_ENABLE
    char pm_lock_name[16];
    snprintf(pm_lock_name, sizeof(pm_lock_name), "uhci_%d", uhci_num_);
    ESP_GOTO_ON_ERROR(esp_pm_lock_create(ESP_PM_CPU_FREQ_MAX, 0, pm_lock_name, &pm_lock_),
                      err, kTag, "failed to create PM lock");
#endif

    // Get cache line sizes
    esp_cache_get_alignment(MALLOC_CAP_SPIRAM, &ext_mem_cache_line_);
    esp_cache_get_alignment(MALLOC_CAP_INTERNAL, &int_mem_cache_line_);

    // Initialize GDMA
    ESP_GOTO_ON_ERROR(InitGdma(config), err, kTag, "failed to initialize GDMA");

    // Initialize RX buffer pool
    ESP_GOTO_ON_ERROR(InitRxBufferPool(config.rx_pool), err, kTag, "failed to initialize RX buffer pool");

    ESP_LOGI(kTag, "UHCI %d initialized (UART %d), RX pool: %d x %d bytes", 
             uhci_num_, config.uart_port, config.rx_pool.buffer_count, config.rx_pool.buffer_size);
    return ESP_OK;

err:
    Deinit();
    return ret;
}

void UartUhci::Deinit() {
    // Stop any ongoing RX
    if (rx_running_.load()) {
        StopReceive();
    }

    // Deinitialize GDMA
    DeinitGdma();

    // Deinitialize RX buffer pool
    DeinitRxBufferPool();

    // Delete PM lock
    if (pm_lock_) {
        esp_pm_lock_delete(pm_lock_);
        pm_lock_ = nullptr;
    }

    // Disable UHCI clock
    if (uhci_num_ >= 0) {
        PERIPH_RCC_ATOMIC() {
            uhci_ll_enable_bus_clock(uhci_num_, false);
        }

        // Release controller slot
        _lock_acquire(&s_platform.mutex);
        s_platform.controllers[uhci_num_] = nullptr;
        _lock_release(&s_platform.mutex);
        uhci_num_ = -1;
    }

    uhci_dev_ = nullptr;
}

esp_err_t UartUhci::InitGdma(const Config& config) {
    // TX DMA is disabled to save GDMA channels on resources-constrained chips like ESP32-C5.
    // Standard UART FIFO writing will be used in Transmit().

    // Allocate RX DMA channel
    gdma_channel_alloc_config_t rx_alloc = {};
#if ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(6, 0, 0)
    ESP_RETURN_ON_ERROR(gdma_new_ahb_channel(&rx_alloc, nullptr, &rx_dma_chan_), kTag, "RX DMA alloc failed");
#else
    rx_alloc.direction = GDMA_CHANNEL_DIRECTION_RX;
    ESP_RETURN_ON_ERROR(gdma_new_ahb_channel(&rx_alloc, &rx_dma_chan_), kTag, "RX DMA alloc failed");
#endif
    gdma_connect(rx_dma_chan_, GDMA_MAKE_TRIGGER(GDMA_TRIG_PERIPH_UHCI, 0));

    gdma_transfer_config_t transfer_cfg = {};
    transfer_cfg.max_data_burst_size = config.dma_burst_size;
    transfer_cfg.access_ext_mem = true;
    ESP_RETURN_ON_ERROR(gdma_config_transfer(rx_dma_chan_, &transfer_cfg), kTag, "RX DMA config failed");

    // Get RX alignment constraints
    gdma_get_alignment_constraints(rx_dma_chan_, &rx_int_mem_align_, &rx_ext_mem_align_);

    // Create RX DMA link list with buffer pool size and owner checking enabled
    // Each buffer gets one DMA node, owner mechanism manages buffer availability
    gdma_link_list_config_t rx_link_cfg = {};
    rx_link_cfg.item_alignment = 4;
    rx_link_cfg.num_items = config.rx_pool.buffer_count;
    ESP_RETURN_ON_ERROR(gdma_new_link_list(&rx_link_cfg, &rx_dma_link_), kTag, "RX link list failed");

    // Save link list head address
    rx_dma_link_head_addr_ = gdma_link_get_head_addr(rx_dma_link_);

    // Enable owner check on DMA channel
    gdma_strategy_config_t strategy = {};
    strategy.owner_check = true;
    ESP_RETURN_ON_ERROR(gdma_apply_strategy(rx_dma_chan_, &strategy), kTag, "DMA strategy failed");

    // Register RX callbacks
    // - on_recv_done: triggers for each completed descriptor with EOF
    // - on_descr_err: triggers when DMA encounters descriptor error (e.g., owner check failed)
    gdma_rx_event_callbacks_t rx_cbs = {};
    rx_cbs.on_recv_done = gdma_rx_callback_wrapper;
    rx_cbs.on_descr_err = gdma_rx_descr_err_wrapper;
    ESP_RETURN_ON_ERROR(gdma_register_rx_event_callbacks(rx_dma_chan_, &rx_cbs, this), kTag, "RX callback failed");

    return ESP_OK;
}

void UartUhci::DeinitGdma() {
    if (rx_dma_chan_) {
        gdma_disconnect(rx_dma_chan_);
        gdma_del_channel(rx_dma_chan_);
        rx_dma_chan_ = nullptr;
    }
    if (rx_dma_link_) {
        gdma_del_link_list(rx_dma_link_);
        rx_dma_link_ = nullptr;
    }
}

esp_err_t UartUhci::InitRxBufferPool(const BufferPoolConfig& config) {
    rx_pool_size_ = config.buffer_count;
    rx_buffer_size_ = config.buffer_size;

    // Calculate alignment requirements
    size_t max_align = MAX_OF(MAX_OF(rx_int_mem_align_, rx_ext_mem_align_), int_mem_cache_line_);
    if (max_align == 0) max_align = 4;

    // Align buffer size
    size_t aligned_size = ALIGN_UP(config.buffer_size, max_align);
    rx_buffer_size_ = aligned_size;
    rx_cache_line_ = int_mem_cache_line_;

    // Allocate buffer descriptor array
    rx_buffer_pool_ = static_cast<RxBuffer*>(heap_caps_calloc(rx_pool_size_, sizeof(RxBuffer), MALLOC_CAP_INTERNAL));
    ESP_RETURN_ON_FALSE(rx_buffer_pool_, ESP_ERR_NO_MEM, kTag, "failed to allocate buffer pool descriptors");

    // Allocate individual buffers (buffer[i] will be mounted to DMA node[i])
    for (size_t i = 0; i < rx_pool_size_; i++) {
        rx_buffer_pool_[i].data = static_cast<uint8_t*>(
            heap_caps_aligned_alloc(max_align, aligned_size, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL));
        ESP_RETURN_ON_FALSE(rx_buffer_pool_[i].data, ESP_ERR_NO_MEM, kTag, 
                            "failed to allocate buffer %d", i);
        
        rx_buffer_pool_[i].capacity = aligned_size;
        rx_buffer_pool_[i].size = 0;
        rx_buffer_pool_[i].index = i;
    }

    ESP_LOGD(kTag, "RX buffer pool: %d buffers x %d bytes (aligned to %d)", 
             rx_pool_size_, aligned_size, max_align);

    return ESP_OK;
}

void UartUhci::DeinitRxBufferPool() {
    if (rx_buffer_pool_) {
        for (size_t i = 0; i < rx_pool_size_; i++) {
            if (rx_buffer_pool_[i].data) {
                heap_caps_free(rx_buffer_pool_[i].data);
            }
        }
        heap_caps_free(rx_buffer_pool_);
        rx_buffer_pool_ = nullptr;
    }

    rx_pool_size_ = 0;
    rx_buffer_size_ = 0;
}

void UartUhci::SetRxCallback(RxCallback callback, void* user_data) {
    rx_callback_ = callback;
    rx_callback_user_data_ = user_data;
}

void UartUhci::SetOverflowCallback(OverflowCallback callback, void* user_data) {
    overflow_callback_ = callback;
    overflow_callback_user_data_ = user_data;
}

void UartUhci::RemountAndRestartDma(bool flush_uart_fifo) {
    // Re-mount all buffers to DMA link list and restart
    // This is used both for initial start and recovery from overflow
    
    // Optionally flush UART RX FIFO to discard stale/incomplete data
    // This is important after overflow recovery to avoid processing corrupted data
    if (flush_uart_fifo) {
        uart_dev_t *hw = UART_LL_GET_HW(uart_port_);
        uart_ll_rxfifo_rst(hw);
    }
    
    std::vector<gdma_buffer_mount_config_t> mount_configs(rx_pool_size_);
    for (size_t i = 0; i < rx_pool_size_; i++) {
        RxBuffer* buf = &rx_buffer_pool_[i];
        buf->size = 0;

        mount_configs[i].buffer = buf->data;
        mount_configs[i].length = buf->capacity;
        mount_configs[i].flags.mark_eof = 1;    // Trigger callback when this buffer is filled
    }

    last_rx_buf_idx_ = -1;  // Reset buffer sequence tracking

    // Mount all buffers and start DMA
    // Owner is set to DMA for all nodes by gdma_link_mount_buffers
    gdma_link_mount_buffers(rx_dma_link_, 0, mount_configs.data(), rx_pool_size_, nullptr);
    
    gdma_reset(rx_dma_chan_);
    gdma_start(rx_dma_chan_, rx_dma_link_head_addr_);
}

esp_err_t UartUhci::StartReceive() {
    ESP_RETURN_ON_FALSE(!rx_running_.load(), ESP_ERR_INVALID_STATE, kTag, "RX already running");
    ESP_RETURN_ON_FALSE(rx_buffer_pool_, ESP_ERR_INVALID_STATE, kTag, "buffer pool not initialized");
    ESP_RETURN_ON_FALSE(rx_pool_size_ >= 2, ESP_ERR_INVALID_STATE, kTag, "need at least 2 buffers");

    // Acquire PM lock
    if (pm_lock_) {
        esp_pm_lock_acquire(pm_lock_);
    }

    rx_running_.store(true);
    buffer_overflow_.store(false);  // Clear overflow flag

    // Mount all buffers and start DMA
    RemountAndRestartDma();

    ESP_LOGD(kTag, "RX started with %d buffers", rx_pool_size_);
    return ESP_OK;
}

esp_err_t UartUhci::StopReceive() {
    if (!rx_running_.load()) {
        return ESP_OK;  // Already stopped
    }

    // Stop and reset DMA
    gdma_stop(rx_dma_chan_);
    gdma_reset(rx_dma_chan_);

    rx_running_.store(false);

    // Release PM lock
    if (pm_lock_) {
        esp_pm_lock_release(pm_lock_);
    }

    // All buffers are now available (no queue operations needed)
    ESP_LOGD(kTag, "RX stopped");
    return ESP_OK;
}

void UartUhci::ReturnBuffer(RxBuffer* buffer) {
    if (!buffer || buffer->index >= rx_pool_size_) {
        return;
    }

    buffer->size = 0;
    
    // Set the corresponding DMA node owner back to DMA
    gdma_link_set_owner(rx_dma_link_, buffer->index, GDMA_LLI_OWNER_DMA);
    
    // Resume DMA if it was paused waiting for available buffers
    if (rx_running_.load()) {
        if (buffer_overflow_.load()) {
            // In overflow state - check if ALL buffers are now returned before resuming
            bool all_returned = true;
            for (size_t i = 0; i < rx_pool_size_; i++) {
                gdma_lli_owner_t owner;
                if (gdma_link_get_owner(rx_dma_link_, i, &owner) == ESP_OK && owner == GDMA_LLI_OWNER_CPU) {
                    all_returned = false;
                    break;
                }
            }
            
            if (all_returned) {
                // All buffers returned, safe to resume DMA
                buffer_overflow_.store(false);
                
                // Re-mount all buffers and restart DMA
                // Flush UART FIFO to discard stale/incomplete data from overflow period
                RemountAndRestartDma(true);
            }
            // If not all returned, don't call gdma_append - wait for more buffers
        } else {
            // Normal operation - just append to resume DMA
            gdma_append(rx_dma_chan_);
        }
    }
}

esp_err_t UartUhci::Transmit(const uint8_t* buffer, size_t size) {
    ESP_RETURN_ON_FALSE(buffer && size > 0, ESP_ERR_INVALID_ARG, kTag, "invalid arguments");

    // Acquire PM lock for TX
    if (pm_lock_) {
        esp_pm_lock_acquire(pm_lock_);
    }

    // Standard UART FIFO writing (Synchronous)
    uart_dev_t *hw = UART_LL_GET_HW(uart_port_);
    const uint8_t* data = buffer;
    size_t remaining = size;

    while (remaining > 0) {
        uint32_t can_write = uart_ll_get_txfifo_len(hw);
        uint32_t to_write = (remaining < can_write) ? (uint32_t)remaining : can_write;
        if (to_write > 0) {
            uart_ll_write_txfifo(hw, data, to_write);
            data += to_write;
            remaining -= to_write;
        } else {
            esp_rom_delay_us(10); 
        }
    }

    // Wait for the last bytes to actually be sent out
    while (!uart_ll_is_tx_idle(hw)) {
        esp_rom_delay_us(10);
    }

    // Release PM lock
    if (pm_lock_) {
        esp_pm_lock_release(pm_lock_);
    }

    return ESP_OK;
}

bool UartUhci::HandleGdmaRxDone(uintptr_t desc_addr, bool is_normal_eof) {
    if (!rx_running_.load()) {
        return false;
    }

    // In UHCI idle EOF mode, multiple buffers may complete nearly simultaneously
    // but only one callback may be triggered. We need to check ALL buffers
    // and process any that have owner=CPU (completed by DMA).
    
    // Track maximum CPU-owned buffers for debugging
    static int max_cpu_owned = 0;
    int cpu_owned = 0;
    for (size_t i = 0; i < rx_pool_size_; i++) {
        gdma_lli_owner_t owner;
        if (gdma_link_get_owner(rx_dma_link_, i, &owner) == ESP_OK && owner == GDMA_LLI_OWNER_CPU) {
            cpu_owned++;
        }
    }
    if (cpu_owned > max_cpu_owned) {
        max_cpu_owned = cpu_owned;
        ESP_DRAM_LOGI(kTag, "New max CPU-owned buffers: %d/%d", max_cpu_owned, rx_pool_size_);
    }
    
    bool need_yield = false;
    
    // Process all completed buffers in order, starting from expected next buffer
    int start_idx = (last_rx_buf_idx_ + 1) % rx_pool_size_;
    
    for (size_t count = 0; count < rx_pool_size_; count++) {
        int buf_idx = (start_idx + count) % rx_pool_size_;
        
        // Check if this buffer has been completed by DMA (owner = CPU)
        gdma_lli_owner_t owner;
        if (gdma_link_get_owner(rx_dma_link_, buf_idx, &owner) != ESP_OK) {
            continue;
        }
        
        if (owner != GDMA_LLI_OWNER_CPU) {
            // Buffer not completed yet, stop scanning
            // (DMA processes buffers in order, so if this one isn't done, 
            // later ones won't be either)
            break;
        }
        
        RxBuffer* buf = &rx_buffer_pool_[buf_idx];
        
        // Get received size from the DMA descriptor
        size_t rx_size = gdma_link_get_length(rx_dma_link_, buf_idx);
        
        // Sanity check
        if (rx_size == 0 || rx_size > buf->capacity) {
            // Invalid size, set owner back to DMA and continue
            ESP_DRAM_LOGW(kTag, "buf[%d] invalid rx_size=%u (last=%d, start=%d)", 
                          buf_idx, rx_size, last_rx_buf_idx_, start_idx);
            gdma_link_set_owner(rx_dma_link_, buf_idx, GDMA_LLI_OWNER_DMA);
            gdma_append(rx_dma_chan_);
            last_rx_buf_idx_ = buf_idx;
            continue;
        }
        
        buf->size = rx_size;

        // Sync cache if needed
        if (rx_cache_line_ > 0) {
            size_t sync_size = ALIGN_UP(rx_size, rx_cache_line_);
            esp_cache_msync(buf->data, sync_size, ESP_CACHE_MSYNC_FLAG_DIR_M2C);
        }

        // Update last processed buffer index
        last_rx_buf_idx_ = buf_idx;

        // Deliver buffer to user via callback
        if (rx_callback_) {
            RxEventData data = {
                .buffer = buf,
                .recv_size = rx_size,
            };
            if (rx_callback_(data, rx_callback_user_data_)) {
                need_yield = true;
            }
        } else {
            // No callback registered, return buffer immediately
            gdma_link_set_owner(rx_dma_link_, buf_idx, GDMA_LLI_OWNER_DMA);
        }
    }

    return need_yield;
}

bool UartUhci::HandleGdmaDescrErr() {
    if (!rx_running_.load()) {
        return false;
    }

    // Only process if not already in overflow state (avoid repeated triggers)
    bool expected = false;
    if (!buffer_overflow_.compare_exchange_strong(expected, true)) {
        // Already in overflow state, ignore repeated triggers
        return false;
    }

    // Count how many buffers are owned by CPU (exhausted)
    int cpu_owned = 0;
    for (size_t i = 0; i < rx_pool_size_; i++) {
        gdma_lli_owner_t owner;
        if (gdma_link_get_owner(rx_dma_link_, i, &owner) == ESP_OK && owner == GDMA_LLI_OWNER_CPU) {
            cpu_owned++;
        }
    }

    // Log warning: DMA stopped due to buffer exhaustion (owner check failed)
    // This happens when all buffers are held by CPU and DMA has no buffer to write to
    ESP_DRAM_LOGW(kTag, "GDMA descr_err: buffer exhaustion, %d/%d buffers held by CPU. "
                  "DMA paused until buffers are returned.", cpu_owned, rx_pool_size_);

    // Don't call gdma_append here - it will be called in ReturnBuffer when buffers are freed
    // Calling it here would cause repeated descr_err triggers since no buffers are available

    // Notify upper layer via callback (only once)
    bool need_yield = false;
    if (overflow_callback_) {
        need_yield = overflow_callback_(overflow_callback_user_data_);
    }

    return need_yield;
}
