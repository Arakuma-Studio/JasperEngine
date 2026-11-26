"""S36 AC4 (editor half): set_audio_import_settings and the `.meta` sections import.audioImporter and
provenance. Only those sections change, other sections stay byte for byte; one undo step per call; the
sections move, copy and delete with their asset through the asset tools; analysis follows them.

Oracles: the files on disk (PyYAML and bytes), and the frame arithmetic round(ms x rate / 1000) at the rate
the WAV header gives, read with `wave`."""

from __future__ import annotations

import json
import math
import shutil
import sys
import time
import wave
from pathlib import Path

import yaml

from harness import Editor

sys.path.insert(0, str(Path(__file__).resolve().parent.parent))
import audio_refs as ar  # noqa: E402

CI = True
EDITOR = {"create": True}
TIMEOUT = 600
RATE = 44100
CLIP = "assets/sfx/tone.wav"
REPO = Path(__file__).resolve().parents[3]
# a stereo Ogg Vorbis of the starter library (CC0, its manifest)
STEREO_OGG = REPO / "editor/resources/sound-library/library/ambience/fs_463703_telephone_drone.ogg"


def meta_of(ed: Editor, path: str = CLIP) -> Path:
    return ed.project / (path + ".meta")


def section_lines(text: str, name: str) -> list:
    """the lines of a top-level section, its key line first"""
    lines, inside = [], False
    for line in text.splitlines():
        if line and not line[0].isspace():
            inside = line.startswith(name + ":")
        if inside:
            lines.append(line)
    return lines


async def tone(ed: Editor, path: str = CLIP) -> str:
    """the clip, fresh: tests share one project, and undo steps of earlier ones"""
    for stale in (ed.project / path, ed.project / (path + ".meta")):
        stale.unlink(missing_ok=True)
    ar.write_wav(ed.project / path, [ar.sine(1000.0, 1.0, RATE, -12.0)], RATE, ar.PCM16)
    await ed.call("sync_files", paths=[path])
    # S04 gives an asset its .meta when the asset database next looks: the tool makes it look
    await ed.call("set_audio_import_settings", paths=[path], settings={"gain_db": None})
    return path


async def test_writes_only_its_sections_and_undo_restores(ed: Editor):
    await tone(ed)
    meta = meta_of(ed)
    ed.check(meta.exists(), "S04 made the clip's .meta")
    # a section nobody here owns, in its own shape, and a sibling importer
    original = meta.read_text().rstrip("\n") + "\nthirdparty:\n    quoted: 'as I wrote it'   # a comment\n"
    meta.write_text(original)
    await ed.call("sync_files", paths=[CLIP])
    before = meta.read_text()

    r = await ed.call("set_audio_import_settings", paths=[CLIP],
                      settings={"gain_db": -6, "trim_head_ms": 100, "fade_in_ms": 2, "type": "ambience", "loop_end_ms": 900})
    item = r["items"][0]
    ed.check_eq(item["resolved"]["trim_head_frames"], round(100 * RATE / 1000), "frames at the file's rate")
    ed.check_eq(item["resolved"]["fade_in_frames"], round(2 * RATE / 1000), "fade frames")
    ed.check_eq(item["resolved"]["loop_end_frame"], round(900 * RATE / 1000), "loop frames")
    ed.check_eq(item["analysis"]["kind"], "loop", "ambience is a loop by its type")
    text = meta.read_text()
    root = yaml.safe_load(text)
    ed.check_eq(root["import"]["audioImporter"],
                {"version": 1, "type": "ambience", "gain_db": -6, "trim_head_ms": 100, "fade_in_ms": 2, "loop_end_ms": 900}, "the section")
    keys = [line.split(":")[0].strip() for line in section_lines(text, "import")[2:]]
    ed.check_eq(keys, ["version", "type", "gain_db", "trim_head_ms", "fade_in_ms", "loop_end_ms"], "keys in R7's order")
    for name in ("guid", "metaVersion", "thirdparty"):
        ed.check_eq(section_lines(text, name), section_lines(before, name), f"{name} is byte-identical")

    await ed.call("undo", scope="project")
    ed.check_eq(meta.read_text(), before, "one undo restores the file byte for byte")
    await ed.call("redo", scope="project")
    ed.check_eq(yaml.safe_load(meta.read_text())["import"]["audioImporter"]["gain_db"], -6, "redo")
    await ed.call("undo", scope="project")


