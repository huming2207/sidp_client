# SIDP agent service (opcodes 0x0300 and up)

An agent session runs JavaScript on the device against a Cortex-M target, for
an LLM agent driving sidp-agent's MCP server (`sidp-agent mcp`). Scripts run
in the device's QuickJS-NG runtime, so they can watch the target and react to
it within milliseconds, without a round trip to the host.

The service keeps SIDP's rules: the 12-byte header and CRC, one outstanding
request, no retry after a timeout. Requests and responses carry protobuf
messages from `proto/agent.proto`; responses still start with the 4-byte
status and carry no message unless it is OK. Captures are binary records,
described below.

## Ownership

`AGENT_OPEN` claims the target the way a debug `ATTACH` does, through the same
backend, and on Soul Injector the same task serves both. While a session is
open, `ATTACH` fails, and so does `AGENT_OPEN` while a debugger is attached;
programming runs are refused during either. The session ends with
`AGENT_CLOSE` or when the connection closes, and the target is then left as
`OpenRequest.session_end` says, after the session's watches and vector catch
are removed. Breakpoints a script set through the FPB are cleared too.

## Opcodes

| Opcode | Kind | Name | Request | Response or payload |
|--------|------|------|---------|---------------------|
| 0x0300 | Request | `AGENT_OPEN` | `OpenRequest` | `SessionInfo` |
| 0x0301 | Request | `AGENT_CLOSE` | empty | empty |
| 0x0302 | Request | `AGENT_INFO` | empty | `SessionInfo` |
| 0x0310 | Request | `SCRIPT_RUN` | `RunRequest` | `ScriptResult` |
| 0x0311 | Request | `SCRIPT_EVAL` | `EvalRequest` | `ScriptResult` |
| 0x0312 | Request | `SCRIPT_STOP` | empty | empty |
| 0x0313 | Request | `SCRIPT_UPLOAD` | `UploadRequest` | empty |
| 0x0320 | Request | `EVENT_READ` | `EventReadRequest` | `EventReadResponse` |
| 0x0321 | Request | `CAPTURE_READ` | `CaptureReadRequest` | `CaptureReadResponse` |
| 0x0380 | Log stream | `AGENT_EVENT` | — | `EventNotify` |

| Status | Meaning |
|--------|---------|
| `STATUS_BUSY` | `AGENT_OPEN` while a debugger, a programming run or another session has the target. |
| `STATUS_SWD_ERROR` | `AGENT_OPEN` could not connect to the target. |
| `STATUS_NEEDS_CORTEX_M` | `AGENT_OPEN` without a Cortex-M job: the job describes the target (memory map, flash algorithm). |
| `STATUS_NO_SESSION` | Anything but `AGENT_OPEN` without an open session. |
| `STATUS_NO_SCRIPTS` | The firmware can't run scripts now: no PSRAM, or the script engine failed (see the device log). |
| `STATUS_INVALID_ARGUMENT` | A malformed message, a bad upload piece, or a capture that is gone. |
| `STATUS_UNSUPPORTED` | Firmware built without scripts. |

## Permissions

Reading memory and registers, halting and resuming are always allowed.
`OpenRequest.permissions` grants the rest: `write` (memory and core register
writes, `target.call()`), `flash` (`flash.erase`, `flash.write`) and `reset`
(`target.reset()`, `reinit()`, 'reset' reflexes and handler results). The
device enforces them; a refused call throws a `TargetError` with code
`NOT_ALLOWED` in the script. The person running the host chooses them, not
the agent.

## Scripts

The session has at most one runtime, in the device's script arena.

- `SCRIPT_RUN` stops the runtime (its watches end with it), starts a new one
  and evaluates an ES module in it. A module bigger than one request goes in
  `SCRIPT_UPLOAD` pieces first (offset 0 starts over; up to
  `CONFIG_SI_JS_MAX_SCRIPT_KB`), then `RunRequest.staged`.
- `SCRIPT_EVAL` evaluates a global script that may use top-level `await`
  (`sidp_qjsc::compile_eval`, or `qjs_esp_bc.py compile --script`) in the
  runtime, starting one if none runs. Its value is the last statement's,
  as JSON: typed arrays become arrays, BigInts strings, Errors their text,
  and `undefined` is the bare word `undefined`. Values longer than 4095
  bytes are cut on a UTF-8 boundary and `truncated` is set. `var`s persist
  between evals, and the globals `target`, `flash`, `crypto`, `agent` are
  the modules.
- Both wait `timeout_ms` (0 for 5 s, at most 60 s) and answer
  `OUTCOME_PENDING` if the code is still going. It carries on, and its
  outcome comes later as a `result` event.
- `SCRIPT_STOP` stops the runtime.

A container whose code crashes the device is refused after the reboot, as
for job scripts.

## Events

The device keeps the newest 64 events in a ring; each has a sequence number
that only grows while the device runs. `EVENT_READ` returns up to 8 after
`after_seq`, oldest first; a jump in `seq` means the ones between were
overwritten. `dropped` counts the `console` and `emit` events the rate limit
(20 a second, bursts of 40) threw away this session. `AGENT_EVENT` notices
are expendable hints that `newest_seq` moved; hosts poll too.

| Kind | JSON |
|------|------|
| `console` | `{level, text}` |
| `emit` | `{name, data}` from `agent.emit()` |
| `error` | `{what, message, stack}`: an uncaught exception or unhandled rejection |
| `result` | `{token, outcome, text}`: a run or eval that finished after its timeout |
| `runtime` | `{reason}`: the runtime ended by itself (too long without yielding, out of memory...) |
| `watch` | `{watch, name, on, cause, reflex, action, handler, pc?, throttled?, note?}`, with `capture_id` |
| `stop` | `{cause, pc?}`: the target halted and no watch was on it |
| `target` | `{state, error?}`: `not responding`, `responding` or `reset` |

## Watches

`target.watch({on, reflex, capture, ...}, handler)` asks the device to watch
for `fault` (vector catch on HardFault, MemManage, BusFault, UsageFault),
`halt` (a halt the session didn't ask for), `lockup` or `reset`, one watch
per kind. The device polls DHCSR every 5 ms. When one fires it applies the
reflex at once (`halt`, `none` or `reset`), captures, then calls the handler
in the runtime; for a held target the handler's result (`hold`, `resume`,
`reset`) is applied when it settles, or `hold` after `handlerTimeoutMs`. More
than 10 fires a second holds the target without calling the handler.

## Captures

`CAPTURE_READ` returns a capture in pieces of up to 4096 bytes; read until
`offset` reaches `total`. The device keeps four. Little endian:

```text
header   "SICP", u8 version (1), u8 kind (0 fault, 1 halt, 2 lockup, 3 reset),
         u16 watch id, u32 capture id, u32 total length
section  u8 type, u8 sub, u16 0, u32 length, then length bytes padded to 4
  1 REGS   {u8 regsel, u8 ok, u16 0, u32 value} for r0-r15, xPSR, MSP, PSP, 20 (special)
  2 FAULT  u32 valid mask, then SHCSR CFSR HFSR DFSR MMFAR BFAR AFSR
  3 MEM    sub 0 stack, 1 range: u32 addr, u32 bytes asked for, then what was read
  4 STOP   u32 DFSR, u32 halted, u64 uptime_us, char cause[16]
```

Unknown sections are skipped by their length. Registers are only read while
the core is halted; the stack starts at the stacked frame's SP (MSP or PSP,
from EXC_RETURN) when the core stopped in a handler.
