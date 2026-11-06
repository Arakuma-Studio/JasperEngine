"""Sound layer, phase 5: the optional semantic audit stage of the starter library (snd/p5-earshot).

Phase 5 spec §3.6 P-12 (the stage's plumbing, calibration, file format and write's use of the
recorded file, with the fake embedder: unit vectors from SHA-256, no torch) and P-13 (the pinned
CLAP model on sndcompute, skipped without its files in $JASPER_SOUND_CACHE/clap/<revision>/).
Offline. P-12 works on sources made by test_sound_library_pipeline.py's generator; P-13 on six
real Kenney clips from the pinned Phase 1 zips in $JASPER_SOUND_CACHE.

Oracles: the file's bytes across runs, its layout re-derived with json (keys sorted, ids sorted),
hashlib for the encoded clips, the selection the test writes, and the manifest the plain-Python
write produces (no numpy, no torch, no stage).
"""

import asyncio
import hashlib
import json
import os
import shutil
import subprocess
import sys
import tempfile
import atexit
import urllib.request
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))

import test_sound_library_pipeline as P  # noqa: E402  (its sources, helpers and pinned uv command)

TOOLS = P.TOOLS
MODEL_FILES = ("config.json", "preprocessor_config.json", "tokenizer.json", "tokenizer_config.json", "special_tokens_map.json",
               "vocab.json", "merges.txt")
FAKE_MODEL = {"repo": "test/fake-clap", "revision": "f" * 40, "licence": "Apache-2.0", "weights": "pytorch_model.bin",
              "files": [{"name": n, "bytes": 1, "sha256": "0" * 64} for n in ("pytorch_model.bin",) + MODEL_FILES]}
# The model of §2.11.9: revision and weights pinned by the spec (Hugging Face API, 2026-10-17), the small files by
# size and git blob SHA-1 there and by the SHA-256 semantic-fetch first recorded (P5 §5.6).
REAL_MODEL = {
    "repo": "laion/larger_clap_general", "revision": "ada0c23a36c4e8582805bb38fec3905903f18b41", "licence": "Apache-2.0",
    "weights": "pytorch_model.bin",
    "files": [
        {"name": "pytorch_model.bin", "bytes": 776444665, "sha256": "314eb00cce6ad68d25237b8446b659ccdb136ed4672c1bca470f142f72455026"},
        {"name": "config.json", "bytes": 643, "sha256": "6268f76a067e3104bf9001e97d7dcc0fcfb16f3086cfb478ee72d95900d7fc1d",
         "blob_sha1": "19cd0d27ed9aa01b7154b32270a02f251b08d120"},
        {"name": "preprocessor_config.json", "bytes": 541, "sha256": "9739f58296aa6f9ac18008fd0150fb2649bc554985fbde86d0a4041c882ac753",
         "blob_sha1": "e8636819eeb1b19f1e5b6f572eca1a101144062e"},
        {"name": "tokenizer.json", "bytes": 2108774, "sha256": "dc239041d98de27ffc3975473a1a23e3db4c937b23c138c38bbc66588bd247e5",
         "blob_sha1": "15cffb1a334cf27ccef68b0b6e2bc6f74f1be219"},
        {"name": "tokenizer_config.json", "bytes": 1362, "sha256": "e2eb445cfdbf4711de620cbdf10478b0423950799e85652d9f28da47066ab86d",
         "blob_sha1": "6e172a6edd7cb77400dc898b7a2d4d7cc0dbd75d"},
        {"name": "special_tokens_map.json", "bytes": 280, "sha256": "06e405a36dfe4b9604f484f6a1e619af1a7f7d09e34a8555eb0b77b66318067f",
         "blob_sha1": "d5698132694f4f1bcff08fa7d937b1701812598e"},
        {"name": "vocab.json", "bytes": 798293, "sha256": "ed19656ea1707df69134c4af35c8ceda2cc9860bf2c3495026153a133670ab5e",
         "blob_sha1": "4ebe4bb3f3114daf2e4cc349f24873a1175a35d7"},
        {"name": "merges.txt", "bytes": 456318, "sha256": "1ce1664773c50f3e0cc8842619a93edc4624525b728b188a9e0be33b7726adc5",
         "blob_sha1": "226b0752cac7789c48f0cb3ec53eda48b7be36cc"},
    ]}
