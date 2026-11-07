#pragma once

#include "network_screen_priv.h"

/** @brief 密码页取消 */
void Network_OnPwdCancel(lv_event_t* e);
/** @brief 清空当前密码缓冲 */
void Network_ClearPassword();
/** @brief 刷新密码与末尾静态 I 光标 */
void Network_RefreshPasswordDisplay();
/** @brief 构建 WiFi 密码输入页 */
void Network_BuildPasswordPage(lv_obj_t* parent, lv_coord_t top_y);
/** @brief 打开指定 SSID 的密码页 */
void Network_OpenPasswordPage(const std::string& ssid, wifi_auth_mode_t authmode);
/** @brief 密码页连接按钮 */
void Network_OnPwdConnect(lv_event_t* e);
/** @brief 关闭密码页并清理临时 UI */
void Network_ClosePasswordPage();
/** @brief 密码缓冲退一格 */
void Network_BackspacePassword();
/** @brief 向密码缓冲追加文本 */
void Network_AppendToPassword(const char* text);
