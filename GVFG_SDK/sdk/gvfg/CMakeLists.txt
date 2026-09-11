# Inherit the version from the standalone root project. The root project()
# declaration is the single source of truth for the public SDK version.
set(_GVFG_VERSION "${PROJECT_VERSION}")
project(gvfg VERSION ${_GVFG_VERSION} LANGUAGES CXX)

add_library(gvfg SHARED
    gvfg.def
    include/gvfg_capture.h
    include/gvfg_debug.h
    src/backend/pcies2mm/pcies2mm_backend_types.h
    src/backend/pcies2mm/pcies2mm_capture_session.cpp
    src/backend/pcies2mm/pcies2mm_capture_session.h
    src/backend/pcies2mm/pcies2mm_device.cpp
    src/backend/pcies2mm/pcies2mm_device.h
    src/backend/pcies2mm/pcies2mm_reg.h
    src/backend/pcies2mm/pcies2mm_video_format.cpp
    src/backend/pcies2mm/pcies2mm_video_format.h
    src/gpu/d3d_conversion_pipeline.cpp
    src/gpu/d3d_conversion_pipeline.h
    src/gpu/gvfg_gpu_convert.cpp
    src/gvfg_error_state.h
    src/gvfg_capture.cpp
)

set_target_properties(gvfg PROPERTIES
    AUTOMOC OFF
    AUTOUIC OFF
    AUTORCC OFF
)

target_include_directories(gvfg
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/include
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src
        ${CMAKE_CURRENT_SOURCE_DIR}/src/backend/pcies2mm
        ${CMAKE_CURRENT_SOURCE_DIR}/src/gpu
)

target_compile_definitions(gvfg PRIVATE
    GVFG_BUILD
    GVFG_VERSION_STRING="${PROJECT_VERSION}"
)

target_compile_definitions(gvfg PRIVATE
  GVFG_INTERNAL_DIAGNOSTICS=$<IF:$<CONFIG:Debug>,1,0>
)

target_link_libraries(gvfg PRIVATE
    giga_ioctl
    setupapi
    user32
    dxgi
    d3d11
    D3DCompiler
)

if (MSVC)
  target_compile_options(gvfg PRIVATE /utf-8 /EHsc)
endif()

if (MSVC AND GVFG_MINGW_DLLTOOL)
  add_custom_command(TARGET gvfg POST_BUILD
    COMMAND "${GVFG_MINGW_DLLTOOL}"
      -d "${CMAKE_CURRENT_SOURCE_DIR}/gvfg.def"
      -D gvfg.dll
      -l libgvfg.dll.a
    WORKING_DIRECTORY "${CMAKE_ARCHIVE_OUTPUT_DIRECTORY}"
    COMMENT "Generating MinGW import library for gvfg.dll"
    VERBATIM
  )
endif()

install(TARGETS gvfg
  RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
  ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
)

install(FILES include/gvfg_capture.h
  DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
)

if (MSVC AND GVFG_MINGW_DLLTOOL)
  install(FILES "${CMAKE_ARCHIVE_OUTPUT_DIRECTORY}/libgvfg.dll.a"
    DESTINATION ${CMAKE_INSTALL_LIBDIR}
  )
endif()
