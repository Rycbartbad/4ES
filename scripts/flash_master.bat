@echo off
REM Flash ESP-LEGO master firmware (ESP32-S3) with COM auto-detect.
REM Delegates to flash_master.ps1 for COM detection logic.
REM
REM Usage:
REM   scripts\flash_master.bat              flash only (auto-detect COM)
REM   scripts\flash_master.bat COM4         specify COM port manually
REM   scripts\flash_master.bat COM4 monitor flash + serial monitor

setlocal
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0flash_master.ps1" %*
if %ERRORLEVEL% neq 0 (
    echo.
    echo Tip: Specify COM port manually:
    echo   scripts\flash_master.bat COM4
    echo   scripts\flash_master.bat COM4 monitor
    pause
    exit /b %ERRORLEVEL%
)
