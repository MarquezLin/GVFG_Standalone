@echo off
setlocal

set "PROJECT_DIR=%~dp0"
set "BUILD_DIR=%PROJECT_DIR%build_diagnostic"
set "VSDEVCMD=C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\Common7\Tools\VsDevCmd.bat"
set "CMAKE=C:\Qt\Tools\CMake_64\bin\cmake.exe"
set "QT_DIR=C:\Qt\6.10.2\msvc2022_64"

call "%VSDEVCMD%" -arch=x64
if errorlevel 1 goto :failed

"%CMAKE%" -S "%PROJECT_DIR%" -B "%BUILD_DIR%" ^
  -DBUILD_GVFG_SAMPLES=ON ^
  -DCMAKE_PREFIX_PATH="%QT_DIR%"
if errorlevel 1 goto :failed

"%CMAKE%" --build "%BUILD_DIR%" --target gvfg_qt_preview --config Debug
if errorlevel 1 goto :failed

start "GVFG Diagnostic" "%BUILD_DIR%\bin\Debug\gvfg_qt_preview.exe"
exit /b 0

:failed
echo.
echo GVFG diagnostic build failed.
pause
exit /b 1
