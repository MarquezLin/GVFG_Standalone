cmake_minimum_required(VERSION 3.16)

project(gvfg_qt_preview LANGUAGES CXX)

include(GNUInstallDirs)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTOUIC ON)
set(CMAKE_AUTORCC ON)

set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")

if (NOT WIN32)
  message(FATAL_ERROR "GVFG Qt Sample currently supports Windows only.")
endif()

set(GVFG_SDK_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../GVFG_SDK"
    CACHE PATH "GVFG SDK source root containing public headers")
set(GVFG_SDK_BUILD_DIR "${GVFG_SDK_ROOT}/build"
    CACHE PATH "Prebuilt GVFG SDK directory containing bin and lib")

set(_GVFG_INCLUDE_DIR "${GVFG_SDK_ROOT}/sdk/gvfg/include")
set(_GVFG_PREVIEW_INCLUDE_DIR "${GVFG_SDK_ROOT}/helpers/gvfg_preview/include")

if (MSVC)
  set(_GVFG_IMPLIB_NAME "gvfg.lib")
  set(_GVFG_PREVIEW_IMPLIB_NAME "gvfg_preview.lib")
elseif (MINGW)
  set(_GVFG_IMPLIB_NAME "libgvfg.dll.a")
  set(_GVFG_PREVIEW_IMPLIB_NAME "libgvfg_preview.dll.a")
else()
  message(FATAL_ERROR "Unsupported compiler. Use MSVC or MinGW on Windows x64.")
endif()

# Accept either one exact SDK build directory or the parent `build` directory
# created by Qt Creator. In the latter case, select the current configuration's
# child only when it contains this compiler's import libraries.
set(_GVFG_RESOLVED_BUILD_DIR "${GVFG_SDK_BUILD_DIR}")
if (NOT EXISTS "${_GVFG_RESOLVED_BUILD_DIR}/lib/${_GVFG_IMPLIB_NAME}")
  if (CMAKE_BUILD_TYPE)
    file(GLOB _GVFG_BUILD_CANDIDATES LIST_DIRECTORIES true
      "${GVFG_SDK_BUILD_DIR}/*-${CMAKE_BUILD_TYPE}")
    foreach(_candidate IN LISTS _GVFG_BUILD_CANDIDATES)
      if (EXISTS "${_candidate}/bin/gvfg.dll" AND
          EXISTS "${_candidate}/lib/${_GVFG_IMPLIB_NAME}" AND
          EXISTS "${_candidate}/lib/${_GVFG_PREVIEW_IMPLIB_NAME}")
        set(_GVFG_RESOLVED_BUILD_DIR "${_candidate}")
        break()
      endif()
    endforeach()
  endif()
endif()

set(_GVFG_BIN_DIR "${_GVFG_RESOLVED_BUILD_DIR}/bin")
set(_GVFG_LIB_DIR "${_GVFG_RESOLVED_BUILD_DIR}/lib")
set(_GVFG_IMPLIB "${_GVFG_LIB_DIR}/${_GVFG_IMPLIB_NAME}")
set(_GVFG_PREVIEW_IMPLIB "${_GVFG_LIB_DIR}/${_GVFG_PREVIEW_IMPLIB_NAME}")

message(STATUS "GVFG SDK build: ${_GVFG_RESOLVED_BUILD_DIR}")

foreach(_required_file
    "${_GVFG_INCLUDE_DIR}/gvfg_capture.h"
    "${_GVFG_PREVIEW_INCLUDE_DIR}/gvfg_preview.h"
    "${_GVFG_BIN_DIR}/gvfg.dll"
    "${_GVFG_BIN_DIR}/giga_ioctl.dll"
    "${_GVFG_BIN_DIR}/gvfg_preview.dll"
    "${_GVFG_IMPLIB}"
    "${_GVFG_PREVIEW_IMPLIB}")
  if (NOT EXISTS "${_required_file}")
    message(FATAL_ERROR
      "Required prebuilt GVFG SDK file was not found: ${_required_file}\n"
      "Build/install GVFG_SDK first or set GVFG_SDK_BUILD_DIR.")
  endif()
endforeach()

add_library(GVFG::Capture SHARED IMPORTED GLOBAL)
set_target_properties(GVFG::Capture PROPERTIES
  IMPORTED_LOCATION "${_GVFG_BIN_DIR}/gvfg.dll"
  IMPORTED_IMPLIB "${_GVFG_IMPLIB}"
  INTERFACE_INCLUDE_DIRECTORIES "${_GVFG_INCLUDE_DIR}"
)

add_library(GVFG::Preview SHARED IMPORTED GLOBAL)
set_target_properties(GVFG::Preview PROPERTIES
  IMPORTED_LOCATION "${_GVFG_BIN_DIR}/gvfg_preview.dll"
  IMPORTED_IMPLIB "${_GVFG_PREVIEW_IMPLIB}"
  INTERFACE_INCLUDE_DIRECTORIES "${_GVFG_PREVIEW_INCLUDE_DIR}"
)

find_package(QT NAMES Qt6 REQUIRED COMPONENTS Widgets Multimedia)
find_package(Qt${QT_VERSION_MAJOR} REQUIRED COMPONENTS Widgets Multimedia)

set(GVFG_QT_PREVIEW_SOURCES
  main.cpp
  mainwindow.cpp
  mainwindow.h
  mainwindow.ui
  previewwindow.cpp
  previewwindow.h
  previewwindow.ui
)

if (QT_VERSION_MAJOR GREATER_EQUAL 6)
  qt_add_executable(gvfg_qt_preview ${GVFG_QT_PREVIEW_SOURCES})
else()
  add_executable(gvfg_qt_preview ${GVFG_QT_PREVIEW_SOURCES})
endif()

target_link_libraries(gvfg_qt_preview PRIVATE
  Qt${QT_VERSION_MAJOR}::Widgets
  Qt${QT_VERSION_MAJOR}::Multimedia
  GVFG::Capture
  GVFG::Preview
)

target_compile_definitions(gvfg_qt_preview PRIVATE
  GVFG_INTERNAL_DIAGNOSTICS=$<IF:$<CONFIG:Debug>,1,0>
)

if (MSVC)
  target_compile_options(gvfg_qt_preview PRIVATE /utf-8)
endif()

add_custom_command(TARGET gvfg_qt_preview POST_BUILD
  COMMAND ${CMAKE_COMMAND} -E copy_if_different
    "${_GVFG_BIN_DIR}/gvfg.dll"
    "${_GVFG_BIN_DIR}/giga_ioctl.dll"
    "${_GVFG_BIN_DIR}/gvfg_preview.dll"
    $<TARGET_FILE_DIR:gvfg_qt_preview>
)

set_target_properties(gvfg_qt_preview PROPERTIES
  WIN32_EXECUTABLE TRUE
)

install(TARGETS gvfg_qt_preview
  RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
)
