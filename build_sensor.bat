@echo off
setlocal

REM Convenience wrapper for ESP-LEGO offline sensor firmware.
REM Preferred direct usage:
REM   powershell -ExecutionPolicy Bypass -File sensor_test.ps1 build
REM   powershell -ExecutionPolicy Bypass -File sensor_test.ps1 flash-monitor -Port COM19

set ACTION=%1
set PORT=%2

if "%ACTION%"=="" set ACTION=build

set SCRIPT_DIR=%~dp0
set PS_ARGS=-ExecutionPolicy Bypass -File "%SCRIPT_DIR%sensor_test.ps1" %ACTION%

if not "%PORT%"=="" (
    set PS_ARGS=%PS_ARGS% -Port %PORT%
)

powershell %PS_ARGS%
exit /b %ERRORLEVEL%
