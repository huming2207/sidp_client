#include "sidp_transport_cdc.hpp"

#include <cerrno>

#include "esp_heap_caps.h"
#include "esp_timer.h"

namespace sidp
{

    int cdc_slip_transport::init(tinyusb_cdcacm_itf_t port) noexcept
    {
        if (initialized) {
            return -EALREADY;
        }

        const int port_index = static_cast<int>(port);
        if (port_index < 0 || port_index >= TINYUSB_CDC_ACM_MAX) {
            return -EINVAL;
        }

        auto *buffers = static_cast<std::uint8_t *>(heap_caps_malloc(MAX_FRAME_SIZE + MAX_ENCODED_FRAME_SIZE, MALLOC_CAP_SPIRAM));
        if (buffers == nullptr) {
            return -ENOMEM;
        }

        RingbufHandle_t rx_ring_buffer = xRingbufferCreateWithCaps(RX_PACKET_RING_BUFFER_SIZE, RINGBUF_TYPE_NOSPLIT, MALLOC_CAP_SPIRAM);
        if (rx_ring_buffer == nullptr) {
            heap_caps_free(buffers);
            return -ENOMEM;
        }

        EventGroupHandle_t event_group = xEventGroupCreate();
        if (event_group == nullptr) {
            vRingbufferDeleteWithCaps(rx_ring_buffer);
            heap_caps_free(buffers);
            return -ENOMEM;
        }

        cdc_port = port;
        events = event_group;
        ring_buffer = rx_ring_buffer;
        frame_buffer = buffers;
        tx_buffer = buffers + MAX_FRAME_SIZE;
        initialized = true;

        const tinyusb_config_cdcacm_t cdc_config = {
            .cdc_port = cdc_port,
            .callback_rx = cdc_event_callback,
            .callback_rx_wanted_char = nullptr,
            .callback_line_state_changed = nullptr,
            .callback_line_coding_changed = nullptr,
        };
        const esp_err_t result = tinyusb_cdcacm_init(&cdc_config);
        if (result != ESP_OK) {
            initialized = false;
            cdc_port = TINYUSB_CDC_ACM_0;
            events = nullptr;
            ring_buffer = nullptr;
            frame_buffer = nullptr;
            tx_buffer = nullptr;
            vEventGroupDelete(event_group);
            vRingbufferDeleteWithCaps(rx_ring_buffer);
            heap_caps_free(buffers);
            return esp_error_to_errno(result);
        }

        return 0;
    }

    int cdc_slip_transport::start_read(std::uint8_t **buf_out, std::size_t *len_out, std::uint32_t timeout_ms) noexcept
    {
        if (buf_out == nullptr || len_out == nullptr) {
            return -EINVAL;
        }

        *buf_out = nullptr;
        *len_out = 0;
        if (!initialized) {
            return -ENOTCONN;
        }
        if ((xEventGroupClearBits(events, EVENT_RX_OVERFLOW) & EVENT_RX_OVERFLOW) != 0) {
            return -ENOBUFS;
        }

        std::size_t packet_size = 0;
        auto *packet = static_cast<std::uint8_t *>(xRingbufferReceive(ring_buffer, &packet_size, timeout_to_ticks(timeout_ms)));
        if (packet == nullptr) {
            return is_open() ? -ETIMEDOUT : -ENOTCONN;
        }

        *buf_out = packet;
        *len_out = packet_size;
        return 0;
    }

    void cdc_slip_transport::end_read(std::uint8_t *buf_return) noexcept
    {
        if (buf_return != nullptr && ring_buffer != nullptr) {
            vRingbufferReturnItem(ring_buffer, buf_return);
        }
    }

    int cdc_slip_transport::write_message(std::span<const std::uint8_t> message) noexcept
    {
        if (!is_open()) {
            discard_pending_tx();
            return -ENOTCONN;
        }
        if (message.size() < sizeof(msg_header_t)) {
            return -EINVAL;
        }
        if (message.size() > MAX_FRAME_SIZE) {
            return -EMSGSIZE;
        }
        if (tx_size != 0) {
            return -EBUSY;
        }

        encode_message(message);
        return 0;
    }

