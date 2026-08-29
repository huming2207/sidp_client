#include "sidp_transport.hpp"

#include <cstring>

namespace sidp
{

    consteval crc32_hasher::table_t crc32_hasher::make_table() noexcept
    {
        table_t result{};

        for (std::size_t index = 0; index < result.size(); ++index) {
            std::uint32_t value = static_cast<std::uint32_t>(index);
            for (std::uint8_t bit = 0; bit < 8; ++bit) {
                if ((value & 1u) != 0) {
                    value = (value >> 1u) ^ 0xEDB88320u;
                } else {
                    value >>= 1u;
                }
            }
            result[index] = value;
        }

        return result;
    }

    const crc32_hasher::table_t &crc32_hasher::table() noexcept
    {
        static constexpr table_t lookup_table = make_table();
        return lookup_table;
    }

    std::uint32_t crc32_hasher::update(std::uint32_t state, std::span<const std::uint8_t> data) noexcept
    {
        const table_t &lookup_table = table();

        for (const std::uint8_t byte : data) {
            const auto index = static_cast<std::uint8_t>(state ^ byte);
            state = lookup_table[index] ^ (state >> 8u);
        }

        return state;
    }

    std::uint32_t crc32_hasher::calculate(std::span<const std::uint8_t> data) noexcept
    {
        return finalize(update(INITIAL_VALUE, data));
    }

    bool crc32_hasher::calculate_message(std::span<const std::uint8_t> message, std::uint32_t &crc) noexcept
    {
        if (!transport_intf::is_valid_message_size(message.size())) {
            crc = 0;
            return false;
        }

        constexpr std::size_t crc_offset = offsetof(msg_header_t, crc32);
        constexpr std::size_t payload_offset = sizeof(msg_header_t);
        static_assert(crc_offset == 8, "SIDP CRC must cover the first 8 header bytes");

        std::uint32_t state = update(INITIAL_VALUE, message.first(crc_offset));
        state = update(state, message.subspan(payload_offset));
        crc = finalize(state);
        return true;
    }

    bool crc32_hasher::set_message_crc(std::span<std::uint8_t> message) noexcept
    {
        std::uint32_t crc = 0;
        if (!calculate_message(message, crc)) {
            return false;
        }

        std::memcpy(message.data() + offsetof(msg_header_t, crc32), &crc, sizeof(crc));
        return true;
    }

    bool crc32_hasher::verify_message_crc(std::span<const std::uint8_t> message) noexcept
    {
        std::uint32_t calculated_crc = 0;
        if (!calculate_message(message, calculated_crc)) {
            return false;
        }

        std::uint32_t received_crc = 0;
        std::memcpy(&received_crc, message.data() + offsetof(msg_header_t, crc32), sizeof(received_crc));
        return received_crc == calculated_crc;
    }

}
