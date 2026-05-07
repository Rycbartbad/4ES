# ESP-LEGO — 阶段实施计划

## 总览

| 阶段 | 名称 | 核心产出 | 依赖 | 估算 |
|------|------|---------|------|------|
| P1 | 项目骨架 | CMake 构建系统 + Kconfig + 空组件骨架 | 无 | 1d |
| P2 | ESP-NOW 通信层 | protocol + peer_mgr + comm (收发+同步请求) | P1 | 2d |
| P3 | 词法分析器 | token.h + lexer (Token 流输出) | P1 | 1d |
| P4 | 语法分析器 | ast.h + parser (递归下降, AST 输出) | P3 | 2d |
| P5 | 解释器核心 | value + environment + interpreter (AST 遍历执行) | P4 | 2d |
| P6 | 内置函数 + 子模块固件 | builtins + hw_drivers + 子模块主循环 | P2, P5 | 2d |
| P7 | 主控集成 | 主控任务架构 + script_io + 完整启动流程 | P2, P5, P6 | 1d |
| P7.5 | Web 控制台与 AI 集成 | web_console 组件 (SoftAP + HTTP + LLM + 脚本注入) | P7 | 5d |
| P8 | 健壮性加固 | watchdog + 深度防御 + 并发锁 + 资源验证 | P7.5 | 1d |
| P9 | 测试 + AI 物料 | 单元测试 + 集成测试 + AI System Prompt | P8 | 3d |

**总计**: ~20 个工作日

> ⚠️ **工期风险**: P7.5（Web Console + AI 集成）涉及 6 个相互耦合的子系统（SoftAP、HTTP Server、WiFi 模式切换、LLM HTTP 客户端、jsmn JSON 解析、脚本注入 + 环形缓冲区），且依赖调试难度高的 WiFi 模式切换和 LLM API 集成。实际可能需要 **6-8 天**而非 4 天。建议在 P7 完成后先做 **P7.5 的 S1-S2（骨架 + 配置 API，~1.5d）** 作为可行性验证，确认 SoftAP + HTTP 链路跑通后再进入 S3-S6。总体乐观估计 19 天，保守估计 **22-25 天**。

---

## P1: 项目骨架

**目标**: 构建系统可编译通过，组件目录结构完整，Kconfig 选项生效。

**依赖**: 无（起始阶段）

**估算**: 1d

**前置条件**:
- [ ] ESP-IDF v5.2.6 工具链已安装，`idf.py` 可用
- [ ] `xtensa-esp32s3-elf-gcc` 可执行
- [ ] 目标芯片选择为 esp32s3（`idf.py set-target esp32s3`）

**产出文件**:
```
main/CMakeLists.txt, main/Kconfig.projbuild, main/app_main.cpp (骨架)
components/interpreter/{CMakeLists.txt, include/interpreter/*.h, src/*.cpp}
components/espnow_comm/{CMakeLists.txt, include/espnow_comm/*.h, src/*.cpp}
components/hw_drivers/{CMakeLists.txt, include/hw_drivers/*.h, src/*.cpp}
components/script_io/{CMakeLists.txt, include/script_io/*.h, src/*.cpp}
```

### 任务清单

- [ ] **P1.1** 改写 `main/CMakeLists.txt`：从 C 切换到 C++（`.c` → `.cpp`），启用 C++17（`set(CMAKE_CXX_STANDARD 17)`）
- [ ] **P1.2** 创建 `main/Kconfig.projbuild`：定义 `CONFIG_DEVICE_ROLE_MASTER` / `CONFIG_DEVICE_ROLE_SENSOR`
- [ ] **P1.3** 创建 4 个组件骨架目录（各含 CMakeLists.txt + 空头文件 + 空源文件）：
      `interpreter/`, `espnow_comm/`, `hw_drivers/`, `script_io/`
- [ ] **P1.4** 创建共享字符串驻留表（intern table）：
      - 固定数组 `intern_table[MAX_INTERN_STRINGS]` + `int intern_count`
      - `intern_string(const char* start, int len)` → `const char*`
      - 放置在 `components/interpreter/include/interpreter/` 下，被 lexer/parser 共享
- [ ] **P1.5** 创建 `components/interpreter/Kconfig`：AST_POOL_SIZE, LIST_POOL_SIZE, FUNC_POOL_SIZE, MAX_BINDINGS, MAX_FUNC_PARAMS, MAX_PARSE_DEPTH, MAX_LOOP_ITERATIONS, MAX_SENSOR_CALLS_PER_SCRIPT, MAX_EXEC_STATEMENTS, SCRIPT_EXEC_TIMEOUT_MS, STRICT_MODE, INTERN_TABLE_SIZE
- [ ] **P1.6** 创建 `components/espnow_comm/Kconfig`：ANNOUNCE_INTERVAL, ANNOUNCE_JITTER, PEER_TIMEOUT, READ_TIMEOUT, COMM_CONCURRENT_CHECK
- [ ] **P1.7** 创建构建脚本：`build_master.bat`（`SDKCONFIG_DEFAULTS=sdkconfig.defaults;sdkconfig.defaults.master`）、`build_sensor.bat`——一条命令构建，无需手动 menuconfig
- [ ] **P1.8** 验证：`idf.py set-target esp32s3 && idf.py build` 零错误通过

### 验收标准 (测试用例)

| ID | 名称 | 步骤 | 预期结果 |
|----|------|------|---------|
| TC-P1.1 | C++17 编译 | 在 `app_main.cpp` 中添加 `static_assert(__cplusplus >= 201703L);` 并编译 | 编译通过 |
| TC-P1.2 | Kconfig 可见 | 执行 `idf.py menuconfig`，检查 Component config → interpreter 和 espnow_comm 下出现 Kconfig 选项 | 所有选项可见 |
| TC-P1.3 | 组件链接 | 在各组件空源文件中添加 `#pragma message("interpreter compiled")` 等标记，编译观察日志 | 4 条编译标记全部出现 |
| TC-P1.4 | intern 表可用 | 在 `app_main.cpp` 中调用 `intern_string("test", 4)`，验证返回非 NULL 且指向驻留池 | 返回合法指针，字符串内容正确 |
| TC-P1.5 | 零错误构建 | 运行 `idf.py build` | exit code 0，无 error 输出 |

### 风险点

- **工具链版本不兼容**: Kconfig 选项名与 ESP-IDF v5.2.6 的命名规范冲突。→ 遵循 `CONFIG_` 前缀 + 小写下划线风格
- **C++ 异常/RTTI 误开启**: ESP-IDF 默认禁用异常和 RTTI，CMakeLists.txt 中必须显式确认未启用
- **组件路径在 Windows 上大小写敏感**: ESP-IDF 在 Windows 上可能因头文件路径大小写不匹配导致编译失败。→ 所有 `#include` 路径严格小写

---

## P2: ESP-NOW 通信层

**目标**: 实现设备发现（宣告包）+ peer 管理器 + 同步请求-响应。

**依赖**: P1（项目骨架）

**估算**: 2d

**前置条件**:
- [ ] 所有 P1 验收测试通过
- [ ] ESP32-S3 开发板至少 2 块可用（主控 + 子模块）
- [ ] ESP-NOW 基础文档已阅读（`esp_now_init`, `esp_now_send`, `esp_now_register_recv_cb`）

**产出文件**: `components/espnow_comm/` 全部源代码

### 任务清单

#### P2.1 协议定义 (protocol.h)

