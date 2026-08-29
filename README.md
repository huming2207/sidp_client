# SIDP Client

SIDP Client is the Soul Injector firmware component for the Soul Injector Debug
Protocol (SIDP). SIDP carries target-control, register, memory, stop-event, and
log-stream data between Soul Injector and Soul Agent, either directly or through
Soul Interconnect.

SIDP v1 targets Cortex-M0, Cortex-M3, and Cortex-M4. Cortex-M4 is the first
hardware-tested target; the other v1 targets remain part of the protocol scope
but require hardware validation.

## Repository layout

- `include/sidp_defs.hpp`: C++ wire-format enums and packed structures.
- `docs/`: protocol, architecture, caching, and target-support documentation.

## License

This project uses the same source-available license as the main SoulInjector
project: the [PolyForm Noncommercial License 1.0.0](LICENSE.md).

Non-commercial use is permitted under that license. Commercial use requires a
separate written commercial license from the copyright holder.
