#ifndef IO_EXPANDER_HPP
#define IO_EXPANDER_HPP

#include <cstdint>
#include <initializer_list>
#include <driver/i2c_master.h>
#include <esp_io_expander.h>
#include <esp_log.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/task.h>
#include "boards/common/bus/i2c/i2c_device_adapters.h"
#include "boards/common/bus/i2c/peripheral_i2c_bus.h"

// IOExpander：TCA9555 的薄封装单例。
// io_index: 0..7 对应 P0.0..P0.7，8..15 对应 P1.0..P1.7。
// 输入读取做短时缓存：按键 5ms 轮询且音量 +/- 各读一次，避免同一 I2C 总线过载。
class IOExpander {
public:
    enum class Pin : uint8_t {
        SCREEN_SOCKET_PWR = 0,  // 屏幕卡座供电
        MAIN_PWR,               // 共享主供电
        PA,                     // 音频功放
        PA_SWITCH,              // 音频路由
        VOLUME_DOWN,            // 音量减输入
        VOLUME_UP,              // 音量加输入
        TOUCH_RST,              // 触摸复位
        PWR_KEY_PULSE,          // 关机脉冲
        ACCEL_INT,              // 加速度计中断
        USB_MUX_SEL,            // USB路由
        kPinCount,              // 逻辑角色数量
    };

    enum class Direction : uint8_t {
        kOutput = 0,  // 输出
        kInput = 1,   // 输入
    };

    struct PinMapEntry {
        Pin pin;                                   // 逻辑角色
        uint8_t io_index;                          // 器件端口位编号
        Direction direction = Direction::kOutput;  // 方向
    };

    /** @brief 获取扩展器常驻实例。 */
    static IOExpander& getInstance() {
        static IOExpander instance;
        return instance;
    }

    IOExpander(const IOExpander&) = delete;
    IOExpander& operator=(const IOExpander&) = delete;

    /** @brief 获取逻辑引脚的固定诊断名称，未知角色返回问号 */
    static const char* PinName(Pin pin) {
        switch (pin) {
            case Pin::SCREEN_SOCKET_PWR:
                return "SCREEN_SOCKET_PWR";
            case Pin::MAIN_PWR:
                return "MAIN_PWR";
            case Pin::PA:
                return "PA";
            case Pin::PA_SWITCH:
                return "PA_SWITCH";
            case Pin::VOLUME_DOWN:
                return "VOLUME_DOWN";
            case Pin::VOLUME_UP:
                return "VOLUME_UP";
            case Pin::TOUCH_RST:
                return "TOUCH_RST";
            case Pin::PWR_KEY_PULSE:
                return "PWR_KEY_PULSE";
            case Pin::ACCEL_INT:
                return "ACCEL_INT";
            case Pin::USB_MUX_SEL:
                return "USB_MUX_SEL";
            default:
                return "?";
        }
    }

    /** @brief 获取引脚方向的固定诊断名称 */
    static const char* DirectionName(Direction d) {
        return d == Direction::kInput ? "IN" : "OUT";
    }

    /** @brief 初始化前登记角色映射，拒绝越界、重复和运行期修改。 */
    esp_err_t setPinMap(std::initializer_list<PinMapEntry> map) {
        if (initialized_) {
            return ESP_ERR_INVALID_STATE;
        }
        if (map.size() == 0 || map.size() > kPinCountValue) {
            ESP_LOGE(TAG, "Invalid pin map");
            return ESP_ERR_INVALID_ARG;
        }
        uint32_t roles = 0, ports = 0;
        for (const auto& entry : map) {
            const auto role = static_cast<size_t>(entry.pin);
            if (role >= kPinCountValue || entry.io_index >= 16 ||
                (entry.direction != Direction::kInput && entry.direction != Direction::kOutput)) {
                ESP_LOGE(TAG, "operation=map result=invalid_entry");
                return ESP_ERR_INVALID_ARG;
            }
            const uint32_t role_mask = 1u << role;
            const uint32_t port_mask = 1u << entry.io_index;
            if ((roles & role_mask) || (ports & port_mask)) {
                ESP_LOGE(TAG, "operation=map result=duplicate_entry");
                return ESP_ERR_INVALID_ARG;
            }
            roles |= role_mask;
            ports |= port_mask;
        }
        clearPinMap();
        for (const auto& entry : map) {
            assignPin(entry);
        }
        return ESP_OK;
    }

