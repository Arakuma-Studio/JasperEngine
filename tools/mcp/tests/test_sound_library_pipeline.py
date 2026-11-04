"""Sound layer, phase 5: the maintainer pipeline tools/sound-library/build_library.py (snd/p5-pipeline,
snd/p5-earshot).

Phase 5 spec §3.6 P-1..P-11, offline, on sources the test makes itself (§2.13): a Kenney-style zip
whose OGG members are synthetic signals with known faults (Phase 2.5's dropout, crackle and the
same dropout typed glitch among them), zips with unsafe members, and fake Freesound cache entries
(landing-page metadata parsed from test HTML shaped like Freesound's sound page, plus OGG previews:
beds, a noise bed whose loop starts mid-signal, a bed under a click field), all pinned by test
selections. No network is used; the pipeline runs with --offline, and with the editor's analyzer
CLI (`audio-analyze`, snd/p5-core M1) only in P-10 and P-11's editor half, which skip without it.

Independent oracles: the test's own CRC-32 (bitwise) and serial check for every Ogg page,
hashlib for files, its own BS.1770-4 K-weighting (the standard's 48 kHz coefficients) and window
loops for loudness, its own true-peak FIR plus an 8x resample_poly peak, its own wrap RMS check and
its own numpy of Phase 1's seam step for loops. Phase 2.5's defect notes come from its own oracle
(tools/mcp/audio_defects_ref.py), the module the pipeline imports by path. The DSP helpers run in a
uv subprocess with the pinned packages: this file, run as a script, is that helper ("gen" makes the
sources, "measure" decodes and measures OGG files, "defectcheck" runs the pipeline's defect filter
on signals no encode can produce).
"""

import asyncio
import atexit
import hashlib
import json
import os
import shutil
import subprocess
import sys
import tempfile
import urllib.error
import urllib.request
import zipfile
from pathlib import Path

HERE = Path(__file__).resolve().parent
REPO = HERE.parent.parent.parent
TOOLS = REPO / "tools/sound-library"
BUILD = TOOLS / "build_library.py"
ESSENTIALS = REPO / "editor/resources/sound-library/essentials"
PINS = ["numpy==2.5.4", "scipy==1.18.1", "soundfile==0.14.0", "pyloudnorm==0.2.0"]
TIMEOUT = 1800
CI_EXCLUDED = "the maintainers' pipeline: its full runs need the pinned download cache (JASPER_SOUND_CACHE), not a commit gate"


def fades(clip: dict):
    """the fades of a v4 clip's meta; None without any"""
    importer = clip["meta"]["import"]["audioImporter"]
    found = {k: importer[k] for k in ("fade_in_ms", "fade_out_ms") if k in importer}
    return found or None
STATE = {}

PACK = "interface-sounds"  # the essentials' pack id, so one member below is an essentials member
SR = 48000
# SoundTypes.h's targets as built (P1 §5.4 lowered click, explosion, hurt and shoot); judge checks them against the
# editor's audio-analyze --list-types
TARGETS = {"click": -32, "key": -26, "tick": -28, "tap": -22, "pop": -22, "thump": -16, "hit": -14, "boom": -14,
           "paper": -22, "swish": -20, "whoosh": -18, "transition": -16, "riser": -16, "reverse": -18, "glitch": -20,
           "ui": -22, "zap": -18, "chime": -20, "shimmer": -20, "footstep": -22, "impact": -18, "explosion": -18,
           "pickup": -18, "jump": -18, "hurt": -24, "shoot": -21, "door": -18, "stinger": -16, "ambience": -28,
           "music": -20, "voice": -20}
LOUDNESS = {"ambience": "integrated", "music": "integrated", "voice": "integrated"}
PREROLL = 128  # build_library.PREROLL_FRAMES

# (member, clip id, type, kind, expected verdict, reason substring for a rejection)
MEMBERS = [
    ("Audio/click_good.ogg", "click/kenney_click_good", "click", "transient", "accepted", None),
    ("Audio/footstep_boomy.ogg", "footstep/kenney_boomy", "footstep", "transient", "rejected", "boomy"),
    ("Audio/tick_hissy.ogg", "tick/kenney_hissy", "tick", "transient", "rejected", "hissy"),
    ("Audio/swish_good.ogg", "swish/kenney_swish_good", "swish", "swell", "accepted", None),
    ("Audio/swish_click.ogg", "swish/kenney_swish_click", "swish", "swell", "rejected", "clicks inside"),
    ("Audio/riser_good.ogg", "riser/kenney_riser_good", "riser", "riser", "accepted", None),
    ("Audio/riser_early.ogg", "riser/kenney_riser_early", "riser", "riser", "rejected", "peak too early"),
    ("Audio/click_antiphase.ogg", "click/kenney_antiphase", "click", "transient", "rejected", "mono downmix cancels"),
    ("Audio/impact_clipped.ogg", "impact/kenney_clipped", "impact", "transient", "rejected", "source is clipped"),
    ("Audio/thump_short.ogg", "thump/kenney_short", "thump", "transient", "rejected", "too short"),
    ("Audio/click_001.ogg", "click/kenney_click_001", "click", "transient", "rejected", "essentials member"),
    # decision 15 (P-11): Phase 2.5's notes on the decoded encoded file, gated by kind and type
    ("Audio/chime_drop3.ogg", "chime/kenney_drop3", "chime", "tonal", "rejected", "clicks inside (Phase 2.5"),
    ("Audio/glitch_drop3.ogg", "glitch/kenney_drop3", "glitch", "glitch", "accepted", None),
    ("Audio/chime_crackle.ogg", "chime/kenney_crackle", "chime", "tonal", "rejected", "crackle (Phase 2.5"),
    # P5 earshot M1.1 (§5.6.3): a cut at the window's maximum keeps room for the pre-roll; a tonal phrase keeps its
    # notes; a 20 ms tick and a sub-heavy thump fit their types; a word-shaped voice line is levelled by whole blocks
    ("Audio/glitch_long.ogg", "glitch/kenney_long", "glitch", "glitch", "accepted", None),
    ("Audio/ui_three_tones.ogg", "ui/kenney_three_tones", "ui", "tonal", "accepted", None),
    ("Audio/tick_20ms.ogg", "tick/kenney_short", "tick", "transient", "accepted", None),
    ("Audio/thump_sub.ogg", "thump/kenney_sub", "thump", "transient", "accepted", None),
    ("Audio/voice_word.ogg", "voice/kenney_word", "voice", "speech", "accepted", None),
]
# Freesound: (id, user, clip id, verdict, reason)
FREESOUND = [
    (1001, "tester", "ambience/fs_1001_bed", "accepted", None),
    (1002, "tester", "ambience/fs_1002_jump", "rejected", "loop seam"),
    (1003, "tester", "ambience/fs_1003_rain", "accepted", None),
    (1004, "tester", "ambience/fs_1004_noise", "accepted", None),
    (1005, "tester", "ambience/fs_1005_crackle", "accepted", None),
]
DEFECTS_ORACLE = REPO / "tools/mcp/audio_defects_ref.py"
# the windows of the types the sources use (§2.11.4, tick's minimum as changed in §5.6.3)
WINDOWS = {"click": (0.03, 0.20), "tick": (0.015, 0.15), "swish": (0.15, 0.70), "riser": (1.0, 6.0), "glitch": (0.06, 1.50),
           "ui": (0.04, 0.90), "thump": (0.12, 1.00), "voice": (0.20, 4.00), "chime": (0.25, 2.50), "ambience": (8.0, 24.0)}
CC0 = "http://creativecommons.org/publicdomain/zero/1.0/"


# ---------------------------------------------------------------------------------------------
# Helpers (test process: standard library only)

def uv():
    return shutil.which("uv") or str(Path.home() / ".local/bin/uv")


def uv_python(*args):
    cmd = [uv(), "run", "--quiet", "--no-project"]
    for p in PINS:
        cmd += ["--with", p]
    return cmd + ["python", *map(str, args)]


async def run(cmd, env=None):
    r = await asyncio.to_thread(subprocess.run, [str(c) for c in cmd], capture_output=True, text=True,
                                env=env, stdin=subprocess.DEVNULL, timeout=1200)
    return r.returncode, r.stdout + r.stderr


async def pipeline(*args, plain=False):
    """build_library.py through uv with the pins (plain: system python3, for the stdlib-only verify)."""
    cmd = ["python3", BUILD, *args] if plain else uv_python(BUILD, *args)
    return await run(cmd)


def import_build_library():
    if str(TOOLS) not in sys.path:
        sys.path.insert(0, str(TOOLS))
    import build_library
    return build_library


def landing_html(sid, user, licence=CC0, extra="", explicit=False, remix=False, page_id=None):
    blocker = '<div class="explicit-sound-blocker"><h4>Content warning</h4></div>' if explicit else ""
    return f"""<!doctype html><html><head>
<meta property="og:title" content="Test bed {sid} by {user}" />
<meta property="og:audio:artist" content="{user}" />
<meta property="og:audio:title" content="Test bed {sid}" />
<meta property="og:url" content="https://freesound.org/people/{user}/sounds/{sid}/" />
</head><body>{blocker}
<div class="bw-player" data-sound-id="{page_id or sid}" data-username="" data-remix-group="{'true' if remix else 'false'}"
    data-duration="16.0" data-samplerate="44100.0" tabindex="0"></div>
<h1><a class="bw-link--black" href="/people/{user}/sounds/{sid}/">Test bed {sid}</a></h1>
<div class="display-inline-block"><a href="/browse/tags/forest/" class="tag-container">forest</a></div>
<div class="col-2"><p class="text-grey no-margins">Type</p><p class="no-margins">Wave (.wav)</p></div>
<div class="col-2"><p class="text-grey no-margins">Channels</p><p class="no-margins">Stereo</p></div>
<span class="bw-icon-zero"></span> <a title="Go to the full license text" href="{licence}" target="_blank">Licence</a>
{extra}
</body></html>"""


def write_meta(cache, sid, html_text):
    bl = import_build_library()
    meta = bl.parse_landing(html_text)
    meta["page_sha256"] = hashlib.sha256(html_text.encode()).hexdigest()
    path = Path(cache) / "freesound" / f"{sid}.json"
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps({"fetched_utc": "2026-10-15T00:00:00Z", "page_url": f"https://freesound.org/s/{sid}/",
                                "meta": meta}, indent=1, sort_keys=True) + "\n")
    return meta


def make_zip(path, members, symlink=None, duplicate=None, stored=False):
    comp = zipfile.ZIP_STORED if stored else zipfile.ZIP_DEFLATED
    with zipfile.ZipFile(path, "w", comp) as z:
        z.writestr("License.txt", "\tLicense (Creative Commons Zero, CC0)\r\n\thttp://creativecommons.org/publicdomain/zero/1.0/\r\n")
        for name, data in members.items():
            z.writestr(zipfile.ZipInfo(name, (2026, 1, 1, 0, 0, 0)), data)
        if symlink:
            info = zipfile.ZipInfo(symlink, (2026, 1, 1, 0, 0, 0))
            info.external_attr = (0o120777 << 16)
            z.writestr(info, "../../etc/passwd")
        if duplicate:
            import warnings
            with warnings.catch_warnings():
                warnings.simplefilter("ignore")
                z.writestr(zipfile.ZipInfo(duplicate, (2026, 1, 1, 0, 0, 0)), members[duplicate])
    data = Path(path).read_bytes()
    return hashlib.sha256(data).hexdigest(), len(data)


def pack_entry(pid, sha, size):
    return {"id": pid, "title": "Test Pack", "url": f"https://kenney.nl/media/pages/assets/{pid}/test/kenney_{pid}.zip",
            "landing": f"https://kenney.nl/assets/{pid}", "sha256": sha, "bytes": size}


def selection(packs, sources, budget=None):
    return {"format": "jasperengine-sound-library-selection", "version": 1, "library_version": 4,
            "encoder": {"soundfile": "0.14.0", "libsndfile": "1.2.2", "vendor": "Xiph.Org libVorbis I 20200704 (Reducing Environment)"},
            "budget": budget or {"max_bytes": 1048576, "total": [1, 50], "min_per_type": {}},
            "targets": TARGETS, "packs": packs, "sources": sources, "rejected": []}


def write_json(path, obj):
    Path(path).write_text(json.dumps(obj, indent=1) + "\n")
    return Path(path)


def tree_bytes(root):
    root = Path(root)
    return {p.relative_to(root).as_posix(): p.read_bytes() for p in sorted(root.rglob("*")) if p.is_file()}


def ogg_crc_bitwise(data):
    crc = 0
    for b in data:
        crc ^= b << 24
        for _ in range(8):
            crc = ((crc << 1) ^ 0x04C11DB7) & 0xFFFFFFFF if crc & 0x80000000 else (crc << 1) & 0xFFFFFFFF
    return crc


def check_ogg(data, clip_id):
    """Every page: magic, the id-derived serial, a valid CRC; one logical stream, BOS first and EOS last."""
    serial = hashlib.sha256(clip_id.encode()).digest()[:4]
    pos, pages = 0, 0
    while pos < len(data):
        assert data[pos:pos + 4] == b"OggS", f"{clip_id}: no page at {pos}"
        nseg = data[pos + 26]
        end = pos + 27 + nseg + sum(data[pos + 27:pos + 27 + nseg])
        page = bytearray(data[pos:end])
        assert bytes(page[14:18]) == serial, f"{clip_id}: page {pages} serial {page[14:18].hex()}, expected {serial.hex()}"
        stored = int.from_bytes(page[22:26], "little")
        page[22:26] = b"\0\0\0\0"
        assert ogg_crc_bitwise(page) == stored, f"{clip_id}: page {pages} CRC is wrong"
        flags = data[pos + 5]
        assert bool(flags & 2) == (pages == 0), f"{clip_id}: begin-of-stream flag on page {pages}"
        last_flags = flags
        pos, pages = end, pages + 1
    assert last_flags & 4, f"{clip_id}: no end-of-stream flag on the last page"
    return pages


def audit_of(path):
    return {r["id"] if r["line"] == "clip" else r["source"]: r for r in map(json.loads, Path(path).read_text().splitlines())}


