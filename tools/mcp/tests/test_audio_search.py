"""S36 AC11: search_sounds. Words, types, filters, scope, similar_to and the Search palette's Sounds, over
the project's clips and the built-in clips, never using the network.

Oracles: signals built here with known character, the cache's numbers for the distance formula, hashlib over
the imported bytes. No network: nothing here needs one, and the editor's sound code has no way to reach it
(test_never_network reads the sources)."""

from __future__ import annotations

import hashlib
import json
import math
import random
import re
import sys
from pathlib import Path

from harness import Editor

sys.path.insert(0, str(Path(__file__).resolve().parent.parent))
import audio_refs as ar  # noqa: E402

CI = True
EDITOR = {"create": True}
TIMEOUT = 1800
REPO = Path(__file__).resolve().parents[3]
ESSENTIALS = REPO / "editor/resources/sound-library/essentials"
RATE = 48000

STEPS = [f"assets/sounds/step_{i}.wav" for i in range(1, 5)]
PAD = "assets/sounds/pad.wav"
CLICK = "assets/sounds/click.wav"
CLICK_1K1 = "assets/sounds/zz_click_1k1.wav"
NOISE_8K = "assets/sounds/zz_noise_8k.wav"
BANDS = ["31.5", "63", "125", "250", "500", "1k", "2k", "4k", "8k", "16k"]
STATE: dict = {}


def manifest() -> dict:
    return json.loads((ESSENTIALS / "manifest.json").read_text())


def burst(freq: float, seconds: float, dbfs: float, fade_ms: float = 2.0) -> list:
    """a sine burst with short raised-cosine ends (no edge click)"""
    out = ar.sine(freq, seconds, RATE, dbfs)
    n = int(fade_ms * RATE / 1000)
    for k in range(n):
        w = 0.5 - 0.5 * math.cos(math.pi * k / n)
        out[k] *= w
        out[-1 - k] *= w
    return out


def pad() -> list:
    """1.5 s of 220 Hz with a 400 ms rise: a soft attack"""
    out = ar.sine(220.0, 1.5, RATE, -18.0)
    rise = int(0.4 * RATE)
    for k in range(rise):
        out[k] *= k / rise
    return out


def impulse() -> list:
    out = [0.0] * int(0.05 * RATE)
    out[100] = 0.5
    return out


def click_1k1() -> list:
    """a 1.1 kHz click: 20 ms of the tone under a fast exponential decay"""
    n = int(0.05 * RATE)
    return [0.5 * math.exp(-k / (0.004 * RATE)) * math.sin(2 * math.pi * 1100 * k / RATE) for k in range(n)]


def noise_8k() -> list:
    """an 8 kHz-ish noise burst: white noise through a first difference (a high shelf), 50 ms"""
    rng = random.Random(8)
    white = [rng.uniform(-0.3, 0.3) for _ in range(int(0.05 * RATE) + 1)]
    return [white[k + 1] - white[k] for k in range(len(white) - 1)]


async def setup(ed: Editor):
    """the clips and events, once per editor"""
    if STATE.get("editor") is ed:
        return
    for i, path in enumerate(STEPS):
        ar.write_wav(ed.project / path, [burst(1000.0, 0.1, -20.0 - 2 * i)], RATE)
    ar.write_wav(ed.project / PAD, [pad()], RATE)
    ar.write_wav(ed.project / CLICK, [impulse()], RATE)
    ar.write_wav(ed.project / CLICK_1K1, [click_1k1()], RATE)
    ar.write_wav(ed.project / NOISE_8K, [noise_8k()], RATE)
    await ed.call("sync_files", paths=[*STEPS, PAD, CLICK, CLICK_1K1, NOISE_8K])
    await ed.call("set_audio_import_settings", paths=STEPS[:2], settings={"type": "footstep"}, labels=["concrete", "player"])
    STATE.clear()
    STATE["editor"] = ed


async def search(ed: Editor, **args) -> dict:
    return await ed.call("search_sounds", args, timeout=900)


def paths(result: dict) -> list:
    return [r.get("path") or r.get("id") for r in result["results"]]


IMPORTED = "assets/imported/essentials"


def mine(result: dict) -> list:
    """the results that are this test's own clips (not the essentials an earlier test imported)"""
    return [p for p in paths(result) if not p.startswith(IMPORTED + "/")]


