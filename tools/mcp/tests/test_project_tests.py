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

import build_cache
import harness
from harness import Editor, free_port, wait_for_port

CI = True
TIMEOUT = 1500
PORT = free_port()
TOKEN = "project-tests-token"
# the CLI attaches to this editor over HTTP (a stdio editor holding the project fails `call` with project_locked)
# test_desktop_player_runs_tests: the run's prebuilt glcore development runtime (tools/mcp/build_cache.py)
PREBUILT_PLAYERS = ["glcore-development"]
EDITOR = {"create": True, "name": "Garden", "extra_args": ["--http-port", str(PORT)],
          "settings": {"mcp": {"http_token": TOKEN}}, "env": build_cache.player_env("glcore", "development")}

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

Test.case("The start scene has a Player", {mode = "edit", tags = {"edit"}}, function(t)
    local scenes = Editor.call("list_scenes", {}).data.scenes
    Test.check(#scenes > 0, "no scenes")
    for _, s in ipairs(scenes) do
        if s.start_scene then
            local found = Editor.call("find_entities", {scene = s.id, name = "Player"})
            Test.check(found.ok and #found.data.entities == 1, s.name .. " has no Player")
        end
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

-- a test file's own globals live in its chunk's environment: the real global table and a
-- library table are what only a new Lua state resets (Play start clears project modules)
Test.case("Leaves Lua state behind", {tags = {"leak"}}, function(t)
    _G.LeakedByAnEarlierCase = true
    string.leakedByAnEarlierCase = true
    package.loaded.leakedModule = {}
end)

Test.case("Starts from a fresh Lua state", {tags = {"leak"}}, function(t)
    Test.check(rawget(_G, "LeakedByAnEarlierCase") == nil, "a global of the earlier case leaked")
    Test.check(string.leakedByAnEarlierCase == nil, "a library change of the earlier case leaked")
    Test.check(package.loaded.leakedModule == nil, "a module of the earlier case leaked")
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
    if start.get("path"):
        await ed.call("save_scene", scene=S["scene"])
    else:
        await ed.call("save_scene", scene=S["scene"], path="scenes/Main.scene")
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
    edit = next(c for c in listed["cases"] if c["id"] == "menu.test.lua#The start scene has a Player")
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
    ed.check_eq((set_result["store"], set_result["profile"]), ("prefs", "clean"), "store is the selected store, profile the Play Data profile")
    await ed.call("play", action="stop", keep_changes="none")
    ed.check(not folder.exists(), "the session folder is gone after stop")
    ed.check_eq(tree_hash(rundata), before, "UserSettings.xml of the project is unchanged")
    after = await ed.call("user_settings", action="list")
    ed.check_eq((after["store"], after["profile"]), ("prefs", "project"))
    text = await ed.call("play", action="start", data="two_coins", expect_error=True)
    ed.check_in("one_coin", text, "the error lists the snapshots")


async def test_edit_mode_cases(ed: Editor):
    s = await setup(ed)
    history = await ed.call("get_undo_history", scope="project")
    result = await run_tests(ed, filter="tag:edit,tag:readonly")
    doc = json.loads((s["project"] / result["results_file"]).read_text())
    by_id = {c["id"]: c for c in doc["cases"]}
    ed.check_eq(by_id["menu.test.lua#The start scene has a Player"]["status"], "passed", json.dumps(by_id["menu.test.lua#The start scene has a Player"]))
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
    await ed.call("play", action="stop", keep_changes="none")
    # a game's own global Test is the game's inside a case (the test API is the chunk's)
    (ed.project / "Tests" / "global.test.lua").write_text(
        'Test.case("A game global named Test", {tags = {"global"}}, function(t)\n'
        '    rawset(_G, "Test", 5)\n'
        '    Test.check(rawget(_G, "Test") == 5, "the game global")\n'
        '    Test.check(type(Test) == "table", "the chunk still has the test API")\n'
        'end)\n')
    try:
        result = await run_tests(ed, filter="tag:global")
        ed.check_eq(result["counts"]["passed"], 1, f"{result}")
    finally:
        (ed.project / "Tests" / "global.test.lua").unlink()


async def test_lua_state_is_fresh_per_case(ed: Editor):
    """R14, R24: each case starts from a new Lua state; nothing an earlier case left reaches it."""
    await setup(ed)
    result = await run_tests(ed, filter="tag:leak")
    ed.check_eq((result["counts"]["passed"], result["counts"]["failed"]), (2, 0), f"{json.dumps(result)[:2000]}")


async def test_cli_test_command(ed: Editor):
    s = await setup(ed)
    await wait_for_port(PORT)
    work = Path(tempfile.mkdtemp(prefix="jasper-test-cli-"))
    env = {k: v for k, v in os.environ.items() if k not in ("DISPLAY", "WAYLAND_DISPLAY")}
    env["HOME"] = str(work)
    # the HTTP editor of this test (its registry of live editors is under the editor's own HOME)
    env["JASPER_MCP_URL"] = f"http://127.0.0.1:{PORT}/mcp"
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


async def test_desktop_player_runs_tests(ed: Editor):
    """AC10: run_tests {target: "desktop"} builds a Development player with the tests, runs it with
    --run-tests and imports its results; its exit code is 8 with a failure, 0 without."""
    if os.environ.get("JASPER_SLOW_BUILD") != "1":
        harness.skip("JASPER_SLOW_BUILD=1 builds a real game (the run builds its prebuilt player runtime once)")
    await setup(ed)
    await build_cache.player_runtime(ed, "glcore", "development")
    with ed.busy():
        red = await ed.call("run_tests", target="desktop", filter="tag:smoke,tag:red,tag:leak,tag:capture", timeout_seconds=3600, timeout=3700)
    ed.check_eq(red["player_exit_code"], 8, f"a failed case: exit 8: {json.dumps(red)[:3000]}")
    s = S
    doc = json.loads(s["project"].joinpath(red["results_file"]).read_text())
    status = {c["name"]: c["status"] for c in doc["cases"]}
    why = {c["name"]: ([f["message"] for f in c.get("failures", [])], [line["message"] for line in c.get("console", [])][-3:])
           for c in doc["cases"] if c["status"] not in ("passed", "skipped")}
    ed.check_eq((status["Player starts with three lives"], status["A coin is saved"], status["Red on purpose"],
                 status["The shop refuses a purchase"], status["Leaves Lua state behind"], status["Starts from a fresh Lua state"]),
                ("passed", "passed", "failed", "skipped", "passed", "passed"), f"{status} {json.dumps(why)[:3000]}")
    ed.check("Edit Mode cannot edit" not in status, "a player runs no Edit Mode case")
    capture = next(c for case in doc["cases"] for c in case["captures"])
    ed.check(capture["status"] != "pending", f"the pending capture was compared at import: {capture}")
    ed.check(doc["run"]["target"]["platform"] == "linux" and not doc["run"]["target"]["editor"], f"{doc['run']['target']}")
    # the player's own data folder: the cases wrote into their sandboxes only, and those went
    user_data = Path(red["player_results"]).parent.parent
    ed.check(not (user_data / "progress.json").exists(), f"progress.json reached the player's real data folder {user_data}")
    runs = user_data / ".jasper-test-runs"
    ed.check(not runs.exists() or not any(p.is_file() for p in runs.rglob("*")), f"case sandboxes left: {list(runs.rglob('*'))[:5]}")
    with ed.busy():
        green = await ed.call("run_tests", target="desktop", filter="tag:smoke", timeout_seconds=3600, timeout=3700)
    ed.check_eq(green["player_exit_code"], 0, f"smoke only: exit 0: {json.dumps(green)[:2000]}")
    ed.check_eq(s["project"].joinpath(".jasperengine/test-results/ledger.json").exists(), True)
    ledger = json.loads(s["project"].joinpath(".jasperengine/test-results/ledger.json").read_text())
    ed.check("linux" in ledger["cases"]["menu.test.lua#Player starts with three lives"], "the ledger has the player's platform")


async def test_test_runner_window(ed: Editor):
    await setup(ed)
    await ed.call("tool_groups", enable=["editor_ux"])
    await ed.call("run_command", id="window.testRunner")
    found = await ed.call("find_ui_item", label="Run All", window="Test Runner")
    ed.check(found["items"], f"the Test Runner shows Run All: {found}")
    shot = os.environ.get("JASPER_TEST_RUNNER_SHOT")
    if shot:
        await ed.screenshot("editor", path=shot)


ROLLER = """local Roller = {}
function Roller:init()
    self.go = GameObject(self.scene, self.entity)
    self.wall = GameObject.find("Wall")
    RegisterEngineEvent(self, "onUpdate")
end
function Roller:onUpdate()
    if Input.isKeyPressed(Input.KEY_RIGHT) then
        local p = self.go.position
        self.go.position = Vector3(math.min(p.x + 0.25, self.wall.position.x - 1), p.y, p.z)
    end
    GameState = GameState or {}
    GameState.ballX = self.go.position.x
end
return Roller
"""

# the recorder starts at init (the session's first frame) when the case's data has record.json
RECORDER = """local Recorder = {}
function Recorder:init()
    if Json.readFile("data://record.json") then
        assert(InputRecording.start({checkpointFrames = 10, probes = {"Ball", "Wall"}}))
    end
end
return Recorder
"""

REPLAY_TESTS = """-- R9 / AC16 (test_project_tests.py)
Test.case("Record the roll", {tags = {"rec"}, data = "rec"}, function(t)
    Test.check(InputRecording.isRecording(), "the recorder started at init")
    Test.input({ {type = "key", key = "right", action = "down"}, {type = "wait", frames = 40},
                 {type = "key", key = "right", action = "up"} })
    Test.waitFrames(10)
    Test.check(InputRecording.stop("data://roll.input.json") ~= nil, "the recording stopped")
    Test.waitFrames(30)
end)

Test.case("The roll replays", {tags = {"replay"}, replay = "Input/roll.input.json"}, function(t)
    Test.waitUntil(function() return not InputRecording.isReplaying() end, {frames = 300})
    Test.check(GameState and GameState.ballX > 1, "the replayed key rolled the ball")
end)
"""


async def test_replay_case_divergence(ed: Editor):
    """AC16: a recording made in a test session replays green as a replay case; after the wall in
    the scene moves, the case fails with a divergence naming the first checkpoint's frame and the
    Wall probe (the state differs from there); divergence: warn passes it."""
    s = await setup(ed)
    for name, script in (("Ball", ROLLER), ("Recorder", RECORDER)):
        await ed.call("create_entity", scene=s["scene"], type="empty", name=name)
        await ed.call("create_script", kind="lua", name=name + "Script", content=script, scene=s["scene"], entity=name)
    await ed.call("create_entity", scene=s["scene"], type="empty", name="Wall")
    await ed.call("set_properties", scene=s["scene"], entity="Wall", values={"transform": {"position": [20, 0, 0]}})
    await ed.call("save_scene", scene=s["scene"])
    tests = s["project"] / "Tests"
    (tests / "Data" / "rec").mkdir(parents=True, exist_ok=True)
    (tests / "Data" / "rec" / "record.json").write_text("{}")
    (tests / "replay.test.lua").write_text(REPLAY_TESTS)

    recorded = await run_tests(ed, filter="tag:rec", keep_data=True)
    ed.check_eq(recorded["counts"]["passed"], 1, f"the recording case: {json.dumps(recorded)[:2000]}")
    sessions = s["project"] / ".jasperengine" / "user"
    roll = None
    for _ in range(100):
        found = sorted(sessions.rglob("roll.input.json"))
        if found:
            roll = found[-1]
            break
        await asyncio.sleep(0.1)
    ed.check(roll is not None, f"the recording was written under {sessions}")
    recording = json.loads(roll.read_text())
    ed.check(recording.get("checkpoints"), f"checkpoints were sampled: {list(recording)}")
    # a Lua recording starts "now"; this one started with the session, as Record Input's do
    recording["start"] = "play"
    (tests / "Input").mkdir(exist_ok=True)
    (tests / "Input" / "roll.input.json").write_text(json.dumps(recording))

    green = await run_tests(ed, filter="tag:replay")
    ed.check_eq((green["counts"]["passed"], green["counts"]["failed"]), (1, 0), f"the replay: {json.dumps(green)[:3000]}")

    await ed.call("set_properties", scene=s["scene"], entity="Wall", values={"transform": {"position": [5, 0, 0]}})
    await ed.call("save_scene", scene=s["scene"])
    red = await run_tests(ed, filter="tag:replay")
    ed.check_eq(red["counts"]["failed"], 1, f"a moved wall diverges: {json.dumps(red)[:3000]}")
    failure = next((f for f in red["failures"] if f["id"] == "replay.test.lua#The roll replays" and f["kind"] == "divergence"), None)
    ed.check(failure is not None, f"a divergence failure: {red['failures']}")
    ed.check_in("diverged at frame 10", failure["message"], "the first checkpoint after the change")
    ed.check_in("Wall", failure["message"], "the report names the probe that differs")

    (tests / "tests.yaml").write_text(TESTS_YAML + "divergence: warn\n")
    try:
        warned = await run_tests(ed, filter="tag:replay")
        ed.check_eq((warned["counts"]["passed"], warned["counts"]["failed"]), (1, 0), f"divergence: warn: {json.dumps(warned)[:3000]}")
        doc = json.loads((s["project"] / warned["results_file"]).read_text())
        console = [line["message"] for case in doc["cases"] for line in case.get("console", [])]
        ed.check(any("diverged at frame" in line for line in console), f"the warning stays in the console: {console[:5]}")
    finally:
        (tests / "tests.yaml").write_text(TESTS_YAML)
