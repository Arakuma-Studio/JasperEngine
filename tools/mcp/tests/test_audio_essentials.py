"""S36 AC7: the 40 essentials, embedded in the editor. `essentials:` references in analyze_audio and
import_files, the `.meta` an import writes, Import Sound Essentials and the embedder's refusal of a
corrupted byte.

Oracles: hashlib over the committed files and the imported bytes, the manifest's `measured` block
(libsndfile and pyloudnorm, an independent decoder), PyYAML over the metas."""

from __future__ import annotations

import hashlib
import json
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

import yaml

from harness import Editor

CI = True
EDITOR = {"create": True}
TIMEOUT = 900

REPO = Path(__file__).resolve().parents[3]
ESSENTIALS = REPO / "editor/resources/sound-library/essentials"
EMBED = REPO / "tools/sound-library/embed_essentials.py"


def manifest() -> dict:
    return json.loads((ESSENTIALS / "manifest.json").read_text())


async def test_pack_and_manifest_v2():
    m = manifest()
    assert (m["format"], m["version"]) == ("jasperengine-sound-essentials", 2)
    assert len(m["clips"]) == 40
    total = 0
    for clip in m["clips"]:
        data = (ESSENTIALS / clip["path"]).read_bytes()
        total += len(data)
        assert hashlib.sha256(data).hexdigest() == clip["sha256"], clip["id"]
        assert clip["meta"]["provenance"]["builtin"] == {"set": "essentials", "id": clip["id"], "sha256": clip["sha256"]}
        assert clip["meta"]["import"]["audioImporter"]["type"] == clip["type"]
        assert "import" not in clip, "v2 holds the fades in meta"
    assert total == 423628, total
    faded = [c for c in m["clips"] if {"fade_in_ms", "fade_out_ms"} & set(c["meta"]["import"]["audioImporter"])]
    assert len(faded) == 13, [c["id"] for c in faded]


async def test_analysis_agrees_with_the_manifest(ed: Editor):
    m = {c["id"]: c for c in manifest()["clips"]}
    items = []
    for offset in (0, 24):
        r = await ed.call("analyze_audio", paths=[f"essentials:{i}" for i in list(sorted(m))[offset:offset + 24]], image="none")
        ed.check(not r["errors"], f"errors: {r['errors']}")
        items += r["items"]
    star = await ed.call("analyze_audio", paths=["essentials:*"], image="none")
    ed.check_eq(len(star["items"]), 40, "essentials:* expands")
    ed.check_eq(len(items), 40, "40 items")
    for item in items:
        clip = m[item["path"][len("essentials:"):]]
        measured = clip["measured"]
        ed.check_eq(item["kind"], clip["kind"], f"{clip['id']}: the manifest's kind")
        ed.check_eq(item["licence"]["id"], "CC0-1.0", f"{clip['id']}: the licence")
        ed.check(abs(item["format"]["frames"] - measured["frames"]) <= 64, f"{clip['id']} frames")
        ed.check(abs(item["source"]["peak"]["sample_dbfs"] - measured["sample_peak_dbfs"]) <= 0.3 if "source" in item
                 else abs(item["peak"]["sample_dbfs"] - measured["sample_peak_dbfs"]) <= 0.3, f"{clip['id']} sample peak within 0.3 dB")
        momentary = (item["source"] if "source" in item else item)["loudness"]["momentary_max"]
        ed.check(momentary is None or abs(momentary - measured["momentary_max_lufs"]) <= 0.3, f"{clip['id']} momentary max within 0.3 LU: {momentary}")
        head = (item["source"] if "source" in item else item)["silence"]["head_ms"]
        ed.check(abs(head - measured["head_ms"]) <= 1.0 + 1.0, f"{clip['id']} head silence within tolerance: {head} vs {measured['head_ms']}")


