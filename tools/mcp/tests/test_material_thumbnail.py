"""A material's thumbnail is never cached from a stand-in (BF23 F16): one that never finishes loading
gets no thumbnail file after the waits run out, is not asked again until it changes, and gets its
thumbnail once it changes and loads. JASPER_TEST_MATERIAL_THUMBNAIL_STUCK holds the load: a texture
that fails to load is not pending, so a project cannot ask for this state itself.
"""

from __future__ import annotations

import asyncio

from harness import Editor

CI = True
TIMEOUT = 600

MATERIAL = "assets/materials/Never.material"
FIRST = """name: materials/Never.material
baseColorFactor: [1, 1, 1, 1]
"""
CHANGED = """name: materials/Never.material
baseColorFactor: [1, 0, 0, 1]
"""


async def test_never_loading_material_gets_no_thumbnail():
    async with Editor(create=True, name="Material Thumbnail CI", env={"JASPER_TEST_MATERIAL_THUMBNAIL_STUCK": "stuck.marker"}) as ed:
        thumbs = ed.project / ".jasperengine" / "thumbs"
        marker = ed.project / "stuck.marker"

        def thumb_files() -> set[str]:
            return {p.name for p in thumbs.glob("*")} if thumbs.exists() else set()

        async def give_ups() -> list:
            return (await ed.call("read_console", contains="No thumbnail for material", limit=5))["entries"]

        await ed.call("create_scene", name="Thumbs", type="3D", path="scenes/Thumbs")
        marker.write_text("held\n")
        await ed.call("write_file", path=MATERIAL, content=FIRST)
        baseline = thumb_files()
        # view_asset asks for it and waits 15 s at most
        await ed.call_raw("view_asset", {"path": MATERIAL})
        for _ in range(240):
            if thumb_files() - baseline or await give_ups():
                break
            await asyncio.sleep(0.5)
        ed.check_eq(sorted(thumb_files() - baseline), [], "the stand-in is not written as the thumbnail")
        ed.check(await give_ups(), "the editor says why there is none")

        # the same file is not asked again
        again = await ed.call_raw("view_asset", {"path": MATERIAL})
        ed.check(again.is_error, f"still none: {again.text[:200]}")
        ed.check_eq(sorted(thumb_files() - baseline), [], "still no thumbnail")
        ed.check_eq(len(await give_ups()), 1, "given up once")

        # a change asks again
        marker.unlink()
        await ed.call("write_file", path=MATERIAL, content=CHANGED)
        fixed = await ed.call_raw("view_asset", {"path": MATERIAL})
        ed.check(not fixed.is_error and fixed.images, f"the changed material's thumbnail: {fixed.text[:200]}")
        ed.check(thumb_files() - baseline, "its file is written")
