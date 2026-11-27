# 公共应用构建：按Kconfig选择板型、后端及资源
# 硬件配置来自 Kconfig；提前依赖扫描时只使用目标芯片确定依赖集合。
if(CMAKE_BUILD_EARLY_EXPANSION)
    if(IDF_TARGET STREQUAL "esp32s3")
        set(_board_ssd1677 TRUE)
    elseif(IDF_TARGET STREQUAL "esp32s31")
        set(_board_ssd1677 FALSE)
    else()
        message(FATAL_ERROR "Unsupported hardware target: ${IDF_TARGET}")
    endif()
elseif(CONFIG_BOARD_TYPE_METALIO_E_INK_4 OR CONFIG_BOARD_TYPE_METALIO_E_INK_4_GLOBAL)
    set(_board_ssd1677 TRUE)
elseif(CONFIG_BOARD_TYPE_METALIO_E_INK4_PLUS OR CONFIG_BOARD_TYPE_METALIO_E_INK4_PLUS_GLOBAL)
    set(_board_ssd1677 FALSE)
else()
    message(FATAL_ERROR "Select a supported hardware board in Kconfig")
endif()

# 所有板型共用的实现，每份源码只编译一次。
set(BOARD_COMMON_SRCS
    "boards/common/audio/bluetooth/bt_audio_codec.cc"
    "boards/common/audio/bluetooth/bluetooth_audio.cc"
    "boards/common/bluetooth/ble_page_turner.cc"
    "boards/common/board/board.cc"
    "boards/common/board/dual_network_board.cc"
    "boards/common/board/nt26_board.cc"
    "boards/common/board/uart_eth_modem.cc"
    "boards/common/board/wifi_board.cc"
    "boards/common/bus/flash_access.cc"
    "boards/common/bus/i2c/i2c_device.cc"
    "boards/common/bus/i2c/power_i2c.c"
    "boards/common/display/backlight.cc"
    "boards/common/display/frontlight.cc"
    "boards/common/haptics/vibration.cc"
    "boards/common/input/input.cc"
    "boards/common/monitor/system_monitor.cc"
    "boards/common/power/bq27220/bq27220_gauge.cc"
    "boards/common/power/cx25601n/cx25601n.cc"
    "boards/common/power/cx25601n/charger_control.cc"
    "boards/common/power/power_hw.cc"
    "sysmgmt/power/policy/power_policy.cc"
    "sysmgmt/power/policy/power_policy_standby_lp.cc"
    "boards/common/rtc/pcf8563/pcf8563.cc"
    "boards/common/sensor/sc7a20h/accelerometer.cc"
    "boards/common/sensor/sc7a20h/sc7a20h.cc"
    "boards/common/storage/SdCardManager.cc"
    "boards/common/storage/sd_access.cc"
    "boards/common/storage/sd_directory_page.cc"
    "boards/common/storage/sd_log_writer.cc"
    "boards/common/storage/usb_virtual_disk.cc"
    "boards/common/storage/wifi_config_storage.cc"
)

set(BOARD_INCLUDE_DIRS
    "boards/common/audio/bluetooth"
    "boards/common/bluetooth"
    "boards/common/board"
    "boards/common/bus"
    "boards/common/bus/i2c"
    "boards/common/camera"
    "boards/common/display"
    "boards/common/haptics"
    "boards/common/input"
    "boards/common/input/buttons"
    "boards/common/io"
    "boards/common/monitor"
    "boards/common/power"
    "boards/common/power/bq27220"
    "boards/common/power/cx25601n"
    "boards/common/power/topology"
    "sysmgmt/power/policy"
    "boards/common/power/s31_sleep"
    "boards/common/rtc/pcf8563"
    "boards/common/sensor/sc7a20h"
    "boards/common/storage"
    "boards/common/storage/sdmmc"
)
set(BOARD_FONT_FILE "${PROJECT_DIR}/main/assets/fonts/font.fontpack")
set(BOARD_EMBED_FILES "")

