#include "encoder-ac3.h"
#include "encoder-dts.h"
#include "iec61937.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <format>
#include <memory>
#include <optional>
#include <ranges>
#include <span>
#include <string_view>

extern "C"
{
#include <pipewire/pipewire.h>
#include <pipewire/impl-module.h>
#include <spa/param/audio/format-utils.h>
#include <spa/param/props.h>
#include <spa/pod/builder.h>
#include <spa/pod/iter.h>
}

static constexpr uint8_t OutputChannels = 2;
static constexpr uint32_t SampleRate = 48000;
static constexpr uint32_t MaxQuantum = 8192;

// Compile-time maximums across all supported codecs (AC3, DTS).
// Used to size fixed arrays that must accommodate any codec at runtime.
static constexpr uint16_t MaxFrameSize = Ac3Encoder::FrameSize;    // 1536
static constexpr uint32_t MaxBurstSize = Ac3Encoder::BurstSize;    // 6144
static constexpr uint8_t MaxInputChannels = Ac3Encoder::InputChannels; // 6

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

    std::optional<AvEncoder> m_Enc;

    // Codec parameters (set at init from the selected encoder's constants)
    uint16_t m_FrameSize{};
    uint32_t m_BurstSize{};
    uint16_t m_DataType{};
    uint8_t m_InputChannels{};

    // Residual buffer: holds leftover F32P samples between quantums that
    // didn't fill a complete encoder frame.
    static constexpr size_t MaxResidual = MaxFrameSize - 1;
    std::array<std::array<float, MaxResidual>, MaxInputChannels> m_Residual{};
    size_t m_ResidualCount{};  // per-channel sample count

    // Encoding workspace — holds raw bitstream from encoder
    std::array<uint8_t, MaxBurstSize> m_EncodeBuf{};

    // Output ring buffer: stores IEC 61937 bursts as raw bytes.
    // Sized for the largest burst (AC3 = 6144 bytes).  When a smaller codec
    // is active (DTS = 2048), the ring effectively holds more bursts.
    // Write position is always burst-aligned so bursts never straddle the wrap.
    static constexpr size_t OutRingBursts = 4;
    static constexpr size_t OutRingBytes = MaxBurstSize * OutRingBursts;
    std::array<uint8_t, OutRingBytes> m_OutRingBuf{};
    size_t m_OutRingWritePos{};  // in bytes, always burst-aligned
    size_t m_OutRingReadPos{};   // in bytes
    size_t m_OutRingStored{};    // in bytes

    bool m_PlaybackStreaming{};  // true when playback stream is in STREAMING state
};

static void FlushRing(ModuleData* data)
{
    data->m_OutRingWritePos = 0;
    data->m_OutRingReadPos = 0;
    data->m_OutRingStored = 0;
    data->m_ResidualCount = 0;
}

// Push an encoded frame into the output ring as an IEC 61937 burst.
static void PushBurst(ModuleData* data, uint32_t encodedSize)
{
    uint32_t const burstSize = data->m_BurstSize;

    if (data->m_OutRingStored + burstSize > ModuleData::OutRingBytes)
    {
        // Output ring full — drop oldest burst
        pw_log_warn("spdif-encode: ring overflow, dropping burst (%zu/%zu bytes stored)",
                    data->m_OutRingStored, ModuleData::OutRingBytes);
        data->m_OutRingReadPos = (data->m_OutRingReadPos + burstSize)
                                 % ModuleData::OutRingBytes;
        data->m_OutRingStored -= burstSize;
    }

    if (!Iec61937::CreateBurst(
        data->m_DataType,
        {data->m_EncodeBuf.data(), encodedSize},
        std::span<uint8_t>(data->m_OutRingBuf.data() + data->m_OutRingWritePos, burstSize)))
    {
        return;
    }
    data->m_OutRingWritePos = (data->m_OutRingWritePos + burstSize)
                              % ModuleData::OutRingBytes;
    data->m_OutRingStored += burstSize;
}

