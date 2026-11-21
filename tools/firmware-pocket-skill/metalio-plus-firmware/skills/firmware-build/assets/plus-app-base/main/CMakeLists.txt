idf_component_register(
    SRCS "main.cc" "generated/body/font.cc" "plus_pocket_bridge.cc" "pocket/pocket_core.cc"
         "pocket/pocket_handoff.cc" "pocket/pocket_image.cc" "pocket/pocket_metadata.cc"
    INCLUDE_DIRS "." "pocket" "${CMAKE_BINARY_DIR}/pocket_generated"
    REQUIRES plus_hal plus_font app_update esp_app_format esp_partition nvs_flash
             fatfs sdmmc mbedtls esp_timer esp_psram
    EMBED_FILES "assets/app_lockscreen.bin" "assets/app_deep_sleep.bin")
target_link_libraries(${COMPONENT_LIB} INTERFACE "-u pocket_compact_metadata"
    "-u _binary_app_lockscreen_bin_start" "-u _binary_app_deep_sleep_bin_start")
target_compile_options(${COMPONENT_LIB} PRIVATE -Wall -Wextra -fstack-usage)