- [ ] **P2.1.1** 消息类型枚举：ANNOUNCE=0x10, CMD=0x20, DATA_REQ=0x30, DATA_RESP=0x40, ACK=0x50
- [ ] **P2.1.2** 消息头结构体：含 `version=0x01, msg_type, target_id, seq_id, cmd_id, payload_len`
      - seq_id 机制详见 design.md §7.3
      - 接收方每个 PeerEntry 独立维护 `uint8_t dedup_seq[8]` 滑动窗口（design.md §5.1）
      - 协议版本校验：接收方检查 version 字段，不匹配时日志警告
- [ ] **P2.1.3** cmd_id 仅对 `MSG_CMD` 有意义（0x0002+）；`MSG_DATA_REQ` 靠 msg_type 分发，cmd_id 置 0
- [ ] **P2.1.4** 宣告包构造/解析函数（详见 design.md §4.2）
- [ ] **P2.1.5** 载荷格式定义：DATA_REQ 无载荷（子模块读取全部传感器）；DATA_RESP 载荷 = `[1B count][8B × count: double values]`（design.md §7.4）

#### P2.2 Peer 管理器 (peer_mgr.h/cpp)

- [ ] **P2.2.1** PeerEntry 结构体：mac, module_id, name, capabilities, last_seen, state, pending_change_tick, peer_id[22]（"aa:bb:cc:dd:ee:ff:255\0" = 21字符 + 1 null = 22）；MAX_PEERS=20
- [ ] **P2.2.2** 状态机：NEW → ACTIVE → OFFLINE，新增 PENDING_CHANGE 暂态（详见 design.md §5.2）
- [ ] **P2.2.3** 查找 API：`find_by_mac()`, `find_by_id()`, `find_by_name()`, `find_by_mac_and_id()`, `active_count()`, `list()`, 老化超时扫描函数（详见 design.md §5.3）
- [ ] **P2.2.4** 并发保护：所有 API 加 `SemaphoreHandle_t s_peer_mutex` 互斥锁；TOCTOU 防范：锁内复制 MAC 到局部数组 `dst_mac[6]`（design.md §5.4）。**严格最小持有原则**：去抖判断逻辑不在锁内执行（使用局部变量缓存判断状态），老化扫描不全程持锁（在锁定状态下逐个判断并立即释放）。此规范需在 P2 实现中严格执行——后续 P8 不再重构锁逻辑

#### P2.3 通信模块 (comm.h/cpp, 按角色条件编译)

- [ ] **P2.3.1** 主控版 `comm_full.cpp`：含 peer_mgr + 同步请求 + 请求队列（V1.0 拒绝模式，V1.1 排队模式）
- [ ] **P2.3.2** 子模块版 `comm_lite.cpp`：仅含宣告包发送 + 命令接收回调 + 简单响应构造
- [ ] **P2.3.3** `espnow_init()`：初始化 Wi-Fi + ESP-NOW + 注册接收回调
- [ ] **P2.3.4** 接收回调（Wi-Fi 任务上下文，非 ISR）→ `xQueueSend` → 后台处理任务
- [ ] **P2.3.5** `espnow_comm_request_read(module_id)`：并发锁检查 → 构造 DATA_REQ → 发送 → 信号量等待 → 超时返回 0（详见 design.md §7.5-§7.6）
- [ ] **P2.3.6** `espnow_comm_handle_resp()`：匹配 source_mac + seq_id，释放信号量（design.md §7.7）
- [ ] **P2.3.7** V1.1 环形请求队列 `req_queue[4]`：排队等待而非直接拒绝
- [ ] **P2.3.8** ACK 配对：接收方处理 CMD/DATA_REQ 后回复 ACK（携带相同 seq_id）；发送方收到 ACK 后取消重传定时器
- [ ] **P2.3.9** 重传机制：CMD/DATA_REQ 发送后 200ms 超时定时器，最多 2 次重试，每次使用新 seq_id（design.md §7.3）

### 验收标准 (测试用例)

| ID | 名称 | 步骤 | 预期结果 |
|----|------|------|---------|
| TC-P2.1 | peer 添加/查找 | 构造 3 个 PeerEntry，依次调用 `insert` → `find_by_mac` → `find_by_id` | 返回正确条目，未找到返回 NULL |
| TC-P2.2 | 老化超时 | 添加 peer，设置 last_seen 为 2×PEER_TIMEOUT 前，运行老化扫描 | 状态变为 OFFLINE |
| TC-P2.3 | 满槽拒绝 | 添加 20 个 ACTIVE peer，尝试插入第 21 个 | 返回 -1，打印警告 |
| TC-P2.4 | PENDING_CHANGE 去抖 | ACTIVE 条目 mac 变化，模拟连续 2 次宣告 | 第一次进入 PENDING_CHANGE，第二次确认后 ACTIVE |
| TC-P2.5 | 同步请求-响应 | 主控调用 `request_read`，子模块回复 DATA_RESP | 主控收到值，信号量释放，返回正确数据 |
| TC-P2.6 | 超时重传 | 子模块不应答，主控连续发送 DATA_REQ | 共 3 次发送（1 初始 + 2 重试），seq_id 递增 |
| TC-P2.7 | 响应来源过滤 | 错误 source_mac 的 DATA_RESP 到达 | 被丢弃，信号量不释放 |
| TC-P2.8 | 并发锁互斥 | 连续快速调用 2 次 `request_read` | 第二次检测到 `s_resp_pending=true`，日志警告并返回 0.0 |
| TC-P2.9 | ACK 配对 | 主控发送 DATA_REQ，子模块正常回复 ACK | 主控收到 ACK 后取消重传定时器，无重发 |

### 风险点

- **ESP-NOW 发送失败率高**: ESP-NOW 基于无连接 UDP 风格，丢包在 RF 噪声环境中常见。→ 重试机制（3 次）是必要保障
- **Wi-Fi 与 ESP-NOW 共存**: ESP32-S3 的 Wi-Fi + ESP-NOW 共享同一天线/基带，扫描或连接 AP 时会中断 ESP-NOW 收发。→ 主控不连接 AP，仅用 ESP-NOW
- **接收回调 ISR 上下文**: 虽然 ESP-NOW 回调在 Wi-Fi task 上下文（非 ISR），但未来版本可能变化。→ 始终使用 `xQueueSend` 解耦，关键资源不直接在回调中操作
- **20 槽位不足**: 大规模部署场景可能不够。→ V1.1 留扩展接口，`MAX_PEERS` Kconfig 化

---

## P3: 词法分析器

**目标**: 源代码 → Token 流，含字符串驻留。

**依赖**: P1（项目骨架 + intern table）

**估算**: 1d

**前置条件**:
- [ ] P1 验收通过，intern 表可工作
- [ ] 脚本语言 Token 类型清单已定稿（design.md §6.4）

**产出文件**:
```
components/interpreter/include/interpreter/token.h
components/interpreter/include/interpreter/lexer.h
components/interpreter/src/lexer.cpp
```

### 任务清单

- [ ] **P3.1** `token.h`：TokenType 枚举（NUMBER, STRING, IDENTIFIER, VAR, IF, ELSE, WHILE, TRUE, FALSE, FUNC, RETURN, PLUS, MINUS, STAR, SLASH, EQ, NEQ, LT, GT, LE, GE, ASSIGN, NOT, AND, OR, LPAREN, RPAREN, LBRACE, RBRACE, SEMICOLON, COMMA, END），Token 结构体（type + union {num, str} + line/col）
- [ ] **P3.2** `lexer.h/cpp`：Lexer 类（构造函数接收源码字符串），`next()` → 返回下一个 Token
      - 跳过空格/换行；数字字面量（含小数点）；字符串字面量（双引号）；标识符/关键字（查表区分）；运算符（支持双字符 == != <= >= && ||）；注释 // 到行尾；错误处理（非法字符）
