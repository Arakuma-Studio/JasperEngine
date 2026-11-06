"""S36 AC2 and BF14, Ogg overshoot.

- Valid Ogg Vorbis files of Kenney's sci-fi pack (tools/fixtures/audio-ogg) decode 44-428 frames past
  their last page's granule position in stb_vorbis. get_asset_info and analyze_audio read them, and Play
  plays them. Files cut with ffmpeg -ss N -c copy overshoot by thousands of frames and are the same class.
  Each loads with exactly its declared frames and the levels of an independent decode cut to that length.
- Ogg Opus, Ogg FLAC and Speex files are named for what they are, not called damaged.
- A cut FLAC, an IMA ADPCM block with a bad step index and an Ogg part that keeps a longer stream's
  granules are refused.

Oracles: the Ogg page headers parsed here (the last page's granule is the length, as the Vorbis spec
and ffmpeg take it), ffmpeg's own decoder, cut to the granule (ffmpeg 4.4 does not trim a stream's end, spec Deviations), Python
sample peaks and pyloudnorm's momentary max of that decode.
"""

from __future__ import annotations

import hashlib
import math
import shutil
import struct
import subprocess
import sys
from pathlib import Path

from harness import Editor, skip

sys.path.insert(0, str(Path(__file__).resolve().parent.parent))
sys.path.insert(0, str(Path(__file__).resolve().parent))
import audio_refs as ar  # noqa: E402
from test_audio_analysis import pyloudnorm  # noqa: E402
CI = True
EDITOR = {"create": True}
RATE = 48000
TIMEOUT = 1800

DIR = "assets/ogg"
OGG_FIXTURES = Path(__file__).resolve().parent.parent.parent / "fixtures" / "audio-ogg"
# Kenney "Sci-Fi Sounds" 1.0 (CC0, License.txt beside them), from kenney_sci-fi-sounds.zip
# (SHA-256 119340f3...9e04, phase 1 spec 2.12.2's pinned packs): Audio/<name>, unchanged
KENNEY = {
    "spaceEngineLow_000.ogg": "d7deee8d7217ce63aab802ad7365a59cacfd243392dc1bd03b0a9d454aa494df",
    "spaceEngineLow_003.ogg": "e8f7ce4dbc20b5aeead35b94f1c8927b0baa416edaa5b39f1699703031111488",
    "spaceEngineSmall_000.ogg": "2bec8ff75d81f0c37a6b78bd63beea6c70375e49ee30b0bbe7f38d654d576831",
    "spaceEngineSmall_002.ogg": "3d7da67ca2d24ca2abd1e257556d74ff28cf152402968c2035d6f1ea92517f25",
}


def rel(ed: Editor, path: Path) -> str:
    return path.relative_to(ed.project).as_posix()


def need_ffmpeg() -> None:
    if not shutil.which("ffmpeg"):
        skip("needs ffmpeg")


def ffmpeg(*args: str) -> None:
    subprocess.run(["ffmpeg", "-nostdin", "-loglevel", "error", "-y", *args], check=True, timeout=300)


# -- Ogg pages --------------------------------------------------------------------------------------


def ogg_pages(raw: bytes) -> list:
    """(flags, granule position, serial, packets ended) of each page, walked by the page headers"""
    pages, pos = [], 0
    while pos + 27 <= len(raw) and raw[pos:pos + 4] == b"OggS":
        flags = raw[pos + 5]
        granule, serial = struct.unpack_from("<qI", raw, pos + 6)
        lacing = raw[pos + 27:pos + 27 + raw[pos + 26]]
        pages.append((flags, granule, serial, sum(1 for x in lacing if x < 255)))
        pos += 27 + len(lacing) + sum(lacing)
    return pages


def declared_frames(path: Path) -> int:
    """the clip's length: the granule position of its last page, which carries the end-of-stream flag"""
    pages = ogg_pages(path.read_bytes())
    assert pages and pages[-1][0] & 4, f"{path.name}: the last page has no end-of-stream flag"
    return pages[-1][1]


def _crc32_ogg(data: bytes) -> int:
    crc = 0
    for byte in data:
        crc ^= byte << 24
        for _ in range(8):
            crc = ((crc << 1) ^ 0x04C11DB7) & 0xFFFFFFFF if crc & 0x80000000 else (crc << 1) & 0xFFFFFFFF
    return crc


