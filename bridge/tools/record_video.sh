#!/bin/bash
# Encode a pcb_record stream: tools/record_video.sh <name> [fps=60] [WxH=1600x900]
# Creates cache/bridge/videos/<name>.fifo and runs ffmpeg (NVENC H.264) on it in the background; start this BEFORE
# `pcb_record start <name>` (the plugin's open waits for this reader). Output: cache/bridge/videos/<name>.mp4.
set -u
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
D="$ROOT/cache/bridge/videos"; mkdir -p "$D"
N="$1"; FPS="${2:-60}"; SZ="${3:-1600x900}"
rm -f "$D/$N.fifo"; mkfifo "$D/$N.fifo"
nohup ffmpeg -loglevel error -y -f rawvideo -pix_fmt rgba -s "$SZ" -r "$FPS" -i "$D/$N.fifo" -vf vflip -c:v h264_nvenc -preset p5 -cq 18 -pix_fmt yuv420p "$D/$N.mp4" > "$D/$N.ffmpeg.log" 2>&1 &
echo "[record] encoder pid $! waiting on $D/$N.fifo -> $D/$N.mp4"
