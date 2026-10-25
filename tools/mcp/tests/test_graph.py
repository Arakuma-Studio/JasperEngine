"""The generic graph data model through MCP (S23 R21-R24, R44; AC15).

The editor started with JASPER_TEST_GRAPH_KIND=1 registers a "test" kind (speed / label / blend
nodes, float -> vector3 implicit). edit_graph connect + setParam is ONE undo step, validate_graph
reports a type-mismatch, dry_run writes nothing, a new error is refused unless allow_invalid,
and keys the model does not know survive an MCP edit.
"""

import json

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
    await ed.call("write_file", path=PATH, content=json.dumps(graph, indent=2) + "\n")


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
