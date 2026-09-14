# Inherit the version from the standalone root project. The root project()
# declaration is the single source of truth for the public SDK version.
set(_GVFG_VERSION "${PROJECT_VERSION}")
project(gvfg VERSION ${_GVFG_VERSION} LANGUAGES CXX)

add_library(gvfg SHARED
    gvfg.def
    include/gvfg_capture.h
    include/gvfg_debug.h
    src/backend/gigabyte/gigabyte_backend_types.h
    src/backend/gigabyte/gigabyte_capture_session.cpp
    src/backend/gigabyte/gigabyte_capture_session.h
    src/backend/gigabyte/gigabyte_device.cpp
    src/backend/gigabyte/gigabyte_device.h
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
        ${CMAKE_CURRENT_SOURCE_DIR}/src/backend/gigabyte
        ${CMAKE_CURRENT_SOURCE_DIR}/src/gpu
        ${CMAKE_CURRENT_SOURCE_DIR}/../../third_party/GigabyteLib
)

target_compile_definitions(gvfg PRIVATE
    GVFG_BUILD
    GVFG_VERSION_STRING="${PROJECT_VERSION}"
)

target_compile_definitions(gvfg PRIVATE
  GVFG_INTERNAL_DIAGNOSTICS=$<IF:$<CONFIG:Debug>,1,0>
)

target_link_libraries(gvfg PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/../../third_party/GigabyteLib/GvfgSdk.lib"
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
