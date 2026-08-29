# SIDP v1 协议

## 1. 目标与非目标

SIDP（Soul Injector Debug Protocol）是开发者电脑上的 Soul Agent 与调试器硬件 Soul Injector 之间的机器级调试和日志流协议。未来通过 Soul Interconnect 连接时，云端只中继 SIDP 二进制消息。

SIDP v1 的目标：

- 支持单核 Cortex-M0、Cortex-M3 和 Cortex-M4。
- 网络往返数尽可能少。
- 协议和 Soul Injector 固件实现尽可能简单。
- 通过架构/profile 和可变寄存器列表保留 RISC-V 和 Armv8-M 扩展能力。
- 不在协议中携带 GDB、DWARF 或 RTOS 语义。

SIDP v1 非目标：

- 通用 RPC framework。
- 原始 SWD/JTAG transaction tunnel。
- 多核/多 hart 调试。
- 断线续传旧调试会话。
- RTOS thread、符号、变量或源码行协议。

当前只有 Cortex-M4 目标板可用于实机测试，因此M4是首个端到端验证profile。Cortex-M0和M3仍属于v1协议与实现范围，但在获得对应硬件前必须标记为未经实机验证。

## 2. 传输与字节序

- 传输使用 WebSocket binary message。
- 一个 WebSocket message 必须包含一个完整 SIDP message。
- WebSocket 分片由 WebSocket library 重组，SIDP 不感知分片。
- SIDP v1 只支持 little-endian 主机和目标，不实现大端兼容。
- 所有多字节整数直接使用 little-endian 本机表示，不做字节序转换。
- 目标内存数据按地址顺序传输，不做字节交换。
- v1 WebSocket message硬上限为8 KiB，单次内存读写默认最大4 KiB。

```c
#include <stdint.h>

static constexpr uint32_t SIDP_MAX_FRAME_SIZE = 8192;
```

接收端在解析任何可变长度字段前先拒绝小于12字节或大于 `SIDP_MAX_FRAME_SIZE` 的message，并使用不溢出的长度计算验证count、length和柔性数组恰好落在WebSocket message边界内。Attach response也不得分片；`memory_region_count` 必须满足固定response字段加全部 `sidp_memory_region_t` 后仍不超过8 KiB，否则这是本地target YAML配置错误，Attach失败。

wire declaration 使用 C++ 固定宽度枚举和 packed struct。两端不需要做 endian conversion，但从网络 buffer 读取多字节字段时仍应使用 `memcpy`，不要对可能未对齐的地址直接解引用。

可变长结构统一使用位于最后的柔性数组成员，例如 `uint8_t payload[]` 或 `sidp_memory_range_t ranges[]`。这是 GCC/Clang 在 C++ 模式下支持的扩展，会产生 pedantic warning，但在 GNU C++ 模式下可用；`sizeof(struct)` 不包含柔性数组数据。

## 3. 12 字节消息头

```c
#include <stdint.h>

enum sidp_msg_kind_t : uint8_t {
    SIDP_KIND_REQUEST  = 1,
    SIDP_KIND_RESPONSE = 2,
    SIDP_KIND_EVENT    = 3,
    SIDP_KIND_LOG_STREAM = 4,
};

/* 在第4节定义；这里的前置声明允许header直接使用枚举类型。 */
enum sidp_opcode_t : uint16_t;

typedef struct __attribute__((packed)) {
    uint8_t version;          /* v1 = 1 */
    sidp_msg_kind_t kind;
    sidp_opcode_t opcode;
    uint32_t request_id;

    /*
     * CRC-32/ISO-HDLC of:
     *   bytes [0, offsetof(crc32)) followed by the complete payload.
     * The crc32 field itself is excluded.
     */
    uint32_t crc32;

    uint8_t payload[];
} sidp_msg_header_t;

static_assert(sizeof(sidp_msg_header_t) == 12, "SIDP header must be 12 bytes");
```

规则：

- Soul Agent 为 Request 分配非零 `request_id`。
- Response 使用对应 Request 的 `request_id`。
- Event 的 `request_id` 固定为 0。
- Log Stream 的 `request_id` 固定为 0，不要求Response。
- v1 中 Soul Agent 同一时间最多保留一个未完成 Request，但 Event 可在任意时刻到达。
- 同一WebSocket连接上的SIDP frame严格按发送顺序交付，Soul Interconnect不得重排。Soul Injector必须通过单一有序TX队列串行发送Response和Event。
- 由某个Request直接导致的Event必须排在该Request的Response之后。例如目标在RUN发出后立即命中断点，Soul Injector也必须先发送RUN Response，再发送已经缓存的STOPPED Event。
- 与当前Request无因果关系的异步Event可以先于其Response到达。例如目标在处理GET_STATE期间自发halt；Soul Agent必须按 `kind + request_id` 分派消息，不能假定收到的下一帧一定是当前Response。
- 连接本身代表会话，因此帧内不再放 session UUID、sequence 或 payload length。
- 每条新连接的 `request_id` 从1开始单调递增；0永不分配。达到 `UINT32_MAX` 后Soul Agent应主动重连，不在同一连接内回绕复用ID。
- Soul Injector收到不支持的 `header.version` 时立即以WebSocket protocol error关闭连接，不尝试按未知布局返回SIDP Response。支持版本内的未知Request opcode返回 `SIDP_STATUS_UNSUPPORTED`。

### 3.1 CRC-32

CRC 使用 CRC-32/ISO-HDLC，也常被称为 CRC-32/IEEE：

```text
width   = 32
poly    = 0x04C11DB7
refin   = true
refout  = true
init    = 0xFFFFFFFF
xorout  = 0xFFFFFFFF
check("123456789") = 0xCBF43926
```

常见反射实现使用多项式 `0xEDB88320`。计算顺序是：

```text
header.version .. header.request_id（前8字节）
payload（WebSocket message size - 12）
```

接收端必须先验证 WebSocket message 至少有12字节，再验证 CRC；CRC 失败的消息直接丢弃并记录协议错误。CRC 是为了发现实现中的缓冲区截断、长度处理或内存损坏，不代替 WSS/TLS 鉴权和完整性保护。

### 3.2 Request超时和歧义恢复

CRC失败时接收端无法信任 `request_id`，所以只能静默丢帧；恢复责任在Soul Agent：

1. 每个Request启动本地timeout。建议普通读操作5秒，HALT/RESET/DETACH等控制操作30秒；具体值由Soul Agent配置，必须明显大于当前网络RTT。
2. timeout后将原 `request_id` 放入本连接的late-response忽略表；迟到Response不得再改变GDB可见状态。
3. GET_STATE、READ_MEMORY和READ_REGISTERS等只读请求可在条件仍成立时用新 `request_id` 重试一次；带 `stop_id` 的请求还必须确认停止世代未变化。
4. RUN、RESET、DETACH、WRITE_MEMORY和WRITE_REGISTERS不得自动重放，因为原请求可能已经执行。Soul Agent先用新ID发送GET_STATE探测；写操作还应使相关cache失效，并按需读回验证。无法确定状态时终止GDB session，而不是猜测。
5. GET_STATE也超时、WebSocket断开或连续协议错误达到本地阈值时，将目标视为TARGET_LOST并关闭连接。

WebSocket ping/pong只判断链路存活，不替代SIDP Request timeout。

## 4. Opcode

```c
enum sidp_opcode_t : uint16_t {
    SIDP_OP_ATTACH             = 0x0001,
    SIDP_OP_DETACH             = 0x0002,
    SIDP_OP_GET_STATE          = 0x0003,

    SIDP_OP_READ_MEMORY        = 0x0010,
    SIDP_OP_READ_MEMORY_VECTOR = 0x0011, /* 可选能力 */
    SIDP_OP_WRITE_MEMORY       = 0x0012,
    SIDP_OP_READ_REGISTERS     = 0x0013,
    SIDP_OP_WRITE_REGISTERS    = 0x0014,

    SIDP_OP_RUN                = 0x0020,
    SIDP_OP_HALT               = 0x0021,
    SIDP_OP_RESET_HALT         = 0x0022,
    SIDP_OP_RESET_RUN          = 0x0023,

    SIDP_OP_SET_LOG_STREAM     = 0x0030,
    SIDP_OP_LOG_DATA           = 0x0031,

    SIDP_EVT_STOPPED           = 0x0080,
    SIDP_EVT_TARGET_LOST       = 0x0081,
};
```

