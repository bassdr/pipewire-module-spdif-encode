#include "encoder-ac3.h"
#include "iec61937.h"

#include <cstdint>
#include <cstring>
#include <vector>

extern "C"
{
#include <pipewire/pipewire.h>
#include <pipewire/impl-module.h>
#include <spa/param/audio/format-utils.h>
}

static constexpr int InputChannels = 6;
static constexpr int OutputChannels = 2;
static constexpr int SampleRate = 48000;
static constexpr int DefaultBitrate = 448000;

struct ModuleData
{
    struct pw_impl_module *m_Module = nullptr;
    struct pw_context *m_Context = nullptr;

    struct pw_stream *m_CaptureStream = nullptr;
    struct pw_stream *m_PlaybackStream = nullptr;

    struct spa_hook m_ModuleListener = {};
    struct spa_hook m_CaptureListener = {};
    struct spa_hook m_PlaybackListener = {};

    Ac3Encoder m_Enc = {};

    // Ring buffer for accumulating interleaved S16 samples
    std::vector<int16_t> m_RingBuf;
    size_t m_RingWritePos = 0;
    size_t m_RingSamplesStored = 0;  // per-channel sample count

    // Encoding workspace
    std::vector<uint8_t> m_EncodeBuf;
    std::vector<uint8_t> m_BurstBuf;
};

static void ConvertF32PlanarToS16Interleaved(const float *const *src, int16_t *dst,
                                              int channels, int sampleCount)
{
    for (int i = 0; i < sampleCount; i++)
    {
        for (int ch = 0; ch < channels; ch++)
        {
            float sample = src[ch][i];
            if (sample > 1.0f)
            {
                sample = 1.0f;
            }
            else if (sample < -1.0f)
            {
                sample = -1.0f;
            }
            dst[i * channels + ch] = static_cast<int16_t>(sample * 32767.0f);
        }
    }
}

static void OnCaptureProcess(void *userData)
{
    auto *data = static_cast<ModuleData *>(userData);
    struct pw_buffer *buf = pw_stream_dequeue_buffer(data->m_CaptureStream);
    if (!buf)
    {
        return;
    }

    struct spa_buffer *spaBuf = buf->buffer;
    int sampleCount = spaBuf->datas[0].chunk->size / sizeof(float);

    // Collect channel pointers (F32P = one spa_data per channel)
    const float *channelPtrs[InputChannels];
    for (int ch = 0; ch < InputChannels; ch++)
    {
        if (ch < static_cast<int>(spaBuf->n_datas) && spaBuf->datas[ch].data)
        {
            channelPtrs[ch] = static_cast<const float *>(spaBuf->datas[ch].data);
        }
        else
        {
            static const float silence[8192] = {};
            channelPtrs[ch] = silence;
        }
    }

    // Convert to interleaved S16 and write into ring buffer
    int interleavedCount = sampleCount * InputChannels;
    std::vector<int16_t> interleaved(interleavedCount);
    ConvertF32PlanarToS16Interleaved(channelPtrs, interleaved.data(),
                                      InputChannels, sampleCount);

    size_t ringCapacity = data->m_RingBuf.size();
    for (int i = 0; i < interleavedCount; i++)
    {
        data->m_RingBuf[data->m_RingWritePos] = interleaved[i];
        data->m_RingWritePos = (data->m_RingWritePos + 1) % ringCapacity;
    }
    data->m_RingSamplesStored += sampleCount;

    pw_stream_queue_buffer(data->m_CaptureStream, buf);

    if (data->m_RingSamplesStored >= static_cast<size_t>(Ac3Encoder::FrameSize))
    {
        pw_stream_trigger_process(data->m_PlaybackStream);
    }
}

