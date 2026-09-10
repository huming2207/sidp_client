# SIDP Client

SIDP Client is the Soul Injector firmware component for the Soul Injector Debug
Protocol (SIDP). SIDP carries target-control, register, memory, stop-event, and
log-stream data between Soul Injector and Soul Agent, either directly or through
Soul Interconnect.

SIDP v1 targets Cortex-M0, Cortex-M3, and Cortex-M4. Cortex-M4 is the first
target planned for hardware validation; the other v1 targets remain part of the protocol scope
but require hardware validation.

## Repository layout

- `include/sidp_defs.hpp`: C++ wire-format enums and packed structures.
- `sidp_session.cpp`: device-side request handling and target state machine.
- `include/sidp_backend.hpp`: hardware interface; the real SWD adapter is still needed.
- `sidp_transport*.cpp`: CRC, queued transport, USB CDC/SLIP and WebSocket client.
- `test/`: host session tests with a mock target.
- `docs/`: protocol, architecture, caching, and target-support documentation.

## Next milestone

Connect the existing session to a real Cortex-M4 target over USB CDC: attach,
halt, read registers/RAM, continue and detach. See the
[revised implementation plan](docs/target-support-roadmap.md#11-当前实施计划2026-09-10-调整)
for the current gaps and deferred features. Host tests do not establish hardware support.

## Host tests

Requires CMake and a GNU C++20-compatible compiler; ESP-IDF is not needed.

```sh
cmake -S test -B /tmp/sidp-host-build
cmake --build /tmp/sidp-host-build
ctest --test-dir /tmp/sidp-host-build --output-on-failure
```

These tests cover the session, queue lifecycle, control/log scheduling, and
CDC/WebSocket receive callbacks using host substitutes for ESP-IDF and the
hardware backend. They do not establish real USB, network, FreeRTOS or SWD timing.

See [component integration](docs/integration.md) for connection acceptance,
TinyUSB event forwarding, and cleanup retry requirements.

## License

This project uses the same source-available license as the main SoulInjector
project: the [PolyForm Noncommercial License 1.0.0](LICENSE.md).

Non-commercial use is permitted under that license. Commercial use requires a
separate written commercial license from the copyright holder.