`READ_MEMORY_VECTOR` 不是 v1 基础实现的硬要求。支持时通过 Attach response 的 capability bit 宣告。

## 5. 统一状态码

```c
enum sidp_status_t : int32_t {
    SIDP_STATUS_OK                   = 0,
    SIDP_STATUS_ERROR                = 1,
    SIDP_STATUS_INVALID_ARGUMENT     = 2,
    SIDP_STATUS_UNSUPPORTED          = 3,
    SIDP_STATUS_BUSY                 = 4,
    SIDP_STATUS_TIMEOUT              = 5,

    SIDP_STATUS_TARGET_RUNNING       = 10,
    SIDP_STATUS_TARGET_HALTED        = 11,
    SIDP_STATUS_TARGET_LOST          = 12,
    SIDP_STATUS_STALE_STOP           = 13,

    SIDP_STATUS_SWD_ERROR            = 20,
    SIDP_STATUS_ADDRESS_ERROR        = 21,
    SIDP_STATUS_NO_BREAKPOINT_SLOT   = 22,
    SIDP_STATUS_NO_WATCHPOINT_SLOT   = 23,
    SIDP_STATUS_ALIGNMENT_ERROR      = 24,
    SIDP_STATUS_TARGET_MISMATCH      = 25,
};

typedef struct __attribute__((packed)) {
    sidp_status_t status;
    uint8_t payload[];
} sidp_response_prefix_t;
```

Response payload 总是以 `sidp_response_prefix_t` 开始。不在wire protocol中直接使用 `esp_err_t`。

## 6. Attach 和目标 profile

```c
enum sidp_architecture_t : uint8_t {
    SIDP_ARCH_ARM_M = 1,
    SIDP_ARCH_RISCV = 2,
};

enum sidp_target_profile_t : uint8_t {
    SIDP_PROFILE_ARMV6M       = 1, /* Cortex-M0; profile也可扩展至M0+ */
    SIDP_PROFILE_ARMV7M       = 2, /* Cortex-M3 */
    SIDP_PROFILE_ARMV7EM      = 3, /* Cortex-M4/M7 */
    SIDP_PROFILE_ARMV8M_BASE  = 4, /* Cortex-M23 */
    SIDP_PROFILE_ARMV8M_MAIN  = 5, /* Cortex-M33 */
    SIDP_PROFILE_ARMV81M_MAIN = 6, /* Cortex-M55/M85 */

    SIDP_PROFILE_RV32         = 32,
    SIDP_PROFILE_RV64         = 33,
};

enum sidp_capability_t : uint32_t {
    SIDP_CAP_STOP_SNAPSHOT     = 1u << 0,
    SIDP_CAP_MEMORY_VECTOR     = 1u << 1,
    SIDP_CAP_SOFTWARE_BP       = 1u << 2,
    SIDP_CAP_HARDWARE_BP       = 1u << 3,
    SIDP_CAP_WATCHPOINT        = 1u << 4,
    SIDP_CAP_SINGLE_STEP       = 1u << 5,
    SIDP_CAP_RESET_HALT        = 1u << 6,
    SIDP_CAP_UART_LOG_STREAM   = 1u << 7,
    SIDP_CAP_RTT_LOG_STREAM    = 1u << 8,
    SIDP_CAP_FPU               = 1u << 9,
    SIDP_CAP_RESET_SYSTEM      = 1u << 10,
    SIDP_CAP_RESET_NRST        = 1u << 11,
    SIDP_CAP_RESET_RUN         = 1u << 12,
};

enum sidp_memory_type_t : uint8_t {
    SIDP_MEMORY_RAM   = 1,
    SIDP_MEMORY_FLASH = 2,
    SIDP_MEMORY_MMIO  = 3,
};

enum sidp_memory_flag_t : uint8_t {
    SIDP_MEM_READ      = 1u << 0,
    SIDP_MEM_WRITE     = 1u << 1,
    SIDP_MEM_EXECUTE   = 1u << 2,
    SIDP_MEM_CACHEABLE = 1u << 3,
    SIDP_MEM_VOLATILE  = 1u << 4,
};

enum sidp_vector_catch_t : uint32_t {
    SIDP_VECTOR_CATCH_NONE         = 0,
    SIDP_VECTOR_CATCH_RESET        = 1u << 0,
    SIDP_VECTOR_CATCH_HARD_FAULT   = 1u << 1,
    SIDP_VECTOR_CATCH_MEM_MANAGE   = 1u << 2,
    SIDP_VECTOR_CATCH_BUS_FAULT    = 1u << 3,
    SIDP_VECTOR_CATCH_USAGE_FAULT  = 1u << 4,
};

typedef struct __attribute__((packed)) {
    uint64_t start;
    uint64_t length;
    sidp_memory_type_t type;
    sidp_memory_flag_t flags;
    uint8_t reserved[2];
} sidp_memory_region_t;

typedef struct __attribute__((packed)) {
    uint16_t initial_stack_before; /* 建议64 */
    uint16_t initial_stack_after;  /* 建议512 */
    uint8_t  connect_under_reset;
    uint8_t  halt_after_attach;
    uint8_t  reserved[2];
} sidp_attach_request_t;

static_assert(sizeof(sidp_attach_request_t) == 8,
              "SIDP wire layout changed");

typedef struct __attribute__((packed)) {
    sidp_status_t status;
    sidp_architecture_t architecture;
    sidp_target_profile_t profile;
    uint8_t address_width; /* 32 or 64 */
    uint8_t reserved0;
    sidp_capability_t capabilities;
    uint32_t target_id;    /* selected local target.yaml definition ID */
    uint32_t cpu_id;       /* Cortex-M CPUID or architecture equivalent */
    sidp_vector_catch_t supported_vector_catch_mask;
    uint16_t hardware_breakpoints;
    uint16_t hardware_watchpoints;
    uint16_t memory_region_count;
    uint16_t max_memory_transfer;
    sidp_memory_region_t memory_regions[];
} sidp_attach_response_t;

static_assert(sizeof(sidp_memory_region_t) == 20,
              "SIDP wire layout changed");
static_assert(sizeof(sidp_attach_response_t) == 32,
              "SIDP attach response prefix changed");
```

Memory map 保留可变条目，因为即使在 Cortex-M0/M3/M4 范围内也常有多块 RAM/Flash：

SWD/JTAG时钟不属于SIDP attach参数。它由Soul Injector本地的目标/板级YAML配置定义，并由对应target backend在attach前应用；Soul Agent不能通过远程请求临时覆盖调试时钟。这样离线烧录和在线调试共用同一份经过验证的电气与时序配置。

- `sidp_attach_response_t::architecture` 使用 `sidp_architecture_t`。
- `sidp_attach_response_t::profile` 使用 `sidp_target_profile_t`。
- `sidp_attach_response_t::capabilities` 使用 `sidp_capability_t`，可组合多个bit。
- `sidp_memory_region_t::type` 使用 `sidp_memory_type_t`。
- `sidp_memory_region_t::flags` 使用 `sidp_memory_flag_t`，可组合多个bit。

Soul Agent 根据 `architecture + profile + capabilities` 选择 GDB target description 和寄存器映射。Soul Injector 不生成 XML。特别是 `SIDP_PROFILE_ARMV7EM` 只表示 ARMv7E-M，不表示目标必然带 FPU；只有 attach response 同时设置 `SIDP_CAP_FPU` 时，Soul Agent 才能在 GDB target description 中暴露浮点寄存器。

`SIDP_CAP_FPU` 必须来自 Soul Injector attach 时的运行时探测，不得根据“Cortex-M4”名称、芯片系列或 profile 硬编码。探测失败时按无 FPU 处理。

### 6.1 目标识别和memory map来源

SIDP v1不尝试自动识别任意芯片，也不允许Soul Agent远程注入memory map。闭环流程固定为：

1. Soul Injector在会话前加载并选择本地 `target.yaml` variant；该配置是目标型号、memory map、debug时钟、reset接线和flash algorithm的唯一权威来源。
2. 配置系统为该variant提供稳定的32位 `target_id`。它标识“使用了哪份目标定义”，不是Arm CPUID；建议在YAML生成/发布阶段显式分配并检查重复值。
3. Attach时Soul Injector读取 `cpu_id`。Cortex-M使用CPUID只验证内核profile，不能据此宣称识别了具体MCU。
4. 如果YAML为该芯片定义了额外识别probe，例如可用的vendor/device ID寄存器，则Soul Injector执行并校验；没有此类寄存器的低端目标只做YAML选择和CPUID兼容性校验。
5. 任一明确probe与配置不匹配时返回 `SIDP_STATUS_TARGET_MISMATCH`，不进入调试状态。不能为了继续连接而静默改用另一个memory map。
6. Attach response中的 `memory_regions[]` 由已选择YAML的完整debug memory map生成。YAML没有明确描述的MMIO范围不出现在response中，Soul Agent也不得猜测。若现有烧录配置解析器只保留flash algorithm所需RAM，v1调试实现必须先扩展出独立的完整debug memory map视图，不能把“解析器当前没保存”误当作目标没有Flash/MMIO区域。

