# Soul Injector 远程调试设计文档

本目录记录 Soul Injector 作为 Cortex-M/RISC-V 远程调试器时的协议和架构设计。

SIDP v1 的目标支持范围是 Cortex-M0、Cortex-M3 和 Cortex-M4。目前手头只有 Cortex-M4 实板，因此 M4 先完成实机验证；M0/M3 仍属于v1必须支持范围，但在获得对应实板前必须标注为“未经实机验证”。协议仅保留必要扩展点，以便未来支持 Cortex-M23/M33/M55/M85 和 RISC-V。

Cortex-M4 的 FPU 是可选能力。SIDP 以 attach 时返回的 `SIDP_CAP_FPU` 区分有/无 FPU 的 M4，Soul Agent 不得仅凭 Cortex-M4 或 ARMv7E-M profile 暴露浮点寄存器。

## 产品命名

- **Soul Agent**：运行在开发者（用户）电脑上的App。
- **Soul Interconnect**：未来的云端会话匹配和中继服务。
- **Soul Injector**：实际调试器硬件。

## 文档索引

- [architecture.md](architecture.md)：总体架构、责任边界和典型调试流程。
- [sidp-protocol.md](sidp-protocol.md)：Soul Agent 与 Soul Injector 之间的极简二进制协议。
- [sidp-addition-esp32-gdbstub.md](sidp-addition-esp32-gdbstub.md)：未来通过目标UART连接ESP32 panic GDB Stub的SIDP扩展设计。
- [agent-cache-rtos.md](agent-cache-rtos.md)：Soul Agent 端缓存、预读、GDB RSP 映射和 RTOS thread awareness。
- [target-support-roadmap.md](target-support-roadmap.md)：Cortex-M0/M3/M4 v1范围、实机验证状态和未来 Cortex-M/RISC-V 扩展方式。

## 核心原则

1. Soul Agent 在开发者电脑上终止 GDB RSP，不将 RSP 字节流原样穿透到 Soul Injector。
2. SIDP 只传输读写内存、读写寄存器、运行控制和停止现场等机器级操作。
3. Soul Injector 本地 poll 目标运行状态；DHCSR 轮询绝不经过云端。
4. ELF/DWARF、源码行、变量解码、调用栈展开和 RTOS 任务解析留在 Soul Agent/GDB 侧。
5. 为降低跨境 RTT 影响，目标停止时由 Soul Injector 一次上报核心寄存器和小块栈快照，Soul Agent 在停止期间缓存读取结果。
6. 连接就是会话。第一版不实现断线后继续旧调试会话。
7. 目标 UART/RTT 日志使用低优先级 SIDP log stream，不得因日志背压阻塞调试控制。
8. 局域网控制连接同样必须经过配对和WSS鉴权；不提供无token的明文调试端口。