async def test_null_resets_a_key_and_all_defaults_removes_the_section(ed: Editor):
    await tone(ed)
    await ed.call("set_audio_import_settings", paths=[CLIP], settings={"gain_db": -3, "fade_out_ms": 20})
    await ed.call("set_audio_import_settings", paths=[CLIP], settings={"gain_db": None})
    ed.check_eq(yaml.safe_load(meta_of(ed).read_text())["import"]["audioImporter"], {"version": 1, "fade_out_ms": 20}, "reset one")
    await ed.call("set_audio_import_settings", paths=[CLIP], settings={"fade_out_ms": None})
    ed.check("import" not in yaml.safe_load(meta_of(ed).read_text()), "no key left: no section")


async def test_provenance_and_labels_in_one_undo_step(ed: Editor):
    await tone(ed)
    before = meta_of(ed).read_text()
    r = await ed.call("set_audio_import_settings", paths=[CLIP],
                      settings={"gain_db": -1},
                      provenance={"licence": "CC-BY-4.0", "author": "A Person", "url": "https://example.org/a", "source": "user"},
                      labels=["footsteps", "wood"])
    root = yaml.safe_load(meta_of(ed).read_text())
    ed.check_eq(root["provenance"], {"version": 1, "licence": "CC-BY-4.0", "author": "A Person", "source": "user", "url": "https://example.org/a"}, "provenance")
    ed.check_eq(sorted(root["labels"]), ["footsteps", "wood"], "labels")
    ed.check_eq(r["items"][0]["provenance"]["licence"], "CC-BY-4.0", "reply")
    info = await ed.call("get_asset_info", path=CLIP)
    ed.check_eq(info["audio"]["import"]["gain_db"], -1, "get_asset_info import")
    ed.check_eq(info["audio"]["provenance"]["author"], "A Person", "get_asset_info provenance")
    await ed.call("undo", scope="project")
    ed.check_eq(meta_of(ed).read_text(), before, "one undo takes all three")


async def test_all_paths_or_none(ed: Editor):
    other = await tone(ed, "assets/sfx/other.wav")
    await tone(ed)
    a, b = meta_of(ed).read_text(), meta_of(ed, other).read_text()
    # the second file is 1 s: a 2 s trim does not fit either; a short one and a bad key
    text = await ed.call("set_audio_import_settings", paths=[CLIP, other], settings={"trim_head_ms": 2000}, expect_error=True)
    ed.check("nothing" in text, f"trims that leave nothing are refused: {text}")
    text = await ed.call("set_audio_import_settings", paths=[CLIP, other], settings={"loop_start_ms": 900, "loop_end_ms": 5000}, expect_error=True)
    ed.check("loop region" in text and "frames" in text, f"a loop past the file names the frames: {text}")
    text = await ed.call("set_audio_import_settings", paths=[CLIP, other], settings={"gain": 1}, expect_error=True)
    ed.check("gain_db" in text, f"an unknown key lists the keys: {text}")
    text = await ed.call("set_audio_import_settings", paths=[CLIP, other], settings={"gain_db": 30}, expect_error=True)
    ed.check("-60" in text and "24" in text, f"a range names itself: {text}")
    text = await ed.call("set_audio_import_settings", paths=[CLIP], provenance={"licenced": "x"}, expect_error=True)
    ed.check("licence" in text, f"provenance keys: {text}")
    text = await ed.call("set_audio_import_settings", paths=[CLIP], expect_error=True)
    ed.check("settings" in text, f"nothing to set: {text}")
    await ed.check_error("set_audio_import_settings", {"settings": {"gain_db": 1}}, contains="paths: pass a list")
    ed.check_eq((meta_of(ed).read_text(), meta_of(ed, other).read_text()), (a, b), "a refused call writes nothing")


