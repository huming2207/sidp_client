#include "sidp_session.hpp"

#include <cstring>

#include "esp_heap_caps.h"
#include "esp_log.h"

namespace sidp
{

    std::uint8_t sidp_session::gate_column(opcode_t opcode) noexcept
    {
        switch (opcode) {
        case OP_ATTACH:          return GATE_ATTACH;
        case OP_DETACH:          return GATE_DETACH;
        case OP_GET_STATE:       return GATE_GET_STATE;
        case OP_READ_MEMORY:     return GATE_READ_MEM;
        case OP_WRITE_MEMORY:    return GATE_WRITE_MEM;
        case OP_READ_REGISTERS:  return GATE_READ_REGS;
        case OP_WRITE_REGISTERS: return GATE_WRITE_REGS;
        case OP_RUN:             return GATE_RUN;
        case OP_HALT:            return GATE_HALT;
        case OP_RESET_HALT:      return GATE_RESET;
        case OP_RESET_RUN:       return GATE_RESET;
        default:                 return 0xFF;
        }
    }

    std::uint16_t sidp_session::flag_bits(memory_access_flag_t flags) noexcept
    {
        return static_cast<std::uint16_t>(flags);
    }

    sidp_session::sidp_session(target_backend_t &backend, tx_sink_t tx_sink) noexcept : backend(backend), tx_sink(tx_sink)
    {
    }

    sidp_session::~sidp_session() noexcept
    {
        release_storage();
    }

    esp_err_t sidp_session::init() noexcept
    {
        if (storage_ready()) {
            return ESP_ERR_INVALID_STATE;
        }

        scratch = static_cast<std::uint8_t *>(heap_caps_calloc(1, MAX_FRAME_SIZE, MALLOC_CAP_SPIRAM));
        if (scratch == nullptr) {
            ESP_LOGE(TAG, "init: scratch allocation failed");
            release_storage();
            return ESP_ERR_NO_MEM;
        }
        pending_registers = static_cast<std::uint8_t *>(heap_caps_calloc(1, MAX_FRAME_SIZE, MALLOC_CAP_SPIRAM));
        if (pending_registers == nullptr) {
            ESP_LOGE(TAG, "init: pending_registers allocation failed");
            release_storage();
            return ESP_ERR_NO_MEM;
        }
        regions = static_cast<memory_region_t *>(heap_caps_calloc(1, sizeof(memory_region_t) * MAX_MEMORY_REGIONS, MALLOC_CAP_SPIRAM));
        if (regions == nullptr) {
            ESP_LOGE(TAG, "init: regions allocation failed");
            release_storage();
            return ESP_ERR_NO_MEM;
        }
        sw_table = static_cast<sw_bp_t *>(heap_caps_calloc(1, sizeof(sw_bp_t) * MAX_SOFTWARE_BREAKPOINTS, MALLOC_CAP_SPIRAM));
        if (sw_table == nullptr) {
            ESP_LOGE(TAG, "init: sw_table allocation failed");
            release_storage();
            return ESP_ERR_NO_MEM;
        }
        ESP_LOGI(TAG, "init: session buffers allocated in PSRAM");
        return ESP_OK;
    }

    void sidp_session::release_storage() noexcept
    {
        if (sw_table != nullptr) {
            free(sw_table);
            sw_table = nullptr;
        }
        if (regions != nullptr) {
            free(regions);
            regions = nullptr;
        }
        if (pending_registers != nullptr) {
            free(pending_registers);
            pending_registers = nullptr;
        }
        if (scratch != nullptr) {
            free(scratch);
            scratch = nullptr;
        }
        tx_frame_size = 0;
    }

    // ---- Frame assembly -------------------------------------------------------

    std::uint8_t *sidp_session::begin_frame(msg_kind_t kind, opcode_t opcode, std::uint32_t request_id,
                                             std::size_t payload_size) noexcept
    {
        if (scratch == nullptr || payload_size > MAX_FRAME_SIZE - sizeof(msg_header_t)) {
            ESP_LOGE(TAG, "begin_frame: tx frame rejected: storage_ready=%d payload=%u",
                     scratch != nullptr, static_cast<unsigned>(payload_size));
            return nullptr;
        }
        tx_frame_size = sizeof(msg_header_t) + payload_size;
        auto &header = *reinterpret_cast<msg_header_t *>(scratch);
        header.version = PROTOCOL_VERSION;
        header.kind = kind;
        header.opcode = opcode;
        header.request_id = request_id;
        return scratch + sizeof(msg_header_t);
    }

    bool sidp_session::finish_frame() noexcept
    {
        const std::span<std::uint8_t> frame(scratch, tx_frame_size);
        if (tx_sink == nullptr || !crc32_hasher::set_message_crc(frame)) {
            ESP_LOGE(TAG, "finish_frame: tx sink unavailable or frame size invalid");
            return false;
        }
        if (!tx_sink(frame)) {
            ESP_LOGE(TAG, "finish_frame: tx sink rejected frame (%u bytes)", static_cast<unsigned>(tx_frame_size));
            return false;
        }
        return true;
    }

    bool sidp_session::send_response(opcode_t opcode, std::uint32_t request_id, status_t status,
                                       std::span<const std::uint8_t> payload) noexcept
    {
        auto *out = begin_frame(KIND_RESPONSE, opcode, request_id, sizeof(response_prefix_t) + payload.size());
        if (out == nullptr) {
            return false;
        }

        if (status != STATUS_OK) {
            ESP_LOGE(TAG, "send_response: request %u op %u rejected: status %u", request_id, opcode, status);
        }
        auto *prefix = reinterpret_cast<response_prefix_t *>(out);
        prefix->status = status;
        if (!payload.empty()) {
            std::memcpy(out + sizeof(response_prefix_t), payload.data(), payload.size());
        }
        return finish_frame();
    }

    bool sidp_session::send_stopped() noexcept
    {
        auto *out = begin_frame(KIND_EVENT, EVT_STOPPED, 0,
                                sizeof(stopped_event_t) + pending_registers_size + pending_stack_size);
        if (out == nullptr) {
            return false;
        }

        auto *event = reinterpret_cast<stopped_event_t *>(out);
        event->stop_id = stop_id;
        event->core_id = 0;
        event->reason = pending_reason;
        event->flags = STOPPED_FLAG_NONE;
        event->reason_detail = pending_detail;
        event->breakpoint_id = pending_breakpoint_id;
        event->watchpoint_address = pending_watchpoint_address;
        std::size_t entry_count = 0;
        for (std::size_t offset = 0; offset + sizeof(register_value_t) <= pending_registers_size;) {
            const auto *entry = reinterpret_cast<const register_value_t *>(pending_registers + offset);
            const std::size_t entry_size = sizeof(register_value_t) + entry->value_size;
            if (offset + entry_size > pending_registers_size) {
                break;
            }
            ++entry_count;
            offset += entry_size;
        }
        event->register_count = static_cast<std::uint16_t>(entry_count);
        event->stack_length = static_cast<std::uint16_t>(pending_stack_size);
        event->stack_address = pending_stack_address;

        std::uint8_t *blob = out + sizeof(stopped_event_t);
        std::memcpy(blob, pending_registers, pending_registers_size);
        // Stack snapshot bytes were appended after the register blob by enter_halted().
        std::memcpy(blob + pending_registers_size,
                    pending_registers + pending_registers_size, pending_stack_size);

        stop_reported = true;
        return finish_frame();
    }

    bool sidp_session::send_target_lost(target_lost_reason_t reason) noexcept
    {
        auto *out = begin_frame(KIND_EVENT, EVT_TARGET_LOST, 0, sizeof(target_lost_event_t));
        if (out == nullptr) {
            return false;
        }

        auto *event = reinterpret_cast<target_lost_event_t *>(out);
        event->reason = reason;
        event->detail = 0;

        return finish_frame();
    }

    // ---- Dispatch ----------------------------------------------------------------

