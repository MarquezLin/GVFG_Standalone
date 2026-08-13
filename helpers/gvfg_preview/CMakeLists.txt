project(gvfg_preview LANGUAGES CXX)

add_library(gvfg_preview SHARED
    include/gvfg_preview.h
    ../../sdk/gvfg/src/gpu/d3d_conversion_pipeline.cpp
    ../../sdk/gvfg/src/gpu/d3d_conversion_pipeline.h
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
        ${CMAKE_CURRENT_SOURCE_DIR}/../../sdk/gvfg/src/gpu
)

target_compile_definitions(gvfg_preview PRIVATE
    GVFG_PREVIEW_BUILD
    GVFG_INTERNAL_DIAGNOSTICS=$<IF:$<CONFIG:Debug>,1,0>
)

target_link_libraries(gvfg_preview
    PRIVATE
    user32
    dxgi
    d3d11
    D3DCompiler
)

if (MSVC)
  target_compile_options(gvfg_preview PRIVATE /utf-8 /EHsc)
endif()

install(TARGETS gvfg_preview
  RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
  ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
)

install(FILES include/gvfg_preview.h
  DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
)
