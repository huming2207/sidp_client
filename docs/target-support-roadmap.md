# 目标架构支持路线

## 1. 设计思路

SIDP 只定义通用调试操作：

```text
attach
read/write memory
read/write registers
halt/run/step/reset
breakpoint/watchpoint set
stopped event
```

架构差异由两端的 profile/backend 处理：

```text
Soul Agent                         Soul Injector

GDB register mapping              target backend
target.xml generation             register access
stop reason mapping               run control
breakpoint allocation policy      breakpoint programming
RTOS saved-frame decoding         halt polling
```

## 2. SIDP v1 支持与验证状态

| 目标 | SIDP profile | v1要求 | 当前实机覆盖 |
|---|---|---|---|
| Cortex-M0 | `SIDP_PROFILE_ARMV6M` | 必须支持 | 暂无实板，未经实机验证 |
| Cortex-M3 | `SIDP_PROFILE_ARMV7M` | 必须支持 | 暂无实板，未经实机验证 |
| Cortex-M4（无FPU） | `SIDP_PROFILE_ARMV7EM`，无 `SIDP_CAP_FPU` | 必须支持 | 取决于当前实板配置，需单独记录 |
| Cortex-M4（有FPU） | `SIDP_PROFILE_ARMV7EM` + `SIDP_CAP_FPU` | 必须支持 | 取决于当前实板配置，需单独记录 |

“v1必须支持”和“已经实机验证”必须分开记录。在获得M0/M3硬件前，可以使用寄存器模型单元测试、SIDP回放测试和mock SWD backend，但不得将其标注为实机验证通过。v1正式发布前应补充至少一块M0和一块M3目标板。

ARMv6-M profile在结构上也可覆盖Cortex-M0+，但M0+不作为当前v1发布验收必须项，除非后续明确加入测试矩阵。

## 3. Cortex-M0

SIDP profile：

```text
architecture = SIDP_ARCH_ARM_M
profile      = SIDP_PROFILE_ARMV6M
address_width = 32
```

功能范围：

- SWD attach，包括 connect-under-reset。
- halt、continue、single-step、reset-halt。
- 分别验证SWD system reset和nRST引脚hardware reset；没有连接nRST的Soul Injector硬件不得声明后者能力。
- R0-R12、SP、LR、PC、xPSR、MSP、PSP、PRIMASK、CONTROL。
- 批量RAM/Flash读写。
- 运行时poll DHCSR。
- 动态读取 FPB/DWT 能力，不根据芯片名硬编码数量。
- Flash使用FPB断点，RAM可使用BKPT软件断点。
- 硬件支持时使用DWT watchpoint。
- 单核 all-stop 模式。

不要假定所有 Cortex-M0 都有相同的FPB/DWT数量。

## 4. Cortex-M3

SIDP profile：

```text
architecture = SIDP_ARCH_ARM_M
profile      = SIDP_PROFILE_ARMV7M
address_width = 32
```

在Cortex-M0基础上增加：

- BASEPRI、FAULTMASK。
- CFSR、HFSR、MMFAR、BFAR等fault diagnosis。
- 更完整的vector catch。
- ARMv7-M寄存器映射和target description。
- FreeRTOS Cortex-M3 saved context解码由Soul Agent provider实现。

M3不应通过M4 profile“顺便兼容”；Soul Agent必须根据 `SIDP_PROFILE_ARMV7M` 生成不包含M4 FPU扩展的正确GDB寄存器描述。

## 5. Cortex-M4

SIDP profile：

```text
architecture = SIDP_ARCH_ARM_M
profile      = SIDP_PROFILE_ARMV7EM
address_width = 32
```

在ARMv7-M/Cortex-M3基础上增加：

- BASEPRI、FAULTMASK。
- CFSR、HFSR、MMFAR、BFAR等fault diagnosis。
- 更完整的vector catch。
- 运行时检测可选FPU；`SIDP_PROFILE_ARMV7EM` 本身绝不代表存在FPU。
- 无FPU时不得设置 `SIDP_CAP_FPU`，Soul Agent生成的GDB target description不得包含S0-S31/FPSCR。
- 有FPU时设置 `SIDP_CAP_FPU`；每个STOPPED主动上传S0-S31/FPSCR或unavailable占位，使GDB首次 `g` 不增加跨境RTT。
- FreeRTOS Cortex-M4 saved context解码由Soul Agent provider实现。
- 当前手头实板用于首先打通Soul Agent、SIDP、Soul Injector和SWD的端到端链路。

测试报告必须注明当前M4实板是否实际带FPU。只有一块M4板不能同时证明“有FPU”和“无FPU”两条路径；缺少的路径至少应先用mock attach response、寄存器映射单元测试和SIDP回放覆盖，之后再补实板。

