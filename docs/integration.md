# SIDP component integration

The firmware owner supplies a real `target_backend_t` and drives each
`sidp_session` from the single debug task. Transport callbacks and the TX task
never call SWD. Programming and debugging must hold exclusive target ownership.

## Connection lifecycle

1. Initialize the transport. `link_is_connected()` reports physical availability;
   `is_open()` stays false until the owner accepts the connection.
2. After the previous target session is fully cleaned up, call
   `transport.begin_session()`. It discards old queues and resets wire buffers.
   It returns `ESP_ERR_INVALID_STATE` until a **new** physical connection exists,
   the old TX call has finished, and all borrowed RX buffers have been returned.
   It cannot revive a connection that already failed.
3. Create and initialize a new `sidp_session`. Its TX sink returns
   `transport.write_message(frame) == ESP_OK`. The sink copies the frame; it does
   not wait for queue space or transmission. If session initialization fails,
   close the transport session and require a new connection.
4. Feed CRC-validated packets from `start_read()` to `handle_request()`, always
   return the packet with `end_read()`, and call `handle_poll()` periodically.
   Use a short receive timeout so the debug task can poll and perform cleanup.
5. Check **both** session and transport `needs_disconnect()` before dispatch and
   after each request/poll. The transport flag covers asynchronous send failure,
   RX/control queue overflow, unknown protocol version and physical disconnect.
   `start_read()`/`flush_write()` also return `ESP_ERR_INVALID_STATE` after loss.
6. On failure, stop all old producers and call `transport.close_session()`.
   On the debug task call `session.handle_disconnect()`.
   If it returns **false**, retain the session object, its saved instructions,
   and exclusive target ownership. Retry cleanup; do not start a new debug or
   programming session. Return true means halt, patch restoration and backend
   detach succeeded. Only then destroy the old session.
7. Reconnect the physical link and repeat. No queued requests or writes are
   replayed. A send already in progress may finish on the old connection; a new
   session cannot be accepted until that call returns.

`begin_session()` and cleanup belong to the owner task, not transport callbacks.
The destructor releases storage; it is not a substitute for successful cleanup.
Initial traffic sent before acceptance may be discarded. The peer should allow
for device readiness when connecting and must not replay an ambiguous operation.

## USB CDC

The application owns the composite TinyUSB driver. In `tinyusb_config_t`, set
`event_cb` to `sidp::cdc_slip_transport::device_event_callback`, or forward events
from the application's existing callback:

```cpp
void usb_event(tinyusb_event_t *event, void *arg)
{
    sidp::cdc_slip_transport::device_event_callback(event, arg);
    // Other application USB event handling.
}
```

This forwarding is required for physical detach/reattach detection. CDC line
state callbacks are registered by `cdc_slip_transport::init()`. The host must
assert DTR to open SIDP and close/reopen the port (toggle DTR) after session loss.
An RTS-only change does not end the session. Device events and CDC callbacks are
serialized by the TinyUSB task. No SWD work belongs in either callback.

On a new session, driver RX/TX FIFOs are cleared; decoder epochs prevent a partial
old SLIP packet being completed in a new session. On an interrupted send, pending
TX bytes are cleared before the sender becomes idle.

## WebSocket

Both unexpected-disconnect and clean-CLOSE automatic reconnect are disabled.
All WebSocket events are registered, including disconnect, error, closed and
finish events. They latch the SIDP connection closed and invalidate partial RX.

After target cleanup, call `websocket_transport::reconnect()` from the connection
manager, never from a WebSocket callback. It waits for the library client to stop
and starts a fresh physical connection; if a TX call remains in flight it returns
`ESP_ERR_INVALID_STATE` so the owner can retry later. After CONNECTED, accept it
with `begin_session()` before dispatching requests.

The caller still supplies authentication/TLS configuration. The component does
not implement pairing, a WSS server or a cloud relay.

## Control and logs