# P-13's six real clips (not essentials members): (pack, member, clip id, type, kind, description)
REAL_CLIPS = [
    ("impact-sounds", "Audio/footstep_snow_002.ogg", "footstep/kenney_snow_002", "footstep", "transient", "Footstep in snow"),
    ("impact-sounds", "Audio/footstep_wood_003.ogg", "footstep/kenney_wood_003", "footstep", "transient", "Footstep on wood"),
    ("impact-sounds", "Audio/impactMetal_heavy_001.ogg", "impact/kenney_metal_heavy_001", "impact", "transient", "Heavy metal impact"),
    ("interface-sounds", "Audio/click_003.ogg", "click/kenney_click_003", "click", "transient", "Interface click"),
    ("sci-fi-sounds", "Audio/laserLarge_001.ogg", "shoot/kenney_laser_large_001", "shoot", "transient", "Large laser shot"),
    ("rpg-audio", "Audio/doorOpen_1.ogg", "door/kenney_door_open_1", "door", "swell", "Wooden door opening"),
]
FOOTSTEP = "footstep/kenney_snow_002"
STATE = {}
# P-13 runs the real stage four times (two runs, --check, the bird-song selection), about 2.5 min each on one thread
TIMEOUT = 2400
CI_EXCLUDED = "the maintainers' semantic stage: needs its model cache (JASPER_SOUND_CACHE), not a commit gate"


async def pipeline(*args, plain=False):
    return await P.pipeline(*args, plain=plain)


def lines_of(path):
    return [json.loads(x) for x in Path(path).read_text().splitlines()]


async def setup_p12():
    """A small run of the pipeline's own sources (four one-shots and a bed), with a fake model block."""
    if STATE:
        return STATE
    s = await P.setup()
    root = Path(tempfile.mkdtemp(prefix="snd-p5-semantic-test-"))
    if not os.environ.get("JASPER_KEEP_SCRATCH"):
        atexit.register(shutil.rmtree, root, True)
    keep = {"click/kenney_click_good", "swish/kenney_swish_good", "riser/kenney_riser_good", "glitch/kenney_drop3",
            "chime/kenney_drop3", "ambience/fs_1001_bed", "ambience/fs_1003_rain"}
    sel = json.loads(json.dumps(s["sel"]))
    sel["sources"] = [src for src in sel["sources"] if any(c["id"] in keep for c in src["clips"])]
    sel["semantic"] = {"model": FAKE_MODEL, "required": False}
    sel_path = P.write_json(root / "selection.json", sel)
    none = root / "no-semantic.jsonl"
    code, out = await pipeline("all", "--selection", sel_path, "--cache", s["cache"], "--work", root / "work", "--out", root / "lib0",
                               "--audit", root / "audit0.jsonl", "--offline", "--semantic-file", none)
    assert code == 0, out
    assert not json.loads((root / "lib0/manifest.json").read_text())["clips"][0].get("semantic"), "no file, no semantic"
    STATE.update(root=root, sel=sel, sel_path=sel_path, cache=s["cache"], work=root / "work")
    return STATE


# ---------------------------------------------------------------------------------------------
# P-12 The stage with the fake embedder, and write's and verify's use of its file

