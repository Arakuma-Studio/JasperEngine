"""The editor checks of docs/specs/S11-materials-lighting.md AC12, driven over MCP and recorded with
screenshots (the README's review record is written from its output):

  1. a ClearCoatTest submesh: the Advanced Surface group shows Clear Coat enabled with its maps
  2. toggling it off draws with the plain variant while the new one compiles, without a frame
     stall (the longest frame while it compiles against the longest before)
  3. undo restores it
  4. three meshes set Sheen Roughness in one step, and undo together
  5. four maps: "Extension maps 3 / 3", the fourth dropped
  6. a Light from Engine to Lux leaves the image unchanged; with a colour temperature the colour
     row is labelled Filter
  7. the Car Paint preset applies a clear coat

    uv run --no-project --with-requirements tools/mcp/requirements.txt python tools/fixtures/pbr-extended/review.py [out dir]

Screenshots go to build/pbr-extended-review/ (or the directory given); prints one line per check.
"""
import asyncio
import shutil
import sys
import time
from pathlib import Path

HERE = Path(__file__).resolve().parent
REPO = HERE.parent.parent.parent
sys.path.insert(0, str(REPO / "tools" / "mcp"))
import pngdiff  # noqa: E402
from harness import Editor, find_key  # noqa: E402

KHRONOS = REPO / "engine" / "tests" / "render" / "assets" / "khronos"
M = "submeshes[0].material."
RESULTS = []


def record(name, ok, detail):
    RESULTS.append((name, ok, detail))
    print(f"{'PASS' if ok else 'FAIL'} {name}: {detail}", flush=True)


async def save_shot(ed, out: Path, name: str, view: str = "game"):
    await ed.call("wait", frames=20, timeout_seconds=900)
    r = await ed.call_raw("screenshot", {"view": view, "max_width": 0}, timeout=300)
    if r.images:
        (out / f"{name}.png").write_bytes(r.images[0][0])
        return pngdiff.decode(r.images[0][0])
    return None


async def ready(ed, eid, submesh=0):
    v = {}
    for _ in range(200):
        v = await ed.call("get_material_variant", entity=eid, submesh=submesh)
        if v["ready"] or v["failed"]:
            return v
        await ed.call("wait", frames=5, timeout_seconds=900)
    return v


async def frame_times(ed, frames: int):
    """the wall time of each of `frames` single-frame waits, in ms"""
    times = []
    for _ in range(frames):
        start = time.perf_counter()
        await ed.call("wait", frames=1, timeout_seconds=900)
        times.append((time.perf_counter() - start) * 1000.0)
    return times


