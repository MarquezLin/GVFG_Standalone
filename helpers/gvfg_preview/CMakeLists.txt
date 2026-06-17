project(gvfg_preview LANGUAGES CXX)

add_library(gvfg_preview SHARED
    include/gvfg_preview.h
    src/d3d_preview_pipeline.cpp
    src/d3d_preview_pipeline.h
    src/gvfg_preview.cpp
)

set_target_properties(gvfg_preview PROPERTIES
    AUTOMOC OFF
    AUTOUIC OFF
    AUTORCC OFF
)

target_include_directories(gvfg_preview
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/include
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src
)

target_compile_definitions(gvfg_preview PRIVATE
    GVFG_PREVIEW_BUILD
)

target_link_libraries(gvfg_preview
    PUBLIC
    gvfg
    PRIVATE
    user32
    dxgi
    d3d11
    D3DCompiler
)

if (MSVC)
  target_compile_options(gvfg_preview PRIVATE /utf-8 /EHsc)
endif()
