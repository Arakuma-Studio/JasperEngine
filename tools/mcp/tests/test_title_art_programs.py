# (c) Eduardo Doria and contributors
# SPDX-License-Identifier: MIT

"""S80 Deviation 61: a Title Art render does not wait on the main thread for a program still being
built. JASPER_TEST_TITLE_PROGRAM_BUILD=delay:<n> (editor/App.cpp) builds every Title Art program only
once n main-loop iterations passed since it was first asked for. A render that waited on the main
thread never saw one built (no iteration ran while it waited) and failed after 60 s ("a title art
program was not built within 60 s", the red run); now the render reports not ready, the frames go
on, and the tools that need the image wait for it: view_asset, check_title's pixel checks and
list_title_library's sheet return what a quick host returns. So do the other callers that wait for
the image: render_title's animation frames, export_title's flipbooks and sheet, the export's
prebakes (a Realtime title's initial state, a Glyph Atlas title's atlas) and the Title Designer's
canvas and Animation panel. The bakes that ran on the main thread in one call (edit_title's, the
export's rebake of stale outputs) never saw one built and hung the editor; the export's glyph atlas
prebake waited 60 s per string and shipped no atlas. `fail`: a program that cannot be built is still
an error, at once.

A program built for one test is built for the next: the first test runs on the file's editor, each
other on an editor of its own."""

import asyncio
import json

from harness import Editor

DELAYED = {"JASPER_TEST_TITLE_PROGRAM_BUILD": "delay:30"}

CI = True
TIMEOUT = 1500
EDITOR = {"create": True, "name": "Title Programs", "env": DELAYED}
PATH = "assets/titles/hero.titleart"
LOOK = "casino/outline-gold"


async def new_title(ed: Editor) -> None:
    await ed.call("set_project_settings", values={"title_art.auto_bake": False})
    # no bake: view_asset then renders (it shows a fresh output as baked)
    await ed.call("edit_title", path=PATH, create={"look": LOOK, "text": "HERO"}, bake="never")


async def finished(ed: Editor, reply: dict) -> dict:
    """The job's result when the call handed back its job_id."""
    if reply.get("status") != "running":
        return reply
    for _ in range(3000):
        job = await ed.call("get_job", job_id=reply["job_id"])
        if job["status"] != "running":
            ed.check_eq(job["status"], "succeeded", str(job)[:400])
            return job["result"]
        await asyncio.sleep(0.2)
    ed.fail(f"job {reply['job_id']} still running")


async def test_view_asset_waits_for_a_program_being_built(ed: Editor):
    await new_title(ed)
    r = await ed.call_raw("view_asset", {"path": PATH})
    ed.check(not r.is_error, f"view_asset renders once the programs are built: {r.text[:300]}")
    ed.check_eq(len(r.images), 1, "one image")
    ed.check("a render" in (r.structured or {}).get("shows", ""), f"it is a render: {r.text[:200]}")


async def test_check_title_pixel_checks_wait_for_programs(ed: Editor):
    async with Editor(create=True, name="Title Programs Check", env=DELAYED) as slow:
        await new_title(slow)
        report = await finished(slow, await slow.call("check_title", path=PATH, backgrounds=["black", "white"], render_scale=0.25))
        checks = {c["id"]: c for c in report["checks"]}
        ed.check("fallback_parity" in checks, f"the pixel checks ran: {sorted(checks)}")
        for cid, check in checks.items():
            message = str(check.get("message", ""))
            ed.check("not built" not in message and "not_ready" not in message, f"{cid}: no render gave up on a program: {message[:200]}")
        ed.check(checks["fallback_parity"]["status"] != "FAIL", f"fallback_parity: {checks['fallback_parity']}")


async def test_library_sheet_waits_for_programs(ed: Editor):
    async with Editor(create=True, name="Title Programs Sheet", env=DELAYED) as slow:
        await slow.call("set_project_settings", values={"title_art.auto_bake": False})
        r = await slow.call_raw("list_title_library", {"kind": "looks", "sheet": {"ids": [LOOK]}})
        ed.check(not r.is_error, r.text[:300])
        result = await finished(slow, r.structured)
        ed.check_eq(len(result["rows"]), 1, f"one cell: {result}")
        ed.check("error" not in result["rows"][0], f"the cell rendered: {result['rows'][0]}")


async def test_a_program_that_cannot_be_built_is_an_error(ed: Editor):
    async with Editor(create=True, name="Title Programs Fail", env={"JASPER_TEST_TITLE_PROGRAM_BUILD": "fail"}) as broken:
        await new_title(broken)
        message = await broken.check_error("view_asset", {"path": PATH}, contains="could not be built")
        ed.check("not built within" not in message, f"a failure, not a wait: {message[:300]}")


ANIMATED = [
    {"op": "add_rim", "layer": "Title"},
    {"op": "add_animation", "name": "Pulse", "duration": 0.2},
    {"op": "set_animation", "animation": "Pulse", "values": {"flipbook.fps": 10, "flipbook.scale": 0.5}},
    {"op": "set_track", "animation": "Pulse", "layer": "Title", "field": "rim#0.width", "keys": [[0, 2], [0.1, 6], [0.2, 2]]},
]


