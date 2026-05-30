# ESP-LEGO 分布式硬件控制系统 — 设计文档

## 1. 项目概述

在 ESP32 上使用 ESP-IDF v5.x + C++17 构建分布式硬件控制系统。一个主控模块运行轻量脚本解释器，多个传感器/执行器模块通过 ESP-NOW 通信。AI（大语言模型）根据用户自然语言生成脚本，由主控解释执行。

## 2. 目录结构

```
Esp lego/
├── CMakeLists.txt                       # 项目级 CMake
├── Kconfig                              # 全局 Kconfig 配置
├── docs/
│   └── design.md                        # 本设计文档
├── main/
│   ├── CMakeLists.txt
│   ├── Kconfig.projbuild                # 设备角色: MASTER / SENSOR
│   └── app_main.cpp                     # 入口函数
├── components/
│   ├── interpreter/                     # 脚本解释器 (C++17)
│   │   ├── CMakeLists.txt
│   │   ├── include/interpreter/
│   │   │   ├── token.h                  # Token 类型定义
│   │   │   ├── lexer.h                  # 词法分析器
│   │   │   ├── ast.h                    # AST 节点定义 + 对象池
│   │   │   ├── parser.h                 # 递归下降解析器
│   │   │   ├── value.h                  # 值系统 (union)
│   │   │   ├── environment.h            # 作用域环境
│   │   │   ├── builtins.h               # 内置函数声明
│   │   │   └── interpreter.h            # 解释器入口
│   │   └── src/
│   │       ├── lexer.cpp
│   │       ├── parser.cpp
│   │       ├── interpreter.cpp
│   │       └── builtins.cpp
│   ├── espnow_comm/                     # ESP-NOW 通信组件
│   │   ├── CMakeLists.txt
│   │   ├── include/espnow_comm/
│   │   │   ├── protocol.h               # 消息格式 + 命令常量
│   │   │   ├── peer_mgr.h               # Peer 管理器
│   │   │   └── comm.h                   # 收发 + 同步请求 API
│   │   └── src/
│   │       ├── peer_mgr.cpp
│   │       ├── comm.cpp
│   │       └── protocol.cpp
│   ├── hw_drivers/                      # 本地硬件抽象
│   │   ├── CMakeLists.txt
│   │   ├── include/hw_drivers/
│   │   │   └── drivers.h
│   │   └── src/
│   │       └── drivers.cpp
│   └── script_io/                       # 脚本输入 (UART)
│       ├── CMakeLists.txt
│       ├── include/script_io/
│       │   └── script_io.h
│       └── src/
│           └── script_io.cpp
```

## 3. 构建配置

模块类型通过 Kconfig 选择:

- **CONFIG_DEVICE_ROLE_MASTER** — 包含解释器 + ESP-NOW 管理 + 所有组件
- **CONFIG_DEVICE_ROLE_SENSOR** — 仅包含 espnow_comm(发送宣告) + hw_drivers

| 模块 | Flash | RAM | 组件 |
|------|-------|-----|------|
| Master | ~200KB | ~40KB | 全组件 |
| Sensor | ~80KB | ~20KB | espnow_comm + hw_drivers |

## 4. 设备发现机制

### 4.1 机制概述

子模块上电后定期向广播地址 FF:FF:FF:FF:FF:FF 发送宣告包。主控被动接收，动态维护设备列表。子模块电池供电，对实时发现延迟容忍度高（秒级）。

### 4.2 宣告包格式 (16 字节)

```
[0x10: msg_type][16B: module_name]
```

| 字段 | 长度 | 说明 |
|------|------|------|
| msg_type | 1 字节 | 固定 0x10 (宣告消息) |
| module_name | 16 字节 | 可读名称, 例如 "kitchen_temp", 不足 \0 补齐 |
| 总计 | 16 字节 | 远小于 ESP-NOW 250 字节限制 |

> **module_id 分配**: 子模块不再携带固定 module_id。主控收到宣告包后，根据源 MAC 地址分配顺序 module_id (1, 2, 3...)，存入 peer 表。宣告包**不包裹在** §7.1 的通用消息头中（无 version/target_id/seq_id/cmd_id/payload_len 字段）。

### 4.3 子模块行为

```c
void announce_task(void*) {
    uint8_t broadcast_mac[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
    esp_now_add_peer(broadcast_mac, ...);

    uint8_t pkt[17];     // header(1) + name(16)
    fill_announce_packet(pkt, module_name);

    while (1) {
        esp_now_send(broadcast_mac, pkt, sizeof(pkt));
        int jitter = (esp_random() % (CONFIG_ANNOUNCE_JITTER_MS * 2)) - CONFIG_ANNOUNCE_JITTER_MS;  // 使用硬件 RNG，避免 rand() 未初始化种子的确定性序列
        vTaskDelay(pdMS_TO_TICKS(CONFIG_ANNOUNCE_INTERVAL_MS + jitter));
    }
}

子模块宣告包中仅包含 `module_name`（名称）。`module_id` 由主控在收到宣告时根据连接顺序依次分配（1, 2, 3...），不存储在子模块固件中。
```

### 4.4 主控行为

接收回调将数据包送入队列，后台任务解析并更新 peer 表:

```
esp_now_recv_cb (运行在 Wi-Fi 任务上下文，非 ISR)
  → xQueueSend(rx_queue, pkt)        ← 使用 xQueueSend，非 xQueueSendFromISR
                  ↓
             处理任务 (解析 + 更新 peer_mgr)      ← 持有 s_peer_mutex
                  ↓
             老化任务 (1s 周期扫描 → 标记 OFFLINE) ← 持有 s_peer_mutex
```

> **锁竞争**: 处理任务和 `exec_task`（`remote_read` 调用 `peer_mgr_find_*`）都可能获取 `s_peer_mutex`。处理宣告包时的去抖判断（多次宣告确认更新）消耗的持有时间最长，应保持严格的最小持有——只在具体的查找/复制/更新操作上加锁，去抖逻辑本身**不在锁内**（使用局部变量缓存判断状态）。老化任务同理，扫描过程不应全程持锁，应在锁定状态下逐个判断条目后立即释放。

## 5. Peer 管理器

### 5.1 PeerEntry

设备唯一标识 = `MAC[6]` + `module_id[1]` 复合键，两者联合作为 `find_by_mac_and_id()` 的匹配条件，避免单字段冲突导致设备错控。

```cpp
#define MAX_PEERS 20
#define MAX_NAME_LEN 16

#define PEER_ID_STR_LEN 22  // "xx:xx:xx:xx:xx:xx:xxx\0" = "aa:bb:cc:dd:ee:ff:255\0" = 21 + 1

struct PeerEntry {
    uint8_t mac[6];
    uint8_t module_id;
    char name[MAX_NAME_LEN];
    uint32_t capabilities;
    uint32_t last_seen_ms;
    char peer_id[PEER_ID_STR_LEN];  // 复合标识: "mac_hex:module_id"
    enum { PEER_NEW, PEER_ACTIVE, PEER_OFFLINE, PEER_PENDING_CHANGE } state;
    uint8_t dedup_seq[8];           // 去重滑动窗口，由 peer 处理任务（非 rx 回调）在持有 s_peer_mutex 时维护
};
```

> **栈溢出保护**: 递归下降解析器和解释器共用在 `exec_task` 栈上运行（三阶段：Lexer → Parser → Interpreter 全部在同一个栈上）。`MAX_PARSE_DEPTH`（默认 32）限制递归深度。每条 C++17 递归路径约消耗 80-120 字节栈（比 C 栈帧大），32 层深度约 2.5-4KB。解释器自身的 `exec_depth` 可能叠加。**`exec_task` 任务栈应设为 8KB**（`configMINIMAL_STACK_SIZE * 8` 或显式 `8192`），确保最坏情况下（解析深度 + 执行深度叠加）不溢出。若栈缩小需同步降低 `MAX_PARSE_DEPTH`。

逻辑 ID（给 AI/脚本使用） vs 物理 ID（系统内部）分离：
- **物理 ID**: `MAC + module_id` 复合键，系统内部唯一匹配
- **逻辑 ID**: `module_id`（数字）或 `name`（字符串），脚本层通过 `remote_read(1)` 或 `remote_read("kitchen_temp")` 调用，内部翻译为物理 ID

### 5.2 状态机

```
NEW ──add_peer成功──→ ACTIVE ──老化超时→esp_now_del_peer──→ OFFLINE
                        ↑                                        │
                        └──────────── 再次收到宣告 ────────────────┘
                                                                  │
                                                            槽位满时被覆盖作为新条目

**去抖机制**: 当已 ACTIVE 条目的 `module_id` 或 `mac` 发生变化时（例如同一 MAC 发来不同的 module_id），不立即更新，而是进入 `PENDING_CHANGE` 暂态。若连续 2 次宣告（间隔 ~3 秒）信息一致，才接受变更（计数式去抖：最多等待两次宣告，超时未收到第二次则回退原状态）。第 2 次宣告超时未到则回退旧状态。这防止信号反射或干扰导致 peer 信息"乒乓效应"。

> **回退超时**: `PENDING_CHANGE` 的回退超时由 `CONFIG_PEER_PENDING_TIMEOUT_MS`（默认 6500 = `ANNOUNCE_INTERVAL_MS × 2 + 500ms`）控制。进入暂态后等待该时长，若仍未收到匹配的第二次宣告，自动回退到 `ACTIVE`。此窗口平衡了去抖效果（太短→收到延迟宣告时拒绝更新）和响应速度（太长→设备变更延迟 6+ 秒才生效）。
```

### 5.3 查找 API

```cpp
// 查找 API — 通过 out_conflict 输出冲突标志
PeerEntry* peer_mgr_find_by_mac(const uint8_t* mac, bool* out_conflict);       // out_conflict: 输出同名/同 module_id 冲突标志
PeerEntry* peer_mgr_find_by_id(uint8_t module_id, bool* out_conflict);          // out_conflict: 若多个设备共享该 module_id 则设为 true
PeerEntry* peer_mgr_find_by_name(const char* name, bool* out_conflict);         // out_conflict: 若多个设备同名则设为 true
PeerEntry* peer_mgr_find_by_mac_and_id(const uint8_t* mac, uint8_t module_id);  // 复合键精确匹配（无冲突标志，因 (MAC,module_id) 天然唯一）
int peer_mgr_active_count();                                 // 在线数量
void peer_mgr_list(char* out, size_t out_size);              // 格式化列表，每行格式 "id:name"（不含 capabilities，仅用于 AI 识别；capabilities 通过独立 API 查询）
```

> **冲突处理**: 调用方（如内置函数 `peer_online`, `remote_read`, `espnow_send`）在获取 `PeerEntry*` 后必须检查 `*out_conflict`。若为 true：严格模式下中止脚本，宽松模式下日志警告并使用第一个匹配项。

### 5.4 暴露给解释器的内置函数

| 函数 | 返回值 | 说明 |
|------|--------|------|
| `list_peers()` | string | 在线模块列表，每行格式 `{id}:{name}`。内部调用 `peer_mgr_list()` |
| `peer_count()` | number | 当前在线模块数 |
| `peer_online(id)` | bool | 指定模块是否在线。严格模式下名称冲突直接中止脚本 |

## 6. 脚本解释器

### 6.1 语言特性

- 变量声明 (`var x = expr`)
- 赋值 (`x = expr`)
- `if`/`else` 条件
- `while` 循环
- 函数调用 (内置函数 + 用户自定义函数)
- 用户函数定义 (`func name(params) { ... }`)
- `return` 语句 (仅函数体内有效)
- 轻量列表 (`list_new`, `list_get`, `list_set`, `list_len`)
- 无数组、无对象
- 类型: 数字(double)、布尔(bool)、字符串(const char*)、列表(list)、函数(func)

### 6.2 BNF 语法 (AI 生成目标)

```
program      = statement*
statement    = var_decl | if_stmt | while_stmt | block | expr_stmt
             | func_decl          // 函数定义
             | return_stmt        // return 语句

func_decl    = "func" IDENTIFIER "(" [IDENTIFIER ("," IDENTIFIER)*] ")" block
return_stmt  = "return" expression ";"

var_decl     = "var" IDENTIFIER "=" expression ";"
if_stmt      = "if" "(" expression ")" statement ("else" statement)?
while_stmt   = "while" "(" expression ")" statement
block        = "{" statement* "}"
expr_stmt    = expression ";"

expression   = assignment
assignment   = IDENTIFIER "=" expression | logic_or
logic_or     = logic_and ("||" logic_and)*
logic_and    = equality ("&&" equality)*
equality     = comparison (("==" | "!=") comparison)*
comparison   = term (("<" | ">" | "<=" | ">=") term)*
term         = factor (("+" | "-") factor)*
factor       = unary (("*" | "/") unary)*
unary        = ("!" | "-") unary | call
call         = IDENTIFIER "(" [expression ("," expression)*] ")" | primary
primary      = NUMBER | STRING | "true" | "false" | IDENTIFIER | "(" expression ")"
```

### 6.3 流水线

```
源码 ──→ 词法分析 (Lexer) ──→ Token 流 ──→ 递归下降解析 (Parser) ──→ AST ──→ 遍历解释执行 (Interpreter)
```

