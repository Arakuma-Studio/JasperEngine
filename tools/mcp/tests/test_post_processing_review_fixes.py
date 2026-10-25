"""Shader library phase 4, review fixes of the editor side (sl/p4-fix-editor):

- renaming, moving or deleting the post-processing LUT image rewrites (or clears) lut_texture, as
  it does for the user passes' textures;
- get_scene_settings post_process gives a pass's textures as material shader textures are given
  (sampler settings and cube maps survive a read-modify-write);
- set_scene_settings refuses out-of-range Vector3 fields (instead of clamping them), takes enums by
  name (any case) or index, refuses unknown keys in post_process items and library pass uniforms
  the shader's manifest does not declare, and refuses post_processing on UI scenes;
- the Stylised preset's lift is warm;
- the cost estimate counts MSAA's resolve blit (exported, nothing else offscreen), the blur chain's
  1 + 2 x levels passes, a user pass's scale and the end of the user chain (copy or blit);
- an image of the post-processing changed on disk (the LUT strip, a pass texture) is picked up:
  the Game view shows the new one."""

from __future__ import annotations

import struct
import sys
import os
import zlib
from pathlib import Path

from harness import Editor

sys.path.insert(0, str(Path(__file__).parent))
from test_runtime_regressions import decode_png  # noqa: E402

EDITOR = {"create": True, "name": "Post Processing Review Fixes"}
TIMEOUT = 600

STATE: dict = {}


def write_png(path: Path, width: int, height: int, rgba: tuple) -> None:
    """A one-colour RGBA PNG (stdlib only); the mtime surely differs from a previous write."""
    raw = b"".join(b"\x00" + bytes(rgba) * width for _ in range(height))

    def chunk(kind, data):
        return struct.pack(">I", len(data)) + kind + data + struct.pack(">I", zlib.crc32(kind + data) & 0xFFFFFFFF)

    png = b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", width, height, 8, 6, 0, 0, 0))
    png += chunk(b"IDAT", zlib.compress(raw)) + chunk(b"IEND", b"")
    previous_mtime = path.stat().st_mtime_ns if path.exists() else None
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_bytes(png)
    if previous_mtime is not None:
        stamp = path.stat()
        os.utime(path, ns=(stamp.st_atime_ns, max(stamp.st_mtime_ns, previous_mtime + 1_000_000_000)))


def mean_rgb(png: bytes) -> tuple:
    w, h, c, rows = decode_png(png)
    total = [0, 0, 0]
    count = 0
    for y in range(0, h, 4):
        for x in range(0, w, 4):
            for k in range(3):
                total[k] += rows[y][x * c + k]
            count += 1
    return tuple(t / max(count, 1) for t in total)


async def shot(ed: Editor) -> bytes:
    await ed.call("wait", frames=20)
    r = await ed.call_raw("screenshot", {"view": "game", "scene": STATE["sid"]}, timeout=300)
    ed.check(not r.is_error and r.images, f"no game screenshot: {r.text[:300]}")
    return r.images[0][0]


async def pp(ed: Editor, sid: int, name: str):
    return (await ed.call("get_scene_settings", scene=sid, names=[name]))["settings"][name]


async def setup(ed: Editor):
    ed.require_tools("create_scene", "create_entity", "save_scene", "set_scene_settings", "get_scene_settings")
    made = await ed.call("create_scene", name="ReviewFixes", type="3d")
    STATE["sid"] = made["scene"]["id"]
    await ed.call("save_scene", scene=STATE["sid"], path="scenes/ReviewFixes.scene")
    await ed.call("create_entity", scene=STATE["sid"], name="Ball", type="sphere")


