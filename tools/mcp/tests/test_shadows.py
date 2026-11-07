"""3D shadows (spec S10, docs/rendering/shadows.md): settings round trip through MCP, save, reload
and Lua (AC17); the shadow cache's paths seen through get_shadow_stats (AC11, AC12, AC13); the
stable cascades' shimmer metric under a moving and a turning fit camera (AC2, AC3); the cascade
blend and distance fade (AC4); acne and contact (AC6); point light faces (AC9); one golden image
per quality tier (AC5); a scene saved before the shadows (AC18); the shadow loops of the
cross-compiled shaders (AC7).

The scenes are built here with the tools (the Scene view with the shadow debug views and its
gizmos hidden). JASPER_SHADOW_UPDATE_GOLDEN=1 rewrites the goldens of tools/fixtures/shadows/golden."""

from __future__ import annotations

import json
import math
import os
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

import harness
import pngdiff
from harness import Editor

sys.path.insert(0, str(Path(__file__).parent))
from test_runtime_regressions import decode_png  # noqa: E402

CI = True
EDITOR = {"create": True, "name": "Shadows"}
TIMEOUT = 1500

GOLDEN_DIR = harness.REPO / "tools" / "fixtures" / "shadows" / "golden"
UPDATE_GOLDEN = os.environ.get("JASPER_SHADOW_UPDATE_GOLDEN") == "1"
# the measured images of each run, to look at (build/ is not in git)
SHOT_DIR = harness.REPO / "build" / "test-output" / "shadows"
SHOT_WIDTH = 640
DISPLAY = {"show_grid_3d": False, "hide_light_icons": True, "hide_selection_outline": True,
           "hide_camera_view": True, "hide_sound_icons": True}


# ------------------------------------------------------------------ helpers

async def make_scene(ed: Editor, name: str) -> int:
    made = await ed.call("create_scene", name=name, type="3D")
    # the goldens were drawn before S11's Lit Quality (a new scene is High): Standard is that shader
    await ed.call("set_scene_settings", scene=made["scene"]["id"], settings={"lit_quality": "standard"})
    return made["scene"]["id"]


async def sun_of(ed: Editor, scene: int) -> int:
    found = await ed.call("find_entities", scene=scene, component="light")
    entities = harness.find_entities(found)
    ed.check(entities, "the new 3D scene has its sun")
    return harness.entity_id_of(entities[0])


async def entity(ed: Editor, scene: int, kind: str, name: str, position, scale=None, values=None) -> int:
    args = {"scene": scene, "type": kind, "name": name, "position": position, "select": False}
    if scale is not None:
        args["scale"] = scale
    if values:
        args["values"] = values
    made = await ed.call("create_entity", **args)
    return made["entity_id"]


async def light_set(ed: Editor, scene: int, light: int, **values):
    await ed.call("set_properties", scene=scene, entity=light, values={"light": values})


# the goldens' and the measurements' field of view: 0.75 rad, the camera component's default the
# Scene view had before S26 made its own 60 degrees the default (set, not taken from a default)
SCENE_FOV = math.degrees(0.75)


async def aim(ed: Editor, scene: int, position, target):
    await ed.call("set_scene_camera", scene=scene, position=position, target=target, display=DISPLAY,
                  camera_settings={"fov": SCENE_FOV})


async def debug_view(ed: Editor, scene: int, view: str):
    await ed.call("set_scene_settings", scene=scene, settings={"shadows_debug_view": view})


async def shot(ed: Editor, scene: int, frames: int = 4) -> tuple:
    await ed.call("wait", frames=frames)
    r = await ed.call_raw("screenshot", {"view": "scene", "scene": scene, "max_width": SHOT_WIDTH}, timeout=300)
    ed.check(not r.is_error and r.images, f"no scene screenshot: {r.text[:300]}")
    return pngdiff.decode(r.images[0][0])


async def stats(ed: Editor, scene: int) -> dict:
    return await ed.call("get_shadow_stats", scene=scene)


def keep_shot(name: str, img) -> None:
    path = SHOT_DIR / f"{name}.png"
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_bytes(pngdiff.encode(img))


def check(condition, message: str) -> None:
    if not condition:
        raise harness.TestFailure(message)


def grey(img, x: int, y: int) -> int:
    w, h, rgba = img
    i = (y * w + x) * 4
    return rgba[i + 1]   # the debug views are grey: green is the value


def changed_fraction(a, b, threshold: int = 2, margin: float = 0.0) -> float:
    """The fraction of pixels (inside a left and right margin, a fraction of the width) whose
    8-bit value changed by more than threshold."""
    wa, ha, ra = a
    wb, hb, rb = b
    assert (wa, ha) == (wb, hb)
    x0 = int(wa * margin)
    x1 = wa - x0
    changed = 0
    for y in range(ha):
        for x in range(x0, x1):
            i = (y * wa + x) * 4
            if max(abs(ra[i] - rb[i]), abs(ra[i + 1] - rb[i + 1]), abs(ra[i + 2] - rb[i + 2])) > threshold:
                changed += 1
    return changed / float((x1 - x0) * ha)


def slots_of(st: dict) -> list:
    return [slot for light in st["lights"] for slot in light["slots"]]


async def settle(ed: Editor, scene: int, frames: int = 6) -> dict:
    await ed.call("wait", frames=frames)
    return await stats(ed, scene)


def normalize(v):
    n = math.sqrt(sum(c * c for c in v))
    return [c / n for c in v]


def cross(a, b):
    return [a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0]]


# ------------------------------------------------------------------ settings (AC17)

