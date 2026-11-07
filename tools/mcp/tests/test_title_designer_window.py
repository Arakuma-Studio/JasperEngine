"""S82 Title Designer through MCP (docs/specs/S82-title-designer.md AC30, AC34, AC35; R74, R77-R81;
jasper://docs/title-art "Title Designer"): focus_window's `title_designer` keys and `state`, the
screenshot view of the window, get_title's `designer` block, the window's actions producing the
documents edit_title produces, and an agent's edit while the document is open.
"""

import json
import re
import struct

import harness
from harness import Editor

CI = True
EDITOR = {"create": True, "name": "Title Designer"}
TIMEOUT = 600

S: dict = {}


async def setup(ed: Editor) -> dict:
    if S.get("ed") is ed:
        return S
    S.clear()
    S["ed"] = ed
    await ed.call("tool_groups", enable=["title_art", "editor_tools", "editor_ux"])
    await ed.call("set_project_settings", values={"title_art.auto_bake": False})
    return S


async def create(ed: Editor, path: str, text: str = "Title") -> None:
    await ed.call("edit_title", path=path, create={"text": text}, bake="never")


async def focus(ed: Editor, **keys) -> dict:
    return await ed.call("focus_window", window="title_designer", title_designer=keys)


def png_size(data: bytes) -> tuple[int, int]:
    return struct.unpack(">II", data[16:24])


def without_ids(text: str) -> dict:
    """A document with its ids replaced by their order of appearance (ids are random per op)."""
    doc = json.loads(text)
    seen: dict[str, str] = {}

    def walk(v):
        if isinstance(v, dict):
            out = {}
            for k, x in v.items():
                if k == "id" and isinstance(x, str) and re.fullmatch(r"[0-9a-f]{8}", x):
                    out[k] = seen.setdefault(x, f"#{len(seen)}")
                else:
                    out[k] = walk(x)
            return out
        if isinstance(v, list):
            return [walk(x) for x in v]
        if isinstance(v, str) and re.fullmatch(r"[0-9a-f]{8}", v) and v in seen:
            return seen[v]
        return v

    return walk(doc)


async def test_focus_window_keys_and_state(ed: Editor):
    """AC35 (R78): focus_window opens, selects, searches, zooms and returns `state` with `rect`;
    an unknown key fails naming the valid ones."""
    await setup(ed)
    path = "assets/designer/focus.titleart"
    await create(ed, path)
    await ed.call("edit_title", path=path, ops=[{"op": "add_effect", "layer": "Title", "type": "outer_glow"}])
    r = await focus(ed, path=path, select="Title/fx:outer_glow", panel="properties", search="glow", zoom="fit")
    state = r.get("state", {})
    ed.check_eq(state.get("path"), path, f"opened: {state}")
    ed.check_eq(state.get("selection"), ["Title/fx:outer_glow"], "selected the effect")
    ed.check_eq(state.get("search"), "glow", "typed into the search")
    ed.check_eq(state.get("zoom"), "fit", "zoom fit")
    ed.check("rect" in state and len(state["rect"]) == 4, f"the window's rect: {state}")
    for key in ("busy", "staged", "preview_active", "last_render_ms", "preview_as"):
        ed.check_in(key, state, f"state carries {key}")
    r = await focus(ed, select="", zoom=2, background="black", preview_as="baked")
    ed.check_eq(r["state"]["selection"], [], "an empty select is the document")
    ed.check_eq(r["state"]["background"], "black", "the backdrop")
    ed.check_eq(r["state"]["preview_as"], "baked", "Preview As")
    await focus(ed, preview_as="realtime", background="checker", zoom="fit")
    msg = await ed.check_error("focus_window", window="title_designer", title_designer={"zoon": 2})
    ed.check_in("zoom", msg, "the valid keys are listed")
    msg = await ed.check_error("focus_window", window="title_designer", title_designer={"select": "Nope"})
    ed.check_in("Title", msg, "a bad layer path lists the valid names")
    msg = await ed.check_error("focus_window", window="title_designer", title_designer={"device": {"profile": "nope"}})
    ed.check_in("iphone", msg, "an unknown device lists the profiles")


async def test_screenshot_view(ed: Editor):
    """AC35 (R79): screenshot view title_designer is the window's rect, as a person sees it."""
    await setup(ed)
    path = "assets/designer/shot.titleart"
    await create(ed, path, text="SHOT")
    r = await focus(ed, path=path, zoom="fit")
    rect = r["state"]["rect"]
    data = await ed.screenshot("title_designer", max_width=0)
    w, h = png_size(data)
    ed.check(abs(w - rect[2]) <= rect[2] * 0.02 + 2 or w > rect[2], f"the image is the window's width ({w} vs {rect[2]})")
    ed.check(h > 100, f"a real image: {w}x{h}")
    small = await ed.screenshot("title_designer", max_width=400)
    ed.check(png_size(small)[0] <= 400, "max_width scales it")


