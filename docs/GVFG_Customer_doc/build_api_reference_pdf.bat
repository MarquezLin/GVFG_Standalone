@echo off
setlocal

set "SCRIPT_DIR=%~dp0"
set "PYTHON_EXE="

if exist "%USERPROFILE%\.cache\codex-runtimes\codex-primary-runtime\dependencies\python\python.exe" (
    set "PYTHON_EXE=%USERPROFILE%\.cache\codex-runtimes\codex-primary-runtime\dependencies\python\python.exe"
)

if not defined PYTHON_EXE (
    where python.exe >nul 2>nul
    if not errorlevel 1 set "PYTHON_EXE=python.exe"
)

if not defined PYTHON_EXE (
    echo ERROR: Python 3 ^(python.exe^) was not found.
    echo Install Python 3 or run this tool on the GVFG development machine.
    pause
    exit /b 1
)

"%PYTHON_EXE%" "%SCRIPT_DIR%build_api_reference_pdf.py" %*
set "RESULT=%ERRORLEVEL%"

if not "%RESULT%"=="0" (
    echo.
    echo PDF generation failed. See the error above.
    pause
)

exit /b %RESULT%