async def test_shadow_settings(ed: Editor):
    ed.require_tools("get_shadow_stats", "set_shadow_debug", "set_properties", "set_scene_settings", "eval_lua")
    scene = await make_scene(ed, "ShadowSettings")
    sun = await sun_of(ed, scene)
    cube = await entity(ed, scene, "cube", "Crate", [0, 0.5, 0])

    light_values = {"shadows": True, "softShadows": False, "shadowStrength": 0.6, "shadowBias": 1.5, "shadowNormalBias": 0.75,
                    "mapResolution": 1024, "numShadowCascades": 2, "cascadeSplitMode": "manual", "cascadeSplits": [0.1, 0.3, 0.6],
                    "shadowDistance": 55.0, "shadowAngularDiameter": 1.2, "shadowSourceRadius": 0.2, "shadowUpdateMode": "on_demand"}
    await ed.call("set_properties", scene=scene, entity=sun, values={"light": light_values})
    await ed.call("set_properties", scene=scene, entity=cube, values={"mesh": {"staticShadowCaster": True}})
    await ed.call("set_scene_settings", scene=scene, settings={"shadows_quality": "ultra", "shadow_distance": 70.0,
                                                               "shadow_fade_fraction": 0.2, "shadow_cascade_blend": False})
    await ed.call("set_project_settings", values={"graphics.shadow_tier.android": "medium"})

    await ed.call("save_scene", scene=scene, path="scenes/ShadowSettings.scene")
    text = (ed.project / "scenes/ShadowSettings.scene").read_text()
    for key in ("softShadows: false", "shadowStrength: 0.6", "shadowNormalBias: 0.75", "cascadeSplitMode: manual",
                "shadowDistance: 55", "shadowUpdateMode: onDemand", "staticShadowCaster: true", "shadowQuality: ultra",
                "shadowFadeFraction: 0.2", "shadowCascadeBlend: false"):
        ed.check(key in text, f"{key} saved")
    ed.check("shadowTier" in (ed.project / "project.yaml").read_text(), "the project's shadow tier saved")

    # reloaded from the file, read back through Lua
    await ed.call("close_scene", scene=scene, unsaved="discard")
    opened = await ed.call("open_scene", scene="scenes/ShadowSettings.scene")
    scene = opened["scene"]["id"]
    sun = await sun_of(ed, scene)
    found = await ed.call("find_entities", scene=scene, name="Crate")
    cube = harness.entity_id_of(harness.find_entities(found)[0])
    # each field on its own (pcall), so a missing binding names itself
    fields = {"type": "l.shadowType", "strength": "l.shadowStrength", "bias": "l.shadowBias", "normal": "l.shadowNormalBias",
              "res": "l.shadowResolution", "cascades": "l.numCascades", "mode": "l.cascadeSplitMode", "split": "l.cascadeSplits.y",
              "dist": "l.shadowDistance", "angle": "l.shadowAngularDiameter", "radius": "l.shadowSourceRadius",
              "update": "l.shadowUpdateMode", "static": "m.staticShadowCaster", "quality": "scene.shadowQuality",
              "sdist": "scene.shadowDistance", "fade": "scene.shadowFadeFraction", "blend": "scene.shadowCascadeBlend",
              "HARD": "ShadowType.HARD", "MANUAL": "CascadeSplitMode.MANUAL", "ON_DEMAND": "ShadowUpdateMode.ON_DEMAND",
              "ULTRA": "ShadowQuality.ULTRA"}
    code = f"local l = object({sun}, 'Light') local m = object({cube}, 'Mesh') local out = {{}} "
    for key, expression in fields.items():
        code += (f"do local ok, value = pcall(function() return {expression} end) "
                 f"if ok then out['{key}'] = value else out['{key}'] = 'ERROR ' .. tostring(value) end end ")
    code += "return out"
    lua = await ed.call("eval_lua", scene=scene, allow_edit_mode=True, code=code)
    v = lua["values"][0]
    errors = {k: x for k, x in v.items() if isinstance(x, str) and x.startswith("ERROR")}
    ed.check(not errors, f"every shadow field reads in Lua: {errors}")
    ed.check_eq(v["type"], v["HARD"], "shadowType")
    ed.check(abs(v["strength"] - 0.6) < 1e-5 and abs(v["bias"] - 1.5) < 1e-5 and abs(v["normal"] - 0.75) < 1e-5, f"biases {v}")
    ed.check_eq(v["res"], 1024)
    ed.check_eq(v["cascades"], 2)
    ed.check_eq(v["mode"], v["MANUAL"])
    ed.check(abs(v["split"] - 0.3) < 1e-5 and abs(v["dist"] - 55) < 1e-4 and abs(v["angle"] - 1.2) < 1e-5 and abs(v["radius"] - 0.2) < 1e-5,
             f"cascade and source values {v}")
    ed.check_eq(v["update"], v["ON_DEMAND"])
    ed.check_eq(v["static"], True)
    ed.check_eq(v["quality"], v["ULTRA"])
    ed.check(abs(v["sdist"] - 70) < 1e-4 and abs(v["fade"] - 0.2) < 1e-5 and v["blend"] is False, f"scene values {v}")
    project = await ed.call("get_project_settings", keys=["graphics.shadow_tier.android"])
    ed.check_eq(harness.find_key(project, "graphics.shadow_tier.android"), "medium")

    # the effective tier follows the scene's Quality
    st = await settle(ed, scene)
    ed.check_eq(st["tier"], "ultra")
    await ed.call("set_scene_settings", scene=scene, settings={"shadows_quality": "low"})
    ed.check_eq((await settle(ed, scene))["tier"], "low")
    lua = await ed.call("eval_lua", scene=scene, allow_edit_mode=True, code="return {t = scene.effectiveShadowTier, L = ShadowTier.LOW}")
    ed.check_eq(lua["values"][0]["t"], lua["values"][0]["L"], "Lua effectiveShadowTier")

    # undo reverts a light field (one step)
    await ed.call("set_properties", scene=scene, entity=sun, values={"light": {"shadowBias": 3.0}})
    await ed.call("undo")
    got = await ed.call("get_entity", scene=scene, entity=sun, components=["light"])
    ed.check(abs(harness.find_key(got, "shadowBias") - 1.5) < 1e-5, "undo reverts the bias")


# ------------------------------------------------------------------ cache (AC10-AC13)

def redrawn_since(st: dict, update: int) -> list:
    """The slots drawn after shadow update `update` (their last_render)."""
    return [s for s in slots_of(st) if s["frames_since_render"] < st["update"] - update]


async def cache_scene(ed: Editor, name: str):
    scene = await make_scene(ed, name)
    sun = await sun_of(ed, scene)
    await light_set(ed, scene, sun, shadows=True, direction=[-0.35, -0.85, -0.4])
    ground = await entity(ed, scene, "plane", "Ground", [0, 0, 0], scale=[2, 1, 2], values={"mesh": {"staticShadowCaster": True}})
    static_box = await entity(ed, scene, "cube", "StaticBox", [-3, 0.5, 0], values={"mesh": {"staticShadowCaster": True}})
    dynamic_box = await entity(ed, scene, "cube", "DynamicBox", [3, 0.5, 0])
    spot = await entity(ed, scene, "spot_light", "Spot", [0, 6, 4])
    await light_set(ed, scene, spot, shadows=True, range=20.0, direction=[0.0, -0.94, -0.34])
    await ed.call("set_scene_settings", scene=scene, settings={"shadows_quality": "medium"})
    await aim(ed, scene, [0, 9, 14], [0, 0, 0])
    return scene, sun, ground, static_box, dynamic_box, spot


