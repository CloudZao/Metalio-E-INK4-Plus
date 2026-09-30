#pragma once

#include "home_screen_priv.h"

/** @brief 盖板键短按翻应用页 */
bool Home_OnVkKey(const char* key);
/** @brief BOOT 长按快捷进百问 */
bool Home_OnBootLongPress();
/** @brief 应用页连翻步进 */
bool HomePageRepeatStep(int page_delta);
/** @brief 首页删除事件：停 Hero */
void Home_OnHomeDeleted(lv_event_t* e);
/** @brief 盖板键长按连翻 */
bool Home_OnVkKeyLongPress(const char* key);
/** @brief 填充经典布局应用页 */
void Home_FillAppsPage();
/** @brief 从 NVS 加载卡片样式到缓存 */
void Home_LoadCardStyleFromNvs();
/** @brief 盖板键抬起停止连翻 */
bool Home_OnVkKeyPressUp(const char* key);
/** @brief 规范化卡片样式枚举 */
int Home_NormalizeCardStyle(int style);
/** @brief 点击应用格 */
void Home_OnAppClicked(lv_event_t* e);
/** @brief 持久化卡片样式后台任务 */
void Home_PersistCardStyleTask(void* arg);
/** @brief 异步启动应用页 */
void Home_LaunchAppAsync(void* user_data);
/** @brief 请求填充应用页 */
void Home_RequestFillAppsPage();
/** @brief 应用页总页数 */
int Home_PageCount();
/** @brief 灰网点纹理图 */
const lv_image_dsc_t* Home_GrayDitherImg();
/** @brief 应用名：清单 */
const char* Home_AppNameTask();
/** @brief 应用名：百问 */
const char* Home_AppNameAssistant();
/** @brief 应用名：书库 */
const char* Home_AppNameBook();
/** @brief 应用名：壁纸 */
const char* Home_AppNameWallpaper();
/** @brief 应用名：云端 */
const char* Home_AppNameCloud();
/** @brief 应用名：设置 */
const char* Home_AppNameSettings();
/** @brief 创建经典布局应用格 */
lv_obj_t* Home_CreateAppCell(lv_obj_t* parent, const AppEntry& entry, int card_style);