async def test_lut_follows_rename_and_delete(ed: Editor):
    await setup(ed)
    ed.require_tools("move_asset", "delete_asset")
    sid = STATE["sid"]
    write_png(ed.project / "assets" / "luts" / "warm.png", 4, 2, (255, 128, 0, 255))
    await ed.call("set_scene_settings", scene=sid, settings={"post_processing": {
        "color_grading_enabled": True, "lut_texture": "assets/luts/warm.png"}})
    ed.check_eq(await pp(ed, sid, "post_processing.lut_texture"), "assets/luts/warm.png")

    await ed.call("move_asset", path="assets/luts/warm.png", to="assets/luts/amber.png")
    ed.check_eq(await pp(ed, sid, "post_processing.lut_texture"), "assets/luts/amber.png", "the LUT follows its image")
    await ed.call("save_scene", scene=sid)
    text = (ed.project / "scenes/ReviewFixes.scene").read_text()
    ed.check("amber.png" in text and "warm.png" not in text, "the scene saves the new LUT path")

    await ed.call("delete_asset", paths=["assets/luts/amber.png"])
    ed.check_eq(await pp(ed, sid, "post_processing.lut_texture"), "", "a deleted LUT image is cleared")
    await ed.call("set_scene_settings", scene=sid, settings={"post_processing": None})


async def test_pass_textures_round_trip(ed: Editor):
    ed.require_tools("fork_shader")
    sid = STATE["sid"]
    write_png(ed.project / "assets" / "textures" / "overlay.png", 4, 4, (0, 255, 0, 255))
    fork = await ed.call("fork_shader", scene=sid, target="post_process", name="ReviewOverlay")
    STATE["fork"] = fork["custom_shader"]
    STATE["fork_frag"] = fork.get("frag_path")
    sampled = {"path": "assets/textures/overlay.png", "min_filter": "nearest", "mag_filter": "nearest"}
    await ed.call("set_scene_settings", scene=sid, settings={"post_process": [
        {"shader": STATE["fork"], "textures": {"u_overlayTexture": sampled, "u_cubeTexture": {"cube": "assets/textures/overlay.png"}}}]})
    first = await pp(ed, sid, "post_process")
    tex = first[0]["textures"]
    ed.check(isinstance(tex["u_overlayTexture"], dict) and tex["u_overlayTexture"].get("min_filter") == "nearest",
             f"the sampler settings are listed: {tex!r}")
    ed.check(isinstance(tex["u_cubeTexture"], dict) and "cube" in tex["u_cubeTexture"], f"the cube map is listed: {tex!r}")
    # read-modify-write: the same chain back
    first[0]["scale"] = 0.5
    await ed.call("set_scene_settings", scene=sid, settings={"post_process": first})
    second = await pp(ed, sid, "post_process")
    ed.check_eq(second[0]["textures"], tex, "a read-modify-write keeps filters and cube maps")
    ed.check_eq(second[0]["scale"], 0.5)
    await ed.call("set_scene_settings", scene=sid, settings={"post_process": []})


