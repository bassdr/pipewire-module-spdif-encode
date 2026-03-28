#include <catch2/catch_test_macros.hpp>

#include "../src/encoder-ac3.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <ranges>

static constexpr uint8_t Channels = Ac3Encoder::InputChannels;

// Helper: create F32P buffers with compile-time sample count and return channel pointers
template <size_t Samples>
struct F32PBuffer
{
    std::array<std::array<float, Samples>, Channels> ch{};
    std::array<float const*, Channels> ptrs{};

    F32PBuffer(float value = 0.0f)
    {
        for (auto&& [arr, ptr] : std::views::zip(ch, ptrs))
        {
            std::ranges::fill(arr, value);
            ptr = arr.data();
        }
    }
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
    CHECK(enc->FrameSize() == Ac3Encoder::FrameSize);
}

TEST_CASE("AC3 encoder rejects too few samples", "[ac3]")
{
    auto enc = Ac3Encoder::Create(Channels, 48000, 448000);
    REQUIRE(enc.has_value());

    uint8_t outputBuf[6144];
    F32PBuffer<100> buf;

    auto result = enc->EncodeFrame(buf.ptrs.data(), Channels, 0, 100, outputBuf, sizeof(outputBuf));
    CHECK(!result.has_value());
    CHECK(result.error() == EncodeError::InsufficientSamples);
}

TEST_CASE("AC3 encoder produces valid output from silence", "[ac3]")
{
    auto enc = Ac3Encoder::Create(Channels, 48000, 448000);
    REQUIRE(enc.has_value());

    uint8_t outputBuf[6144];
    F32PBuffer<Ac3Encoder::FrameSize> buf;

    auto result = enc->EncodeFrame(buf.ptrs.data(), Channels, 0, Ac3Encoder::FrameSize,
                                   outputBuf, sizeof(outputBuf));

    REQUIRE(result.has_value());
    CHECK(*result > 0);
    CHECK(*result <= Ac3Encoder::BurstSize);
}

TEST_CASE("AC3 encoder produces output from non-silent input", "[ac3]")
{
    auto enc = Ac3Encoder::Create(Channels, 48000, 448000);
    REQUIRE(enc.has_value());

    F32PBuffer<Ac3Encoder::FrameSize> buf;
    for (auto&& [arr, ptr] : std::views::zip(buf.ch, buf.ptrs))
    {
        for (auto&& [i, sample] : std::views::enumerate(arr))
        {
            sample = std::sin(static_cast<float>(i) * 0.1f) * 0.5f;
        }
        ptr = arr.data();
    }

    uint8_t outputBuf[6144];
    auto result = enc->EncodeFrame(buf.ptrs.data(), Channels, 0, Ac3Encoder::FrameSize,
                                   outputBuf, sizeof(outputBuf));

    REQUIRE(result.has_value());
    CHECK(*result > 0);
    CHECK(*result <= Ac3Encoder::BurstSize);
}

TEST_CASE("AC3 encoder can encode multiple consecutive frames", "[ac3]")
{
    auto enc = Ac3Encoder::Create(Channels, 48000, 448000);
    REQUIRE(enc.has_value());

    F32PBuffer<Ac3Encoder::FrameSize> buf(0.3f);
    uint8_t outputBuf[6144];

    for (auto frame : std::views::iota(0, 10))
    {
        auto result = enc->EncodeFrame(buf.ptrs.data(), Channels, 0, Ac3Encoder::FrameSize,
                                       outputBuf, sizeof(outputBuf));
        INFO("frame: " << frame);
        REQUIRE(result.has_value());
    }
}

TEST_CASE("AC3 encoded output fits in IEC 61937 burst", "[ac3]")
{
    auto enc = Ac3Encoder::Create(Channels, 48000, 448000);
    REQUIRE(enc.has_value());

    F32PBuffer<Ac3Encoder::FrameSize> buf(0.9f);
    uint8_t outputBuf[6144];

    auto result = enc->EncodeFrame(buf.ptrs.data(), Channels, 0, Ac3Encoder::FrameSize,
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
    size_t const offset = 512;
    F32PBuffer<512 + Ac3Encoder::FrameSize> buf(0.5f);
    uint8_t outputBuf[6144];

    auto result = enc->EncodeFrame(buf.ptrs.data(), Channels, offset, Ac3Encoder::FrameSize,
                                   outputBuf, sizeof(outputBuf));

    REQUIRE(result.has_value());
    CHECK(*result > 0);
}
