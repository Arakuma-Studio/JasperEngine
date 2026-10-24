"""render_pose_animation (spec: pose animation phase 3, sections 2-5) on the pose fixture project
(tools/fixtures/pose-animation: a T-pose hero with Mixamo names, an A-pose robot with Blender
names; the harness runs the editor with --headless).

poses mode: the sheet size follows the layout, the image is not flat, the left (coral) and right
(cyan) overlay colours sit where the character's sides are (front view: its left on image right;
a raised left arm is the highest coral) -> motion returns the frame count asked -> times labels
(with the feet in contact) -> model override renders the A-pose robot -> save_path writes the PNG
-> the Scene view camera and the open scene are unchanged -> the same request twice gives the same
PNG bytes -> argument errors -> a rig missing a slot names it and the set_humanoid fix.
"""

from __future__ import annotations

import harness
from harness import Editor
from htmlui_pixels import decode_png

EDITOR = {"project": harness.REPO / "tools" / "fixtures" / "pose-animation"}
TIMEOUT = 600

HERO = "assets/models/hero_tpose.gltf"
ANIM = "assets/anim/render.poseanim"
NO_MODEL = "assets/anim/no_model.poseanim"

CORAL = (0xFF, 0x7A, 0x45)
CYAN = (0x36, 0xB3, 0xFF)
FLOOR = (0xFF, 0xA7, 0x26)
LABEL = 14          # label strip and title strip height
SHOTS = None        # set to a directory to keep the sheets for a look


def sheet_size(columns: int, rows: int, cell: tuple, overlay: bool = True) -> tuple:
    """PoseSheetLayout: 1 px separators between cells; a title strip and label strips with the overlay."""
    w, h = cell
    label = LABEL if overlay else 0
    title = LABEL + 1 if overlay else 0
    return columns * w + (columns - 1), title + rows * (h + label) + (rows - 1)


def near(px, color, tol=6) -> bool:
    return all(abs(int(px[i]) - color[i]) <= tol for i in range(3))


class Sheet:
    def __init__(self, png: bytes, cell: tuple, overlay: bool = True):
        self.width, self.height, self.ch, self.rows = decode_png(png)
        self.cw, self.chh = cell
        self.top = LABEL + 1 if overlay else 0
        self.label = LABEL if overlay else 0

    def pixel(self, x: int, y: int):
        r = self.rows[y]
        return r[x * self.ch], r[x * self.ch + 1], r[x * self.ch + 2]

    def cell_pixels(self, col: int, row: int, color, tol=6):
        """(x, y) in the cell of every pixel near `color`."""
        x0 = col * (self.cw + 1)
        y0 = self.top + row * (self.chh + self.label + 1)
        out = []
        for y in range(self.chh):
            for x in range(self.cw):
                if near(self.pixel(x0 + x, y0 + y), color, tol):
                    out.append((x, y))
        return out

    def distinct(self, col: int, row: int) -> int:
        x0 = col * (self.cw + 1)
        y0 = self.top + row * (self.chh + self.label + 1)
        return len({self.pixel(x0 + x, y0 + y) for y in range(0, self.chh, 2) for x in range(0, self.cw, 2)})


async def render(ed: Editor, **args):
    r = await ed.call_raw("render_pose_animation", args)
    if r.is_error:
        raise harness.ToolError("render_pose_animation", r.text, r)
    ed.check_eq(len(r.images), 1, "one image")
    data, mime = r.images[0]
    ed.check_eq(mime, "image/png", "a PNG")
    if SHOTS:
        import pathlib
        name = "_".join(f"{k}-{v}" for k, v in args.items() if k in ("mode", "views", "model", "path"))
        pathlib.Path(SHOTS, name.replace("/", "_").replace(" ", "") + ".png").write_bytes(data)
    return r, data


async def make_animation(ed: Editor):
    """Stand, Raise L (left arm straight up), Crouch, and an airborne Jump."""
    if (ed.project / ANIM).exists():
        return
    await ed.call("edit_pose_animation", path=ANIM, create={"preview_model": HERO}, ops=[
        {"op": "add_pose", "name": "Stand", "hold": 0.2, "angles": {"left_upper_arm.raise": 10, "right_upper_arm.raise": 10}},
        {"op": "add_pose", "name": "Raise L", "hold": 0.2, "angles": {"left_upper_arm.raise": 170, "right_upper_arm.raise": 10}},
        {"op": "add_pose", "name": "Crouch", "hold": 0.1, "angles": {
            "left_upper_leg.forward": 95, "right_upper_leg.forward": 95, "left_lower_leg.bend": 110,
            "right_lower_leg.bend": 110, "left_foot.point": -25, "right_foot.point": -25}},
        {"op": "add_pose", "name": "Jump", "hold": 0.15, "grounded": False, "lift": 0.4, "angles": {
            "left_upper_arm.raise": 150, "right_upper_arm.raise": 150}},
    ])


