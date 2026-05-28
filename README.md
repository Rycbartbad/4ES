# ESP-LEGO 离线传感器测试工程

这个 worktree 是一个用于单板传感器 bring-up 的离线测试固件。它面向一块 ESP32-S3 开发板和一个传感器，不启动 Wi-Fi、ESP-NOW、peer discovery，也不依赖 Master/Sensor 的正常数据请求流程。

固件会周期性读取当前选中的传感器，并把紧凑的 key-value 日志打印到串口，方便在 VS Code 终端里边操作传感器边观察数值变化。

## 和 `sensor1` 是否完全一致

不完全一致。

离线测试工程保留了 `sensor1/main/app_main_sensor.cpp` 中的传感器选择思路和大部分硬件假设，但不是逐行复制。

主要区别：

- `sensor1` 是在线 Sensor 角色固件。它等待 ESP-NOW `MSG_DATA_REQ`，读取传感器后用 `protocol_build_data_resp()` 打包，再回传给 Master。
- 离线测试工程不等待 ESP-NOW 请求，而是在 `offline_sensor_task()` 中本地主动读取传感器，并按 `OFFLINE_SENSOR_LOG_INTERVAL_MS` 周期打印串口日志。
- 离线测试工程为 DHT11、BH1750、JW01 增加了更明确的状态和错误日志，更适合硬件调试。
- `USE_SENSOR_ADC_RAW` 在离线工程中是显式模式；在 `sensor1` 中，ADC raw 是没有启用具名传感器时的 fallback 路径。
- `USE_SENSOR_PRESSURE` 是离线工程新增的 AO/DO 压力模块测试模式，`sensor1` 中没有。
- 当前离线工程默认启用的是 `USE_SENSOR_PRESSURE`。

各传感器和 `sensor1` 的接近程度：

| 传感器 | 与 `sensor1` 的关系 |
| --- | --- |
| 数字 GPIO 震动/雨滴 | 默认 GPIO6 思路一致，离线工程直接打印串口日志。 |
| JW01 UART | 同样使用 UART1、TX GPIO16、RX GPIO15、9600 baud，并保留临时字段解析；离线工程额外打印原始字节和状态。 |
| BH1750 I2C | SDA/SCL、地址、lux 换算思路一致；离线工程额外检查并打印 I2C 错误。 |
| DHT11 | 同样基于 GPIO13 的单总线时序读取；离线工程额外提供失败原因和可选扫 pin。 |
| ADC raw | 同样通过 `hw_adc_read()` 测 GPIO4/GPIO5/GPIO6。 |
| AO/DO 压力模块 | 离线工程新增模式。 |

## 选择要测试的传感器

编辑：

```text
main/app_main_sensor.cpp
```

在文件顶部附近找到下面这组宏。每次只能把一个 `USE_SENSOR_*` 设为 `1`，其他都必须是 `0`：

```cpp
#define USE_SENSOR_DHT11     0
#define USE_SENSOR_VIBRATION 0
#define USE_SENSOR_RAINDROP  0
#define USE_SENSOR_BH1750    0
#define USE_SENSOR_JW01      0
#define USE_SENSOR_ADC_RAW   0
#define USE_SENSOR_PRESSURE  1
```

如果同时启用了多个传感器，编译会故意失败，防止测试结果混乱。

日志周期由下面这个宏控制：

```cpp
#define OFFLINE_SENSOR_LOG_INTERVAL_MS  50
```

- 敲击、拍打、快速压力变化测试：建议用 `50`。
- 慢速观察温湿度、光照等稳定传感器：可以改成 `500` 或 `1000`，日志更清爽。

改了传感器宏、引脚或日志周期后，都需要重新 build 和 flash。

## 不同传感器需要改哪里

### DHT11 温湿度传感器

启用：

```cpp
#define USE_SENSOR_DHT11 1
```

修改数据引脚：

```cpp
#define DHT11_PIN GPIO_NUM_13
```

如果已经确认接在哪个固定引脚，建议关闭扫 pin：

```cpp
#define DHT11_PIN_SCAN_ENABLED 0
```

预期日志：

```text
sensor=DHT11 status=OK temp_c=24.0 humidity_pct=56.0
sensor=DHT11 status=READ_FAIL reason=...
```

接线：

- VCC 接 3.3 V
- GND 接 GND
- DATA 接 `DHT11_PIN`

