#pragma once
#include <lvgl.h>
#include "page_lifecycle.h"

enum class UiModalPolicy {
    Dismissible, // 返回交给取消回调，其余按键不传给下层
    Locked // 仅业务完成或失败后关闭，窗口内部控件仍可响应
};
enum class UiModalInput {
    Back, // 返回或 Home 短按
    Other // 翻页、BOOT、长按及松手
};

class UiModal {
public:
    /**
     * @brief 组合到已有窗口；实例须比窗口存活更久，取消回调负责业务取消或窗口关闭
     * @note 离页默认复用取消回调；分级窗口可提供仅清理视图的 leave 回调，避免离页触发内部导航
     */
    void Bind(lv_obj_t* root, void (*cancel)(void*) = nullptr, void* context = nullptr,
              UiModalPolicy policy = UiModalPolicy::Dismissible, void (*leave)(void*) = nullptr);

    /** @brief 撤销窗口登记，不销毁控件或取消业务 */
    void Unbind();

    /** @brief 将按键交给最上层可见窗口；返回 true 表示已消费，不得继续传给页面 */
    static bool HandleInput(UiModalInput input);

    /** @brief 判断控件是否为登记窗口的根，供手势在窗口边界停止追溯底层页面 */
    static bool IsRoot(lv_obj_t* object);

    /** @brief 创建未绑定窗口的控制器 */
    UiModal() = default;

    /** @brief 撤销登记，窗口控件仍由页面所有者管理 */
    ~UiModal();
    UiModal(const UiModal&) = delete;
    UiModal& operator=(const UiModal&) = delete;

private:
    static void OnEvent(lv_event_t* event);
    void Close(bool leaving = false);
    UiPageLifecycle lifecycle_;
    lv_obj_t* root_ = nullptr;
    lv_obj_t* owner_ = nullptr;
    UiModal* next_ = nullptr;
    void (*cancel_)(void*) = nullptr; // 仅在 UI 线程调用，可请求异步停止
    void (*leave_)(void*) = nullptr; // 与取消不同时，负责离页清理
    void* context_ = nullptr; // 由窗口所有者管理，解绑后不再访问
    UiModalPolicy policy_ = UiModalPolicy::Dismissible;
};