if(_board_ssd1677)
    # 397：SPI 显示、CST816S 触摸与 USB UVC 摄像头。
    set(BOARD_TARGET esp32s3)
    set(BOARD_TYPE metalio-e-ink-4)
    set(BOARD_CONFIG_DIR "${CMAKE_CURRENT_LIST_DIR}/boards/metalio-e-ink-4")
    set(BOARD_SRCS
        "boards/common/bus/i2c/peripheral_i2c_bus.c"
        "boards/common/bus/i2c/i2c_device_adapters.c"
        "boards/common/display/ssd1677/epd_gray_aa.cc"
        "boards/common/display/ssd1677/esp_lcd_panel_ssd1677.c"
        "ui/common/drawing/adapter/ssd1677/lv_adapter_ssd1677.cc"
        "ui/common/drawing/adapter/ssd1677/display_setup.cc"
        "boards/common/display/ssd1677/epd_panel.cc"
        "boards/common/input/buttons/gpio/gpio_button_input.cc"
        "boards/common/input/touch/cst816s/touch_input.c"
        "boards/common/io/tca95xx/io_expander_setup.cc"
        "boards/common/power/topology/eink4_power_hw.cc"
        "infra/ota/resources_upgrade.cc"
        "boards/metalio-e-ink-4/metalio_e_ink_4_board.cc"
    )
    list(APPEND BOARD_INCLUDE_DIRS
        "boards/common/camera/uvc"
        "boards/common/display/ssd1677"
        "ui/common/drawing/adapter/ssd1677"
        "boards/common/display/ssd1677/lut"
        "boards/common/input/buttons/gpio"
        "boards/common/input/touch/cst816s"
        "boards/common/io/tca95xx"
        "boards/metalio-e-ink-4"
    )
    list(APPEND BOARD_SRCS
        boards/common/camera/uvc/usb_uvc_still.cc
        boards/common/camera/uvc/uvc_still_camera.cc
        boards/common/camera/uvc/capture_adapter.cc
    )
    list(APPEND BOARD_SRCS
        boards/common/input/buttons/button.cc
    )
    set(BOARD_REQUIRES
        esp_driver_spi
        esp_lvgl_port
        esp_lcd_touch
        esp_lcd_touch_cst816s
        esp_io_expander_tca95xx_16bit
        usb_host_uvc
        usb
    )
    # 资源清单限定打包范围；仅差异图片保留在 compact，公共图片按名称复用。
    set(_compact_dir "${PROJECT_DIR}/main/assets/profiles/compact")
    file(STRINGS "${_compact_dir}/resources.list" _compact_resources)
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${_compact_dir}/resources.list")
    set(BOARD_ASSET_DIR "${CMAKE_CURRENT_BINARY_DIR}/board_assets/device")
    if(NOT CMAKE_BUILD_EARLY_EXPANSION)
        file(GLOB _compact_overrides CONFIGURE_DEPENDS "${_compact_dir}/device/*")
        file(MAKE_DIRECTORY "${BOARD_ASSET_DIR}")
        file(GLOB _old_resources "${BOARD_ASSET_DIR}/*")
        foreach(_old IN LISTS _old_resources)
            get_filename_component(_name "${_old}" NAME)
            if(NOT _name IN_LIST _compact_resources)
                file(REMOVE "${_old}")
            endif()
        endforeach()
        foreach(_name IN LISTS _compact_resources)
            if(NOT _name MATCHES "^[A-Za-z0-9_.-]+$" OR _name MATCHES "\\.\\.")
                message(FATAL_ERROR "Invalid resource filename in compact resource list")
            endif()
            if(EXISTS "${_compact_dir}/device/${_name}")
                set(_source "${_compact_dir}/device/${_name}")
            else()
                set(_source "${PROJECT_DIR}/main/assets/device/${_name}")
            endif()
            configure_file("${_source}" "${BOARD_ASSET_DIR}/${_name}" COPYONLY)
        endforeach()
    endif()
    set(BOARD_PIXEL_DIR "${BOARD_ASSET_DIR}")
    set(BOARD_I1_HEADER "${CMAKE_CURRENT_SOURCE_DIR}/boards/common/display/ssd1677/epd_i1_glyph_thin.h")

