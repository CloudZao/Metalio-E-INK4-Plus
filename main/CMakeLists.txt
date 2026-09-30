# Metalio-E-Ink4-Plus：百问 AI + 首页/设置 + EPDiy
set(BOARD_DIR "boards/metalio-e-ink4-plus")

file(GLOB BOARD_COMMON_SRCS
    "${CMAKE_CURRENT_SOURCE_DIR}/boards/common/board/*.cc"
    "${CMAKE_CURRENT_SOURCE_DIR}/boards/common/power/*.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/boards/common/power/*.cc"
    "${CMAKE_CURRENT_SOURCE_DIR}/boards/common/power_policy/*.cc"
    "${CMAKE_CURRENT_SOURCE_DIR}/boards/common/sensor/*.cc"
    "${CMAKE_CURRENT_SOURCE_DIR}/boards/common/audio/*.cc"
    "${CMAKE_CURRENT_SOURCE_DIR}/boards/common/display/*.cc"
    "${CMAKE_CURRENT_SOURCE_DIR}/boards/common/storage/*.cc"
)

file(GLOB BOARD_SRCS
    "${CMAKE_CURRENT_SOURCE_DIR}/${BOARD_DIR}/*.cc"
    "${CMAKE_CURRENT_SOURCE_DIR}/${BOARD_DIR}/bus/*.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/${BOARD_DIR}/display/*.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/${BOARD_DIR}/display/*.cc"
    "${CMAKE_CURRENT_SOURCE_DIR}/${BOARD_DIR}/display/waveforms/*.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/${BOARD_DIR}/display/output_lcd/*.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/${BOARD_DIR}/display/output_common/*.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/${BOARD_DIR}/input/*.cc"
    "${CMAKE_CURRENT_SOURCE_DIR}/${BOARD_DIR}/periph/*.cc"
)

file(GLOB HOME_SCREEN_SRCS
    "${CMAKE_CURRENT_SOURCE_DIR}/display/screen/home_screen/*.cc"
)

file(GLOB SETTINGS_SCREEN_SRCS
    "${CMAKE_CURRENT_SOURCE_DIR}/display/screen/settings_screen/*.cc"
    "${CMAKE_CURRENT_SOURCE_DIR}/display/screen/settings_screen/settings_test/*.cc"
)

file(GLOB BLUETOOTH_SCREEN_SRCS
    "${CMAKE_CURRENT_SOURCE_DIR}/display/screen/bluetooth_screen/*.cc"
)

file(GLOB NETWORK_SCREEN_SRCS
    "${CMAKE_CURRENT_SOURCE_DIR}/display/screen/network_screen/*.cc"
)

file(GLOB ASSISTANT_SCREEN_SRCS
    "${CMAKE_CURRENT_SOURCE_DIR}/display/screen/assistant_screen/*.cc"
)

file(GLOB BOOK_SCREEN_SRCS
    "${CMAKE_CURRENT_SOURCE_DIR}/display/screen/book_screen/*.cc"
    "${CMAKE_CURRENT_SOURCE_DIR}/display/screen/book_screen/cover/*.cc"
    "${CMAKE_CURRENT_SOURCE_DIR}/display/screen/book_screen/reader/*.cc"
    "${CMAKE_CURRENT_SOURCE_DIR}/display/screen/book_screen/settings/*.cc"
    "${CMAKE_CURRENT_SOURCE_DIR}/display/screen/book_screen/shelf/*.cc"
)

file(GLOB CLOUD_SCREEN_SRCS
    "${CMAKE_CURRENT_SOURCE_DIR}/display/screen/cloud_screen/*.cc"
    "${CMAKE_CURRENT_SOURCE_DIR}/display/screen/cloud_screen/download/*.cc"
    "${CMAKE_CURRENT_SOURCE_DIR}/display/screen/cloud_screen/preview/*.cc"
    "${CMAKE_CURRENT_SOURCE_DIR}/display/screen/cloud_screen/push/*.cc"
)

file(GLOB WALLPAPER_SCREEN_SRCS
    "${CMAKE_CURRENT_SOURCE_DIR}/display/screen/wallpaper_screen/*.cc"
    "${CMAKE_CURRENT_SOURCE_DIR}/display/screen/wallpaper_screen/preview/*.cc"
    "${CMAKE_CURRENT_SOURCE_DIR}/display/screen/wallpaper_screen/thumbs/*.cc"
)

file(GLOB STANDBY_SCREEN_SRCS
    "${CMAKE_CURRENT_SOURCE_DIR}/display/screen/standby_screen/*.cc"
)

