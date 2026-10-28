"""Rendering performance fixes of the upstream catch-up, in a new project's 3D scene:

- the directional light's cascades leave out the casters that cannot shadow what the camera sees,
  but keep a caster outside the view whose shadow falls into it (a slab above the view);
- a reflection probe's runtime capture is prefiltered into roughness mips (the Probe Filter
  variant is in the build's shaders, the picture with the probe, SSR and shadows has no black or
  NaN-coloured pixels and the console no errors);
- the variants these change (probefilter, ssr, Lit with shadows and IBL) compile for every
  graphics API.

Screenshots are kept in the scratch directory (shots/)."""

from __future__ import annotations

import sys
from pathlib import Path

import harness
from harness import Editor

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE.parent))
import pngdiff  # noqa: E402

EDITOR = {"create": True}
CI = True
TIMEOUT = 900

S: dict = {}
M = "submeshes[0].material."


def shots(ed: Editor) -> Path:
    return (ed.scratch.root if ed.scratch else Path("/tmp/render-perf")) / "shots"


def stats(png: bytes):
    """the share of black and of saturated-magenta (NaN-like) pixels"""
    width, height, rgba = pngdiff.decode(png)
    total, black, magenta = 0, 0, 0
    for y in range(0, height, 2):
        row = y * width * 4
        for x in range(0, width, 2):
            r, g, b = rgba[row + x * 4], rgba[row + x * 4 + 1], rgba[row + x * 4 + 2]
            total += 1
            if r < 4 and g < 4 and b < 4:
                black += 1
            if r > 250 and g < 5 and b > 250:
                magenta += 1
    return black / total, magenta / total


def mean_luma(png: bytes, x0: float, y0: float, x1: float, y1: float) -> float:
    """mean luma of a region given in fractions of the picture"""
    width, height, rgba = pngdiff.decode(png)
    total, count = 0.0, 0
    for y in range(int(height * y0), int(height * y1)):
        row = y * width * 4
        for x in range(int(width * x0), int(width * x1)):
            r, g, b = rgba[row + x * 4], rgba[row + x * 4 + 1], rgba[row + x * 4 + 2]
            total += 0.299 * r + 0.587 * g + 0.114 * b
            count += 1
    return total / max(count, 1)


async def errors(ed: Editor):
    return [e for e in await ed.console_since(None) if e.get("type") == "error"]


async def test_scene(ed: Editor):
    sid = (await ed.call("create_scene", name="RenderPerf", type="3D", path="scenes/RenderPerf"))["scene"]["id"]
    S["scene"] = sid
    await ed.call("create_entity", scene=sid, type="plane", name="Ground", scale=[30, 1, 30],
                  values={"mesh": {M + "baseColorFactor": [0.7, 0.7, 0.7, 1], M + "roughnessFactor": 0.3,
                                   "receiveIBL": True}})
    # a smooth and a rough metal sphere reflecting the probe, a red box between them
    await ed.call("create_entity", scene=sid, type="sphere", name="Smooth", position=[-1.6, 1, 0],
                  values={"mesh": {M + "metallicFactor": 1.0, M + "roughnessFactor": 0.05, "receiveIBL": True}})
    await ed.call("create_entity", scene=sid, type="sphere", name="Rough", position=[1.6, 1, 0],
                  values={"mesh": {M + "metallicFactor": 1.0, M + "roughnessFactor": 0.7, "receiveIBL": True}})
    await ed.call("create_entity", scene=sid, type="cube", name="Box", position=[0, 0.5, -2.5],
                  values={"mesh": {M + "baseColorFactor": [0.9, 0.15, 0.1, 1]}})
    # the camera looks down at the ground around the origin; the slab is high above its view
    await ed.call("create_entity", scene=sid, type="camera", name="Main Camera", position=[0, 3, 8],
                  rotation={"euler": [-15, 0, 0]})
    await ed.call("set_main_camera", scene=sid, entity="Main Camera")
    await ed.call("create_entity", scene=sid, type="cube", name="Slab", position=[0, 14, 1.5], scale=[3, 0.4, 3])
    # the new scene's sun (shadows on) travels down, slightly sideways: the slab's shadow falls on
    # the ground in view
    await ed.call("set_properties", scene=sid, entity="Directional Light",
                  values={"light": {"direction": [0.1, -1.0, 0.05], "shadows": True}})
    await ed.call("create_entity", scene=sid, type="reflection_probe", name="Probe", position=[0, 1, 0],
                  values={"reflectionprobe": {"boxSize": [30, 10, 30]}})
    await ed.call("set_scene_settings", scene=sid, settings={"shadows_quality": "high"})
    await ed.call("configure_game_view", aspect="16:9")
    await ed.call("wait", frames=30)
    S["base"] = await ed.screenshot(view="game", path=shots(ed) / "base.png", max_width=960)
    ed.check(not await errors(ed), "no console errors building the scene")


