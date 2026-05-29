@echo off
REM Flash ESP-LEGO sensor firmware (ESP32-C3) with COM auto-detect.
REM Delegates to flash_sensor.ps1 for COM detection logic.
REM
REM Usage:
REM   scripts\flash_sensor.bat              flash only (auto-detect COM)
REM   scripts\flash_sensor.bat COM5         specify COM port manually
REM   scripts\flash_sensor.bat COM5 monitor flash + serial monitor

setlocal
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0flash_sensor.ps1" %*
if %ERRORLEVEL% neq 0 (
    echo.
    echo Tip: Specify COM port manually:
    echo   scripts\flash_sensor.bat COM5
    echo   scripts\flash_sensor.bat COM5 monitor
    pause
    exit /b %ERRORLEVEL%
)
