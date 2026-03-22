# pipewire-module-spdif-encode

A native PipeWire module that creates a virtual 5.1 surround sink, encodes
multichannel PCM audio to AC3 (Dolby Digital) in real-time, and outputs
IEC 61937-framed data to a hardware S/PDIF, TOSLINK, or HDMI device.

## Why?

The only existing solution for real-time surround encoding on Linux is the
[ALSA a52 plugin](https://github.com/alsa-project/alsa-plugins). Under
PipeWire, this plugin is accessed through multiple abstraction layers
(PipeWire -> PulseAudio compat -> ALSA compat -> a52 plugin -> hardware),
causing latency, buffer glitches, and audible artifacts. This module replaces
all of that with a single native PipeWire module that talks directly to the
hardware sink.

```
 Applications          pipewire-module-          ALSA IEC958
 (5.1 PCM)    ------> spdif-encode      ------> device (hw:X,Y)
                       1. Receive F32P PCM       S/PDIF, TOSLINK,
                       2. Buffer 1536 samples    or HDMI output
                       3. Encode AC3
                       4. IEC 61937 framing
                       5. Output stereo S16LE
```

## Dependencies

- `libpipewire-0.3` (>= 0.3.0)
- `libavcodec` / `libavutil` (FFmpeg)
- Optional: [Catch2](https://github.com/catchorg/Catch2) v3 (for tests)

## Building

```bash
meson setup build
meson compile -C build
```

To run tests:

```bash
meson setup build -Dtests=true
meson compile -C build
meson test -C build
```

## Installation

```bash
meson install -C build
```

This installs the module to PipeWire's module directory (typically
`/usr/lib64/pipewire-0.3/`).

## Usage

Copy `config/spdif-encode.conf` to your PipeWire config directory and restart
PipeWire:

```bash
cp config/spdif-encode.conf ~/.config/pipewire/pipewire.conf.d/
systemctl --user restart pipewire
```

The module creates a virtual sink called "S/PDIF Surround Encoder" that
appears in your audio settings. Route any application's audio to this sink
to have it encoded as AC3 and sent to your S/PDIF output.

You can also load it manually at runtime:

```bash
pw-cli load-module libpipewire-module-spdif-encode
```

Verify it's running:

```bash
pw-cli list-objects | grep spdif
```

## Configuration

The module accepts the following arguments in `spdif-encode.conf`:

```
context.modules = [
    {   name = libpipewire-module-spdif-encode
        args = {
            # target.object = ""   # target S/PDIF device name
        }
    }
]
```

## Status

This project is in early development (Phase 1). Currently implemented:

- AC3 encoding via libavcodec (fixed-point `ac3_fixed` preferred)
- IEC 61937 framing
- Dual-stream PipeWire module (virtual sink + hardware output)
- Ring buffer for sample accumulation
- Unit tests for encoder and IEC 61937 framing

## License

[MIT](LICENSE)
