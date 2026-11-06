"""The generic graph data model through MCP (S23 R21-R24, R44; AC15).

The editor started with JASPER_TEST_GRAPH_KIND=1 registers a "test" kind (speed / label / blend
nodes, float -> vector3 implicit). edit_graph connect + setParam is ONE undo step, validate_graph
reports a type-mismatch, dry_run writes nothing, a new error is refused unless allow_invalid,
and keys the model does not know survive an MCP edit.
"""

import asyncio
import json
import time

from harness import Editor

CI = True

TIMEOUT = 600

GRAPH = {
    "formatVersion": 1,
    "kind": "test",
    "kindVersion": 1,
    "nextId": 4,
    "nodes": [
        {"id": "n1", "type": "speed", "position": [0.0, 0.0], "params": {"value": 2.0}, "color": "teal"},
        {"id": "n2", "type": "blend", "position": [200.0, 0.0], "params": {"weight": 0.5}},
        {"id": "n3", "type": "label", "position": [0.0, 120.0], "params": {}},
    ],
    "edges": [],
    "variables": [],
    "groups": [],
    "viewport": {"offset": [0.0, 0.0], "zoom": 1.0},
    "metadata": {},
    "editorOnly": {"future": [1, 2, 3]},
}

PATH = "graphs/Blend.testgraph"


async def write_graph(ed: Editor, graph=GRAPH):
    # (the file as another program leaves it)
    target = ed.project / PATH
    target.parent.mkdir(parents=True, exist_ok=True)
    target.write_text(json.dumps(graph, indent=2) + "\n")


async def test_graph_tools():
    async with Editor(create=True, name="Graph CI", env={"JASPER_TEST_GRAPH_KIND": "1"}) as ed:
        kinds = await ed.call("list_graph_kinds")
        test = [k for k in kinds["kinds"] if k["id"] == "test"]
        ed.check(test, f"the test kind is registered: {kinds}")
        types = await ed.call("list_graph_node_types", kind="test")
        ed.check_eq(sorted(t["id"] for t in types["node_types"]), ["blend", "label", "speed"], "node types")
        unknown = await ed.call_raw("list_graph_node_types", {"kind": "nope"})
        ed.check(unknown.is_error and "list_graph_kinds" in unknown.text, f"an unknown kind: {unknown.text[:200]}")

        await write_graph(ed)
        got = await ed.call("get_graph", path=PATH)
        ed.check_eq(len(got["graph"]["nodes"]), 3, "three nodes")
        before_text = (ed.project / PATH).read_text()

        # dry run: the result, nothing written
        ops = [{"op": "connect", "from": {"node": "n1", "port": "value"}, "to": {"node": "n2", "port": "a"}},
               {"op": "setParam", "node": "n2", "name": "weight", "value": 0.75}]
        dry = await ed.call("edit_graph", path=PATH, ops=ops, dry_run=True)
        ed.check_eq(dry["dry_run"], True, "dry_run")
        ed.check_eq(len(dry["graph"]["edges"]), 1, "the dry run shows the edge")
        ed.check_eq((ed.project / PATH).read_text(), before_text, "dry_run wrote nothing")

        # the edit: one undo step
        r = await ed.call("edit_graph", path=PATH, ops=ops)
        ed.check_eq(r["applied"], 2, "two ops")
        ed.check_eq(r["undo_name"], "Edit Graph (2 Changes)", "the step's name")
        after = json.loads((ed.project / PATH).read_text())
        ed.check_eq(len(after["edges"]), 1, "the edge is written")
        ed.check_eq(after["nodes"][1]["params"]["weight"], 0.75, "setParam is written")
        ed.check_eq(after["editorOnly"], {"future": [1, 2, 3]}, "an unknown graph key survives the MCP edit")
        ed.check_eq(after["nodes"][0]["color"], "teal", "an unknown node key survives")
        status = await ed.call("editor_status")
        ed.check("Edit Graph" in (status["undo"]["undo_name"] or ""), f"the Edit menu names it: {status['undo']}")
        done = await ed.call("undo", scope="project")
        ed.check_eq(len(done["undone"]), 1, f"one step: {done}")
        reverted = json.loads((ed.project / PATH).read_text())
        ed.check_eq(reverted["edges"], [], "one undo removes the edge")
        ed.check_eq(reverted["nodes"][1]["params"]["weight"], 0.5, "and restores the param")
        ed.check_eq((ed.project / PATH).read_text(), before_text, "byte-identical to before the edit")

        # a string into a float input: refused, then allowed and reported
        bad = [{"op": "connect", "from": {"node": "n3", "port": "text"}, "to": {"node": "n2", "port": "a"}}]
        refused = await ed.call_raw("edit_graph", {"path": PATH, "ops": bad})
        ed.check(refused.is_error and "type-mismatch" in json.dumps(refused.structured or {}) + refused.text,
                 f"a new error is refused: {refused.text[:400]}")
        ed.check_eq((ed.project / PATH).read_text(), before_text, "the refused edit wrote nothing")
        await ed.call("edit_graph", path=PATH, ops=bad, allow_invalid=True)
        issues = await ed.call("validate_graph", path=PATH)
        mismatch = [i for i in issues["issues"] if i["code"] == "type-mismatch"]
        ed.check(mismatch, f"validate_graph reports the type-mismatch: {issues}")

        # a bad op stops the batch
        broken = await ed.call_raw("edit_graph", {"path": PATH, "ops": [{"op": "removeNode", "id": "n99"}]})
        ed.check(broken.is_error and "ops[0]" in broken.text, f"the failing op is named: {broken.text[:300]}")