## 6. 后续：ESP32 Panic GDB Stub

该方向不是SIDP v1验收项。它让Soul Injector通过附加UART连接已经崩溃并进入ESP-IDF panic GDB Stub的目标，再把目标Stub提供的寄存器和内存读取转换为SIDP语义操作。完整设计见 [sidp-addition-esp32-gdbstub.md](sidp-addition-esp32-gdbstub.md)。

这条路径与完整ESP32 JTAG/USB-JTAG硬件调试不同：第一阶段仅提供只读post-mortem检查，不实现断点、single-step、寄存器/内存写入、resume或多核run control。

需要增加：

- `SIDP_ARCH_XTENSA` 和受支持ESP32 Xtensa profile；RISC-V ESP32继续复用RISC-V架构，但必须标识具体目标。
- `SIDP_CAP_POST_MORTEM` 和 `SIDP_CAP_TARGET_GDB_STUB`。
- 独立的 `SIDP_OP_ATTACH_GDB_STUB`，不复用SWD attach参数。
- Soul Injector中的小型UART GDB RSP client。
- UART普通日志模式与GDB Stub独占模式之间的可靠切换。
- 目标侧寄存器和栈预读取，再以一个SIDP STOPPED事件上传。

Soul Agent仍负责GDB target description、ELF/DWARF、调用栈和ESP-IDF/FreeRTOS task解析。透明RSP tunnel只作为早期bring-up工具，不作为跨境远程调试的最终数据路径。

## 7. 后续：Cortex-M23/M33

Profile：

```text
Cortex-M23 -> SIDP_PROFILE_ARMV8M_BASE
Cortex-M33 -> SIDP_PROFILE_ARMV8M_MAIN
```

主要新问题：

- Armv8-M Security Extension / TrustZone。
- Secure/Non-secure debug authentication。
- Secure/Non-secure banked stack pointer和特殊寄存器。
- 设备可能禁止Secure state debug。
- CoreSight/FPB/DWT版本和能力不能由CPU名称推断，必须运行时发现。

SIDP 不需要修改寄存器消息格式；新 profile 只需定义新 register ID。

v1 时即使 struct 中保留 `core_id`，也不代表已支持multi-core。TrustZone state 切换和多核作为后续能力单独设计。

## 8. 后续：Cortex-M55/M85

Profile：

```text
SIDP_PROFILE_ARMV81M_MAIN
```

需评估：

- MVE/Helium 寄存器和 GDB target description。
- 可选FPU和更大的寄存器现场。
- Armv8.1-M debug/security extension。
- 更复杂的cache、TCM和memory map。
- 多核SoC中的core selection和全局halt策略。

SIDP STOPPED使用可变 `sidp_register_value_t` 而不是固定Cortex-M struct，正是为了避免MVE/FPU扩展导致协议推翻。

## 9. 后续：RISC-V

Profile：

```text
architecture = SIDP_ARCH_RISCV
profile      = SIDP_PROFILE_RV32 / SIDP_PROFILE_RV64
address_width = 32 / 64
```

Soul Injector侧需新增RISC-V Debug backend，实现：

- JTAG DTM/DMI 或目标所需的debug transport。
- Debug Module attach/authentication。
- abstract command / system bus access。
- GPR/PC/CSR读写。
- halt/resume/single-step。
- trigger module breakpoint/watchpoint。
- halt reason和exception cause读取。

Soul Agent侧需新增：

- RISC-V GDB register number映射。
- RISC-V target description XML。
- trigger数量和类型的断点分配策略。
- RISC-V ABI/DWARF unwind交由GDB。
- FreeRTOS/Zephyr RISC-V saved-frame provider。

SIDP层面仍然使用：

```text
READ_MEMORY
READ_REGISTERS
WRITE_REGISTERS
RUN
HALT
RESET_HALT
STOPPED
```

只有 `architecture/profile`、register ID和stop reason detail的解码发生变化。

## 10. 不应提前实现的内容

为保持Cortex-M0/M3/M4版本简单，下列功能只保留概念上的扩展余地，不在v1实现：

- 多核/multi-hart run control。
- Non-stop debugging。
- TrustZone secure/non-secure双世界同时调试。
- MVE/trace/SWO/ETM。
- RISC-V arbitrary CSR descriptor protocol。
- 动态XML或JSON schema下发到Soul Injector。

新架构应先尝试通过新profile、Soul Agent mapping和Soul Injector固件中的target backend接入现有SIDP。只有现有操作模型无法表达必需语义时，才增加新opcode或升级major version。

## 11. 当前实施计划（2026-09-10 调整）

