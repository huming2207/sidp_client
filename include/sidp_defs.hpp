#pragma once

#include <bit>
#include <cstdint>

namespace sidp
{

    inline constexpr std::uint8_t PROTOCOL_VERSION = 1;
    inline constexpr std::uint32_t MAX_FRAME_SIZE = 8192;

    enum msg_kind_t : std::uint8_t {
        KIND_REQUEST = 1,
        KIND_RESPONSE = 2,
        KIND_EVENT = 3,
        KIND_LOG_STREAM = 4,
    };

    enum opcode_t : std::uint16_t {
        OP_ATTACH = 0x0001,
        OP_DETACH = 0x0002,
        OP_GET_STATE = 0x0003,

        OP_READ_MEMORY = 0x0010,
        OP_READ_MEMORY_VECTOR = 0x0011,
        OP_WRITE_MEMORY = 0x0012,
        OP_READ_REGISTERS = 0x0013,
        OP_WRITE_REGISTERS = 0x0014,

        OP_RUN = 0x0020,
        OP_HALT = 0x0021,
        OP_RESET_HALT = 0x0022,
        OP_RESET_RUN = 0x0023,

        OP_SET_LOG_STREAM = 0x0030,
        OP_LOG_DATA = 0x0031,

        OP_ATTACH_GDB_STUB = 0x0040,

        EVT_STOPPED = 0x0080,
        EVT_TARGET_LOST = 0x0081,
    };

    enum status_t : std::int32_t {
        STATUS_OK = 0,
        STATUS_ERROR = 1,
        STATUS_INVALID_ARGUMENT = 2,
        STATUS_UNSUPPORTED = 3,
        STATUS_BUSY = 4,
        STATUS_TIMEOUT = 5,

        STATUS_TARGET_RUNNING = 10,
        STATUS_TARGET_HALTED = 11,
        STATUS_TARGET_LOST = 12,
        STATUS_STALE_STOP = 13,

        STATUS_SWD_ERROR = 20,
        STATUS_ADDRESS_ERROR = 21,
        STATUS_NO_BREAKPOINT_SLOT = 22,
        STATUS_NO_WATCHPOINT_SLOT = 23,
        STATUS_ALIGNMENT_ERROR = 24,
        STATUS_TARGET_MISMATCH = 25,
    };

    enum architecture_t : std::uint8_t {
        ARCH_ARM_M = 1,
        ARCH_RISCV = 2,
        ARCH_XTENSA = 3,
    };

    enum target_profile_t : std::uint8_t {
        PROFILE_ARMV6M = 1,
        PROFILE_ARMV7M = 2,
        PROFILE_ARMV7EM = 3,
        PROFILE_ARMV8M_BASE = 4,
        PROFILE_ARMV8M_MAIN = 5,
        PROFILE_ARMV81M_MAIN = 6,

        PROFILE_RV32 = 32,
        PROFILE_RV64 = 33,

        PROFILE_XTENSA_ESP32 = 40,
        PROFILE_XTENSA_ESP32S2 = 41,
        PROFILE_XTENSA_ESP32S3 = 42,
    };

    enum capability_t : std::uint32_t {
        CAP_STOP_SNAPSHOT = 1u << 0,
        CAP_MEMORY_VECTOR = 1u << 1,
        CAP_SOFTWARE_BP = 1u << 2,
        CAP_HARDWARE_BP = 1u << 3,
        CAP_WATCHPOINT = 1u << 4,
        CAP_SINGLE_STEP = 1u << 5,
        CAP_RESET_HALT = 1u << 6,
        CAP_UART_LOG_STREAM = 1u << 7,
        CAP_RTT_LOG_STREAM = 1u << 8,
        CAP_FPU = 1u << 9,
        CAP_RESET_SYSTEM = 1u << 10,
        CAP_RESET_NRST = 1u << 11,
        CAP_RESET_RUN = 1u << 12,
        CAP_POST_MORTEM = 1u << 13,
        CAP_TARGET_GDB_STUB = 1u << 14,
    };

    enum memory_type_t : std::uint8_t {
        MEMORY_RAM = 1,
        MEMORY_FLASH = 2,
        MEMORY_MMIO = 3,
    };

