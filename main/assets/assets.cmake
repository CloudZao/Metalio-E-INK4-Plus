# 应用资源：图标生成、镜像打包及网页嵌入。由main组件注册后加载。
set(AP_FILEMAN_GZ "${CMAKE_CURRENT_BINARY_DIR}/ap_fileman.html.gz")
set(AP_FILEMAN_JS_GZ "${CMAKE_CURRENT_BINARY_DIR}/ap_fileman.js.gz")
set(AP_FILEMAN_EN_GZ "${CMAKE_CURRENT_BINARY_DIR}/ap_fileman_en.html.gz")

# Lossless icon tiles keep source pixels in Flash; decode buffers are session-local.
file(GLOB _home_icon_pngs CONFIGURE_DEPENDS "${BOARD_ASSET_DIR}/ic_app_home_*.png"
    "${BOARD_ASSET_DIR}/ic_s_standby_calendar.png")
file(GLOB _home_icon_overrides CONFIGURE_DEPENDS "${PROJECT_DIR}/main/assets/embedded/ic_app_home_*.png")
set(_pixel_icon_dir "${BOARD_PIXEL_DIR}")
file(GLOB _pixel_icon_pngs CONFIGURE_DEPENDS "${_pixel_icon_dir}/ic_px_*.png")
set(_home_icon_pixels "${CMAKE_CURRENT_BINARY_DIR}/home_icon_pixels.cc")
add_custom_command(OUTPUT "${_home_icon_pixels}"
    COMMAND ${PYTHON} "${CMAKE_CURRENT_SOURCE_DIR}/../tools/assets/generate_home_icon_pixels.py"
        --assets "${BOARD_ASSET_DIR}" --output "${_home_icon_pixels}"
        --override-dir "${PROJECT_DIR}/main/assets/embedded"
        --pixel-dir "${_pixel_icon_dir}"
    DEPENDS ${_home_icon_pngs} ${_home_icon_overrides} ${_pixel_icon_pngs} "${CMAKE_CURRENT_SOURCE_DIR}/../tools/assets/generate_home_icon_pixels.py"
        "${BOARD_ASSET_DIR}" "${PROJECT_DIR}/main/assets/embedded" "${_pixel_icon_dir}"
    VERBATIM)
target_sources(${COMPONENT_LIB} PRIVATE "${_home_icon_pixels}" "${CMAKE_CURRENT_SOURCE_DIR}/ui/common/drawing/rendering/home_icon_decoder.cc")

target_add_binary_data(${COMPONENT_LIB} "${PROJECT_DIR}/main/assets/embedded/margin_calibration.png" BINARY)

foreach(_shutdown_lang en zh)
    set(_shutdown_source "${PROJECT_DIR}/main/assets/embedded/bg_shutdown_${_shutdown_lang}.a2i1")
    set(_shutdown_packed "${CMAKE_CURRENT_BINARY_DIR}/bg_shutdown_${_shutdown_lang}.a2i1.z")
    add_custom_command(OUTPUT "${_shutdown_packed}"
        COMMAND ${PYTHON} "${PROJECT_DIR}/tools/assets/compress_binary.py" "${_shutdown_source}" "${_shutdown_packed}"
        DEPENDS "${_shutdown_source}" "${PROJECT_DIR}/tools/assets/compress_binary.py"
        VERBATIM)
    target_add_binary_data(${COMPONENT_LIB} "${_shutdown_packed}" BINARY)
endforeach()

set(_quick_icon_dir "${PROJECT_DIR}/main/assets/embedded/quick_controls")
set(_quick_icon_pixels "${CMAKE_CURRENT_BINARY_DIR}/quick_control_icons.cc")
add_custom_command(OUTPUT "${_quick_icon_pixels}"
    COMMAND ${PYTHON} "${PROJECT_DIR}/tools/assets/generate_quick_control_icons.py"
        --assets "${_quick_icon_dir}" --output "${_quick_icon_pixels}"
    DEPENDS "${PROJECT_DIR}/tools/assets/generate_quick_control_icons.py"
        "${_quick_icon_dir}/cat_paw.png" "${_quick_icon_dir}/light.png" "${_quick_icon_dir}/color_temp.png"
    VERBATIM)
target_sources(${COMPONENT_LIB} PRIVATE "${_quick_icon_pixels}")

set(_resource_include_dir "${CMAKE_CURRENT_BINARY_DIR}/generated")

spiffs_create_partition_assets(
    resources
    "${BOARD_ASSET_DIR}"
    IMPORT_INC_PATH "${_resource_include_dir}"
    FLASH_IN_PROJECT
    MMAP_FILE_SUPPORT_FORMAT ".jpg,.png,.eaf,.a2i1"
    MMAP_SUPPORT_SJPG
    MMAP_SUPPORT_SPNG
    MMAP_SPLIT_HEIGHT 16
    DEPENDS "${BOARD_ASSET_DIR}"
)
# 资源打包会同时生成索引头，须先完成再编译引用它的源码。
add_dependencies(${COMPONENT_LIB} assets_resources_bin)

# 设备页面压缩并嵌入 Flash。
add_custom_command(OUTPUT "${AP_FILEMAN_GZ}" "${AP_FILEMAN_EN_GZ}" "${AP_FILEMAN_JS_GZ}"
    COMMAND "${PYTHON}" "${PROJECT_DIR}/tools/assets/pack_ap_fileman.py"
        "${CMAKE_CURRENT_SOURCE_DIR}/ui/apps/cloud/ap/fileman.html"
        "${CMAKE_CURRENT_BINARY_DIR}"
        "${CMAKE_CURRENT_SOURCE_DIR}/ui/apps/cloud/ap/fileman.en.json"
    DEPENDS "${PROJECT_DIR}/tools/assets/pack_ap_fileman.py"
        "${CMAKE_CURRENT_SOURCE_DIR}/ui/apps/cloud/ap/fileman.html"
        "${CMAKE_CURRENT_SOURCE_DIR}/ui/apps/cloud/ap/fileman.en.json"
    VERBATIM)

target_add_binary_data(${COMPONENT_LIB} "${AP_FILEMAN_GZ}" BINARY)
target_add_binary_data(${COMPONENT_LIB} "${AP_FILEMAN_EN_GZ}" BINARY)
target_add_binary_data(${COMPONENT_LIB} "${AP_FILEMAN_JS_GZ}" BINARY)

# 中英文页面共用同一份标识图片。
target_add_binary_data(${COMPONENT_LIB} "${CMAKE_CURRENT_SOURCE_DIR}/ui/apps/cloud/ap/cloudzao-logo.png" BINARY)
