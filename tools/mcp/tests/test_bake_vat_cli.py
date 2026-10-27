"""`jasperengine-editor bake-vat`: Vertex Animation baking and importing from a shell (editor/vat/VatCli.cpp;
vertex animation spec 7), on scratch projects with the baker fixtures (tools/fixtures/vertex-animation:
rigid_arm.gltf "Swing"/"Stretch", morph_blob.gltf "Pulse"), a copy of the pose-animation fixture for
GameObject sources, and synthetic OpenVAT / Houdini VAT 3 exports (test_bake_vertex_animation's writers).

No window: each run loads the project headlessly and bakes straight through (VatBakeSession).

- usage errors exit 2 with "bake-vat: <kind>: <message> (fix: ...)" on stderr (unknown option, bad values,
  missing --project / --source, --rebake with settings, --overwrite without --output, no project.yaml, a
  source that is not a bake source, an unknown scene or GameObject, an output outside the assets, importer
  options on a baked source and bake options on an import); with --format json the error is in the JSON;
  --help exits 0 and the top-level help lists the command;
- --dry-run prints the MCP preview's report (mode, clips, texture, bytes, errors, fetches, warnings) and
  the planned output, and writes nothing; -q leaves stderr empty;
- a write without --output writes the default <source dir>/<stem>.vat (+ .vat.glb) that parse (format
  "jasperengine.vat", a glTF binary); the same source again rewrites it (same GUID); another source whose
  default name is taken gets a free name ("morph_blob 2.vat");
- an explicit --output that exists exits 1 with output_exists and changes nothing; --overwrite replaces it
  (GUID kept); a clip the source doesn't have exits 1 and lists the source's clips;
- --rebake re-runs the recipe keeping hand edits and the GUID (--dry-run writes nothing);
- a GameObject source (--scene + its path) and .anim clips on a GameObject (--on + --scene), and a rebake
  of the GameObject bake (the recipe's scene is loaded);
- an OpenVAT export outside the project imports into the assets folder, inside it next to the export; its
  rebake re-imports; a Houdini VAT 3 soft export with --fps / --axis.
"""

from __future__ import annotations

import json
import os
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import harness  # noqa: E402
from harness import TestFailure  # noqa: E402
from project_fixture import write_project  # noqa: E402
from test_bake_vertex_animation import write_houdini_soft, write_openvat  # noqa: E402  (helpers only)

TIMEOUT = 900

REPO = Path(__file__).resolve().parents[3]
VAT_MODELS = REPO / "tools" / "fixtures" / "vertex-animation" / "assets" / "models"
POSE_FIXTURE = REPO / "tools" / "fixtures" / "pose-animation"
ARM = "assets/models/rigid_arm.gltf"
BLOB = "assets/models/morph_blob.gltf"
ARM_VAT = "assets/models/rigid_arm.vat"

BOB_CLIP = json.dumps({"format": "jasperengine.anim", "version": 1, "wrap": "loop",
                       "tracks": [{"path": "", "channel": "position",
                                   "keys": [[0, [0, 0, 0]], [0.5, [0, 0.2, 0]], [1, [0, 0, 0]]]}]}, indent=2) + "\n"

S: dict = {}


def fail(message: str) -> None:
    raise TestFailure(message)


def check(cond, message: str) -> None:
    if not cond:
        fail(message)


def tmp() -> Path:
    if "tmp" not in S:
        S["tmp"] = Path(tempfile.mkdtemp(prefix="jasper-bake-vat-"))
    return S["tmp"]


def project(name: str) -> Path:
    """A fresh scratch project with the two baker fixture models"""
    path = tmp() / name
    if path.exists():
        shutil.rmtree(path)
    write_project(path)
    (path / "assets" / "models").mkdir(parents=True, exist_ok=True)
    for model in ("rigid_arm.gltf", "morph_blob.gltf"):
        shutil.copy(VAT_MODELS / model, path / "assets" / "models" / model)
    return path