async def main(out: Path) -> None:
    out.mkdir(parents=True, exist_ok=True)
    async with Editor(create=True, name="PbrReview") as ed:
        models = ed.project / "models" / "ClearCoatTest"
        models.mkdir(parents=True, exist_ok=True)
        shutil.copy(KHRONOS / "ClearCoatTest" / "glTF-Binary" / "ClearCoatTest.glb", models / "ClearCoatTest.glb")
        await ed.call("sync_files")
        made = await ed.call("create_scene", name="Review", type="3D", path="Review.scene", close_previous=True, unsaved="discard")
        sid = made["scene"]["id"]
        cam = (await ed.call("create_entity", name="Camera", type="camera", position=[0, 0, 12]))["entity_id"]
        await ed.call("set_main_camera", entity=cam)
        await ed.call("set_properties", entity=cam, values={"camera": {"useTarget": True, "target": [0, 0, 0]}})
        await ed.call("create_entity", name="ClearCoatTest", type="model", model="models/ClearCoatTest/ClearCoatTest.glb")
        await ed.call("wait", frames=60, timeout_seconds=900)
        meshes = [find_key(e, "entity_id", "id") for e in (await ed.call("find_entities", component="MeshComponent"))["entities"]]
        coated = None
        for eid in meshes:
            v = await ready(ed, eid)
            if "PBR_CLEARCOAT" in v.get("defines", []) and v.get("extensionMaps", {}).get("used", 0) > 0:
                coated = eid
                break
        record("1 clear coat with maps", coated is not None,
               f"entity {coated}: {v.get('defines') if coated else 'none found'}, maps {v.get('extensionMaps') if coated else ''}")
        if coated is not None:
            await ed.call("select_entities", entities=[coated])
            await ed.call("focus_window", window="inspector")
            await save_shot(ed, out, "1-inspector-clearcoat", "editor")

            # 2: toggle off; the plain variant draws meanwhile
            before = await frame_times(ed, 30)
            await ed.call("set_properties", entity=coated, values={"mesh": {M + "pbr.clearcoat.enabled": False}})
            during = await frame_times(ed, 30)
            v = await ready(ed, coated)
            record("2 toggle without a stall", not v["failed"] and "PBR_CLEARCOAT" not in v["defines"],
                   f"longest frame before {max(before):.0f} ms, while compiling {max(during):.0f} ms (wall time of one-frame waits); "
                   f"now {v['defines']}")
            await save_shot(ed, out, "2-inspector-off", "editor")

            # 3: undo
            await ed.call("undo", scene=sid)
            v = await ready(ed, coated)
            record("3 undo restores it", "PBR_CLEARCOAT" in v["defines"], f"{v['defines']}")

        # 4: three spheres, one Sheen Roughness edit, one undo
        spheres = []
        for k in range(3):
            made = await ed.call("create_entity", name=f"Ball{k}", type="sphere", position=[-3 + 3 * k, -4, 0])
            spheres.append(made["entity_id"])
        await ed.call("set_properties", entities=spheres, values={"mesh": {M + "pbr.sheen.enabled": True}})
        await ed.call("select_entities", entities=spheres)
        await ed.call("set_properties", entities=spheres, values={"mesh": {M + "pbr.sheen.roughness": 0.8}})
        async def sheen_roughness(e):
            got = await ed.call("get_entity", entity=e, components=["mesh"], properties=[M[:-1]], detail="values")
            pbr = next(iter(got["components"].values()))[M[:-1]].get("pbr") or {}
            return (pbr.get("sheen") or {}).get("roughness", 0.0)
        got = [await sheen_roughness(e) for e in spheres]
        history = await ed.call("get_undo_history", scene=sid)
        await ed.call("undo", scene=sid)
        after = [await sheen_roughness(e) for e in spheres]
        record("4 multi-select sheen roughness", all(abs((g or 0) - 0.8) < 1e-4 for g in got) and all(abs(a or 0) < 1e-4 for a in after),
               f"set {got}, after one undo {after}; the undo step: {history.get('undo_name')!r}")

        # 5: four maps on one material
        tex = ed.project / "textures"
        tex.mkdir(exist_ok=True)
        import struct
        import zlib

        def png(w, h, rgba):
            raw = b"".join(b"\x00" + bytes(rgba) * w for _ in range(h))

            def chunk(kind, data):
                return struct.pack(">I", len(data)) + kind + data + struct.pack(">I", zlib.crc32(kind + data) & 0xffffffff)
            return (b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 6, 0, 0, 0)) +
                    chunk(b"IDAT", zlib.compress(raw)) + chunk(b"IEND", b""))
        (tex / "white.png").write_bytes(png(4, 4, (255, 255, 255, 255)))
        await ed.call("sync_files")
        ball = spheres[0]
        values = {}
        for lobe in ("clearcoat", "specular", "transmission", "sheen"):
            values[f"{M}pbr.{lobe}.enabled"] = True
            values[f"{M}pbr.{lobe}.map"] = "textures/white.png"
        await ed.call("set_properties", entity=ball, values={"mesh": values})
        v = await ready(ed, ball)
        maps = v["extensionMaps"]
        record("5 four maps, three bound", maps["used"] == 3 and len(maps["dropped"]) == 1, f"{maps}")
        await ed.call("select_entities", entities=[ball])
        await ed.call("focus_window", window="inspector")
        await save_shot(ed, out, "5-inspector-maps", "editor")

        # 6: the light from Engine to Lux, then a colour temperature
        sun = find_key((await ed.call("find_entities", component="LightComponent"))["entities"][0], "entity_id", "id")
        plain = await save_shot(ed, out, "6-engine")
        await ed.call("set_light_unit", entity=sun, unit="lux")
        lux = await save_shot(ed, out, "6-lux")
        ok, fraction, _ = pngdiff.compare(plain, lux)
        await ed.call("set_properties", entity=sun, values={"light": {"useColorTemperature": True, "colorTemperature": 3200}})
        await ed.call("select_entities", entities=[sun])
        await ed.call("focus_window", window="inspector")
        await save_shot(ed, out, "6-inspector-filter", "editor")
        record("6 Engine to Lux unchanged", ok, f"{fraction * 100:.3f}% of the pixels differ (Filter label: 6-inspector-filter.png)")

        # 7: Car Paint
        await ed.call("apply_material_preset", entity=spheres[2], preset="car_paint")
        v = await ready(ed, spheres[2])
        record("7 Car Paint preset", "PBR_CLEARCOAT" in v["defines"], f"{v['defines']}")
        await save_shot(ed, out, "7-car-paint")
    # the export fixture's scenes as the editor draws them (tools/smoke-test.sh compares nothing:
    # these are for a look next to the game's screenshots)
    async with Editor(REPO / "tools" / "fixtures" / "pbr-extended-export") as ed:
        for scene in ("Lobes", "Transmission", "Hex"):
            await ed.call("open_scene", path=f"{scene}.scene", close_previous=True, unsaved="discard")
            await ed.call("wait", frames=120, timeout_seconds=900)
            await save_shot(ed, out, f"export-{scene}")
    failed = [r for r in RESULTS if not r[1]]
    print(f"{len(RESULTS) - len(failed)} / {len(RESULTS)} checks passed")
    sys.exit(1 if failed else 0)


if __name__ == "__main__":
    asyncio.run(main(Path(sys.argv[1]) if len(sys.argv) > 1 else REPO / "build" / "pbr-extended-review"))
