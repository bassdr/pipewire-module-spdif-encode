#!/bin/bash
# Play a 440 Hz sine wave encoded as DTS over IEC 61937 directly to HDMI.
# Bypasses PipeWire entirely — tests hardware DTS decoding.
#
# Usage: ./tests/play-dts-test.sh [hw:CARD,DEV] [duration_sec]
#
# Stop PipeWire first if it holds the device:
#   systemctl --user stop pipewire pipewire-pulse wireplumber

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
SAMPLES_DIR="${SCRIPT_DIR}/../samples"
DEVICE="${1:-hw:3,3}"
DURATION="${2:-5}"

mkdir -p "$SAMPLES_DIR"

echo "Generating ${DURATION}s 440 Hz sine as DTS IEC 61937..."
ffmpeg -f lavfi -i "sine=frequency=440:duration=${DURATION}:sample_rate=48000" \
    -ac 6 -c:a dca -strict experimental -b:a 1509000 \
    -f spdif "$SAMPLES_DIR/dts-test.raw" -y 2>/dev/null

echo "Playing to ${DEVICE} (Ctrl+C to stop)"
aplay -D "$DEVICE" -f S16_LE -r 48000 -c 2 "$SAMPLES_DIR/dts-test.raw" 2>&1
