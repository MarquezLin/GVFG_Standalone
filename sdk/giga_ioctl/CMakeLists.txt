add_library(giga_ioctl SHARED
    README.md
    include/giga_ioctl.h
    src/giga_ioctl.cpp
    src/giga_ioctl_private.h
)

target_include_directories(giga_ioctl PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_compile_definitions(giga_ioctl PRIVATE GIGA_IOCTL_BUILD)
set_target_properties(giga_ioctl PROPERTIES AUTOMOC OFF AUTOUIC OFF AUTORCC OFF)

if (MSVC)
  target_compile_options(giga_ioctl PRIVATE /utf-8 /EHsc)
endif()

install(TARGETS giga_ioctl
  RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
  ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
)
