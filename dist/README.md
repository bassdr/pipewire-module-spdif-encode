# dist/ — Distribution files

Config files to be installed by the package manager alongside the module `.so`.

## Files

| File | Install path | Purpose |
|------|-------------|---------|
| `50-spdif-encode.conf` | `/usr/share/pipewire/pipewire.conf.avail/` | PipeWire module-load config. Symlink into `/etc/pipewire/pipewire.conf.d/` to enable. |
| `50-spdif-encode-wireplumber.conf` | `/usr/share/wireplumber/wireplumber.conf.d/` | WirePlumber drop-in: loads the volume-lock script. |
| `spdif-encode-lock-volume.lua` | `/usr/share/wireplumber/scripts/spdif-encode/` | WirePlumber Lua script that auto-discovers the encoder's target device and locks its route volume to 100%. |

## Enabling

After installation:

```sh
# 1. Symlink the PipeWire config to enable the module
ln -s /usr/share/pipewire/pipewire.conf.avail/50-spdif-encode.conf \
      /etc/pipewire/pipewire.conf.d/50-spdif-encode.conf

# 2. Edit target.object to match your HDMI/S/PDIF device
#    Find candidates with:
wpctl status | grep -i -E 'hdmi|spdif|iec958'

# 3. Restart PipeWire and WirePlumber
systemctl --user restart pipewire wireplumber
# or: rc-service pipewire-system restart
```

The volume-lock script is loaded automatically by WirePlumber and requires
no manual configuration.  It reads `target.object` from the running
`spdif-encode-output` node to determine which device to protect.

## Why the device volume must be 100%

The module outputs AC3-encoded surround audio disguised as stereo S16LE PCM.
PipeWire routes this through its normal audio path, which includes the
audioconvert adapter on the target device.  That adapter applies the device's
volume as float multiplication on every sample.

At volume 1.0 (100%), the multiply-by-one is a no-op and the IEC 61937
bitstream passes through bit-perfect.  At any other volume, the sync words
(0xF872, 0x4E1F) and encoded payload get scaled, the receiver can no longer
detect the AC3 framing, and the output is noise/crackling.
