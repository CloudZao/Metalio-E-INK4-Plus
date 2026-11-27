#ifndef DISPLAY_H
#define DISPLAY_H

#define HAVE_LVGL 1
#include <string>
#include <chrono>
#include <cstdint>
#include <lvgl.h>
#include <esp_timer.h>
#include <esp_log.h>
#include <esp_pm.h>

class Theme {
public:
    /** @brief 创建保存主题名称的对象。 */
    Theme(const std::string& name) : name_(name) {}

    /** @brief 销毁主题对象。 */
    virtual ~Theme() = default;

    /** @brief 获取主题名称。 */
    inline std::string name() const {
        return name_;
    }

private:
    std::string name_;
};

// Idle 时状态栏中部文案策略，由当前 Screen 在进/离页时设置。
enum class IdleStatusMode : uint8_t {
    kClock = 0,      // 周期刷新 HH:MM
    kFixedText = 1,  // 固定文案，如会话页的「待命」
};

class Display {
public:
    /** @brief 初始化显示公共状态和界面主题缓存。 */
    Display();

    /** @brief 销毁显示公共接口。 */
    virtual ~Display();

    /** @brief 设置状态文案，具体显示方式由后端实现。 */
    virtual void SetStatus(const char* status);

    /** @brief 显示临时通知，duration_ms 指定显示时长。 */
    virtual void ShowNotification(const char* notification, int duration_ms = 3000);

    /** @brief 显示临时通知，duration_ms 指定显示时长。 */
    virtual void ShowNotification(const std::string& notification, int duration_ms = 3000);

    /** @brief 设置情绪显示，具体显示方式由后端实现。 */
    virtual void SetEmotion(const char* emotion);

    /** @brief 向显示后端传递会话消息。 */
    virtual void SetChatMessage(const char* role, const char* content);

    /** @brief 设置并保存主题选择；主题对象由调用方持有。 */
    virtual void SetTheme(Theme* theme);

    /** @brief 获取当前主题对象，不转移所有权。 */
    virtual Theme* GetTheme() {
        return current_theme_;
    }

    /** @brief 更新状态栏；update_all 请求重读完整状态。 */
    virtual void UpdateStatusBar(bool update_all = false);

    /** @brief 切换显示后端的省电表现。 */
    virtual void SetPowerSaveMode(bool on);

    /**
     * @brief 设置当前页面的空闲状态栏策略。
     * @note fixed_text 仅在固定文案模式生效；空值使用默认状态文案。
     */
    void SetIdleStatusMode(IdleStatusMode mode, const char* fixed_text = nullptr);

    /** @brief 获取当前空闲状态栏策略。 */
    IdleStatusMode GetIdleStatusMode() const {
        return idle_status_mode_;
    }

    /** @brief 获取已缓存的固定状态文案，返回指针由显示对象持有。 */
    const char* GetIdleStatusFixedText() const {
        return idle_status_fixed_text_;
    }

    /** @brief 判断当前页面是否允许状态栏显示时钟。 */
    bool AllowsIdleStatusClock() const;

    /** @brief 获取显示宽度。 */
    inline int width() const {
        return width_;
    }

    /** @brief 获取显示高度。 */
    inline int height() const {
        return height_;
    }

    /**
     * @brief 墨水落墨临界区：刷屏中置位，供 WiFi 等协作 defer
     * @note 仅一个闸门；硬冲突在 enter 里 pause，软抢 CPU 看本标志推迟
     */
    static void SetPanelBusy(bool busy);

    /** @brief 是否正在落墨 */
    static bool IsPanelBusy();

    /**
     * @brief 等到落墨结束或超时
     * @param timeout_ms 最长等待；0 只探一次
     * @return true 已空闲
     */
    static bool WaitPanelIdle(int timeout_ms);

protected:
    int width_ = 0;
    int height_ = 0;

    Theme* current_theme_ = nullptr;
    IdleStatusMode idle_status_mode_ = IdleStatusMode::kClock;
    char idle_status_fixed_text_[32] = {};

    friend class DisplayLockGuard;
    virtual bool Lock(int timeout_ms = 0) = 0;
    virtual void Unlock() = 0;
};

class DisplayLockGuard {
public:
    /** @brief 尝试在一秒内取得显示锁，仅在成功持锁时由析构函数解锁。 */
    explicit DisplayLockGuard(Display* display) : display_(display), locked_(display && display->Lock(1000)) {
        if (!locked_) {
            ESP_LOGE("Display", "Failed to lock display");
        }
    }

    /** @brief 释放本守卫成功持有的显示锁。 */
    ~DisplayLockGuard() {
        if (locked_) {
            display_->Unlock();
        }
    }

    /** @brief 判断本守卫是否已成功持有显示锁。 */
    explicit operator bool() const {
        return locked_;
    }
    DisplayLockGuard(const DisplayLockGuard&) = delete;
    DisplayLockGuard& operator=(const DisplayLockGuard&) = delete;

private:
    Display* display_;
    bool locked_;  // 仅在本实例成功取得锁后解锁
};

class NoDisplay : public Display {
private:
    virtual bool Lock(int timeout_ms = 0) override {
        return true;
    }
    virtual void Unlock() override {}
};

#endif
