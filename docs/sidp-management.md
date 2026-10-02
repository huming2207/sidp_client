# SIDP 管理服务（opcode 0x0100+）

管理服务让 Soul Agent 通过同一条 SIDP 连接查询设备、上传文件和控制烧录任务（job）。
它沿用 SIDP v1 的 12 字节消息头、CRC、`request_id` 规则、单个未完成 Request
和"超时即断开、不重放"的规则（见 `sidp-protocol.md` 第 3 节）。

## 1. 载荷格式

- Request/Response 的 payload 是 `proto/manage.proto` 中的 protobuf 消息。
  Response 仍以 4 字节 `status` 前缀开头，status 非 OK 时没有 protobuf 载荷。
- 例外：`ASSET_WRITE` 的 payload 是 `asset_write_request_t`（4 字节 offset）
  后接原始数据，与 `WRITE_MEMORY` 相同，不经过 protobuf。
- 设备侧限制见 `proto/manage.options`。字段缺省值即 protobuf 默认值。
- 调试 opcode（< 0x0100）在设备接入 SWD backend 之前一律返回 `STATUS_UNSUPPORTED`。

## 2. Opcode

| Opcode | 名称 | Request | Response |
|--------|------|---------|----------|
| 0x0100 | DEVICE_INFO | 空 | `DeviceInfo` |
| 0x0110 | ASSET_BEGIN | `AssetBeginRequest` | `AssetBeginResponse` |
| 0x0111 | ASSET_WRITE | offset + 数据 | 空 |
| 0x0112 | ASSET_COMMIT | 空 | 空 |
| 0x0120 | JOB_SET | `JobSetRequest` | 空 |
| 0x0121 | JOB_GET | 空 | `JobStatus` |
| 0x0122 | JOB_RUN_ONCE | 空 | `JobRunOnceResponse` |
| 0x0123 | JOB_CANCEL | `JobCancelRequest` | 空 |

## 3. 状态码

| status | 含义 |
|--------|------|
| `STATUS_BUSY` | 正在烧录；该请求会替换 job 或文件，或要求再启动一次烧录。 |
| `STATUS_INVALID_ARGUMENT` | 参数、文件名、offset、大小或哈希不对，或找不到对应的文件、暂存 job 或镜像。 |
| `STATUS_ERROR` | 其它失败（文件系统、job 解码或校验失败、空间不足等），详情见设备日志。 |
| `STATUS_UNSUPPORTED` | 未知 opcode 或调试 opcode。 |

## 4. 文件上传（ASSET_*）

文件存放在设备 `/data/<name>`。`name` 为 1–31 个 `[A-Za-z0-9._-]` 字符，
不能以 `.` 开头，不能以 `.part` 或 `.sha256` 结尾。`job.pb` 只能由 `JOB_SET` 替换。

1. `ASSET_BEGIN {name, size, sha256}`：
   - 设备已有同名、同大小、同哈希的文件：`present = true`，无需上传或提交。
   - 正在上传同一文件（同名、同大小、同哈希）：从 `offset` 继续，用于断线重连。
   - 否则放弃之前未完成的上传，从 `offset = 0` 开始。空间不足时返回错误。
2. `ASSET_WRITE {offset, data}`：`offset` 必须等于已接收字节数，按顺序发送。
3. `ASSET_COMMIT`：校验 SHA-256，替换 `/data/<name>`，并写入 `/data/<name>.sha256`（十六进制）。
   烧录进行中返回 `STATUS_BUSY`。

同一时间只有一个上传。上传状态只在 RAM 中，设备复位后需重新上传。
上传过程中可以烧录；只有提交需要等待烧录结束。

## 5. Job

设备只保存一个 job：`/data/job.pb`。新 job 以文件名 `job.pb.new` 上传，然后：

- `JOB_SET {sha256, trigger}`：`sha256` 必须与已提交的 `job.pb.new` 一致。
  设备解码、校验 job，并检查 job 固定的每个镜像（`CortexM.firmware_sha256`、
  `Esp32Image.sha256`）都已上传且哈希一致，然后才替换当前 job。
  `sha256` 等于当前 job 时只更新 `trigger`。trigger 保存在 NVS，重启后保留。
  - `TRIGGER_MANUAL`：只在 `JOB_RUN_ONCE` 时烧录。
  - `TRIGGER_AUTO_ON_DETECT`：另外每次插入目标时自动烧录。
- `JOB_GET`：当前 job 的名称和哈希、trigger、是否正在烧录、上次结果
  （`run_id`、结果、失败阶段、耗时）。`run_id = 0` 表示开机后还没有烧录过。
- `JOB_RUN_ONCE`：立即返回新的 `run_id`，烧录在后台进行。Agent 轮询 `JOB_GET`
  直到 `last_result.run_id` 等于该值。已在烧录时返回 `STATUS_BUSY`。
- `JOB_CANCEL {run_id}`：请求停止烧录（`run_id = 0` 表示当前任何一次）。
  设备在阶段之间检查取消；目标可能只烧录了一部分。没有匹配的烧录时也返回 OK。
  取消不改变 trigger。

每次烧录开始时设备都会重新比对镜像哈希，镜像在 `JOB_SET` 之后被替换时该次烧录在
LOAD 阶段失败。`run_id` 和上次结果不跨重启保存。
