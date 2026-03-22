#pragma once

#include <cstdint>
#include <expected>

enum class InitError : uint8_t
{
    CodecNotFound,
    ContextAllocFailed,
    ConfigQueryFailed,
    CodecOpenFailed,
    FrameAllocFailed,
    FrameBufferAllocFailed,
    PacketAllocFailed,
};

enum class EncodeError : uint8_t
{
    InsufficientSamples,
    FrameAllocFailed,
    SendFrameFailed,
    ReceivePacketFailed,
    OutputBufferTooSmall,
};

using EncodeResult = std::expected<uint32_t, EncodeError>;