file(GLOB READER_SRCS
    "${CMAKE_CURRENT_SOURCE_DIR}/reader/*.cc"
)

file(GLOB A2UI_SRCS
    "${CMAKE_CURRENT_SOURCE_DIR}/display/a2ui/*.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/display/a2ui/*.cc"
)

file(GLOB OTA_UI_SRCS
    "${CMAKE_CURRENT_SOURCE_DIR}/display/screen/ota_upgrade_screen/*.cc"
    "${CMAKE_CURRENT_SOURCE_DIR}/display/screen/ota_confirm_dialog/*.cc"
)

file(GLOB PROTOCOL_SRCS
    "${CMAKE_CURRENT_SOURCE_DIR}/protocols/protocol.cc"
    "${CMAKE_CURRENT_SOURCE_DIR}/protocols/websocket_protocol.cc"
)

set(AUDIO_PROCESSOR_SRCS "audio/processors/no_audio_processor.cc")
if(CONFIG_USE_AUDIO_PROCESSOR)
    set(AUDIO_PROCESSOR_SRCS "audio/processors/afe_audio_processor.cc")
endif()

set(WAKE_WORD_SRCS "")
if(CONFIG_USE_AFE_WAKE_WORD)
    list(APPEND WAKE_WORD_SRCS "audio/wake_words/afe_wake_word.cc")
elseif(CONFIG_USE_ESP_WAKE_WORD)
    list(APPEND WAKE_WORD_SRCS "audio/wake_words/esp_wake_word.cc")
elseif(CONFIG_USE_CUSTOM_WAKE_WORD)
    list(APPEND WAKE_WORD_SRCS "audio/wake_words/custom_wake_word.cc")
endif()

# 本轮不编摄像头拍照路径（无 UVC）；用 stub
list(FILTER ASSISTANT_SCREEN_SRCS EXCLUDE REGEX "assistant_boot_photo\\.cc$")

set(ASSISTANT_HINT_EN_A2I1
    "${CMAKE_CURRENT_SOURCE_DIR}/assets/embedded/ic_s_assistant_hint_en.a2i1")

idf_component_register(
    SRCS
        "app_main.cc"
        "application.cc"
        "ask_ai_boot_stubs.cc"
        "settings.cc"
        "haptic_feedback.cc"
        "assets/lang_home.cc"
        "assets.cc"
        "ota.cc"
        "ota_ignore.cc"
        "mcp_server.cc"
        "system_info.cc"
        "cloudzao_endpoints.c"
        "device_state_event.cc"
        "display/display.cc"
        "display/display_orient.cc"
        "display/epd_gray_aa.cc"
        "display/lv_adapter_epdiy.cc"
        "display/lv_adapter_epdiy_touch.cc"
        "display/epd_font24/epd_font24.c"
        "display/font/font_loader.c"
        "display/font/fontpack_lvgl.cc"
        "display/font/epdfont.cc"
        "display/font/ttf_convert.cc"
        "display/font/font_misans_regular_160_2.c"
        "display/font/font_awesome_30_1.c"
        "display/lvgl_display/emoji_collection.cc"
        "display/lvgl_display/jpg/jpeg_to_image.c"
        "display/screen/screen_common.cc"
        "display/screen/screen_common_nav.cc"
        "display/screen/vk_key_handler.cc"
        "display/screen/vk_page_repeat.cc"
        "display/screen/boot_key_handler.cc"
        "display/screen/checklist_cache.cc"
        "display/screen/task_screen/task_screen.cc"
        "audio/audio_codec.cc"
        "audio/audio_service.cc"
        "audio/processors/audio_debugger.cc"
        ${AUDIO_PROCESSOR_SRCS}
        ${WAKE_WORD_SRCS}
        ${PROTOCOL_SRCS}
        ${A2UI_SRCS}
        ${ASSISTANT_SCREEN_SRCS}
        ${BOOK_SCREEN_SRCS}
        ${CLOUD_SCREEN_SRCS}
        ${WALLPAPER_SCREEN_SRCS}
        ${STANDBY_SCREEN_SRCS}
        ${READER_SRCS}
        ${OTA_UI_SRCS}
        ${HOME_SCREEN_SRCS}
        ${SETTINGS_SCREEN_SRCS}
        ${BLUETOOTH_SCREEN_SRCS}
        ${NETWORK_SCREEN_SRCS}
        ${BOARD_COMMON_SRCS}
        ${BOARD_SRCS}
        "boards/common/sensor/sc7a20h.cc"
        "boards/metalio-e-ink4-plus/periph/metalio_accel.cc"
    EMBED_FILES
        ${ASSISTANT_HINT_EN_A2I1}
    INCLUDE_DIRS
        "."
        "assets"
        "audio"
        "audio/processors"
        "audio/wake_words"
        "protocols"
        "reader"
        "display"
        "display/a2ui"
        "display/epd_font24"
        "display/font"
        "display/lvgl_display"
        "display/lvgl_display/jpg"
        "display/screen"
        "display/screen/home_screen"
        "display/screen/settings_screen"
        "display/screen/settings_screen/settings_test"
        "display/screen/bluetooth_screen"
        "display/screen/network_screen"
        "display/screen/assistant_screen"
        "display/screen/book_screen"
        "display/screen/cloud_screen"
        "display/screen/wallpaper_screen"
        "display/screen/standby_screen"
        "display/screen/ota_upgrade_screen"
        "display/screen/ota_confirm_dialog"
        "display/screen/task_screen"
        "boards/common/board"
        "boards/common/power"
        "boards/common/power_policy"
        "boards/common/sensor"
        "boards/common/audio"
        "boards/common/display"
        "boards/common/storage"
        "boards/common/camera"
        "${BOARD_DIR}"
        "${BOARD_DIR}/bus"
        "${BOARD_DIR}/display"
        "${BOARD_DIR}/display/waveforms"
        "${BOARD_DIR}/display/output_lcd"
        "${BOARD_DIR}/display/output_common"
        "${BOARD_DIR}/input"
        "${BOARD_DIR}/periph"
    REQUIRES
        lvgl esp_partition fatfs nvs_flash esp_mmap_assets esp_lvgl_adapter
        app_update spi_flash esp_psram esp_wifi esp_netif esp_http_client
        mbedtls esp_timer efuse
    PRIV_REQUIRES
        esp-wifi-connect esp-ml307 espressif__cjson 78__esp-opus-encoder 78__esp-opus
)

