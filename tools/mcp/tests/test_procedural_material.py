"""S90 agent side (docs/specs/S90-procedural-material.md R14-R16, R32-R39; AC2, AC9, AC10, AC13, AC14):
preview_procedural_material, bake_procedural_material, check --materials and procedural-material bake.

- the groups (AC13): `graphs` and `materials` are hidden until tool_groups enables them, callable either way;
- the bake (AC9): the PNGs (recipe chunk, explicit .meta import settings) and the .material are one undo step that
  undo removes whole; a re-bake keeps a hand-edited field of the .material;
- the preview (R34, R36): a contact sheet and S35 rows, the device always named; sheet [] measures only; a baked
  .material previews from its files; a changed threshold is listed;
- never a stale sheet (AC14): past timeout_seconds with the lit ball still pending the call is an error with no image,
  and the next preview of the same graph is the new one;
- staleness (R16, AC10 through the CLI): an edit makes check --materials warn, compiler style;
- the CLI bake (R39): exit 0 and the files on a copy of the project, 2 on an unknown flag, 1 on a bad graph;
- export (AC2): a baked PNG ships without its jasper.procmat chunk.

Runs in one new empty project; tests run in order and share the editor.
"""

from __future__ import annotations

import json
import os
import shutil
import subprocess
import tempfile
from pathlib import Path

import yaml

import harness
from harness import Editor, tool_meta

CI = True
EDITOR = {"create": True, "name": "Procedural Material CI", "env": {"JASPER_TEST_PROCMAT_SPHERE_PENDING": "sphere-pending"}}
TIMEOUT = 2400

GRAPH_PATH = "assets/mats/stone.procmat"
FOLDER = "assets/mats/stone"
CHECK_KEYS = {"id", "kind", "status", "round", "measured", "threshold", "threshold_source", "limited_by", "at", "fix_hint", "method"}


def stone_graph(scale: float = 4.0) -> dict:
    """perlin feeds the base colour, the roughness and the height: the bake makes the normal from the height"""
    return {
        "formatVersion": 1, "kind": "procedural_material", "kindVersion": 1, "nextId": 6,
        "nodes": [{"id": "n1", "type": "perlin", "position": [0, 0], "params": {"scale_x": scale, "scale_y": scale}},
                  {"id": "out", "type": "material_output", "position": [300, 0]}],
        "edges": [{"id": "e2", "from": {"node": "n1", "port": "output"}, "to": {"node": "out", "port": "base_color"}},
                  {"id": "e3", "from": {"node": "n1", "port": "output"}, "to": {"node": "out", "port": "roughness"}},
                  {"id": "e4", "from": {"node": "n1", "port": "output"}, "to": {"node": "out", "port": "height"}}],
    }


def write_graph(ed: Editor, graph: dict, path: str = GRAPH_PATH) -> None:
    target = ed.project / path
    target.parent.mkdir(parents=True, exist_ok=True)
    target.write_text(json.dumps(graph, indent=2) + "\n")


def check_of(result: dict, check_id: str) -> dict:
    for check in result["checks"]:
        if check["id"] == check_id:
            return check
    raise harness.TestFailure(f"no check {check_id} in {[c['id'] for c in result['checks']]}")


def recipe_of(png: bytes) -> dict:
    """a baked PNG's jasper.procmat recipe (an uncompressed iTXt chunk)"""
    at = 8
    while at + 8 <= len(png):
        length = int.from_bytes(png[at:at + 4], "big")
        data = png[at + 8:at + 8 + length]
        if png[at + 4:at + 8] == b"iTXt" and data.startswith(b"jasper.procmat\x00"):
            return json.loads(data[len(b"jasper.procmat") + 5:].decode())
        at += 12 + length
    raise harness.TestFailure("no jasper.procmat chunk")


def chunks(png: bytes) -> list[bytes]:
    """the chunk keywords of a PNG's text chunks"""
    out, at = [], 8
    while at + 8 <= len(png):
        length = int.from_bytes(png[at:at + 4], "big")
        kind = png[at + 4:at + 8]
        if kind in (b"iTXt", b"tEXt"):
            out.append(png[at + 8:at + 8 + length].split(b"\x00", 1)[0])
        at += 12 + length
    return out