    enum memory_flag_t : std::uint8_t {
        MEM_READ = 1u << 0,
        MEM_WRITE = 1u << 1,
        MEM_EXECUTE = 1u << 2,
        MEM_CACHEABLE = 1u << 3,
        MEM_VOLATILE = 1u << 4,
    };

    enum vector_catch_t : std::uint32_t {
        VECTOR_CATCH_NONE = 0,
        VECTOR_CATCH_RESET = 1u << 0,
        VECTOR_CATCH_HARD_FAULT = 1u << 1,
        VECTOR_CATCH_MEM_MANAGE = 1u << 2,
        VECTOR_CATCH_BUS_FAULT = 1u << 3,
        VECTOR_CATCH_USAGE_FAULT = 1u << 4,
    };

    enum target_state_t : std::uint8_t {
        TARGET_DETACHED = 0,
        TARGET_HALTED = 1,
        TARGET_RUNNING = 2,
        TARGET_LOST = 3,
    };

    enum register_value_flag_t : std::uint8_t {
        REGISTER_VALUE_FLAG_NONE = 0,
        REGISTER_VALUE_FLAG_UNAVAILABLE = 1u << 0,
    };

    enum register_id_t : std::uint16_t {
        ARM_REG_R0 = 0x0000,
        ARM_REG_R1 = 0x0001,
        ARM_REG_R2 = 0x0002,
        ARM_REG_R3 = 0x0003,
        ARM_REG_R4 = 0x0004,
        ARM_REG_R5 = 0x0005,
        ARM_REG_R6 = 0x0006,
        ARM_REG_R7 = 0x0007,
        ARM_REG_R8 = 0x0008,
        ARM_REG_R9 = 0x0009,
        ARM_REG_R10 = 0x000A,
        ARM_REG_R11 = 0x000B,
        ARM_REG_R12 = 0x000C,
        ARM_REG_SP = 0x000D,
        ARM_REG_LR = 0x000E,
        ARM_REG_PC = 0x000F,
        ARM_REG_XPSR = 0x0010,
        ARM_REG_MSP = 0x0011,
        ARM_REG_PSP = 0x0012,
        ARM_REG_PRIMASK = 0x0013,
        ARM_REG_BASEPRI = 0x0014,
        ARM_REG_FAULTMASK = 0x0015,
        ARM_REG_CONTROL = 0x0016,

        ARM_REG_S0 = 0x0040,
        ARM_REG_S1 = 0x0041,
        ARM_REG_S2 = 0x0042,
        ARM_REG_S3 = 0x0043,
        ARM_REG_S4 = 0x0044,
        ARM_REG_S5 = 0x0045,
        ARM_REG_S6 = 0x0046,
        ARM_REG_S7 = 0x0047,
        ARM_REG_S8 = 0x0048,
        ARM_REG_S9 = 0x0049,
        ARM_REG_S10 = 0x004A,
        ARM_REG_S11 = 0x004B,
        ARM_REG_S12 = 0x004C,
        ARM_REG_S13 = 0x004D,
        ARM_REG_S14 = 0x004E,
        ARM_REG_S15 = 0x004F,
        ARM_REG_S16 = 0x0050,
        ARM_REG_S17 = 0x0051,
        ARM_REG_S18 = 0x0052,
        ARM_REG_S19 = 0x0053,
        ARM_REG_S20 = 0x0054,
        ARM_REG_S21 = 0x0055,
        ARM_REG_S22 = 0x0056,
        ARM_REG_S23 = 0x0057,
        ARM_REG_S24 = 0x0058,
        ARM_REG_S25 = 0x0059,
        ARM_REG_S26 = 0x005A,
        ARM_REG_S27 = 0x005B,
        ARM_REG_S28 = 0x005C,
        ARM_REG_S29 = 0x005D,
        ARM_REG_S30 = 0x005E,
        ARM_REG_S31 = 0x005F,
        ARM_REG_FPSCR = 0x0060,
    };

    enum memory_access_flag_t : std::uint16_t {
        MEM_ACCESS_REQUIRE_HALTED = 1u << 0,
        MEM_ACCESS_ALLOW_RUNNING = 1u << 1,
        MEM_ACCESS_ALLOW_MMIO = 1u << 2,
    };