    void sidp_session::handle_request(std::span<const std::uint8_t> message) noexcept
    {
        if (!transport_intf::is_valid_message_size(message.size())) {
            return;
        }

        const auto &header = *reinterpret_cast<const msg_header_t *>(message.data());
        if (header.version != PROTOCOL_VERSION) {
            ESP_LOGE(TAG, "handle_request: protocol version mismatch: %u", header.version);
            return; // Unsupported version: the transport layer closes the link.
        }
        if (header.kind != KIND_REQUEST || header.request_id == 0) {
            return; // Events and log streams are never valid from the peer.
        }

        const std::span<const std::uint8_t> payload(message.data() + sizeof(msg_header_t),
                                                    message.size() - sizeof(msg_header_t));
        const std::uint32_t request_id = header.request_id;
        ESP_LOGD(TAG, "handle_request: request op=%u id=%u payload=%u", header.opcode, request_id,
                 static_cast<unsigned>(payload.size()));

        const std::uint8_t column = gate_column(header.opcode);
        if (column == 0xFF) {
            // LOST is terminal for every request, known or not (section 15).
            send_response(header.opcode, request_id,
                          state == TARGET_LOST ? STATUS_TARGET_LOST : STATUS_UNSUPPORTED);
            return;
        }

        switch (GATE_TABLE[static_cast<std::size_t>(state)][column]) {
        case gate_type::ATTACH_BUSY:
            send_response(header.opcode, request_id, STATUS_BUSY);
            return;
        case gate_type::DETACHED_ERROR:
            send_response(header.opcode, request_id, STATUS_ERROR);
            return;
        case gate_type::RUNNING_DENY:
            send_response(header.opcode, request_id, STATUS_TARGET_RUNNING);
            return;
        case gate_type::LOST:
            send_response(header.opcode, request_id, STATUS_TARGET_LOST);
            return;
        default:
            break;
        }

        switch (header.opcode) {
        case OP_ATTACH:          op_attach(request_id, payload); break;
        case OP_DETACH:          op_detach(request_id, payload); break;
        case OP_GET_STATE:       op_get_state(request_id, payload); break;
        case OP_READ_MEMORY:     op_read_memory(request_id, payload); break;
        case OP_WRITE_MEMORY:    op_write_memory(request_id, payload); break;
        case OP_READ_REGISTERS:  op_read_registers(request_id, payload); break;
        case OP_WRITE_REGISTERS: op_write_registers(request_id, payload); break;
        case OP_RUN:             op_run(request_id, payload); break;
        case OP_HALT:            op_halt(request_id, payload); break;
        case OP_RESET_HALT:      op_reset_halt(request_id, payload); break;
        case OP_RESET_RUN:       op_reset_run(request_id, payload); break;
        default:                 break;
        }
    }

    // ---- ATTACH / DETACH / GET_STATE -------------------------------------------------

    void sidp_session::op_attach(std::uint32_t request_id, std::span<const std::uint8_t> payload) noexcept
    {
        if (attached) {
            // §7.1: repeated ATTACH on the same connection stays BUSY, no reinit.
            send_response(OP_ATTACH, request_id, STATUS_BUSY);
            return;
        }
        if (payload.size() != sizeof(attach_request_t)) {
            send_response(OP_ATTACH, request_id, STATUS_INVALID_ARGUMENT);
            return;
        }
        if (!storage_ready()) {
            send_response(OP_ATTACH, request_id, STATUS_ERROR);
            return;
        }

        attach_params_t params{};
        const auto &req = *reinterpret_cast<const attach_request_t *>(payload.data());
        params.connect_under_reset = req.connect_under_reset != 0;
        params.halt_after_attach = req.halt_after_attach != 0;

        attach_info_t info{};
        const esp_err_t result = backend.attach(params, info);
        if (result != ESP_OK) {
            send_backend_failure(OP_ATTACH, request_id, result, STATUS_SWD_ERROR);
            return;
        }

        attached = true;
        capabilities = info.capabilities;
        supported_vector_catch = info.supported_vector_catch_mask;
        stop_id = 0;
        clear_debug_state();
        clear_run_tracking();
        sw_count = 0;

        // Copy the full memory map for access validation and stack clipping;
        // the backend's memory_regions span is not guaranteed to outlive this call.
        region_count = 0;
        if (info.memory_regions.size() % sizeof(memory_region_t) != 0 ||
            info.memory_regions.size() / sizeof(memory_region_t) > MAX_MEMORY_REGIONS) {
            (void)backend.detach(DETACH_KEEP_HALTED);
            attached = false;
            send_response(OP_ATTACH, request_id, STATUS_ERROR);
            return;
        }
        const std::size_t source_regions = info.memory_regions.size() / sizeof(memory_region_t);
        for (std::size_t index = 0; index < source_regions; ++index) {
            const auto *region = reinterpret_cast<const memory_region_t *>(info.memory_regions.data() + index * sizeof(memory_region_t));
            if (region_count == MAX_MEMORY_REGIONS) {
                (void)backend.detach(DETACH_KEEP_HALTED);
                attached = false;
                send_response(OP_ATTACH, request_id, STATUS_ERROR);
                return;
            }
            regions[region_count++] = *region;
        }

        // Attach response: prefix + fixed fields + memory regions blob.
        const std::size_t regions_size = info.memory_regions.size();
        auto *resp = reinterpret_cast<attach_response_t *>(
            begin_frame(KIND_RESPONSE, OP_ATTACH, request_id, sizeof(attach_response_t) + regions_size));
        if (resp == nullptr) {
            (void)backend.detach(DETACH_KEEP_HALTED);
            attached = false;
            send_response(OP_ATTACH, request_id, STATUS_ERROR);
            return;
        }
        resp->status = STATUS_OK;
        resp->architecture = info.architecture;
        resp->profile = info.profile;
        resp->address_width = info.address_width;
        resp->reserved0 = 0;
        resp->capabilities = info.capabilities;
        resp->target_id = info.target_id;
        resp->cpu_id = info.cpu_id;
        resp->supported_vector_catch_mask = info.supported_vector_catch_mask;
        resp->hardware_breakpoints = info.hardware_breakpoints;
        resp->hardware_watchpoints = info.hardware_watchpoints;
        resp->memory_region_count = static_cast<std::uint16_t>(regions_size / sizeof(memory_region_t));
        resp->max_memory_transfer = info.max_memory_transfer;
        if (regions_size != 0) {
            std::memcpy(reinterpret_cast<std::uint8_t *>(resp) + sizeof(attach_response_t),
                        info.memory_regions.data(), regions_size);
        }

        if (!finish_frame()) {
            return;
        }
        ESP_LOGI(TAG, "op_attach: attached: arch=%u hw_bp=%u hw_wp=%u regions=%u",
                 info.architecture, info.hardware_breakpoints, info.hardware_watchpoints, region_count);

        // Post-attach halt snapshot (§7.1 ATTACH from DETACHED executes attach).
        if (params.halt_after_attach) {
            stop_detect_t stop{};
            const esp_err_t poll_result = backend.poll_halted(stop);
            if (poll_result != ESP_OK) {
                enter_lost(poll_result == ESP_ERR_TIMEOUT ? TARGET_LOST_TIMEOUT : TARGET_LOST_SWD_FAULT);
                return;
            }
            if (stop.halted) {
                enter_halted(stop);
            } else {
                const esp_err_t halt_result = backend.halt(HALT_TIMEOUT_MS, stop);
                if (halt_result == ESP_OK) {
                    enter_halted(stop);
                } else {
                    if (halt_result == ESP_FAIL) {
                        enter_lost(TARGET_LOST_SWD_FAULT); // response already sent; event follows
                        return;
                    }
                    ESP_LOGE(TAG, "op_attach: attach halt not confirmed: %s", esp_err_to_name(halt_result));
                    // Halt not confirmed: keep polling instead of guessing.
                    state = TARGET_RUNNING;
                    return;
                }
            }
            emit_pending_stopped();
        } else {
            state = TARGET_RUNNING;
        }
    }

    void sidp_session::op_detach(std::uint32_t request_id, std::span<const std::uint8_t> payload) noexcept
    {
        if (payload.size() != sizeof(detach_request_t)) {
            send_response(OP_DETACH, request_id, STATUS_INVALID_ARGUMENT);
            return;
        }
        const detach_action_t action = reinterpret_cast<const detach_request_t *>(payload.data())->action;
        if (action != DETACH_KEEP_HALTED && action != DETACH_RESUME) {
            send_response(OP_DETACH, request_id, STATUS_INVALID_ARGUMENT);
            return;
        }

        // Internal halt for cleanup when RUNNING; not reported as STOPPED (§10.2).
        if (state == TARGET_RUNNING) {
            stop_detect_t stop{};
            const esp_err_t halt_result = backend.halt(HALT_TIMEOUT_MS, stop);
            if (halt_result != ESP_OK) {
                send_backend_failure(OP_DETACH, request_id, halt_result, STATUS_TIMEOUT);
                return;
            }
        }

        // Cleanup failure keeps the session attached (section 10.2).
        if (!sw_bp_restore_all()) {
            send_response(OP_DETACH, request_id, STATUS_SWD_ERROR);
            return;
        }
        const esp_err_t result = backend.detach(action);
        if (result != ESP_OK) {
            send_response(OP_DETACH, request_id, STATUS_SWD_ERROR);
            return;
        }

        ESP_LOGI(TAG, "op_detach: detached");
        send_response(OP_DETACH, request_id, STATUS_OK);
        attached = false;
        state = TARGET_DETACHED;
        stop_id = 0;
        clear_debug_state();
        clear_run_tracking();
    }

