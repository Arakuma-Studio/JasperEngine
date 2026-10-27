"""Shader resource remapping, preset composition and opaque material previews."""

from __future__ import annotations

import yaml

from htmlui_pixels import decode_png

from harness import Editor, find_key
from shader_project import scene as project_scene

CI = True
EDITOR = {"create": True, "name": "Shader Library Phase2 Review CI"}

TIMEOUT = 900
M = "submeshes[0].material."
P = M + "extraPass."

HOLOGRAM = "library:hologram"  # a blended library mesh shader (@state blend=additive)

MARK = {"compare": "always", "ref": 1, "readMask": 255, "writeMask": 1, "pass": "replace", "fail": "keep", "depthFail": "replace"}


async def make_entity(ed: Editor, name: str, type_: str = "box"):
    made = await ed.call("create_entity", name=name, type=type_)
    eid = find_key(made, "entity_id", "id")
    ed.check(eid is not None, f"create_entity returned no id: {made!r}")
    return eid


async def props(ed: Editor, eid, *names: str) -> dict:
    got = await ed.call("get_entity", entity=eid, components=["mesh"], properties=list(names), detail="values")
    return next(iter(got["components"].values()))


async def preset(ed: Editor, eid, name: str):
    return await ed.call("apply_material_preset", preset=name, entity=eid)


def stencil(compare, ref, read, write, pass_="keep", fail="keep", depth_fail="keep") -> dict:
    return {"compare": compare, "ref": ref, "readMask": read, "writeMask": write, "pass": pass_, "fail": fail, "depthFail": depth_fail}


async def pass_and_stencil(ed: Editor, eid) -> dict:
    got = await props(
        ed, eid, P + "shader", P + "shaderFeatures", P + "order", P + "stencil", P + "shaderParams", M + "stencil", M + "sortPriority", M + "blendMode"
    )
    return {
        "shader": got[P + "shader"],
        "features": got[P + "shaderFeatures"],
        "order": got[P + "order"],
        "pass_stencil": got[P + "stencil"],
        "params": got[P + "shaderParams"],
        "stencil": got[M + "stencil"],
        "priority": got[M + "sortPriority"],
        "blend": got[M + "blendMode"],
    }


def check_visible_outline(ed: Editor, got: dict, stencil_: dict, where: str):
    ed.check_eq(got["shader"], "library:outline_stencil", where)
    ed.check("VISIBILITY_VISIBLE_ONLY" in got["features"], f"{where}: visible only: {got!r}")
    ed.check_eq(got["order"], "after", where)
    ed.check_eq(got["stencil"], stencil_, f"{where}: the material's stencil")