    enum memory_access_width_t : std::uint8_t {
        MEM_WIDTH_DEFAULT = 0,
        MEM_WIDTH_8 = 1,
        MEM_WIDTH_16 = 2,
        MEM_WIDTH_32 = 4,
    };

    enum breakpoint_kind_t : std::uint8_t {
        BREAKPOINT_AUTO = 0,
        BREAKPOINT_HARDWARE = 1,
        BREAKPOINT_SOFTWARE = 2,
    };

    enum watchpoint_access_t : std::uint8_t {
        WATCH_READ = 1,
        WATCH_WRITE = 2,
        WATCH_READ_WRITE = 3,
    };

    enum run_action_t : std::uint8_t {
        RUN_CONTINUE = 1,
        RUN_SINGLE_STEP = 2,
        RUN_TO_ADDRESS = 3,
    };

    enum run_flag_t : std::uint8_t {
        RUN_FLAG_NONE = 0,
    };

    enum reset_kind_t : std::uint8_t {
        RESET_SYSTEM = 1,
        RESET_NRST = 2,
    };

    enum detach_action_t : std::uint8_t {
        DETACH_KEEP_HALTED = 1,
        DETACH_RESUME = 2,
    };

    enum stop_reason_t : std::uint8_t {
        STOP_UNKNOWN = 0,
        STOP_BREAKPOINT = 1,
        STOP_WATCHPOINT = 2,
        STOP_SINGLE_STEP = 3,
        STOP_USER_HALT = 4,
        STOP_VECTOR_CATCH = 5,
        STOP_FAULT = 6,
        STOP_RESET = 7,
        STOP_LOCKUP = 8,
        STOP_RUN_TO_ADDRESS = 9,
    };

    enum stopped_flag_t : std::uint8_t {
        STOPPED_FLAG_NONE = 0,
    };

    enum target_lost_reason_t : std::uint8_t {
        TARGET_LOST_SWD_FAULT = 1,
        TARGET_LOST_POWER = 2,
        TARGET_LOST_DISCONNECT = 3,
        TARGET_LOST_TIMEOUT = 4,
    };

    enum log_channel_t : std::uint16_t {
        LOG_CHANNEL_TARGET_UART0 = 0x0001,
        LOG_CHANNEL_TARGET_RTT_UP_0 = 0x0100,
        LOG_CHANNEL_TARGET_RTT_UP_1 = 0x0101,
        LOG_CHANNEL_TARGET_RTT_UP_2 = 0x0102,
        LOG_CHANNEL_TARGET_RTT_UP_3 = 0x0103,
    };

    enum log_source_t : std::uint8_t {
        LOG_SOURCE_UART = 1,
        LOG_SOURCE_RTT = 2,
    };

    enum log_control_t : std::uint8_t {
        LOG_DISABLE = 0,
        LOG_ENABLE = 1,
    };

    struct __attribute__((packed)) msg_header_t {
        std::uint8_t version;
        msg_kind_t kind;
        opcode_t opcode;
        std::uint32_t request_id;
        std::uint32_t crc32;
        std::uint8_t payload[];
    };

    struct __attribute__((packed)) response_prefix_t {
        status_t status;
        std::uint8_t payload[];
    };

    struct __attribute__((packed)) memory_region_t {
        std::uint64_t start;
        std::uint64_t length;
        memory_type_t type;
        memory_flag_t flags;
        std::uint8_t reserved[2];
    };

    struct __attribute__((packed)) attach_request_t {
        std::uint16_t initial_stack_before;
        std::uint16_t initial_stack_after;
        std::uint8_t connect_under_reset;
        std::uint8_t halt_after_attach;
        std::uint8_t reserved[2];
    };

    struct __attribute__((packed)) attach_response_t {
        status_t status;
        architecture_t architecture;
        target_profile_t profile;
        std::uint8_t address_width;
        std::uint8_t reserved0;
        capability_t capabilities;
        std::uint32_t target_id;
        std::uint32_t cpu_id;
        vector_catch_t supported_vector_catch_mask;
        std::uint16_t hardware_breakpoints;
        std::uint16_t hardware_watchpoints;
        std::uint16_t memory_region_count;
        std::uint16_t max_memory_transfer;
        memory_region_t memory_regions[];
    };