    void sidp_session::op_get_state(std::uint32_t request_id, std::span<const std::uint8_t> payload) noexcept
    {
        if (!payload.empty()) {
            send_response(OP_GET_STATE, request_id, STATUS_INVALID_ARGUMENT);
            return;
        }

        // The get_state_response_t itself starts with the response prefix
        // (protocol section 7), so the frame is assembled directly.
        auto *resp = reinterpret_cast<get_state_response_t *>(
            begin_frame(KIND_RESPONSE, OP_GET_STATE, request_id, sizeof(get_state_response_t)));
        if (resp == nullptr) {
            return;
        }
        resp->status = STATUS_OK;
        resp->target_state = state;
        resp->reserved[0] = resp->reserved[1] = resp->reserved[2] = 0;
        resp->stop_id = (state == TARGET_HALTED) ? stop_id : 0;

        (void)finish_frame();
    }

    void sidp_session::op_read_memory(std::uint32_t request_id, std::span<const std::uint8_t> payload) noexcept
    {
        if (payload.size() != sizeof(read_memory_request_t)) {
            send_response(OP_READ_MEMORY, request_id, STATUS_INVALID_ARGUMENT);
            return;
        }
        const auto &req = *reinterpret_cast<const read_memory_request_t *>(payload.data());

        const std::uint16_t flags = flag_bits(req.flags);
        const bool require_halted = (flags & static_cast<std::uint16_t>(MEM_ACCESS_REQUIRE_HALTED)) != 0;
        const bool allow_running = (flags & static_cast<std::uint16_t>(MEM_ACCESS_ALLOW_RUNNING)) != 0;
        if (require_halted == allow_running) {
            send_response(OP_READ_MEMORY, request_id, STATUS_INVALID_ARGUMENT);
            return;
        }

        if (require_halted) {
            if (state != TARGET_HALTED) {
                send_response(OP_READ_MEMORY, request_id,
                              state == TARGET_RUNNING ? STATUS_TARGET_RUNNING : STATUS_TARGET_LOST);
                return;
            }
            if (req.stop_id != stop_id) {
                send_response(OP_READ_MEMORY, request_id, STATUS_STALE_STOP);
                return;
            }
        } else if (req.stop_id != 0) {
            // allow_running requires stop_id = 0 (section 9).
            send_response(OP_READ_MEMORY, request_id, STATUS_INVALID_ARGUMENT);
            return;
        }

        if (req.length == 0 || req.length > MAX_FRAME_SIZE - sizeof(msg_header_t) - sizeof(read_memory_response_t)) {
            send_response(OP_READ_MEMORY, request_id, STATUS_INVALID_ARGUMENT);
            return;
        }

        const status_t map_status = validate_memory_access(req.address, req.length, req.flags, req.access_width, false);
        if (map_status != STATUS_OK) {
            send_response(OP_READ_MEMORY, request_id, map_status);
            return;
        }

        // The backend reads straight into the frame payload area: the CRC is
        // only applied by finish_frame(), so the frame may be assembled
        // before the payload is valid.
        auto *resp = reinterpret_cast<read_memory_response_t *>(
            begin_frame(KIND_RESPONSE, OP_READ_MEMORY, request_id, sizeof(read_memory_response_t) + req.length));
        if (resp == nullptr) {
            return;
        }
        std::uint8_t *data = reinterpret_cast<std::uint8_t *>(resp) + sizeof(read_memory_response_t);
        const esp_err_t result = backend.read_mem(req.address, data, req.length, req.access_width);
        if (result != ESP_OK) {
            // The staged frame is discarded; the failure path rebuilds it.
            send_backend_failure(OP_READ_MEMORY, request_id, result, STATUS_ADDRESS_ERROR);
            return;
        }

        // Shadow substitution: patched halfwords read back as original bytes (§10.4).
        sw_bp_substitute_read(req.address, data, req.length);

        resp->status = STATUS_OK;
        resp->address = req.address;
        resp->completed_length = req.length;

        (void)finish_frame();
    }

    void sidp_session::op_write_memory(std::uint32_t request_id, std::span<const std::uint8_t> payload) noexcept
    {
        if (payload.size() < sizeof(write_memory_request_t)) {
            send_response(OP_WRITE_MEMORY, request_id, STATUS_INVALID_ARGUMENT);
            return;
        }
        const auto &req = *reinterpret_cast<const write_memory_request_t *>(payload.data());

        const std::uint16_t flags = flag_bits(req.flags);
        const bool require_halted = (flags & static_cast<std::uint16_t>(MEM_ACCESS_REQUIRE_HALTED)) != 0;
        const bool allow_running = (flags & static_cast<std::uint16_t>(MEM_ACCESS_ALLOW_RUNNING)) != 0;
        if (require_halted == allow_running ||
            payload.size() != sizeof(write_memory_request_t) + req.length) {
            send_response(OP_WRITE_MEMORY, request_id, STATUS_INVALID_ARGUMENT);
            return;
        }

        if (require_halted) {
            if (state != TARGET_HALTED) {
                send_response(OP_WRITE_MEMORY, request_id,
                              state == TARGET_RUNNING ? STATUS_TARGET_RUNNING : STATUS_TARGET_LOST);
                return;
            }
            if (req.stop_id != stop_id) {
                send_response(OP_WRITE_MEMORY, request_id, STATUS_STALE_STOP);
                return;
            }
        } else if (req.stop_id != 0) {
            // allow_running requires stop_id = 0 (section 9).
            send_response(OP_WRITE_MEMORY, request_id, STATUS_INVALID_ARGUMENT);
            return;
        }

        const status_t map_status = validate_memory_access(req.address, req.length, req.flags, req.access_width, true);
        if (map_status != STATUS_OK) {
            send_response(OP_WRITE_MEMORY, request_id, map_status);
            return;
        }

        const std::uint8_t *data = payload.data() + sizeof(write_memory_request_t);

        // Build the on-target write image: every installed patch's first
        // halfword is preserved so the breakpoint stays armed (section 10.4).
        // scratch doubles as the frame assembly buffer; the image is fully
        // consumed by backend.write_mem() before any response is assembled.
        std::uint8_t *write_image = scratch;
        std::memcpy(write_image, data, req.length);
        for (std::size_t index = 0; index < sw_count; ++index) {
            const sw_bp_t &entry = sw_table[index];
            if (!entry.installed) {
                continue;
            }
            if (entry.address + 2 <= req.address || req.address + req.length <= entry.address) {
                continue;
            }
            constexpr std::uint8_t BKPT_PATCH[2] = {0x00, 0xBE};
            for (std::size_t offset = 0; offset < req.length; ++offset) {
                const std::uint64_t absolute = req.address + offset;
                if (absolute >= entry.address && absolute < entry.address + 2) {
                    write_image[offset] = BKPT_PATCH[absolute - entry.address];
                }
            }
        }

        const esp_err_t result = backend.write_mem(req.address, write_image, req.length, req.access_width);
        if (result != ESP_OK) {
            send_backend_failure(OP_WRITE_MEMORY, request_id, result, STATUS_ADDRESS_ERROR);
            return;
        }

        // Shadow coherency only after a successful write: overlapping bytes
        // update the shadow while the target-side patch stays armed.
        sw_bp_write_overlap(req.address, data, req.length);

        send_response(OP_WRITE_MEMORY, request_id, STATUS_OK);
    }

    // ---- Registers --------------------------------------------------------------------------

