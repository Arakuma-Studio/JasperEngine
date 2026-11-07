"""S77 R24, R25, R51-R53, R55 (AC9, AC11, AC19): check_animation and optimize_animation are hidden until
tool_groups enables them, run as jobs, preview before they write, and the journey reduce + prove takes
3 calls with numbers measured from the file. `check --anims` plants each of its codes; a clean clip
triggers none. A dense clip is sampled from the fixture's Walk here, since its Walk has few keys.

Runs on a scratch copy of tools/fixtures/pose-animation.
"""

from __future__ import annotations

import hashlib
import json
import math
import shutil
import subprocess
import tempfile
from pathlib import Path

import harness
from harness import Editor, REPO, TestFailure
from project_fixture import write_project

CI = True
EDITOR = {"project": REPO / "tools" / "fixtures" / "pose-animation"}
TIMEOUT = 900

MODEL = "assets/models/hero_animated.gltf"
WALK = f"{MODEL}#Walk"
DENSE = "assets/anim/dense_walk.anim"
OUTPUT = "assets/anim/dense_walk.opt.anim"
STATE: dict = {}


def tree_hash(root: Path) -> dict:
    """every file of the project but the editor's own cache: a preview must leave all of it alone
    (a dict, so a failure names the file that changed)"""
    return {str(p.relative_to(root)): hashlib.sha1(p.read_bytes()).hexdigest()
            for p in sorted(root.rglob("*")) if p.is_file() and ".jasperengine" not in p.parts}


def changed(before: dict, after: dict) -> list:
    return sorted(k for k in set(before) | set(after) if before.get(k) != after.get(k))


def clip_text(tracks: list, wrap: str = "once", preview: str | None = "models/hero_animated.gltf", extra: dict | None = None) -> str:
    # with a GUID: the asset database gives a clip without one its own, a write the hashes below would see
    guid = hashlib.md5(json.dumps(tracks).encode()).hexdigest()
    data = {"format": "jasperengine.anim", "version": 1, "guid": guid, "wrap": wrap, "tracks": tracks, "events": []}
    if preview:
        data["preview"] = {"model": preview}
    data.update(extra or {})
    return json.dumps(data)


async def write_clip(ed: Editor, path: str, text: str) -> None:
    await ed.call("write_file", path=path, content=text)


async def dense_walk(ed: Editor) -> str:
    """the fixture's Walk sampled at 120 Hz through the runtime sampler: real poses, dense keys"""
    if "dense" in STATE:
        return STATE["dense"]
    info = await ed.call("get_animation", clip=WALK, max_keys=1)
    length = info["length"]
    times = [round(i / 120.0, 6) for i in range(int(length * 120) + 1)]
    sampled = await ed.call("get_animation", clip=WALK, times=times[:64], max_keys=1)
    tracks = []
    positions = {t["index"]: t for t in info["tracks"]}
    rows: dict[int, list] = {}
    for start in range(0, len(times), 64):
        chunk = times[start:start + 64]
        sampled = await ed.call("get_animation", clip=WALK, times=chunk, max_keys=1)
        for row in sampled["samples"]["tracks"]:
            rows.setdefault(row["index"], []).extend(zip(chunk, row["values"]))
    for index, keys in rows.items():
        track = positions[index]
        if track["channel"] not in ("position", "rotation", "scale"):
            continue
        out = []
        for time, value in keys:
            if isinstance(value, dict):
                value = value["quat"]
            out.append([time, value])
        tracks.append({"path": track["path"], "channel": track["channel"], "keys": out})
    text = clip_text(tracks, wrap="loop")
    await write_clip(ed, DENSE, text)
    STATE["dense"] = DENSE
    return DENSE


async def test_group_hidden_until_enabled(ed: Editor):
    listed = {t.name for t in await ed.tools(groups=None)}
    for name in ("check_animation", "optimize_animation"):
        ed.check(name not in listed, f"{name} is in a new session's listing")
    r = await ed.call("tool_groups", enable=["animation"])
    ed.check(r is not None, "tool_groups answered")
    after = {t.name for t in await ed.tools(groups=["animation"])}
    for name in ("check_animation", "optimize_animation"):
        ed.check(name in after, f"{name} is listed once animation is on: {sorted(after)}")
    check = ed.tool("check_animation")
    ed.check(check.annotations.read_only_hint is True, "check_animation is read-only")
    enum = check.input_schema["properties"]["checks"]["items"]["enum"]
    ed.check_eq(sorted(enum), sorted(["stats", "loop_seam", "root_motion", "foot_sliding", "transition"]), "every check is listed now")
    for name in ("transition", "feet", "move_speed", "record_seconds", "limits", "top", "compare_to", "entity", "scene", "state"):
        ed.check_in(name, check.input_schema["properties"], "check_animation argument")
    optimize = ed.tool("optimize_animation")
    ed.check(optimize.annotations.read_only_hint is not True, "optimize_animation writes")
    ed.check(optimize.annotations.idempotent_hint is True, "the same arguments give the same file")


