file(GLOB EPDIY_SRCS "*.c" "bus/*.c" "output_common/*.c" "output_lcd/*.c" "waveforms/*.c")
idf_component_register(SRCS ${EPDIY_SRCS} "metalio_vibe.cc"
    INCLUDE_DIRS "." "bus" "output_common" "output_lcd" "waveforms"
    REQUIRES esp_driver_gpio esp_driver_i2c esp_driver_rmt esp_driver_dma esp_driver_sdmmc
        esp_lcd esp_hal_lcd esp_hal_i2s esp_hal_rmt esp_hw_support esp_mm esp_adc esp_timer esp_psram)
target_compile_options(${COMPONENT_LIB} PRIVATE -Wno-error=format -Wno-error=unused-function -Wno-error=unused-variable)