def run(*args: str, cwd: Path | None = None) -> subprocess.CompletedProcess:
    return subprocess.run(harness.editor_command() + ["bake-vat", *args], cwd=str(cwd or tmp()),
                          capture_output=True, text=True, timeout=600)


def bake(proj: Path, *args: str, want: int = 0) -> dict:
    """A --format json -q run: its one JSON object (stderr empty)"""
    r = run("--project", str(proj), *args, "--format", "json", "-q")
    check(r.returncode == want, f"{args}: exit {r.returncode}, want {want}\nstdout: {r.stdout[-3000:]}\nstderr: {r.stderr[-3000:]}")
    check(r.stderr == "", f"{args}: -q --format json: stderr is not empty: {r.stderr[-2000:]}")
    try:
        data = json.loads(r.stdout)
    except json.JSONDecodeError as e:
        fail(f"{args}: stdout is not one JSON object ({e}):\n{r.stdout[-3000:]}")
    check(data["ok"] is (want == 0), f"{args}: ok {data['ok']} with exit {want}")
    return data


def tree(root: Path) -> dict:
    return {p.relative_to(root).as_posix(): p.read_bytes() for p in sorted(root.rglob("*"))
            if p.is_file() and ".jasperengine" not in p.relative_to(root).parts}


def read_vat(proj: Path, rel: str) -> dict:
    data = json.loads((proj / rel).read_text())
    check(data["format"] == "jasperengine.vat" and data["version"] == 1, f"{rel}: not a .vat: {list(data)[:4]}")
    glb = (proj / rel).parent / data["mesh"]
    check(glb.is_file() and glb.read_bytes()[:4] == b"glTF", f"{rel}: its mesh {data['mesh']} is not a glTF binary")
    return data


def errors_of(data: dict) -> list[str]:
    return [e["kind"] for e in data.get("errors", [])]


# ---------------------------------------------------------------------------- tests