    /** @brief 使用指定总线和7位地址初始化器件。 */
    esp_err_t begin(i2c_master_bus_handle_t i2c_bus, uint32_t dev_addr) {
        if (!hasAnyPin()) {
            ESP_LOGE(TAG, "operation=begin result=missing_pin_map");
            return ESP_ERR_INVALID_STATE;
        }
        return beginImpl(i2c_bus, dev_addr);
    }

    /** @brief 设置已登记输出角色的电平。 */
    esp_err_t setLevel(Pin pin, uint8_t level) {
        if (!initialized_ || handle_ == nullptr) {
            ESP_LOGE(TAG, "Not initialized");
            return ESP_ERR_INVALID_STATE;
        }
        const PinSlot* slot = lookupSlot(pin);
        if (slot == nullptr) {
            logUnknownPin(pin);
            return ESP_ERR_NOT_FOUND;
        }
        if (slot->direction != Direction::kOutput) {
            ESP_LOGW(TAG, "setLevel(%s) ignored: pin is input", PinName(pin));
            return ESP_ERR_INVALID_STATE;
        }
        const uint32_t mask = 1U << static_cast<uint32_t>(slot->io_index);
        return esp_io_expander_set_level(handle_, mask, level ? 1 : 0);
    }

    /** @brief 按布尔值设置已登记输出角色的电平 */
    esp_err_t setLevel(Pin pin, bool high) {
        return setLevel(pin, static_cast<uint8_t>(high ? 1 : 0));
    }

    /** @brief 读取已登记角色的输入电平。 */
    esp_err_t getLevel(Pin pin, uint8_t* level) {
        if (!initialized_ || handle_ == nullptr) {
            ESP_LOGE(TAG, "Not initialized");
            return ESP_ERR_INVALID_STATE;
        }
        if (level == nullptr) {
            return ESP_ERR_INVALID_ARG;
        }
        const PinSlot* slot = lookupSlot(pin);
        if (slot == nullptr) {
            logUnknownPin(pin);
            return ESP_ERR_NOT_FOUND;
        }
        const uint32_t mask = 1U << static_cast<uint32_t>(slot->io_index);
        uint32_t value = 0;
        const esp_err_t ret = readInputCached(&value);
        if (ret == ESP_OK) {
            *level = (value & mask) ? 1 : 0;
        }
        return ret;
    }

    /** @brief 查询角色映射与器件初始化是否已完成 */
    bool isInitialized() const {
        return initialized_;
    }

    /** @brief 借用扩展器驱动句柄，不转移释放责任 */
    esp_io_expander_handle_t handle() const {
        return handle_;
    }

private:
    static constexpr const char* TAG = "IOExpander";
    static constexpr uint8_t kUnmapped = 0xFF;
    static constexpr size_t kPinCountValue = static_cast<size_t>(Pin::kPinCount);
    // 音量键 5ms 轮询：缓存 20ms，两键共享一次 I2C 读
    static constexpr TickType_t kInputCacheTtlTicks = pdMS_TO_TICKS(20);
    // 失败后冷却，避免驱动层连续 ESP_LOGE 刷屏
    static constexpr TickType_t kInputFailBackoffTicks = pdMS_TO_TICKS(200);

    struct PinSlot {
        uint8_t io_index = kUnmapped;
        Direction direction = Direction::kOutput;  // 方向
    };

    IOExpander() {
        clearPinMap();
        input_mutex_ = xSemaphoreCreateMutex();
    }