async def test_p12_fake_stage_is_reproducible_and_sorted():
    st = await setup_p12()
    files = []
    for name in ("a", "b"):
        f = st["root"] / f"semantic-{name}.jsonl"
        code, out = await pipeline("semantic", "--selection", st["sel_path"], "--work", st["work"], "--embedder", "fake",
                                   "--semantic-file", f)
        assert code == 0 and "semantic: wrote" in out, out
        files.append(f)
    assert files[0].read_bytes() == files[1].read_bytes(), "two runs of the stage differ"
    rows = lines_of(files[0])
    raw = files[0].read_text().splitlines()
    assert all(r == json.dumps(json.loads(r), sort_keys=True, ensure_ascii=False) for r in raw), "keys not sorted"
    head = rows[0]
    assert head["line"] == "stage" and head["embedder"] == "fake" and head["threads"] == 1
    assert head["model"] == {"repo": FAKE_MODEL["repo"], "revision": FAKE_MODEL["revision"], "sha256": "0" * 64}, head["model"]
    assert head["limits"] == {"label_ok_z": 2.5, "label_weak_z": 2.0, "heard_z": 3.0, "heard_max": 3}, head["limits"]
    assert head["vocab_sha256"] == hashlib.sha256((TOOLS / "semantic-vocab.json").read_bytes()).hexdigest()
    # the reference: the Python-accepted Kenney clips of the selection and the 40 essentials, never a Freesound clip
    kenney = sorted(cid for src in st["sel"]["sources"] if src["source"] == "kenney" for c in src["clips"]
                    for cid in [c["id"]] if json.loads((st["work"] / f"{cid}.json").read_text())["verdict"] == "accepted")
    ess = json.loads((P.ESSENTIALS / "manifest.json").read_text())["clips"]
    ids = kenney + [f"essentials:{c['id']}" for c in ess]
    assert head["reference"] == {"count": len(ids), "ids_sha256": hashlib.sha256(("\n".join(ids) + "\n").encode()).hexdigest()}
    clips = rows[1:]
    encoded = sorted(p.relative_to(st["work"]).as_posix()[:-4] for p in st["work"].rglob("*.ogg"))
    assert [r["id"] for r in clips] == encoded, "one line per encoded clip, sorted by id"
    for r in clips:
        assert r["line"] == "clip" and r["audio_sha256"] == hashlib.sha256((st["work"] / f"{r['id']}.ogg").read_bytes()).hexdigest()
        assert r["label"]["verdict"] in ("ok", "weak", "review") and len(r["heard"]) <= 3
        assert all(round(v, 3) == v for v in (r["label"]["z"], r["label"]["margin"], *[h["z"] for h in r["heard"]]))
        clip = next(c for src in st["sel"]["sources"] for c in src["clips"] if c["id"] == r["id"])
        assert (r["type"], r["description"]) == (clip["type"], clip["description"])
    st["file"] = files[0]
    st["rows"] = clips
    # --check agrees with its own file, and lists a reading moved by more than 0.05
    code, out = await pipeline("semantic", "--selection", st["sel_path"], "--work", st["work"], "--embedder", "fake",
                               "--semantic-file", files[0], "--check")
    assert code == 0 and f"{len(clips)} clips agree" in out, out
    moved = st["root"] / "semantic-moved.jsonl"
    rows2 = lines_of(files[0])
    rows2[1]["label"]["z"] = round(rows2[1]["label"]["z"] + 0.2, 3)
    moved.write_text("".join(json.dumps(r, sort_keys=True, ensure_ascii=False) + "\n" for r in rows2))
    code, out = await pipeline("semantic", "--selection", st["sel_path"], "--work", st["work"], "--embedder", "fake",
                               "--semantic-file", moved, "--check")
    assert code == 1 and f"{rows2[1]['id']}: label z" in out, out