def offset_granules(src: Path, dst: Path, offset: int) -> Path:
    """the Ogg file with `offset` added to every audio page's granule position (CRCs rewritten): a
    part cut out of a longer stream that keeps its granules. Its last page counts the offset more
    frames than its packets hold."""
    raw = bytearray(src.read_bytes())
    pos, page = 0, 0
    while pos + 27 <= len(raw) and raw[pos:pos + 4] == b"OggS":
        end = pos + 27 + raw[pos + 26] + sum(raw[pos + 27:pos + 27 + raw[pos + 26]])
        granule = struct.unpack_from("<q", raw, pos + 6)[0]
        if page >= 2 and granule > 0:
            struct.pack_into("<q", raw, pos + 6, granule + offset)
        struct.pack_into("<I", raw, pos + 22, 0)
        struct.pack_into("<I", raw, pos + 22, _crc32_ogg(bytes(raw[pos:end])))
        pos, page = end, page + 1
    dst.write_bytes(bytes(raw))
    return dst


def twin(src: Path, dst: Path, frames: int | None = None) -> Path:
    """ffmpeg's decode of src as a float WAV, cut to `frames` when given"""
    args = ["-i", str(src)]
    if frames is not None:
        args += ["-af", f"atrim=end_sample={frames}"]
    ffmpeg(*args, "-c:a", "pcm_f32le", str(dst))
    return dst


def peak_dbfs(path: Path) -> float:
    wav = ar.read_wav(path)
    return 20 * math.log10(max(ar.peak(channel) for channel in wav.channels))


def kenney_files(ed: Editor) -> list:
    folder = ed.project / DIR / "kenney"
    folder.mkdir(parents=True, exist_ok=True)
    files = []
    for name, sha in KENNEY.items():
        src = OGG_FIXTURES / name
        assert hashlib.sha256(src.read_bytes()).hexdigest() == sha, f"{name}: the fixture's SHA-256 changed"
        shutil.copy(src, folder / name)
        files.append(folder / name)
    return files


async def check_loads_at(ed: Editor, files: list, frames: dict, twins: dict, rate: int, channels: int) -> None:
    """get_asset_info and analyze_audio read each file at `frames[file]`, with the format given and
    the levels of its twin (an independent decode of the same audio, cut to that length): Python's
    sample peak and pyloudnorm's momentary max, and the editor's own analysis of the twin."""
    for path in files:
        info = await ed.call("get_asset_info", path=rel(ed, path))
        audio = info.get("audio")
        print(f"[ogg] get_asset_info {path.name}: {None if audio is None else (audio['frames'], audio['duration_seconds'])} "
              f"{info.get('audio_error')}")
        ed.check(audio is not None, f"{path.name}: get_asset_info reads it: {info.get('audio_error')}")
        ed.check_eq((audio["frames"], audio["sample_rate"], audio["channels"]), (frames[path], rate, channels),
                    f"{path.name}: get_asset_info's frames, rate and channels")

    r = await ed.call_raw("analyze_audio", {"paths": [rel(ed, p) for p in files] + [rel(ed, twins[p]) for p in files],
                                            "image": "none"}, timeout=600)
    ed.check(not r.is_error, f"analyze_audio: {r.text[:600]}")
    errors = {e["path"]: e["error"] for e in r.structured.get("errors") or []}
    ed.check(not errors, f"analyze_audio refused: {errors}")
    items = {item["path"]: item for item in r.structured["items"]}
    reference = await pyloudnorm([(twins[p], "file") for p in files])
    for path, mmax in zip(files, reference):
        item, other = items[rel(ed, path)], items[rel(ed, twins[path])]
        fmt = item["format"]
        print(f"[ogg] analyze_audio {path.name}: {fmt['frames']} frames ({frames[path]} declared), {fmt['duration_s']} s, "
              f"peak {item['peak']['sample_dbfs']} (twin {other['peak']['sample_dbfs']}, Python {peak_dbfs(twins[path]):.2f}), "
              f"momentary max {item['loudness']['momentary_max']} (twin {other['loudness']['momentary_max']}, pyloudnorm {mmax:.2f})")
        ed.check_eq((fmt["frames"], fmt["sample_rate"], fmt["channels"]), (frames[path], rate, channels),
                    f"{path.name}: exactly its declared frames, not the decoder's overshoot")
        ed.check(abs(fmt["duration_s"] - frames[path] / rate) < 0.0011, f"{path.name}: duration {fmt['duration_s']}")
        ed.check_eq(other["format"]["frames"], frames[path], f"{path.name}: the twin holds the declared frames")
        ed.check(abs(item["peak"]["sample_dbfs"] - peak_dbfs(twins[path])) <= 0.05,
                 f"{path.name}: sample peak {item['peak']['sample_dbfs']} against the twin's {peak_dbfs(twins[path]):.3f}")
        ed.check(abs(item["loudness"]["momentary_max"] - mmax) <= 0.1,
                 f"{path.name}: momentary max {item['loudness']['momentary_max']} against pyloudnorm's {mmax:.3f}")
        ed.check(abs(item["loudness"]["momentary_max"] - other["loudness"]["momentary_max"]) <= 0.05,
                 f"{path.name}: momentary max as the twin's")


