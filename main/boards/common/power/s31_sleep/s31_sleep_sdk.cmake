# All S31 sleep integration is owned by this module.
target_sources(${COMPONENT_LIB} PRIVATE "${CMAKE_CURRENT_LIST_DIR}/s31_sleep.cc")
target_link_options(${COMPONENT_LIB} INTERFACE
    "-Wl,--wrap=esp_sleep_execute_event_callbacks"
    "-Wl,--wrap=pmu_sleep_start"
    "-Wl,--wrap=rtc_clk_mpll_configure"
)

# Backport the S31 PMU PSRAM clock handoff used by current ESP-IDF.
# Keep the installed SDK untouched and compile a project-local source copy.
# https://github.com/espressif/esp-idf/blob/master/components/esp_hw_support/port/esp32s31/pmu_sleep.c
set(_s31_pmu_path "$ENV{IDF_PATH}/components/esp_hw_support/port/esp32s31/pmu_sleep.c")
file(READ "${_s31_pmu_path}" _s31_pmu)
if(NOT _s31_pmu MATCHES "_psram_ctrlr_ll_select_clk_source")
    set(_s31_entry "s_mpll_freq_mhz_before_sleep = rtc_clk_mpll_get_freq();")
    set(_s31_entry_fix_body [=[
        if (s_mpll_freq_mhz_before_sleep) {
#if !BOOTLOADER_BUILD && CONFIG_SPIRAM
            _psram_ctrlr_ll_select_clk_source(PSRAM_CTRLR_LL_MSPI_ID_2, PSRAM_CLK_SRC_XTAL);
            _psram_ctrlr_ll_select_clk_source(PSRAM_CTRLR_LL_MSPI_ID_3, PSRAM_CLK_SRC_XTAL);
            if (!s_pmu_sleep_regdma_backup_enabled) {
                _psram_ctrlr_ll_enable_core_clock(PSRAM_CTRLR_LL_MSPI_ID_2, false);
                _psram_ctrlr_ll_enable_module_clock(PSRAM_CTRLR_LL_MSPI_ID_2, false);
            }
#endif
            rtc_clk_mpll_disable();
        }
]=])
    set(_s31_entry_fix "${_s31_entry}\n${_s31_entry_fix_body}")
    set(_s31_exit "rtc_clk_mpll_configure(clk_hal_xtal_get_freq_mhz(), s_mpll_freq_mhz_before_sleep, false);")
    set(_s31_exit_fix_body [=[
#if !BOOTLOADER_BUILD && CONFIG_SPIRAM
            if (!s_pmu_sleep_regdma_backup_enabled) {
                _psram_ctrlr_ll_enable_core_clock(PSRAM_CTRLR_LL_MSPI_ID_2, true);
                _psram_ctrlr_ll_enable_module_clock(PSRAM_CTRLR_LL_MSPI_ID_2, true);
            }
            _psram_ctrlr_ll_select_clk_source(PSRAM_CTRLR_LL_MSPI_ID_2, PSRAM_CLK_SRC_MPLL);
            _psram_ctrlr_ll_select_clk_source(PSRAM_CTRLR_LL_MSPI_ID_3, PSRAM_CLK_SRC_MPLL);
#endif
]=])
    set(_s31_exit_fix "${_s31_exit}\n${_s31_exit_fix_body}")
    string(FIND "${_s31_pmu}" "${_s31_entry}" _s31_entry_at)
    string(FIND "${_s31_pmu}" "${_s31_exit}" _s31_exit_at)
    if(_s31_entry_at LESS 0 OR _s31_exit_at LESS 0)
        message(FATAL_ERROR "S31 PMU source changed: review the sleep clock handoff backport")
    endif()
    string(REPLACE "${_s31_entry}" "${_s31_entry_fix}" _s31_pmu "${_s31_pmu}")
    string(REPLACE "${_s31_exit}" "${_s31_exit_fix}" _s31_pmu "${_s31_pmu}")
    file(WRITE "${CMAKE_BINARY_DIR}/s31_pmu_sleep_compat.c" "${_s31_pmu}")
    get_target_property(_s31_sources __idf_esp_hw_support SOURCES)
    list(FILTER _s31_sources EXCLUDE REGEX "port/esp32s31/pmu_sleep[.]c$")
    set_property(TARGET __idf_esp_hw_support PROPERTY SOURCES "${_s31_sources}")
    target_sources(__idf_esp_hw_support PRIVATE "${CMAKE_BINARY_DIR}/s31_pmu_sleep_compat.c")
endif()
