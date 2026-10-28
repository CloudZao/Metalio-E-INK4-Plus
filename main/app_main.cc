/**
 * @brief Metalio-E-Ink4-Plus：NVS → Application::Start（网络/激活/OTA/MainEventLoop）
 * @note 显示 Tick 在 LvAdapterEpdiy::Init 后由 lv_epdiy_tick 任务跑，保证开机联网/激活期间顶栏能刷
 */
#include "application.h"
#include "epd_board_metalio_eink4_plus.h"
#include "power_hw.h"

#include <esp_event.h>
#include <esp_log.h>
#include <nvs_flash.h>

#define TAG "eink4p_main"

extern "C" void app_main(void) {
    power_hw_log_sleep_reset();
    epd_board_metalio_eink4_plus_boot_release_holds();

    ESP_ERROR_CHECK(esp_event_loop_create_default());

    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    ESP_LOGI(TAG, "Metalio-E-Ink4-Plus bring-up");
    Application::GetInstance().Start();

    // 运行期由消息调度与显示任务接管；返回后 SDK 回收启动任务的栈。
}
