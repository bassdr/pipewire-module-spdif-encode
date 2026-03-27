#include "encoder-ac3.h"
#include "iec61937.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <memory>
#include <optional>
#include <span>

extern "C"
{
#include <pipewire/pipewire.h>
#include <pipewire/impl-module.h>
#include <spa/param/audio/format-utils.h>
#include <spa/param/props.h>
#include <spa/pod/builder.h>
#include <spa/pod/iter.h>
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

    // Residual buffer: holds leftover F32P samples between quantums that
    // didn't fill a complete 1536-sample AC3 frame.  At most 1535 samples
    // per channel can be pending.
    static constexpr size_t MaxResidual = Ac3Encoder::FrameSize - 1;
    std::array<std::array<float, MaxResidual>, InputChannels> m_Residual{};
    size_t m_ResidualCount{};  // per-channel sample count

    // Encoding workspace — holds raw AC3 bitstream from encoder
    std::array<uint8_t, Ac3Encoder::BurstSize> m_EncodeBuf{};

    // Output ring buffer: stores IEC 61937 bursts as raw bytes.
    // Each AC3 burst is exactly BurstSize (6144) bytes.  Ring holds 4 bursts.
    // Write position is always burst-aligned so bursts never straddle the wrap.
    static constexpr size_t BurstFrames = Ac3Encoder::BurstSize / (sizeof(int16_t) * OutputChannels);
    static constexpr size_t OutRingBursts = 4;
    static constexpr size_t OutRingBytes = Ac3Encoder::BurstSize * OutRingBursts;
    std::array<uint8_t, OutRingBytes> m_OutRingBuf{};
    size_t m_OutRingWritePos{};  // in bytes, always burst-aligned
    size_t m_OutRingReadPos{};   // in bytes
    size_t m_OutRingStored{};    // in bytes
};

// Push an encoded AC3 frame into the output ring as an IEC 61937 burst.
static void PushBurst(ModuleData* data, uint32_t encodedSize)
{
    if (data->m_OutRingStored + Ac3Encoder::BurstSize > ModuleData::OutRingBytes)
    {
        // Output ring full — drop oldest burst
        data->m_OutRingReadPos = (data->m_OutRingReadPos + Ac3Encoder::BurstSize)
                                 % ModuleData::OutRingBytes;
        data->m_OutRingStored -= Ac3Encoder::BurstSize;
    }

    auto burstSpan = std::span(data->m_OutRingBuf.data() + data->m_OutRingWritePos,
                               Ac3Encoder::BurstSize);
    (void)Iec61937::CreateBurst({data->m_EncodeBuf.data(), encodedSize},
                                Ac3Encoder::DataType, burstSpan);
    data->m_OutRingWritePos = (data->m_OutRingWritePos + Ac3Encoder::BurstSize)
                              % ModuleData::OutRingBytes;
    data->m_OutRingStored += Ac3Encoder::BurstSize;
}

