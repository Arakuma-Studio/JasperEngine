"""3D shadows (spec S10, docs/rendering/shadows.md): settings round trip through MCP, save, reload
and Lua (AC17); the shadow cache's paths seen through get_shadow_stats (AC11, AC12, AC13); the
stable cascades' shimmer metric under a moving and a turning fit camera (AC2, AC3); the cascade
blend and distance fade (AC4); acne and contact (AC6); point light faces (AC9); one golden image
per quality tier (AC5); a scene saved before the shadows (AC18).

The scenes are built here with the tools (the Scene view with the shadow debug views and its
gizmos hidden). JASPER_SHADOW_UPDATE_GOLDEN=1 rewrites the goldens of tools/fixtures/shadows/golden."""

from __future__ import annotations

import math
import os
import shutil
import sys
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
SHOT_WIDTH = 640
DISPLAY = {"show_grid_3d": False, "hide_light_icons": True, "hide_selection_outline": True,
           "hide_camera_view": True, "hide_sound_icons": True}


# ------------------------------------------------------------------ helpers

async def make_scene(ed: Editor, name: str) -> int:
    made = await ed.call("create_scene", name=name, type="3D")
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


async def aim(ed: Editor, scene: int, position, target):
    await ed.call("set_scene_camera", scene=scene, position=position, target=target, display=DISPLAY)


async def debug_view(ed: Editor, scene: int, view: str):
    await ed.call("set_scene_settings", scene=scene, settings={"shadows_debug_view": view})


async def shot(ed: Editor, scene: int, frames: int = 4) -> tuple:
    await ed.call("wait", frames=frames)
    r = await ed.call_raw("screenshot", {"view": "scene", "scene": scene, "max_width": SHOT_WIDTH}, timeout=300)
    ed.check(not r.is_error and r.images, f"no scene screenshot: {r.text[:300]}")
    return pngdiff.decode(r.images[0][0])


async def stats(ed: Editor, scene: int) -> dict:
    return await ed.call("get_shadow_stats", scene=scene)


def grey(img, x: int, y: int) -> int:
    w, h, rgba = img
    i = (y * w + x) * 4
    return rgba[i + 1]   # the debug views are grey: green is the value


def changed_fraction(a, b, threshold: int = 2) -> float:
    wa, ha, ra = a
    wb, hb, rb = b
    assert (wa, ha) == (wb, hb)
    changed = 0
    for i in range(0, wa * ha * 4, 4):
        if max(abs(ra[i] - rb[i]), abs(ra[i + 1] - rb[i + 1]), abs(ra[i + 2] - rb[i + 2])) > threshold:
            changed += 1
    return changed / float(wa * ha)


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
    opened = await ed.call("open_scene", path="scenes/ShadowSettings.scene")
    scene = opened["scene"]["id"]
    sun = await sun_of(ed, scene)
    lua = await ed.call("eval_lua", scene=scene, allow_edit_mode=True, code=(
        "local l = object(" + str(sun) + ", 'Light') "
        "local m = object('Crate', 'Mesh') "
        "return {type = l.shadowType, strength = l.shadowStrength, bias = l.shadowBias, normal = l.shadowNormalBias, "
        "res = l.shadowResolution, cascades = l.numCascades, mode = l.cascadeSplitMode, split = l.cascadeSplits.y, "
        "dist = l.shadowDistance, angle = l.shadowAngularDiameter, radius = l.shadowSourceRadius, update = l.shadowUpdateMode, "
        "static = m.staticShadowCaster, quality = scene.shadowQuality, sdist = scene.shadowDistance, "
        "fade = scene.shadowFadeFraction, blend = scene.shadowCascadeBlend, HARD = ShadowType.HARD, MANUAL = CascadeSplitMode.MANUAL, "
        "ON_DEMAND = ShadowUpdateMode.ON_DEMAND, ULTRA = ShadowQuality.ULTRA}"))
    v = lua["value"]
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
    ed.check_eq(lua["value"]["t"], lua["value"]["L"], "Lua effectiveShadowTier")

    # undo reverts a light field (one step)
    await ed.call("set_properties", scene=scene, entity=sun, values={"light": {"shadowBias": 3.0}})
    await ed.call("undo")
    got = await ed.call("get_entity", scene=scene, entity=sun, components=["light"])
    ed.check(abs(harness.find_key(got, "shadowBias") - 1.5) < 1e-5, "undo reverts the bias")


