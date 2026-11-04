"""The project's shader list with closed scenes (docs/specs/S64-shader-lists.md, docs/mcp/guide/
shipping.md "The shader list"): get_build_info, generate_shaders, Play's startup and an export name
the same shaders whatever each closed scene's file saved (AC1, AC2, R5), an unchanged project answers
from the cache by file hash, also after a restart (AC3), work past the budget is a job (R3), and a
scene that cannot be read is named (R4)."""

from __future__ import annotations

import json
import os
import re
import shutil
import tempfile
from pathlib import Path

from harness import Editor, over_budget
from shader_lists import build_info, check_lists_match, exported, finish_job, generated, listed

CI = True
TIMEOUT = 2400

_stale: Path | None = None
KEYS = re.compile(r"^shaderKeys:\n(?:[ ]+.*\n|-.*\n)*", re.M)


async def entity(ed: Editor, sid, kind: str, name: str) -> None:
    await ed.call("create_entity", scene=sid, type=kind, name=name)


async def closed_scene(ed: Editor, name: str, kind: str, *content: tuple[str, str]) -> str:
    made = await ed.call("create_scene", name=name, type=kind)
    sid = made["scene"]["id"]
    for what, label in content:
        await entity(ed, sid, what, label)
    path = f"scenes/{name}.scene"
    await ed.call("save_scene", scene=sid, path=f"scenes/{name}")
    await ed.call("edit_build_scenes", action="add", scene=sid)
    return path


async def stale_project() -> Path:
    """A project with one open and several closed scenes, each with something only it needs, whose
    files saved no shader keys (written by an editor before the content changed): once, for all the
    tests, each of which works on a copy of it."""
    global _stale
    if _stale:
        return _stale
    root = Path(tempfile.mkdtemp(prefix="jasper-shader-lists-"))
    async with Editor(create=True, name="Lists") as ed:
        main = (await ed.call("create_scene", name="Main", type="3D"))["scene"]["id"]
        await entity(ed, main, "cube", "Box")
        await ed.call("save_scene", scene=main, path="scenes/Main")
        await ed.call("edit_build_scenes", action="add", scene=main)

        # a variant's content is its base's: a material only there, in a scene with no lights of its
        # own; the lit form of it is reached by the other scenes (a script can spawn any prefab)
        spawned = (await ed.call("create_scene", name="Spawned", type="3D"))["scene"]["id"]
        await entity(ed, spawned, "sphere", "Ball")
        await ed.call("set_properties", scene=spawned, entity="Ball",
                      values={"mesh": {"submeshes[0].material": {"pbr": {"sheen": {"enabled": True}}}}})
        await ed.call("create_prefab", scene=spawned, entity="Ball", path="prefabs/Ball.prefab")
        await ed.call("create_prefab_variant", base="prefabs/Ball.prefab", path="prefabs/BallVariant.prefab")
        await ed.call("delete_entities", scene=spawned, entities=["Ball"])
        await ed.call("instantiate_prefab", scene=spawned, prefab="prefabs/BallVariant.prefab", name="Variant")
        await ed.call("save_scene", scene=spawned, path="scenes/Spawned")
        await ed.call("edit_build_scenes", action="add", scene=spawned)

        await closed_scene(ed, "Lit", "3D", ("directional_light", "Sun"), ("sphere", "Ball"))
        await closed_scene(ed, "Foggy", "3D", ("fog", "Fog"), ("cube", "Box"))
        await closed_scene(ed, "Flat", "2D", ("sprite", "Sprite"))
        await ed.call("save_project")
        for scene in ("scenes/Spawned.scene", "scenes/Lit.scene", "scenes/Foggy.scene", "scenes/Flat.scene"):
            await ed.call("close_scene", scene=scene)
        await ed.call("save_project")
        shutil.copytree(ed.project, root / "project", symlinks=True)
    shutil.rmtree(root / "project" / ".jasperengine" / "generated", ignore_errors=True)
    for name in ("Spawned", "Lit", "Foggy", "Flat"):
        file = root / "project" / "scenes" / f"{name}.scene"
        file.write_text(KEYS.sub("", file.read_text()))
    (root / "project" / ".jasperengine" / "shader-lists.yaml").unlink(missing_ok=True)
    _stale = root / "project"
    return _stale


async def test_stale_closed_scenes_match_an_export():
    """AC1, AC2: the closed scenes' files saved no keys; the lists name what the export ships."""
    async with Editor(project=await stale_project(), label="stale") as ed:
        await check_lists_match(ed)

        # what the export re-saved answers without reading a scene again (cached by the save)
        info = await ed.call("get_build_info", include_shaders=True)
        ed.check_eq(info["shaders_complete"], True)
        ed.check_eq(info["shader_list_scenes_read"], 0, "the export's saves are the cache")
        names = listed(info)
        from_export, _ = await exported(ed, ed.scratch.root / "export-again")
        ed.check_eq(sorted(from_export ^ names), [], "a second export ships the same list")