async def test_checks_of_a_later_stream_say_so(ed: Editor):
    for args in ({"checks": ["foot_sliding"]}, {"checks": ["transition"]}, {"record_seconds": 3}, {"feet": ["LeftFoot"]},
                 {"move_speed": 1.2}, {"transition": {"from": "A", "to": "B"}}, {"limits": {"foot_slide_mm": 15}}):
        r = await ed.call_raw("check_animation", {"clips": [WALK], **args})
        ed.check(r.is_error and "not_available" in r.text, f"{args}: {r.text[:200]}")


async def test_stats_project_and_model(ed: Editor):
    one = await ed.call("check_animation", clips=[WALK], checks=["stats"])
    ed.check(one["ok"] is True and one["status"] == "succeeded", f"{one}")
    ed.check_eq(one["totals"]["clips"], 1, "one clip")
    entry = one["clips"][0]
    stats = (await ed.call("get_animation", clip=WALK, max_keys=1))["stats"]
    for key in stats:
        ed.check_eq(entry[key], stats[key], f"the same number as get_animation: {key}")

    model = await ed.call("check_animation", clips=[MODEL], checks=["stats"])
    info = await ed.call("get_asset_info", path=MODEL)
    ed.check_eq(model["totals"]["clips"], len(info["model"]["animations"]), "a model means all its clips")

    project = await ed.call("check_animation", checks=["stats"], top=1)
    ed.check(project["totals"]["clips"] >= model["totals"]["clips"], f"{project['totals']}")
    ed.check_eq(len(project["clips"]), 1, "top")
    biggest = max(a["runtime_bytes"] for a in info["model"]["animations"])
    ed.check(project["clips"][0]["runtime_bytes"] >= biggest, "the largest first")

    background = await ed.call("check_animation", clips=[WALK], checks=["stats"], wait=False)
    ed.check("job_id" in background, f"a job: {background}")


async def test_journey_preview_write_prove(ed: Editor):
    """AC19: three calls, real numbers. The words promise nothing: they say what was measured."""
    clip = await dense_walk(ed)
    root = Path(ed.project)
    before = tree_hash(root)

    # 1. the preview
    preview = await ed.call_raw("optimize_animation", {"clips": [clip]})
    ed.check(not preview.is_error, preview.text[:400])
    data = preview.structured
    ed.check(data["written"] is False, f"{data}")
    ed.check(data["undo"] is None, f"{data}")
    entry = data["clips"][0]
    ed.check(entry["written_to"] is None, f"{entry}")
    ed.check(entry["keys_after"] < entry["keys_before"], f"dense keys on linear segments: {entry}")
    ed.check(entry["max_error_mm"] <= data["effective_tolerance_mm"], f"{entry} {data['effective_tolerance_mm']}")
    ed.check(entry["worst_joint"], f"{entry}")
    ed.check("Nothing was written" in preview.text, preview.text)
    for word in ("save", "saving", "smaller", "faster"):
        ed.check(word not in preview.text.lower(), f"the text promises: {preview.text}")
    ed.check_eq(changed(before, tree_hash(root)), [], "a preview writes nothing")
    ed.check(len(json.dumps(data)) < 2500, f"the result is small: {len(json.dumps(data))} chars")

    # 2. the same arguments + output
    written = await ed.call("optimize_animation", clips=[clip], output=True)
    ed.check(written["written"] is True, f"{written}")
    ed.check_eq(written["clips"][0]["written_to"], OUTPUT, "beside the source, .opt.anim")
    ed.check_eq(written["undo"], {"scope": "project"}, "one project undo step")
    ed.check_eq(written["clips"][0]["keys_after"], entry["keys_after"], "exactly what the preview said")
    output = root / OUTPUT
    on_disk = json.loads(output.read_text())
    record = on_disk["optimize"]
    ed.check_eq(record["tolerance_mm"], 1, "the recipe")
    ed.check_eq(record["skin_distance_mm"], 100, "the recipe")
    ed.check_eq(record["source"], clip, "what it was made from")
    ed.check_eq(record["source_sha1"], hashlib.sha1((root / clip).read_bytes()).hexdigest(), "the source's hash")
    ed.check(on_disk["guid"] != json.loads((root / clip).read_text()).get("guid"), "its own GUID")

    # 3. the proof: no warning about the record, within the tolerance it recorded
    proof = await ed.call("check_animation", clips=[OUTPUT], compare_to=clip)
    ed.check(proof["ok"] is True, f"{proof['findings']}")
    compare = proof["clips"][0]["compare"]
    ed.check_eq(compare["tolerance_mm"], 1, "the clip's own record")
    ed.check(compare["within_tolerance"] is True, f"{compare}")
    ed.check(compare["max_error_mm"] <= 1, f"{compare}")
    ed.check(compare["max_error_mm"] == written["clips"][0]["max_error_mm"], f"one number: {compare} {written['clips'][0]}")
    ed.check(compare["events_match"] and compare["length_matches"], f"{compare}")
    got = await ed.call("get_animation", clip=OUTPUT, max_keys=1)
    ed.check_eq(got["warnings"], [], "optimize does not warn")
    STATE["compare"] = compare

    # undo takes the file away, redo puts the same bytes back
    bytes_written = output.read_bytes()
    await ed.call("undo", scope="project")
    ed.check(not output.exists(), "undo removes the new file")
    ed.check_eq(changed(before, tree_hash(root)), [], "undo restores the tree")
    await ed.call("redo", scope="project")
    ed.check_eq(output.read_bytes(), bytes_written, "redo")


