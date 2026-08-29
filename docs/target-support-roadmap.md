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

## 11. 建议实现顺序

按以下阶段实施；上一阶段的验收条件没有满足前，不把后续可选能力塞进固件。

### 阶段1：锁定wire contract和host测试

1. 固定消息头、CRC、8 KiB上限、所有enum数值、Arm寄存器ID和struct size。
2. 用host单元测试覆盖正常encode/decode、截断payload、错误count、CRC失败、未知version和request timeout后的迟到Response。
3. 用mock target覆盖DETACHED/HALTED/RUNNING/LOST状态表和Response-before-Event竞态。

完成条件：Soul Agent与Soul Injector共享同一组golden binary frame，所有结构变化都会使测试失败。

### 阶段2：现有Cortex-M4实板的基础控制

1. 从本地target YAML选择目标、生成memory map，并用CPUID/可用vendor probe验证配置。
2. 完成attach、GET_STATE、寄存器读写、RAM/Flash读取和精确MMIO访问；Flash WRITE_MEMORY必须明确拒绝。
3. 完成HALT、RUN、system reset-halt、nRST reset-halt和RESET_RUN。
4. 完成vector catch mask、STOPPED原因、完整寄存器和栈快照。

完成条件：所有控制操作有确定Response时机；错误状态不会让目标意外resume。

### 阶段3：断点和清理语义

1. 完成FPB、DWT和single-step。
2. 分别验证16位Thumb及32位Thumb-2软件断点：shadow、PC规范化、内部step-over、补插和READ_MEMORY遮罩。
3. 验证temporary断点由Soul Agent从下一次完整集合删除。
4. 验证DETACH、reset和断线时恢复RAM patch并清理FPB/DWT。

完成条件：断点增删、命中、单步、detach和断线后，目标内存中都不会遗留未知BKPT。

### 阶段4：Soul Agent与延迟优化

1. 完成GDB RSP基础包、Arm寄存器映射、stop cache和memory block cache。
2. 分别测试带/不带 `SIDP_CAP_FPU` 的M4；带FPU STOPPED包含S0-S31/FPSCR或unavailable条目，首次GDB `g` 不访问WAN。
3. 映射D、k、R/vRun、monitor reset；明确拒绝GDB vFlash/load并提示使用烧录流程。
4. 完成FreeRTOS Cortex-M provider，保持所有thread awareness在Soul Agent。

完成条件：200ms RTT模拟下，首次 `?`、`g` 和普通栈backtrace主要命中本地cache。

### 阶段5：日志、鉴权和网络

1. 先完成UART log stream，再实现可抢占的低优先级RTT polling。
2. 完成首次配对、WSS、token校验、单控制会话和鉴权失败限速。
3. 完成局域网端到端测试，再接入Soul Interconnect测试跨境RTT、CRC丢帧、Request timeout和断线清理。

完成条件：日志背压不阻塞控制消息；未配对客户端无法attach或halt目标。

### 阶段6：补齐M3和M0实机

1. 抽出ARMv7-M公共层，实现Cortex-M3 profile；先用mock/回放，再补M3实板。
2. 抽出ARMv6-M差异，实现Cortex-M0 profile和受限FPB场景；先用mock/回放，再补M0实板。
3. 分别验证M0/M3/M4的寄存器表、reset方法、断点数量、vector catch和FreeRTOS saved context。

完成条件：v1发布报告明确列出三种core的实板型号与测试结果；缺少实板的profile不得标记为已验证。

### 阶段7：v1之后

先按真实需求实现ESP32 panic GDB Stub扩展；Armv8-M、完整RISC-V硬件debug和调试会话内Flash编程继续独立评估，不提前扩张v1。