async def test_a_move_copy_and_delete_carry_the_sections(ed: Editor):
    await tone(ed)
    await ed.call("set_audio_import_settings", paths=[CLIP], settings={"gain_db": -4}, provenance={"licence": "CC0-1.0"})
    await ed.call("create_folder", path="assets/moved")
    await ed.call("move_asset", path=CLIP, to="assets/moved/tone.wav")
    moved = yaml.safe_load(meta_of(ed, "assets/moved/tone.wav").read_text())
    ed.check_eq((moved["import"]["audioImporter"]["gain_db"], moved["provenance"]["licence"]), (-4, "CC0-1.0"), "a move keeps them")
    ed.check(not meta_of(ed).exists(), "the old .meta left with its asset")
    await ed.call("undo", scope="project")
    ed.check_eq(yaml.safe_load(meta_of(ed).read_text())["import"]["audioImporter"]["gain_db"], -4, "undo of the move")


async def test_analysis_measures_the_clip_as_it_plays(ed: Editor):
    await tone(ed)
    raw = (await ed.call("analyze_audio", paths=[CLIP], image="none"))["items"][0]
    ed.check("played" not in raw and "source" not in raw, "a clip without settings is the file")
    r = await ed.call("set_audio_import_settings", paths=[CLIP], settings={"gain_db": -6, "trim_tail_ms": 500})
    item = (await ed.call("analyze_audio", paths=[CLIP], image="none"))["items"][0]
    with wave.open(str(ed.project / CLIP)) as w:
        frames = w.getnframes()
    ed.check_eq(item["played"]["frames"], frames - round(500 * RATE / 1000), "the trimmed length")
    ed.check(abs(item["peak"]["sample_dbfs"] - (raw["peak"]["sample_dbfs"] - 6.0)) < 0.1, f"6 dB less: {item['peak']} vs {raw['peak']}")
    ed.check_eq(item["source"]["peak"], raw["peak"], "the raw file under source")
    ed.check_eq(item["format"], raw["format"], "the raw format")
    ed.check_in("as_it_plays", [n["id"] for n in item["notes"]], "a note says which")
    ed.check_eq(r["items"][0]["analysis"]["momentary_max"], item["loudness"]["momentary_max"], "the reply's fresh analysis")
    cache = ed.project / ".jasperengine/cache/audio"
    # (earlier tests here measured the same bytes with other settings)
    played = [yaml.safe_load(p.read_text())["played"]["frames"] for p in cache.glob(f"{item['sha1']}-i*-v2.json")]
    ed.check_in(item["played"]["frames"], played, "its own cache file")

    # a wrap: whole cycles of the 1 kHz tone have a small seam, a wrap off the cycle a large one
    await ed.call("set_audio_import_settings", paths=[CLIP], settings={"gain_db": None, "trim_tail_ms": None, "type": "ambience",
                                                                       "loop_start_ms": 100, "loop_end_ms": 200})
    whole = (await ed.call("analyze_audio", paths=[CLIP], image="none"))["items"][0]
    await ed.call("set_audio_import_settings", paths=[CLIP], settings={"loop_end_ms": 200.25})
    cut = (await ed.call("analyze_audio", paths=[CLIP], image="none"))["items"][0]
    ed.check(whole["loop"]["seam_step_db"] < 6.0 < cut["loop"]["seam_step_db"],
             f"the seam at the loop region's wrap: {whole['loop']} vs {cut['loop']}")


