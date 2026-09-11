cmake_minimum_required(VERSION 3.16)

project(gvfg_sdk VERSION 0.2.1 LANGUAGES CXX)

include(GNUInstallDirs)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(CMAKE_AUTOMOC OFF)
set(CMAKE_AUTOUIC OFF)
set(CMAKE_AUTORCC OFF)

set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/lib")

if (NOT WIN32)
  message(FATAL_ERROR "GVFG SDK currently supports Windows only.")
endif()

add_subdirectory(sdk/giga_ioctl)
add_subdirectory(sdk/gvfg)
add_subdirectory(helpers/gvfg_preview)

install(FILES
  ../docs/GVFG_CUSTOMER_API.md
  ../docs/GVFG_CUSTOMER_API_REFERENCE.md
  DESTINATION ${CMAKE_INSTALL_DOCDIR}
)
