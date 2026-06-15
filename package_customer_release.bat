@echo off
setlocal EnableExtensions

set "PROJECT_DIR=%~dp0"
set "PROJECT_DIR=%PROJECT_DIR:~0,-1%"
set "PROJECT_NAME=GVFG_Standalone"
set "CONFIG=Release"

if "%~1"=="" (
    set "OUTPUT_DIR=%PROJECT_DIR%\packages"
) else (
    set "OUTPUT_DIR=%~1"
)

if not defined BUILD_DIR set "BUILD_DIR=%PROJECT_DIR%\build_customer_release"
if not defined QT_DIR set "QT_DIR=C:\Qt\6.10.2\msvc2022_64"
if not defined QT_BIN set "QT_BIN=%QT_DIR%\bin"
if not defined CMAKE_EXE set "CMAKE_EXE=C:\Qt\Tools\CMake_64\bin\cmake.exe"
if not exist "%CMAKE_EXE%" set "CMAKE_EXE=cmake"
if not defined VSDEVCMD set "VSDEVCMD=C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\Common7\Tools\VsDevCmd.bat"

set "Path="
set "PATH=C:\Windows\system32;C:\Windows;C:\Windows\System32\Wbem;C:\Windows\System32\WindowsPowerShell\v1.0"

for /f %%I in ('powershell -NoProfile -ExecutionPolicy Bypass -Command "Get-Date -Format yyyyMMdd_HHmmss"') do set "STAMP=%%I"

set "PACKAGE_NAME=%PROJECT_NAME%_Customer_Release_%STAMP%"
set "ZIP_PATH=%OUTPUT_DIR%\%PACKAGE_NAME%.zip"
set "STAGE_ROOT=%TEMP%\%PACKAGE_NAME%_%RANDOM%"
set "STAGE_DIR=%STAGE_ROOT%\%PACKAGE_NAME%"

echo [release] Project: "%PROJECT_DIR%"
echo [release] Build:   "%BUILD_DIR%"
echo [release] Output:  "%ZIP_PATH%"

if not exist "%OUTPUT_DIR%" (
    echo [release] Create output folder...
    mkdir "%OUTPUT_DIR%" || goto fail
)

if exist "%VSDEVCMD%" (
    echo [release] Load Visual Studio x64 build environment...
    call "%VSDEVCMD%" -arch=x64 || goto fail
) else (
    echo [release] VSDEVCMD not found. Assuming this is already a Visual Studio developer shell.
)

echo [release] Configure CMake...
"%CMAKE_EXE%" -S "%PROJECT_DIR%" -B "%BUILD_DIR%" -DBUILD_GVFG_SAMPLES=ON -DCMAKE_PREFIX_PATH="%QT_DIR%"
if errorlevel 1 goto fail

echo [release] Build %CONFIG%...
"%CMAKE_EXE%" --build "%BUILD_DIR%" --target gvfg_qt_preview --config %CONFIG%
if errorlevel 1 goto fail

set "BIN_DIR=%BUILD_DIR%\bin\%CONFIG%"
set "LIB_DIR=%BUILD_DIR%\lib\%CONFIG%"

if not exist "%BIN_DIR%\gvfg.dll" (
    echo [release] ERROR: "%BIN_DIR%\gvfg.dll" not found.
    goto fail
)

if not exist "%LIB_DIR%\gvfg.lib" (
    echo [release] ERROR: "%LIB_DIR%\gvfg.lib" not found.
    goto fail
)

if not exist "%BIN_DIR%\gvfg_qt_preview.exe" (
    echo [release] ERROR: "%BIN_DIR%\gvfg_qt_preview.exe" not found.
    goto fail
)

if exist "%STAGE_ROOT%" rmdir /S /Q "%STAGE_ROOT%" || goto fail

echo [release] Stage customer package...
mkdir "%STAGE_DIR%\bin" || goto fail
mkdir "%STAGE_DIR%\lib" || goto fail
mkdir "%STAGE_DIR%\include" || goto fail
mkdir "%STAGE_DIR%\docs" || goto fail
mkdir "%STAGE_DIR%\samples\gvfg_qt_preview" || goto fail
mkdir "%STAGE_DIR%\samples\gvfg_qt_preview_source" || goto fail

copy /Y "%PROJECT_DIR%\sdk\gvfg\include\gvfg_capture.h" "%STAGE_DIR%\include\" >nul || goto fail
copy /Y "%BIN_DIR%\gvfg.dll" "%STAGE_DIR%\bin\" >nul || goto fail
copy /Y "%LIB_DIR%\gvfg.lib" "%STAGE_DIR%\lib\" >nul || goto fail
copy /Y "%BIN_DIR%\gvfg_qt_preview.exe" "%STAGE_DIR%\samples\gvfg_qt_preview\" >nul || goto fail
copy /Y "%BIN_DIR%\gvfg.dll" "%STAGE_DIR%\samples\gvfg_qt_preview\" >nul || goto fail
copy /Y "%PROJECT_DIR%\README.md" "%STAGE_DIR%\" >nul || goto fail
copy /Y "%PROJECT_DIR%\docs\GVFG_CUSTOMER_API.md" "%STAGE_DIR%\docs\" >nul || goto fail

robocopy "%PROJECT_DIR%\samples\gvfg_qt_preview" "%STAGE_DIR%\samples\gvfg_qt_preview_source" ^
    /E ^
    /XF "*.user" "*.suo" "*.obj" "*.pdb" "*.ilk" "*.exp" "*.lib" "*.dll" "*.exe" "*.zip" ^
    /NFL /NDL /NJH /NJS /NP

if errorlevel 8 (
    echo [release] ERROR: robocopy sample source failed.
    goto fail
)

echo [release] Deploy Qt runtime for sample...
if not exist "%QT_BIN%\windeployqt.exe" (
    echo [release] ERROR: "%QT_BIN%\windeployqt.exe" not found.
    echo [release] Set QT_DIR or QT_BIN before running this script.
    goto fail
)

"%QT_BIN%\windeployqt.exe" --release --compiler-runtime --force "%STAGE_DIR%\samples\gvfg_qt_preview\gvfg_qt_preview.exe"
if errorlevel 1 goto fail

echo [release] Write package notes...
(
    echo GVFG Standalone Customer Release
    echo.
    echo Layout:
    echo   include\gvfg_capture.h
    echo   lib\gvfg.lib
    echo   bin\gvfg.dll
    echo   samples\gvfg_qt_preview\gvfg_qt_preview.exe
    echo   samples\gvfg_qt_preview_source\
    echo   docs\GVFG_CUSTOMER_API.md
    echo.
    echo Link with lib\gvfg.lib and deploy bin\gvfg.dll next to the customer application executable.
    echo Run samples\gvfg_qt_preview\gvfg_qt_preview.exe to verify the packaged runtime.
) > "%STAGE_DIR%\PACKAGE_README.txt"

echo [release] Create zip...
powershell -NoProfile -ExecutionPolicy Bypass -Command ^
    "Compress-Archive -Path '%STAGE_DIR%' -DestinationPath '%ZIP_PATH%' -Force"
if errorlevel 1 goto fail

echo [release] Clean staging folder...
rmdir /S /Q "%STAGE_ROOT%" || goto fail

echo [release] Done.
echo [release] Zip: "%ZIP_PATH%"
if "%NO_PAUSE%"=="" pause
exit /b 0

:fail
echo [release] FAILED.
if exist "%STAGE_ROOT%" rmdir /S /Q "%STAGE_ROOT%" >nul 2>nul
if "%NO_PAUSE%"=="" pause
exit /b 1
