# CLAUDE.md

## Project Overview

**pipewire-module-spdif-encode** is a native PipeWire module that creates a virtual surround sink, encodes multichannel PCM audio to AC3 (Dolby Digital) in real-time, and outputs IEC 61937-framed data to a hardware S/PDIF, TOSLINK, or HDMI device.

### Why this exists

The only existing solution for real-time surround encoding on Linux is the [ALSA a52 plugin](https://github.com/alsa-project/alsa-plugins). Under PipeWire, this plugin is accessed through multiple abstraction layers (PipeWire → PulseAudio compat → ALSA compat → a52 plugin → hardware), causing latency, buffer glitches, and audible artifacts (scratching sounds at stream start). This module replaces all of that with a single native PipeWire module that talks directly to the hardware sink.

### Target use case

A user has an HDMI or S/PDIF output connected to an AVR/soundbar that expects encoded surround audio. They want any application playing surround audio to be transparently AC3-encoded before reaching the hardware. The audio routing is:

```
Application (5.1 PCM) ──> Virtual Sink ("S/PDIF Surround Encoder") ──> AC3 encode ──> HDMI/S/PDIF device
```

The user sets `target.object` in the module config to their HDMI/S/PDIF device name, then selects the virtual sink as their default output (via pavucontrol or `wpctl set-default`). All audio routed to the virtual sink is encoded on the fly.

Ideally this would be a device profile in pavucontrol's Configuration tab (like "Surround 5.1 AC3 Encoded (HDMI)"), but that requires a SPA device/profile plugin which is significantly more complex. The current approach uses the dual-stream module pattern (from PipeWire's module-loopback), which is the standard way for out-of-tree PipeWire modules.

### How it works

```
┌─────────────┐     ┌──────────────────────────┐     ┌─────────────────┐
│ Applications │────>│ pipewire-module-spdif-    │────>│ HDMI / S/PDIF   │
│ (5.1 PCM)   │     │ encode                    │     │ output device   │
│              │     │                           │     │                 │
│ Route to the │     │ 1. Receives 5.1 F32P PCM  │     │ Set via         │
│ virtual sink │     │ 2. Buffers 1536 samples   │     │ target.object   │
│ "S/PDIF      │     │ 3. Encodes AC3            │     │ module arg      │
│  Surround    │     │ 4. IEC 61937 framing      │     │                 │
│  Encoder"    │     │ 5. Outputs stereo S16LE   │     │                 │
└─────────────┘     └──────────────────────────┘     └─────────────────┘
```

## Architecture

### Dual-stream pattern (from PipeWire's module-loopback)

The module creates two `pw_stream` objects:

1. **Capture stream** (`PW_DIRECTION_INPUT`, `media.class = "Audio/Sink"`)
   - Appears as a virtual sink in pavucontrol / PipeWire
   - Accepts multichannel PCM (5.1 float32 planar)
   - Any application can route audio to it

2. **Playback stream** (`PW_DIRECTION_OUTPUT`, `media.class = "Stream/Output/Audio"`)
   - Outputs stereo S16LE containing IEC 61937-wrapped encoded data
   - Connects to the hardware device specified by `target.object` module arg
   - Marked `node.dont-reconnect` to avoid appearing as a regular playback client
   - Uses `PW_STREAM_FLAG_TRIGGER` — only processes when capture triggers it

### Encoding pipeline

1. Capture callback receives F32P PCM (one `spa_data` per channel), accumulates samples in a residual buffer until a full 1536-sample AC3 frame is available
2. F32P samples are converted directly into the libavcodec frame buffers (S16P, S32P, or FLTP depending on the codec variant)
3. Encoded AC3 bitstream is wrapped in an IEC 61937 burst and pushed to the output ring buffer
4. Capture triggers the playback stream, which drains the output ring into PipeWire's playback buffer
5. Playback volume is locked to 1.0 (any scaling corrupts the bitstream); mute is allowed (outputs zeros)

### IEC 61937 burst format

```
|<------------ 6144 bytes (1536 stereo S16LE samples) ------------>|
+----+----+----+----+----------------------------+------------------+
| Pa | Pb | Pc | Pd | encoded syncframe          | zero padding     |
| 2B | 2B | 2B | 2B | ~768-1920 bytes            | to fill 6144B    |
+----+----+----+----+----------------------------+------------------+

Pa = 0xF872 (sync word 1)
Pb = 0x4E1F (sync word 2)
Pc = data_type | (codec_info << 8)    (0x01 for AC3, 0x0B for DTS type I)
Pd = payload_bytes * 8               (length in bits)
```

