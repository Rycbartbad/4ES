# 门铃从机状态说明

## 当前分支

- 分支名：`doorbell-sensor`
- 基础分支：`sensor1`
- 目标板卡：乐鑫 ESP32-S3-WROOM-1 / ESP32-S3
- 编译目标：`esp32s3`

## 已完成

- 已从 `sensor1` 新建门铃从机分支。
- 已配置传感器从机固件的编译和烧录环境。
- 已新增本地构建和烧录脚本：
  - `doorbell_sensor.ps1`
  - `build_sensor.bat`
- 已配置 ESP-IDF 默认参数：
  - `CONFIG_DEVICE_ROLE_SENSOR=y`
  - `CONFIG_SENSOR_MODULE_NAME="doorbell"`
  - 使用 USB Serial/JTAG 作为当前板子的控制台。
- 已实现门铃输入：
  - 门铃信号脚：`GPIO35`
  - 输入模式，开启内部下拉。
  - 当前假设为高电平触发：按下 = `1`，松开 = `0`。
  - 做了 5 次采样消抖，避免按键抖动误判。
- 已实现无源蜂鸣器输出：
  - 蜂鸣器 I/O 脚：`GPIO36`
  - 使用 LEDC PWM 输出方波驱动。
  - 符合手册里无源蜂鸣器需要 2K 到 5K 方波驱动的要求。
  - 门铃从“松开”变为“按下”时响一次，不会一直按住就重复响。
  - 当前提示音为：
    - `3200 Hz` 持续 `120 ms`
    - 停顿 `60 ms`
    - `2400 Hz` 持续 `180 ms`
- 已保留 ESP-NOW 从机能力：
  - 模块会广播名称 `doorbell`。
  - 主机发送 `MSG_DATA_REQ` 时，从机返回 1 个数值：
    - `1.0` 表示门铃按下
    - `0.0` 表示门铃松开
- 已修正从机响应包里的协议问题：
  - `DATA_RESP` 会回传请求里的 `target_id` 和 `seq_id`。
  - `MSG_CMD` 会从协议头之后读取真正的 payload。
  - `ACK` 会回传请求里的 `target_id` 和 `seq_id`。
- 已验证编译：
  - `.\doorbell_sensor.ps1 build` 执行成功。
- 已验证烧录：
  - `.\doorbell_sensor.ps1 flash -Port COM7` 执行成功。
  - `esptool` 已完成 flash 写入和 hash 校验。

## 当前接线

- 门铃信号线：`GPIO35`
- 无源蜂鸣器模块：
  - `VCC` 接 `5V`
  - `GND` 接 `GND`
  - `I/O` 接 `GPIO36`

## 未完成 / 仍需确认

- 固件运行态还没有完整确认。
  - 之前串口日志多次显示板子进入 ROM 下载模式：
    - `boot:0x0 (DOWNLOAD(USB/UART0))`
    - `waiting for download`
  - 这通常表示 `GPIO0/BOOT` 被拉低，或者 USB 复位线让芯片停在下载模式。
  - 固件已经烧录进 flash，但还需要让板子正常从 flash 启动，才能完整测试运行效果。
- 门铃触发电平目前按“高电平触发”处理。
  - 这个判断来自丝印 GPIO35 和前面检测到按下时 GPIO35 为高。
  - 如果后续实测发现门铃模块是低电平触发，需要把 `DOORBELL_ACTIVE_LEVEL` 从 `1` 改成 `0`。
- 蜂鸣器实际声音还没有物理确认。
  - 代码已经在 `GPIO36` 输出 PWM 方波。
  - 还需要板子正常运行后实际听一下音量和音调是否合适。
- 主机侧还没有完成端到端联调。
  - 从机已经能响应 `MSG_DATA_REQ`。
  - 还需要用主机实际调用 `remote_read("doorbell")` 或对应模块 ID 来验证。
- 目前还没有实现“门铃按下主动上报主机”。
  - 当前逻辑是：本地蜂鸣器响，主机通过轮询读取门铃状态。
  - 如果需要按下后立即通知主机，需要再加 ESP-NOW 主动事件或新的消息类型。
- 门铃脚、蜂鸣器脚、触发电平、提示音频率目前还是代码里的常量。
  - 暂时没有做成 Kconfig 可配置项。
  - 后续如果要支持多种接线，可以再抽成配置项。

