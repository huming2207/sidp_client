#pragma once

#include <cstddef>
#include <cstdint>
#include <span>

#include "esp_err.h"

#include "sidp_defs.hpp"

namespace sidp
{

    /** @brief Target debug resources as reported by a backend after attach. */
    struct attach_info_t {
        architecture_t architecture = ARCH_ARM_M;
        target_profile_t profile = PROFILE_ARMV7EM;
        std::uint8_t address_width = 32;
        capability_t capabilities = static_cast<capability_t>(0);
        std::uint32_t target_id = 0;
        std::uint32_t cpu_id = 0;
        vector_catch_t supported_vector_catch_mask = VECTOR_CATCH_NONE;
        std::uint16_t hardware_breakpoints = 0;
        std::uint16_t hardware_watchpoints = 0;
        /**
         * @brief Largest single memory read/write the peer may request.
         *
         * This is a wire-contract limit for incoming SIDP requests, not a
         * constraint on the backend's own transfers: snapshot, stack, software
         * breakpoint and shadow reads/writes may be larger or smaller. 0 means
         * the protocol default (DEFAULT_MAX_MEMORY_TRANSFER).
         */
        std::uint16_t max_memory_transfer = 0;
        /** @brief Memory regions copied into the attach response. */
        std::span<const std::uint8_t> memory_regions;
    };

    /** @brief Parameters for target_backend_t::attach(). */
    struct attach_params_t {
        bool connect_under_reset = false;
        bool halt_after_attach = true;
    };

    /** @brief Result of observing a running-to-halted transition. */
    struct stop_detect_t {
        bool halted = false;
        /** @brief Raw DHCSR/DFSR observation of the stop cause. */
        std::uint32_t dfsr = 0;
        std::uint32_t dhcsr = 0;
        bool lockup = false;
        /** @brief Raw architectural PC observed at halt. */
        std::uint32_t pc = 0;
        /** @brief Comparator index when an FPB/DWT comparator matched. */
        bool comparator_match = false;
        std::uint8_t comparator_index = 0;
        /** @brief True when the stop was caused by a watchpoint. */
        bool watchpoint_match = false;
        std::uint64_t watchpoint_address = 0;
        /** @brief True when the backend identified a non-vector-catch fault. */
        bool fault = false;
    };

    /** @brief One breakpoint entry passed to apply_breakpoints(). */
    struct bp_entry_t {
        std::uint32_t breakpoint_id = 0;
        std::uint64_t address = 0;
        breakpoint_kind_t kind = BREAKPOINT_HARDWARE;
        std::uint8_t instruction_size = 0;
        std::uint8_t temporary = 0;
    };

    /** @brief One watchpoint entry passed to apply_watchpoints(). */
    struct wp_entry_t {
        std::uint32_t watchpoint_id = 0;
        std::uint64_t address = 0;
        watchpoint_access_t access = WATCH_WRITE;
        std::uint8_t size = 4;
    };

