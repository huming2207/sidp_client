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
| 0x0101 | SET_TIME | `SetTimeRequest` | 空 |
| 0x0110 | ASSET_BEGIN | `AssetBeginRequest` | `AssetBeginResponse` |
| 0x0111 | ASSET_WRITE | offset + 数据 | 空 |
| 0x0112 | ASSET_COMMIT | 空 | 空 |
| 0x0120 | JOB_SET | `JobSetRequest` | 空 |
| 0x0121 | JOB_GET | 空 | `JobStatus` |
| 0x0122 | JOB_RUN_ONCE | 空 | `JobRunOnceResponse` |
| 0x0123 | JOB_CANCEL | `JobCancelRequest` | 空 |
| 0x0130 | LOG_READ | `LogReadRequest` | `LogReadResponse` |
| 0x0131 | LOG_ACK | `LogAckRequest` | 空 |

## 3. 状态码

| status | 含义 |
|--------|------|
| `STATUS_BUSY` | 正在烧录；该请求会替换 job 或文件，或要求再启动一次烧录。 |
| `STATUS_INVALID_ARGUMENT` | 参数、文件名、offset、大小或哈希不对，或找不到对应的文件、暂存 job 或镜像。 |
| `STATUS_ERROR` | 其它失败（文件系统、job 解码或校验失败、空间不足、生产日志写入失败等），详情见设备日志。 |
| `STATUS_UNSUPPORTED` | 未知 opcode 或调试 opcode。 |
| `STATUS_LOG_FULL` | 生产日志已满，需先收集（`LOG_READ` + `LOG_ACK`）才能再烧录。 |

## 4. 文件上传（ASSET_*）

文件存放在设备 `/data/<name>`。`name` 为 1–31 个 `[a-z0-9._-]` 字符，不能以 `.` 开头或结尾，
不能以 `.part` 结尾。只允许小写，因为 FAT 不区分大小写并忽略结尾的点，其它写法会指向
同一个文件。`job.pb` 只能由 `JOB_SET` 替换。设备不另存哈希：需要信任某个文件时
（`ASSET_BEGIN` 判断是否已有、`JOB_SET`、每次烧录开始时）都重新计算文件内容的 SHA-256。

1. `ASSET_BEGIN {name, size, sha256}`：
   - 设备已有同名、同大小、同哈希的文件：`present = true`，无需上传或提交。
   - 正在上传同一文件（同名、同大小、同哈希）：从 `offset` 继续，用于断线重连。
   - 否则放弃之前未完成的上传，从 `offset = 0` 开始。空间不足时返回错误。
2. `ASSET_WRITE {offset, data}`：`offset` 必须等于已接收字节数，按顺序发送。
3. `ASSET_COMMIT`：校验 SHA-256，替换 `/data/<name>`。烧录进行中返回 `STATUS_BUSY`。

同一时间只有一个上传。上传状态只在 RAM 中，设备复位后需重新上传；
复位后设备删除残留的 `.part` 文件。
上传过程中可以烧录；只有提交需要等待烧录结束。

## 5. Job

设备只保存一个 job：`/data/job.pb`。新 job 以文件名 `job.pb.new` 上传，然后：

- `JOB_SET {sha256, trigger}`：`sha256` 必须与已提交的 `job.pb.new` 一致。
  设备解码、校验 job，并计算 job 固定的每个镜像（`CortexM.firmware_sha256`、
  `Esp32Image.sha256`）的哈希，全部一致后先保存 `/data/job.pb`，保存成功才切换到新 job。
  保存失败时返回错误，设备继续使用旧 job（但重启后可能没有 job），重试会再次保存。
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

每次烧录开始时设备都会重新计算镜像哈希，镜像在 `JOB_SET` 之后被替换或损坏时该次烧录在
LOAD 阶段失败，目标不会被擦除。`run_id` 和上次结果不跨重启保存。

## 6. 生产日志

设备把每次烧录的结果和每次开机记入生产日志（独立的 `log` 分区，on9ringstore），
由 host 收集。条目 ID 跨重启递增（开机计数 << 40 | 序号）。

- `DEVICE_INFO` 中的 `log_newest_id` 是最新条目，`log_acked_id` 是已收集到的条目；
  两者不等时有未收集的条目。
- `LOG_READ {after_id}`：返回 ID 大于 `after_id` 的条目，从旧到新，一帧放得下多少返回多少；
  为空表示没有更多。从 `log_acked_id` 开始读。
  - `type = LOG_ENTRY_RUN`：`record` 为 `RunRecord`（结果、trigger、job 名称和哈希、错误码）。
  - `type = LOG_ENTRY_BOOT`：`record` 为 `BootRecord`（复位原因）。
  - `type = LOG_ENTRY_UNKNOWN`：损坏（CRC 错误）、过大或未知类型的条目，`record` 为空；
    `id` 有效，照常确认，之后的条目不受影响。
- `LOG_ACK {up_to_id}`：确认收集到 `up_to_id`（含），设备之后可以覆盖这些条目。
  只能向前移动，小于等于已确认值时直接返回 OK；大于最新条目时返回
  `STATUS_INVALID_ARGUMENT`。Host 应在把条目写入自己的存储之后再确认。
- `SET_TIME {utc_ms}`：设置本次开机的时钟（每次开机只接受第一次）。时间一律为 UTC
  （Unix epoch 起的毫秒），不带时区或夏令时，只在显示时才转换为本地时间。Host 应从
  NTP 服务器取得时间，而不是用自己的系统时钟（`sidp-agent` 默认用 `pool.ntp.org`）；
  取不到时不要发送 `SET_TIME`，以免把错误的时间写进日志。
  条目的 `utc_ms` 由同一次开机的时间设置推算，所以设置之前写入的条目也有时间；
  那次开机从未设置时间时为 0，此时只有 `uptime_us`。

设备从不覆盖未确认的条目。只有日志确定放得下这次烧录的记录时才开始烧录，因此每次开始的
烧录都会被记录。日志满时（写下一条需要重用仍有未确认条目的段），`JOB_RUN_ONCE` 返回
`STATUS_LOG_FULL`，插入目标也不会自动烧录，`DEVICE_INFO.log_full` 为 true，直到 host
收集并确认；重启后依然如此。写入记录失败（文件系统错误）时，设备拒绝烧录直到重启：
`JOB_RUN_ONCE` 返回 `STATUS_ERROR`，`log_full` 同样为 true，这时收集日志无法解除。

确认的作用之一是回收时间锚点：某次开机之前的所有开机，其条目都已确认时，这些开机的
时间锚点可以被新的 `SET_TIME` 覆盖，此后再读这些已确认条目时 `utc_ms` 可能为 0。

后续计划：调试会话中目标崩溃等事件也将记入同一日志，作为新的 `LogEntryType`。