// Encode all complete frames from the available sample data.
// 'channels' points to per-channel float arrays; samples [offset, offset+newSamples)
// are the new data from this quantum.
static void EncodeAvailable(ModuleData* data, std::array<float const*, MaxInputChannels>& channels, size_t newSamples)
{
    uint16_t const frameSize = data->m_FrameSize;
    uint8_t const inputChannels = data->m_InputChannels;
    size_t available = data->m_ResidualCount + newSamples;
    size_t consumed = 0;  // new samples consumed so far

    while (available >= static_cast<size_t>(frameSize))
    {
        std::array<float const*, MaxInputChannels> framePtrs;
        size_t frameOffset = 0;

        if (data->m_ResidualCount > 0)
        {
            // Composite frame: residual prefix + beginning of new data
            // Use thread_local to avoid stack allocation every iteration
            thread_local std::array<std::array<float, MaxFrameSize>, MaxInputChannels> composite;

            size_t const needed = frameSize - data->m_ResidualCount;
            for (auto&& [comp, res, chPtr, fPtr] : std::views::zip(
                     std::span(composite).first(inputChannels),
                     std::span(data->m_Residual).first(inputChannels),
                     std::span(channels).first(inputChannels),
                     std::span(framePtrs).first(inputChannels)))
            {
                std::memcpy(comp.data(), res.data(),
                            data->m_ResidualCount * sizeof(float));
                std::memcpy(comp.data() + data->m_ResidualCount,
                            chPtr + consumed, needed * sizeof(float));
                fPtr = comp.data();
            }
            consumed += needed;
            available -= frameSize;
            data->m_ResidualCount = 0;
        }
        else
        {
            // Encode directly from source pointers at the right offset
            for (auto&& [fPtr, chPtr] : std::views::zip(
                     std::span(framePtrs).first(inputChannels),
                     std::span(channels).first(inputChannels)))
            {
                fPtr = chPtr;
            }
            frameOffset = consumed;
            consumed += frameSize;
            available -= frameSize;
        }

        auto encodeResult = data->m_Enc->EncodeFrame(
            framePtrs.data(), inputChannels, frameOffset, frameSize,
            data->m_EncodeBuf.data(), data->m_EncodeBuf.size());
        if (!encodeResult)
            continue;

        PushBurst(data, *encodeResult);
    }

    // Append unconsumed new samples after any existing residual
    size_t const newRemaining = newSamples - consumed;
    if (newRemaining > 0)
    {
        for (auto&& [res, chPtr] : std::views::zip(
                 std::span(data->m_Residual).first(inputChannels),
                 std::span(channels).first(inputChannels)))
        {
            std::memcpy(res.data() + data->m_ResidualCount, chPtr + consumed,
                        newRemaining * sizeof(float));
        }
        data->m_ResidualCount += newRemaining;
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
    static std::array<float, MaxQuantum> const silence{};

    std::array<float const*, MaxInputChannels> channels;
    for (auto&& [ch, ptr] : std::views::enumerate(std::span(channels).first(data->m_InputChannels)))
    {
        auto idx = static_cast<uint32_t>(ch);
        ptr = (idx < spaBuf->n_datas && spaBuf->datas[idx].data)
                  ? static_cast<float const*>(spaBuf->datas[idx].data)
                  : silence.data();
    }

    // Only encode when the playback stream can actually drain the ring.
    // When the hardware device is suspended the playback stream has no
    // buffers, so encoding would just overflow the ring endlessly.
    if (data->m_PlaybackStreaming)
    {
        EncodeAvailable(data, channels, sampleCount);
    }

    pw_stream_queue_buffer(data->m_CaptureStream.get(), buf);
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

    // Use buf->requested if available (= quantum), else fill the buffer.
    // std::min handles the uint64_t -> uint32_t narrowing safely since maxFrames is uint32_t.
    uint32_t const outFrames = buf->requested
        ? static_cast<uint32_t>(std::min(static_cast<uint64_t>(maxFrames), buf->requested))
        : maxFrames;

    uint32_t const outBytes = outFrames * sizeof(int16_t) * OutputChannels;
    auto output = std::span(static_cast<uint8_t*>(spaBuf->datas[0].data), outBytes);

    // Drain output ring into the PipeWire buffer (up to two contiguous segments)
    size_t const ringBytes = std::min(static_cast<size_t>(outBytes), data->m_OutRingStored);
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
    if (ringBytes == 0 && data->m_OutRingStored == 0)
    {
        pw_log_debug("spdif-encode: ring underrun, outputting silence (%u frames requested)",
                     outFrames);
    }
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

static void OnPlaybackStateChanged(void* userData, enum pw_stream_state,
                                   enum pw_stream_state state, char const*)
{
    auto* data = static_cast<ModuleData*>(userData);
    bool const streaming = (state == PW_STREAM_STATE_STREAMING);

    if (streaming && !data->m_PlaybackStreaming)
    {
        // Flush stale data (silence bursts accumulated while device was suspended)
        FlushRing(data);
        pw_log_info("spdif-encode: playback streaming, ring flushed");
    }
    else if (!streaming && data->m_PlaybackStreaming)
    {
        pw_log_info("spdif-encode: playback stopped (state=%d)", state);
    }

    data->m_PlaybackStreaming = streaming;
}

static void OnPlaybackParamChanged(void* userData, uint32_t id, spa_pod const* param)
{
    if (id != SPA_PARAM_Props || !param)
    {
        return;
    }

    auto const* obj = reinterpret_cast<spa_pod_object const*>(param);

    if (HasNonUnitVolume(obj, SPA_PROP_channelVolumes)
        || HasNonUnitVolume(obj, SPA_PROP_softVolumes))
    {
        pw_log_warn("spdif-encode: volume changed on output stream, "
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
    .state_changed = OnPlaybackStateChanged,
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

    // Select codec from module args (default: ac3)
    char const* codecArg = pw_properties_get(moduleProps, "codec");
    std::string_view codec = codecArg ? codecArg : "ac3";

    if (codec == "ac3")
    {
        auto enc = Ac3Encoder::Create(Ac3Encoder::InputChannels, SampleRate, Ac3Encoder::DefaultBitrate);
        if (!enc)
        {
            pw_log_error("spdif-encode: failed to initialize AC3 encoder");
            pw_properties_free(moduleProps);
            delete data;
            return -1;
        }
        data->m_Enc = std::move(*enc);
        data->m_FrameSize = Ac3Encoder::FrameSize;
        data->m_BurstSize = Ac3Encoder::BurstSize;
        data->m_DataType = Ac3Encoder::DataType;
        data->m_InputChannels = Ac3Encoder::InputChannels;
    }
    else if (codec == "dts")
    {
        auto enc = DtsEncoder::Create(DtsEncoder::InputChannels, SampleRate, DtsEncoder::DefaultBitrate);
        if (!enc)
        {
            pw_log_error("spdif-encode: failed to initialize DTS encoder "
                         "(requires FFmpeg built with libdcaenc)");
            pw_properties_free(moduleProps);
            delete data;
            return -1;
        }
        data->m_Enc = std::move(*enc);
        data->m_FrameSize = DtsEncoder::FrameSize;
        data->m_BurstSize = DtsEncoder::BurstSize;
        data->m_DataType = DtsEncoder::DataType;
        data->m_InputChannels = DtsEncoder::InputChannels;
    }
    else
    {
        pw_log_error("spdif-encode: unknown codec '%s' (expected 'ac3' or 'dts')", codecArg);
        pw_properties_free(moduleProps);
        delete data;
        return -1;
    }

    pw_impl_module_add_listener(module, &data->m_ModuleListener, &ModuleEvents, data);

    // Capture stream: virtual 5.1 sink
    auto* captureProps = pw_properties_new(
        PW_KEY_NODE_NAME, "spdif-encode-sink",
        PW_KEY_NODE_DESCRIPTION, codec == "dts"
            ? "S/PDIF Surround Encoder (DTS)"
            : "S/PDIF Surround Encoder (AC3)",
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
    captureInfo.channels = data->m_InputChannels;
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
    // The output is IEC 61937-framed data disguised as plain stereo PCM.
    // We must prevent audioconvert from modifying the bitstream.
    auto* playbackProps = pw_properties_new(
        PW_KEY_NODE_NAME, "spdif-encode-output",
        PW_KEY_MEDIA_TYPE, "Audio",
        PW_KEY_MEDIA_CATEGORY, "Playback",
        PW_KEY_MEDIA_CLASS, "Stream/Output/Audio",
        PW_KEY_NODE_DONT_RECONNECT, "true",
        // Use the largest codec frame size (AC3 = 1536) as the quantum for all codecs.
        // Smaller quantums (e.g. DTS burst = 512 frames) can fail to start on HDMI devices
        // with large minimum ALSA periods.  1536 is the LCM of AC3 (1536) and DTS (512)
        // burst frame sizes, so it maps to whole bursts for every codec.
        PW_KEY_NODE_LATENCY, std::format("{}/{}", MaxFrameSize, SampleRate).c_str(),
        PW_KEY_NODE_RATE, std::format("1/{}", SampleRate).c_str(),
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
                                                   PW_STREAM_FLAG_RT_PROCESS),
                      playbackParams, 1);

    // Lock playback stream volume to 1.0 — any scaling corrupts the IEC 61937 bitstream
    ForceUnitVolume(data->m_PlaybackStream.get());

    pw_properties_free(moduleProps);

    pw_log_info("spdif-encode: module loaded, codec=%s (frame=%u, burst=%u, ring=%zu bursts)",
                data->m_Enc->CodecName(), data->m_FrameSize, data->m_BurstSize,
                ModuleData::OutRingBytes / data->m_BurstSize);

    return 0;
}