# ---- BF23: what an agent's graph edits now say

STRICT_GRAPH = {
    "formatVersion": 1,
    "kind": "test_strict",
    "kindVersion": 1,
    "nextId": 2,
    "nodes": [{"id": "n1", "type": "speed", "position": [0.0, 0.0], "params": {"value": 2.0}}],
    "edges": [],
}

STRICT_PATH = "graphs/Strict.strictgraph"


async def test_strict_kind_refuses_typos():
    """F5, F9: on a strict kind a misspelled parameter or node type and an out-of-range value are
    errors of the edit, each with what to do instead; a file holding them still loads."""
    async with Editor(create=True, name="Graph Strict CI", env={"JASPER_TEST_GRAPH_KIND": "1"}) as ed:
        target = ed.project / STRICT_PATH
        target.parent.mkdir(parents=True, exist_ok=True)
        target.write_text(json.dumps(STRICT_GRAPH, indent=2) + "\n")
        before = target.read_text()

        typo = await ed.call_raw("edit_graph", {"path": STRICT_PATH, "ops": [{"op": "setParam", "node": "n1", "name": "valeu", "value": 3}]})
        ed.check(typo.is_error, f"a misspelled parameter is refused: {typo.text[:300]}")
        errors = (typo.structured or {}).get("new_errors", [])
        ed.check_eq([e["code"] for e in errors], ["unknown-param"], f"its code: {errors}")
        ed.check("did you mean 'value'" in errors[0].get("suggestion", ""), f"its suggestion: {errors}")
        ed.check("did you mean 'value'" in typo.text, f"the text says so too: {typo.text[:300]}")
        ed.check_eq(target.read_text(), before, "the refused typo wrote nothing")

        node = await ed.call_raw("edit_graph", {"path": STRICT_PATH, "ops": [{"op": "addNode", "node": {"type": "sped"}}]})
        ed.check(node.is_error, f"a misspelled node type is refused: {node.text[:300]}")
        errors = (node.structured or {}).get("new_errors", [])
        ed.check_eq([e["code"] for e in errors], ["unknown-node-type"], f"its code: {errors}")
        ed.check("did you mean 'speed'" in errors[0].get("suggestion", ""), f"its suggestion: {errors}")

        far = await ed.call_raw("edit_graph", {"path": STRICT_PATH, "ops": [{"op": "setParam", "node": "n1", "name": "value", "value": 500}]})
        ed.check(far.is_error and "param-range" in json.dumps(far.structured or {}), f"an out-of-range value is refused: {far.text[:300]}")
        ed.check_eq(target.read_text(), before, "the refused value wrote nothing")

        # a second typo on a node that holds one already is still new
        await ed.call("edit_graph", path=STRICT_PATH, ops=[{"op": "setParam", "node": "n1", "name": "valeu", "value": 3}], allow_invalid=True)
        second = await ed.call_raw("edit_graph", {"path": STRICT_PATH, "ops": [{"op": "setParam", "node": "n1", "name": "vlaue", "value": 3}]})
        ed.check(second.is_error, f"a second typo on the node is refused: {second.text[:300]}")

        # the file holds the typo: it loads, says so, and edits that add nothing bad go on
        got = await ed.call("get_graph", path=STRICT_PATH)
        ed.check_eq(got["graph"]["nodes"][0]["params"]["valeu"], 3, "load keeps the unknown parameter")
        issues = await ed.call("validate_graph", path=STRICT_PATH)
        ed.check([i for i in issues["issues"] if i["code"] == "unknown-param" and i["severity"] == "error"], f"validate_graph: {issues}")
        await ed.call("edit_graph", path=STRICT_PATH, ops=[{"op": "setParam", "node": "n1", "name": "value", "value": 3}])

        # a kind that did not opt in keeps S23's forward compatibility
        await write_graph(ed)
        await ed.call("edit_graph", path=PATH, ops=[{"op": "setParam", "node": "n2", "name": "wieght", "value": 0.7}])
        ed.check_eq(json.loads((ed.project / PATH).read_text())["nodes"][1]["params"]["wieght"], 0.7, "a non-strict kind takes it")