async def test_overrides_and_warmup_are_in_every_list():
    """R2: what a build adds (the warm-up list) and removes (an exclusion) is in the one list."""
    async with Editor(project=await stale_project(), label="overrides") as ed:
        info = await build_info(ed)
        names = listed(info)

        # a variant no scene needs, as Play recorded it
        known = {s["key"] for s in info["shaders"]}
        extra = next(key for key in (hex((4 << 32) | bits) for bits in range(1, 64)) if key not in known)
        (ed.project / "ShaderWarmup.json").write_text(json.dumps({
            "version": 1, "family": "gl", "shaders": [{"key": extra, "custom": ""}], "pipelines": []}))
        info = await build_info(ed)
        added = listed(info) - names
        ed.check_eq(len(added), 1, f"the warm-up list's variant is listed: {sorted(added)}")

        excluded = next(s for s in info["shaders"] if s["shader"].startswith("mesh_"))
        out = ed.scratch.root / "excluded"
        r = await ed.call("build_project", mode="source", output_dir=str(out), backends=["opengl"], overwrite=True,
                          shaders={"exclude": [excluded["key"]]}, timeout_seconds=1800)
        if r["status"] == "running":
            r = await finish_job(ed, r["job_id"])
        ed.check_eq(r["status"], "succeeded", f"{r}")
        info = await build_info(ed)
        ed.check_eq(sorted(listed(info) ^ ((names | added) - {excluded["shader"]})), [], "the exclusion left the list")
        ed.check_eq([s["shader"] for s in info["shaders"] if s.get("excluded")], [excluded["shader"]], "it stays listed, flagged")

        from_tool, _ = await generated(ed, ed.scratch.root / "generated-excluded")
        ed.check_eq(sorted(from_tool ^ listed(info)), [], "generate_shaders follows both")
        from_export, _ = await exported(ed, ed.scratch.root / "export-excluded")
        ed.check_eq(sorted(from_export ^ listed(info)), [], "the export follows both")


async def test_unchanged_project_answers_from_the_cache():
    """AC3: an unchanged project reads no scene, in this session and the next, and answers within the
    budget (enforced by a benchmark run: JASPER_MCP_PERF_SLACK); a changed file is read again."""
    project = await stale_project()
    async with Editor(project=project, label="cache") as ed:
        first = await build_info(ed)
        names = listed(first)
        cached = await ed.call("get_build_info", include_shaders=True)
        ed.check_eq(cached["shaders_complete"], True)
        ed.check_eq(cached["shader_list_scenes_read"], 0, "unchanged files are not read again")
        ed.check_eq(listed(cached), names)
        times = []
        for _ in range(10):
            info = await ed.call("get_build_info")
            ed.check_eq(info["shader_list_scenes_read"], 0)
            times.append(info["shader_list_ms"])
        over = over_budget("shader list of an unchanged project", {"best of 10": min(times), "worst of 10": max(times)}, 50, "ms")
        ed.check(not over, f"over the budget: {over}")

        # the next session answers from the file the cache keeps
        kept = Path(tempfile.mkdtemp(prefix="jasper-shader-lists-kept-")) / "project"
        shutil.copytree(ed.project, kept, symlinks=True)

        # a closed scene's file changes: its new content is in the list, nothing else is read
        scene = ed.project / "scenes" / "Flat.scene"
        before = scene.read_text()
        await ed.call("create_entity", scene="scenes/Main.scene", type="cube", name="Unsaved")
        scene.write_text(before + "\n# touched\n")
        info = await ed.call("get_build_info", include_shaders=True)
        ed.check_eq(info["shader_list_scenes_read"], 1, "only the changed scene is read")
        ed.check_eq(listed(info), names)

        # a touch that keeps the bytes keeps the answer: the cache is by the file's hash
        file_time = scene.stat().st_mtime
        os.utime(scene, (file_time + 60, file_time + 60))
        info = await ed.call("get_build_info")
        ed.check_eq(info["shader_list_scenes_read"], 0, "same bytes, same keys")

        # a prefab every scene can spawn changes: every closed scene is read again
        prefab = ed.project / "prefabs" / "Ball.prefab"
        prefab.write_text(prefab.read_text() + "\n# touched\n")
        info = await ed.call("get_build_info")
        ed.check(info["shader_list_scenes_read"] > 0 or not info["shaders_complete"], f"read again: {info}")
        await build_info(ed)
        info = await ed.call("get_build_info")
        ed.check_eq(info["shader_list_scenes_read"], 0, "and then it is cached again")

    async with Editor(project=kept, label="restart") as ed:
        info = await ed.call("get_build_info", include_shaders=True)
        ed.check_eq(info["shaders_complete"], True)
        ed.check_eq(info["shader_list_scenes_read"], 0, "the cache outlives the session")
        ed.check_eq(listed(info), names)