else()
    # 470：并行显示、FT6336U 触摸与 DVP 摄像头。
    set(BOARD_TARGET esp32s31)
    set(BOARD_TYPE metalio-e-ink4-plus)
    set(BOARD_CONFIG_DIR "${CMAKE_CURRENT_LIST_DIR}/boards/metalio-e-ink4-plus")
    set(BOARD_SRCS
        "boards/common/bus/i2c/shared_i2c_bus.c"
        "boards/common/display/epdiy/panel/board_specific.c"
        "boards/common/display/epdiy/builtin_waveforms.c"
        "boards/common/display/epdiy/displays.c"
        "boards/common/display/epdiy/panel/epd_board.c"
        "boards/common/display/epdiy/panel/epd_board_common.c"
        "boards/common/display/epdiy/panel/epd_board_adapter.c"
        "boards/common/display/epdiy/epdiy.c"
        "boards/common/display/epdiy/epdiy_riscv_ve_stub.c"
        "boards/common/display/epdiy/font.c"
        "boards/common/display/epdiy/highlevel.c"
        "ui/common/drawing/adapter/epdiy/display_setup.cc"
        "boards/common/display/epdiy/epd_panel.c"
        "boards/common/display/epdiy/output_common/line_queue.c"
        "boards/common/display/epdiy/output_common/lut.c"
        "boards/common/display/epdiy/output_common/render_context.c"
        "boards/common/display/epdiy/output_common/render_method.c"
        "boards/common/display/epdiy/output_lcd/lcd_driver.c"
        "boards/common/display/epdiy/output_lcd/render_lcd.c"
        "business/diagnostics/panel_vcom.cc"
        "boards/common/display/panel_calibration_storage.cc"
        "ui/common/drawing/rendering/panel_vcom_notice.cc"
        "boards/common/display/epdiy/render.c"
        "boards/common/display/epdiy/tps65185/tps65185.c"
        "boards/common/display/epdiy/waveforms/my_waveform.c"
        "boards/common/display/epdiy/waveforms/my_waveform_trim.c"
        "boards/common/input/buttons/io_expander/expander_button_input.cc"
        "boards/common/input/touch/ft6336u/touch_input.c"
        "boards/common/input/touch/ft6336u/touch_firmware.cc"
        "boards/common/io/pca9555/pca9555.c"
        "boards/common/power/topology/eink4_plus_power_hw.cc"
        "boards/common/rtc/pcf8563/rtc_service.cc"
        "boards/common/storage/sdmmc/sdmmc_storage.cc"
        "boards/metalio-e-ink4-plus/metalio_e_ink4_plus_board.cc"
    )
    list(APPEND BOARD_INCLUDE_DIRS
        "boards/common/camera/dvp"
        "boards/common/camera/dvp/gc2145"
        "boards/common/display/epdiy"
        "ui/common/drawing/adapter/epdiy"
        "boards/common/display/epdiy/output_common"
        "boards/common/display/epdiy/output_lcd"
        "boards/common/display/epdiy/panel"
        "boards/common/display/epdiy/tps65185"
        "boards/common/display/epdiy/waveforms"
        "boards/common/input/buttons/io_expander"
        "boards/common/input/touch/ft6336u"
        "boards/common/io/pca9555"
        "boards/metalio-e-ink4-plus"
    )
    list(APPEND BOARD_SRCS
        ui/common/drawing/adapter/epdiy/lv_adapter_epdiy.cc
        ui/common/drawing/adapter/epdiy/lv_adapter_epdiy_touch.cc
        ui/common/drawing/adapter/epdiy/epd_refresh.cc
    )
    list(APPEND BOARD_SRCS
        boards/common/camera/dvp/dvp_camera.cc
        boards/common/camera/dvp/dvp_camera_adapter.cc
        boards/common/camera/dvp/capture_adapter.cc
    )
    list(APPEND BOARD_SRCS ui/common/drawing/font/epd_font24/epd_font24.c)
    set(_touch_board_config "${CMAKE_CURRENT_LIST_DIR}/boards/metalio-e-ink4-plus/config.h")
    file(STRINGS "${_touch_board_config}" _touch_upgrade_define
        REGEX "^#define BOARD_TOUCH_UPGRADE_ON_BOOT [01]([ \t]|$)")
    list(LENGTH _touch_upgrade_define _touch_upgrade_count)
    if(NOT _touch_upgrade_count EQUAL 1)
        message(FATAL_ERROR "BOARD_TOUCH_UPGRADE_ON_BOOT must be defined once as 0 or 1 in the board config.h")
    endif()
    if(NOT CMAKE_BUILD_EARLY_EXPANSION)
        set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${_touch_board_config}")
    endif()
    if(_touch_upgrade_define MATCHES "^#define BOARD_TOUCH_UPGRADE_ON_BOOT 1([ \t]|$)")
        list(APPEND BOARD_EMBED_FILES boards/common/input/touch/ft6336u/firmware/ft6336u_v04_app.bin)
    endif()
    set(BOARD_REQUIRES
        esp_driver_cam
        esp_driver_jpeg
        esp_hal_lcd
        esp_hal_i2s
        esp_hal_rmt
        esp_driver_dma
    )
    set(BOARD_ASSET_DIR "${PROJECT_DIR}/main/assets/device")
    set(BOARD_PIXEL_DIR "${PROJECT_DIR}/main/assets/embedded/pixel")
endif()
if(NOT IDF_TARGET STREQUAL BOARD_TARGET)
    message(FATAL_ERROR "Selected board requires ${BOARD_TARGET}; got ${IDF_TARGET}")
endif()
if(NOT CMAKE_BUILD_EARLY_EXPANSION)
    if((_board_ssd1677 AND NOT CONFIG_DISPLAY_BACKEND_SSD1677) OR
       (NOT _board_ssd1677 AND NOT CONFIG_DISPLAY_BACKEND_EPDIY) OR
       (CONFIG_DISPLAY_BACKEND_SSD1677 AND CONFIG_DISPLAY_BACKEND_EPDIY))
        message(FATAL_ERROR "Hardware/display backend configuration mismatch")
    endif()
endif()
if(CONFIG_PRODUCT_GLOBAL)
    if(_board_ssd1677)
        set(BOARD_TYPE metalio-e-ink4-global)
    else()
        set(BOARD_TYPE metalio-e-ink4-plus-global)
    endif()
endif()
if(NOT CMAKE_BUILD_EARLY_EXPANSION)
    file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/generated")
endif()

# 早期依赖扫描为脚本模式，普通配置时跟踪源码目录变化。
set(APP_SOURCE_GLOB_FLAGS)
if(NOT CMAKE_BUILD_EARLY_EXPANSION)
    set(APP_SOURCE_GLOB_FLAGS CONFIGURE_DEPENDS)