async def test_new_ids_come_back():
    """F6: edit_graph says which id each new node, edge and group got."""
    async with Editor(create=True, name="Graph Ids CI", env={"JASPER_TEST_GRAPH_KIND": "1"}) as ed:
        await write_graph(ed)
        ops = [{"op": "addNode", "node": {"type": "speed", "position": [0, 300]}},
               {"op": "moveNodes", "ids": ["n1"], "delta": [1, 1]},
               {"op": "addNode", "node": {"type": "blend", "position": [200, 300]}},
               {"op": "connect", "from": {"node": "n1", "port": "value"}, "to": {"node": "n2", "port": "a"}},
               {"op": "group", "title": "Inputs", "nodes": ["n1"]}]
        dry = await ed.call("edit_graph", path=PATH, ops=ops, dry_run=True)
        ed.check_eq(dry["added"], [{"op_index": 0, "id": "n4"}, {"op_index": 2, "id": "n5"}, {"op_index": 3, "id": "e6"}, {"op_index": 4, "id": "g7"}],
                    "the ids a dry run would allocate")
        done = await ed.call("edit_graph", path=PATH, ops=ops)
        ed.check_eq(done["added"], dry["added"], "the real edit allocates the same ones")
        written = json.loads((ed.project / PATH).read_text())
        ed.check({"n4", "n5"} <= {n["id"] for n in written["nodes"]}, f"in the file: {written['nodes']}")
        ed.check_eq([e["id"] for e in written["edges"]], ["e6"], "the edge's id")
        ed.check_eq([g["id"] for g in written["groups"]], ["g7"], "the group's id")

        # an edit that adds nothing has none; a refused one has no ids to use
        moved = await ed.call("edit_graph", path=PATH, ops=[{"op": "moveNodes", "ids": ["n1"], "delta": [1, 1]}])
        ed.check_eq(moved["added"], [], "nothing added")
        bad = [{"op": "connect", "from": {"node": "n3", "port": "text"}, "to": {"node": "n5", "port": "a"}}]
        refused = await ed.call_raw("edit_graph", {"path": PATH, "ops": bad})
        ed.check(refused.is_error and "added" not in (refused.structured or {}), f"refused: {refused.structured}")


async def test_supplied_id_gets_defaults():
    """F7: a node added with its own id gets its type's defaults, and the undo is still exact."""
    async with Editor(create=True, name="Graph Defaults CI", env={"JASPER_TEST_GRAPH_KIND": "1"}) as ed:
        await write_graph(ed)
        before = (ed.project / PATH).read_text()
        ops = [{"op": "addNode", "node": {"id": "bricks", "type": "speed", "position": [0, 300]}},
               {"op": "connect", "from": {"node": "bricks", "port": "value"}, "to": {"node": "n2", "port": "a"}}]
        r = await ed.call("edit_graph", path=PATH, ops=ops)
        ed.check_eq(r["applied"], 2, "added and connected in one batch")
        after = (ed.project / PATH).read_text()
        node = next(n for n in json.loads(after)["nodes"] if n["id"] == "bricks")
        ed.check_eq(node["params"].get("value"), 1.0, f"the type's default: {node}")
        await ed.call("undo", scope="project")
        ed.check_eq((ed.project / PATH).read_text(), before, "undo is byte-identical")
        await ed.call("redo", scope="project")
        ed.check_eq((ed.project / PATH).read_text(), after, "redo is byte-identical")


