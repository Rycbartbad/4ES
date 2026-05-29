@echo off
REM Build ESP-LEGO master firmware (ESP32-S3). Auto-switches config.
REM Delegates to build_master.ps1.
REM
REM Usage:
REM   scripts\build_master.bat              build only
REM   scripts\build_master.bat flash        build + auto-detect COM + flash
REM   scripts\build_master.bat monitor      build + flash + serial monitor
REM   scripts\build_master.bat flash COM4   specify COM port manually

setlocal
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0build_master.ps1" %*
if %ERRORLEVEL% neq 0 (
    echo.
    echo BUILD FAILED
    pause
    exit /b %ERRORLEVEL%
)