async def test_groups_are_hidden_until_enabled(ed: Editor):
    """AC13: the five graph tools moved to `graphs` (R32), the two new ones are `materials`"""
    graphs = ("list_graph_kinds", "list_graph_node_types", "get_graph", "validate_graph", "edit_graph")
    materials = ("preview_procedural_material", "bake_procedural_material")
    default = {t.name for t in await ed.tools(groups=None)}
    for name in graphs + materials:
        ed.check(name not in default, f"{name} is hidden by default")
    await ed.call("tool_groups", enable=["graphs", "materials"])
    try:
        listed = {t.name: t for t in await ed.tools(groups=None)}
        for name in graphs:
            ed.check(name in listed and tool_meta(listed[name]).get("jasper/group") == "graphs", f"{name} is in graphs")
        for name in materials:
            ed.check(name in listed and tool_meta(listed[name]).get("jasper/group") == "materials", f"{name} is in materials")
    finally:
        await ed.call("tool_groups", disable=["graphs", "materials"])
    kinds = await ed.call("list_graph_kinds")
    ed.check([k for k in kinds["kinds"] if k["id"] == "procedural_material"], "a hidden group's tool is still callable")
    write_graph(ed, stone_graph(), "assets/mats/hidden.procmat")
    hidden = await ed.call("preview_procedural_material", path="assets/mats/hidden.procmat", size=64, sheet=[])
    ed.check(hidden["checks"], "a hidden materials tool is callable too")


async def test_the_catalog_is_signature_lines(ed: Editor):
    """R33: a large kind lists its categories; a search gives one signature line per node"""
    whole = await ed.call("list_graph_node_types", kind="procedural_material")
    ed.check("categories" in whole and "signatures" not in whole, f"categories and counts only: {str(whole)[:300]}")
    ed.check(sum(c["count"] for c in whole["categories"]) == whole["total"] > 200, f"every type counted: {whole['categories']}")
    found = await ed.call("list_graph_node_types", kind="procedural_material", search="perlin")
    line = next((s for s in found["signatures"] if s.startswith("perlin(")), None)
    ed.check(line is not None, f"perlin is found: {found['signatures']}")
    ed.check_in("scale_x=4[1..32]", line)
    ed.check(line.endswith("-> output:f"), f"its output: {line}")
    one = await ed.call("list_graph_node_types", kind="procedural_material", ids=["material_output"], detail=True)
    ed.check_eq(one["node_types"][0]["id"], "material_output", "detail gives the full type")
    ed.check_in("normal_strength=1[0..4]", one["signatures"][0])
    missing = await ed.call_raw("list_graph_node_types", {"kind": "procedural_material", "ids": ["perlinn"]})
    ed.check(missing.is_error and "search" in missing.text, f"an unknown id says how to find one: {missing.text}")


async def test_preview_is_a_sheet_with_checks(ed: Editor):
    write_graph(ed, stone_graph())
    r = await ed.call_raw("preview_procedural_material", {"path": GRAPH_PATH, "size": 128, "seed": 3})
    ed.check(not r.is_error, r.text[:2000])
    data = r.structured
    ed.check_eq(set(data["device"]), {"tier", "name"}, f"the device: {data['device']}")
    ed.check_eq(data["device"]["tier"], "software", f"llvmpipe is the software tier: {data['device']}")
    ed.check_eq(data["outputs"], ["base_color", "normal", "orm", "height"], "the bake's outputs, the normal made from the height")
    ed.check_eq([c["id"] for c in data["checks"]],
                ["tileability", "repetition", "histogram", "base_color", "normal", "roughness", "metallic", "height_8bit"], "every check")
    for check in data["checks"]:
        ed.check(CHECK_KEYS <= set(check), f"{check['id']} has S35's row shape: {sorted(check)}")
    ed.check_eq(check_of(data, "normal")["status"], "PASS", f"the made normal is valid: {check_of(data, 'normal')}")
    ed.check_eq(data["fixes"], [], "the window's report shape: no image check is one edit of the graph")
    ed.check(len(r.images) == 1 and r.images[0][1] == "image/png", "one contact sheet")
    labels = [c["label"] for c in data["image"]["cells"]]
    ed.check_eq(labels, ["flat base_color", "sphere (lit)", "tiled 2x2", "base_color", "normal", "orm", "height"], "flat, ball, tiling, channels")
    again = await ed.call_raw("preview_procedural_material", {"path": GRAPH_PATH, "size": 128, "seed": 3, "sheet": ["channels"]})
    first = r.images[0][0]
    metrics = await ed.call("preview_procedural_material", path=GRAPH_PATH, size=128, seed=3, sheet=[])
    ed.check("image" not in metrics, "sheet [] measures only")
    ed.check_eq(metrics["metrics"], data["metrics"], "the same graph, seed and size measure the same")
    ed.check(again.images and again.images[0][0] != first, "a sheet of the channels alone is another image")