- [ ] **P3.3** 标识符/关键字区分：查表判断是否为关键字（关键字全小写）
- [ ] **P3.4** 使用 P1 中定义的 `intern_string()` 进行字符串驻留，Lexer 产出 intern 后的 `const char*` 指针供 Parser 直接使用

### 验收标准 (测试用例)

| ID | 名称 | 输入 | 预期结果 |
|----|------|------|---------|
| TC-P3.1 | 数字字面量 | `"42"`, `"3.14"`, `"0"` | Token 类型 NUMBER，值正确 |
| TC-P3.2 | 所有关键字 | `"var if else while true false func return"` | 返回对应关键字 Token，非 IDENTIFIER |
| TC-P3.3 | 标识符 | `"foobar _temp var1"` | Token 类型 IDENTIFIER，字符串驻留 |
| TC-P3.4 | 双字符运算符 | `"== != <= >= && ||"` | 每个运算符正确识别为对应 TokenType |
| TC-P3.5 | 字符串 | `"\"hello world\""` | Token 类型 STRING，值 "hello world" |
| TC-P3.6 | 注释 | `"var x = 1; // this is a comment\n print(x);"` | 注释被跳过，后续代码正常解析 |
| TC-P3.7 | 非法字符 | `"var x = @;"` | 返回错误 Token，line/col 指向 `@` |
| TC-P3.8 | EOF | 空字符串 | 第一个 Token 即为 END |
| TC-P3.9 | 字符串驻留 | 两个相同标识符 `"counter counter"` | 两次返回的 `const char*` 指向同一内存地址 |

### 风险点

- **字符串缓冲区生命周期**: Lexer 输出的 intern 指针依赖输入缓冲区存活。→ 调用方（Interpreter/Parser）必须确保缓冲区在 Token 使用期间不被释放（design.md §6.6 注释）
- **双字符运算符优先级**: `==` 被误解析为两个 `=`。→ 前瞻一个字符，贪心匹配最长运算符

---

## P4: 语法分析器

**目标**: Token 流 → AST（递归下降解析器，含函数定义和 return）。

**依赖**: P3（词法分析器）

**估算**: 2d

**前置条件**:
- [ ] P3 验收通过，Lexer 输出正确的 Token 流
- [ ] BNF 语法已定稿（design.md §6.2）
- [ ] AST 节点类型已定稿（design.md §6.5）

**产出文件**:
```
components/interpreter/include/interpreter/ast.h
components/interpreter/include/interpreter/parser.h
components/interpreter/src/parser.cpp
```

### 任务清单

- [ ] **P4.1** `ast.h` — 节点类型定义
      - NodeType 枚举（PROGRAM, BLOCK, VAR_DECL, ASSIGN, IF, WHILE, BINARY_OP, UNARY_OP, LITERAL_NUM, LITERAL_STR, LITERAL_BOOL, IDENT, FUNC_CALL, FUNC_DEF, RETURN_STMT）
      - ASTNode 联合体（全部节点类型共用一个结构体）
      - `list_new(size)` 在 AST 中表示为普通 FUNC_CALL，无需特殊池分配逻辑
- [ ] **P4.2** `ast.h` — 对象池设施
      - `pool[AST_POOL_SIZE]`, `alloc_node()`（返回 NULL 而非越界写入）, `reset_pool()`
      - `parse_depth` 计数器，`MAX_POOL_SIZE(4096)` 硬上限截断（详见 design.md §6.6）
      - 函数定义 `FUNC_DEF` 节点存储函数体指针 (`ASTNode* body`)，池分配在 P5 执行时完成
- [ ] **P4.3** `parser.h/cpp` — 表达式解析
      - Parser 类（构造函数接收 Lexer 引用）
      - 按 BNF 优先级链递归下降：assignment → logic_or → logic_and → equality → comparison → term → factor → unary → call → primary
      - `parse_expression` 入口分派到对应优先级函数
- [ ] **P4.4** `parser.h/cpp` — 语句解析
      - parse_program, parse_statement, parse_var_decl, parse_if, parse_while, parse_block, parse_func_decl, parse_return
      - 递归组合：语句调用表达式解析，块调用语句解析
- [ ] **P4.5** `parser.h/cpp` — 错误处理与验证
      - 错误恢复：语法错误时跳过到下一个分号或块结束，记录错误信息（行号+预期 Token）
      - 名称冲突检测：函数名 vs 内置函数/已定义函数/全局变量
      - 深度检查：`parse_depth > MAX_PARSE_DEPTH` 时报错中止（详见 design.md §6.6；栈溢出防护见 design.md §5.1 注释）

### 验收标准 (测试用例)

| ID | 名称 | 输入 (脚本) | 预期结果 |
|----|------|-------------|---------|
| TC-P4.1 | 变量声明 + 赋值 | `var x = 42;` | AST: PROGRAM → VAR_DECL(x) → LITERAL_NUM(42) |
| TC-P4.2 | if-else | `if (x > 0) { print(1); } else { print(2); }` | AST: IF → condition/if-body/else-body 结构正确 |
| TC-P4.3 | while 循环 | `while (x < 10) { x = x + 1; }` | AST: WHILE → condition/body 正确 |
| TC-P4.4 | 函数定义 + 调用 | `func add(a, b) { return a + b; } var c = add(1, 2);` | AST: FUNC_DEF → FUNC_CALL |
| TC-P4.5 | 运算符优先级 | `1 + 2 * 3` | AST: BINARY_OP(+, LITERAL_NUM(1), BINARY_OP(*, 2, 3)) |
| TC-P4.6 | 语法错误恢复 | `var x = ; print(1);` | 报告错误，跳过到分号，继续解析 print |
| TC-P4.7 | 名称冲突 | `func print() {}` | 报错（print 是内置函数） |
| TC-P4.8 | 深度超限 | 构造深度 > MAX_PARSE_DEPTH 的嵌套表达式 | `alloc_node()` 返回 NULL，解析中止 |
| TC-P4.9 | 空程序 | 空字符串 | AST 根节点为 PROGRAM，子节点数为 0 |

### 风险点

- **递归栈溢出**: 递归下降解析器 + 解释器共用 `exec_task` 栈。`MAX_PARSE_DEPTH=32` × 80-120B/层（C++17）≈ 2.5-4KB，解释器递归可能叠加。→ `exec_task` 栈大小设为 **8KB**（`configMINIMAL_STACK_SIZE * 8` 或显式 `8192`）
- **左递归语法**: BNF 中未使用左递归，但实现时需确认每个产生式的 first/follow 不冲突

---

## P5: 解释器核心

**目标**: AST → 执行，含作用域、值系统和 return 机制。

**依赖**: P4（语法分析器）

**估算**: 2d

**前置条件**:
- [ ] P4 验收通过，Parser 可生成正确 AST
- [ ] 值系统设计已定稿（design.md §6.8）
- [ ] ExecutionContext 结构已定稿（design.md §6.7）

**产出文件**:
```
components/interpreter/include/interpreter/value.h
components/interpreter/include/interpreter/environment.h
components/interpreter/include/interpreter/interpreter.h
components/interpreter/src/interpreter.cpp
```

### 任务清单