    int cdc_slip_transport::flush_write(std::uint32_t timeout_ms) noexcept
    {
        if (tx_size == 0) {
            return 0;
        }
        if (!is_open()) {
            discard_pending_tx();
            return -ENOTCONN;
        }

        const deadline_t deadline{now_ms(), timeout_ms};
        while (tx_offset < tx_size) {
            if (!is_open()) {
                discard_pending_tx();
                return -ENOTCONN;
            }

            const std::size_t queued = tinyusb_cdcacm_write_queue(cdc_port, tx_buffer + tx_offset, tx_size - tx_offset);
            if (queued != 0) {
                tx_offset += queued;
                continue;
            }

            const int flush_result = flush_cdc_once(deadline);
            if (flush_result == -ENOTCONN) {
                discard_pending_tx();
                return flush_result;
            }
            if (flush_result != 0 && flush_result != -EAGAIN) {
                return flush_result;
            }
        }

        while (true) {
            const int flush_result = flush_cdc_once(deadline);
            if (flush_result == -EAGAIN) {
                continue;
            }
            if (flush_result == -ENOTCONN) {
                discard_pending_tx();
            }
            if (flush_result != 0) {
                return flush_result;
            }
            break;
        }

        tx_size = 0;
        tx_offset = 0;
        return 0;
    }

    void cdc_slip_transport::discard_pending_tx() noexcept
    {
        tx_size = 0;
        tx_offset = 0;
    }

    bool cdc_slip_transport::is_open() const noexcept
    {
        return initialized && tinyusb_cdcacm_initialized(cdc_port) &&
               tud_cdc_n_ready(static_cast<std::uint8_t>(cdc_port));
    }

    void cdc_slip_transport::cdc_event_callback(int itf, cdcacm_event_t *event) noexcept
    {
        if (event == nullptr || event->type != CDC_EVENT_RX) {
            return;
        }

        auto &transport = instance();
        if (transport.initialized && static_cast<int>(transport.cdc_port) == itf) {
            transport.drain_cdc_input();
        }
    }

    int cdc_slip_transport::esp_error_to_errno(esp_err_t error) noexcept
    {
        switch (error) {
        case ESP_OK:
            return 0;
        case ESP_ERR_INVALID_ARG:
            return -EINVAL;
        case ESP_ERR_NO_MEM:
            return -ENOMEM;
        case ESP_ERR_TIMEOUT:
        case ESP_ERR_NOT_FINISHED:
            return -ETIMEDOUT;
        case ESP_ERR_INVALID_STATE:
            return -ENOTCONN;
        default:
            return -EIO;
        }
    }

    std::uint32_t cdc_slip_transport::now_ms() noexcept
    {
        return static_cast<std::uint32_t>(esp_timer_get_time() / 1000);
    }

    std::uint32_t cdc_slip_transport::remaining_timeout(const deadline_t &deadline) noexcept
    {
        if (deadline.timeout_ms == WAIT_FOREVER) {
            return WAIT_FOREVER;
        }

        const std::uint32_t elapsed = now_ms() - deadline.started_at;
        return elapsed < deadline.timeout_ms ? deadline.timeout_ms - elapsed : 0;
    }

    TickType_t cdc_slip_transport::timeout_to_ticks(std::uint32_t timeout_ms) noexcept
    {
        if (timeout_ms == WAIT_FOREVER) {
            return portMAX_DELAY;
        }
        if (timeout_ms == 0) {
            return 0;
        }

        const TickType_t ticks = pdMS_TO_TICKS(timeout_ms);
        return ticks == 0 ? 1 : ticks;
    }