    /**
     * @brief Hardware target interface consumed by the SIDP session.
     *
     * One implementation runs on the single pinned debug task. Methods are
     * not required to be reentrant; the session serializes all calls. Pure
     * hardware semantics only: breakpoint shadow substitution, PC
     * normalization policy, and stop_id generations live in the session.
     *
     * Return codes (esp_err_t):
     * - ESP_OK on success,
     * - ESP_FAIL for a protocol-level fault (no-ACK, parity),
     * - ESP_ERR_TIMEOUT when a bounded local wait expired,
     * - ESP_ERR_INVALID_ARG / ESP_ERR_NOT_SUPPORTED for rejected parameters.
     *
     * Error handling is per operation, not uniform:
     * - poll_halted(): any non-OK result makes the session enter LOST.
     * - halt()/reset(): non-OK is reported to the peer; reset also enters LOST
     *   because the target state afterwards is unknown.
     * - read_mem()/write_mem()/read_regs()/write_regs(): non-OK is reported and
     *   the target stays attached (unless the result is ESP_FAIL, which the
     *   session treats as a protocol fault and maps to LOST).
     * - attach(): non-OK is reported; ESP_FAIL enters LOST.
     * - detach(): non-OK leaves the session attached so a later call can retry.
     * - apply_breakpoints()/apply_watchpoints()/set_vector_catch(): a failed
     *   rollback makes the session enter
     *   LOST; a failed initial install keeps the target halted and attached.
     * - resume(): ANY non-OK result makes the session enter LOST, because the
     *   session cannot know whether the request reached the target. Return an
     *   error only when you can accept that consequence; make resume() a no-op
     *   with ESP_OK is never correct.
     *
     * Backend contract (the session relies on all of these):
     *
     * - Validation ownership: the session validates the wire format, request
     *   state and stop generation, memory-region containment, width/alignment
     *   and MMIO rules. The backend validates hardware semantics: register IDs
     *   and widths for the selected profile, whether a hardware slot exists,
     *   and whether the debug port supports the requested access. An unknown or
     *   unsupported register/width must return ESP_ERR_INVALID_ARG ("do not"),
     *   never silently read or write a different register.
     * - Single core: v1 exposes exactly one core (core_id 0 in STOPPED). The
     *   backend interface carries no core argument; it always operates on that
     *   one core.
     * - attach() reports only resources the backend will actually serve.
     *   Capability bits without a working implementation are a bug: the
     *   session strips unhandled bits but cannot detect a feature that is
     *   advertised and then rejected at request time. hardware_breakpoints and
     *   hardware_watchpoints must come from runtime discovery. The
     *   attach_info_t::memory_regions span must stay valid until the session has
     *   copied it (the copy happens before attach() returns to the peer).
     * - attach() must not reset the target unless connect_under_reset was set;
     *   the session's post-attach halt may use halt_after_attach only.
     * - halt() must not return success until the target is confirmed halted, and
     *   must succeed as a no-op with a populated observation when the target is
     *   already halted. handle_disconnect() calls it unconditionally;
     *   prepare_reset() calls it when the session is not yet HALTED.
     * - reset() with halt_after == true must confirm the halt and populate stop
     *   before returning ESP_OK; the session enters HALTED from that observation
     *   without re-checking.
     * - poll_halted() returns ESP_OK with stop.halted == false while the target
     *   is running; it must not use an error return to mean "still running".
     * - read_regs() with an empty id set returns the complete register set
     *   implied by the advertised profile and capabilities (including S0-S31 and
     *   FPSCR when CAP_FPU is set). Registers that cannot be read must still be
     *   reported with a 4-byte placeholder and REGISTER_VALUE_FLAG_UNAVAILABLE.
     *   read_regs() must never write past out_blob and must return the exact
     *   byte count written. read_regs()/write_regs() must also reject
     *   UNAVAILABLE entries on write and duplicate register IDs.
     * - Memory: read_mem()/write_mem() must complete the full requested length on
     *   ESP_OK, or return an error without a partial-success contract. The
     *   session may issue internal transfers that are NOT bounded by
     *   max_memory_transfer (STOPPED stack snapshot, software-breakpoint
     *   shadow reads/writes, patch install/restore). Those still obey the
     *   region/width rules above and only target a region whose flags permit the
     *   access; the stack snapshot requires MEM_READ.
     * - apply_breakpoints(), apply_watchpoints() and set_vector_catch() must
     *   accept an empty/zero set and return ESP_OK even when the feature is not
     *   supported: the session uses them to clear previous RUN configuration and
     *   to roll back a failed RUN. A non-empty set either installs completely or
     *   fails without partial application.
     * - Software breakpoints: continuing past one requires an internal single
     *   step (section 10.4), so a backend must support resume(RUN_SINGLE_STEP)
     *   to advertise CAP_SOFTWARE_BP (the session strips that bit otherwise).
     *   The internal step must complete within a bounded number of poll_halted()
     *   calls; a step that never confirms is treated as TARGET_LOST.
     * - RUN_TO_ADDRESS: the backend must install a temporary comparator (reusing
     *   an existing hardware breakpoint at the same address where possible),
     *   remove it when the stop is reported, and report the comparator match so
     *   the session can map STOPPED to SIDP_STOP_RUN_TO_ADDRESS.
     * - Cleanup retry: after a failed detach or an incomplete halt/restore, the
     *   session keeps the object and the target exclusive and calls again; the
     *   backend must make retries safe and must not partially release owned
     *   debug resources.
     */
    class target_backend_t
    {
    public:
        target_backend_t(const target_backend_t &) = delete;
        target_backend_t &operator=(const target_backend_t &) = delete;
        virtual ~target_backend_t() = default;

        /**
         * @brief Connects and returns the discovered target resources.
         *
         * Must not reset the target unless params.connect_under_reset is set.
         * Report only capabilities and resource counts the backend can actually
         * serve; the session sanitizes extremes but cannot detect a feature that
         * is advertised and then rejected later. See the class-level contract.
         */
        [[nodiscard]] virtual esp_err_t attach(const attach_params_t &params, attach_info_t &info) = 0;

        /**
         * @brief Restores debug resources and applies the final action.
         *
         * The backend restores the hardware breakpoints, watchpoints and vector
         * catch it programmed; the session restores software-breakpoint patches
         * through write_mem() before calling this. A non-OK return keeps the
         * session attached so cleanup can be retried; the backend must not
         * partially release the debug port on failure.
         */
        [[nodiscard]] virtual esp_err_t detach(detach_action_t action) = 0;

