#!/usr/bin/env python3
# The test clips of the HTML UI's <video> (docs/html-ui/phase-6-video.md §5.4), and the looping
# background of the video_menu example.
#
#   python3 tools/htmlui/gen_video_fixtures.py            write every clip (needs FFmpeg with
#                                                         libaom-av1, libopus and libvpx-vp9)
#   python3 tools/htmlui/gen_video_fixtures.py --check    decode the committed clips with FFmpeg and
#                                                         check what the tests rely on (no encoding)
#   python3 tools/htmlui/gen_video_fixtures.py --only testcard.webm [--only ...]
#
# The frames and the audio are made here, in Python (no other dependency), and only encoded by
# FFmpeg. AV1 bitstreams differ between encoder versions, so --check verifies the content of the
# committed files (sizes, frame counts, codecs, timestamps, the frame-index pattern of every frame,
# the tone bursts) instead of comparing bytes with a new encode.
#
# The test card (320x180, 30 fps, 2 s, 60 frames; every 8-bit value below is an sRGB value
# converted to BT.709 limited-range YUV 4:2:0 by FFmpeg's swscale):
#   y   0..47   eight 40 px colour bars at 75 %: grey, yellow, cyan, green, magenta, red, blue, black
#   y  48..111  dark grey (64), a 32x32 white square at x = (8 * frame) mod 288, y = 64; on the first
#               frame of every second (0, 30) the whole band is white (the flash the tone burst marks)
#   y 112..179  mid grey (128): the frame index in 8 bits, most significant first, as 32x32 blocks
#               at y 124..155, x = 16 + 36 * bit (white = 1, black = 0); a black-to-white gradient
#               at y 164..179
# The audio is 48 kHz stereo: silence, with a 50 ms burst at the start of every second, 1 kHz on
# the left channel and 2 kHz on the right. Keyframes are at 0 and 1 s.
#
# Clips (engine/tests/htmlui/fixtures/video/):
#   testcard.webm          AV1 8-bit + Opus stereo
#   testcard-noaudio.webm  AV1 8-bit
#   testcard-10bit.webm    AV1 10-bit (main profile)
#   testcard-444.webm      AV1 8-bit 4:4:4 (high profile), 0.5 s
#   testcard-51.webm       AV1 + Opus 5.1 (six channels, multistream), 0.5 s
#   odd-size.webm          AV1 97x55 (odd sizes, chroma rounding), 0.5 s
#   testcard-vp9.webm      VP9 (the unsupported-codec path), 0.5 s
#   corrupt.webm           testcard-noaudio.webm with the OBU header of frame 40 made invalid
#   poster.png             a 320x180 poster (teal and magenta squares)
# and docs/mcp/guide/html-ui-examples/video_menu/assets/menu-loop.webm: 640x360, 30 fps, 4 s,
# no audio, a seamless loop of drifting colour bands (sin waves with a 4 s period).

import argparse
import math
import os
import re
import shutil
import struct
import subprocess
import sys
import tempfile
import zlib

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
FIXTURES = os.path.join(ROOT, "engine", "tests", "htmlui", "fixtures", "video")
EXAMPLE_ASSETS = os.path.join(ROOT, "docs", "mcp", "guide", "html-ui-examples", "video_menu", "assets")

W, H, FPS, SECONDS = 320, 180, 30, 2
RATE = 48000

BARS = [(191, 191, 191), (191, 191, 0), (0, 191, 191), (0, 191, 0), (191, 0, 191), (191, 0, 0), (0, 0, 191), (0, 0, 0)]
SQUARE = 32
BIT_BLOCK = 32
BIT_Y = 124
BIT_X0 = 16
BIT_STEP = 36
GRADIENT_Y = 164

AV1_COLOR = ["-color_primaries", "bt709", "-color_trc", "bt709", "-colorspace", "bt709", "-color_range", "tv"]
TO_709 = "scale=out_color_matrix=bt709:out_range=limited"


def ffmpeg():
    exe = shutil.which("ffmpeg")
    if not exe:
        sys.exit("gen_video_fixtures: ffmpeg not found on PATH")
    return exe