    struct __attribute__((packed)) get_state_response_t {
        status_t status;
        target_state_t target_state;
        std::uint8_t reserved[3];
        std::uint32_t stop_id;
    };

    struct __attribute__((packed)) register_value_t {
        register_id_t register_id;
        std::uint8_t value_size;
        register_value_flag_t flags;
        std::uint8_t value[];
    };

    struct __attribute__((packed)) read_registers_request_t {
        std::uint32_t stop_id;
        std::uint16_t core_id;
        std::uint16_t register_count;
        register_id_t register_ids[];
    };

    struct __attribute__((packed)) read_registers_response_t {
        status_t status;
        std::uint32_t stop_id;
        std::uint16_t register_count;
        std::uint16_t reserved;
        std::uint8_t registers[];
    };

    struct __attribute__((packed)) write_registers_request_t {
        std::uint32_t stop_id;
        std::uint16_t core_id;
        std::uint16_t register_count;
        std::uint8_t registers[];
    };

    struct __attribute__((packed)) read_memory_request_t {
        std::uint32_t stop_id;
        std::uint64_t address;
        std::uint32_t length;
        memory_access_flag_t flags;
        memory_access_width_t access_width;
        std::uint8_t reserved;
    };

    struct __attribute__((packed)) read_memory_response_t {
        status_t status;
        std::uint64_t address;
        std::uint32_t completed_length;
        std::uint8_t data[];
    };

    struct __attribute__((packed)) write_memory_request_t {
        std::uint32_t stop_id;
        std::uint64_t address;
        std::uint32_t length;
        memory_access_flag_t flags;
        memory_access_width_t access_width;
        std::uint8_t reserved;
        std::uint8_t data[];
    };

    struct __attribute__((packed)) memory_range_t {
        std::uint64_t address;
        std::uint32_t length;
        memory_access_flag_t flags;
        memory_access_width_t access_width;
        std::uint8_t reserved;
    };

    struct __attribute__((packed)) read_memory_vector_request_t {
        std::uint32_t stop_id;
        std::uint16_t range_count;
        std::uint16_t reserved;
        memory_range_t ranges[];
    };

    struct __attribute__((packed)) memory_block_t {
        std::uint64_t address;
        std::uint32_t completed_length;
        std::uint8_t data[];
    };

    struct __attribute__((packed)) read_memory_vector_response_t {
        status_t status;
        std::uint32_t stop_id;
        std::uint16_t range_count;
        std::uint16_t reserved;
        std::uint8_t blocks[];
    };

    struct __attribute__((packed)) breakpoint_t {
        std::uint32_t breakpoint_id;
        std::uint64_t address;
        breakpoint_kind_t kind;
        std::uint8_t instruction_size;
        std::uint8_t enabled;
        std::uint8_t temporary;
    };

    struct __attribute__((packed)) watchpoint_t {
        std::uint32_t watchpoint_id;
        std::uint64_t address;
        watchpoint_access_t access;
        std::uint8_t size;
        std::uint8_t enabled;
        std::uint8_t reserved;
    };

    struct __attribute__((packed)) run_request_t {
        std::uint32_t stop_id;
        std::uint16_t core_id;
        run_action_t action;
        run_flag_t flags;
        vector_catch_t vector_catch_mask;
        std::uint64_t run_to_address;
        std::uint16_t breakpoint_count;
        std::uint16_t watchpoint_count;
        std::uint8_t payload[];
    };

    struct __attribute__((packed)) reset_request_t {
        reset_kind_t kind;
        std::uint8_t reserved[3];
    };

    struct __attribute__((packed)) detach_request_t {
        detach_action_t action;
        std::uint8_t reserved[3];
    };

    struct __attribute__((packed)) stopped_event_t {
        std::uint32_t stop_id;
        std::uint16_t core_id;
        stop_reason_t reason;
        stopped_flag_t flags;
        std::uint32_t reason_detail;
        std::uint32_t breakpoint_id;
        std::uint64_t watchpoint_address;
        std::uint16_t register_count;
        std::uint16_t stack_length;
        std::uint64_t stack_address;
        std::uint8_t payload[];
    };

