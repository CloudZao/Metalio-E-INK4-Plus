#pragma once

// A2UI 功能域内部共享的辅助接口。

#ifdef __cplusplus
extern "C" {
#endif

/** @brief 将有效按钮动作交给已注册的上层回调。 */
void a2ui_dispatch_action(const char* name);

#ifdef __cplusplus
}
#endif