endif()
file(GLOB WORKBUDDY_SRCS ${APP_SOURCE_GLOB_FLAGS} "${CMAKE_CURRENT_SOURCE_DIR}/business/workbuddy/*.cc"
    "${CMAKE_CURRENT_SOURCE_DIR}/ui/apps/workbuddy/*.cc")

file(GLOB APP_STORE_SRCS ${APP_SOURCE_GLOB_FLAGS} "${CMAKE_CURRENT_SOURCE_DIR}/business/app_store/*.cc"
    "${CMAKE_CURRENT_SOURCE_DIR}/ui/apps/app_store/*.cc")

file(GLOB HOME_SCREEN_SRCS ${APP_SOURCE_GLOB_FLAGS}
    "${CMAKE_CURRENT_SOURCE_DIR}/ui/home/*.cc"
    "${CMAKE_CURRENT_SOURCE_DIR}/ui/home/pixel/*.cc"
)

# Settings sources include manual log upload and its UI.
file(GLOB SETTINGS_SCREEN_SRCS ${APP_SOURCE_GLOB_FLAGS}
    "${CMAKE_CURRENT_SOURCE_DIR}/ui/apps/settings/pager/*.cc"
    "${CMAKE_CURRENT_SOURCE_DIR}/ui/apps/settings/*.cc"
    "${CMAKE_CURRENT_SOURCE_DIR}/ui/apps/settings/settings_test/*.cc"
)

file(GLOB BLUETOOTH_SCREEN_SRCS ${APP_SOURCE_GLOB_FLAGS}
    "${CMAKE_CURRENT_SOURCE_DIR}/ui/apps/settings/bluetooth/*.cc"
)

file(GLOB NETWORK_SCREEN_SRCS ${APP_SOURCE_GLOB_FLAGS}
    "${CMAKE_CURRENT_SOURCE_DIR}/ui/apps/settings/network/*.cc"
)

file(GLOB ASSISTANT_SCREEN_SRCS ${APP_SOURCE_GLOB_FLAGS}
    "${CMAKE_CURRENT_SOURCE_DIR}/ui/apps/assistant/*.cc"
)

file(GLOB BOOK_SCREEN_SRCS ${APP_SOURCE_GLOB_FLAGS}
    "${CMAKE_CURRENT_SOURCE_DIR}/ui/apps/book/*.cc"
    "${CMAKE_CURRENT_SOURCE_DIR}/ui/apps/book/cover/*.cc"
    "${CMAKE_CURRENT_SOURCE_DIR}/ui/apps/book/reader/*.cc"
    "${CMAKE_CURRENT_SOURCE_DIR}/ui/apps/book/settings/*.cc"
    "${CMAKE_CURRENT_SOURCE_DIR}/ui/apps/book/shelf/*.cc"
)

file(GLOB CLOUD_SCREEN_SRCS ${APP_SOURCE_GLOB_FLAGS}
    "${CMAKE_CURRENT_SOURCE_DIR}/ui/apps/cloud/*.cc"
    "${CMAKE_CURRENT_SOURCE_DIR}/ui/apps/cloud/ap/*.cc"
    "${CMAKE_CURRENT_SOURCE_DIR}/ui/apps/cloud/usb/*.cc"
)

file(GLOB WALLPAPER_SCREEN_SRCS ${APP_SOURCE_GLOB_FLAGS}
    "${CMAKE_CURRENT_SOURCE_DIR}/ui/apps/wallpaper/*.cc"
    "${CMAKE_CURRENT_SOURCE_DIR}/ui/apps/wallpaper/preview/*.cc"
    "${CMAKE_CURRENT_SOURCE_DIR}/ui/apps/wallpaper/thumbs/*.cc"
)

file(GLOB STANDBY_SCREEN_SRCS ${APP_SOURCE_GLOB_FLAGS}
    "${CMAKE_CURRENT_SOURCE_DIR}/ui/apps/standby/*.cc"
)

file(GLOB READER_SRCS ${APP_SOURCE_GLOB_FLAGS}
    "${CMAKE_CURRENT_SOURCE_DIR}/business/reader/*.cc"
)

file(GLOB A2UI_SRCS ${APP_SOURCE_GLOB_FLAGS}
    "${CMAKE_CURRENT_SOURCE_DIR}/ui/common/drawing/a2ui/*.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/ui/common/drawing/a2ui/*.cc"
)

file(GLOB OTA_UI_SRCS ${APP_SOURCE_GLOB_FLAGS}
    "${CMAKE_CURRENT_SOURCE_DIR}/ui/apps/update/*.cc"
)

set(PROTOCOL_SRCS
    "${CMAKE_CURRENT_SOURCE_DIR}/infra/protocols/protocol.cc"
    "${CMAKE_CURRENT_SOURCE_DIR}/infra/protocols/websocket_protocol.cc"
    "${CMAKE_CURRENT_SOURCE_DIR}/infra/protocols/mqtt_protocol.cc"
)