async def test_p12_write_reads_only_the_file():
    st = STATE
    if "file" not in st:
        from harness import skip
        skip("needs the stage's run")
    f, rows = st["file"], {r["id"]: r for r in st["rows"]}
    out_dir, audit = st["root"] / "lib1", st["root"] / "audit1.jsonl"

    async def write(sel_path, out=out_dir, fake=True, semantic_file=f):
        args = ["write", "--selection", sel_path, "--work", st["work"], "--out", out, "--audit", audit, "--semantic-file", semantic_file]
        return await pipeline(*(args + (["--allow-fake-semantic"] if fake else [])), plain=True)

    # the fake embedder's file is for tests only
    code, out = await write(st["sel_path"], fake=False)
    assert code == 1 and "fake" in out and "--allow-fake-semantic" in out and not out_dir.exists(), out
    # a clip read as review needs the curator's review_note before write accepts it
    manifest0 = {c["id"]: c for c in json.loads((st["root"] / "lib0/manifest.json").read_text())["clips"]}
    review = sorted(cid for cid in manifest0 if rows[cid]["label"]["verdict"] == "review")
    assert review, "the fake embedder marked no accepted clip review (the test needs one)"
    code, out = await write(st["sel_path"])
    assert code == 1 and "write refused, nothing was written" in out and not out_dir.exists(), out
    for cid in review:
        assert f"{cid}: the semantic stage reads it as review" in out, out
    sel = json.loads(json.dumps(st["sel"]))
    for src in sel["sources"]:
        for c in src["clips"]:
            if c["id"] in review:
                c["review_note"] = f"Kept: the test's fake embedder reads noise ({c['id']})."
    noted = P.write_json(st["root"] / "selection-noted.json", sel)
    # plain Python (no numpy, no torch, no stage environment): write takes each reading from the file alone
    code, out = await write(noted)
    assert code == 0 and ": ok" in out, out
    manifest = {c["id"]: c for c in json.loads((out_dir / "manifest.json").read_text())["clips"]}
    assert set(manifest) == set(manifest0)
    for cid, c in manifest.items():
        r = rows[cid]
        assert c["semantic"] == {"label": r["label"]["verdict"], "heard": [h["tag"] for h in r["heard"]]}, cid
        assert list(c)[-1] == "semantic" and list(c["semantic"]) == ["label", "heard"], list(c)
        assert {k: v for k, v in c.items() if k != "semantic"} == manifest0[cid], f"{cid}: only semantic is added"
    recs = P.audit_of(audit)
    for cid in review:
        assert recs[cid]["semantic"]["review_note"].startswith("Kept:") and recs[cid]["semantic"]["label"] == rows[cid]["label"]
    # one review_note taken away again: refused, naming that clip only
    gone = review[0]
    sel2 = json.loads(json.dumps(sel))
    for src in sel2["sources"]:
        for c in src["clips"]:
            if c["id"] == gone:
                del c["review_note"]
    code, out = await write(P.write_json(st["root"] / "selection-one-gone.json", sel2), out=st["root"] / "lib-gone")
    assert code == 1 and f"{gone}: the semantic stage reads it as review" in out, out
    assert all(f"{cid}: the semantic stage" not in out for cid in review[1:]), out
    st.update(noted=noted, noted_sel=sel, lib=out_dir, audit=audit)


async def test_p12_changed_clip_loses_its_reading():
    st = STATE
    if "lib" not in st:
        from harness import skip
        skip("needs the write")
    # a changed clip: the bed's crossfade changes its encoded bytes, so its line is no longer current
    work = st["root"] / "work-changed"
    shutil.rmtree(work, ignore_errors=True)
    shutil.copytree(st["work"], work)
    sel = json.loads(json.dumps(st["noted_sel"]))
    bed = "ambience/fs_1001_bed"
    for src in sel["sources"]:
        for c in src["clips"]:
            if c["id"] == bed:
                c["crossfade_ms"] = 400
    changed = P.write_json(st["root"] / "selection-changed.json", sel)
    code, out = await pipeline("prepare", "--selection", changed, "--cache", st["cache"], "--work", work, "--only", bed)
    assert code == 0, out
    old = hashlib.sha256((st["work"] / f"{bed}.ogg").read_bytes()).hexdigest()
    assert hashlib.sha256((work / f"{bed}.ogg").read_bytes()).hexdigest() != old
    out_dir = st["root"] / "lib-changed"
    code, out = await pipeline("write", "--selection", changed, "--work", work, "--out", out_dir, "--audit", st["root"] / "audit-changed.jsonl",
                               "--semantic-file", st["file"], "--allow-fake-semantic", plain=True)
    assert code == 0, out
    manifest = {c["id"]: c for c in json.loads((out_dir / "manifest.json").read_text())["clips"]}
    assert "semantic" not in manifest[bed] and all("semantic" in c for cid, c in manifest.items() if cid != bed), manifest[bed]
    # with semantic.required, verify (and write) name it
    sel["semantic"]["required"] = True
    required = P.write_json(st["root"] / "selection-required.json", sel)
    code, out = await pipeline("verify", "--selection", required, "--out", out_dir, "--audit", st["root"] / "audit-changed.jsonl",
                               "--semantic-file", st["file"], "--allow-fake-semantic", "--allow-no-analysis", plain=True)
    assert code == 1 and f"{bed}: no current semantic reading (semantic.required)" in out, out
    assert out.count("no current semantic reading") == 1, out
    code, out = await pipeline("write", "--selection", required, "--work", work, "--out", st["root"] / "lib-required",
                               "--audit", st["root"] / "audit-required.jsonl", "--semantic-file", st["file"], "--allow-fake-semantic",
                               plain=True)
    assert code == 1 and f"{bed}: no current semantic reading" in out and not (st["root"] / "lib-required").exists(), out


