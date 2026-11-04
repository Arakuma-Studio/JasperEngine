"""S36 AC8: the committed starter library's data, the licence and SHA-256 gate.

Data tests on the real library (editor/resources/sound-library/library), its selection
(tools/sound-library/library.json), audit log (library-audit.jsonl) and semantic file
(library-semantic.jsonl).

Independent oracles: hashlib SHA-256 and SHA-1 over the files; the manifest's `measured` (soundfile, numpy
and pyloudnorm in the pipeline) against the editor's own analyzer (`jasperengine-editor audio-analyze`, every
clip in one process); the true peak by this file's numpy over audio_refs' BS.1770-4 Annex 2 FIR on soundfile's
decode, and by 16x resample_poly; this file's own normalised cross-correlation for near-duplicates; the defect
oracle (tools/mcp/audio_defects_ref.py) on every decoded clip with its manifest fades; a Python wrap-RMS check
for loops; the pinned Kenney zips' License.txt when $JASPER_SOUND_CACHE holds them; `git check-attr` for the
attributes. The DSP runs in a uv subprocess with numpy and soundfile: this file, run as a script ("measure
<library>"), is that helper.
"""

from __future__ import annotations

CI = True

import asyncio
import hashlib
import json
import math
import os
import re
import shutil
import subprocess
import sys
import zipfile
from pathlib import Path

HERE = Path(__file__).resolve().parent
REPO = HERE.parent.parent.parent
TOOLS = REPO / "tools/sound-library"
BUILD = TOOLS / "build_library.py"
SELECTION = TOOLS / "library.json"
AUDIT = TOOLS / "library-audit.jsonl"
SEMANTIC = TOOLS / "library-semantic.jsonl"
ESSENTIALS_SELECTION = TOOLS / "essentials.json"
LIBRARY = REPO / "editor/resources/sound-library/library"
ENV_DIR = "JASPERENGINE_SOUND_LIBRARY_DIR"
TIMEOUT = 1800
STATE: dict = {}

HARD_CAP = 37748736           # spec 2.3.1: 36 MiB for the whole library/ tree
COUNT = (880, 1200)           # spec 2.3.1 as amended by P5 §5.7: the floor 880, under the shipped count
LICENCE_MARKERS = ("Creative Commons Zero", "publicdomain/zero/1.0")
CC0_DEED = "creativecommons.org/publicdomain/zero/1.0/"
LOOP_TYPES = ("ambience", "music")
SEAM_STEP_MAX_DB = 8.0        # P5 §5.7: the library's bound, under SoundTypes::LOOP_SEAM_STEP_MAX_DB (+12, A28)
SEAM_LEVEL_MAX_DB = 3.0
CEILING_DBTP = -1.0
# spec 2.3.2: the per-type minimums of the finished library
SPEC_MIN = {"click": 35, "key": 8, "tick": 8, "tap": 25, "pop": 10, "thump": 15, "hit": 30, "boom": 10, "paper": 15,
            "swish": 20, "whoosh": 20, "transition": 10, "riser": 10, "reverse": 8, "glitch": 15, "ui": 40, "zap": 20,
            "chime": 15, "shimmer": 10, "footstep": 50, "impact": 50, "explosion": 15, "pickup": 25, "jump": 10,
            "hurt": 12, "shoot": 25, "door": 15, "stinger": 40, "voice": 80, "ambience": 20, "music": 6}
# spec 2.11.4's table: the types whose flags include allow_low (the editor's low_end note is fatal for the others)
ALLOW_LOW = {"thump", "hit", "boom", "transition", "riser", "reverse", "impact", "explosion", "shoot", "door", "ambience",
             "music"}
# spec 2.11.4's editor rules: (note id, applies to the clip)
EDITOR_FATAL = (
    ("silent", lambda c: True),
    ("clipping", lambda c: True),
    ("true_peak", lambda c: True),
    ("dc_offset", lambda c: True),
    ("head_silence", lambda c: c["kind"] != "loop"),
    ("loop_seam", lambda c: c["kind"] == "loop"),
    ("phase", lambda c: c["measured"]["channels"] == 2),
    ("riser_early", lambda c: c["kind"] == "riser"),
    ("soft_attack", lambda c: c["kind"] == "transient"),
    ("low_end", lambda c: c["type"] not in ALLOW_LOW),
    ("edge_click", lambda c: True),
    ("clicks", lambda c: True),
    ("crackle", lambda c: c["kind"] != "loop"),
    ("silence_gap", lambda c: True),
    ("truncated_tail", lambda c: True),  # P5 §5.7: prepare repairs a cut tail or rejects the clip
)
# P5 §5.7: the only notes left on library clips, each where it is inherent and documented (§5.3.12): the type's
# loudness target missed because the true peak stopped the gain; a low end on the types whose weight sits low; a narrow
# resonance on risers (bells, glasses, cymbals reversed) and voice lines; folded partials on synthesised tonal and glitch
# designs. Any other note, or one of these elsewhere, is a clip an agent would be told to fix.
INHERENT_NOTES = {
    # said of every clip whose manifest gives fades: the numbers are of the clip as an import plays it
    "as_it_plays": lambda c, it: fades_of(c) is not None,
    "loudness_target": lambda c, it: c["kind"] not in ("loop", "speech") and c["processing"].get("limited_by") == "true_peak",
    "low_end": lambda c, it: c["type"] in ALLOW_LOW,
    "harsh_resonance": lambda c, it: c["kind"] in ("riser", "speech"),
    "aliasing": lambda c, it: c["kind"] in ("tonal", "glitch"),
}
# P5 §5.7 (the review's L-3): tags, ids and descriptions name the sound, not its file or licence
FORMAT_WORDS = {"wav", "ogg", "mp3", "flac", "aif", "aiff", "aifc", "m4a", "wma", "aac"}
NON_SOUND_TAGS = FORMAT_WORDS | {"cc0", "cczero", "cc_zero", "cc", "creative_commons", "public_domain", "publicdomain", "free",
                                 "freesound", "free_to_use", "freeware", "royalty_free", "license", "licence", "stereo", "mono",
                                 "hq", "bit", "khz", "sample", "sound", "sfx", "fx", "audio"}