- [ ] **P5.1** `value.h`：
      - Value 联合体（VAL_NUM, VAL_STR, VAL_BOOL, VAL_LIST, VAL_FUNC）
      - List 结构体（`double data[16]`, len）
      - FuncObj 结构体（`params[MAX_FUNC_PARAMS]`, param_count, body）
      - `FuncObj.params` 使用 `const char* params[MAX_FUNC_PARAMS]` 内联数组（避免 reset_pool 后悬空）
      - `FuncObj.body` 指向 AST 池，`reset_pool` 时必须置 NULL
      - Value union 中的 `List*`/`FuncObj*` 指针在 `reset_pool` 后置 NULL
- [ ] **P5.2** `environment.h`：
      - Binding 结构体（name + value）
      - Environment 结构体（parent + bindings[MAX_BINDINGS] + count）
      - `env_define()`, `env_set()`, `env_get()` → Value
      - 变量查找：当前环境 → parent 链
      - `env_snapshot()` → 深拷贝 bindings 到静态快照区
      - `env_restore_pristine()`：恢复 env 为仅含内置函数的初始状态（design.md §6.9/§6.9.1）
- [ ] **P5.3** `interpreter.h/cpp`：
      - `ExecutionContext` 结构体：所有运行时状态收敛于此（design.md §6.7）
      - `exec_depth` 在 `execute_block`/`execute_while`/`eval` 递归入口 +1、出口 -1，超过 `MAX_EXEC_DEPTH`（默认与 `MAX_PARSE_DEPTH` 相同）时立即中止
      - `execute(ast, ctx)` 入口 → `validate_resources(ast)` → `execute_program(ast, ctx)`
      - `validate_resources()` 基础实现（P8 增强）：AST 节点数 > 90% 池大小报错；基础 AST 树深度检查；`ResourceReport` 结构体
      - 语句执行函数簇：`execute_var_decl`, `execute_assign`, `execute_if`, `execute_while`, `execute_block`, `execute_func_def`, `execute_return`, `execute_expr`（全部携带 `ExecutionContext* ctx`）
      - 表达式求值：`eval(ctx)` 递归求值二元运算、一元运算、函数调用
      - 运行时约束：
        - `total_statements++`，检查 `script_timeout` / 全局 `s_script_timeout`
        - `loop_iterations[]` 跟踪嵌套循环深度（design.md §6.7）
        - `sensor_calls_total++`，超过 `MAX_SENSOR_CALLS_PER_SCRIPT` 拒绝
        - `constraint_violated` 标志触发中止
- [ ] **P5.4** 函数调用流程：
      - 从环境查找 FuncObj → 检查形参/实参数量 → 创建局部环境 → 绑定形参 → 重置 `has_returned=false` → `execute_block(body)` → 消费标志
- [ ] **P5.5** return 传播：`execute_block` 每条语句后检查 `has_returned`；`execute_while` 迭代后检查并 break

### 验收标准 (测试用例)

| ID | 名称 | 输入 (AST 或脚本) | 预期结果 |
|----|------|-------------------|---------|
| TC-P5.1 | 变量作用域 | 定义全局变量 + 在块中定义同名局部变量 | 块内访问局部，块外访问全局 |
| TC-P5.2 | if-else 控制流 | `if (true) { x=1; } else { x=2; }` | x=1 |
| TC-P5.3 | while 循环 | `var i=0; while(i<3){i=i+1;}` | i=3 |
| TC-P5.4 | 函数调用 + return | `func add(a,b){return a+b;} var c=add(1,2);` | c=3 |
| TC-P5.5 | 二元运算优先级 | `1 + 2 * 3` | 7 |
| TC-P5.6 | 环境隔离（宽松模式） | 设置 `STRICT_MODE=false`，连续执行两条脚本：`var x=1;` 然后 `print(x);` | 第二条中 x 未定义，宽松模式返回 0 并日志警告 |
| TC-P5.6a | 环境隔离（严格模式） | 设置 `STRICT_MODE=true`，连续执行两条脚本：`var x=1;` 然后 `print(x);` | 第二条中 x 未定义，严格模式中止脚本并报告错误 |
| TC-P5.7 | 循环嵌套限制 | 嵌套循环总迭代 > MAX_LOOP_ITERATIONS | 约束违反，循环中止 |
| TC-P5.8 | 空函数 | `func nop(){} nop();` | 无错误，正常返回 |

### 风险点

- **Value union 内存安全**: `Value` 使用 union 类型，写 VAL_NUM 后读 VAL_LIST 会导致未定义行为。→ 严格的类型检查，无类型转换语法
- **递归深度攻击**: 嵌套函数调用可能导致 `exec_depth` 超过配置上限。→ `exec_depth` + 静态 `MAX_PARSE_DEPTH` 双重保护
- **env_snapshot 内存**: 快照区大小需要 Kconfig 可配。→ `ENV_SNAPSHOT_SIZE` 默认 64 条目

---

## P6: 内置函数 + 子模块固件

**目标**: 完成内置函数注册 + 子模块收发固件。

**依赖**: P2（ESP-NOW 通信层）、P5（解释器核心）

**估算**: 2d

**前置条件**:
- [ ] P2 验收通过，ESP-NOW 收发可用
- [ ] P5 验收通过，解释器可执行基本脚本
- [ ] hw_drivers 硬件抽象层设计已确认（见 design.md §6.10 内置函数中的硬件操作签名、§12 子模块固件中的使用模式）

**产出文件**:
```
components/interpreter/src/builtins.cpp
components/interpreter/include/interpreter/builtins.h
components/hw_drivers/src/drivers.cpp
components/hw_drivers/include/hw_drivers/drivers.h
main/app_main_sensor.cpp
```

### 任务清单

- [ ] **P6.1** `builtins.h/cpp`：
      - `register_builtins(env)`：在全局环境中注册所有内置函数（静态 FuncObj，不从用户池分配，design.md §6.10）
      - `BIF_COUNT=21` 枚举（design.md §6.10）
      - 基础 I/O：`digital_read`, `digital_write`, `analog_read`, `analog_write` → 调用 `hw_drivers` 层
      - 远程操作：`remote_read(id/name)` → `espnow_comm_request_read()`；`espnow_send(id, cmd, data)`
      - 工具：`sleep(ms)` → `vTaskDelay`；`print(val)` → `printf`
      - 列表：`list_new(size)` → 列表池分配；`list_get`, `list_set`, `list_len`, `list_free`
      - 聚合函数：`remote_read_avg`, `remote_read_max`, `remote_read_min`（栈数组实现，超时保护见 design.md §15.5）
      - Peer 查询：`list_peers()`, `peer_count()`, `peer_online(id/name)`
      - 名称寻址：函数同时支持 VAL_NUM 和 VAL_STR
      - 超时保护：聚合函数每次子请求后检查 `ctx.script_timeout` + 全局 `s_script_timeout`（design.md §15.5）
- [ ] **P6.2** `hw_drivers`：
      - `hw_gpio_read(pin)`, `hw_gpio_write(pin, val)`
      - `hw_adc_read(pin)`, `hw_pwm_write(pin, val)`
- [ ] **P6.3** 子模块固件 (`main/app_main_sensor.cpp`)：
      - `announce_task`：定期广播宣告包（从 NVS 读 module_name）
      - 接收回调 `on_command`：解析 CMD/DATA_REQ → 操作硬件 → 回复 DATA_RESP/ACK

### 验收标准 (测试用例)

