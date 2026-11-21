#pragma once
#include "esp_partition.h"
using esp_ota_img_states_t=int;
constexpr int ESP_OTA_IMG_VALID=2,ESP_OTA_IMG_PENDING_VERIFY=1;
const esp_partition_t* esp_ota_get_running_partition();
int esp_ota_get_state_partition(const esp_partition_t*,esp_ota_img_states_t*);
int esp_ota_mark_app_valid_cancel_rollback();
int esp_ota_set_boot_partition(const esp_partition_t*);
