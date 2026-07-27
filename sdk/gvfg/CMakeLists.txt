if (NOT DEFINED GVFG_SDK_VERSION)
  set(GVFG_SDK_VERSION "0.1.0" CACHE STRING "GVFG SDK version")
endif()

option(GVFG_PCIES2MM_DEBUG_LOG "Write verbose PCIES2MM backend trace logs" OFF)

project(gvfg VERSION ${GVFG_SDK_VERSION} LANGUAGES CXX)

add_library(gvfg SHARED
    include/gvfg_capture.h
    src/backend/pcies2mm/pcies2mm_backend_types.h
    src/backend/pcies2mm/pcies2mm_capture_session.cpp
    src/backend/pcies2mm/pcies2mm_capture_session.h
    src/backend/pcies2mm/pcies2mm_device.cpp
    src/backend/pcies2mm/pcies2mm_device.h
    src/backend/pcies2mm/pcies2mm_ioctl.h
    src/backend/pcies2mm/pcies2mm_reg.h
    src/backend/pcies2mm/pcies2mm_video_format.cpp
    src/backend/pcies2mm/pcies2mm_video_format.h
    src/gvfg_capture.cpp
)

if (GVFG_ENABLE_INTERNAL_DEBUG_API)
  target_sources(gvfg PRIVATE include/gvfg_debug.h)
endif()

set_target_properties(gvfg PROPERTIES
    AUTOMOC OFF
    AUTOUIC OFF
    AUTORCC OFF
)

target_include_directories(gvfg
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/include
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src/backend/pcies2mm
)

target_compile_definitions(gvfg PRIVATE
    GVFG_BUILD
    GVFG_SDK_VERSION="${GVFG_SDK_VERSION}"
    GVFG_ENABLE_INTERNAL_DEBUG_API=$<BOOL:${GVFG_ENABLE_INTERNAL_DEBUG_API}>
)

if (GVFG_PCIES2MM_DEBUG_LOG)
  target_compile_definitions(gvfg PRIVATE GVFG_PCIES2MM_DEBUG_LOG=1)
else()
  target_compile_definitions(gvfg PRIVATE GVFG_PCIES2MM_DEBUG_LOG=0)
endif()

target_link_libraries(gvfg PRIVATE
    setupapi
)

if (MSVC)
  target_compile_options(gvfg PRIVATE /utf-8 /EHsc)
endif()

install(TARGETS gvfg
  RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
  ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
)

install(FILES include/gvfg_capture.h
  DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
)

if (GVFG_ENABLE_INTERNAL_DEBUG_API)
  install(FILES include/gvfg_debug.h
    DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
  )
endif()

