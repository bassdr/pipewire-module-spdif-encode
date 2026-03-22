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

    std::memcpy(outputBuf + 8, encodedFrame, encodedSize);

    return burstSize;
}

} // namespace Iec61937
