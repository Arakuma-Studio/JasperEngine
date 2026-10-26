"""Fluid Simulation editor fixes, end to end through MCP:

- a texture linked to a fluid ("fluid|<entity>") is unbound from the fluid's output when the fluid
  goes: deleting its GameObject (undo binds it again), reverting an added GameObject or an added
  Fluid Simulation component of a prefab instance (no texture left on a freed framebuffer);
- a preset on a prefab instance records overrides only for the fields it changes;
- fluid_stats.running is false for a disabled fluid and for one on an inactive GameObject;
- Play starts every fluid afresh (not from the edit-mode state an eval_lua step() left), two
  Plays of a seeded fluid give identical images, Stop brings the fluid back to its edit-mode start
  (not the game's last frame), and seed 0 takes a new seed at each Play.

Whether a texture is bound is read in edit mode through eval_lua (Texture:isFramebuffer()).

Runs in a new empty project (EDITOR create).
"""

from __future__ import annotations

import asyncio
import sys
from pathlib import Path

from harness import Editor

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from htmlui_pixels import decode_png  # noqa: E402,F401  (the screenshots are compared as PNG bytes)

EDITOR = {"create": True, "name": "Fluid Fixed"}
TIMEOUT = 1200

KEY = "fluidsimulation"
S: dict = {}

# a small, seeded, paused SCREEN fluid: only scripted steps move it
RUN = {"output": "screen", "interactive": False, "paused": True, "seed": 4242, "splatsOnStart": True,
       "randomSplatCount": 10, "dyeResolution": 256, "simResolution": 64}
SMALL = {"output": "texture", "dyeResolution": 128, "simResolution": 32, "paused": True}
INK_CHANGES = {"densityDissipation", "velocityDissipation", "pressureIterations", "curl", "splatRadius", "splatForce",
               "colorful", "bloom", "sunrays"}


async def lua(ed: Editor, code: str, **args):
    r = await ed.call("eval_lua", code=code, **args)
    return r.get("values")


async def bound(ed: Editor, name: str) -> bool:
    """the base colour texture of this Sprite shows a framebuffer (edit mode)"""
    got = await lua(ed, f'return object("{name}", "Mesh").material.baseColorTexture:isFramebuffer()',
                    allow_edit_mode=True, scene=S["scene"])
    return bool(got and got[0])


async def link(ed: Editor, display: str, fluid: str):
    await ed.call("set_properties", scene=S["scene"], entity=display,
                  values={"mesh": {"submeshes[0].material.baseColorTexture": {"fluid": fluid}}})


async def texture_of(ed: Editor, entity):
    got = await ed.call("get_entity", entity=entity, scene=S["scene"], components=["mesh"],
                        properties=["submeshes[0].material.baseColorTexture"])
    return got["components"]["mesh"]["submeshes[0].material.baseColorTexture"]


async def make(ed: Editor, name: str, kind: str = "empty", parent=None, fluid: dict | None = None) -> int:
    args = {"scene": S["scene"], "name": name, "type": kind}
    if parent is not None:
        args["parent"] = parent
    if fluid is not None:
        args["components"] = ["fluid"]
        args["values"] = {"fluid": fluid}
    return (await ed.call("create_entity", args))["entity_id"]


async def stats(ed: Editor, entity) -> dict:
    got = await ed.call("get_entity", scene=S["scene"], entity=entity, components=[KEY])
    return got.get("fluid_stats") or {}


async def drawn_stats(ed: Editor, entity, until, what: str, tries: int = 40) -> dict:
    """the edit-mode fluid's stats once `until(stats)` holds (screenshots make the editor draw)"""
    st = {}
    for _ in range(tries):
        await ed.call_raw("screenshot", {"view": "game", "max_width": 256})
        st = await stats(ed, entity)
        if until(st):
            return st
        await asyncio.sleep(0.25)
    ed.fail(f"{what}: {st}")


def handle(name: str = "Screen") -> str:
    return f'object("{name}", "FluidSimulation")'


async def until_lua(ed: Editor, condition: str, what: str, timeout: int = 120):
    r = await ed.call_raw("wait", {"until": condition, "timeout_seconds": timeout})
    ed.check(not r.is_error, f"{what}: {r.text[:800]}")


async def game_png(ed: Editor, name: str) -> bytes:
    r = await ed.call_raw("screenshot", {"view": "game", "max_width": 1024})
    ed.check(not r.is_error and r.images, f"game screenshot: {r.text[:300]}")
    png = r.images[0][0]
    (ed.scratch.root / f"{name}.png").write_bytes(png)
    return png


async def play(ed: Editor):
    await ed.call("play", action="start", scene=S["scene"], fixed_fps=60, auto_pause=True, timeout_seconds=300)