async def changed(ed: Editor, scene: int, change) -> tuple:
    """(stats, slots redrawn) after one change, the scene settled before it."""
    before = await settle(ed, scene, frames=6)
    await change()
    st = await settle(ed, scene, frames=3)
    return st, redrawn_since(st, before["update"])


async def test_shadow_cache(ed: Editor):
    scene, sun, ground, static_box, dynamic_box, spot = await cache_scene(ed, "ShadowCache")

    # still: every slot skips and nothing is drawn
    await settle(ed, scene, frames=10)
    st = await settle(ed, scene, frames=3)
    slots = slots_of(st)
    ed.check(len(slots) >= 2, f"the sun and the spot have slots: {st['lights']}")
    ed.check(all(s["path"] == "skip" for s in slots), f"a still scene skips every slot: {[(s['slot'], s['path'], s['dirty_cause']) for s in slots]}")
    ed.check_eq(st["draw_calls_total"], 0, "a still scene draws nothing into its shadow maps")
    ed.check_eq(st["passes"], 0, "nor begins a shadow pass")

    # a dynamic caster moves: its slots restore their static layer under it, within its rects
    st, touched = await changed(ed, scene, lambda: ed.call("set_transform", scene=scene, entity=dynamic_box, position=[3.5, 0.5, 0.5]))
    ed.check(touched, "the move redrew something")
    for s in touched:
        last = s["last_render"]
        ed.check(last["path"] in ("restore", "full"), f"a dynamic move restores or redraws: {s}")
        ed.check_eq(last["dirty_cause"], "dynamic_caster")
        if last["path"] == "restore":
            r, d = s["rect"], last["dirty_rect"]
            ed.check(d[0] >= r[0] and d[1] >= r[1] and d[0] + d[2] <= r[0] + r[2] and d[1] + d[3] <= r[1] + r[3] and d[2] < r[2],
                     f"the dirty rect lies inside the slot and is smaller: {s}")
    ed.check(any(s["last_render"]["path"] == "restore" for s in touched), f"the static layer is restored under it: {touched}")
    ed.check(all(s["path"] == "skip" for s in slots_of(st)), "and the cache settles again")

    # a static caster moves: the static layer of the slots it touches is redrawn
    st, touched = await changed(ed, scene, lambda: ed.call("set_transform", scene=scene, entity=static_box, position=[-3.5, 0.5, 0]))
    ed.check(touched and all(s["last_render"]["path"] in ("static", "full") and s["last_render"]["dirty_cause"] == "static_caster"
                             for s in touched), f"a static move redraws the static layers it touches: {touched}")

    # the image is the same whether the slots were cached or redrawn
    await debug_view(ed, scene, "shadow_factor")
    cached = await shot(ed, scene)
    await ed.call("set_shadow_debug", scene=scene, force_rerender=True)
    forced = await shot(ed, scene)
    await ed.call("set_shadow_debug", scene=scene, force_rerender=False)
    if changed_fraction(cached, forced, 0) > 0.0:
        keep_shot("cache_cached", cached)
        keep_shot("cache_forced", forced)
    ed.check_eq(changed_fraction(cached, forced, 0), 0.0, "cached and re-rendered shadows are identical")
    await debug_view(ed, scene, "none")


async def test_shadow_invalidation(ed: Editor):
    """Each R18 trigger, once: the redrawn slots' cause, and the image equal to a forced re-render."""
    scene, sun, ground, static_box, dynamic_box, spot = await cache_scene(ed, "ShadowInvalidation")
    await settle(ed, scene, frames=10)
    await debug_view(ed, scene, "shadow_factor")

    async def expect(change, causes: set, label: str):
        st, touched = await changed(ed, scene, change)
        ed.check(touched, f"{label}: something was redrawn")
        ed.check(all(s["last_render"]["dirty_cause"] in causes for s in touched),
                 f"{label}: causes {[s['last_render']['dirty_cause'] for s in touched]} in {causes}")
        cached = await shot(ed, scene, frames=2)
        await ed.call("set_shadow_debug", scene=scene, force_rerender=True)
        forced = await shot(ed, scene, frames=2)
        await ed.call("set_shadow_debug", scene=scene, force_rerender=False)
        if changed_fraction(cached, forced, 0) > 0.0:
            keep_shot(f"invalidation_{label}_cached", cached)
            keep_shot(f"invalidation_{label}_forced", forced)
        ed.check_eq(changed_fraction(cached, forced, 0), 0.0, f"{label}: the cached image is the re-rendered one")

    await expect(lambda: ed.call("set_transform", scene=scene, entity=dynamic_box, position=[2.5, 0.5, -0.5]), {"dynamic_caster"}, "transform")
    await expect(lambda: ed.call("set_transform", scene=scene, entity=dynamic_box, rotation={"euler": [0, 30, 0]}), {"dynamic_caster"}, "rotation")
    await expect(lambda: ed.call("set_entity_state", scene=scene, entity=dynamic_box, active=False), {"dynamic_caster"}, "deactivate")
    await expect(lambda: ed.call("set_entity_state", scene=scene, entity=dynamic_box, active=True), {"dynamic_caster"}, "activate")
    await expect(lambda: ed.call("set_entity_state", scene=scene, entity=static_box, layer=3), {"static_caster"}, "layer")
    await expect(lambda: ed.call("set_properties", scene=scene, entity=dynamic_box, values={"mesh": {"castShadows": False}}),
                 {"dynamic_caster"}, "castShadows off")
    await expect(lambda: ed.call("set_properties", scene=scene, entity=dynamic_box, values={"mesh": {"castShadows": True}}),
                 {"dynamic_caster"}, "castShadows on")
    await expect(lambda: ed.call("set_properties", scene=scene, entity=dynamic_box, values={"mesh": {"staticShadowCaster": True}}),
                 {"static_caster"}, "static flag")
    await expect(lambda: light_set(ed, scene, spot, range=18.0), {"light", "fit"}, "light range")
    await expect(lambda: light_set(ed, scene, sun, mapResolution=512), {"light", "atlas"}, "resolution")
    await expect(lambda: light_set(ed, scene, sun, direction=[-0.3, -0.8, -0.5]), {"light", "fit"}, "light direction")
    await expect(lambda: light_set(ed, scene, spot, cullingMask=1), {"light"}, "culling mask")
    await expect(lambda: ed.call("delete_entities", scene=scene, entities=[static_box]), {"static_caster"}, "removed")

    # receiver-only settings redraw nothing
    st, touched = await changed(ed, scene, lambda: light_set(ed, scene, sun, shadowBias=2.0, shadowNormalBias=2.0, shadowStrength=0.5,
                                                             softShadows=False))
    ed.check(not touched, f"receiver-only edits redraw nothing: {[s['last_render'] for s in touched]}")
    await debug_view(ed, scene, "none")


