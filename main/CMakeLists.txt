# Metalio-E-Ink4-Plus：百问 AI + 首页/设置 + EPDiy
set(AP_FILEMAN_GZ "${CMAKE_CURRENT_BINARY_DIR}/ap_fileman.html.gz")
set(BOARD_DIR "boards/metalio-e-ink4-plus")

# Storage sources include the runtime log writer.
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

file(GLOB WORKBUDDY_SRCS "${CMAKE_CURRENT_SOURCE_DIR}/workbuddy/*.cc"
    "${CMAKE_CURRENT_SOURCE_DIR}/display/screen/workbuddy_screen/*.cc")

file(GLOB HOME_SCREEN_SRCS
    "${CMAKE_CURRENT_SOURCE_DIR}/display/screen/home_screen/*.cc"
    "${CMAKE_CURRENT_SOURCE_DIR}/display/screen/home_screen/pixel/*.cc"
)

# Settings sources include manual log upload and its UI.
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
    "${CMAKE_CURRENT_SOURCE_DIR}/display/screen/cloud_screen/ap/*.cc"
    "${CMAKE_CURRENT_SOURCE_DIR}/display/screen/cloud_screen/ap/*.c"
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
    "${CMAKE_CURRENT_SOURCE_DIR}/reader/jpeg/*.cc"
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
    "${CMAKE_CURRENT_SOURCE_DIR}/protocols/mqtt_protocol.cc"
)

set(AUDIO_PROCESSOR_SRCS "audio/processors/no_audio_processor.cc")
if(CONFIG_USE_AUDIO_PROCESSOR)
    list(APPEND AUDIO_PROCESSOR_SRCS "audio/processors/afe_audio_processor.cc")
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

idf_component_register(
    SRCS
        "app_main.cc"
        "application.cc"
        "application_tasks.cc"
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
        "display/epd_reader_aa.cc"
        "display/epd_content_policy.cc"
        "display/lv_adapter_epdiy.cc"
        "display/epd_refresh.cc"
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
        "boards/common/camera/dvp_camera.cc"
        "display/screen/screen_common.cc"
        "display/screen/screen_common_nav.cc"
        "display/screen/screen_frontlight.cc"
        "display/screen/vk_key_handler.cc"
        "display/screen/vk_page_repeat.cc"
        "display/screen/boot_key_handler.cc"
        "display/screen/checklist_cache.cc"
        "display/screen/task_screen/task_screen.cc"
        "audio/audio_codec.cc"
        "audio/audio_service.cc"
        "audio/audio_service_capture.cc"
        "display/ui_qrcode.cc"
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
        ${WORKBUDDY_SRCS}
        ${SETTINGS_SCREEN_SRCS}
        ${BLUETOOTH_SCREEN_SRCS}
        ${NETWORK_SCREEN_SRCS}
        ${BOARD_COMMON_SRCS}
        ${BOARD_SRCS}
        "boards/common/sensor/sc7a20h.cc"
        "boards/metalio-e-ink4-plus/periph/metalio_accel.cc"
    EMBED_FILES
        "assets/embedded/bg_shutdown_en.a2i1"
        "assets/embedded/bg_shutdown_zh.a2i1"
        "factory-test-assets/factory_test_audio.ogg"
        "boards/metalio-e-ink4-plus/display/firmware/ft6336u_v03_app.bin"
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
        app_update spi_flash esp_psram esp_wifi esp_netif esp_http_client esp_http_server
        mbedtls esp_timer efuse espcoredump 78__uart-uhci espressif__iot_eth esp_driver_cam esp_driver_i2c
    PRIV_REQUIRES
        esp-wifi-connect esp-ml307 espressif__cjson 78__esp-opus-encoder 78__esp-opus
)

if(CONFIG_IDF_TARGET_ESP32S31)
    include("${CMAKE_CURRENT_LIST_DIR}/boards/common/power_policy/s31_sleep/s31_sleep_sdk.cmake")
endif()

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

# All application and library FatFS admissions participate in standby draining.
foreach(_sd_fn f_open f_close f_opendir f_closedir f_stat f_mkdir f_unlink f_rename f_utime f_getfree f_mount
    _open_r _close_r _stat_r _unlink_r _rename_r _link_r mkdir rmdir access utime truncate opendir closedir esp_vfs_fat_info)
    target_link_options(${COMPONENT_LIB} INTERFACE "-Wl,--wrap=${_sd_fn}")
endforeach()

# UI objects never require DMA; keep even tiny labels/events out of DMA SRAM.
target_link_options(${COMPONENT_LIB} INTERFACE
    "-Wl,--wrap=lv_malloc_core" "-Wl,--wrap=lv_realloc_core")

target_compile_definitions(${COMPONENT_LIB} PRIVATE
    BOARD_TYPE="metalio-e-ink4-plus"
    BOARD_NAME="metalio-e-ink4-plus"
    LV_LVGL_H_INCLUDE_SIMPLE
)

