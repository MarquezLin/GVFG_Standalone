# Internal WASAPI backend. Only gvfg_playback is installed as a playback DLL.
add_library(gvfg_audio_playback STATIC
    include/gvfg_audio_playback.h
    src/audio_backend_internal.h
    src/gvfg_audio_playback.cpp
    src/audio_output_internal.h
)
set_target_properties(gvfg_audio_playback PROPERTIES AUTOMOC OFF AUTOUIC OFF AUTORCC OFF)
target_include_directories(gvfg_audio_playback PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(gvfg_audio_playback PRIVATE ole32)
if(MSVC)
  target_compile_options(gvfg_audio_playback PRIVATE /utf-8 /EHsc)
endif()
