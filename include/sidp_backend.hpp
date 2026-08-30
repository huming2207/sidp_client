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
     * - ESP_FAIL for a protocol-level fault (no-ACK, parity): the session
     *   enters LOST,
     * - ESP_ERR_TIMEOUT when a bounded local wait expired,
     * - ESP_ERR_INVALID_ARG / ESP_ERR_NOT_SUPPORTED for rejected parameters.
     */
    class target_backend_t
    {
    public:
        target_backend_t(const target_backend_t &) = delete;
        target_backend_t &operator=(const target_backend_t &) = delete;
        virtual ~target_backend_t() = default;

        /** @brief Connects and returns the discovered target resources. */
        [[nodiscard]] virtual esp_err_t attach(const attach_params_t &params, attach_info_t &info) = 0;

        /**
         * @brief Restores debug resources and applies the final action.
         *
         * The backend restores breakpoints it programmed; the session restores
         * software-breakpoint patches through write_mem() before calling this.
         */
        [[nodiscard]] virtual esp_err_t detach(detach_action_t action) = 0;

        /**
         * @brief Executes a reset and optionally halts after it.
         * @param kind Reset method requested by the peer.
         * @param halt_after True for RESET_HALT, false for RESET_RUN.
         * @param stop Receives the post-reset stop observation when halt_after.
         */
        [[nodiscard]] virtual esp_err_t reset(reset_kind_t kind, bool halt_after, stop_detect_t &stop) = 0;

        /** @brief Reads the debug state once; true when the target halted. */
        [[nodiscard]] virtual bool poll_halted(stop_detect_t &stop) = 0;

        /**
         * @brief Requests a halt and waits locally until the target confirms.
         * @param timeout_ms Local bounded wait.
         */
        [[nodiscard]] virtual esp_err_t halt(std::uint32_t timeout_ms, stop_detect_t &stop) = 0;

        /** @brief Issues the next resume or single step.
         * @param action Continue, single step, or run to address.
         * @param address Target address for RUN_TO_ADDRESS, otherwise 0.
         */
        [[nodiscard]] virtual esp_err_t resume(run_action_t action, std::uint64_t address) = 0;

        /**
         * @brief Reads registers by ID into little-endian TLV values.
         * @param ids Register IDs; empty set reads the profile core set.
         * @param out_blob Output buffer; implementations must never write
         *        beyond its extent.
         * @param out_size Actual number of bytes written (<= out_blob.size()).
         */
        [[nodiscard]] virtual esp_err_t read_regs(std::span<const register_id_t> ids, std::span<std::uint8_t> out_blob, std::size_t &out_size) = 0;

        /** @brief Writes registers from little-endian byte values. */
        [[nodiscard]] virtual esp_err_t write_regs(const std::uint8_t *data, std::size_t size) = 0;

        /**
         * @brief Reads target memory into out; the session validates ranges.
         *
         * MEM_WIDTH_DEFAULT is a byte-stream RAM/Flash transfer which may be
         * implemented using safe aligned chunks. An explicit width requires
         * accesses of exactly that width; MMIO requests contain exactly one.
         */
        [[nodiscard]] virtual esp_err_t read_mem(std::uint64_t address, std::uint8_t *out, std::size_t size,
                                                 memory_access_width_t width) = 0;

        /** @copydoc read_mem */
        [[nodiscard]] virtual esp_err_t write_mem(std::uint64_t address, const std::uint8_t *data, std::size_t size,
                                                  memory_access_width_t width) = 0;

        /**
         * @brief Installs the complete hardware breakpoint set (replacement
         *        semantics). Fails without partial application. Entries are
         *        assigned to comparator indices in span order.
         */
        [[nodiscard]] virtual esp_err_t apply_breakpoints(std::span<const bp_entry_t> entries) = 0;

        /** @brief Installs the complete watchpoint set (replacement semantics). */
        [[nodiscard]] virtual esp_err_t apply_watchpoints(std::span<const wp_entry_t> entries) = 0;

        /** @brief Programs the vector catch mask. */
        [[nodiscard]] virtual esp_err_t set_vector_catch(vector_catch_t mask) = 0;

    protected:
        target_backend_t() = default;
    };

}
