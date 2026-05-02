#include <catch2/catch_test_macros.hpp>

#include "../src/encoder-dts.h"
#include "../src/iec61937.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <ranges>

static constexpr uint8_t Channels = DtsEncoder::InputChannels;

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

// DTS encoding requires libdcaenc (or another DTS encoder) compiled into FFmpeg.
// Skip all tests gracefully if the codec is not available.
static bool DtsCodecAvailable()
{
    auto enc = DtsEncoder::Create(Channels, 48000, DtsEncoder::DefaultBitrate);
    return enc.has_value();
}

TEST_CASE("DTS encoder constants are correct", "[dts]")
{
    CHECK(DtsEncoder::FrameSize == 512);
    CHECK(DtsEncoder::BurstSize == 2048);
    CHECK(DtsEncoder::DataType == 0x0B);
}

TEST_CASE("DTS encoder creates successfully", "[dts]")
{
    if (!DtsCodecAvailable())
    {
        SKIP("DTS encoder not available (requires FFmpeg with libdcaenc)");
    }

    auto enc = DtsEncoder::Create(Channels, 48000, DtsEncoder::DefaultBitrate);
    REQUIRE(enc.has_value());
    CHECK(enc->FrameSize() == DtsEncoder::FrameSize);
}

TEST_CASE("DTS encoder rejects too few samples", "[dts]")
{
    if (!DtsCodecAvailable())
    {
        SKIP("DTS encoder not available (requires FFmpeg with libdcaenc)");
    }

    auto enc = DtsEncoder::Create(Channels, 48000, DtsEncoder::DefaultBitrate);
    REQUIRE(enc.has_value());

    uint8_t outputBuf[2048];
    F32PBuffer<100> buf;

    auto result = enc->EncodeFrame(buf.ptrs.data(), Channels, 0, 100, outputBuf, sizeof(outputBuf));
    CHECK(!result.has_value());
    CHECK(result.error() == EncodeError::InsufficientSamples);
}

TEST_CASE("DTS encoder produces valid output from silence", "[dts]")
{
    if (!DtsCodecAvailable())
    {
        SKIP("DTS encoder not available (requires FFmpeg with libdcaenc)");
    }

    auto enc = DtsEncoder::Create(Channels, 48000, DtsEncoder::DefaultBitrate);
    REQUIRE(enc.has_value());

    uint8_t outputBuf[2048];
    F32PBuffer<DtsEncoder::FrameSize> buf;

    auto result = enc->EncodeFrame(buf.ptrs.data(), Channels, 0, DtsEncoder::FrameSize,
                                   outputBuf, sizeof(outputBuf));

    REQUIRE(result.has_value());
    CHECK(*result > 0);
    CHECK(*result <= DtsEncoder::BurstSize);
}

TEST_CASE("DTS encoder produces output from non-silent input", "[dts]")
{
    if (!DtsCodecAvailable())
    {
        SKIP("DTS encoder not available (requires FFmpeg with libdcaenc)");
    }

    auto enc = DtsEncoder::Create(Channels, 48000, DtsEncoder::DefaultBitrate);
    REQUIRE(enc.has_value());

    F32PBuffer<DtsEncoder::FrameSize> buf;
    for (auto&& [arr, ptr] : std::views::zip(buf.ch, buf.ptrs))
    {
        float i = 0.f;
        for (auto&& sample : arr)
        {
            sample = std::sin(i) * 0.5f;
            i += 0.1f;
        }
        ptr = arr.data();
    }

    uint8_t outputBuf[2048];
    auto result = enc->EncodeFrame(buf.ptrs.data(), Channels, 0, DtsEncoder::FrameSize,
                                   outputBuf, sizeof(outputBuf));

    REQUIRE(result.has_value());
    CHECK(*result > 0);
    CHECK(*result <= DtsEncoder::BurstSize);
}

TEST_CASE("DTS encoder can encode multiple consecutive frames", "[dts]")
{
    if (!DtsCodecAvailable())
    {
        SKIP("DTS encoder not available (requires FFmpeg with libdcaenc)");
    }

    auto enc = DtsEncoder::Create(Channels, 48000, DtsEncoder::DefaultBitrate);
    REQUIRE(enc.has_value());

    F32PBuffer<DtsEncoder::FrameSize> buf(0.3f);
    uint8_t outputBuf[2048];

    for (auto frame : std::views::iota(0, 10))
    {
        auto result = enc->EncodeFrame(buf.ptrs.data(), Channels, 0, DtsEncoder::FrameSize,
                                       outputBuf, sizeof(outputBuf));
        INFO("frame: " << frame);
        REQUIRE(result.has_value());
    }
}

TEST_CASE("DTS encoded output fits in IEC 61937 burst", "[dts]")
{
    if (!DtsCodecAvailable())
    {
        SKIP("DTS encoder not available (requires FFmpeg with libdcaenc)");
    }

    auto enc = DtsEncoder::Create(Channels, 48000, DtsEncoder::DefaultBitrate);
    REQUIRE(enc.has_value());

    F32PBuffer<DtsEncoder::FrameSize> buf(0.9f);
    uint8_t outputBuf[2048];

    auto result = enc->EncodeFrame(buf.ptrs.data(), Channels, 0, DtsEncoder::FrameSize,
                                   outputBuf, sizeof(outputBuf));

    REQUIRE(result.has_value());
    // Must fit in burst with 8-byte IEC 61937 header
    CHECK(*result + 8 <= DtsEncoder::BurstSize);
}

