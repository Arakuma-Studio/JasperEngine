"""S36 AC9: the starter library's reader, CLI, staging and install. Every check runs on a mini library
(tools/mcp/sound_library_refs.py: 8 clips over 4 types, encoded by ffmpeg or soundfile), plus the install tree.
"""

from __future__ import annotations

CI = True

import atexit
import hashlib
import json
import os
import re
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

import yaml

import harness
from harness import Editor, skip

sys.path.insert(0, str(Path(__file__).resolve().parent.parent))
sys.path.insert(0, str(Path(__file__).parent))
import audio_refs as ar  # noqa: E402
import sound_library_refs as slr  # noqa: E402
from test_runtime_regressions import decode_png  # noqa: E402

TIMEOUT = 1500
REPO = Path(__file__).resolve().parents[3]
ESSENTIALS = REPO / "editor/resources/sound-library/essentials"
REPO_LIBRARY = REPO / "editor/resources/sound-library/library"
SOUND_TYPES = REPO / "editor/audio/SoundTypes.h"
STAGING_SCRIPT = REPO / "CopySoundLibrary.cmake"

LOOP = "ambience/fs_200003_drone_bed"      # import {fade_in_ms: 2}: its first frame clicks without it
FLIPPED = "click/kenney_click_1"
OTHER = "click/kenney_click_2"
KIND_DIFFERS = "chime/kenney_chime_1"   # kind transient, its type's is tonal

STATE: dict = {}


def scratch(prefix: str) -> Path:
    path = Path(tempfile.mkdtemp(prefix=prefix))
    atexit.register(shutil.rmtree, path, True)
    return path


def editor() -> list:
    return harness.editor_command()


def mini() -> slr.MiniLibrary:
    """the mini library, built once per run (with its analysis.json from the CLI)"""
    if "lib" not in STATE:
        STATE["lib"] = slr.build(scratch("jasper-p5core-") / "library", editor())
    return STATE["lib"]


def fresh(name: str) -> slr.MiniLibrary:
    """a copy of the mini library to tamper with"""
    return mini().copy(scratch("jasper-p5core-" + name + "-") / "library")


class Run:
    def __init__(self, proc: subprocess.CompletedProcess):
        self.code = proc.returncode
        self.stdout = proc.stdout
        self.stderr = proc.stderr.decode("utf-8", "replace")
        try:
            self.json = json.loads(proc.stdout.decode("utf-8"))
        except ValueError:
            self.json = None

    def __repr__(self) -> str:
        return f"exit {self.code}; stdout {self.stdout[:2000]!r}; stderr {self.stderr[-1500:]!r}"


def cli(*args, library: Path | None = None, timeout: float = 900) -> Run:
    """the editor binary with args; JASPERENGINE_SOUND_LIBRARY_DIR is the library given, or unset"""
    env = dict(os.environ)
    env.pop(slr.ENV_DIR, None)
    if library is not None:
        env[slr.ENV_DIR] = str(library)
    return Run(subprocess.run(editor() + [str(a) for a in args], capture_output=True, env=env, timeout=timeout,
                              stdin=subprocess.DEVNULL))


def check(cond, message: str) -> None:
    if not cond:
        raise harness.TestFailure(message)


def check_eq(actual, expected, message: str) -> None:
    if actual != expected:
        raise harness.TestFailure(f"{message}: got {actual!r}, expected {expected!r}")


def without(item: dict, *keys) -> dict:
    return {k: v for k, v in item.items() if k not in keys}


def matches(clip: dict, words: list) -> bool:
    """spec 2.6.3's --query: every word in the id, the tags or the description, any case"""
    hay = "\n".join([clip["id"], clip["description"], *clip["tags"]]).lower()
    return all(w.lower() in hay for w in words)


# -- R-1 ---------------------------------------------------------------------------------------------------


