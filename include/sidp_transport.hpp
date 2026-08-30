#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <span>

#include "esp_err.h"

#include "sidp_defs.hpp"

namespace sidp
{

    /** @brief Timeout value that requests an indefinite wait. */
    inline constexpr std::uint32_t WAIT_FOREVER = std::numeric_limits<std::uint32_t>::max();

    /**
     * @brief How long write_message() waits for TX queue space before it
     *        gives up and loses the frame. Session responses must not be
     *        dropped, but a stuck TX task must not wedge the debug task.
     */
    inline constexpr std::uint32_t TX_ENQUEUE_TIMEOUT_MS = 10000;

    /**
     * @brief Interface for transporting complete SIDP messages.
     *
     * WebSocket implementations map one SIDP message to one binary WebSocket
     * message. Stream implementations, such as USB CDC-ACM, own their framing
     * and return only the decoded SIDP message.
     *
     * Each direction owns one queue: decoded inbound frames are received
     * through start_read(), outbound frames are enqueued with write_message()
     * and drained by the transport's own TX task. One reader and any number
     * of writers may operate concurrently; the single TX queue preserves
     * Response/Event ordering.
     *
     * Methods return ESP_OK on success and an ESP_ERR_* code on failure.
     */
    class transport_intf
    {
    public:
        transport_intf(const transport_intf &) = delete;
        transport_intf &operator=(const transport_intf &) = delete;
        transport_intf(transport_intf &&) = delete;
        transport_intf &operator=(transport_intf &&) = delete;

        /** @brief Destroys the transport interface. */
        virtual ~transport_intf() = default;

        /**
         * @brief Acquires one complete, decoded, CRC-validated SIDP message.
         *
         * On success, ownership of the returned transport buffer is lent to
         * the caller. The caller must eventually pass the exact pointer to
         * end_read(), and must not use it afterward. Implementations may
         * require buffers to be returned in acquisition order.
         *
         * @param buf_out Receives the acquired message pointer on success;
         *        set to nullptr on failure.
         * @param len_out Receives a value in [sizeof(msg_header_t),
         *        MAX_FRAME_SIZE] on success; set to zero on failure.
         * @param timeout_ms Maximum wait in milliseconds, or WAIT_FOREVER.
         * @return ESP_OK on success.
         * @return ESP_ERR_INVALID_ARG if an output pointer is null.
         * @return ESP_ERR_TIMEOUT if no complete message arrives before timeout.
         * @return ESP_ERR_INVALID_STATE if the transport is disconnected or
         *         not yet initialized.
         * @return ESP_ERR_NO_MEM if the receive queue overflowed.
         * @return ESP_FAIL for another underlying transport failure.
         */
        [[nodiscard]] virtual esp_err_t start_read(std::uint8_t **buf_out, std::size_t *len_out, std::uint32_t timeout_ms) noexcept = 0;

        /**
         * @brief Returns a message buffer acquired by start_read().
         * @param buf_return Exact non-null pointer returned by start_read().
         *
         * Passing a foreign pointer, returning a pointer twice, or using it
         * after this call is invalid.
         */
        virtual void end_read(std::uint8_t *buf_return) noexcept = 0;

        /**
         * @brief Enqueues one complete SIDP message for transmission.
         *
         * The message is copied into the transport's TX queue; the transport
         * owns the copy and the caller may immediately reuse its buffer. The
         * transport's TX task serializes queued frames onto the wire in FIFO
         * order. Waits up to TX_ENQUEUE_TIMEOUT_MS for queue space: session
         * responses and events must not be lost. Frames on a disconnected
         * transport are dropped by the TX task.
         *
         * @param message Complete SIDP message to send.
         * @return ESP_OK on success.
         * @return ESP_ERR_INVALID_ARG if message is shorter than sizeof(msg_header_t).
         * @return ESP_ERR_INVALID_SIZE if message exceeds MAX_FRAME_SIZE.
         * @return ESP_ERR_INVALID_STATE if the transport is not yet initialized.
         * @return ESP_ERR_TIMEOUT if the queue stayed full for the whole
         *         timeout; the frame is lost.
         */
        [[nodiscard]] virtual esp_err_t write_message(std::span<const std::uint8_t> message) noexcept = 0;

        /**
         * @brief Enqueues one expendable SIDP message (e.g. a LOG frame).
         *
         * Never blocks: when the TX queue is full the frame is dropped and
         * counted in tx_dropped_frames().
         *
         * @param message Complete SIDP message to send.
         * @return ESP_OK when the message was enqueued.
         * @return ESP_ERR_INVALID_ARG if message is shorter than sizeof(msg_header_t).
         * @return ESP_ERR_INVALID_SIZE if message exceeds MAX_FRAME_SIZE.
         * @return ESP_ERR_INVALID_STATE if the transport is not yet initialized.
         * @return ESP_ERR_NO_MEM when the queue was full and the frame was dropped.
         */
        [[nodiscard]] virtual esp_err_t write_message_log(std::span<const std::uint8_t> message) noexcept = 0;

