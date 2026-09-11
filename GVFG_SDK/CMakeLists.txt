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

set(GVFG_MINGW_DLLTOOL "" CACHE FILEPATH
    "MinGW dlltool used to generate GNU import libraries from the MSVC DLL exports")
if (MSVC AND NOT GVFG_MINGW_DLLTOOL)
  find_program(_GVFG_MINGW_DLLTOOL_FOUND
    NAMES dlltool.exe x86_64-w64-mingw32-dlltool.exe
    HINTS
      "C:/Qt/Tools/mingw1310_64/bin"
      "C:/Qt/Tools/mingw1120_64/bin"
    NO_CACHE
  )
  if (_GVFG_MINGW_DLLTOOL_FOUND)
    set(GVFG_MINGW_DLLTOOL "${_GVFG_MINGW_DLLTOOL_FOUND}" CACHE FILEPATH
        "MinGW dlltool used to generate GNU import libraries from the MSVC DLL exports" FORCE)
  endif()
endif()

if (MSVC AND NOT GVFG_MINGW_DLLTOOL)
  message(WARNING
    "MinGW dlltool was not found. MSVC libraries will be built, but GNU "
    "import libraries for MinGW consumers will not be generated. Set "
    "GVFG_MINGW_DLLTOOL to enable them.")
endif()

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
