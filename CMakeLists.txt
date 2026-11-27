# For more information about build system see
# https://docs.espressif.com/projects/esp-idf/en/latest/api-guides/build-system.html
cmake_minimum_required(VERSION 3.16)

set(PROJECT_VER "2.0.85")

add_compile_options(-Wno-missing-field-initializers)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
# 首次配置在 Kconfig 求值之前加载默认值；已有 sdkconfig 始终保留用户选择。
if(NOT SDKCONFIG_DEFAULTS)
    if(NOT IDF_TARGET)
        set(_config_path "${SDKCONFIG}")
        if(NOT _config_path)
            set(_config_path "${CMAKE_CURRENT_LIST_DIR}/sdkconfig")
        endif()
        if(EXISTS "${_config_path}")
            file(STRINGS "${_config_path}" _target_line REGEX "^CONFIG_IDF_TARGET=\"")
            string(REGEX REPLACE "^CONFIG_IDF_TARGET=\"([^\"]+)\"$" "\\1" IDF_TARGET "${_target_line}")
        elseif(DEFINED ENV{IDF_TARGET})
            set(IDF_TARGET "$ENV{IDF_TARGET}")
        else()
            set(IDF_TARGET esp32s31)
        endif()
    endif()
    set(SDKCONFIG_DEFAULTS "${CMAKE_CURRENT_LIST_DIR}/sdkconfig.defaults")
    if(IDF_TARGET STREQUAL "esp32s3")
        list(APPEND SDKCONFIG_DEFAULTS "${CMAKE_CURRENT_LIST_DIR}/main/boards/metalio-e-ink-4/sdkconfig.defaults")
    elseif(IDF_TARGET STREQUAL "esp32s31")
        list(APPEND SDKCONFIG_DEFAULTS "${CMAKE_CURRENT_LIST_DIR}/main/boards/metalio-e-ink4-plus/sdkconfig.defaults")
    else()
        message(FATAL_ERROR "Unsupported hardware target: ${IDF_TARGET}")
    endif()
endif()
if(IDF_TARGET STREQUAL "esp32s3")
    project(Metalio-E-Ink4)
else()
    project(Metalio-E-Ink4-Plus)
endif()

# 各板共用自有 main 的跨文件优化，SDK 与第三方库保持原编译策略。
idf_component_get_property(_main_lib main COMPONENT_LIB)
set_property(TARGET ${_main_lib} PROPERTY INTERPROCEDURAL_OPTIMIZATION TRUE)
idf_build_get_property(_app_elf EXECUTABLE)
get_target_property(_app_link_options ${_app_elf} LINK_OPTIONS)
list(REMOVE_ITEM _app_link_options "-fno-lto")
set_property(TARGET ${_app_elf} PROPERTY LINK_OPTIONS "${_app_link_options}")
set_property(TARGET ${_app_elf} PROPERTY INTERPROCEDURAL_OPTIMIZATION TRUE)

# 随 app 一并烧录 fontpack 到 font_data（替换 epd_font24）
set(FONTPACK_FILE "${XINGZHI_BOARD_FONT_FILE}")
if(EXISTS "${FONTPACK_FILE}")
    esptool_py_flash_to_partition(flash font_data "${FONTPACK_FILE}")
    message(STATUS "font_data flash: ${FONTPACK_FILE}")
else()
    message(WARNING "main/assets/fonts/font.fontpack missing — font_data will not be flashed")
endif()

if(CONFIG_DISPLAY_BACKEND_SSD1677)
    set(_uvc_fix "${PROJECT_SOURCE_DIR}/patches/usb_host_uvc")
    idf_component_get_property(_uvc_dir espressif__usb_host_uvc COMPONENT_DIR)
    configure_file("${_uvc_fix}/uvc_descriptor_parsing.c" "${_uvc_dir}/uvc_descriptor_parsing.c" COPYONLY)
    configure_file("${_uvc_fix}/uvc_idf_version_priv.h" "${_uvc_dir}/private_include/uvc_idf_version_priv.h" COPYONLY)
    get_filename_component(_asset_name "${XINGZHI_BOARD_ASSET_DIR}" NAME)
    add_custom_target(attach_ota_resources ALL
        COMMAND ${PYTHON} "${PROJECT_SOURCE_DIR}/tools/assets/attach_upgrade_resources.py"
            --app "${CMAKE_BINARY_DIR}/${CMAKE_PROJECT_NAME}.bin"
            --resources "${CMAKE_BINARY_DIR}/mmap_build/${_asset_name}/resources/resources.bin"
        VERBATIM)
    add_dependencies(attach_ota_resources gen_project_binary app_check_size assets_resources_bin)
    if(TARGET flash)
        add_dependencies(flash attach_ota_resources)
    endif()
endif()
