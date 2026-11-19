"""A project's own tests: run_tests, Play Data, the CLI and the ledger (docs/specs/project-test-runner.md
AC3-AC9, AC14, AC15, AC18, AC19).

The fixture is built through the tools on a fresh project: its start scene with a Player cube whose
script sets GameState.lives = 3 and a Coin script that adds a coin to data://progress.json; the test
root Tests/ holds tests.yaml, menu.test.lua (smoke cases, a red one, a skipped one, Edit Mode cases, a
capture, a runaway loop, a refused data = "project") and a snapshot Data/one_coin/.
"""

import asyncio
import hashlib
import json
import os
import shutil
import subprocess
import tempfile
import xml.etree.ElementTree as ET
from pathlib import Path

import harness
from harness import Editor, free_port, wait_for_port

CI = True
TIMEOUT = 1500
PORT = free_port()
TOKEN = "project-tests-token"
# the CLI attaches to this editor over HTTP (a stdio editor holding the project fails `call` with project_locked)
EDITOR = {"create": True, "name": "Garden", "extra_args": ["--http-port", str(PORT)],
          "settings": {"mcp": {"http_token": TOKEN}}}

PLAYER = """local Player = {}
function Player:init()
    GameState = GameState or {}
    GameState.lives = 3
end
return Player
"""

COIN = """local Coin = {}
function Coin:init()
    local progress = Json.readFile("data://progress.json") or {coins = 0}
    progress.coins = progress.coins + 1
    assert(Json.writeFile("data://progress.json", progress))
    GameState = GameState or {}
    GameState.coins = progress.coins
end
return Coin
"""

TESTS_YAML = "version: 1\ntargets: [editor, android]\nfixed_fps: 60\nresolution: 320x180\n"

# line numbers matter: the red case's Test.equal is on line 17
MENU = """-- the fixture's cases (test_project_tests.py)
Test.case("Player starts with three lives", {tags = {"smoke"}}, function(t)
    Test.waitUntil(function() return GameState and GameState.lives end, {frames = 120, message = "the Player never started"})
    Test.equal(GameState.lives, 3, "lives at start")
end)

Test.case("A coin is saved", {tags = {"smoke", "data"}}, function(t)
    Test.waitUntil(function() return GameState and GameState.coins end, {frames = 120})
    Test.equal(GameState.coins, 1, "coins on clean data")
    local saved = Json.readFile("data://progress.json")
    Test.equal(saved and saved.coins, 1, "progress.json in the case's folder")
end)

Test.case("Red on purpose", {tags = {"red"}}, function(t)
    Test.waitFrames(5)
    Test.log("about to fail")
    Test.equal(GameState.lives, 2, "lives at start")
end)

Test.case("The shop refuses a purchase", {tags = {"smoke"}, skip = "the shop is not built"}, function(t)
    Test.fail("never runs")
end)

Test.case("One coin snapshot", {tags = {"data"}, data = "one_coin"}, function(t)
    Test.waitUntil(function() return GameState and GameState.coins end, {frames = 120})
    Test.equal(GameState.coins, 2, "the snapshot's coin plus this one")
end)

Test.case("Project data is refused", {tags = {"projectdata"}, data = "project"}, function(t) end)

Test.case("Every scene has a camera", {mode = "edit", tags = {"edit"}}, function(t)
    local scenes = Editor.call("list_scenes", {}).data.scenes
    Test.check(#scenes > 0, "no scenes")
    for _, s in ipairs(scenes) do
        if s.start_scene then Test.check(s.main_camera ~= nil, s.name .. " has no camera") end
    end
end)

Test.case("Edit Mode cannot edit", {mode = "edit", tags = {"readonly"}}, function(t)
    Editor.call("set_properties", {entity = "Player", values = {transform = {position = {9, 9, 9}}}})
end)

Test.case("Level start looks right", {tags = {"capture"}}, function(t)
    Test.waitUntil(function() return GameState and GameState.lives end, {frames = 120})
    Test.waitFrames(3)
    Test.capture("level1_start")
end)

Test.case("Runaway loop", {tags = {"runaway"}}, function(t)
    while true do end
end)

Test.case("After the runaway", {tags = {"runaway"}}, function(t)
    Test.waitFrames(1)
end)
"""

S: dict = {}


def tree_hash(root: Path) -> str:
    h = hashlib.sha256()
    if root.exists():
        for path in sorted(p for p in root.rglob("*") if p.is_file()):
            h.update(str(path.relative_to(root)).encode())
            h.update(path.read_bytes())
    return h.hexdigest()


async def run_tests(ed: Editor, **args):
    return await ed.call("run_tests", timeout=900, **args)


