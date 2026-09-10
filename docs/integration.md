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