async def animated_title(ed: Editor) -> None:
    await ed.call("set_project_settings", values={"title_art.auto_bake": False})
    await ed.call("edit_title", path=PATH, create={"look": LOOK, "text": "HERO"}, bake="never", ops=ANIMATED)


async def test_edit_title_bakes_while_a_program_is_built(ed: Editor):
    """edit_title's bake (a new document's under `auto`, and `now` with a render after it) steps from
    the frames; on the main thread in one call it never saw a program built (the red run hung)."""
    async with Editor(create=True, name="Title Programs Bake", env=DELAYED) as slow:
        await slow.call("set_project_settings", values={"title_art.auto_bake": False})
        made = await finished(slow, await slow.call("edit_title", path=PATH, create={"look": LOOK, "text": "HERO"}))
        ed.check_eq(made["bake"]["status"], "baked", f"a new document bakes at once: {made.get('bake')}")
        ed.check((slow.project / (PATH + ".png")).exists(), "its PNG is written")
        r = await slow.call_raw("edit_title", {"path": PATH, "bake": "now", "render": True,
                                               "ops": [{"op": "set", "layer": "Title", "values": {"text": "HEROES"}}]})
        ed.check(not r.is_error, r.text[:300])
        again = await finished(slow, r.structured)
        ed.check_eq(again["bake"]["status"], "baked", f"bake now: {again.get('bake')}")
        ed.check(again.get("metrics") or again.get("cells"), f"the render after the bake: {sorted(again)}")
        got = await slow.call("get_title", path=PATH)
        ed.check_eq(got["outputs"][0]["status"], "fresh", f"the bake is the edited document's: {got['outputs'][0]}")


async def test_animation_frames_flipbook_and_sheet_wait_for_programs(ed: Editor):
    """render_title's animation frames, export_title's flipbook bake and its sheet format meet a program
    still being built and finish once it is."""
    async with Editor(create=True, name="Title Programs Frames", env=DELAYED) as slow:
        await animated_title(slow)
        frames = await finished(slow, await slow.call("render_title", path=PATH, image="none",
                                                      options={"animation": {"name": "Pulse", "times": [0, 0.1]}}))
        ed.check_eq(len(frames.get("cells", [])), 2, f"two frames: {str(frames)[:300]}")
        ed.check(all(c.get("coverage_pct", 0) > 0 for c in frames["cells"]), f"the frames have ink: {frames['cells']}")
    async with Editor(create=True, name="Title Programs Flipbook", env=DELAYED) as slow:
        await animated_title(slow)
        baked = await finished(slow, await slow.call("export_title", path=PATH))
        pulse = next(a for a in baked["flipbooks"]["animations"] if a["animation"] == "Pulse")
        ed.check(pulse["frames"] == 3 and pulse["files"], f"the flipbook's frames and pages: {pulse}")
        ed.check((slow.project / (PATH + ".anim.json")).exists(), "the animation data is written")
    async with Editor(create=True, name="Title Programs Sheet Export", env=DELAYED) as slow:
        await animated_title(slow)
        sheet = await finished(slow, await slow.call("export_title", path=PATH, format="sheet", options={"fps": 10, "overwrite": True}))
        ed.check(sheet.get("ok") and sheet.get("frame_count") == 3, f"the sheet of the 3 frames: {str(sheet)[:300]}")
        ed.check(any(f.endswith(".png") and (slow.project / f).exists() for f in sheet.get("files", [])), f"the sheet image: {sheet.get('files')}")


async def build_scene(ed: Editor, titles: dict) -> None:
    """A UI scene in the build with an Image per title ({name: titleart values})."""
    made = await ed.call("create_scene", name="Hud", type="UI", path="scenes/Hud")
    scene = made["scene"]["id"]
    for name, values in titles.items():
        await ed.call("create_ui", scene=scene, root={"type": "image", "name": name, "layout": {"anchor": "center", "size": [320, 160]}})
        await ed.call("add_component", scene=scene, entity=name, component="titleart")
        await ed.call("set_properties", scene=scene, entity=name, values={"titleart": values})
    saved = await ed.call("save_scene", scene=scene)
    await ed.call("edit_build_scenes", action="add", scene=saved["scene"]["path"])
    await ed.call("save_project")


async def source_export(ed: Editor, name: str) -> tuple:
    """A source export for Linux (no compiler, no player): the output directory and what it said."""
    out = ed.scratch.root / name
    r = await ed.call_raw("build_project", {"mode": "source", "output_dir": str(out), "platform": "linux", "backends": ["opengl"],
                                            "allow_missing_assets": True, "overwrite": True, "timeout_seconds": 1200})
    ed.check(not r.is_error, r.text[:400])
    result = await finished(ed, r.structured) if (r.structured or {}).get("status") == "running" else (r.structured or {})
    ed.check_eq(result.get("status", "succeeded"), "succeeded", f"the export: {str(result)[:400]}")
    return out, json.dumps(result.get("warnings", []))