async def setup(ed: Editor) -> dict:
    if S.get("ed") is ed:
        return S
    S.clear()
    S["ed"] = ed
    project = ed.project
    scenes = (await ed.call("list_scenes"))["scenes"]
    start = next(s for s in scenes if s.get("start_scene")) if any(s.get("start_scene") for s in scenes) else scenes[0]
    S["scene"] = start["id"]
    await ed.call("create_entity", scene=S["scene"], type="cube", name="Player")
    await ed.call("create_script", kind="lua", name="Player", content=PLAYER, scene=S["scene"], entity="Player")
    await ed.call("create_entity", scene=S["scene"], type="empty", name="Coin")
    await ed.call("create_script", kind="lua", name="Coin", content=COIN, scene=S["scene"], entity="Coin")
    await ed.call("save_scene", scene=S["scene"])
    await ed.call("save_project")
    tests = project / "Tests"
    (tests / "Data" / "one_coin").mkdir(parents=True, exist_ok=True)
    (tests / "tests.yaml").write_text(TESTS_YAML)
    (tests / "menu.test.lua").write_text(MENU)
    (tests / "Data" / "one_coin" / "progress.json").write_text('{"coins": 1}')
    S["project"] = project
    return S


async def test_run_tests_is_in_the_testing_group(ed: Editor):
    await setup(ed)
    default = {t.name for t in await ed.tools(groups=None)}
    ed.check("run_tests" not in default, "run_tests is hidden until the testing group is enabled")
    await ed.call("tool_groups", enable=["testing"])
    ed.check("run_tests" in {t.name for t in await ed.tools(groups=None)}, "listed once enabled")
    listed = await ed.call("run_tests", action="list")
    ids = [c["id"] for c in listed["cases"]]
    ed.check_in("menu.test.lua#Red on purpose", ids)
    red = next(c for c in listed["cases"] if c["id"] == "menu.test.lua#Red on purpose")
    ed.check_eq(red["line"], 14, "the case's line")
    edit = next(c for c in listed["cases"] if c["id"] == "menu.test.lua#Every scene has a camera")
    ed.check_eq(edit["mode"], "edit")


async def test_run_tests_counts_and_failures(ed: Editor):
    s = await setup(ed)
    rundata = s["project"] / ".jasperengine" / "user" / "rundata"
    before = tree_hash(rundata)
    result = await run_tests(ed, filter="tag:smoke,tag:red")
    counts = result["counts"]
    ed.check_eq((counts["total"], counts["passed"], counts["failed"], counts["skipped"], counts["error"], counts["hung"]),
                (4, 2, 1, 1, 0, 0), f"counts: {json.dumps(result)[:3000]}")
    failure = next(f for f in result["failures"] if f["id"] == "menu.test.lua#Red on purpose")
    ed.check_eq((failure["kind"], failure["file"], failure["line"]), ("equal", "menu.test.lua", 17), f"failure: {failure}")
    ed.check_eq(failure["message"], "lives at start: expected 2, got 3")
    ed.check(failure["frame"] >= 5, f"the frame of the failure: {failure}")
    doc = json.loads((s["project"] / result["results_file"]).read_text())
    ed.check_eq((doc["format"], doc["version"]), ("jasperengine-test-results", 1), "results document v1")
    ed.check_eq(doc["counts"]["total"], 4)
    ed.check_eq(tree_hash(rundata), before, "the project's Play data is untouched by a run")


async def test_filter_matching_nothing_is_an_error(ed: Editor):
    await setup(ed)
    text = await ed.call("run_tests", filter="tag:nope", expect_error=True)
    ed.check_in('run_tests {action: "list"}', text, "the error names the listing")
    ed.check_in('No test matched "tag:nope"', text)


async def test_case_data_is_sandboxed(ed: Editor):
    s = await setup(ed)
    snapshot = s["project"] / "Tests" / "Data" / "one_coin"
    rundata = s["project"] / ".jasperengine" / "user" / "rundata"
    snap_before, run_before = tree_hash(snapshot), tree_hash(rundata)
    result = await run_tests(ed, filter="tag:data,tag:projectdata")
    by_id = {}
    doc = json.loads((s["project"] / result["results_file"]).read_text())
    for case in doc["cases"]:
        by_id[case["id"]] = case
    ed.check_eq(by_id["menu.test.lua#A coin is saved"]["status"], "passed", json.dumps(by_id["menu.test.lua#A coin is saved"]))
    ed.check_eq(by_id["menu.test.lua#One coin snapshot"]["status"], "passed", json.dumps(by_id["menu.test.lua#One coin snapshot"]))
    refused = by_id["menu.test.lua#Project data is refused"]
    ed.check_eq(refused["status"], "error")
    ed.check_in("tests never use the project's Play data", refused["failures"][0]["message"])
    ed.check_eq(tree_hash(snapshot), snap_before, "the snapshot is byte-identical")
    ed.check_eq(tree_hash(rundata), run_before, "rundata is untouched")
    ed.check(not (rundata / "progress.json").exists(), "progress.json never reached rundata")
    sessions = s["project"] / ".jasperengine" / "user" / "sessions"
    ed.check(not sessions.exists() or not any(sessions.iterdir()), "the case folders are deleted")


