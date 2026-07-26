@echo off
REM Build ESP-LEGO sensor firmware (ESP32-C3). Auto-switches config.
REM Delegates to build_sensor.ps1.
REM
REM Usage:
REM   scripts\build_sensor.bat              build only
REM   scripts\build_sensor.bat -Profile pump
REM   scripts\build_sensor.bat -Profile pump -Flash -Port COM5
REM   scripts\build_sensor.bat flash        build + auto-detect COM + flash
REM   scripts\build_sensor.bat monitor      build + flash + serial monitor
REM   scripts\build_sensor.bat flash COM5   specify COM port manually

setlocal
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0build_sensor.ps1" %*
if %ERRORLEVEL% neq 0 (
    echo.
    echo BUILD FAILED
    pause
    exit /b %ERRORLEVEL%
)