async def test_help_and_usage_errors():
    r = run("--help")
    check(r.returncode == 0 and "bake-vat --project <path> --source <path>" in r.stdout and "--rebake" in r.stdout and
          "Exit code" in r.stdout, f"--help: exit {r.returncode}\n{r.stdout[-2000:]}")
    r = subprocess.run(harness.editor_command() + ["--help"], capture_output=True, text=True, timeout=60)
    check("bake-vat" in r.stdout + r.stderr, f"the top-level help lists bake-vat:\n{r.stdout}{r.stderr}")

    proj = project("usage")
    p = ["--project", str(proj)]
    s = p + ["--source", ARM]
    cases = [
        ([], "usage: --project is required"),
        (p, "usage: --source is required"),
        (s + ["--bogus"], "usage: unknown option --bogus"),
        (s + ["stray"], "usage: unexpected argument stray"),
        (s + ["--mode"], "usage: missing value for --mode"),
        (s + ["--mode", "pixels"], "invalid --mode 'pixels' (auto, vertices or bones)"),
        (s + ["--fps", "0"], "invalid --fps '0'"),
        (s + ["--fps", "500"], "invalid --fps '500'"),
        (s + ["--fps", "fast"], "invalid --fps 'fast'"),
        (s + ["--normals", "yes"], "invalid --normals 'yes' (on or off)"),
        (s + ["--precision", "double"], "invalid --precision 'double'"),
        (s + ["--preset", "console"], "invalid --preset 'console'"),
        (s + ["--clips", "Swing,,Stretch"], "invalid --clips 'Swing,,Stretch'"),
        (s + ["--format", "yaml"], "--format must be text or json"),
        (s + ["--axis", "sideways"], "invalid --axis 'sideways'"),
        (s + ["--upsample", "-1"], "invalid --upsample '-1'"),
        (s + ["--overwrite"], "--overwrite needs --output"),
        (p + ["--rebake", ARM_VAT, "--fps", "24"], "leave out --fps"),
        (p + ["--rebake", ARM_VAT, "--source", ARM], "leave out --source"),
        (["--project", str(tmp()), "--source", ARM], "usage: no project.yaml in"),
        # what the project answers
        (p + ["--source", "assets/models/nope.gltf"], "invalid_source: source: 'assets/models/nope.gltf' does not exist"),
        (p + ["--source", "assets/models/rigid_arm.gltf#Nope", "--clips", "Swing", "--axis", "unity"],
         "usage: --axis is an importer option"),
        (s + ["--upsample", "2"], "usage: --upsample is an importer option"),
        (s + ["--output", "assets/models/arm.txt"], "usage: --output: 'assets/models/arm.txt' is not a .vat path inside the assets folder"),
        (s + ["--output", "elsewhere/arm.vat"], "is not a .vat path inside the assets folder"),
        (s + ["--scene", "Nowhere"], "usage: --scene: no scene 'Nowhere' in the project"),
        (p + ["--rebake", "assets/models/none.vat"], "usage: --rebake:"),
    ]
    before = tree(proj)
    for args, expected in cases:
        r = run("-q", *args)
        check(r.returncode == 2, f"{args}: exit {r.returncode}, want 2\nstderr: {r.stderr[-1500:]}")
        check("bake-vat: " + expected in r.stderr or expected in r.stderr, f"{args}: stderr lacks {expected!r}:\n{r.stderr[-1500:]}")
        check(r.stderr.startswith("bake-vat: ") and "(fix: " in r.stderr, f"{args}: 'bake-vat: <kind>: <message> (fix: ...)': {r.stderr}")
        check(r.stdout == "", f"{args}: a usage error printed on stdout: {r.stdout[-500:]}")
    check(tree(proj) == before, "usage errors write nothing")

    # .anim clips need what they play on
    (proj / "assets" / "anim").mkdir(parents=True, exist_ok=True)
    (proj / "assets" / "anim" / "bob.anim").write_text(BOB_CLIP)
    r = run(*p, "--source", "assets/anim/bob.anim", "-q")
    check(r.returncode == 2 and "--on is required" in r.stderr, f".anim without --on: {r.returncode} {r.stderr}")

    # --format json: the usage error in the one JSON object, nothing on stderr
    data = bake(proj, "--source", ARM, "--fps", "-3", want=2)
    check(errors_of(data) == ["usage"] and "invalid --fps '-3'" in data["errors"][0]["message"] and data["errors"][0]["fix"],
          f"json usage error: {data}")
    data = bake(proj, "--source", ARM, "--clips", "Swing", "--upsample", "4", want=2)
    check(errors_of(data) == ["usage"] and "--upsample is an importer option" in data["errors"][0]["message"], f"{data}")


