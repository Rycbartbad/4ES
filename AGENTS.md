# ESP-LEGO — Agent Guide

## Project status

P0 skeleton: `main/hello_world_main.c` (C) + empty `build/`. The architecture in `docs/design.md` (interpreter, ESP-NOW comm, hw_drivers, script_io) has **no code yet**. All `components/` subdirectories are absent. Phase plan at `docs/phases.md`.

## Toolchain (Windows, local)

ESP-IDF v5.2.6, chip target **esp32s3** (VS Code default). Verified paths:
- `IDF_PATH`: `d:\IDF_v5.2.6\v5.2.6\esp-idf`
- Toolchain: `d:\IDF_v5.2.6\TOOLS_PATH\tools\xtensa-esp-elf\esp-13.2.0_20250707\xtensa-esp-elf\bin\`
- clangd: `d:\IDF_v5.2.6\TOOLS_PATH\tools\esp-clang\16.0.1-fe4f10a809\esp-clang\bin\clangd.exe`

VS Code `idf.customExtraVars` sets `IDF_TARGET=esp32s3`.

## Build & dev commands

```powershell
& 'd:\IDF_v5.2.6\v5.2.6\esp-idf\export.ps1'
idf.py set-target esp32s3
idf.py build
idf.py flash monitor
idf.py menuconfig
```

`idf.py` requires IDF environment exported first.

## Architecture (planned — read before writing code)

| Directory | Role |
|-----------|------|
| `components/interpreter/` | Script lexer, parser, AST, interpreter (C++17) |
| `components/espnow_comm/` | ESP-NOW protocol, peer manager, sync request |
| `components/hw_drivers/` | GPIO/ADC/PWM abstraction |
| `components/script_io/` | UART script input |
| `main/` | App entry (`app_main.cpp`), Kconfig role selection |

Device roles via Kconfig: `CONFIG_DEVICE_ROLE_MASTER` (full) / `CONFIG_DEVICE_ROLE_SENSOR` (lite). Conditionally compiled in `espnow_comm`.

Key design constraints (from `docs/design.md`):
- No `std::vector`, `std::map`, `std::string`, dynamic allocation, or C++ exceptions
- Static object pools for AST, lists, functions, bindings — all Kconfig-tunable
- Script language: BNF-defined subset with `var`, `if`/`else`, `while`, `func`/`return`, lists via builtins
- ESP-NOW sync request: single semaphore, serial only, 500ms timeout, MAC-level response filtering
- Peer ID: composite key `MAC[6] + module_id[1]`, TOCTOU-safe copies required

## Language conventions

- Current source is **C** (`hello_world_main.c`). Design calls for **C++17** (`app_main.cpp`). Migration path: rename `.c` → `.cpp`, update `main/CMakeLists.txt`, add `set(CMAKE_CXX_STANDARD 17)`.
- `#include "sdkconfig.h"` always first (ESP-IDF convention).
- C++ header style: `<component>/include/<component>/header.h`.
- `.clangd` strips `-f*` and `-m*` flags from compile_commands.

## Testing

Hardware integration tests use `pytest-embedded` framework (`pytest_hello_world.py`). Markers: `@pytest.mark.generic`, `@pytest.mark.host_test`, `@pytest.mark.qemu`. Dev container at `.devcontainer/` provides QEMU environment.

Run: `pytest pytest_hello_world.py` (requires connected DUT or QEMU).

## Git

No commits yet (empty master branch). No conventions established.
