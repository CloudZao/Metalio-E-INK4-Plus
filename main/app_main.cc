/**
 * @file app_main.cc
 * @brief 初始化公共运行环境并启动应用
 */
#include <esp_event.h>
#include <esp_log.h>
#include <nvs_flash.h>
#include "business/conversation/activation_polling.h"
#include "boards/common/bluetooth/ble_page_turner.h"
#include "boards/common/power/power_hw.h"
#include "boards/common/storage/sd_access.h"
#include "business/reader/list_thumbnail_cache.h"
#include "infra/network/http/http_download_file.h"
#include "infra/network/time/ntp_server.h"
#include "sysmgmt/application.h"
#include "sysmgmt/power/power_lifecycle.h"
#include "ui/common/runtime/input/pager_key_handler.h"

#define TAG "Main"

extern "C" void app_main(void) {
    // SDK 调频配置会随屏幕刷新频繁打印，默认仅保留该模块的告警和错误。
    esp_log_level_set("pm", ESP_LOG_WARN);
    SdAccessConfigureCacheRelease(ClearListThumbnailCache);
    PowerLifecycleInit();
    HttpDownloadConfigureActivity([] { ActivationPolling::GetInstance().Pause(); },
                                  [] { ActivationPolling::GetInstance().Resume(); });
    power_hw_boot_prepare();

    ESP_ERROR_CHECK(esp_event_loop_create_default());

    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);
    NtpServer::GetInstance().Initialize();

    ESP_LOGI(TAG, "Board bring-up");
    BlePageTurnerInit(PagerKey_Receive);
    if (!Application::GetInstance().Start()) {
        ESP_LOGW(TAG, "startup incomplete");
    }

    // 运行期由消息调度与显示任务接管；返回后 SDK 回收启动任务的栈。
}