async def test_fork_rename_remaps_material_and_pass_shaders(ed: Editor):
    async with project_scene(ed, "test_fork_rename_remaps_material_and_pass_shaders", with_texture=False) as SCENE:
        eid = await make_entity(ed, "ForkUser")
        forked = await ed.call("fork_shader", entity=eid, directory="shaders/review", name="ReviewFork")
        base = forked["vert_path"][: -len(".vert")]
        ed.check_eq(base, "shaders/review/ReviewFork", f"fork base: {forked!r}")
        # the fork as the material's and the pass's shader, the mesh back on the built-in shader
        await ed.call("set_properties", entity=eid, values={"mesh": {"customShader": ""}})
        await ed.call("set_properties", entity=eid, values={"mesh": {M + "shader": base, P + "shader": base}})
        await ed.call("create_material", path="assets/materials/ReviewFork", values={"shader": base, "extraPass": {"shader": base}})
        await ed.call("create_prefab", entity=eid, path="prefabs/ReviewFork.prefab")
        await ed.call("save_scene")
        material_file = ed.project / "assets/materials/ReviewFork.material"
        prefab_file = ed.project / "prefabs/ReviewFork.prefab"
        material = yaml.safe_load(material_file.read_text())
        ed.check(material.get("shaderGuid") and (material.get("extraPass") or {}).get("shaderGuid"),
                 f"the .material file names the fork with its GUIDs: {material!r}")
        material_text, prefab_text = material_file.read_text(), prefab_file.read_text()

        # S04 R13: the move rewrites no referrer file; the references follow by GUID
        await ed.call("move_asset", path="shaders/review", to="shaders/review_moved")
        moved = "shaders/review_moved/ReviewFork"
        got = await props(ed, eid, M + "shader", P + "shader")
        ed.check_eq((got[M + "shader"], got[P + "shader"]), (moved, moved), "the scene's submesh follows the move")
        ed.check_eq((material_file.read_text(), prefab_file.read_text()), (material_text, prefab_text),
                    "R13: the move rewrites no referrer file")
        for owner in ("assets/materials/ReviewFork.material", "prefabs/ReviewFork.prefab"):
            targets = [d["target"]["path"] for d in (await ed.call("get_asset_dependencies", asset=owner))["dependencies"]]
            for ext in (".vert", ".frag"):
                ed.check(sum(t.endswith(moved + ext) for t in targets) >= 2,
                         f"{owner}: the shader and the pass's resolve to the moved fork's {ext}: {targets}")
        await ed.call("save_scene")
        scene = (ed.project / SCENE).read_text()
        ed.check(moved in scene, "the saved scene keeps the moved shader reference")

        # undo of the move puts the references back
        await ed.call("undo", scope="project")
        got = await props(ed, eid, M + "shader", P + "shader")
        ed.check_eq((got[M + "shader"], got[P + "shader"]), (base, base), "undo of the move")
        await ed.call("move_asset", path="shaders/review", to="shaders/review_moved")

        # deleting the fork's files clears nothing (S04 R15): the references are Missing until it is back
        await ed.call("delete_asset", paths=["shaders/review_moved"])
        got = await props(ed, eid, M + "shader", P + "shader")
        ed.check_eq((got[M + "shader"], got[P + "shader"]), (moved, moved), "a deleted fork stays referenced")
        ed.check_eq(material_file.read_text(), material_text, "the .material file keeps the deleted fork")
        listed = (await ed.call("list_missing_assets"))["missing"]
        missing = {r["owner"] for g in listed if "ReviewFork" in g["last_path"] for r in g["referrers"]}
        ed.check(any(o.endswith("ReviewFork.material") for o in missing), f"Missing Assets lists the .material file: {listed!r:.3000}")
        await ed.call("undo", scope="project")
        got = await props(ed, eid, M + "shader", P + "shader")
        ed.check_eq((got[M + "shader"], got[P + "shader"]), (moved, moved), "the fork is back")
        await ed.call("save_scene")


async def test_whole_value_and_inner_path_refused(ed: Editor):
    async with project_scene(ed, "test_whole_value_and_inner_path_refused", with_texture=False) as SCENE:
        eid = await make_entity(ed, "MixedBox")
        await ed.call("set_properties", entity=eid, values={"mesh": {P + "shader": "library:outline"}})
        # a whole pass with a new shader and one of its entries: refused, naming the one-value form
        r = await ed.call_raw(
            "set_properties", {"entity": eid, "values": {"mesh": {M + "extraPass": {"shader": "library:unlit"}, P + "shaderParams.baseColor": [1, 0, 0, 1]}}}
        )
        ed.check(r.is_error and "in one call" in r.text, "a whole value and a nested edit must be refused atomically")
        got = await props(ed, eid, P + "shader")
        ed.check_eq(got[P + "shader"], "library:outline", "nothing applied")

        # the one-value form: the entry follows the pass's new shader (baseColor is unlit's, not outline's)
        await ed.call(
            "set_properties",
            entity=eid,
            values={"mesh": {M + "extraPass": {"shader": "library:unlit", "shaderParams": {"baseColor": [1, 0, 0, 1]}, "stencil": {"ref": 3}}}},
        )
        got = await props(ed, eid, P + "shader", P + "shaderParams", P + "stencil.ref")
        ed.check_eq(got[P + "shader"], "library:unlit")
        ed.check_eq(got[P + "shaderParams"].get("baseColor"), [1, 0, 0, 1])
        ed.check_eq(got[P + "stencil.ref"], 3)
        # paths of the same value side by side still work
        await ed.call("set_properties", entity=eid, values={"mesh": {P + "shaderParams.alphaCutoff": 0.3, P + "stencil.ref": 4, P + "order": "before"}})
        await ed.call("delete_entities", entities=[eid])


