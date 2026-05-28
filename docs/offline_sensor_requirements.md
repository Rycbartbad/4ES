# 离线传感器测试需求文档

状态：需求已冻结，可进入代码实现。

## 1. 目标

在 `sensor_offline_test` 分支上创建一个本地/离线传感器测试固件。

第一阶段的目标不是完成完整的 Master/Sensor 联网通信，而是让初学者只用一块 ESP32-S3 开发板，就能完成：

1. 连接传感器。
2. 编译固件。
3. 烧录到 ESP32-S3。
4. 打开串口监视器。
5. 实时看到传感器读数日志。

这样可以先单独验证每个传感器是否接线正确、读数是否合理，再把稳定的传感器读取逻辑接回 ESP-NOW 通信流程。

## 2. 工程来源与当前状态

本方案基于当前工程结构和 `sensor1` 分支代码整理：

- 完整主工程：`E:\4ES\4ES`
- 离线测试分支 worktree：`E:\4ES\4ES_sensor_offline_test`
- 传感器参考分支 worktree：`E:\4ES\4ES_sensor1`
- 传感器参考文件：`main/app_main_sensor.cpp`

当前重要问题：`sensor_offline_test` 是从 `sensor1` 创建出来的，而 `sensor1` 目前缺少完整 ESP-IDF 顶层工程文件，例如：

- `CMakeLists.txt`
- `sdkconfig.defaults`
- `sdkconfig.defaults.sensor`
- `build_sensor.bat`
- `export_idf.ps1`
- README、测试文件和原始设计文档

因此，正式写代码前，需要先让 `sensor_offline_test` 成为一个可以独立编译的完整 ESP-IDF 工程。

提交策略确认：

- 第一版代码应尽量保持和 `sensor1` 一样的组织方式和逻辑。
- 离线测试只在 `sensor1` 基础上删除或隔离 Wi-Fi/ESP-NOW 通信部分。
- 传感器读取部分优先复用 `sensor1/main/app_main_sensor.cpp` 中已有实现。
- 最后提交到仓库时，尽量只提交关键修改文件，不提交与 `master` 重合的工程文件。
- 如果本地为了编译需要临时补齐顶层工程文件，应在最终提交前确认这些文件是否真的需要进入 commit。

## 3. 目标硬件

开发板：

- 类似 Espressif 官方 ESP32-S3 开发板的 44 引脚引出版。
- 开发板引出了 44 个 ESP32-S3 引脚，并带有丝印。
- 当前确认：GPIO 状况正常，可按丝印接线使用。

目前在 `sensor1/main/app_main_sensor.cpp` 中看到的传感器如下：

| 传感器 | 通信/读取方式 | `sensor1` 当前引脚 | 预期读数 |
| --- | --- | --- | --- |
| DHT11 | 单 GPIO 时序协议 | GPIO13 | 温度、湿度 |
| 震动模块 | 数字 GPIO | GPIO6 | 有震动/无震动 |
| 雨滴模块 | 数字 GPIO | GPIO6 | 有雨/无雨，或干/湿 |
| BH1750 | I2C | SCL GPIO22，SDA GPIO21 | 光照强度 |
| JW01 气体传感器 | UART | TX GPIO16，RX GPIO15，UART1，9600 baud | 气体相关数值，具体单位依赖 datasheet |
| ADC 原始值测试 | ADC | GPIO4，GPIO5，GPIO6 | ADC 原始读数 |

已确认的硬件条件：

- GPIO 使用状态正常。
- DHT11 模块自带上拉电阻，接线后即可使用。
- BH1750 模块自带 I2C 上拉电阻，接线后即可使用。
- GPIO 设计沿用 `sensor1`。
- DHT11 沿用 `sensor1` 中的 GPIO13。
- 传感器供电使用 3.3 V。

暂不处理的问题：

- JW01 的准确模块型号、购买链接或 datasheet 是否可获得。

第一版已确认测试对象：

- 先测试湿度传感器，即当前 `sensor1` 代码中的 DHT11。
- DHT11 会同时输出温度和湿度；本阶段重点关注湿度读数是否正常。
- JW01 暂时没有 datasheet，因此第一版不优先测试 JW01。

## 4. 面向使用者的成功标准

第一次离线测试成功，应该满足：

1. ESP-IDF 环境可以正常导出。
2. `sensor_offline_test` 可以作为 ESP32-S3 固件成功编译。
3. 固件可以烧录到一块 ESP32-S3 开发板。
4. 串口监视器能看到启动日志。
5. 串口监视器能周期性打印某个传感器读数。
6. 当传感器输入发生变化时，日志中的数值或状态也能发生可理解的变化。
7. 如果读取失败，日志能说明是哪个传感器失败，并给出有用的状态信息。

## 5. 功能需求

### FR-1 离线传感器固件必须可独立编译

`sensor_offline_test` worktree 必须变成完整 ESP-IDF 工程，并且可以独立编译。

建议做法：

