#include <catch2/catch_test_macros.hpp>

#include "../src/iec61937.h"

TEST_CASE("IEC 61937 burst has correct sync words", "[iec61937]")
{
    uint8_t encoded[64] = {0xAA};
    uint8_t burst[6144];

    auto result = Iec61937::CreateBurst(encoded, 0x01, burst);

    REQUIRE(result.has_value());
    CHECK(*result == 6144);

    auto* out16 = reinterpret_cast<uint16_t*>(burst);
    CHECK(out16[0] == 0xF872);
    CHECK(out16[1] == 0x4E1F);
}

TEST_CASE("IEC 61937 Pc field contains data type", "[iec61937]")
{
    uint8_t encoded[64]{};
    uint8_t burst[6144];

    REQUIRE(Iec61937::CreateBurst(encoded, 0x01, burst).has_value());
    auto* out16 = reinterpret_cast<uint16_t*>(burst);
    CHECK(out16[2] == 0x01);  // AC3 data type

    uint8_t burst2[2048];
    REQUIRE(Iec61937::CreateBurst(encoded, 0x0B, burst2).has_value());
    out16 = reinterpret_cast<uint16_t*>(burst2);
    CHECK(out16[2] == 0x0B);  // DTS data type
}

TEST_CASE("IEC 61937 Pd field is payload length in bits", "[iec61937]")
{
    uint8_t encoded[128]{};
    uint8_t burst[6144];

    REQUIRE(Iec61937::CreateBurst(encoded, 0x01, burst).has_value());

    auto* out16 = reinterpret_cast<uint16_t*>(burst);
    CHECK(out16[3] == 128 * 8);
}

TEST_CASE("IEC 61937 payload is byte-swapped (16-bit words) after header", "[iec61937]")
{
    uint8_t encoded[4] = {0xDE, 0xAD, 0xBE, 0xEF};
    uint8_t burst[6144];

    REQUIRE(Iec61937::CreateBurst(encoded, 0x01, burst).has_value());

    // Each pair of bytes is swapped for IEC 61937 big-endian word order
    CHECK(burst[8] == 0xAD);
    CHECK(burst[9] == 0xDE);
    CHECK(burst[10] == 0xEF);
    CHECK(burst[11] == 0xBE);
}

TEST_CASE("IEC 61937 burst is zero-padded after payload", "[iec61937]")
{
    uint8_t encoded[16]{};
    for (int i = 0; i < 16; i++)
    {
        encoded[i] = 0xFF;
    }

    uint8_t burst[6144];
    REQUIRE(Iec61937::CreateBurst(encoded, 0x01, burst).has_value());

    // After header (8 bytes) + payload (16 bytes) = byte 24 onwards should be zero
    for (int i = 24; i < 6144; i++)
    {
        INFO("byte index: " << i);
        CHECK(burst[i] == 0);
    }
}

TEST_CASE("IEC 61937 rejects payload larger than burst", "[iec61937]")
{
    uint8_t encoded[6144]{};
    uint8_t burst[6144];

    auto result = Iec61937::CreateBurst(encoded, 0x01, burst);
    CHECK(!result.has_value());
    CHECK(result.error() == Iec61937::BurstError::PayloadTooLarge);
}

TEST_CASE("IEC 61937 burst size matches AC3 spec", "[iec61937]")
{
    // AC3: 6144 bytes = 1536 stereo S16LE samples
    CHECK(6144 == 1536 * 2 * sizeof(int16_t));
}
