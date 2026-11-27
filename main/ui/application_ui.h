#pragma once
#include <cstdint>

enum class UiAppId : uint8_t {
    None, // 非内置应用入口
    Checklist, // 待办
    Assistant, // 百问
    Book, // 阅读
    Wallpaper, // 壁纸
    Cloud, // 传输
    Settings, // 设置
    WorkBuddy, // WorkBuddy
    Store, // 应用商店
};

struct UiAppInfo {
    const char* icon_path = nullptr; // 内置应用图标资源
    const char* name = nullptr; // 当前语言下的应用名称
};

/** @brief 获取内置应用展示信息，无有效入口时返回空信息。 */
UiAppInfo UiGetAppInfo(UiAppId id);

/**
 * @brief 在界面上下文请求打开内置应用，待机中先完成硬件唤醒。
 * @return 请求已接受（含等待唤醒）返回true；未知应用或独占界面阻止导航时返回false。
 */
bool UiOpenApp(UiAppId id);

/** @brief 在首次建屏前绑定各应用与公共导航的协作入口。 */
void UiInitializeApplications();

/** @brief 在首次建屏前恢复首页偏好，不创建显示对象。 */
void UiLoadApplicationPreferences();