# ------------------------------------------------------------------ cache (AC10-AC13)

async def cache_scene(ed: Editor, name: str):
    scene = await make_scene(ed, name)
    sun = await sun_of(ed, scene)
    await light_set(ed, scene, sun, shadows=True)
    ground = await entity(ed, scene, "plane", "Ground", [0, 0, 0], scale=[6, 1, 6], values={"mesh": {"staticShadowCaster": True}})
    static_box = await entity(ed, scene, "cube", "StaticBox", [-3, 0.5, 0], values={"mesh": {"staticShadowCaster": True}})
    dynamic_box = await entity(ed, scene, "cube", "DynamicBox", [3, 0.5, 0])
    spot = await entity(ed, scene, "spot_light", "Spot", [0, 6, 4])
    await light_set(ed, scene, spot, shadows=True, range=20.0)
    await ed.call("set_transform", scene=scene, entity=spot, rotation={"euler": [-70, 0, 0]})
    await ed.call("set_scene_settings", scene=scene, settings={"shadows_quality": "medium"})
    await aim(ed, scene, [0, 9, 14], [0, 0, 0])
    return scene, sun, ground, static_box, dynamic_box, spot


async def test_shadow_cache(ed: Editor):
    scene, sun, ground, static_box, dynamic_box, spot = await cache_scene(ed, "ShadowCache")

    # still: every slot skips and nothing is drawn
    st = await settle(ed, scene, frames=10)
    st = await settle(ed, scene, frames=3)
    slots = slots_of(st)
    ed.check(len(slots) >= 2, f"the sun and the spot have slots: {st['lights']}")
    ed.check(all(s["path"] == "skip" for s in slots), f"a still scene skips every slot: {[(s['slot'], s['path'], s['dirty_cause']) for s in slots]}")
    ed.check_eq(st["draw_calls_total"], 0, "a still scene draws nothing into its shadow maps")
    ed.check_eq(st["passes"], 0, "nor begins a shadow pass")

    # a dynamic caster moves: its slots restore their static layer under it, within its rects
    await ed.call("set_transform", scene=scene, entity=dynamic_box, position=[3.5, 0.5, 0.5])
    await ed.call("wait", frames=1)
    st = await stats(ed, scene)
    paths = {(s["atlas"], s["slot"]): s for s in slots_of(st)}
    touched = [s for s in paths.values() if s["path"] != "skip"]
    ed.check(touched, "the move redrew something")
    for s in touched:
        ed.check(s["path"] in ("restore", "full"), f"a dynamic move restores or redraws: {s}")
        ed.check_eq(s["dirty_cause"], "dynamic_caster")
        if s["path"] == "restore":
            r, d = s["rect"], s["dirty_rect"]
            ed.check(d[0] >= r[0] and d[1] >= r[1] and d[0] + d[2] <= r[0] + r[2] and d[1] + d[3] <= r[1] + r[3] and d[2] < r[2],
                     f"the dirty rect lies inside the slot and is smaller: {s}")
    st = await settle(ed, scene, frames=3)
    ed.check(all(s["path"] == "skip" for s in slots_of(st)), "and the cache settles again")

    # a static caster moves: the static layer of the slots it touches is redrawn
    await ed.call("set_transform", scene=scene, entity=static_box, position=[-3.5, 0.5, 0])
    await ed.call("wait", frames=1)
    st = await stats(ed, scene)
    touched = [s for s in slots_of(st) if s["path"] != "skip"]
    ed.check(touched and all(s["path"] in ("static", "full") and s["dirty_cause"] == "static_caster" for s in touched),
             f"a static move redraws the static layers it touches: {touched}")
    await settle(ed, scene, frames=3)

    # the image is the same whether the slots were cached or redrawn
    await debug_view(ed, scene, "shadow_factor")
    cached = await shot(ed, scene)
    await ed.call("set_shadow_debug", scene=scene, force_rerender=True)
    forced = await shot(ed, scene)
    await ed.call("set_shadow_debug", scene=scene, force_rerender=False)
    ed.check_eq(changed_fraction(cached, forced, 0), 0.0, "cached and re-rendered shadows are identical")
    await debug_view(ed, scene, "none")


