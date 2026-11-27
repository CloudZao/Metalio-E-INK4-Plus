#pragma once

#include "boards/common/board/board.h"

/** @brief 将联网状态映射为状态栏图标。 */
const char* UiNetworkIcon(const NetworkStatus& status);

/** @brief 将联网阶段映射为当前语言的进度提示，空闲时返回空指针。 */
const char* UiNetworkBusyText(const NetworkStatus& status);