TEST_CASE("DTS encoder works with non-zero offset", "[dts]")
{
    if (!DtsCodecAvailable())
    {
        SKIP("DTS encoder not available (requires FFmpeg with libdcaenc)");
    }

    auto enc = DtsEncoder::Create(Channels, 48000, DtsEncoder::DefaultBitrate);
    REQUIRE(enc.has_value());

    size_t const offset = 256;
    F32PBuffer<256 + DtsEncoder::FrameSize> buf(0.5f);
    uint8_t outputBuf[2048];

    auto result = enc->EncodeFrame(buf.ptrs.data(), Channels, offset, DtsEncoder::FrameSize,
                                   outputBuf, sizeof(outputBuf));

    REQUIRE(result.has_value());
    CHECK(*result > 0);
}

TEST_CASE("DTS encoded frame contains valid sync word", "[dts]")
{
    if (!DtsCodecAvailable())
    {
        SKIP("DTS encoder not available (requires FFmpeg with libdcaenc)");
    }

    auto enc = DtsEncoder::Create(Channels, 48000, DtsEncoder::DefaultBitrate);
    REQUIRE(enc.has_value());

    F32PBuffer<DtsEncoder::FrameSize> buf(0.5f);
    uint8_t outputBuf[2048];

    auto result = enc->EncodeFrame(buf.ptrs.data(), Channels, 0, DtsEncoder::FrameSize,
                                   outputBuf, sizeof(outputBuf));
    REQUIRE(result.has_value());

    uint32_t const encodedSize = *result;
    INFO("encoded frame size: " << encodedSize << " bytes");
    REQUIRE(encodedSize >= 4);

    // DTS sync word is 0x7FFE8001 in big-endian (16-bit aligned format).
    // FFmpeg's DCA encoder should output this at the start of each frame.
    CHECK(outputBuf[0] == 0x7F);
    CHECK(outputBuf[1] == 0xFE);
    CHECK(outputBuf[2] == 0x80);
    CHECK(outputBuf[3] == 0x01);
}

TEST_CASE("DTS encoded frame size fits IEC 61937 burst with header", "[dts]")
{
    if (!DtsCodecAvailable())
    {
        SKIP("DTS encoder not available (requires FFmpeg with libdcaenc)");
    }

    auto enc = DtsEncoder::Create(Channels, 48000, DtsEncoder::DefaultBitrate);
    REQUIRE(enc.has_value());

    // Encode 100 frames and check every one fits
    F32PBuffer<DtsEncoder::FrameSize> buf;
    uint8_t outputBuf[2048];

    static constexpr size_t MaxPayload = DtsEncoder::BurstSize - sizeof(Iec61937::BurstHeader);

    for (auto frame : std::views::iota(0, 100))
    {
        // Vary input to exercise different frame sizes
        for (auto&& arr : buf.ch)
        {
            float i = 0.f;
            for (auto&& sample : arr)
            {
                sample = std::sin(i + frame * 3.7f) * 0.9f;
                i += 0.1f;
            }
        }

        auto result = enc->EncodeFrame(buf.ptrs.data(), Channels, 0, DtsEncoder::FrameSize,
                                       outputBuf, sizeof(outputBuf));
        INFO("frame: " << frame);
        REQUIRE(result.has_value());
        INFO("encoded size: " << *result << " max payload: " << MaxPayload);
        CHECK(*result <= MaxPayload);
    }
}

TEST_CASE("DTS encode-to-IEC61937 pipeline produces valid burst", "[dts]")
{
    if (!DtsCodecAvailable())
    {
        SKIP("DTS encoder not available (requires FFmpeg with libdcaenc)");
    }

    auto enc = DtsEncoder::Create(Channels, 48000, DtsEncoder::DefaultBitrate);
    REQUIRE(enc.has_value());

    F32PBuffer<DtsEncoder::FrameSize> buf(0.4f);
    uint8_t encodeBuf[2048];

    auto result = enc->EncodeFrame(buf.ptrs.data(), Channels, 0, DtsEncoder::FrameSize,
                                   encodeBuf, sizeof(encodeBuf));
    REQUIRE(result.has_value());

    uint32_t const encodedSize = *result;
    INFO("encoded frame size: " << encodedSize);

    // Wrap in IEC 61937 burst
    std::array<uint8_t, DtsEncoder::BurstSize> burst{};
    auto burstResult = Iec61937::CreateBurst(
        DtsEncoder::DataType,
        {encodeBuf, encodedSize},
        std::span<uint8_t>(burst));
    REQUIRE(burstResult.has_value());

    // Verify IEC 61937 header (stored as native uint16_t, so on LE: bytes are swapped)
    auto const* header = reinterpret_cast<Iec61937::BurstHeader const*>(burst.data());
    CHECK(header->Pa == 0xF872);
    CHECK(header->Pb == 0x4E1F);
    CHECK(header->Pc == DtsEncoder::DataType);
    CHECK(header->Pd == static_cast<uint16_t>(encodedSize * 8));

    // Verify the DTS sync word is byte-swapped in the burst payload.
    // Original big-endian: 7F FE 80 01
    // After 16-bit byte-swap for S16_LE: FE 7F 01 80
    CHECK(burst[8] == 0xFE);
    CHECK(burst[9] == 0x7F);
    CHECK(burst[10] == 0x01);
    CHECK(burst[11] == 0x80);

    // Verify zero-padding after payload
    size_t const payloadEnd = sizeof(Iec61937::BurstHeader) + ((encodedSize + 1) & ~size_t{1});
    for (size_t i = payloadEnd; i < burst.size(); ++i)
    {
        INFO("padding byte at offset " << i);
        CHECK(burst[i] == 0);
    }
}