`target_id` 让Soul Agent选择芯片级UI/provider，`cpu_id` 用于诊断和profile验证。二者不能互相替代。SWD/JTAG实际时钟即使由本地backend按YAML策略降速，也只进入Soul Injector诊断日志；它不是SIDP协商字段。YAML要求严格频率且无法建立连接时，Attach直接返回SWD错误。

## 7. 目标状态

```c
enum sidp_target_state_t : uint8_t {
    SIDP_TARGET_DETACHED = 0,
    SIDP_TARGET_HALTED   = 1,
    SIDP_TARGET_RUNNING  = 2,
    SIDP_TARGET_LOST     = 3,
};

typedef struct __attribute__((packed)) {
    sidp_status_t status;
    sidp_target_state_t target_state;
    uint8_t reserved[3];
    uint32_t stop_id;
} sidp_get_state_response_t;
```

`sidp_get_state_response_t::target_state` 使用 `sidp_target_state_t`。

GET_STATE和HALT使用空payload；payload长度不为0时返回 `SIDP_STATUS_INVALID_ARGUMENT`。

`stop_id = 0` 表示当前会话尚无有效停止现场。每条新连接成功Attach前从0开始，目标每次发生新的running-to-halted转换时递增，第一次有效停止为1。保持在同一halt状态、重复GET_STATE或重复HALT都不递增。DETACH或连接关闭后内部清零；如果计数将从 `UINT32_MAX` 回绕，Soul Injector终止当前会话，不能在同一会话复用旧世代号。

Soul Agent 的停止态缓存以 `stop_id` 为世代号。

### 7.1 v1状态转移和错误

| Request | DETACHED | HALTED | RUNNING | LOST |
|---|---|---|---|---|
| ATTACH | 执行attach | `SIDP_STATUS_BUSY` | `SIDP_STATUS_BUSY` | `SIDP_STATUS_TARGET_LOST` |
| GET_STATE | 返回DETACHED | 返回HALTED和当前 `stop_id` | 返回RUNNING，`stop_id=0` | 返回LOST |
| READ/WRITE_REGISTERS | `SIDP_STATUS_ERROR` | 允许，校验 `stop_id` | `SIDP_STATUS_TARGET_RUNNING` | `SIDP_STATUS_TARGET_LOST` |
| READ/WRITE_MEMORY | `SIDP_STATUS_ERROR` | 按memory map允许 | 仅显式ALLOW_RUNNING且region允许时 | `SIDP_STATUS_TARGET_LOST` |
| RUN | `SIDP_STATUS_ERROR` | 允许，校验 `stop_id` | `SIDP_STATUS_TARGET_RUNNING` | `SIDP_STATUS_TARGET_LOST` |
| HALT | `SIDP_STATUS_ERROR` | `SIDP_STATUS_TARGET_HALTED`，不重复STOPPED | 执行同步halt | `SIDP_STATUS_TARGET_LOST` |
| RESET_HALT/RESET_RUN | `SIDP_STATUS_ERROR` | 按capability执行 | 按capability执行 | `SIDP_STATUS_TARGET_LOST` |
| DETACH | `SIDP_STATUS_ERROR` | 执行清理和final action | 内部halt后清理并执行final action | `SIDP_STATUS_TARGET_LOST` |

同一连接重复ATTACH不重新初始化backend，也不覆盖当前断点状态。其他Soul Agent尝试连接到已占用的Soul Injector时，应在WebSocket会话建立阶段被拒绝，而不是进入第二个SIDP控制会话。

如果目标已经halt但对应STOPPED还在Soul Injector队列中，HALT按第10节的竞态规则先回复再发送那一个STOPPED；表中的“HALTED不重复STOPPED”指该 `stop_id` 已经上报的稳定状态。

## 8. 通用寄存器表示

SIDP 不固定为 Cortex-M `r0-r15` struct，否则将来支持 Armv8-M banked register、MVE 或 RISC-V CSR 时必须推翻协议。

寄存器 ID 在各 profile 内定义，由 Soul Agent 转换为 GDB register number。

```c
enum sidp_register_value_flag_t : uint8_t {
    SIDP_REGISTER_VALUE_FLAG_NONE        = 0,
    SIDP_REGISTER_VALUE_FLAG_UNAVAILABLE = 1u << 0,
};

enum sidp_register_id_t : uint16_t {
    SIDP_ARM_REG_R0        = 0x0000,
    SIDP_ARM_REG_R1        = 0x0001,
    SIDP_ARM_REG_R2        = 0x0002,
    SIDP_ARM_REG_R3        = 0x0003,
    SIDP_ARM_REG_R4        = 0x0004,
    SIDP_ARM_REG_R5        = 0x0005,
    SIDP_ARM_REG_R6        = 0x0006,
    SIDP_ARM_REG_R7        = 0x0007,
    SIDP_ARM_REG_R8        = 0x0008,
    SIDP_ARM_REG_R9        = 0x0009,
    SIDP_ARM_REG_R10       = 0x000A,
    SIDP_ARM_REG_R11       = 0x000B,
    SIDP_ARM_REG_R12       = 0x000C,
    SIDP_ARM_REG_SP        = 0x000D,
    SIDP_ARM_REG_LR        = 0x000E,
    SIDP_ARM_REG_PC        = 0x000F,
    SIDP_ARM_REG_XPSR      = 0x0010,
    SIDP_ARM_REG_MSP       = 0x0011,
    SIDP_ARM_REG_PSP       = 0x0012,
    SIDP_ARM_REG_PRIMASK   = 0x0013,
    SIDP_ARM_REG_BASEPRI   = 0x0014,
    SIDP_ARM_REG_FAULTMASK = 0x0015,
    SIDP_ARM_REG_CONTROL   = 0x0016,

    SIDP_ARM_REG_S0        = 0x0040,
    SIDP_ARM_REG_S1        = 0x0041,
    SIDP_ARM_REG_S2        = 0x0042,
    SIDP_ARM_REG_S3        = 0x0043,
    SIDP_ARM_REG_S4        = 0x0044,
    SIDP_ARM_REG_S5        = 0x0045,
    SIDP_ARM_REG_S6        = 0x0046,
    SIDP_ARM_REG_S7        = 0x0047,
    SIDP_ARM_REG_S8        = 0x0048,
    SIDP_ARM_REG_S9        = 0x0049,
    SIDP_ARM_REG_S10       = 0x004A,
    SIDP_ARM_REG_S11       = 0x004B,
    SIDP_ARM_REG_S12       = 0x004C,
    SIDP_ARM_REG_S13       = 0x004D,
    SIDP_ARM_REG_S14       = 0x004E,
    SIDP_ARM_REG_S15       = 0x004F,
    SIDP_ARM_REG_S16       = 0x0050,
    SIDP_ARM_REG_S17       = 0x0051,
    SIDP_ARM_REG_S18       = 0x0052,
    SIDP_ARM_REG_S19       = 0x0053,
    SIDP_ARM_REG_S20       = 0x0054,
    SIDP_ARM_REG_S21       = 0x0055,
    SIDP_ARM_REG_S22       = 0x0056,
    SIDP_ARM_REG_S23       = 0x0057,
    SIDP_ARM_REG_S24       = 0x0058,
    SIDP_ARM_REG_S25       = 0x0059,
    SIDP_ARM_REG_S26       = 0x005A,
    SIDP_ARM_REG_S27       = 0x005B,
    SIDP_ARM_REG_S28       = 0x005C,
    SIDP_ARM_REG_S29       = 0x005D,
    SIDP_ARM_REG_S30       = 0x005E,
    SIDP_ARM_REG_S31       = 0x005F,
    SIDP_ARM_REG_FPSCR     = 0x0060,
};

typedef struct __attribute__((packed)) {
    sidp_register_id_t register_id;
    uint8_t  value_size;     /* 1, 2, 4, 8, 16... */
    sidp_register_value_flag_t flags;
    uint8_t  value[];       /* value_size bytes */
} sidp_register_value_t;
```

