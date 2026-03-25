#include "encoder-ac3.h"
#include "iec61937.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <memory>
#include <optional>
#include <ranges>
#include <span>

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

struct PWStreamDeleter { void operator()(pw_stream* s) const { pw_stream_destroy(s); } };
using UniquePWStream = std::unique_ptr<pw_stream, PWStreamDeleter>;

struct ModuleData
{
    pw_impl_module* m_Module = nullptr;
    pw_context* m_Context = nullptr;

    UniquePWStream m_CaptureStream;
    UniquePWStream m_PlaybackStream;

    spa_hook m_ModuleListener{};
    spa_hook m_CaptureListener{};
    spa_hook m_PlaybackListener{};

    std::optional<Ac3Encoder> m_Enc;

    // Ring buffer for accumulating interleaved S16 samples
    static constexpr size_t MaxQuantum = 8192;
    static constexpr size_t RingBufSize = Ac3Encoder::FrameSize * InputChannels * 4;
    std::array<int16_t, RingBufSize> m_RingBuf{};
    size_t m_RingWritePos{};
    size_t m_RingSamplesStored{};  // per-channel sample count

    // Scratch buffer for F32P → S16 interleaved conversion
    std::array<int16_t, MaxQuantum * InputChannels> m_InterleavedBuf{};

    // Encoding workspace
    std::array<uint8_t, Ac3Encoder::BurstSize> m_EncodeBuf{};
    std::array<uint8_t, Ac3Encoder::BurstSize> m_BurstBuf{};
};

[[gnu::always_inline]] static inline void ConvertF32PlanarToS16Interleaved(
    std::span<std::span<float const> const> src, std::span<int16_t> dst)
{
    size_t channels = src.size();
    if (channels == 0 || dst.size() % channels != 0)
    {
        std::ranges::fill(dst, 0);
        return;
    }

    for (auto [idx, val] : std::views::enumerate(dst))
    {
        float sample = std::clamp(src[idx % channels][idx / channels], -1.0f, 1.0f);
        val = static_cast<int16_t>(sample * 32767.0f);
    }
}

static void OnCaptureProcess(void* userData)
{
    auto* data = static_cast<ModuleData*>(userData);
    pw_buffer* buf = pw_stream_dequeue_buffer(data->m_CaptureStream.get());
    if (!buf)
    {
        return;
    }

    spa_buffer* spaBuf = buf->buffer;
    size_t sampleCount = std::min(spaBuf->datas[0].chunk->size / sizeof(float), ModuleData::MaxQuantum);

    // Collect channel spans (F32P = one spa_data per channel)
    static std::array<float, ModuleData::MaxQuantum> const silence{};

    std::array<std::span<float const>, InputChannels> channels;
    for (auto [ch, span] : std::views::enumerate(channels))
    {
        auto* ptr = (ch < spaBuf->n_datas && spaBuf->datas[ch].data)
                        ? static_cast<float const*>(spaBuf->datas[ch].data)
                        : silence.data();
        span = {ptr, sampleCount};
    }

    // Convert to interleaved S16 and write into ring buffer
    std::span<int16_t> interleaved{data->m_InterleavedBuf.data(),
                                    sampleCount * InputChannels};
    ConvertF32PlanarToS16Interleaved(channels, interleaved);

    for (int16_t sample : interleaved)
    {
        data->m_RingBuf[data->m_RingWritePos] = sample;
        data->m_RingWritePos = (data->m_RingWritePos + 1) % ModuleData::RingBufSize;
    }
    data->m_RingSamplesStored += sampleCount;

    pw_stream_queue_buffer(data->m_CaptureStream.get(), buf);

    if (data->m_RingSamplesStored >= static_cast<size_t>(Ac3Encoder::FrameSize))
    {
        pw_stream_trigger_process(data->m_PlaybackStream.get());
    }
}