async def test_the_seed_is_the_graphs_unless_given(ed: Editor):
    """the window keeps a graph's seed in metadata.seed: the default of both tools, a call's seed overrides it unwritten"""
    graph = stone_graph()
    graph["metadata"] = {"seed": 7}
    write_graph(ed, graph, "assets/mats/seeded.procmat")
    kept = await ed.call("preview_procedural_material", path="assets/mats/seeded.procmat", size=64, sheet=[])
    ed.check_eq(kept["seed"], 7, "the graph's seed")
    given = await ed.call("preview_procedural_material", path="assets/mats/seeded.procmat", size=64, sheet=[], seed=3)
    ed.check_eq(given["seed"], 3, "the call's seed")
    ed.check_eq(json.loads((ed.project / "assets/mats/seeded.procmat").read_text())["metadata"], {"seed": 7}, "never written back")


async def test_a_changed_threshold_is_listed(ed: Editor):
    await ed.call("set_project_settings", values={"materials.qa.seam_fail": 5})
    try:
        result = await ed.call("preview_procedural_material", path=GRAPH_PATH, size=64, sheet=[])
        changed = {c["key"] for c in result["thresholds"]["changed"]}
        ed.check("materials.qa.seam_fail" in changed, f"changed: {result['thresholds']['changed']}")
        ed.check_eq(check_of(result, "tileability")["threshold"], 5, "the threshold used")
    finally:
        await ed.call("set_project_settings", values={"materials.qa.seam_fail": 1.5})


async def test_a_bake_is_one_undo_step(ed: Editor):
    """AC9: PNGs with their recipe, explicit import settings, the .material: one step undo removes whole"""
    steps = len((await ed.call("get_undo_history", scope="project"))["entries"])
    baked = await ed.call("bake_procedural_material", path=GRAPH_PATH, size=64, seed=3)
    ed.check_eq(len((await ed.call("get_undo_history", scope="project"))["entries"]), steps + 1, "one undo step")
    ed.check_eq(baked["undo"]["name"], "Bake Procedural Material stone", f"its name: {baked['undo']}")
    ed.check(isinstance(baked["undo"].get("id"), int), f"the undo id: {baked['undo']}")
    ed.check_eq(baked["device"]["tier"], "software", "the device")
    names = ["stone_base_color.png", "stone_normal.png", "stone_orm.png", "stone_height.png"]
    ed.check_eq(baked["images"], [f"{FOLDER}/{n}" for n in names], "the PNGs, in R14's order")
    ed.check_eq(baked["material"], f"{FOLDER}/stone.material", "the material beside them")
    for name in names:
        png = (ed.project / FOLDER / name).read_bytes()
        ed.check(b"jasper.procmat" in chunks(png), f"{name} holds its recipe")
        meta = (ed.project / FOLDER / (name + ".meta")).read_text()
        ed.check_in("textureImporter", meta)
    ed.check_in("type: normal", (ed.project / FOLDER / "stone_normal.png.meta").read_text())
    ed.check_in("srgb: true", (ed.project / FOLDER / "stone_base_color.png.meta").read_text())
    ed.check_in("type: single_channel", (ed.project / FOLDER / "stone_height.png.meta").read_text())
    orm_meta = (ed.project / FOLDER / "stone_orm.png.meta").read_text()
    ed.check("type: default" in orm_meta and "srgb: false" in orm_meta, f"an ORM is linear and not inferred: {orm_meta}")
    material = (ed.project / FOLDER / "stone.material").read_text()
    for slot, name in (("baseColorTexture", "stone_base_color.png"), ("normalTexture", "stone_normal.png"),
                       ("metallicRoughnessTexture", "stone_orm.png"), ("occlusionTexture", "stone_orm.png")):
        ed.check(slot in material and name in material, f"{slot} holds {name}: {material}")

    await ed.call("undo", scope="project")
    left = [n for n in names + [n + ".meta" for n in names] + ["stone.material"] if (ed.project / FOLDER / n).exists()]
    ed.check(not left, f"undo removes every file: {left}")
    await ed.call("redo", scope="project")
    ed.check((ed.project / FOLDER / "stone.material").exists() and (ed.project / FOLDER / names[0]).exists(), "redo writes them again")


