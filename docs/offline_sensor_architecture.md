# 离线传感器测试架构设计

状态：方案已冻结，可进入代码实现。

## 1. 设计意图

离线传感器测试固件应该是一个小而清晰的硬件 bring-up 层，建立在现有 ESP-IDF 工程之上。

它要支持初学者完成下面这个循环：

1. 选择一个传感器。
2. 编译并烧录。
3. 打开串口 monitor。
4. 观察实时读数。
5. 根据日志修正接线或配置，直到读数符合预期。

某个传感器离线验证稳定后，同一套读取逻辑应尽量保持与 `sensor1` 一致，后续再接回正常 Sensor 的 ESP-NOW 响应流程中。

## 2. 推荐模块拆分

### 方案 A：最小化第一版实现（第一版采用）

第一版全部放在：

- `main/app_main_sensor.cpp`

新增函数：

- `offline_sensor_init()`
- `offline_sensor_read()`
- `offline_sensor_task()`

优点：

- 实现最快。
- 初学者只需要看一个文件，理解成本低。
- 和 `sensor1` 的现有逻辑最接近。
- 最终 commit 可以只包含关键文件改动，diff 更小。

缺点：

- 文件会越来越拥挤。
- 后续复用到 ESP-NOW 在线模式时不够清晰。

### 方案 B：后续可选的清晰实现

新增一个组件：

```text
components/sensor_drivers/
|-- CMakeLists.txt
|-- Kconfig
|-- include/sensor_drivers/sensor_drivers.h
`-- src/sensor_drivers.cpp
```

应用层逻辑仍然放在：

```text
main/app_main_sensor.cpp
```

优点：

- 应用流程和硬件读取边界清楚。
- 后续更容易复用到 ESP-NOW 模式。
- 更容易逐个修复不同传感器的问题。

缺点：

- 文件数量更多，对初学者稍微复杂一点。

当前结论：第一版采用方案 A，不新增 `sensor_drivers` 组件。方案 B 作为后续重构方向保留。

## 3. 建议的数据模型

第一版可以先沿用 `sensor1` 中的局部变量和 `double values[]` 数组，保持代码变化小。

后续如果要重构为独立驱动组件，再考虑使用固定大小结构体，不使用动态分配：

```cpp
typedef enum {
    SENSOR_KIND_NONE = 0,
    SENSOR_KIND_DHT11,
    SENSOR_KIND_VIBRATION,
    SENSOR_KIND_RAINDROP,
    SENSOR_KIND_BH1750,
    SENSOR_KIND_JW01,
    SENSOR_KIND_ADC_RAW,
} sensor_kind_t;

typedef struct {
    const char* name;
    bool ok;
    int value_count;
    double values[4];
    const char* labels[4];
    const char* units[4];
    int raw[4];
    const char* status;
} sensor_reading_t;
```

后续重构时采用该模型的原因：

- `values[]` 用来保存已经可靠换算后的物理量。
- `raw[]` 用来保存协议字段或 ADC 原始值。
- `labels[]` 和 `units[]` 让串口日志更容易读。
- 当前列出的传感器最多 4 个值已经够用。

## 4. 传感器选择方式

第一版确认方向：

- 保留 `sensor1/main/app_main_sensor.cpp` 中的 `USE_SENSOR_*` 宏。
- 每次只把一个传感器宏设为 `1`。
- 不在第一版强制引入 Kconfig，避免扩大改动范围。
- 第一版将 `USE_SENSOR_DHT11` 设为 `1`，其余传感器宏设为 `0`。

后续可选：新增类似下面的 Kconfig 菜单：

```text
menu "ESP-LEGO Offline Sensor Test"

config OFFLINE_SENSOR_TEST_ENABLED
    bool "Enable offline sensor test mode"
    default y
    depends on DEVICE_ROLE_SENSOR

choice OFFLINE_SENSOR_TYPE
    prompt "Offline sensor type"
    default OFFLINE_SENSOR_TYPE_JW01

config OFFLINE_SENSOR_TYPE_DHT11
    bool "DHT11 temperature/humidity"

config OFFLINE_SENSOR_TYPE_VIBRATION
    bool "Vibration digital module"

config OFFLINE_SENSOR_TYPE_RAINDROP
    bool "Raindrop digital module"

config OFFLINE_SENSOR_TYPE_BH1750
    bool "BH1750 light sensor"

config OFFLINE_SENSOR_TYPE_JW01
    bool "JW01 gas sensor"

config OFFLINE_SENSOR_TYPE_ADC_RAW
    bool "Raw ADC test"

endchoice

config OFFLINE_SENSOR_LOG_INTERVAL_MS
    int "Offline sensor log interval (ms)"
    default 1000
    range 100 10000

endmenu
```

引脚值第一版沿用 `sensor1` 中的常量，等离线测试跑通后，再决定是否迁移到 Kconfig。

## 5. 主控制流程

离线 Sensor 固件流程：

```text
app_main()
  初始化 NVS
  如果启用离线测试:
      初始化选中的传感器驱动
      创建 offline_sensor_task
      主循环保持运行
```

第一版离线测试不执行：

- `espnow_comm_init()`
- `espnow_comm_register_recv_callback()`
- `announce_task`
- `espnow_comm_send_announce()`
- 依赖 `MSG_DATA_REQ` 的被动读取流程

离线任务流程：

```text
offline_sensor_task()
  无限循环:
      reading = sensor_read()
      用 ESP_LOGI/ESP_LOGW 打印 reading
      延时 500 ms