READ_REGISTERS request：

```c
typedef struct __attribute__((packed)) {
    uint32_t stop_id;       /* 必须与当前halt一致 */
    uint16_t core_id;       /* v1固定为0 */
    uint16_t register_count;/* 0=读取profile的全部核心寄存器 */
    sidp_register_id_t register_ids[];
} sidp_read_registers_request_t;

typedef struct __attribute__((packed)) {
    sidp_status_t status;
    uint32_t stop_id;
    uint16_t register_count;
    uint16_t reserved;
    uint8_t registers[]; /* variable-length sidp_register_value_t entries */
} sidp_read_registers_response_t;

typedef struct __attribute__((packed)) {
    uint32_t stop_id;
    uint16_t core_id; /* v1=0 */
    uint16_t register_count;
    uint8_t registers[]; /* variable-length sidp_register_value_t entries */
} sidp_write_registers_request_t;
```

WRITE_REGISTERS 使用相同的 `sidp_register_value_t` 列表，并必须携带当前 `stop_id`。

上述数值是wire contract，不是GDB register number。Soul Agent按profile转换；未知或不属于当前profile的ID返回 `SIDP_STATUS_INVALID_ARGUMENT`。

v1 profile的完整寄存器集合如下：

| Profile | 必须存在的32位寄存器ID |
|---|---|
| `SIDP_PROFILE_ARMV6M` | `R0`-`R12` (`0x0000`-`0x000C`)、`SP` (`0x000D`)、`LR` (`0x000E`)、`PC` (`0x000F`)、`XPSR` (`0x0010`)、`MSP` (`0x0011`)、`PSP` (`0x0012`)、`PRIMASK` (`0x0013`)、`CONTROL` (`0x0016`) |
| `SIDP_PROFILE_ARMV7M` | ARMv6-M集合，加 `BASEPRI` (`0x0014`) 和 `FAULTMASK` (`0x0015`) |
| `SIDP_PROFILE_ARMV7EM` 无FPU | 与ARMv7-M集合相同 |
| `SIDP_PROFILE_ARMV7EM` + `SIDP_CAP_FPU` | ARMv7-M集合，加 `S0`-`S31` (`0x0040`-`0x005F`) 和 `FPSCR` (`0x0060`) |

所有这些寄存器在v1中 `value_size = 4`。`0x0017`-`0x003F` 保留给后续Arm核心/特殊寄存器扩展，`0x0061`-`0x00FF` 保留给后续浮点/MVE扩展。

`register_count == 0` 表示读取当前profile和capability对GDB暴露的完整寄存器集合；因此带 `SIDP_CAP_FPU` 的Cortex-M4也包含S0-S31/FPSCR。STOPPED必须主动上传同一完整集合，使Soul Agent回答GDB `g` 时不产生额外跨境请求。

如果某个已声明寄存器在该次停止现场无法可靠读取，仍要发送该ID和4字节占位value，并设置 `SIDP_REGISTER_VALUE_FLAG_UNAVAILABLE`；Soul Agent在GDB RSP中把对应字节编码为 `xx`，而不是临时发起逐寄存器SIDP读取。

## 9. 内存读写

```c
enum sidp_memory_access_flag_t : uint16_t {
    SIDP_MEM_ACCESS_REQUIRE_HALTED = 1u << 0,
    SIDP_MEM_ACCESS_ALLOW_RUNNING  = 1u << 1,
    SIDP_MEM_ACCESS_ALLOW_MMIO     = 1u << 2,
};

enum sidp_memory_access_width_t : uint8_t {
    SIDP_MEM_WIDTH_DEFAULT = 0,
    SIDP_MEM_WIDTH_8       = 1,
    SIDP_MEM_WIDTH_16      = 2,
    SIDP_MEM_WIDTH_32      = 4,
};

typedef struct __attribute__((packed)) {
    uint32_t stop_id; /* require-halted时校验，否则为0 */
    uint64_t address;
    uint32_t length;
    sidp_memory_access_flag_t flags;
    sidp_memory_access_width_t access_width;
    uint8_t reserved;
} sidp_read_memory_request_t;

typedef struct __attribute__((packed)) {
    sidp_status_t status;
    uint64_t address;
    uint32_t completed_length;
    uint8_t data[]; /* completed_length bytes */
} sidp_read_memory_response_t;

typedef struct __attribute__((packed)) {
    uint32_t stop_id;
    uint64_t address;
    uint32_t length;
    sidp_memory_access_flag_t flags;
    sidp_memory_access_width_t access_width;
    uint8_t reserved;
    uint8_t data[]; /* length bytes */
} sidp_write_memory_request_t;

static_assert(sizeof(sidp_read_memory_request_t) == 20,
              "SIDP wire layout changed");
static_assert(sizeof(sidp_write_memory_request_t) == 20,
              "SIDP write-memory prefix changed");
```

WRITE_MEMORY request 使用 `sidp_write_memory_request_t`，其 `data[]` 包含 `length` 字节。写入成功后 Soul Agent 必须更新或使对应缓存区间失效。

`SIDP_MEM_ACCESS_REQUIRE_HALTED` 和 `SIDP_MEM_ACCESS_ALLOW_RUNNING` 必须恰好设置一个：两者都不设置或同时设置都返回 `SIDP_STATUS_INVALID_ARGUMENT`。`ALLOW_MMIO` 是与这两个状态位正交的附加许可。

- 设置 `REQUIRE_HALTED` 时，`stop_id` 必须是当前非零停止世代；目标正在运行则返回 `SIDP_STATUS_TARGET_RUNNING`，世代不匹配则返回 `SIDP_STATUS_STALE_STOP`。
- 设置 `ALLOW_RUNNING` 时，`stop_id` 必须为0；该请求可以在目标当前为RUNNING或HALTED时执行，但不获得停止态一致性保证。

内存请求必须完全落在Attach返回的单个memory region中，不能跨region。Soul Injector先按region type和flags校验，再访问目标：

- RAM：允许 `SIDP_MEM_WIDTH_DEFAULT` 做普通字节流传输；backend可在不改变可见语义的前提下使用对齐批量传输。
- Flash：v1允许READ_MEMORY，但WRITE_MEMORY一律返回 `SIDP_STATUS_UNSUPPORTED`，即使YAML中存在flash algorithm且region物理可编程。
- MMIO：必须设置 `SIDP_MEM_ACCESS_ALLOW_MMIO`，且必须显式选择8/16/32位宽度。

RAM和Flash请求也允许显式 `SIDP_MEM_WIDTH_8/16/32`。此时 `address` 必须按width自然对齐，`length` 必须是width的整数倍，Soul Injector按地址递增执行一系列同宽访问，不得在两端扩大访问或执行RMW；不满足对齐/倍数关系返回 `SIDP_STATUS_ALIGNMENT_ERROR`。`DEFAULT` 只用于RAM/Flash字节流，不允许用于MMIO。

### 9.1 MMIO精确访问规则

每个MMIO request只表示一次总线访问：

- `length` 必须等于 `access_width`，只能为1、2或4。
- `address` 必须按 `access_width` 自然对齐，否则返回 `SIDP_STATUS_ALIGNMENT_ERROR`。
- backend必须使用目标debug port原生支持的相同访问宽度。
- 如果AHB-AP或目标总线不支持所请求的sub-word访问，返回 `SIDP_STATUS_UNSUPPORTED`。
- 禁止为了字节/半字MMIO读而扩大成32位读取，也禁止为了字节/半字写执行read-modify-write。
- MMIO不允许partial success，不自动重试，不预读，不合并，也不进入Soul Agent cache。

这些限制避免对read-clear状态、FIFO和write-one-to-clear寄存器产生请求范围之外的副作用。RAM读取不受“单次访问”限制。

### 9.2 Flash写与现有烧录流程

SIDP v1调试会话和Soul Injector烧录会话互斥。GDB `load`、`vFlashErase`、`vFlashWrite` 和 `vFlashDone` 不转换为普通WRITE_MEMORY；Soul Agent明确返回不支持，并提示用户结束调试会话后调用既有烧录流程。烧录流程继续使用Soul Injector本地 `target.yaml` 中的flash algorithm、sector/page参数和校验逻辑。