| ID | 名称 | 步骤 | 预期结果 |
|----|------|------|---------|
| TC-P6.1 | 本地 GPIO 读写 | 调用 `digital_write(2, 1)`，读取 `digital_read(2)` | 返回 1 |
| TC-P6.2 | sleep 延迟 | 调用 `sleep(100)` 前后记录 tick | tick 差 ≥ 90ms（FreeRTOS tick 周期可能导致 90-100ms） |
| TC-P6.3 | 列表操作 | `list_new(3)` → `list_set(t,0,1.0)` → `list_get(t,0)` | 返回 1.0 |
| TC-P6.4 | 聚合函数 (无列表) | `remote_read_avg("1,2,3")` | 正确计算平均值 |
| TC-P6.5 | 子模块宣告 | 烧录子模块固件，上电观察 UART 日志 | 定期打印 "Sending announce..." |
| TC-P6.6 | 主控查询子模块 | 主控 `remote_read(1)` | 返回子模块 pin 读数 |
| TC-P6.7 | 名称寻址 | `remote_read("kitchen")` 等价于 `remote_read(1)` | 返回相同值 |
| TC-P6.8 | list_new 池耗尽 | 列表池满时再分配 | 返回全局空列表（只读），日志警告 |

### 风险点

- **内置函数与池交互**: 内置函数中的 `list_new` 从列表池分配，需透传 `ExecutionContext*`。→ 所有内置函数签名包含 `(span<Value> args, Value& result, ExecutionContext* ctx)`
- **子模块固件维护两套入口**: 主控和子模块各有一个 `app_main`，通过 Kconfig 条件编译。→ 确保 `main/CMakeLists.txt` 正确根据 `CONFIG_DEVICE_ROLE_*` 选择源文件
- **NVS 缺失场景**: 子模块首次启动 NVS 未初始化。→ 检测错误并回退到默认名称，打印警告

---

## P7: 主控集成

**目标**: 完整主控固件启动、脚本接收、解释执行链路跑通。

**依赖**: P2（ESP-NOW）、P5（解释器）、P6（内置函数 + 子模块）

**估算**: 1d

**前置条件**:
- [ ] P6 验收通过，内置函数可调用
- [ ] 子模块固件可烧录并产生宣告包
- [ ] 硬件：主控板 + 子模块板各 1 块，串口线可用

**产出文件**:
```
main/app_main.cpp (主控版本)
components/script_io/src/script_io.cpp
components/script_io/include/script_io/script_io.h
```

### 任务清单

- [ ] **P7.1** `app_main.cpp`：
      - `espnow_init()` + `peer_mgr_init()`
      - `hardware_init()`（本地 GPIO）
      - `interpreter_init()` → 创建全局环境 → 注册所有内置函数
      - `script_io_init()` → 初始化 UART
      - 创建 4 个任务：
        - `rx_task`：处理 ESP-NOW 接收队列 → peer_mgr 更新 + DATA_RESP 处理
        - `timeout_task`：每秒扫描 peer 超时 → OFFLINE
        - `shell_task`：UART 读取脚本 → 放入 `script_queue`
        - `exec_task`：从 `script_queue` 取脚本 → `reset_pool()` → `env_restore_pristine()` → lex → parse → `validate_resources()` → execute → `reset_pool()` + `env_restore_pristine()` + `ctx.reset()`，同时维护 watchdog 定时器
        - **关键：如果在 lex/parse 阶段失败（语法错误、池耗尽、深度超限），必须在返回等待下一个脚本前先 `xTimerStop(s_watchdog_timer, portMAX_DELAY)` 再 `xTimerDelete(s_watchdog_timer, portMAX_DELAY)`。必须两步骤：`xTimerDelete` 并非同步，回调可能在删除期间触发导致野指针。此要求在 P8 中再次强调，但 P7 实现时必须落实**
- [ ] **P7.2** `script_io.h/cpp`：
      - UART 初始化 + 行读取（简单行缓冲，支持退格编辑）
      - `script_queue`：FreeRTOS 静态队列，深度 `CONFIG_SCRIPT_QUEUE_LEN`（默认 4）
      - 入队行为：`shell_task` 使用 `xQueueSend(script_queue, ..., 0)`（超时 0，不阻塞）；队列满时丢弃最旧脚本（`xQueueOverwrite` 或手动出队一个），日志警告 "脚本队列满，丢弃旧脚本"
      - `script_io_enqueue()` 同样使用非阻塞入队 + 队满丢弃策略
      - **任务清单**
      - 行缓冲使用 `CONFIG_SCRIPT_MAX_LEN`（默认 2048）作为硬上限：输入超过该长度时截断忽略，日志警告 "脚本超长"
      - `script_io_enqueue()` 同样检查输入长度，超长则返回错误码 `-1`

### 验收标准 (测试用例)

| ID | 名称 | 步骤 | 预期结果 |
|----|------|------|---------|
| TC-P7.1 | 算术运算 | 从串口发送 `print(1+2*3);` | 串口输出 `7` |
| TC-P7.2 | 变量存储 | `var x=5; print(x);` | 输出 `5` |
| TC-P7.3 | 条件分支 | `if(10>5){print(1);}else{print(2);}` | 输出 `1` |
| TC-P7.4 | 远程读取 | `print(remote_read(1));` | 输出子模块传感器值 |
| TC-P7.5 | peer 可见 | `print(peer_count());` | 输出 ≥ 1（子模块已宣告） |
| TC-P7.6 | 多语句脚本 | `var a=1; var b=2; print(a+b);` | 输出 `3` |
| TC-P7.7 | 错误上报 | 串口发送语法错误脚本 `var x = ;` | 串口输出错误信息含行号 |

### 风险点

- **任务栈大小**: `exec_task` 给递归下降解析器 + 解释器共用，`MAX_PARSE_DEPTH=32` 最坏情况约 4KB，若 `exec_depth` 叠加可能更大。→ 创建任务时使用 **8KB**（`configMINIMAL_STACK_SIZE * 8` 或显式 `8192`）
- **script_queue 阻塞**: 如果 `exec_task` 执行耗时脚本，`shell_task` 的 `xQueueSend` 可能超时。→ 使用 `portMAX_DELAY` 或足够大的队列长度（默认 4）
- **UART 中断与行缓冲竞争**: 行缓冲被 ISR 和 shell_task 同时访问。→ 使用互斥锁或仅从 shell_task 读取 `xQueueReceive`

---

## P7.5: Web 控制台与 AI 集成

**目标**: 主控可通过 SoftAP 热点提供网页配置界面，支持 Wi-Fi 扫描、LLM API 调用、自然语言生成脚本并持续执行。

**依赖**: P7（主控集成，脚本执行链路已通）

**估算**: 5d（子任务各段之和 4.5d → 取整 5d，含集成验证余量）

**前置条件**:
- [ ] P7 验收通过，主控可正常启动，shell_task + exec_task 回路工作
- [ ] ESP-IDF 组件 `esp_http_server` 可用
- [ ] cJSON 组件可用 (`REQUIRES cJSON`)
- [ ] ESP32-S3 开发板带 BOOT 按钮 (GPIO 0)

**产出文件**:
```
components/web_console/
├── CMakeLists.txt
├── include/web_console/
│   └── web_console.h
└── src/
    ├── web_console.cpp          # SoftAP + HTTP 服务器 + 页面渲染 + API 路由
    ├── wifi_scan.cpp            # Wi-Fi 扫描封装
    ├── llm_client.cpp           # LLM API HTTP 客户端
    └── script_inject.cpp        # 脚本入队 + print 输出环形缓冲区
```

