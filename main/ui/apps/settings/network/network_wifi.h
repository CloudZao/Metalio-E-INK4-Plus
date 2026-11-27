#pragma once
#include <string>
#include <lvgl.h>
#include "business/network/wifi_setup.h"

/** @brief 绑定普通配网页或传输子页面的更新、离页与待机协作。 */
void Network_BindWifiSetup(lv_obj_t* root);

/** @brief 请求扫描，结果由页面生命周期更新接收。 */
void Network_ScheduleScan();

/** @brief 提交连接，不在UI线程初始化驱动或保存配置。 */
void Network_ScheduleConnect(const std::string& ssid, const std::string& password);

/** @brief 提交已保存网络的默认项、删除或清空操作。 */
void Network_ScheduleSavedChange(WifiSetupOperation operation, const std::string& ssid = {});

/** @brief 解除结果接收并请求后台关闭，不等待扫描、连接或驱动销毁。 */
void Network_StopForScreen();