async def test_p12_verify_checks_the_file():
    st = STATE
    if "lib" not in st:
        from harness import skip
        skip("needs the write")

    async def verify(semantic_file, lib=None, fake=True):
        args = ["verify", "--selection", st["noted"], "--out", lib or st["lib"], "--audit", st["audit"], "--semantic-file", semantic_file,
                "--allow-no-analysis"]
        return await pipeline(*(args + (["--allow-fake-semantic"] if fake else [])), plain=True)

    code, out = await verify(st["file"])
    assert code == 0 and ": ok" in out, out
    code, out = await verify(st["file"], fake=False)
    assert code == 1 and "written by the 'fake' embedder" in out, out
    rows = lines_of(st["file"])

    def variant(name, rows):
        p = st["root"] / f"semantic-{name}.jsonl"
        p.write_text("".join(json.dumps(r, sort_keys=True, ensure_ascii=False) + "\n" for r in rows))
        return p

    unsorted = [rows[0], rows[2], rows[1]] + rows[3:]
    code, out = await verify(variant("unsorted", unsorted))
    assert code == 1 and "not sorted by id" in out, out
    pinned = json.loads(json.dumps(rows))
    pinned[0]["model"]["sha256"] = "1" * 64
    code, out = await verify(variant("pin", pinned))
    assert code == 1 and "model pin" in out, out
    lib = st["root"] / "lib-differs"
    shutil.rmtree(lib, ignore_errors=True)
    shutil.copytree(st["lib"], lib)
    m = json.loads((lib / "manifest.json").read_text())
    victim = m["clips"][0]
    victim["semantic"]["label"] = "ok" if victim["semantic"]["label"] != "ok" else "weak"
    (lib / "manifest.json").write_text(json.dumps(m, indent=1, ensure_ascii=False) + "\n")
    code, out = await verify(st["file"], lib=lib)
    assert code == 1 and f"{victim['id']}: manifest semantic" in out and "the semantic file gives" in out, out


async def test_p12_review_lists_the_readings():
    """review --semantic adds each candidate's recorded reading to the numbered listing (the sheet is the editor's)."""
    import harness
    st = STATE
    if "file" not in st:
        harness.skip("needs the stage's run")
    bl = P.import_build_library()
    editor = harness.editor_command()[0]
    info, why = bl.editor_types(editor)
    if info is None:
        harness.skip(f"review needs the editor's analyzer: {why}")
    png = st["root"] / "review-click.png"
    code, out = await pipeline("review", "--selection", st["sel_path"], "--work", st["work"], "--editor", editor, "--type", "click",
                               "--png", png, "--semantic", "--semantic-file", st["file"])
    assert code == 0 and png.is_file(), out
    r = {x["id"]: x for x in st["rows"]}["click/kenney_click_good"]
    line = next(x for x in out.splitlines() if "click/kenney_click_good" in x)
    assert f"[semantic: {r['label']['verdict']} z {r['label']['z']} margin {r['label']['margin']} vs {r['label']['confusion']!r};" in line, line
    code, out = await pipeline("review", "--selection", st["sel_path"], "--work", st["work"], "--editor", editor, "--type", "click",
                               "--png", png, "--semantic", "--semantic-file", st["root"] / "absent.jsonl")
    assert code == 1 and "review --semantic needs" in out, out


# ---------------------------------------------------------------------------------------------
# P-13 The real model (sndcompute only)

def real_cache():
    from harness import skip
    cache = os.environ.get("JASPER_SOUND_CACHE")
    if not cache:
        skip("kenney.nl no longer serves the pinned zips (S36 Deviations): set JASPER_SOUND_CACHE to a cache that holds them and clap/<revision>/")
    d = Path(cache) / "clap" / REAL_MODEL["revision"]
    if not d.is_dir():
        skip(f"{d} is absent: run build_library.py semantic-fetch into the cache first")
    missing = [p for p in ("impact-sounds", "interface-sounds", "sci-fi-sounds", "rpg-audio") if not (Path(cache) / f"kenney_{p}.zip").is_file()]
    if missing:
        skip(f"the Phase 1 zips {missing} are not in {cache}")
    return Path(cache)