因此Attach返回的Flash region在v1中必须带 `SIDP_MEM_READ`，但不得仅因为烧录器拥有flash algorithm就带 `SIDP_MEM_WRITE`。未来若要支持调试会话内GDB load，应设计独立的FLASH_ERASE/FLASH_PROGRAM/FLASH_VERIFY操作和cache失效规则，不能改变v1 WRITE_MEMORY的含义。

按地址范围在目标侧计算CRC-32适合未来降低flash verify流量，但它不是v1 opcode；等真实远程烧录需求出现时再分配opcode和capability。

### 9.3 可选向量读取

```c
typedef struct __attribute__((packed)) {
    uint64_t address;
    uint32_t length;
    sidp_memory_access_flag_t flags;
    sidp_memory_access_width_t access_width;
    uint8_t reserved;
} sidp_memory_range_t;

typedef struct __attribute__((packed)) {
    uint32_t stop_id;
    uint16_t range_count;
    uint16_t reserved;
    sidp_memory_range_t ranges[];
} sidp_read_memory_vector_request_t;

static_assert(sizeof(sidp_memory_range_t) == 16,
              "SIDP wire layout changed");

typedef struct __attribute__((packed)) {
    uint64_t address;
    uint32_t completed_length;
    uint8_t data[];
} sidp_memory_block_t;

typedef struct __attribute__((packed)) {
    sidp_status_t status;
    uint32_t stop_id;
    uint16_t range_count;
    uint16_t reserved;
    uint8_t blocks[]; /* sequential sidp_memory_block_t entries */
} sidp_read_memory_vector_response_t;
```

该操作用于 Soul Agent 已知多个独立地址区间时合并 RTT，不用于将原始 SWD 操作暴露给 Soul Agent。

READ_MEMORY_VECTOR中的MMIO range仍逐项遵守单次精确访问规则，Soul Injector不得把相邻MMIO range合并成更宽访问。

同一个vector request中的所有range必须选择相同的状态模式：全部REQUIRE_HALTED或全部ALLOW_RUNNING。前者要求外层 `stop_id` 匹配当前停止世代，后者要求外层 `stop_id = 0`；混用返回 `SIDP_STATUS_INVALID_ARGUMENT`。

Response中的block与request range顺序一一对应。v1采用all-or-nothing：任一range校验或读取失败时返回错误、`range_count = 0` 且没有blocks；成功时每个 `completed_length` 必须等于对应request的length。所有block总和仍受8 KiB单帧上限约束。

## 10. 断点和运行控制

GDB `Z/z` 在 Soul Agent 本地修改期望断点集合。Soul Agent 在 RUN 时将完整集合一次下发，避免每个断点一个跨境 RTT，也避免 Soul Agent/Soul Injector 的增量状态偏离。

```c
enum sidp_breakpoint_kind_t : uint8_t {
    SIDP_BREAKPOINT_AUTO     = 0,
    SIDP_BREAKPOINT_HARDWARE = 1,
    SIDP_BREAKPOINT_SOFTWARE = 2,
};

typedef struct __attribute__((packed)) {
    uint32_t breakpoint_id;
    uint64_t address;
    sidp_breakpoint_kind_t kind;
    uint8_t  instruction_size;
    uint8_t  enabled;
    uint8_t  temporary;
} sidp_breakpoint_t;

enum sidp_watchpoint_access_t : uint8_t {
    SIDP_WATCH_READ       = 1,
    SIDP_WATCH_WRITE      = 2,
    SIDP_WATCH_READ_WRITE = 3,
};

typedef struct __attribute__((packed)) {
    uint32_t watchpoint_id;
    uint64_t address;
    sidp_watchpoint_access_t access;
    uint8_t  size;
    uint8_t  enabled;
    uint8_t  reserved;
} sidp_watchpoint_t;

enum sidp_run_action_t : uint8_t {
    SIDP_RUN_CONTINUE       = 1,
    SIDP_RUN_SINGLE_STEP    = 2,
    SIDP_RUN_TO_ADDRESS     = 3,
};

enum sidp_run_flag_t : uint8_t {
    SIDP_RUN_FLAG_NONE = 0,
};

enum sidp_reset_kind_t : uint8_t {
    SIDP_RESET_SYSTEM = 1,
    SIDP_RESET_NRST   = 2,
};

enum sidp_detach_action_t : uint8_t {
    SIDP_DETACH_KEEP_HALTED = 1,
    SIDP_DETACH_RESUME      = 2,
};

typedef struct __attribute__((packed)) {
    uint32_t stop_id;
    uint16_t core_id; /* v1=0 */
    sidp_run_action_t action;
    sidp_run_flag_t flags;
    sidp_vector_catch_t vector_catch_mask;
    uint64_t run_to_address;
    uint16_t breakpoint_count;
    uint16_t watchpoint_count;
    uint8_t payload[];
    /* payload: sidp_breakpoint_t[], then sidp_watchpoint_t[] */
} sidp_run_request_t;

static_assert(sizeof(sidp_run_request_t) == 24,
              "SIDP run request prefix changed");

typedef struct __attribute__((packed)) {
    sidp_reset_kind_t kind;
    uint8_t reserved[3];
} sidp_reset_request_t;

static_assert(sizeof(sidp_reset_request_t) == 4,
              "SIDP wire layout changed");

typedef struct __attribute__((packed)) {
    sidp_detach_action_t action;
    uint8_t reserved[3];
} sidp_detach_request_t;

static_assert(sizeof(sidp_detach_request_t) == 4,
              "SIDP wire layout changed");
```

枚举在结构中的对应关系：

- `sidp_breakpoint_t::kind` 使用 `sidp_breakpoint_kind_t`。
- `sidp_watchpoint_t::access` 使用 `sidp_watchpoint_access_t`。
- `sidp_run_request_t::action` 使用 `sidp_run_action_t`。
- `sidp_run_request_t::flags` 是 `sidp_run_flag_t` 的 bitmask。
- `sidp_run_request_t::vector_catch_mask` 使用 `sidp_vector_catch_t` bitmask。
- `sidp_reset_request_t::kind` 使用 `sidp_reset_kind_t`，同时用于RESET_HALT和RESET_RUN。
- `sidp_detach_request_t::action` 使用 `sidp_detach_action_t`。

Soul Injector必须先完整验证和应用断点/watchpoint集合及 `vector_catch_mask`，然后才resume。RUN中的vector catch采用替换语义；mask必须是attach response中 `supported_vector_catch_mask` 的子集，否则RUN返回 `SIDP_STATUS_UNSUPPORTED` 且目标保持halt。任意配置无法应用时RUN整体失败，不允许带着部分配置继续运行。

断点条目的v1校验规则：

- `breakpoint_id` 必须非零；0保留给Soul Injector内部临时断点。
- `enabled` 必须为1。Soul Agent不得发送disabled条目；Soul Injector收到 `enabled = 0` 返回 `SIDP_STATUS_INVALID_ARGUMENT`。
- `SIDP_BREAKPOINT_AUTO` 只允许存在于Soul Agent内部选择逻辑，不能出现在wire request；Soul Injector收到AUTO返回 `SIDP_STATUS_INVALID_ARGUMENT`。
- `SIDP_BREAKPOINT_HARDWARE` 的 `instruction_size` 必须为0，地址必须半字对齐且位于可执行region；能力/slot不足返回 `SIDP_STATUS_NO_BREAKPOINT_SLOT`。
- `SIDP_BREAKPOINT_SOFTWARE` 的 `instruction_size` 必须按第10.4节为2或4。

Watchpoint全部由DWT或目标架构等价硬件实现。`watchpoint_id` 必须非零，`access` 必须是READ/WRITE/READ_WRITE之一，`size` 只允许1、2或4，`address` 必须按size自然对齐，`enabled` 必须为1；非法字段返回 `SIDP_STATUS_INVALID_ARGUMENT` 或 `SIDP_STATUS_ALIGNMENT_ERROR`。Attach未设置 `SIDP_CAP_WATCHPOINT`、`hardware_watchpoints = 0`，或本次完整集合超过可用slot时，RUN返回 `SIDP_STATUS_NO_WATCHPOINT_SLOT` 且不resume。

`SIDP_VECTOR_CATCH_RESET`、HARD_FAULT、MEM_MANAGE、BUS_FAULT和USAGE_FAULT是SIDP语义位，Soul Injector backend负责映射到当前Cortex-M实际支持的DEMCR VC位。Lockup不是一个可配置的exception vector catch位；Soul Injector通过运行态debug状态检测lockup，并以 `SIDP_STOP_LOCKUP` 报告。

### 10.1 RUN、HALT和RESET的完成语义