**修改的现有文件**:
- `main/app_main.cpp` — 增加 `web_console_init()` 调用 + 按键检测 GPIO 任务
- `main/Kconfig.projbuild` — 新增 Web Console 配置菜单
- `main/CMakeLists.txt` — 添加 `REQUIRES web_console cJSON`
- `components/interpreter/src/builtins.cpp` — print 增加环形缓冲区写入
- `components/script_io/include/script_io/script_io.h` — 新增 `script_io_enqueue()` 声明
- `components/script_io/src/script_io.cpp` — 实现 `script_io_enqueue()`
- `docs/design.md` — 已更新（新增 §16 Web Console 组件）

### 任务清单

#### S1: Web Console 骨架 + SoftAP + HTTP 服务器 + 页面 (1d)

- [ ] **P7.5.1** 创建 `components/web_console/` 目录结构和 `CMakeLists.txt`
- [ ] **P7.5.2** `web_console.h/cpp`：SoftAP 启动（`ESP-LEGO-Setup` 热点，无密码）
- [ ] **P7.5.3** `web_console.h/cpp`：`esp_http_server` 初始化，注册 URI 路由
- [ ] **P7.5.4** `web_console.h/cpp`：提供 `/` 静态 HTML 页面（内联 CSS/JS，无外部依赖）
  - 页面包含：WiFi SSID/密码表单、LLM Base URL/Key/Model 表单、设备列表显示、AI 指令输入框、执行日志面板
- [ ] **P7.5.5** `web_console.h/cpp`：实现软重启回调（LLM 调用前暂停 HTTP 服务器）

#### S2a: `/api/config/wifi` 保存 WiFi 配置 (0.25d)

- [ ] **P7.5.6** `web_console.h/cpp`：HTTP POST `/api/config/wifi` — 仅解析 `wifi_ssid`, `wifi_pass`，写入 NVS；**不接受 `llm_key` 等不相关字段**
- [ ] **P7.5.7** `web_console.h/cpp`：HTTP GET `/api/config` — 从 NVS 读取完整配置返回 JSON（key 用 `"***"` 掩码，`wifi_pass` 不返回）

#### S2b: `/api/config/llm` 保存 LLM 配置 (0.25d)

- [ ] **P7.5.8** `web_console.h/cpp`：HTTP POST `/api/config/llm` — 仅解析 `llm_url`, `llm_key`, `llm_model`，写入 NVS；**不接受 `wifi_ssid`/`wifi_pass` 等不相关字段**
- [ ] **P7.5.9** `web_console.h/cpp`：HTTP GET `/api/status` — 返回系统状态（peer 数量、配网状态、脚本运行状态）

#### S3: `/api/scan` Wi-Fi 扫描 (0.5d)

- [ ] **P7.5.9** `wifi_scan.cpp`：封装 `esp_wifi_scan_start()`，返回 SSID 列表（按 RSSI 降序，去重）
- [ ] **P7.5.10** 扫描期间暂停 ESP-NOW 处理：`vTaskSuspend(rx_task)` + `vTaskSuspend(timeout_task)`；扫描完成后 `xQueueReset(rx_queue)` 丢弃过时包，再 `vTaskResume` 恢复。**不调用 `esp_now_deinit()`**（避免 peer 表丢失和底层层资源重新分配，与 design.md §16.11 一致）
- [ ] **P7.5.11** 扫描结果 → JSON 数组 → HTTP 响应
- [ ] **P7.5.12** Kconfig 控制是否显示 5GHz 网络（`WIFI_SCAN_SHOW_5GHZ`）

#### S4: LLM 客户端 — 按需联网调用 (1d)

- [ ] **P7.5.13** `llm_client.cpp`：WiFi 模式切换（SoftAP → STA → SoftAP 完整序列，见 design.md §16.5）
- [ ] **P7.5.14** `llm_client.cpp`：`esp_http_client` 构造 POST 请求到 LLM API（OpenAI-compatible）
- [ ] **P7.5.15** `llm_client.cpp`：System Prompt 自动构建（含 BNF + 设备列表 + 内置函数 + 资源约束）
- [ ] **P7.5.16** `llm_client.cpp`：`cJSON` 解析响应，提取 `choices[0].message.content`
- [ ] **P7.5.17** `llm_client.cpp`：`extract_script_from_response()` — 去除 markdown 代码块标记，提取纯脚本
- [ ] **P7.5.18** `web_console.cpp`：HTTP POST `/api/ai` — 串联整条链路：设备列表 → System Prompt → LLM 调用 → 提取脚本 → 注入执行

#### S5: 脚本注入 + Print 捕获 + 日志回传 (1d)

- [ ] **P7.5.19** `script_inject.cpp`：定义 `g_print_buffer[EXEC_LOG_BUF_SIZE]` + `g_print_buffer_pos` + `g_print_mutex`
- [ ] **P7.5.20** `script_inject.cpp`：实现 `s_script_abort_requested` 原子标志 + `script_io_enqueue()` 注入新脚本
- [ ] **P7.5.21** 修改 `interpreter/src/builtins.cpp`：`print` 函数增加环形缓冲区写入（空指针安全跳过）
- [ ] **P7.5.22** 修改 `main/app_main.cpp`：`exec_task` 主循环中 (a) `execute_block` 每语句检查 `s_script_abort_requested` 并 break；(b) 当前脚本执行完毕后（`on_script_end()` → `reset_pool()` → `env_restore_pristine()` → `ctx.reset()` 之后），清零 `s_script_abort_requested = false`；(c) 然后才从队列取下一个脚本执行
- [ ] **P7.5.23** `web_console.cpp`：HTTP GET `/api/exec_log` — 锁定互斥锁，从环形缓冲区读取最近内容 → JSON
- [ ] **P7.5.24** `web_console.cpp`：HTTP POST `/api/script` — 直接注入脚本（含 abort 当前 + enqueue 新脚本）
- [ ] **P7.5.25** 修改 `components/script_io/`：新增 `script_io_enqueue()` API（调用 `xQueueSend(script_queue, ...)`）

#### S6: 按键检测 + 整合 (0.5d)

- [ ] **P7.5.26** 修改 `main/app_main.cpp`：创建按键检测任务（GPIO 0 长按 3 秒 → 进入配置模式）
- [ ] **P7.5.27** 修改 `main/Kconfig.projbuild`：添加 `CONFIG_WEB_CONSOLE_ENABLED`, `CONFIG_WEB_CONSOLE_TIMEOUT_SEC`, `WIFI_SCAN_SHOW_5GHZ`, `BUTTON_PIN`, `EXEC_LOG_BUF_SIZE`, `HTTP_SERVER_STACK_SIZE`
- [ ] **P7.5.28** 修改 `main/CMakeLists.txt`：添加 `REQUIRES web_console cJSON`
- [ ] **P7.5.29** 全系统编译验证（`idf.py build`），确认零错误

### 验收标准 (测试用例)