file(GLOB FEATURE_SERVICE_SRCS ${APP_SOURCE_GLOB_FLAGS}
    "${CMAKE_CURRENT_SOURCE_DIR}/business/bluetooth/*.cc"
    "${CMAKE_CURRENT_SOURCE_DIR}/business/cloud/*.cc"
    "${CMAKE_CURRENT_SOURCE_DIR}/business/weather/*.cc"
    "${CMAKE_CURRENT_SOURCE_DIR}/business/checklist/*.cc"
    "${CMAKE_CURRENT_SOURCE_DIR}/business/haptics/*.cc"
    "${CMAKE_CURRENT_SOURCE_DIR}/business/network/*.cc"
    "${CMAKE_CURRENT_SOURCE_DIR}/business/transfer/*.cc"
    "${CMAKE_CURRENT_SOURCE_DIR}/business/wallpaper/*.cc"
)

file(GLOB UI_COMMON_SRCS ${APP_SOURCE_GLOB_FLAGS}
    "${CMAKE_CURRENT_SOURCE_DIR}/ui/common/widgets/*.cc"
    "${CMAKE_CURRENT_SOURCE_DIR}/ui/common/widgets/ime/*/*.cc"
    "${CMAKE_CURRENT_SOURCE_DIR}/ui/common/runtime/*.cc"
    "${CMAKE_CURRENT_SOURCE_DIR}/ui/common/runtime/input/*.cc"
    "${CMAKE_CURRENT_SOURCE_DIR}/ui/common/runtime/lvgl/*.cc")
unset(APP_SOURCE_GLOB_FLAGS)

idf_component_register(
    SRCS
        "app_main.cc"
        "infra/diagnostics/firmware_memory.cc"
        "ui/application_ui.cc"
        ${UI_COMMON_SRCS}
        ${FEATURE_SERVICE_SRCS}
        "business/conversation/assistant_chat_store.cc"
        "infra/network/http/http_download_file.cc"
        "infra/network/wifi_radio.cc"

        "ui/common/drawing/rendering/network_status.cc"
        "sysmgmt/application.cc"
        "sysmgmt/system_state.cc"
        "sysmgmt/system_control.cc"
        "sysmgmt/update/firmware_update.cc"
        "business/conversation/chat_activation.cc"
        "business/conversation/conversation_controller.cc"
        "business/conversation/conversation_settings.cc"
        "business/conversation/conversation_photo.cc"
        "business/conversation/conversation_protocol.cc"
        "business/conversation/conversation_question.cc"
        "business/conversation/conversation_text.cc"
        "ui/common/drawing/rendering/system_notice.cc"
        "business/conversation/activation_polling.cc"
        "sysmgmt/audio/audio_runtime.cc"
        "sysmgmt/power/power_lifecycle.cc"
        "sysmgmt/tasks/task_executor.cc"
        "infra/time/time_service.cc"
        "sysmgmt/time/network_time.cc"
        "business/diagnostics/log_upload.cc"
        "business/diagnostics/aging_test.cc"
        "business/diagnostics/cell_diagnostic.cc"
        "business/diagnostics/camera_diagnostic.cc"
        "business/diagnostics/wifi_diagnostic.cc"
        "business/mcp_tool/mcp_tools.cc"
        "business/yunxin_binding/yunxin_binding.cc"
        "infra/storage/settings.cc"
        "infra/storage/key_bindings.cc"
        "infra/storage/sd_layout.cc"
        "infra/storage/sd_data_layout.cc"
        "infra/storage/sd_file_commit.cc"
        "ui/common/runtime/haptic_feedback.cc"

        "assets/lang_home.cc"
        "infra/network/device_config_client.cc"
        "infra/ota/firmware_image.cc"
        "infra/network/time/ntp_server.cc"
        "infra/ota/firmware_ignore.cc"
        "infra/protocols/mcp/mcp_server.cc"
        "infra/platform/system_info.cc"
        "infra/network/endpoints/cloudzao_endpoints.c"
        "ui/common/drawing/display.cc"

        "ui/common/drawing/ui_theme.cc"
        "ui/common/drawing/display_orient.cc"
        "ui/common/drawing/rendering/wallpaper_frame.cc"
        "ui/common/drawing/rendering/epd_reader_aa.cc"
        "ui/common/drawing/rendering/epd_content_policy.cc"

        "ui/common/drawing/font/font_loader.c"
        "ui/common/drawing/font/fontpack_lvgl.cc"
        "ui/common/drawing/font/ui_fonts.cc"
        "ui/common/drawing/font/epdfont.cc"
        "ui/common/drawing/font/ttf_convert.cc"
        "ui/common/drawing/font/ttf_face.cc"
        "ui/common/drawing/font/ttf_runtime.cc"
        "ui/common/drawing/font/font_misans_regular_160_2.c"
        "ui/common/drawing/font/font_awesome_30_1.c"
        "infra/image/image_decode.cc"
        "infra/image/image_process.cc"
        "infra/image/image_stream.cc"
        "infra/image/image_overlay.cc"
        "infra/image/jpeg/progressive_jpeg.cc"
        "infra/image/jpeg/progressive_jpeg_paged.cc"
        "infra/image/jpeg/progressive_jpeg_storage.cc"
        "infra/storage/file_write.cc"
        "infra/storage/storage_error.cc"
        "ui/common/widgets/storage_error_text.cc"
        "infra/tasks/task_create.cc"
        "infra/image/image_geometry.cc"
        "infra/image/image_tone.cc"
        "infra/image/jpeg/jpeg_to_image.c"
        "infra/image/jpeg/jpeg_memory.cc"

        "business/conversation/vision/camera_vision.cc"

        "ui/apps/checklist/checklist_screen.cc"
        "boards/common/audio/audio_codec.cc"
        "infra/audio/audio_service.cc"
        "infra/audio/audio_service_capture.cc"

        "infra/audio/processors/pcm_audio_processor.cc"
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
        ${APP_STORE_SRCS}
        ${WORKBUDDY_SRCS}
        ${SETTINGS_SCREEN_SRCS}
        ${BLUETOOTH_SCREEN_SRCS}
        ${NETWORK_SCREEN_SRCS}
        ${BOARD_COMMON_SRCS}
        ${BOARD_SRCS}

    EMBED_FILES
        "${PROJECT_DIR}/main/assets/factory-test/factory_test_audio.ogg"
        ${BOARD_EMBED_FILES}
    INCLUDE_DIRS
        "${CMAKE_CURRENT_BINARY_DIR}/generated"
        "."
        "sysmgmt"
        "business/conversation"
        "business/conversation/yunxin"
        "business/conversation/vision"
        "infra/network/http"
        "infra/network/endpoints"
        "infra/network/time"
        "infra/storage"
        "infra/protocols/mcp"
        "boards/common/audio"
        "ui/common/drawing/adapter"
        "ui/common/drawing/rendering"
        "assets"
        "infra/audio"
        "infra/audio/processors"
        "infra/protocols"
        "business/reader"
        "ui"
        "ui/common/drawing/a2ui"
        "ui/common/drawing/font/epd_font24"
        "ui/common/drawing/font"
        "infra/image/jpeg"
        "ui/common"
        "ui/home"
        "ui/apps/settings"
        "ui/apps/settings/settings_test"
        "ui/apps/settings/bluetooth"
        "ui/apps/settings/network"
        "ui/apps/assistant"
        "ui/apps/book"
        "ui/apps/cloud"
        "ui/apps/wallpaper"
        "ui/apps/standby"
        "ui/apps/update"
        "ui/apps/checklist"
        ${BOARD_INCLUDE_DIRS}
    REQUIRES
        lvgl esp_partition fatfs nvs_flash esp_mmap_assets esp_lvgl_adapter
        app_update spi_flash esp_psram esp_wifi esp_netif esp_http_client esp_http_server
        mbedtls esp_timer efuse espcoredump 78__uart-uhci espressif__iot_eth esp_driver_i2c ${BOARD_REQUIRES}
        driver esp_driver_gpio esp_driver_i2s esp_driver_uart esp_driver_rmt esp_driver_ledc esp_driver_sdmmc esp_adc esp_lcd esp_hw_support esp_mm esp_event freertos log sdmmc
    PRIV_REQUIRES
        esp_driver_usb_serial_jtag
        bt elf_loader espressif__zlib espressif__freetype espressif__esp_new_jpeg
        esp-wifi-connect esp-ml307 espressif__cjson 78__esp-opus-encoder 78__esp-opus
)