async def test_a_rebake_keeps_a_hand_edited_field(ed: Editor):
    material_path = f"{FOLDER}/stone.material"
    await ed.call("update_material", path=material_path, values={"alphaCutoff": 0.3, "stochasticTiling": True})
    write_graph(ed, stone_graph(scale=6.0))
    await ed.call("sync_files")
    await ed.call("bake_procedural_material", path=GRAPH_PATH, size=64, seed=3)
    text = (ed.project / material_path).read_text()
    material = yaml.safe_load(text)
    ed.check(abs(float(material.get("alphaCutoff", 0)) - 0.3) < 1e-6, f"the hand-edited cutoff survives: {text}")
    ed.check(material.get("stochasticTiling") is True, f"so does stochastic tiling: {text}")
    ed.check("stone_base_color.png" in text, "the slots are the bake's")


async def test_a_rebake_undoes_to_the_earlier_bake(ed: Editor):
    """undo of a re-bake puts back the earlier PNG and meta bytes, rather than removing them"""
    before = {n: (ed.project / FOLDER / n).read_bytes() for n in ("stone_base_color.png", "stone_base_color.png.meta", "stone.material")}
    await ed.call("bake_procedural_material", path=GRAPH_PATH, size=64, seed=9)
    ed.check((ed.project / FOLDER / "stone_base_color.png").read_bytes() != before["stone_base_color.png"], "another seed, other pixels")
    await ed.call("undo", scope="project")
    for name, data in before.items():
        ed.check_eq((ed.project / FOLDER / name).read_bytes(), data, f"undo puts {name} back")


async def test_a_partial_rebake_keeps_the_other_slots(ed: Editor):
    """I1: a re-bake of base_color alone leaves the normal and ORM slots of the earlier bake"""
    await ed.call("bake_procedural_material", path=GRAPH_PATH, size=64, seed=3, outputs=["base_color"])
    material = yaml.safe_load((ed.project / FOLDER / "stone.material").read_text())
    for slot, name in (("normalTexture", "stone_normal.png"), ("metallicRoughnessTexture", "stone_orm.png"), ("occlusionTexture", "stone_orm.png")):
        ed.check(name in json.dumps(material.get(slot)), f"{slot} still holds {name}: {material.get(slot)}")


async def test_a_baked_material_previews_from_its_files(ed: Editor):
    result = await ed.call("preview_procedural_material", path=f"{FOLDER}/stone.material", sheet=[])
    ed.check_eq(result["source"]["kind"], "material", "a .material")
    ed.check_eq(sorted(result["outputs"]), ["base_color", "normal", "orm"], f"its slots' images: {result['outputs']}")
    ed.check_eq(check_of(result, "normal")["status"], "PASS", f"{check_of(result, 'normal')}")
    ed.check_eq(check_of(result, "height_8bit")["status"], "SKIP", "no slot holds a height")


async def test_a_sheet_past_its_wait_bound_is_an_error(ed: Editor):
    """AC14: the lit ball still pending past timeout_seconds: an error and no image, never the previous material's"""
    write_graph(ed, stone_graph(scale=8.0), "assets/mats/other.procmat")
    marker = ed.project / "sphere-pending"
    marker.write_text("")
    try:
        r = await ed.call_raw("preview_procedural_material", {"path": "assets/mats/other.procmat", "size": 64, "timeout_seconds": 3})
        ed.check(r.is_error, f"past its bound it fails: {r.text[:500]}")
        ed.check_in("Not ready within 3 s", r.text)
        ed.check(not r.images, "no image of anything")
    finally:
        marker.unlink()
    r = await ed.call_raw("preview_procedural_material", {"path": "assets/mats/other.procmat", "size": 64, "sheet": ["flat", "sphere"]})
    ed.check(not r.is_error and len(r.images) == 1, f"once the ball draws, the sheet comes: {r.text[:500]}")
    stone = await ed.call_raw("preview_procedural_material", {"path": GRAPH_PATH, "size": 64, "sheet": ["flat", "sphere"]})
    again = await ed.call_raw("preview_procedural_material", {"path": "assets/mats/other.procmat", "size": 64, "sheet": ["flat", "sphere"]})
    ed.check(r.images[0][0] != stone.images[0][0], "the sheet is not the material previewed before")
    ed.check_eq(r.images[0][0], again.images[0][0], "it is other.procmat's: the same request gives the same sheet")