async def stop(ed: Editor):
    await ed.call("play", action="stop", fixed_fps=0, auto_pause=False)


# ---------------------------------------------------------------------------- texture links


async def test_setup(ed: Editor):
    made = await ed.call("create_scene", name="FluidFixed", type="2D")
    S["scene"] = made["scene"]["id"]
    await ed.call("select_scene", scene=S["scene"])
    S["source"] = await make(ed, "Source", fluid=SMALL)
    S["display"] = await make(ed, "Display", "sprite")
    await link(ed, "Display", "Source")
    ed.check(await bound(ed, "Display"), "the linked sprite shows the fluid's output")


async def test_delete_unbinds_links(ed: Editor):
    await ed.call("delete_entities", scene=S["scene"], entity=S["source"])
    tex = await texture_of(ed, S["display"])
    ed.check(isinstance(tex, dict) and "fluid" in tex, f"the link stays (saved as one): {tex}")
    ed.check(not await bound(ed, "Display"), "deleting the fluid unbinds the texture linked to its output")

    await ed.call("undo", scene=S["scene"])
    ed.check(await bound(ed, "Display"), "undo of the delete binds the link to the fluid again")
    ed.check_eq((await texture_of(ed, S["display"]))["fluid"]["id"], S["source"], "the same fluid")

    # cut (copy + delete) and paste back: the pasted fluid gets its id back, the link rebinds
    await ed.call("copy_entities", scene=S["scene"], entity=S["source"], cut=True)
    ed.check(not await bound(ed, "Display"), "a cut fluid unbinds the link")
    await ed.call("paste_entities", scene=S["scene"])
    found = await ed.call("find_entities", scene=S["scene"], name="Source")
    if found["count"] and found["entities"][0]["id"] == S["source"]:
        ed.check(await bound(ed, "Display"), "pasting the cut fluid back binds the link again")
    await ed.call("undo", scene=S["scene"])
    await ed.call("undo", scene=S["scene"])
    ed.check(await bound(ed, "Display"), "undo of the cut binds the link again")


async def test_prefab_reverts_unbind_links(ed: Editor):
    holder = await make(ed, "Holder")
    await ed.call("create_prefab", scene=S["scene"], entity=holder, path="prefabs/Holder.prefab")

    # an added GameObject with a fluid, reverted
    child = await make(ed, "Child", parent=holder, fluid=SMALL)
    await make(ed, "ChildDisplay", "sprite")
    await link(ed, "ChildDisplay", "Child")
    ed.check(await bound(ed, "ChildDisplay"), "linked to the added fluid")
    await ed.call("prefab_overrides", action="revert", scene=S["scene"], entity=child, kind="added_gameobject")
    found = await ed.call("find_entities", scene=S["scene"], name="Child")
    ed.check_eq(found["count"], 0, "the added GameObject is reverted")
    ed.check(not await bound(ed, "ChildDisplay"), "reverting the added fluid GameObject unbinds the link")
    await ed.call("undo", scene=S["scene"])
    ed.check(await bound(ed, "ChildDisplay"), "undo of the revert binds the link again")

    # an added Fluid Simulation component, reverted
    await ed.call("add_component", scene=S["scene"], entity=holder, component="fluid", values={"fluid": SMALL})
    await make(ed, "HolderDisplay", "sprite")
    await link(ed, "HolderDisplay", "Holder")
    ed.check(await bound(ed, "HolderDisplay"), "linked to the added component")
    await ed.call("prefab_overrides", action="revert", scene=S["scene"], entity=holder, kind="added_component",
                  component="fluid")
    got = await ed.call("get_entity", scene=S["scene"], entity=holder)
    ed.check(KEY not in got["components"], f"the added component is reverted: {sorted(got['components'])}")
    ed.check(not await bound(ed, "HolderDisplay"), "reverting the added fluid component unbinds the link")


# ---------------------------------------------------------------------------- presets on prefab instances


def overridden(entries: list) -> set:
    out = set()
    for e in entries:
        if e["kind"] == "property" and KEY in str(e.get("component", "")).lower().replace(" ", ""):
            out |= set(e.get("properties") or [])
    return out


async def test_preset_overrides_only_changes(ed: Editor):
    pf = await make(ed, "PrefabFluid", fluid={"output": "texture"})
    await ed.call("create_prefab", scene=S["scene"], entity=pf, path="prefabs/PrefabFluid.prefab")

    async def overrides() -> list:
        return (await ed.call("get_prefab_info", scene=S["scene"], entity=pf, usage=False))["overrides"]

    ed.check_eq(await overrides(), [], "a new instance has no overrides")
    await ed.call("set_properties", scene=S["scene"], entity=pf, values={"fluid": {"preset": "default"}})
    ed.check_eq(await overrides(), [], "the preset it already matches overrides nothing")
    await ed.call("set_properties", scene=S["scene"], entity=pf, values={"fluid": {"preset": "ink"}})
    entries = await overrides()
    ed.check_eq(overridden(entries), INK_CHANGES, f"only the fields ink changes are overrides: {entries}")
    await ed.call("undo", scene=S["scene"])
    ed.check_eq(await overrides(), [], "undo removes them in one step")


