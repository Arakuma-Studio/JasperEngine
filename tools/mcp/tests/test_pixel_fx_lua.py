"""The PixelFX player in Play over MCP (docs/specs/S48-pixel-fx.md R24-R26, AC20): PixelFX.play from eval_lua runs each
effect over its duration and leaves its value where the spec says (the neutral 0; fade_out and dissolve end gone at 1);
Coroutine.waitFor(handle) resumes once, when the effect ends; a material without the feature gets it at runtime on the
GameObject's own material only (a linked .material file is unchanged on disk) and the development build logs it once
per material; PixelFX.set writes a channel; a hit-stop holds a game-time effect and not an unscaled one; bad arguments
raise errors that name the fix.

A new project: a 2D scene with three 16 x 16 Sprites drawn with library:sprite: Hero (Hit flash on), Plain (no feature)
and Linked (its material linked to materials/linked.material)."""

from __future__ import annotations

import pngdiff
from harness import Editor

CI = True
TIMEOUT = 900
EDITOR = {"create": True, "name": "PixelFxLua"}

M = "submeshes[0].material."
TEXTURE = "assets/sprites/blob.png"
MATERIAL = "assets/materials/linked.material"
DT = 1.0 / 60.0
S: dict = {}

LOG_LINE = 'PixelFX: "fade_out" turned on DITHER_FADE on'


def go(name: str) -> str:
    return f"GameObject(Engine.scene, Engine.scene:findEntity('{name}'))"


def param(name: str, member: str) -> str:
    return f"Mesh(Engine.scene, Engine.scene:findEntity('{name}')):getMaterial():getShaderParam('{member}').x"


async def lua(ed: Editor, code: str) -> list:
    return (await ed.call("eval_lua", code=code))["values"]


async def test_setup(ed: Editor):
    await ed.call("get_project_summary")
    (ed.project / "assets").mkdir(exist_ok=True)
    await ed.call("set_project_settings", values={"editor.assets_dir": "assets"})
    texture = ed.project / TEXTURE
    texture.parent.mkdir(parents=True, exist_ok=True)
    texture.write_bytes(pngdiff.encode(16, 16, bytes([200, 80, 40, 255]) * 256))
    scene = (await ed.call("create_scene", name="PixelFx", type="2D"))["scene"]["id"]
    S["scene"] = scene
    value = {"path": TEXTURE, "min_filter": "nearest", "mag_filter": "nearest"}
    for i, name in enumerate(("Hero", "Plain", "Linked")):
        made = await ed.call("create_entity", scene=scene, type="sprite", name=name, position=[-60 + 60 * i, 0, 0],
                             values={"mesh": {M + "baseColorTexture": value}, "sprite": {"pivotPreset": "center", "width": 16, "height": 16}})
        S[name] = made["entity_id"]
        await ed.call("set_properties", scene=scene, entity=S[name], values={"mesh": {M + "shader": "library:sprite"}})
    await ed.call("set_properties", scene=scene, entity=S["Hero"], values={"mesh": {M + "shaderFeatures": ["FLASH"]}})
    await ed.call("create_material", path=MATERIAL, scene=scene, entity=S["Linked"], submesh=0)
    S["material_bytes"] = (ed.project / MATERIAL).read_bytes()
    ed.check("DITHER_FADE" not in S["material_bytes"].decode(), "the linked file has no Dither Fade")
    await ed.call("save_scene", scene=scene, path="scenes/PixelFx")