async def test_check_reports_a_stale_bake(ed: Editor):
    """R16 through R39's check --materials: fresh after the bake, a warning once the graph changes"""
    cmd = harness.editor_command() + ["check", "--project", str(ed.project), "--materials"]
    fresh = subprocess.run(cmd, capture_output=True, text=True, timeout=600)
    ed.check_eq(fresh.returncode, 0, (fresh.stdout + fresh.stderr)[-2000:])
    ed.check("stale" not in fresh.stdout, f"nothing stale yet: {fresh.stdout[-2000:]}")
    await ed.call("edit_graph", path=GRAPH_PATH, ops=[{"op": "setParam", "node": "n1", "name": "persistence", "value": 0.8}])
    stale = subprocess.run(cmd, capture_output=True, text=True, timeout=600)
    ed.check_eq(stale.returncode, 0, "a stale bake is a warning")
    ed.check_in(f"{FOLDER}/stone_base_color.png: warning: a stale bake: {GRAPH_PATH} changed since this bake", stale.stdout)
    await ed.call("edit_graph", path=GRAPH_PATH, ops=[{"op": "setParam", "node": "n1", "name": "persistence", "value": 0.5}])
    back = subprocess.run(cmd, capture_output=True, text=True, timeout=600)
    ed.check("stale" not in back.stdout, f"edited back, fresh again: {back.stdout[-2000:]}")


async def test_the_cli_bake(ed: Editor):
    """R39: on a copy of the project, with no editor holding it: exit 0 and the files; 2 on an unknown flag; 1 on a bad graph"""
    with tempfile.TemporaryDirectory() as scratch:
        copy = Path(scratch) / "project"
        shutil.copytree(ed.project, copy, ignore=shutil.ignore_patterns(".jasperengine"))
        shutil.rmtree(copy / FOLDER)
        base = harness.editor_command() + ["procedural-material", "bake"]
        unknown = subprocess.run(base + [str(copy / GRAPH_PATH), "--colour"], capture_output=True, text=True, timeout=600)
        ed.check_eq(unknown.returncode, 2, f"an unknown flag: {unknown.stderr[-1000:]}")
        ok = subprocess.run(base + [str(copy / GRAPH_PATH), "--size", "64"], capture_output=True, text=True, timeout=1200)
        ed.check_eq(ok.returncode, 0, (ok.stdout + ok.stderr)[-3000:])
        ed.check((copy / FOLDER / "stone_orm.png").exists() and (copy / FOLDER / "stone.material").exists(), "the files")
        # AC5: no display and nothing on PATH, and no editor within the spawn bound (the editor's PATH bootstrap
        # would find Xvfb in /usr/bin again): the bake runs in this process on tier 3
        shutil.rmtree(copy / FOLDER)
        env = {k: v for k, v in os.environ.items() if k not in ("DISPLAY", "WAYLAND_DISPLAY")}
        env.update(PATH=str(Path(scratch) / "empty-path"), JASPER_CALL_SPAWN_TIMEOUT="0.001")
        cpu = subprocess.run(base + [str(copy / GRAPH_PATH), "--size", "64"], capture_output=True, text=True, timeout=1200, env=env)
        ed.check_eq(cpu.returncode, 0, (cpu.stdout + cpu.stderr)[-3000:])
        ed.check_in("baking on the cpu tier", cpu.stderr)
        recipe = recipe_of((copy / FOLDER / "stone_orm.png").read_bytes())
        ed.check_eq(recipe["tier"], "cpu", f"the recipe says where it was baked: {recipe}")
        ed.check((copy / FOLDER / "stone.material").exists(), "and the .material")
        sized = subprocess.run(base + [str(copy / GRAPH_PATH), "--size", "100"], capture_output=True, text=True, timeout=600)
        ed.check_eq(sized.returncode, 2, f"a size that is no power of two is a usage error: {sized.stderr[-500:]}")
        bad = stone_graph()
        bad["nodes"][0]["params"]["scale_q"] = 1
        (copy / "assets/mats/bad.procmat").write_text(json.dumps(bad))
        failed = subprocess.run(base + [str(copy / "assets/mats/bad.procmat")], capture_output=True, text=True, timeout=1200)
        ed.check_eq(failed.returncode, 1, (failed.stdout + failed.stderr)[-3000:])
        ed.check_in("unknown-param", failed.stdout + failed.stderr)


