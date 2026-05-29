# ESP-LEGO LVGL 图形界面技术架构与实现计划

## 1. 架构原则

LVGL 接入采用分层设计，保持现有组件职责清晰。

核心原则：

- `lcd_touch` 只做底层 LCD 与触摸驱动。
- 新增 `ui_lvgl` 作为 LVGL 适配与 UI 组件。
- `main` 只负责初始化编排，不承载 UI 业务逻辑。
- interpreter、ESP-NOW、Web Console 不因 LVGL 引入而改变核心职责。
- 只有 LVGL 任务直接绘屏，其他任务通过状态读取或事件队列间接影响 UI。

## 2. 目标组件结构

建议新增：

```text
components/ui_lvgl/
├── CMakeLists.txt
├── idf_component.yml
├── include/ui_lvgl/ui_lvgl.h
└── src/
    ├── ui_lvgl.cpp
    ├── ui_screen_diag.cpp
    └── ui_peer_view.cpp
```

说明：

- `ui_lvgl.cpp`：LVGL 初始化、display driver、input driver、tick、UI task。
- `ui_screen_diag.cpp`：触摸诊断界面。
- `ui_peer_view.cpp`：peer 数量和简短 peer 列表显示。
- `ui_lvgl.h`：对 `main` 暴露 `ui_lvgl_init()` 等少量入口。
- `idf_component.yml`：声明 LVGL 依赖。

第一版也可以先只实现 `ui_lvgl.cpp`，待结构稳定后再拆分页面文件。

## 3. 组件依赖

`ui_lvgl` 依赖：

- `lvgl`
- `lcd_touch`
- `esp_timer`
- `freertos`
- `espnow_comm`

建议 CMake：

```cmake
idf_component_register(
    SRCS
        "src/ui_lvgl.cpp"
        "src/ui_screen_diag.cpp"
        "src/ui_peer_view.cpp"
    INCLUDE_DIRS "include"
    REQUIRES lvgl lcd_touch esp_timer espnow_comm
)
```

`main` 增加依赖：

```cmake
REQUIRES ... lcd_touch ui_lvgl
```

`app_main.cpp` 在 `lcd_touch_init()` 成功后调用 `ui_lvgl_init()`。

## 4. LVGL 依赖管理

使用 ESP-IDF Component Manager。

建议在 `components/ui_lvgl/idf_component.yml` 中声明 LVGL 版本，例如：

```yaml
dependencies:
  lvgl/lvgl: "^8.3.0"
```

具体版本可在实现时根据 ESP-IDF v5.2.6 的兼容性确认。第一版优先选择 LVGL 8.x，因为 ESP-IDF 生态中资料和示例较成熟。

## 5. 显示刷新路径

数据流：

```text
LVGL draw buffer
  -> flush callback
  -> lcd_set_window(x0, y0, x1, y1)
  -> lcd_write_pixels(color_buffer, pixel_count)
  -> lv_disp_flush_ready()
```

实现要点：

- 使用部分 draw buffer，不申请 240 x 240 全屏 framebuffer。
- 建议初始 buffer 高度为 20 到 40 行。
- RGB565 格式与当前 `lcd_write_pixels()` 对齐。
- `lcd_write_pixels()` 内部已经处理 ST7789 所需的大端字节序。
- flush callback 内必须在写屏完成后调用 `lv_disp_flush_ready()`。

初始 buffer 建议：

```text
240 x 40 x 2 bytes = 19200 bytes
```

如 RAM 紧张，可降为：

```text
240 x 20 x 2 bytes = 9600 bytes
```

## 6. 触摸输入路径

数据流：

```text
CST816D
  -> touch_read()
  -> LVGL indev read callback
  -> LVGL pointer event
  -> button/click/pressing/long press
```

LVGL input callback 负责：

- 调用 `touch_read(&data)`。
- 如果 `data.points > 0`，设置 `LV_INDEV_STATE_PR`。
- 否则设置 `LV_INDEV_STATE_REL`。
- 将 `data.p[0].x/y` 写入 LVGL point。
- 缓存最近一次坐标和 gesture，供诊断界面显示。

注意：

- 如后续调用 `lcd_set_rotation()`，触摸坐标也必须同步旋转。
- 第一版建议不旋转屏幕，保持 `rotation = 0`，降低 bring-up 变量。
- CST816D 原始 gesture 可用于诊断显示，但 LVGL 自己也能识别部分点击/长按事件。

## 7. Tick 与任务模型

推荐：

- 使用 `esp_timer` 每 1 ms 调用 `lv_tick_inc(1)`。
- 创建独立 `ui_task` 周期调用 `lv_timer_handler()`。
- `ui_task` 延迟建议 5 到 10 ms。

任务建议：

```text
ui_task
  stack: 4096 到 6144
  priority: 2 到 3
```

现有任务关系：

- `exec_task` 优先级 5，负责脚本执行。
- `shell_task` 优先级 4，负责 UART 输入。
- `timeout_task` 优先级 3，负责 peer aging。
- `ui_task` 不应抢占脚本执行主路径，优先级可设为 2 或 3。

## 8. UI 数据来源

第一版 UI 显示的数据来源：

- 触摸诊断状态：来自 `ui_lvgl` 内部缓存。
- peer 数量：调用 `peer_mgr_active_count()`。
- peer 列表：调用 `peer_mgr_list()` 或 `peer_mgr_active_count()` 加简短格式化。

注意：

- peer 表可能被 ESP-NOW RX 任务更新。
- UI 不应长时间持有 peer 指针。
- 如果使用 `peer_mgr_list()` 返回内部指针数组，UI 应只做短时间读取，并尽快复制需要显示的 name/id。
- 避免在 UI 刷新路径里做阻塞 ESP-NOW 请求。

