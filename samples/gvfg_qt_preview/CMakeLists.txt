cmake_minimum_required(VERSION 3.16)

project(gvfg_qt_preview LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(QT NAMES Qt6 Qt5 REQUIRED COMPONENTS Widgets)
find_package(Qt${QT_VERSION_MAJOR} REQUIRED COMPONENTS Widgets)

set(GVFG_QT_PREVIEW_SOURCES
  main.cpp
  mainwindow.cpp
  mainwindow.h
  mainwindow.ui
  previewwindow.cpp
  previewwindow.h
  previewwindow.ui
)

function(add_gvfg_qt_preview_target target_name internal_diagnostics)
  if (QT_VERSION_MAJOR GREATER_EQUAL 6)
    qt_add_executable(${target_name} ${GVFG_QT_PREVIEW_SOURCES})
  else()
    add_executable(${target_name} ${GVFG_QT_PREVIEW_SOURCES})
  endif()

  target_link_libraries(${target_name} PRIVATE
    Qt${QT_VERSION_MAJOR}::Widgets
    gvfg
    gvfg_preview
  )

  target_compile_definitions(${target_name} PRIVATE
    GVFG_INTERNAL_DIAGNOSTICS=${internal_diagnostics}
  )

  if (MSVC)
    target_compile_options(${target_name} PRIVATE /utf-8)
  endif()

  add_custom_command(TARGET ${target_name} POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
      $<TARGET_FILE:gvfg>
      $<TARGET_FILE_DIR:${target_name}>
  )

  set_target_properties(${target_name} PROPERTIES
    WIN32_EXECUTABLE TRUE
  )
endfunction()

if (BUILD_GVFG_SAMPLES)
  add_gvfg_qt_preview_target(gvfg_qt_preview 0)
  install(TARGETS gvfg_qt_preview
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
  )
endif()

if (BUILD_GVFG_INTERNAL_TOOLS)
  add_gvfg_qt_preview_target(gvfg_qt_diagnostic 1)
  install(TARGETS gvfg_qt_diagnostic
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
  )
endif()
