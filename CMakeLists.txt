cmake_minimum_required(VERSION 3.16)

project(gvfg_standalone VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(GVFG_SDK_VERSION "${PROJECT_VERSION}" CACHE STRING "GVFG SDK version")

set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTOUIC ON)
set(CMAKE_AUTORCC ON)

set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/lib")

option(BUILD_GVFG_SAMPLES "Build GVFG customer samples" ON)

if (NOT WIN32)
  message(FATAL_ERROR "GVFG currently supports Windows only.")
endif()

add_subdirectory(sdk/gvfg)
add_subdirectory(helpers/gvfg_preview)
add_subdirectory(helpers/gvfg_convert)

if (BUILD_GVFG_SAMPLES)
  add_subdirectory(samples/gvfg_qt_preview)
endif()