async def test_shadow_invalidation(ed: Editor):
    """Each R18 trigger, once: the next frame's cause on the slots, and the image equal to a forced re-render."""
    scene, sun, ground, static_box, dynamic_box, spot = await cache_scene(ed, "ShadowInvalidation")
    await settle(ed, scene, frames=10)
    await debug_view(ed, scene, "shadow_factor")

    async def expect(change, causes: set, label: str):
        await settle(ed, scene, frames=4)
        await change()
        await ed.call("wait", frames=1)
        st = await stats(ed, scene)
        touched = [s for s in slots_of(st) if s["path"] != "skip"]
        ed.check(touched, f"{label}: something was redrawn")
        ed.check(all(s["dirty_cause"] in causes for s in touched), f"{label}: causes {[s['dirty_cause'] for s in touched]} in {causes}")
        cached = await shot(ed, scene, frames=2)
        await ed.call("set_shadow_debug", scene=scene, force_rerender=True)
        forced = await shot(ed, scene, frames=2)
        await ed.call("set_shadow_debug", scene=scene, force_rerender=False)
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
    await expect(lambda: ed.call("set_transform", scene=scene, entity=sun, rotation={"euler": [-60, 20, 0]}), {"light", "fit"}, "light direction")
    await expect(lambda: light_set(ed, scene, spot, cullingMask=1), {"light"}, "culling mask")
    await expect(lambda: ed.call("delete_entities", scene=scene, entities=[static_box]), {"static_caster"}, "removed")

    # receiver-only settings redraw nothing
    await settle(ed, scene, frames=4)
    await light_set(ed, scene, sun, shadowBias=2.0, shadowNormalBias=2.0, shadowStrength=0.5, softShadows=False)
    await ed.call("wait", frames=1)
    st = await stats(ed, scene)
    ed.check(all(s["path"] == "skip" for s in slots_of(st)), f"receiver-only edits skip: {[s['dirty_cause'] for s in slots_of(st)]}")
    await debug_view(ed, scene, "none")


async def test_on_demand(ed: Editor):
    scene, sun, ground, static_box, dynamic_box, spot = await cache_scene(ed, "ShadowOnDemand")
    await light_set(ed, scene, sun, shadowUpdateMode="on_demand")
    await light_set(ed, scene, spot, shadowUpdateMode="on_demand")
    await debug_view(ed, scene, "shadow_factor")
    before = await shot(ed, scene, frames=8)
    await ed.call("set_transform", scene=scene, entity=dynamic_box, position=[0, 0.5, 2])
    held = await shot(ed, scene, frames=6)
    st = await stats(ed, scene)
    ed.check(all(s["path"] == "skip" for s in slots_of(st)), "On Demand: a moving caster redraws nothing")
    ed.check(changed_fraction(before, held, 8) > 0.0005, "the cube itself moved")
    # the shadow is where it was: compare a view of the shadows only, not of the cube (cube pixels differ)
    await ed.call("eval_lua", scene=scene, allow_edit_mode=True,
                  code="object('Sun', 'Light'):requestShadowUpdate(); object('Spot', 'Light'):requestShadowUpdate(); return true")
    await ed.call("wait", frames=1)
    st = await stats(ed, scene)
    ed.check(any(s["path"] != "skip" and s["dirty_cause"] == "forced" for s in slots_of(st)), "requestShadowUpdate redraws")
    updated = await shot(ed, scene, frames=3)
    ed.check(changed_fraction(held, updated, 8) > 0.0005, "then the shadows follow")
    await debug_view(ed, scene, "none")


# ------------------------------------------------------------------ stability (AC2, AC3)

async def stability_scene(ed: Editor, name: str):
    scene = await make_scene(ed, name)
    sun = await sun_of(ed, scene)
    await light_set(ed, scene, sun, shadows=True, softShadows=True)
    await ed.call("set_transform", scene=scene, entity=sun, rotation={"euler": [-50, 35, 0]})
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
    got = await ed.call("get_entity", scene=scene, entity=sun, components=["light"])
    direction = normalize(harness.find_key(got, "worldDirection"))
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
        worst = max(worst, changed_fraction(previous, current))
        previous = current
    await ed.call("set_shadow_debug", scene=scene, fit_camera_offset=[0, 0, 0])
    print(f"     [shimmer] translate: worst {worst * 100:.4f}% of the pixels changed by more than 2")
    ed.check(worst <= 0.0005, f"translation shimmer: {worst * 100:.4f}% of the pixels changed (at most 0.05%)")
    await debug_view(ed, scene, "none")