async def test_on_demand(ed: Editor):
    scene, sun, ground, static_box, dynamic_box, spot = await cache_scene(ed, "ShadowOnDemand")
    await light_set(ed, scene, sun, shadowUpdateMode="on_demand")
    await light_set(ed, scene, spot, shadowUpdateMode="on_demand")
    await debug_view(ed, scene, "shadow_factor")
    before = await shot(ed, scene, frames=8)
    st, touched = await changed(ed, scene, lambda: ed.call("set_transform", scene=scene, entity=dynamic_box, position=[0, 0.5, 2]))
    ed.check(not touched, f"On Demand: a moving caster redraws nothing: {[s['last_render'] for s in touched]}")
    held = await shot(ed, scene, frames=2)
    ed.check(changed_fraction(before, held, 8) > 0.0005, "the cube itself moved")
    st, touched = await changed(ed, scene, lambda: ed.call(
        "eval_lua", scene=scene, allow_edit_mode=True,
        code=f"object({sun}, 'Light'):requestShadowUpdate(); object({spot}, 'Light'):requestShadowUpdate(); return true"))
    ed.check(touched and all(s["last_render"]["dirty_cause"] == "forced" for s in touched),
             f"requestShadowUpdate redraws: {[s['last_render'] for s in touched]}")
    updated = await shot(ed, scene, frames=2)
    ed.check(changed_fraction(held, updated, 8) > 0.0005, "then the shadows follow")
    await debug_view(ed, scene, "none")


# ------------------------------------------------------------------ stability (AC2, AC3)

async def stability_scene(ed: Editor, name: str):
    scene = await make_scene(ed, name)
    sun = await sun_of(ed, scene)
    await light_set(ed, scene, sun, shadows=True, softShadows=True, direction=[-0.45, -0.8, -0.4])
    await entity(ed, scene, "plane", "Ground", [0, 0, 0], scale=[20, 1, 20])
    for i in range(-3, 4):
        for j in range(-2, 3):
            await entity(ed, scene, "cylinder" if (i + j) % 2 else "cube", f"Caster{i}_{j}", [i * 3.0, 0.75, j * 3.0], scale=[0.6, 1.5, 0.6])
    await ed.call("set_scene_settings", scene=scene, settings={"shadows_quality": "high"})
    await aim(ed, scene, [0, 6, 16], [0, 0, 0])
    await debug_view(ed, scene, "shadow_factor")
    await ed.call("wait", frames=10)
    st = await stats(ed, scene)
    sun_stats = [light for light in st["lights"] if light["type"] == "directional"][0]
    return scene, sun, sun_stats


async def test_no_shimmer_translate(ed: Editor):
    scene, sun, sun_stats = await stability_scene(ed, "Stability")
    texel = sun_stats["texel_world"][0]
    ed.check(texel > 0, f"cascade 0 has a texel size: {sun_stats}")
    # light-space X (the cascade basis: up Y unless the light is near vertical) and the view's Z
    direction = normalize(sun_stats["direction"])
    light_x = normalize(cross(direction, [0.0, 1.0, 0.0]))
    view_z = normalize([0 - 0, 0 - 6, 0 - 16])
    step = 0.13 * texel
    previous = await shot(ed, scene)
    worst = 0.0
    for k in range(1, 33):
        axis = light_x if k <= 16 else view_z
        n = k if k <= 16 else k - 16
        base = [light_x[i] * step * 16 for i in range(3)] if k > 16 else [0.0, 0.0, 0.0]
        offset = [base[i] + axis[i] * step * n for i in range(3)]
        await ed.call("set_shadow_debug", scene=scene, fit_camera_offset=offset)
        current = await shot(ed, scene, frames=2)
        changed = changed_fraction(previous, current)
        if changed > worst:
            worst = changed
            keep_shot("shimmer_translate_a", previous)
            keep_shot("shimmer_translate_b", current)
        previous = current
    await ed.call("set_shadow_debug", scene=scene, fit_camera_offset=[0, 0, 0])
    print(f"     [shimmer] translate: worst {worst * 100:.4f}% of the pixels changed by more than 2")
    ed.check(worst <= 0.0005, f"translation shimmer: {worst * 100:.4f}% of the pixels changed (at most 0.05%)")
    await debug_view(ed, scene, "none")


async def test_no_shimmer_rotate(ed: Editor):
    """Only the camera the cascades are fitted to turns (the view stays), from -4 to +4 degrees of yaw:
    the screen's sides it turns away from leave its frustum, and with it the cascades (a camera that
    really turns takes its view along), so they are left out of the metric."""
    scene, sun, sun_stats = await stability_scene(ed, "StabilityRotate")
    await ed.call("set_shadow_debug", scene=scene, fit_camera_euler=[0.0, -4.0, 0.0])
    previous = await shot(ed, scene)
    worst = 0.0
    for k in range(1, 33):
        await ed.call("set_shadow_debug", scene=scene, fit_camera_euler=[0.0, -4.0 + 0.25 * k, 0.0])
        current = await shot(ed, scene, frames=2)
        changed = changed_fraction(previous, current, margin=0.1)
        if changed > worst:
            worst = changed
            keep_shot("shimmer_rotate_a", previous)
            keep_shot("shimmer_rotate_b", current)
        previous = current
    await ed.call("set_shadow_debug", scene=scene, fit_camera_euler=[0, 0, 0])
    st = await stats(ed, scene)
    radius = [light for light in st["lights"] if light["type"] == "directional"][0]["radius"]
    ed.check_eq(radius, sun_stats["radius"], "turning keeps the cascade extents")
    print(f"     [shimmer] rotate: worst {worst * 100:.4f}% of the pixels changed by more than 2")
    ed.check(worst <= 0.0005, f"rotation shimmer: {worst * 100:.4f}% of the pixels changed (at most 0.05%)")
    await debug_view(ed, scene, "none")


# ------------------------------------------------------------------ blend and fade (AC4)

def tint_of(img, x: int, y: int):
    """The cascade of a Cascades debug pixel (0 red, 1 green, 2 blue, 3 yellow), 'beyond' for the
    grey past the last cascade, None for a pixel the view does not tint (the sky)."""
    w, h, rgba = img
    i = (y * w + x) * 4
    r, g, b = rgba[i], rgba[i + 1], rgba[i + 2]
    if max(r, g, b) - min(r, g, b) <= 3:
        return "beyond"
    # the tints (red, green, blue, yellow) dimmed by the shadow and the band's hatching
    if r > 1.3 * b and g > 1.3 * b and abs(r - g) < 0.15 * max(r, g):
        return 3
    if r > 1.3 * g and r > 1.3 * b:
        return 0
    if g > 1.3 * r and g > 1.3 * b:
        return 1
    if b > 1.2 * r and b > 1.15 * g:
        return 2
    return None