async def test_effects_end_where_the_spec_says(ed: Editor):
    """Each effect over 0.2 s of game time: the handle is done once it is over, the value it animated where it ends."""
    await ed.call("play", action="start", scene=S["scene"], fixed_fps=60, auto_pause=True)
    try:
        await ed.call("wait", frames=5)
        ends = {"flash": ("flashAmount", 0.0), "fade_out": ("ditherFadeAmount", 1.0), "fade_in": ("ditherFadeAmount", 0.0),
                "blink": ("ditherFadeAmount", 0.0), "dissolve": ("dissolveProgress", 1.0), "undissolve": ("dissolveProgress", 0.0),
                "petrify": ("rampAmount", 0.0), "freeze": ("rampAmount", 0.0), "poison": ("rampAmount", 0.0)}
        for effect, (member, end) in ends.items():
            started = await lua(ed, f"PFX_H = PixelFX.play({go('Hero')}, '{effect}', 0.2) return PFX_H.valid, PFX_H.progress")
            ed.check(started[0] is True, f"{effect}: a valid handle: {started}")
            waited = await ed.call_raw("wait", {"until": "PFX_H:isDone()", "timeout_seconds": 30})
            ed.check(not waited.is_error, f"{effect}: the handle finishes: {waited.text}")
            values = await lua(ed, f"return PFX_H.progress, {param('Hero', member)}, PixelFX.activeCount")
            ed.check_eq(values[0], 1.0, f"{effect}: progress 1 at its end")
            ed.check(abs(values[1] - end) < 1e-6, f"{effect}: {member} ends at {end}: {values[1]}")
        # the outline pulse ends with its outline hidden (OUTLINE turned on by it)
        await lua(ed, f"PFX_H = PixelFX.play({go('Hero')}, 'outline_pulse', 0.2, {{count = 2, color = '#ff0000'}})")
        await ed.call_raw("wait", {"until": "PFX_H:isDone()", "timeout_seconds": 30})
        values = await lua(ed, f"local m = Mesh(Engine.scene, Engine.scene:findEntity('Hero')):getMaterial() "
                               "return m:isShaderFeatureEnabled('OUTLINE'), m:getShaderParam('outlineColor').w")
        ed.check(values[0] is True and values[1] == 0.0, f"outline_pulse: its outline on, hidden at the end: {values}")
    finally:
        await ed.call("play", action="stop", fixed_fps=0, auto_pause=False)


async def test_wait_for_resumes_once(ed: Editor):
    """Coroutine.waitFor(handle) resumes once, the frame the effect ends; onFinished runs once."""
    await ed.call("play", action="start", scene=S["scene"], fixed_fps=60, auto_pause=True)
    try:
        await ed.call("wait", frames=3)
        await lua(ed, f"PFX_RESUMED = 0 PFX_FINISHED = 0 "
                      f"local h = PixelFX.play({go('Hero')}, 'flash', 0.25, {{color = '#ffffff', onFinished = function() PFX_FINISHED = PFX_FINISHED + 1 end}}) "
                      "PFX_H = h "
                      "Coroutine.start(function() Coroutine.waitFor(h) PFX_RESUMED = PFX_RESUMED + 1 PFX_AT = h.progress end)")
        await ed.call("wait", frames=5)
        mid = await lua(ed, "return PFX_RESUMED, PFX_H.progress")
        ed.check(mid[0] == 0 and 0.0 < mid[1] < 1.0, f"still running: {mid}")
        await ed.call_raw("wait", {"until": "PFX_RESUMED > 0", "timeout_seconds": 30})
        await ed.call("wait", frames=10)
        after = await lua(ed, "return PFX_RESUMED, PFX_FINISHED, PFX_AT")
        ed.check_eq(after[:2], [1, 1], f"resumed once, onFinished once: {after}")
        ed.check_eq(after[2], 1.0, "it resumed once the effect was over")
        # a handle's onFinished set after play
        await lua(ed, f"PFX_LATE = 0 local h = PixelFX.play({go('Hero')}, 'flash', 0.1) h.onFinished = function() PFX_LATE = PFX_LATE + 1 end")
        await ed.call_raw("wait", {"until": "PFX_LATE > 0", "timeout_seconds": 30})
        ed.check_eq((await lua(ed, "return PFX_LATE"))[0], 1)
    finally:
        await ed.call("play", action="stop", fixed_fps=0, auto_pause=False)


