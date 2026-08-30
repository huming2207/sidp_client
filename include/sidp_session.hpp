#pragma once

#include <cstddef>
#include <cstdint>
#include <span>

#include "sidp_backend.hpp"
#include "sidp_defs.hpp"
#include "sidp_transport.hpp"

namespace sidp
{

    /** @brief Maximum concurrent software breakpoints tracked by the shadow table. */
    inline constexpr std::size_t MAX_SOFTWARE_BREAKPOINTS = 32;

    /** @brief Maximum hardware breakpoint comparators tracked by the session. */
    inline constexpr std::size_t MAX_HARDWARE_BREAKPOINTS = 16;

    /** @brief Maximum memory regions copied from the attach response. */
    inline constexpr std::size_t MAX_MEMORY_REGIONS = 64;

    /** @brief Bytes captured below SP in the STOPPED stack snapshot. */
    inline constexpr std::uint32_t STACK_SNAPSHOT_BELOW = 64;

    /** @brief Bytes captured above SP in the STOPPED stack snapshot. */
    inline constexpr std::uint32_t STACK_SNAPSHOT_ABOVE = 512;

    /**
     * @brief SIDP request/response/event state machine bound to one transport.
     *
     * sidp_session runs entirely on the single pinned debug task. It owns:
     *
     * - the four-state target model (DETACHED/HALTED/RUNNING/LOST) with the
     *   protocol's per-request gating table,
     * - stop_id generation and the STOPPED-event snapshot pipeline,
     * - the software-breakpoint shadow table with READ_MEMORY overlap
     *   substitution and WRITE_MEMORY shadow coherency,
     * - response/event transmission ordering (causal responses precede their
     *   STOPPED events) through a transport-owned TX sink.
     *
     * The session never blocks on transport TX; outgoing frames are handed
     * to the tx_sink callback, which the integrator wires to a TX queue.
     * The session is not thread-safe by itself: one task calls
     * handle_request()/handle_poll() sequentially.
     */
    class sidp_session
    {
    public:
        /** @brief Destination for complete, CRC-encoded SIDP frames. */
        using tx_sink_t = bool (*)(std::span<const std::uint8_t> frame);

        /** @brief Upper bound for the local HALT confirmation wait. */
        static constexpr std::uint32_t HALT_TIMEOUT_MS = 3000;
        /** @brief Upper bound for the local RESET_HALT confirmation wait. */
        static constexpr std::uint32_t RESET_TIMEOUT_MS = 5000;

        /**
         * @brief Binds the session to a backend and a TX sink.
         *
         * The backend must outlive the session. The TX sink receives fully
         * encoded SIDP frames (header + payload, CRC set) and must not block;
         * returning false marks the session transport-dead.
         */
        sidp_session(target_backend_t &backend, tx_sink_t tx_sink) noexcept;
        ~sidp_session() noexcept;

        sidp_session(const sidp_session &) = delete;
        sidp_session &operator=(const sidp_session &) = delete;

        /**
         * @brief Allocates the session working buffers in PSRAM.
         *
         * Must be called once after construction, before any request is fed
         * into the session. On failure every partial allocation is released
         * and the session stays unallocated; a retry is allowed.
         *
         * @return ESP_OK on success.
         * @return ESP_ERR_NO_MEM when a buffer cannot be allocated.
         * @return ESP_ERR_INVALID_STATE when already initialized.
         */
        [[nodiscard]] esp_err_t init() noexcept;

        /** @brief Feeds one decoded, CRC-validated SIDP request into the FSM. */
        void handle_request(std::span<const std::uint8_t> message) noexcept;

        /**
         * @brief Advances running-state polling and asynchronous stops.
         *
         * Call periodically while RUNNING (the debug task tick); the call
         * performs one backend poll and emits STOPPED on a transition.
         */
        void handle_poll() noexcept;

        /** @brief Performs session teardown for a lost transport connection. */
        void handle_disconnect() noexcept;

        /** @brief Current externally visible target state. */
        [[nodiscard]] target_state_t get_state() const noexcept { return state; }

        /** @brief Current stop generation; zero before the first stop. */
        [[nodiscard]] std::uint32_t get_stop_id() const noexcept { return stop_id; }

    private:
        // ---- Protocol gating (protocol doc section 7.1) ------------------------
        // Columns: ATTACH, DETACH, GET_STATE, READ_MEM, WRITE_MEM,
        //          READ_REGS, WRITE_REGS, RUN, HALT, RESET_HALT/RUN.

        enum gate_type : std::uint8_t {
            ALLOW = 0,
            ATTACH_BUSY,     ///< ATTACH while HALTED/RUNNING
            DETACHED_ERROR,  ///< plain STATUS_ERROR from DETACHED
            RUNNING_DENY,    ///< requires HALTED state (protocol section 7.1)
            LOST,            ///< terminal LOST state
        };

