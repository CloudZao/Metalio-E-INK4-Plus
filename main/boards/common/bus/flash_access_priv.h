#pragma once

#include <esp_err.h>

#ifdef __cplusplus
extern "C" {
#endif

/** @brief 已持缓存门闩时排空当前实现组合的共享总线，仅供Flash接纳层调用。 */
esp_err_t FlashBusAcquire(void);

/** @brief 释放Flash接纳层持有的共享总线。 */
void FlashBusRelease(void);
#ifdef __cplusplus
}
#endif