async def test_a_baked_png_ships_without_its_recipe(ed: Editor):
    """AC2 for baked PNGs: the export strips jasper.* chunks, and ships no .procmat"""
    await ed.call("save_project")
    out = ed.scratch.root / "export"
    r = await ed.call("build_project", mode="source", output_dir=str(out), platform="linux", backends=["opengl"], allow_missing_assets=True,
                      overwrite=True, timeout_seconds=900)
    ed.check_eq(r["status"], "succeeded", f"{r}")
    files = [p for p in (out / "project" / "assets").rglob("*") if p.is_file()]
    ed.check(not [p for p in files if p.suffix == ".procmat"], "no .procmat ships")
    shipped = [p for p in files if p.name == "stone_base_color.png" or (p.name == "index.json" and b"stone_base_color.png" in p.read_bytes())]
    ed.check(shipped, f"the baked base colour ships, as its PNG or a cook of it: {[str(p) for p in files][:30]}")
    carrying = [str(p) for p in files if b"jasper.procmat" in p.read_bytes()]
    ed.check(not carrying, f"no recipe ships: {carrying}")


async def test_a_device_failure_continues_on_the_cpu_with_a_warning(ed: Editor):
    """AC4, MCP half: the headless editor is the software tier; a pass that fails to compile on it continues on the
    CPU, the result names the cpu tier and warns which pass failed on which device"""
    data = await ed.call("preview_procedural_material", path=GRAPH_PATH, size=64, sheet=[])
    ed.check_eq(data["device"]["tier"], "software", f"llvmpipe and lavapipe are software: {data['device']}")
    async with Editor(create=True, name="Procmat device failure", env={"JASPER_TEST_PROCMAT_DEVICE_FAIL": "compile:normal"}) as failing:
        write_graph(failing, stone_graph())
        await failing.call("sync_files")
        r = await failing.call_raw("preview_procedural_material", {"path": GRAPH_PATH, "size": 64, "sheet": []})
        failing.check(not r.is_error, r.text[:1000])
        failing.check_eq(r.structured["device"]["tier"], "cpu", f"it continued on the CPU: {r.structured['device']}")
        warnings = " ".join(r.structured.get("warnings", []))
        failing.check("(normal) failed on" in warnings and "llvmpipe" in warnings.lower(), f"the warning names the pass and the device: {warnings}")
        failing.check_in("Warning: ", r.text)


async def test_a_tileability_fail_names_its_node_and_edit(ed: Editor):
    """R27, R36: a seam a scale with a fraction makes names the node, and its fix is one edit the window applies"""
    write_graph(ed, stone_graph(scale=4.5), "assets/mats/seam.procmat")
    seam = await ed.call("preview_procedural_material", path="assets/mats/seam.procmat", size=128, sheet=[])
    row = check_of(seam, "tileability")
    ed.check_eq(row["status"], "FAIL", f"a scale of 4.5 does not tile: {row}")
    ed.check_in("node n1 (perlin) has scale_x 4.5", row["limited_by"])
    fixes = [f for f in seam["fixes"] if f["check"] == "tileability"]
    ed.check(fixes, f"a single edit: {seam['fixes']}")
    for fix in fixes:
        await ed.call("edit_graph", path="assets/mats/seam.procmat", ops=[fix["op"]])
    tiled = await ed.call("preview_procedural_material", path="assets/mats/seam.procmat", size=128, sheet=[])
    ed.check_eq(check_of(tiled, "tileability")["status"], "PASS", f"applied, it tiles: {check_of(tiled, 'tileability')}")


async def test_a_bake_finishing_in_another_project_writes_nothing(ed: Editor):
    """I5: open another project while a bake runs: nothing lands in it (runs last, it switches the project)"""
    # (in the harness's scratch, which outlives the editor that will hold it open)
    other = ed.scratch.root / "other"
    shutil.copytree(ed.project, other, ignore=shutil.ignore_patterns(".jasperengine"))
    shutil.rmtree(other / FOLDER, ignore_errors=True)
    started = await ed.call("bake_procedural_material", path=GRAPH_PATH, size=2048, wait=False)
    await ed.call("open_project", path=str(other), unsaved="discard")
    for _ in range(120):
        job = await ed.call("get_job", job_id=started["job_id"])
        if job["status"] != "running":
            break
        await ed.call("wait", frames=10)
    ed.check(job["status"] in ("failed", "cancelled"), f"the bake did not finish into the other project: {job}")
    ed.check(not (other / FOLDER).exists(), f"nothing written into {other / FOLDER}")
