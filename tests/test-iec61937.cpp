#include <catch2/catch_test_macros.hpp>

#include "../src/iec61937.h"

#include <algorithm>
#include <array>
#include <ranges>
#include <span>

TEST_CASE("IEC 61937 burst has correct sync words", "[iec61937]")
{
    std::array<uint8_t, 64> encoded{0xAA};
    std::array<uint8_t, 6144> burst;

    auto result = Iec61937::CreateBurst<0x01>(encoded, burst);

    REQUIRE(result.has_value());

    auto* out16 = reinterpret_cast<uint16_t*>(burst.data());
    CHECK(out16[0] == 0xF872);
    CHECK(out16[1] == 0x4E1F);
}

TEST_CASE("IEC 61937 Pc field contains data type", "[iec61937]")
{
    std::array<uint8_t, 64> encoded{};
    std::array<uint8_t, 6144> burst;

    REQUIRE(Iec61937::CreateBurst<0x01>(encoded, burst).has_value());
    auto* out16 = reinterpret_cast<uint16_t*>(burst.data());
    CHECK(out16[2] == 0x01);  // AC3 data type

    std::array<uint8_t, 2048> burst2;
    REQUIRE(Iec61937::CreateBurst<0x0B>(encoded, burst2).has_value());
    out16 = reinterpret_cast<uint16_t*>(burst2.data());
    CHECK(out16[2] == 0x0B);  // DTS data type
}

TEST_CASE("IEC 61937 Pd field is payload length in bits", "[iec61937]")
{
    std::array<uint8_t, 128> encoded{};
    std::array<uint8_t, 6144> burst;

    REQUIRE(Iec61937::CreateBurst<0x01>(encoded, burst).has_value());

    auto* out16 = reinterpret_cast<uint16_t*>(burst.data());
    CHECK(out16[3] == 128 * 8);
}

TEST_CASE("IEC 61937 payload is byte-swapped (16-bit words) after header", "[iec61937]")
{
    std::array<uint8_t, 4> encoded{0xDE, 0xAD, 0xBE, 0xEF};
    std::array<uint8_t, 6144> burst;

    REQUIRE(Iec61937::CreateBurst<0x01>(encoded, burst).has_value());

    // Each pair of bytes is swapped for IEC 61937 big-endian word order
    CHECK(burst[8] == 0xAD);
    CHECK(burst[9] == 0xDE);
    CHECK(burst[10] == 0xEF);
    CHECK(burst[11] == 0xBE);
}

TEST_CASE("IEC 61937 burst is zero-padded after payload", "[iec61937]")
{
    std::array<uint8_t, 16> encoded;
    std::ranges::fill(encoded, 0xFF);

    std::array<uint8_t, 6144> burst;
    REQUIRE(Iec61937::CreateBurst<0x01>(encoded, burst).has_value());

    // After header (8 bytes) + payload (16 bytes) = byte 24 onwards should be zero
    for (auto&& [i, b] : std::views::enumerate(std::span(burst).subspan(24)))
    {
        INFO("byte index: " << (i + 24));
        CHECK(b == 0);
    }
}

TEST_CASE("IEC 61937 rejects payload larger than burst", "[iec61937]")
{
    std::array<uint8_t, 6144> encoded{};
    std::array<uint8_t, 6144> burst;

    auto result = Iec61937::CreateBurst<0x01>(encoded, burst);
    CHECK(!result.has_value());
    CHECK(result.error() == Iec61937::BurstError::PayloadTooLarge);
}

TEST_CASE("IEC 61937 burst size matches AC3 spec", "[iec61937]")
{
    // AC3: 6144 bytes = 1536 stereo S16LE samples
    CHECK(6144 == 1536 * 2 * sizeof(int16_t));
}
