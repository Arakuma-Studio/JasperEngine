#!/usr/bin/env bash
# Makes the video clips of the MP4 tests (engine/tests/media/clips/, committed: they are data;
# docs/superpowers/specs/2026-10-31-video-mp4-design.md §9). Any FFmpeg build with libx264 (or
# libopenh264), libx265, libvpx, libopus and its AAC encoder:
#
#   tools/media/make_test_clips.sh [output-dir]
#
# Every clip is 160x90 at 30 fps. The picture says its frame number N: the top-left, top-right
# and bottom-left quadrants show the base-4 digits of N (digit d of quadrant q = (N / 4^q) % 4),
# as black, red, green or blue; the bottom-right quadrant stays white (a reference). The sound is
# a 1 kHz tone.
set -euo pipefail

out=${1:-$(cd "$(dirname "$0")/../.." && pwd)/engine/tests/media/clips}
mkdir -p "$out"

frames='color=c=black:s=160x90:r=30:d=SECONDS,format=rgb24,geq='\
'r=if(gte(X\,W/2)*gte(Y\,H/2)\,255\,255*eq(mod(floor(N/pow(4\,gte(X\,W/2)+2*gte(Y\,H/2)))\,4)\,1)):'\
'g=if(gte(X\,W/2)*gte(Y\,H/2)\,255\,255*eq(mod(floor(N/pow(4\,gte(X\,W/2)+2*gte(Y\,H/2)))\,4)\,2)):'\
'b=if(gte(X\,W/2)*gte(Y\,H/2)\,255\,255*eq(mod(floor(N/pow(4\,gte(X\,W/2)+2*gte(Y\,H/2)))\,4)\,3))'
tone='sine=frequency=1000:sample_rate=48000:duration=SECONDS'

video(){ echo "${frames//SECONDS/$1}"; }
audio(){ echo "${tone//SECONDS/$1}"; }

ff(){ ffmpeg -hide_banner -loglevel error -y "$@"; }

h264=(-c:v libx264 -profile:v baseline -level 3.0 -pix_fmt yuv420p -g 30 -keyint_min 30 -sc_threshold 0 -bf 0 -crf 30 -preset veryslow)
encoders=$(ffmpeg -hide_banner -encoders 2>/dev/null || true)
if [[ "$encoders" != *libx264* ]]; then
    h264=(-c:v libopenh264 -profile:v constrained_baseline -pix_fmt yuv420p -g 30 -b:v 120k)
fi
aac=(-c:a aac -b:a 24k -ac 1)

# H.264 Baseline + AAC, 2 s, the moov box first (fast start)
ff -f lavfi -i "$(video 2)" -f lavfi -i "$(audio 2)" "${h264[@]}" "${aac[@]}" -movflags +faststart -t 2 "$out/h264_aac_160x90.mp4"
# the same with the moov box at the end (FFmpeg's default)
ff -f lavfi -i "$(video 2)" -f lavfi -i "$(audio 2)" "${h264[@]}" "${aac[@]}" -t 2 "$out/h264_aac_160x90_moov_end.mp4"
# HEVC (hvc1, as Apple wants it) + AAC
ff -f lavfi -i "$(video 2)" -f lavfi -i "$(audio 2)" -c:v libx265 -tag:v hvc1 -pix_fmt yuv420p -x265-params log-level=error:keyint=30:min-keyint=30:bframes=0 -crf 32 \
   "${aac[@]}" -movflags +faststart -t 2 "$out/hevc_aac_160x90.mp4"
# VP9 + Opus WebM (the platform decoders' WebM)
ff -f lavfi -i "$(video 2)" -f lavfi -i "$(audio 2)" -c:v libvpx-vp9 -b:v 60k -g 30 -pix_fmt yuv420p -c:a libopus -b:a 24k -ac 1 -t 2 "$out/vp9_opus_160x90.webm"
# a phone video: H.264 shown turned by 90 degrees (the track matrix), 1 s, no sound
# (encoded upright, then the display matrix is set on a stream copy: FFmpeg would turn the pixels)
ff -f lavfi -i "$(video 1)" "${h264[@]}" -t 1 "$out/h264_rotated_160x90.tmp.mp4"
ff -display_rotation:v 270 -i "$out/h264_rotated_160x90.tmp.mp4" -c copy -movflags +faststart "$out/h264_rotated_160x90.mp4"
rm -f "$out/h264_rotated_160x90.tmp.mp4"

ls -l "$out"
