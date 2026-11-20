"""Dialogue graphs through MCP (docs/specs/dialogue-graphs.md R25, R26, AC11, AC15), on a new project:

- run_dialogue is in the `graphs` group: hidden by default, listed once tool_groups enables it;
- a .dialogue made and edited with edit_graph (namespace and GUID from create, option ports from params);
- validate_graph reports each seeded lint defect once;
- run_dialogue walks with choices and condition overrides, and explore finds the endings and a dead loop;
- in Play, run_dialogue {entity} drives a Dialogue Runner whose Lua handler logs to the Console;
- the `dialogue` HTML UI template plays its runner: Space completes a line, the next one shows;
- (JASPER_SLOW_BUILD=1) a prebuilt Linux export prints the transcript run_dialogue predicts.
"""

from __future__ import annotations

import asyncio
import json
import os
import subprocess
import time

import build_cache
import harness
from harness import Editor, skip

CI = True
TIMEOUT = 900
PREBUILT_PLAYERS = ["glcore-release"]
EDITOR = {"create": True, "name": "Dialogues", "env": build_cache.player_env("glcore", "release")}

GUARD = "assets/dialogue/guard.dialogue"
S: dict = {}


def line(node_id: str, text: str, speaker: str = "guard", options: list | None = None, **extra) -> dict:
    params = {"speaker": speaker, "text": text}
    if options is not None:
        params["options"] = options
        params["next_sub_id"] = len(options) + 1
    params.update(extra)
    return {"op": "addNode", "node": {"id": node_id, "type": "line", "position": [0, 0], "params": params}}


def node(node_id: str, kind: str, **params) -> dict:
    return {"op": "addNode", "node": {"id": node_id, "type": kind, "position": [0, 0], "params": params}}


def wire(source: str, port: str, target: str) -> dict:
    return {"op": "connect", "from": {"node": source, "port": port}, "to": {"node": target, "port": "in"}}


async def test_group_hidden_until_enabled(ed: Editor):
    await ed.call("get_project_summary")
    default = {t.name for t in await ed.tools(groups=None)}
    ed.check("run_dialogue" not in default, "graphs is hidden by default")
    await ed.call("tool_groups", enable=["graphs"])
    ed.check("run_dialogue" in {t.name for t in await ed.tools(groups=None)}, "listed once tool_groups enables graphs")
    types = await ed.call("list_graph_node_types", kind="dialogue")
    ed.check_eq(sorted(s.split("(")[0] for s in types["signatures"]), ["action", "branch", "end", "entry", "jump", "line", "set"],
                f"the dialogue node types: {types['signatures']}")


async def test_create_and_edit_with_edit_graph(ed: Editor):
    made = await ed.call("edit_graph", path=GUARD, create={"kind": "dialogue"}, ops=[
        node("n1", "entry", name="start", arguments="player"),
        node("n2", "set", assign="$gold = 10"),
        line("n3", "Halt, {player}! The toll is five coins.", options=[
            {"id": "o1", "text": "Pay", "condition": "$gold >= 5", "action": "$gold -= 5; quest.start:gate", "style": "accept"},
            {"id": "o2", "text": "Bribe", "condition": "shop.canAfford(20)", "when_false": "disable"},
            {"id": "o3", "text": "Leave", "style": "exit"}]),
        node("n4", "branch", condition="quest.done:gate"),
        line("n5", "Go on through."),
        line("n6", "Come back with the papers."),
        node("n7", "end", result="paid"),
        node("n8", "end", result="left"),
        wire("n1", "next", "n2"), wire("n2", "next", "n3"),
        wire("n3", "o1", "n4"), wire("n3", "o2", "n5"), wire("n3", "o3", "n8"),
        wire("n4", "true", "n5"), wire("n4", "false", "n6"), wire("n5", "next", "n7"), wire("n6", "next", "n8"),
        {"op": "addVariable", "variable": {"name": "gold", "type": "number", "default": 0}}])
    ed.check_eq(made["applied"], 18, f"every op applied: {made}")
    text = json.loads((ed.project / GUARD).read_text(encoding="utf-8"))
    ed.check_eq(text["metadata"]["namespace"], "guard", "the namespace comes from the file name")
    ed.check(text.get("guid"), "the file carries its GUID")
    ports = [p["id"] for n in text["nodes"] if n["id"] == "n3" for p in n.get("ports", [])]
    ed.check_eq(ports, ["in", "next", "o1", "o2", "o3"], "one output port per option")
    # an option added later gets its port: a wire to it is no dangling edge
    g = (await ed.call("get_graph", path=GUARD))["graph"]
    options = [n for n in g["nodes"] if n["id"] == "n3"][0]["params"]["options"]
    options.append({"id": "o4", "text": "Ask about the town"})
    edited = await ed.call("edit_graph", path=GUARD, ops=[
        {"op": "setParam", "node": "n3", "name": "options", "value": options},
        {"op": "setParam", "node": "n3", "name": "next_sub_id", "value": 5},
        wire("n3", "o4", "n6")])
    ed.check_eq(edited["applied"], 3, f"the option and its wire: {edited}")
    # a text edit is one undo step
    await ed.call("undo", scope="timeline")
    g = (await ed.call("get_graph", path=GUARD))["graph"]
    ed.check_eq(len([n for n in g["nodes"] if n["id"] == "n3"][0]["params"]["options"]), 3, "undone in one step")