async def test_settings_validation(ed: Editor):
    sid = STATE["sid"]
    # Vector3 out of range: refused (not clamped)
    await ed.check_error("set_scene_settings", scene=sid, settings={"post_processing.lift": [2, 0, 0]}, contains="-1.0 to 1.0")
    await ed.check_error("set_scene_settings", scene=sid, settings={"post_processing": {"gain": [1, 1, 9]}}, contains="0.0 to 4.0")
    ed.check_eq(await pp(ed, sid, "post_processing.lift"), [0, 0, 0], "a refused value changes nothing")
    await ed.call("set_scene_settings", scene=sid, settings={"post_processing.lift": [-0.5, 0, 1]})
    ed.check_eq(await pp(ed, sid, "post_processing.lift"), [-0.5, 0, 1], "the range's ends are taken")

    # enums: by name (any case) or index
    await ed.call("set_scene_settings", scene=sid, settings={"post_processing.tonemap": 1})
    ed.check_eq(await pp(ed, sid, "post_processing.tonemap"), "aces", "an enum by index")
    await ed.call("set_scene_settings", scene=sid, settings={"post_processing": {"tonemap": "AgX", "monochrome_mode": "SEPIA"}})
    ed.check_eq(await pp(ed, sid, "post_processing.tonemap"), "agx", "an enum by name, any case")
    ed.check_eq(await pp(ed, sid, "post_processing.monochrome_mode"), "sepia")
    await ed.check_error("set_scene_settings", scene=sid, settings={"post_processing.tonemap": 9}, contains="index 0 to 4")
    await ed.check_error("set_scene_settings", scene=sid, settings={"post_processing.tonemap": "filmic"}, contains="aces")
    await ed.call("set_scene_settings", scene=sid, settings={"post_processing": None})

    # post_process items: unknown keys and undeclared library uniforms refused
    await ed.check_error("set_scene_settings", scene=sid, settings={"post_process": [
        {"shader": "library:crt", "uniform": {"curvature": 0.5}}]}, contains="unknown key \"uniform\"")
    await ed.check_error("set_scene_settings", scene=sid, settings={"post_process": [
        {"shader": "library:crt", "uniforms": {"curvture": 0.5}}]}, contains="no parameter \"curvture\"")
    await ed.check_error("set_scene_settings", scene=sid, settings={"post_process": [
        {"shader": "library:crt", "enabled": "yes"}]}, contains="true or false")
    await ed.call("set_scene_settings", scene=sid, settings={"post_process": [{"shader": "library:crt", "uniforms": {"curvature": 0.5}}]})
    ed.check_eq((await pp(ed, sid, "post_process"))[0]["uniforms"], {"curvature": [0.5, 0.0, 0.0, 0.0]})
    await ed.call("set_scene_settings", scene=sid, settings={"post_process": []})

    # UI scenes are never post-processed: refused as the preset tool does
    ui = await ed.call("create_scene", name="ReviewUi", type="ui")
    uid = ui["scene"]["id"]
    await ed.check_error("set_scene_settings", scene=uid, settings={"post_processing.fxaa_enabled": True}, contains="UI scene")
    await ed.check_error("set_scene_settings", scene=uid, settings={"post_processing": {"fxaa_enabled": True}}, contains="UI scene")
    await ed.call("select_scene", scene=sid)


async def cost(ed: Editor, sid: int, settings: dict | None, passes: list | None = None, fixed: bool = False) -> dict:
    await ed.call("set_scene_settings", scene=sid, settings={
        "post_processing": None, "post_process": passes or [], "fixed_resolution_enabled": fixed})
    if settings:
        await ed.call("set_scene_settings", scene=sid, settings={"post_processing": settings})
    listed = await ed.call("list_post_processing_presets", scene=sid)
    return listed["current"]["cost"]["engine"]


async def test_cost_estimate(ed: Editor):
    sid = STATE["sid"]
    labels = lambda side: [p["label"] for p in side["pass_list"]]  # noqa: E731

    # MSAA alone (exported, nothing offscreen): the resolve blit, one full-resolution pass
    e = await cost(ed, sid, {"msaa_samples": 4})
    ed.check_eq((e["passes"], e["full_resolution_passes"]), (1, 1), f"MSAA alone: {labels(e)}")
    ed.check(any("MSAA" in label for label in labels(e)), f"the MSAA blit is named: {labels(e)}")
    # with the uber pass the resolve is free
    e = await cost(ed, sid, {"msaa_samples": 4, "fxaa_enabled": True})
    ed.check_eq(labels(e), ["uber"], "MSAA with the uber pass adds nothing")
    # with fixed resolution: the blit is drawn anyway
    e = await cost(ed, sid, {"msaa_samples": 2}, fixed=True)
    ed.check_eq(e["passes"], 0, f"MSAA with fixed resolution: {labels(e)}")

    # blur: 1 + 2 x ceil(radius) passes, then the uber pass
    for radius, chain in ((1.0, 3), (1.5, 5), (2.5, 7)):
        e = await cost(ed, sid, {"blur_enabled": True, "blur_radius": radius})
        ed.check_eq(e["passes"], chain + 1, f"blur radius {radius}: {labels(e)}")
    e = await cost(ed, sid, {"bloom_enabled": True})
    ed.check_eq(e["passes"], 8, "bloom: 7 + uber")

    # user passes: each at its scale; without the uber pass the chain ends with a blit to the screen
    e = await cost(ed, sid, None, passes=[{"shader": "library:pixelate", "scale": 0.5}])
    ed.check_eq((e["passes"], e["full_resolution_passes"]), (2, 1), f"a half-scale pass and the blit: {labels(e)}")
    ed.check(abs(e["screens"] - 1.25) < 0.01, f"0.25 + 1 screens: {e['screens']}")
    # with the uber pass the chain's output is its source: no blit, no copy
    e = await cost(ed, sid, {"fxaa_enabled": True}, passes=[{"shader": "library:pixelate", "scale": 0.5}])
    ed.check_eq(e["passes"], 2, f"pass + uber: {labels(e)}")
    ed.check(abs(e["screens"] - 1.25) < 0.01, f"0.25 + 1 screens: {e['screens']}")
    # fixed resolution: a last pass below full size is copied, a full one writes the target
    e = await cost(ed, sid, None, passes=[{"shader": "library:pixelate", "scale": 0.5}], fixed=True)
    ed.check_eq(e["passes"], 2, f"pass + copy: {labels(e)}")
    e = await cost(ed, sid, None, passes=[{"shader": "library:posterize"}, {"shader": "library:crt", "enabled": False}], fixed=True)
    ed.check_eq(e["passes"], 1, f"one full pass, a disabled one: {labels(e)}")
    await cost(ed, sid, None)


