#pragma once

#include <algorithm>
#include <array>
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
    LengthFieldOverflow,
};

using BurstResult = std::expected<void, BurstError>;

template <uint16_t DataType, size_t BurstSize>
inline BurstResult CreateBurst(std::span<uint8_t const> encodedFrame,
                               std::span<uint8_t, BurstSize> outputBuf)
{
    size_t const encodedSize = encodedFrame.size();

    if (encodedSize + sizeof(BurstHeader) > BurstSize)
    {
        return std::unexpected(BurstError::PayloadTooLarge);
    }

    if (encodedSize > UINT16_MAX / 8)
    {
        return std::unexpected(BurstError::LengthFieldOverflow);
    }

    BurstHeader const header
    {
        .Pc = DataType,
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

    // Zero only the padding after header + payload (round up to even byte count)
    size_t const payloadEnd = sizeof(header) + ((encodedSize + 1) & ~size_t{1});
    std::memset(outputBuf.data() + payloadEnd, 0, BurstSize - payloadEnd);

    return {};
}

template <uint16_t DataType, size_t BurstSize>
inline BurstResult CreateBurst(std::span<uint8_t const> encodedFrame,
                               std::array<uint8_t, BurstSize>& outputBuf)
{
    return CreateBurst<DataType>(encodedFrame, std::span<uint8_t, BurstSize>(outputBuf));
}

} // namespace Iec61937