async def test_dry_run_reports_and_writes_nothing():
    proj = project("dry")
    before = tree(proj)
    data = bake(proj, "--source", ARM, "--dry-run")
    check(tree(proj) == before, "--dry-run wrote files")
    check(data["dry_run"] is True and "written" not in data and "errors" not in data, f"a dry run: {data}")
    check(data["source"] == {"kind": "model", "path": ARM}, f"source: {data['source']}")
    report = data["report"]
    for key in ("mode", "mode_reason", "vertices", "bones", "clips", "texture", "bytes", "max_error_mm", "mean_error_mm",
                "fetches_per_vertex", "warnings"):
        check(key in report, f"the report lacks {key}: {report}")
    check(report["mode"] in ("vertices", "bones") and report["mode_reason"], f"mode and why: {report}")
    check([c["name"] for c in report["clips"]] == ["Swing", "Stretch"], f"both clips in source order: {report['clips']}")
    check(all(c["frames"] > 0 and c["bytes"] > 0 for c in report["clips"]), f"clip sizes: {report['clips']}")
    check(report["texture"]["width"] == 2048 and report["texture"]["format"] in ("rgba16f", "rgba32f"), f"mobile_web: {report['texture']}")
    for w in report["warnings"]:
        check(w["kind"] and w["message"] and "fix" in w, f"a warning has kind, message and fix: {w}")
    check(data["output"] == {"vat": ARM_VAT, "rewrite": False}, f"the planned output: {data['output']}")

    # settings reach the bake: one clip, Bones mode, desktop, 24 fps
    data = bake(proj, "--source", ARM, "--clips", "Swing", "--mode", "bones", "--preset", "desktop", "--fps", "24", "--dry-run")
    report = data["report"]
    check(report["mode"] == "bones" and [c["name"] for c in report["clips"]] == ["Swing"], f"--mode/--clips: {report}")
    check(report["texture"]["width"] == 4096 and abs(report["clips"][0]["fps"] - 24) < 1.5, f"--preset desktop, --fps 24: {report}")
    # model#Clip picks one clip too
    data = bake(proj, "--source", ARM + "#Stretch", "--dry-run")
    check([c["name"] for c in data["report"]["clips"]] == ["Stretch"], f"model#Clip: {data['report']['clips']}")

    # text: the summary on stdout, nothing written
    r = run("--project", str(proj), "--source", ARM, "--dry-run", "-q")
    check(r.returncode == 0 and r.stderr == "", f"text dry run: {r.returncode} {r.stderr}")
    check(r.stdout.startswith("baked " + ARM + ": 2 clips: Swing") and "texture 2048x" in r.stdout and
          "dry run, nothing written: would write " + ARM_VAT in r.stdout, f"text summary:\n{r.stdout}")
    # without -q the engine's log and the progress go to stderr, never to stdout
    r = run("--project", str(proj), "--source", ARM, "--dry-run", "--format", "json")
    check(r.returncode == 0 and "bake-vat: baking 2 clip(s)" not in r.stdout, f"stdout carries only the JSON: {r.stdout[:300]}")
    json.loads(r.stdout)
    check(tree(proj) == before, "dry runs wrote files")


async def test_write_default_rewrite_and_free_name():
    proj = project("write")
    data = bake(proj, "--source", ARM)
    written = data["written"]
    check(written["vat"] == ARM_VAT and written["mesh"] == ARM_VAT + ".glb" and written["rewritten"] is False,
          f"the default output next to the source: {written}")
    check(sorted(written["files"]) == sorted([ARM_VAT, ARM_VAT + ".glb"]), f"files: {written['files']}")
    vat = read_vat(proj, ARM_VAT)
    check(vat["guid"] == written["guid"] and len(vat["guid"]) == 32, f"guid: {vat['guid']} vs {written['guid']}")
    check([c["name"] for c in vat["clips"]] == ["Swing", "Stretch"], f"clips: {vat['clips']}")
    check(vat["recipe"]["sources"][0]["kind"] == "model" and vat["recipe"]["sources"][0]["path"] == ARM, f"recipe: {vat['recipe']}")
    check(vat["layout"]["height"] == data["report"]["texture"]["height"], "the report describes the written file")

    # the same source and clips again, no --output: the same file rewritten, same GUID
    data = bake(proj, "--source", ARM)
    check(data["written"]["vat"] == ARM_VAT and data["written"]["rewritten"] is True and data["written"]["guid"] == vat["guid"],
          f"rewrites the default file: {data['written']}")
    # other clips of the same model: another asset, the first one untouched
    data = bake(proj, "--source", ARM, "--clips", "Swing")
    check(data["written"]["vat"] == "assets/models/rigid_arm 2.vat" and data["written"]["rewritten"] is False,
          f"another clip set takes a free name: {data['written']}")
    check([c["name"] for c in read_vat(proj, ARM_VAT)["clips"]] == ["Swing", "Stretch"], "the first bake is untouched")
    check([c["name"] for c in read_vat(proj, "assets/models/rigid_arm 2.vat")["clips"]] == ["Swing"], "the new bake")
    # a dry run says it would rewrite
    data = bake(proj, "--source", ARM, "--dry-run")
    check(data["output"] == {"vat": ARM_VAT, "rewrite": True}, f"planned rewrite: {data['output']}")

    # another source whose default name is taken (by another bake): a free name
    taken = bake(proj, "--source", ARM, "--output", "assets/models/morph_blob.vat")
    check(taken["written"]["vat"] == "assets/models/morph_blob.vat", f"{taken['written']}")
    data = bake(proj, "--source", BLOB)
    check(data["written"]["vat"] == "assets/models/morph_blob 2.vat" and data["written"]["rewritten"] is False,
          f"a free name next to the source: {data['written']}")
    check(read_vat(proj, "assets/models/morph_blob.vat")["guid"] == taken["written"]["guid"], "the other bake is untouched")
    check([c["name"] for c in read_vat(proj, "assets/models/morph_blob 2.vat")["clips"]] == ["Pulse"], "morph_blob's clip")

    # text mode: the written files and how to use them
    r = run("--project", str(proj), "--source", ARM, "-q")
    check(r.returncode == 0 and "wrote " + ARM_VAT + ", " + ARM_VAT + ".glb (rewrote the existing file" in r.stdout and
          "Vertex Animator" in r.stdout, f"text write:\n{r.stdout}\n{r.stderr}")