QUALITY_TAG = re.compile(r"^([0-9]+|(24|32)_?bits?|[0-9]+(_[0-9]+)?_?k(hz)?|[0-9]+_?hz|[0-9]+_?kbps)$")
# P5 §5.7 (the review's L-4): the smallest of the full-band, over-1 kHz and over-3 kHz normalised cross-correlations
# of two clips of a type (or a source); 0.9 or more is one sound twice. Two authors' clips at 0.9 full band are the
# same audio uploaded twice.
NEAR_DUP = 0.90
# spec 2.2.3: what analysis.json leaves out of the CLI's item (and format.load_type)
ANALYSIS_EXCLUDED = ("n", "path", "type", "labels", "licence", "notes", "import")


def fades_of(clip: dict):
    """the fades of a v4 clip's meta, as the pipeline's `import`; None without any"""
    importer = clip["meta"]["import"]["audioImporter"]
    fades = {k: importer[k] for k in ("fade_in_ms", "fade_out_ms") if k in importer}
    return fades or None
LOOP_FADE_IN_MS = 2.0         # P2.5 decision 8's loop length, the only import prepare derives


# ---------------------------------------------------------------------------------------------
# Helpers (test process: standard library only)

def check(cond, message: str) -> None:
    if not cond:
        import harness
        raise harness.TestFailure(message)


def check_eq(actual, expected, message: str) -> None:
    check(actual == expected, f"{message}: got {actual!r}, expected {expected!r}")


def problems(found: list, what: str) -> None:
    """fail with every problem found (at most 40 listed)"""
    check(not found, f"{len(found)} {what}:\n  " + "\n  ".join(found[:40]))


def need_library() -> None:
    if not (LIBRARY / "manifest.json").is_file():
        import harness
        harness.skip("no starter library is committed yet (editor/resources/sound-library/library)")


def sha256(path: Path) -> str:
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


def manifest() -> dict:
    need_library()
    if "manifest" not in STATE:
        STATE["manifest"] = json.loads((LIBRARY / "manifest.json").read_text(encoding="utf-8"))
    return STATE["manifest"]


def clips() -> list:
    return manifest()["clips"]


def selection() -> dict:
    if "selection" not in STATE:
        STATE["selection"] = json.loads(SELECTION.read_text(encoding="utf-8"))
    return STATE["selection"]


def audit() -> list:
    if "audit" not in STATE:
        STATE["audit"] = [json.loads(line) for line in AUDIT.read_text(encoding="utf-8").splitlines()]
    return STATE["audit"]


def finished() -> bool:
    """the Freesound part is in (P5 curate DONE): spec 2.3.1's count and 2.3.2's minimums bind in full"""
    return any(c["source"] == "freesound" for c in clips())


def tree(root: Path) -> dict:
    """relative path -> SHA-256 of every file under root (a link reads 'symlink')"""
    out = {}
    for p in sorted(Path(root).rglob("*")):
        rel = p.relative_to(root).as_posix()
        if p.is_symlink():
            out[rel] = "symlink"
        elif p.is_file():
            out[rel] = sha256(p)
    return out


def normalise_licence(url: str):
    m = re.match(r"^https?://([^/?#]+)(/[^?#]*)?$", (url or "").strip())
    if not m:
        return None
    path = m.group(2) or "/"
    return m.group(1).lower() + (path if path.endswith("/") else path + "/")


def editor() -> list:
    import harness
    return harness.editor_command()


async def run(cmd, env=None, timeout=1200):
    r = await asyncio.to_thread(subprocess.run, [str(c) for c in cmd], capture_output=True, text=True, env=env,
                                stdin=subprocess.DEVNULL, timeout=timeout)
    return r.returncode, r.stdout, r.stderr


def library_env(root: Path = LIBRARY) -> dict:
    env = dict(os.environ)
    env[ENV_DIR] = str(root)
    return env


async def cli_json(*args, root: Path = LIBRARY) -> tuple:
    code, out, err = await run(editor() + list(args), env=library_env(root))
    try:
        return code, json.loads(out), err
    except ValueError:
        check(False, f"{' '.join(args[:3])} printed no JSON (exit {code}): {err[-1500:]}")


async def analyzed() -> dict:
    """`audio-analyze --json library:*` over the committed library, every clip in one process: id -> item"""
    if "items" not in STATE:
        code, out, err = await cli_json("audio-analyze", "--json", "library:*")
        check_eq((code, out.get("errors")), (0, []), f"audio-analyze library:* analyses every clip ({err[-800:]})")
        items = {}
        for it in out["items"]:
            check(it["path"].startswith("library:"), f"an item for {it['path']!r}, not a library: id")
            items[it["path"][len("library:"):]] = it
        STATE["items"] = items
    return STATE["items"]


def uv() -> str:
    return shutil.which("uv") or str(Path.home() / ".local/bin/uv")


async def measured() -> dict:
    """this file's numpy helper over the decoded clips (true peak, edges, defects, loop wraps): id -> reading"""
    if "measure" not in STATE:
        cmd = [uv(), "run", "--quiet", "--no-project", "--with", "numpy", "--with", "scipy", "--with", "soundfile",
               "python", str(Path(__file__).resolve()), "measure", str(LIBRARY)]
        code, out, err = await run(cmd, timeout=1500)
        check_eq(code, 0, f"the numpy helper ran ({err[-1500:]})")
        STATE["measure"] = json.loads(out.strip().splitlines()[-1])
    return STATE["measure"]


# ---------------------------------------------------------------------------------------------
# D-1 The committed tree


async def test_d1_verify_passes_on_plain_python():
    """`python3 tools/sound-library/build_library.py verify` (no packages) passes on the repository's library."""
    need_library()
    code, out, err = await run([sys.executable if Path(sys.executable).name.startswith("python3") else "python3", BUILD, "verify"])
    check_eq(code, 0, f"build_library.py verify: {out[-600:]} {err[-1500:]}")
    check("ok" in out, f"verify prints its summary: {out[-300:]}")