### 数字 GPIO 传感器

适合只有 `DO`、`OUT` 或数字阈值输出的模块，例如震动模块、雨滴模块、一些避障/触发类模块。

震动模块：

```cpp
#define USE_SENSOR_VIBRATION 1
#define VIBRATION_PIN GPIO_NUM_6
```

雨滴模块：

```cpp
#define USE_SENSOR_RAINDROP 1
#define RAINDROP_PIN GPIO_NUM_6
```

预期日志：

```text
sensor=VIBRATION status=OK gpio=6 raw_level=0 state=inactive
sensor=RAINDROP status=OK gpio=6 raw_level=1 state=wet
```

注意：有些模块是 active-low，也就是触发时输出低电平。此时 `raw_level` 仍然可信，但 `state` 文字可能需要在 `offline_sensor_log_reading()` 中反过来解释。

接线：

- VCC 接 3.3 V
- GND 接 GND
- DO/OUT 接配置的 GPIO

### BH1750 I2C 光照传感器

启用：

```cpp
#define USE_SENSOR_BH1750 1
```

修改 I2C 引脚或地址：

```cpp
#define I2C_MASTER_SCL_IO  GPIO_NUM_22
#define I2C_MASTER_SDA_IO  GPIO_NUM_21
#define BH1750_SENSOR_ADDR 0x23
```

有些 BH1750 模块地址是 `0x5C`，取决于 ADDR 引脚接法。

预期日志：

```text
sensor=BH1750 status=OK raw=278 lux=231.67
sensor=BH1750 status=WRITE_FAIL err=0x...
sensor=BH1750 status=READ_FAIL err=0x...
```

接线：

- VCC 接 3.3 V
- GND 接 GND
- SCL 接 `I2C_MASTER_SCL_IO`
- SDA 接 `I2C_MASTER_SDA_IO`

### JW01 UART 传感器

启用：

```cpp
#define USE_SENSOR_JW01 1
```

修改 UART、TX/RX 引脚或波特率：

```cpp
#define JW01_UART_NUM       UART_NUM_1
#define JW01_TX_PIN         GPIO_NUM_16
#define JW01_RX_PIN         GPIO_NUM_15
#define JW01_BAUD_RATE      9600
```

预期日志：

```text
sensor=JW01 status=NO_BYTES len=0
sensor=JW01 status=SHORT_FRAME len=...
sensor=JW01 status=OK len=... raw_hex=... co2_raw=... tvoc_raw=... ch2o_raw=...
```

接线：

- VCC 按模块要求接电源，但 ESP32-S3 GPIO 逻辑电平必须是 3.3 V
- GND 接 GND
- ESP32 TX 接传感器 RX
- ESP32 RX 接传感器 TX

JW01 当前解析仍是临时假设。拿到 datasheet 前，`co2_raw`、`tvoc_raw`、`ch2o_raw` 只能当作原始字段，不要直接当成可靠物理单位。

### ADC Raw 模拟量测试

适合只测试 AO 模拟输出，不关心数字阈值 DO 的情况。

启用：

```cpp
#define USE_SENSOR_ADC_RAW 1
```

修改采样引脚：

```cpp
static const uint8_t s_adc_pins[SENSOR_ADC_COUNT] = {4, 5, 6};
```

预期日志：

```text
sensor=ADC_RAW status=OK gpio4_raw=123 gpio5_raw=456 gpio6_raw=789
```

当前 `hw_adc_read()` 把 ESP32-S3 的 GPIO1 到 GPIO10 映射到 ADC1 channel。除非更新过驱动，ADC 测试建议优先使用 GPIO1 到 GPIO10。

### AO/DO 压力模块

适合 `AO`、`DO`、`VCC`、`GND` 四针压力模块。

启用：

```cpp
#define USE_SENSOR_PRESSURE 1
```

修改 AO/DO 引脚：

```cpp
#define PRESSURE_AO_PIN GPIO_NUM_4
#define PRESSURE_DO_PIN GPIO_NUM_6
```

预期日志：

```text
sensor=PRESSURE_AO_DO status=OK ao_gpio=4 ao_raw=80 do_gpio=6 do_level=1 do_state=high
```

接线：

- VCC 接 3.3 V
- GND 接 GND
- AO 接 `PRESSURE_AO_PIN`
- DO 接 `PRESSURE_DO_PIN`