三个操作的Response都使用仅含 `sidp_status_t` 的统一Response prefix，但完成时机不同。

#### RUN：异步，立即ACK

RUN是异步操作：

1. 校验 `stop_id`、action、完整断点集合和watchpoint集合。
2. 以替换语义完成断点/watchpoint配置以及必要的软件断点step-over准备。
3. 成功向目标发出本次执行所需的第一次resume或single-step命令。
4. 立即将RUN Response加入有序TX队列，不等待目标下一次停止。

RUN Response为 `SIDP_STATUS_OK` 表示执行请求已经被目标侧接受和发起，不表示目标会持续运行，也不表示本次continue最终成功到达某个特定位置。Soul Agent收到成功Response后立即进入RUNNING逻辑状态、使旧 `stop_id` 的StopCache失效，并可以发送后续GET_STATE或HALT。RUN request timeout只约束步骤1至4，不覆盖程序运行时间。

目标可能在resume后极快命中断点、fault或lockup，甚至在Soul Injector准备发送RUN Response前就再次halt。此时Soul Injector必须先缓存停止现场，将RUN Response排入TX队列，再排入STOPPED Event。因此线上顺序始终为：

```text
RUN Request
RUN Response
STOPPED Event
```

Soul Agent不得因为RUN Response和STOPPED紧邻到达而丢弃STOPPED。

#### RUN_TO_ADDRESS

`SIDP_RUN_TO_ADDRESS` 在v1中由Soul Injector使用一个临时FPB硬件断点实现，不回退为RAM `BKPT` patch。这样不会为一次run-to引入额外shadow状态。

处理顺序：

1. `run_to_address` 必须半字对齐，并完全位于带 `SIDP_MEM_EXECUTE` 的memory region，否则返回 `SIDP_STATUS_ADDRESS_ERROR`。
2. Soul Injector优先复用同地址的已安装硬件断点；否则占用预留的临时FPB slot。
3. 没有可用slot时返回 `SIDP_STATUS_NO_BREAKPOINT_SLOT`，目标保持halt。
4. 命中后立即移除临时配置，并发送 `reason = SIDP_STOP_RUN_TO_ADDRESS`、`breakpoint_id = 0` 的STOPPED。
5. 如果先因fault、watchpoint、用户HALT或其他原因停止，则移除临时配置并按真实原因上报。

除 `SIDP_RUN_TO_ADDRESS` 外，其他RUN action要求 `run_to_address = 0`，否则返回 `SIDP_STATUS_INVALID_ARGUMENT`。临时run-to断点不加入Soul Agent的期望断点集合。

#### HALT：同步到确认停止

HALT request使用空payload。Soul Injector收到后立即抢占低优先级UART/RTT工作，发出halt请求，并在本地等待目标确认halt。成功Response表示目标已经halt且STOPPED快照已经准备好；线上顺序为：

```text
HALT Request
HALT Response
STOPPED Event
```

HALT必须有本地有界超时。超时则返回 `SIDP_STATUS_TIMEOUT`，不得伪造STOPPED。如果目标随后因其他原因进入halt，仍可作为独立的自发STOPPED报告。

如果HALT到达时目标已经产生一个尚未上报的停止转换，Soul Injector完成该现场后先回复HALT，再发送该STOPPED；停止原因必须来自实际硬件状态，不得强制改写为 `SIDP_STOP_USER_HALT`。如果该halt状态此前已经用相同 `stop_id` 上报，则返回 `SIDP_STATUS_TARGET_HALTED`，不重复发送STOPPED。

#### RESET_HALT：同步到reset后停止

RESET_HALT使用 `sidp_reset_request_t`，必须显式指定reset方法，不提供含糊的默认值：

- `SIDP_RESET_SYSTEM`：Soul Injector通过SWD发出CPU/system reset请求。对Cortex-M0/M3/M4通常是写SCB AIRCR的 `SYSRESETREQ`；它不是一个独立的“SWD reset packet”。目标backend负责所需的debug重连和reset catch。
- `SIDP_RESET_NRST`：Soul Injector实际驱动目标板的 `nRESET`/`nRST` 引脚，保持有效电平后释放，再重新建立debug连接并halt。

支持 `SIDP_RESET_SYSTEM` 时attach response设置 `SIDP_CAP_RESET_SYSTEM`；硬件实际连接并支持nRST控制时设置 `SIDP_CAP_RESET_NRST`。只要至少支持一种方法，仍设置总能力 `SIDP_CAP_RESET_HALT`。请求未声明的方法必须返回 `SIDP_STATUS_UNSUPPORTED`。

nRST的GPIO、有效电平、拉低时间、释放后延时以及目标特有的连接时序属于Soul Injector板级/目标YAML配置，不进入SIDP request。Soul Agent只选择语义上的 `SYSTEM` 或 `NRST`，不能从远端指定脉宽或引脚。

Soul Injector配置目标所需的reset catch，执行指定reset，并在本地等待reset后的halt。成功Response表示reset-halt已经完成且新快照已经准备好；随后发送 `SIDP_STOP_RESET` 的STOPPED Event：

```text
RESET_HALT Request
RESET_HALT Response
STOPPED Event
```

RESET_HALT必须有本地有界超时。失败或超时时Response返回对应错误，不发送由该请求伪造的STOPPED；如果目标之后真实halt，则按独立停止转换正常上报。

执行任何reset前，Soul Injector应先在目标可访问时halt、用shadow恢复已安装的软件断点并关闭FPB/DWT，避免RAM保留的 `BKPT` 或残留比较器在复位后意外触发。若必须用nRST恢复一个已经无法通过debug访问的目标，则保留原installed table和shadow，reset-halt完成后再次检查并恢复仍残留的patch；在清理完成前不得把目标resume。reset成功后旧断点安装状态和StopCache全部失效，下一次RUN根据Soul Agent下发的完整集合重新安装。

#### RESET_RUN：同步到确认运行

RESET_RUN与RESET_HALT使用相同的 `sidp_reset_request_t` 和reset method capability检查。Soul Injector清理软件/硬件断点，执行指定reset，释放reset catch并确认目标进入running后返回Response；成功时不产生STOPPED。只有设置 `SIDP_CAP_RESET_RUN` 才允许该opcode。

如果目标在RESET_RUN后极快fault或halt，仍遵守因果顺序：先发送RESET_RUN Response，再发送真实STOPPED。RESET_RUN成功后旧 `stop_id`、StopCache和断点安装状态全部失效。

### 10.2 DETACH

GET_STATE和HALT request为空payload；DETACH使用 `sidp_detach_request_t`，由Soul Agent明确选择最终目标状态：

- `SIDP_DETACH_KEEP_HALTED`：清理调试资源后保持目标halt。
- `SIDP_DETACH_RESUME`：清理调试资源后让目标从当前逻辑PC继续运行。

无论DETACH从HALTED还是RUNNING开始，Soul Injector都必须先获得目标控制，恢复所有RAM软件断点原指令，关闭FPB/DWT和vector catch配置，并清除内部shadow/installed table。RUNNING状态下允许为清理而执行一次内部halt，该内部停止不发送STOPPED。

DETACH成功Response表示清理和最终halt/resume动作都已完成。Soul Injector必须先发送Response，再把会话状态改为DETACHED；之后不再发送该目标的STOPPED或日志。清理失败时返回错误并保持attached，不能带着未知断点状态假装detach成功。

Soul Agent的GDB映射：

- `D`：发送DETACH；默认action由用户/项目配置选择，建议 `SIDP_DETACH_RESUME`。
- `k`：按产品策略先RESET_RUN再DETACH，或仅DETACH；必须在Soul Agent配置中明确，不能由Soul Injector猜测。
- `monitor reset halt`：RESET_HALT。
- `monitor reset run`：RESET_RUN。
- `R`/`vRun`：需要停在入口时使用RESET_HALT，之后重新下发完整断点集合；需要直接运行时使用RESET_RUN。

DETACH本身不隐式reset。reset与detach保持两个可观察、可分别报错的Request，避免一个复杂组合opcode。

### 10.3 无对应RUN的STOPPED

STOPPED不是RUN的response，也不携带RUN的 `request_id`。目标在任何已attach时刻都可能因为vector catch、fault、lockup、外部debug request或其他可检测原因进入halt，因此没有先行RUN Request的STOPPED完全合法。每次新的running-to-halted转换只生成一次新 `stop_id` 和一次STOPPED Event。

### 10.4 Cortex-M软件断点

