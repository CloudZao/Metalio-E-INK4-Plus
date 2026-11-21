idf_component_register(
    SRCS "plus_hal.cc" "power/power_i2c.c" "power/cx25601n.cc" "power/bq27220_gauge.cc"
    INCLUDE_DIRS "include"
    PRIV_INCLUDE_DIRS "power" "${PROJECT_DIR}/main/generated"
    REQUIRES epdiy
    PRIV_REQUIRES esp_driver_gpio esp_driver_i2c esp_driver_sdmmc esp_driver_ledc
                  esp_timer esp_system esp_psram esp_mm esp_hw_support fatfs sdmmc
)
target_compile_definitions(${COMPONENT_LIB} PRIVATE PLUS_CHARGER_INIT_ONLY=1 PLUS_APP_WAKE_MAGIC=${PLUS_APP_WAKE_MAGIC})
target_compile_options(${COMPONENT_LIB} PRIVATE -Wall -Wextra
    -Wno-missing-field-initializers -Wno-unused-function -Wno-unused-variable)
