# ebook-emulator 构建（原生 SDL / Emscripten WASM）

# 本仓：tools/ebook/emulator → ../../../main
set(ESP_MAIN "${CMAKE_CURRENT_SOURCE_DIR}/../../../main" CACHE PATH "ESP32 main 源码根")

if(NOT EXISTS "${ESP_MAIN}/reader/book_session.cc")
    message(FATAL_ERROR "ESP_MAIN 无效: ${ESP_MAIN}")
endif()

set(EBOOK_INCLUDES
    ${CMAKE_CURRENT_SOURCE_DIR}/src/esp_compat
    ${CMAKE_CURRENT_SOURCE_DIR}/src/font
    ${CMAKE_CURRENT_SOURCE_DIR}/src/emulator
    ${ESP_MAIN}/reader
    ${ESP_MAIN}/boards/common
    ${CMAKE_CURRENT_SOURCE_DIR}
)

set(EBOOK_READER_SRC
    ${ESP_MAIN}/reader/book_session.cc
    ${ESP_MAIN}/reader/ebook_document.cc
    ${ESP_MAIN}/reader/epub_document.cc
    ${ESP_MAIN}/reader/html_content.cc
    ${ESP_MAIN}/reader/text_encoding.cc
    ${ESP_MAIN}/reader/txt_chapter.cc
    ${ESP_MAIN}/reader/zip_reader.cc
)

set(EBOOK_FONT_SRC
    ${ESP_MAIN}/display/font/epdfont.cc
    ${CMAKE_CURRENT_SOURCE_DIR}/src/font/font_loader_emulator.c
    ${CMAKE_CURRENT_SOURCE_DIR}/src/font/fontpack_lvgl_emulator.cc
    ${CMAKE_CURRENT_SOURCE_DIR}/src/font/epdfont_emulator.cc
)

set(EBOOK_EMULATOR_SRC
    ${CMAKE_CURRENT_SOURCE_DIR}/src/emulator/book_reader_emulator.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/emulator/image_util_emulator.cc
)

function(ebook_emulator_target target_name main_src)
    add_executable(${target_name}
        ${main_src}
        ${EBOOK_EMULATOR_SRC}
        ${EBOOK_FONT_SRC}
        ${EBOOK_READER_SRC}
    )
    target_include_directories(${target_name} PRIVATE ${EBOOK_INCLUDES})
    target_compile_definitions(${target_name} PRIVATE EBOOK_EMULATOR=1)
    target_link_libraries(${target_name} lvgl_linux lvgl m z pthread)
endfunction()

# 原生 SDL 模拟器（480×800）
ebook_emulator_target(ebook_sim src/main_ebook.c)