async def test_offview_caster_still_shadows_the_view(ed: Editor):
    """The slab is outside the view; its shadow is in it, so the cascades keep it"""
    sid = S["scene"]
    lit = S["base"]
    await ed.call("set_properties", scene=sid, entity="Slab", values={"mesh": {"castShadows": False}})
    await ed.call("wait", frames=10)
    unshadowed = await ed.screenshot(view="game", path=shots(ed) / "slab-no-shadow.png", max_width=960)
    # the slab itself is not in the picture: moved away (casting nothing) it looks the same
    await ed.call("set_transform", scene=sid, entity="Slab", position=[0, 60, 1.5])
    await ed.call("wait", frames=10)
    away = await ed.screenshot(view="game", path=shots(ed) / "slab-away.png", max_width=960)
    ok, fraction, _ = pngdiff.compare(pngdiff.decode(unshadowed), pngdiff.decode(away), channel_tolerance=8, max_fraction=0.002)
    ed.check(ok, f"the slab is out of the camera's view ({fraction:.4f} of the pixels change when it moves)")
    await ed.call("set_transform", scene=sid, entity="Slab", position=[0, 14, 1.5])
    await ed.call("set_properties", scene=sid, entity="Slab", values={"mesh": {"castShadows": True}})
    await ed.call("wait", frames=10)

    # its shadow: the ground under it is darker with it casting
    ok, fraction, _ = pngdiff.compare(pngdiff.decode(lit), pngdiff.decode(unshadowed), channel_tolerance=24, max_fraction=0.01)
    ed.check(not ok and fraction > 0.004, f"the slab's shadow is in the picture ({fraction:.4f} of the pixels)")
    # where it falls: the lower right of the picture (the probe and sky keep it a soft one)
    with_shadow = mean_luma(lit, 0.52, 0.7, 0.86, 0.94)
    without = mean_luma(unshadowed, 0.52, 0.7, 0.86, 0.94)
    ed.check(with_shadow < without - 3, f"the ground in front is darker in the slab's shadow ({with_shadow:.1f} vs {without:.1f})")


async def test_probe_ssr_and_shadows_render(ed: Editor):
    sid = S["scene"]
    names = {s["name"] for s in (await ed.call("get_build_info", include_shaders=True))["shaders"]}
    ed.check("Probe Filter" in names, f"the probe filter ships with a probe: {sorted(names)}")

    await ed.call("set_scene_settings", scene=sid, settings={"ssr_enabled": True})
    await ed.call("wait", frames=30)
    ssr = await ed.screenshot(view="game", path=shots(ed) / "ssr.png", max_width=960)
    for tag, png in (("probe and shadows", S["base"]), ("with SSR", ssr)):
        black, magenta = stats(png)
        ed.check(black < 0.02, f"{tag}: no black areas ({black:.4f})")
        ed.check(magenta == 0, f"{tag}: no NaN-coloured pixels")
    # the probe reflects the scene: the smooth sphere is not the empty black cube
    ed.check(mean_luma(S["base"], 0.25, 0.35, 0.42, 0.7) > 20, "the smooth sphere reflects the probe")
    await ed.call("set_scene_settings", scene=sid, settings={"ssr_enabled": False})
    ed.check(not await errors(ed), f"console errors: {(await errors(ed))[:5]!r}")


async def test_variants_compile_for_every_api(ed: Editor):
    out = ed.scratch.root / "render-perf-shaders"
    specs = ["probefilter", "ssr", "mesh:Uv1,Puc,Shw,Nor", "mesh:Uv1,Puc,Shw,Nor,Ibl"]
    r = await ed.call("generate_shaders", output_dir=str(out), backends="all", format="binary",
                      shaders=specs, timeout_seconds=900)
    ed.check_eq(r["status"], "succeeded", f"generate_shaders: {r}")
    files = {p.name for p in out.rglob("*") if p.is_file()}
    for stem in ("probefilter", "ssr", "mesh_Uv1PucShwNor", "mesh_Uv1PucShwNorIbl"):
        for language in ("glsl410", "glsl300es", "hlsl5", "msl21macos", "msl21ios", "spirv10"):
            ed.check(f"{stem}_{language}.sdat" in files, f"{stem}_{language}.sdat not generated: {sorted(files)}")