idf_component_get_property(_wifi_connect_lib esp-wifi-connect COMPONENT_LIB)
target_include_directories(${_wifi_connect_lib} PRIVATE "${BOARD_CONFIG_DIR}")

if(CONFIG_IDF_TARGET_ESP32S31)
    include("${CMAKE_CURRENT_LIST_DIR}/boards/common/power/s31_sleep/s31_sleep_sdk.cmake")
endif()

if(TARGET freetype)
    target_compile_definitions(freetype PRIVATE
        FT_CONFIG_MODULES_H="${CMAKE_CURRENT_SOURCE_DIR}/ui/common/drawing/font/freetype_modules.h")
    # FreeType 在普通任务中执行；字形解释和栅格化可使用跳转表。
    target_compile_options(freetype PRIVATE -O2 -fjump-tables -ftree-switch-conversion)
endif()
# 设置页模拟 U 盘（TinyUSB MSC）；IDF 6.1/S31 无独立 idf::usb，依赖由 esp_tinyusb 带入
if(TARGET idf::espressif__esp_tinyusb)
    target_link_libraries(${COMPONENT_LIB} PRIVATE idf::espressif__esp_tinyusb)
endif()

if(NOT CONFIG_LV_USE_FONT_COMPRESSED)
    message(FATAL_ERROR "Enable CONFIG_LV_USE_FONT_COMPRESSED in the selected sdkconfig; the aging counter uses a compressed LVGL font.")
endif()

if(NOT CONFIG_COMPILER_CXX_EXCEPTIONS)
    message(FATAL_ERROR "Enable CONFIG_COMPILER_CXX_EXCEPTIONS in the selected sdkconfig; application recovery requires SDK exception support.")
