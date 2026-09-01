@echo off
setlocal EnableExtensions

set "PROJECT_DIR=%~dp0"
set "PROJECT_DIR=%PROJECT_DIR:~0,-1%"

if not defined SOURCE_BIN set "SOURCE_BIN=%PROJECT_DIR%\build\Desktop_Qt_6_10_2_MSVC2022_64bit-Debug\bin"
if not defined QT_BIN set "QT_BIN=C:\Qt\6.10.2\msvc2022_64\bin"
if not defined VSDEVCMD set "VSDEVCMD=C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\Common7\Tools\VsDevCmd.bat"

if "%~1"=="" (
    set "OUTPUT_DIR=%PROJECT_DIR%\packages"
) else (
    set "OUTPUT_DIR=%~1"
)

for /f %%I in ('powershell -NoProfile -ExecutionPolicy Bypass -Command "Get-Date -Format yyyyMMdd_HHmmss"') do set "STAMP=%%I"

set "PACKAGE_NAME=GVFG_Qt_Preview_Debug_%STAMP%"
set "ZIP_PATH=%OUTPUT_DIR%\%PACKAGE_NAME%.zip"
set "STAGE_ROOT=%TEMP%\%PACKAGE_NAME%_%RANDOM%"
set "STAGE_DIR=%STAGE_ROOT%\%PACKAGE_NAME%"
set "WINDEPLOYQT=%QT_BIN%\windeployqt.exe"

echo [package] Configuration: Debug
echo [package] Source: "%SOURCE_BIN%"
echo [package] Output: "%ZIP_PATH%"

if not exist "%SOURCE_BIN%\gvfg_qt_preview.exe" (
    echo [package] ERROR: gvfg_qt_preview.exe not found.
    goto fail
)
if not exist "%SOURCE_BIN%\gvfg.dll" (
    echo [package] ERROR: gvfg.dll not found.
    goto fail
)
if not exist "%SOURCE_BIN%\giga_ioctl.dll" (
    echo [package] ERROR: giga_ioctl.dll not found.
    goto fail
)
if not exist "%SOURCE_BIN%\gvfg_preview.dll" (
    echo [package] ERROR: gvfg_preview.dll not found.
    goto fail
)
if not exist "%WINDEPLOYQT%" (
    echo [package] ERROR: "%WINDEPLOYQT%" not found.
    goto fail
)

if not exist "%OUTPUT_DIR%" mkdir "%OUTPUT_DIR%" || goto fail
if exist "%STAGE_ROOT%" rmdir /S /Q "%STAGE_ROOT%" || goto fail
mkdir "%STAGE_DIR%" || goto fail

echo [package] Copy application files and debug symbols...
copy /Y "%SOURCE_BIN%\gvfg_qt_preview.exe" "%STAGE_DIR%\" >nul || goto fail
copy /Y "%SOURCE_BIN%\gvfg.dll" "%STAGE_DIR%\" >nul || goto fail
copy /Y "%SOURCE_BIN%\giga_ioctl.dll" "%STAGE_DIR%\" >nul || goto fail
copy /Y "%SOURCE_BIN%\gvfg_preview.dll" "%STAGE_DIR%\" >nul || goto fail
if exist "%SOURCE_BIN%\gvfg_qt_preview.pdb" copy /Y "%SOURCE_BIN%\gvfg_qt_preview.pdb" "%STAGE_DIR%\" >nul || goto fail
if exist "%SOURCE_BIN%\gvfg.pdb" copy /Y "%SOURCE_BIN%\gvfg.pdb" "%STAGE_DIR%\" >nul || goto fail
if exist "%SOURCE_BIN%\giga_ioctl.pdb" copy /Y "%SOURCE_BIN%\giga_ioctl.pdb" "%STAGE_DIR%\" >nul || goto fail
if exist "%SOURCE_BIN%\gvfg_preview.pdb" copy /Y "%SOURCE_BIN%\gvfg_preview.pdb" "%STAGE_DIR%\" >nul || goto fail

echo [package] Deploy Qt Debug runtime...
if exist "%VSDEVCMD%" call "%VSDEVCMD%" -arch=x64 >nul || goto fail
"%WINDEPLOYQT%" --debug --compiler-runtime --force --dir "%STAGE_DIR%" "%STAGE_DIR%\gvfg_qt_preview.exe"
if errorlevel 1 goto fail

echo [package] Create zip...
powershell -NoProfile -ExecutionPolicy Bypass -Command ^
    "Compress-Archive -Path '%STAGE_DIR%' -DestinationPath '%ZIP_PATH%' -Force"
if errorlevel 1 goto fail

rmdir /S /Q "%STAGE_ROOT%"

echo [package] Done.
echo [package] Zip: "%ZIP_PATH%"
echo [package] NOTE: The target PC may require the Visual C++ Debug Runtime.
if "%NO_PAUSE%"=="" pause
exit /b 0

:fail
echo [package] FAILED.
if exist "%STAGE_ROOT%" rmdir /S /Q "%STAGE_ROOT%" >nul 2>nul
if "%NO_PAUSE%"=="" pause
exit /b 1