async def test_outline_hull_follows_the_drawn_shader(ed: Editor):
    async with project_scene(ed, "test_outline_hull_follows_the_drawn_shader", with_texture=False) as SCENE:
        opaque = await make_entity(ed, "HullOpaque")
        await preset(ed, opaque, "outline_hull")
        got = await pass_and_stencil(ed, opaque)
        ed.check_eq((got["shader"], got["order"], got["stencil"]["compare"]), ("library:outline", "after", "auto"), "opaque")

        custom = await make_entity(ed, "HullMeshShader")
        await ed.call("set_properties", entity=custom, values={"mesh": {"customShader": HOLOGRAM}})
        await preset(ed, custom, "outline_hull")
        got = await pass_and_stencil(ed, custom)
        check_visible_outline(ed, got, MARK, "blended through the mesh's customShader")
        # the look of the hull: its default colour, not outline_stencil's orange
        ed.check(got["params"].get("outlineColor", [1])[0] < 0.1, f"the hull's colour: {got['params']!r}")

        await ed.call("set_scene_settings", settings={"default_mesh_shader": HOLOGRAM})
        default = await make_entity(ed, "HullSceneDefault")
        await preset(ed, default, "outline_hull")
        check_visible_outline(ed, await pass_and_stencil(ed, default), MARK, "blended through the scene default")
        await ed.call("set_scene_settings", settings={"default_mesh_shader": ""})

        # opaque again (explicit blend): re-applying makes it a hull, keeping its colour and width
        await ed.call("set_properties", entity=custom, values={"mesh": {P + "shaderParams.screenWidth": 6, M + "blendMode": "opaque"}})
        await preset(ed, custom, "outline_hull")
        got = await pass_and_stencil(ed, custom)
        ed.check_eq((got["shader"], got["stencil"]["compare"]), ("library:outline", "auto"), "back to the hull, mark gone")
        ed.check_eq(got["params"].get("screenWidth"), [6, 0, 0, 0], "the width carried over")
        ed.check("VISIBILITY_VISIBLE_ONLY" not in got["features"], f"no visibility keyword on the hull: {got!r}")
        # and blended again: the hull becomes the stencil outline with its values
        await ed.call("set_properties", entity=custom, values={"mesh": {M + "blendMode": "alpha"}})
        await preset(ed, custom, "outline_hull")
        got = await pass_and_stencil(ed, custom)
        check_visible_outline(ed, got, MARK, "blended again")
        ed.check_eq(got["params"].get("screenWidth"), [6, 0, 0, 0])
        # the through-walls preset over it: Visibility back to Always
        await preset(ed, custom, "outline_stencil")
        got = await pass_and_stencil(ed, custom)
        ed.check("VISIBILITY_VISIBLE_ONLY" not in got["features"], f"through walls: {got!r}")
        for eid in (opaque, custom, default):
            await ed.call("delete_entities", entities=[eid])