# -- A: an Ogg Vorbis file that decodes past its last granule ----------------------------------------


async def test_kenney_oggs_that_decode_past_their_end_load_at_their_length(ed: Editor):
    """The four Kenney files declare 220,500 frames (5 s at 44.1 kHz) on their last page.
    get_asset_info and analyze_audio read exactly that many, with the
    levels of ffmpeg's decode (on the tip all four were refused as "cut or damaged")."""
    need_ffmpeg()
    files = kenney_files(ed)
    frames = {p: declared_frames(p) for p in files}
    # ffmpeg's decode cut to the page's count: not every ffmpeg honours the last granule itself
    twins = {p: twin(p, p.with_name(p.stem + "_ffmpeg.wav"), frames[p]) for p in files}
    for path in files:
        ed.check_eq(frames[path], 220500, f"{path.name}: its last page counts 5 s")
        ed.check_eq(len(ar.read_wav(twins[path]).channels[0]), 220500, f"{path.name}: the twin holds its last page's count")
    await ed.call("sync_files", paths=[rel(ed, p) for p in files] + [rel(ed, t) for t in twins.values()])
    await check_loads_at(ed, files, frames, twins, 44100, 1)


async def test_kenney_oggs_play_to_their_end(ed: Editor):
    """Each Kenney file plays in a scene (the pool loads it) and ends by itself."""
    files = kenney_files(ed)
    await ed.call("sync_files", paths=[rel(ed, p) for p in files])
    sid = (await ed.call("create_scene", name="Overshoot", type="2D"))["scene"]["id"]
    await ed.call("create_entity", scene=sid, type="audio_source", name="S",
                  values={"audiosource": {"filename": rel(ed, files[0]), "state": "playing"}})
    await ed.call("save_scene", scene=sid, path="Overshoot.scene")
    for path in files:
        await ed.call("set_properties", scene=sid, entity="S", values={"audiosource": {"filename": rel(ed, path)}})
        await ed.call("play", action="start", scene="Overshoot")
        await ed.call("eval_lua", code="Ended = 0; object('S', 'AudioSource'):getAudioSourceComponent().onStop:add('ogg', "
                                       "function() Ended = Ended + 1 end); return true")
        await ed.call("wait", frames=320)
        stopped, playing = (await ed.call("eval_lua", code="return Ended, object('S', 'AudioSource'):isPlaying()"))["values"]
        await ed.call("play", action="stop")
        print(f"[ogg] play {path.name}: onStop {stopped}, playing {playing}")
        ed.check_eq((stopped, playing), (1, False), f"{path.name}: it ended by itself")