### 6.4 Token 类型

```cpp
enum class TokenType {
    // 字面量
    NUMBER, STRING, IDENTIFIER,
    // 关键字
    VAR, IF, ELSE, WHILE, TRUE, FALSE,
    FUNC, RETURN,                    // 函数定义 + return
    // 运算符
    PLUS, MINUS, STAR, SLASH,          // + - * /
    EQ, NEQ, LT, GT, LE, GE,           // == != < > <= >=
    ASSIGN,                            // =
    NOT, AND, OR,                      // ! && ||
    // 分隔符
    LPAREN, RPAREN, LBRACE, RBRACE, SEMICOLON, COMMA,
    // 特殊
    END
};
```

### 6.5 AST 节点类型

```cpp
enum class NodeType {
    PROGRAM, BLOCK,
    VAR_DECL, ASSIGN,
    IF, WHILE,
    BINARY_OP, UNARY_OP,
    LITERAL_NUM, LITERAL_STR, LITERAL_BOOL, IDENT,
    FUNC_CALL,
    FUNC_DEF,       // 函数定义
    RETURN_STMT     // return 语句
};
```

### 6.6 对象池

```cpp
#define AST_POOL_SIZE       CONFIG_AST_POOL_SIZE
#define MAX_PARSE_DEPTH     CONFIG_MAX_PARSE_DEPTH
#define MAX_LOOP_ITERATIONS CONFIG_MAX_LOOP_ITERATIONS

#define MAX_POOL_SIZE 4096  // 硬上限, 即使 CONFIG 设更大也截断
#if AST_POOL_SIZE > MAX_POOL_SIZE
#undef AST_POOL_SIZE
#define AST_POOL_SIZE MAX_POOL_SIZE
#endif

ASTNode pool[AST_POOL_SIZE];
int pool_used = 0;

ASTNode* alloc_node() {
    if (pool_used >= AST_POOL_SIZE) return nullptr;  // 池耗尽返回 NULL
    return &pool[pool_used++];
}
void reset_pool() { pool_used = 0; }
// 注意: reset_pool() 只清理 AST 节点池的 pool_used 计数器。
// func_pool[i].body 悬空指针需由 func_free() 或 ctx.reset() 统一清 NULL。
// 见 §15.4 func_free() 和 ExecutionContext::reset() 的实现。

// 列表对象池 (List) — 声明在全局，分配/释放经由 ctx->list_pool_used[]
#define LIST_POOL_SIZE    CONFIG_LIST_POOL_SIZE
static List list_pool[LIST_POOL_SIZE];

// 函数对象池 (用户自定义函数) — 声明在全局，分配/释放经由 ctx->func_pool_used[]
#define FUNC_POOL_SIZE    CONFIG_FUNC_POOL_SIZE
static FuncObj func_pool[FUNC_POOL_SIZE];
```

> **intern_table 指针生命周期**: 标识符字符串的源码缓冲区（来自 Lexer 的 input）必须在整个执行期间存活，`env_restore_pristine` 不释放字符串。如果 script_io 释放了缓冲区，后续 env_get 访问 `Binding.name` 将产生悬空指针。实现需确保 `ExecutionContext` 持有输入脚本的引用或拷贝。

### 6.7 ExecutionContext

所有运行时状态收敛到 `ExecutionContext` 结构体，显式传递，消除全局变量：

```cpp
#define MAX_LIST_POOL_TRACK 64
#define MAX_FUNC_POOL_TRACK 64
#define MAX_NESTED_LOOPS 16  // 支持最多 16 层嵌套循环

struct ExecutionContext {
    int parse_depth;
    int exec_depth;               // 执行递归深度（独立于 parse_depth）
    bool has_returned;
    Value return_value;         // 单槽设计：A 调用 B 时，B 的 return 覆盖此字段。调用者（execute_block）在 B 返回后的下一个检查点立即消费 → 复制到局部变量，确保 A 继续执行时不再读取 ctx.return_value

    // (已移除: script_timeout — 统一使用 constraint_violated 处理内部约束违反)
    // 内部超时(语句数超限)走 constraint_violated, 外部超时(watchdog)走全局 s_script_timeout

    int loop_iterations[MAX_NESTED_LOOPS];
    int loop_depth;

    int total_statements;
    int sensor_calls_total;

    bool constraint_violated;
    const char* violation_msg;

    int list_pool_used[MAX_LIST_POOL_TRACK];
    int func_pool_used[MAX_FUNC_POOL_TRACK];

    void reset() {
        parse_depth = 0;
        exec_depth = 0;
        has_returned = false;
        loop_depth = 0;
        memset(loop_iterations, 0, sizeof(loop_iterations));
        total_statements = 0;
        sensor_calls_total = 0;
        constraint_violated = false;
        violation_msg = nullptr;
        memset(list_pool_used, 0, sizeof(list_pool_used));
        memset(func_pool_used, 0, sizeof(func_pool_used));
    }
};
```

`execute_block` 每条语句后执行:
```
ctx.total_statements++;
if (ctx.total_statements > MAX_EXEC_STATEMENTS) {
    ctx.constraint_violated = true;
    ctx.violation_msg = "执行语句数超过上限 (50000条/脚本)";
}
if (ctx.constraint_violated) break;
if (s_script_abort_requested) break;   // Web Console 用户主动中止
```

`execute_while` 每次迭代前推入循环深度，迭代后自增:
```
int depth = ctx.loop_depth++;
ctx.loop_iterations[depth] = 0;
while (eval(cond, env)) {
    ctx.loop_iterations[depth]++;
    if (ctx.loop_iterations[depth] > MAX_LOOP_ITERATIONS) {
        ctx.constraint_violated = true;
        ctx.violation_msg = "循环迭代超过上限";
        break;
    }
    execute(body, env);
    if (ctx.has_returned || ctx.constraint_violated) break;
    if (s_script_abort_requested) break;        // Web Console 用户主动中止
}
ctx.loop_depth--;
```

`remote_read` 调用时:
```
ctx.sensor_calls_total++;
if (ctx.sensor_calls_total > MAX_SENSOR_CALLS_PER_SCRIPT) {
    ctx.constraint_violated = true;
    ctx.violation_msg = "remote_read 调用次数超过限制 (20次/脚本)";
    return 0.0;
}
```

**软件 Watchdog**: `exec_task` 中创建 FreeRTOS 软件定时器 `s_watchdog_timer`，超时周期为 `CONFIG_SCRIPT_EXEC_TIMEOUT_MS`（默认 30s）。

Watchdog 回调运行在 FreeRTOS timer task 上下文，**严禁获取任何互斥锁**。回调仅设置一个**全局无锁原子标志** `atomic_bool s_script_timeout`（使用 `atomic_store` 写入，`atomic_load` 读取），`execute_block` 每条语句前读取判断是否超时。

**三标志退出模型**（避免语义混淆）：

| 标志 | 写入者 | 含义 | 处理 |
|------|--------|------|------|
| `s_script_timeout` | watchdog 回调 | 外部异常：执行超时 | `on_script_end()` + 日志 `ESP_LOGE` |
| `s_script_abort_requested` | web_console | 用户主动切换脚本 | `on_script_end()` + 日志 `ESP_LOGW` |
| `ctx.constraint_violated` | 执行器自身 | 内部约束违反（语句数/循环/池耗尽） | `on_script_end()` + 错误信息含具体原因 |

三者任一为 true 均跳出当前语句执行循环。`ctx.script_timeout` 字段已移除，所有内部超时行为统一走 `constraint_violated`。

`volatile` 确保编译器每次读取该标志时从内存重新加载，而非优化到寄存器副本。但在多核 ESP32-S3 上，timer task 与 `exec_task` 可能运行在不同核心，`volatile` 无法保证缓存一致性。需使用 `atomic_store(&s_script_timeout, true)` / `atomic_load(&s_script_timeout)` 或显式 `__sync_synchronize()` 内存屏障。实现时在 P8 中确认。

此外，当 `exec_task` 因解析错误提前退出（watchdog 定时器尚未触发）时，须先调用 `xTimerStop(s_watchdog_timer, portMAX_DELAY)` 确保定时器停止，再调用 `xTimerDelete(s_watchdog_timer, portMAX_DELAY)` 删除。**必须两步**：`xTimerDelete` 并非同步操作，不先 `xTimerStop` 的话，回调可能仍在待执行队列中，在删除期间或之后触发，导致回调在 `exec_task` 栈已释放后写入 `s_script_timeout`，造成野指针访问。

### 6.8 值系统

```cpp
#define MAX_LIST_SIZE 16
#define MAX_FUNC_PARAMS CONFIG_MAX_FUNC_PARAMS

// 轻量列表 (仅存储 double)
struct List {
    double data[MAX_LIST_SIZE];
    uint8_t len;
};

// 内置函数 ID 枚举 (用于 builtin_funcs 索引)
enum BuiltinFuncId {
    BIF_READ_SENSOR = 0,
    BIF_SEND_MOTOR,
    BIF_DIGITAL_READ,
    BIF_DIGITAL_WRITE,
    BIF_ANALOG_READ,
    BIF_ANALOG_WRITE,
    BIF_SLEEP,
    BIF_PRINT,
    BIF_LIST_PEERS,
    BIF_PEER_COUNT,
    BIF_PEER_ONLINE,
    BIF_REMOTE_READ,
    BIF_ESPNOW_SEND,
    BIF_LIST_NEW,
    BIF_LIST_GET,
    BIF_LIST_SET,
    BIF_LIST_LEN,
    BIF_REMOTE_READ_AVG,
    BIF_REMOTE_READ_MAX,
    BIF_REMOTE_READ_MIN,
    BIF_LIST_FREE,
    BIF_COUNT  // = 27
};

// 内置函数使用独立静态数组 (BIF_COUNT 个)，不从用户 func_pool 分配，不受 ctx.reset() 影响
static FuncObj builtin_funcs[BIF_COUNT];

// 值系统
struct Value {
    enum Type { VAL_NUM, VAL_STR, VAL_BOOL, VAL_LIST, VAL_FUNC };
    Type type;
    union {
        double num;
        const char* str;       // 指向 intern 表或字面量, reset 后不失效
        bool boolean;
        List* list;            // 指向列表对象池 (reset 后需置 NULL)
        FuncObj* func;         // 用户函数指向 func_pool；内置函数指向 builtin_funcs 静态区
    };
};
```

### 6.9 作用域环境

```cpp
#define MAX_BINDINGS        CONFIG_MAX_BINDINGS
#define ENV_POOL_SIZE       CONFIG_ENV_POOL_SIZE   // 调用 Environment 对象池大小 (默认 4)

struct Binding {
    const char* name;
    Value value;
};

struct Environment {
    Environment* parent;
    Binding bindings[MAX_BINDINGS];
    int count;
};

// Environment 对象池 — 函数调用时分配，退出时释放
extern Environment env_pool[ENV_POOL_SIZE];
extern bool env_pool_used[ENV_POOL_SIZE];

Environment* env_alloc(Environment* parent);   // 从池中分配一个 Environment，设置 parent，count=0
void env_free(Environment* env);               // 释放回池
```

变量查找规则: 当前环境 → parent 链 → 运行时错误。

> **Environment 对象池说明**: 函数调用分配 Environment 的成本 = 标记 `env_pool_used[i] = true`（无内存清零，调用方自行初始化 count=0）。`ENV_POOL_SIZE=4` 支持最多 4 层嵌套函数调用（每层约 520 字节，总计 ~2KB 静态 RAM）。超出时 `env_alloc()` 返回 nullptr → 调用中止，错误 "函数嵌套超出上限"。
>
> **与 `MAX_EXEC_DEPTH` 的关系**: 有效函数嵌套上限 = `min(ENV_POOL_SIZE, MAX_EXEC_DEPTH)`。默认 ENV_POOL_SIZE=4 远小于 MAX_EXEC_DEPTH=64，实际限制为 4 层函数调用。调用者在排查"函数嵌套超出上限"错误时应优先检查 ENV_POOL_SIZE 而非 MAX_EXEC_DEPTH。若需更多嵌套，同步增大两个值。

### 6.9.1 脚本间环境隔离

每次脚本执行完成后，全局环境中用户定义的函数和变量必须被清理，避免残留到下一段脚本：

```
interpreter_init()
  ├─ 创建全局 Environment (parent = nullptr)   // 全局环境是 env_pool[0]，不在函数池中分配
  ├─ 注册所有内置函数
  ├─ 保存全局环境的深度快照 → s_pristine_env   // **静态分配**: static Environment s_pristine_env; 约 MAX_BINDINGS × sizeof(Binding) ≈ 1KB
  └─ 此快照在系统运行期间只读，作为后续每次恢复的基准

exec_task (每段脚本):
  ├─ reset_pool()                          // 清 AST/列表/函数对象池
  ├─ env_restore_pristine(global_env)      // 将 global_env 恢复为 s_pristine_env 的拷贝
  ├─ lex → parse → execute                 // 执行脚本，期间用户函数定义写入 global_env
  └─ reset_pool() + env_restore_pristine() // 脚本结束，清理所有用户态污染
```