async def test_d1_files_hashes_and_attributes():
    """Every file's SHA-256 and size equal the manifest; the tree holds nothing else and no link; the tree is
    at most 36 MiB; `git check-attr` gives spec 2.3.4's attributes for a clip and for manifest.json."""
    m = manifest()
    found = []
    expected = {"manifest.json", "analysis.json", ".gitattributes"}
    for c in m["clips"]:
        check_eq(c["path"], c["id"] + ".ogg", f"{c['id']}: path is id + .ogg")
        p = LIBRARY / c["path"]
        expected.add(c["path"])
        if p.is_symlink() or not p.is_file():
            found.append(f"{c['path']}: missing or not a regular file")
            continue
        data = p.read_bytes()
        if len(data) != c["bytes"] or hashlib.sha256(data).hexdigest() != c["sha256"]:
            found.append(f"{c['path']}: {len(data)} bytes, SHA-256 {hashlib.sha256(data).hexdigest()} (manifest: {c['bytes']}, {c['sha256']})")
    for pack in m["packs"]:
        expected.add(pack["licence_file"])
        if "credits_file" in pack:
            expected.add(pack["credits_file"])
    files = tree(LIBRARY)
    found += [f"{rel}: not named by the manifest" for rel in sorted(set(files) - expected)]
    found += [f"{rel}: a link" for rel, h in files.items() if h == "symlink"]
    found += [f"{rel}: missing" for rel in sorted(expected - set(files))]
    problems(found, "problems in the committed tree")
    total = sum((LIBRARY / rel).stat().st_size for rel in files)
    check(total <= HARD_CAP, f"the tree is {total} bytes, over the {HARD_CAP} cap")
    STATE["total_bytes"] = total

    ogg = LIBRARY / m["clips"][0]["path"]
    code, out, err = await run(["git", "-C", REPO, "check-attr", "-a", "--", ogg.relative_to(REPO).as_posix()])
    check_eq(code, 0, f"git check-attr ran: {err}")
    for attr in ("text", "diff", "merge"):
        check(f": {attr}: unset" in out, f"{ogg.name}: {attr} is unset (spec 2.3.4): {out!r}")
    code, out, err = await run(["git", "-C", REPO, "check-attr", "-a", "--", (LIBRARY / "manifest.json").relative_to(REPO).as_posix()])
    check(": text: unset" in out, f"manifest.json: text is unset (spec 2.3.4): {out!r}")


async def test_d1_count_and_minimums():
    """The clip count is inside the selection's budget and every type meets its minimum. The selection's budget
    keeps spec 2.3.1's cap and maximum, and never asks more than spec 2.3.2; the finished library sits above its
    floor and above every minimum (P5 §5.7: slack for a removal)."""
    sel = selection()
    budget = sel["budget"]
    check_eq(budget["max_bytes"], HARD_CAP, "budget.max_bytes is spec 2.3.1's cap")
    check_eq(budget["total"][1], COUNT[1], "budget.total's maximum is spec 2.3.1's")
    n = len(clips())
    check(budget["total"][0] <= n <= budget["total"][1], f"{n} clips, outside the selection's budget.total {budget['total']}")
    per_type = {}
    for c in clips():
        per_type[c["type"]] = per_type.get(c["type"], 0) + 1
    found = [f"{t}: {per_type.get(t, 0)} clips, under the selection's minimum {m}" for t, m in sorted(budget["min_per_type"].items())
             if per_type.get(t, 0) < m]
    found += [f"{t}: the selection's minimum {m} is above spec 2.3.2's {SPEC_MIN[t]}" for t, m in budget["min_per_type"].items()
              if m > SPEC_MIN[t]]
    if finished():
        check(COUNT[0] <= n <= COUNT[1], f"{n} clips, outside spec 2.3.1's {COUNT[0]}-{COUNT[1]}")
        check_eq(budget["total"], list(COUNT), "budget.total is spec 2.3.1's")
        # P5 §5.7 (the review's L-6): slack, so one clip can leave without write refusing the library
        if n <= budget["total"][0]:
            found.append(f"{n} clips, no more than the floor {budget['total'][0]}")
        found += [f"{t}: {per_type.get(t, 0)} clips, no more than its minimum {m}" for t, m in sorted(budget["min_per_type"].items())
                  if 0 < per_type.get(t, 0) <= m]
    problems(found, "budget problems")


async def test_d1_tags_ids_and_descriptions_name_the_sound():
    """P5 §5.7 (the review's L-3): no tag names a licence, a file format or a quality; no id carries a file
    extension; every description is words, not a number or a file name, and no Freesound description is a
    multi-clip source's title repeated for each of its clips."""
    found = []
    by_source = {}
    for c in clips():
        name = c["id"].split("/", 1)[1]
        bad = [t for t in c["tags"] if t in NON_SOUND_TAGS or QUALITY_TAG.match(t)]
        if bad:
            found.append(f"{c['id']}: tags {bad}")
        if any(w in FORMAT_WORDS for w in name.split("_")):
            found.append(f"{c['id']}: the id carries a file extension")
        d = c["description"]
        if sum(ch.isalpha() for ch in d) < 3 or re.search(r"\.(" + "|".join(sorted(FORMAT_WORDS)) + r")\b", d, re.I):
            found.append(f"{c['id']}: description {d!r}")
        if c["source"] == "freesound":
            by_source.setdefault(c["source_id"], []).append(c)
    titles = {r["source"]: r.get("page_title") for r in audit() if r["line"] == "source"}
    for sid, cs in sorted(by_source.items()):
        title = (titles.get(f"freesound:{sid}") or "").strip().lower()
        same = [c["id"] for c in cs if c["description"].strip().lower() == title]
        if len(cs) > 1 and len(same) == len(cs):
            found.append(f"freesound:{sid}: its {len(cs)} clips all repeat the page title {title!r}")
    problems(found, "names that do not describe the sound")


# ---------------------------------------------------------------------------------------------
# D-2 The installed tree