async def test_a_planted_move_fails_the_compare(ed: Editor):
    root = Path(ed.project)
    data = json.loads((root / OUTPUT).read_text())
    # the first translation track: 5 mm on its last key
    track = next(t for t in data["tracks"] if t["channel"] == "position")
    track["keys"][-1][1][0] += 0.005
    await write_clip(ed, "assets/anim/planted.anim", json.dumps(data))
    r = await ed.call_raw("check_animation", {"clips": ["assets/anim/planted.anim"], "compare_to": DENSE, "limits": {"error_mm": 1}})
    ed.check(not r.is_error, f"ok false is a successful call: {r.text[:300]}")
    out = r.structured
    ed.check(out["ok"] is False, f"{out}")
    compare = out["clips"][0]["compare"]
    ed.check(compare["within_tolerance"] is False and compare["max_error_mm"] > 4.5, f"{compare}")
    ed.check("anim-compare-error" in {f["code"] for f in out["findings"]}, f"{out['findings']}")
    finding = next(f for f in out["findings"] if f["code"] == "anim-compare-error")
    for key in ("tool", "path", "line", "col", "severity", "message", "suggestion"):
        ed.check_in(key, finding, "the ProjectChecks shape")
    ed.check("ok: false" in r.text, r.text)


async def test_loop_seam_root_drift_and_a_clean_clip(ed: Editor):
    info = await ed.call("get_model_info", entity="Hero", include_bones=True)
    bones = {b["name"]: b for b in info["bones"]}
    ed.check(all("parent" in b for b in info["bones"]), f"bones[i].parent: {info['bones'][:2]}")
    ed.check(any(b["parent"] is None for b in info["bones"]), "a root bone has no parent")
    child = next(b for b in info["bones"] if b["parent"] is not None)
    ed.check_in(child["parent"], bones, "a parent is a bone")

    walk = await ed.call("get_animation", clip=WALK, max_keys=1)
    moved = next(t for t in walk["tracks"] if t["channel"] == "rotation")
    path = moved["path"]
    still = [0, 0, 0, 1]
    half = math.sin(math.radians(30))
    turned = [half, 0, 0, math.cos(math.radians(30))]

    # a seam: the rotation ends somewhere else, on a Loop clip
    await write_clip(ed, "assets/anim/seam.anim", clip_text([{"path": path, "channel": "rotation", "keys": [[0, still], [1, turned]]}], wrap="loop"))
    seam = await ed.call("check_animation", clips=["assets/anim/seam.anim"])
    ed.check("anim-loop-seam" in {f["code"] for f in seam["findings"]}, f"{seam}")
    ed.check(seam["clips"][0]["loop_seam"]["max_error_mm"] > 10, f"{seam['clips'][0]}")
    # the same keys on a Once clip have no seam to show
    await write_clip(ed, "assets/anim/once.anim", clip_text([{"path": path, "channel": "rotation", "keys": [[0, still], [1, turned]]}], wrap="once"))
    once = await ed.call("check_animation", clips=["assets/anim/once.anim"])
    ed.check("anim-loop-seam" not in {f["code"] for f in once["findings"]}, f"{once['findings']}")

    # drift: a root that creeps 50 mm in 1 s on what is meant to stay in place
    moving = next(t for t in walk["tracks"] if t["channel"] == "position")
    rest = (await ed.call("get_animation", clip=WALK, times=[0], max_keys=1))["samples"]["tracks"]
    start = next(r["values"][0] for r in rest if r["index"] == moving["index"])
    end = [start[0] + 0.05, start[1], start[2]]
    await write_clip(ed, "assets/anim/drift.anim", clip_text([{"path": moving["path"], "channel": "position", "keys": [[0, start], [1, end]]}], wrap="loop"))
    drift = await ed.call("check_animation", clips=["assets/anim/drift.anim"], checks=["root_motion"])
    ed.check("anim-root-drift" in {f["code"] for f in drift["findings"]}, f"{drift}")
    ed.check(abs(drift["clips"][0]["root_motion"]["net_translation_m"][0] - 0.05) < 1e-3, f"{drift['clips'][0]}")
    loose = await ed.call("check_animation", clips=["assets/anim/drift.anim"], checks=["root_motion"], limits={"root_drift_mm": 100})
    ed.check(loose["ok"] is True, f"the limit is the agent's: {loose['findings']}")

    # clean: first and last keys match
    await write_clip(ed, "assets/anim/clean.anim", clip_text(
        [{"path": path, "channel": "rotation", "keys": [[0, still], [0.5, turned], [1, still]]}], wrap="loop"))
    clean = await ed.call("check_animation", clips=["assets/anim/clean.anim"])
    ed.check(clean["ok"] is True, f"{clean['findings']}")
    ed.check(clean["clips"][0]["loop_seam"]["max_error_mm"] < 1, f"{clean['clips'][0]}")