async def setup():
    """Sources, cache and the good selection, made once per test file."""
    if STATE:
        return STATE
    root = Path(tempfile.mkdtemp(prefix="snd-p5-pipeline-test-"))
    if not os.environ.get("JASPER_KEEP_SCRATCH"):
        atexit.register(shutil.rmtree, root, True)
    gen = root / "gen"
    code, out = await run(uv_python(__file__, "gen", gen))
    assert code == 0, out
    cache = root / "cache"
    (cache / "freesound").mkdir(parents=True)
    members = {m: (gen / Path(m).name).read_bytes() for m, *_ in MEMBERS}
    sha, size = make_zip(cache / f"kenney_{PACK}.zip", members)
    sources = [{"source": "kenney", "pack": PACK, "member": m,
                "clips": [{"id": cid, "type": t, "kind": k, "tags": ["test"], "description": f"Test {Path(m).stem}"}]}
               for m, cid, t, k, *_ in MEMBERS]
    for sid, user, cid, *_ in FREESOUND:
        data = (gen / f"bed_{sid}.ogg").read_bytes()
        (cache / "freesound" / f"{sid}-hq.ogg").write_bytes(data)
        write_meta(cache, sid, landing_html(sid, user))
        sources.append({"source": "freesound", "id": sid, "username": user,
                        "landing": f"https://freesound.org/people/{user}/sounds/{sid}/",
                        "preview": f"https://cdn.freesound.org/previews/{sid // 1000}/{sid}_42-hq.ogg",
                        "sha256": hashlib.sha256(data).hexdigest(), "bytes": len(data),
                        "clips": [{"id": cid, "type": "ambience", "kind": "loop", "tags": ["test", "bed"],
                                   "description": f"Test bed {sid}", "segment_s": [2.0, 10.0], "crossfade_ms": 500}]})
    sel = selection([pack_entry(PACK, sha, size)], sources)
    STATE.update(root=root, gen=gen, cache=cache, sel=sel, sel_path=write_json(root / "selection.json", sel))
    return STATE


async def run_all(name, sel_path=None):
    s = STATE
    d = s["root"] / name
    shutil.rmtree(d, ignore_errors=True)
    d.mkdir()
    code, out = await pipeline("all", "--selection", sel_path or s["sel_path"], "--cache", s["cache"], "--work", d / "work",
                               "--out", d / "library", "--audit", d / "audit.jsonl", "--offline")
    return d, code, out


# ---------------------------------------------------------------------------------------------
# P-1 Reproducible

async def test_p1_two_runs_identical_and_in_place_unchanged():
    s = await setup()
    a, code, out = await run_all("runA")
    assert code == 0, out
    assert "editor judge skipped" in out and "fetch: 0 network request(s)" in out, out
    b, code, out = await run_all("runB")
    assert code == 0, out
    ta, tb = tree_bytes(a / "library"), tree_bytes(b / "library")
    assert ta == tb, f"the two trees differ: {sorted(k for k in set(ta) | set(tb) if ta.get(k) != tb.get(k))}"
    assert (a / "audit.jsonl").read_bytes() == (b / "audit.jsonl").read_bytes()
    assert tree_bytes(a / "work") == tree_bytes(b / "work")
    s["runA"] = a
    # all again in place: no file changes (not even an mtime)
    before = {p: (p.stat().st_mtime_ns, p.read_bytes()) for p in (a / "library").rglob("*") if p.is_file()}
    code, out = await pipeline("all", "--selection", s["sel_path"], "--cache", s["cache"], "--work", a / "work",
                               "--out", a / "library", "--audit", a / "audit.jsonl", "--offline")
    assert code == 0, out
    after = {p: (p.stat().st_mtime_ns, p.read_bytes()) for p in (a / "library").rglob("*") if p.is_file()}
    assert before == after, "an in-place run changed the tree"
    # verify on plain Python (standard library only)
    code, out = await pipeline("verify", "--selection", s["sel_path"], "--out", a / "library", "--audit", a / "audit.jsonl",
                               "--allow-no-analysis", plain=True)
    assert code == 0 and "verify:" in out and ": ok" in out, out
    manifest = json.loads((a / "library/manifest.json").read_text())
    assert manifest["clips"], "nothing was accepted"
    for c in manifest["clips"]:
        data = (a / "library" / c["path"]).read_bytes()
        assert hashlib.sha256(data).hexdigest() == c["sha256"] and len(data) == c["bytes"]
        assert check_ogg(data, c["id"]) >= 3
    # A network namespace makes "offline" structural where the host allows it.
    if subprocess.run(["unshare", "-rn", "true"], capture_output=True).returncode == 0:
        code, out = await run(["unshare", "-rn", *uv_python(BUILD, "all", "--selection", s["sel_path"], "--cache", s["cache"],
                                                             "--work", s["root"] / "runC/work", "--out", s["root"] / "runC/library",
                                                             "--audit", s["root"] / "runC/audit.jsonl", "--offline")])
        assert code == 0, out
        assert tree_bytes(s["root"] / "runC/library") == ta


# ---------------------------------------------------------------------------------------------
# P-2 Processing, P-3 Filters

async def test_p2_processing_oracles():
    s = STATE
    if "runA" not in s:
        from harness import skip
        skip("needs P-1's run")
    lib = s["runA"] / "library"
    manifest = json.loads((lib / "manifest.json").read_text())
    clips = {c["id"]: c for c in manifest["clips"]}
    code, out = await run(uv_python(__file__, "measure", *[lib / c["path"] for c in manifest["clips"]]))
    assert code == 0, out
    measured = json.loads(out.strip().splitlines()[-1])
    for c in manifest["clips"]:
        m = measured[str(lib / c["path"])]
        kind, typ = c["kind"], c["type"]
        assert m["rate"] == SR, f"{c['id']}: {m['rate']} Hz"
        assert c["processing"]["resampled_from"] == 44100, c["id"]
        if kind == "loop":
            assert m["channels"] == 2, f"{c['id']}: a bed keeps 2 channels"
            assert m["seam_step_db"] <= 12.0, f"{c['id']}: seam step {m['seam_step_db']}"
            assert abs(m["wrap_rms_db"] - m["rms_db"]) <= 1.0, f"{c['id']}: wrap RMS {m['wrap_rms_db']} vs {m['rms_db']}"
            assert abs(c["processing"]["seam_shift"]) <= 96 and c["processing"]["segment"][0] == 96000, c["processing"]
        else:
            assert m["channels"] == 1, f"{c['id']}: one-shots are mono"
            assert m["head_ms"] <= 5.0 + c["processing"]["fade_in_ms"] + 0.05, f"{c['id']}: head {m['head_ms']} ms"
            assert c["processing"]["preroll"] == PREROLL and c["processing"]["postroll"] in (0, 1024), c["processing"]
            lo, hi = WINDOWS[typ]
            assert lo <= c["measured"]["duration_s"] <= hi, f"{c['id']}: {c['measured']['duration_s']} s outside {lo}-{hi}"
        level = m["integrated"] if LOUDNESS.get(typ) else m["momentary_max"]
        if c["processing"]["limited_by"] is None:
            assert abs(level - TARGETS[typ]) <= 0.2, f"{c['id']}: {level} LUFS against the {TARGETS[typ]} target"
        else:
            assert c["processing"]["limited_by"] == "true_peak" and level < TARGETS[typ] + 0.2, c["id"]
        assert m["true_peak_fir"] <= -1.0 + 1e-6, f"{c['id']}: FIR true peak {m['true_peak_fir']}"
        assert m["peak_8x"] <= -0.7, f"{c['id']}: 8x peak {m['peak_8x']}"
        # the manifest's measured block agrees with the independent measurement
        assert m["frames"] == c["measured"]["frames"] and m["channels"] == c["measured"]["channels"], c["id"]
        assert abs(m["true_peak_fir"] - c["measured"]["true_peak_dbtp"]) <= 0.02, c["id"]
        own = c["measured"]["integrated_lufs"] if LOUDNESS.get(typ) else c["measured"]["momentary_max_lufs"]
        assert abs(level - own) <= 0.1, f"{c['id']}: oracle {level} vs manifest {own}"
    assert any(c["kind"] == "loop" for c in clips.values()), "the bed was not accepted"


async def test_p3_filters_reject_with_reasons():
    s = STATE
    if "runA" not in s:
        from harness import skip
        skip("needs P-1's run")
    audit = audit_of(s["runA"] / "audit.jsonl")
    manifest_ids = {c["id"] for c in json.loads((s["runA"] / "library/manifest.json").read_text())["clips"]}
    expected = [(cid, verdict, reason) for _, cid, _, _, verdict, reason in MEMBERS] + \
               [(cid, verdict, reason) for _, _, cid, verdict, reason in FREESOUND]
    for cid, verdict, reason in expected:
        rec = audit[cid]
        assert rec["verdict"] == verdict, f"{cid}: {rec['verdict']} ({rec['reasons']}), expected {verdict}"
        assert (cid in manifest_ids) == (verdict == "accepted"), cid
        if reason:
            assert any(reason in r for r in rec["reasons"]), f"{cid}: {reason!r} not in {rec['reasons']}"
        else:
            assert rec["reasons"] == [], f"{cid}: {rec['reasons']}"
    # the seam search moves the wrap by at most 2 ms and never hides a real level step at the seam
    jump = audit["ambience/fs_1002_jump"]
    assert jump["measured"]["seam_level_db"] < -10 and abs(jump["processing"]["seam_shift"]) <= 96, jump["processing"]
    # 40 ms of head silence in the source was trimmed, not rejected
    assert audit["click/kenney_click_good"]["measured"]["head_ms"] <= 10.0
    # source lines carry the licence evidence
    fs = audit["freesound:1001"]
    assert fs["licence_url"] == CC0 and fs["explicit"] is False and fs["remix_group"] is False and fs["preview_format"] == "hq.ogg"
    assert len(fs["page_sha256"]) == 64
    k = audit[f"kenney:{PACK}:Audio/click_good.ogg"]
    assert k["licence_markers"] == ["Creative Commons Zero", "publicdomain/zero/1.0"] and len(k["licence_sha256"]) == 64
    # sorted by source, then clip id; keys sorted; no timestamps
    lines = (s["runA"] / "audit.jsonl").read_text().splitlines()
    keys = [(r["source"], 0 if r["line"] == "source" else 1, r.get("id", "")) for r in map(json.loads, lines)]
    assert keys == sorted(keys)
    assert all(list(json.loads(l)) == sorted(json.loads(l)) for l in lines)
    assert "fetched_utc" not in "".join(lines)


# ---------------------------------------------------------------------------------------------
# P-4 Pins and licence evidence

async def test_p4_pins_and_licence_evidence_fail():
    s = await setup()
    bed = next(src for src in s["sel"]["sources"] if src["source"] == "freesound" and src["id"] == 1001)
    pages = {
        "a CC-BY page": (landing_html(1001, "tester", licence="https://creativecommons.org/licenses/by/4.0/"), "not CC0"),
        "a second licence link": (landing_html(1001, "tester", extra='<a href="https://creativecommons.org/licenses/by-nc/3.0/">x</a>'),
                                  "also links another licence"),
        "an explicit page": (landing_html(1001, "tester", explicit=True), "marked explicit"),
        "a remix-group page": (landing_html(1001, "tester", remix=True), "remix group"),
        "another sound's page": (landing_html(1001, "tester", page_id=999), "the page is for sound 999"),
    }
    cases = []
    for name, (page, reason) in pages.items():
        cache = s["root"] / f"p4-cache-{len(cases)}"
        shutil.copytree(s["cache"], cache)
        write_meta(cache, 1001, page)
        cases.append((name, cache, s["sel_path"], reason))
    # a cached zip with one appended byte
    cache = s["root"] / "p4-cache-zip"
    shutil.copytree(s["cache"], cache)
    with open(cache / f"kenney_{PACK}.zip", "ab") as f:
        f.write(b"\0")
    cases.append(("a zip with an appended byte", cache, s["sel_path"], "size or SHA-256 differs"))
    # a preview whose SHA-256 differs from its pin
    sel = json.loads(json.dumps(s["sel"]))
    next(src for src in sel["sources"] if src.get("id") == 1001)["sha256"] = "0" * 64
    cases.append(("a preview off its pin", s["cache"], write_json(s["root"] / "p4-sel-preview.json", sel),
                  "cached preview's size or SHA-256 differs"))
    for name, cache, sel_path, reason in cases:
        work = s["root"] / "p4-work"
        shutil.rmtree(work, ignore_errors=True)
        code, out = await pipeline("prepare", "--selection", sel_path, "--cache", cache, "--work", work)
        assert code == 1 and reason in out, f"{name}: prepare exit {code}: {out[-600:]}"
        out_dir = s["root"] / "p4-out"
        code, out = await pipeline("all", "--selection", sel_path, "--cache", cache, "--work", work, "--out", out_dir,
                                   "--audit", s["root"] / "p4-audit.jsonl", "--offline")
        assert code == 1, f"{name}: all exit {code}"
        assert not out_dir.exists() and not (s["root"] / "p4-audit.jsonl").exists(), f"{name}: something was written"
    # the seed pin (§2.14 H4): an opus file whose SHA-256 or size differs is refused before anything in it is read
    opus = s["root"] / "p4-opus.json"
    opus.write_bytes(b'{"freesound": []}\n')
    pin = {"repo": "https://github.com/Bodila51/opus-sound-layer", "commit": "52ac266a" + "0" * 32, "path": "scripts/library.json",
           "sha256": hashlib.sha256(opus.read_bytes()).hexdigest(), "bytes": opus.stat().st_size}
    pinned = write_json(s["root"] / "p4-sel-seed.json", dict(json.loads(json.dumps(s["sel"])), seed=pin))
    for name, data in (("different bytes", b'{"freesound": [] }\n'), ("not even JSON, same size", b"not json at all!\n"[:opus.stat().st_size])):
        other = s["root"] / "p4-opus-other.json"
        other.write_bytes(data)
        seed_out = s["root"] / "p4-seed-out.json"
        code, out = await pipeline("seed", "--selection", pinned, "--opus", other, "--cache", s["root"] / "p4-seed-cache", "--seed-out", seed_out)
        assert code == 1 and "refusing to read it" in out and pin["sha256"] in out, f"{name}: {out[-600:]}"
        assert "not valid JSON" not in out and not seed_out.exists(), name
    code, out = await pipeline("seed", "--selection", pinned, "--opus", opus, "--cache", s["root"] / "p4-seed-cache", "--seed-out", seed_out)
    assert code == 0 and "matches the seed pin" in out and "0 network request(s)" in out, out
    assert json.loads(seed_out.read_text())["opus"] == {"sha256": pin["sha256"], "bytes": pin["bytes"], "pinned": True}
    # a malformed seed object fails the selection, in every command
    for bad, reason in (({"commit": "52ac266a"}, "seed.commit must be the full 40-digit commit SHA-1"),
                        ({"path": "../library.json"}, "seed.path must be a relative path"),
                        ({"extra": 1}, "seed must have exactly the keys"),
                        ({"sha256": "X" * 64}, "seed.sha256 must be 64 lowercase hex digits")):
        sel_path = write_json(s["root"] / "p4-sel-badseed.json", dict(json.loads(json.dumps(s["sel"])), seed=dict(pin, **bad)))
        code, out = await pipeline("verify", "--selection", sel_path, "--out", s["root"] / "p4-nothing", plain=True)
        assert code == 1 and reason in out, f"{bad}: {out[-600:]}"
        code, out = await pipeline("seed", "--selection", sel_path, "--opus", opus, "--cache", s["root"] / "p4-seed-cache",
                                   "--seed-out", s["root"] / "p4-seed-out2.json")
        assert code == 1 and reason in out and not (s["root"] / "p4-seed-out2.json").exists(), f"{bad}: {out[-600:]}"
    # fetch reports the licence problems themselves (offline, from the cached metadata)
    code, out = await pipeline("fetch", "--selection", s["sel_path"], "--cache", s["root"] / "p4-cache-0", "--offline")
    assert code == 1 and "freesound:1001" in out and "not CC0" in out and "move it to rejected" in out, out
    assert bed["preview"].endswith("-hq.ogg")


