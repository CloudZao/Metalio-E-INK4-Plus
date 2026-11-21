#pragma once
#include <cstddef>
#include <cstdint>
using esp_partition_type_t=int;
using esp_partition_subtype_t=int;
constexpr int ESP_OK=0;
constexpr int ESP_PARTITION_TYPE_APP=0,ESP_PARTITION_TYPE_DATA=1;
constexpr int ESP_PARTITION_SUBTYPE_DATA_NVS=2,ESP_PARTITION_SUBTYPE_DATA_OTA=0,ESP_PARTITION_SUBTYPE_DATA_PHY=1;
constexpr int ESP_PARTITION_SUBTYPE_APP_FACTORY=0,ESP_PARTITION_SUBTYPE_APP_OTA_0=16,ESP_PARTITION_SUBTYPE_APP_OTA_1=17;
struct esp_partition_t{int type,subtype;uint32_t address,size;const char* label;};
const esp_partition_t* esp_partition_find_first(int,int,const char*);
int esp_partition_read(const esp_partition_t*,uint32_t,void*,size_t);
