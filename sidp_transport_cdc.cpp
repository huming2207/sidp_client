#include "sidp_transport_cdc.hpp"

#include "esp_log.h"

#include "esp_heap_caps.h"
#include "esp_timer.h"

namespace sidp
{

    esp_err_t cdc_slip_transport::init(tinyusb_cdcacm_itf_t port) noexcept
    {
        if (initialized) {
            ESP_LOGE(TAG, "init: cdc transport already initialized");
            return ESP_ERR_INVALID_STATE;
        }

        const int port_index = static_cast<int>(port);
        if (port_index < 0 || port_index >= TINYUSB_CDC_ACM_MAX) {
            ESP_LOGE(TAG, "init: invalid CDC port %d", port_index);
            return ESP_ERR_INVALID_ARG;
        }

        auto *buffers = static_cast<std::uint8_t *>(heap_caps_malloc(MAX_FRAME_SIZE + MAX_ENCODED_FRAME_SIZE, MALLOC_CAP_SPIRAM));
        if (buffers == nullptr) {
            ESP_LOGE(TAG, "init: buffer allocation failed");
            return ESP_ERR_NO_MEM;
        }

        const int queue_result = create_packet_queue();
        if (queue_result != ESP_OK) {
            ESP_LOGE(TAG, "init: rx queue creation failed");
            heap_caps_free(buffers);
            return queue_result;
        }

        cdc_port = port;
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
            ESP_LOGE(TAG, "init: tinyusb_cdcacm_init failed: %s", esp_err_to_name(result));
            initialized = false;
            cdc_port = TINYUSB_CDC_ACM_0;
            frame_buffer = nullptr;
            tx_buffer = nullptr;
            destroy_packet_queue();
            heap_caps_free(buffers);
            return map_esp_error(result);
        }

        ESP_LOGI(TAG, "init: cdc transport ready (itf=%d)", port_index);
        return ESP_OK;
    }

    esp_err_t cdc_slip_transport::write_message(std::span<const std::uint8_t> message) noexcept
    {
        if (!is_open()) {
            discard_pending_tx();
            return ESP_ERR_INVALID_STATE;
        }
        if (message.size() < sizeof(msg_header_t)) {
            return ESP_ERR_INVALID_ARG;
        }
        if (message.size() > MAX_FRAME_SIZE) {
            return ESP_ERR_INVALID_SIZE;
        }
        if (tx_size != 0) {
            return ESP_ERR_INVALID_STATE;
        }

        encode_message(message);
        return 0;
    }

    esp_err_t cdc_slip_transport::flush_write(std::uint32_t timeout_ms) noexcept
    {
        if (tx_size == 0) {
            return 0;
        }
        if (!is_open()) {
            discard_pending_tx();
            return ESP_ERR_INVALID_STATE;
        }

        const deadline_t deadline{now_ms(), timeout_ms};
        while (tx_offset < tx_size) {
            if (!is_open()) {
                discard_pending_tx();
                return ESP_ERR_INVALID_STATE;
            }

            const std::size_t queued = tinyusb_cdcacm_write_queue(cdc_port, tx_buffer + tx_offset, tx_size - tx_offset);
            if (queued != 0) {
                tx_offset += queued;
                continue;
            }

            const esp_err_t flush_result = flush_cdc_once(deadline);
            if (flush_result == ESP_ERR_INVALID_STATE) {
                discard_pending_tx();
                return flush_result;
            }
            if (flush_result != ESP_OK && flush_result != ESP_ERR_NOT_FINISHED) {
                return flush_result;
            }
        }

        while (true) {
            const esp_err_t flush_result = flush_cdc_once(deadline);
            if (flush_result == ESP_ERR_NOT_FINISHED) {
                continue;
            }
            if (flush_result == ESP_ERR_INVALID_STATE) {
                discard_pending_tx();
            }
            if (flush_result != ESP_OK) {
                return flush_result;
            }
            break;
        }

        tx_size = 0;
        tx_offset = 0;
        return ESP_OK;
    }

    void cdc_slip_transport::discard_pending_tx() noexcept
    {
        if (tx_size != 0) {
            ESP_LOGE(TAG, "discard_pending_tx: discarding %u pending tx bytes", static_cast<unsigned>(tx_size));
        }
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

    esp_err_t cdc_slip_transport::map_esp_error(esp_err_t error) noexcept
    {
        switch (error) {
        case ESP_ERR_INVALID_ARG:
        case ESP_ERR_NO_MEM:
        case ESP_ERR_TIMEOUT:
        case ESP_ERR_NOT_FINISHED:
        case ESP_ERR_INVALID_STATE:
            return error;
        default:
            return ESP_FAIL;
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

                deliver_packet(frame_buffer, frame_size);
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

            if (escape_pending) {
                ESP_LOGE(TAG, "consume_input_byte: slip frame aborted: ESC before END");
                reset_frame_state();
                return false;
            }
            if (frame_discarded) {
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
                ESP_LOGE(TAG, "consume_input_byte: slip frame dropped: invalid escape 0x%02x", byte);
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
            ESP_LOGE(TAG, "append_decoded_byte: slip frame dropped: exceeds frame size");
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
            return ESP_ERR_INVALID_STATE;
        }

        const std::uint32_t remaining_ms = remaining_timeout(deadline);
        std::uint32_t wait_ms = remaining_ms;
        if (wait_ms == WAIT_FOREVER || wait_ms > FLUSH_WAIT_SLICE_MS) {
            wait_ms = FLUSH_WAIT_SLICE_MS;
        }

        const esp_err_t result = tinyusb_cdcacm_write_flush(cdc_port, timeout_to_ticks(wait_ms));
        if (result == ESP_OK) {
            return ESP_OK;
        }
        if (!is_open()) {
            return ESP_ERR_INVALID_STATE;
        }
        if (result != ESP_ERR_TIMEOUT && result != ESP_ERR_NOT_FINISHED) {
            return map_esp_error(result);
        }
        if (deadline.timeout_ms != WAIT_FOREVER && remaining_timeout(deadline) == 0) {
            ESP_LOGE(TAG, "flush_cdc_once: cdc flush timeout");
            return ESP_ERR_TIMEOUT;
        }
        return ESP_ERR_NOT_FINISHED; // slice expired: retry
    }

}
