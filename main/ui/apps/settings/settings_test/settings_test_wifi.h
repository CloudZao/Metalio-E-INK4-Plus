#pragma once

#include "lvgl.h"

/** @brief 调度测试页 WiFi 扫描。 */
void SettingsTestWifi_ScheduleScan();

/** @brief 作废在途扫描，并释放测试模块持有的 WiFi 资源。 */
void SettingsTestWifi_Teardown();

/** @brief 处理 WiFi 测试行点击，打开扫描结果。 */
void SettingsTestWifi_OnRowClicked(lv_event_t* e);
