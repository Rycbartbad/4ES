# ESP-LEGO 从机配置与烧录指南

## 1. 当前构建模型

Master 使用 ESP32-S3 固件，从机使用 ESP32-C3 固件。所有从机目前共用 `main/app_main_sensor.cpp`，编译前通过宏选择硬件能力；代码不会在启动时自动判断接的是 Doorbell、DHT11 还是其他模块。

相关文件：

- `main/app_main_sensor.cpp`：选择传感器、蜂鸣器和舵机功能。
- `sdkconfig.defaults.sensor`：从机默认角色、芯片和模块名。
- `sdkconfig.sensor`：构建脚本保存的实际从机配置。
- `scripts/build_sensor.ps1`：配置 ESP-IDF、编译、烧录和监视串口。

每次给不同硬件烧录前，必须确认“硬件功能宏”和“模块名”一致，不能只改名字。

## 2. Doorbell 固件的具体配置

### 2.1 选择功能

打开 `main/app_main_sensor.cpp`，把功能选择区改为：

```cpp
#define USE_SENSOR_DHT11     0
#define USE_SENSOR_VIBRATION 0
#define USE_SENSOR_RAINDROP  0
#define USE_SENSOR_BH1750    0
#define USE_SENSOR_JW01      0

#define USE_BUZZER           1
#define USE_SERVO            0
```

Doorbell 当前使用无源蜂鸣器 PWM，代码默认引脚为 GPIO4。不要同时启用 `USE_SERVO`，因为当前蜂鸣器和舵机都使用 GPIO4，会发生引脚冲突。

### 2.2 设置模块名

把以下配置改为：

```text
CONFIG_SENSOR_MODULE_NAME="doorbell"
```

至少检查：

- `sdkconfig.defaults.sensor`
- `sdkconfig.sensor`（如果该文件已经存在）

构建脚本会优先恢复已有的 `sdkconfig.sensor`，所以只修改 defaults 文件可能不会改变已经保存的配置。构建后也可在生成的 `sdkconfig` 中确认最终值。

### 2.3 连接正确的板子

1. 暂时拔掉其他 ESP32 开发板，避免自动检测选错设备。
2. 用支持数据传输的 USB 线连接要烧录的 ESP32-C3 从机。
3. 在 PowerShell 查看端口：

```powershell
[System.IO.Ports.SerialPort]::GetPortNames()
```

记下新出现的端口，例如 `COM5`。

### 2.4 编译、烧录并打开监视器

在项目根目录执行：

```powershell
cd C:\Users\jjy\Desktop\4ES\4ES
.\scripts\build_sensor.ps1 -Monitor -Port COM5
```

`-Monitor` 会依次编译、烧录并打开串口监视器。如果只想编译和烧录：

```powershell
.\scripts\build_sensor.ps1 -Flash -Port COM5
```

不写 `-Port` 时脚本会自动寻找 ESP32-C3，但同时连接多块 C3 时无法保证选中你想烧的那一块。

如果无法进入下载模式：按住从机的 BOOT，短按 RESET，松开 BOOT，然后重新执行烧录命令。不同开发板的按键标记可能不同。

### 2.5 验证结果

串口应至少确认：

- 芯片目标为 ESP32-C3。
- 启动日志包含 `Sensor ready — name=doorbell`。
- Master 随后发现名为 `doorbell` 的 peer。

按 `Ctrl+]` 退出 ESP-IDF 串口监视器。

## 3. 其他从机如何配置

原则是每次只开启实际存在的功能：

| 从机 | 功能宏 | 建议模块名 |
| --- | --- | --- |
| DHT11 | `USE_SENSOR_DHT11=1`，其余 Sensor=0 | `dht11` 或 `room_dht11` |
| BH1750 | `USE_SENSOR_BH1750=1`，其余 Sensor=0 | `bh1750` |
| 振动 | `USE_SENSOR_VIBRATION=1`，其余 Sensor=0 | `vibration` |
| 雨滴 | `USE_SENSOR_RAINDROP=1`，其余 Sensor=0 | `raindrop` |
| JW01 | `USE_SENSOR_JW01=1`，其余 Sensor=0 | `jw01` |
| Doorbell | 所有 Sensor=0，`USE_BUZZER=1` | `doorbell` |
| 舵机 | 所有 Sensor=0，`USE_SERVO=1` | `servo` |

完成配置后，对目标板重复同一条命令，只替换实际 COM 口：

```powershell
.\scripts\build_sensor.ps1 -Monitor -Port COM端口号
```

不要拿上一次编译好的固件直接烧给另一类硬件。每次切换设备类型后都应重新 build；最好在烧录前检查编译日志中的配置和串口启动后的模块名。

## 4. 纯执行器行为

当所有 Sensor 宏为 0、只开启 `USE_BUZZER` 或 `USE_SERVO` 时：

- capability 只广播实际执行器能力，不再伪装成 Generic ADC。
- Master UI 不对纯执行器发起周期性读取。
- 详情页显示 `Actuator device / No trend data`，不创建无意义的趋势图。
- Doorbell 仍可通过 `buzzer_beep`、`buzzer_note` 等命令控制。

## 5. 多台从机的身份和容量

- 模块名用于人类识别和能力匹配，建议每台设备使用清晰且尽量唯一的名称，例如 `doorbell_front`、`doorbell_back`。
- `module_id` 由 Master 在设备加入时动态分配，不是烧录进从机的固定编号。
- 当前 Master 的 `CONFIG_MAX_PEERS=20`，通信层最多登记 20 台从设备。
- 当前板载 UI 只有 4 个卡片位置，只显示/轮询前 4 台。
- 若要在屏幕上管理超过 4 台，需另外实现分页或可滚动设备列表；单纯把 peer 上限调大不够。

## 6. 常见问题

### 烧录脚本找不到 ESP32-C3

- 确认 USB 线支持数据而非仅充电。
- 在设备管理器中查看是否出现新的 COM 口。
- 手动传入 `-Port COMx`。
- 必要时用 BOOT + RESET 进入下载模式。

### 烧完仍显示旧设备名

- 检查 `sdkconfig.sensor`，因为脚本可能恢复了旧配置。
- 检查构建后的根目录 `sdkconfig` 中 `CONFIG_SENSOR_MODULE_NAME` 的最终值。
- 确认烧录端口对应的是目标板。

### Master 没有马上发现从机

- 等待至少一个 announce 周期，默认约 3 秒。
- 确认 Master 和从机均已启动，ESP-NOW 使用相同的工作信道。
- 查看双方串口日志，而不是只根据屏幕空卡片判断通信是否正常。
