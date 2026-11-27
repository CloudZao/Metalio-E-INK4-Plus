#pragma once
#include <lvgl.h>

/** @brief 按当前界面语言返回按键设置文案。 */
const char *SettingsKeysText(const char *chinese, const char *english);

/** @brief 构建按键与手势设置的开关、按键列表、功能说明和恢复默认入口。 */
void SettingsKeysBuild(lv_obj_t *page);

/** @brief 同步手势开关的配置与保存状态。 */
void SettingsKeysRefresh();

/** @brief 创建当前所选按键的全屏编辑页。 */
lv_obj_t *SettingsKeysCreate();