async def test_runtime_feature_on_its_own_material(ed: Editor):
    """A fade on Plain and Linked: DITHER_FADE turned on in their own materials, logged once per material; the linked
    .material file is unchanged; the progress stays 0 until the variant draws."""
    cursor = (await ed.call("read_console", limit=1))["next_cursor"]
    await ed.call("play", action="start", scene=S["scene"], fixed_fps=60, auto_pause=True)
    try:
        await ed.call("wait", frames=3)
        first = await lua(ed, f"PFX_P = PixelFX.play({go('Plain')}, 'fade_out', 0.3) PFX_L = PixelFX.play({go('Linked')}, 'fade_out', 0.3) "
                              "return PFX_P.progress, PFX_L.progress")
        ed.check_eq(first, [0.0, 0.0], "progress 0 before any frame")
        await ed.call_raw("wait", {"until": "PFX_P:isDone() and PFX_L:isDone()", "timeout_seconds": 60})
        values = await lua(ed, f"local p = Mesh(Engine.scene, Engine.scene:findEntity('Plain')):getMaterial() "
                               f"local l = Mesh(Engine.scene, Engine.scene:findEntity('Linked')):getMaterial() "
                               f"return p:isShaderFeatureEnabled('DITHER_FADE'), l:isShaderFeatureEnabled('DITHER_FADE'), "
                               f"{param('Plain', 'ditherFadeAmount')}")
        ed.check_eq(values, [True, True, 1.0], "the feature on in both GameObjects' materials, the fade at its end")
        # the same effect again on Plain: no second log line for that material
        await lua(ed, f"PFX_P = PixelFX.play({go('Plain')}, 'fade_out', 0.1)")
        await ed.call_raw("wait", {"until": "PFX_P:isDone()", "timeout_seconds": 30})
    finally:
        await ed.call("play", action="stop", fixed_fps=0, auto_pause=False)
    entries = (await ed.call("read_console", since=cursor, limit=200))["entries"]
    lines = [e["message"] for e in entries if LOG_LINE in e.get("message", "")]
    ed.check_eq(len(lines), 2, f"one line per material (Plain's, Linked's): {lines}")
    ed.check(any("linked.material" in line for line in lines), f"the linked one named by its file: {lines}")
    ed.check(all("add it to the material (or record the warm-up list)" in line for line in lines), f"the fix in the line: {lines}")
    ed.check_eq((ed.project / MATERIAL).read_bytes(), S["material_bytes"], "the shared .material file is unchanged")
    got = await ed.call("get_entity", scene=S["scene"], entity=S["Plain"], components=["mesh"])
    ed.check("DITHER_FADE" not in str(got["components"]["mesh"]), "the edit scene's material is untouched by Play")


async def test_set_hit_stop_and_errors(ed: Editor):
    """PixelFX.set writes a channel; a hit-stop holds a game-time effect, not an unscaled one; errors name the fix."""
    await ed.call("play", action="start", scene=S["scene"], fixed_fps=60, auto_pause=True)
    try:
        await ed.call("wait", frames=3)
        values = await lua(ed, f"PixelFX.set({go('Hero')}, 'flash', 0.5) return {param('Hero', 'flashAmount')}")
        ed.check_eq(values[0], 0.5, "set writes the channel")
        await lua(ed, f"HitStop.play(1.0) PFX_G = PixelFX.play({go('Hero')}, 'flash', 0.5) "
                      f"PFX_U = PixelFX.play({go('Plain')}, 'flash', 0.5, {{unscaled = true}})")
        await ed.call("wait", frames=12)
        progress = await lua(ed, "return PFX_G.progress, PFX_U.progress")
        ed.check(progress[0] == 0.0, f"the game-time effect is frozen by the hit-stop: {progress}")
        ed.check(progress[1] > 0.0, f"the unscaled one runs (once its variant draws): {progress}")
        await lua(ed, "HitStop.stop()")
        for code, needle in ((f"PixelFX.play({go('Hero')}, 'melt', 1)", "flash, fade_out"),
                             ("PixelFX.play(42, 'flash', 1)", "GameObject"),
                             (f"PixelFX.play({go('Hero')}, 'flash', 1, {{ease = 'wobbly'}})", "ease"),
                             (f"PixelFX.play({go('Hero')}, 'flash', 1, {{embers = 'ember'}})", "dissolve"),
                             (f"PixelFX.set({go('Hero')}, 'melt', 1)", "channel")):
            r = await ed.call_raw("eval_lua", {"code": code})
            ed.check(r.is_error or needle in r.text, f"{code}: an error naming {needle!r}: {r.text}")
            ed.check_in(needle, r.text)
    finally:
        await ed.call("play", action="stop", fixed_fps=0, auto_pause=False)