def cached(ed: Editor, rel: str) -> dict:
    """the analysis cache file of a project file (its bytes' SHA-1, the newest version)"""
    sha1 = hashlib.sha1((ed.project / rel).read_bytes()).hexdigest()
    found = sorted((ed.project / ".jasperengine" / "cache" / "audio").glob(f"{sha1}-v*.json"),
                   key=lambda p: int(re.search(r"-v(\d+)\.json$", p.name).group(1)))
    assert found, f"no cache file for {rel}"
    return json.loads(found[-1].read_text())


def distance(a: dict, b: dict) -> float:
    """R23's distance, from two cached analyses (SoundSearch's guards: centroid at least 1 Hz)"""
    def features(c: dict) -> list:
        return [math.log2(max(c["format"]["duration_s"], 0.01)) / 4,
                math.log2(max(c["onset"]["attack_ms"] or 0.0, 0.5)) / 4,
                math.log2(max(c["centroid_hz"], 1.0) / 1000) / 2,
                c["flatness"],
                c["pitch"]["confidence"] or 0.0]
    fa, fb = features(a), features(b)
    d = math.sqrt(sum((x - y) ** 2 for x, y in zip(fa, fb)))
    return d + 0.5 * sum(abs(a["band_share"][k] - b["band_share"][k]) for k in BANDS)


# -- M-1 ---------------------------------------------------------------------------------------------


async def test_words_type_and_scope(ed: Editor):
    """query "step" in the project gives the four steps, and with scope all
    the essentials' footsteps too (their ids hold "footstep"); every word must match; type footstep in
    the essentials gives the manifest's footsteps; scope essentials with no query gives all 40."""
    await setup(ed)
    got = await search(ed, query="step", scope="project", limit=50)
    print(f"[search] step in the project: {paths(got)}")
    ed.check_eq(sorted(mine(got)), sorted(STEPS), "the four steps")
    labelled = next(r for r in got["results"] if r.get("path") == STEPS[0])
    ed.check_eq((labelled["type"], sorted(labelled["labels"])), ("footstep", ["concrete", "player"]), "the importer's type, the S25 labels")

    everywhere = await search(ed, query="step", limit=200)
    footsteps = sorted(f"essentials:{c['id']}" for c in manifest()["clips"] if "step" in c["id"] or "step" in c.get("description", "").lower())
    ed.check(set(footsteps) <= set(paths(everywhere)), f"scope all adds the essentials that say step: {paths(everywhere)}")
    # ranking: query words in tags first ("footstep" tags hold "step"), then in the name, then project
    # before essentials and path order
    words_in_tags = [any("step" in t for t in r["labels"]) for r in everywhere["results"]]
    ed.check(words_in_tags == sorted(words_in_tags, reverse=True), f"tag matches rank first: {paths(everywhere)}")

    both = await search(ed, query="STEP concrete", scope="project")
    ed.check_eq(sorted(mine(both)), sorted(STEPS[:2]), "every word must match (any case), labels too")

    typed = await search(ed, type="footstep", scope="essentials", limit=200)
    expected = sorted(f"essentials:{c['id']}" for c in manifest()["clips"] if c["type"] == "footstep")
    print(f"[search] footstep essentials: {paths(typed)}")
    ed.check_eq(sorted(paths(typed)), expected, "type footstep: the manifest's footsteps")
    ed.check_eq(len(expected), 6, "six essential footsteps")
    typed_project = await search(ed, type="footstep", scope="project")
    ed.check_eq(sorted(paths(typed_project)), sorted(STEPS[:2]), "type matches the importer's")

    all40 = await search(ed, scope="essentials", limit=200)
    ed.check_eq((all40["total"], len(all40["results"])), (40, 40), "scope essentials: all 40")
    ed.check_eq(all40["scope"], "essentials", "the scope in the reply")
    limited = await search(ed, scope="essentials", limit=5)
    ed.check_eq((limited["total"], len(limited["results"])), (40, 5), "limit cuts the list, total counts every match")
    first = all40["results"][0]
    for key in ("n", "id", "source", "kind", "type", "sound_kind", "labels", "duration_ms", "lufs", "centroid_hz", "licence", "in_project"):
        ed.check(key in first, f"an essential's item has {key}: {first}")
    ed.check_eq((first["source"], first["kind"], first["licence"]["id"]), ("essentials", "clip", "CC0-1.0"), "an essential's labels")