async def test_r1_resolver():
    """Without the variable the editor finds sound-library next to its executable (bin/sound-library of its
    install); the variable points it elsewhere, and --dir over both."""
    lib = mini()
    exe = Path(editor()[0]).resolve()
    beside = exe.parent / "sound-library"
    r = cli("sound-library", "list", "--json")
    check(r.json is not None, f"list --json prints JSON: {r}")
    check_eq(r.json["root"], beside.as_posix(), "without the variable: the folder next to the editor")
    if (REPO_LIBRARY / "manifest.json").exists():
        check_eq((r.code, r.json["available"]), (0, True), f"the install has the committed library: {r}")
        check_eq(slr.tree(beside), {k: v for k, v in slr.tree(REPO_LIBRARY).items() if k != ".gitattributes"},
                 "bin/sound-library is the repository's library, file for file")
    else:
        # no library is committed yet (snd/p5-curate writes it): the install has none, and says so
        check_eq((r.code, r.json["available"]), (2, False), f"no library to install yet: {r}")
        check(not beside.exists(), f"the install holds no stale sound-library: {beside}")
        check(beside.as_posix() in r.json["error"] and slr.ENV_DIR in r.json["error"], f"the reason names the path and the variable: {r.json}")

    r = cli("sound-library", "list", "--json", library=lib.root)
    check_eq((r.code, r.json["root"], len(r.json["clips"])), (0, lib.root.as_posix(), 8), f"the variable's library: {r}")
    other = fresh("dir")
    r = cli("sound-library", "list", "--json", "--dir", other.root, library=lib.root)
    check_eq((r.code, r.json["root"]), (0, other.root.as_posix()), f"--dir over the variable: {r}")
    # a relative variable is made absolute against the working directory
    rel = cli("sound-library", "verify", "--json", library=Path(os.path.relpath(lib.root)))
    check_eq((rel.code, rel.json["root"]), (0, lib.root.resolve().as_posix()), f"a relative variable: {rel}")


# -- R-2 ---------------------------------------------------------------------------------------------------


async def test_r2_verify_and_list():
    lib = mini()
    m = lib.manifest
    total = sum(c["bytes"] for c in m["clips"])
    r = cli("sound-library", "verify", "--json", library=lib.root)
    check_eq(r.code, 0, f"verify passes on the mini library: {r}")
    check_eq(r.json, {"ok": True, "root": lib.root.as_posix(), "version": 4, "clips": 8, "bytes": total, "licences": 1, "issues": []},
             "verify --json")
    r = cli("sound-library", "verify", library=lib.root)
    check_eq((r.code, r.stdout.decode()), (0, f"ok: 8 clips, {total} bytes, version 4, 1 licence\n"), "verify's text")

    r = cli("sound-library", "list", "--json", library=lib.root)
    check_eq(r.code, 0, f"list: {r}")
    check_eq(r.json["clips"], m["clips"], "list --json: the manifest's clip objects, in id order")
    check_eq((r.json["version"], r.json["missing"], r.json["bytes"], r.json["analysis_current"]), (4, 0, total, True),
             "list --json: version, missing, bytes and a current analysis.json")
    r = cli("sound-library", "list", "--json", "--type", "click", library=lib.root)
    check_eq([c["id"] for c in r.json["clips"]], [c["id"] for c in m["clips"] if c["type"] == "click"], "list --type click")
    for query in ("glass soft", "OUTDOOR", "footstep wood", "chime", "nothing"):
        r = cli("sound-library", "list", "--json", "--query", query, library=lib.root)
        want = [c["id"] for c in m["clips"] if matches(c, query.split())]
        check_eq((r.code, [c["id"] for c in r.json["clips"]]), (0, want), f"list --query {query!r}")
    r = cli("sound-library", "list", "--type", "footstep", "--query", "gravel", library=lib.root)
    lines = r.stdout.decode().splitlines()
    check_eq(r.code, 0, f"list's text: {r}")
    check(re.fullmatch(rf"Starter library v4 at {re.escape(lib.root.as_posix())}: 8 clips, \d+\.\d MiB", lines[0]), f"the header: {lines[0]!r}")
    gravel = lib.clip("footstep/fs_200002_gravel")
    want = (f"library:footstep/fs_200002_gravel  footstep/transient  {gravel['measured']['duration_s']:.2f} s  "
            f"{gravel['measured']['momentary_max_lufs']:.1f} LUFS-M  freesound tester")
    check_eq(lines[1:], [want], "one line per clip")

    # exit codes: 0 / 1 (a failed verify) / 2 (an error or a usage error), help 0
    for args, code in ((("sound-library",), 2), (("sound-library", "frob"), 2), (("sound-library", "list", "--bogus"), 2),
                       (("sound-library", "verify", "--type", "click"), 2), (("sound-library", "list", "--type", "nope"), 2),
                       (("sound-library", "list", "--dir"), 2), (("audio-analyze",), 2), (("audio-analyze", "--bogus", "x.wav"), 2),
                       (("audio-analyze", "--detail", "huge", "x.wav"), 2), (("audio-analyze", "--kind", "loop", "x.wav"), 2),
                       (("audio-analyze", "--type", "nope", "x.wav"), 2), (("audio-analyze", "--fade-in-ms", "2000", "x.wav"), 2),
                       (("audio-analyze", "--image", "sheet", "x.wav"), 2), (("audio-analyze", "--list-types", "x.wav"), 2),
                       (("audio-analyze", "--fade-in-ms", "2", "library:" + LOOP), 2)):
        r = cli(*args, library=lib.root)
        check_eq(r.code, code, f"{' '.join(args)}: {r}")
        check(r.stdout == b"" and r.stderr.strip(), f"{' '.join(args)}: the usage error on stderr, nothing on stdout: {r}")
    for args in (("sound-library", "--help"), ("audio-analyze", "--help"), ("sound-library", "list", "-h")):
        r = cli(*args)
        check(r.code == 0 and b"Exit codes" in r.stdout, f"{' '.join(args)}: the help with its exit codes: {r}")
    r = cli("--help")
    check(r.code == 0 and b" audio-analyze [options]" in r.stdout and b" sound-library verify|list" in r.stdout, f"the main usage: {r}")
    missing = scratch("jasper-p5core-none-") / "nowhere"
    r = cli("sound-library", "list", "--json", library=missing)
    check_eq((r.code, r.json["available"]), (2, False), f"list of a missing library exits 2: {r}")
    # the usage text's Installed line describes the library --dir names, not the default (the closure's I-3)
    here = f"Installed: {len(lib.ids)} clips, version {lib.manifest['version']}, at {Path(lib.root).absolute().as_posix()}."
    r = cli("sound-library", "list", "--dir", lib.root, "--bogus", library=missing)
    check(r.code == 2 and here in r.stderr, f"list --dir <mini> --bogus names the mini library ({here}): {r}")
    r = cli("sound-library", "list", "--bogus", "--dir", missing, library=lib.root)
    check(r.code == 2 and f"Installed: none. Starter library not found at {missing.as_posix()}" in r.stderr,
          f"list --bogus --dir <missing> says that folder has none: {r}")
    r = cli("sound-library", "verify", "--dir", lib.root, "--help", library=missing)
    check(r.code == 0 and here.encode() in r.stdout, f"verify --dir <mini> --help names the mini library: {r}")


