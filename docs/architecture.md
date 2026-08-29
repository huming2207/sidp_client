# 远程调试总体架构

## 1. 系统组成

```text
+-------------+       +------------------+       +-------------------+
| GDB / CLion | <---> | Soul Agent       | <---> | Soul Interconnect |
+-------------+  RSP  | RSP Server       | SIDP  | WebSocket relay   |
                      +------------------+       +---------+---------+
                                                         |
                                                         | SIDP
                                                         v
                                               +-------------------+
                                               | Soul Injector     |
                                               +---------+---------+
                                                         |
                                                         | SWD
                                                         v
                                               +-------------------+
                                               | Target MCU        |
                                               +-------------------+
```

局域网模式只是绕过 Soul Interconnect：

```text
GDB <-> Soul Agent <---- local WebSocket/SIDP ----> Soul Injector <-> SWD target
```

SIDP 在两种网络模式下保持一致。

## 2. 责任边界

### 2.1 Soul Agent

Soul Agent 是运行在开发者（用户）电脑上的App，负责所有需要较强计算能力、频繁迭代或理解高层调试语义的工作：

- 监听 `127.0.0.1:<port>` 并实现 GDB RSP server。
- 将 GDB 寄存器编号映射到 SIDP 寄存器 ID。
- 保存用户期望的断点/watchpoint 集合。
- 保存期望的vector catch mask，并在每次RUN完整下发。
- 根据 ELF/DWARF 解析符号、变量、源码行和调用栈。
- 缓存目标停止现场和已读内存。
- 通过可插拔 RTOS provider 将目标 RAM 中的 TCB/任务栈映射为 GDB thread。
- 接收 SIDP log stream，将目标 UART/RTT 日志输出到文件、终端或IDE。
- 拒绝v1调试会话中的GDB Flash load，并引导用户调用独立烧录流程。
- 处理 IDE/GDB 兼容性，避免因 RSP 行为变化频繁升级远程设备固件。

### 2.2 Soul Injector

Soul Injector 是实际的调试器硬件。其固件负责以下机器级操作：

- 初始化 SWD/Debug Port。
- 读写目标内存和核心寄存器。
- 配置 FPB 硬件断点、RAM 软件断点和 DWT watchpoint。
- halt、continue、single-step、system/nRST reset-halt和reset-run。
- 从本地target YAML选择目标定义，校验CPU/芯片probe并返回debug memory map。
- 目标运行时在本地 poll DHCSR 或对应架构的调试状态。
- 目标停止后立即采集寄存器和小块栈快照。
- 从新版硬件的额外UART或通过RTT采集目标日志，以低优先级SIDP log stream上报。
- 将 SWD WAIT/FAULT、超时和目标丢失映射为稳定的 SIDP 错误。

Soul Injector 不负责：

- GDB RSP 解码。
- Microsoft DAP。
- ELF/DWARF。
- C/C++ 表达式求值。
- RTOS 任务列表或 TCB 布局。
- 源码文件和行号。

### 2.3 Soul Interconnect

Soul Interconnect 是未来的云端中继服务，只负责鉴权后的会话匹配和二进制转发：

- Soul Injector 和 Soul Agent 均主动建立 WSS 连接。
- 一个设备同一时间最多存在一个控制会话。
- 云端不解析 GDB RSP，也不执行调试逻辑。
- 云端应限制单帧大小、缓冲队列大小和会话寿命。

## 3. Soul Injector 任务模型

SWD 底层只允许一个固定在指定 CPU core 上的任务访问。

```text
WebSocket RX task
        |
        v
request queue
        |
        v
Pinned Debug/SWD task
        |
        +-- 处理SIDP request
        +-- RUNNING时poll目标状态
        +-- HALTED时采集快照
        |
        v
response/event queue
        |
        v
WebSocket TX task
```

不允许 WebSocket callback、UI task 或离线烧录 FSM 直接并发调用 `swd_*()`。烧录会话和调试会话也必须互斥。

## 4. 典型 continue/断点流程