async def test_filters(ed: Editor):
    """attack soft gives the pad and not the clicks; brightness buckets follow the cached centroid;
    duration, tonal, kind and loop (a loop region) filter as R23 says."""
    await setup(ed)
    soft = await search(ed, scope="project", filters={"attack": "soft"}, limit=200)
    print(f"[search] soft: {paths(soft)}")
    ed.check_in(PAD, paths(soft), "the pad has a soft attack")
    for clip in (CLICK, CLICK_1K1, *STEPS):
        ed.check(clip not in paths(soft), f"{clip} is not soft")
    sharp = await search(ed, scope="project", filters={"attack": "sharp"}, limit=50)
    ed.check_in(CLICK, paths(sharp), "an impulse is sharp")

    every = await search(ed, scope="project", filters={"kind": "clip"}, limit=200)
    clips = [r["path"] for r in every["results"]]
    ed.check(all(r["kind"] == "clip" for r in every["results"]) and PAD in clips, "kind clip")
    for bucket in ("dark", "neutral", "bright"):
        got = set(paths(await search(ed, scope="project", filters={"brightness": bucket, "kind": "clip"}, limit=200)))
        for rel in clips:
            c = cached(ed, rel)["centroid_hz"]
            if c is None:
                continue
            want = "dark" if c < 800 else "bright" if c > 3000 else "neutral"
            ed.check((rel in got) == (want == bucket), f"{rel}: centroid {c} Hz is {want}, filter {bucket}: {sorted(got)}")
    ed.check_in(NOISE_8K, paths(await search(ed, scope="project", filters={"brightness": "bright"})), "noise is bright")

    long_ones = await search(ed, scope="project", filters={"min_ms": 1000, "kind": "clip"}, limit=200)
    ed.check_eq(mine(long_ones), [PAD], "min_ms 1000: the 1.5 s pad")
    short = await search(ed, scope="project", filters={"max_ms": 60, "kind": "clip"}, limit=200)
    ed.check(set(paths(short)) >= {CLICK, CLICK_1K1, NOISE_8K} and PAD not in paths(short), f"max_ms 60: {paths(short)}")
    tonal = paths(await search(ed, scope="project", filters={"tonal": True}, limit=200))
    for rel in clips:
        conf = cached(ed, rel)["pitch"]["confidence"] or 0.0
        ed.check((rel in tonal) == (conf >= 0.6), f"{rel}: pitch confidence {conf}, tonal results {tonal}")
    ed.check_eq(paths(await search(ed, scope="project", filters={"kind": "event"}, limit=50)), [], "kind event: no Sound Events here")

    ed.check(PAD not in paths(await search(ed, scope="project", filters={"loop": True}, limit=200)), "no loop yet")
    await ed.call("set_audio_import_settings", paths=[PAD], settings={"loop_start_ms": 100, "loop_end_ms": 1000})
    looped = await search(ed, scope="project", filters={"loop": True}, limit=200)
    ed.check_in(PAD, paths(looped), "a loop region makes it a loop")
    await ed.call("set_audio_import_settings", paths=[PAD], settings={"loop_start_ms": None, "loop_end_ms": None})


async def test_similar_to(ed: Editor):
    """similar_to the 1.1 kHz click ranks the 1 kHz step bursts above the impulse, the 8 kHz noise burst
    and the pad; similar_to the impulse (a flat spectrum, as noise has) ranks the noise burst above the
    1.1 kHz click and the pad; every distance equals the Python computation from the cached JSON within
    1e-6; the reference itself is left out; a second call analyses nothing."""
    await setup(ed)
    for reference, near, far in ((CLICK_1K1, STEPS, [CLICK, NOISE_8K, PAD]), (CLICK, [NOISE_8K], [CLICK_1K1, PAD])):
        got = await search(ed, scope="project", similar_to=reference, filters={"kind": "clip"}, limit=200)
        order = mine(got)
        print(f"[search] similar to {reference}: {[(r['path'], round(r['distance'], 4)) for r in got['results']]}")
        ed.check(reference not in order, "the reference is excluded")
        ed.check(max(order.index(p) for p in near) < min(order.index(p) for p in far), f"{near} nearer to {reference} than {far}: {order}")
        ref_cache = cached(ed, reference)
        for r in got["results"]:
            want = distance(cached(ed, r["path"]), ref_cache)
            ed.check(abs(r["distance"] - want) < 1e-6, f"{r['path']}: distance {r['distance']} against Python's {want}")
        ed.check(all(a["distance"] <= b["distance"] for a, b in zip(got["results"], got["results"][1:])), "ordered by distance")

    across = await search(ed, scope="essentials", similar_to=CLICK, limit=3)
    ed.check(all(p.startswith("essentials:") for p in paths(across)) and len(across["results"]) == 3,
             f"a project reference ranks the essentials: {paths(across)}")
    of_essential = await search(ed, scope="project", similar_to="essentials:footstep/concrete_1", filters={"kind": "clip"})
    ed.check(of_essential["results"] and all(r["source"] == "project" for r in of_essential["results"]), "an essential reference ranks the project")

    again = await search(ed, scope="all", query="step")
    ed.check_eq(again["analysed"], 0, "everything is cached by now")
    await ed.check_error("search_sounds", similar_to="assets/sounds/nothing.wav", contains="nothing.wav")
    await ed.check_error("search_sounds", similar_to="essentials:nope/none", contains="not an essential")