# -- R-3 ---------------------------------------------------------------------------------------------------


def verify_issues(lib: slr.MiniLibrary) -> tuple:
    r = cli("sound-library", "verify", "--json", library=lib.root)
    check(r.json is not None, f"verify --json: {r}")
    return r.code, {i["path"]: i["problem"] for i in r.json["issues"]}


def analyze(lib: slr.MiniLibrary, *refs) -> Run:
    return cli("audio-analyze", "--json", *refs, library=lib.root)


async def test_r3_flipped_byte():
    lib = fresh("flip")
    lib.flip_byte(FLIPPED)
    code, issues = verify_issues(lib)
    check_eq(code, 1, "verify fails")
    check(set(issues) == {FLIPPED + ".ogg"} and "SHA-256" in issues[FLIPPED + ".ogg"], f"naming the clip and its hash: {issues}")
    r = analyze(lib, "library:" + FLIPPED, "library:" + OTHER)
    check_eq(r.code, 2, f"audio-analyze with a failed input exits 2: {r}")
    check_eq([e["path"] for e in r.json["errors"]], ["library:" + FLIPPED], "the tampered clip is an error")
    want = hashlib.sha256(lib.path(FLIPPED).read_bytes()).hexdigest()
    error = r.json["errors"][0]["error"]
    check(f"library:{FLIPPED} does not match its manifest (SHA-256 {want}, expected {lib.clip(FLIPPED)['sha256']})" in error and
          "reinstall the editor" in error, f"the read's text: {error}")
    check_eq([(i["n"], i["path"]) for i in r.json["items"]], [(1, "library:" + OTHER)], "the other clip of the call is analysed")
    r = cli("sound-library", "list", "--json", library=lib.root)
    check(FLIPPED in [c["id"] for c in r.json["clips"]], "a changed byte is not seen at open (no hashing): the clip is listed")