    struct __attribute__((packed)) target_lost_event_t {
        target_lost_reason_t reason;
        std::uint8_t reserved[3];
        std::uint32_t detail;
    };

    struct __attribute__((packed)) set_log_stream_request_t {
        log_channel_t log_channel;
        log_source_t source;
        log_control_t control;
        std::uint32_t uart_baud;
        std::uint64_t rtt_control_block_address;
        std::uint16_t rtt_up_buffer_index;
        std::uint16_t rtt_poll_interval_ms;
    };

    struct __attribute__((packed)) log_data_t {
        log_channel_t log_channel;
        std::uint16_t buf_len;
        std::uint8_t buf[];
    };

    struct __attribute__((packed)) attach_gdb_stub_request_t {
        log_channel_t uart_channel;
        std::uint16_t reserved;
        std::uint32_t uart_baud;
        std::uint16_t initial_stack_before;
        std::uint16_t initial_stack_after;
    };

    static_assert(std::endian::native == std::endian::little, "SIDP v1 requires a little-endian host");
    static_assert(sizeof(msg_header_t) == 12, "SIDP message header must be 12 bytes");
    static_assert(sizeof(response_prefix_t) == 4, "SIDP response prefix must be 4 bytes");
    static_assert(sizeof(memory_region_t) == 20, "SIDP memory region must be 20 bytes");
    static_assert(sizeof(attach_request_t) == 8, "SIDP attach request must be 8 bytes");
    static_assert(sizeof(attach_response_t) == 32, "SIDP attach response prefix must be 32 bytes");
    static_assert(sizeof(get_state_response_t) == 12, "SIDP get-state response must be 12 bytes");
    static_assert(sizeof(register_value_t) == 4, "SIDP register-value prefix must be 4 bytes");
    static_assert(sizeof(read_registers_request_t) == 8, "SIDP read-registers request prefix must be 8 bytes");
    static_assert(sizeof(read_registers_response_t) == 12, "SIDP read-registers response prefix must be 12 bytes");
    static_assert(sizeof(write_registers_request_t) == 8, "SIDP write-registers request prefix must be 8 bytes");
    static_assert(sizeof(read_memory_request_t) == 20, "SIDP read-memory request must be 20 bytes");
    static_assert(sizeof(read_memory_response_t) == 16, "SIDP read-memory response prefix must be 16 bytes");
    static_assert(sizeof(write_memory_request_t) == 20, "SIDP write-memory request prefix must be 20 bytes");
    static_assert(sizeof(memory_range_t) == 16, "SIDP memory range must be 16 bytes");
    static_assert(sizeof(read_memory_vector_request_t) == 8, "SIDP read-memory-vector request prefix must be 8 bytes");
    static_assert(sizeof(memory_block_t) == 12, "SIDP memory-block prefix must be 12 bytes");
    static_assert(sizeof(read_memory_vector_response_t) == 12, "SIDP read-memory-vector response prefix must be 12 bytes");
    static_assert(sizeof(breakpoint_t) == 16, "SIDP breakpoint must be 16 bytes");
    static_assert(sizeof(watchpoint_t) == 16, "SIDP watchpoint must be 16 bytes");
    static_assert(sizeof(run_request_t) == 24, "SIDP run request prefix must be 24 bytes");
    static_assert(sizeof(reset_request_t) == 4, "SIDP reset request must be 4 bytes");
    static_assert(sizeof(detach_request_t) == 4, "SIDP detach request must be 4 bytes");
    static_assert(sizeof(stopped_event_t) == 36, "SIDP stopped event prefix must be 36 bytes");
    static_assert(sizeof(target_lost_event_t) == 8, "SIDP target-lost event must be 8 bytes");
    static_assert(sizeof(set_log_stream_request_t) == 20, "SIDP set-log-stream request must be 20 bytes");
    static_assert(sizeof(log_data_t) == 4, "SIDP log-data prefix must be 4 bytes");
    static_assert(sizeof(attach_gdb_stub_request_t) == 12, "SIDP attach-GDB-stub request must be 12 bytes");

}