async def test_post_images_reload(ed: Editor):
    ed.require_tools("screenshot", "write_file")
    sid = STATE["sid"]
    await ed.call("select_scene", scene=sid)

    # the LUT strip: green everywhere, then red once the file changes
    lut = ed.project / "assets" / "luts" / "flat.png"
    write_png(lut, 4, 2, (0, 255, 0, 255))
    await ed.call("set_scene_settings", scene=sid, settings={"post_processing": {
        "color_grading_enabled": True, "lut_texture": "assets/luts/flat.png"}})
    r, g, b = mean_rgb(await shot(ed))
    ed.check(g > 150 and r < 60, f"the green LUT applies: {r:.0f},{g:.0f},{b:.0f}")
    write_png(lut, 4, 2, (255, 0, 0, 255))
    await ed.call("sync_files")
    r, g, b = mean_rgb(await shot(ed))
    ed.check(r > 150 and g < 60, f"the changed LUT image applies: {r:.0f},{g:.0f},{b:.0f}")
    await ed.call("set_scene_settings", scene=sid, settings={"post_processing": None})

    # a pass texture: the fork draws its overlay over the whole view
    frag = STATE.get("fork_frag") or (STATE["fork"] + ".frag")
    await ed.call("write_file", path=frag, content="""#version 450

in vec2 v_texcoord;
out vec4 frag_color;

uniform texture2D u_sceneColorTexture;
uniform sampler u_sceneColor_smp;
uniform texture2D u_overlayTexture;
uniform sampler u_overlay_smp;

void main(){
    vec4 scene = texture(sampler2D(u_sceneColorTexture, u_sceneColor_smp), v_texcoord);
    frag_color = vec4(texture(sampler2D(u_overlayTexture, u_overlay_smp), v_texcoord).rgb + scene.rgb * 0.0, 1.0);
}
""")
    overlay = ed.project / "assets" / "textures" / "overlay.png"
    write_png(overlay, 4, 4, (0, 0, 255, 255))
    await ed.call("sync_files")
    await ed.call("set_scene_settings", scene=sid, settings={"post_process": [
        {"shader": STATE["fork"], "textures": {"u_overlayTexture": "assets/textures/overlay.png"}}]})
    r, g, b = mean_rgb(await shot(ed))
    ed.check(b > 150 and r < 60, f"the pass draws its blue texture: {r:.0f},{g:.0f},{b:.0f}")
    write_png(overlay, 4, 4, (255, 255, 0, 255))
    await ed.call("sync_files")
    r, g, b = mean_rgb(await shot(ed))
    ed.check(r > 150 and g > 150 and b < 60, f"the changed pass texture is drawn: {r:.0f},{g:.0f},{b:.0f}")
    await ed.call("set_scene_settings", scene=sid, settings={"post_process": []})
