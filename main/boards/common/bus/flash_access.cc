#include <esp_log.h>
#include "flash_access.h"
#include "flash_access_priv.h"

static unsigned s_access_depth = 0;  // 仅在持有递归缓存门闩的任务中访问
static const char* TAG = "FlashAccess";

static std::recursive_mutex& FlashCacheMutex() {
    static std::recursive_mutex mutex;
    return mutex;
}

FlashCacheGuard::FlashCacheGuard() : lock_(FlashCacheMutex()) {}

FlashAccessGuard::FlashAccessGuard() {
    if (s_access_depth == 0) {
        status_ = FlashBusAcquire();
        if (status_ != ESP_OK) {
            ESP_LOGE(TAG, "bus admission failed: code=%d; Flash operation rejected", static_cast<int>(status_));
            return;
        }
    }
    ++s_access_depth;
}

FlashAccessGuard::~FlashAccessGuard() {
    if (status_ == ESP_OK && --s_access_depth == 0) {
        FlashBusRelease();
    }
}