| ID | 名称 | 步骤 | 预期结果 |
|----|------|------|---------|
| TC-P7.5.1 | SoftAP 可见 | 烧录未配网固件，手机扫描 Wi-Fi | 可见 `ESP-LEGO-Setup` 热点 |
| TC-P7.5.2 | 页面可访问 | 手机连热点，打开 http://192.168.4.1 | 配置页面渲染正常，无外部资源加载失败 |
| TC-P7.5.3 | Wi-Fi 扫描 | 点击页面"扫描"按钮 | 返回附近 2.4GHz SSID 列表，按信号强度排序 |
| TC-P7.5.4 | WiFi 配置独立保存 | 仅填写 WiFi SSID/密码，提交到 `/api/config/wifi` | NVS `wifi_ssid`/`wifi_pass` 写入成功，`llm_key`/`llm_url` 不变 |
| TC-P7.5.4a | LLM 配置独立保存 | 仅填写 LLM URL/Key/Model，提交到 `/api/config/llm` | NVS `llm_url`/`llm_key`/`llm_model` 写入成功，`wifi_ssid`/`wifi_pass` 不变 |
| TC-P7.5.4b | 配置域隔离 | 提交 `/api/config/wifi` 时附带 `llm_key` 字段 | 服务端忽略 `llm_key`，不写入 NVS |
| TC-P7.5.5 | LLM 调用 | 输入"每 5 秒读温度"，点击生成 | LLM 返回脚本，注入执行，print 输出在日志面板显示 |
| TC-P7.5.6 | 持续执行 | TC-P7.5.5 生成的脚本包含 `while(true)` | 脚本持续运行，日志不断更新 |
| TC-P7.5.7 | 脚本切换 | 脚本 A 运行中，输入新指令 | A 中止，A 的输出引脚复位，B 立即开始执行 |
| TC-P7.5.8 | 日志回显 | 脚本中有 `print("hello")` | 网页日志面板显示 "hello" |
| TC-P7.5.9 | 按键进入配置 | 运行中长按 BOOT 键 3 秒 | SoftAP ESP-LEGO-Setup 出现 |
| TC-P7.5.10 | 自动退出配置 | 进入配置模式后 5 分钟无操作 | SoftAP 自动关闭，恢复正常运行 |

### 风险点

- **WiFi 模式切换复杂性**: SoftAP ↔ STA 切换涉及 Wi-Fi 驱动的完整去初始化/重新初始化，可能触发未预期的时序问题。→ 在 llm_client 中使用 `esp_wifi_set_mode()` 而非 `esp_wifi_deinit/init`，减少状态转换环节
- **HTTP 服务器栈溢出**: `esp_http_server` 默认栈可能不足以处理带 JSON 解析的复杂请求。→ 设置 `HTTP_SERVER_STACK_SIZE=6144`（6KB），留足够余量
- **LLM API Key 泄露**: 虽然仅限本地热点访问，但攻击者连接热点后可能通过 GET 获取 key。→ 双重防护：① GET `/api/config` 用 `"***"` 掩码返回 key 字段、`wifi_pass` 不返回；② `/api/config/wifi` 端点**不接受** `llm_key`，即使误提交也不写入 NVS
- **print 环形缓冲区竞争**: `exec_task` 写入日志缓冲区，`web_console` 的 HTTP handler 读取，形成跨任务竞争。→ 使用互斥锁保护缓冲区访问
- **HotSpot 创建失败**: 某些 ESP32-S3 开发板的射频校准或天线配置可能导致 SoftAP 创建失败。→ 设计回退到纯 UART 运行模式，不影响核心功能

---

## P8: 健壮性加固

**目标**: 将所有防御机制落地为代码。

**依赖**: P7.5（主控集成 + Web Console）

**估算**: 1d

**前置条件**:
- [ ] P7.5 验收通过，主控可执行完整脚本链路 + Web Console 页面可访问
- [ ] design.md §6.7（ExecutionContext）、§6.6（对象池）、§15（深度防御）已理解

**产出文件**: 在已有 .h/.cpp 文件中增量修改，无新文件

### 任务清单

- [ ] **P8.1** 深度防御：
      - `parse_depth` 在 Parser 每条递归路径入口 +1、出口 -1（已完成 P4 框架，此处确保全面覆盖）
      - `exec_depth` 在 Interpreter 的 `execute_block`/`eval`/`execute_while` 递归入口 +1、出口 -1（通过 ExecutionContext），**注意不是 `parse_depth`**（parse_depth 仅用于解析器，exec_depth 用于执行器，两者独立）
      - 超限时设置 `constraint_violated` 标志，返回/中止（详见 design.md §6.7）
- [ ] **P8.2** Watchdog（设计细节见 design.md §6.7）：
      - `exec_task` 创建 `xTimerCreate("watchdog", SCRIPT_EXEC_TIMEOUT_MS, pdFALSE, ...)`
      - 开始执行脚本前 `xTimerStart()`，结束后 `xTimerStop()`
      - 回调函数设置 `s_script_timeout = true`（使用 `atomic_store` 确保多核可见性）
      - `execute_block` 每条语句前检查 `s_script_timeout` / `ctx.script_timeout` → 跳出
      - 约束：回调运行在 timer task 上下文，**严禁获取任何互斥锁**
      - 异常退出路径：`exec_task` 因解析错误提前退出时须 `xTimerDelete(s_watchdog_timer, portMAX_DELAY)`
- [ ] **P8.3** 增强 `validate_resources()`：
      - 全局绑定 > 80% `MAX_BINDINGS` → 警告
      - 函数定义 > 90% `FUNC_POOL_SIZE` → 警告
      - AST 树深度 > 80% `MAX_PARSE_DEPTH` → 警告
      - 列表总元素 > 50 或循环内 `list_new` → 警告（动态参数标注 `[runtime]`）
- [ ] **P8.4** 全局清理增强（详见 design.md §6.6/§15.4）：
      - `reset_pool()` 重置：`pool_used` + AST 对象池全部清零
      - `ctx.reset()` 重置：所有 ExecutionContext 字段 + `list_pool_used[]` + `func_pool_used[]`
      - 悬空指针保护：`func_pool[i].body` 在 `reset_pool()` 时置 NULL
      - 池状态统一由 ExecutionContext 管理：取消全局 `list_used[]`/`func_used[]`
      - 在所有退出路径（正常结束/超时/解析错误）上确认调用
- [ ] **P8.5** 运行时约束增强：
      - 连续 `remote_read` 检测：相邻两条语句均为 `FUNC_CALL(REMOTE_READ)` 时日志警告
      - 循环内 `list_new` 检测：`execute_while` 迭代体中出现 `list_new` 时警告
      - `exec_task` 创建 `ExecutionContext` 后注入 watchdog 回调关联，确保 watchdog 和 `ctx.script_timeout` 联动
- [ ] **P8.6** on_script_end() 硬件安全钩子：
      - 每次脚本结束（正常/超时/解析错误），先调用 `on_script_end()` 将操作过的输出引脚置为安全值 0
      - `exec_task` 中维护已操作引脚列表（局部数组），`on_script_end()` 遍历并调用 `digital_write(pin, 0)`

### 验收标准 (测试用例)

| ID | 名称 | 步骤 | 预期结果 |
|----|------|------|---------|
| TC-P8.1 | Watchdog 中止死循环 | 执行 `while (true) {}` | 30s 后 watchdog 触发，脚本中止，控制权返回 |
| TC-P8.2 | 深度解析中止 | 构造深度 > MAX_PARSE_DEPTH 的脚本 | 解析中止，错误信息包含深度限制 |
| TC-P8.3 | 深度执行中止 | 构造深度递归函数调用 | 执行中止，`exec_depth` 超限 |
| TC-P8.4 | 语句数超限 | 执行长脚本超过 MAX_EXEC_STATEMENTS | 触发约束违反标志，脚本中止 |
| TC-P8.5 | 池耗尽优雅降级 | 列表池满时调用 `list_new` | 返回全局空列表，`constraint_violated` 设置 |
| TC-P8.6 | on_script_end 触发 | 脚本超时中止后检查输出引脚 | 所有操作过的输出引脚为 0 |
| TC-P8.7 | validate_resources 阈值 | 构造接近 AST_POOL_SIZE 上限的脚本 | 拒绝执行，错误信息含配置值 |
| TC-P8.8 | 连续 remote_read 警告 | 执行 `remote_read(1); remote_read(2);` | 日志输出 "建议使用聚合函数" |
| TC-P8.9 | 解析错误后清理 | 发送语法错误脚本，检查池和定时器 | `reset_pool()` 被调用，watchdog 定时器已删除 |