async def test_r3_extra_files():
    lib = fresh("extra")
    lib.add_file("click/stray.ogg", b"OggS stray")
    lib.add_file("notes.txt")
    lib.add_file("licenses/sub/deep.txt")
    code, issues = verify_issues(lib)
    check_eq(code, 1, "verify fails")
    check_eq(issues, {"click/stray.ogg": "not in the manifest", "notes.txt": "not in the manifest", "licenses/sub/deep.txt": "not in the manifest"},
             "each file not in the manifest")
    r = cli("sound-library", "verify", library=lib.root)
    check_eq(r.stdout.decode().splitlines(), ["click/stray.ogg: not in the manifest", "licenses/sub/deep.txt: not in the manifest",
                                              "notes.txt: not in the manifest"], "verify's text: one line per issue, by path")


async def test_r3_missing_clip():
    lib = fresh("missing")
    gone = "footstep/kenney_wood_1"
    lib.drop_file(gone)
    r = cli("sound-library", "list", "--json", library=lib.root)
    check_eq((r.code, r.json["missing"]), (0, 1), f"the status counts it missing: {r}")
    check_eq([c["id"] for c in r.json["clips"]], [i for i in lib.ids if i != gone], "and lists the others")
    code, issues = verify_issues(lib)
    check_eq((code, issues), (1, {gone + ".ogg": "missing"}), "verify names it")
    r = analyze(lib, "library:footstep/*")
    check_eq((r.code, [i["path"] for i in r.json["items"]]), (0, ["library:footstep/fs_200002_gravel"]), "the type's other clip")
    r = analyze(lib, "library:" + gone, "library:" + OTHER)
    check_eq([i["path"] for i in r.json["items"]], ["library:" + OTHER], "others work")
    check("is missing from the installed library" in r.json["errors"][0]["error"], f"the missing one fails: {r.json['errors']}")


async def test_r3_symlinked_clip():
    lib = fresh("link")
    target = scratch("jasper-p5core-target-") / "same.ogg"
    shutil.copyfile(lib.path(OTHER), target)          # the same bytes: only the link policy can refuse it
    lib.symlink_clip(OTHER, target)
    r = cli("sound-library", "list", "--json", library=lib.root)
    check(OTHER not in [c["id"] for c in r.json["clips"]] and r.json["missing"] == 1, f"a link counts as missing: {r}")
    code, issues = verify_issues(lib)
    check(code == 1 and "symbolic link" in issues.get(OTHER + ".ogg", ""), f"verify names the link: {issues}")
    r = analyze(lib, "library:" + OTHER)
    check(r.code == 2 and not r.json["items"] and "missing" in r.json["errors"][0]["error"], f"never read: {r}")
    # a type folder that is a link is refused too
    folder = fresh("folderlink")
    real = scratch("jasper-p5core-realdir-") / "chime"
    shutil.move(str(folder.root / "chime"), real)
    os.symlink(real, folder.root / "chime")
    r = cli("sound-library", "list", "--json", library=folder.root)
    check(r.json["missing"] == 2 and not any(c["type"] == "chime" for c in r.json["clips"]), f"a linked folder's clips are missing: {r}")