async def test_no_shimmer_rotate(ed: Editor):
    scene, sun, sun_stats = await stability_scene(ed, "StabilityRotate")
    previous = await shot(ed, scene)
    worst = 0.0
    for k in range(1, 33):
        await ed.call("set_shadow_debug", scene=scene, fit_camera_euler=[0.0, 0.25 * k, 0.0])
        current = await shot(ed, scene, frames=2)
        worst = max(worst, changed_fraction(previous, current))
        previous = current
    await ed.call("set_shadow_debug", scene=scene, fit_camera_euler=[0, 0, 0])
    st = await stats(ed, scene)
    radius = [light for light in st["lights"] if light["type"] == "directional"][0]["radius"]
    ed.check_eq(radius, sun_stats["radius"], "turning keeps the cascade extents")
    print(f"     [shimmer] rotate: worst {worst * 100:.4f}% of the pixels changed by more than 2")
    ed.check(worst <= 0.0005, f"rotation shimmer: {worst * 100:.4f}% of the pixels changed (at most 0.05%)")
    await debug_view(ed, scene, "none")


# ------------------------------------------------------------------ blend and fade (AC4)

async def test_cascade_blend_fade(ed: Editor):
    scene = await make_scene(ed, "BlendFade")
    sun = await sun_of(ed, scene)
    await light_set(ed, scene, sun, shadows=True, shadowDistance=60.0, numShadowCascades=4)
    await ed.call("set_transform", scene=scene, entity=sun, rotation={"euler": [-75, 0, 0]})
    await entity(ed, scene, "plane", "Long", [0, 0, -50], scale=[4, 1, 12])
    # a long roof over the centre line: it is in shadow from near to far, so a seam would show
    await entity(ed, scene, "cube", "Roof", [0, 2.5, -50], scale=[2.0, 0.2, 120.0])
    await ed.call("set_scene_settings", scene=scene, settings={"shadows_quality": "high", "shadow_fade_fraction": 0.1})
    await aim(ed, scene, [0, 3.0, 8], [0, 0.6, -40])

    await debug_view(ed, scene, "cascades")
    cascades = await shot(ed, scene, frames=10)
    await debug_view(ed, scene, "shadow_factor")
    factor = await shot(ed, scene)
    await debug_view(ed, scene, "none")

    w, h, rgba = cascades
    cx = w // 2
    # rows where the cascade tint changes along the centre column (top = far)
    def tint(y):
        i = (y * w + cx) * 4
        r, g, b = rgba[i], rgba[i + 1], rgba[i + 2]
        return max(range(3), key=lambda k: (r, g, b)[k])
    boundaries = [y for y in range(1, h) if tint(y) != tint(y - 1)]
    ed.check(boundaries, "the cascades show along the plane")
    for y in boundaries:
        for yy in range(max(1, y - 3), min(h, y + 4)):
            step = abs(grey(factor, cx, yy) - grey(factor, cx, yy - 1))
            ed.check(step <= 8, f"no seam at the split near row {y}: step {step} at row {yy}")
    # beyond the shadow distance: exactly lit (the far end of the plane, top rows that show it)
    far_rows = [y for y in range(h // 2) if grey(factor, cx, y) > 0 and rgba[(y * w + cx) * 4 + 3] > 0]
    ed.check(far_rows and grey(factor, cx, far_rows[0]) == 255, "past the shadow distance the plane is lit (255)")
    await golden(ed, "cascades", cascades)


# ------------------------------------------------------------------ bias (AC6)

async def test_bias(ed: Editor):
    scene = await make_scene(ed, "Bias")
    sun = await sun_of(ed, scene)
    await light_set(ed, scene, sun, shadows=True)
    await ed.call("set_transform", scene=scene, entity=sun, rotation={"euler": [-45, 0, 0]})
    await ed.call("set_scene_settings", scene=scene, settings={"shadows_quality": "high"})
    plane = await entity(ed, scene, "plane", "Tilted", [0, 0, 0], scale=[3, 1, 3])
    blocker = await entity(ed, scene, "cube", "Blocker", [-4, 2, 0], scale=[6, 0.1, 12])
    await aim(ed, scene, [0, 14, 0.01], [0, 0, 0])
    await debug_view(ed, scene, "shadow_factor")
    for angle in (1, 15, 30, 45, 60, 80):
        await ed.call("set_transform", scene=scene, entity=plane, rotation={"euler": [0, 0, -angle]})
        img = await shot(ed, scene, frames=6)
        w, h, rgba = img
        lit = shadowed = 0
        lit_full = 0
        values = []
        # the plane's right half (x > 0) is lit, the left half under the blocker
        for y in range(h // 4, 3 * h // 4, 2):
            for x in range(w // 2 + w // 16, w // 2 + w // 5, 2):
                lit += 1
                lit_full += 1 if grey(img, x, y) == 255 else 0
            for x in range(w // 2 - w // 5, w // 2 - w // 16, 2):
                shadowed += 1
                values.append(grey(img, x, y))
        mean = sum(values) / max(len(values), 1)
        variance = sum((v - mean) ** 2 for v in values) / max(len(values), 1)
        ed.check(lit_full / max(lit, 1) >= 0.995, f"{angle} deg: {lit_full}/{lit} lit pixels at 255 (no acne)")
        ed.check(variance < 1.0, f"{angle} deg: the shadowed half is even (variance {variance:.2f})")
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


# ------------------------------------------------------------------ goldens per tier (AC5, AC8)

async def golden(ed: Editor, name: str, img):
    path = GOLDEN_DIR / f"{name}.png"
    if UPDATE_GOLDEN:
        GOLDEN_DIR.mkdir(parents=True, exist_ok=True)
        path.write_bytes(pngdiff.encode(img))
        print(f"     [golden] wrote {path.relative_to(harness.REPO)}: look at it")
        return
    ed.check(path.is_file(), f"no golden {path} (JASPER_SHADOW_UPDATE_GOLDEN=1 writes it)")
    ok, fraction, diff = pngdiff.compare(img, pngdiff.load(path), 24, 0.01)
    if not ok:
        actual = ed.scratch.root / f"{name}.actual.png"
        actual.write_bytes(pngdiff.encode(img))
        (ed.scratch.root / f"{name}.diff.png").write_bytes(diff or b"")
        ed.keep = True
    ed.check(ok, f"{name}: {fraction * 100:.2f}% of the pixels differ from the golden (at most 1%)")


async def test_shadow_goldens(ed: Editor):
    scene = await make_scene(ed, "Filters")
    sun = await sun_of(ed, scene)
    await light_set(ed, scene, sun, shadows=True, softShadows=True, shadowAngularDiameter=3.0)
    await ed.call("set_transform", scene=scene, entity=sun, rotation={"euler": [-55, 30, 0]})
    await entity(ed, scene, "plane", "Ground", [0, 0, 0], scale=[3, 1, 3])
    # a contact-hardening pole, a hard and a soft spot, a point light
    await entity(ed, scene, "cylinder", "Pole", [0, 1.5, 0], scale=[0.15, 3.0, 0.15])
    await entity(ed, scene, "cube", "Box", [-4, 0.5, 2])
    hard = await entity(ed, scene, "spot_light", "HardSpot", [-4, 5, 4])
    await light_set(ed, scene, hard, shadows=True, softShadows=False, range=12.0, intensity=60.0)
    await ed.call("set_transform", scene=scene, entity=hard, rotation={"euler": [-60, 0, 0]})
    soft = await entity(ed, scene, "spot_light", "SoftSpot", [4, 5, 4])
    await light_set(ed, scene, soft, shadows=True, softShadows=True, range=12.0, intensity=60.0, shadowSourceRadius=0.3)
    await ed.call("set_transform", scene=scene, entity=soft, rotation={"euler": [-60, 0, 0]})
    await entity(ed, scene, "sphere", "Ball", [4, 0.6, 2], scale=[1.2, 1.2, 1.2])
    point = await entity(ed, scene, "point_light", "Lamp", [0, 2.5, -4])
    await light_set(ed, scene, point, shadows=True, range=10.0, intensity=25.0)
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


# ------------------------------------------------------------------ old scenes (AC18)

async def test_shadow_migration(ed: Editor):
    fixture = harness.REPO / "tools" / "fixtures" / "shadows"
    shutil.copy(fixture / "Legacy.scene", ed.project / "Legacy.scene")
    scripts = ed.project / "scripts"
    scripts.mkdir(exist_ok=True)
    shutil.copy(fixture / "scripts" / "Legacy.lua", scripts / "Legacy.lua")
    opened = await ed.call("open_scene", path="Legacy.scene")
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