async def test_play_data_profiles(ed: Editor):
    s = await setup(ed)
    rundata = s["project"] / ".jasperengine" / "user" / "rundata"
    before = tree_hash(rundata)
    await ed.call("play", action="start", data="clean")
    state = await ed.call("get_play_state")
    ed.check_eq(state["data"]["profile"], "clean", f"get_play_state.data: {state.get('data')}")
    folder = s["project"] / state["data"]["folder"]
    ed.check(folder.exists() and ".jasperengine/user/sessions" in state["data"]["folder"], f"a session folder: {state['data']}")
    set_result = await ed.call("user_settings", action="set", key="volume", value=0.5)
    ed.check_eq(set_result["store"], "clean")
    await ed.call("play", action="stop", keep_changes="none")
    ed.check(not folder.exists(), "the session folder is gone after stop")
    ed.check_eq(tree_hash(rundata), before, "UserSettings.xml of the project is unchanged")
    after = await ed.call("user_settings", action="list")
    ed.check_eq(after["store"], "project")
    text = await ed.call("play", action="start", data="two_coins", expect_error=True)
    ed.check_in("one_coin", text, "the error lists the snapshots")


async def test_edit_mode_cases(ed: Editor):
    s = await setup(ed)
    history = await ed.call("get_undo_history", scope="project")
    result = await run_tests(ed, filter="tag:edit,tag:readonly")
    doc = json.loads((s["project"] / result["results_file"]).read_text())
    by_id = {c["id"]: c for c in doc["cases"]}
    ed.check_eq(by_id["menu.test.lua#Every scene has a camera"]["status"], "passed", json.dumps(by_id["menu.test.lua#Every scene has a camera"]))
    readonly = by_id["menu.test.lua#Edit Mode cannot edit"]
    ed.check_eq(readonly["status"], "failed")
    ed.check_in("Edit Mode tests are read-only: set_properties edits the project; test it in a Play Mode case",
                readonly["failures"][0]["message"])
    after = await ed.call("get_undo_history", scope="project")
    ed.check_eq(len(after.get("entries", after.get("steps", []))), len(history.get("entries", history.get("steps", []))),
                "no undo step: the scene is unchanged")


async def test_captures_new_accept_and_fail(ed: Editor):
    s = await setup(ed)
    golden = s["project"] / "Tests" / "Golden" / "menu.test.lua__Level_start_looks_right" / "level1_start.png"
    shutil.rmtree(golden.parent, ignore_errors=True)
    first = await run_tests(ed, filter="tag:capture")
    ed.check_eq(first["counts"]["failed"], 1, "a capture without a golden is new: a failure, never a pass")
    ed.check_in("run_tests {action: \"accept\"", first.get("next", ""))
    accepted = await ed.call("run_tests", action="accept", captures=["menu.test.lua#Level start looks right/level1_start"])
    ed.check(golden.exists(), f"Accept wrote the golden: {accepted}")
    await ed.call("undo", scope="project")
    ed.check(not golden.exists(), "undo removes the accepted golden")
    await ed.call("run_tests", action="accept", captures=["all"])
    ed.check(golden.exists(), "accept all wrote it again")
    second = await run_tests(ed, filter="tag:capture")
    ed.check_eq(second["counts"]["passed"], 1, f"the next run matches the golden: {second}")
    # the Player moved 2 m: the capture differs, with the diff numbers and one image
    await ed.call("set_properties", scene=s["scene"], entity="Player", values={"transform": {"position": [2, 0, 0]}})
    await ed.call("save_scene", scene=s["scene"])
    raw = await ed.call_raw("run_tests", {"filter": "tag:capture"}, timeout=900)
    ed.check(not raw.is_error, raw.text[:500])
    value = raw.value
    ed.check_eq(value["counts"]["failed"], 1, f"a moved Player fails the capture: {value}")
    ed.check_eq(len(raw.images), 1, "one image: the a | b | diff panel")
    await ed.call("set_properties", scene=s["scene"], entity="Player", values={"transform": {"position": [0, 0, 0]}})
    await ed.call("save_scene", scene=s["scene"])


