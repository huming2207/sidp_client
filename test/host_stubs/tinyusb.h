#pragma once
#include <cstdint>
inline constexpr int TINYUSB_EVENT_ATTACHED = 0, TINYUSB_EVENT_DETACHED = 1;
struct tinyusb_event_t { int id; };
