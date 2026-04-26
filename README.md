# ESP-LEGO — 分布式硬件控制系统

在 ESP32-S3 上使用 ESP-IDF v5.2.6 + C++17 构建。Master 运行轻量脚本解释器，Sensor 通过 ESP-NOW 通信。AI 根据自然语言生成脚本，由 Master 解释执行。

## 硬件要求

- 开发板：ESP32-S3（推荐 2 块，1× Master + 1× Sensor）
- USB 数据线（支持串口通信）

## 环境配置（Windows）

```powershell
# 初始化 ESP-IDF 环境
& 'd:\IDF_v5.2.6\v5.2.6\esp-idf\export.ps1'

# 设置目标芯片
idf.py set-target esp32s3
```

## 选择设备角色

项目支持 **Master**（完整解释器 + ESP-NOW 管理器）和 **Sensor**（ESP-NOW 从机）两种角色，通过 Kconfig 切换：

```powershell
idf.py menuconfig
# Component config → ESP-LEGO Device Role → Device role
#   → Master (interpreter + ESP-NOW manager)    # 主控
#   → Sensor (ESP-NOW slave)                    # 传感器
```

保存后退出，CMake 根据 `main/CMakeLists.txt` 自动选择 `app_main.cpp` 或 `app_main_sensor.cpp`。

## 编译 & 烧录

### VS Code 图形界面

底部状态栏：点击 🔥 火焰图标烧录，点击 📺 显示器图标查看串口输出。

> ⚠️ 如果报 `esp_usb_jtag: could not find or open device`：`Ctrl+Shift+P` → `ESP-IDF: Select Flash Method` → 选择 **UART**。

### PowerShell 命令行

```powershell
& 'd:\IDF_v5.2.6\v5.2.6\esp-idf\export.ps1'

# === 编译 ===
idf.py build

# === 烧录（COM3 替换为实际串口号）===
idf.py -p COM3 flash

# === 查看串口输出 ===
idf.py -p COM3 monitor
```

`Ctrl+]` 退出 monitor。

### 两块板子（一主一从）

```powershell
# 板子 A — Master（sdkconfig 默认即 Master）
idf.py build
idf.py -p COM3 flash

# 板子 B — Sensor（切换角色后编译烧录）
idf.py menuconfig          # Device Role → Sensor → 保存
idf.py build
idf.py -p COM4 flash

# 分别打开 Monitor 观察 ESP-NOW 通信
idf.py -p COM3 monitor     # Master 端
idf.py -p COM4 monitor     # Sensor 端（另开终端）
```

### 保留两套配置（避免反复 menuconfig）

```powershell
copy sdkconfig sdkconfig.master
idf.py menuconfig          # 切 Sensor → 保存
copy sdkconfig sdkconfig.sensor

# 以后切换只需：
copy /y sdkconfig.sensor sdkconfig
idf.py build
```

## 启动验证

Master 启动后应输出：
```
ESP-LEGO V1.0 starting
Ready
```

Sensor 启动后应输出：
```
Sensor booting
Announcing to master...
```

## 目录结构

```
Esp lego/
├── components/
│   ├── interpreter/        # 脚本解释器（lexer, parser, AST, 内置函数）
│   ├── espnow_comm/        # ESP-NOW 通信层 + 节点管理器
│   ├── hw_drivers/         # GPIO/ADC/PWM 硬件抽象
│   ├── script_io/          # UART 脚本输入
│   └── web_console/        # Web 控制台
├── main/
│   ├── Kconfig.projbuild   # 设备角色 Kconfig 菜单
│   ├── CMakeLists.txt      # 根据角色条件编译
│   ├── app_main.cpp        # Master 固件入口
│   └── app_main_sensor.cpp # Sensor 固件入口
├── tests/
│   ├── compile_win.bat     # x86 单元测试入口（无需硬件）
│   ├── conftest.py         # pytest 硬件测试夹具
│   └── README.md           # 硬件测试详细说明
├── docs/
│   ├── design.md           # 架构设计文档
│   └── phases.md           # 开发阶段计划
└── build/
    └── Esp.elf             # 编译产物
```

## 测试

### x86 单元测试（无需硬件，需要 MinGW）

```powershell
.\tests\compile_win.bat
```

覆盖：词法分析器、环境作用域、节点管理器、ESP-NOW 协议编解码（共 30 个用例）。

### 硬件集成测试（需要 2× ESP32-S3）

```powershell
# 安装依赖
pip install pytest-embedded

# 设置串口
$env:MASTER_PORT="COM3"
$env:SENSOR_PORT="COM4"

# 运行冒烟测试
pytest tests/test_smoke.py -m generic -v
```

详见 `tests/README.md`。

## 架构约束

- C++17，禁用异常、RTTI、动态内存分配
- 不使用 `std::vector`、`std::map`、`std::string`
- 所有数据结构使用静态对象池（Kconfig 可调）
- `#include "sdkconfig.h"` 必须为第一个 include