async def test_runaway_lua_is_an_error(ed: Editor):
    s = await setup(ed)
    result = await run_tests(ed, filter="tag:runaway")
    doc = json.loads((s["project"] / result["results_file"]).read_text())
    by_id = {c["id"]: c for c in doc["cases"]}
    runaway = by_id["menu.test.lua#Runaway loop"]
    ed.check_eq(runaway["status"], "error", json.dumps(runaway))
    ed.check_in("script interrupted", runaway["failures"][0]["message"])
    ed.check_eq(by_id["menu.test.lua#After the runaway"]["status"], "passed", "the next case runs")
    state = await ed.call("get_play_state")
    ed.check_eq(state["state"], "stopped", "Play stopped after the run")


async def test_test_api_absent_outside_runs(ed: Editor):
    await setup(ed)
    await ed.call("play", action="start")
    value = (await ed.call("eval_lua", code="return Test == nil"))["values"][0]
    ed.check(value is True, "Test is not bound in a normal Play")
    await ed.call("eval_lua", code="Test = 5")
    await ed.call("play", action="stop", keep_changes="none")
    await run_tests(ed, filter="tag:smoke")
    await ed.call("play", action="start")
    value = (await ed.call("eval_lua", code="return Test"))["values"][0]
    ed.check_eq(value, 5, "a game's global Test survives a test run")
    await ed.call("eval_lua", code="Test = nil")
    await ed.call("play", action="stop", keep_changes="none")


async def test_cli_test_command(ed: Editor):
    s = await setup(ed)
    await wait_for_port(PORT)
    work = Path(tempfile.mkdtemp(prefix="jasper-test-cli-"))
    env = {k: v for k, v in os.environ.items() if k not in ("DISPLAY", "WAYLAND_DISPLAY")}
    env["HOME"] = str(work)
    env["JASPER_MCP_TOKEN"] = TOKEN

    def cli(*args: str) -> subprocess.CompletedProcess:
        return subprocess.run(harness.editor_command() + ["test", "--project", str(s["project"]), *args], cwd=str(work), env=env,
                              capture_output=True, text=True, timeout=900)

    try:
        out = work / "out.xml"
        green = await asyncio.to_thread(cli, "--filter", "tag:smoke", "--junit", str(out))
        ed.check_eq(green.returncode, 0, f"smoke passes: {green.stdout[-2000:]} {green.stderr[-2000:]}")
        ed.check_in("PASS menu.test.lua#Player starts with three lives", green.stdout)
        red = await asyncio.to_thread(cli, "--filter", "tag:red", "--junit", str(out), "--json", str(work / "results.json"))
        ed.check_eq(red.returncode, 1, f"red fails: {red.stdout[-2000:]} {red.stderr[-2000:]}")
        suites = ET.parse(out).getroot()
        ed.check_eq((suites.get("tests"), suites.get("failures"), suites.get("skipped")), ("1", "1", "0"), ET.tostring(suites)[:2000])
        failure = suites.find(".//failure")
        ed.check(failure is not None and failure.get("type") == "equal", "a <failure type=\"equal\">")
        ed.check_eq(json.loads((work / "results.json").read_text())["format"], "jasperengine-test-results")
        none = await asyncio.to_thread(cli, "--filter", "tag:nope")
        ed.check_eq(none.returncode, 3, f"no case matched: {none.stderr[-1000:]}")
    finally:
        shutil.rmtree(work, ignore_errors=True)


async def test_ledger_unverified(ed: Editor):
    s = await setup(ed)
    result = await run_tests(ed, filter="tag:smoke")
    unverified = result["unverified"]
    passing = "menu.test.lua#Player starts with three lives"
    ed.check_eq(unverified.get(passing), ["android"], f"a pass on the editor leaves android: {unverified}")
    ed.check_in("Unverified on android", result.get("next", ""))
    # an Android run's results (the checked-in sample, with this project's ids) clear it
    sample = Path(__file__).resolve().parents[2] / "test-projects" / "game-tests" / "samples" / "android-results.json"
    imported = await ed.call("run_tests", action="import", results_file=str(sample))
    ed.check(passing not in imported["unverified"], f"android passed it: {imported['unverified']}")
    ledger = json.loads((s["project"] / ".jasperengine" / "test-results" / "ledger.json").read_text())
    ed.check_eq(sorted(k for k, v in ledger["cases"][passing].items() if "last_pass" in v), ["android", "editor"])


async def test_test_runner_window(ed: Editor):
    await setup(ed)
    await ed.call("tool_groups", enable=["editor_ux"])
    await ed.call("run_command", command="window.testRunner")
    found = await ed.call("find_ui_item", label="Run All", window="Test Runner")
    ed.check(found["items"], f"the Test Runner shows Run All: {found}")
    shot = os.environ.get("JASPER_TEST_RUNNER_SHOT")
    if shot:
        await ed.screenshot("editor", path=shot)