async def test_get_title_designer_block(ed: Editor):
    """AC35 (R80): get_title has `designer` for the document the window shows, and not for another."""
    await setup(ed)
    a, b = "assets/designer/a.titleart", "assets/designer/b.titleart"
    await create(ed, a)
    await create(ed, b)
    await focus(ed, path=a, select="Title")
    got = await ed.call("get_title", path=a)
    designer = got.get("designer")
    ed.check(designer is not None, f"designer while shown: {got.keys()}")
    ed.check_eq(designer["selection"], ["Title"], "its selection")
    for key in ("staged", "preview_active", "view"):
        ed.check_in(key, designer, f"designer carries {key}")
    other = await ed.call("get_title", path=b)
    ed.check("designer" not in other, "no designer block for a document the window does not show")


async def test_parity(ed: Editor):
    """AC34 (R77, §6.3): the window's actions (its catalog commands, run as a person's keys) and
    edit_title with the listed ops produce the same documents and step names."""
    await setup(ed)
    window_doc, agent_doc = "assets/designer/parity_w.titleart", "assets/designer/parity_a.titleart"
    for p in (window_doc, agent_doc):
        await create(ed, p)
    cases = [
        ("titleDesigner.duplicate", [{"op": "duplicate_layer", "layer": "Title"}], "Duplicate Layer"),
        ("titleDesigner.toggleVisible", [{"op": "set", "layer": "Title", "values": {"visible": False}}], "Hide Layer"),
        ("titleDesigner.toggleLock", [{"op": "set", "layer": "Title", "values": {"locked": True}}], "Lock Layer"),
        ("titleDesigner.group", [{"op": "group", "layers": ["Title"], "name": "Group"}], "Group Layers"),
    ]
    for command, ops, name in cases:
        await focus(ed, path=window_doc, select="Title" if command != "titleDesigner.group" else "Title")
        await ed.call("run_command", id=command)
        history = await ed.call("get_undo_history", scope="project", limit=1)
        steps = history.get("steps") or history.get("entries") or []
        # run_command marks an agent's steps "mcp: <name>": the window's own name follows
        top = steps[0].get("name", "") if steps else ""
        ed.check(top.split(": ", 1)[-1] == name, f"{command}: one step '{name}': {steps[:1]}")
        await ed.call("edit_title", path=agent_doc, ops=ops)
        w = without_ids((ed.project / window_doc).read_text())
        a = without_ids((ed.project / agent_doc).read_text())
        w.pop("guid", None)
        a.pop("guid", None)
        ed.check_eq(w, a, f"{command}: the same document as edit_title {ops}")
        if command == "titleDesigner.group":
            break


async def test_agent_edit_while_open(ed: Editor):
    """AC30 (R74): an edit_title while the document is open reloads it and keeps the selection by id;
    the agent's step is the one undo removes."""
    await setup(ed)
    path = "assets/designer/agent.titleart"
    await create(ed, path)
    await focus(ed, path=path, select="Title")
    await ed.call("edit_title", path=path, ops=[{"op": "set", "layer": "Title", "values": {"text": "AGENT", "size": 260}}])
    r = await focus(ed)
    ed.check_eq(r["state"]["selection"], ["Title"], "the selection is kept")
    ed.check_eq(r["state"]["staged"], False, "nothing staged")
    got = await ed.call("get_title", path=path)
    ed.check_eq(got["layers"][0]["text"], "AGENT", "reloaded")
    await ed.call("undo", scope="project")
    got = await ed.call("get_title", path=path)
    ed.check_eq(got["layers"][0]["text"], "Title", "undo removes the agent's step")


async def test_canvas_shows_edits(ed: Editor):
    """R28, R66 (visible): the window's image changes after an edit and is stable when idle."""
    await setup(ed)
    path = "assets/designer/canvas.titleart"
    await create(ed, path, text="CANVAS")
    await focus(ed, path=path, zoom="fit", background="black")
    await ed.call("editor_status")
    first = await ed.screenshot("title_designer", max_width=600)
    again = await ed.screenshot("title_designer", max_width=600)
    ed.check_eq(first, again, "an idle canvas gives the same image")
    await ed.call("edit_title", path=path, ops=[{"op": "set", "layer": "Title", "values": {"style.face.fill.color": "#ff2020"}}])
    changed = await ed.screenshot("title_designer", max_width=600)
    ed.check(changed != first, "the canvas shows the edit")