Responses and events share one ordered control FIFO. Logs have a separate 4 KiB
queue and a maximum 1024-byte log buffer per frame. A full log queue drops logs
only; a full control queue ends the session immediately. The TX task selects a
control frame before selecting a log frame, so a log backlog cannot fill or sit
in front of the control queue.

A frame already on the wire cannot be preempted. Log sends use a 20 ms API timeout
(rounded up to a FreeRTOS tick), rather than the 5 s control-send timeout. Driver
scheduling and lock acquisition also contribute to actual elapsed time. Any
failed transmission ends the session because a partially delivered frame cannot
be treated as an intact message. `flush_write()` reports failure after that loss,
not successful delivery just because the queue became empty.

## Backend contract

The session enforces the attach response on the wire and never advertises a
capability or resource limit it cannot serve:

- `CAP_MEMORY_VECTOR`, the UART/RTT log-stream bits and the reserved ESP32
  GDB-Stub bits have no session handler and are stripped from the response.
- `hardware_breakpoints`/`hardware_watchpoints` are clamped to the session's own
  comparator tables, and a feature bit left with zero slots is dropped.
- `CAP_RESET_HALT`/`CAP_RESET_RUN` without `CAP_RESET_SYSTEM` or
  `CAP_RESET_NRST` are dropped.
- `max_memory_transfer == 0` becomes the protocol default 4096, and anything
  larger than one SIDP frame can carry is clamped.

After that, `RUN` rejects a request that needs an absent feature: a software or
hardware breakpoint without its capability, or a watchpoint without
`CAP_WATCHPOINT`, returns `NO_BREAKPOINT_SLOT`/`NO_WATCHPOINT_SLOT` (protocol
section 10 treats "no capability or no slots" as the same condition), and
hardware breakpoint/watchpoint counts are bounded by the advertised slot count.
`RUN_SINGLE_STEP` without `CAP_SINGLE_STEP` returns `UNSUPPORTED` because that
action cannot be represented at all. `RUN_TO_ADDRESS` returns
`NO_BREAKPOINT_SLOT` whenever there is no reusable or free hardware comparator,
including a backend with zero hardware-breakpoint slots (protocol section 10.1
treats this as "no available slot"). Memory reads/writes are bounded by
`max_memory_transfer`, which is a limit on wire requests only; internal
snapshot, stack and software-breakpoint transfers may exceed it. Internally,
the stack snapshot is taken only from a RAM region whose flags include
`MEM_READ`, and software breakpoints require RAM that is readable, writable and
executable (the original instruction is read back and substituted).

The backend must implement the invariants documented in
`include/sidp_backend.hpp`; that header is the authoritative contract, including
per-operation error handling, validation ownership, software-breakpoint
single-step and `RUN_TO_ADDRESS` comparator requirements, and retryable cleanup.
The session relies on those and cannot compensate for a backend that reports
resources it will not serve.

## Resource discipline (ESP32)

This is firmware: heap use is deliberately minimal and no allocation happens on
the per-request path.

- `sidp_session::init()` allocates its four PSRAM buffers once and
  `release_storage()` (destructor) frees them with the matching
  `heap_caps_free`. After `init()`, handling requests performs no allocation.
- The queue mutex uses `StaticSemaphore_t` storage created in `create_queues()`
  instead of `std::mutex`, because ESP-IDF's `std::mutex` lazily `malloc`s a
  pthread control block on first lock. `packet_queue_transport::queue_guard`
  takes/gives that static semaphore.
- Transport queues, staging buffers and TX tasks are allocated once in the
  transport `init()` and live for the process lifetime, by design.
- STL use is limited to non-allocating facilities (`std::span`, `std::array`,
  `std::atomic`, `<cstdint>`/`<cstddef>`, `std::numeric_limits`, `std::endian`);
  no runtime containers, strings, `std::function` or exceptions are used.
- Known library exceptions outside component control: `esp_websocket_client`
  allocates internally per event/message, and `reconnect()` restarts its task.
  Those are connection-level, not per-request, and the caller must budget for
  them (or use the CDC transport, which does not).
