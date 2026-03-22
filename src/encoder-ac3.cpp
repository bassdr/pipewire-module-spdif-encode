#include "encoder-ac3.h"

extern "C"
{
#include <libavcodec/avcodec.h>
#include <libavutil/channel_layout.h>
}

#include <cstring>

void AVCodecContextDeleter::operator()(AVCodecContext* p) const { avcodec_free_context(&p); }
void AVFrameDeleter::operator()(AVFrame* p) const { av_frame_free(&p); }
void AVPacketDeleter::operator()(AVPacket* p) const { av_packet_free(&p); }

std::expected<Ac3Encoder, InitError> Ac3Encoder::Create(int channels, int sampleRate, int bitrate)
{
    // Prefer fixed-point encoder for RT safety
    AVCodec const* codec = avcodec_find_encoder_by_name("ac3_fixed");
    if (!codec)
    {
        codec = avcodec_find_encoder(AV_CODEC_ID_AC3);
    }
    if (!codec)
    {
        return std::unexpected(InitError::CodecNotFound);
    }

    Ac3Encoder enc;

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
    enc.m_CodecCtx->sample_fmt = sampleFmts[0];
    enc.m_CodecCtx->sample_rate = sampleRate;
    enc.m_CodecCtx->bit_rate = bitrate;

    AVChannelLayout layout{};
    av_channel_layout_default(&layout, channels);
    av_channel_layout_copy(&enc.m_CodecCtx->ch_layout, &layout);
    av_channel_layout_uninit(&layout);

    if (avcodec_open2(enc.m_CodecCtx.get(), codec, nullptr) < 0)
    {
        return std::unexpected(InitError::CodecOpenFailed);
    }

    enc.m_Frame.reset(av_frame_alloc());
    if (!enc.m_Frame)
    {
        return std::unexpected(InitError::FrameAllocFailed);
    }

    enc.m_Frame->nb_samples = FrameSize;
    enc.m_Frame->format = enc.m_CodecCtx->sample_fmt;
    av_channel_layout_copy(&enc.m_Frame->ch_layout, &enc.m_CodecCtx->ch_layout);

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

EncodeResult Ac3Encoder::EncodeFrame(int16_t const* samples, int sampleCount,
                                     uint8_t* outputBuf, size_t outputBufSize)
{
    if (sampleCount < FrameSize)
    {
        return std::unexpected(EncodeError::InsufficientSamples);
    }

    if (av_frame_make_writable(m_Frame.get()) < 0)
    {
        return std::unexpected(EncodeError::FrameAllocFailed);
    }

    int channels = m_CodecCtx->ch_layout.nb_channels;

    // Input is interleaved S16, deinterleave to the codec's planar format
    switch (m_CodecCtx->sample_fmt)
    {
    case AV_SAMPLE_FMT_S16P:
        for (int ch = 0; ch < channels; ch++)
        {
            auto* dst = reinterpret_cast<int16_t*>(m_Frame->data[ch]);
            for (int i = 0; i < FrameSize; i++)
            {
                dst[i] = samples[i * channels + ch];
            }
        }
        break;
    case AV_SAMPLE_FMT_S32P:
        for (int ch = 0; ch < channels; ch++)
        {
            auto* dst = reinterpret_cast<int32_t*>(m_Frame->data[ch]);
            for (int i = 0; i < FrameSize; i++)
            {
                dst[i] = static_cast<int32_t>(samples[i * channels + ch]) << 16;
            }
        }
        break;
    case AV_SAMPLE_FMT_FLTP:
        for (int ch = 0; ch < channels; ch++)
        {
            auto* dst = reinterpret_cast<float*>(m_Frame->data[ch]);
            for (int i = 0; i < FrameSize; i++)
            {
                dst[i] = samples[i * channels + ch] / 32768.0f;
            }
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
