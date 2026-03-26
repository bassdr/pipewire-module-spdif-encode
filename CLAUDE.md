# CLAUDE.md

## Project Overview

**pipewire-module-spdif-encode** is a native PipeWire module that creates a virtual surround sink, encodes multichannel PCM audio to AC3 (Dolby Digital) or DTS in real-time, and outputs IEC 61937-framed data to a hardware S/PDIF, TOSLINK, or HDMI device.

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
│ "S/PDIF      │     │ 3. Encodes AC3 or DTS     │     │ module arg      │
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
   - Marked `node.passive` and `node.dont-reconnect` to avoid appearing as a regular playback client
   - Uses `PW_STREAM_FLAG_TRIGGER` — only processes when capture triggers it

### Encoding pipeline

1. Capture callback receives PCM, triggers playback
2. Playback callback:
   - Dequeues capture buffer (6-channel F32P)
   - Converts F32P → S16P with SMPTE channel reorder (FL,FR,FC,LFE,RL,RR)
   - Feeds 1536-sample frames to libavcodec AC3/DTS encoder
   - Wraps encoded frame in IEC 61937 burst (8-byte header + payload + zero padding = 6144 bytes)
   - Queues to playback output

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
├── meson_options.txt                  # Build options (codec selection)
├── src/
│   ├── module-spdif-encode.cpp        # PipeWire module entry point, stream setup, processing
│   ├── encoder.h                      # Codec-agnostic encoder interface
│   ├── encoder-ac3.h                  # AC3 encoder header
│   ├── encoder-ac3.cpp                # AC3 encoder (libavcodec ac3_fixed/ac3)
│   └── iec61937.h                     # IEC 61937 framing (header-only, small enough)
├── tests/
│   ├── test-iec61937.cpp              # IEC 61937 framing tests (Catch2)
│   └── test-encoder-ac3.cpp           # AC3 encoder tests (Catch2)
└── config/
    └── spdif-encode.conf              # Example PipeWire config to load the module
```

## Implementation Plan

### Phase 1: Minimal viable module (AC3 only)

1. **`meson.build`** — build system linking against `libpipewire-0.3` and `libavcodec`/`libavutil`
2. **`src/iec61937.h`** — IEC 61937 framing functions (header-only)
3. **`src/encoder.h`** — encoder interface struct (init, encode_frame, destroy, frame_size, burst_size)
4. **`src/encoder-ac3.cpp`** — AC3 implementation using libavcodec
5. **`src/module-spdif-encode.cpp`** — PipeWire module with dual-stream pattern
6. **`config/spdif-encode.conf`** — working config file

Goal: select the virtual sink in pavucontrol, hear 5.1 audio encoded through S/PDIF.

### Phase 2: DTS support

7. **`src/encoder-dts.cpp`** — DTS encoder (libavcodec or libdcaenc)
8. **`meson_options.txt`** — options for enabling/disabling codecs
9. Module arg `codec=ac3|dts` to select at load time (or auto-detect from hardware caps)

### Phase 3: Polish

10. Channel layout flexibility (5.1, 7.1 downmix, stereo passthrough)
11. Bitrate configuration via module args
12. Proper latency reporting to PipeWire
13. Distribution packaging
14. Handle device hot-plug (S/PDIF cable connect/disconnect)

## Coding Style

- C++, 4 spaces indentation, Allman brace style
- 120 character line length, LF line endings
- No trailing whitespace, single final newline
- See `.editorconfig` for editor integration

## Build & Test Commands

```bash
# Build
meson setup build
meson compile -C build

# Install (to PipeWire module dir)
meson install -C build

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
- `libavcodec` / `libavutil` from FFmpeg (for AC3/DTS encoding)
- Optional: `libdcaenc` (alternative DTS encoder)

## Technical Notes

- **Channel order**: PipeWire uses FL,FR,FC,LFE,RL,RR. libavcodec expects SMPTE order (same). The ALSA a52 plugin remaps because ALSA's order differs, but PipeWire's `SPA_AUDIO_CHANNEL_*` positions already match SMPTE — verify this during implementation.
- **Real-time safety**: `ac3_fixed` (fixed-point encoder) is preferred over `ac3` (float) because it's less likely to allocate memory. The encoding happens in the RT-flagged process callback — verify no allocations occur.
- **Buffer accumulation**: PipeWire may deliver buffers smaller than 1536 samples. The module needs a ring buffer to accumulate a full AC3 frame before encoding. This is critical.
- **Playback stream format**: The output MUST appear as stereo S16LE to the hardware. PipeWire should not try to "process" it as normal audio. Setting the right node properties and target device is important.
- **IEC958 non-audio flag**: The hardware device needs `IEC958_AES0_NONAUDIO` set in the channel status bits so the S/PDIF transmitter marks it as compressed data. This may need to be done via ALSA controls or the correct device string.