async def test_validate_graph_dialogue_lint(ed: Editor):
    path = "assets/dialogue/broken.dialogue"
    await ed.call("edit_graph", path=path, create={"kind": "dialogue"}, allow_invalid=True, ops=[
        node("n1", "entry", name="start"),
        line("n2", "Hello {stranger}.", options=[{"id": "o1", "text": "Again"}, {"id": "o2", "text": "Again"}]),
        node("n3", "set", assign="$visits_count += 1"),
        node("n4", "branch", condition="$x >"),
        line("n5", "Nobody comes here."),
        node("n6", "end"), node("n7", "end"),
        wire("n1", "next", "n2"), wire("n2", "o1", "n3"), wire("n2", "o2", "n3"), wire("n3", "next", "n2"),
        wire("n5", "next", "n6"), wire("n5", "next", "n7")])
    issues = (await ed.call("validate_graph", path=path))["issues"]
    codes = sorted(i["code"] for i in issues)
    for code in ("no-exit", "unreachable", "fan-out", "bad-expression", "unknown-variable", "unknown-argument", "duplicate-option-text"):
        ed.check(code in codes, f"{code} is reported: {codes}")
    ed.check_eq(codes.count("fan-out"), 1, f"the fan-out once: {codes}")
    ed.check_eq(codes.count("no-exit"), 1, f"the loop once: {codes}")
    bad = [i for i in issues if i["code"] == "bad-expression"][0]
    ed.check("column" in bad["message"], f"the parse error names its column: {bad}")


async def test_run_dialogue_walk_choices_and_conditions(ed: Editor):
    walked = await ed.call("run_dialogue", path=GUARD, choices=[1])
    ed.check_eq(walked["state"]["state"], "ended", f"pay leads to an end: {walked}")
    first = walked["transcript"][0]
    ed.check_eq(first["key"], "guard.n3", "the line's key")
    ed.check(first["text"].startswith("Halt, {player}!") or "player" in first["text"], f"no player argument passed: {first['text']}")
    ed.check(not first.get("actions"), "no action before the first line")
    ed.check_in("quest.done(\"gate\")", walked.get("unknown_calls", []), "an unlisted condition is reported")
    ed.check("conditions" in walked.get("next", ""), f"the next hint says how to take the other branch: {walked}")
    ed.check_eq(walked["transcript"][-1]["node"], "n6", "quest.done was false: the papers line")
    ed.check_eq(walked["variables"]["gold"], 5, "the choice's action ran on the walk's store")
    walked = await ed.call("run_dialogue", path=GUARD, choices=[1], conditions={"quest.*": True}, variables={"player": "Ana"})
    ed.check_eq(walked["transcript"][-1]["node"], "n5", "quest.* true: through the gate")
    ed.check_eq(walked["state"]["result"], "paid", f"the end's result: {walked['state']}")
    ed.check("Ana" in walked["transcript"][0]["text"], f"a variable named like the argument fills it: {walked['transcript'][0]}")
    menu = await ed.call("run_dialogue", path=GUARD)
    ed.check_eq(menu["state"]["state"], "line", "no choices: it stops at the menu")
    disabled = [o for o in menu["state"]["options"] if o.get("enabled") is False]
    ed.check_eq([o["id"] for o in disabled], ["o2"], f"the disabled Bribe option is listed greyed: {menu['state']}")
    r = await ed.call_raw("run_dialogue", {"path": GUARD, "entity": 1})
    ed.check(r.is_error and "exactly one" in r.text, f"path and entity together are refused: {r.text}")


