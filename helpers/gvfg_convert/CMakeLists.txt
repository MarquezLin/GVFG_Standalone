project(gvfg_convert LANGUAGES CXX)

add_library(gvfg_convert SHARED
    include/gvfg_convert.h
    src/gvfg_convert.cpp
)

set_target_properties(gvfg_convert PROPERTIES
    AUTOMOC OFF
    AUTOUIC OFF
    AUTORCC OFF
)

target_include_directories(gvfg_convert
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/include
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src
)

target_compile_definitions(gvfg_convert PRIVATE
    GVFG_CONVERT_BUILD
)

target_link_libraries(gvfg_convert PUBLIC
    gvfg
)

if (MSVC)
  target_compile_options(gvfg_convert PRIVATE /utf-8 /EHsc)
endif()