static void OnPlaybackProcess(void *userData)
{
    auto *data = static_cast<ModuleData *>(userData);

    if (data->m_RingSamplesStored < static_cast<size_t>(Ac3Encoder::FrameSize))
    {
        return;
    }

    struct pw_buffer *buf = pw_stream_dequeue_buffer(data->m_PlaybackStream);
    if (!buf)
    {
        return;
    }

    // Read one frame from ring buffer
    int interleavedFrameSize = Ac3Encoder::FrameSize * InputChannels;
    std::vector<int16_t> frame(interleavedFrameSize);
    size_t ringCapacity = data->m_RingBuf.size();
    size_t readPos = (data->m_RingWritePos + ringCapacity
                      - data->m_RingSamplesStored * InputChannels) % ringCapacity;

    for (int i = 0; i < interleavedFrameSize; i++)
    {
        frame[i] = data->m_RingBuf[readPos];
        readPos = (readPos + 1) % ringCapacity;
    }
    data->m_RingSamplesStored -= Ac3Encoder::FrameSize;

    // Encode
    int encodedSize = data->m_Enc.EncodeFrame(frame.data(), Ac3Encoder::FrameSize,
                                               data->m_EncodeBuf.data(),
                                               data->m_EncodeBuf.size());
    if (encodedSize < 0)
    {
        pw_stream_queue_buffer(data->m_PlaybackStream, buf);
        return;
    }

    // IEC 61937 framing
    Iec61937::CreateBurst(data->m_EncodeBuf.data(), encodedSize,
                          Ac3Encoder::DataType, Ac3Encoder::BurstSize,
                          data->m_BurstBuf.data());

    // Write burst to output
    struct spa_buffer *spaBuf = buf->buffer;
    if (spaBuf->datas[0].data)
    {
        int outputBytes = Ac3Encoder::BurstSize;
        if (static_cast<size_t>(outputBytes) > spaBuf->datas[0].maxsize)
        {
            outputBytes = spaBuf->datas[0].maxsize;
        }
        std::memcpy(spaBuf->datas[0].data, data->m_BurstBuf.data(), outputBytes);
        spaBuf->datas[0].chunk->offset = 0;
        spaBuf->datas[0].chunk->size = outputBytes;
        spaBuf->datas[0].chunk->stride = sizeof(int16_t) * OutputChannels;
    }

    pw_stream_queue_buffer(data->m_PlaybackStream, buf);
}

static const struct pw_stream_events CaptureStreamEvents = {
    .version = PW_VERSION_STREAM_EVENTS,
    .process = OnCaptureProcess,
};

static const struct pw_stream_events PlaybackStreamEvents = {
    .version = PW_VERSION_STREAM_EVENTS,
    .process = OnPlaybackProcess,
};

static void ModuleDestroy(void *userData)
{
    auto *data = static_cast<ModuleData *>(userData);

    spa_hook_remove(&data->m_ModuleListener);

    if (data->m_CaptureStream)
    {
        pw_stream_destroy(data->m_CaptureStream);
    }
    if (data->m_PlaybackStream)
    {
        pw_stream_destroy(data->m_PlaybackStream);
    }

    data->m_Enc.Destroy();
    delete data;
}

static const struct pw_impl_module_events ModuleEvents = {
    .version = PW_VERSION_IMPL_MODULE_EVENTS,
    .destroy = ModuleDestroy,
};