async def test_run_dialogue_explore_finds_endings_and_dead_loops(ed: Editor):
    explored = await ed.call("run_dialogue", path=GUARD, explore=True)
    results = sorted(e["result"] for e in explored["endings"])
    ed.check_eq(results, ["left", "paid"], f"both endings: {explored['endings']}")
    paid = [e for e in explored["endings"] if e["result"] == "paid"][0]
    # paying, or bribing once shop.canAfford is taken true
    ed.check(paid["choices"] in ([1], [2]), f"the choices to the paid ending: {paid}")
    ed.check_eq(explored["no_exit_loops"], [], "the guard has no dead loop")
    loop = await ed.call("run_dialogue", path="assets/dialogue/broken.dialogue", explore=True, max_paths=32)
    ed.check(loop["no_exit_loops"] or loop.get("errors"), f"the endless menu is found: {loop}")
    ed.check_in("n5", loop["lines_never_shown"], f"the unreachable line is never shown: {loop}")


async def test_run_dialogue_live_runner_in_play(ed: Editor):
    made = await ed.call("create_scene", name="Gate", type="3D")
    scene = made["scene"]["id"]
    ent = await ed.call("create_entity", scene=scene, name="Guard")
    await ed.call("add_component", scene=scene, entity="Guard", component="dialoguerunner",
                  values={"dialoguerunner": {"dialogue": GUARD}})
    await ed.call("create_script", kind="lua", name="GateQuests", entity="Guard", scene=scene, content="""---@class GateQuests : ScriptInstance
local GateQuests = { properties = {} }

function GateQuests:init()
    Dialogue.onAction("quest", function(runner, verb, args)
        Log.print("quest action " .. verb .. " " .. tostring(args[1]))
    end)
    Dialogue.onCondition("quest", function(runner, verb, args) return true end)
end

return GateQuests
""")
    await ed.call("save_scene", scene=scene, path="scenes/Gate")
    ed.check(await ed.call("play", action="start", scene=scene, fixed_fps=60), "play started")
    await ed.call("wait", frames=3)
    await ed.console_since(None, limit=1)
    live = await ed.call("run_dialogue", entity=ent["entity_id"], entry="start", choices=[1])
    ed.check_eq(live["state"]["state"], "line", f"the live runner shows the next line: {live}")
    ed.check_eq(live["state"]["node"], "n5", "the real handler answered quest.done true")
    messages = [e.get("message", "") for e in await ed.console_since(limit=200)]
    ed.check(any("quest action start gate" in m for m in messages), f"the Lua handler logged: {messages[-10:]}")
    state = await ed.call("get_runtime_entity", entity=ent["entity_id"], components=["dialoguerunner"])
    runner = state["components"]["dialoguerunner"]
    ed.check_eq((runner["state"], runner["currentNode"], runner["isRunning"]), ("line", "n5", True), f"the runtime state: {runner}")
    await ed.call("play", action="stop")