static void OnPlaybackProcess(void* userData)
{
    auto* data = static_cast<ModuleData*>(userData);

    if (data->m_RingSamplesStored < static_cast<size_t>(Ac3Encoder::FrameSize))
    {
        return;
    }

    auto queueBack = [&](pw_buffer* b) { pw_stream_queue_buffer(data->m_PlaybackStream.get(), b); };
    auto buf = std::unique_ptr<pw_buffer, decltype(queueBack)>(
        pw_stream_dequeue_buffer(data->m_PlaybackStream.get()), queueBack);
    if (!buf)
    {
        return;
    }

    // Read one frame from ring buffer
    static constexpr size_t InterleavedFrameSize = Ac3Encoder::FrameSize * InputChannels;
    std::array<int16_t, InterleavedFrameSize> frame{};
    size_t readPos = (data->m_RingWritePos + ModuleData::RingBufSize
                      - data->m_RingSamplesStored * InputChannels) % ModuleData::RingBufSize;

    for (int16_t& sample : frame)
    {
        sample = data->m_RingBuf[readPos];
        readPos = (readPos + 1) % ModuleData::RingBufSize;
    }
    data->m_RingSamplesStored -= Ac3Encoder::FrameSize;

    // Encode
    auto encodeResult = data->m_Enc->EncodeFrame(frame.data(), Ac3Encoder::FrameSize,
                                                 data->m_EncodeBuf.data(),
                                                 data->m_EncodeBuf.size());
    if (!encodeResult)
    {
        return;
    }

    // IEC 61937 framing
    auto burstResult = Iec61937::CreateBurst(data->m_EncodeBuf.data(), *encodeResult,
                                              Ac3Encoder::DataType, Ac3Encoder::BurstSize,
                                              data->m_BurstBuf.data());
    if (!burstResult)
    {
        return;
    }

    // Write burst to output
    spa_buffer* spaBuf = buf->buffer;
    if (spaBuf->datas[0].data)
    {
        uint32_t outputBytes = Ac3Encoder::BurstSize;
        if (outputBytes > spaBuf->datas[0].maxsize)
        {
            outputBytes = spaBuf->datas[0].maxsize;
        }
        std::memcpy(spaBuf->datas[0].data, data->m_BurstBuf.data(), outputBytes);
        spaBuf->datas[0].chunk->offset = 0;
        spaBuf->datas[0].chunk->size = outputBytes;
        spaBuf->datas[0].chunk->stride = sizeof(int16_t) * OutputChannels;
    }
}

static pw_stream_events const CaptureStreamEvents = {
    .version = PW_VERSION_STREAM_EVENTS,
    .destroy = {},
    .state_changed = {},
    .control_info = {},
    .io_changed = {},
    .param_changed = {},
    .add_buffer = {},
    .remove_buffer = {},
    .process = OnCaptureProcess,
    .drained = {},
    .command = {},
    .trigger_done = {},
};

static pw_stream_events const PlaybackStreamEvents = {
    .version = PW_VERSION_STREAM_EVENTS,
    .destroy = {},
    .state_changed = {},
    .control_info = {},
    .io_changed = {},
    .param_changed = {},
    .add_buffer = {},
    .remove_buffer = {},
    .process = OnPlaybackProcess,
    .drained = {},
    .command = {},
    .trigger_done = {},
};

static void ModuleDestroy(void* userData)
{
    auto* data = static_cast<ModuleData*>(userData);

    spa_hook_remove(&data->m_ModuleListener);
    delete data;
}

static pw_impl_module_events const ModuleEvents = {
    .version = PW_VERSION_IMPL_MODULE_EVENTS,
    .destroy = ModuleDestroy,
    .free = {},
    .initialized = {},
    .registered = {},
};