    void cdc_slip_transport::drain_cdc_input() noexcept
    {
        if (!is_open()) {
            return;
        }

        std::uint8_t chunk[RX_READ_CHUNK_SIZE];
        while (true) {
            std::size_t bytes_read = 0;
            const esp_err_t result = tinyusb_cdcacm_read(cdc_port, chunk, sizeof(chunk), &bytes_read);
            if (result != ESP_OK || bytes_read == 0) {
                return;
            }

            for (std::size_t index = 0; index < bytes_read; ++index) {
                if (!consume_input_byte(chunk[index])) {
                    continue;
                }

                const std::span<const std::uint8_t> packet(frame_buffer, frame_size);
                if (!transport_intf::is_valid_message_size(frame_size) || !crc32_hasher::verify_message_crc(packet)) {
                    reset_frame_state();
                    continue;
                }

                if (xRingbufferSend(ring_buffer, frame_buffer, frame_size, 0) != pdTRUE) {
                    reset_frame_state();
                    xEventGroupSetBits(events, EVENT_RX_OVERFLOW);
                    continue;
                }

                reset_frame_state();
            }
        }
    }

    bool cdc_slip_transport::consume_input_byte(std::uint8_t byte) noexcept
    {
        if (byte == SLIP_END) {
            if (!receiving_frame) {
                receiving_frame = true;
                reset_frame_state();
                return false;
            }

            if (frame_discarded || escape_pending) {
                reset_frame_state();
                return false;
            }
            if (frame_size == 0) {
                return false;
            }

            return true;
        }

        if (!receiving_frame || frame_discarded) {
            return false;
        }

        if (escape_pending) {
            escape_pending = false;
            if (byte == SLIP_ESC_END) {
                append_decoded_byte(SLIP_END);
            } else if (byte == SLIP_ESC_ESC) {
                append_decoded_byte(SLIP_ESC);
            } else {
                frame_discarded = true;
            }
            return false;
        }

        if (byte == SLIP_ESC) {
            escape_pending = true;
        } else {
            append_decoded_byte(byte);
        }
        return false;
    }

    void cdc_slip_transport::append_decoded_byte(std::uint8_t byte) noexcept
    {
        if (frame_size == MAX_FRAME_SIZE) {
            frame_discarded = true;
            return;
        }
        frame_buffer[frame_size++] = byte;
    }

    void cdc_slip_transport::reset_frame_state() noexcept
    {
        frame_size = 0;
        frame_discarded = false;
        escape_pending = false;
    }

    void cdc_slip_transport::encode_message(std::span<const std::uint8_t> message) noexcept
    {
        tx_size = 0;
        tx_offset = 0;
        append_tx_byte(SLIP_END);
        for (const std::uint8_t byte : message) {
            append_escaped_tx_byte(byte);
        }
        append_tx_byte(SLIP_END);
    }

    void cdc_slip_transport::append_tx_byte(std::uint8_t byte) noexcept
    {
        tx_buffer[tx_size++] = byte;
    }

    void cdc_slip_transport::append_escaped_tx_byte(std::uint8_t byte) noexcept
    {
        if (byte == SLIP_END) {
            append_tx_byte(SLIP_ESC);
            append_tx_byte(SLIP_ESC_END);
        } else if (byte == SLIP_ESC) {
            append_tx_byte(SLIP_ESC);
            append_tx_byte(SLIP_ESC_ESC);
        } else {
            append_tx_byte(byte);
        }
    }

    int cdc_slip_transport::flush_cdc_once(const deadline_t &deadline) noexcept
    {
        if (!is_open()) {
            return -ENOTCONN;
        }

        const std::uint32_t remaining_ms = remaining_timeout(deadline);
        std::uint32_t wait_ms = remaining_ms;
        if (wait_ms == WAIT_FOREVER || wait_ms > FLUSH_WAIT_SLICE_MS) {
            wait_ms = FLUSH_WAIT_SLICE_MS;
        }

        const esp_err_t result = tinyusb_cdcacm_write_flush(cdc_port, timeout_to_ticks(wait_ms));
        if (result == ESP_OK) {
            return 0;
        }
        if (!is_open()) {
            return -ENOTCONN;
        }
        if (result != ESP_ERR_TIMEOUT && result != ESP_ERR_NOT_FINISHED) {
            return esp_error_to_errno(result);
        }
        if (deadline.timeout_ms != WAIT_FOREVER && remaining_timeout(deadline) == 0) {
            return -ETIMEDOUT;
        }
        return -EAGAIN;
    }

}