async def test_cascade_blend_fade(ed: Editor):
    scene = await make_scene(ed, "BlendFade")
    sun = await sun_of(ed, scene)
    # light from the left: a long fence at x = -2 shades the ground under the view's centre line,
    # from the camera to past the shadow distance
    await light_set(ed, scene, sun, shadows=True, shadowDistance=60.0, numShadowCascades=4, direction=[0.6, -0.8, 0.0])
    await entity(ed, scene, "plane", "Long", [0, 0, -50], scale=[4, 1, 12])
    await entity(ed, scene, "cube", "Fence", [-2.0, 1.5, -50], scale=[0.2, 3.0, 120.0])
    await ed.call("set_scene_settings", scene=scene, settings={"shadows_quality": "high", "shadow_fade_fraction": 0.1})
    await aim(ed, scene, [0, 6.0, 8], [0, 0.0, -30])

    await debug_view(ed, scene, "cascades")
    cascades = await shot(ed, scene, frames=10)
    await debug_view(ed, scene, "shadow_factor")
    factor = await shot(ed, scene)
    await debug_view(ed, scene, "none")
    keep_shot("blend_cascades", cascades)
    keep_shot("blend_factor", factor)

    w, h, _ = cascades
    cx = w // 2
    column = [tint_of(cascades, cx, y) for y in range(h)]
    # the cascade changes along the centre column (bottom = near); a blend band between two
    # cascades mixes their tints (rows of no cascade), so a split is where the cascade of the
    # rows that have one changes, however many rows the band takes at this view's size
    boundaries = []
    last = None
    for y, c in enumerate(column):
        if c == "beyond":
            last = None   # past the shadow distance: its fade is no split
        elif isinstance(c, int):
            if last is not None and c != last:
                boundaries.append(y)
            last = c
    ed.check(len(boundaries) >= 2, f"the cascades show along the centre line: {sorted(set(c for c in column if c is not None), key=str)}")
    for y in boundaries:
        for yy in range(max(1, y - 3), min(h, y + 4)):
            step = abs(grey(factor, cx, yy) - grey(factor, cx, yy - 1))
            ed.check(step <= 8, f"no seam at the split near row {y}: step {step} at row {yy}")
    # inside the shadow distance the centre line is shadowed, beyond it exactly lit
    near = [y for y in range(h) if isinstance(column[y], int)]
    ed.check(near and grey(factor, cx, max(near)) < 64, "the fence shades the centre line near the camera")
    beyond = [y for y in range(h) if column[y] == "beyond"]
    ed.check(beyond, "the plane reaches past the shadow distance")
    ed.check(all(grey(factor, cx, y) == 255 for y in beyond), "past the shadow distance the plane is lit (255)")
    await golden(ed, "cascades", cascades)


# ------------------------------------------------------------------ bias (AC6)

def is_grey(img, x: int, y: int) -> bool:
    w, h, rgba = img
    i = (y * w + x) * 4
    return rgba[i] == rgba[i + 1] == rgba[i + 2]


async def test_bias(ed: Editor):
    """A flat plane under a sun 1 to 80 degrees from vertical (the receiver at that angle to the
    light): its lit half (x > 0) shows no acne, its shadowed half (under a slab placed up the light
    so its shadow's edge is x = 0 at every angle, above and behind the camera) is even. Then a
    cube's shadow meets its base: no lit gap between the cube's shaded side and its shadow."""
    scene = await make_scene(ed, "Bias")
    sun = await sun_of(ed, scene)
    await light_set(ed, scene, sun, shadows=True)
    await ed.call("set_scene_settings", scene=scene, settings={"shadows_quality": "high"})
    await entity(ed, scene, "plane", "Ground", [0, 0, 0])
    slab = await entity(ed, scene, "cube", "Slab", [-5, 8, 0], scale=[10, 0.1, 30])
    await aim(ed, scene, [0, 5, 0.01], [0, 0, 0])
    await debug_view(ed, scene, "shadow_factor")
    # High (the receiver-plane bias) and Low (without it: the normal offset alone)
    for tier, angle in [(t, a) for t in ("high", "low") for a in (1, 15, 30, 45, 60, 80)]:
        await ed.call("set_scene_settings", scene=scene, settings={"shadows_quality": tier})
        a = math.radians(angle)
        await light_set(ed, scene, sun, direction=[math.sin(a), -math.cos(a), 0.0])
        # its shadow's edge on the ground at x = 0: the slab's right edge 8 m up the light
        await ed.call("set_transform", scene=scene, entity=slab, position=[-8.0 * math.tan(a) - 5.0, 8.0, 0.0])
        img = await shot(ed, scene, frames=6)
        keep_shot(f"bias_{tier}_{angle}", img)
        w, h, rgba = img
        lit = lit_full = 0
        values = []
        margin = w // 12
        for y in range(0, h, 2):
            for x in range(0, w, 2):
                if not is_grey(img, x, y):
                    continue   # not the plane
                if x > w // 2 + margin:
                    lit += 1
                    lit_full += 1 if grey(img, x, y) == 255 else 0
                elif x < w // 2 - margin:
                    values.append(grey(img, x, y))
        ed.check(lit > 1000 and len(values) > 1000, f"{tier} {angle} deg: the plane fills both halves ({lit}, {len(values)} pixels)")
        mean = sum(values) / len(values)
        variance = sum((v - mean) ** 2 for v in values) / len(values)
        ed.check(lit_full / lit >= 0.995, f"{tier} {angle} deg: {lit_full}/{lit} lit pixels at 255 (no acne)")
        ed.check(mean < 8 and variance < 1.0,
                 f"{tier} {angle} deg: the shadowed half is dark and even (mean {mean:.1f}, variance {variance:.2f})")

    # contact: a cube on the lit side, seen from the side its shadow falls to; along the column
    # through its shaded face, the ground's shadow runs into the face with no lit texel between
    await ed.call("set_scene_settings", scene=scene, settings={"shadows_quality": "high"})
    a = math.radians(45)
    await light_set(ed, scene, sun, direction=[math.sin(a), -math.cos(a), 0.0])
    await ed.call("set_transform", scene=scene, entity=slab, position=[-13.0, 8.0, 0.0])
    await entity(ed, scene, "cube", "Contact", [2.5, 0.5, 0])
    await aim(ed, scene, [7.0, 2.5, 0.01], [3.0, 0.3, 0.0])
    img = await shot(ed, scene, frames=6)
    keep_shot("bias_contact", img)
    w, h, rgba = img
    cx = w // 2
    column = [grey(img, cx, y) for y in range(h - 1, -1, -1)]   # bottom up: near ground first
    dark = [i for i, v in enumerate(column) if v < 64]
    ed.check(dark, "the cube and its shadow show")
    first, last = dark[0], dark[0]
    while last + 1 < len(column) and (column[last + 1] < 64 or (last + 2 < len(column) and column[last + 2] < 64)):
        last += 1
    gap = sum(1 for v in column[first:last + 1] if v >= 128)
    ed.check(gap <= 1, f"the shadow touches the cube's base: {gap} lit pixels between them")
    await debug_view(ed, scene, "none")