async def test_d2_installed_tree_equals_the_repository():
    """build/linux/editor-install/bin/sound-library equals the repository's library file for file, and the
    installed editor's `sound-library verify --json` (no variable) exits 0 with the manifest's counts."""
    need_library()
    exe = Path(editor()[0]).resolve()
    installed = exe.parent / "sound-library"
    check(installed.is_dir(), f"{installed} exists (the install rule of spec 2.4.2)")
    check_eq(tree(installed), {k: v for k, v in tree(LIBRARY).items() if k != ".gitattributes"}, "bin/sound-library is the repository's library, path for path and SHA-256 for SHA-256")
    env = dict(os.environ)
    env.pop(ENV_DIR, None)
    code, out, err = await run([*editor(), "sound-library", "verify", "--json"], env=env)
    check_eq(code, 0, f"sound-library verify exits 0: {out[-800:]} {err[-800:]}")
    res = json.loads(out)
    m = manifest()
    check_eq((res["ok"], res["root"], res["issues"]), (True, installed.as_posix(), []), "verify of the installed library")
    check_eq((res["version"], res["clips"], res["bytes"], res["licences"]),
             (m["version"], len(m["clips"]), sum(c["bytes"] for c in m["clips"]), len(m["packs"])),
             "verify's counts equal the manifest's (version, clips, clip bytes, licences)")


# ---------------------------------------------------------------------------------------------
# D-3 Licences and provenance


async def test_d3_licences_and_provenance():
    """Every clip is CC0-1.0; every Kenney pack's licence file holds both CC0 markers and is the License.txt
    its audit line hashed in the pinned zip; every Freesound clip's licence_url normalises to CC0's deed and its
    source line records that URL, the page's SHA-256, explicit false and remix group false; no clip is cut from
    an essentials member; manifest clips and accepted audit lines are the same set, with the same bytes."""
    m = manifest()
    found = []
    ess = json.loads(ESSENTIALS_SELECTION.read_text(encoding="utf-8"))
    ess_members = {(c["pack"], c["member"]) for c in ess["clips"]}
    lines = audit()
    sources = {r["source"]: r for r in lines if r["line"] == "source"}
    accepted = {r["id"]: r for r in lines if r["line"] == "clip" and r["verdict"] == "accepted"}
    packs = {p["id"]: p for p in m["packs"]}
    check_eq(m["licence"], "CC0-1.0", "the manifest's licence")
    for p in m["packs"]:
        text = (LIBRARY / p["licence_file"]).read_bytes()
        for marker in LICENCE_MARKERS:
            if marker not in text.decode("utf-8", "replace"):
                found.append(f"{p['licence_file']}: no {marker!r}")
        if p["licence"] != "CC0-1.0":
            found.append(f"pack {p['id']}: licence {p['licence']}")
    for c in m["clips"]:
        cid = c["id"]
        if c["licence"] != "CC0-1.0":
            found.append(f"{cid}: licence {c['licence']}")
        if c["source"] == "kenney":
            key = f"kenney:{c['pack']}:{c['source_file']}"
            if (c["pack"], c["source_file"]) in ess_members:
                found.append(f"{cid}: cut from an essentials member ({c['pack']} {c['source_file']})")
            src = sources.get(key)
            pack = packs.get(c["pack"])
            if src is None or pack is None:
                found.append(f"{cid}: no source audit line {key} or no pack")
                continue
            if src.get("zip_sha256") != pack["sha256"] or src.get("zip_bytes") != pack["bytes"]:
                found.append(f"{cid}: the audit's zip pin differs from the manifest's pack")
            if src.get("licence_sha256") != sha256(LIBRARY / pack["licence_file"]):
                found.append(f"{cid}: {pack['licence_file']} is not the License.txt its audit line hashed")
            if sorted(src.get("licence_markers") or []) != sorted(LICENCE_MARKERS):
                found.append(f"{cid}: the audit found markers {src.get('licence_markers')}")
            if "credits_file" in pack and src.get("credits_sha256") != sha256(LIBRARY / pack["credits_file"]):
                found.append(f"{cid}: {pack['credits_file']} is not the Credits.txt its audit line hashed")
        elif c["source"] == "freesound":
            src = sources.get(f"freesound:{c['source_id']}")
            if normalise_licence(c.get("licence_url")) != CC0_DEED:
                found.append(f"{cid}: licence_url {c.get('licence_url')!r} is not CC0's deed")
            if src is None:
                found.append(f"{cid}: no source audit line")
                continue
            if (src.get("licence_url"), src.get("explicit"), src.get("remix_group")) != (c.get("licence_url"), False, False):
                found.append(f"{cid}: audit evidence {src.get('licence_url')!r}, explicit {src.get('explicit')}, remix {src.get('remix_group')}")
            if not re.fullmatch(r"[0-9a-f]{64}", str(src.get("page_sha256"))):
                found.append(f"{cid}: the audit records no page SHA-256")
            if c.get("pack") is not None or c.get("source_file") is not None:
                found.append(f"{cid}: a Freesound clip names a pack or member")
        else:
            found.append(f"{cid}: source {c['source']!r}")
        a = accepted.get(cid)
        if a is None:
            found.append(f"{cid}: no accepted audit line")
        elif a["ogg"] != {"sha256": c["sha256"], "bytes": c["bytes"]}:
            found.append(f"{cid}: the accepted audit line's file is {a['ogg']}, the manifest's {c['sha256']} {c['bytes']}")
    found += [f"{cid}: accepted in the audit log but not in the manifest" for cid in sorted(set(accepted) - {c['id'] for c in m['clips']})]
    # one recording, one clip: no two clips cut the same event from the same source bytes (Kenney zips hold
    # byte-identical members under two names), and no two decode to the same measurements (a sound uploaded twice)
    cuts, measured = {}, {}
    for c in m["clips"]:
        src = sources.get(f"kenney:{c['pack']}:{c['source_file']}" if c["source"] == "kenney" else f"freesound:{c.get('source_id')}") or {}
        data = src.get("member_sha256") or src.get("preview_sha256")
        p = c["processing"]
        cut = json.dumps([data, p.get("event", [None])[0], p.get("segment")])
        if data and cut in cuts:
            found.append(f"{c['id']}: the same source bytes and cut as {cuts[cut]}")
        cuts.setdefault(cut, c["id"])
        key = json.dumps(c["measured"], sort_keys=True)
        if key in measured:
            found.append(f"{c['id']}: measures exactly as {measured[key]} (the same recording twice)")
        measured.setdefault(key, c["id"])
    problems(found, "licence and provenance problems")