    void sidp_session::op_read_registers(std::uint32_t request_id, std::span<const std::uint8_t> payload) noexcept
    {
        if (payload.size() < sizeof(read_registers_request_t)) {
            send_response(OP_READ_REGISTERS, request_id, STATUS_INVALID_ARGUMENT);
            return;
        }
        const auto &req = *reinterpret_cast<const read_registers_request_t *>(payload.data());
        if (payload.size() != sizeof(read_registers_request_t) + static_cast<std::size_t>(req.register_count) * sizeof(register_id_t)) {
            send_response(OP_READ_REGISTERS, request_id, STATUS_INVALID_ARGUMENT);
            return;
        }
        // State gating is handled by the dispatch table; only HALTED reaches here.
        if (req.stop_id != stop_id) {
            send_response(OP_READ_REGISTERS, request_id, STATUS_STALE_STOP);
            return;
        }

        // Alignment-safe copy of the requested register IDs: the wire buffer
        // carries no alignment guarantee for 2-byte register_id_t. The IDs
        // must not live in scratch: the backend writes the register blob
        // straight into the frame payload area, which overlaps scratch_ids().
        static constexpr std::size_t MAX_REGISTER_IDS = 64;
        if (static_cast<std::size_t>(req.register_count) > MAX_REGISTER_IDS) {
            send_response(OP_READ_REGISTERS, request_id, STATUS_INVALID_ARGUMENT);
            return;
        }
        register_id_t ids[MAX_REGISTER_IDS];
        const std::uint8_t *id_bytes = payload.data() + sizeof(read_registers_request_t);
        for (std::size_t index = 0; index < req.register_count; ++index) {
            std::memcpy(&ids[index], id_bytes + index * sizeof(register_id_t), sizeof(register_id_t));
        }

        // The register blob is read directly into the frame payload area.
        auto *resp = reinterpret_cast<read_registers_response_t *>(
            begin_frame(KIND_RESPONSE, OP_READ_REGISTERS, request_id,
                        MAX_FRAME_SIZE - sizeof(msg_header_t)));
        if (resp == nullptr) {
            send_response(OP_READ_REGISTERS, request_id, STATUS_SWD_ERROR);
            return;
        }
        constexpr std::size_t BLOB_CAPACITY = MAX_FRAME_SIZE - sizeof(msg_header_t) - sizeof(read_registers_response_t);
        std::size_t out_size = 0;
        const std::span<std::uint8_t> blob(reinterpret_cast<std::uint8_t *>(resp) + sizeof(read_registers_response_t),
                                           BLOB_CAPACITY);
        const esp_err_t result = backend.read_regs({ids, req.register_count}, blob, out_size);
        if (result != ESP_OK) {
            send_backend_failure(OP_READ_REGISTERS, request_id, result, STATUS_SWD_ERROR);
            return;
        }
        if (out_size > BLOB_CAPACITY) {
            send_response(OP_READ_REGISTERS, request_id, STATUS_SWD_ERROR);
            return;
        }
        normalize_pc_in_register_blob(blob.data(), out_size);
        tx_frame_size = sizeof(msg_header_t) + sizeof(read_registers_response_t) + out_size;

        // Count actual entries in the blob (register_count == 0 requested the
        // full profile set, so the response count comes from the blob).
        std::size_t entry_count = 0;
        for (std::size_t offset = 0; offset + sizeof(register_value_t) <= out_size;) {
            const auto *entry = reinterpret_cast<const register_value_t *>(reinterpret_cast<const std::uint8_t *>(resp) + sizeof(read_registers_response_t) + offset);
            const std::size_t entry_size = sizeof(register_value_t) + entry->value_size;
            if (offset + entry_size > out_size) {
                break;
            }
            ++entry_count;
            offset += entry_size;
        }

        resp->status = STATUS_OK;
        resp->stop_id = stop_id;
        resp->register_count = static_cast<std::uint16_t>(entry_count);
        resp->reserved = 0;

        (void)finish_frame();
    }

    void sidp_session::op_write_registers(std::uint32_t request_id, std::span<const std::uint8_t> payload) noexcept
    {
        if (payload.size() < sizeof(write_registers_request_t)) {
            send_response(OP_WRITE_REGISTERS, request_id, STATUS_INVALID_ARGUMENT);
            return;
        }
        const auto &req = *reinterpret_cast<const write_registers_request_t *>(payload.data());
        if (req.stop_id != stop_id) {
            send_response(OP_WRITE_REGISTERS, request_id, STATUS_STALE_STOP);
            return;
        }

        const esp_err_t result = backend.write_regs(payload.data() + sizeof(write_registers_request_t),
                                                             payload.size() - sizeof(write_registers_request_t));
        if (result != ESP_OK) {
            send_backend_failure(OP_WRITE_REGISTERS, request_id, result, STATUS_SWD_ERROR);
            return;
        }
        update_step_over_after_register_write(payload.data() + sizeof(write_registers_request_t),
                                              payload.size() - sizeof(write_registers_request_t),
                                              req.register_count);
        send_response(OP_WRITE_REGISTERS, request_id, STATUS_OK);
    }

    // ---- Run control ------------------------------------------------------------------------

    void sidp_session::op_run(std::uint32_t request_id, std::span<const std::uint8_t> payload) noexcept
    {
        if (payload.size() < sizeof(run_request_t)) {
            send_response(OP_RUN, request_id, STATUS_INVALID_ARGUMENT);
            return;
        }
        const auto &req = *reinterpret_cast<const run_request_t *>(payload.data());

        if (req.stop_id != stop_id) {
            send_response(OP_RUN, request_id, STATUS_STALE_STOP);
            return;
        }
        if ((req.action != RUN_CONTINUE && req.action != RUN_SINGLE_STEP && req.action != RUN_TO_ADDRESS) ||
            (req.action != RUN_TO_ADDRESS && req.run_to_address != 0) ||
            (req.action == RUN_TO_ADDRESS && req.run_to_address == 0)) {
            send_response(OP_RUN, request_id, STATUS_INVALID_ARGUMENT);
            return;
        }
        if (req.action == RUN_TO_ADDRESS &&
            ((req.run_to_address & 1u) != 0 || !is_executable_range(req.run_to_address, 2, false))) {
            send_response(OP_RUN, request_id, STATUS_ADDRESS_ERROR);
            return;
        }
        const auto requested_vector_catch = static_cast<std::uint32_t>(req.vector_catch_mask);
        const auto supported_vector_bits = static_cast<std::uint32_t>(supported_vector_catch);
        if ((requested_vector_catch & ~supported_vector_bits) != 0) {
            send_response(OP_RUN, request_id, STATUS_UNSUPPORTED);
            return;
        }

        // Trailing tables: breakpoints then watchpoints (§10).
        const std::size_t fixed = sizeof(run_request_t);
        const std::size_t bp_bytes = static_cast<std::size_t>(req.breakpoint_count) * sizeof(breakpoint_t);
        const std::size_t wp_bytes = static_cast<std::size_t>(req.watchpoint_count) * sizeof(watchpoint_t);
        if (payload.size() != fixed + bp_bytes + wp_bytes) {
            send_response(OP_RUN, request_id, STATUS_INVALID_ARGUMENT);
            return;
        }

        constexpr std::size_t MAX_WP = 16;
        if (req.breakpoint_count > MAX_HARDWARE_BREAKPOINTS + MAX_SOFTWARE_BREAKPOINTS ||
            req.watchpoint_count > MAX_WP) {
            send_response(OP_RUN, request_id, STATUS_NO_BREAKPOINT_SLOT);
            return;
        }

        bp_entry_t hw_entries[MAX_HARDWARE_BREAKPOINTS]{};
        std::size_t requested_hw_count = 0;
        bp_entry_t sw_entries[MAX_SOFTWARE_BREAKPOINTS]{};
        std::size_t requested_sw_count = 0;

        const auto *bps = reinterpret_cast<const breakpoint_t *>(payload.data() + fixed);
        for (std::size_t index = 0; index < req.breakpoint_count; ++index) {
            const breakpoint_t &entry = bps[index];
            if (entry.breakpoint_id == 0 || entry.enabled != 1 ||
                (entry.kind != BREAKPOINT_HARDWARE && entry.kind != BREAKPOINT_SOFTWARE)) {
                send_response(OP_RUN, request_id, STATUS_INVALID_ARGUMENT);
                return;
            }
            if (entry.kind == BREAKPOINT_SOFTWARE) {
                if ((entry.instruction_size != 2 && entry.instruction_size != 4) ||
                    (entry.address & 1u) != 0 ||
                    !is_executable_range(entry.address, entry.instruction_size, true)) {
                    send_response(OP_RUN, request_id, STATUS_INVALID_ARGUMENT);
                    return;
                }
                if (requested_sw_count == MAX_SOFTWARE_BREAKPOINTS) {
                    send_response(OP_RUN, request_id, STATUS_NO_BREAKPOINT_SLOT);
                    return;
                }
                sw_entries[requested_sw_count++] = bp_entry_t{entry.breakpoint_id, entry.address, entry.kind,
                                                              entry.instruction_size, entry.temporary};
            } else {
                if (entry.instruction_size != 0 || (entry.address & 1u) != 0 ||
                    !is_executable_range(entry.address, 2, false)) {
                    send_response(OP_RUN, request_id, STATUS_INVALID_ARGUMENT);
                    return;
                }
                if (requested_hw_count == MAX_HARDWARE_BREAKPOINTS) {
                    send_response(OP_RUN, request_id, STATUS_NO_BREAKPOINT_SLOT);
                    return;
                }
                hw_entries[requested_hw_count++] = bp_entry_t{entry.breakpoint_id, entry.address, entry.kind,
                                                              0, entry.temporary};
            }
        }

        const auto *wps = reinterpret_cast<const watchpoint_t *>(payload.data() + fixed + bp_bytes);
        wp_entry_t wp_entries[MAX_WP]{};
        for (std::size_t index = 0; index < req.watchpoint_count; ++index) {
            const watchpoint_t &entry = wps[index];
            if (entry.watchpoint_id == 0 || entry.enabled != 1 ||
                (entry.access != WATCH_READ && entry.access != WATCH_WRITE && entry.access != WATCH_READ_WRITE) ||
                (entry.size != 1 && entry.size != 2 && entry.size != 4)) {
                send_response(OP_RUN, request_id, STATUS_INVALID_ARGUMENT);
                return;
            }
            if ((entry.address % entry.size) != 0) {
                send_response(OP_RUN, request_id, STATUS_ALIGNMENT_ERROR);
                return;
            }
            wp_entries[index] = wp_entry_t{entry.watchpoint_id, entry.address, entry.access, entry.size};
        }

        // All-or-nothing installation: software shadow, hardware, watchpoints,
        // vector catch, then resume. Any failure rolls back to a zero
        // configuration and keeps the target halted.
        if (!sw_bp_apply({sw_entries, requested_sw_count})) {
            fail_run(request_id, STATUS_INVALID_ARGUMENT);
            return;
        }
        if (backend.apply_breakpoints({hw_entries, requested_hw_count}) != ESP_OK) {
            fail_run(request_id, STATUS_NO_BREAKPOINT_SLOT);
            return;
        }
        hw_count = requested_hw_count;
        for (std::size_t index = 0; index < hw_count; ++index) {
            hw_breakpoint_ids[index] = hw_entries[index].breakpoint_id;
        }
        if (backend.apply_watchpoints({wp_entries, req.watchpoint_count}) != ESP_OK) {
            fail_run(request_id, STATUS_NO_WATCHPOINT_SLOT);
            return;
        }
        if (backend.set_vector_catch(req.vector_catch_mask) != ESP_OK) {
            fail_run(request_id, STATUS_UNSUPPORTED);
            return;
        }

        // §10.4: when halted on a software breakpoint, run the internal
        // step-over sequence before the real resume.
        stop_detect_t step_stop{};
        target_lost_reason_t step_lost_reason = TARGET_LOST_SWD_FAULT;
        const step_over_t step = execute_step_over(req.action, step_stop, step_lost_reason);
        if (step == step_over_t::FAILED || step == step_over_t::FAILED_LOST) {
            const bool rolled_back = rollback_run_config();
            if (!rolled_back && step == step_over_t::FAILED) {
                send_response(OP_RUN, request_id, STATUS_SWD_ERROR);
                return;
            }
            send_response(OP_RUN, request_id,
                          step == step_over_t::FAILED_LOST ? STATUS_TARGET_LOST : STATUS_SWD_ERROR);
            if (step == step_over_t::FAILED_LOST) {
                enter_lost(step_lost_reason); // response first, event follows (§3)
            }
            return;
        }

        if (step == step_over_t::STOPPED_STEP || step == step_over_t::STOPPED_REAL) {
            // The first single-step was issued; report the real outcome.
            send_response(OP_RUN, request_id, STATUS_OK);
            enter_halted(step_stop);
            if (step == step_over_t::STOPPED_STEP) {
                pending_reason = STOP_SINGLE_STEP;
            }
            emit_pending_stopped();
            return;
        }

        const esp_err_t resume_result = backend.resume(req.action, req.run_to_address);
        if (resume_result != ESP_OK) {
            const bool rolled_back = rollback_run_config();
            if (!rolled_back && resume_result != ESP_FAIL) {
                send_response(OP_RUN, request_id, STATUS_SWD_ERROR);
            } else {
                send_backend_failure(OP_RUN, request_id, resume_result, STATUS_SWD_ERROR);
            }
            return;
        }

        running_action = req.action;
        run_to_active = req.action == RUN_TO_ADDRESS;
        active_run_to_address = run_to_active ? req.run_to_address : 0;
        state = TARGET_RUNNING;
        send_response(OP_RUN, request_id, STATUS_OK);

        // The target may halt immediately; check once so STOPPED always
        // follows the RUN response (§10.1).
        stop_detect_t stop{};
        const esp_err_t poll_result = backend.poll_halted(stop);
        if (poll_result != ESP_OK) {
            enter_lost(poll_result == ESP_ERR_TIMEOUT ? TARGET_LOST_TIMEOUT : TARGET_LOST_SWD_FAULT);
        } else if (stop.halted) {
            enter_halted(stop);
            if (req.action == RUN_SINGLE_STEP) {
                pending_reason = STOP_SINGLE_STEP;
            }
            emit_pending_stopped();
        }
    }