async def test_r3_invalid_manifest():
    cases = []
    lib = fresh("dotdot")
    lib.set_clip_key(OTHER, "path", "../outside.ogg")
    cases.append((lib, "path is click/kenney_click_2.ogg, not ../outside.ogg"))
    lib = fresh("absolute")
    lib.set_clip_key(OTHER, "path", "/tmp/outside.ogg")
    cases.append((lib, "not /tmp/outside.ogg"))
    lib = fresh("badid")
    lib.rewrite(lambda m: m["clips"][0].update(id="Ambience/Bad Id", path="Ambience/Bad Id.ogg"))
    cases.append((lib, "the id is <group>/<name>"))
    lib = fresh("truncated")
    lib.truncate_manifest()
    cases.append((lib, "not valid JSON"))
    lib = fresh("format")
    lib.rewrite(lambda m: m.update(format="jasperengine-sound-essentials"))
    cases.append((lib, "format is jasperengine-sound-essentials, not jasperengine-sound-library"))
    lib = fresh("semantic")
    lib.set_clip_key(OTHER, "semantic", {"label": "ok", "heard": ["a", "b", "c", "d"]})
    cases.append((lib, "heard is a list of at most 3 texts"))
    lib = fresh("import")
    lib.set_clip_key(OTHER, "meta", {"import": {}, "provenance": {}, "extra": {}})
    cases.append((lib, "only import, provenance and labels"))
    for lib, reason in cases:
        r = cli("sound-library", "list", "--json", library=lib.root)
        check(r.code == 2 and r.json["available"] is False, f"{reason}: the library is unavailable: {r}")
        check(reason in r.json["error"] and "invalid manifest.json" in r.json["error"] and lib.root.as_posix() in r.json["error"],
              f"the reason: {r.json['error']}")
        code, issues = verify_issues(lib)
        check(code == 1 and len(issues) == 1 and reason in list(issues.values())[0], f"verify: one issue, the reason: {issues}")
        r = analyze(lib, "library:" + OTHER)
        check(r.code == 2 and reason in r.json["errors"][0]["error"], f"a reference fails with the reason: {r}")

    # the rest of the editor works: search_sounds still finds the project's clips and the essentials
    broken = cases[3][0]
    async with Editor(create=True, xvfb="harness", env=broken.env(), label="p5core-broken") as ed:
        ar.write_wav(ed.project / "footstep_own.wav", [ar.sine(660.0, 0.5, 48000, -12.0)], 48000)
        await ed.call("sync_files", paths=["footstep_own.wav"])
        got = await ed.call("search_sounds", query="footstep", limit=200, timeout=900)
        sources = {r.get("source") for r in got["results"]}
        check("essentials" in sources, f"essentials results with a broken library: {sources}")
        everything = await ed.call("search_sounds", limit=200, timeout=900)
        check({"project", "essentials"} <= {r.get("source") for r in everything["results"]},
              f"scope all: project and essentials results: {[r.get('path') or r.get('id') for r in everything['results']]}")


async def test_r3_no_directory():
    missing = scratch("jasper-p5core-gone-") / "not-here"
    r = cli("sound-library", "list", "--json", library=missing)
    want = f"Starter library not found at {missing.as_posix()} (reinstall the editor, or set {slr.ENV_DIR})"
    check_eq((r.code, r.json), (2, {"available": False, "root": missing.as_posix(), "error": want}), "list of a missing folder")
    r = cli("sound-library", "verify", "--json", library=missing)
    check_eq((r.code, r.json["ok"], r.json["issues"]), (1, False, [{"path": missing.as_posix(), "problem": want}]), "verify: one issue")
    r = cli("audio-analyze", "--json", "library:click/*", "essentials:ui/click", library=missing)
    check(r.code == 2 and want in r.json["errors"][0]["error"] and [i["path"] for i in r.json["items"]] == ["essentials:ui/click"],
          f"library references fail with the text, essentials work: {r}")


# -- R-4 ---------------------------------------------------------------------------------------------------


def write_meta(path: Path, sections: dict) -> None:
    """an asset's .meta holding these sections, as an import writes it"""
    Path(str(path) + ".meta").write_text(yaml.safe_dump({"metaVersion": 1, "guid": hashlib.md5(str(path).encode()).hexdigest(), **sections}, sort_keys=False))