```

## 6. 各传感器设计

### DHT11

`sensor1` 当前引脚：

- GPIO13。

第一版最终确认：

- 沿用 GPIO13。
- DHT11 使用 3.3 V 供电。

预期输出：

- 温度，单位摄氏度。
- 湿度，单位相对湿度百分比。
- 第一版重点测试湿度读数。

风险：

- DHT11 是时序敏感的单总线协议。
- 如果读取失败，应打印 `READ_FAIL`，不要用假的 0 值冒充有效读数。

### 震动模块

`sensor1` 当前引脚：

- GPIO6。

预期输出：

- GPIO 原始电平：0 或 1。
- 解释后的状态：inactive/active，或中文“无震动/有震动”。

单位：

- 当前代码里没有物理单位。

### 雨滴模块

`sensor1` 当前引脚：

- GPIO6。

预期输出：

- GPIO 原始电平：0 或 1。
- 解释后的状态：dry/wet、no_rain/rain，或中文“干/湿”“无雨/有雨”。

单位：

- 当前代码里没有物理单位。

风险：

- 有些雨滴模块同时提供模拟输出和数字阈值输出。`sensor1` 当前只读数字 GPIO。

### BH1750

`sensor1` 当前配置：

- SCL GPIO22。
- SDA GPIO21。
- I2C 频率 100 kHz。
- 地址 `0x23`。

预期输出：

- 光照强度，单位 lux。

风险：

- I2C 需要上拉电阻。
- 地址可能是 `0x23` 或 `0x5C`，取决于模块 ADDR 引脚。

### JW01

`sensor1` 当前配置：

- UART1。
- TX GPIO16。
- RX GPIO15。
- 波特率 9600。

`sensor1` 当前解析假设：

- `data[1..2]` 解析为类似 CO2 的原始值。
- `data[3..4]` 解析为类似 TVOC 的原始值。
- `data[5..6]` 解析为类似 CH2O 的原始值。

重要说明：

- 在 JW01 datasheet 确认帧格式、缩放系数、校验方式和单位之前，这些字段应该保留为 `raw` 原始值。

第一版建议输出：

- 原始帧字节。
- 解码出的原始字段。
- 是否收到足够字节的状态。

### ADC 原始值

主工程 Sensor 当前 fallback 引脚：

- GPIO4。
- GPIO5。
- GPIO6。

预期输出：

- ADC 原始计数，12 位宽时通常约为 0 到 4095。

后续可选输出：

- 加入 ADC 校准后，再估算电压，单位 mV。

风险：

- 当前 `hw_adc_read()` 手动把 pin number 映射到 `ADC1_CHANNEL_x`。这个映射是否适用于 ESP32-S3 需要再核对，不能直接完全信任。

## 7. 日志方案

建议使用紧凑 key-value 日志，方便串口 monitor 阅读和复制：

```text
I (1032) offline_sensor: sensor=DHT11 status=OK temp_c=24.0 humidity_pct=56.0
I (1532) offline_sensor: sensor=DHT11 status=OK temp_c=24.0 humidity_pct=56.0
W (2032) offline_sensor: sensor=DHT11 status=READ_FAIL
```

日志原则：

- 有效读数使用 `ESP_LOGI`。
- 读取失败或数据可疑时使用 `ESP_LOGW`。
- 每个传感器都尽量打印原始值。
- 只有换算关系确定时，才打印带单位的物理量。
- 第一版日志使用英文 key-value 格式。
- 第一版日志周期固定为 500 ms。

## 8. 错误处理

建议行为：

- 如果传感器初始化失败，打印清晰错误，并周期性重试。
- 如果单次读取失败，打印失败状态，但任务继续运行。
- 普通传感器读取失败不自动重启 ESP32。
- UART 传感器要区分“没有收到字节”和“收到无效帧”。
- I2C 传感器要打印写命令失败或读数据失败。

## 9. 构建集成计划

实现前应先让 `sensor_offline_test` 可构建：

1. 优先检查 `sensor_offline_test` 当前是否能直接构建。
2. 如果缺少顶层 ESP-IDF 工程文件，本地可临时从 `master` 补齐用于编译验证。
3. 第一版不新增 `sensor_drivers` 组件。
4. 第一版不强制新增 Kconfig。
5. 主要修改文件预计集中在 `main/app_main_sensor.cpp`。
6. 最终 commit 只提交关键修改文件，避免把与 `master` 重合的工程文件一并提交。
7. 使用 Sensor 角色构建：

```powershell
idf.py -B build\sensor `
  -DSDKCONFIG=build\sensor\sdkconfig `
  -DSDKCONFIG_DEFAULTS="sdkconfig.defaults;sdkconfig.defaults.sensor" `
  build
```

## 10. 验证计划

第一次硬件 bench test 流程：

1. 使用 `OFFLINE_SENSOR_TEST_ENABLED=y` 编译。
2. 烧录到一块 ESP32-S3。
3. 以 115200 baud 打开 monitor。
4. 确认启动日志。
5. 确认周期性传感器日志。
6. 改变传感器输入，确认日志变化。
7. 断电重启，确认行为可重复。

建议测试顺序：

1. 数字震动模块或雨滴模块。
2. ADC 原始值测试。
3. BH1750 I2C。
4. DHT11。
5. 拿到 datasheet 或确认协议后，再测 JW01 UART。

## 11. 尚未冻结的决策

以下问题不影响第一版实现，后续再处理：

- JW01 的真实帧格式和单位；当前没有 datasheet，第一版不处理。
- ESP32-S3 上 ADC pin/channel 的准确映射。
- 最终 commit 已确认只提交关键文件。
