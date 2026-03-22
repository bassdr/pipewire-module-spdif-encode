#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>

namespace Iec61937
{

static constexpr uint16_t SyncWord1 = 0xF872;
static constexpr uint16_t SyncWord2 = 0x4E1F;

// Wrap an encoded frame into an IEC 61937 burst.
// outputBuf must be at least burstSize bytes.
// Returns the burst size on success, -1 on error.
inline int CreateBurst(const uint8_t *encodedFrame, int encodedSize,
                       uint16_t dataType, int burstSize,
                       uint8_t *outputBuf)
{
    if (encodedSize + 8 > burstSize)
    {
        return -1;
    }

    std::memset(outputBuf, 0, burstSize);

    auto *out16 = reinterpret_cast<uint16_t *>(outputBuf);
    out16[0] = SyncWord1;  // Pa
    out16[1] = SyncWord2;  // Pb
    out16[2] = dataType;   // Pc
    out16[3] = static_cast<uint16_t>(encodedSize * 8);  // Pd: length in bits

    std::memcpy(outputBuf + 8, encodedFrame, encodedSize);

    return burstSize;
}

} // namespace Iec61937
