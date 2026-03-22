#include <catch2/catch_test_macros.hpp>

#include "../src/encoder-ac3.h"

#include <cstring>
#include <vector>

TEST_CASE("AC3 encoder constants are correct", "[ac3]")
{
    CHECK(Ac3Encoder::FrameSize == 1536);
    CHECK(Ac3Encoder::BurstSize == 6144);
    CHECK(Ac3Encoder::DataType == 0x01);
}

TEST_CASE("AC3 encoder creates successfully", "[ac3]")
{
    auto enc = Ac3Encoder::Create(6, 48000, 448000);
    REQUIRE(enc.has_value());
}

TEST_CASE("AC3 encoder rejects too few samples", "[ac3]")
{
    auto enc = Ac3Encoder::Create(6, 48000, 448000);
    REQUIRE(enc.has_value());

    uint8_t outputBuf[6144];
    std::vector<int16_t> samples(100 * 6, 0);

    auto result = enc->EncodeFrame(samples.data(), 100, outputBuf, sizeof(outputBuf));
    CHECK(!result.has_value());
    CHECK(result.error() == EncodeError::InsufficientSamples);
}

TEST_CASE("AC3 encoder produces valid output from silence", "[ac3]")
{
    auto enc = Ac3Encoder::Create(6, 48000, 448000);
    REQUIRE(enc.has_value());

    // 1536 samples * 6 channels of silence
    std::vector<int16_t> samples(Ac3Encoder::FrameSize * 6, 0);
    uint8_t outputBuf[6144];

    auto result = enc->EncodeFrame(samples.data(), Ac3Encoder::FrameSize,
                                   outputBuf, sizeof(outputBuf));

    REQUIRE(result.has_value());
    CHECK(*result > 0);
    CHECK(*result <= Ac3Encoder::BurstSize);
}

TEST_CASE("AC3 encoder produces output from non-silent input", "[ac3]")
{
    auto enc = Ac3Encoder::Create(6, 48000, 448000);
    REQUIRE(enc.has_value());

    std::vector<int16_t> samples(Ac3Encoder::FrameSize * 6);
    // Generate a simple sine-ish pattern
    for (size_t i = 0; i < samples.size(); i++)
    {
        samples[i] = static_cast<int16_t>((i % 1000) - 500);
    }

    uint8_t outputBuf[6144];
    auto result = enc->EncodeFrame(samples.data(), Ac3Encoder::FrameSize,
                                   outputBuf, sizeof(outputBuf));

    REQUIRE(result.has_value());
    CHECK(*result > 0);
    CHECK(*result <= Ac3Encoder::BurstSize);
}

TEST_CASE("AC3 encoder can encode multiple consecutive frames", "[ac3]")
{
    auto enc = Ac3Encoder::Create(6, 48000, 448000);
    REQUIRE(enc.has_value());

    std::vector<int16_t> samples(Ac3Encoder::FrameSize * 6, 1000);
    uint8_t outputBuf[6144];

    for (int frame = 0; frame < 10; frame++)
    {
        auto result = enc->EncodeFrame(samples.data(), Ac3Encoder::FrameSize,
                                       outputBuf, sizeof(outputBuf));
        INFO("frame: " << frame);
        REQUIRE(result.has_value());
    }
}

TEST_CASE("AC3 encoded output fits in IEC 61937 burst", "[ac3]")
{
    auto enc = Ac3Encoder::Create(6, 48000, 448000);
    REQUIRE(enc.has_value());

    std::vector<int16_t> samples(Ac3Encoder::FrameSize * 6, 16000);
    uint8_t outputBuf[6144];

    auto result = enc->EncodeFrame(samples.data(), Ac3Encoder::FrameSize,
                                   outputBuf, sizeof(outputBuf));

    REQUIRE(result.has_value());
    // Must fit in burst with 8-byte IEC 61937 header
    CHECK(*result + 8 <= Ac3Encoder::BurstSize);
}
