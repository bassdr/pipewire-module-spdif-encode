#include "encoder-ac3.h"

extern "C"
{
#include <libavcodec/avcodec.h>
#include <libavutil/channel_layout.h>
}

#include <cstring>

struct Ac3Encoder::Context
{
    const AVCodec *Codec = nullptr;
    AVCodecContext *CodecCtx = nullptr;
    AVFrame *Frame = nullptr;
    AVPacket *Packet = nullptr;
};

bool Ac3Encoder::Init(int channels, int sampleRate, int bitrate)
{
    m_Ctx = new Context();

    // Prefer fixed-point encoder for RT safety
    m_Ctx->Codec = avcodec_find_encoder_by_name("ac3_fixed");
    if (!m_Ctx->Codec)
    {
        m_Ctx->Codec = avcodec_find_encoder(AV_CODEC_ID_AC3);
    }
    if (!m_Ctx->Codec)
    {
        return false;
    }

    m_Ctx->CodecCtx = avcodec_alloc_context3(m_Ctx->Codec);
    if (!m_Ctx->CodecCtx)
    {
        return false;
    }

    // Query the codec's preferred sample format
    const AVSampleFormat *sampleFmts = nullptr;
    int numFmts = 0;
    int cfgRet = avcodec_get_supported_config(m_Ctx->CodecCtx, m_Ctx->Codec,
                                               AV_CODEC_CONFIG_SAMPLE_FORMAT,
                                               0, reinterpret_cast<const void **>(&sampleFmts),
                                               &numFmts);
    if (cfgRet < 0 || numFmts == 0 || !sampleFmts)
    {
        return false;
    }
    m_Ctx->CodecCtx->sample_fmt = sampleFmts[0];
    m_Ctx->CodecCtx->sample_rate = sampleRate;
    m_Ctx->CodecCtx->bit_rate = bitrate;

    AVChannelLayout layout = {};
    av_channel_layout_default(&layout, channels);
    av_channel_layout_copy(&m_Ctx->CodecCtx->ch_layout, &layout);
    av_channel_layout_uninit(&layout);

    if (avcodec_open2(m_Ctx->CodecCtx, m_Ctx->Codec, nullptr) < 0)
    {
        return false;
    }

    m_Ctx->Frame = av_frame_alloc();
    if (!m_Ctx->Frame)
    {
        return false;
    }

    m_Ctx->Frame->nb_samples = FrameSize;
    m_Ctx->Frame->format = m_Ctx->CodecCtx->sample_fmt;
    av_channel_layout_copy(&m_Ctx->Frame->ch_layout, &m_Ctx->CodecCtx->ch_layout);

    if (av_frame_get_buffer(m_Ctx->Frame, 0) < 0)
    {
        return false;
    }

    m_Ctx->Packet = av_packet_alloc();
    if (!m_Ctx->Packet)
    {
        return false;
    }

    return true;
}

void Ac3Encoder::Destroy()
{
    if (!m_Ctx)
    {
        return;
    }

    if (m_Ctx->Frame)
    {
        av_frame_free(&m_Ctx->Frame);
    }
    if (m_Ctx->Packet)
    {
        av_packet_free(&m_Ctx->Packet);
    }
    if (m_Ctx->CodecCtx)
    {
        avcodec_free_context(&m_Ctx->CodecCtx);
    }

    delete m_Ctx;
    m_Ctx = nullptr;
}

int Ac3Encoder::EncodeFrame(const int16_t *samples, int sampleCount,
                            uint8_t *outputBuf, size_t outputBufSize)
{
    if (sampleCount < FrameSize)
    {
        return -1;
    }

    if (av_frame_make_writable(m_Ctx->Frame) < 0)
    {
        return -1;
    }

    int channels = m_Ctx->CodecCtx->ch_layout.nb_channels;

    // Input is interleaved S16, deinterleave to the codec's planar format
    AVSampleFormat fmt = m_Ctx->CodecCtx->sample_fmt;
    for (int ch = 0; ch < channels; ch++)
    {
        if (fmt == AV_SAMPLE_FMT_S16P)
        {
            auto *dst = reinterpret_cast<int16_t *>(m_Ctx->Frame->data[ch]);
            for (int i = 0; i < FrameSize; i++)
            {
                dst[i] = samples[i * channels + ch];
            }
        }
        else if (fmt == AV_SAMPLE_FMT_S32P)
        {
            auto *dst = reinterpret_cast<int32_t *>(m_Ctx->Frame->data[ch]);
            for (int i = 0; i < FrameSize; i++)
            {
                dst[i] = static_cast<int32_t>(samples[i * channels + ch]) << 16;
            }
        }
        else if (fmt == AV_SAMPLE_FMT_FLTP)
        {
            auto *dst = reinterpret_cast<float *>(m_Ctx->Frame->data[ch]);
            for (int i = 0; i < FrameSize; i++)
            {
                dst[i] = samples[i * channels + ch] / 32768.0f;
            }
        }
    }

    int ret = avcodec_send_frame(m_Ctx->CodecCtx, m_Ctx->Frame);
    if (ret < 0)
    {
        return -1;
    }

    ret = avcodec_receive_packet(m_Ctx->CodecCtx, m_Ctx->Packet);
    if (ret < 0)
    {
        return -1;
    }

    if (static_cast<size_t>(m_Ctx->Packet->size) > outputBufSize)
    {
        av_packet_unref(m_Ctx->Packet);
        return -1;
    }

    int size = m_Ctx->Packet->size;
    std::memcpy(outputBuf, m_Ctx->Packet->data, size);
    av_packet_unref(m_Ctx->Packet);

    return size;
}
