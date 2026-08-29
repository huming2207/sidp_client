#pragma once

#include <array>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <span>

#include "sidp_defs.hpp"

namespace sidp
{

    /** @brief Timeout value that requests an indefinite wait. */
    inline constexpr std::uint32_t WAIT_FOREVER = std::numeric_limits<std::uint32_t>::max();

    /**
     * @brief Interface for transporting complete SIDP messages.
     *
     * WebSocket implementations map one SIDP message to one binary WebSocket
     * message. Stream implementations, such as USB CDC-ACM, own their framing
     * and return only the decoded SIDP message.
     *
     * One reader and one writer may operate concurrently. Multiple writers
     * must be serialized by the caller so Response/Event ordering is retained.
     *
     * Methods return zero on success and a negative errno value on failure.
     * Implementations return errors directly and do not modify the global
     * @c errno variable.
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
         * @return 0 on success.
         * @return -EINVAL if an output pointer is null.
         * @return -ETIMEDOUT if no complete message arrives before timeout.
         * @return -ENOTCONN if the transport is disconnected or not yet
         *         initialized.
         * @return -ENOBUFS if the receive queue overflowed.
         * @return -EIO for another underlying transport failure.
         */
        [[nodiscard]] virtual int start_read(std::uint8_t **buf_out, std::size_t *len_out, std::uint32_t timeout_ms) noexcept = 0;

        /**
         * @brief Returns a message buffer acquired by start_read().
         * @param buf_return Exact non-null pointer returned by start_read().
         *
         * Passing a foreign pointer, returning a pointer twice, or using it
         * after this call is invalid.
         */
        virtual void end_read(std::uint8_t *buf_return) noexcept = 0;

        /**
         * @brief Writes one complete SIDP message.
         *
         * This is a non-blocking enqueue operation. On success the transport
         * owns a copy of the message and the caller may immediately reuse its
         * buffer. Call flush_write() to drive and wait for transmission.
         *
         * @param message Complete SIDP message to send. The caller retains
         *        ownership and may reuse it after this call returns.
         * @return 0 on success.
         * @return -EINVAL if message is shorter than sizeof(msg_header_t).
         * @return -EMSGSIZE if message exceeds MAX_FRAME_SIZE.
         * @return -EBUSY if a previously enqueued message is still pending.
         * @return -ENOTCONN if the transport is disconnected or not yet
         *         initialized; any previously enqueued message is discarded.
         * @return -EIO for another underlying transport failure.
         */
        [[nodiscard]] virtual int write_message(std::span<const std::uint8_t> message) noexcept = 0;

        /**
         * @brief Drives and waits for all currently enqueued output.
         *
         * If the call times out, unsent data remains queued and a later call
         * may continue flushing it. write_message() returns @c -EBUSY until
         * the queued message has been completely flushed. A disconnected
         * transport discards any pending output instead.
         *
         * @param timeout_ms Maximum wait in milliseconds, or WAIT_FOREVER.
         * @return 0 when no output remains pending.
         * @return -ETIMEDOUT if output remains when the timeout expires.
         * @return -ENOTCONN if the transport is disconnected or not yet
         *         initialized.
         * @return -EIO for another underlying transport failure.
         */
        [[nodiscard]] virtual int flush_write(std::uint32_t timeout_ms) noexcept = 0;

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
