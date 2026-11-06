# (c) Eduardo Doria and contributors
# SPDX-License-Identifier: MIT
"""Build Settings > Shaders without building (agent-authoring): edit_build_shaders adds, excludes,
removes and clears shader overrides in one project undo step, build_project shaders goes through
the same command (undoable now), and get_build_info reports Play's own C++ toolchain check."""

from harness import Editor

EDITOR = {"create": True, "name": "Build Shaders"}
CI = True


async def keys(ed: Editor) -> list[str]:
    sid = (await ed.call("create_scene", name="Shaded", type="3d"))["scene"]["id"]
    await ed.call("create_entity", scene=sid, name="Box", type="box")
    await ed.call("save_project", untitled_scene_dir="scenes")
    info = await ed.call("get_build_info", include_shaders=True)
    found = [s["key"] for s in info["shaders"]]
    ed.check(len(found) >= 1, f"the scene needs shaders: {info['shaders']}")
    return found


async def test_group_lists_the_tool(ed: Editor):
    default = {t.name for t in await ed.tools(groups=None)}
    ed.check("edit_build_shaders" not in default, "hidden until the shipping group is enabled")
    await ed.call("tool_groups", enable=["shipping"])
    ed.check("edit_build_shaders" in {t.name for t in await ed.tools(groups=None)}, "listed once enabled")
    await ed.call("tool_groups", disable=["shipping"])


async def test_edit_without_building(ed: Editor):
    """red before the change: overrides changed only through build_project, which always builds"""
    key = (await keys(ed))[0]
    listed = await ed.call("edit_build_shaders")
    ed.check(listed["added"] == [] and listed["excluded"] == [], f"none yet: {listed}")
    out = await ed.call("edit_build_shaders", exclude=[key])
    ed.check_eq([s["key"] for s in out["excluded"]], [key], "excluded")
    ed.check("undo" in out, f"one undo step: {out}")
    ed.check((await ed.call("get_build_info"))["running_job"] is None, "no build started")
    info = await ed.call("get_build_info", include_shaders=True)
    ed.check(any(s["key"] == key and s.get("excluded") for s in info["shaders"]), "get_build_info shows it excluded")
    ed.check_in("shaderExclusions", (ed.project / "project.yaml").read_text(), "saved to project.yaml")

    await ed.call("edit_build_shaders", remove=[key])
    ed.check_eq((await ed.call("edit_build_shaders"))["excluded"], [], "remove drops one override")
    await ed.call("undo", scope="project")
    ed.check_eq([s["key"] for s in (await ed.call("edit_build_shaders"))["excluded"]], [key], "undo brings it back")
    await ed.call("edit_build_shaders", clear=True)
    ed.check_eq((await ed.call("edit_build_shaders"))["excluded"], [], "clear")

    err = await ed.call("edit_build_shaders", add=["not-a-key"], expect_error=True)
    ed.check_in("Bad shader key", err, "a malformed key is refused")


async def test_build_project_shaders_is_undoable(ed: Editor):
    key = (await keys(ed))[0]
    out_dir = ed.scratch.root / "build-out"
    # a source export (no compiler): the overrides apply before it starts
    await ed.call("build_project", mode="source", output_dir=str(out_dir), overwrite=True, shaders={"exclude": [key]},
                  wait=False)
    ed.check_eq([s["key"] for s in (await ed.call("edit_build_shaders"))["excluded"]], [key], "build_project applied it")
    for _ in range(600):
        status = await ed.call("get_build_info")
        if status.get("running_job") is None:
            break
        await ed.call("wait", seconds=0.5)
    await ed.call("undo", scope="project")
    ed.check_eq((await ed.call("edit_build_shaders"))["excluded"], [], "one project undo step")


async def test_build_info_reports_plays_toolchain_check(ed: Editor):
    info = await ed.call("get_build_info")
    cpp = info["cpp_scripts"]
    ed.check(isinstance(cpp["can_build"], bool) and "missing_tools" in cpp and "kit" in cpp, f"{cpp}")
    ed.check_eq(cpp["can_build"], cpp["missing_tools"] == "", "can_build is no missing tools")