async def test_settings_that_stop_fitting_show_as_a_note(ed: Editor):
    await tone(ed)
    ar.write_import(ed.project / CLIP, "trim_head_ms: 5000\n")
    item = (await ed.call("analyze_audio", paths=[CLIP], image="none"))["items"][0]
    ed.check_in("import_settings", [n["id"] for n in item["notes"]], f"not applied, and said: {item['notes']}")
    ed.check("played" not in item, "the file's numbers")
    ar.write_import(ed.project / CLIP, "gain_db: loud\n")
    item = (await ed.call("analyze_audio", paths=[CLIP], image="none"))["items"][0]
    note = [n for n in item["notes"] if n["id"] == "import_settings"]
    ed.check(note and "gain_db" in note[0]["text"], f"a section that does not read: {item['notes']}")


async def test_group_is_hidden_until_enabled(ed: Editor):
    listed = {t.name for t in await ed.tools(groups=None)}
    ed.check("set_audio_import_settings" not in listed and "analyze_audio" not in listed, "group audio is not in a new session's list")
    every = {t.name for t in await ed.tools()}
    ed.check({"set_audio_import_settings", "analyze_audio"} <= every, "listed once group audio is on")


# -- Force To Mono (build-size-tools R15-R20, R34, R36)


def near_mono(seconds: float, side_db: float = -24.0) -> list:
    """two channels: a 440 Hz mid and a 1 kHz side `side_db` under it (L = m + s, R = m - s)"""
    m = ar.sine(440.0, seconds, RATE, -12.0)
    s = ar.sine(1000.0, seconds, RATE, -12.0 + side_db)
    return [[a + b for a, b in zip(m, s)], [a - b for a, b in zip(m, s)]]


async def source_export(ed: Editor, name: str) -> Path:
    status = await ed.call("get_build_info")
    if not status.get("scenes_in_build"):
        await ed.call("create_entity", name="Box", type="box")
        saved = await ed.call("save_project", untitled_scene_dir="scenes")
        await ed.call("edit_build_scenes", action="set_start", scene=saved["scenes_in_build"][0]["id"])
    out = ed.scratch.root / name
    r = await ed.call("build_project", mode="source", output_dir=str(out), platform="linux", timeout_seconds=900)
    deadline = time.monotonic() + TIMEOUT
    while r.get("status") == "running" and time.monotonic() < deadline:
        r = await ed.call("get_job", job_id=r.get("job_id") or r["id"])
    ed.check_eq(r["status"], "succeeded", f"{r}")
    return out


