#pragma once

#include "lvgl.h"

// WiFi 连接页：扫描周边网络、保存已连接网络并处理重连/清理。
class NetworkScreen {
public:
    /** @brief 创建 WiFi 连接页 */
    static lv_obj_t* Create();

    /**
     * @brief 在传输页内容区构建 WIFI 列表与密码输入
     * @note 待机抑制、保网和激活轮询暂停由外层传输页管理
     */
    static void BuildTransfer(lv_obj_t* parent);

    /** @brief 在设置内容区接入 Wi-Fi 列表，旧页面尚未释放时返回 false，不启动文件服务。 */
    static bool BuildSettings(lv_obj_t* parent);

    /** @brief 关闭设置中的 Wi-Fi 面板并撤销扫描会话。 */
    static void CloseSettings();

    /** @brief 关闭嵌入的 WIFI 面板并释放扫描、连接和文件服务 */
    static void CloseTransfer();

    /** @brief 虚拟键处理；返回 true 表示已消费 */
    static bool OnVkKey(const char* key_name);

    /** @brief 处理独立网络列表的纯翻页，密码页接管输入，嵌入列表交给外层滚动。 */
    static bool OnPageKey(const char* key_name, bool long_press);
};