`env_restore_pristine()` 实现：
```
env_restore_pristine(env):
    env->count = s_pristine_env->count
    memcpy(env->bindings, s_pristine_env->bindings,
           sizeof(Binding) * s_pristine_env->count)
    // 用户定义的所有变量/函数绑定被清零，内置函数保留
```

> **内置函数指针有效性说明**: 快照中存储的 `Value.func` 指向 `builtin_funcs[]` 静态数组，该数组在程序运行期间永不释放、无需回收。`env_restore_pristine()` 每次将 `global_env` 的 bindings 恢复为仅含内置函数的 `s_pristine_env` 副本，用户定义的变量/函数绑定被清除，但内置函数的 `Value.func` 指针永远指向有效静态对象，不会产生悬空。

这样保证每段脚本都在"出厂设置"的全局环境中开始执行，P9 的"脚本间环境隔离"测试自然通过。额外开销：一段 memcpy 约 32 × sizeof(Binding) ≈ 1KB，每秒最多执行数段脚本，完全可接受。

**STRICT_MODE**: 通过 `CONFIG_STRICT_MODE` 控制运行时错误的处理行为:

| 检查场景 | 严格模式 (y，默认) | 宽松模式 (n) |
|----------|-------------------|-------------|
| 未定义变量访问 | 脚本中止，错误信息含变量名和行号 | 返回 0/false + 日志警告 |
| peer 名称冲突 | 脚本中止，错误 "名称冲突: xxx 对应多个设备" | 日志警告，使用第一个匹配项 |
| `peer_online`/`remote_read` 参数类型不匹配 | 脚本中止 | 日志警告，尝试隐式转换 |
| 函数调用形参/实参数量不匹配 | 脚本中止（始终严格，无宽松选项——这是安全风险） | N/A（始终中止） |

> **设计原则**: 严格模式是默认且推荐的生产配置。宽松模式仅用于探索式使用（如实验新脚本）。函数参数数量不匹配始终中止，不受 STRICT_MODE 影响——因为这通常是 AI 生成错误的明确信号，宽松模式无法安全降级。

函数定义执行时将函数名与 FuncObj 绑定到当前环境 (顶层作用域)。函数调用时:

```
call_func(fn, args, ctx)
  ├─ 检查形参/实参数量一致
  ├─ env_alloc(global_env)                // 从 Environment 对象池分配，设 parent=全局环境
  ├─ if (local_env == nullptr) 中止       // 池耗尽，错误 "函数嵌套超出上限"
  ├─ 将形参绑定到新环境
  ├─ 重置 ctx.has_returned = false
  ├─ 执行函数体 (execute_block)
  ├─ result = consume_return_value(ctx)   // 读取 return_value 后立即消费
  ├─ env_free(local_env)                  // 释放 Environment 回池
  └─ return result                        // 返回 Value，不再依赖 ctx.return_value 传播
```

> **环境释放时序**: 必须在读取 `return_value` 之后、返回 Value 之前释放 Environment，确保返回的 Value 中不包含指向已释放环境的指针。

return 语句通过 `ExecutionContext.has_returned` 传播: `execute_block` 每条语句后检查，为 true 时停止遍历。`execute_while` 执行迭代体后检查并 break。

> **return_value 嵌套安全**: 函数 A 调用函数 B 时，B 的 `return` 写入 `ctx.return_value`。`execute_block` 在 B 返回后立即通过 `consume_return_value(ctx)` 将 `Value` 复制到 `call_func` 的局部变量中，然后才让 A 继续执行。这确保了嵌套调用时不会丢失返回值。`consume_return_value()` 实现为：`Value val = ctx.return_value; ctx.has_returned = false; return val;`（在读取后立刻清除标志，防止被后续语句误消费）。

### 6.10 内置函数

| 签名 | 说明 |
|------|------|
| `read_sensor(pin)` → number | 读取本地 GPIO 传感器（LEDC/ADC） |
| `send_motor(pin, speed)` → void | 控制本地电机 (PWM) |
| `digital_read(pin)` → number | GPIO 数字输入 |
| `digital_write(pin, val)` → void | GPIO 数字输出 |
| `analog_read(pin)` → number | ADC 读取 |
| `analog_write(pin, val)` → void | PWM/模拟输出 |
| `sleep(ms)` → void | 延时 |
| `print(val)` → void | 串口打印 + 写入环形缓冲区 `g_print_buffer`（供 Web 控制台捕获轮询） |
| `list_peers()` → string | 在线模块列表 |
| `peer_count()` → number | 在线模块数量 |
| `peer_online(id)` → bool | 模块是否在线 (支持 number/string 参数) |
| `remote_read(id)` → number / list | **远程读取**: 通过 ESP-NOW 同步请求-响应。子模块返回全部传感器值数组。单个传感器返回 number，多个传感器返回 list。支持 number/string 参数 |
| `espnow_send(id, cmd, data)` → void | 向模块发单向命令 |
| `list_new(size)` → list | 分配一个指定长度的列表 (初始值 0) |
| `list_get(lst, i)` → number | 获取列表第 i 个元素 |
| `list_set(lst, i, v)` → void | 设置列表第 i 个元素 (原地修改) |
| `list_len(lst)` → number | 返回列表长度 |
| `remote_read_avg(id_list)` → number | 读取多个远程传感器并返回平均值 (id_list 为逗号分隔字符串) |
| `remote_read_max(id_list)` → number | 读取多个远程传感器并返回最大值 |
| `remote_read_min(id_list)` → number | 读取多个远程传感器并返回最小值 |
| `list_free(lst)` → void | 显式释放列表槽位 |

> **命名规则**: `read_sensor` 固定映射到本地硬件。远程读取统一用 `remote_read` 前缀，语义清晰无歧义。

所有接受模块标识的内置函数 (`peer_online`, `remote_read`, `espnow_send`) 同时支持数字 ID 和字符串名称两种参数。内部优先用字符串查找 peer，找不到再尝试数字 ID：

> **AI System Prompt 注意**: `list_free` 是高级手动资源管理函数，不应出现在 AI 可见的内置函数列表中。AI 应优先使用 `remote_read_avg/max/min` 等聚合函数，无需感知池管理细节。

```cpp
Value builtin_remote_read(span<Value> args) {
    PeerEntry* peer = nullptr;
    if (args[0].type == VAL_NUM) {
        peer = peer_mgr_find_by_id((uint8_t)args[0].num);
    } else if (args[0].type == VAL_STR) {
        peer = peer_mgr_find_by_name(args[0].str);
    }
    // ... 后续同步请求
}
```

子模块模块名称通过 Kconfig 配置（`CONFIG_SENSOR_MODULE_NAME`），也可在 `sdkconfig.defaults.sensor` 中覆盖。传感器不再携带固定 `module_id`——主控在收到宣告时根据连接顺序依次分配。

## 7. ESP-NOW 通信协议

### 7.1 消息格式

V1.0 消息头:

```
[1B: version][1B: msg_type][1B: target_id][1B: seq_id][2B: cmd_id][4B: payload_len][payload...]
```

| 字段 | 长度 | 说明 |
|------|------|------|
| version | 1 字节 | 协议版本号，V1.0 = 0x01 |
| msg_type | 1 字节 | 消息类型 |
| target_id | 1 字节 | 目标模块 ID（广播=0xFF） |
| seq_id | 1 字节 | 序列号（用于去重 + 重传匹配） |
| cmd_id | 2 字节 | 命令 ID |
| payload_len | 4 字节 | 负载长度 |
| payload | 变长 | 负载数据 |

`seq_id` 由发送方维护递增（1 字节，范围 0~255，轮回周期 256）。接收方每个 `PeerEntry` 独立维护去重滑动窗口 `uint8_t dedup_seq[8]`，作为**环形位图**使用（非滑动队列）——`dedup_seq[seq_id % 8]` 存储最近在该槽位上收到的 seq_id，收到新包时检查槽位值是否匹配当前 seq_id。匹配则丢弃，不匹配则写入新值。不同设备的序列号相互独立，不会互相干扰。

> **环形位图 vs 滑动窗口**: 环形位图方案使用 `dedup_seq[seq_id % 8]` 哈希索引，而非顺序滑动窗口。优点是实现简单（O(1) 查重），缺点是 8 槽 × 256 取值 = 每槽覆盖 32 个 seq_id：当两个 seq_id 满足 `i ≡ j (mod 8)` 时会发生哈希冲突，误判为重复。V1.0 接受此风险：冲突概率在正常通信速率下极低；若未来需要更高精度，可替换为 8 条目显式滑动队列（O(8) 线性查找，无哈希冲突）。

CMD/ACK 配对用于重传：发送时记录 `(seq_id, timestamp)`，若超时未收到对应 ACK 则重试（最多 2 次）。

> **seq_id 轮回**: 由于 seq_id 仅 1 字节，发送约 256 个包后编号轮回。每个 peer 的去重窗口仅 8 条，旧序列号被移出窗口后不会被误判为重复。当连续发送速度超过每 256 包 × 网络延迟时，旧条目被新条目覆盖后才可能轮回碰撞，但此场景在物理约束下极少出现。V1.0 接受此风险，不做额外防护。
>
> **补充防护: 时间戳守卫**: 在重传场景下（高速重试 + seq_id 递增），可在 `dedup_seq[8]` 之外为每个 PeerEntry 增加一个 `last_valid_pkt_tick` 时间戳（FreeRTOS tick）。当收到 seq_id 匹配窗口的包但 `xTaskGetTickCount() - last_valid_pkt_tick > MAX_PKT_ACCEPT_DELAY_TICKS`（建议 5 秒），判定为超迟到达的旧重传包，予以丢弃。此防护是可选增强，V1.0 不强制实现。

### 7.2 消息类型

| MsgType | 值 | 方向 | 说明 |
|---------|-----|------|------|
| ANNOUNCE | 0x10 | 子模块 → 主控 | 设备宣告 |
| CMD | 0x20 | 主控 → 子模块 | 执行命令 |
| DATA_REQ | 0x30 | 主控 → 子模块 | 请求传感器数据 |
| DATA_RESP | 0x40 | 子模块 → 主控 | 响应传感器数据 (8B double) |
| ACK | 0x50 | 双向 | 确认 |

### 7.3 可靠性与重传机制

| 机制 | 实现 |
|------|------|
| 序列号去重 | 每个 PeerEntry 维护 `dedup_seq[8]` 环形位图（§7.1），`dedup_seq[seq_id % 8]` 存储该槽最近 seq_id。新包 hash 匹配槽位值则丢弃，不匹配则写入新值。**注意**：环形位图有哈希冲突（`i ≡ j mod 8` 的 seq_id 共享同一槽），V1.0 接受此风险 |
| 命令重试 | CMD/DATA_REQ 发送后启动 200ms 超时定时器，未收到 ACK/DATA_RESP 则重发（最多 2 次，合计 3 次尝试）。**每次重试使用新 seq_id**，避免被子模块去重窗口过滤。**约束**: 重试机制假定所有命令操作是幂等的——子模块对同一命令执行多次产生相同结果。当前 V1.0 仅 `remote_read()`（只读的 DATA_REQ 路径）支持重试。未来增加写操作命令（如电机控制）时，需明确标注是否允许重试 |
| 宣告包不重试 | ANNOUNCE 为周期性广播，天然冗余，无需重传 |
| 传输日志 | 每次重传记录 `ESP_LOGW`，3 次均失败记录 `ESP_LOGE` |

### 7.4 载荷格式与命令 ID

DATA_REQ 无载荷（子模块读取全部传感器），DATA_RESP 使用多值数组格式：

```
DATA_RESP payload: [1B: value_count][8B × N: double values...]
```

| 命令 | 值 | 说明 |
|------|-----|------|
| (保留) | 0x0000 | DATA_REQ/DATA_RESP 不使用 cmd_id，子模块靠 `msg_type` 分发处理 |
| 自定义 | 0x0001+ | `espnow_send()` 传递的 cmd_id，子模块 `cmd_task` 中 switch-case 分发 |

> **设计原则**: `cmd_id` 字段仅对 `MSG_CMD` 消息有意义。`MSG_DATA_REQ` 通过消息类型本身即可判定为"读传感器"操作，无需 cmd_id。Script 层保持 `remote_read()` (只读) vs `espnow_send()` (只发) 严格分离。

### 7.5 同步请求-响应流程 (remote_read)