async def test_import_files_writes_bytes_and_meta(ed: Editor):
    m = {c["id"]: c for c in manifest()["clips"]}
    r = await ed.call("import_files", sources=["essentials:footstep/*"], to_dir="assets/sfx")
    names = [i for i in m if i.startswith("footstep/")]
    ed.check_eq(len(r["imported"]), len(names), "the group")
    guids = set()
    for name in names:
        path = ed.project / "assets/sfx" / (name + ".ogg")
        ed.check_eq(hashlib.sha256(path.read_bytes()).hexdigest(), m[name]["sha256"], f"{name}: the bytes")
        meta = yaml.safe_load(Path(str(path) + ".meta").read_text())
        ed.check(len(meta["guid"]) == 32, f"{name}: a minted GUID")
        guids.add(meta["guid"])
        ed.check_eq(meta["provenance"], m[name]["meta"]["provenance"], f"{name}: the provenance")
        ed.check_eq(meta["import"], m[name]["meta"]["import"], f"{name}: the importer")
    ed.check_eq(len(guids), len(names), "each its own GUID")
    # a second import of the same bytes is skipped; another destination mints its own
    again = await ed.call("import_files", sources=["essentials:footstep/*"], to_dir="assets/sfx")
    ed.check_eq((len(again["imported"]), len(again["skipped"])), (0, len(names)), "identical files are skipped")
    # different bytes are refused unless overwrite
    target = ed.project / "assets/sfx" / (names[0] + ".ogg")
    target.write_bytes(b"OggS" + b"x" * 64)
    text = await ed.call("import_files", sources=[f"essentials:{names[0]}"], to_dir="assets/sfx", expect_error=True)
    ed.check("overwrite" in text, f"a different file names the fix: {text}")
    forced = await ed.call("import_files", sources=[f"essentials:{names[0]}"], to_dir="assets/sfx", overwrite=True)
    ed.check_eq(len(forced["imported"]), 1, "overwrite")
    ed.check_eq(hashlib.sha256(target.read_bytes()).hexdigest(), m[names[0]]["sha256"], "the bytes are back")
    # one undo removes what one call wrote
    fresh = await ed.call("import_files", sources=["essentials:ui/*"], to_dir="assets/ui_sfx")
    await ed.call("undo", scope="project")
    ed.check(not (ed.project / fresh["imported"][0]).exists(), "undo removes the files")
    ed.check(not Path(str(ed.project / fresh["imported"][0]) + ".meta").exists(), "and their metas")
    await ed.call("redo", scope="project")
    ed.check((ed.project / fresh["imported"][0]).exists(), "redo")


async def test_import_sound_essentials_command(ed: Editor):
    await ed.call("run_command", id="assets.importSoundEssentials")
    folder = ed.project / "sounds/essentials"  # a new project's assets folder is the project's
    # a menu command runs on the editor's next turn: wait for the files, not for time
    for _ in range(300):
        if len(list(folder.rglob("*.ogg"))) == 40:
            break
        await ed.call("editor_status")
    ed.check_eq(len(list(folder.rglob("*.ogg"))), 40, "40 clips")
    ed.check_eq(len(list(folder.rglob("*.ogg.meta"))), 40, "40 metas")
    # again: skipped
    first = folder / "ui/tick.ogg"
    first.write_bytes(first.read_bytes() + b"\0")
    await ed.call("run_command", id="assets.importSoundEssentials")
    await ed.call("editor_status")
    ed.check(first.read_bytes().endswith(b"\0"), "a different file is left alone")
    await ed.call("undo", scope="project")
    ed.check_eq(len(list(folder.rglob("*.ogg"))), 0, "one undo removes the 40")


def run_embed(root: Path, out: Path) -> subprocess.CompletedProcess:
    return subprocess.run([sys.executable, "-I", str(EMBED), str(root), str(out)], capture_output=True, text=True)


async def test_embedder_refuses_a_corrupted_byte():
    with tempfile.TemporaryDirectory() as tmp:
        root = Path(tmp) / "essentials"
        shutil.copytree(ESSENTIALS, root)
        out = Path(tmp) / "sound_essentials.h"
        r = run_embed(root, out)
        assert r.returncode == 0, r.stderr
        assert out.stat().st_size > 400000
        clip = root / manifest()["clips"][0]["path"]
        data = bytearray(clip.read_bytes())
        data[100] ^= 0xFF
        clip.write_bytes(bytes(data))
        r = run_embed(root, out)
        assert r.returncode == 1 and "SHA-256" in r.stderr, r.stderr


async def test_group_is_hidden_until_enabled(ed: Editor):
    listed = {t.name for t in await ed.tools(groups=None)}
    ed.check("generate_sound" not in listed and "search_sounds" not in listed, "group audio is off by default")
