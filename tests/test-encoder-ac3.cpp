#include <catch2/catch_test_macros.hpp>

#include "../src/encoder-ac3.h"

#include <array>
#include <cmath>
#include <vector>

static constexpr int Channels = 6;

// Helper: create F32P silence buffers and return channel pointers
struct F32PBuffer
{
    std::array<std::vector<float>, Channels> ch;
    std::array<float const*, Channels> ptrs;

    F32PBuffer(int samples, float value = 0.0f)
    {
        for (int c = 0; c < Channels; ++c)
        {
            ch[c].assign(samples, value);
            ptrs[c] = ch[c].data();
        }
    }

    float const* const* data() { return ptrs.data(); }
};

TEST_CASE("AC3 encoder constants are correct", "[ac3]")
{
    CHECK(Ac3Encoder::FrameSize == 1536);
    CHECK(Ac3Encoder::BurstSize == 6144);
    CHECK(Ac3Encoder::DataType == 0x01);
}

TEST_CASE("AC3 encoder creates successfully", "[ac3]")
{
    auto enc = Ac3Encoder::Create(Channels, 48000, 448000);
    REQUIRE(enc.has_value());
}

TEST_CASE("AC3 encoder rejects too few samples", "[ac3]")
{
    auto enc = Ac3Encoder::Create(Channels, 48000, 448000);
    REQUIRE(enc.has_value());

    uint8_t outputBuf[6144];
    F32PBuffer buf(100);

    auto result = enc->EncodeFrame(buf.data(), 0, 100, outputBuf, sizeof(outputBuf));
    CHECK(!result.has_value());
    CHECK(result.error() == EncodeError::InsufficientSamples);
}

TEST_CASE("AC3 encoder produces valid output from silence", "[ac3]")
{
    auto enc = Ac3Encoder::Create(Channels, 48000, 448000);
    REQUIRE(enc.has_value());

    uint8_t outputBuf[6144];
    F32PBuffer buf(Ac3Encoder::FrameSize);

    auto result = enc->EncodeFrame(buf.data(), 0, Ac3Encoder::FrameSize,
                                   outputBuf, sizeof(outputBuf));

    REQUIRE(result.has_value());
    CHECK(*result > 0);
    CHECK(*result <= Ac3Encoder::BurstSize);
}

TEST_CASE("AC3 encoder produces output from non-silent input", "[ac3]")
{
    auto enc = Ac3Encoder::Create(Channels, 48000, 448000);
    REQUIRE(enc.has_value());

    F32PBuffer buf(Ac3Encoder::FrameSize);
    for (int c = 0; c < Channels; ++c)
    {
        for (int i = 0; i < Ac3Encoder::FrameSize; ++i)
        {
            buf.ch[c][i] = std::sin(static_cast<float>(i) * 0.1f) * 0.5f;
        }
        buf.ptrs[c] = buf.ch[c].data();
    }

    uint8_t outputBuf[6144];
    auto result = enc->EncodeFrame(buf.data(), 0, Ac3Encoder::FrameSize,
                                   outputBuf, sizeof(outputBuf));

    REQUIRE(result.has_value());
    CHECK(*result > 0);
    CHECK(*result <= Ac3Encoder::BurstSize);
}

TEST_CASE("AC3 encoder can encode multiple consecutive frames", "[ac3]")
{
    auto enc = Ac3Encoder::Create(Channels, 48000, 448000);
    REQUIRE(enc.has_value());

    F32PBuffer buf(Ac3Encoder::FrameSize, 0.3f);
    uint8_t outputBuf[6144];

    for (int frame = 0; frame < 10; frame++)
    {
        auto result = enc->EncodeFrame(buf.data(), 0, Ac3Encoder::FrameSize,
                                       outputBuf, sizeof(outputBuf));
        INFO("frame: " << frame);
        REQUIRE(result.has_value());
    }
}

TEST_CASE("AC3 encoded output fits in IEC 61937 burst", "[ac3]")
{
    auto enc = Ac3Encoder::Create(Channels, 48000, 448000);
    REQUIRE(enc.has_value());

    F32PBuffer buf(Ac3Encoder::FrameSize, 0.9f);
    uint8_t outputBuf[6144];

    auto result = enc->EncodeFrame(buf.data(), 0, Ac3Encoder::FrameSize,
                                   outputBuf, sizeof(outputBuf));

    REQUIRE(result.has_value());
    // Must fit in burst with 8-byte IEC 61937 header
    CHECK(*result + 8 <= Ac3Encoder::BurstSize);
}

TEST_CASE("AC3 encoder works with non-zero offset", "[ac3]")
{
    auto enc = Ac3Encoder::Create(Channels, 48000, 448000);
    REQUIRE(enc.has_value());

    // Buffer with extra samples before the frame
    int const offset = 512;
    F32PBuffer buf(offset + Ac3Encoder::FrameSize, 0.5f);
    uint8_t outputBuf[6144];

    auto result = enc->EncodeFrame(buf.data(), offset, Ac3Encoder::FrameSize,
                                   outputBuf, sizeof(outputBuf));

    REQUIRE(result.has_value());
    CHECK(*result > 0);
}