```
exec_task (解释器线程):
  remote_read(3)
    → peer_mgr_lock()
    → peer_mgr_find_by_id(3)           // 查找 MAC (在锁内完成)
    → 复制目标 mac 到局部变量 dst_mac[6]
    → peer_mgr_unlock()
    → 检查 s_resp_pending (comm_lock 保护)
    → 设置 s_resp_pending = true, 复制 dst_mac → s_resp_expected_mac
    → 构造 DATA_REQ 包（无载荷）→ comm_unlock()
    →
    → for (retry = 0; retry <= 2; retry++) {   // 最多 3 次尝试
    →     pkt.seq_id = next_seq_id++;            // 每次重试使用新 seq_id
    →     s_resp_expected_seq = pkt.seq_id;         // 记录期望 seq_id，用于响应匹配
    →     esp_now_send(mac, pkt)
    →     result = xSemaphoreTake(resp_sem, 200ms)  // 单次等待 200ms
    →     if (result == pdTRUE) break               // 收到响应, 跳出重试循环
    →     ESP_LOGW("remote_read(%d): retry %d", id, retry + 1)
    → }
       │
        │ ← DATA_RESP 到达 →
        │   rx_task 收到 0x40
        │   → comm_lock()
        │   → 校验包中的 source_mac == s_resp_expected_mac[6]
        │   → 校验包中的 seq_id == s_resp_expected_seq         // 确保请求‑响应一一对应
        │   → 匹配: 提取 payload 中的 values 数组 → xSemaphoreGive(resp_sem)
        │   → 不匹配: 丢弃包, 日志 "响应来源 MAC 不匹配"（或 seq_id 不匹配）
        │   → comm_unlock()
        │
    → 根据 values 数量返回值:
      count=1 → return Value(num)       // 单传感器，返回数字
      count>1 → return Value(list)      // 多传感器，返回列表

  超时 (3 次均失败, 总耗时 ≈ 600ms):
    → comm_lock(); s_resp_pending = false; comm_unlock()
    → ESP_LOGE("remote_read(3): timeout after 3 attempts")
    → return Value(0.0)                // 脚本继续执行
```

> **TOCTOU 防范**: `peer_mgr_find_*` 返回的 `PeerEntry*` 指针在锁外可能失效（被老化任务修改）。因此 `peer_mgr_lock` 必须在查找和复制 MAC 的整个区间保持持有，释放锁后只使用局部副本 `dst_mac[6]` 进行后续操作。

### 7.6 comm 层 API

```cpp
// 供 builtins.cpp 调用
double espnow_comm_request_read(uint8_t module_id);

// rx_task 收到 DATA_RESP 时调用 (内部)
void espnow_comm_handle_resp(const uint8_t* source_mac, double value);
```

`espnow_comm_request_read()` 内部实现（V1.0 — 直接返回/拒绝模式）：

```
1. comm_lock()
2. 检查 s_resp_pending（CONFIG_COMM_CONCURRENT_CHECK）
   └─ 若为 true → comm_unlock(); 日志 "并发请求冲突"; 返回 0.0
3. 设置 s_resp_pending = true
4. 从局部变量复制 dst_mac → s_resp_expected_mac[6]
5. comm_unlock()
6. for (retry = 0; retry <= 2; retry++) {
7.     pkt.seq_id = ++s_current_seq_id;
8.     构造 DATA_REQ 包 → esp_now_send(mac, pkt)
9.     result = xSemaphoreTake(resp_sem, CONFIG_READ_TIMEOUT_MS)  // 200ms
10.    // seq_id + source_mac 的校验已由 espnow_comm_handle_resp() 在 give 信号量之前完成（见 §7.5）
11.    // 此处仅校验 resp_sem 确实被正确释放（非超时），不再重复 seq_id 检查
12.    if (result == pdTRUE) break         // 收到响应, 跳出循环
12.    if (retry < 2) ESP_LOGW("重试 %d", retry + 1)
13. }
12. comm_lock(); s_resp_pending = false; comm_unlock()
13. └─ 正常 (break) → comm_lock(); 取出 s_resp_value; comm_unlock(); 返回 resp_value
14. └─ 3 次均超时 → ESP_LOGE("超时"); 返回 0.0
```

**V1.1 改进（请求排队）**：增加环形请求队列 `req_queue[4]`。当 s_resp_pending 时，新 (module_id, mac) 对入队而非丢弃。`rx_task` 在完成当前响应后自动取出队列中下一个请求发送。对脚本层完全透明。

> **AI 约束**: V1.0 中 AI 生成的脚本应避免连续调用 `remote_read`。系统提示需强制要求用户一次最多使用一个 `remote_read`，批量需求使用 `remote_read_avg/max/min` 聚合函数。

### 7.7 全局状态 (comm 层)

```cpp
static SemaphoreHandle_t s_comm_mutex;  // 保护 comm 层全局状态
#define comm_lock()   xSemaphoreTake(s_comm_mutex, portMAX_DELAY)
#define comm_unlock() xSemaphoreGive(s_comm_mutex)

static SemaphoreHandle_t s_resp_sem;
static double s_resp_value;
static bool s_resp_pending;           // 防止并发请求误匹配
static uint8_t s_resp_expected_mac[6]; // 期望响应的 MAC[6], 用于精确过滤
static uint8_t s_resp_expected_seq;   // 当前期望的请求 seq_id，用于响应精确匹配
```

### 7.8 安全性说明

| 版本 | 加密 | 说明 |
|------|------|------|
| V1.0 | 明文 | 所有消息以明文传输，无线范围内可被监听 |
| V1.1 | ESP-NOW LMK + PMK | 通过 `esp_now_set_pmk()` 和 `esp_now_add_peer()` 的 LMK 参数配置 AES-CMAC 加密 |

V1.0 适用于隔离的专用控制网络（无敏感数据场景）。V1.1 升级路径已规划但不在初始实现范围内。

## 8. 主控任务架构

```
app_main()
├── espnow_init()
├── peer_mgr_init()
├── script_io_init()            // UART 接收脚本
├── hardware_init()
├── interpreter_init()          // 注册内置函数
│
├── xTaskCreate(rx_task)        // 处理 ESP-NOW 接收队列 → 协议解析 → peer_mgr 更新
├── xTaskCreate(timeout_task)   // 每秒扫描 peer 超时
├── xTaskCreate(shell_task)     // UART 读取脚本 → 消息队列
├── xTaskCreate(exec_task)      // 从队列取脚本 → lex → parse → interpret
│                              //   创建软件 watchdog 定时器 (SCRIPT_EXEC_TIMEOUT_MS)
│                              //   execute_block 每条语句前检查超时标志
│                              //   同时检查 s_script_abort_requested 以支持 Web Console 脚本切换
│
├── web_console_init()          // SoftAP + HTTP 服务器（若 CONFIG_WEB_CONSOLE_ENABLED）
│   ├── 首次启动/未配网 → 自动创建 ESP-LEGO-Setup 热点
│   ├── 长按 BOOT 键 (GPIO 0, 3 秒) → 手动进入配置模式
│   └── web_console_task 内处理 HTTP 请求: /, /api/config, /api/config/wifi, /api/config/llm, /api/ai, /api/scan, /api/exec_log, /api/script
│
└── [SENSOR 固件: announce_task + hw_control]
```

> **WiFi 模式切换**: LLM 调用时需经过 SoftAP→STA→SoftAP 的完整模式切换序列（见 §16.5），期间 HTTP 服务器暂停，网页客户端断连，LLM 响应返回后自动恢复。

## 9. 用户 → AI → 主控流程

### 9.0 双输入路径

V1.0 支持两条用户输入路径：

**路径 A — 串口（经典）**：
```
用户 (连接 UART): "每 5 秒读温度"
  ↓
AI (system prompt 含 BNF + 内置函数表)
  ↓ 生成脚本
while (true) { var temp = remote_read("kitchen_temp"); ... }
  ↓ 通过串口发送
主控 UART 接收 → shell_task → script_queue → exec_task → interpreter_exec(source)
```

**路径 B — Web UI（V1 新增）**：
```
用户 (手机连主控热点): 打开 192.168.4.1 → 配置 WiFi/LLM → 输入自然语言指令
  ↓
HTTP POST /api/ai {"prompt": "每 5 秒读温度, 超过 30 就开风扇"}
  ↓ 1. 获取在线设备列表 (list_peers())
  ↓ 2. 拼接 System Prompt（含 BNF + 设备清单 + 内置函数表 + 资源约束）
  ↓ 3. 按需连接路由器 WiFi（模式切换 SoftAP → STA）
  ↓ 4. 调 LLM API (HTTP POST)
  ↓ 5. 断开路由器 WiFi（模式切换 STA → SoftAP）
  ↓ 6. 提取脚本代码（去除 markdown 标记）
  ↓
设置 s_script_abort_requested → 等待当前脚本停止 → inject 新脚本到 script_queue → exec_task 执行
  ↓
解释器 lex → parse → validate_resources() → execute → 调用内置函数 → ESP-NOW 控制子模块
  ↓
print() 输出 → 写入 ring buffer → 网页通过 GET /api/exec_log 轮询获取
  ↓
脚本结束 / 超时中止 / 用户新指令 → on_script_end() → reset_pool() + env_restore_pristine() + 停止 watchdog 定时器
```

> **路径 B 核心差异**: 输入来源从 UART 变为 HTTP + LLM，执行后的输出反馈通过环形缓冲区回传到网页（而非仅串口）。脚本切换机制允许用户在任意时刻通过网页发送新指令覆盖当前脚本。

> **硬件安全钩子 `on_script_end()`**: 每次脚本结束（包括超时中止、解析错误、约束违反），先调用 `on_script_end()` 将过程中操作过的输出引脚（如电机 PWM pin）置为安全值（0/PIN_LOW），再执行软件清理。实现时在 `exec_task` 中维护一个已操作引脚列表（局部数组），`on_script_end()` 遍历该列表并调用 `digital_write(pin, 0)`。此机制确保异常退出时外设不会停留在危险电平。

> **全局清理**: `reset_pool()` 在脚本正常结束、超时中止、解析错误三种路径上均被调用，确保所有静态池（AST 节点、列表、函数对象）物理清零。`parse_depth` 和 `has_returned` 标志一并重置。同时调用 `env_restore_pristine()` 恢复全局环境到仅含内置函数的初始状态，实现脚本间环境隔离。

> **AI Prompt 隔离**: `list_free` 不出现在 AI 可见的内置函数列表中。AI 的工具描述中仅包含 `remote_read_avg/max/min` 聚合函数，引导优先使用并降低 AST 节点消耗。

### 9.1 设备信息注入

AI 在生成脚本时，需要知道当前系统中存在哪些设备（模块 ID、名称、功能）。流程如下：

```
主控: list_peers() → 获取 "{id}:{name}" 在线列表
  ↓ 拼接
System Prompt 设备清单部分:
  "当前在线设备:
   - id=1, name=kitchen_temp, capabilities=温度
   - id=3, name=exhaust_fan, capabilities=电机"
  ↓
AI 根据清单生成具体控制脚本
  while (true) {
      var t = remote_read("kitchen_temp");
      if (t > 30) { send_motor(3, 100); }
      sleep(2000);
  }
```

应用层（串口终端工具或 Web UI）负责在执行前从主控拉取 `list_peers()` 并填入 system prompt。主控解释器本身不参与此过程。

> **注入安全**: `module_name` 来自子模块 NVS（可能被篡改）。在注入 System Prompt 前，应对 `module_name` 做白名单过滤——仅允许 `[a-zA-Z0-9_\-]` 字符，拒绝含换行符、花括号、或 `"` 的名称。此过滤在 Web Console 的 `/api/ai` 处理程序中实施。

> **V1.0 设备清单手动注入**：用户手动查询后粘贴。
> 
> **V1.1 自动注入（Web Console 实现）**：Web Console 的 `/api/ai` 处理程序自动调用 `list_peers()` → 将 `id:name` 格式转换为 System Prompt 可读描述 → 将设备信息拼入 System Prompt → 发送给 LLM → 用户无需任何手动操作。此流程也适用于串口路径，上层 UART 工具可类似实现自动注入。
>
> **格式权威定义**: `list_peers()` 底层使用 `peer_mgr_list()`，输出规范格式 `{id}:{name}`（每行一个设备）。System Prompt 构建器负责将行解析为完整描述。三种格式并不矛盾：`peer_mgr_list()` 是数据层序列化格式，`list_peers()` 直接对外暴露，System Prompt 构建器做语义增强。

### 9.2 运行时约束强执行

AI 约束不仅仅停留在 system prompt，解释器运行时强制执行以下策略：

| 约束 | 实现方式 | 违反后果 |
|------|----------|----------|
| `remote_read` 总次数 | `ctx.sensor_calls_total` 计数器，>20次/脚本则拒绝 | 返回 0.0，日志警告，脚本继续 |
| 循环内 `list_new` | 静态分析 + 运行时 `ctx.loop_iterations` 检测 | 日志警告，返回安全空列表 |
| 连续 `remote_read` | 语句序列检测（相邻两条调用） | 日志警告 "建议使用聚合函数替代连续 remote_read" |
| 循环最大迭代 | `ctx.loop_iterations > MAX_LOOP_ITERATIONS` | 中止循环，设置 `ctx.constraint_violated` |
| 总执行语句 | `ctx.total_statements > MAX_EXEC_STATEMENTS` | 触发 watchdog 等价行为 |

