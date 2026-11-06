"""The project's shader list with closed scenes (docs/specs/S64-shader-lists.md): get_build_info, generate_shaders, Play's startup and an export name
the same shaders whatever each closed scene's file saved (AC1, AC2, R5), an unchanged project answers
from the cache by file hash, also after a restart (AC3), work past the budget is a job (R3), and a
scene that cannot be read is named (R4)."""

from __future__ import annotations

import asyncio
import json
import os
import re
import shutil
import tempfile
import time
from pathlib import Path

from harness import Editor, over_budget
from shader_lists import build_info, check_lists_match, exported, finish_job, generated, listed

CI = True
TIMEOUT = 2400

_stale: Path | None = None
CACHE = ".jasperengine/shader-lists.yaml"
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

        # what a Prefab Variant overrides of a GameObject of its base is in no file's GameObjects: an
        # instance of it is the only way to see that material. Only this scene has one, so its form
        # under fog is reached from Foggy (a script can spawn any prefab), which does not load the
        # variant when it is read
        spawned = (await ed.call("create_scene", name="Spawned", type="3D"))["scene"]["id"]
        for light in (await ed.call("find_entities", scene=spawned, component="light"))["entities"]:
            await ed.call("delete_entities", scene=spawned, entities=[light["id"]])
        await entity(ed, spawned, "empty", "Ball")
        await ed.call("create_entity", scene=spawned, type="sphere", name="Inner", parent="Ball")
        await ed.call("create_prefab", scene=spawned, entity="Ball", path="prefabs/Ball.prefab")
        await ed.call("set_properties", scene=spawned, entity="Ball/Inner",
                      values={"mesh": {"submeshes[0].material": {"pbr": {"sheen": {"enabled": True}}}}})
        await ed.call("create_prefab_variant", scene=spawned, entity="Ball", path="prefabs/BallVariant.prefab")
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


async def cache_written(ed: Editor, deadline: float = 60.0) -> None:
    """The write is off the main thread: until the file holds the closed scenes; the deadline only guards a hang."""
    file = ed.project / CACHE
    end = time.monotonic() + deadline
    while not (file.exists() and "scenes/Flat.scene" in file.read_text()):
        ed.check(time.monotonic() < end, f"{file} was not written")
        await asyncio.sleep(0.05)


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
        await cache_written(ed)
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


async def test_the_reply_does_not_wait_for_the_cache_write():
    """R3: writing the cache ends in an fsync, which a slow disk makes hundreds of milliseconds: the
    reply comes first. The hold keeps the write pending, so this needs no timing."""
    hold = Path(tempfile.mkdtemp(prefix="jasper-shader-lists-hold-")) / "hold"
    hold.write_text("")
    async with Editor(project=await stale_project(), label="hold",
                      env={"JASPER_SHADER_LIST_WRITE_HOLD_FILE": str(hold)}) as ed:
        info = await build_info(ed)
        ed.check_eq(info["shaders_complete"], True)
        ed.check(not (ed.project / CACHE).exists(), "the reply waited for the cache file")
        again = await ed.call("get_build_info")
        ed.check_eq(again["shaders_complete"], True, "and the next call answers while it is written")
        hold.unlink()
        await cache_written(ed)


async def test_past_the_budget_is_a_job():
    """R3: with no time for reading scenes, get_build_info says so and how to finish; the job does it,
    the editor answering meanwhile; generate_shaders waits for it on its own."""
    project = await stale_project()
    async with Editor(project=project, label="job-reference") as ed:
        names = listed(await build_info(ed))

    async with Editor(project=project, label="job", env={"JASPER_SHADER_LIST_SYNC_MS": "0"}) as ed:
        replies = (await ed.batch([
            ("get_build_info", {"include_shaders": True}),
            ("get_build_info", {}),
            ("get_build_info", {}),
        ]))["results"]
        ed.check(all(reply["ok"] for reply in replies), f"{replies}")
        info = replies[0]["structured"]
        ed.check_eq(info["shaders_complete"], False, "no time for the closed scenes")
        ed.check(sorted(info["shaders_pending_scenes"]), f"names them: {info}")
        said = replies[1]["text"]
        ed.check(f"get_job {info['shaders_job_id']}" in said, f"how to finish: {said}")
        again = replies[2]["structured"]
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


async def test_reading_closed_scenes_makes_no_gpu_resources():
    """R2: no GPU work. The closed scenes read again, the device holds what it held (the proof that
    no device is needed at all: ProjectShaderList.ReadsClosedScenesWithoutADevice)."""
    async with Editor(project=await stale_project(), label="gpu") as ed:
        await build_info(ed)

        async def alive() -> dict:
            await ed.call("wait", frames=5, timeout_seconds=300)
            return (await ed.call("get_render_stats"))["alive"]

        before = await alive()
        for name in ("Spawned", "Lit", "Foggy", "Flat"):
            scene = ed.project / "scenes" / f"{name}.scene"
            scene.write_text(scene.read_text() + "\n# touched\n")
        read = 0
        while True:
            info = await ed.call("get_build_info")
            read += info["shader_list_scenes_read"]
            if info["shaders_complete"]:
                break
            await finish_job(ed, info["shaders_job_id"])
            read += 1
        ed.check(read >= 1, "the closed scenes were read again")
        ed.check_eq(await alive(), before, "reading them made or dropped GPU resources")


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