async def test_r4_audio_analyze():
    """audio-analyze's items are analyze_audio's for the same inputs in a project at the default settings
    (same editor, two code paths): files, essentials (one with manifest fades) and a library loop with
    import {fade_in_ms: 2}, which analyze_audio sees as the same file imported with the sidecar an import
    writes (labels and kind through its library.id, spec A1/A3)."""
    lib = mini()
    async with Editor(create=True, xvfb="harness", env=lib.env(), label="p5core-r4") as ed:
        sounds = ed.project / "assets" / "sounds"
        sounds.mkdir(parents=True, exist_ok=True)
        tone = ar.write_wav(sounds / "tone.wav", [ar.sine(660.0, 0.5, 48000, -12.0)], 48000)
        click = sounds / "click.ogg"
        shutil.copyfile(lib.path(OTHER), click)
        faded = ar.write_wav(sounds / "faded.wav", [ar.sine(330.0, 0.6, 44100, -9.0, phase=1.2)], 44100)
        write_meta(faded, {"import": {"audioImporter": {"version": 1, "fade_in_ms": 2}}})
        bed = sounds / "bed.ogg"
        shutil.copyfile(lib.path(LOOP), bed)
        clip = lib.clip(LOOP)
        write_meta(bed, clip["meta"])

        async def mcp(paths: list) -> list:
            return (await ed.call("analyze_audio", paths=paths, image="none", timeout=900))["items"]

        def compare(cli_items: list, mcp_items: list, what: str, drop=("n", "path", "differences_vs_1", "import")) -> None:
            check_eq(len(cli_items), len(mcp_items), f"{what}: as many items")
            for a, b in zip(cli_items, mcp_items):
                check_eq(without(a, *drop), without(b, *drop), f"{what}: {a['path']} against {b['path']}")

        # files without sidecars
        r = cli("audio-analyze", "--json", tone, click, library=lib.root)
        check_eq((r.code, r.json["errors"]), (0, []), f"files: {r}")
        check_eq([(i["n"], i["path"]) for i in r.json["items"]], [(1, str(tone)), (2, str(click))], "items numbered from 1, paths as given")
        compare(r.json["items"], await mcp(["assets/sounds/tone.wav", "assets/sounds/click.ogg"]), "files")
        check(all(i["kind_source"] == "measured" and "type" not in i for i in r.json["items"]), "without --type: a measured kind, no type")

        # essentials: one with manifest fades, one without
        essentials = json.loads((ESSENTIALS / "manifest.json").read_text())["clips"]
        importer = lambda c: c["meta"]["import"]["audioImporter"]  # noqa: E731
        with_fades = next(c["id"] for c in essentials if "fade_in_ms" in importer(c) and "fade_out_ms" in importer(c))
        plain = next(c["id"] for c in essentials if "fade_in_ms" not in importer(c) and "fade_out_ms" not in importer(c))
        refs = [f"essentials:{with_fades}", f"essentials:{plain}"]
        r = cli("audio-analyze", "--json", *refs, library=lib.root)
        check_eq(r.code, 0, f"essentials: {r}")
        compare(r.json["items"], await mcp(refs), "essentials")
        check("played" in r.json["items"][0] and "played" not in r.json["items"][1], "a clip with manifest fades is measured as it plays")

        # the library loop: its manifest's fade-in is the played region
        r = cli("audio-analyze", "--json", "library:" + LOOP, library=lib.root)
        check_eq(r.code, 0, f"library: {r}")
        item = r.json["items"][0]
        check_eq(item["played"]["fade_in_frames"], 96, "the manifest's fade at 48 kHz")
        check_eq((item["path"], item["type"], item["kind"], item["kind_source"], item["labels"]),
                 ("library:" + LOOP, "ambience", "loop", "type", clip["tags"]), "labels from the manifest")
        check_eq((item["licence"]["id"], item["licence"]["author"], item["licence"]["attribution"]), ("CC0-1.0", "tester", "tester (freesound.org)"),
                 "the licence an import writes")
        other = (await mcp(["assets/sounds/bed.ogg"]))[0]
        check_eq(other["import"]["fade_in_frames"], 96, "analyze_audio plays the meta's 2 ms at 48 kHz")
        # the tempo grid (S39 R17) is detected from the file by both; what reads the clip's stored tempo (its
        # .meta, a project's) is the tool's alone
        stored = ("current", "agrees", "loop_beats", "notes")
        ed_music, cli_music = other.get("music"), item.get("music")
        check(isinstance(cli_music, dict) and cli_music.get("candidates") is not None, f"the CLI reports the loop's grid: {cli_music}")
        check_eq(without(cli_music, *stored), without(ed_music, *stored), "the same detected grid")
        compare([item], [other], "the library loop against its imported file", drop=("n", "path", "differences_vs_1", "import", "music"))
        check(item["defects"]["region"]["fade_in_frames"] == 96, f"the played region's defects: {item['defects'].get('region')}")
        raw = cli("audio-analyze", "--json", "--type", "ambience", lib.path(LOOP), library=lib.root).json["items"][0]
        check("edge_click" in [n["id"] for n in raw["notes"]], f"the file as it is clicks at its start: {raw['notes']}")
        check("edge_click" not in [n["id"] for n in item["notes"]], f"its played region does not: {item['notes']}")

        # the built-in block of a .meta gives the manifest's kind
        chime = lib.clip(KIND_DIFFERS)
        copied = sounds / "chime.ogg"
        shutil.copyfile(lib.path(KIND_DIFFERS), copied)
        write_meta(copied, chime["meta"])
        b = (await mcp(["assets/sounds/chime.ogg"]))[0]
        check_eq((b["type"], b["kind"], b["kind_source"], b["labels"]), ("chime", "transient", "type", chime["meta"]["labels"]),
                 "the manifest's type, its own kind (not chime's tonal)")
        a = cli("audio-analyze", "--json", "library:" + KIND_DIFFERS, library=lib.root).json["items"][0]
        check_eq(a["kind"], b["kind"], "the library clip against its imported file")
        info = await ed.call("get_asset_info", path="assets/sounds/chime.ogg")
        check_eq(info["audio"]["provenance"]["builtin"], {"set": "library", "id": KIND_DIFFERS, "sha256": chime["sha256"]},
                 "get_asset_info shows the built-in block")

        # --fade-in-ms on a file is an importer with fade_in_ms
        r = cli("audio-analyze", "--json", "--fade-in-ms", "2", faded, library=lib.root)
        a, b = r.json["items"][0], (await mcp(["assets/sounds/faded.wav"]))[0]
        compare([a], [b], "--fade-in-ms 2 against a meta's fade_in_ms: 2")

        # --type and --kind
        r = cli("audio-analyze", "--json", "--type", "footstep", tone, library=lib.root)
        i = r.json["items"][0]
        check_eq((i["type"], i["kind"], i["kind_source"]), ("footstep", "transient", "type"), "--type sets the type and kind_source")
        r = cli("audio-analyze", "--json", "--type", "door", "--kind", "transient", tone, library=lib.root)
        i = r.json["items"][0]
        check_eq((i["type"], i["kind"], i["kind_source"]), ("door", "transient", "type"), "--type door --kind transient")
        r = cli("audio-analyze", "--json", "--type", "door", tone, library=lib.root)
        check_eq(r.json["items"][0]["kind"], "swell", "door's own kind without --kind")

        # --image sheet: analyze_audio's sheet for the same items
        png = scratch("jasper-p5core-png-") / "sheet.png"
        r = cli("audio-analyze", "--json", "--image", "sheet", "--png", png, tone, click, library=lib.root)
        check_eq(r.code, 0, f"--image sheet: {r}")
        width, height = decode_png(png.read_bytes())[:2]
        raw = await ed.call_raw("analyze_audio", {"paths": ["assets/sounds/tone.wav", "assets/sounds/click.ogg"], "image": "sheet"},
                                timeout=900)
        check(not raw.is_error and raw.images, f"analyze_audio's sheet: {raw.text[:300]}")
        check_eq((width, height), decode_png(raw.images[0][0])[:2], "the sheet's size is analyze_audio's")
        check_eq((r.json["image"]["width"], r.json["image"]["height"], width), (width, height, 768), "the default width, reported")
        r = cli("audio-analyze", "--image", "waveform", "--png", png, "--max-width", "512", tone, library=lib.root)
        check_eq((r.code, decode_png(png.read_bytes())[0]), (0, 512), f"a waveform at --max-width 512: {r}")

    # the text lines, the list of types, determinism, a bad file
    r = cli("audio-analyze", "library:" + LOOP, library=lib.root)
    check(r.code == 0 and r.stdout.decode().startswith(f"1. library:{LOOP}: 4.50 s, "), f"one line per item: {r}")
    r = cli("audio-analyze", "--list-types", "--json")
    check_eq(r.code, 0, f"--list-types: {r}")
    table = {}
    for m in re.finditer(r'\{"(\w+)", (TRANSIENT|TONAL|SWELL|RISER|GLITCH|LOOP|SPEECH), (-?\d+\.\d+), (true|false), (true|false)\}',
                         SOUND_TYPES.read_text()):
        table[m.group(1)] = {"type": m.group(1), "kind": m.group(2).lower(), "target_lufs": float(m.group(3)),
                             "measure": "integrated" if m.group(4) == "true" else "momentary_max"}
    check_eq(len(table), 31, "SoundTypes.h's table read")
    check_eq(r.json["analyzer_version"], 1, "the analyzer version")
    check_eq({t["type"]: t for t in r.json["types"]}, table, "the 31 types with SoundTypes.h's kinds, targets and measures")
    check_eq([t["target_lufs"] for t in r.json["types"] if t["type"] in ("click", "explosion", "hurt", "shoot")], [-32.0, -18.0, -24.0, -21.0],
             "the four lowered targets")
    once = cli("audio-analyze", "--json", "library:*", "essentials:footstep/*", library=lib.root)
    again = cli("audio-analyze", "--json", "library:*", "essentials:footstep/*", library=lib.root)
    check(once.code == 0 and len(once.json["items"]) == 14 and once.stdout == again.stdout, "two runs print the same bytes")
    bad = scratch("jasper-p5core-bad-") / "broken.wav"
    bad.write_bytes(b"RIFF\x00\x00\x00\x00WAVEjunk")
    r = cli("audio-analyze", "--json", bad, "library:" + OTHER, library=lib.root)
    check_eq(r.code, 2, f"a bad file exits 2 (D4): {r}")
    check(r.json["errors"][0]["path"] == str(bad) and [i["path"] for i in r.json["items"]] == ["library:" + OTHER],
          f"an errors[] entry, the other input analysed: {r.json['errors']}")

    # analysis.json as the helper wrote it from the CLI is what the reader calls current
    analysis = json.loads((lib.root / "analysis.json").read_text())
    check_eq((analysis["format"], analysis["analyzer_version"], sorted(analysis["items"])), (slr.ANALYSIS_FORMAT, 1, sorted(lib.ids)),
             "analysis.json")
    for cid, it in analysis["items"].items():
        check_eq(it["sha1"], hashlib.sha1(lib.path(cid).read_bytes()).hexdigest(), f"{cid}: the item's sha1 is the file's")
    stale = fresh("stale")
    text = (stale.root / "analysis.json").read_text().replace('"analyzer_version": 1', '"analyzer_version": 99', 1)
    (stale.root / "analysis.json").write_text(text)
    r = cli("sound-library", "list", "--json", library=stale.root)
    check_eq((r.code, r.json["analysis_current"]), (0, False), "another analyzer version: stale")
    (stale.root / "analysis.json").write_text("{not json")
    r = cli("sound-library", "list", "--json", library=stale.root)
    check_eq((r.code, r.json["analysis_current"]), (0, False), "an unreadable analysis.json: not current, the library still works")