```text
GDB              Soul Agent                Soul Injector               Target
 |                 |                    |                    |
 | Z/z breakpoints |                    |                    |
 +---------------->|                    |                    |
 |<----------------+                    |                    |
 | Agent本地记录/校验                  |                    |
 |                 |                    |                    |
 | continue        |                    |                    |
 +---------------->| RUN+完整断点集合      |                    |
 |                 +------------------->|                    |
 |                 |                    | 配置FPB/DWT       |
 |                 |                    +------------------->|
 |                 |                    | resume             |
 |                 |                    +------------------->|
 |                 | RUN Response (OK)  |                    |
 |                 |<-------------------+                    |
 |                 | invalidate StopCache                    |
 |                 |                    |                    | RUNNING
 |                 |                    | poll debug state   |
 |                 |                    +------------------->|
 |                 |                    |                    |
 |                 |                    |                    | breakpoint
 |                 |                    |                    | HALTED
 |                 |                    | poll -> halted     |
 |                 |                    | read regs + stack  |
 |                 | STOPPED snapshot   |                    |
 |                 |<-------------------+                    |
 | stop reply      |                    |                    |
 |<----------------+                    |                    |
 | read regs/stack |                    |                    |
 +---------------->|                    |                    |
 |<----------------+                    |                    |
 | Agent本地缓存命中，无WAN RPC             |                    |
```

RUN Response只确认Soul Injector已经完成断点配置并成功发出第一次resume/step，不等待目标停止；GDB的continue命令仍由Soul Agent保持等待，直到STOPPED Event到达后才返回stop reply。即使目标在resume后立即再次halt，Soul Injector也必须先发送RUN Response，再发送缓存的STOPPED。

HALT和RESET_HALT采用同步完成语义：Soul Injector在本地确认目标halt并准备好快照后，先发送成功Response，再发送STOPPED Event。RESET_HALT必须由Soul Agent显式选择SWD system reset或Soul Injector驱动nRST引脚的hardware reset；nRST引脚与时序来自本地YAML。无对应RUN的自发STOPPED同样合法，例如fault、lockup或vector catch导致的停止。

单一WebSocket上的Response和Event由同一个有序TX队列发送。与控制Request有因果关系的Event必须排在对应Response后；不相关的异步Event仍可穿插在其他Request处理期间，因此Soul Agent始终按消息kind和request ID分派。

## 5. 断线策略

第一版使用简单规则：

1. WebSocket 连接就是 SIDP 会话。
2. 连接断开后旧会话立即作废，不重放未确认的写操作。
3. v1没有lease、续租或断线后保留旧会话。Soul Injector立即执行本地YAML中的 `debug.disconnect_action`；默认安全策略是halt目标、恢复RAM软件断点并保持halt。
4. 如果YAML明确选择resume，Soul Injector也必须先halt并清理所有软件/硬件断点，再resume；不得让带patch的目标继续运行。
5. 重连后 Soul Agent 必须重新attach、读取目标能力并重新下发断点集合。

## 6. 连接鉴权

局域网直连不等于可信连接。任何能够建立SIDP控制会话的客户端都可以halt、reset或修改目标RAM，因此Soul Injector不得提供无鉴权的明文WebSocket控制端口。

v1采用WebSocket建立前鉴权，不在每个SIDP frame重复携带凭据：

1. 首次绑定通过物理确认或设备的一次性配对码完成，Soul Agent保存设备身份和长期随机token。
2. 后续局域网连接使用WSS，并在HTTP Upgrade阶段提交token；Soul Agent固定/pin设备证书或公钥，避免token被同网段窃听或中间人转发。
3. Soul Injector在鉴权成功前不解析SIDP，不允许ATTACH，并限制失败尝试频率。
4. 一个Soul Injector同时只允许一个已鉴权控制会话。新连接不能抢占现有调试会话。
5. 通过Soul Interconnect时，Soul Agent和Soul Injector分别认证云端，会话匹配仍需验证设备归属；Soul Interconnect只转发已授权的SIDP字节。

设备重置配对关系必须要求本地物理操作或已认证的管理流程，不能通过未认证网络请求完成。
