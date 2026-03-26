#pragma once

#include <cstdint>
#include <cstring>
#include <expected>

namespace Iec61937
{

static constexpr uint16_t SyncWord1 = 0xF872;
static constexpr uint16_t SyncWord2 = 0x4E1F;

enum class BurstError : uint8_t
{
    PayloadTooLarge,
};

using BurstResult = std::expected<uint32_t, BurstError>;

inline BurstResult CreateBurst(uint8_t const* encodedFrame, uint32_t encodedSize,
                               uint16_t dataType, uint32_t burstSize,
                               uint8_t* outputBuf)
{
    if (encodedSize + 8 > burstSize)
    {
        return std::unexpected(BurstError::PayloadTooLarge);
    }

    std::memset(outputBuf, 0, burstSize);

    auto* out16 = reinterpret_cast<uint16_t*>(outputBuf);
    out16[0] = SyncWord1;  // Pa
    out16[1] = SyncWord2;  // Pb
    out16[2] = dataType;   // Pc
    out16[3] = static_cast<uint16_t>(encodedSize * 8);  // Pd: length in bits

    // Byte-swap the payload: IEC 61937 transmits each 16-bit word in
    // big-endian order, so on little-endian systems every pair of bytes
    // must be swapped (same as ALSA a52 plugin's swab() and FFmpeg's
    // ff_spdif_bswap_buf16).
    auto* dst = outputBuf + 8;
    uint32_t i = 0;
    for (; i + 1 < encodedSize; i += 2)
    {
        dst[i]     = encodedFrame[i + 1];
        dst[i + 1] = encodedFrame[i];
    }
    if (i < encodedSize)
    {
        dst[i] = 0;
    }

    return burstSize;
}

} // namespace Iec61937
