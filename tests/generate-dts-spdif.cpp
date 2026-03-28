// Generates a raw S16_LE stereo file containing IEC 61937-wrapped DTS bursts.
// Use with: aplay -D hw:NVidia -f S16_LE -r 48000 -c 2 /tmp/dts-test.raw
// Or:       pw-play --target=<hdmi-node-id> --format=s16 --rate=48000 --channels=2 /tmp/dts-test.raw
//
// If the receiver decodes this, the encoder and IEC 61937 framing are correct.
// If not, the FFmpeg DCA encoder output may be incompatible with hardware.

#include "../src/encoder-dts.h"
#include "../src/iec61937.h"

#include <array>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ranges>
#include <span>

static constexpr uint8_t Channels = DtsEncoder::InputChannels;
static constexpr uint32_t SampleRate = 48000;
static constexpr uint32_t DurationSec = 5;
static constexpr uint32_t TotalFrames = (SampleRate * DurationSec) / DtsEncoder::FrameSize;

int main(int argc, char** argv)
{
    char const* outPath = (argc > 1) ? argv[1] : "samples/dts-test.raw";

    auto enc = DtsEncoder::Create(Channels, SampleRate, DtsEncoder::DefaultBitrate);
    if (!enc)
    {
        fprintf(stderr, "Failed to create DTS encoder (error %d)\n", static_cast<int>(enc.error()));
        return 1;
    }

    FILE* f = fopen(outPath, "wb");
    if (!f)
    {
        perror("fopen");
        return 1;
    }

    // Generate a 440 Hz sine wave in the front-left channel
    std::array<std::array<float, DtsEncoder::FrameSize>, Channels> pcm{};
    std::array<float const*, Channels> ptrs;
    for (auto&& [arr, ptr] : std::views::zip(pcm, ptrs))
    {
        ptr = arr.data();
    }

    std::array<uint8_t, DtsEncoder::BurstSize> encodeBuf{};
    std::array<uint8_t, DtsEncoder::BurstSize> burst{};

    uint32_t samplePos = 0;

    for (uint32_t frame = 0; frame < TotalFrames; ++frame)
    {
        // Fill FL and FR with a sine wave, silence on other channels
        for (uint32_t i = 0; i < DtsEncoder::FrameSize; ++i)
        {
            float const t = static_cast<float>(samplePos + i) / static_cast<float>(SampleRate);
            float const sine = std::sin(2.0f * 3.14159265f * 440.0f * t) * 0.5f;
            pcm[0][i] = sine;  // FL
            pcm[1][i] = sine;  // FR
            pcm[2][i] = 0.0f;  // FC
            pcm[3][i] = 0.0f;  // LFE
            pcm[4][i] = 0.0f;  // SL
            pcm[5][i] = 0.0f;  // SR
        }
        samplePos += DtsEncoder::FrameSize;

        auto result = enc->EncodeFrame(ptrs.data(), Channels, 0, DtsEncoder::FrameSize,
                                       encodeBuf.data(), encodeBuf.size());
        if (!result)
        {
            fprintf(stderr, "Encode failed at frame %u (error %d)\n",
                    frame, static_cast<int>(result.error()));
            fclose(f);
            return 1;
        }

        uint32_t encodedSize = *result;

        auto burstResult = Iec61937::CreateBurst(
            DtsEncoder::DataType,
            {encodeBuf.data(), encodedSize},
            std::span<uint8_t>(burst));

        if (!burstResult)
        {
            fprintf(stderr, "IEC 61937 burst failed at frame %u (error %d)\n",
                    frame, static_cast<int>(burstResult.error()));
            fclose(f);
            return 1;
        }

        fwrite(burst.data(), 1, burst.size(), f);
    }

    fclose(f);

    uint32_t totalBytes = TotalFrames * DtsEncoder::BurstSize;
    fprintf(stderr, "Wrote %u DTS frames (%u bytes, %.1f seconds) to %s\n",
            TotalFrames, totalBytes, static_cast<float>(DurationSec), outPath);
    fprintf(stderr, "Play with:\n");
    fprintf(stderr, "  aplay -D hw:NVidia -f S16_LE -r 48000 -c 2 %s\n", outPath);
    fprintf(stderr, "  pw-play --target=<hdmi-node-id> --format=s16 --rate=48000 --channels=2 %s\n", outPath);

    return 0;
}