async def test_ogg_cut_with_ffmpeg_copy_loads_at_its_granule_length(ed: Editor):
    """ffmpeg -ss N -c copy keeps the packets before N and shifts the granules, so the stream
    decodes thousands of frames past its last page's count (ffmpeg's own decoder too). It loads at
    that count, with the levels of the decode cut there (on the tip it was refused as "cut or
    damaged"; before the check SoLoud wrote the extra frames past its buffer)."""
    need_ffmpeg()
    folder = ed.project / DIR / "trimmed"
    folder.mkdir(parents=True, exist_ok=True)
    src = folder / "source.ogg"
    ffmpeg("-f", "lavfi", "-i", "sine=f=440:d=6:r=44100", "-af", "volume=-6dB", "-ac", "2", "-c:a", "libvorbis", str(src))
    cuts = []
    for at in ("2.5", "3.7"):
        cut = folder / f"cut_{at}.ogg"
        ffmpeg("-ss", at, "-i", str(src), "-c", "copy", str(cut))
        cuts.append(cut)
    frames = {p: declared_frames(p) for p in cuts}
    twins = {p: twin(p, p.with_name(p.stem + "_ffmpeg.wav"), frames[p]) for p in cuts}
    for path in cuts:
        decoded = len(ar.read_wav(twin(path, path.with_name(path.stem + "_all.wav"))).channels[0])
        print(f"[ogg] {path.name}: its last page counts {frames[path]} frames, ffmpeg decodes {decoded}")
        if decoded <= frames[path]:
            skip(f"this ffmpeg's -ss -c copy writes {path.name} without the overshoot it is meant to test")
        ed.check_eq(len(ar.read_wav(twins[path]).channels[0]), frames[path], f"{path.name}: the twin is cut to the count")
    await ed.call("sync_files", paths=[rel(ed, p) for p in cuts] + [rel(ed, t) for t in twins.values()])
    await check_loads_at(ed, cuts, frames, twins, 44100, 2)


# -- C: other codecs in an Ogg container -------------------------------------------------------------


async def test_other_ogg_codecs_are_named_not_called_damaged(ed: Editor):
    """Ogg Opus, Ogg FLAC and Speex files are refused by get_asset_info, analyze_audio and Play's
    load with the codec's name and "only Ogg Vorbis is supported" (on the tip the pool said "its
    comment header is damaged", the analysis "Ogg Opus is not supported" or "an Ogg file without
    a Vorbis stream"). The editor answers the next call."""
    need_ffmpeg()
    folder = ed.project / DIR / "codecs"
    folder.mkdir(parents=True, exist_ok=True)
    sine = ["-f", "lavfi", "-i", "sine=f=440:d=1:r=48000"]
    cases = {"opus.ogg": "Opus", "flac.ogg": "FLAC", "speex.ogg": "Speex"}
    ffmpeg(*sine, "-c:a", "libopus", str(folder / "opus.ogg"))
    ffmpeg(*sine, "-c:a", "flac", "-f", "ogg", str(folder / "flac.ogg"))
    ffmpeg("-f", "lavfi", "-i", "sine=f=440:d=1:r=16000", "-c:a", "libspeex", str(folder / "speex.ogg"))
    await ed.call("sync_files", paths=[f"{DIR}/codecs/{name}" for name in cases])
    for name, codec in cases.items():
        path = f"{DIR}/codecs/{name}"
        info = await ed.call("get_asset_info", path=path)
        error = info.get("audio_error") or ""
        print(f"[ogg] get_asset_info {name}: {error}")
        ed.check(info.get("audio") is None, f"{name}: refused")
        ed.check(codec in error and "only Ogg Vorbis is supported" in error and "comment header" not in error,
                 f"{name}: the reason names {codec} and says only Ogg Vorbis is supported: {error!r}")
        text = await ed.call("analyze_audio", paths=[path], image="none", expect_error=True)
        ed.check(codec in text and "only Ogg Vorbis is supported" in text, f"{name}: analyze_audio says so too: {text[:300]}")
    (ed.project / "assets/audio").mkdir(parents=True, exist_ok=True)
    ar.write_wav(ed.project / "assets/audio/click_48k.wav", [ar.sine(1000.0, 0.1, RATE, -12.0)], RATE)
    still = await ed.call("get_asset_info", path="assets/audio/click_48k.wav")
    ed.check(still.get("audio") is not None, "the editor answers the next call")

    # Play: the pool's load says the same in the console
    sid = (await ed.call("create_scene", name="Opus", type="2D"))["scene"]["id"]
    await ed.call("create_entity", scene=sid, type="audio_source", name="S",
                  values={"audiosource": {"filename": f"{DIR}/codecs/opus.ogg", "state": "playing"}})
    await ed.call("save_scene", scene=sid, path="Opus.scene")
    cursor = (await ed.call("read_console", limit=1))["next_cursor"]
    await ed.call("play", action="start", scene="Opus")
    await ed.call("wait", frames=5)
    playing = (await ed.call("eval_lua", code="return object('S', 'AudioSource'):isPlaying()"))["values"][0]
    await ed.call("play", action="stop")
    lines = [e["message"] for e in (await ed.call("read_console", since=cursor))["entries"] if "cannot be played" in e["message"]]
    print(f"[ogg] play opus.ogg: playing {playing}, console {lines}")
    ed.check(not playing and any("Opus" in line and "only Ogg Vorbis is supported" in line for line in lines),
             f"Play logs that only Ogg Vorbis is supported: {lines}")