def run(cmd, data=None):
    proc = subprocess.run(cmd, input=data, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    if proc.returncode != 0:
        sys.exit("gen_video_fixtures: %s failed:\n%s" % (" ".join(cmd[:6]), proc.stderr.decode(errors="replace")[-2000:]))
    return proc.stdout


# ---------------------------------------------------------------- the test card

def card_frame(index, width=W, height=H):
    """RGB24 bytes of frame `index` of the test card (at 320x180; other sizes crop or pad it)."""
    gray = lambda v: bytes((v, v, v))
    rows = []
    bars_row = b"".join(bytes(c) * 40 for c in BARS)
    flash = index % FPS == 0
    square_x = (8 * index) % (W - SQUARE)
    band = gray(255) * W if flash else gray(64) * W
    square_row = band if flash else gray(64) * square_x + gray(255) * SQUARE + gray(64) * (W - SQUARE - square_x)
    bits_row = bytearray(gray(128) * W)
    for bit in range(8):
        on = (index >> (7 - bit)) & 1
        x = BIT_X0 + BIT_STEP * bit
        bits_row[3 * x:3 * (x + BIT_BLOCK)] = gray(255 if on else 0) * BIT_BLOCK
    bits_row = bytes(bits_row)
    gradient_row = b"".join(gray(round(255 * x / (W - 1))) for x in range(W))
    for y in range(H):
        if y < 48:
            rows.append(bars_row)
        elif y < 112:
            rows.append(square_row if 64 <= y < 64 + SQUARE else band)
        elif BIT_Y <= y < BIT_Y + BIT_BLOCK:
            rows.append(bits_row)
        elif y >= GRADIENT_Y:
            rows.append(gradient_row)
        else:
            rows.append(gray(128) * W)
    if (width, height) == (W, H):
        return b"".join(rows)
    out = []
    for y in range(height):
        row = rows[min(y, H - 1)]
        out.append((row * ((width // W) + 1))[:3 * width])
    return b"".join(out)


def card_frames(count, width=W, height=H):
    return b"".join(card_frame(i, width, height) for i in range(count))


def card_audio(seconds, channels=2):
    """float32 little-endian interleaved PCM: a 50 ms burst at the start of every second."""
    samples = int(seconds * RATE)
    burst = int(0.05 * RATE)
    freqs = [1000.0, 2000.0, 3000.0, 4000.0, 5000.0, 6000.0][:channels]
    out = bytearray()
    for n in range(samples):
        t = n % RATE
        frame = []
        for ch in range(channels):
            frame.append(0.5 * math.sin(2.0 * math.pi * freqs[ch] * t / RATE) if t < burst else 0.0)
        out += struct.pack("<%df" % channels, *frame)
    return bytes(out)


def encode_card(path, frames, seconds, width=W, height=H, pix_fmt="yuv420p", audio_channels=0, codec="av1"):
    with tempfile.TemporaryDirectory() as tmp:
        video = os.path.join(tmp, "card.rgb")
        with open(video, "wb") as f:
            f.write(card_frames(frames, width, height))
        cmd = [ffmpeg(), "-v", "error", "-nostdin", "-y", "-f", "rawvideo", "-pix_fmt", "rgb24", "-s", "%dx%d" % (width, height),
               "-r", str(FPS), "-i", video]
        if audio_channels:
            audio = os.path.join(tmp, "card.f32")
            with open(audio, "wb") as f:
                f.write(card_audio(seconds, audio_channels))
            cmd += ["-f", "f32le", "-ar", str(RATE), "-ac", str(audio_channels), "-i", audio]
        cmd += ["-map", "0:v:0"] + (["-map", "1:a:0"] if audio_channels else [])
        cmd += ["-vf", TO_709 + ",format=" + pix_fmt]
        if codec == "av1":
            cmd += ["-c:v", "libaom-av1", "-cpu-used", "8", "-threads", "1", "-row-mt", "0", "-tiles", "1x1", "-crf", "18", "-b:v", "0",
                    "-g", str(FPS), "-keyint_min", str(FPS), "-force_key_frames", "expr:eq(mod(n,%d),0)" % FPS, "-lag-in-frames", "0"]
        else:
            cmd += ["-c:v", "libvpx-vp9", "-deadline", "good", "-cpu-used", "8", "-threads", "1", "-crf", "30", "-b:v", "0", "-g", str(FPS)]
        cmd += AV1_COLOR
        if audio_channels:
            cmd += ["-c:a", "libopus", "-b:a", "%dk" % (48 * audio_channels), "-frame_duration", "20", "-application", "audio"]
            if audio_channels > 2:
                cmd += ["-mapping_family", "1"]
        cmd += ["-t", str(seconds), "-f", "webm", path]
        run(cmd)


# ---------------------------------------------------------------- EBML (for corrupt.webm)

def read_vint(data, pos, keep_marker):
    first = data[pos]
    length = 1
    mask = 0x80
    while length <= 8 and not (first & mask):
        mask >>= 1
        length += 1
    value = first if keep_marker else first & (mask - 1)
    for i in range(1, length):
        value = (value << 8) | data[pos + i]
    return value, length


def simple_blocks(data):
    """(payload offset, track) of every SimpleBlock, in file order."""
    blocks = []
    containers = {0x18538067, 0x1F43B675, 0xA0}   # Segment, Cluster, BlockGroup

    def walk(start, end):
        pos = start
        while pos < end:
            element, n = read_vint(data, pos, True)
            size, m = read_vint(data, pos + n, False)
            body = pos + n + m
            if size == (1 << (7 * m)) - 1:       # unknown size: up to the parent's end
                size = end - body
            if element in containers:
                walk(body, min(end, body + size))
            elif element in (0xA3, 0xA1):        # SimpleBlock, Block
                track, t = read_vint(data, body, False)
                blocks.append((body + t + 3, track))
            pos = body + size

    walk(0, len(data))
    return blocks


def make_corrupt(source, path, frame=40):
    data = bytearray(open(source, "rb").read())
    video = [offset for offset, track in simple_blocks(bytes(data)) if track == 1]
    if len(video) <= frame:
        sys.exit("gen_video_fixtures: %s has %d video blocks" % (source, len(video)))
    # the first OBU header of the frame: forbidden bit set, reserved OBU type 0 -> invalid stream
    data[video[frame]] = 0x80
    with open(path, "wb") as f:
        f.write(data)


# ---------------------------------------------------------------- PNG (poster)

def write_png(path, width, height, rgb):
    raw = b"".join(b"\x00" + rgb[3 * width * y:3 * width * (y + 1)] for y in range(height))

    def chunk(tag, body):
        return struct.pack(">I", len(body)) + tag + body + struct.pack(">I", zlib.crc32(tag + body) & 0xFFFFFFFF)

    png = b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", width, height, 8, 2, 0, 0, 0))
    png += chunk(b"IDAT", zlib.compress(raw, 9)) + chunk(b"IEND", b"")
    with open(path, "wb") as f:
        f.write(png)


def poster_rgb():
    teal, magenta = bytes((0, 128, 128)), bytes((200, 0, 160))
    out = bytearray()
    for y in range(H):
        for x in range(W):
            out += teal if ((x // 40) + (y // 45)) % 2 == 0 else magenta
    return bytes(out)


# ---------------------------------------------------------------- the example's loop

def encode_menu_loop(path):
    # drifting bands: every term has a period of 4 s, so frame 120 equals frame 0 (seamless)
    expr_r = "clip(50+28*sin(2*PI*(X/W*1.5+T/4))+18*sin(2*PI*(Y/H-T/4)),0,255)"
    expr_g = "clip(34+22*sin(2*PI*(X/W+Y/H*0.5+T/2)),0,255)"
    expr_b = "clip(96+56*sin(2*PI*(X/W*0.7-T/4)+1.3)+30*sin(2*PI*(Y/H*1.2+T/4)),0,255)"
    src = "color=c=black:s=640x360:r=30:d=4,format=rgb24,geq=r='%s':g='%s':b='%s'" % (expr_r, expr_g, expr_b)
    run([ffmpeg(), "-v", "error", "-nostdin", "-y", "-f", "lavfi", "-i", src, "-vf", TO_709 + ",format=yuv420p",
         "-c:v", "libaom-av1", "-cpu-used", "6", "-threads", "1", "-row-mt", "0", "-crf", "34", "-b:v", "0", "-g", "120",
         "-lag-in-frames", "0"] + AV1_COLOR + ["-f", "webm", path])


# ---------------------------------------------------------------- the clips

def clips():
    return {
        "testcard.webm": lambda p: encode_card(p, FPS * SECONDS, SECONDS, audio_channels=2),
        "testcard-noaudio.webm": lambda p: encode_card(p, FPS * SECONDS, SECONDS),
        "testcard-10bit.webm": lambda p: encode_card(p, FPS * SECONDS, SECONDS, pix_fmt="yuv420p10le"),
        "testcard-444.webm": lambda p: encode_card(p, FPS // 2, 0.5, pix_fmt="yuv444p"),
        "testcard-51.webm": lambda p: encode_card(p, FPS // 2, 0.5, audio_channels=6),
        "odd-size.webm": lambda p: encode_card(p, FPS // 2, 0.5, width=97, height=55),
        "testcard-vp9.webm": lambda p: encode_card(p, FPS // 2, 0.5, codec="vp9"),
        "corrupt.webm": lambda p: make_corrupt(os.path.join(FIXTURES, "testcard-noaudio.webm"), p),
        "poster.png": lambda p: write_png(p, W, H, poster_rgb()),
    }


def generate(only):
    os.makedirs(FIXTURES, exist_ok=True)
    for name, make in clips().items():
        if only and name not in only:
            continue
        path = os.path.join(FIXTURES, name)
        make(path)
        print("wrote %s (%d bytes)" % (os.path.relpath(path, ROOT), os.path.getsize(path)))
    if not only or "menu-loop.webm" in only:
        os.makedirs(EXAMPLE_ASSETS, exist_ok=True)
        path = os.path.join(EXAMPLE_ASSETS, "menu-loop.webm")
        encode_menu_loop(path)
        print("wrote %s (%d bytes)" % (os.path.relpath(path, ROOT), os.path.getsize(path)))


# ---------------------------------------------------------------- --check

def probe(path):
    out = run(["ffprobe", "-v", "error", "-show_entries",
               "stream=index,codec_type,codec_name,width,height,pix_fmt,channels,sample_rate,color_space,color_range",
               "-show_entries", "format=format_name,duration", "-of", "default=nw=0", path]).decode()
    streams, current, fmt = [], None, {}
    for line in out.splitlines():
        if line == "[STREAM]":
            current = {}
        elif line == "[/STREAM]":
            streams.append(current)
            current = None
        elif "=" in line:
            key, value = line.split("=", 1)
            (current if current is not None else fmt)[key] = value
    return streams, fmt


def frame_index(rgb, width, frame):
    row = BIT_Y + BIT_BLOCK // 2
    value = 0
    for bit in range(8):
        x = BIT_X0 + BIT_STEP * bit + BIT_BLOCK // 2
        luma = rgb[frame + 3 * (row * width + x) + 1]
        value = (value << 1) | (1 if luma > 128 else 0)
    return value


def check():
    problems = []

    def expect(cond, message):
        if not cond:
            problems.append(message)

    specs = {
        "testcard.webm": (W, H, FPS * SECONDS, "yuv420p", 2),
        "testcard-noaudio.webm": (W, H, FPS * SECONDS, "yuv420p", 0),
        "testcard-10bit.webm": (W, H, FPS * SECONDS, "yuv420p10le", 0),
        "testcard-444.webm": (W, H, FPS // 2, "yuv444p", 0),
        "testcard-51.webm": (W, H, FPS // 2, "yuv420p", 6),
        "odd-size.webm": (97, 55, FPS // 2, "yuv420p", 0),
    }
    for name, (width, height, frames, pix_fmt, channels) in specs.items():
        path = os.path.join(FIXTURES, name)
        if not os.path.exists(path):
            problems.append(name + ": missing")
            continue
        streams, fmt = probe(path)
        video = [s for s in streams if s.get("codec_type") == "video"]
        audio = [s for s in streams if s.get("codec_type") == "audio"]
        expect("webm" in fmt.get("format_name", ""), name + ": not WebM")
        expect(len(video) == 1 and video[0].get("codec_name") == "av1", name + ": no AV1 video track")
        if not video:
            continue
        v = video[0]
        expect((int(v["width"]), int(v["height"])) == (width, height), name + ": size %sx%s" % (v["width"], v["height"]))
        expect(v.get("pix_fmt") == pix_fmt, name + ": pixel format " + v.get("pix_fmt", "?"))
        expect(v.get("color_space") == "bt709" and v.get("color_range") == "tv", name + ": not tagged BT.709 limited")
        if channels:
            expect(len(audio) == 1 and audio[0].get("codec_name") == "opus" and int(audio[0]["channels"]) == channels,
                   name + ": audio is not Opus with %d channels" % channels)
        else:
            expect(not audio, name + ": has an audio track")
        rgb = run([ffmpeg(), "-v", "error", "-i", path, "-map", "0:v:0", "-vf", "scale=in_color_matrix=bt709:in_range=limited",
                   "-pix_fmt", "rgb24", "-f", "rawvideo", "-"])
        count = len(rgb) // (3 * width * height)
        expect(count == frames, name + ": %d frames, expected %d" % (count, frames))
        if width == W:
            for i in range(count):
                got = frame_index(rgb, width, i * 3 * width * height)
                if got != i:
                    problems.append("%s: frame %d shows index %d" % (name, i, got))
                    break
        if channels == 2:
            pcm = run([ffmpeg(), "-v", "error", "-i", path, "-map", "0:a:0", "-f", "f32le", "-ac", "2", "-ar", str(RATE), "-"])
            values = struct.unpack("<%df" % (len(pcm) // 4), pcm)
            for second in range(SECONDS):
                window = values[2 * second * RATE:2 * (second * RATE + RATE // 10)]
                onset = next((n // 2 for n, s in enumerate(window) if abs(s) > 0.1), None)
                expect(onset is not None and onset < 48, "%s: burst %d starts at sample %s" % (name, second, onset))
    vp9 = os.path.join(FIXTURES, "testcard-vp9.webm")
    if os.path.exists(vp9):
        streams, _ = probe(vp9)
        expect(any(s.get("codec_name") == "vp9" for s in streams), "testcard-vp9.webm: not VP9")
    else:
        problems.append("testcard-vp9.webm: missing")
    corrupt = os.path.join(FIXTURES, "corrupt.webm")
    clean = os.path.join(FIXTURES, "testcard-noaudio.webm")
    if os.path.exists(corrupt) and os.path.exists(clean):
        a, b = open(clean, "rb").read(), open(corrupt, "rb").read()
        diff = [i for i in range(min(len(a), len(b))) if a[i] != b[i]]
        expect(len(a) == len(b) and len(diff) == 1 and b[diff[0]] == 0x80, "corrupt.webm: not testcard-noaudio.webm with one OBU header byte changed")
        blocks = [o for o, t in simple_blocks(a) if t == 1]
        expect(diff and len(blocks) > 40 and diff[0] == blocks[40], "corrupt.webm: the changed byte is not frame 40's first byte")
    else:
        problems.append("corrupt.webm: missing")
    poster = os.path.join(FIXTURES, "poster.png")
    expect(os.path.exists(poster) and open(poster, "rb").read(8) == b"\x89PNG\r\n\x1a\n", "poster.png: missing or not a PNG")
    loop = os.path.join(EXAMPLE_ASSETS, "menu-loop.webm")
    if os.path.exists(loop):
        streams, fmt = probe(loop)
        expect(any(s.get("codec_name") == "av1" and s.get("width") == "640" for s in streams), "menu-loop.webm: not 640x360 AV1")
        expect(abs(float(fmt.get("duration", "0")) - 4.0) < 0.05, "menu-loop.webm: duration " + fmt.get("duration", "?"))
    else:
        problems.append("menu-loop.webm: missing")
    for problem in problems:
        print("gen_video_fixtures --check: " + problem, file=sys.stderr)
    if problems:
        return 1
    print("gen_video_fixtures --check: every clip is as described")
    return 0


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--check", action="store_true")
    parser.add_argument("--only", action="append", default=[])
    args = parser.parse_args()
    if args.check:
        return check()
    generate(set(args.only))
    return 0


if __name__ == "__main__":
    sys.exit(main())
