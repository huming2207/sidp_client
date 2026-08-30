#pragma once

#include <cstddef>
#include <cstdint>
#include <span>

#include "freertos/FreeRTOS.h"
#include "tinyusb.h"
#include "tinyusb_cdc_acm.h"

#include "sidp_transport_queue.hpp"

namespace sidp
{

    /**
     * @brief Process-wide SIDP transport over esp_tinyusb CDC-ACM and SLIP.
     *
     * Singleton: obtain the single instance from instance() and call init()
     * once during startup. The transport owns its resources for the lifetime
     * of the process and is never torn down at runtime. This removes any
     * teardown race with the TinyUSB task that dispatches the CDC receive
     * callback. The shared composite TinyUSB device driver remains owned by
     * the application because it also serves classes such as MSC.
     *
     * Each message is encoded as END, escaped SIDP bytes, END. END (0xC0) is
     * encoded as ESC ESC_END and ESC (0xDB) as ESC ESC_ESC. Empty frames are
     * ignored. The leading END resynchronizes the peer after an interrupted
     * session.
     *
     * The CDC callback reads the interface in chunks of up to
     * RX_READ_CHUNK_SIZE bytes and feeds the SLIP decoder one byte at a
     * time. Decoder state persists across chunks, so frames may start and
     * end anywhere inside a chunk or span chunk boundaries. At END the
     * complete decoded packet is validated and queued by the
     * packet_queue_transport base. Invalid packets never enter the consumer
     * queue.
     *
     * init() performs all allocations: the decoded RX and encoded TX storage
     * (one contiguous block in PSRAM) plus the receive queue owned by the
     * base. No other method allocates. One reader and one writer may operate
     * concurrently; callers must serialize multiple writers.
     */
    class cdc_slip_transport final : public packet_queue_transport
    {
    public:
        /** @brief SLIP frame delimiter. */
        inline static constexpr std::uint8_t SLIP_END = 0xC0;

        /** @brief SLIP escape marker. */
        inline static constexpr std::uint8_t SLIP_ESC = 0xDB;

        /** @brief Escaped representation of SLIP_END. */
        inline static constexpr std::uint8_t SLIP_ESC_END = 0xDC;

        /** @brief Escaped representation of SLIP_ESC. */
        inline static constexpr std::uint8_t SLIP_ESC_ESC = 0xDD;

        /**
         * @brief Returns the process-wide transport instance.
         *
         * The instance is constructed on first use and never destroyed.
         */
        [[nodiscard]] static cdc_slip_transport &instance() noexcept
        {
            static cdc_slip_transport transport;
            return transport;
        }

        cdc_slip_transport(const cdc_slip_transport &) = delete;
        cdc_slip_transport &operator=(const cdc_slip_transport &) = delete;

        /**
         * @brief Initializes and binds the esp_tinyusb CDC-ACM interface.
         *
         * Must be called exactly once, before any other method. A second call
         * returns @c ESP_ERR_INVALID_STATE and changes nothing.
         *
         * @param cdc_port CDC interface used for SIDP.
         * @return ESP_OK on success.
         * @return ESP_ERR_INVALID_ARG if cdc_port is not enabled by the current build.
         * @return ESP_ERR_INVALID_STATE if already initialized.
         * @return ESP_ERR_NO_MEM if initialization memory cannot be allocated.
         * @return ESP_FAIL if CDC initialization otherwise fails.
         */
        [[nodiscard]] esp_err_t init(tinyusb_cdcacm_itf_t cdc_port) noexcept;

        /** @copydoc transport_intf::write_message */
        [[nodiscard]] esp_err_t write_message(std::span<const std::uint8_t> message) noexcept override;

        /** @copydoc transport_intf::flush_write */
        [[nodiscard]] esp_err_t flush_write(std::uint32_t timeout_ms) noexcept override;

        /**
         * @brief Reports whether the transport can currently exchange data.
         *
         * True once init() succeeded and the CDC interface is mounted and
         * configured by the USB host. USB disconnect is observed here; there
         * is no explicit close operation.
         */
        [[nodiscard]] bool is_open() const noexcept override;

    private:
        cdc_slip_transport() noexcept = default;
        ~cdc_slip_transport() override = default;

        /** @brief Maximum SLIP size when every SIDP byte requires escaping. */
        inline static constexpr std::size_t MAX_ENCODED_FRAME_SIZE = (MAX_FRAME_SIZE * 2u) + 2u;

        /** @brief Maximum bytes fetched from the CDC interface per read. */
        inline static constexpr std::size_t RX_READ_CHUNK_SIZE = 64;

        /** @brief Maximum blocking interval before flush_write() rechecks liveness. */
        inline static constexpr std::uint32_t FLUSH_WAIT_SLICE_MS = 10;

        /** @brief Per-operation deadline in monotonic milliseconds. */
        struct deadline_t {
            std::uint32_t started_at;
            std::uint32_t timeout_ms;
        };

        /** @brief Routes an esp_tinyusb callback to the singleton. */
        static void cdc_event_callback(int itf, cdcacm_event_t *event) noexcept;

        /** @brief Normalizes an ESP-IDF result to the transport error codes. */
        [[nodiscard]] static esp_err_t map_esp_error(esp_err_t error) noexcept;

        /** @brief Returns monotonic milliseconds modulo 2^32. */
        [[nodiscard]] static std::uint32_t now_ms() noexcept;

        /** @brief Computes remaining milliseconds for an operation. */
        [[nodiscard]] static std::uint32_t remaining_timeout(const deadline_t &deadline) noexcept;

        /** @brief Reads, decodes, validates, and queues all available CDC bytes. */
        void drain_cdc_input() noexcept;

        /**
         * @brief Consumes one encoded SLIP byte.
         * @return true when the byte completes a frame, otherwise false.
         *         Malformed bytes mark the current frame discarded; the frame
         *         is then silently skipped until the next END.
         */
        [[nodiscard]] bool consume_input_byte(std::uint8_t byte) noexcept;

        /** @brief Appends a decoded byte or marks the frame discarded. */
        void append_decoded_byte(std::uint8_t byte) noexcept;

        /** @brief Resets decoded state while retaining raw buffered input. */
        void reset_frame_state() noexcept;

        /** @brief Discards any partially flushed TX message. */
        void discard_pending_tx() noexcept;

        /** @brief Encodes one SIDP message into the preallocated TX buffer. */
        void encode_message(std::span<const std::uint8_t> message) noexcept;

        /** @brief Appends one byte to the preallocated TX buffer. */
        void append_tx_byte(std::uint8_t byte) noexcept;

        /** @brief Appends one SLIP-escaped byte to the preallocated TX buffer. */
        void append_escaped_tx_byte(std::uint8_t byte) noexcept;

        /** @brief Runs one bounded esp_tinyusb transmit flush. */
        [[nodiscard]] esp_err_t flush_cdc_once(const deadline_t &deadline) noexcept;

        tinyusb_cdcacm_itf_t cdc_port = TINYUSB_CDC_ACM_0;
        std::uint8_t *frame_buffer = nullptr;
        std::uint8_t *tx_buffer = nullptr;
        std::size_t frame_size = 0;
        std::size_t tx_size = 0;
        std::size_t tx_offset = 0;
        bool frame_discarded = false;
        bool initialized = false;
        bool receiving_frame = false;
        bool escape_pending = false;
        static constexpr char TAG[] = "sidp_cdc";
    };

}