# -- R-5 ---------------------------------------------------------------------------------------------------


def stage(source: Path, dest: Path, stamp: Path) -> None:
    cmake = shutil.which("cmake")
    if not cmake:
        skip("needs cmake")
    r = subprocess.run([cmake, f"-DSOUND_LIBRARY_SOURCE:PATH={source}", f"-DSOUND_LIBRARY_DEST:PATH={dest}",
                        f"-DSOUND_LIBRARY_STAMP:PATH={stamp}", "-P", str(STAGING_SCRIPT)], capture_output=True, text=True, timeout=300)
    check_eq(r.returncode, 0, f"cmake -P CopySoundLibrary.cmake: {r.stdout} {r.stderr}")


async def test_r5_staging():
    lib = fresh("stage")
    out = scratch("jasper-p5core-staged-")
    dest, stamp = out / "bin" / "sound-library", out / "sound-library-Release.stamp"
    stage(lib.root, dest, stamp)
    check_eq(slr.tree(dest), slr.tree(lib.root), "the destination holds the source tree")
    check(stamp.is_file(), "the stamp is written")
    (dest / "click" / "stale.ogg").write_bytes(b"left from an earlier build")
    lib.drop_clip("chime/kenney_chime_1")
    stamp.unlink()
    stage(lib.root, dest, stamp)
    check_eq(slr.tree(dest), slr.tree(lib.root), "after a clip left the library: exactly the source again")
    check(not (dest / "chime" / "kenney_chime_1.ogg").exists() and stamp.is_file(), "the dropped clip is gone, the stamp written")
    r = cli("sound-library", "verify", "--json", "--dir", dest)
    check_eq((r.code, r.json["clips"]), (0, 7), f"the staged library verifies: {r}")
    # a tree without a library yet stages nothing (and removes what an earlier build staged)
    stage(out / "no-library", dest, stamp)
    check(not dest.exists() and stamp.is_file(), "no source library: no destination, the stamp written")
