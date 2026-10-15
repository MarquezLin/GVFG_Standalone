project(gvfg_osd_core LANGUAGES CXX)

add_library(gvfg_osd_core STATIC
    include/gvfg_osd.h
    src/gvfg_osd.cpp
)

set_target_properties(gvfg_osd_core PROPERTIES
    AUTOMOC OFF
    AUTOUIC OFF
    AUTORCC OFF
)

target_include_directories(gvfg_osd_core
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_link_libraries(gvfg_osd_core PUBLIC
    d2d1
    dwrite
    windowscodecs
    ole32
)

if (MSVC)
  target_compile_options(gvfg_osd_core PRIVATE /utf-8 /EHsc)
endif()