async def test_d3_licence_files_match_the_pinned_zips():
    """Each committed licence (and credits) file is byte for byte the License.txt (Credits.txt) of its pinned
    zip, read from $JASPER_SOUND_CACHE (skipped without it)."""
    m = manifest()
    cache = os.environ.get("JASPER_SOUND_CACHE")
    if not cache:
        import harness
        harness.skip("set JASPER_SOUND_CACHE to the directory holding the pinned Kenney zips (kenney_<pack>.zip)")
    sel_packs = {p["id"]: p for p in selection()["packs"]}
    found = []
    for p in m["packs"]:
        z = Path(cache) / f"kenney_{p['id']}.zip"
        if not z.is_file():
            found.append(f"{z.name}: not in {cache}")
            continue
        if sha256(z) != p["sha256"] or z.stat().st_size != p["bytes"]:
            found.append(f"{z.name}: not the pinned zip")
            continue
        with zipfile.ZipFile(z) as zf:
            if zf.read("License.txt") != (LIBRARY / p["licence_file"]).read_bytes():
                found.append(f"{p['licence_file']} differs from {z.name}'s License.txt")
            if "credits_file" in p:
                member = sel_packs[p["id"]]["credits_member"]
                if zf.read(member) != (LIBRARY / p["credits_file"]).read_bytes():
                    found.append(f"{p['credits_file']} differs from {z.name}'s {member}")
    problems(found, "licence files that are not their zips'")


async def test_d3_no_near_duplicates():
    """P5 §5.7 (the review's L-4): no two clips of a type, or of one source, are one sound twice (this file's
    band-wise cross-correlation under 0.9), and no two clips by different authors are the same audio (full band
    under 0.9)."""
    need_library()
    readings = await measured()
    pairs = readings["_pairs"]
    found = [f"{p['a']} ~ {p['b']}: similarity {p['similarity']} (full {p['full']}, bands {p['bands']})" for p in pairs
             if p["similarity"] >= NEAR_DUP]
    found += [f"{p['a']} ({p['authors'][0]}) ~ {p['b']} ({p['authors'][1]}): full-band {p['full']}" for p in pairs
              if p["authors"][0] != p["authors"][1] and p["full"] >= NEAR_DUP]
    STATE["near_pairs"] = sorted(pairs, key=lambda p: -p["similarity"])[:5]
    problems(found, "near-duplicate pairs")


# ---------------------------------------------------------------------------------------------
# D-4 The analyzer agrees with the oracle


async def test_d4_analyzer_agrees_with_the_oracle():
    """`audio-analyze --json library:*` against the manifest's measured: frames +/-64, duration +/-2 ms, sample
    peak +/-0.2 dB, true peak +/-0.3 dB, the type's loudness measure +/-0.2 LU, head +/-2 ms; kind_source
    "type" with the manifest's kind; none of spec 2.11.4's fatal notes (truncated_tail with them, P5 §5.7), and
    no note but §5.7's inherent ones where they are inherent; one-shots within 6 LU under and 0.5 LU over their
    target; true peak <= -1.0 dBTP by numpy's FIR and by 16x resample_poly on the decoded file; 48 kHz, mono
    except the loop types."""
    need_library()
    items = await analyzed()
    readings = await measured()
    targets = selection()["targets"]
    found = []
    check_eq(sorted(items), sorted(c["id"] for c in clips()), "one item per manifest clip")
    for c in clips():
        cid, ms, it = c["id"], c["measured"], items[c["id"]]
        fmt, peak, loud, sil = it["format"], it["peak"], it["loudness"], it["silence"]
        integrated = c["kind"] in ("loop", "speech")
        lk, mk = ("integrated_lufs", "integrated_lufs") if integrated else ("momentary_max", "momentary_max_lufs")
        pairs = [("frames", fmt["frames"], ms["frames"], 64), ("duration", fmt["duration_s"], ms["duration_s"], 0.002 + 1e-9),
                 ("sample peak", peak["sample_dbfs"], ms["sample_peak_dbfs"], 0.2 + 1e-9),
                 ("true peak", peak["true_peak_dbtp"], ms["true_peak_dbtp"], 0.3 + 1e-9),
                 ("loudness", loud[lk], ms[mk], 0.2 + 1e-9)]
        if c["kind"] != "loop":
            pairs.append(("head", sil["head_ms"], ms["head_ms"], 2.0 + 1e-9))
        for name, ed, py, tol in pairs:
            if ed is None or py is None or abs(ed - py) > tol:
                found.append(f"{cid}: {name} {ed} (editor) vs {py} (manifest), tolerance {tol:g}")
        if (it["kind"], it["kind_source"]) != (c["kind"], "type"):
            found.append(f"{cid}: kind {it['kind']} ({it['kind_source']}), the manifest's {c['kind']}")
        notes = {n["id"] for n in it.get("notes") or []}
        found += [f"{cid}: fatal note {n}" for n, applies in EDITOR_FATAL if n in notes and applies(c)]
        found += [f"{cid}: note {n} ({next(x['text'] for x in it['notes'] if x['id'] == n)})" for n in sorted(notes)
                  if n not in INHERENT_NOTES or not INHERENT_NOTES[n](c, it)]
        if not integrated:
            t = targets[c["type"]]
            if not t - 6.0 <= loud["momentary_max"] <= t + 0.5:
                found.append(f"{cid}: momentary max {loud['momentary_max']} LUFS, outside {t - 6.0}..{t + 0.5} (target {t})")
        tp, tp16 = readings[cid]["true_peak_dbtp"], readings[cid]["true_peak_16x_dbtp"]
        if tp > CEILING_DBTP or tp16 > CEILING_DBTP + 1e-6:
            found.append(f"{cid}: true peak {tp:.3f} dBTP by numpy's FIR, {tp16:.3f} by 16x resample_poly, over {CEILING_DBTP}")
        channels = 2 if c["type"] in LOOP_TYPES else 1
        if (fmt["channels"], fmt["sample_rate"], readings[cid]["channels"], readings[cid]["rate"]) != (channels, 48000, channels, 48000):
            found.append(f"{cid}: {fmt['channels']} channel(s) at {fmt['sample_rate']} Hz (decoded {readings[cid]['channels']} at "
                         f"{readings[cid]['rate']}), expected {channels} at 48000")
    problems(found, "disagreements between the analyzer, the manifest and the oracle")


