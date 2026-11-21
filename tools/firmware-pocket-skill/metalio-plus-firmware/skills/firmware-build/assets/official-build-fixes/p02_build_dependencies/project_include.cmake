# The official resources generator emits mmap_generate_resources.h as a side
# effect. Its ALL target alone does not order compilation of the main library.
# ESP-IDF loads project_include.cmake before adding the component directories,
# so attach this authored development repair after those targets have been made.
include_guard(GLOBAL)

function(_p02_require_resource_header_before_main)
    if(NOT TARGET __idf_main)
        message(FATAL_ERROR "P02 resource ordering requires the fixed Plus main component target __idf_main")
    endif()
    if(NOT TARGET assets_resources_bin)
        message(FATAL_ERROR "P02 resource ordering requires the upstream assets_resources_bin target")
    endif()
    add_dependencies(__idf_main assets_resources_bin)
endfunction()

cmake_language(DEFER DIRECTORY "${CMAKE_SOURCE_DIR}"
    CALL _p02_require_resource_header_before_main)
