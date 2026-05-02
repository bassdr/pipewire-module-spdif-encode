#pragma once

#include <cstddef>
#include <cstdint>
#include <expected>
#include <memory>

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

struct AVCodecContext;
struct AVCodec;
struct AVFrame;
struct AVPacket;

struct AVCodecContextDeleter { void operator()(AVCodecContext* p) const; };
struct AVFrameDeleter { void operator()(AVFrame* p) const; };
struct AVPacketDeleter { void operator()(AVPacket* p) const; };

using UniqueAVCodecContext = std::unique_ptr<AVCodecContext, AVCodecContextDeleter>;
using UniqueAVFrame = std::unique_ptr<AVFrame, AVFrameDeleter>;
using UniqueAVPacket = std::unique_ptr<AVPacket, AVPacketDeleter>;

// Hint for surround channel placement in the default layout.
enum class AvChannelLayoutHint : uint8_t
{
    Default,  // av_channel_layout_default (RL/RR for 5.1)
    Side,     // 5.1(side) variant (SL/SR) — required by some codecs (e.g. DCA)
};

// Codec-agnostic libavcodec encoder.  Codec-specific wrappers (Ac3Encoder,
// DtsEncoder) perform codec lookup and call Init() with the resolved codec.
struct AvEncoder
{
    static std::expected<AvEncoder, InitError> Init(AVCodec const* codec,
                                                     int channels, int sampleRate,
                                                     int64_t bitrate, uint16_t frameSize,
                                                     AvChannelLayoutHint layoutHint = AvChannelLayoutHint::Default);

    // Encode one frame of F32 planar audio.
    // 'channels' is an array of per-channel float pointers (at least inputChannels entries).
    // Reads FrameSize() samples starting at 'offset' from each channel.
    // 'sampleCount' is the number of available samples (must be >= FrameSize()).
    EncodeResult EncodeFrame(float const* const* channels, uint32_t inputChannels,
                             size_t offset, uint16_t sampleCount,
                             uint8_t* outputBuf, size_t outputBufSize);

    char const* CodecName() const;
    uint16_t FrameSize() const { return m_FrameSize; }

private:
    UniqueAVCodecContext m_CodecCtx;
    UniqueAVFrame m_Frame;
    UniqueAVPacket m_Packet;
    uint16_t m_FrameSize{};
};