# ---------------------------------------------------------------------------------------------
# D-5 Precomputed analysis is current


async def test_d5_analysis_is_current():
    """analysis.json's analyzer_version is the editor's AudioAnalysis::VERSION, every item is the CLI's
    (`--detail summary`) minus spec 2.2.3's excluded keys and format.load_type, and its sha1 is the file's."""
    need_library()
    analysis = json.loads((LIBRARY / "analysis.json").read_text(encoding="utf-8"))
    code, types, err = await cli_json("audio-analyze", "--list-types", "--json")
    check_eq(code, 0, f"--list-types: {err[-500:]}")
    check_eq((analysis["format"], analysis["version"]), ("jasperengine-sound-analysis", 1), "analysis.json's format")
    check_eq(analysis["analyzer_version"], types["analyzer_version"], "analysis.json's analyzer_version is the editor's")
    items = await analyzed()
    check_eq(sorted(analysis["items"]), sorted(items), "analysis.json names exactly the analysed clips")
    found = []
    for cid, it in items.items():
        want = {k: v for k, v in it.items() if k not in ANALYSIS_EXCLUDED}
        want["format"] = {k: v for k, v in want["format"].items() if k != "load_type"}
        if analysis["items"][cid] != want:
            diff = sorted(k for k in set(want) | set(analysis["items"][cid]) if want.get(k) != analysis["items"][cid].get(k))
            found.append(f"{cid}: differs from the CLI's item in {diff}")
        sha1 = hashlib.sha1((LIBRARY / f"{cid}.ogg").read_bytes()).hexdigest()
        if analysis["items"][cid].get("sha1") != sha1:
            found.append(f"{cid}: sha1 {analysis['items'][cid].get('sha1')}, the file's {sha1}")
    problems(found, "stale analysis items")


# ---------------------------------------------------------------------------------------------
# D-6 Targets


async def test_d6_targets_and_vocabulary_are_the_editors():
    """The selection's targets and every clip's type and kind are the editor's (`audio-analyze --list-types
    --json`); every manifest type is one of its 31 types."""
    need_library()
    code, out, err = await cli_json("audio-analyze", "--list-types", "--json")
    check_eq(code, 0, f"--list-types: {err[-500:]}")
    types = {t["type"]: t for t in out["types"]}
    check_eq(len(types), 31, "the editor's types")
    check_eq(selection()["targets"], {t: v["target_lufs"] for t, v in types.items()}, "the selection's targets are SoundTypes.h's")
    kinds = {t["kind"] for t in types.values()}
    found = []
    for c in clips():
        if c["type"] not in types:
            found.append(f"{c['id']}: type {c['type']} is not the editor's")
            continue
        if c["kind"] not in kinds:
            found.append(f"{c['id']}: kind {c['kind']} is not one of the editor's")
        tk = types[c["type"]]["kind"]
        if (c["kind"] in ("loop", "speech") or tk in ("loop", "speech")) and c["kind"] != tk:
            found.append(f"{c['id']}: kind {c['kind']} on a {c['type']} ({tk}) clip")
    for c in selection()["sources"]:
        for clip in c["clips"]:
            if clip["type"] not in types or clip["kind"] not in kinds:
                found.append(f"selection {clip['id']}: type {clip['type']} / kind {clip['kind']} not the editor's")
    problems(found, "type or kind problems")


# ---------------------------------------------------------------------------------------------
# D-7 Loops


async def test_d7_loops_wrap_cleanly():
    """Every loop clip: seam step <= +8 dB (the library's bound, P5 §5.7; the editor notes +12) and |seam level| <= 3 dB
    from the CLI, no loop_seam note, the manifest's seam_step_db <= +8, and the RMS of the 20 ms around the wrap within
    1 dB of the clip's."""
    loops = [c for c in clips() if c["kind"] == "loop"]
    if not loops:
        import harness
        harness.skip("no loop clips in the library yet (the Kenney part has none; ambience and music come with Freesound)")
    items = await analyzed()
    readings = await measured()
    found = []
    for c in loops:
        it, cid = items[c["id"]], c["id"]
        if it["loop"]["seam_step_db"] > SEAM_STEP_MAX_DB or abs(it["loop"]["seam_level_db"]) > SEAM_LEVEL_MAX_DB:
            found.append(f"{cid}: seam step {it['loop']['seam_step_db']} dB, level {it['loop']['seam_level_db']} dB (editor)")
        if "loop_seam" in {n["id"] for n in it.get("notes") or []}:
            found.append(f"{cid}: loop_seam note")
        if c["measured"]["seam_step_db"] > SEAM_STEP_MAX_DB:
            found.append(f"{cid}: manifest seam_step_db {c['measured']['seam_step_db']}")
        w = readings[cid]["wrap"]
        if abs(w["wrap_rms_db"] - w["rms_db"]) > 1.0:
            found.append(f"{cid}: the 20 ms around the wrap {w['wrap_rms_db']:.2f} dB against the clip's {w['rms_db']:.2f} dB")
    problems(found, "loop problems")


# ---------------------------------------------------------------------------------------------
# D-8 Defects and semantics