        /**
         * @brief Executes a reset and optionally halts after it.
         * @param kind Reset method requested by the peer.
         * @param halt_after True for RESET_HALT, false for RESET_RUN.
         * @param stop Receives the post-reset stop observation when halt_after;
         *        must be a confirmed halt on ESP_OK.
         */
        [[nodiscard]] virtual esp_err_t reset(reset_kind_t kind, bool halt_after, stop_detect_t &stop) = 0;

        /**
         * @brief Reads the debug state once.
         * @param stop Receives the current halt state when ESP_OK is returned.
         *
         * Returns ESP_OK with stop.halted == false while running; an error
         * return means the observation itself failed, not that the target runs.
         */
        [[nodiscard]] virtual esp_err_t poll_halted(stop_detect_t &stop) = 0;

        /**
         * @brief Requests a halt and waits locally until the target confirms.
         * @param timeout_ms Local bounded wait.
         *
         * Must succeed as a no-op with a populated stop observation when the
         * target is already halted: handle_disconnect() calls it unconditionally,
         * and prepare_reset() calls it whenever the session is not yet HALTED.
         */
        [[nodiscard]] virtual esp_err_t halt(std::uint32_t timeout_ms, stop_detect_t &stop) = 0;

        /**
         * @brief Issues the next resume or single step.
         * @param action Continue, single step, or run to address.
         * @param address Target address for RUN_TO_ADDRESS, otherwise 0.
         *
         * Resume is special: ANY non-OK return makes the session enter LOST,
         * because a failed acknowledgement does not prove the request did not
         * reach the target. Validate everything checkable before issuing it, and
         * return an error only when losing the session is the correct outcome.
         * RUN_TO_ADDRESS requires the backend to install a temporary comparator
         * and remove it when the stop is reported (see the class contract).
         */
        [[nodiscard]] virtual esp_err_t resume(run_action_t action, std::uint64_t address) = 0;

        /**
         * @brief Reads registers by ID into little-endian TLV values.
         * @param ids Register IDs; empty set reads the full set implied by the
         *        advertised profile and capabilities, with UNAVAILABLE
         *        placeholder entries for registers that cannot be read.
         * @param out_blob Output buffer; implementations must never write
         *        beyond its extent.
         * @param out_size Actual number of bytes written (<= out_blob.size()).
         */
        [[nodiscard]] virtual esp_err_t read_regs(std::span<const register_id_t> ids, std::span<std::uint8_t> out_blob, std::size_t &out_size) = 0;

        /**
         * @brief Writes registers from little-endian byte values.
         *
         * Must reject unsupported register IDs or widths, UNAVAILABLE/empty
         * entries and duplicate IDs with ESP_ERR_INVALID_ARG rather than writing
         * a different register. The session validates the wire framing only.
         */
        [[nodiscard]] virtual esp_err_t write_regs(const std::uint8_t *data, std::size_t size) = 0;

        /**
         * @brief Reads target memory into out; the session validates ranges.
         *
         * MEM_WIDTH_DEFAULT is a byte-stream RAM/Flash transfer which may be
         * implemented using safe aligned chunks. An explicit width requires
         * accesses of exactly that width; MMIO requests contain exactly one.
         * ESP_OK means the full requested length was read; there is no partial
         * success. The session also calls this internally for the STOPPED stack
         * snapshot and for software-breakpoint shadow accesses, which are not
         * bounded by attach_info_t::max_memory_transfer (a peer-request limit).
         */
        [[nodiscard]] virtual esp_err_t read_mem(std::uint64_t address, std::uint8_t *out, std::size_t size,
                                                 memory_access_width_t width) = 0;

        /** @copydoc read_mem */
        [[nodiscard]] virtual esp_err_t write_mem(std::uint64_t address, const std::uint8_t *data, std::size_t size,
                                                  memory_access_width_t width) = 0;

        /**
         * @brief Installs the complete hardware breakpoint set (replacement
         *        semantics). Fails without partial application. Entries are
         *        assigned to comparator indices in span order. An empty set
         *        must clear all comparators and return ESP_OK.
         */
        [[nodiscard]] virtual esp_err_t apply_breakpoints(std::span<const bp_entry_t> entries) = 0;

        /**
         * @brief Installs the complete watchpoint set (replacement semantics).
         *        An empty set must clear all watchpoints and return ESP_OK even
         *        when the backend has no watchpoint support.
         */
        [[nodiscard]] virtual esp_err_t apply_watchpoints(std::span<const wp_entry_t> entries) = 0;

        /**
         * @brief Programs the vector catch mask.
         *        VECTOR_CATCH_NONE must clear the mask and return ESP_OK even
         *        when the backend has no vector-catch support.
         */
        [[nodiscard]] virtual esp_err_t set_vector_catch(vector_catch_t mask) = 0;

    protected:
        target_backend_t() = default;
    };

}
