#pragma once

/** @brief 持有界面锁时采样并分发公共按键业务；待机电源键由低功耗任务独占。 */
void UiInputPoll();

/** @brief 当前系统状态是否阻止侧键导航；普通窗口由公共模态策略处理。 */
bool UiInputNavigationBlocked();

/** @brief 仅升级执行阶段禁止下拉菜单，不与普通弹窗及按键策略绑定。 */
bool UiInputQuickPanelBlocked();
