project(gvfg_audio_playback LANGUAGES CXX)

add_library(gvfg_audio_playback SHARED
    gvfg_audio_playback.def
    include/gvfg_audio_playback.h
    src/gvfg_audio_playback.cpp
)

set_target_properties(gvfg_audio_playback PROPERTIES
    AUTOMOC OFF
    AUTOUIC OFF
    AUTORCC OFF
)

target_include_directories(gvfg_audio_playback
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_compile_definitions(gvfg_audio_playback PRIVATE
    GVFG_AUDIO_PLAYBACK_BUILD
)

target_link_libraries(gvfg_audio_playback PRIVATE
    ole32
)

if (MSVC)
  target_compile_options(gvfg_audio_playback PRIVATE /utf-8 /EHsc)
endif()

if (MSVC AND GVFG_MINGW_DLLTOOL)
  add_custom_command(TARGET gvfg_audio_playback POST_BUILD
    COMMAND "${GVFG_MINGW_DLLTOOL}"
      -d "${CMAKE_CURRENT_SOURCE_DIR}/gvfg_audio_playback.def"
      -D gvfg_audio_playback.dll
      -l libgvfg_audio_playback.dll.a
    WORKING_DIRECTORY "${CMAKE_ARCHIVE_OUTPUT_DIRECTORY}"
    COMMENT "Generating MinGW import library for gvfg_audio_playback.dll"
    VERBATIM
  )
endif()

install(TARGETS gvfg_audio_playback
  RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
  ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
)

install(FILES include/gvfg_audio_playback.h
  DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
)

if (MSVC AND GVFG_MINGW_DLLTOOL)
  install(FILES "${CMAKE_ARCHIVE_OUTPUT_DIRECTORY}/libgvfg_audio_playback.dll.a"
    DESTINATION ${CMAKE_INSTALL_LIBDIR}
  )
endif()