# ---------------------------------------------------------------------------------------------
# P-5 Archive safety

async def test_p5_unsafe_archives_refused():
    s = await setup()
    good = (s["gen"] / "click_good.ogg").read_bytes()
    big = b"OggS" + os.urandom(1100 * 1024)
    zips = {
        "dotdot": ({"Audio/a.ogg": good, "../evil.ogg": good}, {}, "Audio/a.ogg", "unsafe member path"),
        "symlink": ({"Audio/a.ogg": good}, {"symlink": "Audio/link.ogg"}, "Audio/a.ogg", "symlink member"),
        "oversize": ({"Audio/big.ogg": big}, {"stored": True}, "Audio/big.ogg", "over the 1048576-byte cap"),
        "twice": ({"Audio/a.ogg": good}, {"duplicate": "Audio/a.ogg"}, "Audio/a.ogg", "found 2 times"),
    }
    for pid, (members, opts, member, reason) in zips.items():
        cache = s["root"] / f"p5-cache-{pid}"
        cache.mkdir()
        sha, size = make_zip(cache / f"kenney_{pid}.zip", members, **opts)
        sel = selection([pack_entry(pid, sha, size)],
                        [{"source": "kenney", "pack": pid, "member": member,
                          "clips": [{"id": "click/kenney_a", "type": "click", "kind": "transient", "tags": [], "description": "Test click"}]}])
        code, out = await pipeline("prepare", "--selection", write_json(s["root"] / f"p5-{pid}.json", sel), "--cache", cache,
                                   "--work", s["root"] / f"p5-work-{pid}")
        assert code == 1 and reason in out, f"{pid}: exit {code}: {out[-600:]}"
        assert not list((s["root"] / f"p5-work-{pid}").rglob("*.ogg")), f"{pid}: a clip was prepared"


# ---------------------------------------------------------------------------------------------
# P-6 Budgets, P-7 Verify

async def test_p6_budget_failures_write_nothing():
    s = STATE
    if "runA" not in s:
        from harness import skip
        skip("needs P-1's run")
    a = s["runA"]
    for name, budget, reason in (
            ("bytes", {"max_bytes": 1000, "total": [1, 50], "min_per_type": {}}, "over budget.max_bytes"),
            ("minimum", {"max_bytes": 1048576, "total": [1, 50], "min_per_type": {"click": 5}}, "under its minimum"),
            ("count", {"max_bytes": 1048576, "total": [40, 50], "min_per_type": {}}, "outside budget.total")):
        sel = json.loads(json.dumps(s["sel"]))
        sel["budget"] = budget
        sel_path = write_json(s["root"] / f"p6-{name}.json", sel)
        fresh = s["root"] / f"p6-out-{name}"
        code, out = await pipeline("write", "--selection", sel_path, "--work", a / "work", "--out", fresh,
                                   "--audit", s["root"] / f"p6-audit-{name}.jsonl")
        assert code == 1 and "write refused, nothing was written" in out and reason in out, f"{name}: {out[-600:]}"
        assert not fresh.exists() and not (s["root"] / f"p6-audit-{name}.jsonl").exists(), name
        existing = s["root"] / f"p6-existing-{name}"
        shutil.copytree(a / "library", existing)
        before = tree_bytes(existing)
        code, out = await pipeline("write", "--selection", sel_path, "--work", a / "work", "--out", existing,
                                   "--audit", s["root"] / f"p6-audit-{name}.jsonl")
        assert code == 1 and tree_bytes(existing) == before, f"{name}: the existing tree changed"
    # removing a clip needs a library_version bump (§2.2.2); with the bump it is written and the file goes
    sel = json.loads(json.dumps(s["sel"]))
    sel["sources"] = [src for src in sel["sources"] if src.get("member") != "Audio/click_good.ogg"]
    existing = s["root"] / "p6-version"
    shutil.copytree(a / "library", existing)
    code, out = await pipeline("write", "--selection", write_json(s["root"] / "p6-version.json", sel), "--work", a / "work",
                               "--out", existing, "--audit", s["root"] / "p6-audit-version.jsonl")
    assert code == 1 and "library_version must go up" in out and (existing / "click/kenney_click_good.ogg").is_file(), out
    sel["library_version"] = 5
    code, out = await pipeline("write", "--selection", write_json(s["root"] / "p6-version.json", sel), "--work", a / "work",
                               "--out", existing, "--audit", s["root"] / "p6-audit-version.jsonl")
    assert code == 0 and "1 removed: click/kenney_click_good.ogg" in out, out
    assert json.loads((existing / "manifest.json").read_text())["version"] == 5
    assert not (existing / "click").exists(), "the emptied type folder stays"
    # write refuses a directory that is not a library
    stranger = s["root"] / "p6-stranger"
    stranger.mkdir()
    (stranger / "notes.txt").write_text("keep me")
    code, out = await pipeline("write", "--selection", s["sel_path"], "--work", a / "work", "--out", stranger,
                               "--audit", s["root"] / "p6-audit-stranger.jsonl")
    assert code == 1 and "refusing to write" in out and (stranger / "notes.txt").read_text() == "keep me", out