SIDP v1的软件断点仅用于可写、可执行RAM中的Cortex-M Thumb代码。Flash断点使用FPB；Soul Injector不得为了实现普通软件断点而擅自擦写Flash。

职责分工：

- Soul Agent拥有期望断点集合。GDB `Z/z` 只修改该集合，下一次RUN携带完整集合。
- Soul Injector不拥有用户断点策略，但必须维护执行所需的临时installed table：`breakpoint_id`、地址、原始指令字节、patch字节、大小和当前安装状态。
- `temporary` 是Soul Agent语义。temporary断点命中后，Soul Agent从下一次RUN的完整集合中删除它；Soul Injector不自行永久删除期望断点。

对Cortex-M0/M3/M4，v1软件断点patch始终是写在指令首个halfword上的16位Thumb `BKPT`，地址必须半字对齐。`instruction_size` 表示原指令长度，只允许2或4：16位Thumb指令保存2字节，32位Thumb-2指令保存完整4字节，但目标内存只把前2字节替换为BKPT。安装前Soul Injector必须验证整个集合、读取并保存每条完整原指令，然后再写入patch；软件断点覆盖区间不得互相重叠。若任意断点无法安装，必须回滚到RUN前的完整安装状态，返回错误且不resume。

不能把当前目标内存中的 `BKPT` 再次当作“原指令”保存。重复下发同一完整集合时，应复用installed table中的shadow；集合删除或地址改变时，先用shadow恢复旧地址，再安装新集合。

#### 命中后的PC和step-over

Cortex-M执行16位Thumb `BKPT` 后，调试现场中的硬件PC可能已经指向下一条指令。Soul Injector识别出 `PC - 2` 命中其installed table中的软件断点后，必须把对外可见的PC规范化为断点地址，在STOPPED寄存器快照中报告该地址，并设置 `reason = SIDP_STOP_BREAKPOINT` 和对应 `breakpoint_id`。后续READ_REGISTERS也必须看到规范化后的PC，不得让Soul Agent或GDB再次猜测和减2。

从该STOPPED继续时，Soul Injector执行以下本地序列：

1. 根据下一次RUN携带的完整集合确定该断点是否仍然需要安装。
2. 用shadow恢复完整的2字节Thumb或4字节Thumb-2原指令。
3. 确保目标PC指向断点地址。
4. 使用硬件single-step执行一次原始指令；这个内部停止不产生用户可见STOPPED。
5. 如果断点仍在期望集合中，重新写入 `BKPT` patch；temporary断点已被Soul Agent移除时不再补插。
6. 对 `SIDP_RUN_CONTINUE`/`SIDP_RUN_TO_ADDRESS` 继续resume；对用户请求的 `SIDP_RUN_SINGLE_STEP`，内部这一次step就是用户要求的step，随后生成正常的 `SIDP_STOP_SINGLE_STEP` STOPPED。

如果停止期间Soul Agent通过WRITE_REGISTERS把PC改到其他地址，则取消“从当前软件断点step-over”的待处理状态：按新完整集合恢复或保留该地址的patch，然后直接从新PC执行，不能擅自执行原断点地址的指令。

如果内部step发生fault、watchpoint或其他非预期停止，Soul Injector不得吞掉它；在尽可能恢复一致的断点安装状态后，按真实原因生成STOPPED。如果恢复patch或目标状态失败，则保持目标halt并报告明确错误/目标丢失，绝不能在断点表半更新时继续运行。

#### READ_MEMORY shadow

只要某个软件断点patch仍安装在目标内存中，所有SIDP READ_MEMORY和READ_MEMORY_VECTOR结果都必须做overlap替换：首个halfword返回installed table保存的原始字节，而不是 `BKPT` 的 `0xBE00`；32位Thumb-2指令的其余2字节本来未被替换，但仍属于同一shadow记录。规则同样适用于部分重叠和一次读取跨越多个软件断点。

因此Soul Agent cache、GDB反汇编和源码单步始终看到程序原始内容。shadow替换由Soul Injector完成，Soul Agent不需要知道目标内存此刻是否仍有patch。

WRITE_MEMORY若与已安装软件断点的完整 `instruction_size` 区间重叠，Soul Injector必须把新数据更新到shadow并保持目标中的断点patch有效；如果无法原子地完成“恢复、写入、更新shadow、补插”，则整个WRITE_MEMORY失败且保持原状态。不得让shadow与程序的新原始字节失配。

detach、切换目标backend或正常结束调试会话前，Soul Injector必须尽力恢复所有软件断点的原指令。连接意外断开时按会话断线策略先halt，再恢复patch；恢复失败必须记录并上报，不能静默把RAM中的 `BKPT` 留给继续运行的目标。

## 11. STOPPED Event

```c
enum sidp_stop_reason_t : uint8_t {
    SIDP_STOP_UNKNOWN      = 0,
    SIDP_STOP_BREAKPOINT   = 1,
    SIDP_STOP_WATCHPOINT   = 2,
    SIDP_STOP_SINGLE_STEP  = 3,
    SIDP_STOP_USER_HALT    = 4,
    SIDP_STOP_VECTOR_CATCH = 5,
    SIDP_STOP_FAULT        = 6,
    SIDP_STOP_RESET        = 7,
    SIDP_STOP_LOCKUP       = 8,
    SIDP_STOP_RUN_TO_ADDRESS = 9,
};

enum sidp_stopped_flag_t : uint8_t {
    SIDP_STOPPED_FLAG_NONE = 0,
};

typedef struct __attribute__((packed)) {
    uint32_t stop_id;
    uint16_t core_id;       /* v1=0 */
    sidp_stop_reason_t reason;
    sidp_stopped_flag_t flags;
    uint32_t reason_detail; /* Arm可放DFSR，RISC-V可放cause */
    uint32_t breakpoint_id;
    uint64_t watchpoint_address;
    uint16_t register_count;
    uint16_t stack_length;
    uint64_t stack_address;
    uint8_t payload[];
    /* payload: sidp_register_value_t[], then stack bytes */
} sidp_stopped_event_t;
```

`sidp_stopped_event_t::reason` 使用 `sidp_stop_reason_t`；`flags` 是 `sidp_stopped_flag_t` 的 bitmask。

STOPPED 中的寄存器采用 `sidp_register_value_t` 列表，并上报当前profile和capability对GDB暴露的完整集合。无 `SIDP_CAP_FPU` 时不得上报浮点寄存器；有该能力时必须包含S0-S31/FPSCR的值或带UNAVAILABLE标志的占位条目，保证首次GDB `g` 不产生新SIDP request。

上述完整快照要求仅在Attach设置 `SIDP_CAP_STOP_SNAPSHOT` 时成立。未设置该capability时，STOPPED仍必须发送停止原因、`stop_id` 和命中信息，但设置 `register_count = 0`、`stack_length = 0`、`stack_address = 0` 且payload为空；Soul Agent随后用READ_REGISTERS/READ_MEMORY获取现场。这是允许的高延迟降级路径，不满足“首次 `g` 无WAN request”的延迟验收目标。

建议默认栈快照范围：

```text
[SP - 64, SP + 512)
```

实际范围必须裁剪到 Attach 返回的 RAM region。快照数据量通常不超过700字节，不设计额外快照分片协议。GDB 后续需要更多栈时，Soul Agent 通过普通 READ_MEMORY 扩展缓存。

## 12. TARGET_LOST Event

```c
enum sidp_target_lost_reason_t : uint8_t {
    SIDP_TARGET_LOST_SWD_FAULT  = 1,
    SIDP_TARGET_LOST_POWER      = 2,
    SIDP_TARGET_LOST_DISCONNECT = 3,
    SIDP_TARGET_LOST_TIMEOUT    = 4,
};

typedef struct __attribute__((packed)) {
    sidp_target_lost_reason_t reason;
    uint8_t reserved[3];
    uint32_t detail;
} sidp_target_lost_event_t;
```

`sidp_target_lost_event_t::reason` 使用 `sidp_target_lost_reason_t`。

Soul Agent 收到后应终止当前 GDB session，而不是继续返回旧缓存。

## 13. UART/RTT Log Stream

Soul Injector 可以通过新版硬件的额外UART采集目标串口日志，也可以通过SWD读取RTT up-buffer。日志是低优先级字节流，不属于Request/Response，因此使用 `SIDP_KIND_LOG_STREAM`。

