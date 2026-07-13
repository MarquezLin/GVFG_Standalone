if (NOT DEFINED GVFG_SDK_VERSION)
  set(GVFG_SDK_VERSION "0.1.0" CACHE STRING "GVFG SDK version")
endif()

option(GVFG_PCIES2MM_DEBUG_LOG "Write verbose PCIES2MM backend trace logs" OFF)

project(gvfg VERSION ${GVFG_SDK_VERSION} LANGUAGES CXX)

add_library(gvfg SHARED
    include/gvfg_capture.h
    include/gvfg_debug.h
    src/backend/pcies2mm/pcies2mm_backend_types.h
    src/backend/pcies2mm/pcies2mm_capture_session.cpp
    src/backend/pcies2mm/pcies2mm_capture_session.h
    src/backend/pcies2mm/pcies2mm_public.h
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
        ${CMAKE_CURRENT_SOURCE_DIR}/src/backend/pcies2mm
)

target_compile_definitions(gvfg PRIVATE
    GVFG_BUILD
    GVFG_SDK_VERSION="${GVFG_SDK_VERSION}"
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