def real_selection(cache, descriptions=None):
    packs = {p["id"]: p for p in json.loads((TOOLS / "essentials.json").read_text())["packs"]}
    used = sorted({c[0] for c in REAL_CLIPS})
    sources = [{"source": "kenney", "pack": pack, "member": member,
                "clips": [{"id": cid, "type": t, "kind": k, "tags": ["test"], "description": (descriptions or {}).get(cid, d)}]}
               for pack, member, cid, t, k, d in REAL_CLIPS]
    sel = P.selection([{k: packs[p][k] for k in ("id", "title", "url", "landing", "sha256", "bytes")} for p in used], sources)
    sel["semantic"] = {"model": REAL_MODEL, "required": False}
    return sel


async def test_p13_semantic_fetch_verifies_the_pins():
    cache = real_cache()
    root = Path(tempfile.mkdtemp(prefix="snd-p5-semantic-real-"))
    sel_path = P.write_json(root / "selection.json", real_selection(cache))
    code, out = await pipeline("semantic-fetch", "--selection", sel_path, "--cache", cache, "--offline")
    assert code == 0 and "0 network request(s)" in out, out
    for f in REAL_MODEL["files"]:
        assert f"semantic-fetch: {f['name']} cached, {f['bytes']} bytes" in out, f["name"]
    assert "SHA-256 314eb00cce6ad68d25237b8446b659ccdb136ed4672c1bca470f142f72455026: pins ok" in out, out
    # the Hub and its CDN only: a redirect anywhere else is refused, and a file off its pins is never kept
    bl = P.import_build_library()
    import soundlib
    handler = soundlib.AllowListRedirects(bl.HF_HOSTS)
    req = urllib.request.Request("https://huggingface.co/laion/x/resolve/r/f")
    for url in ("https://evil.example/f", "http://huggingface.co/f", "https://huggingface.co.evil.example/f", "https://hf.co.evil.example/f"):
        try:
            handler.redirect_request(req, None, 302, "Found", {}, url)
            raise AssertionError(f"redirect to {url} was not refused")
        except soundlib.BuildError as e:
            assert "refusing redirect" in str(e)
    for url in ("https://cdn-lfs.hf.co/x", "https://cas-bridge.xethub.hf.co/x", "https://cdn-lfs-us-1.huggingface.co/x"):
        assert handler.redirect_request(req, None, 302, "Found", {}, url) is not None, url
    fake_cache = root / "cache"

    class Wrong:
        requests = 0

        def get_to(self, url, dest, limit, timeout=60):
            self.requests += 1
            assert url.startswith("https://huggingface.co/laion/larger_clap_general/resolve/ada0c23a36c4e8582805bb38fec3905903f18b41/")
            Path(dest).write_bytes(b"x" * 643 if url.endswith("/config.json") else b"x")
            return 1, ""

    blob_only = real_selection(cache)
    for f in blob_only["semantic"]["model"]["files"]:
        if f["name"] == "config.json":
            del f["sha256"]
    for sel_file, reason in ((sel_path, "SHA-256 "), (P.write_json(root / "selection-blob.json", blob_only), "git blob SHA-1 ")):
        args = type("A", (), {"selection": str(sel_file), "cache": str(fake_cache), "offline": False})()
        try:
            bl.cmd_semantic_fetch(args, fetcher=Wrong())
            raise AssertionError("files off their pins were kept")
        except soundlib.BuildError as e:
            assert "config.json: https://huggingface.co/laion/larger_clap_general/resolve/" in str(e), e
            assert f"config.json: {reason}" in str(e).replace("config.json: https://huggingface.co/laion/larger_clap_general/resolve/"
                                                               "ada0c23a36c4e8582805bb38fec3905903f18b41/config.json: ", "config.json: "), e
            assert "pytorch_model.bin: " in str(e) and "1 bytes, pinned 776444665" in str(e), e
        assert not [p for p in (fake_cache / "clap").rglob("*") if p.is_file()], "a file off its pins was kept"
    shutil.rmtree(root)