敲击或按压测试时，建议保持：

```cpp
#define OFFLINE_SENSOR_LOG_INTERVAL_MS  50
```

判断方式：

- 敲击/按压时 `ao_raw` 有明显变化，说明 AO 模拟输出在响应。
- `do_level` 一直不变时，先调模块电位器阈值，再检查 DO 是否真的接到配置的 GPIO。
- 如果 DO 悬空且代码启用了内部上拉，它也可能一直显示 high。

## Windows 下构建、烧录和查看串口

传感器类型是在代码里选的。换传感器时，通常只需要改 `USE_SENSOR_*` 和对应引脚常量；命令本身不因为传感器类型变化而变化。

不同 Windows 电脑之间通常只需要改：

- 工程路径
- ESP-IDF 安装路径
- 串口号
- 少数非标准安装才需要改 ESP-IDF tools 路径和 Python 环境路径

## 迁移到主工程需要哪些文件

可以做到：把本离线测试工程中的一组文件替换或新增到原主工程 `E:\4ES\4ES` 后，就能得到与当前离线验证工程相同的测试效果。

推荐以 `origin/master` 为基础创建新分支，然后只引入下面这些文件。不要直接把整个 `E:\4ES\4ES_sensor_offline_test` worktree 覆盖到主工程，因为这个 worktree 来源于 `sensor1`，直接整体覆盖会误删主工程里的测试、文档、VS Code 配置等文件。

需要替换原主工程中的文件：

| 文件 | 作用 |
| --- | --- |
| `main/app_main_sensor.cpp` | Sensor 入口改为离线传感器测试固件；周期读取当前选中的传感器并打印串口日志。 |
| `main/CMakeLists.txt` | Sensor 构建时只依赖离线测试需要的组件，避免继续拉入 ESP-NOW/Web Console 等在线模式依赖。 |
| `components/hw_drivers/src/drivers.cpp` | 修正 ADC attenuation 兼容写法，兼容 ESP-IDF 中 `ADC_ATTEN_DB_12`/`ADC_ATTEN_DB_11` 的差异。 |
| `build_sensor.bat` | 改为调用通用 PowerShell 测试脚本，方便 Windows 用户 build/flash/monitor。 |
| `sdkconfig.defaults.sensor` | 保留 Sensor 角色默认配置，并说明这是离线测试固件默认配置。 |
| `README.md` | 改为中文离线测试说明文档。 |

需要新增到主工程中的文件：

| 文件 | 作用 |
| --- | --- |
| `sensor_test.ps1` | 推荐入口脚本，支持 `ports`、`build`、`flash`、`monitor`、`flash-monitor`。 |
| `export_idf.ps1` | 可配置的 ESP-IDF 环境导出脚本，自动优先寻找 ESP-IDF v5.2.6。 |
| `docs/offline_sensor_requirements.md` | 离线传感器测试需求说明。 |
| `docs/offline_sensor_architecture.md` | 离线传感器测试架构说明。 |

不需要替换的主工程文件：

- `.vscode/`
- `.devcontainer/`
- `.clangd`
- `tests/`
- `docs/design.md`
- `docs/phases.md`
- `build_master.bat`
- `sdkconfig.defaults.master`
- `components/espnow_comm/`
- `components/interpreter/`
- `components/web_console/`
- `components/script_io/`

也就是说，离线测试分支应该是“在主工程基础上增加离线 Sensor 测试能力”，而不是把主工程裁剪成只剩 Sensor 测试工程。

### 推荐方式：使用脚本

在 VS Code 的 PowerShell 终端中运行：

```powershell
cd E:\4ES\4ES_sensor_offline_test
.\sensor_test.ps1 ports
.\sensor_test.ps1 build
.\sensor_test.ps1 flash-monitor -Port COM19
```

说明：

- `ports`：列出当前可见串口。
- `build`：编译 Sensor 离线测试固件。
- `flash-monitor`：烧录后直接进入串口 monitor。
- `-Port COM19`：把 `COM19` 改成你电脑上 ESP32-S3 对应的串口。

退出 monitor：

```text
Ctrl+]
```

`sensor_test.ps1` 会自动探测常见 ESP-IDF v5.2.6 安装位置。如果自动探测失败，可以显式传路径：