    void sidp_session::op_halt(std::uint32_t request_id, std::span<const std::uint8_t> payload) noexcept
    {
        if (!payload.empty()) {
            send_response(OP_HALT, request_id, STATUS_INVALID_ARGUMENT);
            return;
        }

        // Stable state: this stop generation is already reported.
        if (state == TARGET_HALTED && stop_reported) {
            send_response(OP_HALT, request_id, STATUS_TARGET_HALTED);
            return;
        }
        // Pending unreported stop: deliver HALT response, then that STOPPED
        // with its existing generation and real reason (section 10.1).
        if (state == TARGET_HALTED && !stop_reported) {
            send_response(OP_HALT, request_id, STATUS_OK);
            emit_pending_stopped();
            return;
        }

        stop_detect_t stop{};
        const esp_err_t result = backend.halt(HALT_TIMEOUT_MS, stop);
        if (result != ESP_OK) {
            send_backend_failure(OP_HALT, request_id, result, STATUS_TIMEOUT);
            return;
        }

        // A queued unreported stop is delivered after the HALT response.
        enter_halted(stop);
        send_response(OP_HALT, request_id, STATUS_OK);
        emit_pending_stopped();
    }

    void sidp_session::op_reset_halt(std::uint32_t request_id, std::span<const std::uint8_t> payload) noexcept
    {
        if (payload.size() != sizeof(reset_request_t)) {
            send_response(OP_RESET_HALT, request_id, STATUS_INVALID_ARGUMENT);
            return;
        }
        const auto kind = reinterpret_cast<const reset_request_t *>(payload.data())->kind;
        if (kind != RESET_SYSTEM && kind != RESET_NRST) {
            send_response(OP_RESET_HALT, request_id, STATUS_INVALID_ARGUMENT);
            return;
        }
        const auto caps = static_cast<std::uint32_t>(capabilities);
        if ((caps & CAP_RESET_HALT) == 0 ||
            (kind == RESET_SYSTEM && (caps & CAP_RESET_SYSTEM) == 0) ||
            (kind == RESET_NRST && (caps & CAP_RESET_NRST) == 0)) {
            send_response(OP_RESET_HALT, request_id, STATUS_UNSUPPORTED);
            return;
        }

        // Restore patches before reset so no BKPT survives it (§10.1 RESET_HALT).
        if (!sw_bp_restore_all()) {
            send_response(OP_RESET_HALT, request_id, STATUS_SWD_ERROR);
            return;
        }

        stop_detect_t stop{};
        const esp_err_t result = backend.reset(kind, true, stop);
        if (result != ESP_OK) {
            send_backend_failure(OP_RESET_HALT, request_id, result, STATUS_TIMEOUT);
            return;
        }

        // Reset invalidates the old stop cache, but the stop generation stays
        // monotonic for the lifetime of this connection (§7).
        clear_debug_state();
        clear_run_tracking();
        enter_halted(stop);
        send_response(OP_RESET_HALT, request_id, STATUS_OK);
        emit_pending_stopped();
    }

    void sidp_session::op_reset_run(std::uint32_t request_id, std::span<const std::uint8_t> payload) noexcept
    {
        if (payload.size() != sizeof(reset_request_t)) {
            send_response(OP_RESET_RUN, request_id, STATUS_INVALID_ARGUMENT);
            return;
        }
        const auto kind = reinterpret_cast<const reset_request_t *>(payload.data())->kind;
        if (kind != RESET_SYSTEM && kind != RESET_NRST) {
            send_response(OP_RESET_RUN, request_id, STATUS_INVALID_ARGUMENT);
            return;
        }
        const auto caps = static_cast<std::uint32_t>(capabilities);
        if ((caps & CAP_RESET_RUN) == 0 ||
            (kind == RESET_SYSTEM && (caps & CAP_RESET_SYSTEM) == 0) ||
            (kind == RESET_NRST && (caps & CAP_RESET_NRST) == 0)) {
            send_response(OP_RESET_RUN, request_id, STATUS_UNSUPPORTED);
            return;
        }

        if (!sw_bp_restore_all()) {
            send_response(OP_RESET_RUN, request_id, STATUS_SWD_ERROR);
            return;
        }
        stop_detect_t unused{};
        const esp_err_t result = backend.reset(kind, false, unused);
        if (result != ESP_OK) {
            send_backend_failure(OP_RESET_RUN, request_id, result, STATUS_TIMEOUT);
            return;
        }

        clear_debug_state();
        clear_run_tracking();
        state = TARGET_RUNNING;
        send_response(OP_RESET_RUN, request_id, STATUS_OK);
    }
    
