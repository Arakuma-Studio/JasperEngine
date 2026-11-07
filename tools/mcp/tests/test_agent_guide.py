# (c) Eduardo Doria and contributors
# SPDX-License-Identifier: MIT
"""The agent guide for clients that cut it, and the authoring hints in results (agent-authoring).

Claude Code keeps the first 4096 characters of the server instructions: the guide puts the intro
(with the pointer to the rest), Addressing, The loop, Building scenes and the Tool groups line
first, and read_file reads every jasper:// resource (the whole guide, the docs) for clients without
MCP resources. Results say where a HUD and repeated objects belong (next)."""

from harness import Editor

EDITOR = {"create": True, "name": "Agent Guide"}
CI = True

CUT = 4096


def utf16_len(text: str) -> int:
    return len(text.encode("utf-16-le")) // 2


def cut(text: str) -> str:
    """What a client that keeps CUT characters (JavaScript's length) sees."""
    out, units = [], 0
    for ch in text:
        units += 2 if ord(ch) > 0xFFFF else 1
        if units > CUT:
            break
        out.append(ch)
    return "".join(out)


async def test_guide_survives_the_client_cut(ed: Editor):
    """red before the change: Building scenes and the pointer did not exist, Tool groups came last"""
    guide = ed.instructions
    ed.check(utf16_len(guide) > CUT, "the guide is longer than the cut (else this test proves nothing)")
    kept = cut(guide)
    for needle in ("read_file jasper://guide", "## Addressing", "## The loop", "## Building scenes", "## Tool groups",
                   "tool_groups"):
        ed.check_in(needle, kept, "inside the first 4096 characters")
    ed.check("\n## Other areas" in guide and "\n## Other areas" not in kept,
             "the pointer names the last section, which a cut client does not see")


async def test_read_file_reads_resources(ed: Editor):
    """red before the change: read_file refused jasper:// paths"""
    whole = await ed.call_raw("read_file", {"path": "jasper://guide", "max_lines": 20000})
    ed.check(not whole.is_error, f"read_file jasper://guide: {whole.text[:300]}")
    ed.check_in(ed.instructions.splitlines()[-1], whole.text, "the whole guide, to its last line")
    ed.check_eq(whole.value["path"], "jasper://guide", "structured path")
    authoring = await ed.call_raw("read_file", {"path": "jasper://docs/authoring"})
    ed.check_in("Build once, as a Prefab", authoring.text, "the authoring doc")
    terrain = await ed.call_raw("read_file", {"path": "jasper://docs/terrain"})
    ed.check_in("edit_terrain_details", terrain.text, "the terrain doc")
    window = await ed.call("read_file", path="jasper://docs/authoring", start_line=3, max_lines=2)
    ed.check(window["start_line"] == 3 and window["lines"] == 2, f"a line window: {window}")
    err = await ed.call("read_file", path="jasper://docs/nope", expect_error=True)
    ed.check_in("Documents:", err, "an unknown document lists the documents")


async def test_prompt_builds_scenes_in_the_editor(ed: Editor):
    got = await ed.session.get_prompt("new_2d_game", {"idea": "a coin collector"})
    text = got.messages[0].content.text
    for needle in ("set_child_scene", "create_prefab", "edit_animation", "jasper://docs/authoring"):
        ed.check_in(needle, text, "the prompt's authoring rules")


async def test_results_point_to_prefabs_and_ui_scenes(ed: Editor):
    sid = (await ed.call("create_scene", name="Level", type="3d"))["scene"]["id"]
    coin = (await ed.call("create_entity", scene=sid, name="Coin", type="box"))["entity_id"]
    copies = await ed.call("duplicate_entities", scene=sid, entities=[coin])
    ed.check_in("create_prefab", copies.get("next", ""), "copies of a plain GameObject suggest a Prefab")
    hud = await ed.call("create_ui", scene=sid, root={"type": "text", "name": "Score", "text": "Score: 0"})
    ed.check_in("set_child_scene", hud.get("next", ""), "UI widgets in a 3D scene point to a UI scene")
    ui = (await ed.call("create_scene", name="Hud", type="ui"))["scene"]["id"]
    in_ui = await ed.call("create_ui", scene=ui, root={"type": "text", "name": "Score", "text": "Score: 0"})
    ed.check("next" not in in_ui, f"no hint in a UI scene: {in_ui}")