async def test_hand_written_edges_get_ids():
    """F8: edges written without ids load with distinct ones; the first save keeps them."""
    async with Editor(create=True, name="Graph Edges CI", env={"JASPER_TEST_GRAPH_KIND": "1"}) as ed:
        graph = json.loads(json.dumps(GRAPH))
        graph["edges"] = [{"from": {"node": "n1", "port": "value"}, "to": {"node": "n2", "port": "a"}},
                          {"from": {"node": "n1", "port": "value"}, "to": {"node": "n2", "port": "b"}}]
        await write_graph(ed, graph)
        issues = await ed.call("validate_graph", path=PATH)
        ed.check(not [i for i in issues["issues"] if i["code"] == "duplicate-id"], f"no duplicate id: {issues}")
        got = await ed.call("get_graph", path=PATH)
        ids = [e["id"] for e in got["graph"]["edges"]]
        ed.check(len(set(ids)) == 2 and all(ids), f"two distinct ids: {ids}")
        await ed.call("edit_graph", path=PATH, ops=[{"op": "disconnect", "id": ids[0]}])
        ed.check_eq([e["id"] for e in json.loads((ed.project / PATH).read_text())["edges"]], [ids[1]], "the other edge keeps its id")


async def test_graph_files_are_text_and_tracked():
    """F11: a registered kind's extension, any kind's, is a text file and a tracked one."""
    async with Editor(create=True, name="Graph Files CI", env={"JASPER_TEST_GRAPH_KIND": "1"}) as ed:
        await write_graph(ed)
        for path in (PATH, "graphs/Other.strictgraph"):
            kind = "test_strict" if path.endswith(".strictgraph") else "test"
            written = await ed.call_raw("write_file", {"path": path, "content": json.dumps({**GRAPH, "kind": kind}, indent=2) + "\n"})
            ed.check(not written.is_error, f"write_file {path}: {written.text[:300]}")
            read = await ed.call_raw("read_file", {"path": path})
            ed.check(not read.is_error and "formatVersion" in read.text, f"read_file {path}: {read.text[:300]}")
        unknown = await ed.call_raw("write_file", {"path": "graphs/Other.nokindgraph", "content": "{}"})
        ed.check(unknown.is_error, f"no kind has this extension: {unknown.text[:300]}")
        notjson = await ed.call_raw("write_file", {"path": PATH, "content": "{not json"})
        ed.check(notjson.is_error and "not valid JSON" in notjson.text, f"a graph file must stay loadable: {notjson.text[:300]}")

        # another program's edit: its problems come with the next call
        async def problems() -> tuple[list, dict]:
            r = await ed.call_raw("editor_status")
            ext = (r.structured if isinstance(r.structured, dict) else {}).get("external_changes") or {}
            return [p for p in ext.get("problems", []) if p.get("path") == PATH], ext

        target = ed.project / PATH
        broken = json.loads(json.dumps(GRAPH))
        broken["edges"] = [{"id": "e9", "from": {"node": "n1", "port": "value"}, "to": {"node": "n404", "port": "a"}}]
        time.sleep(0.05)
        target.write_text(json.dumps(broken, indent=2) + "\n")
        found, ext = await problems()
        ed.check(found and "n404" in found[0]["message"], f"the dangling edge is reported: {ext}")
        time.sleep(0.05)
        target.write_text("{not json")
        found, ext = await problems()
        ed.check(found and "not valid JSON" in found[0]["message"], f"a file that is not a graph is reported: {ext}")
        time.sleep(0.05)
        await write_graph(ed)
        await problems()

        # the tool's own write is not another program's
        await ed.call("edit_graph", path=PATH, ops=[{"op": "setParam", "node": "n2", "name": "weight", "value": 0.25}])
        for _ in range(3):
            found, ext = await problems()
            ed.check(PATH not in ext.get("changed", []), f"edit_graph's own write: {ext}")
            await asyncio.sleep(0.25)