> **设计原则**: AI prompt 是"引导"，运行时是"强制"。prompt 用于减少无效生成，运行时用于兜底。两者缺一不可。
```

## 10. 内存优化策略

| 手段 | 适用环节 |
|------|----------|
| AST 对象池 (Kconfig 可配) | 解析阶段 |
| 标识符字符串驻留 (intern table, 默认 128 条) | 词法/解析阶段 |
| Value 使用 union 而非 std::variant | 运行时 |
| Environment 固定数组 (Kconfig 可配) | 运行时 |
| 列表对象池 (Kconfig 可配) | 运行时 |
| 函数对象池 (Kconfig 可配) | 运行时 |
| 接收队列使用静态分配 FreeRTOS 队列 | 通信层 |
| 避免 std::vector / std::map / std::string | 全项目 |
| [**例外**] LLM 客户端路径：`esp_http_client` 内部堆分配 + `cJSON`/`malloc` 响应解析 + System Prompt heap 缓冲区 | Web Console LLM 调用（见 §16.6） |

## 11. Kconfig 参数汇总

| 参数 | 默认值 | 范围 | 所属 |
|------|--------|------|------|
| CONFIG_DEVICE_ROLE_MASTER | y | - | 角色选择 |
| CONFIG_ANNOUNCE_INTERVAL_MS | 3000 | - | 通信 |
| CONFIG_ANNOUNCE_JITTER_MS | 200 | - | 通信 |
| CONFIG_PEER_TIMEOUT_MS | 10000 | - | 通信 |
| CONFIG_PEER_PENDING_TIMEOUT_MS | 6500 | 1000-30000 | 通信 (PENDING_CHANGE 回退超时) |
| CONFIG_MAX_PEERS | 20 | - | 通信 |
| CONFIG_READ_TIMEOUT_MS | 200 | - | 通信 (单次同步请求超时, 最多重试 2 次, 总计 ≈ 600ms) |
| CONFIG_COMM_CONCURRENT_CHECK | y | - | 通信 (并发请求拦截) |
| CONFIG_AST_POOL_SIZE | 256 | 64-4096 | 解释器 |
| CONFIG_LIST_POOL_SIZE | 8 | 0-64 | 解释器 (列表) |
| CONFIG_FUNC_POOL_SIZE | 16 | 0-64 | 解释器 (函数) |
| CONFIG_MAX_BINDINGS | 48 | 16-128 | 解释器 |
| CONFIG_MAX_FUNC_PARAMS | 8 | - | 解释器 (函数形参) |
| CONFIG_MAX_PARSE_DEPTH | 32 | 16-256 | 解释器 (深度防御，与 8KB 任务栈匹配) |
| CONFIG_MAX_LOOP_ITERATIONS | 10000 | 100-1000000 | 解释器 (循环上限) |
| CONFIG_MAX_EXEC_STATEMENTS | 50000 | 1000-500000 | 解释器 (语句上限) |
| CONFIG_MAX_SENSOR_CALLS_PER_SCRIPT | 20 | 5-200 | 解释器 (远程读取调用限制) |
| CONFIG_SCRIPT_EXEC_TIMEOUT_MS | 30000 | 1000-600000 | 解释器 (看门狗) |
| CONFIG_STRICT_MODE | y | y/n | 解释器 (严格模式) |
| CONFIG_INTERN_TABLE_SIZE | 128 | 32-512 | 解释器 (标识符驻留表) |
| CONFIG_ENV_POOL_SIZE | 4 | 1-16 | 解释器 (函数调用 Environment 对象池) |
| CONFIG_SCRIPT_MAX_LEN | 2048 | 256-8192 | 脚本输入 (单条脚本最大字节数, 含 null) |
| CONFIG_SCRIPT_QUEUE_LEN | 4 | 1-16 | 脚本输入 (script_queue 深度；队满丢弃最旧) |
| CONFIG_MAX_EXEC_DEPTH | 64 | 16-256 | 解释器 (执行递归深度上限，独立于解析深度) |
| CONFIG_WEB_CONSOLE_ENABLED | y | y/n | Web Console (启用开关) |
| CONFIG_WEB_CONSOLE_TIMEOUT_SEC | 300 | 0-3600 | Web Console (配置模式自动退出时间) |
| CONFIG_WIFI_SCAN_SHOW_5GHZ | n | y/n | Web Console (扫描是否显示 5GHz) |
| CONFIG_BUTTON_PIN | 0 | 0-39 | Web Console (进入配置模式的 GPIO 引脚) |
| CONFIG_SOFTAP_CHANNEL | 1 | 1-11 | Web Console (SoftAP 信道；需避开目标路由器信道以减少切换成本) |
| CONFIG_EXEC_LOG_BUF_SIZE | 4096 | 1024-16384 | Web Console (print 输出环形缓冲区大小) |

## 12. 子模块固件

### 12.1 包含组件

- espnow_comm (仅发送宣告 + 接收主控命令/请求)
- hw_drivers (GPIO / ADC / PWM 操作)

### 12.2 条件编译分离

`espnow_comm` 组件通过 `CONFIG_DEVICE_ROLE_MASTER` 宏分离主控/子模块逻辑：

```cpp
// espnow_comm/CMakeLists.txt
if(CONFIG_DEVICE_ROLE_MASTER)
    target_sources(espnow_comm PRIVATE peer_mgr.cpp comm_full.cpp)
else()
    target_sources(espnow_comm PRIVATE comm_lite.cpp)  // 仅宣告+命令回调
endif()
```

| 模块 | 编译的文件 | 包含功能 |
|------|-----------|----------|
| 主控 | `protocol.cpp` + `peer_mgr.cpp` + `comm_full.cpp` | 完整: 发现/管理/同步请求 |
| 子模块 | `protocol.cpp` + `comm_lite.cpp` | 极简: 宣告发送 + 命令接收回调 |

子模块固件中不含 peer 管理器、信号量、同步请求逻辑，节省 ~2KB Flash + ~0.5KB RAM。

### 12.3 主循环

```
sub_app_main()
├── hardware_init()
├── espnow_init()
├── xTaskCreate(cmd_task)                  // 从 cmd_queue 取命令 → 解析 cmd_id → 操作硬件 → 回复
├── xTaskCreate(announce_task)             // 定期广播宣告
│
└── esp_now_register_recv_cb(on_command)   // 仅 xQueueSend → cmd_queue，不直接在 Wi-Fi 任务中操作硬件
```

DATA_REQ 处理：子模块读取全部预设传感器引脚，打包为多值数组返回：

```
case MSG_DATA_REQ:
    double values[N];                      // N = 传感器数量
    for each sensor pin:
        values[i] = hw_adc_read(pin);
    protocol_build_data_resp(buf, &len, id, 0, values, N);
    esp_now_send(src_mac, buf, len);       // 仅 DATA_RESP，无需 ACK
```

> **命令异步化**: 子模块的接收回调不直接操作 GPIO/ADC/PWM（这些操作可能阻塞或耗时），而是将原始数据包入队到 `cmd_queue`（FreeRTOS 静态队列，长度 4）。专用 `cmd_task` 从队列取出并处理：解析 `cmd_id` → 调用 `hw_drivers` → 构造 DATA_RESP/ACK 回复。这避免了 Wi-Fi 任务栈溢出，同时保持主控命令的低延迟响应。`cmd_task` 栈大小 2048 字节即可满足递归调用链。

## 13. 边界与异常处理

| 场景 | 处理 |
|------|------|
| 同一 module_id 不同 MAC | 标记为**ID 冲突**，该 module_id 禁止被 `peer_mgr_find_by_id` 寻址（通过 `out_conflict` 标志位通知调用方），直到冲突解除（仅保留最后收到的，前一条目标记覆盖/离线） |
| 同一 MAC 不同 module_id | 进入 PENDING_CHANGE 暂态，需连续 2 次确认才更新，防止乒乓效应 |
| 20 槽位全满且全部 ACTIVE | 新设备无法加入，打印警告 |
| remote_read 超时 (3 次尝试 × 200ms = 600ms 总等待) | 返回 0.0 + 错误日志，脚本继续 |
| 主控重启 | 全数组清零; esp_now_deinit 自动清除所有 peer 注册 |
| parser 遇到语法错误 | 返回错误信息，脚本中止，等待下一段脚本 |
| 运行时变量未定义（宽松模式） | 错误日志 + 返回 0/false，脚本继续 |
| 运行时变量未定义（严格模式） | 脚本中止 |
| remote_read 调用超限 (> MAX_SENSOR_CALLS_PER_SCRIPT) | 错误日志 + 返回 0.0 |
| 循环迭代超过 MAX_LOOP_ITERATIONS | 中止循环，设置约束违反标志 |
| 总执行语句超过 MAX_EXEC_STATEMENTS | 触发 watchdog 等价行为 |
| 连续 remote_read 调用检测 | 日志警告 "建议使用聚合函数" |
| 函数名与内置函数冲突 | 解析时报错，脚本中止 |
| 函数名与变量或已定义函数同名 | 解析时报错，脚本中止 |
| 形参/实参数量不匹配 (函数调用) | 运行时错误，脚本中止 |
| return 出现在函数体外 | 运行时错误，脚本中止 |
| 列表池用尽 (list_new) | 返回空列表，错误日志 |
| 列表索引越界 (list_get/set) | `list_get` 返回 0 + 日志警告；`list_set` **不执行写入**（确保不破坏内存），仅日志警告 + 返回 |
| AST 节点池耗尽 (解析时) | 脚本中止，错误信息包含当前配置值 |
| 函数池耗尽 (解析时) | 脚本中止 |
| 绑定池耗尽 (运行时变量定义) | 脚本中止 |
| 解析/执行深度超限 (> MAX_PARSE_DEPTH) | 立即中止，错误日志 "嵌套过深" |
| 脚本执行超时 (> SCRIPT_EXEC_TIMEOUT_MS) | watchdog 触发，强制中止循环，脚本结束 |
| 并发 remote_read 请求 (COMM_CONCURRENT_CHECK) | 返回 0.0，错误日志 "并发请求冲突" |
| DATA_RESP 的 source_mac 与期望不匹配 | 丢弃该包，日志警告 "响应来源 MAC 不匹配" |
| 名称查找冲突（严格模式） | 日志错误 "名称冲突: kitchen_temp 对应多个设备"，脚本中止 |

## 14. 扩展功能汇总

### 14.1 函数定义

| 项目 | 说明 |
|------|------|
| 关键字 | `func`, `return` |
| 新增 AST 节点 | `FUNC_DEF`, `RETURN_STMT` |
| 新增类型 | `Value::VAL_FUNC` + `FuncObj` 结构体 |
| 运行时机制 | 全局 `has_returned` 标志 + `return_value`; `execute_block` 每条语句后检查并提前退出 |
| 函数对象池 | `CONFIG_FUNC_POOL_SIZE` (默认 16) |
| 限制 | 仅顶层定义, 无嵌套, 无闭包, 无递归(不限制但不主动说明) |
| Flash 增量 | ~2KB |

脚本示例:

```js
func average(a, b, c) {
    return (a + b + c) / 3;
}