async def test_explicit_output_exists_and_overwrite():
    proj = project("overwrite")
    out = "assets/vat/arm.vat"
    data = bake(proj, "--source", ARM, "--output", out)
    check(data["written"]["vat"] == out and (proj / "assets/vat/arm.vat.glb").is_file(), f"--output (a new folder): {data['written']}")
    guid = data["written"]["guid"]
    before = tree(proj)
    data = bake(proj, "--source", ARM, "--clips", "Swing", "--output", out, want=1)
    check(errors_of(data) == ["output_exists"] and "overwrite" in data["errors"][0]["fix"], f"output_exists: {data.get('errors')}")
    check("written" not in data, "nothing written")
    check(tree(proj) == before, "an existing output is kept")
    r = run("--project", str(proj), "--source", ARM, "--output", out, "-q")
    check(r.returncode == 1 and r.stderr.startswith("bake-vat: output_exists: '" + out + "' exists") and "(fix: " in r.stderr,
          f"text: {r.returncode} {r.stderr}")
    data = bake(proj, "--source", ARM, "--clips", "Swing", "--output", out, "--overwrite")
    check(data["written"]["vat"] == out and data["written"]["rewritten"] is True and data["written"]["guid"] == guid,
          f"--overwrite keeps the GUID: {data['written']}")
    check([c["name"] for c in read_vat(proj, out)["clips"]] == ["Swing"], "the new bake is written")

    # a clip the source doesn't have: exit 1, the source's clips listed
    data = bake(proj, "--source", ARM, "--clips", "Run", want=1)
    check("invalid_settings" in errors_of(data) and data.get("source_clips") == ["Swing", "Stretch"], f"unknown clip: {data}")
    r = run("--project", str(proj), "--source", ARM, "--clips", "Run", "-q")
    check(r.returncode == 1 and "bake-vat: invalid_settings: the source has no clip 'Run'" in r.stderr, f"text: {r.stderr}")