    void clearPinMap() {
        for (auto& slot : pin_to_slot_) {
            slot = PinSlot{};
        }
    }

    void assignPin(const PinMapEntry& entry) {
        const size_t idx = static_cast<size_t>(entry.pin);
        if (idx >= kPinCountValue || entry.io_index >= 16) {
            ESP_LOGE(TAG, "Invalid pin map entry");
            return;
        }
        pin_to_slot_[idx] = PinSlot{entry.io_index, entry.direction};
    }

    bool hasAnyPin() const {
        for (const auto& slot : pin_to_slot_) {
            if (slot.io_index != kUnmapped) {
                return true;
            }
        }
        return false;
    }

    const PinSlot* lookupSlot(Pin pin) const {
        const size_t idx = static_cast<size_t>(pin);
        if (idx >= kPinCountValue) {
            return nullptr;
        }
        const PinSlot& slot = pin_to_slot_[idx];
        return slot.io_index == kUnmapped ? nullptr : &slot;
    }

    void logUnknownPin(Pin pin) const {
        ESP_LOGE(TAG, "Pin '%s' not present in map", PinName(pin));
    }

    void buildDirectionMasks(uint32_t* output_mask, uint32_t* input_mask) const {
        uint32_t out = 0;
        uint32_t in = 0;
        for (size_t i = 0; i < kPinCountValue; ++i) {
            const PinSlot& slot = pin_to_slot_[i];
            if (slot.io_index == kUnmapped) {
                continue;
            }
            const uint32_t bit = 1U << slot.io_index;
            if (slot.direction == Direction::kInput) {
                in |= bit;
            } else {
                out |= bit;
            }
        }
        *output_mask = out;
        *input_mask = in;
    }

    esp_err_t readInputCached(uint32_t* value) {
        if (value == nullptr) {
            return ESP_ERR_INVALID_ARG;
        }
        if (input_mutex_ != nullptr) {
            xSemaphoreTake(input_mutex_, portMAX_DELAY);
        }

        const TickType_t now = xTaskGetTickCount();
        esp_err_t ret = ESP_OK;

        if (input_cache_valid_ && (now - input_cache_tick_) < kInputCacheTtlTicks) {
            *value = input_cache_value_;
            ret = ESP_OK;
        } else if (input_failed_ && now - input_fail_tick_ < kInputFailBackoffTicks) {
            // 冷却期不重试，也不把旧电平作为新采样交付。
            ret = ESP_ERR_TIMEOUT;
        } else {
            uint32_t raw = 0;
            ret = esp_io_expander_get_level(handle_, 0xFFFFu, &raw);
            if (ret == ESP_OK) {
                input_cache_value_ = raw;
                input_cache_tick_ = now;
                input_cache_valid_ = true;
                if (input_failed_) {
                    ESP_LOGI(TAG, "operation=read_input result=recovered");
                }
                input_failed_ = false;
                *value = raw;
            } else {
                input_fail_tick_ = now;
                input_cache_valid_ = false;
                const TickType_t since_log = now - input_fail_log_tick_;
                if (!input_failed_ || since_log >= pdMS_TO_TICKS(5000)) {
                    input_fail_log_tick_ = now;
                    ESP_LOGW(TAG, "operation=read_input code=%d; cached level not delivered", static_cast<int>(ret));
                }
                input_failed_ = true;
            }
        }

        if (input_mutex_ != nullptr) {
            xSemaphoreGive(input_mutex_);
        }
        return ret;
    }