        /**
         * @brief Waits until every queued frame has left the wire.
         *
         * @param timeout_ms Maximum wait in milliseconds, or WAIT_FOREVER.
         * @return ESP_OK when the TX queue is drained.
         * @return ESP_ERR_TIMEOUT if frames remain after the timeout expires.
         * @return ESP_ERR_INVALID_STATE if the transport is not yet initialized.
         */
        [[nodiscard]] virtual esp_err_t flush_write(std::uint32_t timeout_ms) noexcept = 0;

        /** @brief Number of frames dropped by write_message_expendable() so far. */
        [[nodiscard]] virtual std::uint32_t tx_dropped_frames() const noexcept = 0;

        /**
         * @brief Reports whether the transport can currently exchange data.
         * @return true while the transport is open, otherwise false.
         */
        [[nodiscard]] virtual bool is_open() const noexcept = 0;

        /**
         * @brief Checks whether a byte count can represent a complete SIDP message.
         * @param message_size Candidate message size in bytes.
         * @return true if the size is within the SIDP v1 frame limits.
         */
        [[nodiscard]] static constexpr bool is_valid_message_size(std::size_t message_size) noexcept
        {
            return message_size >= sizeof(msg_header_t) && message_size <= MAX_FRAME_SIZE;
        }

    protected:
        /** @brief Constructs the base portion of a concrete transport. */
        transport_intf() = default;
    };

    /**
     * @brief Static CRC-32/ISO-HDLC helpers for SIDP messages.
     *
     * The implementation uses the reflected polynomial 0xEDB88320 and a
     * compile-time generated 256-entry lookup table. The class has no state
     * and cannot be instantiated.
     */
    class crc32_hasher final
    {
    public:
        /** @brief CRC-32/ISO-HDLC initial register value. */
        inline static constexpr std::uint32_t INITIAL_VALUE = 0xFFFFFFFFu;

        /** @brief CRC-32/ISO-HDLC final XOR value. */
        inline static constexpr std::uint32_t XOR_OUT = 0xFFFFFFFFu;

        crc32_hasher() = delete;

        /**
         * @brief Updates an unfinalized CRC state.
         * @param state Running CRC state, initially INITIAL_VALUE.
         * @param data Bytes to add to the calculation.
         * @return Updated, unfinalized CRC state.
         */
        [[nodiscard]] static std::uint32_t update(std::uint32_t state, std::span<const std::uint8_t> data) noexcept;

        /**
         * @brief Applies the CRC-32/ISO-HDLC final XOR.
         * @param state Unfinalized CRC state.
         * @return Finalized CRC value.
         */
        [[nodiscard]] static constexpr std::uint32_t finalize(std::uint32_t state) noexcept
        {
            return state ^ XOR_OUT;
        }

        /**
         * @brief Calculates CRC-32/ISO-HDLC for a contiguous byte range.
         * @param data Bytes to checksum.
         * @return Finalized CRC value.
         */
        [[nodiscard]] static std::uint32_t calculate(std::span<const std::uint8_t> data) noexcept;

        /**
         * @brief Calculates a SIDP message CRC while excluding its crc32 field.
         * @param message Complete SIDP message, including the 12-byte header.
         * @param crc Receives the calculated CRC, or zero if the size is invalid.
         * @return true on success; false if the message size is invalid.
         */
        [[nodiscard]] static bool calculate_message(std::span<const std::uint8_t> message, std::uint32_t &crc) noexcept;

        /**
         * @brief Calculates and stores msg_header_t::crc32.
         *
         * The value is stored using the little-endian host representation
         * required by SIDP v1.
         *
         * @param message Writable complete SIDP message.
         * @return true on success; false if the message size is invalid.
         */
        [[nodiscard]] static bool set_message_crc(std::span<std::uint8_t> message) noexcept;

        /**
         * @brief Verifies the CRC stored in a complete SIDP message.
         * @param message Complete SIDP message to validate.
         * @return true if the size and CRC are valid, otherwise false.
         */
        [[nodiscard]] static bool verify_message_crc(std::span<const std::uint8_t> message) noexcept;

    private:
        /** @brief Type of the CRC lookup table. */
        using table_t = std::array<std::uint32_t, 256>;

        /** @brief Builds the reflected CRC lookup table at compile time. */
        [[nodiscard]] static consteval table_t make_table() noexcept;

        /** @brief Returns the process-wide compile-time generated lookup table. */
        [[nodiscard]] static const table_t &table() noexcept;
    };

}