async def test_rebake_keeps_edits():
    proj = project("rebake")
    first = bake(proj, "--source", ARM)
    vat = read_vat(proj, ARM_VAT)
    vat["clips"][0]["name"] = "Swing Edited"
    vat["clips"][0]["speed"] = 2
    (proj / ARM_VAT).write_text(json.dumps(vat, indent=2))
    before = tree(proj)
    data = bake(proj, "--rebake", ARM_VAT, "--dry-run")
    check(tree(proj) == before and data["output"] == {"vat": ARM_VAT, "rewrite": True}, f"a rebake dry run writes nothing: {data}")
    check(data["source"]["rebake"] == ARM_VAT and data["source"]["kind"] == "model", f"source: {data['source']}")
    # the model changed: the rebake follows it, the hand edits stay
    model = proj / ARM
    model.write_text(model.read_text() + "\n")
    data = bake(proj, "--rebake", ARM_VAT)
    after = read_vat(proj, ARM_VAT)
    check(data["written"]["rewritten"] is True and after["guid"] == first["written"]["guid"], f"GUID kept: {data['written']}")
    check(after["clips"][0]["name"] == "Swing Edited" and after["clips"][0]["speed"] == 2, f"hand edits kept: {after['clips'][0]}")
    check(after["recipe"]["sources"][0]["hashes"] != vat["recipe"]["sources"][0]["hashes"], "the recipe has the new source hash")
    r = run("--project", str(proj), "--rebake", ARM_VAT, "-q")
    check(r.returncode == 0 and r.stdout.startswith("rebaked " + ARM_VAT), f"text rebake:\n{r.stdout}{r.stderr}")


async def test_gameobject_sources():
    proj = tmp() / "scene"
    if proj.exists():
        shutil.rmtree(proj)
    shutil.copytree(POSE_FIXTURE, proj, ignore=shutil.ignore_patterns(".jasperengine"))
    (proj / "assets" / "anim").mkdir(parents=True, exist_ok=True)
    (proj / "assets" / "anim" / "bob.anim").write_text(BOB_CLIP)
    # .anim clips on a GameObject of a scene
    data = bake(proj, "--source", "assets/anim/bob.anim", "--on", "Hero", "--scene", "Main Scene", "--dry-run")
    check(data["source"]["kind"] == "anim" and data["source"]["path"] == "Hero" and data["source"]["scene"] == "Main Scene.scene",
          f"on a GameObject: {data['source']}")
    check([c["name"] for c in data["report"]["clips"]] == ["bob"], f"{data['report']['clips']}")
    data = bake(proj, "--source", "assets/anim/bob.anim", "--on", "Nobody", "--scene", "Main Scene.scene", want=2)
    check(errors_of(data) == ["invalid_source"] and "no GameObject 'Nobody'" in data["errors"][0]["message"], f"{data}")

    # a GameObject source: its Animation Player's clips
    scene = proj / "Main Scene.scene"
    text = scene.read_text()
    at = text.index("    components:\n", text.index("    name: Hero\n")) + len("    components:\n")
    scene.write_text(text[:at] + "      animationplayer:\n        clips:\n          - clip: anim/bob.anim\n" + text[at:])
    data = bake(proj, "--source", "Hero", "--scene", "Main Scene")
    check(data["source"] == {"kind": "entity", "path": "Hero", "scene": "Main Scene.scene"}, f"{data['source']}")
    vat_rel = data["written"]["vat"]
    check(vat_rel == "assets/models/Hero.vat", f"next to the GameObject's model: {vat_rel}")
    vat = read_vat(proj, vat_rel)
    check(vat["recipe"]["sources"][0]["kind"] == "entity" and vat["recipe"]["sources"][0]["scene"] == "Main Scene.scene",
          f"recipe: {vat['recipe']['sources']}")
    # its rebake loads the recipe's scene
    data = bake(proj, "--rebake", vat_rel)
    check(data["written"]["guid"] == vat["guid"] and data["written"]["rewritten"] is True, f"rebake of a GameObject bake: {data}")
    data = bake(proj, "--source", "Hero", "--scene", "Main Scene", "--axis", "unity", want=2)
    check(errors_of(data) == ["usage"], f"importer options on a GameObject: {data}")