endif()
if(CONFIG_COMPILER_CXX_EXCEPTIONS_EMG_POOL_SIZE LESS 2048)
    message(FATAL_ERROR "Set CONFIG_COMPILER_CXX_EXCEPTIONS_EMG_POOL_SIZE to at least 2048 for allocation-failure recovery.")
endif()

# All application and library FatFS admissions participate in standby draining.
foreach(_sd_fn f_open f_close f_opendir f_closedir f_stat f_mkdir f_unlink f_rename f_utime f_getfree f_mount
    _open_r _close_r _stat_r _unlink_r _rename_r _link_r mkdir rmdir access utime truncate opendir closedir esp_vfs_fat_info)
    target_link_options(${COMPONENT_LIB} INTERFACE "-Wl,--wrap=${_sd_fn}")
endforeach()

if(CONFIG_DISPLAY_BACKEND_EPDIY OR CONFIG_DISPLAY_BACKEND_SSD1677)
    target_sources(${COMPONENT_LIB} PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/boards/common/storage/nvs_access.cc")
    target_link_options(${COMPONENT_LIB} INTERFACE
        "-Wl,--wrap=lv_async_call" "-Wl,--wrap=lv_async_call_cancel")
    # Admit NVS before its SDK lock: Flash -> I2C -> NVS, including read-only access.
    foreach(_nvs_fn nvs_open nvs_open_from_partition nvs_close nvs_commit
        nvs_set_i8 nvs_set_u8 nvs_set_i16 nvs_set_u16 nvs_set_i32 nvs_set_u32 nvs_set_i64 nvs_set_u64
        nvs_set_float nvs_set_double nvs_set_str nvs_set_blob
        nvs_get_i8 nvs_get_u8 nvs_get_i16 nvs_get_u16 nvs_get_i32 nvs_get_u32 nvs_get_i64 nvs_get_u64
        nvs_get_float nvs_get_double nvs_get_str nvs_get_blob nvs_find_key
        nvs_erase_key nvs_erase_all nvs_purge_all nvs_get_stats nvs_get_used_entry_count
        nvs_entry_find nvs_entry_find_in_handle nvs_entry_next nvs_entry_info nvs_release_iterator
        nvs_flash_init nvs_flash_init_partition nvs_flash_init_partition_ptr
        nvs_flash_deinit nvs_flash_deinit_partition nvs_flash_erase nvs_flash_erase_partition
        nvs_flash_erase_partition_ptr nvs_flash_secure_init nvs_flash_secure_init_partition
        nvs_flash_generate_keys nvs_flash_read_security_cfg
        nvs_flash_generate_keys_v2 nvs_flash_read_security_cfg_v2)
        target_link_options(${COMPONENT_LIB} INTERFACE "-Wl,--wrap=${_nvs_fn}")
    endforeach()
    if(CONFIG_NVS_BDL_STACK)
        target_link_options(${COMPONENT_LIB} INTERFACE "-Wl,--wrap=nvs_flash_init_partition_bdl")
    endif()
endif()

# UI objects never require DMA; keep even tiny labels/events out of DMA SRAM.
target_link_options(${COMPONENT_LIB} INTERFACE
    "-Wl,--wrap=lv_malloc_core" "-Wl,--wrap=lv_realloc_core"
    "-Wl,--undefined=__wrap_lv_malloc_core" "-Wl,--undefined=__wrap_lv_realloc_core")

# 预编译 JPEG 库需要显式保留分配策略包装对象。
target_link_options(${COMPONENT_LIB} INTERFACE
    "-Wl,--wrap=jpeg_calloc_inner" "-Wl,--wrap=jpeg_calloc_align_inner"
    "-Wl,--undefined=__wrap_jpeg_calloc_inner")

target_compile_definitions(${COMPONENT_LIB} PRIVATE
    BOARD_TYPE="${BOARD_TYPE}"
    BOARD_NAME="${BOARD_TYPE}"
    LV_LVGL_H_INCLUDE_SIMPLE
)

if(CONFIG_FATFS_CODEPAGE_936)
    idf_component_get_property(_fatfs_dir fatfs COMPONENT_DIR)
    set(_cp936_generator "${PROJECT_DIR}/tools/firmware/generate_fatfs_cp936.py")
    set(_cp936_source "${CMAKE_CURRENT_BINARY_DIR}/fatfs_cp936.c")
    add_custom_command(OUTPUT "${_cp936_source}"
        COMMAND ${PYTHON} "${_cp936_generator}"
            --input "${_fatfs_dir}/src/ffunicode.c" --output "${_cp936_source}"
        DEPENDS "${_cp936_generator}" "${_fatfs_dir}/src/ffunicode.c"
        VERBATIM)
    target_sources(${COMPONENT_LIB} PRIVATE "${_cp936_source}")
    target_link_options(${COMPONENT_LIB} INTERFACE
        "-Wl,--wrap=ff_uni2oem" "-Wl,--wrap=ff_oem2uni")
endif()

# I1 is the default fast UI path; native L8 is used only for opted-in content.
# Keep both blend kernels optimized, with the original binary fill/copy helpers.
if(CONFIG_DISPLAY_BACKEND_EPDIY)
    set(BOARD_I1_HEADER "${CMAKE_CURRENT_SOURCE_DIR}/ui/common/drawing/rendering/epd_i1_solid_fill.h")
endif()
foreach(_lvgl_comp lvgl__lvgl lvgl)
    idf_component_get_property(_lvgl_lib ${_lvgl_comp} COMPONENT_LIB)
    if(_lvgl_lib)
        idf_component_get_property(_lvgl_dir ${_lvgl_comp} COMPONENT_DIR)
        if(CONFIG_LV_FONT_MONTSERRAT_14 AND CONFIG_LV_USE_FONT_COMPRESSED)
            set(_font_original "${_lvgl_dir}/src/font/lv_font_montserrat_14.c")
            set(_font_generated "${CMAKE_CURRENT_BINARY_DIR}/lv_font_montserrat_14.c")
            set(_font_encoder "${PROJECT_DIR}/tools/assets/compress_lvgl_font.py")
            add_custom_command(OUTPUT "${_font_generated}"
                COMMAND ${PYTHON} "${_font_encoder}"
                    --input "${_font_original}" --output "${_font_generated}"
                DEPENDS "${_font_original}" "${_font_encoder}"
                VERBATIM)
            add_custom_target(compact_builtin_font DEPENDS "${_font_generated}")
            add_dependencies(${_lvgl_lib} compact_builtin_font)
            get_target_property(_font_sources ${_lvgl_lib} SOURCES)
            list(REMOVE_ITEM _font_sources "${_font_original}")
            set_property(TARGET ${_lvgl_lib} PROPERTY SOURCES "${_font_sources};${_font_generated}")
            set_source_files_properties("${_font_generated}" TARGET_DIRECTORY ${_lvgl_lib}
                PROPERTIES GENERATED TRUE)
        endif()
        set_property(SOURCE "${_lvgl_dir}/src/draw/sw/blend/lv_draw_sw_blend_to_l8.c"
            TARGET_DIRECTORY ${_lvgl_lib} APPEND PROPERTY COMPILE_OPTIONS -O2)
        set_property(SOURCE "${_lvgl_dir}/src/draw/sw/blend/lv_draw_sw_blend_to_i1.c"
            TARGET_DIRECTORY ${_lvgl_lib} APPEND PROPERTY COMPILE_OPTIONS
            -O2 -include "${BOARD_I1_HEADER}")
        break()
    endif()
endforeach()

include("${CMAKE_CURRENT_LIST_DIR}/assets/assets.cmake")

if(CONFIG_DISPLAY_BACKEND_SSD1677)
    set(_i1_patch "${PROJECT_DIR}/patches/lvgl_i1/apply_patch.py")
    if(_lvgl_lib)
        set(_i1_original "${_lvgl_dir}/src/draw/sw/blend/lv_draw_sw_blend_to_i1.c")
        set(_i1_generated "${CMAKE_CURRENT_BINARY_DIR}/lv_draw_sw_blend_to_i1_397.c")
        add_custom_command(OUTPUT "${_i1_generated}"
            COMMAND ${PYTHON} "${_i1_patch}" --target "${_i1_original}" --output "${_i1_generated}"
            DEPENDS "${_i1_original}" "${_i1_patch}" "${BOARD_I1_HEADER}"
            VERBATIM)
        add_custom_target(epd_i1_blend_patch DEPENDS "${_i1_generated}")
        add_dependencies(${_lvgl_lib} epd_i1_blend_patch)
        get_target_property(_lvgl_sources ${_lvgl_lib} SOURCES)
        list(REMOVE_ITEM _lvgl_sources "${_i1_original}")
        set_property(TARGET ${_lvgl_lib} PROPERTY SOURCES "${_lvgl_sources};${_i1_generated}")
        set_source_files_properties("${_i1_generated}" TARGET_DIRECTORY ${_lvgl_lib}
            PROPERTIES GENERATED TRUE COMPILE_OPTIONS "-O2")
        target_include_directories(${_lvgl_lib} PRIVATE "${PROJECT_DIR}/main/boards/common/display/ssd1677")
    endif()
endif()
set(XINGZHI_BOARD_FONT_FILE "${BOARD_FONT_FILE}" CACHE INTERNAL "Selected board font pack" FORCE)
set(XINGZHI_BOARD_ASSET_DIR "${BOARD_ASSET_DIR}" CACHE INTERNAL "Selected board assets" FORCE)

# 单组件保留硬件选择；每次构建检查跨层引用和私有头边界。
add_custom_target(check_main_dependencies
    COMMAND ${PYTHON} "${PROJECT_DIR}/tools/checks/check_dependencies.py" --root "${PROJECT_DIR}"
        --compile-commands "${CMAKE_BINARY_DIR}/compile_commands.json"
    VERBATIM)
add_dependencies(${COMPONENT_LIB} check_main_dependencies)
