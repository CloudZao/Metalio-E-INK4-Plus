#pragma once
#include <sdkconfig.h>

#if CONFIG_IDF_TARGET_ESP32S3
#include "esp32s3/rom/tjpgd.h"
#define IMAGE_HAS_ROM_JPEG 1  // 平台提供 TJpgDec ROM 接口
#elif CONFIG_IDF_TARGET_ESP32
#include "esp32/rom/tjpgd.h"
#define IMAGE_HAS_ROM_JPEG 1
#elif CONFIG_IDF_TARGET_ESP32S31
#include "esp32s31/rom/tjpgd.h"
#define IMAGE_HAS_ROM_JPEG 1
#else
#define IMAGE_HAS_ROM_JPEG 0
#endif
