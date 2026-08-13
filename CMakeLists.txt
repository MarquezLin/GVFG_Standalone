cmake_minimum_required(VERSION 3.16)

project(gvfg_standalone VERSION 0.1.0 LANGUAGES CXX)

include(GNUInstallDirs)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

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

if (BUILD_GVFG_SAMPLES)
  add_subdirectory(samples/gvfg_qt_preview)
endif()

install(FILES
  docs/GVFG_CUSTOMER_API.md
  docs/GVFG_CUSTOMER_API_REFERENCE.md
  DESTINATION ${CMAKE_INSTALL_DOCDIR}
)