async def test_dialogue_template_runs_a_runner(ed: Editor):
    made = await ed.call("create_scene", name="Talk", type="2D")
    scene = made["scene"]["id"]
    created = await ed.call("create_ui_document", scene=scene, name="Talk", html="assets/ui/talk.html", template="dialogue", script=True)
    entity = created["entity"]["id"]
    S["talk_scene"], S["talk_entity"] = scene, entity
    ed.check((ed.project / "assets/ui/talk_assets/demo.dialogue").exists(), "the demo conversation beside the document")
    demo = json.loads((ed.project / "assets/ui/talk_assets/demo.dialogue").read_text(encoding="utf-8"))
    ed.check_eq(demo["metadata"]["namespace"], "talk_demo", "its namespace follows the document")
    await ed.call("save_scene", scene=scene, path="scenes/Talk")
    ed.check(await ed.call("play", action="start", scene=scene, fixed_fps=60), "play started")
    await ed.call("wait", frames=240)
    state = await ed.call("get_runtime_entity", entity=entity, components=["dialoguerunner"])
    ed.check_eq(state["components"]["dialoguerunner"]["currentNode"], "n2", "the template's runner shows the first line")
    # a choice, then Space completes the new line
    await ed.call("send_input", events=[{"type": "mouse", "action": "click", "target": ".choice:nth-child(2)", "document": "Talk"}], frames_after=10)
    await ed.call("send_input", events=[{"type": "key", "key": "space"}], frames_after=2)
    tree = await ed.call("inspect_ui", entity=entity, selector="#line", detail="full")
    text = str(tree["elements"][0])
    ed.check("Good eye." in text and "light low." in text, f"the second line, completed: {text[:300]}")
    await ed.call("play", action="stop")


async def test_export_runs_dialogue(ed: Editor):
    if os.environ.get("JASPER_SLOW_BUILD", "") in ("", "0"):
        skip("set JASPER_SLOW_BUILD=1 to build and run a real Desktop player (the run builds its prebuilt runtime once)")
    demo = "assets/ui/talk_assets/demo.dialogue"
    predicted = await ed.call("run_dialogue", path=demo, choices=[2], seed=7)
    expected = [step["text"] for step in predicted["transcript"]]
    await ed.call("create_script", kind="lua", name="Transcript", scene=S["talk_scene"], entity=S["talk_entity"], content="""---@class Transcript : ScriptInstance
local Transcript = { properties = {} }

function Transcript:init()
    self.runner = DialogueRunner(self.scene, self.entity)
    self.runner.seed = 7
    self.runner.onLine:add("transcript", function(runner, line) Log.print("TRANSCRIPT " .. line.text) end)
    self.frames = 0
    RegisterEngineEvent(self, "onUpdate")
end

function Transcript:onUpdate()
    self.frames = self.frames + 1
    if self.frames == 30 then
        self.runner:start()
        self.runner:choose(2)
        self.runner:advance()
        self.runner:advance()
    end
end

return Transcript
""")
    saved = await ed.call("save_scene", scene=S["talk_scene"])
    await ed.call("edit_build_scenes", action="add", scene=saved["scene"]["path"])
    await ed.call("edit_build_scenes", action="set_start", scene=saved["scene"]["path"])
    runtime = await build_cache.player_runtime(ed, "glcore", "release")
    out = ed.scratch.root / "export-dialogue"
    job = await ed.call("build_project", mode="desktop", output_dir=str(out), method="prebuilt", timeout_seconds=600)
    deadline = time.monotonic() + 1200
    while job.get("status") == "running" and time.monotonic() < deadline:
        await asyncio.sleep(3.0)
        job = await ed.call("get_job", job_id=job["job_id"], log_lines=10)
    ed.check_eq(job["status"], "succeeded", f"{job}")
    ed.check_eq((job["result"]["method_used"], job["result"]["player_id"]), ("prebuilt", runtime.name), f"{job}")
    ed.check("dialogue" in job["result"].get("modules", []), f"the build ships the dialogue module: {job['result']}")
    run = await asyncio.to_thread(
        subprocess.run, [str(harness.REPO / "tools" / "with-xvfb.sh"), job["result"]["executable"], "--headless", "--fixed-fps", "60",
                         "--after-ready", "--quit-after", "120"],
        cwd=out, capture_output=True, text=True, timeout=900)
    log = run.stdout + run.stderr
    (ed.scratch.root / "dialogue-player.log").write_text(log)
    shown = [line.split("TRANSCRIPT ", 1)[1].strip() for line in log.splitlines() if "TRANSCRIPT " in line]
    ed.check_eq(shown[-len(expected):], expected, f"the shipped game shows what run_dialogue predicted (D7): {log[-2000:]}")


if __name__ == "__main__":
    harness.main(globals())