```c
enum sidp_log_channel_t : uint16_t {
    SIDP_LOG_CHANNEL_TARGET_UART0 = 0x0001,

    SIDP_LOG_CHANNEL_TARGET_RTT_UP_0 = 0x0100,
    SIDP_LOG_CHANNEL_TARGET_RTT_UP_1 = 0x0101,
    SIDP_LOG_CHANNEL_TARGET_RTT_UP_2 = 0x0102,
    SIDP_LOG_CHANNEL_TARGET_RTT_UP_3 = 0x0103,
};

enum sidp_log_source_t : uint8_t {
    SIDP_LOG_SOURCE_UART = 1,
    SIDP_LOG_SOURCE_RTT  = 2,
};

enum sidp_log_control_t : uint8_t {
    SIDP_LOG_DISABLE = 0,
    SIDP_LOG_ENABLE  = 1,
};

typedef struct __attribute__((packed)) {
    sidp_log_channel_t log_channel;
    sidp_log_source_t source;
    sidp_log_control_t control;

    /* UART source: 0 means use Soul Injector target configuration default. */
    uint32_t uart_baud;

    /* RTT source: Soul Agent normally resolves _SEGGER_RTT from the ELF. */
    uint64_t rtt_control_block_address;
    uint16_t rtt_up_buffer_index;
    uint16_t rtt_poll_interval_ms;
} sidp_set_log_stream_request_t;

typedef struct __attribute__((packed)) {
    sidp_log_channel_t log_channel;
    uint16_t buf_len;
    uint8_t buf[];
} sidp_log_data_t;

static_assert(sizeof(sidp_log_data_t) == 4,
              "SIDP log data prefix must be 4 bytes");
```

`SIDP_OP_SET_LOG_STREAM` 是普通Request，使用 `sidp_set_log_stream_request_t`，Response使用统一 `sidp_status_t`。对UART source：

- `log_channel` 使用 `SIDP_LOG_CHANNEL_TARGET_UART0`。
- `uart_baud` 指定波特率，0表示使用Soul Injector中的目标配置默认值。
- RTT相关字段必须为0。

对RTT source：

- `log_channel` 对应RTT up-buffer的逻辑SIDP通道。
- `rtt_control_block_address` 由Soul Agent从ELF符号 `_SEGGER_RTT` 解析后下发。
- `rtt_up_buffer_index` 指定RTT up-buffer编号。
- `rtt_poll_interval_ms` 为0时使用Soul Injector默认值。
- `uart_baud` 必须为0。

Soul Injector 发送日志数据时：

```text
header.kind       = SIDP_KIND_LOG_STREAM
header.opcode     = SIDP_OP_LOG_DATA
header.request_id = 0
payload           = sidp_log_data_t
```

`buf_len` 必须等于 WebSocket message 中剩余的字节数，`buf[]` 是原始字节，不要假设其为UTF-8、以0结尾或每帧包含完整行。建议单帧 `buf_len <= 1024`。

日志流的调度规则：

- Response、STOPPED、TARGET_LOST 和 HALT 始终高于日志优先级。
- UART RX 应先进入有界 ring buffer，再由非SWD任务组帧发送。
- RTT 需要占用SWD，必须小块读取，并为运行状态poll、HALT、寄存器/内存读写立即让路。
- 当Soul Injector、Soul Interconnect或Soul Agent的日志队列已满时，允许丢弃日志数据；不得因日志背压阻塞调试控制。
- log stream 不要求单独Response或应用层重传；每帧仍使用SIDP header中的CRC-32。

## 14. 流量和帧数

常规 STOPPED 消息约为：

```text
SIDP header                12 bytes
STOPPED fixed fields       36 bytes
20-25个基础寄存器        约160-200 bytes（含条目头）
可选33个FPU寄存器       264 bytes
栈快照                    576 bytes以内
--------------------------------------------
合计                        无FPU通常小于850 bytes，有FPU通常小于1.2 KiB
```

DHCSR 或 RISC-V Debug Module 状态轮询只发生在 Soul Injector 与目标之间，不产生互联网流量。

UART/RTT 日志流量取决于目标输出速率，与停止快照流量分开计算。Soul Injector 和中继服务必须使用有界日志队列。

## 15. v1 处理约束

- Soul Injector 只使用一个 pinned Debug/SWD task 执行所有操作。
- 一次只执行一个 Request。
- READ_REGISTERS和WRITE_REGISTERS始终要求HALTED及匹配的非零 `stop_id`；RUNNING时返回 `SIDP_STATUS_TARGET_RUNNING`，不存在allow-running寄存器flag。
- Soul Agent通常为内存访问设置 `SIDP_MEM_ACCESS_REQUIRE_HALTED`；协议没有flags=0的隐式默认值。只有显式设置 `SIDP_MEM_ACCESS_ALLOW_RUNNING` 时才允许backend评估运行态访问。
- 不缓存 MMIO。
- RUN、WRITE_MEMORY、WRITE_REGISTERS 不因网络断开自动重试。
- WebSocket 断开即结束会话；重连从 Attach 重新开始。
- LOST是当前连接的终态；该状态下包括DETACH在内的Request均返回 `SIDP_STATUS_TARGET_LOST`。唯一出口是关闭WebSocket并建立新连接。关闭时Soul Injector仍按disconnect策略尽力halt目标、恢复已知软件patch和清理比较器，但清理失败只能记录本地错误，不能假装目标已恢复。

## 16. 枚举与结构字段映射

所有枚举都指定了固定底层类型，并直接用作对应的wire struct字段类型：

| 枚举类型 | 底层类型 | 使用字段 |
|---|---:|---|
| `sidp_msg_kind_t` | `uint8_t` | `sidp_msg_header_t::kind` |
| `sidp_opcode_t` | `uint16_t` | `sidp_msg_header_t::opcode` |
| `sidp_status_t` | `int32_t` | 所有Response的 `status` |
| `sidp_architecture_t` | `uint8_t` | `sidp_attach_response_t::architecture` |
| `sidp_target_profile_t` | `uint8_t` | `sidp_attach_response_t::profile` |
| `sidp_capability_t` | `uint32_t` | `sidp_attach_response_t::capabilities` |
| `sidp_memory_type_t` | `uint8_t` | `sidp_memory_region_t::type` |
| `sidp_memory_flag_t` | `uint8_t` | `sidp_memory_region_t::flags` |
| `sidp_vector_catch_t` | `uint32_t` | `sidp_attach_response_t::supported_vector_catch_mask`、`sidp_run_request_t::vector_catch_mask` |
| `sidp_target_state_t` | `uint8_t` | `sidp_get_state_response_t::target_state` |
| `sidp_register_value_flag_t` | `uint8_t` | `sidp_register_value_t::flags` |
| `sidp_register_id_t` | `uint16_t` | `sidp_register_value_t::register_id`、`sidp_read_registers_request_t::register_ids[]` |
| `sidp_memory_access_flag_t` | `uint16_t` | 内存读写请求和 `sidp_memory_range_t::flags` |
| `sidp_memory_access_width_t` | `uint8_t` | 内存读写请求和 `sidp_memory_range_t::access_width` |
| `sidp_breakpoint_kind_t` | `uint8_t` | `sidp_breakpoint_t::kind` |
| `sidp_watchpoint_access_t` | `uint8_t` | `sidp_watchpoint_t::access` |
| `sidp_run_action_t` | `uint8_t` | `sidp_run_request_t::action` |
| `sidp_run_flag_t` | `uint8_t` | `sidp_run_request_t::flags` |
| `sidp_reset_kind_t` | `uint8_t` | `sidp_reset_request_t::kind` |
| `sidp_detach_action_t` | `uint8_t` | `sidp_detach_request_t::action` |
| `sidp_stop_reason_t` | `uint8_t` | `sidp_stopped_event_t::reason` |
| `sidp_stopped_flag_t` | `uint8_t` | `sidp_stopped_event_t::flags` |
| `sidp_target_lost_reason_t` | `uint8_t` | `sidp_target_lost_event_t::reason` |
| `sidp_log_channel_t` | `uint16_t` | `sidp_set_log_stream_request_t::log_channel`、`sidp_log_data_t::log_channel` |
| `sidp_log_source_t` | `uint8_t` | `sidp_set_log_stream_request_t::source` |
| `sidp_log_control_t` | `uint8_t` | `sidp_set_log_stream_request_t::control` |

flags/capabilities 枚举允许组合多个bit。C++实现可以为对应枚举定义 `operator|`/`operator&`，或在组合后显式 `static_cast` 回枚举类型；wire field本身始终保持表中规定的固定宽度。