# -- registry ------------------------------------------------------------------

async def test_tool_listed(ed: Editor):
    ed.require_tools("render_pose_animation")
    tool = ed.tool("render_pose_animation")
    ed.check(tool.annotations.read_only_hint, "read-only")
    props = tool.input_schema["properties"]
    ed.check_eq(props["mode"]["enum"], ["poses", "motion", "times"], "modes")
    ed.check_eq(props["views"]["items"]["enum"], ["front", "side", "three_quarter", "back", "top"], "views")
    for name in ("path", "poses", "frames", "times", "model", "scene", "cell", "overlay", "save_path"):
        ed.check_in(name, props, "arguments")


# -- poses mode ----------------------------------------------------------------

async def test_poses_layout_and_sides(ed: Editor):
    await make_animation(ed)
    r, png = await render(ed, path=ANIM, poses=[0, 1, 2])
    s = r.structured
    ed.check_eq(s["mode"], "poses", "mode")
    ed.check_eq(s["rows"], ["front", "side"], "default views of poses mode")
    ed.check_eq([c["index"] for c in s["columns"]], [0, 1, 2], "columns")
    ed.check_eq([c["label"] for c in s["columns"]], ["0 Stand", "1 Raise L", "2 Crouch"], "labels")
    ed.check_eq(s["cell"], [160, 200], "default cell")
    sheet = Sheet(png, (160, 200))
    ed.check_eq((sheet.width, sheet.height), sheet_size(3, 2, (160, 200)), "sheet size follows the layout")
    ed.check_eq(s["size"], [sheet.width, sheet.height], "size in the result")
    for col in range(3):
        for row in range(2):
            ed.check(sheet.distinct(col, row) > 20, f"cell {col},{row} is not flat")
    # metrics: the phase 2 shape, per column
    ed.check_eq(len(s["metrics"]), 3, "metrics per column")
    for key in ("feet", "lowest", "balanced", "balance_margin", "center_of_mass", "limit_hits"):
        ed.check_in(key, s["metrics"][0], "metrics shape")
    ed.check(s["metrics"][0]["feet"]["left"]["contact"] and s["metrics"][0]["feet"]["right"]["contact"], "Stand on both feet")
    got = await ed.call("get_pose_animation", path=ANIM, poses=[1], detail="summary")
    ed.check_eq(s["metrics"][1], got["poses"][0]["metrics"], "the same metrics as get_pose_animation")
    for word in ("coral", "cyan", "front", "side", "Raise L"):
        ed.check_in(word, r.text, "caption")

    # front view, Raise L: the character's left (coral) is on image right, its raised arm the highest
    coral = sheet.cell_pixels(1, 0, CORAL)
    cyan = sheet.cell_pixels(1, 0, CYAN)
    ed.check(len(coral) > 40 and len(cyan) > 40, f"both sides drawn: coral {len(coral)}, cyan {len(cyan)}")
    mean_x = lambda pts: sum(p[0] for p in pts) / len(pts)
    ed.check(mean_x(coral) > 80 > mean_x(cyan), f"front view: left on image right ({mean_x(coral):.0f} vs {mean_x(cyan):.0f})")
    top_coral = min(p[1] for p in coral)
    top_cyan = min(p[1] for p in cyan)
    ed.check(top_coral + 20 < top_cyan, f"the raised left arm is the highest: coral top {top_coral}, cyan top {top_cyan}")
    # Stand: the arms hang symmetrically
    coral0 = sheet.cell_pixels(0, 0, CORAL)
    cyan0 = sheet.cell_pixels(0, 0, CYAN)
    ed.check(abs(min(p[1] for p in coral0) - min(p[1] for p in cyan0)) <= 12, "Stand is symmetric")
    # side view, Raise L: the raised (left, coral) arm is above everything right
    coral_s = sheet.cell_pixels(1, 1, CORAL)
    cyan_s = sheet.cell_pixels(1, 1, CYAN)
    ed.check(coral_s and cyan_s, "side view draws both sides")
    ed.check(min(p[1] for p in coral_s) + 20 < min(p[1] for p in cyan_s), "side view: the raised left arm is highest")
    # the floor line, in every cell
    for col in range(3):
        ed.check(len(sheet.cell_pixels(col, 1, FLOOR, tol=4)) >= 160, f"floor line in cell {col}")