# ------------------------------------------------------------------ point lights (AC9)

async def test_point_shadows(ed: Editor):
    scene = await make_scene(ed, "PointShadows")
    sun = await sun_of(ed, scene)
    await light_set(ed, scene, sun, shadows=False, intensity=0.0)
    point = await entity(ed, scene, "point_light", "Point", [0, 3, 0])
    await light_set(ed, scene, point, shadows=True, range=30.0, intensity=40.0)
    await entity(ed, scene, "plane", "Ground", [0, 0, 0], scale=[4, 1, 4])
    # casters around the light, so shadows cross the cube faces' edges (the diagonals)
    for k in range(8):
        a = k * math.pi / 4 + math.pi / 8
        await entity(ed, scene, "cylinder", f"Pole{k}", [math.cos(a) * 3, 0.75, math.sin(a) * 3], scale=[0.3, 1.5, 0.3])
    await ed.call("set_scene_settings", scene=scene, settings={"shadows_quality": "high"})
    await aim(ed, scene, [0, 16, 0.01], [0, 0, 0])
    await debug_view(ed, scene, "shadow_factor")
    img = await shot(ed, scene, frames=10)
    keep_shot("point_faces", img)
    w, h, rgba = img
    # along the face edges seen from above (the x = z diagonals): no step over 8 between neighbours
    # on the lit ground (shadows from the poles are away from the diagonals)
    worst = 0
    for t in range(-h // 3, h // 3):
        for sx, sy in ((1, 1), (1, -1)):
            x, y = w // 2 + t * sx, h // 2 + t * sy
            x2, y2 = x + sx, y + sy
            if 0 < x < w - 1 and 0 < y < h - 1 and 0 < x2 < w - 1 and 0 < y2 < h - 1:
                worst = max(worst, abs(grey(img, x, y) - grey(img, x2, y2)))
    ed.check(worst <= 8, f"no seam across the cube faces: step {worst}")
    st = await stats(ed, scene)
    point_stats = [light for light in st["lights"] if light["type"] == "point"][0]
    ed.check(len(point_stats["slots"]) == 6 and all(s["atlas"] == "point" for s in point_stats["slots"]), "six faces in the point atlas")
    await debug_view(ed, scene, "none")


async def penumbra_of(ed: Editor, kind: str) -> tuple:
    """(penumbra pixels per edge, pixels per metre) of a pole's soft shadow cast by a light of kind
    4 m above the ground, 1024 texels per map: across the shadow 2.2 m from under the light."""
    scene = await make_scene(ed, f"Penumbra{kind}")
    sun = await sun_of(ed, scene)
    await light_set(ed, scene, sun, shadows=False, intensity=0.0)
    light = await entity(ed, scene, f"{kind}_light", "Light", [0, 4, 0])
    values = {"shadows": True, "softShadows": True, "range": 30.0, "intensity": 40.0, "shadowSourceRadius": 0.3, "mapResolution": 1024}
    if kind == "spot":
        values.update({"direction": [0.0, -1.0, 0.0], "outerConeCos": 0.7071068, "innerConeCos": 0.75})   # a 90 degree cone: a cube face
    await light_set(ed, scene, light, **values)
    await entity(ed, scene, "plane", "Ground", [0, 0, 0])
    await entity(ed, scene, "cylinder", "Pole", [1.5, 1.0, 0], scale=[0.1, 1.0, 0.1])
    await ed.call("set_scene_settings", scene=scene, settings={"shadows_quality": "high"})
    await aim(ed, scene, [2.2, 6.0, 0.01], [2.2, 0.0, 0.0])
    await debug_view(ed, scene, "shadow_factor")
    img = await shot(ed, scene, frames=10)
    keep_shot(f"penumbra_{kind}", img)
    w, h, rgba = img
    column = [grey(img, w // 2, y) for y in range(h)]
    soft = sum(1 for v in column if 24 < v < 231)
    # the shadow's width there: the pole (0.2 m) at the height whose shadow lands 2.2 m out,
    # 1.27 m, magnified by 4 / (4 - 1.27): the ground's scale in pixels
    dark = sum(1 for v in column if v < 128)
    await debug_view(ed, scene, "none")
    await ed.call("close_scene", scene=scene, unsaved="discard")
    return soft / 2.0, dark / (0.2 * 4.0 / (4.0 - 1.27))


async def test_point_spot_penumbra(ed: Editor):
    """AC9: a point light's soft penumbra is a spot light's at the same distance and resolution."""
    spot, spot_ppm = await penumbra_of(ed, "spot")
    point, point_ppm = await penumbra_of(ed, "point")
    ppm = max(spot_ppm, point_ppm, 1.0)
    # one shadow texel 4.5 m from the light at 1024 texels over a 90 degree face: 2 * 4.5 / 1024 m
    texel_px = 2.0 * 4.5 / 1024.0 * ppm
    print(f"     [penumbra] spot {spot:.1f} px, point {point:.1f} px per edge, texel {texel_px:.2f} px")
    ed.check(spot > 0 and point > 0, "both shadows are soft")
    ed.check(abs(spot - point) <= max(texel_px, 1.0) + 1.0,
             f"point {point:.1f} px vs spot {spot:.1f} px: within a texel ({texel_px:.2f} px)")


# ------------------------------------------------------------------ goldens per tier (AC5, AC8)

async def golden(ed: Editor, name: str, img):
    path = GOLDEN_DIR / f"{name}.png"
    if UPDATE_GOLDEN:
        GOLDEN_DIR.mkdir(parents=True, exist_ok=True)
        path.write_bytes(pngdiff.encode(img))
        keep_shot(f"golden/{name}", img)
        print(f"     [golden] wrote {path.relative_to(harness.REPO)}: look at it")
        return
    ed.check(path.is_file(), f"no golden {path} (JASPER_SHADOW_UPDATE_GOLDEN=1 writes it)")
    # the Scene view's size follows the editor's layout: compared in the frame both show
    shown, reference = pngdiff.same_frame(img, pngdiff.load(path))
    ok, fraction, diff = pngdiff.compare(shown, reference, 24, 0.01)
    if not ok:
        keep_shot(f"{name}.actual", img)
        if diff:
            SHOT_DIR.mkdir(parents=True, exist_ok=True)
            (SHOT_DIR / f"{name}.diff.png").write_bytes(diff if isinstance(diff, bytes) else pngdiff.encode(diff))
    ed.check(ok, f"{name}: {fraction * 100:.2f}% of the pixels differ from the golden (at most 1%)")


async def test_shadow_goldens(ed: Editor):
    scene = await make_scene(ed, "Filters")
    sun = await sun_of(ed, scene)
    await light_set(ed, scene, sun, shadows=True, softShadows=True, shadowAngularDiameter=3.0, direction=[-0.4, -0.82, -0.4],
                    intensity=2.5)
    # little ambient light, so the filters' penumbras show at the goldens' tolerance
    await ed.call("set_scene_settings", scene=scene, settings={"global_illumination_intensity": 0.03})
    await entity(ed, scene, "plane", "Ground", [0, 0, 0], scale=[3, 1, 3])
    # a contact-hardening pole, a hard and a soft spot, a point light
    await entity(ed, scene, "cylinder", "Pole", [0, 1.5, 0], scale=[0.15, 3.0, 0.15])
    await entity(ed, scene, "cube", "Box", [-4, 0.5, 2])
    hard = await entity(ed, scene, "spot_light", "HardSpot", [-4, 5, 4])
    await light_set(ed, scene, hard, shadows=True, softShadows=False, range=12.0, intensity=25.0, direction=[0.0, -0.87, -0.5])
    soft = await entity(ed, scene, "spot_light", "SoftSpot", [4, 5, 4])
    await light_set(ed, scene, soft, shadows=True, softShadows=True, range=12.0, intensity=25.0, shadowSourceRadius=0.3,
                    direction=[0.0, -0.87, -0.5])
    await entity(ed, scene, "sphere", "Ball", [4, 0.6, 2], scale=[1.2, 1.2, 1.2])
    point = await entity(ed, scene, "point_light", "Lamp", [0, 2.5, -4])
    await light_set(ed, scene, point, shadows=True, range=10.0, intensity=12.0)
    await entity(ed, scene, "cube", "Crate", [1, 0.5, -3])
    await aim(ed, scene, [0, 9, 13], [0, 0, 0])
    for tier in ("low", "medium", "high", "ultra"):
        await ed.call("set_scene_settings", scene=scene, settings={"shadows_quality": tier})
        img = await shot(ed, scene, frames=12)
        await golden(ed, f"filter_{tier}", img)
        st = await stats(ed, scene)
        ed.check_eq(st["tier"], tier)
        ed.check(st["memory_bytes"] <= st["memory_budget_bytes"], f"{tier}: {st['memory_bytes']} bytes within the budget")

    # PCSS (Ultra): the pole's shadow is sharp at its base and wide at its top
    await ed.call("set_scene_settings", scene=scene, settings={"shadows_quality": "ultra"})
    await debug_view(ed, scene, "shadow_factor")
    img = await shot(ed, scene, frames=8)
    await debug_view(ed, scene, "none")
    await golden(ed, "pcss_factor", img)


async def test_pcss_contact_hardening(ed: Editor):
    """AC8: on Ultra (PCSS) a pole's shadow is sharp at its base and soft where its top casts. The
    sun is 45 degrees up, so the pole's point h metres up shades the ground h metres out; the shadow
    is measured across, from just above the ground (its umbra is the pole's 0.3 m). At the base the
    penumbra is under 2 shadow texels. Up the pole it is measured in metres: a count of texels
    follows the cascade's texel size, which follows the Scene view's frustum (S05's status bar made
    the view shorter and the same shadow 5.5 texels instead of 6.4), while PCSS widens the penumbra
    with the distance to the occluder as the sun's disc does (0.53 degrees: about 4 cm one edge at
    3 m up, 4.2 m along the light)."""
    scene = await make_scene(ed, "Pcss")
    sun = await sun_of(ed, scene)
    a = math.radians(45)
    await light_set(ed, scene, sun, shadows=True, softShadows=True, shadowAngularDiameter=0.53, direction=[math.sin(a), -math.cos(a), 0.0])
    await entity(ed, scene, "plane", "Ground", [0, 0, 0])
    await entity(ed, scene, "cylinder", "Pole", [0, 2.0, 0], scale=[0.15, 2.0, 0.15])
    await ed.call("set_scene_settings", scene=scene, settings={"shadows_quality": "ultra"})
    await debug_view(ed, scene, "shadow_factor")

    async def across(x: float) -> float:
        await aim(ed, scene, [x, 0.8, 0.01], [x, 0.0, 0.0])
        img = await shot(ed, scene, frames=8)
        keep_shot(f"pcss_{x:.1f}", img)
        w, h, rgba = img
        column = [grey(img, w // 2, y) for y in range(h)]
        soft = sum(1 for v in column if 24 < v < 231) / 2.0
        dark = sum(1 for v in column if v < 128)
        st = await stats(ed, scene)
        texel = [light for light in st["lights"] if light["type"] == "directional"][0]["texel_world"][0]
        ed.check(dark > 0 and texel > 0, f"the shadow shows {x} m out")
        metres = soft / (dark / 0.3)
        print(f"     [pcss] {x} m: soft {soft} px, dark {dark} px, texel {texel:.5f} m, penumbra {metres:.4f} m")
        return metres, metres / texel

    base_m, base = await across(0.25)
    top_m, top = await across(3.0)
    await debug_view(ed, scene, "none")
    disc = 3.0 / math.cos(a) * math.radians(0.53)
    print(f"     [pcss] penumbra {base:.1f} texels at the base, {top:.1f} texels ({top_m:.4f} m, the sun's disc "
          f"{disc:.4f} m) 3 m up")
    ed.check(base < 2.0, f"sharp at the base: {base:.1f} texels")
    ed.check(top_m > 0.5 * disc and top_m > 2.5 * base_m,
             f"soft 3 m up: {top_m:.4f} m (the sun's disc gives {disc:.4f} m; {base_m:.4f} m at the base)")


# ------------------------------------------------------------------ old scenes (AC18)

async def test_shadow_migration(ed: Editor):
    fixture = harness.REPO / "tools" / "fixtures" / "shadows"
    shutil.copy(fixture / "Legacy.scene", ed.project / "Legacy.scene")
    scripts = ed.project / "scripts"
    scripts.mkdir(exist_ok=True)
    shutil.copy(fixture / "scripts" / "Legacy.lua", scripts / "Legacy.lua")
    opened = await ed.call("open_scene", scene="Legacy.scene")
    scene = opened["scene"]["id"]
    report = await ed.call("get_load_report", scene=scene)
    text = str(report)
    ed.check("shadow texels" in text or "Bias reset" in text, f"the migration is reported: {text[:400]}")
    sun = await sun_of(ed, scene)
    got = await ed.call("get_entity", scene=scene, entity=sun, components=["light"])
    ed.check(abs(harness.find_key(got, "shadowBias") - 1.0) < 1e-6, "the old bias is 1 texel now")
    settings = await ed.call("get_scene_settings", scene=scene, names=["shadows_quality"])
    ed.check_eq(settings["settings"]["shadows_quality"], "auto", "the old default quality is Auto")
    await ed.call("create_entity", scene=scene, type="plane", name="Ground", scale=[2, 1, 2], select=False)
    await ed.call("create_entity", scene=scene, type="cube", name="Box", position=[0, 1, 0], select=False)
    await aim(ed, scene, [0, 6, 9], [0, 0, 0])
    img = await shot(ed, scene, frames=8)
    ed.check(not pngdiff.flat(img), "the migrated scene renders")


async def test_legacy_fork(ed: Editor):
    """AC18, R34: a fork of mesh.vert/frag written before these shadows (the runtime-shaders
    fixture's tinted.*: u_vs_shadows, u_fs_shadows, v_lightProjPos) compiles, check_shader names
    the legacy interface, and its directional shadows still show."""
    shaders = ed.project / "shaders"
    shaders.mkdir(exist_ok=True)
    source = harness.REPO / "tools" / "fixtures" / "runtime-shaders" / "assets" / "shaders"
    for name in ("tinted.vert", "tinted.frag"):
        shutil.copy(source / name, shaders / name)
    scene = await make_scene(ed, "LegacyFork")
    sun = await sun_of(ed, scene)
    await light_set(ed, scene, sun, shadows=True, direction=[-0.3, -0.85, -0.4])
    ground = await entity(ed, scene, "plane", "Ground", [0, 0, 0])
    box = await entity(ed, scene, "cube", "Box", [0, 1.5, 0], scale=[2, 0.3, 2])
    await ed.call("set_properties", scene=scene, entity=ground, values={"mesh": {"submeshes[0].material.shader": "shaders/tinted"}})
    checked = await ed.call("check_shader", shader="shaders/tinted", timeout=300)
    ed.check_eq(checked["failed"], 0, f"the legacy fork compiles: {checked}")
    ed.check("legacy shadow interface" in str(checked), f"check_shader names the legacy interface: {str(checked)[:600]}")
    await aim(ed, scene, [0, 9, 0.01], [0, 0, 0])
    with_box = await shot(ed, scene, frames=10)
    await ed.call("set_entity_state", scene=scene, entity=box, active=False)
    without = await shot(ed, scene, frames=6)
    keep_shot("legacy_with_box", with_box)
    keep_shot("legacy_without_box", without)
    # the ground beside the box (it hides the ground under itself, seen from above): darker
    # where its shadow falls, off its -x and -z sides, the way this sun points
    w, h, _ = with_box
    def luma(img, x, y):
        _, _, rgba = img
        i = (y * w + x) * 4
        return (rgba[i] * 30 + rgba[i + 1] * 59 + rgba[i + 2] * 11) // 100
    darker = sum(1 for y in range(0, h, 2) for x in range(0, w, 2) if luma(without, x, y) - luma(with_box, x, y) > 24)
    ed.check(darker > 200, f"the fork shows the box's directional shadow ({darker} darker pixels)")


# ------------------------------------------------------------------ cross-compile (AC7)

async def test_shader_cross_compile():
    """The Lit variants with and without the High filters ('Shq') on every backend: the HLSL keeps
    its shadow loops as loops (explicit LOD 0 comparisons: SampleCmpLevelZero in [loop], no forced
    [unroll]), Metal compares at level(0), and only the High variants declare the raw-depth view."""
    out = Path(tempfile.mkdtemp(prefix="jasper-shadow-shaders-"))
    try:
        specs = {"high": "mesh:Puc,Shw,Shq,Nor,Tan", "medium": "mesh:Puc,Shw,Nor,Tan",
                 "high_terrain": "mesh:Puc,Shw,Shq,Nor,Ter,Fog", "high_skinned": "mesh:Puc,Shw,Shq,Nor,Ski"}
        args = ["shaders", "--out", str(out), "--format", "json", "--backend", "all"]
        for spec in specs.values():
            args += ["--shader", spec]
        r = subprocess.run(harness.editor_command() + args, capture_output=True, text=True, timeout=1500)
        check(r.returncode == 0, f"the shadow variants compile on every backend: {r.stdout[-3000:]}{r.stderr[-3000:]}")

        def fragment(name: str, lang: str) -> str:
            matches = sorted(out.glob(f"*_{lang}.json"))
            props = specs[name].split(":")[1].split(",")
            for path in matches:
                stem = path.stem
                if all(f"{p}" in stem for p in props) and (("Shq" in stem) == ("Shq" in props)) and \
                        (("Ter" in stem) == ("Ter" in props)) and (("Ski" in stem) == ("Ski" in props)):
                    data = json.loads(path.read_text())
                    stages = data.get("stages", [])
                    frag = [st for st in stages if "frag" in str(st.get("type", "")).lower() or st.get("name", "").endswith("frag")]
                    return (frag or stages)[-1]["source"]
            check(False, f"no {lang} output of {specs[name]} in {[p.name for p in matches]}")
            return ""

        for name in ("high", "high_terrain", "high_skinned"):
            hlsl = fragment(name, "hlsl5")
            check("SampleCmpLevelZero" in hlsl, f"{name}: HLSL compares at LOD 0")
            check("[loop]" in hlsl, f"{name}: HLSL keeps the shadow loops as loops")
            check("[unroll]" not in hlsl, f"{name}: no loop is forced to unroll")
            for lang in ("msl21macos", "msl21ios"):
                msl = fragment(name, lang)
                check("sample_compare" in msl and "level(0" in msl, f"{name}: {lang} compares at level(0)")
        check("u_shadowAtlasRaw" in fragment("high", "glsl410"), "a High variant reads the raw depths (PCSS)")
        check("u_shadowAtlasRaw" not in fragment("medium", "glsl410"), "a Medium variant has no raw-depth view")
        check("u_shadowAtlasRaw" in fragment("high", "glsl300es"), "GLES3 has a texture unit left for it (PCSS)")
    finally:
        shutil.rmtree(out, ignore_errors=True)
