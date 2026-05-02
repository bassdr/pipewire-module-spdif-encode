#pragma once

#include "encoder-av.h"

#include <cstdint>
#include <expected>

// DTS Core (DCA) encoder for IEC 61937 Type I transport over S/PDIF.
// DTS-HD (7.1, lossless) requires HDMI HBR and is out of scope.
struct DtsEncoder
{
    static constexpr uint16_t FrameSize = 512;
    static constexpr uint32_t BurstSize = 2048;
    static constexpr uint16_t DataType = 0x0B;
    static constexpr uint32_t InputChannels = 6;
    // DVD-standard DTS bitrate: rate * 503 / 16 = 1,509,000 at 48 kHz.
    // Must stay ≤ 1,509,750 so encoded frames fit in the IEC 61937
    // Type I burst (max payload 2013 bytes per the spdif transport spec).
    static constexpr int64_t DefaultBitrate = 1509000;

    static std::expected<AvEncoder, InitError> Create(int channels, int sampleRate, int64_t bitrate);
};