前面的架构清单是目标范围，不是开工前必须全部实现的依赖树。先完成一块现有M4板的USB调试闭环，再增加功能。M0/M3仍在v1目标范围内，但不阻塞M4开发里程碑；没有实机记录就不能声称验证通过。

### 当前代码实际到了哪里

- 已有wire定义、CRC、CDC/SLIP和WebSocket transport，以及带mock backend的session测试。
- session已实现基础控制、内存/寄存器访问、快照、软件断点shadow和step-over；这些是host模型覆盖，不是硬件验证。
- 本组件只有 `sidp::target_backend_t` 接口，没有真实SWD实现。主工程的 `swd_cortexm_backend` 是烧录接口的实现，不能直接作为SIDP backend使用；主工程main目前也没有SIDP调用点。
- `READ_MEMORY_VECTOR`、日志配置和ESP32 GDB Stub目前没有session handler，不得声明对应能力。
- WebSocket实现是主动连接的client，不等于已经有局域网WSS server、配对流程或Soul Interconnect。

### 下一步：USB + 一块M4 + 最小控制闭环

1. 在主工程添加一个直接调用现有 `swd_*` 的SIDP backend adapter。先完成attach、halt/poll、寄存器读取、RAM/Flash读取、continue和detach；只报告真正可用的能力。未实现的可选操作返回unsupported，空断点/watchpoint集合与空vector catch配置应能成功清理。
2. 明确由一个固定debug任务拥有SWD，和现有烧录流程互斥。先做简单的独占切换，不引入通用调度框架或多backend注册系统。
3. 接入现有CDC/SLIP，用一个小型主机脚本发送真实SIDP帧，验证attach → STOPPED → read → continue → halt → detach。此时不要求完整Soul Agent UI、GDB或云服务。
4. 每次request/poll返回后检查session和transport的 `needs_disconnect()`；置位则关闭连接并在debug任务调用 `handle_disconnect()`，确认返回true。新连接使用新session，旧RX/TX数据由transport丢弃。

完成条件：记录实际目标型号、FPU情况、接线和使用的固件revision；连续执行上述流程，并验证拔线后目标清理。不要把mock的两条寄存器记录当作完整M4快照的证明。

transport现在要求显式 `begin_session()`，断线、队列溢出和异步发送失败会锁定当前会话；新物理连接、旧TX结束和旧RX buffer归还后才能接受新会话。控制和日志分队列，控制入队不等待空间。应用接线步骤见 [组件集成](integration.md)。尤其要转发TinyUSB设备事件，不能只靠轮询 `is_open()` 检测快速拔插。

session断线清理固定使用keep-halted，尚未接入YAML中的resume策略。`handle_disconnect()` 返回false时必须保留session和SWD所有权，重试清理成功后才允许新调试/烧录会话；不能丢失尚未恢复的原始指令记录。

### 然后：最小GDB体验

- 实现RAM/寄存器写入、single-step和FPB硬件断点，接Soul Agent的最小RSP子集。
- 先只使用硬件断点，资源用尽明确报错；现有RAM软件断点实现保留测试，但在实机验证shadow/step-over/失败清理前不要声明能力。
- 增加实际可用的reset方法、完整寄存器和小块栈快照；已有快照代码可以复用。
- GDB负责ELF/DWARF和栈展开，Agent先只缓存当前STOPPED，不做自适应预读、RTOS provider或ELF/DWARF解析框架。

完成条件：能在一个实际程序中打断点、查看寄存器/栈、单步和继续；断线/错误路径不会让带未知patch的目标继续运行。

### 再按需求扩展

1. 验证软件断点、DWT、其余reset路径以及M4有/无FPU差异。
2. 需要远程使用时增加鉴权WSS和连接生命周期测试，然后才接云中继。网络调试启用前必须完成鉴权，不能把它推迟到上线以后。
3. M3/M0实板补齐v1验证；使用实际差异指导公共代码提取。
4. 测量真实RTT和SWD吞吐后，再决定memory block cache、vector read和预读是否值得实现。
5. UART/RTT、FreeRTOS awareness作为独立增量；ESP32 panic stub、Armv8-M和RISC-V继续留在未来设计中。

### 保留什么，简化什么

保留12字节头、CRC、stop_id、单请求顺序、Response-before-Event和严格内存边界。这些都已有用途和代码，删除它们会引入兼容性或调试正确性成本。

不为未来架构重写现有Cortex-M session，不提前新增opcode。软件断点复杂是因为patch需要恢复，不适合靠删错误处理来“简化”；首个backend不声明该能力即可避开硬件bring-up负担。

v1请求超时直接结束连接并重新attach，不维护迟到response表，也不自动重试或重放操作。这样与“连接就是会话”的规则一致；后续有实际需求时再设计恢复机制。