```powershell
.\sensor_test.ps1 build `
  -IdfPath "C:\Users\Fancy\esp\v5.2.6\esp-idf" `
  -IdfToolsPath "D:\ESPIDF\IDF_5_1_2\TOOLS" `
  -IdfPythonEnvPath "D:\ESPIDF\IDF_5_1_2\TOOLS\python_env\idf5.2_py3.11_env"

.\sensor_test.ps1 flash-monitor `
  -Port COM19 `
  -IdfPath "C:\Users\Fancy\esp\v5.2.6\esp-idf" `
  -IdfToolsPath "D:\ESPIDF\IDF_5_1_2\TOOLS" `
  -IdfPythonEnvPath "D:\ESPIDF\IDF_5_1_2\TOOLS\python_env\idf5.2_py3.11_env"
```

也可以使用批处理包装脚本：

```bat
build_sensor.bat
build_sensor.bat flash-monitor COM19
```

### 手动命令

如果不想使用脚本，也可以手动执行等价命令。某些 ESP-IDF 安装会把 framework 和 tools 放在不同目录，此时需要先设置 tools 路径：

```powershell
$ProjectDir = "E:\4ES\4ES_sensor_offline_test"
$IDFPath = "C:\Users\Fancy\esp\v5.2.6\esp-idf"
$Port = "COM19"

$env:IDF_TOOLS_PATH = "D:\ESPIDF\IDF_5_1_2\TOOLS"
$env:IDF_PYTHON_ENV_PATH = "D:\ESPIDF\IDF_5_1_2\TOOLS\python_env\idf5.2_py3.11_env"
Remove-Item Env:ESP_IDF_VERSION -ErrorAction SilentlyContinue

cd $ProjectDir
$env:IDF_TARGET = "esp32s3"
& "$IDFPath\export.ps1"

python "$IDFPath\tools\idf.py" -B "build\sensor" `
  -DSDKCONFIG="build\sensor\sdkconfig" `
  -DSDKCONFIG_DEFAULTS="sdkconfig.defaults;sdkconfig.defaults.sensor" `
  build

python "$IDFPath\tools\idf.py" -B "build\sensor" `
  -DSDKCONFIG="build\sensor\sdkconfig" `
  -p $Port `
  flash monitor
```

优先推荐使用 `sensor_test.ps1`。手动命令主要用于排查脚本无法自动识别环境的情况。

## 查找 COM 串口

PowerShell 中运行：

```powershell
[System.IO.Ports.SerialPort]::GetPortNames()
```

或者：

```powershell
Get-PnpDevice -Class Ports | Select-Object Status,FriendlyName,InstanceId
```

把 ESP32-S3 对应的串口填到 `-Port` 后面，例如：

```powershell
.\sensor_test.ps1 flash-monitor -Port COM19
```

## 标准测试流程

1. 选择一个传感器，把对应 `USE_SENSOR_*` 设为 `1`，其他设为 `0`。
2. 修改该传感器的 GPIO、UART、I2C 或 ADC 配置。
3. 按 README 中对应传感器接线，注意 ESP32-S3 GPIO 不能超过 3.3 V。
4. 运行 `.\sensor_test.ps1 build`。
5. 运行 `.\sensor_test.ps1 flash-monitor -Port COMx`。
6. 在串口日志中确认 `status=INIT_OK` 和周期性 sensor 日志。
7. 改变传感器输入，观察 raw 值、数字电平或物理量是否变化。

每次换传感器、换引脚、改波特率或改 I2C 地址后，都要重新 build 和 flash。

## 快速判断硬件是否正常

正常迹象：

- 能看到启动日志。
- 能看到 `status=INIT_OK`。
- 能看到周期性传感器日志。
- 改变传感器输入时，raw 值、数字电平或物理量跟着变化。

常见异常：

- 没有 COM 口：检查 USB 线、驱动、开发板供电。
- 烧录连接失败：检查 COM 口是否选错，必要时手动按 BOOT/RESET。
- `READ_FAIL`、`NO_BYTES` 或 I2C 失败：检查接线、引脚宏、电压、上拉电阻、I2C 地址或 UART 波特率。
- ADC 值长期接近 0 或 4095：检查 AO 接线、电压范围，以及所选 GPIO 是否支持 ADC。
- DO 一直 high 或一直 low：调模块电位器阈值，确认模块是 active-high 还是 active-low，并检查 DO 是否悬空。