while (true) {
    var avg = average(read_sensor(1), read_sensor(2), read_sensor(3));  // 本地 GPIO 引脚 1/2/3
    print(avg);
    sleep(2000);
}
```

### 14.2 轻量列表

| 项目 | 说明 |
|------|------|
| 新语法 | 无 (通过内置函数操作) |
| 新增类型 | `Value::VAL_LIST` + `List` 结构体 |
| 存储 | `double data[16]` + `uint8_t len`, 从对象池分配 |
| 对象池 | `CONFIG_LIST_POOL_SIZE` (默认 8) |
| 语义 | 指针/引用语义, 赋值共享同一份数据 |
| 内置函数 | `list_new(size)`, `list_get(lst, i)`, `list_set(lst, i, v)`, `list_len(lst)` |
| Flash 增量 | ~0.5KB |

脚本示例:

```js
var temps = list_new(5);
var i = 0;
while (i < list_len(temps)) {
    list_set(temps, i, read_sensor(i + 1));  // 本地 GPIO 引脚
    i = i + 1;
    sleep(500);
}
```

### 14.3 名称寻址

| 项目 | 说明 |
|------|------|
| 新语法 | 无 |
| 新增 API | `peer_mgr_find_by_name()` |
| 参数扩展 | `read_sensor`, `peer_online`, `espnow_send` 同时支持 `number` 和 `string` |
| 名称来源 | 子模块 NVS 存储, 上电读取后填入宣告包 |
| 冲突处理 | 同名只匹配首个, 日志警告 |
| Flash 增量 | ~0.3KB |

脚本示例:

```js
while (true) {
    var t = remote_read("kitchen_temp");
    if (t > 30) { send_motor("exhaust_fan", 100); }
    sleep(2000);
}
```

### 14.4 总开销

| 资源 | 增量 |
|------|------|
| Flash | < 3KB |
| RAM (列表池) | 8 × 136 ≈ 1KB |
| RAM (函数对象池) | 16 × ~20 = 320 字节 |
| RAM (s_pristine_env 环境快照) | MAX_BINDINGS × sizeof(Binding) ≈ 32 × 16 ≈ 512 字节 (静态, 不含在堆中) |
| RAM (其他) | 忽略不计 |
| 总计 RAM 增量 | ~2KB (含 s_pristine_env) |

## 15. 资源限制与健壮性设计

### 15.1 设计哲学

ESP-LEGO 使用静态对象池来保证内存分配的确定性，避免堆碎片。然而，硬编码的池上限对不同类型的脚本和应用场景缺乏弹性。本章将 Kconfig 可配置化与五层防御体系融合，构建一个从"编译时配置 → 执行前检查 → 运行时降级 → 聚合函数减耗 → AI 源头约束"的完整健壮性方案。

核心目标：
- **确定性与灵活性兼顾**：通过 Kconfig 让同一套固件适配从简单到复杂的多种场景。
- **可预测的优雅降级**：在任何资源耗尽的情况下，系统都能给出清晰反馈，而非硬崩溃。
- **AI 友好**：资源边界清晰到可以被 system prompt 描述和约束。

### 15.2 第一层：Kconfig 可配置池 (编译时弹性)

#### 15.2.1 配置项定义

在 `components/interpreter/Kconfig` 中新增菜单：

```
menu "ESP-LEGO Interpreter"
    config AST_POOL_SIZE
        int "AST node pool size"
        default 256
        range 64 4096
        help
            Maximum number of AST nodes. Each node ~40 bytes.
            Simple scripts: 50-100 nodes. Complex scripts: 500+.
            Recommended: 256 (standard), 1024 (complex).

    config LIST_POOL_SIZE
        int "List pool size"
        default 8
        range 0 64
        help
            Maximum number of list objects. Each list ~136 bytes.
            Set to 0 to disable list feature entirely.

    config FUNC_POOL_SIZE
        int "Function object pool size"
        default 16
        range 0 64
        help
            Maximum number of user-defined functions. Each ~20 bytes.
            Set to 0 to disable user-defined functions.

    config MAX_BINDINGS
        int "Max bindings per scope"
        default 48
        range 16 128
        help
            Max variables/functions per scope. Built-in functions
            occupy 27 slots. User available = MAX_BINDINGS - 27.
            WARNING: With default 48, only 21 slots remain for user
            variables and functions. If scripts need >10 variables,
            increase this value accordingly.

    config MAX_SENSOR_CALLS_PER_SCRIPT
        int "Max remote_read calls per script"
        default 20
        range 5 200
        help
            Maximum number of remote_read calls allowed in a single
            script execution. Reset on each ctx.reset().

    config INTERN_TABLE_SIZE
        int "Identifier intern table size"
        default 128
        range 32 512
        help
            Max unique identifier strings cached in the intern table.
            When full, new identifiers are NOT added. If the identifier
            already exists, return the existing pointer. If it is new
            and the table is full, return nullptr (NOT the raw source
            pointer — relying on raw source pointer would create a
            dangling reference if the source buffer is freed). The
            caller (Lexer) must handle nullptr by aborting with
            "intern table full" error. No dynamic allocation.
            Simple scripts: 10-30. Complex: 100+.
endmenu
```

#### 15.2.2 源码适配

解释器源码中使用 `CONFIG_` 宏替换硬编码常量：

```cpp
#define AST_POOL_SIZE   CONFIG_AST_POOL_SIZE
#define LIST_POOL_SIZE  CONFIG_LIST_POOL_SIZE
#define FUNC_POOL_SIZE  CONFIG_FUNC_POOL_SIZE
#define MAX_BINDINGS    CONFIG_MAX_BINDINGS
```

当池大小设为 0 时，通过条件编译禁用相关功能，节省 Flash 和 RAM：

```cpp
#if CONFIG_LIST_POOL_SIZE > 0
    // 注册 list_new, list_get, list_set, list_len
#endif
```

#### 15.2.3 预设场景参考

| 使用场景 | AST 节点 | 列表 | 函数 | 绑定 | 静态 RAM | 脚本规模 |
|----------|---------|------|------|------|---------|----------|
| 极简 | 128 | 2 | 4 | 16 | ~8KB | < 20 行，无函数 |
| 标准 | 256 | 8 | 16 | 32 | ~15KB | < 60 行，1-2 函数 |
| 复杂 | 1024 | 16 | 32 | 64 | ~55KB | < 200 行，5 函数+列表 |
| 极限 | 2048 | 32 | 64 | 128 | ~110KB | < 500 行，仅限 PSRAM 设备 |

> 注意：以上为静态池 RAM 估算，不含 FreeRTOS 任务栈、Wi-Fi 和 ESP-NOW 驱动内存。推荐总静态池不超过 100KB。

### 15.3 第二层：执行前资源验证 (编译后、执行前)

在 `execute()` 之前调用 `validate_resources()`，遍历 AST 并基于当前 Kconfig 配置值进行检查：

```cpp
struct ResourceReport {
    int ast_nodes_used;
    int ast_pool_size;
    int global_bindings_used;
    int max_bindings;
    int funcs_defined;
    int func_pool_size;
    int list_elements_total;
    bool list_in_loop;
    int max_ast_depth;           // AST 树最大深度
    int max_parse_depth;         // Kconfig 配置值
    bool passed;
    const char* error_msg;
};

ResourceReport validate_resources(ASTNode* program);
```

| 检查项 | 方法 | 阈值 | 错误消息示例 |
|--------|------|------|-------------|
| AST 节点数 | `pool_used` | > 90% `AST_POOL_SIZE` | `脚本过大：使用 238/256 AST 节点，请简化逻辑` |
| 全局绑定 | 遍历顶层声明+内置函数计数 | > 80% `MAX_BINDINGS` | `全局变量过多：已定义 18 个，最多 24 个可用` |
| 函数对象 | 计数 `FUNC_DEF` 节点 | > 90% `FUNC_POOL_SIZE` | `函数定义过多：15/16` |
| 列表总元素 | 扫描 `list_new` size 总和 | > 50 或循环内调用 | `警告：列表总元素>50 或 循环内创建` |
| AST 树深度 | 递归遍历统计最大深度 | > 80% `MAX_PARSE_DEPTH` | `嵌套过深：最大深度 52/64，请减少 if/while 嵌套` |

**收益**：脚本执行前就能修正问题，避免运行到一半才耗尽资源。耗时 < 1ms，完全可接受。

> **时机限制**: `validate_resources()` 在 parse 完成后、execute 前调用。此时 AST 池已经被 consume，`pool_used` 反映了实际解析消耗。因此该检查实际上只能捕获"刚好在 90-100% 灰色区间"的脚本（池未满但接近上限）。如果脚本真的超大（>100%），parse 阶段 `alloc_node()` 就会返回 nullptr 导致解析中断，`validate_resources()` 不会被调用。**这不是漏洞而是设计选择**：`validate_resources()` 的阈值（90%）提供了比硬限制更早的警告，帮助用户在触及硬限制前修正脚本。

> **已知局限**: 静态分析无法预检动态参数（如 `list_new(x)` 中 x 为变量时的实际 size）。此类情况依赖运行时降级策略（返回安全空列表）作为最终防线。`validate_resources` 报告中对此类动态调用标注为 `[runtime]`，提醒用户该检查项依赖运行时而非静态分析。

### 15.4 第三层：运行时优雅降级 (池耗尽不崩溃)

当资源池在运行时动态耗尽时，不返回空指针或崩溃，而是返回特殊"安全值"。

| 池类型 | 耗尽时机 | 降级策略 |
|--------|----------|----------|
| AST 节点池 | 解析时 | 脚本中止，错误信息包含当前配置值 |
| 列表池 | 运行时 `list_new` | 返回全局空列表对象（长度 0，只读） |
| 函数池 | 解析时 | 脚本中止 |
| 绑定池 | 运行时变量定义 | 脚本中止（核心状态，无法降级） |

> **已知限制**: 当前 `remote_read` 的实现采用单个信号量串行阻塞等待。若脚本连续读取 N 个远程传感器，总耗时 ≈ N × (单次等待 + 空中时间)。聚合函数 `remote_read_avg/max/min` 内部同样为串行调用，不改变此限制。对于需要低延迟批量读取的场景，建议在脚本层面使用更少的 `read_sensor` 调用，或依赖后续版本引入的并发请求机制。V1.0 通过 `CONFIG_COMM_CONCURRENT_CHECK` 保证任何时刻最多只有一个未完成的请求，彻底避免响应错位，代价是串行化。

**列表安全空对象**：

```cpp
static List g_empty_list = { .len = 0, .data = {0} };

// 分配: 遍历 ctx->list_pool_used[] 找空闲槽，标记后返回指针
List* list_alloc(ExecutionContext* ctx) {
    for (int i = 0; i < LIST_POOL_SIZE; i++) {
        if (ctx->list_pool_used[i] == 0) {
            ctx->list_pool_used[i] = 1;
            return &list_pool[i];
        }
    }
    ESP_LOGE("list", "Pool exhausted (%d), returning empty list", LIST_POOL_SIZE);
    ctx->constraint_violated = true;
    ctx->violation_msg = "列表池耗尽，返回空列表";
    return &g_empty_list;
}

// 释放: 仅清对应 ctx->list_pool_used[] 标志，不触碰 list_pool 数组
void list_free(ExecutionContext* ctx, List* lst) {
    for (int i = 0; i < LIST_POOL_SIZE; i++) {
        if (&list_pool[i] == lst && ctx->list_pool_used[i]) {
            ctx->list_pool_used[i] = 0;
            return;
        }
    }
}
```

**函数对象池分配/释放**:

```cpp
// 分配: 遍历 ctx->func_pool_used[] 找空闲槽，标记后返回指针
FuncObj* func_alloc(ExecutionContext* ctx) {
    for (int i = 0; i < FUNC_POOL_SIZE; i++) {
        if (ctx->func_pool_used[i] == 0) {
            ctx->func_pool_used[i] = 1;
            return &func_pool[i];
        }
    }
    return nullptr;  // 函数池耗尽, 由解析器中止脚本
}

// 释放: 清对应 ctx->func_pool_used[] 标志，将 FuncObj 归零
void func_free(ExecutionContext* ctx, FuncObj* func) {
    for (int i = 0; i < FUNC_POOL_SIZE; i++) {
        if (&func_pool[i] == func && ctx->func_pool_used[i]) {
            ctx->func_pool_used[i] = 0;
            memset(func, 0, sizeof(FuncObj));  // 清空包括 body 指针
            return;
        }
    }
}
```

> **统一池管理**: 取消全局 `list_used[]`/`func_used[]` 数组，所有槽位占用状态完全由 `ExecutionContext` 的 `list_pool_used[]` / `func_pool_used[]` 追踪。每次脚本结束（正常/超时/错误）统一调用 `ctx.reset()`，自动清零所有追踪器，无需 `list_free`/`func_free` 显式调用。`list_free` 仅供高级用户在脚本内手动管理。

**绑定池耗尽**：通常是编程错误（如 32 个局部变量），直接中止并给出清晰错误信息最安全。

### 15.5 第四层：聚合函数减少资源消耗

提供聚合内置函数，用 C++ 本地栈数组完成批量操作，完全不消耗脚本层资源池。

| 函数 | 说明 | 内部实现 |
|------|------|----------|
| `remote_read_avg(id_list)` | 读取多个远程传感器并返回平均值 | C++ 栈数组收集，计算均值 |
| `remote_read_max(id_list)` | 读取多个远程传感器并返回最大值 | 同上，取最大值 |
| `remote_read_min(id_list)` | 读取多个远程传感器并返回最小值 | 同上，取最小值 |

> **超时保护**: `remote_read_avg/max/min` 内部每完成一次 `espnow_comm_request_read()` 后，必须检查 `ctx.constraint_violated` 和全局 `s_script_timeout`（注意：`ctx.script_timeout` 已移除，见 §6.7，内部约束违反统一走 `constraint_violated`）。若超时则立即返回当前部分结果，放弃剩余子请求。实现时通过传入 `ExecutionContext*` 实现。

#### 手动资源管理

`list_free(lst)` 提供列表槽位的显式回收能力，但不属于聚合函数。它是高级用户的手动资源管理工具，AI 生成的脚本默认不使用。内部实现：标记 `ctx->list_pool_used[i] = 0`。

`id_list` 参数格式：逗号分隔的字符串，如 `"1,2,3"` 或 `"kitchen_temp,living_temp"`。

**示例**：

```js
// 聚合函数写法（高效，无列表开销）
var avg = remote_read_avg("1,2,3");

