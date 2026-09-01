cmake_minimum_required(VERSION 3.16)

project(gvfg_qt_preview LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

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
  gvfg
  gvfg_preview
)

target_compile_definitions(gvfg_qt_preview PRIVATE
  GVFG_INTERNAL_DIAGNOSTICS=$<IF:$<CONFIG:Debug>,1,0>
)

if (MSVC)
  target_compile_options(gvfg_qt_preview PRIVATE /utf-8)
endif()

add_custom_command(TARGET gvfg_qt_preview POST_BUILD
  COMMAND ${CMAKE_COMMAND} -E copy_if_different
    $<TARGET_FILE:gvfg>
    $<TARGET_FILE_DIR:gvfg_qt_preview>
)

set_target_properties(gvfg_qt_preview PROPERTIES
  WIN32_EXECUTABLE TRUE
)

install(TARGETS gvfg_qt_preview
  RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
)