    void sidp_session::handle_poll() noexcept
    {
        if (state != TARGET_RUNNING || !attached) {
            return;
        }

        stop_detect_t stop{};
        const esp_err_t poll_result = backend.poll_halted(stop);
        if (poll_result != ESP_OK) {
            enter_lost(poll_result == ESP_ERR_TIMEOUT ? TARGET_LOST_TIMEOUT : TARGET_LOST_SWD_FAULT);
        } else if (stop.halted) {
            enter_halted(stop);
            emit_pending_stopped();
        }
    }

    void sidp_session::handle_disconnect() noexcept
    {
        if (!attached) {
            return;
        }
        if (state == TARGET_RUNNING) {
            stop_detect_t stop{};
            (void)backend.halt(HALT_TIMEOUT_MS, stop); // internal, unreported
        }
        (void)sw_bp_restore_all();
        (void)backend.detach(DETACH_KEEP_HALTED);
        attached = false;
        state = TARGET_DETACHED;
        stop_id = 0;
        clear_debug_state();
        clear_run_tracking();
    }

    // ---- Stop pipeline --------------------------------------------------------------------

    void sidp_session::enter_halted(const stop_detect_t &stop) noexcept
    {
        if (state == TARGET_LOST) {
            return;
        }

        stop_reason_t reason = STOP_UNKNOWN;
        std::uint32_t breakpoint_id = 0;
        std::uint64_t watchpoint_address = 0;
        sw_step_over_pending = false;
        sw_step_over_address = 0;

        constexpr std::uint32_t DFSR_HALTED = 1u << 0;
        constexpr std::uint32_t DFSR_BKPT = 1u << 1;
        constexpr std::uint32_t DFSR_DWTTRAP = 1u << 2;
        constexpr std::uint32_t DFSR_VCATCH = 1u << 3;

        if (stop.lockup) {
            reason = STOP_LOCKUP;
        } else if (stop.watchpoint_match || (stop.dfsr & DFSR_DWTTRAP) != 0) {
            reason = STOP_WATCHPOINT;
            watchpoint_address = stop.watchpoint_address;
        } else if (run_to_active && stop.comparator_match && stop.pc == active_run_to_address) {
            reason = STOP_RUN_TO_ADDRESS;
        } else if (!stop.comparator_match && (stop.dfsr & DFSR_BKPT) != 0) {
            sw_bp_t *sw = sw_bp_find_by_halt_pc(stop.pc);
            reason = STOP_BREAKPOINT;
            if (sw != nullptr) {
                breakpoint_id = sw->id;
                sw_step_over_pending = true;
                sw_step_over_address = sw->address;
            }
        } else if (stop.comparator_match || (stop.dfsr & DFSR_BKPT) != 0) {
            reason = STOP_BREAKPOINT;
            if (stop.comparator_match && stop.comparator_index < hw_count) {
                breakpoint_id = hw_breakpoint_ids[stop.comparator_index];
            }
        } else if (stop.fault) {
            reason = STOP_FAULT;
        } else if ((stop.dfsr & DFSR_VCATCH) != 0) {
            reason = STOP_VECTOR_CATCH;
        } else if ((stop.dfsr & DFSR_HALTED) != 0 && running_action == RUN_SINGLE_STEP) {
            reason = STOP_SINGLE_STEP;
        } else {
            reason = STOP_USER_HALT;
        }

        ESP_LOGD(TAG, "enter_halted: target halted: reason=%u dfsr=0x%x pc=0x%llx bp=%u wp=0x%llx",
                 static_cast<unsigned>(reason), stop.dfsr,
                 static_cast<unsigned long long>(stop.pc), breakpoint_id,
                 static_cast<unsigned long long>(watchpoint_address));
        if (++stop_id == 0) {
            enter_lost(TARGET_LOST_TIMEOUT); // generation wrap: terminal (§7)
            return;
        }

        state = TARGET_HALTED;
        stop_reported = false;
        pending_reason = reason;
        pending_detail = stop.dfsr;
        pending_breakpoint_id = breakpoint_id;
        pending_watchpoint_address = watchpoint_address;
        pending_registers_size = 0;
        pending_stack_size = 0;
        pending_stack_address = 0;
        running_action = RUN_CONTINUE;
        run_to_active = false;
        active_run_to_address = 0;

        // Full snapshot only when the capability is advertised (section 11);
        // otherwise STOPPED degrades to reason + stop_id + hit info only.
        if ((static_cast<std::uint32_t>(capabilities) & CAP_STOP_SNAPSHOT) != 0) {
            std::size_t out_size = 0;
            constexpr std::size_t SNAPSHOT_BLOB_CAPACITY = MAX_FRAME_SIZE - sizeof(msg_header_t) - sizeof(stopped_event_t);
            if (backend.read_regs({}, std::span<std::uint8_t>(pending_registers, SNAPSHOT_BLOB_CAPACITY), out_size) == ESP_OK &&
                out_size <= SNAPSHOT_BLOB_CAPACITY) {
                pending_registers_size = out_size;
                normalize_pc_in_register_blob(pending_registers, pending_registers_size);
                capture_stack_snapshot();
            }
        }
    }

    void sidp_session::emit_pending_stopped() noexcept
    {
        if (stop_reported || state != TARGET_HALTED) {
            return;
        }
        send_stopped();
    }

    void sidp_session::enter_lost(target_lost_reason_t reason) noexcept
    {
        if (state == TARGET_LOST) {
            return;
        }
        ESP_LOGE(TAG, "enter_lost: target lost (reason %d)", static_cast<int>(reason));
        state = TARGET_LOST;
        send_target_lost(reason);
    }

    void sidp_session::send_backend_failure(opcode_t opcode, std::uint32_t request_id,
                                              esp_err_t result, status_t fallback) noexcept
    {
        ESP_LOGE(TAG, "send_backend_failure: backend op %u failed: %s", opcode, esp_err_to_name(result));
        if (result == ESP_FAIL) {
            send_response(opcode, request_id, STATUS_TARGET_LOST);
            enter_lost(TARGET_LOST_SWD_FAULT); // response first, event follows (section 3)
            return;
        }
        send_response(opcode, request_id, result == ESP_ERR_TIMEOUT ? STATUS_TIMEOUT : fallback);
    }

    void sidp_session::capture_stack_snapshot() noexcept
    {
        // Parse SP out of the register blob (section 11: [SP-64, SP+512)).
        std::uint32_t sp = 0;
        bool found = false;
        for (std::size_t offset = 0; offset + sizeof(register_value_t) <= pending_registers_size;) {
            const auto *entry = reinterpret_cast<const register_value_t *>(pending_registers + offset);
            const std::size_t entry_size = sizeof(register_value_t) + entry->value_size;
            if (offset + entry_size > pending_registers_size) {
                break;
            }
            if (entry->register_id == ARM_REG_SP && entry->value_size == sizeof(sp)) {
                std::memcpy(&sp, reinterpret_cast<const std::uint8_t *>(entry) + sizeof(register_value_t), sizeof(sp));
                found = true;
                break;
            }
            offset += entry_size;
        }
        if (!found) {
            return;
        }

        // Clip the range to the RAM region containing SP (64-bit math; the
        // register blob must always leave room for the event headers).
        const memory_region_t *region = nullptr;
        for (std::size_t index = 0; index < region_count; ++index) {
            const memory_region_t &candidate = regions[index];
            if (candidate.type == MEMORY_RAM && sp >= candidate.start && sp < candidate.start + candidate.length) {
                region = &candidate;
                break;
            }
        }
        if (region == nullptr) {
            return;
        }

        const std::uint64_t region_end = region->start + region->length;
        std::uint64_t start = static_cast<std::uint64_t>(sp) - STACK_SNAPSHOT_BELOW;
        std::uint64_t end = static_cast<std::uint64_t>(sp) + STACK_SNAPSHOT_ABOVE;
        if (start < region->start) {
            start = region->start;
        }
        if (end > region_end) {
            end = region_end;
        }
        if (end <= start) {
            return;
        }

        std::size_t size = static_cast<std::size_t>(end - start);
        const std::size_t capacity = MAX_FRAME_SIZE - sizeof(msg_header_t) - sizeof(stopped_event_t) -
                                     pending_registers_size;
        if (size > capacity) {
            size = capacity;
        }
        if (backend.read_mem(start, pending_registers + pending_registers_size, size, MEM_WIDTH_DEFAULT) != ESP_OK) {
            return;
        }
        pending_stack_address = start;
        pending_stack_size = size;
    }