async def test_past_the_budget_is_a_job():
    """R3: with no time for reading scenes, get_build_info says so and how to finish; the job does it,
    the editor answering meanwhile; generate_shaders waits for it on its own."""
    project = await stale_project()
    async with Editor(project=project, label="job-reference") as ed:
        names = listed(await build_info(ed))

    async with Editor(project=project, label="job", env={"JASPER_SHADER_LIST_SYNC_MS": "0"}) as ed:
        info = await ed.call("get_build_info", include_shaders=True)
        ed.check_eq(info["shaders_complete"], False, "no time for the closed scenes")
        ed.check(sorted(info["shaders_pending_scenes"]), f"names them: {info}")
        ed.check(info["shaders_job_id"] in info["next"] and "get_job" in info["next"], f"how to finish: {info['next']}")
        again = await ed.call("get_build_info")
        ed.check_eq(again["shaders_job_id"], info["shaders_job_id"], "one job at a time")
        job = await finish_job(ed, info["shaders_job_id"])
        ed.check_eq(job["status"], "succeeded", f"{job}")
        info = await ed.call("get_build_info", include_shaders=True)
        ed.check_eq(info["shaders_complete"], True)
        ed.check_eq(listed(info), names)

    async with Editor(project=project, label="job-generate", env={"JASPER_SHADER_LIST_SYNC_MS": "0"}) as ed:
        built, _ = await generated(ed, ed.scratch.root / "generated")
        ed.check_eq(sorted(built ^ names), [], "generate_shaders reads what is stale before it builds")


async def test_unreadable_scene_is_named():
    """R4: a closed scene that cannot be read is in the result, in each consumer."""
    source = await stale_project()
    bad = Path(tempfile.mkdtemp(prefix="jasper-shader-lists-bad-")) / "project"
    shutil.copytree(source, bad, symlinks=True)
    async with Editor(project=bad, label="unreadable") as ed:
        await build_info(ed)
        scene = ed.project / "scenes" / "Foggy.scene"
        scene.write_text("id: [not yaml\n")
        info = await build_info(ed)
        named = {s["path"]: s for s in info["shaders_unreadable_scenes"]}
        ed.check_eq(sorted(named), ["scenes/Foggy.scene"], f"{info['shaders_unreadable_scenes']}")
        ed.check(named["scenes/Foggy.scene"]["error"], "with why")
        ed.check("Foggy" in (await ed.call_raw("get_build_info")).text, "the summary names it")

        _, result = await generated(ed, ed.scratch.root / "generated")
        ed.check_eq([s["path"] for s in result["result"]["unreadable_scenes"]], ["scenes/Foggy.scene"], f"{result}")

        # a file with no scene in it is no more readable, and a deleted one neither
        scene.write_text("id: 5\nname: Empty\n")
        (ed.project / "scenes" / "Flat.scene").unlink()
        info = await build_info(ed)
        ed.check_eq(sorted(s["path"] for s in info["shaders_unreadable_scenes"]), ["scenes/Flat.scene", "scenes/Foggy.scene"])

    # one the project knew by name when it opened
    (bad / "scenes" / "Lit.scene").unlink()
    async with Editor(project=bad, label="unresolved") as ed:
        info = await build_info(ed)
        paths = sorted(s["path"] for s in info["shaders_unreadable_scenes"])
        ed.check_in("scenes/Lit.scene", paths, f"{info['shaders_unreadable_scenes']}")


async def test_play_startup_builds_what_closed_scenes_need():
    """R5: Play's startup builds the whole list, the variants of scenes loaded later included."""
    async with Editor(project=await stale_project(), label="play") as ed:
        await ed.call("play", action="start", scene="scenes/Main.scene")
        try:
            await ed.call("wait", frames=2, timeout_seconds=900)
        finally:
            await ed.call("play", action="stop")
        # against what an export really ships: the list alone would only agree with itself
        built = {p.name[:-len("_glsl410.sdat")] for p in ed.scratch.root.rglob("*_glsl410.sdat")}
        from_export, _ = await exported(ed, ed.scratch.root / "export")
        ed.check_eq(sorted(from_export - built), [], f"Play's startup did not build them ({len(built)} built)")
