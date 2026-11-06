"""Exports of cooked textures (docs/specs/S14-texture-compression.md R9, AC12): a source export of
tools/fixtures/textures ships assets/je-cooked/<family>/ for every family a platform of it loads,
with an index.json whose files exist, leaves out the sources its cooks replace (but ships those a
reference reads from the file; a glTF's external images load cooked, and the glTF ships as its
cook in je-cooked/models, S53 R12), reserves je-cooked, and its
packaging keeps one family per platform (CMake copies, the web's --preload-file, Gradle's synced
assets, the Xcode run script) with the Android manifest asking for ASTC.
"""

from __future__ import annotations

import json
from pathlib import Path

from harness import Editor

REPO = Path(__file__).resolve().parents[3]
FIXTURE = REPO / "tools" / "fixtures" / "textures"
EDITOR = {"project": FIXTURE}
TIMEOUT = 1800
CI = True


async def test_source_export_ships_cooked_families(ed: Editor):
    out = ed.scratch.root / "export-source"
    r = await ed.call("build_project", mode="source", output_dir=str(out), platform="android", timeout_seconds=1500)
    ed.check_eq(r["status"], "succeeded", f"{r}")
    # a source export: the engine at the root, the game project under project/
    assets = out / "project" / "assets"
    cooked = assets / "je-cooked"
    for family in ("desktop", "mobile", "web"):
        index_path = cooked / family / "index.json"
        ed.check(index_path.exists(), f"{index_path} exported")
        index = json.loads(index_path.read_text())
        textures = index["textures"]
        ed.check("textures/photo.png" in textures, f"{family} index: {sorted(textures)}")
        for source, entry in textures.items():
            ed.check((cooked / family / entry["file"]).exists(), f"{family} {source}: {entry['file']} missing")
            ed.check(entry["width"] > 0 and entry["levels"] >= 1 and entry["bytes"] > 0, f"{family} {source}: {entry}")
        ed.check_eq(textures["textures/photo.png"]["width"], 256)
    ed.check(not (cooked / "android_etc2").exists(), "no ETC2 family while the fallback is off")
    desktop = json.loads((cooked / "desktop" / "index.json").read_text())["textures"]
    ed.check_eq(desktop["textures/photo.png"]["format"], "BC1")
    ed.check_in("normalRG", desktop["textures/normal.png"]["flags"])
    web = json.loads((cooked / "web" / "index.json").read_text())["textures"]
    ed.check_eq(web["textures/photo.png"]["format"], "ETC1S")
    # the cube map has its CPU copy for IBL only when cooked as a cube; the spot mask is CPU-readable
    ed.check_eq(desktop["textures/ao.png"]["format"], "R8")
    ed.check_in("cpu", desktop["textures/ao.png"]["flags"])

    # sources: the cooked ones stay out, the glTF's external image too (the model loads its cook)
    ed.check(not (assets / "textures" / "photo.png").exists(), "photo.png ships cooked only")
    ed.check(not (assets / "textures" / "ao.png").exists(), "ao.png ships cooked only")
    ed.check(not (assets / "textures" / "normal.png").exists(), "the glTF's normal map ships cooked only")
    models = json.loads((cooked / "models" / "index.json").read_text())["models"]
    ed.check("models/panel.gltf" in models, f"the model ships cooked: {sorted(models)}")
    for family, entry in models.get("models/panel.gltf", {}).items():
        ed.check((cooked / "models" / entry["file"]).is_file(), f"its {family} cook {entry['file']} ships")
    ed.check(not (assets / "models" / "panel.gltf").exists(), "the model's source ships cooked only")
    ed.check(not any(p.suffix == ".meta" for p in assets.rglob("*")), "no metas in the export")

    # the web's .data packs the assets less the other families (--exclude-file): >= 60% smaller
    # than the same assets with the texture sources instead of the cooks (AC12)
    def tree_bytes(root: Path, skip=()) -> int:
        return sum(p.stat().st_size for p in root.rglob("*") if p.is_file() and not any(part in skip for part in p.parts))
    web_data = tree_bytes(assets, skip=("desktop", "mobile", "android_etc2"))
    sources = tree_bytes(ed.project / "assets") - sum(p.stat().st_size for p in (ed.project / "assets").rglob("*.meta"))
    raw_data = tree_bytes(assets, skip=("je-cooked",)) + sum(
        (ed.project / "assets" / source).stat().st_size for source in web if not (assets / source).exists())
    ed.check(web_data <= 0.4 * raw_data, f"web payload {web_data} bytes vs {raw_data} with the sources ({sources} in the project)")

    # packaging: each platform keeps its family
    cmake = [p for p in out.rglob("CMakeLists.txt") if "je-cooked" in p.read_text(errors="replace")]
    ed.check(cmake, "the exported engine's CMakeLists.txt drops the other families")
    text = cmake[0].read_text(errors="replace")
    ed.check_in("--exclude-file */je-cooked/${family}", text)
    gradle = (out / "workspaces" / "androidstudio" / "app" / "build.gradle").read_text()
    ed.check_in("exclude 'je-cooked/desktop/**', 'je-cooked/web/**'", gradle)
    manifest = (out / "workspaces" / "androidstudio" / "app" / "src" / "main" / "AndroidManifest.xml").read_text()
    ed.check_in('<supports-gl-texture android:name="GL_KHR_texture_compression_astc_ldr" />', manifest)
    pbxproj = (out / "workspaces" / "xcode" / "JasperEngine.xcodeproj" / "project.pbxproj").read_text()
    ed.check_in("Keep this platform's cooked textures", pbxproj)

    # a second export finds every cook in the cache
    r = await ed.call("build_project", mode="source", output_dir=str(out), overwrite=True, platform="android", timeout_seconds=600)
    ed.check_eq(r["status"], "succeeded", f"{r}")
    log = Path(r["result"]["log_file"]).read_text(errors="replace")
    ed.check_in("Textures: 0 cooked", log)


async def test_etc2_fallback_and_reserved_name(ed: Editor):
    await ed.call("set_project_settings", values={"texture.android_etc2_fallback": True})
    await ed.check_error("set_project_settings", values={"texture.default_max_size.web": 300}, contains="power of two")
    got = await ed.call("get_project_settings")
    ed.check_in("texture.android_etc2_fallback", json.dumps(got))
    out = ed.scratch.root / "export-etc2"
    r = await ed.call("build_project", mode="source", output_dir=str(out), platform="android", timeout_seconds=1500)
    ed.check_eq(r["status"], "succeeded", f"{r}")
    etc2 = json.loads((out / "project" / "assets" / "je-cooked" / "android_etc2" / "index.json").read_text())["textures"]
    ed.check_eq(etc2["textures/photo.png"]["format"], "ETC2_RGB8")
    manifest = (out / "workspaces" / "androidstudio" / "app" / "src" / "main" / "AndroidManifest.xml").read_text()
    ed.check("supports-gl-texture" not in manifest, "with the ETC2 fallback no store filter")

    # je-cooked is the export's: a project directory of that name fails the export, naming it
    reserved = ed.project / "assets" / "je-cooked"
    reserved.mkdir()
    (reserved / "x.txt").write_text("mine")
    msg = await ed.call("build_project", mode="source", output_dir=str(ed.scratch.root / "export-reserved"), timeout_seconds=900,
                        expect_error=True)
    ed.check_in("je-cooked", msg)