async def test_no_rig_is_an_error_with_its_fix(ed: Editor):
    await write_clip(ed, "assets/anim/norig.anim", clip_text([{"path": "Bone", "channel": "position", "keys": [[0, [0, 0, 0]], [0.5, [1, 0, 0]], [1, [2, 0, 0]]]}], preview=None))
    r = await ed.call_raw("optimize_animation", {"clips": ["assets/anim/norig.anim"]})
    ed.check(r.is_error and "no_rig" in r.text, r.text[:300])
    ed.check("Fix: pass on" in r.text, f"names the fix: {r.text[:300]}")
    errors = r.structured["errors"]
    ed.check_eq(errors[0]["kind"], "no_rig", "a named kind")
    check = await ed.call("check_animation", clips=["assets/anim/norig.anim"])
    ed.check("rig_error" in check["clips"][0] and "anim-no-rig" in {f["code"] for f in check["findings"]}, f"{check}")
    # with a GameObject there is a rig; this path is on none of it, and that is said too
    r = await ed.call_raw("optimize_animation", {"clips": ["assets/anim/norig.anim"], "on": "Hero"})
    ed.check(r.is_error and "nothing_measured" in r.text, r.text[:300])


async def test_output_rules(ed: Editor):
    root = Path(ed.project)
    r = await ed.call_raw("optimize_animation", {"clips": [DENSE], "output": True})
    ed.check(r.is_error and "output_exists" in r.text, r.text[:300])
    ed.check_eq(r.structured["errors"][0]["kind"], "output_exists", "a named kind")
    again = await ed.call("optimize_animation", clips=[DENSE], output=True, overwrite=True)
    ed.check(again["written"] is True, f"{again}")

    # a model's clip writes a new .anim; the model is never changed
    model_before = (root / MODEL).read_bytes()
    r = await ed.call_raw("optimize_animation", {"clips": [WALK], "output": MODEL, "overwrite": True})
    ed.check(r.is_error and "never changed" in r.text, r.text[:300])
    out = await ed.call("optimize_animation", clips=[WALK], output="assets/anim/walk_from_model.anim")
    ed.check_eq(out["clips"][0]["written_to"], "assets/anim/walk_from_model.anim", "the path given")
    ed.check_eq((root / MODEL).read_bytes(), model_before, "the model")
    made = json.loads((root / "assets/anim/walk_from_model.anim").read_text())
    ed.check_eq(made["source"]["kind"], "model", "its source")
    proof = await ed.call("check_animation", clips=["assets/anim/walk_from_model.anim"], compare_to=WALK, limits={"error_mm": 1})
    ed.check(proof["clips"][0]["compare"]["within_tolerance"] is True, f"{proof}")

    # Replace: only explicit, and one undo step puts the bytes back
    source_before = (root / DENSE).read_bytes()
    replaced = await ed.call("optimize_animation", clips=[DENSE], output=DENSE, overwrite=True)
    ed.check(replaced["written"] is True, f"{replaced}")
    now = root / DENSE
    ed.check(now.read_bytes() != source_before, "replaced")
    ed.check(len(json.loads(now.read_text())["tracks"]) > 0, "still a clip")
    ed.check("source" not in json.loads(now.read_text())["optimize"], "nothing left to go stale against")
    await ed.call("undo", scope="project")
    ed.check_eq(now.read_bytes(), source_before, "undo restores the source's bytes")


