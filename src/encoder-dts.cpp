#include "encoder-dts.h"

extern "C"
{
#include <libavcodec/avcodec.h>
}

std::expected<AvEncoder, InitError> DtsEncoder::Create(int channels, int sampleRate, int64_t bitrate)
{
    // DTS encoding requires libdcaenc (or another DTS encoder) compiled into FFmpeg.
    // The codec name is "dca" internally (Digital Coherent Acoustics).
    AVCodec const* codec = avcodec_find_encoder(AV_CODEC_ID_DTS);
    if (!codec)
    {
        return std::unexpected(InitError::CodecNotFound);
    }

    // FFmpeg's DCA encoder requires 5.1(side) layout (SL/SR) rather than
    // the default 5.1 layout (RL/RR).  Use a codec-specific channel layout.
    return AvEncoder::Init(codec, channels, sampleRate, bitrate, FrameSize,
                           AvChannelLayoutHint::Side);
}