async def test_an_existing_error_that_changes_is_not_new():
    """Fix round 1 of BF23: only a parameter's error is told apart by its message. An edit that shrinks a
    cycle adds nothing, on a kind that did not opt in; a new bad value on a strict kind is still new."""
    async with Editor(create=True, name="Graph Keys CI", env={"JASPER_TEST_GRAPH_KIND": "1"}) as ed:
        graph = json.loads(json.dumps(GRAPH))
        graph["nodes"] = [{"id": n, "type": "blend", "position": [0.0, 0.0], "params": {}} for n in ("n1", "n2", "n3")]
        graph["edges"] = [{"id": "e4", "from": {"node": "n1", "port": "out"}, "to": {"node": "n2", "port": "a"}},
                          {"id": "e5", "from": {"node": "n2", "port": "out"}, "to": {"node": "n1", "port": "a"}},
                          {"id": "e6", "from": {"node": "n2", "port": "out"}, "to": {"node": "n3", "port": "a"}}]
        graph["nextId"] = 7
        await write_graph(ed, graph)
        issues = await ed.call("validate_graph", path=PATH)
        ed.check([i for i in issues["issues"] if i["code"] == "cycle" and "n3" in i["message"]], f"the cycle's message lists n3: {issues}")
        shrink = await ed.call_raw("edit_graph", {"path": PATH, "ops": [{"op": "disconnect", "id": "e6"}]})
        ed.check(not shrink.is_error, f"the edit adds no error: {shrink.text[:300]}")
        ed.check_eq([e["id"] for e in json.loads((ed.project / PATH).read_text())["edges"]], ["e4", "e5"], "it was written")

        target = ed.project / STRICT_PATH
        target.parent.mkdir(parents=True, exist_ok=True)
        target.write_text(json.dumps(STRICT_GRAPH, indent=2) + "\n")
        far = [{"op": "setParam", "node": "n1", "name": "value", "value": 500}]
        await ed.call("edit_graph", path=STRICT_PATH, ops=far, allow_invalid=True)
        farther = await ed.call_raw("edit_graph", {"path": STRICT_PATH, "ops": [{"op": "setParam", "node": "n1", "name": "value", "value": 600}]})
        ed.check(farther.is_error, f"another bad value is new: {farther.text[:300]}")


async def test_a_graph_file_is_of_its_kind():
    """Fix round 1 of BF23: a file whose kind is not its extension's, or whose lists are not lists, is not
    that kind's file: write_file and edit_graph refuse it, another program's edit reports it."""
    async with Editor(create=True, name="Graph Kind CI", env={"JASPER_TEST_GRAPH_KIND": "1"}) as ed:
        path = "graphs/Other.strictgraph"
        target = ed.project / path
        target.parent.mkdir(parents=True, exist_ok=True)
        good = json.dumps(STRICT_GRAPH, indent=2) + "\n"
        await ed.call("write_file", path=path, content=good)
        for content, fragment in (("{}", "names no kind"),
                                  (json.dumps({**STRICT_GRAPH, "kind": "tes"}), "'test_strict'"),
                                  (json.dumps({**STRICT_GRAPH, "kind": "test"}), "'test_strict'"),
                                  (json.dumps({**STRICT_GRAPH, "nodes": "oops"}), "'nodes'")):
            refused = await ed.call_raw("write_file", {"path": path, "content": content})
            ed.check(refused.is_error and fragment in refused.text, f"{content[:40]}: {refused.text[:300]}")
            ed.check_eq(target.read_text(), good, f"{content[:40]} wrote nothing")

        async def report() -> dict:
            r = await ed.call_raw("editor_status")
            return (r.structured if isinstance(r.structured, dict) else {}).get("external_changes") or {}

        for content, fragment in ((json.dumps({**STRICT_GRAPH, "kind": "test"}), "'test_strict'"),
                                  (json.dumps({**STRICT_GRAPH, "kindVersion": 99}), "newer than this build")):
            time.sleep(0.05)
            target.write_text(content)
            ext = await report()
            found = [p for p in ext.get("problems", []) if p.get("path") == path and fragment in p.get("message", "")]
            ed.check(found, f"{content[:60]}: {ext}")

        # edit_graph does not rewrite such a file either: the strict check would be skipped, a value lost
        for content in (json.dumps({**STRICT_GRAPH, "kind": "test"}), json.dumps({**STRICT_GRAPH, "groups": "oops"})):
            time.sleep(0.05)
            target.write_text(content)
            refused = await ed.call_raw("edit_graph", {"path": path, "ops": [{"op": "moveNodes", "ids": ["n1"], "delta": [1, 1]}]})
            ed.check(refused.is_error, f"edit_graph on {content[:40]}: {refused.text[:300]}")
            ed.check_eq(target.read_text(), content, "nothing written")

