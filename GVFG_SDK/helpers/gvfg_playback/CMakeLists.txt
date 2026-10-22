# Public playback DLL; backend libraries remain implementation details.
enable_language(RC)
add_library(gvfg_playback SHARED
    gvfg_playback.def
    include/gvfg_playback.h
    src/gvfg_playback.cpp
    src/playback_timing.h
    src/signal_card.h
    src/signal_card.cpp
    src/signal_card.rc
)
set_target_properties(gvfg_playback PROPERTIES AUTOMOC OFF AUTOUIC OFF AUTORCC OFF)
target_include_directories(gvfg_playback
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/include
        ${CMAKE_CURRENT_SOURCE_DIR}/../gvfg_preview/include
        ${CMAKE_CURRENT_SOURCE_DIR}/../gvfg_audio_playback/include
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src
        ${CMAKE_CURRENT_SOURCE_DIR}/../gvfg_audio_playback/src
        ${CMAKE_CURRENT_SOURCE_DIR}/../gvfg_preview/src
)
target_compile_definitions(gvfg_playback PRIVATE GVFG_PLAYBACK_BUILD)
target_link_libraries(gvfg_playback PRIVATE gvfg_audio_playback gvfg_preview user32 gdi32 comctl32)
if(MSVC)
  target_compile_options(gvfg_playback PRIVATE /utf-8 /EHsc)
endif()
if(MSVC AND GVFG_MINGW_DLLTOOL)
  add_custom_command(TARGET gvfg_playback POST_BUILD
    COMMAND "${GVFG_MINGW_DLLTOOL}"
      -d "${CMAKE_CURRENT_SOURCE_DIR}/gvfg_playback.def"
      -D gvfg_playback.dll -l libgvfg_playback.dll.a
    WORKING_DIRECTORY "${CMAKE_ARCHIVE_OUTPUT_DIRECTORY}"
    COMMENT "Generating MinGW import library for gvfg_playback.dll"
    VERBATIM
  )
  install(FILES "${CMAKE_ARCHIVE_OUTPUT_DIRECTORY}/libgvfg_playback.dll.a"
    DESTINATION ${CMAKE_INSTALL_LIBDIR})
endif()
install(TARGETS gvfg_playback
  RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
  ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR})
# Backend headers supply the frame/format/stat types used by the public API.
install(FILES include/gvfg_playback.h
    ../gvfg_audio_playback/include/gvfg_audio_playback.h
    ../gvfg_preview/include/gvfg_preview.h
    DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
# This README describes private internals. Customer guides are installed by
# the SDK root with the same relative layout as docs/.