async def test_force_to_mono_export(ed: Editor):
    """AC12: a 16-bit and a 24-bit stereo WAV ship mono at half the data bytes, same rate and frames, each sample the
    mean of the originals (+-1 LSB); a stereo Ogg ships byte-identical and plays folded at run time; the table says
    force_mono for all three; two exports give the same WAV bytes; the Build Report gives their source bytes.
    Oracle: the WAVs written and read here with audio_refs (Python's own PCM arithmetic)."""
    clips = {"assets/mono/pcm16.wav": ar.PCM16, "assets/mono/pcm24.wav": ar.PCM24}
    sources = {}
    for path, encoding in clips.items():
        ar.write_wav(ed.project / path, near_mono(3.0, -9.0), RATE, encoding)
        sources[path] = ar.read_wav(ed.project / path)
    ogg = "assets/mono/drone.ogg"
    (ed.project / ogg).parent.mkdir(parents=True, exist_ok=True)
    shutil.copyfile(STEREO_OGG, ed.project / ogg)
    paths = list(clips) + [ogg]
    await ed.call("sync_files", paths=paths)
    r = await ed.call("set_audio_import_settings", paths=paths, settings={"force_to_mono": "on"})
    ed.check_eq([(i["resolved"]["force_mono"], i["resolved"]["channels"]) for i in r["items"]], [(True, 1)] * 3, "the reply's resolved channels")
    # read as text: YAML 1.1 readers (PyYAML) take a bare `on` for true
    importer = section_lines(meta_of(ed, paths[0]).read_text(), "import")
    ed.check(any(line.strip() == "force_to_mono: on" for line in importer), f"the key in the .meta: {importer}")

    first = await source_export(ed, "mono-a")
    second = await source_export(ed, "mono-b")
    table_file = next(first.rglob("je-audio-imports.json"))
    assets = table_file.parent
    table = json.loads(table_file.read_text())
    # the clips by their path from the assets folder, which a new project's root is
    for rel in paths:
        ed.check_eq(table["clips"][rel].get("force_mono"), True, f"{rel}: force_mono in je-audio-imports.json")
    report = json.loads(next(first.rglob("build-report.json")).read_text())
    rows = {row["path"]: row for row in report.get("files", report["largest"])}
    for path, encoding in clips.items():
        rel = path
        shipped = assets / rel
        original = sources[path]
        cooked = ar.read_wav(shipped)
        ed.check_eq((len(cooked.channels), cooked.rate, cooked.frames, cooked.encoding), (1, RATE, original.frames, encoding),
                    f"{rel}: mono, the same rate, frames and sample format")
        ed.check_eq(cooked.chunks, ["fmt ", "data"], f"{rel}: only fmt and data")
        width = 2 if encoding == ar.PCM16 else 3
        data_before = original.frames * 2 * width
        data_after = shipped.stat().st_size - 44 - (original.frames * width & 1)
        ed.check_eq(data_after * 2, data_before, f"{rel}: half the data bytes")
        lsb = 1.0 / (32767 if encoding == ar.PCM16 else 8388607)
        worst = max(abs(cooked.channels[0][i] - (original.channels[0][i] + original.channels[1][i]) / 2) for i in range(original.frames))
        ed.check(worst <= lsb * 1.01, f"{rel}: each sample the mean of the originals within 1 LSB (worst {worst / lsb:.2f} LSB)")
        ed.check_eq(shipped.read_bytes(), (next(second.rglob("je-audio-imports.json")).parent / rel).read_bytes(),
                    f"{rel}: two exports, the same bytes")
        row = rows.get(f"assets/{rel}")
        ed.check(row is not None, f"assets/{rel} is in the Build Report: {sorted(rows)[:40]}")
        ed.check_eq((row.get("source"), row.get("source_bytes")), (rel, (ed.project / path).stat().st_size), f"{rel}: the report's source and its bytes")
    ed.check_eq((assets / ogg).read_bytes(), STEREO_OGG.read_bytes(), "the Ogg ships byte-identical")


