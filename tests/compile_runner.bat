@echo off
REM Windows x86 build for ESP-LEGO script runner
REM Requires: MinGW g++ with C++17 support
REM
REM Include path order is CRITICAL:
REM   -Itests/mocks MUST come first so mock headers override real ESP-IDF ones.
REM   -Icomponents/*/include comes second for real component headers.

set INC=-Itests/mocks -Icomponents/interpreter/include -Icomponents/espnow_comm/include -Icomponents/hw_drivers/include
set SRC=tests/script_runner.cpp ^
  tests/mocks/comm_stubs.cpp ^
  components/interpreter/src/lexer.cpp ^
  components/interpreter/src/parser.cpp ^
  components/interpreter/src/interpreter.cpp ^
  components/interpreter/src/ast.cpp ^
  components/interpreter/src/environment.cpp ^
  components/interpreter/src/intern.cpp ^
  components/interpreter/src/builtins.cpp ^
  components/espnow_comm/src/peer_mgr.cpp ^
  components/espnow_comm/src/protocol.cpp

echo Compiling ESP-LEGO script runner...
g++ -std=c++17 -Wall -Wno-unused-function -Wno-unused-variable %INC% %SRC% -o tests\script_runner.exe 2>&1

if %ERRORLEVEL% equ 0 (
    echo.
    echo COMPILATION OK
    echo.
    echo Usage:
    echo   tests\script_runner.exe "var x = 1 + 2; print(x);"
    echo   type script.txt ^| tests\script_runner.exe
    echo   tests\script_runner.exe @script.txt
) else (
    echo.
    echo COMPILATION FAILED
    exit /b 1
)
