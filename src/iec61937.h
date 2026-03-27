#pragma once

#include <algorithm>
#include <bit>
#include <cstdint>
#include <cstring>
#include <expected>
#include <span>

namespace Iec61937
{

static constexpr uint16_t SyncWord1 = 0xF872;
static constexpr uint16_t SyncWord2 = 0x4E1F;

struct BurstHeader
{
    uint16_t Pa = SyncWord1;
    uint16_t Pb = SyncWord2;
    uint16_t Pc;
    uint16_t Pd;
};

enum class BurstError : uint8_t
{
    PayloadTooLarge,
};

using BurstResult = std::expected<uint32_t, BurstError>;

inline BurstResult CreateBurst(std::span<uint8_t const> encodedFrame,
                               uint16_t dataType, std::span<uint8_t> outputBuf)
{
    uint32_t const encodedSize = static_cast<uint32_t>(encodedFrame.size());
    uint32_t const burstSize = static_cast<uint32_t>(outputBuf.size());

    if (encodedSize + sizeof(BurstHeader) > burstSize)
    {
        return std::unexpected(BurstError::PayloadTooLarge);
    }

    std::ranges::fill(outputBuf, uint8_t{0});

    BurstHeader const header
    {
        .Pc = dataType,
        .Pd = static_cast<uint16_t>(encodedSize * 8),
    };
    std::memcpy(outputBuf.data(), &header, sizeof(header));

    // Byte-swap the payload: IEC 61937 transmits each 16-bit word in
    // big-endian order, so on little-endian systems every pair of bytes
    // must be swapped (same as ALSA a52 plugin's swab() and FFmpeg's
    // ff_spdif_bswap_buf16).
    auto payload = std::span(reinterpret_cast<uint16_t*>(outputBuf.data() + sizeof(header)),
                             encodedSize / 2);
    auto source = std::span(reinterpret_cast<uint16_t const*>(encodedFrame.data()),
                            encodedSize / 2);

    std::ranges::transform(source, payload.begin(), std::byteswap<uint16_t>);

    // Odd trailing byte: pair with zero and swap
    if (encodedSize & 1)
    {
        auto* tail = outputBuf.data() + sizeof(header) + encodedSize - 1;
        tail[0] = 0;
        tail[1] = encodedFrame[encodedSize - 1];
    }

    return burstSize;
}

} // namespace Iec61937