// Encode all complete AC3 frames from the available sample data.
// 'channels' points to per-channel float arrays; samples [offset, offset+newSamples)
// are the new data from this quantum.
static void EncodeAvailable(ModuleData* data, float const* const* channels,
                            size_t offset, size_t newSamples)
{
    size_t available = data->m_ResidualCount + newSamples;
    size_t consumed = 0;  // new samples consumed so far

    while (available >= static_cast<size_t>(Ac3Encoder::FrameSize))
    {
        float const* framePtrs[InputChannels];
        int frameOffset = 0;

        if (data->m_ResidualCount > 0)
        {
            // Composite frame: residual prefix + beginning of new data
            // Use thread_local to avoid stack allocation every iteration
            thread_local std::array<std::array<float, Ac3Encoder::FrameSize>, InputChannels> composite;

            size_t const needed = Ac3Encoder::FrameSize - data->m_ResidualCount;
            for (int ch = 0; ch < InputChannels; ++ch)
            {
                std::memcpy(composite[ch].data(), data->m_Residual[ch].data(),
                            data->m_ResidualCount * sizeof(float));
                std::memcpy(composite[ch].data() + data->m_ResidualCount,
                            channels[ch] + offset + consumed, needed * sizeof(float));
                framePtrs[ch] = composite[ch].data();
            }
            consumed += needed;
            available -= Ac3Encoder::FrameSize;
            data->m_ResidualCount = 0;
        }
        else
        {
            // Encode directly from source pointers at the right offset
            for (int ch = 0; ch < InputChannels; ++ch)
            {
                framePtrs[ch] = channels[ch];
            }
            frameOffset = static_cast<int>(offset + consumed);
            consumed += Ac3Encoder::FrameSize;
            available -= Ac3Encoder::FrameSize;
        }

        auto encodeResult = data->m_Enc->EncodeFrame(
            framePtrs, frameOffset, Ac3Encoder::FrameSize,
            data->m_EncodeBuf.data(), data->m_EncodeBuf.size());
        if (!encodeResult)
        {
            continue;
        }

        PushBurst(data, *encodeResult);
    }

    // Store remaining samples as residual for next quantum
    if (available > 0)
    {
        size_t const tailStart = offset + consumed;
        for (int ch = 0; ch < InputChannels; ++ch)
        {
            std::memcpy(data->m_Residual[ch].data(), channels[ch] + tailStart,
                        available * sizeof(float));
        }
        data->m_ResidualCount = available;
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
    size_t sampleCount = spaBuf->datas[0].chunk->size / sizeof(float);

    // Collect channel pointers (F32P = one spa_data per channel)
    static std::array<float, 8192> const silence{};

    float const* channels[InputChannels];
    for (int ch = 0; ch < InputChannels; ++ch)
    {
        channels[ch] = (ch < static_cast<int>(spaBuf->n_datas) && spaBuf->datas[ch].data)
                            ? static_cast<float const*>(spaBuf->datas[ch].data)
                            : silence.data();
    }

    EncodeAvailable(data, channels, 0, sampleCount);

    pw_stream_queue_buffer(data->m_CaptureStream.get(), buf);

    // Always trigger playback to drain the output ring
    pw_stream_trigger_process(data->m_PlaybackStream.get());
}

static void OnPlaybackProcess(void* userData)
{
    auto* data = static_cast<ModuleData*>(userData);

    auto queueBack = [&](pw_buffer* b) { pw_stream_queue_buffer(data->m_PlaybackStream.get(), b); };
    auto buf = std::unique_ptr<pw_buffer, decltype(queueBack)>(
        pw_stream_dequeue_buffer(data->m_PlaybackStream.get()), queueBack);
    if (!buf)
    {
        return;
    }

    spa_buffer* spaBuf = buf->buffer;
    if (!spaBuf->datas[0].data)
    {
        return;
    }

    // Determine how many stereo frames the output buffer can hold
    uint32_t const maxFrames = spaBuf->datas[0].maxsize / (sizeof(int16_t) * OutputChannels);

    // Use buf->requested if available (= quantum), else fill the buffer
    uint32_t outFrames = buf->requested ? static_cast<uint32_t>(buf->requested) : maxFrames;
    if (outFrames > maxFrames)
    {
        outFrames = maxFrames;
    }

    uint32_t const outBytes = outFrames * sizeof(int16_t) * OutputChannels;
    auto output = std::span(static_cast<uint8_t*>(spaBuf->datas[0].data), outBytes);

    // Drain output ring into the PipeWire buffer (up to two contiguous segments)
    uint32_t const ringBytes = std::min(outBytes, static_cast<uint32_t>(data->m_OutRingStored));
    size_t const firstRun = std::min(static_cast<size_t>(ringBytes),
                                     ModuleData::OutRingBytes - data->m_OutRingReadPos);
    auto ringStart = data->m_OutRingBuf.begin() + data->m_OutRingReadPos;

    auto it = std::ranges::copy(ringStart, ringStart + firstRun, output.begin()).out;
    if (firstRun < ringBytes)
    {
        it = std::ranges::copy_n(data->m_OutRingBuf.begin(), ringBytes - firstRun, it).out;
    }
    data->m_OutRingReadPos = (data->m_OutRingReadPos + ringBytes) % ModuleData::OutRingBytes;
    data->m_OutRingStored -= ringBytes;

    // Zero-fill remainder (IEC 61937 silence = zeros)
    std::ranges::fill(std::span(it, output.end()), uint8_t{0});

    static constexpr int32_t FrameBytes = sizeof(int16_t) * OutputChannels;
    *spaBuf->datas[0].chunk = {
        .offset = 0,
        .size = outBytes,
        .stride = FrameBytes,
        .flags = {},
    };
}

static void ForceUnitVolume(pw_stream* stream)
{
    float const volumes[OutputChannels] = {1.0f, 1.0f};

    uint8_t buf[1024];
    spa_pod_builder b = SPA_POD_BUILDER_INIT(buf, sizeof(buf));
    spa_pod_frame f;

    spa_pod_builder_push_object(&b, &f, SPA_TYPE_OBJECT_Props, SPA_PARAM_Props);
    spa_pod_builder_prop(&b, SPA_PROP_channelVolumes, 0);
    spa_pod_builder_array(&b, sizeof(float), SPA_TYPE_Float, OutputChannels, volumes);
    spa_pod_builder_prop(&b, SPA_PROP_softVolumes, 0);
    spa_pod_builder_array(&b, sizeof(float), SPA_TYPE_Float, OutputChannels, volumes);
    spa_pod_builder_prop(&b, SPA_PROP_mute, 0);
    spa_pod_builder_bool(&b, false);
    auto* param = static_cast<spa_pod*>(spa_pod_builder_pop(&b, &f));

    pw_stream_set_param(stream, SPA_PARAM_Props, param);
}

static bool HasNonUnitVolume(spa_pod_object const* obj, uint32_t key)
{
    auto const* prop = spa_pod_object_find_prop(obj, nullptr, key);
    if (!prop)
    {
        return false;
    }

    std::array<float, SPA_AUDIO_MAX_CHANNELS> vols{};
    uint32_t n = spa_pod_copy_array(&prop->value, SPA_TYPE_Float, vols.data(), vols.size());
    return std::ranges::any_of(std::span(vols).first(n), [](float v) { return v != 1.0f; });
}

static bool HasMute(spa_pod_object const* obj)
{
    auto const* prop = spa_pod_object_find_prop(obj, nullptr, SPA_PROP_mute);
    if (!prop)
    {
        return false;
    }

    bool muted = false;
    spa_pod_get_bool(&prop->value, &muted);
    return muted;
}

static void OnPlaybackParamChanged(void* userData, uint32_t id, spa_pod const* param)
{
    if (id != SPA_PARAM_Props || !param)
    {
        return;
    }

    auto const* obj = reinterpret_cast<spa_pod_object const*>(param);

    if (HasNonUnitVolume(obj, SPA_PROP_channelVolumes)
        || HasNonUnitVolume(obj, SPA_PROP_softVolumes)
        || HasMute(obj))
    {
        pw_log_warn("spdif-encode: volume/mute changed on output stream, "
                    "resetting to 1.0 (encoded bitstream cannot be volume-adjusted)");
        auto* data = static_cast<ModuleData*>(userData);
        ForceUnitVolume(data->m_PlaybackStream.get());
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
    .param_changed = OnPlaybackParamChanged,
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
int pipewire__module_init(pw_impl_module* module, char const* args)
{
    auto* moduleProps = args ? pw_properties_new_string(args) : pw_properties_new(nullptr, nullptr);
    auto* data = new ModuleData();
    data->m_Module = module;
    data->m_Context = pw_impl_module_get_context(module);

    auto enc = Ac3Encoder::Create(InputChannels, SampleRate, DefaultBitrate);
    if (!enc)
    {
        pw_log_error("spdif-encode: failed to initialize AC3 encoder");
        pw_properties_free(moduleProps);
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
    // The output is IEC 61937-framed AC3 data disguised as plain stereo PCM.
    // We must prevent audioconvert from modifying the bitstream.
    auto* playbackProps = pw_properties_new(
        PW_KEY_NODE_NAME, "spdif-encode-output",
        PW_KEY_MEDIA_TYPE, "Audio",
        PW_KEY_MEDIA_CATEGORY, "Playback",
        PW_KEY_MEDIA_CLASS, "Stream/Output/Audio",
        PW_KEY_NODE_DONT_RECONNECT, "true",
        PW_KEY_NODE_LATENCY, "512/48000",
        PW_KEY_NODE_RATE, "1/48000",
        "stream.dont-remix", "true",
        "channelmix.disable", "true",
        "dither.noise", "0",
        nullptr
    );

    char const* target = pw_properties_get(moduleProps, "target.object");
    if (target && target[0])
    {
        pw_properties_set(playbackProps, PW_KEY_TARGET_OBJECT, target);
    }

    data->m_PlaybackStream.reset(pw_stream_new_simple(
        pw_context_get_main_loop(data->m_Context),
        "spdif-encode-playback",
        playbackProps,
        &PlaybackStreamEvents,
        data
    ));

    uint8_t playbackParamBuf[1024];
    spa_pod_builder playbackBuilder = SPA_POD_BUILDER_INIT(playbackParamBuf, sizeof(playbackParamBuf));

    spa_audio_info_raw playbackRawInfo{};
    playbackRawInfo.format = SPA_AUDIO_FORMAT_S16_LE;
    playbackRawInfo.rate = SampleRate;
    playbackRawInfo.channels = OutputChannels;
    playbackRawInfo.position[0] = SPA_AUDIO_CHANNEL_FL;
    playbackRawInfo.position[1] = SPA_AUDIO_CHANNEL_FR;

    spa_pod const* playbackParams[1];
    playbackParams[0] = spa_format_audio_raw_build(&playbackBuilder, SPA_PARAM_EnumFormat, &playbackRawInfo);

    pw_stream_connect(data->m_PlaybackStream.get(),
                      PW_DIRECTION_OUTPUT,
                      PW_ID_ANY,
                      static_cast<pw_stream_flags>(PW_STREAM_FLAG_AUTOCONNECT |
                                                   PW_STREAM_FLAG_MAP_BUFFERS |
                                                   PW_STREAM_FLAG_RT_PROCESS |
                                                   PW_STREAM_FLAG_TRIGGER),
                      playbackParams, 1);

    // Lock playback stream volume to 1.0 — any scaling corrupts the IEC 61937 bitstream
    ForceUnitVolume(data->m_PlaybackStream.get());

    pw_properties_free(moduleProps);

    pw_log_info("spdif-encode: module loaded, AC3 encoder ready");

    return 0;
}
