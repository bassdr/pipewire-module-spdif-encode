#pragma once

#include "encoder.h"

#include <cstddef>
#include <cstdint>
#include <expected>
#include <memory>

struct AVCodecContext;
struct AVFrame;
struct AVPacket;

struct AVCodecContextDeleter { void operator()(AVCodecContext* p) const; };
struct AVFrameDeleter { void operator()(AVFrame* p) const; };
struct AVPacketDeleter { void operator()(AVPacket* p) const; };

using UniqueAVCodecContext = std::unique_ptr<AVCodecContext, AVCodecContextDeleter>;
using UniqueAVFrame = std::unique_ptr<AVFrame, AVFrameDeleter>;
using UniqueAVPacket = std::unique_ptr<AVPacket, AVPacketDeleter>;

struct Ac3Encoder
{
    static constexpr int FrameSize = 1536;
    static constexpr uint32_t BurstSize = 6144;
    static constexpr uint16_t DataType = 0x01;

    static std::expected<Ac3Encoder, InitError> Create(int channels, int sampleRate, int bitrate);

    EncodeResult EncodeFrame(int16_t const* samples, int sampleCount,
                             uint8_t* outputBuf, size_t outputBufSize);

private:
    UniqueAVCodecContext m_CodecCtx;
    UniqueAVFrame m_Frame;
    UniqueAVPacket m_Packet;
};
