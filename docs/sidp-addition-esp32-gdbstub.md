# SIDP 未来扩展：ESP32 Panic GDB Stub

本文描述一个计划中的 SIDP 扩展：让 Soul Injector 通过附加 UART 连接已经崩溃并进入 ESP-IDF panic GDB Stub 的 ESP32 目标，再通过 Soul Agent 向开发者本机的 GDB/IDE 提供远程故障现场检查。

本扩展不属于 SIDP v1。SIDP v1 的验收范围仍为 Cortex-M0、Cortex-M3 和 Cortex-M4。

## 1. 能力边界

ESP-IDF 启用 `CONFIG_ESP_SYSTEM_PANIC_GDBSTUB` 后，目标发生严重错误时不立即复位，而是在 UART 上运行 GDB RSP server。panic 模式主要用于事后检查：可以读取 CPU 寄存器、变量和内存，但不能将其视为完整硬件调试会话。

本扩展第一阶段只要求：

- 检测目标进入 panic GDB Stub。
- 读取崩溃核的寄存器。
- 读取内存和栈。
- 向 Soul Agent 主动发送 STOPPED 快照。
- 让 Soul Agent 继续承担 ELF、DWARF、GDB target description、调用栈和 RTOS task 解析。

第一阶段明确不提供：

- 断点和watchpoint。
- single-step。
- 修改PC或通用寄存器。
- 修改目标内存。
- 从panic现场continue/resume。
- 等价于JTAG的多核run control。

不支持的SIDP操作必须返回现有的 `SIDP_STATUS_UNSUPPORTED`，不能假装成功。

## 2. 推荐的数据路径

```text
GDB / IDE
    |
    | GDB RSP over localhost
    v
Soul Agent
    |- GDB RSP server
    |- ESP target/register mapping
    |- cache、ELF/DWARF、RTOS awareness
    |
    | semantic SIDP
    v
Soul Injector
    |- SIDP endpoint
    |- small GDB RSP client
    |- register/stack prefetch
    |- target UART driver
    |
    | GDB RSP over UART
    v
crashed ESP32 panic GDB Stub
```

Soul Agent 对开发者的GDB表现为server；目标上的panic GDB Stub同样是server。因此Soul Injector必须在目标UART一侧扮演一个小型GDB RSP client，并把 `g`、`p`、`m` 等目标RSP操作转换为SIDP的寄存器、内存和停止事件。

不推荐把目标UART上的RSP字节直接穿透到Soul Agent。透明tunnel可用于早期bring-up，但跨境连接中每个串行RSP request/response都可能产生一次网络RTT，也无法充分利用现有STOPPED快照和内存缓存。

## 3. 可以直接复用的SIDP结构

当前SIDP以下部分不需要推翻：

- `sidp_msg_header_t`、CRC-32和request ID。
- `sidp_read_registers_request_t` / `sidp_read_registers_response_t`。
- `sidp_read_memory_request_t` / `sidp_read_memory_response_t`。
- `sidp_read_memory_vector_request_t`。
- 可变宽度的 `sidp_register_value_t`。
- `sidp_stopped_event_t`、`stop_id`、`core_id`和栈快照。
- Soul Agent的停止态cache和内存block cache。

SIDP的寄存器值没有固定成Cortex-M结构，因此Xtensa和RISC-V寄存器仍可用 `register_id + value_size + value[]` 表示。

## 4. 架构和profile扩展

原始ESP32和部分ESP32系列使用Xtensa核，不能用现有Arm-M或RISC-V架构标识。建议增加：

```c
enum sidp_architecture_t : uint8_t {
    SIDP_ARCH_ARM_M  = 1,
    SIDP_ARCH_RISCV  = 2,
    SIDP_ARCH_XTENSA = 3,
};

enum sidp_target_profile_t : uint8_t {
    /* existing profiles keep their assigned values */

    SIDP_PROFILE_XTENSA_ESP32   = 40,
    SIDP_PROFILE_XTENSA_ESP32S2 = 41,
    SIDP_PROFILE_XTENSA_ESP32S3 = 42,
};
```

Xtensa是可配置架构，不应假定所有Xtensa目标共享同一套GDB寄存器排列。每个已支持profile必须同时定义：

- 目标GDB Stub中 `g` packet的寄存器顺序和大小。
- SIDP register ID。
- Soul Agent中的GDB register number映射。
- 对应的GDB target description。