// 等价于手动列表写法（消耗 3 个 AST 节点 + 1 个列表槽位）
var temps = list_new(3);
list_set(temps, 0, remote_read(1));
list_set(temps, 1, remote_read(2));
list_set(temps, 2, remote_read(3));
var avg = (list_get(temps,0) + list_get(temps,1) + list_get(temps,2)) / 3;
list_free(temps);
```

**收益**：聚合函数减少脚本 AST 节点消耗 40-60%，列表池消耗为 0，显著降低池压力。`list_free` 作为手动资源管理工具，不在 AI 可见的内置函数列表中，仅限高级用户手动使用。

### 15.6 第五层：AI System Prompt 资源约束

根据固件的 Kconfig 配置，自动或手动在 AI 的 system prompt 中注入资源限制。V1.0 手动编写对应关系，V1.1 可由构建系统生成。

**标准配置（AST=256, List=8, Func=16, Binding=32）的约束文本**：

```
资源限制（严格遵守）：
- 脚本总行数 ≤ 30 行（不含空行和注释）
- 最多定义 2 个函数
- 列表操作 ≤ 2 个，禁止在循环内创建列表
- 变量总数（含函数形参）≤ 10 个
- 批量读传感器优先用 remote_read_avg/max/min，不要手动循环收集
```

**复杂配置（AST=1024）的约束文本**：

```
资源限制（遵守）：
- 脚本总行数 ≤ 100 行
- 最多定义 5 个函数
- 列表操作 ≤ 5 个
- 变量总数 ≤ 30 个
- 推荐使用聚合函数简化代码
```

这些数字约为实际硬件上限的 50-60%，确保即使 AI 生成略有超出，也不会触及硬限制。

### 15.7 资源开销评估

| 新增机制 | Flash 增量 | RAM 增量 | 条件 |
|----------|-----------|---------|------|
| Kconfig + 条件编译 | ~0.3 KB | 0 | 始终包含 |
| 资源验证函数 | ~0.5 KB | 0（栈临时） | 始终包含 |
| 列表安全空对象 | ~0.1 KB | 17 字节 | `LIST_POOL_SIZE > 0` |
| 列表释放机制 | ~0.1 KB | 0 | `LIST_POOL_SIZE > 0` |
| 聚合内置函数 x3 | ~0.5 KB | 局部栈数组（临时） | 始终包含 |
| **总计** | **~1.5 KB** | **~17 字节** | |

所有修改严守无动态分配、固定内存的原则。无需 C++ 异常。

### 15.8 验收标准

| 测试场景 | 预期行为 |
|----------|----------|
| Kconfig `AST_POOL_SIZE=64`，执行需 100 节点的脚本 | 解析后资源验证阶段报错："脚本过大：使用 102 节点，当前配置仅 64" |
| Kconfig `LIST_POOL_SIZE=0`，脚本调用 `list_new` | 运行时错误："列表功能未启用" |
| `LIST_POOL_SIZE=2`，脚本在循环中创建第 3 个列表 | 返回安全空列表，日志警告，脚本继续执行 |
| AI 生成的 30 行脚本，标准配置 | 正常解析执行，无资源耗尽 |
| 用户手动调用 `list_free` 后复用槽位 | 池槽位正确回收，后续 `list_new` 成功 |

## 16. Web Console 组件

### 16.1 设计目标

允许用户手机连接主控热点，通过网页界面完成 Wi-Fi 配置和自然语言 AI 控制，无需串口线或专用 App。

### 16.2 组件结构

```
components/web_console/
├── CMakeLists.txt
├── include/web_console/
│   └── web_console.h            # web_console_init(), 公共 API
└── src/
    ├── web_console.cpp          # SoftAP 启动 + HTTP 服务器 + 路由分发 + HTML 页面
    ├── wifi_scan.cpp            # Wi‑Fi 扫描封装 (esp_wifi_scan_start → JSON)
    ├── llm_client.cpp           # LLM API HTTP 客户端 + 按需连接/断开 WiFi
    └── script_inject.cpp        # 脚本入队 (script_io_enqueue) + print 输出环形缓冲区管理
```

### 16.3 HTTP 服务器 API 端点

| 端点 | 方法 | 功能 | 请求/响应 |
|------|------|------|-----------|
| `/` | GET | 返回静态 HTML 配置页面 | HTML (含内联 CSS/JS, 无外部依赖) |
| `/api/status` | GET | 当前系统状态 | `{"peers": N, "wifi_configured": bool, "llm_configured": bool, "script_running": bool}` |
| `/api/config` | GET | 获取当前完整配置 | `{"wifi_ssid":"...", "wifi_configured":bool, "llm_url":"...", "llm_model":"..."}` (key 字段用 `"***"` 掩码, `wifi_pass` 不返回) |
| `/api/config/wifi` | POST | 仅保存 WiFi 配置到 NVS | 表单字段: `wifi_ssid, wifi_pass` |
| `/api/config/llm` | POST | 仅保存 LLM 配置到 NVS | 表单字段: `llm_url, llm_key, llm_model` |
| `/api/scan` | GET | 扫描附近 Wi-Fi | `[{"ssid":"...","rssi":-50}, ...]` (按 RSSI 降序, 去重) |
| `/api/ai` | POST | 自然语言 → LLM → 脚本执行 | `{"prompt":"每 5 秒读温度"}` → `{"status":"ok","script":"...", "log":"..."}` |
| `/api/script` | POST | 直接注入脚本 | `{"script":"while(true){...}"}` → 中止当前脚本 + 执行新脚本 |
| `/api/exec_log` | GET | 获取 print 输出日志 | `{"lines":["temp=25.3","fan=on",...]}` (环形缓冲区最近 N 条) |

> **API Key 安全**: 仅 `/api/config/llm` POST 接受 `llm_key` 字段并写入 NVS，`/api/config/wifi` POST **不接受** `llm_key`，从根本上避免 WiFi 配置操作意外接触 API key。GET `/api/config` 响应中**不返回** key 值（仅返回 `"***"` 掩码），`wifi_pass` **不返回**（写入后不可读），防止通过浏览器查看已保存的密钥。

### 16.4 SoftAP 与配置模式入口

| 触发条件 | 行为 |
|----------|------|
| 首次启动且 NVS 无 `wifi_ssid` | 自动创建 `ESP-LEGO-Setup` 热点，无密码 |
| 长按 BOOT 键 (GPIO 0) 3 秒 | 从运行模式切换到配置模式，创建热点 |
| `CONFIG_WEB_CONSOLE_TIMEOUT_SEC` (默认 300s) 无操作 | 自动退出配置模式，切换回正常运行（无热点，全速 ESP-NOW） |

> **⚠️ 安全边界**: SoftAP 无密码是设计决策——简化首次配置体验。但此设计假设部署环境是**受控私有环境**（如个人工作室、家庭实验室）。在公共或物理不可控环境中，攻击者连接热点后可访问 `/api/script` 注入任意代码控制硬件。V1.0 用户需自行确保物理安全；V2 计划增加热点密码保护（通过首次配置时设置）。

### 16.5 WiFi 模式切换序列 (LLM 调用时)

ESP32-S3 单射频，SoftAP 和 Station 模式不能完全独立共存。LLM 客户端需要完整的模式切换：

```
初始状态: SoftAP 模式 (wifi_mode=WIFI_MODE_AP), ESP-NOW 运行中

步骤 1: 暂停 ESP-NOW 接收 (停止 rx 队列处理)
步骤 2: 用 `esp_wifi_scan_get_ap_records()` 或连接回调获取目标 AP 的信道号 `ap_channel`
步骤 3: `esp_wifi_set_channel(ap_channel, WIFI_SECOND_CHAN_NONE)`  // **关键**: 预设 ESP-NOW 信道与目标 AP 一致，切换 STA 后不因信道跳变而丢失 ESP-NOW 同步
步骤 4: esp_wifi_set_mode(WIFI_MODE_STA)    // 关闭 SoftAP，切换为 STA
步骤 5: esp_wifi_set_config(WIFI_IF_STA, &wifi_config)  // 设置目标路由器
步骤 6: esp_wifi_connect()                   // 连接路由器
步骤 7: 等待连接成功 (DHCP 获取 IP, 超时 15s)
步骤 8: 构造 HTTP POST → 发送到 LLM API     // 含 System Prompt + 设备列表 + 用户指令
步骤 9: 接收并解析 LLM 响应                  // 使用 cJSON 解析
步骤 10: esp_wifi_disconnect()                // 断开路由器
步骤 11: esp_wifi_set_mode(WIFI_MODE_AP)      // 恢复 SoftAP
步骤 12: esp_wifi_set_channel(CONFIG_SOFTAP_CHANNEL, WIFI_SECOND_CHAN_NONE)  // 恢复预设信道（默认 1，见 Kconfig）
步骤 13: 恢复 ESP-NOW 接收

恢复后状态: SoftAP 模式, ESP-NOW 运行中
```

> **后果**: 切换期间（步骤 2-9，约 2-10 秒）HTTP 服务器不可用，浏览器断连。LLM 响应返回后自动恢复，页面需实现自动重连逻辑（WebSocket 或周期轮询 `/` 直到返回 200）。

### 16.6 LLM 请求构建

System Prompt 由 `llm_client.c` 的 `build_system_prompt()` 自动构建，heap 分配 4KB 缓冲区（用后释放），包含以下内容：

```
System Prompt (自动构建):
  [BNF 语法 (design.md §6.2)]
  [内置函数列表 + 语义说明 (不含 list_free)]
  例:
  - digital_read(pin) -> number   0/1, reads GPIO level
  - remote_read(id[,pin]) -> num  read sensor from remote by id/name
  - send_motor(pin,speed)->void   DC motor PWM: 0(stop)-100(full)
  ...
  [资源约束 (按 Kconfig 标准配置)]
  [当前在线设备列表 (来自 list_peers()):
   - id=1, name=kitchen_temp, capabilities=温度
   - id=3, name=exhaust_fan, capabilities=电机]

User Prompt:  用户输入的自然语言指令

API Request (OpenAI-compatible):
  POST {llm_url}/chat/completions
  Authorization: Bearer {llm_key}
  {
    "model": "{llm_model}",
    "messages": [
      {"role": "system", "content": "{system_prompt}"},
      {"role": "user", "content": "{user_prompt}"}
    ],
    "temperature": 0
  }
```

**响应解析**: 使用 `cJSON` 解析 LLM 返回的 JSON，通过 event_handler 在 `esp_http_client_perform()` 执行过程中实时捕获 `HTTP_EVENT_ON_DATA` 数据块，组合为完整响应体。提取 `choices[0].message.content` 字段内容，再通过 `extract_script_from_response()` 去除 markdown 代码块标记（` ``` `、` ```javascript ` 等），提取纯脚本代码。

> **System Prompt 中的设备列表动态注入**: 每次调用 `/api/ai` 时，先调用 `list_peers()` 获取当前在线设备，拼入 System Prompt。确保 LLM 始终知道当前有哪些设备可用。

### 16.7 脚本切换机制

为支持用户在网页上随时输入新指令覆盖当前脚本，实现主动中止机制：

```cpp
// 全局原子标志（_Atomic bool，非 volatile bool — 保证双核 ESP32-S3 的 cache 一致性）
extern _Atomic bool s_script_abort_requested;
extern _Atomic bool s_script_timeout;

// web_console/script_inject.cpp 中:
// taskENTER_CRITICAL(&s_inject_mux);                // 防止 xQueueReset 与 exec_task 的 xQueueReceive 竞态
//   atomic_store(&s_script_abort_requested, true);
//   xQueueReset(script_queue);
//   xQueueSend(script_queue, &new_script, 0);       // 非阻塞入队
// taskEXIT_CRITICAL(&s_inject_mux);
// 
// main/app_main.cpp exec_task 主循环中:
// while (1) {
//   xTimerStop(s_watchdog_timer, portMAX_DELAY);
//   xTimerReset(s_watchdog_timer, portMAX_DELAY);
//   atomic_store(&s_script_timeout, false);          // 使用 atomic_store，非普通赋值
//   script = xQueueReceive(script_queue, portMAX_DELAY);
//   execute(script, ...);                            // 内部每语句检查原子标志
// 
//   // 脚本退出后：
//   on_script_end();
//   reset_pool();
//   env_restore_pristine();
//   ctx.reset();
//   atomic_store(&s_script_abort_requested, false);  // 使用 atomic_store
//   xTimerStop(s_watchdog_timer, portMAX_DELAY);
// }
```

> **竞态防护**: `taskENTER_CRITICAL` 确保 `xQueueReset` + `xQueueSend` 原子执行，防止 `exec_task` 在两者之间取走空队列而阻塞。`xQueueSend` 使用超时 0（不阻塞），队列复位后总有一空闲槽。
>
> **清零窗口期 — 单次注入**: 在 `atomic_store(&s_script_abort_requested, false)` 和 `xQueueReceive(script_queue, portMAX_DELAY)` 之间，web_console 的 HTTP handler（运行在其他任务）可能再次设置 `s_script_abort_requested = true`。此时新脚本刚从队列取出，执行该脚本的第一条语句前会检查到标志并立即中止——用户看到新指令未执行且无提示。

**处理方式**: `exec_task` 的取脚本循环改为：
```
loop:
  script = xQueueReceive(script_queue, portMAX_DELAY)  // 阻塞等待直到有脚本
  // 此时一定取到了脚本，再检查 abort 标志
  if (s_script_abort_requested):
    // 有未处理的 abort 请求（可能是取队列期间注入的，或前一个脚本退出后注入的）
    discard(script)      // 丢弃刚取出的脚本
    atomic_store(&s_script_abort_requested, false)
    continue loop        // 重新取队列（新注入的脚本已在队列中）
  // 正常执行 script
  execute(script, ...)
  // 脚本退出后不清零 s_script_abort_requested（由下一轮循环开始前处理）
```

**双击场景（连续两次快速注入）**:
```
用户快速发送 A→B→C:
  inject_B: abort=A, enqueue=B
  inject_C: abort=B, xQueueReset, enqueue=C   // B 被清出队列
  
