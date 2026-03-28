#pragma once

#include "encoder-av.h"

#include <cstdint>
#include <expected>

struct Ac3Encoder
{
    static constexpr uint16_t FrameSize = 1536;
    static constexpr uint32_t BurstSize = 6144;
    static constexpr uint16_t DataType = 0x01;
    static constexpr uint8_t InputChannels = 6;
    static constexpr int64_t DefaultBitrate = 448000;

    static std::expected<AvEncoder, InitError> Create(int channels, int sampleRate, int64_t bitrate);
};
