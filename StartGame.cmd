@echo off
setlocal

powershell.exe -NoProfile -ExecutionPolicy Bypass -File "%~dp0StartEditor.ps1" -Game %*
set "EXIT_CODE=%ERRORLEVEL%"

if not "%EXIT_CODE%"=="0" (
    echo.
    echo Failed to start MatterFlux. See the error above.
    pause
)

exit /b %EXIT_CODE%