    esp_err_t beginImpl(i2c_master_bus_handle_t i2c_bus, uint32_t dev_addr) {
        if (!input_mutex_) {
            ESP_LOGE(TAG, "operation=begin stage=mutex result=no_memory");
            return ESP_ERR_NO_MEM;
        }
        if (i2c_bus == nullptr || dev_addr > 0x7F) {
            return ESP_ERR_INVALID_ARG;
        }
        if (handle_ != nullptr) {
            return initialized_ ? ESP_OK : ESP_ERR_INVALID_STATE;
        }

        // 上电偶发：总线未稳 / SDA 粘滞 → 写方向寄存器卡满 I2C_TIMEOUT(1s)；
        // 板级 ESP_ERROR_CHECK 会 abort。短超时 probe，失败则复位控制器再试。
        // 勿在此处把全部输出拉低：MAIN_PWR/SCREEN 等会被瞬间掉电，未上电从设备
        // 可能钳住 SDA，下一轮开机 probe 全失败。驱动 reset 后输出锁存为 0xFFFF，
        // 仅改方向即可保持高；具体低电平由板级显式 setLevel。
        constexpr int kAttempts = 10;
        constexpr int kProbeTimeoutMs = 100;
        vTaskDelay(pdMS_TO_TICKS(20));
        esp_err_t ret = ESP_FAIL;
        for (int i = 0; i < kAttempts; ++i) {
            if (i > 0) {
                ESP_LOGW(TAG, "TCA9555 init retry %d/%d after %s", i + 1, kAttempts, esp_err_to_name(ret));
                vTaskDelay(pdMS_TO_TICKS(30 * i));
                if (PeripheralI2cBus_Lock(1000)) {
                    i2c_master_bus_reset(i2c_bus);
                    PeripheralI2cBus_Unlock();
                }
            }

            ret = I2cDeviceAdapters_Probe(static_cast<uint8_t>(dev_addr), kProbeTimeoutMs);
            if (ret != ESP_OK) {
                continue;
            }

            ret = I2cDeviceAdapters_NewExpander(static_cast<uint8_t>(dev_addr), &handle_);
            if (ret == ESP_OK) {
                break;
            }
            handle_ = nullptr;
        }

        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "TCA9555 init failed: %s", esp_err_to_name(ret));
            handle_ = nullptr;
            return ret;
        }

        struct InitRollback {
            esp_io_expander_handle_t& handle;  // 未完成初始化时仍需释放的器件
            bool committed = false;            // 方向配置已全部成功
            ~InitRollback() {
                if (committed) {
                    return;
                }
                const auto result = esp_io_expander_del(handle);
                if (result == ESP_OK) {
                    handle = nullptr;
                } else
                    ESP_LOGE("IOExpander", "operation=begin stage=cleanup code=%d", static_cast<int>(result));
            }
        } rollback{handle_};

        uint32_t output_mask = 0;
        uint32_t input_mask = 0;
        buildDirectionMasks(&output_mask, &input_mask);

        if (output_mask != 0) {
            ret = esp_io_expander_set_dir(handle_, output_mask, IO_EXPANDER_OUTPUT);
            if (ret != ESP_OK) {
                return ret;
            }
        }

        if (input_mask != 0) {
            ret = esp_io_expander_set_dir(handle_, input_mask, IO_EXPANDER_INPUT);
            if (ret != ESP_OK) {
                return ret;
            }
        }

        rollback.committed = true;
        initialized_ = true;
        ESP_LOGI(TAG, "TCA9555 ready addr=0x%02lx out=0x%04lx in=0x%04lx", (unsigned long)dev_addr,
                 (unsigned long)output_mask, (unsigned long)input_mask);
        return ESP_OK;
    }

    PinSlot pin_to_slot_[kPinCountValue];
    esp_io_expander_handle_t handle_ = nullptr;
    bool initialized_ = false;

    SemaphoreHandle_t input_mutex_ = nullptr;
    uint32_t input_cache_value_ = 0xFFFFu;  // 默认高=未按下（低有效）
    TickType_t input_cache_tick_ = 0;
    TickType_t input_fail_tick_ = 0;  // 最近读失败时刻
    bool input_failed_ = false;       // 失败冷却状态
    TickType_t input_fail_log_tick_ = 0;
    bool input_cache_valid_ = false;
};

#endif  // IO_EXPANDER_HPP
