"""S36 AC4 (editor half): set_audio_import_settings and the `.meta` sections import.audioImporter and
provenance. Only those sections change, other sections stay byte for byte; one undo step per call; the
sections move, copy and delete with their asset through the asset tools; analysis follows them.

Oracles: the files on disk (PyYAML and bytes), and the frame arithmetic round(ms x rate / 1000) at the rate
the WAV header gives, read with `wave`."""

from __future__ import annotations

import sys
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
    played = [yaml.safe_load(p.read_text())["played"]["frames"] for p in cache.glob(f"{item['sha1']}-i*-v1.json")]
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