每张寄存器表还必须记录其来源ESP-IDF版本、目标芯片、相关GDB Stub配置和用于验证的packet fixture/hash。不能仅写“ESP32-S3 profile”后跨ESP-IDF版本沿用，因为Xtensa和RISC-V panic GDB Stub的 `g` packet排列都可能随ESP-IDF实现变化。升级支持版本时先回放验证寄存器表，再更新profile实现。

RISC-V ESP32目标可以继续使用 `SIDP_ARCH_RISCV` 和 `SIDP_PROFILE_RV32`。`sidp_attach_response_t::target_id` 必须进一步标识实际ESP32型号，Soul Injector和Soul Agent都不能只凭 `RV32` 推断目标GDB Stub的具体寄存器布局。

这些新增profile值为本扩展的预留建议；实现前仍应和SIDP主协议中的最终编号表一起确认，避免编号冲突。

## 5. 新增capability

建议在 `sidp_capability_t` 中预留：

```c
enum sidp_capability_t : uint32_t {
    /* existing capabilities keep their assigned bits */

    SIDP_CAP_POST_MORTEM     = 1u << 13,
    SIDP_CAP_TARGET_GDB_STUB = 1u << 14,
};
```

- `SIDP_CAP_TARGET_GDB_STUB` 表示Soul Injector当前通过目标自身的GDB Stub提供SIDP语义操作，而不是通过SWD/JTAG直接控制CPU。
- `SIDP_CAP_POST_MORTEM` 表示这是只读的崩溃现场会话。

设置 `SIDP_CAP_POST_MORTEM` 时具有强制语义：

```text
READ_REGISTERS      allowed
READ_MEMORY         allowed
WRITE_REGISTERS     not supported
WRITE_MEMORY        not supported
RUN / CONTINUE      not supported
HALT                not supported; target is already stopped
breakpoint          not supported
watchpoint          not supported
single-step         not supported
```

Attach response中的 `hardware_breakpoints` 和 `hardware_watchpoints` 必须为0，并且不得设置软件/硬件断点、watchpoint、single-step或resume相关capability。

## 6. 新的attach操作

现有 `sidp_attach_request_t` 表达connect-under-reset等SWD会话行为，不适合复用为UART GDB Stub配置。SWD/JTAG时钟本来就由Soul Injector本地YAML配置决定，不属于任何SIDP attach request。保持主结构简单，新增独立opcode：

```c
enum sidp_opcode_t : uint16_t {
    /* existing opcodes keep their assigned values */

    SIDP_OP_ATTACH_GDB_STUB = 0x0040,
};

typedef struct __attribute__((packed)) {
    sidp_log_channel_t uart_channel;
    uint16_t reserved;
    uint32_t uart_baud;
    uint16_t initial_stack_before;
    uint16_t initial_stack_after;
} sidp_attach_gdb_stub_request_t;

static_assert(sizeof(sidp_attach_gdb_stub_request_t) == 12,
              "SIDP wire layout changed");
```

`sidp_attach_gdb_stub_request_t::uart_channel` 直接使用底层类型为 `uint16_t` 的 `sidp_log_channel_t`。第一版只接受 `SIDP_LOG_CHANNEL_TARGET_UART0`。

该操作的response复用 `sidp_attach_response_t`，避免复制memory map和target capability结构。成功时至少满足：

```text
architecture          = detected/configured target architecture
profile               = exact supported target profile
address_width         = 32
capabilities          includes POST_MORTEM and TARGET_GDB_STUB
cpu_id                = 0
supported_vector_catch_mask = 0
hardware_breakpoints  = 0
hardware_watchpoints  = 0
```

Attach绝不能隐式复位目标，否则会销毁故障现场。如果当前UART上没有已进入panic状态的合法GDB Stub，返回超时或目标状态错误。复位必须是用户明确触发的独立操作。

## 7. UART日志与GDB Stub状态机

目标通常先在UART输出普通日志和panic文本，随后在同一UART进入RSP会话。Soul Injector应维护：

```text
UART_LOG_MODE
    |
    | detect panic GDB Stub marker or a valid RSP stop packet
    v
GDB_STUB_MODE
    |
    | target reset, disconnect or unrecoverable RSP failure
    v
UART_LOG_MODE
```

在 `UART_LOG_MODE`：

- UART字节使用现有 `SIDP_KIND_LOG_STREAM` 和 `sidp_log_data_t` 上报。
- Soul Injector应保留一个有界的最近日志ring buffer，避免Soul Agent连接稍晚时完全丢失panic前日志。

进入 `GDB_STUB_MODE` 后：

- 停止把RSP字节作为日志上报。
- UART由Soul Injector的目标RSP client独占。
- 日志流允许丢包的规则不再适用于RSP。
- 退出stub模式后才能恢复该UART的普通日志流。