        enum gate_column_index : std::uint8_t {
            GATE_ATTACH = 0,
            GATE_DETACH,
            GATE_GET_STATE,
            GATE_READ_MEM,
            GATE_WRITE_MEM,
            GATE_READ_REGS,
            GATE_WRITE_REGS,
            GATE_RUN,
            GATE_HALT,
            GATE_RESET,
            GATE_COUNT,
        };

        static constexpr gate_type GATE_TABLE[4][GATE_COUNT] = {
            /* DETACHED */ {gate_type::ALLOW, gate_type::DETACHED_ERROR, gate_type::ALLOW, gate_type::DETACHED_ERROR, gate_type::DETACHED_ERROR, gate_type::DETACHED_ERROR, gate_type::DETACHED_ERROR, gate_type::DETACHED_ERROR, gate_type::DETACHED_ERROR, gate_type::DETACHED_ERROR},
            /* HALTED   */ {gate_type::ATTACH_BUSY, gate_type::ALLOW, gate_type::ALLOW, gate_type::ALLOW, gate_type::ALLOW, gate_type::ALLOW, gate_type::ALLOW, gate_type::ALLOW, gate_type::ALLOW, gate_type::ALLOW},
            /* RUNNING  */ {gate_type::ATTACH_BUSY, gate_type::ALLOW, gate_type::ALLOW, gate_type::ALLOW, gate_type::ALLOW, gate_type::RUNNING_DENY, gate_type::RUNNING_DENY, gate_type::RUNNING_DENY, gate_type::ALLOW, gate_type::ALLOW},
            /* LOST     */ {gate_type::LOST, gate_type::LOST, gate_type::ALLOW, gate_type::LOST, gate_type::LOST, gate_type::LOST, gate_type::LOST, gate_type::LOST, gate_type::LOST, gate_type::LOST},
        };

        /** @brief Maps an opcode to its gating-table column; 0xFF when unknown. */
        [[nodiscard]] static std::uint8_t gate_column(opcode_t opcode) noexcept;

        /** @brief Raw bits of a memory access flag set. */
        [[nodiscard]] static std::uint16_t flag_bits(memory_access_flag_t flags) noexcept;

        // ---- Frame assembly -------------------------------------------------
        /** @brief Begins a frame; returns the payload pointer or nullptr when oversized. */
        [[nodiscard]] std::uint8_t *begin_frame(msg_kind_t kind, opcode_t opcode, std::uint32_t request_id,
                                                std::size_t payload_size) noexcept;
        /** @brief Applies the CRC and sends the frame through the TX sink. */
        bool finish_frame() noexcept;

        bool send_response(opcode_t opcode, std::uint32_t request_id, status_t status,
                                         std::span<const std::uint8_t> payload = {}) noexcept;
        bool send_stopped() noexcept;
        bool send_target_lost(target_lost_reason_t reason) noexcept;

        // ---- Request handlers (one per opcode) ------------------------------
        void op_attach(std::uint32_t request_id, std::span<const std::uint8_t> payload) noexcept;
        void op_detach(std::uint32_t request_id, std::span<const std::uint8_t> payload) noexcept;
        void op_get_state(std::uint32_t request_id, std::span<const std::uint8_t> payload) noexcept;
        void op_read_memory(std::uint32_t request_id, std::span<const std::uint8_t> payload) noexcept;
        void op_write_memory(std::uint32_t request_id, std::span<const std::uint8_t> payload) noexcept;
        void op_read_registers(std::uint32_t request_id, std::span<const std::uint8_t> payload) noexcept;
        void op_write_registers(std::uint32_t request_id, std::span<const std::uint8_t> payload) noexcept;
        void op_run(std::uint32_t request_id, std::span<const std::uint8_t> payload) noexcept;
        void op_halt(std::uint32_t request_id, std::span<const std::uint8_t> payload) noexcept;
        void op_reset_halt(std::uint32_t request_id, std::span<const std::uint8_t> payload) noexcept;
        void op_reset_run(std::uint32_t request_id, std::span<const std::uint8_t> payload) noexcept;

        // ---- Stop pipeline ---------------------------------------------------
        /** @brief Enters HALTED, bumps stop_id, captures the snapshot. */
        void enter_halted(const stop_detect_t &stop) noexcept;
        /** @brief Queues the STOPPED event for the pending snapshot. */
        void emit_pending_stopped() noexcept;
        /** @brief Unified lost-target transition. */
        void enter_lost(target_lost_reason_t reason) noexcept;
        /** @brief Maps a backend failure to a response; ESP_FAIL enters LOST. */
        void send_backend_failure(opcode_t opcode, std::uint32_t request_id,
                                  esp_err_t result, status_t fallback) noexcept;
        /** @brief Appends the clipped stack snapshot to the pending registers. */
        void capture_stack_snapshot() noexcept;

        // ---- Memory access validation (protocol section 9) ---------------------
        /** @brief Validates one memory request against the memory map. */
        [[nodiscard]] status_t validate_memory_access(std::uint64_t address, std::uint32_t length,
                                                      memory_access_flag_t flags, memory_access_width_t width,
                                                      bool is_write) const noexcept;