async def test_export_rebakes_stale_outputs_while_programs_build(ed: Editor):
    """title_art.stale_bakes_on_export `rebake` steps the bake from the frames (the red run hung the
    editor: the bake ran on the main thread in one call)."""
    async with Editor(create=True, name="Title Programs Rebake", env=DELAYED) as slow:
        await new_title(slow)
        await finished(slow, await slow.call("export_title", path=PATH))
        await slow.call("edit_title", path=PATH, bake="never", ops=[{"op": "set", "layer": "Title", "values": {"text": "STALE"}}])
        ed.check_eq((await slow.call("get_title", path=PATH))["outputs"][0]["status"], "stale", "the edit made it stale")
        await slow.call("set_project_settings", values={"title_art.stale_bakes_on_export": "rebake"})
        await build_scene(slow, {"Logo": {"title": PATH}})
        _, said = await source_export(slow, "export-rebake")
        ed.check("could not rebake" not in said, said[:400])
        ed.check_eq((await slow.call("get_title", path=PATH))["outputs"][0]["status"], "fresh", "the export rebaked it")


async def test_export_prebakes_while_programs_build(ed: Editor):
    """The export's prebakes of a Realtime title's initial state and of a Glyph Atlas title's atlas
    resume on later frames until their programs are built (the glyph atlas's red run: "did not finish
    within 60 s", and no atlas shipped)."""
    async with Editor(create=True, name="Title Programs Prebake", env=DELAYED) as slow:
        await slow.call("set_project_settings", values={"title_art.auto_bake": False, "title_art.stale_bakes_on_export": "warn"})
        score = "assets/titles/score.titleart"
        await slow.call("edit_title", path=score, create={"text": "1234", "canvas": [640, 200]}, bake="never")
        await new_title(slow)
        await build_scene(slow, {"Score": {"title": score, "renderMode": "glyph_atlas", "prebakeInitialState": True},
                                 "Logo": {"title": PATH, "renderMode": "realtime", "prebakeInitialState": True}})
        out, said = await source_export(slow, "export-prebake")
        ed.check("stops at" not in said and "did not render" not in said, f"no prebake gave up: {said[:600]}")
        manifests = sorted(out.rglob("score.titleart.glyphs-*.json"))
        ed.check_eq(len(manifests), 1, f"the glyph atlas ships: {sorted(p.name for p in out.rglob('*.titleart*'))}")
        cells = json.loads(manifests[0].read_text()).get("cells", [])
        ed.check(len(cells) >= 10, f"the digits at each place: {len(cells)} cells")
        states = list(out.rglob("hero.titleart.prebake-*.png"))
        ed.check(states, f"the Realtime title's initial state ships: {sorted(p.name for p in out.rglob('hero.titleart*'))}")


async def test_designer_canvas_and_animation_wait_for_programs(ed: Editor):
    """The Title Designer's canvas and its Animation panel's pose, asked for before any program is
    built, show once the programs are built (the panel says no error), and another time of the
    animation then shows another pose."""
    async with Editor(create=True, name="Title Programs Designer", env=DELAYED) as slow:
        await slow.call("tool_groups", enable=["title_art", "editor_tools", "editor_ux"])
        await animated_title(slow)
        await slow.call("focus_window", window="title_designer", title_designer={"path": PATH, "zoom": "fit", "background": "black"})
        # the pose as soon as the panel has drawn (its first frames), while the programs are still being built
        for _ in range(120):
            r = await slow.call_raw("focus_window", {"window": "title_designer", "title_designer": {"animation": {"name": "Pulse", "time": 0.1}}})
            if not r.is_error:
                break
            ed.check("has not drawn yet" in r.text, r.text[:300])
            await slow.call("wait", frames=1)
        ed.check(not r.is_error, f"the panel previews the animation: {r.text[:300]}")

        async def settled(unlike: bytes = b"") -> tuple:
            """The window's state and image once the canvas is drawn and the image stays the same."""
            shot, state = b"", {}
            for _ in range(240):
                state = (await slow.call("focus_window", window="title_designer", title_designer={}))["state"]
                canvas = state.get("canvas", {})
                again = await slow.screenshot("title_designer", max_width=600)
                if not state.get("busy") and canvas.get("title_drawn") and again == shot and again != unlike:
                    return state, shot
                shot = again
                await asyncio.sleep(0.5)
            ed.fail(f"the canvas never settled on a new image: {state}")
            return state, shot

        state, first = await settled()
        panel = state.get("animation_panel", {})
        ed.check_eq(panel.get("animation"), "Pulse", f"the panel previews Pulse: {panel}")
        ed.check(not panel.get("status"), f"the pose waited for its programs, it did not fail: {panel.get('status')}")
        ed.check(not state["canvas"].get("error"), f"the canvas rendered: {state['canvas']}")
        await slow.call("focus_window", window="title_designer", title_designer={"animation": {"name": "Pulse", "time": 0.0}})
        state, second = await settled(unlike=first)
        ed.check(not state.get("animation_panel", {}).get("status"), f"no error at 0: {state.get('animation_panel')}")
