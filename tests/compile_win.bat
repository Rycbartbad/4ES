@echo off
REM Windows x86 compilation for ESP-LEGO unit tests
REM Requires: MinGW g++ or MSVC cl.exe with C++17 support
REM
REM Include path order is CRITICAL:
REM   -Itests/mocks MUST come first so mock sdkconfig.h, esp_log.h,
REM                freertos/*.h, esp_now.h etc. are found before real ESP-IDF ones.
REM   -Icomponents/*/include comes second for real component headers.

set INC=-Itests/mocks -Itests -Icomponents/interpreter/include -Icomponents/espnow_comm/include -Icomponents/hw_drivers/include -Icomponents/lcd_touch/include -Icomponents/ui_lvgl/include -Icomponents/web_console/include
set SRC=tests/main.cpp ^
  tests/lexer_test.cpp ^
  tests/parser_test.cpp ^
  tests/interpreter_test.cpp ^
  tests/builtins_test.cpp ^
  tests/environment_test.cpp ^
  tests/protocol_test.cpp ^
  tests/peer_mgr_test.cpp ^
  tests/touch_logic_test.cpp ^
  tests/ui_sensor_model_test.cpp ^
  tests/mic_level_test.cpp ^
  tests/script_normalizer_test.cpp ^
  tests/mocks/comm_stubs.cpp ^
  components/interpreter/src/lexer.cpp ^
  components/interpreter/src/parser.cpp ^
  components/interpreter/src/interpreter.cpp ^
  components/interpreter/src/ast.cpp ^
  components/interpreter/src/environment.cpp ^
  components/interpreter/src/intern.cpp ^
  components/interpreter/src/builtins.cpp ^
  components/espnow_comm/src/peer_mgr.cpp ^
  components/espnow_comm/src/protocol.cpp ^
  components/lcd_touch/src/touch_logic.cpp ^
  components/ui_lvgl/src/ui_sensor_model.cpp ^
  components/hw_drivers/src/mic_level.cpp ^
  components/web_console/src/script_normalizer.cpp

echo Compiling ESP-LEGO unit tests...
g++ -std=c++17 -Wall -Wno-unused-function -Wno-unused-variable %INC% %SRC% -o tests/test_runner.exe 2>&1

if %ERRORLEVEL% equ 0 (
    echo.
    echo COMPILATION OK
    echo.
    tests\test_runner.exe
) else (
    echo.
    echo COMPILATION FAILED
    exit /b 1
)
