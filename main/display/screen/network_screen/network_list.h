#pragma once

#include "network_screen_priv.h"

/** @brief 已保存项设为默认网络 */
void Network_OnSavedSetDefault(lv_event_t* e);
/** @brief 投递「刷新」按钮使能状态到 UI 任务 */
void Network_PostScanBtnEnabled(bool enabled);
/** @brief 重建附近 WiFi 列表当前页 */
void Network_RebuildNearbyPage();
/** @brief 设置状态栏文案（须在 LVGL 任务） */
void Network_SetStatus(const char* text);
/** @brief 异步设置状态栏文案（lv_async 载荷） */
void Network_AsyncSetStatus(void* p);
/** @brief 重建已保存列表当前页 */
void Network_RebuildSavedPage();
/** @brief 切换附近 / 已保存 Tab */
void Network_SwitchTab(Tab tab);
/** @brief 异步重建列表（lv_async 载荷） */
void Network_AsyncRebuildList(void* p);
/** @brief 附近列表项删除 */
void Network_OnNearbyItemDelete(lv_event_t* e);
/** @brief 已保存项移除 */
void Network_OnSavedRemove(lv_event_t* e);
/** @brief 异步刷新扫描按钮样式 */
void Network_AsyncStyleScan(void* p);
/** @brief 已保存列表总页数 */
int Network_SavedPageCount();
/** @brief 切到附近 Tab */
void Network_OnNearbyTab(lv_event_t* e);
/** @brief 附近列表总页数 */
int Network_NearbyPageCount();
/** @brief 按当前 Tab 重建列表页 */
void Network_RebuildListPage();
/** @brief 切到已保存 Tab */
void Network_OnSavedTab(lv_event_t* e);
/** @brief 已保存行删除按钮 */
void Network_OnSavedBtnDelete(lv_event_t* e);
/** @brief 投递状态文案到 UI 任务 */
void Network_PostStatus(const char* text);
/** @brief 附近列表项点击（连接或弹密码） */
void Network_OnNearbyItemClicked(lv_event_t* e);
/** @brief 清空已保存网络 */
void Network_OnClearSaved(lv_event_t* e);
/** @brief 投递重建列表到 UI 任务 */
void Network_PostRebuildList();
/** @brief 点击刷新：发起扫描 */
void Network_OnScanClicked(lv_event_t* e);