async def test_outline_with_draw_once_and_masks(ed: Editor):
    async with project_scene(ed, "test_outline_with_draw_once_and_masks", with_texture=False) as SCENE:
        # draw-once then outline: one stencil, the draw-once test plus the outline mark
        glass = await make_entity(ed, "OnceGlass")
        await preset(ed, glass, "draw_once_transparency")
        await preset(ed, glass, "outline_hull")
        once_marked = stencil("not_equal", 5, 12, 13, "replace")
        check_visible_outline(ed, await pass_and_stencil(ed, glass), once_marked, "draw-once, then outline")
        # removing the outline (another pass preset) keeps the draw-once test
        await preset(ed, glass, "see_through_silhouette")
        got = await pass_and_stencil(ed, glass)
        ed.check_eq(got["stencil"], stencil("not_equal", 4, 12, 12, "replace"), "the mark goes, draw-once stays")

        # outline on an opaque box, then draw-once: the hull becomes the blended kind
        box = await make_entity(ed, "HullThenOnce")
        await preset(ed, box, "outline_hull")
        await preset(ed, box, "draw_once_transparency")
        got = await pass_and_stencil(ed, box)
        check_visible_outline(ed, got, once_marked, "outline, then draw-once")
        ed.check_eq(got["blend"], "alpha")

        # a mask reader masks the pass: a hull inside, the visible-only outline inside
        inside = await make_entity(ed, "InsideHull")
        await preset(ed, inside, "outline_hull")
        await preset(ed, inside, "visible_inside_mask")
        got = await pass_and_stencil(ed, inside)
        ed.check_eq(got["shader"], "library:outline")
        ed.check_eq(got["pass_stencil"], stencil("equal", 16, 240, 0), "the hull reads the mask")
        # outside: the same with not equal; the order of the presets does not matter
        outside = await make_entity(ed, "OutsideHull")
        await preset(ed, outside, "visible_outside_mask")
        await preset(ed, outside, "outline_hull")
        ed.check_eq((await pass_and_stencil(ed, outside))["pass_stencil"], stencil("not_equal", 16, 240, 0))

        # a blended reader: the mark over the reader's test, the outline masked inside
        blended = await make_entity(ed, "InsideGlass")
        await ed.call("set_properties", entity=blended, values={"mesh": {M + "blendMode": "alpha"}})
        await preset(ed, blended, "visible_inside_mask")
        await preset(ed, blended, "outline_hull")
        got = await pass_and_stencil(ed, blended)
        check_visible_outline(ed, got, stencil("equal", 17, 240, 1, "replace"), "blended inside a mask")
        ed.check_eq(got["pass_stencil"], stencil("equal", 16, 241, 0), "inside the mask and outside the mark")
        # switching to outside keeps the mark; the outline keeps its own test (one test can't do both)
        await preset(ed, blended, "visible_outside_mask")
        got = await pass_and_stencil(ed, blended)
        check_visible_outline(ed, got, stencil("not_equal", 17, 240, 1, "replace"), "blended outside a mask")
        ed.check_eq(got["pass_stencil"]["compare"], "auto")

        # silhouette: blends once inside a mask (equal on the mask bits and bit 2, invert), mask test outside
        sil = await make_entity(ed, "MaskedSilhouette")
        await preset(ed, sil, "see_through_silhouette")
        await preset(ed, sil, "visible_inside_mask")
        ed.check_eq((await pass_and_stencil(ed, sil))["pass_stencil"], stencil("equal", 16, 242, 2, "invert"))
        await preset(ed, sil, "visible_outside_mask")
        ed.check_eq((await pass_and_stencil(ed, sil))["pass_stencil"], stencil("not_equal", 16, 240, 0))
        # draw-once replaces the reader: the pass's mask test goes back to its shader's
        await preset(ed, sil, "draw_once_transparency")
        ed.check_eq((await pass_and_stencil(ed, sil))["pass_stencil"]["compare"], "auto")

        # a through-walls outline does not combine: a reader removes it (as before)
        walls = await make_entity(ed, "WallsOutline")
        await preset(ed, walls, "outline_stencil")
        await preset(ed, walls, "visible_inside_mask")
        got = await pass_and_stencil(ed, walls)
        ed.check_eq((got["shader"], got["stencil"]), ("", stencil("equal", 16, 240, 240)), "the through-walls outline goes")

        # a .material file is judged by its own shader
        await ed.call("create_material", path="assets/materials/ReviewGlass", values={"shader": HOLOGRAM})
        r = await ed.call("apply_material_preset", preset="outline_hull", path="assets/materials/ReviewGlass.material")
        ed.check_eq(r["material"]["extraPass"]["shader"], "library:outline_stencil", f"{r['material']!r}")
        for eid in (glass, box, inside, outside, blended, sil, walls):
            await ed.call("delete_entities", entities=[eid])


async def test_material_tools_submesh_argument(ed: Editor):
    async with project_scene(ed, "test_material_tools_submesh_argument", with_texture=False) as SCENE:
        eid = await make_entity(ed, "SubmeshArg")
        await ed.check_error("apply_material_preset", preset="outline_hull", entity=eid, submesh=0.5, contains="submesh")
        await ed.call("apply_material_preset", preset="outline_hull", entity=eid, submesh=0)
        await ed.check_error("apply_material_preset", preset="outline_hull", entity=eid, submesh=4, contains="submesh")


async def test_preview_without_alpha_write_is_opaque(ed: Editor):
    async with project_scene(ed, "test_preview_without_alpha_write_is_opaque", with_texture=False) as SCENE:
        path = "assets/materials/OpaqueRgb.material"
        await ed.call("create_material", path=path, values={"colorWrite": "rgb", "baseColorFactor": [0.9, 0.2, 0.1, 1]})
        r = await ed.call_raw("view_asset", {"path": path, "max_size": 128}, timeout=120)
        ed.check(not r.is_error and r.images, f"material thumbnail: {r.text[:300]}")
        (ed.scratch.root / "material-rgb-preview.png").write_bytes(r.images[0][0])
        _, _, channels, rows = decode_png(r.images[0][0])
        ed.check_eq(channels, 4, "previews preserve a transparent background")
        alpha = [row[i] for row in rows for i in range(3, len(row), 4)]
        ed.check(0 in alpha, "thumbnail has a transparent background")
        ed.check(255 in alpha, "the material remains opaque even when its scene colorWrite omits alpha")