async def test_p7_verify_catches_tampering():
    s = STATE
    if "runA" not in s:
        from harness import skip
        skip("needs P-1's run")
    a = s["runA"]
    manifest = json.loads((a / "library/manifest.json").read_text())
    clip = manifest["clips"][0]

    async def verify(tree, sel_path=None, allow=True):
        args = ["verify", "--selection", sel_path or s["sel_path"], "--out", tree, "--audit", a / "audit.jsonl"]
        return await pipeline(*(args + (["--allow-no-analysis"] if allow else [])), plain=True)

    def copy(name):
        d = s["root"] / f"p7-{name}"
        shutil.rmtree(d, ignore_errors=True)
        shutil.copytree(a / "library", d)
        return d

    d = copy("flip")
    f = d / clip["path"]
    data = bytearray(f.read_bytes())
    data[len(data) // 2] ^= 0x01
    f.write_bytes(bytes(data))
    code, out = await verify(d)
    assert code == 1 and clip["path"] in out and "SHA-256" in out, out
    d = copy("extra")
    (d / "click/stray.ogg").write_bytes(b"OggS")
    code, out = await verify(d)
    assert code == 1 and "not in the manifest: click/stray.ogg" in out, out
    d = copy("licence")
    (d / f"licenses/{PACK}.txt").unlink()
    code, out = await verify(d)
    assert code == 1 and f"licenses/{PACK}.txt is missing" in out, out
    sel = json.loads(json.dumps(s["sel"]))
    for src in sel["sources"]:
        for c in src["clips"]:
            if c["id"] == clip["id"]:
                c["tags"] = ["changed"]
    code, out = await verify(a / "library", write_json(s["root"] / "p7-sel.json", sel))
    assert code == 1 and f"{clip['id']}: manifest tags" in out, out
    # analysis.json: a correct one passes without --allow-no-analysis; a missing id or a wrong sha1 fails
    d = copy("analysis")
    items = {c["id"]: {"sha1": hashlib.sha1((d / c["path"]).read_bytes()).hexdigest()} for c in manifest["clips"]}

    def analysis(it):
        (d / "analysis.json").write_text(json.dumps({"format": "jasperengine-sound-analysis", "version": 1, "analyzer_version": 1,
                                                    "generated_by": "test", "items": it}) + "\n")

    analysis(items)
    code, out = await verify(d, allow=False)
    assert code == 0, out
    analysis({k: v for k, v in items.items() if k != clip["id"]})
    code, out = await verify(d, allow=False)
    assert code == 1 and "does not name exactly the manifest's clips" in out, out
    analysis(dict(items, **{clip["id"]: {"sha1": "0" * 40}}))
    code, out = await verify(d, allow=False)
    assert code == 1 and f"{clip['id']}'s sha1 is not its file's" in out, out
    (d / "analysis.json").unlink()
    code, out = await verify(d, allow=False)
    assert code == 1 and "analysis.json is missing" in out, out
    # the import object: only a loop prepare gave one may carry it, exactly as prepare derived it (the audit)
    looped = next(c for c in manifest["clips"] if fades(c))
    one_shot = next(c for c in manifest["clips"] if c["kind"] != "loop")

    def tamper(name, edit):
        d = copy(name)
        m = json.loads((d / "manifest.json").read_text())
        for c in m["clips"]:
            edit(c)
        (d / "manifest.json").write_text(json.dumps(m, indent=1, ensure_ascii=False) + "\n")
        return d

    d = tamper("import-oneshot", lambda c: c["meta"]["import"]["audioImporter"].update({"fade_in_ms": 2.0}) if c["id"] == one_shot["id"] else None)
    code, out = await verify(d)
    assert code == 1 and f"{one_shot['id']}: fades" in out and "are not ones prepare derives" in out, out
    d = tamper("import-gone", lambda c: c["meta"]["import"]["audioImporter"].pop("fade_in_ms") if c["id"] == looped["id"] else None)
    code, out = await verify(d)
    assert code == 1 and f"{looped['id']}: manifest fades None are not the ones prepare derived" in out, out
    d = tamper("import-key", lambda c: c["meta"]["import"]["audioImporter"].update({"gain_db": 1.0}) if c["id"] == looped["id"] else None)
    code, out = await verify(d)
    assert code == 1 and "meta is not the .meta sections an import of it writes" in out, out


# ---------------------------------------------------------------------------------------------
# P-8 Network

async def test_p8_network_rules():
    s = await setup()
    bl = import_build_library()
    import soundlib

    def no_network(*a, **k):
        raise AssertionError("a request was attempted")

    real_opener = urllib.request.build_opener
    urllib.request.build_opener = no_network
    try:
        for url in ("http://freesound.org/x", "https://freesound.org.evil.example/x", "https://evil.example/x"):
            try:
                soundlib.download(url, 100, bl.FREESOUND_HOSTS, "test")
                raise AssertionError(f"{url} was not refused")
            except soundlib.BuildError as e:
                assert "refusing to fetch" in str(e)
    finally:
        urllib.request.build_opener = real_opener
    handler = soundlib.AllowListRedirects(bl.FREESOUND_HOSTS)
    req = urllib.request.Request("https://freesound.org/people/a/sounds/1/")
    for url in ("http://freesound.org/x", "https://freesound.org.evil.example/x", "https://evil.example/x"):
        try:
            handler.redirect_request(req, None, 302, "Found", {}, url)
            raise AssertionError(f"redirect to {url} was not refused")
        except soundlib.BuildError as e:
            assert "refusing redirect" in str(e)
    assert handler.redirect_request(req, None, 301, "Moved", {}, "https://cdn.freesound.org/previews/0/1_2-hq.ogg") is not None
    assert soundlib.AllowListRedirects((bl.KENNEY_HOST,)).redirect_request(
        urllib.request.Request("https://kenney.nl/a"), None, 302, "Found", {}, "https://kenney.nl/b") is not None

    # the polite fetcher: interval, Retry-After, backoff, no retry on 404, offline
    clock = [0.0]
    sleeps = []

    def sleep(t):
        sleeps.append(t)
        clock[0] += t

    answers = [urllib.error.HTTPError("u", 429, "Too Many", {"Retry-After": "7"}, None),
               urllib.error.HTTPError("u", 503, "Unavailable", {}, None), b"ok", b"ok2",
               urllib.error.HTTPError("u", 404, "Not Found", {}, None)]

    def fake(url, limit, hosts, ua, timeout):
        assert "JasperEngine" in ua
        a = answers.pop(0)
        if isinstance(a, Exception):
            raise a
        return a

    f = soundlib.PoliteFetcher(bl.FREESOUND_HOSTS, bl.USER_AGENT, min_interval=1.0, backoff=2.0, sleep=sleep,
                               clock=lambda: clock[0], opener=fake)
    assert f.get("https://freesound.org/a", 10) == b"ok"
    assert f.get("https://freesound.org/b", 10) == b"ok2"
    assert sleeps == [7.0, 4.0, 1.0], sleeps
    try:
        f.get("https://freesound.org/c", 10)
        raise AssertionError("404 was retried or swallowed")
    except urllib.error.HTTPError as e:
        assert e.code == 404 and f.requests == 5
    off = soundlib.PoliteFetcher(bl.FREESOUND_HOSTS, bl.USER_AGENT, offline=True, opener=no_network)
    try:
        off.get("https://freesound.org/a", 10)
        raise AssertionError("offline fetched")
    except soundlib.BuildError as e:
        assert "--offline" in str(e)

    # robots.txt is obeyed before the first page
    net = bl.Net(offline=False)
    net.freesound.get = lambda url, limit: b"User-agent: *\nDisallow: /people/\n" if url.endswith("robots.txt") else no_network()
    try:
        net.page("https://freesound.org/people/a/sounds/1/")
        raise AssertionError("robots.txt was not obeyed")
    except soundlib.BuildError as e:
        assert "robots.txt" in str(e)

    # --offline with a source missing from the cache fails before any request
    cache = s["root"] / "p8-cache"
    shutil.copytree(s["cache"], cache)
    (cache / "freesound/1002-hq.ogg").unlink()
    code, out = await pipeline("fetch", "--selection", s["sel_path"], "--cache", cache, "--offline")
    assert code == 1 and "--offline: refusing to fetch https://cdn.freesound.org/previews/1/1002_42-hq.ogg" in out, out
    assert "0 network request(s)" in out, out

    # the landing-page parser keeps only the fields it needs
    meta = bl.parse_landing(landing_html(1001, "tester"))
    assert meta["id"] == 1001 and meta["username"] == "tester" and meta["licence_links"] == [CC0]
    assert meta["landing"] == "https://freesound.org/people/tester/sounds/1001/" and meta["tags"] == ["forest"]
    assert meta["explicit"] is False and meta["remix_group"] is False and meta["channels"] == "Stereo"
    assert bl.freesound_problems(meta, 1001, "tester", "https://freesound.org/people/tester/sounds/1001/") == []


# ---------------------------------------------------------------------------------------------
# P-9 Essentials unchanged

async def test_p9_essentials_verify():
    code, out = await run(["python3", TOOLS / "build_essentials.py", "verify"])
    assert code == 0 and "verify: 40 clips (423628 bytes), 7 licences: ok" in out, out


async def test_p9_essentials_rebuild_in_place_unchanged():
    from harness import skip
    cache = os.environ.get("JASPER_SOUND_CACHE")
    if not cache or not all((Path(cache) / f"kenney_{p}.zip").is_file() for p in
                            ("interface-sounds", "ui-audio", "impact-sounds", "sci-fi-sounds", "digital-audio", "rpg-audio", "music-jingles")):
        skip("set JASPER_SOUND_CACHE to a directory holding the Phase 1 Kenney zips to rebuild the essentials")
    d = Path(tempfile.mkdtemp(prefix="snd-p5-essentials-")) / "essentials"
    shutil.copytree(ESSENTIALS, d)
    code, out = await run(uv_python(TOOLS / "build_essentials.py", "all", "--cache", cache, "--out", d))
    assert code == 0, out
    assert tree_bytes(d) == tree_bytes(ESSENTIALS), "build_essentials.py all changed the committed essentials"
    shutil.rmtree(d.parent)


# ---------------------------------------------------------------------------------------------
# P-10 Judge (the editor's analyzer)

async def test_p10a_judge_without_the_analyzer_skips_cleanly():
    s = STATE
    if "runA" not in s:
        from harness import skip
        skip("needs P-1's run")
    import harness
    work = s["root"] / "p10a-work"
    shutil.copytree(s["runA"] / "work", work)
    bl = import_build_library()
    editor = harness.editor_command()[0]
    info, why = bl.editor_types(editor)
    if info is not None:
        harness.skip(f"the editor has the analyzer ({' '.join(info['command'])}); test_p10_judge_with_the_editor covers it")
    code, out = await pipeline("judge", "--selection", s["sel_path"], "--work", work, "--editor", editor)
    assert code == 0 and "editor judge skipped" in out and "snd/p5-core M1" in out, out
    judge = json.loads((work / "judge.json").read_text())
    assert judge["editor"]["available"] is False and "no working analyze command" in judge["editor"]["reason"]
    code, out = await pipeline("analyse", "--selection", s["sel_path"], "--out", s["runA"] / "library", "--editor", editor)
    assert code == 1 and "analyse needs the editor" in out, out


async def test_p10_judge_with_the_editor():
    import harness
    s = STATE
    if "runA" not in s:
        harness.skip("needs P-1's run")
    bl = import_build_library()
    editor = harness.editor_command()[0]
    info, why = bl.editor_types(editor)
    if info is None:
        harness.skip(f"waits for snd/p5-core M1 (the editor's analyze CLI): {why}")
    work = s["root"] / "p10-work"
    shutil.rmtree(work, ignore_errors=True)
    shutil.copytree(s["runA"] / "work", work)
    manifest = json.loads((s["runA"] / "library/manifest.json").read_text())
    one_shots = [c for c in manifest["clips"] if c["kind"] != "loop"]
    dc, corrupt = one_shots[0], one_shots[1] if len(one_shots) > 1 else None
    code, out = await run(uv_python(__file__, "dcforce", work / dc["path"], dc["id"]))
    assert code == 0, out
    if corrupt:
        p = work / (corrupt["id"] + ".json")
        res = json.loads(p.read_text())
        res["measured"]["sample_peak_dbfs"] += 1.0
        p.write_text(json.dumps(res, indent=1, sort_keys=True) + "\n")
    code, out = await pipeline("judge", "--selection", s["sel_path"], "--work", work, "--editor", editor)
    assert code == 0, out
    judge = json.loads((work / "judge.json").read_text())
    assert judge["editor"]["available"] is True
    assert judge["clips"][dc["id"]]["verdict"] == "rejected" and "editor note dc_offset" in judge["clips"][dc["id"]]["reasons"], \
        judge["clips"][dc["id"]]
    if corrupt:
        reasons = judge["clips"][corrupt["id"]]["reasons"]
        assert any(r.startswith("analyzers disagree: sample peak") for r in reasons), reasons
    others = [c for c in manifest["clips"] if c["id"] not in (dc["id"], corrupt and corrupt["id"])]
    for c in others:
        assert judge["clips"][c["id"]]["verdict"] == "accepted", judge["clips"][c["id"]]


# ---------------------------------------------------------------------------------------------
# P-11 Defect filters (decision 15) and the seam metric

async def test_p11_defect_filters_and_the_seam_metric():
    s = STATE
    if "runA" not in s:
        from harness import skip
        skip("needs P-1's run")
    a = s["runA"]
    audit = audit_of(a / "audit.jsonl")
    manifest = {c["id"]: c for c in json.loads((a / "library/manifest.json").read_text())["clips"]}
    oracle = hashlib.sha256(DEFECTS_ORACLE.read_bytes()).hexdigest()
    for cid in [cid for _, cid, *_ in MEMBERS if audit[cid].get("defects")] + [cid for _, _, cid, *_ in FREESOUND]:
        rec = audit[cid]
        assert rec["defects"]["oracle_sha256"] == oracle, f"{cid}: {rec['defects'].get('oracle_sha256')}"
        assert set(rec["defects"]["verdicts"]) == {"edge_click", "clicks", "crackle", "silence_gap"}, cid
    # the dropout tone is rejected for its clicks; the same dropout typed glitch keeps it (Phase 2.5's gating): the
    # oracle still hears the click, the glitch kind takes no note
    drop = audit["chime/kenney_drop3"]
    assert drop["verdict"] == "rejected" and drop["defects"]["verdicts"]["clicks"], drop["defects"]
    assert any(r.startswith("clicks inside (Phase 2.5: ") for r in drop["reasons"]), drop["reasons"]
    glitch = audit["glitch/kenney_drop3"]
    assert glitch["verdict"] == "accepted" and glitch["reasons"] == [] and "glitch/kenney_drop3" in manifest, glitch["reasons"]
    assert glitch["defects"]["clicks"]["count"] >= 1 and not any(glitch["defects"]["verdicts"].values()), glitch["defects"]
    # a click field: crackle rejects a one-shot, and is information on a loop bed
    crackle = audit["chime/kenney_crackle"]
    assert crackle["verdict"] == "rejected" and crackle["defects"]["verdicts"]["crackle"], crackle["defects"]
    assert any(r.startswith("crackle (Phase 2.5: ") for r in crackle["reasons"]), crackle["reasons"]
    bed = audit["ambience/fs_1005_crackle"]
    assert bed["verdict"] == "accepted" and bed["reasons"] == [], bed["reasons"]
    assert bed["defects"]["verdicts"]["crackle"] and bed["defects"]["clicks"]["crackle"], bed["defects"]
    # a bed whose segment starts mid-signal clicks at its first frame: accepted with the loop fade-in, judged with it
    noise = audit["ambience/fs_1004_noise"]
    assert noise["verdict"] == "accepted" and noise["import"] == {"fade_in_ms": 2.0}, noise
    assert "edge_click" in noise["defects"]["notes_without_import"] and "edge_click" not in noise["defects"]["notes"], noise["defects"]
    assert fades(manifest["ambience/fs_1004_noise"]) == {"fade_in_ms": 2.0}
    assert list(manifest["ambience/fs_1004_noise"])[-1] == "meta", "meta goes after measured"
    # every loop with an import clicks at its start without it and not with it (the oracle, independently); no
    # one-shot has one
    lib = a / "library"
    items = [{"path": str(lib / c["path"]), "region": {}} for c in manifest.values()]
    items += [{"path": str(lib / c["path"]), "region": {"fade_in_frames": 96}} for c in manifest.values() if fades(c)]
    readings = await oracle_defects(items)
    for c in manifest.values():
        plain = readings[(str(lib / c["path"]), 0)]
        if fades(c):
            assert c["kind"] == "loop" and plain["edges"]["start_click"], c["id"]
            assert not readings[(str(lib / c["path"]), 96)]["edges"]["start_click"], c["id"]
        elif c["kind"] == "loop":
            assert not plain["edges"]["start_click"], f"{c['id']}: clicks at its start without an import"
        else:
            assert not plain["edges"]["start_click"] and not plain["edges"]["end_click"], f"{c['id']}: {plain['edges']}"
    # every accepted loop: measured.seam_step_db is Phase 1's metric (this file's own numpy) and at most +12; the
    # broadband bed needs no more than the +/-2 ms search
    code, out = await run(uv_python(__file__, "measure", *[lib / c["path"] for c in manifest.values() if c["kind"] == "loop"]))
    assert code == 0, out
    measured = json.loads(out.strip().splitlines()[-1])
    loops = [c for c in manifest.values() if c["kind"] == "loop"]
    assert {c["id"] for c in loops} == {"ambience/fs_1001_bed", "ambience/fs_1003_rain", "ambience/fs_1004_noise", "ambience/fs_1005_crackle"}
    for c in loops:
        mine = measured[str(lib / c["path"])]["seam_step_db"]
        assert abs(mine - c["measured"]["seam_step_db"]) <= 0.01, f"{c['id']}: {mine} here, {c['measured']['seam_step_db']} in the manifest"
        assert c["measured"]["seam_step_db"] <= 12.0 and abs(c["measured"]["seam_level_db"]) <= 3.0, c["measured"]
    rain = manifest["ambience/fs_1003_rain"]["processing"]
    assert rain["seam_shift"] == 0 or abs(rain["seam_shift"]) <= 96, rain
    # signals no encode produces (a decoder's digital-silence gap, an unfaded start), through the pipeline's own filter
    code, out = await run(uv_python(__file__, "defectcheck"))
    assert code == 0, out
    checks = json.loads(out.strip().splitlines()[-1])
    assert any(r.startswith("silence gap (Phase 2.5: 1 gap(s)") for r in checks["gap_tonal"]["reasons"]), checks["gap_tonal"]
    assert checks["gap_glitch"]["reasons"] == [] and checks["gap_glitch"]["gaps"] == 1, checks["gap_glitch"]
    assert any(r.startswith("edge click (Phase 2.5's edge rule: start") for r in checks["edge_tonal"]["reasons"]), checks["edge_tonal"]
    assert checks["edge_loop"] == {"import": {"fade_in_ms": 2.0}, "reasons": [], "gaps": 0}, checks["edge_loop"]


async def test_m11_windows_phrases_and_loudness():
    """§5.6.3 on the test's own sources: a cut at the window's maximum keeps room for the pre-roll, a tonal phrase keeps
    every note, and the integrated loudness of a voice line is BS.1770-4's (whole blocks), as P-2's oracle measures."""
    s = STATE
    if "runA" not in s:
        from harness import skip
        skip("needs P-1's run")
    manifest = {c["id"]: c for c in json.loads((s["runA"] / "library/manifest.json").read_text())["clips"]}
    glitch = manifest["glitch/kenney_long"]["measured"]["duration_s"]
    assert 1.49 <= glitch <= 1.5, f"the 2.2 s glitch is cut at its 1.5 s window, pre-roll included: {glitch} s"
    tones = manifest["ui/kenney_three_tones"]["measured"]["duration_s"]
    assert tones >= 0.33, f"the three-tone blip keeps its three notes (about 0.36 s), not its first: {tones} s"
    tick = manifest["tick/kenney_short"]["measured"]["duration_s"]
    assert 0.015 <= tick < 0.03, f"the tick is shorter than opus's old 30 ms minimum and fits the new 15 ms one: {tick} s"
    code, out = await run(uv_python(__file__, "measure", s["runA"] / "library" / manifest["voice/kenney_word"]["path"]))
    assert code == 0, out
    m = next(iter(json.loads(out.strip().splitlines()[-1]).values()))
    own = manifest["voice/kenney_word"]["measured"]["integrated_lufs"]
    assert abs(m["integrated"] - own) <= 0.02 and abs(m["integrated"] - TARGETS["voice"]) <= 0.15, (m["integrated"], own)


async def test_m11_curate_regressions_on_real_kenney_clips():
    """§5.6.3 on the Kenney clips snd/p5-curate's first run found (the Phase 1 zips and two Phase 5 packs in
    $JASPER_SOUND_CACHE): codec clicks at a clip's start and end, cuts past the window, a tonal phrase cut to its first
    note, a 21 ms tick, a sub-only thump, and an announcer line the two loudness meters disagreed on."""
    import harness
    cache = os.environ.get("JASPER_SOUND_CACHE")
    packs = {p["id"]: {k: p[k] for k in ("id", "title", "url", "landing", "sha256", "bytes")}
             for p in json.loads((TOOLS / "essentials.json").read_text())["packs"]}
    packs["voiceover-pack-fighter"] = {
        "id": "voiceover-pack-fighter", "title": "Voiceover Pack: Fighter", "landing": "https://kenney.nl/assets/voiceover-pack-fighter",
        "url": "https://kenney.nl/media/pages/assets/voiceover-pack-fighter/6ceb77c6f1-1677589837/kenney_voiceover-pack-fighter.zip",
        "sha256": "368ac0711e59521aff965a833e22e2b5218c397cf8b059ce3cb72e8d13219ee2", "bytes": 1265449}
    clips = [  # (pack, member, id, type, kind, events, event): curate's entries
        ("interface-sounds", "Audio/click_004.ogg", "click/kenney_click_004", "click", "transient", None, 1),
        ("interface-sounds", "Audio/minimize_007.ogg", "ui/kenney_minimize_007", "ui", "tonal", None, 1),
        ("interface-sounds", "Audio/minimize_009.ogg", "ui/kenney_minimize_009", "ui", "tonal", None, 1),
        ("digital-audio", "Audio/threeTone1.ogg", "ui/kenney_three_tone1", "ui", "tonal", None, 1),
        ("ui-audio", "Audio/switch21.ogg", "click/kenney_ui_switch_21", "click", "transient", None, 1),
        ("sci-fi-sounds", "Audio/computerNoise_001.ogg", "glitch/kenney_computer_noise_001", "glitch", "glitch", None, 1),
        ("music-jingles", "Audio/8-Bit jingles/jingles_NES06.ogg", "stinger/kenney_jingle_8bit_06", "stinger", "tonal", None, 1),
        ("interface-sounds", "Audio/scroll_003.ogg", "tick/kenney_scroll_003_e1", "tick", "transient", 3, 1),
        ("impact-sounds", "Audio/impactSoft_heavy_001.ogg", "thump/kenney_soft_heavy_001", "thump", "transient", None, 1),
        ("voiceover-pack-fighter", "Audio/fight.ogg", "voice/kenney_announcer_fight", "voice", "speech", None, None),
    ]
    used = sorted({c[0] for c in clips})
    if not cache or not all((Path(cache) / f"kenney_{p}.zip").is_file() for p in used):
        harness.skip(f"set JASPER_SOUND_CACHE to the pipeline's cache holding the zips of {', '.join(used)}")
    sources = []
    for pack, member, cid, typ, kind, events, event in clips:
        clip = {"id": cid, "type": typ, "kind": kind, "tags": ["test"], "description": "Curate's regression clip"}
        if event:
            clip["event"] = event
        src = {"source": "kenney", "pack": pack, "member": member, "clips": [clip]}
        if events:
            src["events"] = events
        sources.append(src)
    root = Path(tempfile.mkdtemp(prefix="snd-p5-m11-"))
    sel_path = write_json(root / "selection.json", selection([packs[p] for p in used], sources))
    code, out = await pipeline("prepare", "--selection", sel_path, "--cache", cache, "--work", root / "work")
    assert code == 0, out
    res = {c[2]: json.loads((root / "work" / f"{c[2]}.json").read_text()) for c in clips}

    def has(cid, text):
        return any(text in r for r in res[cid]["reasons"])

    for cid in res:
        # computerNoise_001's cut ends at glitch's window, with no room for the post-roll: test_c2 checks its edge
        if cid != "glitch/kenney_computer_noise_001":
            assert not has(cid, "edge click"), f"{cid}: {res[cid]['reasons']}"
    # minimize_009's decoded end still clicks without the post-roll in this build; with it the click is gone
    assert res["ui/kenney_minimize_009"]["processing"]["postroll"] == 1024, res["ui/kenney_minimize_009"]["processing"]
    assert res["click/kenney_click_004"]["processing"]["preroll"] == PREROLL
    for cid in ("click/kenney_ui_switch_21", "glitch/kenney_computer_noise_001", "stinger/kenney_jingle_8bit_06", "tick/kenney_scroll_003_e1"):
        assert not has(cid, "outside the") and not has(cid, "too short"), f"{cid}: {res[cid]['reasons']}"
        lo, hi = {"click": (0.03, 0.2), "glitch": (0.06, 1.5), "stinger": (0.3, 6.0), "tick": (0.015, 0.15)}[res[cid]["type"]]
        assert lo <= res[cid]["measured"]["duration_s"] <= hi, (cid, res[cid]["measured"]["duration_s"])
    assert res["ui/kenney_three_tone1"]["measured"]["duration_s"] >= 0.5, res["ui/kenney_three_tone1"]["measured"]
    assert res["tick/kenney_scroll_003_e1"]["verdict"] == "accepted", res["tick/kenney_scroll_003_e1"]["reasons"]
    assert res["thump/kenney_soft_heavy_001"]["verdict"] == "accepted", res["thump/kenney_soft_heavy_001"]["reasons"]
    # the announcer line: the Python meter now reads the editor's integrated loudness (when the editor is here)
    bl = import_build_library()
    editor = harness.editor_command()[0]
    info, why = bl.editor_types(editor)
    if info is None:
        harness.skip(f"the loudness agreement needs the editor's analyzer: {why}")
    code, out = await pipeline("judge", "--selection", sel_path, "--work", root / "work", "--editor", editor)
    assert code == 0, out
    judge = json.loads((root / "work/judge.json").read_text())["clips"]
    fight = judge.get("voice/kenney_announcer_fight")
    assert fight is not None and not any(r.startswith("analyzers disagree: loudness") for r in fight["reasons"]), fight
    py = res["voice/kenney_announcer_fight"]["measured"]["integrated_lufs"]
    assert abs(fight["item"]["integrated_lufs"] - py) <= 0.05, (fight["item"]["integrated_lufs"], py)
    shutil.rmtree(root)


async def test_c1_prepare_holds_d4_and_d8():
    """§5.3.6 (snd/p5-curate): prepare itself keeps what §3.1's data test checks. D-7's wrap level (§5.3.8): a loop
    wrapping in a lull between loud bursts is rejected, a steady bed is not. D-8's edge rule, ungated: a clip
    cut at a crest is rejected as glitch too (Phase 2.5's gating spares glitch, and the post-roll only runs when it
    fits the window). D-4's +0.5 LU: a clip whose meter answers each gain change by 30% of it is brought under the
    bound by the extra corrections, one that answers by 10% is rejected with the reason; and judge applies D-4's band
    to the editor's own reading."""
    code, out = await run(uv_python(__file__, "curategaps"))
    assert code == 0, out
    r = json.loads(out.strip().splitlines()[-1])
    assert r["abrupt_glitch"]["end_click"], r["abrupt_glitch"]  # the signal: its last sample clicks by the edge rule
    assert any(x.startswith("edge click") for x in r["abrupt_glitch"]["reasons"]), r["abrupt_glitch"]
    assert any(x.startswith("edge click") for x in r["abrupt_tonal"]["reasons"]), r["abrupt_tonal"]
    d3, d1 = r["damped_0.3"], r["damped_0.1"]
    assert d3["final"] is not None and d3["final"] <= 0.5 + 1e-9, d3
    assert d3["processing"].get("over_target_corrections", 0) >= 1, d3["processing"]
    assert not any("over its type's target" in x for x in d3["reasons"]), d3["reasons"]
    assert d1["verdict"] == "rejected" and d1["final"] > 0.5, d1
    assert any("over its type's target" in x for x in d1["reasons"]), d1["reasons"]
    lull, steady = r["wrap_in_lull"], r["wrap_steady"]
    assert lull["wrap"] is not None and lull["wrap"] < -1.0, lull  # the signal: its wrap sits in a lull
    assert any(x.startswith("loop wrap level") for x in lull["reasons"]), lull["reasons"]
    assert steady["wrap"] is not None and abs(steady["wrap"]) <= 1.0, steady
    assert not any(x.startswith("loop wrap level") for x in steady["reasons"]), steady["reasons"]
    bl = import_build_library()
    # §5.3.7: a transient hit rising over 40 ms is a soft attack for Python too (opus's rise_max 60 let it reach
    # the editor, whose 25 ms rule rejects it); 20 ms passes
    py = {"below150": 0.1, "above6k": 0.1, "flatness": 0.1, "rise_ms": 40.0, "start_db": -30.0, "end_db": -40.0,
          "spikiness": 1.5, "dur_s": 0.8, "build_db": -20.0, "peak_offset_s": 0.04}
    meas = {"sample_peak_dbfs": -3.0, "true_peak_dbtp": -2.9, "clipped": 0, "head_ms": 1.0, "duration_s": 0.8}
    assert any(x.startswith("soft attack (rise 40.0 ms > 25)") for x in bl.python_reasons(bl.TYPES["hit"], "transient", py, meas, -20.0))
    assert not any(x.startswith("soft attack") for x in bl.python_reasons(bl.TYPES["hit"], "transient", dict(py, rise_ms=20.0), meas, -20.0))
    target = TARGETS["click"]
    clip_res = {"kind": "transient", "type": "click", "defects": {"verdicts": {n: False for n in bl.DEFECT_NOTES}},
                "measured": {"channels": 1, "duration_s": 0.1, "sample_peak_dbfs": -10.0, "true_peak_dbtp": -9.9,
                             "momentary_max_lufs": target + 0.45, "head_ms": 1.0}}
    for editor_mm, over in ((target + 0.6, True), (target + 0.3, False), (target - 6.1, True)):
        clip_res["measured"]["momentary_max_lufs"] = editor_mm - 0.15  # the meters agree within 0.2 LU
        item = {"kind": "transient", "kind_source": "type", "notes": [], "format": {"duration_s": 0.1},
                "peak": {"sample_dbfs": -10.0, "true_peak_dbtp": -9.9}, "loudness": {"momentary_max": editor_mm},
                "silence": {"head_ms": 1.0}}
        reasons, _, _ = bl.editor_reasons(clip_res, item, target)
        assert any("D-4" in x for x in reasons) == over, (editor_mm, reasons)
        assert not any(x.startswith("analyzers disagree") for x in reasons), reasons


async def test_c2_curate_m1_clips_handled_by_prepare():
    """§5.3.6 on the two Kenney members §3.1 caught after the pipeline accepted them (from the pinned zips in
    $JASPER_SOUND_CACHE): ui-audio switch26 (+0.61 LU over click's target after the two corrections) is either brought
    within +0.5 or rejected for it, and sci-fi computerNoise_001 (its decoded end clicks, glitch kind, its cut at the
    window's end) is rejected for the edge or delivered without the click."""
    import harness
    cache = os.environ.get("JASPER_SOUND_CACHE")
    packs = {p["id"]: {k: p[k] for k in ("id", "title", "url", "landing", "sha256", "bytes")}
             for p in json.loads((TOOLS / "essentials.json").read_text())["packs"]}
    clips = [("ui-audio", "Audio/switch26.ogg", "click/kenney_ui_switch_26", "click", "transient"),
             ("sci-fi-sounds", "Audio/computerNoise_001.ogg", "glitch/kenney_computer_noise_001", "glitch", "glitch")]
    used = sorted({c[0] for c in clips})
    if not cache or not all((Path(cache) / f"kenney_{p}.zip").is_file() for p in used):
        harness.skip(f"set JASPER_SOUND_CACHE to the pipeline's cache holding the zips of {', '.join(used)}")
    sources = [{"source": "kenney", "pack": p, "member": m,
                "clips": [{"id": cid, "type": t, "kind": k, "tags": ["test"], "description": "Curate's M1 clip", "event": 1}]}
               for p, m, cid, t, k in clips]
    root = Path(tempfile.mkdtemp(prefix="snd-p5-c2-"))
    sel_path = write_json(root / "selection.json", selection([packs[p] for p in used], sources))
    code, out = await pipeline("prepare", "--selection", sel_path, "--cache", cache, "--work", root / "work")
    assert code == 0, out
    sw = json.loads((root / "work/click/kenney_ui_switch_26.json").read_text())
    final = sw["python"]["final_vs_target_lu"]
    over = [x for x in sw["reasons"] if "over its type's target" in x]
    assert (final <= 0.5 + 1e-9 and not over) or (sw["verdict"] == "rejected" and over), (final, sw["reasons"])
    cn = json.loads((root / "work/glitch/kenney_computer_noise_001.json").read_text())
    edges = (cn["defects"] or {}).get("edges") or {}
    edge = [x for x in cn["reasons"] if x.startswith("edge click")]
    assert (cn["verdict"] == "rejected" and edge) or not (edges.get("start_click") or edges.get("end_click")), (cn["reasons"], edges)
    shutil.rmtree(root)


# ---------------------------------------------------------------------------------------------
# P5 §5.7 (snd/p5-fix-data): the review's M-1 and L-2..L-8 in the pipeline

async def test_f1_tails_cut_later_or_released():
    """A one-shot whose decoded tail Phase 2.5's oracle calls truncated is repaired by prepare: a ping cut by the
    cut rule's -40 dB end while it still decays ends later, under -45 dB (tail_repair later_end); a reversed decay
    typed reverse, which stops loud with its source by design, gets a release (tail_repair release); neither carries
    truncated_tail by the oracle on the delivered file. The editor's truncated_tail note rejects a clip in judge."""
    code, out = await run(uv_python(__file__, "fixdata", "tails"))
    assert code == 0, out
    r = json.loads(out.strip().splitlines()[-1])
    ping, rev = r["ping"], r["reverse"]
    assert ping["cut_rule_end"] is not None and ping["cut_rule_end_notes"] == ["truncated_tail"], ping  # the signal shows it
    assert ping["processing"]["tail_repair"] == "later_end", ping["processing"]
    assert ping["processing"]["event"][2] > ping["cut_rule_end"], (ping["processing"]["event"], ping["cut_rule_end"])
    assert "truncated_tail" not in ping["notes"] and ping["verdict"] == "accepted", ping
    assert ping["tail"]["end_db"] < -45.0, ping["tail"]
    assert rev["processing"].get("tail_repair") in ("release", "cosine_release"), rev["processing"]
    assert rev["processing"]["fade_out_ms"] in (20.0, 50.0), rev["processing"]
    assert "truncated_tail" not in rev["notes"] and rev["verdict"] == "accepted", rev
    assert r["editor_reasons"] == ["editor note truncated_tail"], r["editor_reasons"]


async def test_f2_gain_is_the_highest_under_the_ceiling():
    """The review's L-5 and L-10: a true-peak-limited clip takes the highest gain on prepare's 0.05 dB grid whose
    decoded peak is under -1 dBTP: the same cut one step louder goes over (this file's BS.1770-4 FIR or a 16x
    resample_poly peak), the delivered file stays at or under -1.0 by both. A tone near Nyquist reads higher by 16x
    than by the 4x FIR; the pipeline holds the ceiling against the higher one and rejects a 16x reading over it."""
    code, out = await run(uv_python(__file__, "fixdata", "gain"))
    assert code == 0, out
    r = json.loads(out.strip().splitlines()[-1])
    for name, g in r["clips"].items():
        assert g["limited_by"] == "true_peak", (name, g)
        assert max(g["fir"], g["x16"]) <= -1.0 + 1e-9, (name, g)
        assert max(g["fir_up"], g["x16_up"]) > -1.0, (name, g)  # one grid step louder is over the ceiling
    f = r["fine"]
    assert f["x16"] > f["x4"] + 0.05 and abs(f["guard"] - f["x16"]) < 1e-9, f
    assert any("16x oversampling" in x for x in f["reasons"]), f["reasons"]


async def test_f3_near_duplicates_and_reuploads():
    """The review's L-4, through write: a Kenney member that is another member's audio (later, quieter) is left out
    as a near-duplicate of it, another take of the same action is kept; a Freesound upload by a second user of the
    first user's sound, its upper band changed, is left out as the same audio (provenance), and the original stays;
    the reasons reach the audit log, and dedupe prints the pairs."""
    root = Path(tempfile.mkdtemp(prefix="snd-p5-f3-"))
    gen = root / "gen"
    code, out = await run(uv_python(__file__, "gendup", gen))
    assert code == 0, out
    sims = json.loads(out.strip().splitlines()[-1])
    cache = root / "cache"
    (cache / "freesound").mkdir(parents=True)
    names = ("impact_a.ogg", "impact_copy.ogg", "impact_take.ogg")
    sha, size = make_zip(cache / "kenney_test-dups.zip", {f"Audio/{n}": (gen / n).read_bytes() for n in names})
    sources = [{"source": "kenney", "pack": "test-dups", "member": f"Audio/{n}",
                "clips": [{"id": f"impact/kenney_{Path(n).stem}", "type": "impact", "kind": "transient", "tags": ["test"],
                           "description": f"Test impact {Path(n).stem}"}]} for n in names]
    for sid, user in ((2001, "first"), (2002, "second")):
        data = (gen / f"whoosh_{sid}.ogg").read_bytes()
        (cache / "freesound" / f"{sid}-hq.ogg").write_bytes(data)
        write_meta(cache, sid, landing_html(sid, user))
        sources.append({"source": "freesound", "id": sid, "username": user, "landing": f"https://freesound.org/people/{user}/sounds/{sid}/",
                        "preview": f"https://cdn.freesound.org/previews/{sid // 1000}/{sid}_7-hq.ogg",
                        "sha256": hashlib.sha256(data).hexdigest(), "bytes": len(data),
                        "clips": [{"id": f"whoosh/fs_{sid}_whoosh", "type": "whoosh", "kind": "swell", "tags": ["test"],
                                   "description": f"Test whoosh {sid}"}]})
    sel_path = write_json(root / "selection.json", selection([pack_entry("test-dups", sha, size)], sources))
    code, out = await pipeline("all", "--selection", sel_path, "--cache", cache, "--work", root / "work", "--out", root / "library",
                               "--audit", root / "audit.jsonl", "--offline")
    assert code == 0, out
    kept = {c["id"] for c in json.loads((root / "library/manifest.json").read_text())["clips"]}
    assert kept == {"impact/kenney_impact_a", "impact/kenney_impact_take", "whoosh/fs_2001_whoosh"}, (kept, sims, out[-3000:])
    a = audit_of(root / "audit.jsonl")
    assert any(x.startswith("near-duplicate of impact/kenney_impact_a") for x in a["impact/kenney_impact_copy"]["reasons"]), a["impact/kenney_impact_copy"]
    assert any(x.startswith("the same audio as whoosh/fs_2001_whoosh by first") for x in a["whoosh/fs_2002_whoosh"]["reasons"]), a["whoosh/fs_2002_whoosh"]
    code, out = await pipeline("dedupe", "--selection", sel_path, "--work", root / "work")
    assert code == 0 and "2 rejected" in out and "near_duplicate" in out and "provenance" in out, out
    shutil.rmtree(root)


async def test_f4_loop_seams_take_the_lowest_step():
    """The review's L-8: of the segment-end candidates (+/-2 ms) the pipeline encodes, the one whose decoded seam step
    (this file's own Phase 1 seam metric) is lowest is kept, not the first that passes the +12 dB limit; a library
    loop's step is held to +8 dB."""
    code, out = await run(uv_python(__file__, "fixdata", "seams"))
    assert code == 0, out
    r = json.loads(out.strip().splitlines()[-1])
    assert r["tries"] >= 2 and r["tried_steps"], r
    assert abs(r["chosen_step"] - min(r["tried_steps"])) < 1e-6, r
    # the library holds loops to +8 dB, under the editor's +12 note
    bl = import_build_library()
    py = {"below150": 0.1, "above6k": 0.1, "flatness": 0.1, "rise_ms": 1.0, "start_db": -1.0, "end_db": -1.0, "spikiness": 1.0,
          "dur_s": 10.0, "build_db": 0.0, "peak_offset_s": 1.0, "lra_lu": 2.0}
    meas = {"sample_peak_dbfs": -10.0, "true_peak_dbtp": -9.9, "clipped": 0, "head_ms": 0.0, "duration_s": 10.0, "seam_level_db": 0.1}
    for step, over in ((9.0, True), (7.5, False)):
        why = bl.python_reasons(bl.TYPES["ambience"], "loop", py, dict(meas, seam_step_db=step), -20.0)
        assert any("loop seam step" in x for x in why) == over, (step, why)


async def test_f5_selection_names_the_sound():
    """The review's L-3: the selection refuses a tag that names a licence, a file format or a quality (cc0, free, wav,
    48khz, 24bit), an id that carries a file extension, and a description with no words (a page title like "28")."""
    bl = import_build_library()
    root = Path(tempfile.mkdtemp(prefix="snd-p5-f5-"))
    good = {"source": "freesound", "id": 14176, "username": "someone", "landing": "https://freesound.org/people/someone/sounds/14176/",
            "preview": "https://cdn.freesound.org/previews/14/14176_1-hq.ogg", "sha256": "0" * 64, "bytes": 10,
            "clips": [{"id": "swish/fs_14176_stick_swish", "type": "swish", "kind": "swell", "tags": ["stick", "swish"],
                       "description": "Swish of a thin stick through the air", "event": 1}]}
    bl.Selection(write_json(root / "good.json", selection([], [good])))
    cases = [("tags", ["swish", "cc0"], "name a licence"), ("tags", ["swish", "48khz"], "name a licence"),
             ("tags", ["wav", "swish"], "name a licence"), ("tags", ["24bit"], "name a licence"), ("tags", ["free"], "name a licence"),
             ("id", "swish/fs_14176_28_wav_3", "names a file format"), ("id", "swish/fs_14176_whoosh_ogg", "names a file format"),
             ("description", "28", "must name the sound in words"), ("description", "Whoosh 3.wav", "must name the sound in words")]
    for key, value, why in cases:
        src = json.loads(json.dumps(good))
        src["clips"][0][key] = value
        try:
            bl.Selection(write_json(root / "bad.json", selection([], [src])))
        except bl.BuildError as e:
            assert why in str(e), (key, value, str(e))
        else:
            raise AssertionError(f"the selection accepted {key} {value!r}")
    shutil.rmtree(root)


async def test_f6_landing_page_keeps_the_uploaders_text():
    """The review's L-3 and L-2 evidence: the landing page's soundDescriptionSection (the uploader's own text) is kept
    in the cached metadata as one line of text, nested markup and entities flattened; a page without it keeps None."""
    bl = import_build_library()
    page = landing_html(3001, "teller", extra='<div id="soundDescriptionSection"><p>Swishing a thin <b>hazel</b> stick &amp; '
                                              'a rope.</p><div class="x"><p>Recorded outdoors.</p></div><br>Mono.</div>'
                                              '<div id="other"><p>Not this.</p></div>')
    m = bl.parse_landing(page)
    assert m["description"] == "Swishing a thin hazel stick & a rope. Recorded outdoors. Mono.", m["description"]
    assert bl.parse_landing(landing_html(3002, "teller"))["description"] is None


async def oracle_defects(items):
    """Phase 2.5's oracle on files, through its own JSON interface: {(path, fade_in_frames): defects}."""
    r = await asyncio.to_thread(subprocess.run, uv_python(DEFECTS_ORACLE), input=json.dumps({"op": "defects", "items": items}),
                                capture_output=True, text=True, timeout=1200)
    assert r.returncode == 0, r.stderr[-2000:]
    return {(it["path"], it["region"].get("fade_in_frames", 0)): res["defects"] for it, res in zip(items, json.loads(r.stdout))}


async def test_p11_editor_judges_the_played_region_and_agrees():
    import harness
    s = STATE
    if "runA" not in s:
        harness.skip("needs P-1's run")
    bl = import_build_library()
    editor = harness.editor_command()[0]
    info, why = bl.editor_types(editor)
    if info is None:
        harness.skip(f"waits for snd/p5-core M1 (the editor's analyze CLI): {why}")
    work = s["root"] / "p11-work"
    shutil.rmtree(work, ignore_errors=True)
    shutil.copytree(s["runA"] / "work", work)
    # one forced disagreement on a defect verdict: the Python oracle "heard" an edge click the editor does not
    victim = "click/kenney_click_good"
    p = work / (victim + ".json")
    res = json.loads(p.read_text())
    res["defects"]["verdicts"]["edge_click"] = True
    p.write_text(json.dumps(res, indent=1, sort_keys=True) + "\n")
    code, out = await pipeline("judge", "--selection", s["sel_path"], "--work", work, "--editor", editor)
    assert code == 0, out
    judge = json.loads((work / "judge.json").read_text())
    assert any(r.startswith("analyzers disagree: edge_click present (Python)") or r.startswith("analyzers disagree: edge_click absent (editor)")
               for r in judge["clips"][victim]["reasons"]), judge["clips"][victim]
    # the mid-signal bed is judged on the region it plays (--fade-in-ms 2): no edge_click, the region in `played` (S36 R3)
    noise = judge["clips"]["ambience/fs_1004_noise"]
    assert noise["verdict"] == "accepted", noise
    assert "edge_click" not in noise["notes"] and noise["item"]["played"]["fade_in_frames"] == 96, noise["item"]
    # the crackle bed: the editor hears the crackle too (information), and the dropout glitch is judged as a glitch
    bed = judge["clips"]["ambience/fs_1005_crackle"]
    assert bed["verdict"] == "accepted" and bed["item"]["defects"]["crackle"], bed
    glitch = judge["clips"]["glitch/kenney_drop3"]
    assert glitch["verdict"] == "accepted" and glitch["item"]["kind"] == "glitch", glitch
    # the seam checks (D-7's, on the written library): the editor's own seam step of every accepted loop is at most +12
    # and its level at most 3 dB, with no loop_seam note, as is the manifest's (no tolerance between the two)
    lib = s["runA"] / "library"
    manifest = json.loads((lib / "manifest.json").read_text())
    loops = [c for c in manifest["clips"] if c["kind"] == "loop"]
    files = [str(lib / c["path"]) for c in loops]
    out = bl.run_editor_json(editor, [*info["command"], "--json", "--detail", "summary", "--type", "ambience", *files])
    items = {it["path"]: it for it in out["items"]}
    assert not out.get("errors") and set(items) == set(files), out.get("errors")
    for c, f in zip(loops, files):
        loop = items[f]["loop"]
        assert loop["seam_step_db"] <= 12.0 and abs(loop["seam_level_db"]) <= 3.0, f"{c['id']}: {loop}"
        assert "loop_seam" not in [n["id"] for n in items[f]["notes"]], f"{c['id']}: {items[f]['notes']}"
        assert c["measured"]["seam_step_db"] <= 12.0, c["measured"]
    # an input the editor cannot analyse (exit 2 under the audio commands' convention, its errors[] entry printed)
    # rejects that clip only; the rest of the run is judged
    broken = s["root"] / "p11-broken"
    shutil.rmtree(broken, ignore_errors=True)
    shutil.copytree(s["runA"] / "work", broken)
    (broken / "click/kenney_click_good.ogg").write_bytes(b"OggS" + b"\0" * 64)
    code, out = await pipeline("judge", "--selection", s["sel_path"], "--work", broken, "--editor", editor)
    assert code == 0, out
    judge = json.loads((broken / "judge.json").read_text())
    assert judge["clips"]["click/kenney_click_good"]["reasons"][0].startswith("editor could not analyse it: "), judge["clips"]["click/kenney_click_good"]
    assert judge["clips"]["swish/kenney_swish_good"]["verdict"] == "accepted", judge["clips"]["swish/kenney_swish_good"]


# ---------------------------------------------------------------------------------------------
# Script mode (run through uv with the pinned packages): sources and the independent measurements

def _gen(out):
    import numpy as np
    import soundfile as sf
    from scipy import signal

    out = Path(out)
    out.mkdir(parents=True, exist_ok=True)
    rng = np.random.default_rng(20261015)
    r = 44100

    def t(d):
        return np.arange(int(round(d * r))) / r

    def noise(d, lo, hi):
        sos = signal.butter(4, [lo, hi], "bandpass", fs=r, output="sos")
        x = signal.sosfilt(sos, rng.standard_normal(int(round(d * r))))
        return x / np.sqrt(np.mean(x * x))

    def sil(d):
        return np.zeros(int(round(d * r)))

    def save(name, y, stereo=True):
        y = np.asarray(y, dtype=np.float64)
        if y.ndim == 1 and stereo:
            y = np.stack([y, y], axis=1)
        sf.write(str(out / name), y, r, format="OGG", subtype="VORBIS")

    tt = t(0.12)
    click = 0.5 * np.sin(2 * np.pi * 2500 * tt) * np.exp(-tt / 0.012)
    save("click_good.ogg", np.concatenate([sil(0.04), click, sil(0.05)]))
    save("click_001.ogg", np.concatenate([sil(0.01), click, sil(0.05)]))
    save("click_antiphase.ogg", np.stack([np.concatenate([click, sil(0.05)]), -np.concatenate([click, sil(0.05)])], axis=1))
    tt = t(0.2)
    save("footstep_boomy.ogg", np.concatenate([sil(0.01), 0.6 * np.sin(2 * np.pi * 90 * tt) * np.exp(-tt / 0.03), sil(0.05)]))
    tt = t(0.06)
    save("tick_hissy.ogg", np.concatenate([sil(0.01), 0.3 * noise(0.06, 7000, 11000) * np.exp(-tt / 0.008), sil(0.05)]))
    d = 0.5
    tt = t(d)
    lap = np.exp(-np.abs(tt - d / 2) / 0.05)
    swell = 0.1 * noise(d, 300, 3000) * lap
    save("swish_good.ogg", np.concatenate([sil(0.05), swell, sil(0.05)]))
    spike = swell.copy()
    at = int(round((d / 2 + 0.03) * r))
    n = int(round(0.002 * r))
    spike[at:at + n] = 0.6 * np.sign(np.sin(2 * np.pi * 4000 * np.arange(n) / r) + 1e-9)
    save("swish_click.ogg", np.concatenate([sil(0.05), spike, sil(0.05)]))
    rise = 2.0
    tt = t(rise)
    env = 10 ** ((-30 + 30 * tt / rise) / 20)
    td = t(0.3)
    body = np.concatenate([env, np.exp(-td / 0.04)])
    save("riser_good.ogg", np.concatenate([sil(0.1), 0.3 * noise(rise + 0.3, 200, 3000) * body, sil(0.1)]))
    tt = t(2.0)
    save("riser_early.ogg", np.concatenate([sil(0.05), 0.3 * noise(2.0, 200, 3000) * np.exp(-tt / 0.5), sil(0.1)]))
    tt = t(0.5)
    save("impact_clipped.ogg", np.clip(4.0 * np.sin(2 * np.pi * 220 * tt), -1.0, 1.0))
    tt = t(0.03)
    save("thump_short.ogg", np.concatenate([sil(0.01), 0.6 * np.sin(2 * np.pi * 150 * tt) * np.exp(-tt / 0.005), sil(0.2)]))

    def bed(level_fn):
        tt = t(16.0)
        am = 1.0 + 0.2 * np.sin(2 * np.pi * 0.25 * tt)
        chans = []
        for ph in (0.0, 0.7):
            x = 0.12 * (np.sin(2 * np.pi * 110 * tt + ph) + np.sin(2 * np.pi * 165 * tt + 2 * ph) + 0.6 * np.sin(2 * np.pi * 220 * tt + 3 * ph))
            x = x * am + 0.02 * noise(16.0, 60, 400)
            chans.append(x * level_fn(tt))
        return np.stack(chans, axis=1)

    sf.write(str(out / "bed_1001.ogg"), bed(lambda tt: np.ones_like(tt)), r, format="OGG", subtype="VORBIS")
    # quiet up to the segment's end (2 s + 10 s), loud after: the crossfade's head is loud, its tail quiet
    sf.write(str(out / "bed_1002.ogg"), bed(lambda tt: np.where(tt < 12.0, 0.1, 1.0)), r, format="OGG", subtype="VORBIS")
    # a broadband bed (pink-ish noise, 60 Hz-8 kHz): continuous at any wrap, but most single wrap pairs step more
    # than Phase 1's -20 dB seam limit, so it needs the seam search
    w = rng.standard_normal((int(16 * r), 2))
    pink = signal.lfilter([0.049922035, -0.095993537, 0.050612699, -0.004408786], [1, -2.494956002, 2.017265875, -0.522189400], w, axis=0)
    pink = signal.sosfilt(signal.butter(4, [60, 8000], "bandpass", fs=r, output="sos"), pink, axis=0)
    sf.write(str(out / "bed_1003.ogg"), 0.1 * pink / np.sqrt(np.mean(pink * pink)), r, format="OGG", subtype="VORBIS")

    # Decision 15's sources (P-11), from their own generator so the ones above keep their bytes.
    rng2 = np.random.default_rng(20261021)
    tt = t(1.0)
    def decaying(freq):
        return 0.5 * np.sin(2 * np.pi * freq * tt) * np.exp(-tt / 0.25) * (1.0 - np.exp(-tt / 0.002))

    # 440 Hz: low enough that the click stands clear of the codec's high-frequency noise under the glitch type's
    # 16 kHz low-pass too (at 880 Hz the oracle no longer finds it there), so the glitch clip shows Phase 2.5's gating
    tone = decaying(880)
    dropped = decaying(440)
    at = int(round(0.12 * r))
    dropped[at:at + 3] = 0.0  # a 3-sample dropout while the tone is loud: "clicks inside"
    save("chime_drop3.ogg", np.concatenate([sil(0.01), dropped, sil(0.05)]))
    save("glitch_drop3.ogg", np.concatenate([sil(0.01), dropped, sil(0.05)]))  # the same, typed glitch: no note

    def click_field(n, count, lo, hi, amp):
        """single-sample clicks at irregular times in [lo, hi) seconds: Phase 2.5's crackle (8 or more,
        spread over the sound, intervals' coefficient of variation >= 0.5)"""
        out = np.zeros(n)
        for at in np.sort(rng2.uniform(lo, hi, count)):
            out[int(at * r)] += amp * rng2.choice([-1.0, 1.0])
        return out

    crackled = tone + click_field(len(tone), 14, 0.05, 0.9, 0.25)
    save("chime_crackle.ogg", np.concatenate([sil(0.01), crackled, sil(0.05)]))
    # a noise bed: the loop segment [2 s, 12 s) starts and wraps mid-signal, so its first frame clicks
    w = rng2.standard_normal((int(16 * r), 2))
    noise_bed = signal.sosfilt(signal.butter(4, [80, 6000], "bandpass", fs=r, output="sos"), w, axis=0)
    sf.write(str(out / "bed_1004.ogg"), 0.1 * noise_bed / np.sqrt(np.mean(noise_bed * noise_bed)), r, format="OGG", subtype="VORBIS")
    # a tonal bed under a click field (fire, rain): crackle, information for a loop; the clicks keep away from
    # the segment's wrap and crossfade ([2.0, 2.6) and [11.4, 12.6) s)
    field = click_field(int(16 * r), 20, 2.6, 11.4, 0.3)
    sf.write(str(out / "bed_1005.ogg"), bed(lambda tt: np.ones_like(tt)) + field[:, None], r, format="OGG", subtype="VORBIS")

    # M1.1's sources (§5.6.3), from their own generator
    rng3 = np.random.default_rng(20261022)

    def band(d, lo, hi):
        x = signal.sosfilt(signal.butter(4, [lo, hi], "bandpass", fs=r, output="sos"), rng3.standard_normal(int(round(d * r))))
        return x / np.sqrt(np.mean(x * x))

    def faded(x, ms):
        n = int(ms / 1000 * r)
        w = np.ones(len(x))
        w[:n], w[-n:] = np.linspace(0, 1, n), np.linspace(1, 0, n)
        return x * w

    save("glitch_long.ogg", np.concatenate([sil(0.01), faded(0.1 * band(2.2, 300, 3000), 5), sil(0.05)]))
    # three notes with silence between them (a jingle): the cut keeps the phrase, not its first note
    notes = [faded(0.4 * np.sin(2 * np.pi * f * t(0.1)), 5) for f in (660, 880, 1320)]
    save("ui_three_tones.ogg", np.concatenate([sil(0.01), notes[0], sil(0.03), notes[1], sil(0.03), notes[2], sil(0.05)]))
    tt = t(0.012)
    save("tick_20ms.ogg", np.concatenate([sil(0.01), 0.5 * np.sin(2 * np.pi * 2500 * tt) * np.exp(-tt / 0.0015), sil(0.05)]))
    tt = t(0.4)
    save("thump_sub.ogg", np.concatenate([sil(0.01), 0.7 * np.cos(2 * np.pi * 55 * tt) * np.exp(-tt / 0.08), sil(0.05)]))
    # a word: 50 ms attack, held to 0.6 s, released by 0.97 s; pyloudnorm 0.2.0 reads it 0.6 LU under BS.1770-4
    tt = t(0.97)
    env = np.clip(tt / 0.05, 0, 1) * np.where(tt < 0.6, 1.0, np.exp(-(tt - 0.6) / 0.075))
    word = 0.3 * env * (np.sin(2 * np.pi * 180 * tt) + 0.5 * np.sin(2 * np.pi * 360 * tt + 1) + 0.3 * np.sin(2 * np.pi * 900 * tt))
    save("voice_word.ogg", word)


K_STAGE1 = ([1.53512485958697, -2.69169618940638, 1.19839281085285], [1.0, -1.69065929318241, 0.73248077421585])
K_STAGE2 = ([1.0, -2.0, 1.0], [1.0, -1.99004745483398, 0.99007225036621])
FIR = [[0.0017089843750, 0.0109863281250, -0.0196533203125, 0.0332031250000, -0.0594482421875, 0.1373291015625,
        0.9721679687500, -0.1022949218750, 0.0476074218750, -0.0266113281250, 0.0148925781250, -0.0083007812500],
       [-0.0291748046875, 0.0292968750000, -0.0517578125000, 0.0891113281250, -0.1665039062500, 0.4650878906250,
        0.7797851562500, -0.2003173828125, 0.1015625000000, -0.0582275390625, 0.0330810546875, -0.0189208984375]]
FIR += [list(reversed(FIR[1])), list(reversed(FIR[0]))]


def _measure(files):
    import numpy as np
    import soundfile as sf
    from scipy import signal

    def db(v):
        return -120.0 if v <= 0 else 20 * np.log10(v)

    res = {}
    for f in files:
        y, rate = sf.read(f, dtype="float64", always_2d=True)
        frames, ch = y.shape
        z = signal.lfilter(*K_STAGE2, signal.lfilter(*K_STAGE1, y, axis=0), axis=0)
        pad = int(0.4 * rate)
        zp = np.vstack([np.zeros((pad, ch)), z, np.zeros((pad, ch))])
        c = np.vstack([np.zeros((1, ch)), np.cumsum(zp * zp, axis=0)])
        w, hop = int(0.4 * rate), int(0.01 * rate)
        starts = np.arange(0, len(zp) - w + 1, hop)
        ms = (c[starts + w] - c[starts]).sum(axis=1) / w
        mm = float(-0.691 + 10 * np.log10(ms.max()))
        # integrated: 400 ms blocks every 100 ms inside the signal, gates -70 and -10
        cz = np.vstack([np.zeros((1, ch)), np.cumsum(z * z, axis=0)])
        bs = np.arange(0, frames - w + 1, int(0.1 * rate))
        bl = (cz[bs + w] - cz[bs]).sum(axis=1) / w
        lk = -0.691 + 10 * np.log10(np.maximum(bl, 1e-30))
        g = bl[lk > -70]
        integ = None
        if len(g):
            rel = -0.691 + 10 * np.log10(g.mean()) - 10
            g2 = bl[(lk > -70) & (lk > rel)]
            integ = float(-0.691 + 10 * np.log10(g2.mean()))
        tp = float(np.abs(y).max())
        for k in range(ch):
            xp = np.concatenate([np.zeros(12), y[:, k], np.zeros(12)])
            for h in FIR:
                tp = max(tp, float(np.abs(np.convolve(xp, h)).max()))
        up = signal.resample_poly(y, 8, 1, axis=0)
        loud = np.nonzero(np.abs(y).max(axis=1) >= 10 ** (-60 / 20))[0]
        n10 = int(0.01 * rate)
        wrap = np.vstack([y[-n10:], y[:n10]])
        res[f] = {"rate": rate, "channels": ch, "frames": frames, "momentary_max": mm, "integrated": integ,
                  "true_peak_fir": db(tp), "peak_8x": db(float(np.abs(up).max())),
                  "head_ms": float(loud[0] / rate * 1000) if len(loud) else None,
                  "rms_db": db(float(np.sqrt(np.mean(y * y)))), "wrap_rms_db": db(float(np.sqrt(np.mean(wrap * wrap)))),
                  "seam_step_db": seam_step_db(y, rate)}
    print(json.dumps(res))


def seam_step_db(y, rate):
    """Phase 1 §2.7.2's seam step as the editor builds it (A28), written here from the spec's text: per channel
    |x[0] - x[N-1]| over the RMS of the W-1 first differences in each of the last and first W = round(10 ms)
    samples (every difference when N < 2W), floored at 2^-15; the largest channel, floored at -120 dB."""
    import numpy as np
    n = len(y)
    w = int(np.floor(0.010 * rate + 0.5))
    steps = [-120.0]
    for ch in range(y.shape[1]):
        x = y[:, ch]
        diffs = np.diff(x) if n < 2 * w else np.concatenate([np.diff(x[n - w:]), np.diff(x[:w])])
        ref = max(2.0 ** -15, float(np.sqrt(np.mean(diffs * diffs))))
        jump = abs(float(x[0]) - float(x[n - 1]))
        if jump > 0:
            steps.append(20 * np.log10(jump / ref))
    return float(max(steps))


def _dcforce(path, clip_id):
    """Re-encode a work clip with a DC offset of 0.05, keeping its id-derived serial."""
    import numpy as np
    import soundfile as sf
    sys.path.insert(0, str(TOOLS))
    import build_library as bl
    bl._dsp()
    y, rate = sf.read(path, dtype="float64", always_2d=True)
    data = bl.fix_ogg(bl.encode_ogg(np.clip(y * 0.5 + 0.05, -0.9, 0.9), 0.6), clip_id)
    Path(path).write_bytes(data)


def _defectcheck():
    """build_library.defect_check on decoded-like signals no Vorbis encode gives: a tone with 5 ms of digital
    silence inside (as tonal, then as glitch), a tone that starts at its peak, and a bed that starts mid-signal."""
    import numpy as np
    sys.path.insert(0, str(TOOLS))
    import build_library as bl
    bl._dsp()
    t = np.arange(SR) / SR
    tone = 0.5 * np.sin(2 * np.pi * 440 * t) * np.exp(-t / 0.3)
    fade = np.minimum(1.0, np.arange(SR) / 240.0) * np.minimum(1.0, (SR - 1 - np.arange(SR)) / 960.0)
    gap = tone * fade
    at = int(0.2 * SR) + 7  # away from a zero crossing: both edges cut
    gap[at:at + 240] = 0.0
    loud_start = 0.5 * np.cos(2 * np.pi * 440 * t) * np.exp(-t / 0.3)  # its first sample is the peak
    bed = np.stack([0.2 * np.sin(2 * np.pi * 220 * t[:SR // 2] + 1.0)] * 2, axis=1)
    out = {}
    for name, y, kind, typ in (("gap_tonal", gap, "tonal", "chime"), ("gap_glitch", gap, "glitch", "glitch"),
                               ("edge_tonal", loud_start, "tonal", "chime"), ("edge_loop", bed, "loop", "ambience")):
        imp, rec, reasons = bl.defect_check(y, kind, typ)
        out[name] = {"import": imp, "reasons": reasons, "gaps": rec.get("gaps", {}).get("count")}
    print(json.dumps(out))


def _curategaps():
    """snd/p5-curate's two pipeline gaps (§5.3.6) on signals made here, through build_library's own code:
    defect_check on a tone cut at its crest (as glitch, which Phase 2.5's gating spares, and as tonal), and
    prepare_clip on a ping whose momentary meter is replaced by a damped one (each gain change moves the reading by
    only a share of itself), so the gain loop's corrections cannot reach the target in their usual two steps."""
    import io
    import types
    import numpy as np
    import soundfile
    sys.path.insert(0, str(TOOLS))
    import build_library as bl
    bl._dsp()
    out = {}
    n = int(0.3 * SR)
    k = np.arange(n)
    crest_end = 0.5 * np.cos(2 * np.pi * 480 * k / SR) * np.minimum(1.0, k / 240.0)  # 480 Hz: its last sample is near a crest
    for name, kind, typ in (("abrupt_glitch", "glitch", "glitch"), ("abrupt_tonal", "tonal", "chime")):
        _, rec, reasons = bl.defect_check(crest_end, kind, typ)
        out[name] = {"reasons": reasons, "end_click": rec["edges"]["end_click"]}
    t = np.arange(int(0.25 * SR)) / SR
    ping = np.concatenate([np.zeros(2400), 0.1 * np.sin(2 * np.pi * 1000 * t) * np.exp(-t / 0.02), np.zeros(2400)])
    buf = io.BytesIO()
    soundfile.write(buf, ping, SR, format="OGG", subtype="VORBIS")
    audio = bl.SourceAudio(buf.getvalue(), lossy=True)
    target = TARGETS["click"]
    sel = types.SimpleNamespace(targets=TARGETS)
    src = {"source": "kenney", "pack": "test", "member": "Audio/ping.ogg"}
    real = bl.momentary_max
    for name, response in (("damped_0.3", 0.3), ("damped_0.1", 0.1)):
        state = {}

        def damped(y, response=response, state=state):
            # 4 LU over the target at the clip's own level; a gain change of G dB moves it by response x G
            v = real(y)
            if v is None:
                return None
            base = state.setdefault("base", v)
            return target + 4.0 + response * (v - base)

        bl.momentary_max = damped
        try:
            clip = {"id": f"click/kenney_{name.replace('.', '')}", "type": "click", "kind": "transient", "tags": [], "description": "x",
                    "event": 1}
            res, _ = bl.prepare_clip(clip, src, audio, sel, 1)
        finally:
            bl.momentary_max = real
        out[name] = {"verdict": res["verdict"], "reasons": res["reasons"],
                     "final": (res["python"] or {}).get("final_vs_target_lu"), "processing": res["processing"]}
    # §3.1 D-7: a bed whose loop wraps in a lull between loud bursts, and a steady bed (both stereo, 30 s)
    rng = np.random.default_rng(11)
    n = 30 * SR
    floor = 10 ** (-50 / 20) * rng.standard_normal((n, 2))
    bursts = floor.copy()
    for k in range(30):  # 0.3 s of noise at -20 dBFS starting every second at k + 0.2 s
        a = int((k + 0.2) * SR)
        bursts[a:a + int(0.3 * SR)] += 10 ** (-20 / 20) * rng.standard_normal((int(0.3 * SR), 2))
    steady = 10 ** (-30 / 20) * rng.standard_normal((n, 2))
    for name, bed, seg in (("wrap_in_lull", bursts, [1.0, 10.0]), ("wrap_steady", steady, [1.0, 10.0])):
        buf = io.BytesIO()
        soundfile.write(buf, np.clip(bed, -0.99, 0.99), SR, format="OGG", subtype="VORBIS")
        audio = bl.SourceAudio(buf.getvalue(), lossy=True)
        clip = {"id": f"ambience/fs_1_{name}", "type": "ambience", "kind": "loop", "tags": [], "description": "x", "segment_s": seg}
        res, _ = bl.prepare_clip(clip, {"source": "freesound", "id": 1}, audio, sel, 1)
        out[name] = {"reasons": res["reasons"], "wrap": (res["python"] or {}).get("wrap_rms_db")}
    print(json.dumps(out))


def _fixdata(part):
    """P5 §5.7's pipeline changes on signals made here, through build_library's own code."""
    import io
    import types
    import numpy as np
    import soundfile
    from scipy import signal as sg
    sys.path.insert(0, str(TOOLS))
    import build_library as bl
    bl._dsp()
    ref = bl.defects_oracle()
    sel = types.SimpleNamespace(targets=TARGETS)
    out = {}

    def source(y):
        buf = io.BytesIO()
        soundfile.write(buf, y, SR, format="WAV", subtype="FLOAT")
        return bl.SourceAudio(buf.getvalue(), lossy=False)

    def oracle(ogg, kind, typ):
        y, _ = soundfile.read(io.BytesIO(ogg), dtype="float32", always_2d=True)
        d = ref.defects(np.ascontiguousarray(y.T), float(SR), {})
        return ref.note_ids(d, kind, typ), d["tail"]

    def fir_peak(y):
        y = y if y.ndim == 2 else y[:, None]
        tp = float(np.abs(y).max())
        for k in range(y.shape[1]):
            xp = np.concatenate([np.zeros(12), y[:, k], np.zeros(12)])
            for h in FIR:
                tp = max(tp, float(np.abs(np.convolve(xp, h)).max()))
        return 20 * np.log10(tp)

    def x16_peak(y):
        y = y if y.ndim == 2 else y[:, None]
        return 20 * np.log10(float(np.abs(sg.resample_poly(np.vstack([np.zeros((64, y.shape[1])), y, np.zeros((64, y.shape[1]))]),
                                                           16, 1, axis=0)).max()))

    if part == "tails":
        t = np.arange(int(1.5 * SR)) / SR
        ping = np.concatenate([np.zeros(480), 0.5 * np.sin(2 * np.pi * 1000 * t) * np.exp(-t / 0.06), np.zeros(9600)])
        audio = source(ping)
        clip = {"id": "impact/kenney_ping", "type": "impact", "kind": "transient", "tags": [], "description": "x", "event": 1}
        src = {"source": "kenney", "pack": "test", "member": "Audio/ping.ogg"}
        ev = audio.events(0, len(audio.mono), dict(bl.TYPES["impact"], win=(0.05, 2.0 - bl.PREROLL_FRAMES / SR)), "transient", 1)
        # the merged pipeline's render of the cut rule's end: band filter, the 5 ms fade, the gain loop, the oracle
        y, _ = bl.band_filter(audio.mono[ev[0][0]:ev[0][1]].copy(), bl.TYPES["impact"])
        fo, _ = bl.fades(y, bl.TYPES["impact"])
        first, _ = bl.gain_loop(y, TARGETS["impact"], bl.momentary_max, fo, True, 0.6, clip["id"])
        first_notes, _ = oracle(first["ogg"], "transient", "impact")
        res, ogg = bl.prepare_clip(clip, src, audio, sel, 1)
        notes, tail = oracle(ogg, "transient", "impact")
        out["ping"] = {"cut_rule_end": ev[0][1], "cut_rule_end_notes": [n for n in first_notes if n == "truncated_tail"],
                       "processing": res["processing"], "verdict": res["verdict"], "reasons": res["reasons"], "notes": notes, "tail": tail}
        rng = np.random.default_rng(7)
        t = np.arange(int(1.2 * SR)) / SR
        noise = sg.sosfilt(sg.butter(4, [300, 6000], "bandpass", fs=SR, output="sos"), rng.standard_normal(len(t)))
        # a reversed decay that falls fast after its peak (50 ms): riser's cut stops it 20 dB down while it still
        # decays, its peak more than the oracle's 60 ms before the end (so the tail is judged), as the library's
        # reversed cymbals were
        body = 0.3 * noise / np.sqrt(np.mean(noise ** 2))
        fall = int(0.15 * SR)
        rev = np.concatenate([np.zeros(4800), (body * np.exp(-t / 0.3))[::-1], body[:fall] * np.exp(-np.arange(fall) / SR / 0.05)])
        clip = {"id": "reverse/kenney_rev", "type": "reverse", "kind": "riser", "tags": [], "description": "x", "event": 1}
        res, ogg = bl.prepare_clip(clip, src, source(rev), sel, 1)
        notes, tail = oracle(ogg, "riser", "reverse")
        out["reverse"] = {"processing": res["processing"], "verdict": res["verdict"], "reasons": res["reasons"], "notes": notes, "tail": tail}
        clip_res = {"kind": "transient", "type": "impact", "defects": {"verdicts": {n: False for n in bl.DEFECT_NOTES}},
                    "measured": {"channels": 1, "duration_s": 0.4, "sample_peak_dbfs": -3.0, "true_peak_dbtp": -2.9,
                                 "momentary_max_lufs": -18.0, "head_ms": 1.0}}
        item = {"kind": "transient", "kind_source": "type", "notes": [{"id": "truncated_tail", "text": "tail cut"}],
                "format": {"duration_s": 0.4}, "peak": {"sample_dbfs": -3.0, "true_peak_dbtp": -2.9},
                "loudness": {"momentary_max": -18.0}, "silence": {"head_ms": 1.0}}
        out["editor_reasons"] = bl.editor_reasons(clip_res, item, TARGETS["impact"])[0]
    elif part == "gain":
        out["clips"] = {}
        for name, freq, body in (("snap_4k", 4000, 0.02), ("snap_7k", 7000, 0.015), ("snap_2k", 2500, 0.03)):
            t = np.arange(int(0.4 * SR)) / SR
            sig = 0.9 * np.sin(2 * np.pi * freq * t) * np.exp(-t / 0.002) + body * np.sin(2 * np.pi * 180 * t) * np.exp(-t / 0.08)
            y0 = np.concatenate([np.zeros(960), sig, np.zeros(4800)])
            audio = source(y0)
            clip = {"id": f"impact/kenney_{name}", "type": "impact", "kind": "transient", "tags": [], "description": "x", "event": 1}
            res, ogg = bl.prepare_clip(clip, {"source": "kenney", "pack": "test", "member": "a.ogg"}, audio, sel, 1)
            p = res["processing"]
            n, a, e = p["event"]
            y, _ = bl.band_filter(audio.mono[a:e].copy(), bl.TYPES["impact"])
            fo, _ = bl.fades(y, bl.TYPES["impact"])
            head, tail = p["trim"]
            dec, _ = soundfile.read(io.BytesIO(ogg), dtype="float64")
            # the same cut and trim one grid step louder, through the pinned encoder
            up = y * 10 ** ((p["gain_db"] + 0.05) / 20)  # build_library.TP_STEP_DB
            up = np.concatenate([np.zeros(p["preroll"]), up[head:len(up) - tail], np.zeros(p["postroll"])])
            _, dec_up = bl.encoded_clip(up, 0.6, clip["id"])
            out["clips"][name] = {"limited_by": p["limited_by"], "gain_db": p["gain_db"], "fir": fir_peak(dec), "x16": x16_peak(dec),
                                  "fir_up": fir_peak(dec_up), "x16_up": x16_peak(dec_up)}
        t = np.arange(4800) / SR
        tone = 0.85 * np.sin(2 * np.pi * 20000 * t + 0.1)  # near Nyquist the 4x FIR reads 0.3 dB low; the guard is near -1
        py = {"below150": 0.0, "above6k": 0.1, "flatness": 0.1, "rise_ms": 1.0, "start_db": -30.0, "end_db": -40.0, "spikiness": 1.2,
              "dur_s": 0.3, "build_db": -20.0, "peak_offset_s": 0.01, "true_peak_16x_dbtp": -0.98}
        meas = {"sample_peak_dbfs": -3.0, "true_peak_dbtp": -1.02, "clipped": 0, "head_ms": 1.0, "duration_s": 0.3}
        fine, guard = getattr(bl, "true_peak_fine", None), getattr(bl, "peak_guard", None)  # absent before P5 §5.7
        out["fine"] = {"x4": bl.true_peak(tone), "x16": fine(tone) if fine else x16_peak(tone[:, None]),
                       "guard": guard(tone) if guard else bl.true_peak(tone),
                       "reasons": bl.python_reasons(bl.TYPES["impact"], "transient", py, meas, -20.0)}
    elif part == "seams":
        rng = np.random.default_rng(31)
        n = 14 * SR
        tt = np.arange(n) / SR
        pad = np.stack([0.2 * np.sin(2 * np.pi * 110 * tt + ph) + 0.1 * np.sin(2 * np.pi * 330 * tt + 2 * ph) for ph in (0.0, 0.9)], axis=1)
        pad = pad + 0.01 * sg.sosfilt(sg.butter(4, [100, 3000], "bandpass", fs=SR, output="sos"), rng.standard_normal((n, 2)), axis=0)
        audio = source(pad)
        filtered, _ = audio.filtered_stereo("ambience")
        start, length, xf = 2 * SR, 10 * SR, int(0.5 * SR)
        cands = bl.seam_candidates(filtered, start, length, xf)
        y = bl.loop_crossfade(filtered, start, length + cands[0], xf) * 0.5
        data, dec = bl.encoded_clip(y, 0.7, "ambience/fs_1_pad")
        first = {"gain_db": 20 * np.log10(0.5), "y": y, "ogg": data, "decoded": dec, "bounds": (0, len(y))}
        best, d, tries = bl.seam_retry(first, filtered, start, length, xf, cands, 0.7, "ambience/fs_1_pad")
        # the candidates the pipeline encoded (its tries), each decoded and measured here; the ones within the limits
        tried = []
        for c in cands[:tries]:
            yy = bl.loop_crossfade(filtered, start, length + c, xf) * 0.5
            _, dd = bl.encoded_clip(yy, 0.7, "ambience/fs_1_pad")
            step, level = bl.seam_measures(dd, SR)
            if abs(level) <= bl.LOOP_SEAM_LEVEL_MAX_DB and step <= bl.LOOP_SEAM_STEP_MAX_DB:
                tried.append(seam_step_db(dd, SR))
        out = {"first_shift": cands[0], "chosen_shift": d, "tries": tries,
               "chosen_step": seam_step_db(best["decoded"], SR), "tried_steps": tried}
    print(json.dumps(out))


def _gendup(out):
    """F-3's sources: a Kenney-style impact, the same audio later and quieter, another take of the action (the same
    partials and envelope, its own phases, detuning and noise); a whoosh and its re-upload by a second user whose
    band over 3 kHz was replaced (a lossy re-encode changes it). Prints the pipeline's similarities of the pairs."""
    import numpy as np
    import soundfile as sf
    from scipy import signal as sg
    sys.path.insert(0, str(TOOLS))
    import build_library as bl
    bl._dsp()
    out = Path(out)
    out.mkdir(parents=True, exist_ok=True)
    rng = np.random.default_rng(20261022)
    t = np.arange(int(0.35 * SR)) / SR

    def take(seed):
        g = np.random.default_rng(seed)
        y = np.zeros_like(t)
        for f, a in ((310, 0.5), (720, 0.3), (1490, 0.2), (2830, 0.12), (4100, 0.08)):
            y += a * np.sin(2 * np.pi * f * (1 + 0.01 * g.standard_normal()) * t + g.uniform(0, 2 * np.pi))
        y += 0.05 * sg.sosfilt(sg.butter(4, [800, 9000], "bandpass", fs=SR, output="sos"), g.standard_normal(len(t)))
        return 0.6 * y * np.exp(-t / 0.05) * np.minimum(1.0, t / 0.001)

    a, other = take(1), take(2)
    sil = np.zeros
    sf.write(str(out / "impact_a.ogg"), np.concatenate([sil(480), a, sil(4800)]), SR, format="OGG", subtype="VORBIS")
    sf.write(str(out / "impact_copy.ogg"), np.concatenate([sil(14400), 0.7 * a, sil(4800)]), SR, format="OGG", subtype="VORBIS")
    sf.write(str(out / "impact_take.ogg"), np.concatenate([sil(480), other, sil(4800)]), SR, format="OGG", subtype="VORBIS")
    tw = np.arange(int(0.7 * SR)) / SR
    env = np.exp(-np.abs(tw - 0.35) / 0.08)
    noise = rng.standard_normal(len(tw))
    low = sg.sosfilt(sg.butter(4, [200, 3000], "bandpass", fs=SR, output="sos"), noise)
    high = sg.sosfilt(sg.butter(4, [3000, 7000], "bandpass", fs=SR, output="sos"), noise)
    high2 = sg.sosfilt(sg.butter(4, [3000, 7000], "bandpass", fs=SR, output="sos"), rng.standard_normal(len(tw)))
    w1 = 0.3 * (low + 0.1 * high) * env
    w2 = 0.3 * (low + 0.1 * high2) * env
    for sid, w in ((2001, w1), (2002, w2)):
        sf.write(str(out / f"whoosh_{sid}.ogg"), np.concatenate([sil(4800), w, sil(4800)]), SR, format="OGG", subtype="VORBIS")
    sims = {}
    if hasattr(bl, "clip_similarity"):  # absent before P5 §5.7
        for name, x, y in (("copy", a, 0.7 * a), ("take", a, other), ("reupload", w1, w2)):
            sim, full, lag, bands = bl.clip_similarity(x, y)
            sims[name] = {"similarity": round(sim, 3), "full": round(full, 3), "bands": bands}
    print(json.dumps(sims))


if __name__ == "__main__":
    {"gen": lambda: _gen(sys.argv[2]), "measure": lambda: _measure(sys.argv[2:]),
     "dcforce": lambda: _dcforce(sys.argv[2], sys.argv[3]), "defectcheck": _defectcheck,
     "curategaps": _curategaps, "fixdata": lambda: _fixdata(sys.argv[2]), "gendup": lambda: _gendup(sys.argv[2])}[sys.argv[1]]()