### Key constants

| Parameter | AC3 | DTS |
|-----------|-----|-----|
| Frame size (samples) | 1536 | 512 (type I) |
| Burst size (bytes) | 6144 | 2048 (type I) |
| Latency at 48kHz | ~32ms | ~10.7ms |
| Typical bitrate | 448 kbps (5.1) | 1509 kbps (5.1) |
| IEC 61937 data_type | 0x01 | 0x0B |

## File Structure

```
pipewire-module-spdif-encode/
├── CLAUDE.md                          # This file
├── meson.build                        # Build system
├── meson_options.txt                  # Build options
├── src/
│   ├── module-spdif-encode.cpp        # PipeWire module entry point, stream setup, processing
│   ├── encoder.h                      # Codec-agnostic encoder interface
│   ├── encoder-ac3.h                  # AC3 encoder header
│   ├── encoder-ac3.cpp                # AC3 encoder (libavcodec ac3_fixed/ac3)
│   └── iec61937.h                     # IEC 61937 framing (header-only)
├── tests/
│   ├── test-iec61937.cpp              # IEC 61937 framing tests (Catch2)
│   └── test-encoder-ac3.cpp           # AC3 encoder tests (Catch2)
└── config/
    └── spdif-encode.conf              # Example PipeWire config to load the module
```

## Roadmap

### DTS support

- **`src/encoder-dts.cpp`** — DTS encoder (libavcodec or libdcaenc)
- **`meson_options.txt`** — options for enabling/disabling codecs
- Module arg `codec=ac3|dts` to select at load time

### Stereo passthrough

When input is stereo, forward raw PCM to the hardware device instead of AC3-encoding it. This bypasses both the 32ms frame accumulation and the receiver's ~100-150ms AC3 decode latency. Challenges: PipeWire upmixes stereo to 5.1 before it reaches the capture sink, so detection must happen via port/stream metadata rather than signal analysis. Mode transitions (PCM ↔ encoded) cause receiver re-lock gaps.

### Stream suspension

Cork the playback stream when no clients are routing audio to the capture sink. Currently the module continuously encodes silence when idle, wasting CPU and keeping the HDMI/S/PDIF device active. Requires `state_changed` callbacks on the capture stream to detect idle/active transitions.

### Additional improvements

- Encoded passthrough — detect already-encoded AC3/DTS input and forward directly to IEC 61937 framing (skip re-encoding)
- Bitrate configuration via module args
- Channel layout flexibility (5.1, 7.1 downmix)
- Handle device hot-plug (S/PDIF cable connect/disconnect)

### SPA device plugin (long-term)

Replace the dual-stream module with a proper SPA device/profile plugin. This would make the encoded output appear as an HDMI device profile in pavucontrol's Configuration tab, eliminating routing footguns (mixing conflicts, no runtime disable) and enabling proper format negotiation for stereo passthrough. Significantly more complex (~2000-5000 lines vs current ~480).

## Known Issues & Limitations

### No way to disable the module at runtime

The module has no on/off switch once loaded. The mute button on the playback stream in pavucontrol works as a soft-disable (outputs S/PDIF silence), but there is no way to fully unload the module without editing the config and restarting PipeWire. A proper solution would require a SPA device/profile plugin so the encoded output appears as a device profile in pavucontrol's Configuration tab.

### Mixing conflicts on shared HDMI/S/PDIF device

If another PipeWire client targets the same HDMI output as the encoder, PipeWire's mixer will combine the IEC 61937-encoded bitstream with regular PCM, producing noise/garbage. This is a fundamental routing issue — encoded data cannot be mixed with anything. Workaround: ensure no other streams target the same hardware device.

### Latency

End-to-end latency is higher than plain PCM output due to several unavoidable stages:

| Stage | Latency | Notes |
|-------|---------|-------|
| AC3 frame accumulation | ~32ms | Must collect 1536 samples before encoding (inherent to AC3) |
| PipeWire graph scheduling | ~1 quantum (~32ms) | Quantum matches AC3 frame size |
| Output ring buffer | 0-32ms | Typically 1 burst; ring holds up to 4 as safety margin |
| HDMI/S/PDIF transmitter | Hardware-dependent | Typically small |
| AVR/soundbar AC3 decode | 100-150ms | Receiver-dependent, cannot be controlled |