async def test_p13_real_model_readings():
    cache = real_cache()
    root = Path(tempfile.mkdtemp(prefix="snd-p5-semantic-real-"))
    if not os.environ.get("JASPER_KEEP_SCRATCH"):
        atexit.register(shutil.rmtree, root, True)
    sel_path = P.write_json(root / "selection.json", real_selection(cache))
    work = root / "work"
    code, out = await pipeline("prepare", "--selection", sel_path, "--cache", cache, "--work", work)
    assert code == 0, out
    files = []
    for name in ("a", "b"):
        f = root / f"semantic-{name}.jsonl"
        code, out = await pipeline("semantic", "--selection", sel_path, "--work", work, "--cache", cache, "--semantic-file", f)
        assert code == 0 and "semantic: wrote" in out, out
        files.append(f)
    assert files[0].read_bytes() == files[1].read_bytes(), "two runs of the real stage differ"
    rows = {r["id"]: r for r in lines_of(files[0])[1:]}
    head = lines_of(files[0])[0]
    assert head["embedder"] == "clap" and head["model"]["sha256"] == REAL_MODEL["files"][0]["sha256"], head
    assert head["packages"]["torch"].startswith("2.14.1") and head["packages"]["transformers"] == "5.20.1", head["packages"]
    code, out = await pipeline("semantic", "--selection", sel_path, "--work", work, "--cache", cache, "--semantic-file", files[0], "--check")
    assert code == 0 and "agree with" in out, out
    assert rows[FOOTSTEP]["label"]["verdict"] in ("ok", "weak"), rows[FOOTSTEP]
    # the same footstep described as bird song is flagged for review
    bird = P.write_json(root / "selection-bird.json", real_selection(cache, {FOOTSTEP: "bird song"}))
    fb = root / "semantic-bird.jsonl"
    code, out = await pipeline("semantic", "--selection", bird, "--work", work, "--cache", cache, "--semantic-file", fb)
    assert code == 0, out
    wrong = {r["id"]: r for r in lines_of(fb)[1:]}[FOOTSTEP]
    assert wrong["label"]["verdict"] == "review" and wrong["description"] == "bird song", wrong
    # build_library.py loads no torch, even in the stage's environment, for verify and write
    sel = real_selection(cache)
    for src in sel["sources"]:
        for c in src["clips"]:
            if rows.get(c["id"], {}).get("label", {}).get("verdict") == "review":
                c["review_note"] = "Kept for the test: the reading is recorded."
    noted = P.write_json(root / "selection-noted.json", sel)
    code, out = await pipeline("judge", "--selection", noted, "--work", work)
    assert code == 0, out
    probe = ("import json, sys; sys.path.insert(0, sys.argv[1]); import build_library as bl; "
             "a = bl.main(['write', '--selection', sys.argv[2], '--work', sys.argv[3], '--out', sys.argv[4], '--audit', sys.argv[5], "
             "'--semantic-file', sys.argv[6]]); "
             "b = bl.main(['verify', '--selection', sys.argv[2], '--out', sys.argv[4], '--audit', sys.argv[5], '--semantic-file', sys.argv[6], "
             "'--allow-no-analysis']); "
             "print(json.dumps({'codes': [a, b], 'loaded': sorted(m for m in sys.modules if m.split('.')[0] in ('torch', 'transformers'))}))")
    bl = P.import_build_library()
    python = await asyncio.to_thread(bl.semantic_python, cache)  # the stage's own environment, where torch is importable
    r = await asyncio.to_thread(subprocess.run, [str(python), "-c", probe, str(TOOLS), str(noted), str(work), str(root / "lib"),
                                                 str(root / "audit.jsonl"), str(files[0])], capture_output=True, text=True, timeout=1800)
    assert r.returncode == 0, r.stdout[-2000:] + r.stderr[-2000:]
    res = json.loads(r.stdout.strip().splitlines()[-1])
    assert res == {"codes": [0, 0], "loaded": []}, res
    check = await asyncio.to_thread(subprocess.run, [str(python), "-c", "import torch, transformers"], capture_output=True)
    assert check.returncode == 0, "torch is importable there, so the check above means something"