async def test_in_project_and_sha256(ed: Editor):
    """the manifest's sha256 is hashlib's for all 40; once all 40 are imported, in_project is true for
    each (the editor's SHA-256 of the copy equals the manifest's), and false for one whose bytes change
    (its size kept)."""
    await setup(ed)
    for clip in manifest()["clips"]:
        ed.check_eq(hashlib.sha256((ESSENTIALS / clip["path"]).read_bytes()).hexdigest(), clip["sha256"], f"{clip['id']}'s manifest hash")
    before = await search(ed, scope="essentials", limit=200)
    ed.check(not any(r["in_project"] for r in before["results"]), "nothing imported yet")
    await ed.call("import_files", sources=["essentials:*"], to_dir=IMPORTED)
    after = await search(ed, scope="essentials", limit=200)
    missing = [r["id"] for r in after["results"] if not r["in_project"]]
    ed.check_eq(missing, [], "every imported essential is in the project (SHA-256 of the copy equals the manifest's)")

    concrete = next(c for c in manifest()["clips"] if c["id"] == "footstep/concrete_1")
    copy = ed.project / IMPORTED / concrete["path"]
    data = bytearray(copy.read_bytes())
    data[len(data) // 2] ^= 0x55
    copy.write_bytes(bytes(data))
    changed = await search(ed, scope="essentials", query="concrete_1")
    ed.check_eq([(r["id"], r["in_project"]) for r in changed["results"]], [("essentials:footstep/concrete_1", False)],
                "a changed byte (same size) is not the essential")


async def test_schema(ed: Editor):
    """keys outside the schema are refused, at the top and inside filters; bad values name the key"""
    await setup(ed)
    await ed.check_error("search_sounds", query="step", colour="red")
    await ed.check_error("search_sounds", filters={"colour": "red"}, contains="filters")
    await ed.check_error("search_sounds", filters={"attack": "medium"}, contains="filters.attack")
    await ed.check_error("search_sounds", type="dragon", contains="type")
    await ed.check_error("search_sounds", scope="everywhere", contains="scope")
    await ed.check_error("search_sounds", limit=0, contains="limit")
    await ed.check_error("search_sounds", filters={"min_ms": 100, "max_ms": 50}, contains="max_ms")
    sheet = await ed.call_raw("search_sounds", {"query": "step", "scope": "project", "sheet": True})
    ed.check(not sheet.is_error and sheet.images, "sheet: a PNG of the results")
    ed.check_eq(sheet.structured["image"]["mode"], "sheet", "the sheet's info")


# -- M-2 ---------------------------------------------------------------------------------------------


async def test_never_network(ed: Editor):
    """SoundSearch and the search_sounds handler use no command, curl, socket or HTTP."""
    source = (REPO / "editor/audio/SoundSearch.cpp").read_text() + (REPO / "editor/audio/SoundSearch.h").read_text()
    tools = (REPO / "editor/mcp/tools/SoundTools.cpp").read_text()
    handler = tools[tools.index("// ------------------------------------------------------------------ search_sounds"):
                    tools.index("// ------------------------------------------------------------------ set_audio_import_settings")]
    for text, name in ((source, "SoundSearch"), (handler, "the search_sounds handler")):
        for word in ("CommandRunner", "curl", "socket", "http", "connect(", "system(", "popen"):
            ed.check(word.lower() not in text.lower(), f"{name} mentions {word}")


async def test_search_palette_lists_and_imports_a_builtin_clip(ed: Editor):
    """R24: the palette's Sounds: a built-in clip by its words; running it imports it; a project clip selects."""
    await setup(ed)
    found = await ed.call("search_editor", query="sound: concrete_1")
    keys = [r["key"] for r in found["results"]]
    ed.check_in("sound:essentials:footstep/concrete_1", keys, f"the palette lists an essential: {found}")
    await ed.call("run_command", key="sound:essentials:footstep/concrete_1")
    copy = ed.project / "sounds/essentials/footstep/concrete_1.ogg"
    ed.check(copy.exists(), "Enter imported it")
    ed.check(Path(str(copy) + ".meta").exists(), "with its .meta")