target_link_libraries(${COMPONENT_LIB} PUBLIC
    idf::driver
    idf::esp_driver_gpio
    idf::esp_driver_i2c
    idf::esp_driver_i2s
    idf::esp_driver_uart
    idf::esp_driver_rmt
    idf::esp_driver_dma
    idf::esp_driver_ledc
    idf::esp_driver_sdmmc
    idf::esp_timer
    idf::esp_adc
    idf::esp_lcd
    idf::esp_hal_lcd
    idf::esp_hal_i2s
    idf::esp_hal_rmt
    idf::esp_hw_support
    idf::esp_mm
    idf::esp_event
    idf::esp_wifi
    idf::esp_netif
    idf::nvs_flash
    idf::freertos
    idf::log
    idf::sdmmc
    idf::app_update
)

# zlib：EPUB/.ebook；freetype：阅读 TTF；esp_new_jpeg：封面/插图
target_link_libraries(${COMPONENT_LIB} PRIVATE
    idf::espressif__zlib
    idf::espressif__freetype
    idf::espressif__esp_new_jpeg
)
# 设置页模拟 U 盘（TinyUSB MSC）；IDF 6.1/S31 无独立 idf::usb，依赖由 esp_tinyusb 带入
if(TARGET idf::espressif__esp_tinyusb)
    target_link_libraries(${COMPONENT_LIB} PRIVATE idf::espressif__esp_tinyusb)
endif()

target_compile_options(${COMPONENT_LIB} PRIVATE
    -Wno-error=format-truncation
    -Wno-error=class-memaccess
    -Wno-error=format=
    -Wno-format
    -Wno-error=unused-function
    -Wno-error=maybe-uninitialized
    -fexceptions
)

target_compile_definitions(${COMPONENT_LIB} PRIVATE
    BOARD_TYPE="metalio-e-ink4-plus"
    BOARD_NAME="metalio-e-ink4-plus"
    LV_LVGL_H_INCLUDE_SIMPLE
)

# LVGL I1 落墨补丁 #include "epd_i1_glyph_thin.h"：须让 LVGL 编译单元找到 main/display/
foreach(_lvgl_comp lvgl__lvgl lvgl)
    idf_component_get_property(_lvgl_lib ${_lvgl_comp} COMPONENT_LIB)
    if(_lvgl_lib)
        target_include_directories(${_lvgl_lib} PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/display")
        break()
    endif()
endforeach()

spiffs_create_partition_assets(
    resources
    "./xingzhi-assets"
    FLASH_IN_PROJECT
    MMAP_FILE_SUPPORT_FORMAT ".jpg,.png,.eaf,.a2i1"
    MMAP_SUPPORT_SJPG
    MMAP_SUPPORT_SPNG
    MMAP_SPLIT_HEIGHT 16
)