async def test_d8_defects_by_the_phase25_oracle():
    """Phase 2.5's oracle on every decoded clip with its manifest import defaults: no edge_click, clicks or
    silence_gap where its gating gives the note, no crackle on one-shot and speech clips; a loop with an import
    clicks at its start without it and not with it, and no other clip has an import; every non-loop clip's first
    and last decoded samples pass the edge rule (under -60 dBFS, -40 dB re the peak, or a zero crossing)."""
    need_library()
    readings = await measured()
    found = []
    for c in clips():
        r, cid = readings[c["id"]], c["id"]
        notes = set(r["notes"])
        for n in ("edge_click", "clicks", "silence_gap"):
            if n in notes:
                found.append(f"{cid}: {n} ({r['detail']})")
        if "crackle" in notes and c["kind"] != "loop":
            found.append(f"{cid}: crackle on a {c['kind']} clip")
        imp = fades_of(c)
        if imp is not None:
            if c["kind"] != "loop" or imp != {"fade_in_ms": LOOP_FADE_IN_MS}:
                found.append(f"{cid}: import {imp} on a {c['kind']} clip")
            elif not r["start_click_without_import"]:
                found.append(f"{cid}: has an import fade but its first frame does not click without it")
        elif c["kind"] == "loop" and r["start_click_without_import"]:
            found.append(f"{cid}: a loop whose first frame clicks has no import fade")
        if c["kind"] != "loop" and (r["edges"]["start_click"] or r["edges"]["end_click"]):
            found.append(f"{cid}: edge sample clicks (start {r['edges']['start_db']} dB, end {r['edges']['end_db']} dB re the peak)")
    problems(found, "defects")


async def test_d8_semantic_file():
    """With library-semantic.jsonl committed: build_library.py verify checks it (D-1); every clip whose label
    is review has a review_note in the selection, and the manifest's semantic is the file's reading for the
    clip's encoded SHA-256. The test loads no model."""
    need_library()
    if not SEMANTIC.is_file():
        import harness
        harness.skip("library-semantic.jsonl is not committed")
    lines = [json.loads(x) for x in SEMANTIC.read_text(encoding="utf-8").splitlines()]
    check_eq(lines[0].get("line"), "stage", "the semantic file's header")
    by_sha = {}
    for x in lines[1:]:
        by_sha[(x["id"], x["audio_sha256"])] = x
    notes = {cl["id"]: cl.get("review_note") for s in selection()["sources"] for cl in s["clips"]}
    found = []
    for c in clips():
        x = by_sha.get((c["id"], c["sha256"]))
        want = None if x is None else {"label": x["label"]["verdict"], "heard": [h["tag"] for h in x["heard"]]}
        if c.get("semantic") != want:
            found.append(f"{c['id']}: manifest semantic {c.get('semantic')}, the file's {want}")
        if want and want["label"] == "review" and not notes.get(c["id"]):
            found.append(f"{c['id']}: label review without a review_note")
    problems(found, "semantic problems")


# ---------------------------------------------------------------------------------------------
# The numpy helper (run as a script under uv with numpy and soundfile)

def _near_pairs(man: dict, mono: dict) -> list:
    """P5 §5.7's near-duplicate reading, written here from the spec's text: for the clips of one type and of one
    source, the largest |cross-correlation| over all lags over the root of the two energies, full band and after a
    4th-order Butterworth high-pass at 1 and 3 kHz; pairs whose full band reads 0.7 or more are returned with the
    smallest of the three (the near-duplicate similarity can only be under the full band's)."""
    import itertools
    import numpy as np
    from scipy import signal

    from scipy import fft

    def spectra(xs):
        # single precision: a type's 38 beds of 24 s would hold 1.2 GB of spectra in double
        n = fft.next_fast_len(2 * max(len(x) for x in xs), real=True)
        return n, [(fft.rfft(np.asarray(x, dtype=np.float32), n), float(np.sqrt(np.dot(x, x)))) for x in xs]

    def ncc(fa, fb, n):
        (sa, ea), (sb, eb) = fa, fb
        return float(np.abs(fft.irfft(sa * np.conj(sb), n)).max()) / (ea * eb) if ea > 0 and eb > 0 else 0.0

    hp = {f: signal.butter(4, f, "highpass", fs=48000, output="sos") for f in (1000, 3000)}
    groups = {}
    for c in man["clips"]:
        groups.setdefault(("type", c["type"]), []).append(c)
        groups.setdefault(("source", c.get("source_file") or c.get("source_id")), []).append(c)
    seen, out = set(), []
    for members in groups.values():
        members = sorted(members, key=lambda c: c["id"])
        if len(members) < 2:
            continue
        n, sp = spectra([mono[c["id"]] for c in members])
        for i, j in itertools.combinations(range(len(members)), 2):
            a, b = members[i], members[j]
            if (a["id"], b["id"]) in seen:
                continue
            seen.add((a["id"], b["id"]))
            full = ncc(sp[i], sp[j], n)
            if full < 0.7:
                continue
            x, y = mono[a["id"]], mono[b["id"]]
            bands = {}
            for f, sos in hp.items():
                m, (fx, fy) = spectra([signal.sosfilt(sos, x), signal.sosfilt(sos, y)])
                bands[str(f)] = round(ncc(fx, fy, m), 3)
            out.append({"a": a["id"], "b": b["id"], "full": round(full, 3), "bands": bands,
                        "similarity": round(min(full, *bands.values()), 3), "authors": [a["author"], b["author"]]})
    return out


