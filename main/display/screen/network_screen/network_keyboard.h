#pragma once

#include "network_screen_priv.h"

/** @brief 密码页取消 */
void Network_OnPwdCancel(lv_event_t* e);
/** @brief 显示后再按实际尺寸均分方键 */
void Network_LayoutCustomKeyboardSquares();
/** @brief 清空当前密码缓冲 */
void Network_ClearPassword();
/** @brief 按键外观：模式键高亮等 */
void Network_StyleKbKey(lv_obj_t* btn, lv_obj_t* lbl, KbAction action, bool active_mode);
/** @brief 键盘区域尺寸变化时重排方键 */
void Network_OnKbAreaSizeChanged(lv_event_t* e);
/** @brief 刷新密码掩码显示 */
void Network_RefreshPasswordDisplay();
/** @brief 退格键长按：连续删字 */
void Network_OnBackspaceLongPressed(lv_event_t* e);
/** @brief 按当前布局填充键位描述表 */
void Network_FillKbLayout(KbKeyDesc out[kKbRows * kKbCols]);
/** @brief 字符/空格/退格：按下即写入 */
void Network_OnCustomKeyPressed(lv_event_t* e);
/** @brief 切布局：松手再建盘，避免 PRESSED 里删掉当前键 */
void Network_OnCustomKeyClicked(lv_event_t* e);
/** @brief 构建 WiFi 密码输入页 */
void Network_BuildPasswordPage(lv_obj_t* parent, lv_coord_t top_y);
/** @brief 打开指定 SSID 的密码页 */
void Network_OpenPasswordPage(const std::string& ssid, wifi_auth_mode_t authmode);
/** @brief 密码页连接按钮 */
void Network_OnPwdConnect(lv_event_t* e);
/** @brief 按当前模式重建自定义键盘 */
void Network_RebuildCustomKeyboard();
/** @brief 关闭密码页并清理临时 UI */
void Network_ClosePasswordPage();
/** @brief 密码缓冲退一格 */
void Network_BackspacePassword();
/** @brief 向密码缓冲追加文本 */
void Network_AppendToPassword(const char* text);
