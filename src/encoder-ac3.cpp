#include "encoder-ac3.h"

extern "C"
{
#include <libavcodec/avcodec.h>
}

std::expected<AvEncoder, InitError> Ac3Encoder::Create(int channels, int sampleRate, int64_t bitrate)
{
    // Prefer floating-point encoder for better audio quality (FLTP avoids
    // the F32->S16 precision loss of ac3_fixed).  Fall back to ac3_fixed if
    // the float variant is unavailable.
    AVCodec const* codec = avcodec_find_encoder_by_name("ac3");
    if (!codec)
    {
        codec = avcodec_find_encoder_by_name("ac3_fixed");
    }
    if (!codec)
    {
        codec = avcodec_find_encoder(AV_CODEC_ID_AC3);
    }
    if (!codec)
    {
        return std::unexpected(InitError::CodecNotFound);
    }

    return AvEncoder::Init(codec, channels, sampleRate, bitrate, FrameSize);
}