async def test_root_switches_the_player_in_one_scene_step(ed: Editor):
    await ed.call("add_component", entity="Hero", component="animationplayer", values={"animationplayer": {"clips": [{"clip": "anim/clean.anim"}]}})
    r = await ed.call("optimize_animation", clips=["assets/anim/clean.anim"], output="assets/anim/clean_switched.anim", root="Hero")
    ed.check_eq(r["undo"], {"scope": "scene"}, "the player's change and the file are one scene step")
    players = (await ed.call("get_animation", entity="Hero"))["players"]
    ed.check_eq(players[0]["states"][0]["clip"], "assets/anim/clean_switched.anim", f"{players}")
    ed.check_eq(players[0]["states"][0]["name"], "clean", "the state keeps its name")
    await ed.call("undo", scope="scene")
    players = (await ed.call("get_animation", entity="Hero"))["players"]
    ed.check_eq(players[0]["states"][0]["clip"], "assets/anim/clean.anim", "undo")
    ed.check(not (Path(ed.project) / "assets/anim/clean_switched.anim").exists(), "and the file")


async def test_tolerance_and_skin_change_the_answer(ed: Editor):
    fine = await ed.call("optimize_animation", clips=[DENSE], tolerance_mm=0.05)
    coarse = await ed.call("optimize_animation", clips=[DENSE], tolerance_mm=20)
    ed.check(coarse["clips"][0]["keys_after"] <= fine["clips"][0]["keys_after"], f"{fine['clips'][0]} {coarse['clips'][0]}")
    ed.check(fine["clips"][0]["max_error_mm"] <= 0.05, f"{fine['clips'][0]}")
    # D5: a floor, not an error; the result says what was used
    floored = await ed.call("optimize_animation", clips=[DENSE], skin_distance_mm=5)
    ed.check_eq(floored["skin_distance_mm"], 10, "raised to the floor")
    ed.check_eq(floored["clips"][0]["skin_distance_mm"], 10, "measured at the floor")
    r = await ed.call_raw("optimize_animation", {"clips": [DENSE], "joints": [{"path": "NoSuchBone", "tolerance_mm": 1}]})
    ed.check(r.is_error and "unknown_joint" in r.text, r.text[:200])


async def test_skeleton_summary(ed: Editor):
    plain = await ed.call("get_asset_info", path=MODEL)
    ed.check("skeleton_summary" not in plain["model"], "only when asked")
    info = await ed.call("get_asset_info", path=MODEL, skeleton=True)
    summary = info["model"]["skeleton_summary"]
    bones = await ed.call("get_model_info", entity="Hero")
    ed.check(summary["joint_count"] >= summary["skinned_joints"] > 0, f"{summary}")
    ed.check(summary["max_depth"] > 1, f"{summary}")
    ed.check(len(summary["roots"]) >= 1, f"{summary}")
    roots = {b["name"] for b in bones["bones"] if b["parent"] is None}
    ed.check(set(summary["roots"]) <= roots, f"{summary['roots']} {roots}")


# ---------------------------------------------------------------- check --anims (R25, AC11)

def check(condition, message: str) -> None:
    if not condition:
        raise TestFailure(message)


def check_eq(actual, expected, message: str) -> None:
    check(actual == expected, f"{message}: expected {expected!r}, got {actual!r}")


