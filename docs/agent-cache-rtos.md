# Soul Agent：缓存、预读与 RTOS Thread Awareness

本文是Agent后续设计参考。当前先完成基本RSP和当前停止现场cache；自动预读、RTOS provider、vector read和日志均不阻塞首个USB/M4调试闭环。实施顺序以 [当前计划](target-support-roadmap.md#11-当前实施计划2026-09-10-调整) 为准。

## 1. Soul Agent 内部分层

```text
GDB RSP connection
        |
        v
RSP command handler
        |
        +-- target profile / register mapping
        +-- breakpoint manager
        +-- stop-state cache
        +-- RTOS provider
        |
        v
SIDP client
        |
        v
Soul Injector
```

SIDP client 只提供机器级操作。RSP handler 和 RTOS provider 不应将 GDB packet 或 RTOS 结构传给 Soul Injector。

Soul Agent 还要单独处理 `SIDP_KIND_LOG_STREAM`。日志通道不进入 GDB request/response 队列，不得阻塞调试控制。

## 2. 缓存世代

Soul Agent 为每次 STOPPED 建立一个与 `stop_id` 绑定的缓存：

```text
StopCache {
    stop_id
    reason
    registers
    memory blocks
    decoded RTOS task list
}
```

失效规则：

- RUN Response返回 `SIDP_STATUS_OK`：立即丢弃整个StopCache；RUN后紧邻到达的新STOPPED使用新 `stop_id` 建立新cache。
- RESET_HALT Response返回 `SIDP_STATUS_OK`：旧StopCache失效，紧随其后的STOPPED使用新 `stop_id` 建立reset后的cache。
- RESET_RUN或DETACH Response返回 `SIDP_STATUS_OK`：立即清除全部停止态和内存cache。
- 新 STOPPED：新建 StopCache，丢弃上一个 stop_id 的所有数据。
- WRITE_REGISTER：更新对应寄存器缓存。
- WRITE_MEMORY：更新完全覆盖的缓存字节，或简单使与该区间相交的 block 失效。
- TARGET_LOST/断线：立即清除全部缓存。

Soul Agent 绝不能用旧 `stop_id` 的数据回答新的 GDB 会话。

### 2.1 缓存成立的前提和边界

内存缓存只在目标HALTED时存在。它成立的前提是：CPU halt后，只由CPU写入的内存（栈、全局变量、heap、RTOS TCB/链表）不会再变化，同一 `stop_id` 内重复读取得到相同字节。GDB的backtrace、局部变量和RTOS任务列表绝大部分读取都落在这类内存上。

Cortex-M debug halt只停止内核，不停止DMA控制器。以下来源在halt期间仍可修改内存，缓存会把它们“冻结”成第一次读取时的值：

- DMA（例如UART RX DMA在halt期间继续填充buffer）。
- 其他bus master：第二个内核、带独立DMA的Ethernet/USB控制器等。
- MMIO：按第4.3节始终不缓存。

这类内存即使不缓存，读回的值也只是读取瞬间的快照；缓存的问题是把“读取后几微秒即过时”延长为“整个halt期间都过时”。因此缓存定位为**突发读取合并器（burst coalescer）**，而不是目标内存的长期镜像：

- **短TTL**：一次 `bt` 或IDE刷新变量窗口会在几毫秒内产生几十个小读取，缓存的收益主要来自这一个突发内。每个缓存block（包括STOPPED栈快照中的内存部分）除受 `stop_id` 约束外，还设置较短的有效期（建议默认0.5-1秒，可配置）；同一halt内超过TTL的block重新经SIDP读取。寄存器不受TTL影响，halt期间内核寄存器不会变化。
- **写前新鲜读取**：GDB对bitfield或sub-word字段赋值时会读取所在word、修改后整体写回（read-modify-write）。如果这次读取命中陈旧缓存，写回可能覆盖DMA在此期间修改过的字节——这是缓存唯一可能导致目标数据被破坏而不只是显示错误的路径。Soul Agent在处理落在某block内的WRITE_MEMORY之前，若该block的读取时间超过很短的阈值（建议几毫秒），应先经SIDP重新读取该区间，再基于新数据完成写入；写入本身始终直达目标。写操作很少，这只增加一次RTT。
- **运行态不缓存**：带 `SIDP_MEM_ACCESS_ALLOW_RUNNING` 的访问始终直达目标，结果不进入缓存。

## 3. STOPPED 时的主动数据

Soul Injector 的 STOPPED Event 默认包含：

- profile和capability对GDB暴露的全部寄存器；带FPU的Cortex-M4包括S0-S31/FPSCR，读取失败的条目缓存为unavailable。
- 停止原因和原始 debug cause。
- 与断点/watchpoint 对应的 Soul Agent ID。
- `[SP - 64, SP + 512)` 与有效 RAM 边界的交集。

这要求Attach设置 `SIDP_CAP_STOP_SNAPSHOT`。能力缺席时STOPPED只建立stop reason/stop_id，Soul Agent必须随后读取寄存器和栈；功能仍可用，但不满足低RTT验收目标。

Soul Agent 收到后可以不访问 WAN 地回答：

- `?`：当前停止原因。
- `g`：完整GDB寄存器集合，包括已声明的FPU寄存器或 `xx` unavailable占位。
- `p`：已在 snapshot 中的单个寄存器。
- `m`：完全位于栈快照范围内的内存。

## 4. 按需缓存和预读

Soul Agent 而不是 Soul Injector 决定普通内存的预读范围。

### 4.1 RAM

目标 halt 时，GDB 要求读取一小段可缓存 RAM 后，Soul Agent 将请求扩大为对齐 block：

```text
GDB请求：  address=0x20000134, length=4
Agent读取：address=0x20000100, length=256
```

建议初始 block size 为 256 字节。连续命中时可扩大为512字节；不建议无条件扩大到4KiB，因为500kHz SWD上的本地读取时间也不可忽略。

预读是否值得取决于链路RTT与SWD吞吐的比值，Soul Agent应根据实测值决定，而不是固定开启：

- **远程链路（例如200ms RTT）**：为4字节请求读取256字节几乎总是划算，额外SWD时间远小于一次RTT。
- **本地USB高速（亚毫秒RTT）**：预读可能反而变慢。500kHz SWD读取256字节约需6ms，而GDB实际只需要的4字节远低于1ms；8MHz时约0.4ms，大致持平。

因此Soul Agent在会话中测量请求RTT和每字节读取耗时，按两者估算预读收益：RTT足够小时关闭预读（block size退化为请求长度），RTT较大时才启用并调节block size。本地USB场景的主要加速来自STOPPED快照和RUN批量下发断点，而不是RAM预读。

### 4.2 Flash

- ELF 中已有的 code/rodata 应由 GDB 或 Soul Agent 本地读取，不经过 SIDP。
- 只有目标上的实际 Flash 可能与 ELF 不同时才读取目标。
- 读回的 Flash 数据可在会话中长期缓存，直到reset/reattach。
- SIDP v1调试会话不支持Flash写；GDB load/vFlash请求应明确提示用户结束调试并使用Soul Injector既有烧录流程。

### 4.3 MMIO

- MMIO 绝不自动预读。
- MMIO 绝不缓存。
- 每次请求显式携带1/2/4字节access width，且只执行一次同宽、自然对齐的目标总线访问。
- Soul Injector不得把sub-word访问扩大为word，也不得执行MMIO read-modify-write；目标debug port不支持该宽度时返回unsupported。
- 原因是外设读取可能清除状态位、弹出 FIFO 或触发其他副作用。

### 4.4 DMA 区域

即使 CPU halt，DMA 也可能修改 RAM（见第2.1节）。Soul Agent 允许 target configuration 将特定 RAM region 标记为 volatile，不对其做自动缓存。

仅靠target configuration的region标记不够：DMA buffer通常分散在普通SRAM的 `.bss` 中，与普通变量相邻，除非固件把它们放进独立section（STM32H7等较常见，其他平台少见），memory map层面无法区分。Soul Agent应合并以下来源得到volatile范围，任一来源命中即按volatile处理（精确读取、不缓存、不预读，预读block也不得跨入该范围）：

1. Attach response中带 `SIDP_MEM_VOLATILE` 的region（来自target YAML）。
2. ELF section：Soul Agent持有ELF时，按名称规则自动标记，例如 `.dma*`、`.noncacheable`、`RAM_D2` 等；规则可由用户配置。
3. 用户命令：例如 `monitor volatile add <addr> <len>` / `monitor volatile clear`，用于用户已知的个别buffer。
4. GDB memory region属性：GDB端对某region设置 `nocache` 时，Soul Agent同样不缓存该范围。

另外提供逃生开关 `monitor cache off|on`，以及按region关闭缓存的方式，供用户直接调试DMA时使用。即使上述标记有遗漏，第2.1节的短TTL和写前新鲜读取仍限制了陈旧数据的影响范围。

## 5. 缓存命中决策

```text
GDB READ_MEMORY
       |
       v
目标是否HALTED？ ----否----> SIDP真实读取或拒绝
       |是
       v
是否MMIO/volatile？ --是----> SIDP精确读取，不缓存
       |否                    （volatile来源见4.4；cache off时同样走此路径）
       v
当前stop_id缓存是否完全覆盖，且block未超过TTL？
       |                    |
      是                    否
       |                    |
       v                    v
本地回答             按实测RTT决定是否对齐扩大读取 -> 缓存 -> 回答
```

WRITE_MEMORY不走上图：先按第2.1节对相交且超过写前阈值的block做新鲜读取，然后写入目标，再更新或失效相交的缓存block。

## 6. 断点管理

Soul Agent 在 Attach 时获得 FPB/DWT 数量和 memory map。

GDB `Z/z` 只修改 Soul Agent 的 `DesiredBreakpointSet`：

```text
DesiredBreakpointSet {
    id
    address
    requested kind
    selected hardware slot or software method
}
```

分配规则：

- Flash 断点使用 FPB。
- RAM 断点优先使用 BKPT 软件断点。
- 建议保留1个 FPB slot给 `next`/run-to 等临时断点。
- 资源不足时 Soul Agent 当地拒绝，不向 GDB 假报成功。
- RUN 一次携带完整断点/watchpoint 集合，Soul Injector 以替换语义应用。
- Soul Agent同时维护期望的vector catch mask，并在每次RUN中完整下发；IDE/GDB的catch reset、hardfault等设置只修改该本地mask。

## 7. RTOS Thread Awareness 所在层级

RTOS awareness 完全在 Soul Agent 端，SIDP 和 Soul Injector 不知道 RTOS 类型。

```text
GDB thread packet
       |
       v
Soul Agent RTOS provider
       |
       +-- 向GDB查询关键符号，或本地解析ELF
       +-- 通过SIDP READ_MEMORY读取TCB/list/stack
       +-- 按RTOS port规则恢复非当前任务寄存器
       +-- 将RTOS task映射为GDB thread ID
```

### 7.1 Provider 接口

概念接口：

```cpp
class RtosProvider {
public:
    virtual bool probe(SymbolLookup&, TargetMemory&) = 0;
    virtual std::vector<ThreadInfo> list_threads(StopCache&) = 0;
    virtual RegisterSet read_thread_registers(
        uint64_t thread_id,
        StopCache&
    ) = 0;
};
```

建议的provider：

```text
FreeRtosCortexMProvider   当前优先
ZephyrCortexMProvider    后续
ThreadXCortexMProvider   后续
FreeRtosRiscvProvider    RISC-V阶段
```

### 7.2 符号来源

Soul Agent 可选两种方式：

1. Soul Agent 获得当前 ELF 路径，自己读取符号表。
2. Soul Agent 通过 GDB RSP `qSymbol` 向 GDB 请求 `pxCurrentTCB`、任务链表等符号地址。

第一版建议支持 `qSymbol`，避免 Soul Agent 必须自己实现完整 DWARF parser。仅解析 ELF symbol table 也是可接受的简化方案。

### 7.3 GDB RSP 映射

Soul Agent 至少需处理：

- `qfThreadInfo` / `qsThreadInfo`：枚举RTOS任务。
- `qThreadExtraInfo`：返回任务名称和状态。
- `Hg`：选择寄存器查询的任务。
- `Hc`：选择continue任务；all-stop第一版可简化处理。
- `T`：检查thread ID是否仍有效。
- `qC`：当前执行任务。
- `D`：映射为带明确final action的SIDP DETACH。
- `k`：按用户配置执行RESET_RUN后DETACH，或仅DETACH。
- `R` / `vRun`：按前端期望映射为RESET_HALT或RESET_RUN；RESET_HALT后重新下发完整断点集合。
- `vFlashErase` / `vFlashWrite` / `vFlashDone`：v1明确拒绝并提示改用Soul Injector烧录流程。

当前正在 CPU 上执行的RTOS任务使用 STOPPED 中的真实核心寄存器。其他任务的寄存器从其保存栈帧中恢复；具体布局由 RTOS provider + CPU profile 决定。

### 7.4 RTOS 缓存

- 任务列表仅在目标 halted 时解析。
- 解析结果与 `stop_id` 绑定。
- 同一 stop_id 内IDE重复请求thread list时直接使用缓存。
- resume/reset/write-memory 可使RTOS列表缓存失效。
- TCB链表需逐指针遍历时仍可能有依赖RTT；得到各TCB地址后，可使用可选 `READ_MEMORY_VECTOR` 批量读取各TCB和任务栈顶。

## 8. 延迟目标

在200ms RTT环境下，建议以下作为验收目标：

- STOPPED 到达 Soul Agent 后，GDB的 `?` 和 `g` 不产生新 SIDP request。
- 初始backtrace需要的栈数据尽量命中 STOPPED 快照。
- 同一地址block在TTL内的重复内存读取不产生新 SIDP request（volatile范围除外，见4.4）。
- 多个GDB断点仅在 RUN 时产生一个 SIDP request。
- 无FPU目标的初始停止快照尽量保持在1KiB以内；带FPU目标允许约1.2KiB，不因“可能有用”而预读大块RAM。

## 9. UART/RTT 日志处理

Soul Agent 将每个 `sidp_log_channel_t` 映射为独立字节流：

```text
SIDP_KIND_LOG_STREAM
        |
        v
channel demultiplexer
        |
        +-- TARGET_UART0 -> terminal/file/IDE console
        +-- RTT_UP_0     -> terminal/file/IDE console
        +-- RTT_UP_1     -> optional sink
```

规则：

- `buf[]` 是原始字节，Soul Agent 必须能保存非UTF-8和包含0的数据。
- 按通道维护有界ring buffer，默认不建议超过64KiB/通道。
- 队列满时丢弃最旧日志或新日志，策略可配置，但不得延迟SIDP Response/STOPPED。
- Soul Agent 可以在接收时附加本地时间戳，但不能将其当作目标精确产生时间。
- 默认将日志输出到Soul Agent自身的独立console/file API，不要默认转换为GDB RSP `O` packet，避免大量日志占用GDB控制链路。
- 用户选择RTT时，Soul Agent从ELF解析 `_SEGGER_RTT` 地址，再使用 `SIDP_OP_SET_LOG_STREAM` 配置Soul Injector。
- 用户选择UART时，Soul Agent下发波特率，或0表示使用Soul Injector target configuration中的默认值。
