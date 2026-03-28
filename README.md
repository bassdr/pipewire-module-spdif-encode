# pipewire-module-spdif-encode

A native PipeWire module that creates a virtual 5.1 surround sink, encodes
multichannel PCM audio to AC3 (Dolby Digital) in real-time, and outputs
IEC 61937-framed data to a hardware S/PDIF, TOSLINK, or HDMI device.

## Why?

The only existing solution for real-time surround encoding on Linux is the
[ALSA a52 plugin](https://github.com/alsa-project/alsa-plugins). Under
PipeWire, this plugin is accessed through multiple abstraction layers
(PipeWire &rarr; PulseAudio compat &rarr; ALSA compat &rarr; a52 plugin &rarr; hardware),
causing latency, buffer glitches, and audible artifacts. This module replaces
all of that with a single native PipeWire module that talks directly to the
hardware sink.

```
Application (5.1 PCM) --> Virtual Sink ("S/PDIF Surround Encoder") --> AC3 encode --> HDMI/S/PDIF device
```

## Dependencies

- `libpipewire-0.3` (>= 0.3.0)
- `libavcodec` / `libavutil` (FFmpeg)
- C++23 compiler (GCC 14+ or Clang 18+)
- Meson build system
- Optional: [Catch2](https://github.com/catchorg/Catch2) v3 (for tests)

## Building

```bash
meson setup build
meson compile -C build
```

To run tests (requires Catch2 v3):

```bash
meson setup build -Dtest=true
meson compile -C build
meson test -C build
```

## Installation

Install the shared library to PipeWire's module directory:

```bash
meson install -C build
```

Or test without installing by setting the module search path:

```bash
PIPEWIRE_MODULE_DIR=build/src pipewire
```

## Usage

1. Find your HDMI or S/PDIF device name:

```bash
pw-cli list-objects | grep -E "node.name.*alsa_output"
# or
pactl list sinks short
```

2. Copy the example config and set your device:

```bash
cp config/spdif-encode.conf ~/.config/pipewire/pipewire.conf.d/
```

Edit `~/.config/pipewire/pipewire.conf.d/spdif-encode.conf`:

```
context.modules = [
    {   name = libpipewire-module-spdif-encode
        args = {
            target.object = "alsa_output.pci-0000_01_00.1.hdmi-stereo"
        }
    }
]
```

3. Restart PipeWire:

```bash
systemctl --user restart pipewire
```

4. Select "S/PDIF Surround Encoder" as your output device in pavucontrol or
   your desktop's audio settings. Any audio routed to this sink will be
   AC3-encoded and sent to the target device.

You can verify the module is loaded with:

```bash
pw-cli list-objects | grep spdif
pactl list sinks short | grep spdif
```

## How it works

The module uses PipeWire's dual-stream pattern (same as module-loopback):

1. A **capture stream** appears as a virtual 5.1 surround sink that applications can route audio to
2. A **playback stream** connects to the target HDMI/S/PDIF hardware device

When audio arrives at the virtual sink:

1. Incoming 5.1 float32 planar PCM is accumulated until a full AC3 frame (1536 samples) is available
2. The frame is encoded to AC3 using libavcodec's fixed-point encoder (`ac3_fixed`)
3. The encoded bitstream is wrapped in an IEC 61937 burst (6144 bytes)
4. The burst is output as stereo S16LE to the hardware device

The playback stream's volume is locked to 1.0 since any scaling would corrupt
the encoded bitstream. The mute button works as expected (outputs silence).

## Known limitations

- **No runtime disable** -- the module cannot be unloaded without restarting PipeWire.
  Use the mute button on the playback stream in pavucontrol as a soft-disable.

- **Exclusive HDMI/S/PDIF use** -- if another application routes audio directly
  to the same HDMI device, PipeWire will mix it with the encoded bitstream,
  producing noise. Ensure only the encoder targets the hardware device.

- **Latency** -- AC3 encoding adds ~32ms of frame accumulation latency.
  The receiver (AVR/soundbar) adds its own AC3 decode latency (typically
  100-150ms), which is outside this module's control.

- **AC3 only** -- DTS encoding is not yet supported.

## License

[MIT](LICENSE)