- 第一优先级是保持 `sensor1` 的分支逻辑和文件组织，不做大范围重构。
- 如本地编译确实缺少必要顶层工程文件，可以临时从 `master` 补齐用于本地验证。
- 最终 commit 前，需要区分“为了本地编译临时补齐的重合文件”和“真正需要提交的关键修改文件”。
- 后续实现只放在 `sensor_offline_test` 中。
- 不改动 `master` 和纯参考用的 `sensor1` worktree。

### FR-2 第一版每次只启用一个传感器

第一版实现中，建议每次固件只启用一个传感器。

第一版确认方向：

- 暂时沿用 `sensor1` 里的编译期宏，例如 `USE_SENSOR_JW01`。
- 保持和 `sensor1` 一样的传感器选择逻辑，减少代码变化。

后续可选优化：

- 再迁移到 Kconfig 菜单，例如 `CONFIG_OFFLINE_SENSOR_TYPE_JW01`。
- Kconfig 对初学者更友好，但不是第一版必须项。

### FR-3 串口日志输出

固件必须按固定周期打印传感器读数。

默认周期：

- 500 ms。

建议日志格式：

```text
I (12345) offline_sensor: sensor=BH1750 status=OK lux=231.67
I (13345) offline_sensor: sensor=JW01 status=OK co2_raw=482 tvoc_raw=17 ch2o_raw=3
W (14345) offline_sensor: sensor=DHT11 status=READ_FAIL
```

### FR-4 尽可能同时保留原始值和解释后的值

为了方便初学者 debug，日志应该尽可能同时显示：

- 硬件或协议读到的原始值；
- 在换算可靠时，再显示转换后的物理量。

例子：

- ADC：先显示 raw 原始计数；只有确认衰减和校准后，再显示电压估算值。
- BH1750：可以显示 lux。
- DHT11：可以显示摄氏度和相对湿度百分比。
- 数字模块：显示 GPIO 原始电平和解释后的状态。
- JW01：第一版先显示原始解析字段；只有 datasheet 确认后，再显示带单位的物理量。

第一版 DHT11 日志要求：

- 日志使用英文。
- 打印温度和湿度。
- 湿度字段建议使用 `humidity_pct`，表示相对湿度百分比。
- 温度字段建议使用 `temp_c`，表示摄氏度。
- 读取失败时打印 `status=READ_FAIL`。

### FR-5 离线测试阶段隔离 Wi-Fi/ESP-NOW

离线测试模式不应该依赖 Master 发来的 ESP-NOW 数据包，也不应该依赖 Wi-Fi 初始化。

第一版确认方向：

- 基于 `sensor1` 删除或隔离 Wi-Fi/ESP-NOW 通信部分。
- 不初始化 ESP-NOW。
- 不创建 announce task。
- 不注册 ESP-NOW receive callback。
- 不需要收到 `DATA_REQ`。
- 不需要 peer discovery。
- 不需要第二块 ESP32-S3。
- 保留传感器读取逻辑，并改为由本地周期任务主动读取和打印。

### FR-6 保留未来在线集成路径

传感器读取函数应设计成可以复用，后续同一套读取逻辑可以被两个路径调用：

- 离线日志任务；
- ESP-NOW `MSG_DATA_REQ` 响应处理。

## 6. 非功能需求

- 使用 ESP-IDF v5.2.6。
- 目标芯片是 ESP32-S3。
- 使用 C++17，与当前工程一致。
- 尽量避免动态内存分配，贴合当前工程约束。
- 日志必须清晰、适合初学者阅读。
- 传感器读取不能长时间阻塞到触发 watchdog。
- 初始传感器验证不依赖 Master 开发板。

## 7. 第一版不做的事情

- 不改完整 Web Console。
- 不改 AI 脚本生成逻辑。
- 不同时启用多个传感器。
- 不做自动传感器识别。
- 不做长期校准。
- 不接云端或互联网。
- 不立即完成最终 ESP-NOW 联调打磨。

## 8. 硬件安全注意事项

- ESP32-S3 GPIO 输入电压不能超过 3.3 V。
- UART TX/RX 要交叉连接：ESP32 TX 接传感器 RX，ESP32 RX 接传感器 TX。
- I2C 的 SDA/SCL 需要上拉电阻。
- ADC 原始值不是可靠电压，除非做了衰减配置和 ADC 校准。
- DHT11 对时序敏感，可能受 Wi-Fi 和 FreeRTOS 调度影响。
- 某些 ESP32-S3 开发板上，一些 GPIO 可能被板载外设占用或不适合使用。

## 9. 需求冻结前需要确认

开始代码实现前，请先确认：

1. 第一版优先测试 DHT11 湿度传感器：已确认。
2. JW01 datasheet：暂时没有，第一版不优先处理。
3. 串口日志周期：已确认为 500 ms。
4. 日志语言：已确认为英文，格式采用紧凑 key-value。
5. 最终 commit：已确认只提交关键文件。

需求已冻结：

- 第一版 DHT11 使用 GPIO13。
- 供电使用 3.3 V。
- 后续可以开始代码实现。