def _measure(root: str) -> None:
    import numpy as np
    import soundfile
    from scipy import signal
    sys.path.insert(0, str(HERE.parent))
    import audio_refs
    import importlib.util
    spec = importlib.util.spec_from_file_location("audio_defects_ref", HERE.parent / "audio_defects_ref.py")
    ref = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(ref)
    fir = np.array(audio_refs.TRUE_PEAK_FIR, dtype=np.float64)
    man = json.loads((Path(root) / "manifest.json").read_text(encoding="utf-8"))
    out, mono = {}, {}
    for c in man["clips"]:
        data, sr = soundfile.read(str(Path(root) / c["path"]), dtype="float64", always_2d=True)
        mono[c["id"]] = data.mean(axis=1)
        x = data.T
        up = max(float(np.abs(signal.resample_poly(np.concatenate([np.zeros(64), ch, np.zeros(64)]), 16, 1)).max()) for ch in x)
        best = float(np.abs(x).max()) if x.size else 0.0
        for ch in x:
            # the 4 phases of 12 taps (BS.1770-4 Annex 2), the samples themselves included above
            padded = np.concatenate([np.zeros(12), ch, np.zeros(12)])
            for phase in fir:
                best = max(best, float(np.abs(np.convolve(padded, phase)).max()))
        tp = 20 * math.log10(best) if best > 0 else -120.0
        x32 = np.ascontiguousarray(x.astype(np.float32))
        imp = fades_of(c) or {}
        region = {}
        if imp:
            region = {"fade_in_frames": ref.lround(imp.get("fade_in_ms", 0.0) * sr / 1000.0),
                      "fade_out_frames": ref.lround(imp.get("fade_out_ms", 0.0) * sr / 1000.0)}
        d = ref.defects(x32, float(sr), region)
        notes = ref.note_ids(d, c["kind"], c["type"])
        d0 = ref.defects(x32, float(sr), {}) if imp else d
        e = d["edges"] if d and not d.get("empty") else {"start_click": False, "end_click": False, "start_db": None, "end_db": None}
        detail = {} if not d or d.get("empty") else {"clicks": d["clicks"]["count"], "gaps": d["gaps"]["count"],
                                                     "start_db": e["start_db"], "end_db": e["end_db"]}
        reading = {"true_peak_dbtp": tp, "true_peak_16x_dbtp": 20 * math.log10(max(up, best)) if up > 0 else -120.0,
                   "channels": int(x.shape[0]), "rate": int(sr), "notes": notes, "detail": detail,
                   "edges": {k: e[k] for k in ("start_click", "end_click", "start_db", "end_db")},
                   "start_click_without_import": bool(d0 and not d0.get("empty") and d0["edges"]["start_click"])}
        if c["kind"] == "loop":
            w = int(round(0.010 * sr))
            wrap = np.concatenate([x[:, -w:], x[:, :w]], axis=1)
            db = lambda v: 20 * math.log10(max(float(np.sqrt(np.mean(v * v))), 1e-12))
            reading["wrap"] = {"wrap_rms_db": db(wrap), "rms_db": db(x)}
        out[c["id"]] = reading
    out["_pairs"] = _near_pairs(man, mono)
    print(json.dumps(out))


if __name__ == "__main__":
    {"measure": lambda: _measure(sys.argv[2])}[sys.argv[1]]()



async def test_d9_v4_and_the_exclusion():
    """927 clips, manifest v4 with a per-clip meta; fs_608433 absent everywhere but excluded[]; no denied
    term in an id, tag or description; every clip's meta is what an import writes; migrate-manifest
    reproduces the committed manifest byte for byte, from its v3 form."""
    need_library()
    text = (LIBRARY / "manifest.json").read_text(encoding="utf-8")
    m = json.loads(text)
    check_eq((m["format"], m["version"], len(m["clips"])), ("jasperengine-sound-library", 4, 927), "manifest v4, 927 clips")
    sources = {}
    for c in m["clips"]:
        sources[c["source"]] = sources.get(c["source"], 0) + 1
    check_eq(sources, {"freesound": 531, "kenney": 396}, "the sources")
    sel = json.loads(SELECTION.read_text(encoding="utf-8"))
    check_eq(sel["excluded"][0]["id"], "pickup/fs_608433_handheld_coin", "the first exclusion")
    for name in ("manifest.json", "analysis.json"):
        check("608433" not in (LIBRARY / name).read_text(encoding="utf-8"), f"{name} does not name fs_608433")
    check(not (LIBRARY / "pickup/fs_608433_handheld_coin.ogg").exists(), "its file is gone")
    for path in (AUDIT, SEMANTIC):
        check("608433" not in path.read_text(encoding="utf-8"), f"{path.name} does not name fs_608433")
    check("608433" not in json.dumps(sel["sources"]), "the selection's sources do not name it")
    denied = [t.lower() for t in sel["denied_terms"]]
    check(denied, "denied terms are listed")
    for c in m["clips"]:
        words = " ".join([c["id"], c["path"], c["description"], *c["tags"]]).lower().replace("_", " ")
        for term in denied:
            check(not re.search(r"(?<![a-z0-9])" + re.escape(term) + r"(?![a-z0-9])", words), f"{c['id']} names {term}")
        meta = c["meta"]
        check_eq(sorted(meta), ["import", "labels", "provenance"], f"{c['id']}: the sections")
        check_eq(meta["provenance"]["builtin"], {"set": "library", "id": c["id"], "sha256": c["sha256"]}, f"{c['id']}: builtin")
        check_eq(meta["provenance"]["licence"], "CC0-1.0", f"{c['id']}: licence")
        check(meta["provenance"].get("author") and meta["provenance"].get("url"), f"{c['id']}: author and URL")

    # the v3 form: `import` in place of `meta`, and the excluded clip back
    v3 = dict(m)
    v3["version"] = 3
    clips = []
    for c in m["clips"]:
        old = {}
        for key, value in c.items():
            if key == "meta":
                fades = fades_of(c)
                if fades:
                    old["import"] = fades
                continue
            old[key] = value
        clips.append(old)
    excluded = dict(clips[0], id=sel["excluded"][0]["id"], path=sel["excluded"][0]["id"] + ".ogg", type="pickup", tags=["pickup"])
    v3["clips"] = sorted(clips + [excluded], key=lambda c: c["id"])
    import tempfile
    with tempfile.TemporaryDirectory() as tmp:
        source, out = Path(tmp) / "v3.json", Path(tmp) / "v4.json"
        source.write_text(json.dumps(v3, indent=1, ensure_ascii=False) + "\n", encoding="utf-8")
        for attempt in range(2):
            r = subprocess.run([sys.executable, str(BUILD), "migrate-manifest", "--input", str(source), "--output", str(out)],
                               capture_output=True, text=True)
            check_eq(r.returncode, 0, f"migrate-manifest: {r.stderr}")
            check_eq(out.read_text(encoding="utf-8"), text, f"run {attempt + 1} reproduces the committed v4")