# ---------------------------------------------------------------------------- fluid_stats.running


async def test_running_stat(ed: Editor):
    S["screen"] = await make(ed, "Screen", fluid=RUN)
    await drawn_stats(ed, S["screen"], lambda s: s.get("running") is True, "the edit-mode fluid is drawn")

    await ed.call("set_properties", scene=S["scene"], entity=S["screen"], values={"fluid": {"enabled": False}})
    st = await drawn_stats(ed, S["screen"], lambda s: s.get("running") is not None, "stats of the disabled fluid", 3)
    ed.check(st["running"] is False, f"a disabled fluid is not running: {st}")
    await ed.call("undo", scene=S["scene"])
    await drawn_stats(ed, S["screen"], lambda s: s.get("running") is True, "enabled again")

    await ed.call("set_entity_state", scene=S["scene"], entity=S["screen"], active=False)
    st = await drawn_stats(ed, S["screen"], lambda s: s.get("running") is not None, "stats of the inactive fluid", 3)
    ed.check(st["running"] is False, f"a fluid on an inactive GameObject is not running: {st}")
    await ed.call("undo", scene=S["scene"])
    await drawn_stats(ed, S["screen"], lambda s: s.get("running") is True, "active again")


# ---------------------------------------------------------------------------- Play and Stop


async def scripted_play(ed: Editor, name: str, steps: int) -> tuple[bytes, dict]:
    await play(ed)
    try:
        # Play restarted the fluid: no step yet (it is paused), the start burst splatted
        await until_lua(ed, f"{handle()}:getStats().splats >= {RUN['randomSplatCount']}", "the start burst")
        st = (await ed.call("get_runtime_entity", entity="Screen", components=["fluid"],
                            include_script_state=False))["fluid_stats"]
        ed.check_eq(st["steps"], 0, f"Play starts the fluid afresh, not from the edit-mode steps: {st}")
        await lua(ed, f"{handle()}:step({steps}); return true")
        await until_lua(ed, f"{handle()}:getStats().steps >= {steps}", "the scripted steps")
        png = await game_png(ed, name)
        st = (await ed.call("get_runtime_entity", entity="Screen", components=["fluid"],
                            include_script_state=False))["fluid_stats"]
        ed.check_eq(st["steps"], steps, f"exactly the scripted steps: {st}")
        return png, st
    finally:
        await stop(ed)


async def test_play_restarts_and_stop_restores(ed: Editor):
    await ed.call("save_scene", scene=S["scene"], path="scenes/FluidFixed")
    # the edit-mode fluid stepped on (as eval_lua or the Inspector's preview leave it)
    await lua(ed, f"{handle()}:step(30); return true", allow_edit_mode=True, scene=S["scene"])
    await drawn_stats(ed, S["screen"], lambda s: s.get("steps", 0) >= 30, "the edit-mode steps")

    first, st1 = await scripted_play(ed, "fluid-play-1", 20)
    ed.check_eq(st1["seed"], RUN["seed"], "the seed in use")
    # Stop: back to the edit-mode start, not the game's last frame
    await drawn_stats(ed, S["screen"], lambda s: s.get("steps") == 0 and s.get("splats", 0) >= RUN["randomSplatCount"],
                      "Stop restarts the fluid to its edit-mode start")

    second, _ = await scripted_play(ed, "fluid-play-2", 20)
    ed.check(first == second, "two Plays of a seeded fluid give identical images")


async def test_seed_zero_new_each_play(ed: Editor):
    await ed.call("set_properties", scene=S["scene"], entity=S["screen"], values={"fluid": {"seed": 0}})
    seeds = []
    for _ in range(2):
        await play(ed)
        try:
            await until_lua(ed, f"{handle()}:getStats().splats >= {RUN['randomSplatCount']}", "the start burst")
            st = (await ed.call("get_runtime_entity", entity="Screen", components=["fluid"],
                                include_script_state=False))["fluid_stats"]
            seeds.append(st["seed"])
        finally:
            await stop(ed)
    ed.check(all(seeds) and seeds[0] != seeds[1], f"seed 0: a new seed at each Play: {seeds}")
    await ed.call("undo", scene=S["scene"])
    errors = [e.get("message", "") for e in await ed.console_since(None, types=["error"], limit=200)]
    ed.check(not [e for e in errors if "luid" in e], f"no fluid errors: {errors}")