## 9. 诊断界面设计

240 x 240 第一版建议布局：

```text
+------------------------+
| ESP-LEGO        peers:N |
|------------------------|
| Touch: x=123 y=045     |
| Gesture: swipe_left    |
| Long: 2       Click: 5 |
|                        |
|      [  Tap Test  ]    |
|                        |
| Peers:                 |
| 1 sensor               |
| 2 motor                |
+------------------------+
```

控件：

- 标题 label。
- peer count label。
- 坐标 label。
- gesture label。
- 点击计数 label。
- 长按计数 label。
- 一个按钮。
- peer 列表 label。

LVGL 事件：

- `LV_EVENT_CLICKED`：点击计数加一。
- `LV_EVENT_LONG_PRESSED`：长按计数加一。
- `LV_EVENT_PRESSING` 或 indev 状态：持续更新坐标。
- CST816D gesture 字段：更新 swipe/click/long_press 文本。

## 10. 与现有 lcd_touch 的边界

`lcd_touch` 保持：

- `lcd_init()`
- `lcd_set_window()`
- `lcd_write_pixels()`
- `lcd_fill()`
- `touch_init()`
- `touch_read()`
- `lcd_touch_init()`

`lcd_touch` 不负责：

- LVGL 对象。
- UI 页面。
- 字体。
- 状态显示。
- peer 列表。

建议后续微调：

- 将启动时 `lcd_fill(..., COLOR_RED)` 改成可选 bring-up 测试，避免 LVGL 启动前闪红屏。
- 将引脚宏迁移到 Kconfig，但第一版可暂不做。

## 11. 与 main 的集成顺序

当前 Master 启动顺序建议调整为：

```text
NVS init
ESP-NOW init
LCD/Touch init
LVGL UI init
interpreter init
script_io init
create shell_task
create timeout_task
create exec_task
web_console_init
```

也可以放在 interpreter 初始化之后，但必须满足：

- `lcd_touch_init()` 成功后再初始化 LVGL。
- `ui_lvgl_init()` 失败不能阻止核心 Master 功能启动。
- 如果 LCD/Touch 初始化失败，固件应继续 UART/Web/ESP-NOW 路径。

## 12. 错误处理策略

- LVGL 初始化失败：记录 `ESP_LOGW`，继续启动系统。
- LCD 初始化失败：跳过 `ui_lvgl_init()`，继续启动系统。
- Touch 初始化失败：LCD UI 可继续显示，触摸状态显示为 unavailable。
- peer 列表读取失败或为空：显示 `peers: 0` 或 `no peers`。

## 13. 并发与线程安全

LVGL 通常不是多线程安全的。

第一版规则：

- 只有 `ui_task` 调用 LVGL API。
- 其他任务不直接创建、删除或修改 LVGL 对象。
- 如果后续需要从脚本或 Web Console 更新 UI，先通过 FreeRTOS queue 把事件发给 `ui_task`。

第一版 peer 状态可由 `ui_task` 周期读取，不需要新增事件队列。

## 14. 实现步骤

建议实现顺序：

1. 新建 `components/ui_lvgl/`。
2. 添加 `idf_component.yml`，引入 LVGL。
3. 添加 `ui_lvgl.h`，声明 `esp_err_t ui_lvgl_init(void)`。
4. 在 `ui_lvgl.cpp` 初始化 LVGL。
5. 配置 draw buffer 与 display driver。
6. 实现 flush callback，调用 `lcd_set_window()` 和 `lcd_write_pixels()`。
7. 配置 input driver，调用 `touch_read()`。
8. 创建 `esp_timer` tick。
9. 创建 `ui_task`，周期调用 `lv_timer_handler()`。
10. 绘制触摸诊断页面。
11. 周期更新 peer count 和 peer 列表摘要。
12. 在 `main/app_main.cpp` 中接入 `ui_lvgl_init()`。
13. 执行构建验证。

## 15. 构建与验证命令

Windows 本地 ESP-IDF 环境：

```powershell
& 'd:\IDF_v5.2.6\v5.2.6\esp-idf\export.ps1'
idf.py set-target esp32s3
idf.py build
```

如后续使用 Component Manager，首次构建会解析并下载依赖。

## 16. 风险与缓解

### 16.1 LVGL 版本兼容

风险：LVGL 版本 API 差异导致 port 代码不兼容。

缓解：第一版锁定 LVGL 8.x；实现时避免使用过新的 API。

### 16.2 RAM 占用

风险：draw buffer 或 LVGL heap 过大导致内存紧张。

缓解：使用 20 到 40 行部分缓冲；不使用全屏 framebuffer；UI 控件数量保持少。

### 16.3 SPI 刷新速度

风险：10 MHz SPI 在全屏刷新时速度有限。

缓解：第一版只做状态面板；减少频繁全屏重绘；必要时后续提高 `LCD_SPI_CLOCK_HZ`。

### 16.4 触摸坐标方向

风险：LCD 显示方向与触摸坐标方向不一致。

缓解：第一版固定 rotation 0；诊断页显示坐标，便于人工确认；后续增加坐标映射函数。

### 16.5 多任务访问 LVGL

风险：其他任务直接调用 LVGL 导致竞态。

缓解：第一版只允许 `ui_task` 调用 LVGL；未来统一通过 UI 事件队列。

## 17. 后续扩展方向

本阶段完成后可扩展：

- 脚本运行状态页。
- `print()` 日志尾部显示。
- Sensor 数据卡片。
- peer 详情页。
- Web Console 与板载 UI 状态同步。
- 简单脚本快捷按钮。
- Kconfig 配置 LCD/Touch 引脚。