async def test_import_openvat_and_houdini():
    proj = project("import")
    outside = tmp() / "exports" / "flag"
    write_openvat(outside)
    # an export outside the project: written into the assets folder
    data = bake(proj, "--source", str(outside / "Flag-remap_info.json"), "--dry-run")
    check(data["source"]["kind"] == "openvat" and data["report"]["kind"] == "openvat", f"an OpenVAT import: {data['source']}")
    check(sorted(c["name"] for c in data["report"]["clips"]) == ["Flap", "Wave"], f"clips from remap_info: {data['report']['clips']}")
    check(data["output"] == {"vat": "assets/Flag.vat", "rewrite": False}, f"outside the project: into the assets folder: {data['output']}")
    data = bake(proj, "--source", str(outside), "--clips", "Wave", want=2)
    check(errors_of(data) == ["usage"] and "--clips is for baked sources" in data["errors"][0]["message"], f"{data}")
    data = bake(proj, "--source", str(outside))
    check(data["written"]["vat"] == "assets/Flag.vat", f"{data['written']}")
    vat = read_vat(proj, "assets/Flag.vat")
    check(vat["recipe"]["sources"][0]["kind"] == "openvat", f"recipe: {vat['recipe']}")
    data = bake(proj, "--source", str(outside), "--output", "assets/Flag.vat", want=1)
    check(errors_of(data) == ["output_exists"], f"an existing import output: {data}")
    # its rebake imports it again, GUID and edits kept
    vat["clips"][0]["speed"] = 3
    (proj / "assets/Flag.vat").write_text(json.dumps(vat, indent=2))
    data = bake(proj, "--rebake", "assets/Flag.vat")
    after = read_vat(proj, "assets/Flag.vat")
    check(data["source"]["kind"] == "openvat" and after["guid"] == vat["guid"] and after["clips"][0]["speed"] == 3,
          f"re-import keeps edits: {data.get('written')} {after['clips'][0]}")
    r = run("--project", str(proj), "--rebake", "assets/Flag.vat", "-q")
    check(r.returncode == 0 and r.stdout.startswith("re-imported assets/Flag.vat"), f"text:\n{r.stdout}{r.stderr}")

    # an export inside the assets: next to it, by a relative path from the current directory
    write_openvat(proj / "assets" / "exports" / "flag")
    data = bake(proj, "--source", "assets/exports/flag")
    check(data["written"]["vat"] == "assets/exports/flag/Flag.vat", f"next to the export: {data['written']}")
    # paths relative to the current directory (inside the project: the output too)
    r = run("--project", "..", "--source", "exports/flag", "--dry-run", "--format", "json", "-q", cwd=proj / "assets")
    check(r.returncode == 0, f"paths relative to the current directory: {r.stdout[-1000:]} {r.stderr}")
    check(json.loads(r.stdout)["output"] == {"vat": "assets/exports/flag/Flag.vat", "rewrite": True}, r.stdout[-500:])
    r = run("--project", "..", "--source", "exports/flag", "--output", "vat/rel.vat", "--dry-run", "--format", "json", "-q",
            cwd=proj / "assets")
    check(r.returncode == 0 and json.loads(r.stdout)["output"] == {"vat": "assets/vat/rel.vat", "rewrite": False},
          f"an output relative to the current directory: {r.stdout[-500:]} {r.stderr}")

    # Houdini VAT 3 soft: --fps and --axis reach the importer
    wave = tmp() / "exports" / "wave"
    write_houdini_soft(wave)
    data = bake(proj, "--source", str(wave), "--fps", "24", "--normals", "off", "--axis", "unity", "--output", "assets/vat/wave.vat")
    check(data["report"]["kind"] == "houdini_vat3" and data["report"]["houdini_mode"] == "soft" and data["report"]["vertices"] == 9,
          f"Houdini soft import: {data['report']}")
    vat = read_vat(proj, "assets/vat/wave.vat")
    check(vat["recipe"]["sources"][0]["kind"] == "houdini_vat3" and vat["recipe"]["import"]["axis"] == "unity",
          f"the recipe keeps the import options: {vat['recipe']}")
