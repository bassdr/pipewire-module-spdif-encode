#pragma once

#include <cstddef>
#include <cstdint>

struct Ac3Encoder
{
    static constexpr int FrameSize = 1536;
    static constexpr int BurstSize = 6144;
    static constexpr uint16_t DataType = 0x01;

    struct Context;
    Context *m_Ctx = nullptr;

    bool Init(int channels, int sampleRate, int bitrate);
    void Destroy();

    // Encode a complete frame of interleaved S16 samples.
    // Returns bytes written to outputBuf, or -1 on error.
    int EncodeFrame(const int16_t *samples, int sampleCount,
                    uint8_t *outputBuf, size_t outputBufSize);
};