# I1 is the default fast UI path; native L8 is used only for opted-in content.
# Keep both blend kernels optimized, with the original binary fill/copy helpers.
foreach(_lvgl_comp lvgl__lvgl lvgl)
    idf_component_get_property(_lvgl_lib ${_lvgl_comp} COMPONENT_LIB)
    if(_lvgl_lib)
        idf_component_get_property(_lvgl_dir ${_lvgl_comp} COMPONENT_DIR)
        set_property(SOURCE "${_lvgl_dir}/src/draw/sw/blend/lv_draw_sw_blend_to_l8.c"
            TARGET_DIRECTORY ${_lvgl_lib} APPEND PROPERTY COMPILE_OPTIONS -O2)
        set_property(SOURCE "${_lvgl_dir}/src/draw/sw/blend/lv_draw_sw_blend_to_i1.c"
            TARGET_DIRECTORY ${_lvgl_lib} APPEND PROPERTY COMPILE_OPTIONS
            -O2 -include "${CMAKE_CURRENT_SOURCE_DIR}/display/epd_i1_solid_fill.h")
        break()
    endif()
endforeach()

# Offline expansion avoids repeated PNG inflate and decoder allocations at runtime.
# Immutable pixels stay in application Flash; no RAM image cache is introduced.
file(GLOB _home_icon_pngs CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/xingzhi-assets/ic_app_home_*.png")
file(GLOB _home_icon_overrides CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/assets/embedded/ic_app_home_*.png")
set(_pixel_icon_dir "${CMAKE_CURRENT_SOURCE_DIR}/assets/embedded/pixel")
file(GLOB _pixel_icon_pngs CONFIGURE_DEPENDS "${_pixel_icon_dir}/ic_px_*.png")
set(_home_icon_pixels "${CMAKE_CURRENT_BINARY_DIR}/home_icon_pixels.cc")
add_custom_command(OUTPUT "${_home_icon_pixels}"
    COMMAND ${PYTHON} "${CMAKE_CURRENT_SOURCE_DIR}/../tools/generate_home_icon_pixels.py"
        --assets "${CMAKE_CURRENT_SOURCE_DIR}/xingzhi-assets" --output "${_home_icon_pixels}"
        --override-dir "${CMAKE_CURRENT_SOURCE_DIR}/assets/embedded"
        --pixel-dir "${_pixel_icon_dir}"
    DEPENDS ${_home_icon_pngs} ${_home_icon_overrides} ${_pixel_icon_pngs} "${CMAKE_CURRENT_SOURCE_DIR}/../tools/generate_home_icon_pixels.py"
        "${CMAKE_CURRENT_SOURCE_DIR}/xingzhi-assets" "${CMAKE_CURRENT_SOURCE_DIR}/assets/embedded" "${_pixel_icon_dir}"
    VERBATIM)
target_sources(${COMPONENT_LIB} PRIVATE "${_home_icon_pixels}" "${CMAKE_CURRENT_SOURCE_DIR}/display/home_icon_decoder.cc")

set(_quick_icon_dir "${CMAKE_CURRENT_SOURCE_DIR}/assets/embedded/quick_controls")
set(_quick_icon_pixels "${CMAKE_CURRENT_BINARY_DIR}/quick_control_icons.cc")
add_custom_command(OUTPUT "${_quick_icon_pixels}"
    COMMAND ${PYTHON} "${PROJECT_DIR}/tools/generate_quick_control_icons.py"
        --assets "${_quick_icon_dir}" --output "${_quick_icon_pixels}"
    DEPENDS "${PROJECT_DIR}/tools/generate_quick_control_icons.py"
        "${_quick_icon_dir}/cat_paw.png" "${_quick_icon_dir}/light.png" "${_quick_icon_dir}/color_temp.png"
    VERBATIM)
target_sources(${COMPONENT_LIB} PRIVATE "${_quick_icon_pixels}")

spiffs_create_partition_assets(
    resources
    "./xingzhi-assets"
    FLASH_IN_PROJECT
    MMAP_FILE_SUPPORT_FORMAT ".jpg,.png,.eaf,.a2i1"
    MMAP_SUPPORT_SJPG
    MMAP_SUPPORT_SPNG
    MMAP_SPLIT_HEIGHT 16
    DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/xingzhi-assets"
)

# 设备页面压缩并嵌入 Flash。
add_custom_command(OUTPUT "${AP_FILEMAN_GZ}"
    COMMAND "${PYTHON}" "${PROJECT_DIR}/scripts/pack_ap_fileman.py"
        "${CMAKE_CURRENT_SOURCE_DIR}/display/screen/cloud_screen/ap/fileman.html"
        "${AP_FILEMAN_GZ}"
    DEPENDS "${PROJECT_DIR}/scripts/pack_ap_fileman.py"
        "${CMAKE_CURRENT_SOURCE_DIR}/display/screen/cloud_screen/ap/fileman.html"
        "${CMAKE_CURRENT_SOURCE_DIR}/display/screen/cloud_screen/ap/cloudzao-logo.png"
    VERBATIM)
target_add_binary_data(${COMPONENT_LIB} "${AP_FILEMAN_GZ}" BINARY)