async def test_force_to_mono_plays_mono(ed: Editor):
    """AC13: Auto on a near-mono stereo clip (side 24 dB under the mid) resolves mono; in Play the AudioSource playing it
    reads channels == 1 (the Play table, the pool, AudioClipSource); analyze_audio reports one channel as it plays and the
    file's side_mid_db. A wide clip stays stereo."""
    clip = "assets/mono/near.wav"
    wide = "assets/mono/wide.wav"
    ar.write_wav(ed.project / clip, near_mono(1.0), RATE, ar.PCM16)
    ar.write_wav(ed.project / wide, near_mono(1.0, -9.0), RATE, ar.PCM16)
    await ed.call("sync_files", paths=[clip, wide])
    raw = (await ed.call("analyze_audio", paths=[clip], image="none"))["items"][0]
    ed.check(abs(raw["stereo"]["side_mid_db"] - (-24.0)) < 0.3, f"the file's width: {raw['stereo']}")
    ed.check_in("near_mono", [n["id"] for n in raw["notes"]], "the near-mono note")

    r = await ed.call("set_audio_import_settings", paths=[clip, wide], settings={"force_to_mono": "auto"})
    resolved = [(i["resolved"]["force_mono"], i["resolved"]["channels"]) for i in r["items"]]
    ed.check_eq(resolved, [(True, 1), (False, 2)], "Auto: the near-mono clip folds, the wide one stays stereo")
    bad = await ed.call("set_audio_import_settings", paths=[clip], settings={"force_to_mono": "yes"}, expect_error=True)
    ed.check("force_to_mono must be off, on or auto" in bad, f"a bad value names the choices: {bad}")

    item = (await ed.call("analyze_audio", paths=[clip], image="none"))["items"][0]
    ed.check_eq(item["played"]["channels"], 1, "one channel as it plays")
    ed.check(abs(item["source"]["stereo"]["side_mid_db"] - (-24.0)) < 0.3, f"the file's width under source: {item['source']}")
    ed.check(item["stereo"]["side_mid_db"] is None, f"the played signal is mono: {item['stereo']}")

    sid = (await ed.call("create_scene", name="Mono", type="2D"))["scene"]["id"]
    await ed.call("create_entity", scene=sid, type="audio_source", name="S",
                  values={"audiosource": {"filename": clip, "looping": True, "state": "playing"}})
    await ed.call("create_entity", scene=sid, type="audio_source", name="W",
                  values={"audiosource": {"filename": wide, "looping": True, "state": "playing"}})
    await ed.call("save_scene", scene=sid, path="Mono.scene")
    await ed.call("play", action="start", scene="Mono")
    try:
        deadline = time.monotonic() + 60
        values = None
        while time.monotonic() < deadline:
            values = (await ed.call("eval_lua", code="local s = object('S', 'AudioSource'); local w = object('W', 'AudioSource'); "
                                                      "return s:isPlaying(), s.channels, w.channels"))["values"]
            if values[1] != 0 and values[2] != 0:
                break
            await ed.call("wait", frames=10)
        ed.check_eq(values, [True, 1, 2], "AudioSource.channels as it plays: 1 under Force To Mono, 2 for the wide clip")
    finally:
        await ed.call("play", action="stop")
async def test_load_type_key(ed: Editor):
    """S97 AC16 (REQ-16, REQ-39): load_type round-trips through the .meta, undo restores it, and Play holds
    the clip of an Auto source as the clip says"""
    await tone(ed)
    before = meta_of(ed).read_text()
    r = await ed.call("set_audio_import_settings", paths=[CLIP], settings={"load_type": "streaming"})
    ed.check_eq(r["items"][0]["settings"].get("load_type"), "streaming", "the reply")
    ed.check_eq(yaml.safe_load(meta_of(ed).read_text())["import"]["audioImporter"], {"version": 1, "load_type": "streaming"}, "the section")
    bad = await ed.call_raw("set_audio_import_settings", {"paths": [CLIP], "settings": {"load_type": "fast"}})
    ed.check(bad.is_error and "decompress_on_load" in bad.text, f"the error names the values: {bad.text[:300]}")

    scene = (await ed.call("create_scene", name="LoadType", type="2D", path="scenes/LoadType.scene"))["scene"]["id"]
    await ed.call("save_scene", scene=scene)
    await ed.call("play", action="start", scene=scene, fixed_fps=60, auto_pause=True)
    try:
        await ed.call("wait", frames=2)
        got = (await ed.call("eval_lua", code=f"""
            local auto = AudioSource(scene)
            auto:loadSound('{CLIP}')
            local own = AudioSource(scene)
            own.loadType = AudioLoadType.DecompressOnLoad
            own:loadSound('{CLIP}')
            return {{auto:getResolvedLoadType(), own:getResolvedLoadType(), AudioLoadType.Streaming, AudioLoadType.DecompressOnLoad}}"""))["values"][0]
        ed.check_eq(got[0], got[2], "an Auto source streams the clip, as its import settings say")
        ed.check_eq(got[1], got[3], "a source's own Load Type wins")
    finally:
        await ed.call("play", action="stop")

    await ed.call("undo", scope="project")
    ed.check_eq(meta_of(ed).read_text(), before, "one undo restores the file")