    bool sidp_session::write_current_pc(std::uint32_t pc) noexcept
    {
        std::uint8_t data[sizeof(register_value_t) + sizeof(pc)]{};
        auto *entry = reinterpret_cast<register_value_t *>(data);
        entry->register_id = ARM_REG_PC;
        entry->value_size = sizeof(pc);
        entry->flags = REGISTER_VALUE_FLAG_NONE;
        std::memcpy(data + sizeof(register_value_t), &pc, sizeof(pc));
        return backend.write_regs(data, sizeof(data)) == ESP_OK;
    }

    void sidp_session::normalize_pc_in_register_blob(std::uint8_t *data, std::size_t size) const noexcept
    {
        if (!sw_step_over_pending) {
            return;
        }
        for (std::size_t offset = 0; offset + sizeof(register_value_t) <= size;) {
            auto *entry = reinterpret_cast<register_value_t *>(data + offset);
            const std::size_t entry_size = sizeof(register_value_t) + entry->value_size;
            if (offset + entry_size > size) {
                return;
            }
            if (entry->register_id == ARM_REG_PC && entry->value_size == sizeof(std::uint32_t) &&
                (entry->flags & REGISTER_VALUE_FLAG_UNAVAILABLE) == 0) {
                const std::uint32_t pc = static_cast<std::uint32_t>(sw_step_over_address);
                std::memcpy(data + offset + sizeof(register_value_t), &pc, sizeof(pc));
                return;
            }
            offset += entry_size;
        }
    }

    void sidp_session::update_step_over_after_register_write(const std::uint8_t *data, std::size_t size,
                                                              std::uint16_t register_count) noexcept
    {
        if (!sw_step_over_pending) {
            return;
        }
        std::size_t offset = 0;
        for (std::uint16_t index = 0; index < register_count; ++index) {
            if (offset + sizeof(register_value_t) > size) {
                return;
            }
            const auto *entry = reinterpret_cast<const register_value_t *>(data + offset);
            const std::size_t entry_size = sizeof(register_value_t) + entry->value_size;
            if (offset + entry_size > size) {
                return;
            }
            if (entry->register_id == ARM_REG_PC && entry->value_size == sizeof(std::uint32_t)) {
                std::uint32_t pc = 0;
                std::memcpy(&pc, data + offset + sizeof(register_value_t), sizeof(pc));
                if (pc != sw_step_over_address) {
                    sw_step_over_pending = false;
                    sw_step_over_address = 0;
                }
                return;
            }
            offset += entry_size;
        }
    }

    sidp_session::step_over_t sidp_session::execute_step_over(run_action_t action, stop_detect_t &step_stop,
                                                               target_lost_reason_t &lost_reason) noexcept
    {
        step_stop = stop_detect_t{};

        if (!sw_step_over_pending) {
            return step_over_t::NOT_NEEDED;
        }

        const std::uint32_t pc = static_cast<std::uint32_t>(sw_step_over_address);
        sw_bp_t *bp = sw_bp_find_by_address(sw_step_over_address);
        if (bp != nullptr && !sw_bp_restore_one(*bp)) {
            return step_over_t::FAILED;
        }
        if (!write_current_pc(pc)) {
            if (bp != nullptr) {
                (void)sw_bp_install_one(*bp);
            }
            return step_over_t::FAILED;
        }
        if (backend.resume(RUN_SINGLE_STEP, 0) != ESP_OK) {
            if (bp != nullptr) {
                (void)sw_bp_install_one(*bp);
            }
            return step_over_t::FAILED;
        }

        // A single step on real hardware completes within a few DHCSR polls.
        stop_detect_t stop{};
        bool halted = false;
        for (int attempt = 0; attempt < 100; ++attempt) {
            const esp_err_t poll_result = backend.poll_halted(stop);
            if (poll_result != ESP_OK) {
                if (bp != nullptr) {
                    (void)sw_bp_install_one(*bp);
                }
                lost_reason = poll_result == ESP_ERR_TIMEOUT ? TARGET_LOST_TIMEOUT : TARGET_LOST_SWD_FAULT;
                return step_over_t::FAILED_LOST;
            }
            if (stop.halted) {
                halted = true;
                break;
            }
        }
        if (!halted) {
            // Target state unknown after an unconsummated step: terminal (§10.4).
            if (bp != nullptr) {
                (void)sw_bp_install_one(*bp);
            }
            lost_reason = TARGET_LOST_TIMEOUT;
            return step_over_t::FAILED_LOST;
        }

        // Keep the breakpoint armed for the resumed run; a failed re-patch
        // must not silently disarm the breakpoint.
        if (bp != nullptr && !sw_bp_install_one(*bp)) {
            return step_over_t::FAILED;
        }
        sw_step_over_pending = false;
        sw_step_over_address = 0;

        // A clean step only sets the step/halt/external bits; anything else
        // (fault, vector catch, watchpoint, another breakpoint) must surface.
        const bool real_stop = stop.lockup || stop.comparator_match ||
                               (stop.dfsr & 0x0000000Eu) != 0; // BKPT | DWTTRAP | VCATCH
        if (real_stop) {
            step_stop = stop;
            return step_over_t::STOPPED_REAL;
        }
        if (action == RUN_SINGLE_STEP) {
            step_stop = stop;
            return step_over_t::STOPPED_STEP;
        }
        return step_over_t::STEPPED;
    }

    // ---- Software breakpoint shadow table ---------------------------------------------------

    bool sidp_session::sw_bp_apply(const std::span<const bp_entry_t> entries) noexcept
    {
        if (!sw_bp_restore_all()) {
            return false;
        }

        // Stage 1: read and remember every original instruction. Software
        // breakpoint intervals must not overlap each other (section 10).
        for (const bp_entry_t &entry : entries) {
            if (sw_count == MAX_SOFTWARE_BREAKPOINTS) {
                ESP_LOGE(TAG, "sw_bp_apply: sw bp table full (%u)", static_cast<unsigned>(sw_count));
                (void)sw_bp_restore_all();
                return false;
            }
            for (std::size_t prior = 0; prior < sw_count; ++prior) {
                const std::uint64_t prior_start = sw_table[prior].address;
                const std::uint64_t prior_end = prior_start + sw_table[prior].instruction_size;
                if (entry.address < prior_end && prior_start < entry.address + entry.instruction_size) {
                    ESP_LOGE(TAG, "sw_bp_apply: sw bp intervals overlap @0x%llx",
                             static_cast<unsigned long long>(entry.address));
                    (void)sw_bp_restore_all();
                    return false;
                }
            }
            sw_bp_t &slot = sw_table[sw_count];
            slot.id = entry.breakpoint_id;
            slot.address = entry.address;
            slot.instruction_size = entry.instruction_size;
            slot.installed = false;

            std::uint8_t original[4]{};
            if (backend.read_mem(entry.address, original, entry.instruction_size, MEM_WIDTH_DEFAULT) != ESP_OK) {
                ESP_LOGE(TAG, "sw_bp_apply: sw bp original read failed @0x%llx",
                         static_cast<unsigned long long>(entry.address));
                (void)sw_bp_restore_all();
                return false;
            }
            if (entry.instruction_size >= 2 && original[0] == 0x00 && original[1] == 0xBE) {
                // Never treat an existing BKPT patch as the original (§10.4).
                ESP_LOGE(TAG, "sw_bp_apply: sw bp @0x%llx is already BKPT-patched",
                         static_cast<unsigned long long>(entry.address));
                (void)sw_bp_restore_all();
                return false;
            }
            std::memcpy(slot.original, original, sizeof(slot.original));
            sw_count++;
        }

        // Stage 2: write the halfword BKPT patches.
        for (std::size_t index = 0; index < sw_count; ++index) {
            sw_bp_t &slot = sw_table[index];
            if (!sw_bp_install_one(slot)) {
                ESP_LOGE(TAG, "sw_bp_apply: sw bp patch write failed @0x%llx",
                         static_cast<unsigned long long>(slot.address));
                (void)sw_bp_restore_all();
                return false;
            }
        }
        ESP_LOGD(TAG, "sw_bp_apply: sw breakpoints armed: %u", static_cast<unsigned>(sw_count));
        return true;
    }