        // ---- Run-configuration rollback ----------------------------------------
        /** @brief Clears every debug resource installed for a RUN attempt. */
        [[nodiscard]] bool rollback_run_config() noexcept;
        void fail_run(std::uint32_t request_id, status_t status) noexcept;

        // ---- Software breakpoint step-over -------------------------------------
        /** @brief Outcome of the internal step-over sequence. */
        enum class step_over_t : std::uint8_t {
            NOT_NEEDED,   ///< The halt PC does not sit on a software breakpoint.
            STEPPED,      ///< Internal step done; resume may continue.
            STOPPED_STEP, ///< The internal step was the user-requested single step.
            STOPPED_REAL, ///< The internal step hit a real stop; must surface it.
            FAILED,       ///< Step-over could not complete; target stays halted.
            FAILED_LOST,  ///< Target state unknown after an unconsummated step.
        };
        /** @brief Executes the step-over sequence when halted on a sw breakpoint. */
        [[nodiscard]] step_over_t execute_step_over(run_action_t action, stop_detect_t &step_stop) noexcept;
        [[nodiscard]] bool write_current_pc(std::uint32_t pc) noexcept;
        void normalize_pc_in_register_blob(std::uint8_t *data, std::size_t size) const noexcept;
        void update_step_over_after_register_write(const std::uint8_t *data, std::size_t size,
                                                   std::uint16_t register_count) noexcept;

        // ---- Software breakpoint shadow table --------------------------------
        struct sw_bp_t {
            std::uint32_t id = 0;
            std::uint64_t address = 0;
            std::uint8_t instruction_size = 0;
            bool installed = false;
            std::uint8_t original[4]{};
        };

        [[nodiscard]] bool sw_bp_apply(const std::span<const bp_entry_t> entries) noexcept;
        [[nodiscard]] bool sw_bp_install_one(sw_bp_t &entry) noexcept;
        [[nodiscard]] bool sw_bp_restore_all() noexcept;
        [[nodiscard]] bool sw_bp_restore_one(sw_bp_t &entry) noexcept;
        void sw_bp_substitute_read(std::uint64_t address, std::uint8_t *data, std::size_t size) const noexcept;
        void sw_bp_write_overlap(std::uint64_t address, const std::uint8_t *data, std::size_t size) noexcept;
        [[nodiscard]] sw_bp_t *sw_bp_find_by_address(std::uint64_t address) noexcept;
        [[nodiscard]] sw_bp_t *sw_bp_find_by_halt_pc(std::uint64_t pc) noexcept;

        [[nodiscard]] bool storage_ready() const noexcept;
        /** @brief Frees every init()-allocated buffer and nulls the pointers. */
        void release_storage() noexcept;
        [[nodiscard]] bool is_executable_range(std::uint64_t address, std::size_t size,
                                               bool require_writable) const noexcept;
        void clear_debug_state() noexcept;
        void clear_run_tracking() noexcept;

        target_backend_t &backend;
        tx_sink_t tx_sink;

        target_state_t state = TARGET_DETACHED;
        std::uint32_t stop_id = 0;
        bool stop_reported = false;
        bool attached = false;
        capability_t capabilities = static_cast<capability_t>(0);
        vector_catch_t supported_vector_catch = VECTOR_CATCH_NONE;
        run_action_t running_action = RUN_CONTINUE;
        bool run_to_active = false;
        std::uint64_t active_run_to_address = 0;
        std::uint32_t hw_breakpoint_ids[MAX_HARDWARE_BREAKPOINTS]{};
        std::size_t hw_count = 0;
        bool sw_step_over_pending = false;
        std::uint64_t sw_step_over_address = 0;

        // Frame assembly scratch: the session is driven by one task, so a single
        // reusable frame buffer and data scratch replace 8 KiB stack arrays.
        // Frame assembly happens in scratch: the session is driven by one
        // task, and finish_frame() only applies the CRC and hands the frame
        // to tx_sink, so assembly and use never overlap.
        std::uint8_t *scratch = nullptr;
        std::size_t tx_frame_size = 0;

        // Snapshot captured at the last transition, reported by emit_pending_stopped().
        stop_reason_t pending_reason = STOP_UNKNOWN;
        std::uint32_t pending_detail = 0;
        std::uint32_t pending_breakpoint_id = 0;
        std::uint64_t pending_watchpoint_address = 0;
        std::uint8_t *pending_registers = nullptr;
        std::size_t pending_registers_size = 0;
        std::uint64_t pending_stack_address = 0;
        std::size_t pending_stack_size = 0;

        // Memory map copied from the attach response (all region types).
        memory_region_t *regions = nullptr;
        std::size_t region_count = 0;

        // Active software breakpoint shadow state (replacement semantics per RUN).
        sw_bp_t *sw_table = nullptr;
        std::size_t sw_count = 0;
        static constexpr char TAG[] = "sidp_session";
    };

}