**Module-controllable latency: ~32-64ms.** Receiver AC3 decode dominates. Measured ~200ms overhead vs plain PCM in a full MIDI→USB→synth→PipeWire→encode→HDMI→SPDIF extractor→speakers chain, of which roughly half is receiver decode latency.

## Coding Style

- C++, 4 spaces indentation, Allman brace style
- 120 character line length, LF line endings
- No trailing whitespace, single final newline
- Prefer ranged-for loops (`for (auto&& x : range)`) over index-based loops wherever possible. Use `std::span`, `std::views::enumerate`, `std::views::zip`, etc. to make loops rangeable.
- Avoid generic `int` for sizes/indices — use explicitly-sized types (`uint8_t`, `uint16_t`, `uint32_t`, `size_t`) to prevent implicit signed/unsigned conversions and unnecessary casts. Match the width to the domain (e.g. channel count fits in `uint8_t`, sample rate in `uint32_t`).
- Prefer `std::array` over C arrays and raw pointers at function signature scope — avoids implicit array-to-pointer decay and useless span conversions.
- **No heap allocations on the critical (real-time audio) path.** Avoid `std::vector`, `new`, `malloc`, `std::string`, or any allocating container in RT callbacks and encoding functions. Use `std::array`, fixed-size buffers, and pre-allocated storage. Tests are exempt — allocating containers are acceptable in test code.
- See `.editorconfig` for editor integration

## Build & Test Commands

```bash
# Build
meson setup build
meson compile -C build

# Test without installing (set module search path)
PIPEWIRE_MODULE_DIR=build/src pipewire  # or restart pipewire

# Load manually at runtime
pw-cli load-module libpipewire-module-spdif-encode

# Check it appears
pw-cli list-objects | grep spdif
pactl list sinks short | grep spdif
```

## Key References

- **PipeWire module-loopback source**: dual-stream pattern template — https://github.com/PipeWire/pipewire/blob/master/src/modules/module-loopback.c
- **PipeWire module-example-sink**: simplest sink module — https://docs.pipewire.org/page_module_example_sink.html
- **pipewire-module-xrdp**: real out-of-tree module example — https://github.com/neutrinolabs/pipewire-module-xrdp
- **ALSA a52 plugin source**: reference IEC 61937 framing — https://github.com/alsa-project/alsa-plugins/blob/master/a52/pcm_a52.c
- **FFmpeg spdifenc.c**: IEC 61937 implementation — https://github.com/FFmpeg/FFmpeg/blob/master/libavformat/spdifenc.c
- **FFmpeg AC3 encoder**: https://github.com/FFmpeg/FFmpeg/blob/master/libavcodec/ac3enc.c
- **IEC 61937 spec (Microsoft docs)**: https://learn.microsoft.com/en-us/windows/win32/coreaudio/representing-formats-for-iec-61937-transmissions

## Dependencies

- `libpipewire-0.3` (headers + pkg-config)
- `libavcodec` / `libavutil` from FFmpeg (for AC3 encoding)
- Optional: [Catch2](https://github.com/catchorg/Catch2) v3 (for tests)

## Technical Notes

- **Channel order**: PipeWire uses FL,FR,FC,LFE,RL,RR which matches SMPTE/libavcodec order. No remapping needed (unlike the ALSA a52 plugin which must remap ALSA's channel order).
- **Real-time safety**: `ac3_fixed` (fixed-point encoder) is preferred over `ac3` (float) because it avoids floating-point allocations. The encoding happens in the RT-flagged process callback.
- **Buffer accumulation**: PipeWire may deliver quantums smaller than 1536 samples. A residual buffer accumulates F32P samples across quantums until a full AC3 frame is available.
- **Playback stream format**: The output appears as stereo S16LE to the hardware. Properties like `stream.dont-remix` and `channelmix.disable` prevent PipeWire from processing the encoded bitstream as normal audio.
- **IEC958 non-audio flag**: The hardware device ideally needs `IEC958_AES0_NONAUDIO` set in the channel status bits. Currently not set by the module — most receivers auto-detect based on IEC 61937 sync words.
