# ESP-LEGO LVGL 图形界面需求文档

## 1. 目标

为 ESP-LEGO Master 固件加入一版最小可用的板载图形界面，用于验证 ST7789 LCD、CST816D 触摸控制器、LVGL 显示刷新链路和触摸输入链路是否正常。

本阶段只做需求冻结和后续实现基线，不替代现有 Web Console，不扩展脚本语言的 UI 能力。

## 2. 硬件前提

- 主控芯片：ESP32-S3。
- LCD 控制器：ST7789。
- 触摸控制器：CST816D。
- 分辨率：240 x 240。
- LCD 总线：SPI。
- Touch 总线：I2C。
- 接线沿用当前 `components/lcd_touch/include/lcd_touch/lcd_touch.h` 默认定义：
  - LCD_CS: GPIO10
  - LCD_DC: GPIO11
  - LCD_RST: GPIO12
  - LCD_BL: GPIO13
  - LCD_MOSI: GPIO14
  - LCD_SCLK: GPIO15
  - TOUCH_SDA: GPIO16
  - TOUCH_SCL: GPIO17
  - TOUCH_RST: GPIO18
  - TOUCH_INT: GPIO21

## 3. 软件范围

本阶段新增 LVGL 板载 UI，只用于状态显示与触摸诊断。

包含：

- 引入 LVGL。
- 初始化 LCD 与触摸后启动 LVGL。
- 显示一个 240 x 240 的诊断界面。
- 验证点击、滑动、长按、坐标读取。
- 显示通用 peer 列表或 peer 占位状态。

不包含：

- 不替代 Web Console。
- 不在屏幕上配置 Wi-Fi 或 LLM。
- 不在屏幕上输入自然语言。
- 不执行复杂动画、图表或多页面业务 UI。
- 不把 LVGL 直接暴露给脚本语言。
- 不改变 ESP-NOW 通信协议。
- 不改变 interpreter 的核心语法。

## 4. LVGL 引入方式

优先使用 ESP-IDF Component Manager 引入 LVGL。

理由：

- 版本可锁定，依赖来源清晰。
- 后期升级只需调整依赖版本，不需要手工替换第三方源码目录。
- 仓库体积更小，业务代码和第三方库边界清楚。
- 与 ESP-IDF 构建系统集成更自然。

除非后续必须完全离线开发，或需要深度修改 LVGL 源码，否则不把 LVGL 源码拷贝到本地 `components/lvgl/`。

## 5. UI 第一版功能

第一版界面是“触摸诊断 + 状态面板”。

屏幕建议布局：

- 顶部：`ESP-LEGO` 标题与当前 UI 状态。
- 上半区：在线 peer 数量。
- 中部：一个 LVGL 按钮，用于验证点击事件。
- 下半区：显示最近触摸坐标 `x/y`。
- 下半区：显示最近手势名称。
- 底部：显示通用 peer 列表的前几项，空间不足时只显示数量或简短条目。

触摸诊断至少显示：

- 点击次数。
- 最近一次触摸坐标。
- 最近一次滑动方向。
- 长按次数或长按状态。
- 最近一次原始 gesture 名称。

## 6. 触摸验证要求

只验证按钮点击不足以证明触摸链路全部正常。

按钮点击可以证明：

- I2C 能读到触摸芯片。
- 坐标解析大致可用。
- LVGL input device 回调工作。
- LVGL 事件派发工作。

但不能充分证明：

- CST816D gesture 寄存器解析正确。
- 长按识别稳定。
- 滑动方向与屏幕旋转匹配。
- 边缘区域坐标映射正确。

因此第一版应同时覆盖点击、滑动、长按和坐标显示。实现上仍保持轻量，不做复杂手势业务。

## 7. 状态显示要求

板载 UI 只做状态显示。

第一版状态信息：

- 系统名或固件名。
- UI/LCD/Touch 初始化结果。
- 当前在线 peer 数。
- 通用 peer 列表摘要。
- 最近触摸事件。

后续可扩展但本阶段不实现：

- 当前脚本运行状态。
- 最近脚本错误。
- `print()` 日志尾部。
- Sensor 详细数据卡片。

## 8. 内存与性能约束

项目业务代码继续遵守现有嵌入式约束：

- 不引入 STL 容器作为业务核心数据结构。
- 不改变现有静态池设计。
- 不在解释器或通信链路中引入不必要动态分配。

允许 LVGL 内部使用自己的内存管理机制。

刷新性能目标：

- 只服务状态面板和按钮。
- 不追求高帧率动画。
- 优先稳定与可调试。
- 使用小型 draw buffer，避免全屏 framebuffer。

## 9. 与现有系统关系

LVGL UI 与 Web Console 并存。

- Web Console 继续负责 Wi-Fi/LLM/脚本注入等配置和高级交互。
- LVGL UI 只负责本地屏幕状态显示和触摸诊断。
- ESP-NOW、interpreter、script_io、web_console 的现有职责保持不变。

## 10. 验收标准

第一版实现完成后应满足：

- 固件可编译通过。
- 启动时 LCD 不再停留在纯红色测试画面，而是进入 LVGL 诊断界面。
- 屏幕显示清晰，无明显坐标错位或文字越界。
- 点击按钮后点击计数递增。
- 触摸屏幕不同区域时 `x/y` 坐标变化合理。
- 滑动时能显示方向，方向与手指动作大体一致。
- 长按时能记录长按事件。
- peer 数量显示不影响 ESP-NOW 原有流程。
- Web Console 仍可按原设计启用。

## 11. 后续实现节奏

建议下一阶段按最小闭环实现：

1. 引入 LVGL 依赖。
2. 新增 `components/ui_lvgl/`。
3. 打通 LCD flush callback。
4. 打通 touch input callback。
5. 启动 LVGL tick 与 UI task。
6. 绘制诊断界面。
7. 接入 peer 数量与简短 peer 列表。
8. 构建验证。