def run_anims(project: Path, *args: str) -> tuple[int, dict]:
    r = subprocess.run(harness.editor_command() + ["check", "--project", str(project), "--anims", "--format", "json", *args],
                       cwd=str(project), input="", capture_output=True, text=True, timeout=120)
    return r.returncode, json.loads(r.stdout)


def rotation_y(degrees: float) -> list:
    return [0, math.sin(math.radians(degrees) / 2), 0, math.cos(math.radians(degrees) / 2)]


async def test_cli_codes():
    tmp = Path(tempfile.mkdtemp(prefix="jasper-anim-quality-"))
    try:
        project = write_project(tmp / "project")
        anims = project / "assets/anim"
        anims.mkdir(parents=True)

        def write(name: str, tracks: list, **extra) -> str:
            data = {"format": "jasperengine.anim", "version": 1, "tracks": tracks, "events": []}
            data.update(extra)
            (anims / name).write_text(json.dumps(data))
            return f"assets/anim/{name}"

        moving = {"path": "Bone", "channel": "position", "keys": [[0, [0, 0, 0]], [0.5, [1, 0, 0]], [1, [0, 0, 0]]]}
        clean = write("clean.anim", [moving])
        redundant = write("redundant.anim", [{"path": "Bone", "channel": "position", "keys": [[i / 10, [i / 10, 0, 0]] for i in range(11)]}])
        constant = write("constant.anim", [moving, {"path": "Other", "channel": "position", "keys": [[0, [1, 2, 3]], [1, [1, 2, 3]]]}])
        flipped = write("flipped.anim", [{"path": "Bone", "channel": "rotation", "keys": [[0, [0, 0, 0, 1]], [1, [0, 0, -0.1, -0.994987]]]}])
        wide = write("wide.anim", [{"path": "Bone", "channel": "rotation", "keys": [[0, rotation_y(0)], [1, rotation_y(170)]]}])
        source = anims / "source.anim"
        source.write_text(json.dumps({"format": "jasperengine.anim", "version": 1, "tracks": [moving], "events": []}))
        good_hash = hashlib.sha1(source.read_bytes()).hexdigest()
        fresh = write("fresh.opt.anim", [moving], optimize={"tolerance_mm": 1, "skin_distance_mm": 100, "source_sha1": good_hash, "max_error_mm": 0.5, "source": "assets/anim/source.anim"})
        stale = write("stale.opt.anim", [moving], optimize={"tolerance_mm": 1, "skin_distance_mm": 100, "source_sha1": "0" * 40, "max_error_mm": 0.5, "source": "assets/anim/source.anim"})
        gone = write("gone.opt.anim", [moving], optimize={"tolerance_mm": 1, "skin_distance_mm": 100, "source_sha1": good_hash, "max_error_mm": 0.5, "source": "assets/anim/nothing.anim"})

        code, data = run_anims(project)
        check_eq(code, 0, "warnings keep the exit code")
        by_path: dict[str, set] = {}
        for item in data["findings"]:
            by_path.setdefault(item["path"], set()).add(item.get("code", ""))
            check_eq(item["severity"], "warning", f"{item}")
        check(clean not in by_path, f"a clean clip triggers none: {by_path.get(clean)}")
        check(fresh not in by_path, f"a fresh reduced clip triggers none: {by_path.get(fresh)}")
        check("anim-redundant-keys" in by_path.get(redundant, set()), f"{by_path}")
        check("anim-constant-track" in by_path.get(constant, set()), f"{by_path}")
        check("anim-rotation-flip" in by_path.get(flipped, set()), f"{by_path}")
        check("anim-rotation-flip" in by_path.get(wide, set()), f"{by_path}")
        check("anim-optimize-stale" in by_path.get(stale, set()), f"{by_path}")
        check("anim-optimize-stale" in by_path.get(gone, set()), f"{by_path}")

        # the source changes: the record goes stale. Fix it and the code goes
        source.write_text(source.read_text() + " ")
        _, data = run_anims(project)
        check("anim-optimize-stale" in {f.get("code") for f in data["findings"] if f["path"] == fresh}, f"{data['findings']}")

        # a file with an error still exits 1, as before
        (anims / "broken.anim").write_text("{not json")
        code, _ = run_anims(project)
        check_eq(code, 1, "an error keeps its exit code")
    finally:
        shutil.rmtree(tmp, ignore_errors=True)