async def test_poses_without_overlay(ed: Editor):
    await make_animation(ed)
    r, png = await render(ed, path=ANIM, poses=[1], views=["front"], overlay=False)
    sheet = Sheet(png, (160, 200), overlay=False)
    ed.check_eq((sheet.width, sheet.height), sheet_size(1, 1, (160, 200), overlay=False), "no label or title strips")
    ed.check(len(sheet.cell_pixels(0, 0, CORAL)) < 5 and len(sheet.cell_pixels(0, 0, CYAN)) < 5,
             "no skeleton colours without the overlay")
    ed.check(sheet.distinct(0, 0) > 20, "the model is drawn")


async def test_all_views(ed: Editor):
    await make_animation(ed)
    views = ["front", "side", "three_quarter", "back", "top"]
    r, png = await render(ed, path=ANIM, poses=[1], views=views, cell=[96, 128])
    ed.check_eq(r.structured["rows"], views, "rows")
    sheet = Sheet(png, (96, 128))
    ed.check_eq((sheet.width, sheet.height), sheet_size(1, 5, (96, 128)), "size")
    for row in range(5):
        ed.check(sheet.distinct(0, row) > 10, f"{views[row]} is not flat")
    # back view: the character's left is on image left
    coral = sheet.cell_pixels(0, 3, CORAL)
    cyan = sheet.cell_pixels(0, 3, CYAN)
    ed.check(sum(p[0] for p in coral) / len(coral) < sum(p[0] for p in cyan) / len(cyan), "back view: left on image left")


# -- motion and times ----------------------------------------------------------

async def test_motion_frames(ed: Editor):
    await make_animation(ed)
    info = await ed.call("get_pose_animation", path=ANIM, detail="summary")
    r, png = await render(ed, path=ANIM, mode="motion", frames=6)
    s = r.structured
    ed.check_eq(len(s["columns"]), 6, "the frame count asked")
    ed.check_eq(s["rows"], ["side"], "default view of motion")
    times = [c["time"] for c in s["columns"]]
    ed.check_eq(times[0], 0, "starts at 0")
    step = info["length"] / 6   # a loop: [0, length)
    ed.check(all(abs(t - k * step) < 1e-3 for k, t in enumerate(times)), f"evenly spaced: {times}")
    ed.check(all(c["label"].startswith("t=") for c in s["columns"]), "time labels")
    sheet = Sheet(png, (160, 200))
    ed.check_eq((sheet.width, sheet.height), sheet_size(6, 1, (160, 200)), "size")
    ed.check_eq(len(s["metrics"]), 6, "metrics per frame")
    # 24 frames wrap after 12; a big cell shrinks to fit 2048 px
    r, png = await render(ed, path=ANIM, mode="motion", frames=24, cell=[512, 256], overlay=False)
    s = r.structured
    ed.check_eq(s["columns_per_row"], 12, "12 columns a row")
    ed.check(s["size"][0] <= 2048 and s["size"][1] <= 2048, f"capped: {s['size']}")
    ed.check(s["cell"][0] < 512 and abs(s["cell"][0] / s["cell"][1] - 2) < 0.05, f"cells shrink, keeping the aspect: {s['cell']}")
    sheet = Sheet(png, tuple(s["cell"]), overlay=False)
    ed.check_eq((sheet.width, sheet.height), sheet_size(12, 2, tuple(s["cell"]), overlay=False), "two rows of 12")


async def test_times_labels(ed: Editor):
    await make_animation(ed)
    info = await ed.call("get_pose_animation", path=ANIM, detail="summary")
    jump = info["poses"][3]["start_time"]
    r, _ = await render(ed, path=ANIM, mode="times", times=[0, 0.1, jump + 0.05], views=["front"])
    labels = [c["label"] for c in r.structured["columns"]]
    ed.check_eq(labels[0], "t=0.00 [LR]", "standing: both feet in contact")
    ed.check_eq(labels[1], "t=0.10 [LR]", "still standing")
    ed.check_eq(labels[2], f"t={jump + 0.05:.2f}", f"airborne: no contact marker ({labels[2]})")
    ed.check(not r.structured["metrics"][2]["feet"]["left"]["contact"], "the jump is off the floor")
    err = await ed.call("render_pose_animation", path=ANIM, mode="times", times=[info["length"] + 1], expect_error=True)
    ed.check_in("outside the animation", err, "times out of range")


# -- model override, save_path, scene untouched, determinism --------------------