exec_task 循环:
  取队列 → 取到 C (B 已被 xQueueReset 清掉)
  检查 abort → 为 true (C 注入时设置的) → 丢弃 C → 清零 → 重取
  取队列 → 阻塞等待 (队列已空，等待新脚本)
  
此时用户看到 C 被执行后立即中止，"没有执行任何脚本"。
```

**更健壮的解决方案**: 不使用 `xQueueReset` 冲刷，而是在 `script_inject.cpp` 中维护一个单调递增的脚本序列号 `s_script_seq`，每次注入时递增。`exec_task` 在取到脚本后检查 `s_script_seq` 在取队列期间是否变化（由另一个注入线程更新），若变化则丢弃当前脚本、清 abort 标志、重取。这完全避免了 `xQueueReset` 丢失脚本的问题。该方案为 V1.1 改进，V1.0 允许双击场景下的短暂空等。

> **与 watchdog 的 `s_script_timeout` 区分**: `s_script_abort_requested` 表示"用户主动请求切换"（正常终止），`s_script_timeout` 表示"执行超时"（异常终止）。两者在 `on_script_end()` 中的处理路径相同（都复位引脚），但错误日志级别不同。
>
> **远程引脚安全限制**: `on_script_end()` 仅复位主控本地输出引脚（通过 `exec_task` 的本地引脚列表）。通过 `espnow_send()` 控制的子模块远程引脚不受影响。脚本超时时，子模块的电机/继电器可能保持激活状态。V1.0 不实现远程安全复位；V1.1 可考虑在脚本超时后广播 RESET 命令到所有在线子模块。
> 
> **队列冲刷必要性**: 如果 `exec_task` 正执行脚本 A，队列中可能已有另一个脚本 B（来自用户的串口输入）。不冲刷的话，`abort` + `enqueue(new_script)` 后 `exec_task` 取到的是 B 而非新脚本。`xQueueReset` 保证新脚本紧跟在当前脚本后第一个执行。

### 16.8 Print 输出环形缓冲区

```
// 全局声明 (在 web_console.h 或 main 中)
#define EXEC_LOG_BUF_SIZE   CONFIG_EXEC_LOG_BUF_SIZE  // 默认 4096 字节

extern char g_print_buffer[EXEC_LOG_BUF_SIZE];
extern int  g_buf_write_pos;          // 环形写入位置（仅由 print 函数推进）
extern int  g_buf_read_pos;           // 环形读取位置（由 /api/exec_log 读取后推进）
extern SemaphoreHandle_t g_print_mutex;

// builtins.cpp 中 print 函数:
void builtin_print(Value val) {
    char buf[128];
    int len = snprintf(buf, sizeof(buf), ...);
    printf("%s", buf);                 // 串口输出 (原有)
    
    // Web Console 捕获 (新增):
    if (g_print_buffer) {
        xSemaphoreTake(g_print_mutex, portMAX_DELAY);
        for (int i = 0; i < len && i < EXEC_LOG_BUF_SIZE - 1; i++) {
            g_print_buffer[g_buf_write_pos] = buf[i];
            g_buf_write_pos = (g_buf_write_pos + 1) % EXEC_LOG_BUF_SIZE;
            if (g_buf_write_pos == g_buf_read_pos) {
                // 写指针追上读指针 → 推进读指针（丢弃最旧内容）
                g_buf_read_pos = (g_buf_read_pos + 1) % EXEC_LOG_BUF_SIZE;
            }
        }
        // 不写 \0 — HTTP handler 通过 read_pos/write_pos 差值确定可读范围
        xSemaphoreGive(g_print_mutex);
    }
}

// 网页通过 GET /api/exec_log 获取:
// 锁定互斥锁 → 从 g_buf_read_pos 读到 g_buf_write_pos → 计算长度 → 复制到响应缓冲区 → 推进 g_buf_read_pos = g_buf_write_pos
// 响应格式: {"lines": ["line1", "line2", ...]}，按换行符分割
```

> **环形缓冲区说明**: 使用双指针（读/写）而非单指针 + `\0` 终止，避免 `\0` 写入对已有内容的破坏。读指针由 `/api/exec_log` 的 HTTP handler 推进。写入超过读指针时自动覆盖最旧内容（写指针追尾时推进读指针）。缓冲区不会通过 `\0` 判断边界，HTTP handler 通过指针差值确定可读段。

### 16.9 Kconfig 配置项

```
menu "ESP-LEGO Web Console"
    config WEB_CONSOLE_ENABLED
        bool "Enable web console (SoftAP + HTTP server + LLM client)"
        default y
        help
            When enabled, the master firmware creates a SoftAP hotspot
            on first boot (or BOOT button long-press) and serves a web
            configuration page at 192.168.4.1.

    config WEB_CONSOLE_TIMEOUT_SEC
        int "Auto-exit config mode after (seconds)"
        default 300
        range 0 3600
        help
            Set to 0 to disable auto-exit (stay in config mode until
            BOOT button long-press again).

    config WIFI_SCAN_SHOW_5GHZ
        bool "Show 5GHz networks in scan results"
        default n
        help
            ESP32-S3 is 2.4GHz only, but some APs broadcast both bands.
            Showing 5GHz SSIDs is informational only — they cannot be
            connected. Not recommended for typical use.

    config BUTTON_PIN
        int "GPIO pin for entering config mode"
        default 0
        range 0 39
        help
            Long-press (3s) on this pin triggers ESP-LEGO-Setup SoftAP.
            Default GPIO 0 is the BOOT button on most dev boards.

    config EXEC_LOG_BUF_SIZE
        int "Print output ring buffer size (bytes)"
        default 4096
        range 1024 16384
        help
            Circular buffer for capturing print() output from running
            scripts. Visible via web UI /api/exec_log endpoint.
            4096 bytes ≈ 40 lines of typical output.

    config HTTP_SERVER_STACK_SIZE
        int "HTTP server task stack size"
        default 6144
        range 4096 16384
        help
            Stack size for the esp_http_server task that handles
            web console requests. Larger if serving complex pages.
endmenu
```

### 16.10 NVS 新增键

| 键名 | 类型 | 说明 | 写入路径 |
|------|------|------|----------|
| `wifi_ssid` | 字符串 | 目标路由器 SSID | `/api/config/wifi` POST |
| `wifi_pass` | 字符串 | 目标路由器密码 | `/api/config/wifi` POST |
| `llm_url` | 字符串 | LLM API Base URL (如 `https://api.openai.com/v1`) | `/api/config/llm` POST |
| `llm_key` | 字符串 | LLM API Key | `/api/config/llm` POST |
| `llm_model` | 字符串 | LLM 模型名称 (如 `gpt-4o-mini`) | `/api/config/llm` POST |

### 16.11 边界与异常处理

| 场景 | 处理 |
|------|------|
| SoftAP 创建失败 | 日志错误，回退到纯 UART 模式 |
| WiFi 扫描中 ESP-NOW 中断 | 扫描前 `vTaskSuspend(rx_task)` 暂停 rx 队列处理 + `vTaskSuspend(timeout_task)` 暂停老化；扫描完成后 `xQueueReset(rx_queue)` 丢弃扫描期间堆积的过时宣告包，再 `vTaskResume` 恢复。**不调用 `esp_now_deinit()`**，避免 peer 表丢失和底层层资源重新分配 |
| LLM 调用时路由器连接超时 (15s) | 返回 JSON 错误 `{"error":"wifi_connect_timeout"}`，恢复 SoftAP |
| LLM API 返回非 JSON/HTTP 错误 | 返回错误信息到网页，用户可重试 |
| LLM 响应无有效脚本 | 返回错误 `{"error":"no_script_in_response"}` |
| NVS 写入失败（空间不足） | 返回 HTTP 500，旧配置保持不变 |
| 脚本注入时队列满 | 丢弃最旧脚本，注入新脚本 |
| 环形缓冲区溢出 | 自动覆盖最旧内容（循环写入），日志警告 |
| HTTP 服务器并发请求 | `esp_http_server` 自身处理并发，每个 URI handler 需是线程安全的 |
| 网页客户端在 LLM 调用期间断连 | 页面 JS 实现自动重连（每 2s 轮询 `/api/status`） |

### 16.12 资源开销评估

| 新增模块 | Flash 增量 | RAM 增量 | 条件 |
|----------|-----------|---------|------|
| web_console (HTTP 服务器 + 路由) | ~6 KB | 栈 6KB + 少量堆 | `WEB_CONSOLE_ENABLED` |
| HTTP 页面 (HTML+CSS+JS, 内联) | ~4 KB | 0 (从 Flash 服务) | `WEB_CONSOLE_ENABLED` |
| wifi_scan | ~1 KB | ~0.5 KB (扫描结果临时) | `WEB_CONSOLE_ENABLED` |
| llm_client (HTTP + cJSON) | ~10 KB | 栈 4KB + 请求/响应缓冲区 2KB | `WEB_CONSOLE_ENABLED` |
| script_inject + print 缓冲区 | ~0.5 KB | `EXEC_LOG_BUF_SIZE` (默认 4KB) + 互斥锁 | `WEB_CONSOLE_ENABLED` |
| **总计** | **~21.5 KB** | **~16.5 KB (含 4KB 日志缓冲)** | |

### 16.13 对现有设计的影响

| 组件 | 改动 | 兼容性 |
|------|------|--------|
| `interpreter/builtins.cpp` | `print` 增加环形缓冲区写入 | 向后兼容（缓冲区未初始化时跳过） |
| `script_io` | 新增 `script_io_enqueue()` API | 新增 API，不影响现有 UART 路径 |
| `main/app_main.cpp` | 增加 `web_console_init()` + 按键检测 | `shell_task`/`exec_task` 保持不变 |
| `main/Kconfig.projbuild` | 新增 Web Console 配置菜单 | 不影响现有 Kconfig 选项 |
| NVS 分区 | 新增 5 个 NVS 键 | 默认分区大小应足够 |

> **WiFi 与 ESP-NOW 共存限制**: Web Console 涉及的 WiFi STA 操作（LLM 调用、WiFi 扫描）会暂时中断 ESP-NOW。这是 ESP32-S3 单射频的硬件限制。设计通过"用完即断"策略将中断窗口控制在 2-10 秒内，并通过模式切换序列确保恢复完整。

## 17. 已知限制与 Roadmap

### V1.0 已知限制

| 限制 | 影响 | 缓解措施 |
|------|------|----------|
| **同步阻塞模型** | 连续 remote_read 累加延迟 (N × 600ms) | 强制使用聚合函数 `remote_read_avg/max/min` |
| **WiFi 模式切换中断 ESP-NOW** | LLM 调用/WiFi 扫描期间 ESP-NOW 中断 2-10s | 用完即断策略 + 切换前后 deinit/init 恢复 |
| **LLM 调用期间 SoftAP 不可用** | 浏览器断连，用户无法操作 | Web UI 自动重连机制（周期轮询） |
| **LLM API Key 明文存储** | NVS 中 API Key 以明文存储 | 本地 SoftAP 连接，风险可控；V2 可加 Flash 加密 |
| **无 AST 优化器** | AI 生成的冗余/深嵌套脚本直接 hitting 硬限制 | `validate_resources()` 提前拒绝 + 运行时约束强执行 |
| **无 Future/Promise** | 无法并行请求多设备 | 当前不解决，V1.1 引入请求队列 |
| **List 仅支持 double** | 无法存储字符串/混合类型 | AI prompt 强约束 + 运行时类型检查 |
| **无远程调试/Trace** | AI 脚本执行过程不可观测 | 通过 `print()` + ESP_LOG + Web Console exec_log 进行基础调试 |
| **无脚本版本控制** | 新 prompt 生成的脚本可能在旧固件上失败 | 人工确认后再执行 |
| **dedup_seq 环形位图哈希冲突** | 环形位图使用 `seq_id % 8` 索引，每 8 个 seq_id 即可能发生哈希槽复用，导致真实包被误判为重复而丢弃 | 真实冲突率取决于发包密度和时序；V1.0 接受此风险；V1.1 可替换为 8 条目显式滑动队列（O(8) 线性查找，无哈希冲突） |
| **远程执行器安全复位缺失** | 脚本超时时子模块的电机/继电器可能处于激活状态 | `on_script_end()` 仅复位本地引脚；V1.1 可广播 RESET 命令 |

### V2.0 Roadmap

| 特性 | 说明 | 优先级 |
|------|------|--------|
| 请求排队 (V1.1) | `req_queue[4]` 环形缓冲区，自动发送下一个请求 | P1 |
| 批量请求批处理 | `read_multi("1,2,3")` 一次性发射 3 个请求 | P1 |
| WebSocket 实时日志 | 替代 exec_log 轮询，print 输出实时推送 | P1 |
| AST 优化器 | 常量折叠、死代码消除、嵌套 flatten | P2 |
| 可观测性 | 脚本执行 trace、变量监控、远程日志 | P2 |
| Value List | 支持 `Value[]` 而非仅 `double[]` | P2 |
| NVS Key 加密 | Flash 中加密存储 API Key | P2 |
| ESP-NOW LMK/PMK 加密 | AES-CMAC 加密通信 | P3 |
| OTA 升级 | 主控/子模块空中升级 | P3 |
