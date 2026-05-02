#include "encoder-av.h"

extern "C"
{
#include <libavcodec/avcodec.h>
#include <libavutil/channel_layout.h>
}

#include <algorithm>
#include <cstring>
#include <ranges>
#include <span>

void AVCodecContextDeleter::operator()(AVCodecContext* p) const { avcodec_free_context(&p); }
void AVFrameDeleter::operator()(AVFrame* p) const { av_frame_free(&p); }
void AVPacketDeleter::operator()(AVPacket* p) const { av_packet_free(&p); }

std::expected<AvEncoder, InitError> AvEncoder::Init(AVCodec const* codec,
                                                     int channels, int sampleRate,
                                                     int64_t bitrate, uint16_t frameSize,
                                                     AvChannelLayoutHint layoutHint)
{
    AvEncoder enc;
    enc.m_FrameSize = frameSize;

    enc.m_CodecCtx.reset(avcodec_alloc_context3(codec));
    if (!enc.m_CodecCtx)
    {
        return std::unexpected(InitError::ContextAllocFailed);
    }

    // Query the codec's preferred sample format
    AVSampleFormat const* sampleFmts = nullptr;
    int numFmts = 0;
    int cfgRet = avcodec_get_supported_config(enc.m_CodecCtx.get(), codec,
                                               AV_CODEC_CONFIG_SAMPLE_FORMAT,
                                               0, reinterpret_cast<void const**>(&sampleFmts),
                                               &numFmts);
    if (cfgRet < 0 || numFmts == 0 || !sampleFmts)
    {
        return std::unexpected(InitError::ConfigQueryFailed);
    }
    // avcodec_alloc_context3 pre-fills codec-specific defaults; we only
    // override the fields that depend on our caller's parameters.
    // Untouched: time_base, flags, thread_count, etc. — codec defaults.
    auto* ctx = enc.m_CodecCtx.get();
    ctx->sample_fmt = sampleFmts[0];
    ctx->sample_rate = sampleRate;
    ctx->bit_rate = bitrate;
    ctx->strict_std_compliance = FF_COMPLIANCE_EXPERIMENTAL; // for DCA encoder

    // ch_layout requires av_channel_layout_copy (it owns allocated memory)
    AVChannelLayout layout{};
    if (layoutHint == AvChannelLayoutHint::Side && channels == 6)
        av_channel_layout_from_string(&layout, "5.1(side)");
    else
        av_channel_layout_default(&layout, channels);
    av_channel_layout_copy(&ctx->ch_layout, &layout);
    av_channel_layout_uninit(&layout);

    if (avcodec_open2(ctx, codec, nullptr) < 0)
    {
        return std::unexpected(InitError::CodecOpenFailed);
    }

    // Verify the codec's frame size matches the expected value
    if (ctx->frame_size != 0 && static_cast<uint16_t>(ctx->frame_size) != frameSize)
    {
        return std::unexpected(InitError::CodecOpenFailed);
    }

    enc.m_Frame.reset(av_frame_alloc());
    if (!enc.m_Frame)
    {
        return std::unexpected(InitError::FrameAllocFailed);
    }

    enc.m_Frame->nb_samples = frameSize;
    enc.m_Frame->format = ctx->sample_fmt;
    av_channel_layout_copy(&enc.m_Frame->ch_layout, &ctx->ch_layout);

    if (av_frame_get_buffer(enc.m_Frame.get(), 0) < 0)
    {
        return std::unexpected(InitError::FrameBufferAllocFailed);
    }

    enc.m_Packet.reset(av_packet_alloc());
    if (!enc.m_Packet)
    {
        return std::unexpected(InitError::PacketAllocFailed);
    }

    return enc;
}

char const* AvEncoder::CodecName() const
{
    return m_CodecCtx ? m_CodecCtx->codec->name : "none";
}

EncodeResult AvEncoder::EncodeFrame(float const* const* channels, uint32_t inputChannels,
                                     size_t offset, uint16_t sampleCount,
                                     uint8_t* outputBuf, size_t outputBufSize)
{
    if (sampleCount < m_FrameSize)
    {
        return std::unexpected(EncodeError::InsufficientSamples);
    }

    // Convert F32P input directly into the codec's frame buffers.
    // The frame is exclusively owned — av_frame_make_writable() is unnecessary.
    uint32_t const numChannels = m_CodecCtx->ch_layout.nb_channels > 0
        ? std::min<uint32_t>(inputChannels, static_cast<uint32_t>(m_CodecCtx->ch_layout.nb_channels))
        : inputChannels;
    auto const frameBufs = std::span{m_Frame->data, numChannels};
    auto const channelSpan = std::span{channels, inputChannels};

    switch (m_CodecCtx->sample_fmt)
    {
    case AV_SAMPLE_FMT_S16P:
        for (auto&& [buf, chPtr] : std::views::zip(frameBufs, channelSpan))
        {
            auto dst = std::span(reinterpret_cast<int16_t*>(buf), m_FrameSize);
            auto src = std::span(chPtr + offset, m_FrameSize);
            for (auto&& [d, s] : std::views::zip(dst, src))
            {
                d = static_cast<int16_t>(std::clamp(s, -1.0f, 1.0f) * 32767.0f);
            }
        }
        break;
    case AV_SAMPLE_FMT_S32P:
        for (auto&& [buf, chPtr] : std::views::zip(frameBufs, channelSpan))
        {
            auto dst = std::span(reinterpret_cast<int32_t*>(buf), m_FrameSize);
            auto src = std::span(chPtr + offset, m_FrameSize);
            for (auto&& [d, s] : std::views::zip(dst, src))
            {
                d = static_cast<int32_t>(std::clamp(s, -1.0f, 1.0f) * 2147483647.0f);
            }
        }
        break;
    case AV_SAMPLE_FMT_S32:
    {
        // Interleaved S32: all channels in data[0], samples interleaved.
        auto dst = std::span(reinterpret_cast<int32_t*>(m_Frame->data[0]),
                             static_cast<size_t>(m_FrameSize) * numChannels);
        for (size_t i = 0; i < m_FrameSize; ++i)
        {
            auto frame = dst.subspan(i * numChannels, numChannels);
            for (auto&& [d, chPtr] : std::views::zip(frame, channelSpan))
            {
                d = static_cast<int32_t>(std::clamp(chPtr[offset + i], -1.0f, 1.0f) * 2147483647.0f);
            }
        }
        break;
    }
    case AV_SAMPLE_FMT_FLTP:
        for (auto&& [buf, chPtr] : std::views::zip(frameBufs, std::span{channels, inputChannels}))
        {
            std::memcpy(buf, chPtr + offset, m_FrameSize * sizeof(float));
        }
        break;
    default:
        break;
    }

    int ret = avcodec_send_frame(m_CodecCtx.get(), m_Frame.get());
    if (ret < 0)
    {
        return std::unexpected(EncodeError::SendFrameFailed);
    }

    ret = avcodec_receive_packet(m_CodecCtx.get(), m_Packet.get());
    if (ret < 0)
    {
        return std::unexpected(EncodeError::ReceivePacketFailed);
    }

    if (static_cast<size_t>(m_Packet->size) > outputBufSize)
    {
        av_packet_unref(m_Packet.get());
        return std::unexpected(EncodeError::OutputBufferTooSmall);
    }

    uint32_t size = m_Packet->size;
    std::memcpy(outputBuf, m_Packet->data, size);
    av_packet_unref(m_Packet.get());

    return size;
}