# -- the structural checks that replaced the check's decodes (a guard; passes on the tip too) --------


def ima_with_bad_step(path: Path) -> Path:
    """an IMA ADPCM WAV whose third block's right-channel step index is 100 (0-88 are valid):
    dr_wav stops decoding there"""
    need_ffmpeg()
    ffmpeg("-f", "lavfi", "-i", "sine=f=440:d=0.5:r=48000", "-ac", "2", "-c:a", "adpcm_ima_wav", str(path))
    raw = bytearray(path.read_bytes())
    pos, fmt, data = 12, None, None
    while pos + 8 <= len(raw):
        tag, size = raw[pos:pos + 4], struct.unpack_from("<I", raw, pos + 4)[0]
        if tag == b"fmt ":
            fmt = pos + 8
        if tag == b"data":
            data = pos + 8
            break
        pos += 8 + size + (size & 1)
    block = struct.unpack_from("<H", raw, fmt + 12)[0]
    raw[data + 2 * block + 4 + 2] = 100
    path.write_bytes(bytes(raw))
    return path


async def test_cut_and_damaged_files_are_still_refused(ed: Editor):
    """A FLAC file cut at 50% and at 99%, an IMA ADPCM WAV with a bad block header and an Ogg part
    that keeps a longer stream's granules are refused as cut or damaged, without the decode the
    check used to run; the uncut FLAC, the IMA file before the damage and the Ogg before its
    granules moved still load."""
    need_ffmpeg()
    folder = ed.project / DIR / "damaged"
    folder.mkdir(parents=True, exist_ok=True)
    flac = folder / "whole.flac"
    ffmpeg("-f", "lavfi", "-i", "sine=f=440:d=2:r=44100", "-c:a", "flac", str(flac))
    raw = flac.read_bytes()
    (folder / "cut50.flac").write_bytes(raw[:len(raw) // 2])
    (folder / "cut99.flac").write_bytes(raw[:len(raw) * 99 // 100])
    ima = folder / "ima.wav"
    ffmpeg("-f", "lavfi", "-i", "sine=f=440:d=0.5:r=48000", "-ac", "2", "-c:a", "adpcm_ima_wav", str(ima))
    ima_with_bad_step(folder / "ima_bad.wav")
    ogg = folder / "whole.ogg"
    ffmpeg("-f", "lavfi", "-i", "sine=f=440:d=3:r=22050", "-c:a", "libvorbis", str(ogg))
    offset_granules(ogg, folder / "offset.ogg", 10 * 22050)
    good = ["whole.flac", "ima.wav", "whole.ogg"]
    bad = {"cut50.flac": "cut or damaged", "cut99.flac": "cut or damaged", "ima_bad.wav": "damaged", "offset.ogg": "cut or damaged"}
    await ed.call("sync_files", paths=[f"{DIR}/damaged/{name}" for name in good + list(bad)])
    for name in good:
        info = await ed.call("get_asset_info", path=f"{DIR}/damaged/{name}")
        ed.check(info.get("audio") is not None, f"{name}: loads: {info.get('audio_error')}")
    for name, reason in bad.items():
        info = await ed.call("get_asset_info", path=f"{DIR}/damaged/{name}")
        error = info.get("audio_error") or ""
        print(f"[ogg] get_asset_info {name}: {error}")
        ed.check(info.get("audio") is None and reason in error, f"{name}: refused as {reason}: {error!r}")
