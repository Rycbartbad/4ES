@echo off
REM Build / flash / monitor ESP-LEGO sensor firmware

if "%IDF_PATH%"=="" (
    echo Exporting IDF environment...
    call d:\IDF_v5.2.6\v5.2.6\esp-idf\export.bat
)

set SDKCONFIG_DEFAULTS=sdkconfig.defaults;sdkconfig.defaults.sensor

if "%1"=="flash" goto flash
if "%1"=="monitor" goto monitor

echo Building SENSOR firmware...
idf.py build
if %ERRORLEVEL% equ 0 (
    echo.
    echo SENSOR BUILD OK
    echo   build_sensor.bat flash    - flash to device
    echo   build_sensor.bat monitor  - serial monitor
) else (
    echo.
    echo BUILD FAILED
)
exit /b %ERRORLEVEL%

:flash
echo Flashing SENSOR firmware...
idf.py flash
exit /b %ERRORLEVEL%

:monitor
idf.py monitor
exit /b %ERRORLEVEL%