### 风险点

- **Watchdog 与互斥锁死锁**: timer task 回调尝试获取 `s_peer_mutex` 或 `s_comm_mutex` 时，与持锁阻塞的 `exec_task` 形成死锁。→ 回调节约地使用原子操作，零锁获取（design.md §6.7 强约束）
- **atomic 跨核可见性**: ESP32-S3 双核架构下 `volatile` 不能保证缓存一致性。→ 使用 `atomic_store`/`atomic_load` 或 `__sync_synchronize()` 内存屏障
- **`on_script_end` 未覆盖路径**: 某些异常退出路径（如硬件故障）可能跳过清理。→ 在 `exec_task` 的 `catch`/错误处理块统一调用

---

## P9: 测试 + AI 物料

**目标**: 充分测试覆盖 + AI 集成文档。

**依赖**: P8（健壮性加固）

**估算**: 3d

**前置条件**:
- [ ] P8 验收通过，所有防御机制生效
- [ ] 至少 2 块 ESP32-S3 开发板（主控 + 子模块）
- [ ] pytest-embedded 环境已配置（`pip install pytest-embedded`）
- [ ] 串口转 USB 线缆 x2

**产出文件**:
```
tests/lexer_test.cpp
tests/parser_test.cpp
tests/interpreter_test.cpp
tests/peer_mgr_test.cpp
tests/comm_test.cpp
docs/ai_system_prompt.md
```

### 任务清单

#### P9.1 单元测试 (在 x86 或 ESP32 host test 上运行)

- [ ] **P9.1.1** Lexer 测试：各 Token 类型、注释、非法字符、字符串驻留一致性
- [ ] **P9.1.2** Parser 测试：各语法结构、嵌套、错误恢复、名称冲突检测、深度超限
- [ ] **P9.1.3** Interpreter 测试：变量作用域、if/else/while 控制流、函数调用+return、列表操作、聚合函数、环境隔离
- [ ] **P9.1.4** Peer mgr 测试：添加/查找/删除、超时老化、20 满覆盖、PENDING_CHANGE 去抖
- [ ] **P9.1.5** Comm 测试：并发锁争用、响应来源过滤（错误 source_mac）、重试超时

#### P9.2 集成测试 (ESP32 硬件，pytest-embedded)

- [ ] **P9.2.1** 子模块发送宣告 → 主控 peer 表更新
- [ ] **P9.2.2** `remote_read(module_id)` 同步读取
- [ ] **P9.2.3** 完整脚本执行：while + remote_read + if + send_motor
- [ ] **P9.2.4** 死循环脚本 → watchdog 中止
- [ ] **P9.2.5** 深度嵌套脚本 → 解析中止
- [ ] **P9.2.6** 并发请求冲突：连续快速 `remote_read(a); remote_read(b);` → 触发 `COMM_CONCURRENT_CHECK` 日志
- [ ] **P9.2.7** 响应来源过滤：模拟错误 source_mac 的 DATA_RESP → 被丢弃
- [ ] **P9.2.8** 20 槽位满：模拟 21 个设备 → 第 21 个无法加入并打印警告
- [ ] **P9.2.9** 老化重连：宣告 → 超时 → OFFLINE → 再次宣告 → ACTIVE 完整状态机
- [ ] **P9.2.10** validate_resources 阈值：构造接近 AST_POOL_SIZE 上限的脚本 → 拒绝执行
- [ ] **P9.2.11** 脚本间环境隔离：连续两条独立脚本，验证第二条不残留第一条的变量/函数
- [ ] **P9.2.12** 子模块 NVS 缺失：擦除 NVS 后重启子模块 → 使用默认名称并打印警告
- [ ] **P9.2.13** Peer 振荡压力：20 设备反复上下线 → 验证互斥锁保护和内存稳定性
- [ ] **P9.2.14** 24 小时稳定性测试：主控 + 3 子模块持续运行 24h，周期性执行脚本 → 无内存泄漏或状态异常
- [ ] **P9.2.15** 错误信息可读性验证：所有语法/运行时错误信息包含行号和建议修复 → 非专业用户可理解

#### P9.3 AI System Prompt 文档

- [ ] **P9.3.1** 完整 BNF 语法（from design.md §6.2）
- [ ] **P9.3.2** 内置函数列表（不含 list_free）
- [ ] **P9.3.3** 聚合函数优先原则
- [ ] **P9.3.4** 资源约束文本（按标准配置）
- [ ] **P9.3.5** 示例脚本（3-5 个，从简单到复杂）

### 验收标准 (测试用例)

| ID | 名称 | 步骤 | 预期结果 |
|----|------|------|---------|
| TC-P9.1 | 全部单元测试通过 | 运行测试二进制 | 所有测试 exit code 0 |
| TC-P9.2 | 全部集成测试通过 | `pytest tests/` | 所有测试 PASS |
| TC-P9.3 | AI 生成有效脚本（参考验证） | 用参考 LLM（如 GPT-4o, temperature=0）加载 system prompt，要求生成控制电机的脚本 | 脚本语法正确，可被解释器执行；结果因 LLM 版本有差异，非强制门禁 |
| TC-P9.4 | 24h 稳定性 | 运行 TC-P9.2.14 | 无崩溃、无内存增长、无死锁 |
| TC-P9.5 | 错误可读性 | 按错误来源清单（lexer 非法字符/parser 语法错误/interpreter 未定义变量/comm 超时）逐条验证错误输出 | 每条含 line:col + 简短描述 + 建议修复 |
| TC-P9.6 | 代码覆盖率 | 分析单元测试覆盖率（若 gcov 不可用则改为人工代码走查 + 功能覆盖矩阵） | 核心模块（lexer/parser/interpreter）行覆盖率 ≥ 80% 或功能覆盖矩阵 100% |

### 风险点

- **硬件集成测试偶发失败**: ESP-NOW 在 RF 噪声环境下可能偶尔丢包。→ 允许测试中的合理重试次数，区分"系统缺陷"和"RF 偶发"
- **24h 稳定性测试时间成本**: 阻塞开发进度 24 小时。→ 将 P9 内部分为"日间工作"和"过夜测试"两个时间块：
  1. **白天**: 编写所有单元测试 + 集成测试（除 TC-P9.2.14）+ AI 物料文档
  2. **晚间**: 启动 TC-P9.2.14（24h 稳定性测试）
  3. **次日**: 验证测试结果，撰写报告
  这样 24h 测试不阻塞其他开发，P9 的实际日历日 ≈ 2 天工时 + 1 夜自动化
- **pytest-embedded 配置**: 需要正确的串口映射和 DUT 配置。→ 参考现有 `pytest_hello_world.py` 创建测试框架基类，基类中统一处理串口发现和 DUT 初始化
- **代码覆盖率门槛**: ESP-IDF 构建的 host test 可能不完全支持 gcov。→ 优先覆盖核心解释器逻辑（lexer/parser/interpreter），通信层可用集成测试覆盖