extern "C" SPA_EXPORT
int pipewire__module_init(struct pw_impl_module *module, const char *args)
{
    auto *data = new ModuleData();
    data->m_Module = module;
    data->m_Context = pw_impl_module_get_context(module);

    if (!data->m_Enc.Init(InputChannels, SampleRate, DefaultBitrate))
    {
        pw_log_error("spdif-encode: failed to initialize AC3 encoder");
        delete data;
        return -1;
    }

    // Allocate buffers
    data->m_RingBuf.resize(Ac3Encoder::FrameSize * InputChannels * 4, 0);
    data->m_EncodeBuf.resize(Ac3Encoder::BurstSize);
    data->m_BurstBuf.resize(Ac3Encoder::BurstSize);

    pw_impl_module_add_listener(module, &data->m_ModuleListener, &ModuleEvents, data);

    // Capture stream: virtual 5.1 sink
    auto *captureProps = pw_properties_new(
        PW_KEY_NODE_NAME, "spdif-encode-sink",
        PW_KEY_NODE_DESCRIPTION, "S/PDIF Surround Encoder",
        PW_KEY_MEDIA_CLASS, "Audio/Sink",
        PW_KEY_MEDIA_TYPE, "Audio",
        PW_KEY_MEDIA_CATEGORY, "Playback",
        nullptr
    );

    data->m_CaptureStream = pw_stream_new_simple(
        pw_context_get_main_loop(data->m_Context),
        "spdif-encode-capture",
        captureProps,
        &CaptureStreamEvents,
        data
    );

    uint8_t captureParamBuf[1024];
    struct spa_pod_builder captureBuilder = SPA_POD_BUILDER_INIT(captureParamBuf, sizeof(captureParamBuf));

    struct spa_audio_info_raw captureInfo = {};
    captureInfo.format = SPA_AUDIO_FORMAT_F32P;
    captureInfo.rate = SampleRate;
    captureInfo.channels = InputChannels;
    captureInfo.position[0] = SPA_AUDIO_CHANNEL_FL;
    captureInfo.position[1] = SPA_AUDIO_CHANNEL_FR;
    captureInfo.position[2] = SPA_AUDIO_CHANNEL_FC;
    captureInfo.position[3] = SPA_AUDIO_CHANNEL_LFE;
    captureInfo.position[4] = SPA_AUDIO_CHANNEL_RL;
    captureInfo.position[5] = SPA_AUDIO_CHANNEL_RR;

    const struct spa_pod *captureParams[1];
    captureParams[0] = spa_format_audio_raw_build(&captureBuilder, SPA_PARAM_EnumFormat, &captureInfo);

    pw_stream_connect(data->m_CaptureStream,
                      PW_DIRECTION_INPUT,
                      PW_ID_ANY,
                      static_cast<pw_stream_flags>(PW_STREAM_FLAG_AUTOCONNECT |
                                                   PW_STREAM_FLAG_MAP_BUFFERS |
                                                   PW_STREAM_FLAG_RT_PROCESS),
                      captureParams, 1);

    // Playback stream: stereo S16LE to hardware
    auto *playbackProps = pw_properties_new(
        PW_KEY_NODE_NAME, "spdif-encode-output",
        PW_KEY_MEDIA_TYPE, "Audio",
        PW_KEY_MEDIA_CATEGORY, "Playback",
        nullptr
    );

    data->m_PlaybackStream = pw_stream_new_simple(
        pw_context_get_main_loop(data->m_Context),
        "spdif-encode-playback",
        playbackProps,
        &PlaybackStreamEvents,
        data
    );

    uint8_t playbackParamBuf[1024];
    struct spa_pod_builder playbackBuilder = SPA_POD_BUILDER_INIT(playbackParamBuf, sizeof(playbackParamBuf));

    struct spa_audio_info_raw playbackInfo = {};
    playbackInfo.format = SPA_AUDIO_FORMAT_S16_LE;
    playbackInfo.rate = SampleRate;
    playbackInfo.channels = OutputChannels;
    playbackInfo.position[0] = SPA_AUDIO_CHANNEL_FL;
    playbackInfo.position[1] = SPA_AUDIO_CHANNEL_FR;

    const struct spa_pod *playbackParams[1];
    playbackParams[0] = spa_format_audio_raw_build(&playbackBuilder, SPA_PARAM_EnumFormat, &playbackInfo);

    pw_stream_connect(data->m_PlaybackStream,
                      PW_DIRECTION_OUTPUT,
                      PW_ID_ANY,
                      static_cast<pw_stream_flags>(PW_STREAM_FLAG_AUTOCONNECT |
                                                   PW_STREAM_FLAG_MAP_BUFFERS |
                                                   PW_STREAM_FLAG_RT_PROCESS |
                                                   PW_STREAM_FLAG_TRIGGER),
                      playbackParams, 1);

    pw_log_info("spdif-encode: module loaded, AC3 encoder ready");

    return 0;
}