extern "C" SPA_EXPORT
int pipewire__module_init(pw_impl_module* module, char const* /*args*/)
{
    auto* data = new ModuleData();
    data->m_Module = module;
    data->m_Context = pw_impl_module_get_context(module);

    auto enc = Ac3Encoder::Create(InputChannels, SampleRate, DefaultBitrate);
    if (!enc)
    {
        pw_log_error("spdif-encode: failed to initialize AC3 encoder");
        delete data;
        return -1;
    }
    data->m_Enc = std::move(*enc);

    pw_impl_module_add_listener(module, &data->m_ModuleListener, &ModuleEvents, data);

    // Capture stream: virtual 5.1 sink
    auto* captureProps = pw_properties_new(
        PW_KEY_NODE_NAME, "spdif-encode-sink",
        PW_KEY_NODE_DESCRIPTION, "S/PDIF Surround Encoder",
        PW_KEY_MEDIA_CLASS, "Audio/Sink",
        PW_KEY_MEDIA_TYPE, "Audio",
        PW_KEY_MEDIA_CATEGORY, "Playback",
        nullptr
    );

    data->m_CaptureStream.reset(pw_stream_new_simple(
        pw_context_get_main_loop(data->m_Context),
        "spdif-encode-capture",
        captureProps,
        &CaptureStreamEvents,
        data
    ));

    uint8_t captureParamBuf[1024];
    spa_pod_builder captureBuilder = SPA_POD_BUILDER_INIT(captureParamBuf, sizeof(captureParamBuf));

    spa_audio_info_raw captureInfo{};
    captureInfo.format = SPA_AUDIO_FORMAT_F32P;
    captureInfo.rate = SampleRate;
    captureInfo.channels = InputChannels;
    captureInfo.position[0] = SPA_AUDIO_CHANNEL_FL;
    captureInfo.position[1] = SPA_AUDIO_CHANNEL_FR;
    captureInfo.position[2] = SPA_AUDIO_CHANNEL_FC;
    captureInfo.position[3] = SPA_AUDIO_CHANNEL_LFE;
    captureInfo.position[4] = SPA_AUDIO_CHANNEL_RL;
    captureInfo.position[5] = SPA_AUDIO_CHANNEL_RR;

    spa_pod const* captureParams[1];
    captureParams[0] = spa_format_audio_raw_build(&captureBuilder, SPA_PARAM_EnumFormat, &captureInfo);

    pw_stream_connect(data->m_CaptureStream.get(),
                      PW_DIRECTION_INPUT,
                      PW_ID_ANY,
                      static_cast<pw_stream_flags>(PW_STREAM_FLAG_AUTOCONNECT |
                                                   PW_STREAM_FLAG_MAP_BUFFERS |
                                                   PW_STREAM_FLAG_RT_PROCESS),
                      captureParams, 1);

    // Playback stream: stereo S16LE to hardware
    auto* playbackProps = pw_properties_new(
        PW_KEY_NODE_NAME, "spdif-encode-output",
        PW_KEY_MEDIA_TYPE, "Audio",
        PW_KEY_MEDIA_CATEGORY, "Playback",
        nullptr
    );

    data->m_PlaybackStream.reset(pw_stream_new_simple(
        pw_context_get_main_loop(data->m_Context),
        "spdif-encode-playback",
        playbackProps,
        &PlaybackStreamEvents,
        data
    ));

    uint8_t playbackParamBuf[1024];
    spa_pod_builder playbackBuilder = SPA_POD_BUILDER_INIT(playbackParamBuf, sizeof(playbackParamBuf));

    spa_audio_info_raw playbackInfo{};
    playbackInfo.format = SPA_AUDIO_FORMAT_S16_LE;
    playbackInfo.rate = SampleRate;
    playbackInfo.channels = OutputChannels;
    playbackInfo.position[0] = SPA_AUDIO_CHANNEL_FL;
    playbackInfo.position[1] = SPA_AUDIO_CHANNEL_FR;

    spa_pod const* playbackParams[1];
    playbackParams[0] = spa_format_audio_raw_build(&playbackBuilder, SPA_PARAM_EnumFormat, &playbackInfo);

    pw_stream_connect(data->m_PlaybackStream.get(),
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