    bool sidp_session::sw_bp_install_one(sw_bp_t &entry) noexcept
    {
        constexpr std::uint8_t BKPT_PATCH[2] = {0x00, 0xBE};
        if (backend.write_mem(entry.address, BKPT_PATCH, sizeof(BKPT_PATCH), MEM_WIDTH_DEFAULT) != ESP_OK) {
            return false;
        }
        entry.installed = true;
        return true;
    }

    bool sidp_session::sw_bp_restore_all() noexcept
    {
        bool all_restored = true;
        std::size_t remaining = 0;
        for (std::size_t index = 0; index < sw_count; ++index) {
            if (!sw_bp_restore_one(sw_table[index])) {
                all_restored = false;
                if (remaining != index) {
                    sw_table[remaining] = sw_table[index];
                }
                ++remaining;
            }
        }
        sw_count = remaining;
        return all_restored;
    }

    bool sidp_session::sw_bp_restore_one(sw_bp_t &entry) noexcept
    {
        if (!entry.installed) {
            return true;
        }
        const bool ok = backend.write_mem(entry.address, entry.original, entry.instruction_size,
                                          MEM_WIDTH_DEFAULT) == ESP_OK;
        if (ok) {
            entry.installed = false;
        } else {
            // Patch left behind in target memory: the next RUN must retry.
            ESP_LOGE(TAG, "sw_bp_restore_one: sw bp restore failed @0x%llx",
                     static_cast<unsigned long long>(entry.address));
        }
        return ok;
    }

    void sidp_session::sw_bp_substitute_read(std::uint64_t address, std::uint8_t *data, std::size_t size) const noexcept
    {
        for (std::size_t index = 0; index < sw_count; ++index) {
            const sw_bp_t &entry = sw_table[index];
            if (!entry.installed) {
                continue;
            }
            const std::uint64_t bp_start = entry.address;
            const std::uint64_t bp_end = entry.address + entry.instruction_size;
            if (bp_end <= address || address + size <= bp_start) {
                continue;
            }
            for (std::size_t offset = 0; offset < size; ++offset) {
                const std::uint64_t absolute = address + offset;
                if (absolute >= bp_start && absolute < bp_end) {
                    data[offset] = entry.original[absolute - bp_start];
                }
            }
        }
    }

    void sidp_session::sw_bp_write_overlap(std::uint64_t address, const std::uint8_t *data, std::size_t size) noexcept
    {
        for (std::size_t index = 0; index < sw_count; ++index) {
            sw_bp_t &entry = sw_table[index];
            if (!entry.installed) {
                continue;
            }
            const std::uint64_t bp_start = entry.address;
            const std::uint64_t bp_end = entry.address + entry.instruction_size;
            if (bp_end <= address || address + size <= bp_start) {
                continue;
            }
            for (std::size_t offset = 0; offset < size; ++offset) {
                const std::uint64_t absolute = address + offset;
                if (absolute >= bp_start && absolute < bp_end) {
                    entry.original[absolute - bp_start] = data[offset];
                }
            }
        }
    }

    sidp_session::sw_bp_t *sidp_session::sw_bp_find_by_address(std::uint64_t address) noexcept
    {
        for (std::size_t index = 0; index < sw_count; ++index) {
            if (sw_table[index].address == address) {
                return &sw_table[index];
            }
        }
        return nullptr;
    }

    sidp_session::sw_bp_t *sidp_session::sw_bp_find_by_halt_pc(std::uint64_t pc) noexcept
    {
        if (pc >= 2) {
            sw_bp_t *entry = sw_bp_find_by_address(pc - 2);
            if (entry != nullptr && entry->installed) {
                return entry;
            }
        }
        sw_bp_t *entry = sw_bp_find_by_address(pc);
        return entry != nullptr && entry->installed ? entry : nullptr;
    }

    bool sidp_session::rollback_run_config() noexcept
    {
        bool ok = sw_bp_restore_all();
        ok = backend.apply_breakpoints({}) == ESP_OK && ok;
        ok = backend.apply_watchpoints({}) == ESP_OK && ok;
        ok = backend.set_vector_catch(VECTOR_CATCH_NONE) == ESP_OK && ok;
        clear_run_tracking();
        return ok;
    }

    void sidp_session::fail_run(std::uint32_t request_id, status_t status) noexcept
    {
        send_response(OP_RUN, request_id, rollback_run_config() ? status : STATUS_SWD_ERROR);
    }

    // ---- Memory access validation (protocol section 9) -----------------------------------------

    status_t sidp_session::validate_memory_access(std::uint64_t address, std::uint32_t length,
                                                    memory_access_flag_t flags, memory_access_width_t width,
                                                    bool is_write) const noexcept
    {
        if (length == 0 || address > UINT64_MAX - length) {
            return STATUS_ADDRESS_ERROR;
        }
        const std::uint16_t flag_bits = static_cast<std::uint16_t>(flags);
        const bool allow_mmio = (flag_bits & static_cast<std::uint16_t>(MEM_ACCESS_ALLOW_MMIO)) != 0;

        // The request must fall entirely inside one attached region (section 9).
        const memory_region_t *region = nullptr;
        for (std::size_t index = 0; index < region_count; ++index) {
            const memory_region_t &candidate = regions[index];
            if (candidate.start > UINT64_MAX - candidate.length) {
                continue;
            }
            const std::uint64_t region_end = candidate.start + candidate.length;
            if (address >= candidate.start && address + length <= region_end) {
                region = &candidate;
                break;
            }
        }
        if (region == nullptr) {
            return STATUS_ADDRESS_ERROR;
        }

        // Flash is read-only in v1 debug sessions (section 9.2).
        if (region->type == MEMORY_FLASH && is_write) {
            return STATUS_UNSUPPORTED;
        }
        const auto region_flags = static_cast<std::uint8_t>(region->flags);
        const auto required_flag = static_cast<std::uint8_t>(is_write ? MEM_WRITE : MEM_READ);
        if ((region_flags & required_flag) == 0) {
            return STATUS_ADDRESS_ERROR;
        }

        const std::uint8_t width_value = static_cast<std::uint8_t>(width);
        if (region->type == MEMORY_MMIO) {
            if (!allow_mmio) {
                return STATUS_ADDRESS_ERROR;
            }
            if (width != MEM_WIDTH_8 && width != MEM_WIDTH_16 && width != MEM_WIDTH_32) {
                return STATUS_INVALID_ARGUMENT; // MMIO requires an explicit width
            }
            if (length != width_value) {
                return STATUS_INVALID_ARGUMENT; // one bus access per request (section 9.1)
            }
            if ((address % width_value) != 0) {
                return STATUS_ALIGNMENT_ERROR;
            }
        } else if (width != MEM_WIDTH_DEFAULT) {
            // Explicit width for RAM/Flash: natural alignment and whole multiples.
            if (width != MEM_WIDTH_8 && width != MEM_WIDTH_16 && width != MEM_WIDTH_32) {
                return STATUS_INVALID_ARGUMENT;
            }
            if ((address % width_value) != 0 || (length % width_value) != 0) {
                return STATUS_ALIGNMENT_ERROR;
            }
        }
        return STATUS_OK;
    }

    // ---- Helpers -----------------------------------------------------------------------------

    bool sidp_session::storage_ready() const noexcept
    {
        return scratch != nullptr && pending_registers != nullptr &&
               regions != nullptr && sw_table != nullptr && tx_sink != nullptr;
    }

    bool sidp_session::is_executable_range(std::uint64_t address, std::size_t size,
                                           bool require_writable) const noexcept
    {
        if (size == 0 || address > UINT64_MAX - size) {
            return false;
        }
        for (std::size_t index = 0; index < region_count; ++index) {
            const memory_region_t &region = regions[index];
            if (region.start > UINT64_MAX - region.length) {
                continue;
            }
            const auto flags = static_cast<std::uint8_t>(region.flags);
            if (address >= region.start && address + size <= region.start + region.length &&
                (flags & static_cast<std::uint8_t>(MEM_EXECUTE)) != 0 &&
                (!require_writable ||
                 (region.type == MEMORY_RAM && (flags & static_cast<std::uint8_t>(MEM_WRITE)) != 0))) {
                return true;
            }
        }
        return false;
    }

    void sidp_session::clear_debug_state() noexcept
    {
        stop_reported = false;
        pending_reason = STOP_UNKNOWN;
        pending_detail = 0;
        pending_breakpoint_id = 0;
        pending_watchpoint_address = 0;
        pending_registers_size = 0;
        pending_stack_size = 0;
        pending_stack_address = 0;
        sw_step_over_pending = false;
        sw_step_over_address = 0;
    }

    void sidp_session::clear_run_tracking() noexcept
    {
        running_action = RUN_CONTINUE;
        run_to_active = false;
        active_run_to_address = 0;
        hw_count = 0;
    }

}