async def test_model_override_robot(ed: Editor):
    await make_animation(ed)
    r, png = await render(ed, path=ANIM, poses=[1], views=["front"], model="Robot")
    s = r.structured
    ed.check_eq(s["model"]["name"], "Robot", "the scene model")
    ed.check_in("robot_apose", r.text, "the caption names the robot's model")
    sheet = Sheet(png, (160, 200))
    coral = sheet.cell_pixels(0, 0, CORAL)
    cyan = sheet.cell_pixels(0, 0, CYAN)
    ed.check(coral and cyan, "the robot gets the overlay")
    ed.check(min(p[1] for p in coral) + 20 < min(p[1] for p in cyan), "the robot raises its left arm too")
    _, hero_png = await render(ed, path=ANIM, poses=[1], views=["front"])
    ed.check(hero_png != png, "a different model draws a different image")


async def test_save_path(ed: Editor):
    await make_animation(ed)
    r, png = await render(ed, path=ANIM, poses=[0], views=["side"], save_path="renders/stand.png")
    ed.check_eq(r.structured["saved_to"], "renders/stand.png", "saved_to")
    ed.check_eq((ed.project / "renders/stand.png").read_bytes(), png, "the file holds the image")
    err = await ed.call("render_pose_animation", path=ANIM, save_path="../outside.png", expect_error=True)
    ed.check_in("save_path", err, "a path outside the project is refused")


async def test_scene_untouched_and_deterministic(ed: Editor):
    await make_animation(ed)
    camera = await ed.call("set_scene_camera")
    tree = await ed.call("get_scene_tree")
    _, first = await render(ed, path=ANIM, poses=[0, 1], views=["front", "three_quarter"])
    _, second = await render(ed, path=ANIM, poses=[0, 1], views=["front", "three_quarter"])
    ed.check(first == second, "the same request gives the same PNG bytes")
    ed.check_eq(await ed.call("set_scene_camera"), camera, "the Scene view camera is unchanged")
    ed.check_eq(await ed.call("get_scene_tree"), tree, "the open scene is unchanged")


# -- errors ----------------------------------------------------------------------

async def test_errors(ed: Editor):
    await make_animation(ed)
    if not (ed.project / NO_MODEL).exists():
        await ed.call("edit_pose_animation", path=NO_MODEL, create={}, ops=[{"op": "add_pose", "name": "A"}])
    err = await ed.call("render_pose_animation", path=NO_MODEL, expect_error=True)
    ed.check_in("Set a preview model: edit_pose_animation set_preview_model, or pass model", err, "no preview model")
    err = await ed.call("render_pose_animation", path=ANIM, mode="motion", frames=2, expect_error=True)
    ed.check_in("frames", err, "frames range")
    err = await ed.call("render_pose_animation", path=ANIM, views=["sideways"], expect_error=True)
    ed.check_in("views[0]", err, "unknown view")
    err = await ed.call("render_pose_animation", path=ANIM, poses=[9], expect_error=True)
    ed.check_in("poses[0]: no pose 9", err, "pose index")
    err = await ed.call("render_pose_animation", path=ANIM, cell=[32, 200], expect_error=True)
    ed.check_in("64..512", err, "cell range")
    err = await ed.call("render_pose_animation", path=ANIM, mode="times", expect_error=True)
    ed.check_in("times", err, "times mode needs times")
    err = await ed.call("render_pose_animation", path=ANIM, model="Ground", expect_error=True)
    ed.check_in("not a model GameObject", err, "model must be a model")


async def test_rig_failure_names_slots_and_fix(ed: Editor):
    """A model whose humanoid map misses a required slot: the error names it and the set_humanoid fix."""
    robot = "assets/models/robot_apose.gltf"
    path = "assets/anim/robot_unmapped.poseanim"
    await ed.call("edit_pose_animation", path=path, create={"preview_model": robot}, ops=[{"op": "add_pose", "name": "A"}])
    await ed.call("edit_pose_animation", path=path, ops=[
        {"op": "set_humanoid", "model": robot, "bones": {"left_lower_arm": ""}}])
    try:
        err = await ed.call("render_pose_animation", path=path, expect_error=True)
        for needle in ("left_lower_arm", "set_humanoid", "Missing slots"):
            ed.check_in(needle, err, "rig failure message")
    finally:
        await ed.call("undo", scope="project")
    ed.check(not (ed.project / (robot + ".humanoid")).exists(), "the sidecar is gone again")
    _, png = await render(ed, path=path, views=["front"])
    ed.check(len(png) > 0, "renders again once the map is fixed")