`sidp_log_data_t` 不可直接承载目标GDB RSP，因为日志流是单向、低优先级且允许丢包，而RSP是双向、有状态且不能静默丢包。

## 8. Soul Injector中的最小RSP client

第一阶段只实现目标panic Stub所需子集：

- RSP packet framing、checksum、ACK/NAK和超时重试。
- 初始stop reply解析。
- capability探测，例如目标支持时使用 `qSupported`。
- 读取全部寄存器的 `g`。
- 按编号读取寄存器的 `p`，目标支持时使用。
- 读取内存的 `m`。
- 正确处理错误response和连接丢失。

不应在Soul Injector中实现：

- ELF和DWARF解析。
- 符号查找。
- 源码行映射。
- FreeRTOS task list解析。
- 通用GDB命令行或target description生成。

目标UART RSP自身的checksum与外层SIDP CRC-32各自保留：前者保护Soul Injector到目标的UART链路，后者保护Soul Injector到Soul Agent的消息。

## 9. STOPPED预读取

检测到panic stop reply后，Soul Injector应在本地完成：

1. 读取崩溃核的基础寄存器。
2. 根据profile找到SP。
3. 将请求范围裁剪到有效RAM region。
4. 读取 `[SP - initial_stack_before, SP + initial_stack_after)`。
5. 生成新的 `stop_id`。
6. 一次发送包含寄存器和栈的 `sidp_stopped_event_t`。

这些UART交互发生在Soul Injector本地，不应逐条穿过Soul Interconnect。Soul Agent收到STOPPED后即可从停止态cache回答GDB最初的寄存器、backtrace和栈读取请求。

更大的内存读取继续使用普通 `READ_MEMORY` 或 `READ_MEMORY_VECTOR`。Soul Agent根据ELF和调试请求决定扩展哪些范围；Soul Injector不需要理解变量或调用栈。

## 10. core和RTOS语义

部分ESP32型号具有多个CPU核，但panic GDB Stub不能自动视为完整多核JTAG调试器。第一阶段只暴露目标Stub实际提供的崩溃核：

```text
sidp_stopped_event_t::core_id = 0
```

这里的0表示“SIDP会话中的第一个可见core”，并不声称芯片物理上只有一个core。只有确认目标Stub能够稳定提供其他core现场后，才扩展attach中的可见core描述和多核寄存器读取。

RTOS thread awareness继续由Soul Agent负责。Soul Agent使用ELF symbols、目标内存和对应ESP-IDF/FreeRTOS provider解析task list及saved context；Soul Injector不解析RTOS线程。

## 11. 透明RSP tunnel

可选的bring-up模式可以增加通用双向byte stream，把本机GDB直接连接到目标panic Stub。这有利于先验证UART接线和Stub兼容性，但不是正式远程方案。

它不能复用 `SIDP_KIND_LOG_STREAM`，而需要可靠、双向、不可丢包的独立stream类型。该模式还会绕过：

- SIDP STOPPED快照。
- Soul Agent寄存器cache。
- 内存block cache和vector prefetch。
- 对跨境RTT的请求合并。

因此实施顺序应当是：透明tunnel只用于早期诊断，正式功能使用Soul Injector目标侧RSP client和语义SIDP。

## 12. 验证计划

至少覆盖：

1. 正常UART日志持续进入 `SIDP_KIND_LOG_STREAM`。
2. panic文本和初始RSP stop packet跨分片到达时仍能正确识别。
3. 切换到GDB Stub模式后不再把RSP字节泄漏到日志通道。
4. Xtensa目标寄存器顺序、宽度和SIDP register ID映射。
5. RISC-V ESP32目标寄存器映射。
6. SP附近栈快照和越界裁剪。
7. 写寄存器、写内存、continue和断点返回not supported。
8. UART checksum错误、超时、NAK、断线和目标复位。
9. Soul Agent晚于目标crash连接时，仍能获得保留的panic日志和故障现场。
10. 约200ms RTT环境下首次停止展示不产生逐寄存器网络往返。

## 13. 与完整ESP32硬件调试的关系

本扩展只是在UART上使用目标固件内置的panic GDB Stub，不等同于通过JTAG/USB-JTAG调试ESP32。未来若Soul Injector增加完整ESP32硬件调试，应作为另一个target backend实现halt、resume、breakpoint、watchpoint和多核控制；它可以继续复用SIDP语义操作，但不能把本扩展的post-mortem限制当作硬件调试器限制。
