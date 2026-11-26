"""Console commands for agents (docs/specs/dev-console-for-agents.md AC17, AC20): a script's typed
`give` run through eval_lua {command} (its lines, value and cheat flag, get_play_state's cheats_used),
the errors that name the fix (an unknown name with a suggestion, a usage error, code and command
both or neither, not playing, Console Commands off), help's value, an old-style command's print
output as lines, and the player's --command in a development and a release prebuilt runtime.

A file of its own: test_debug_overlay.py is out of the CI suite for its long export test.
The prebuilt test needs JASPER_SLOW_BUILD=1 (the run's shared glcore runtimes,
tools/mcp/build_cache.py).
"""

import asyncio
import os
import subprocess
import time

import build_cache
from harness import REPO, Editor, skip

EDITOR = {"create": True, "name": "Console Commands"}
CI = True
PREBUILT_PLAYERS = ["glcore-development", "glcore-release"]
TIMEOUT = int(os.environ.get("JASPER_BUILD_TIMEOUT", "7200"))

CHEATS = """---@class Cheats : ScriptInstance
local Cheats = { properties = {} }

function Cheats:init()
    self.gold = 120
    DebugOverlay.addCommand{
        name = "give", description = "adds gold or gems", aliases = {"g"}, cheat = true,
        args = {
            {name = "item", type = "enum", choices = {"gold", "gem"}},
            {name = "count", type = "int", optional = true, default = 1, min = 1, max = 999},
        },
        run = function(args, call)
            if args.item == "gold" then self.gold = self.gold + args.count end
            call:print(("%d %s"):format(args.count, args.item))
            return self.gold
        end,
    }
    DebugOverlay.addCommand("legacy", "an old-style command", function(words)
        print("legacy got " .. table.concat(words, ","))
    end)
end

return Cheats
"""


async def setup_cheats(ed: Editor) -> None:
    """The Cheats script on a GameObject of the saved scene (once per editor: the file's tests share it)."""
    if getattr(ed, "console_cheats_ready", False):
        return
    ed.console_cheats_ready = True
    await ed.call("create_entity", name="Cheats", type="empty")
    await ed.call("create_script", kind="lua", name="Cheats", entity="Cheats", content=CHEATS)
    await ed.call("save_project", untitled_scene_dir="scenes")


async def start(ed: Editor) -> None:
    await ed.call("play", action="start", auto_pause=True, fixed_fps=60)
    await ed.call("wait", frames=3)


async def stop(ed: Editor) -> None:
    await ed.call("play", action="stop", auto_pause=False, fixed_fps=0)


async def command(ed: Editor, line: str):
    return await ed.call_raw("eval_lua", {"command": line})


async def test_command_mode(ed: Editor):
    await setup_cheats(ed)
    await start(ed)
    try:
        state = await ed.call("get_play_state")
        ed.check_eq((state["debug_overlay"]["commands_enabled"], state["debug_overlay"]["cheats_used"]), (True, False),
                    f"commands on in Play, no cheat yet: {state['debug_overlay']}")
        r = await command(ed, "give gold 10")
        ed.check(not r.is_error, r.text[:500])
        got = r.value["command"]
        ed.check_eq((got["name"], got["status"], got["ok"], got["lines"], got["value"], got["cheat"]),
                    ("give", "ok", True, ["10 gold"], 130, True), f"{got}")
        ed.check_eq(r.value["mode"], "play", "play mode")
        ed.check("10 gold" in r.text, f"the text is the lines: {r.text[:300]}")
        ed.check(any("[give] 10 gold" in str(line) for line in r.value["console"]), "the printed line is in the console, prefixed")
        alias = await command(ed, "g gem")
        ed.check_eq((alias.value["command"]["name"], alias.value["command"]["value"]), ("give", 130), "an alias")
        state = await ed.call("get_play_state")
        ed.check(state["debug_overlay"]["cheats_used"], f"cheats_used: {state['debug_overlay']}")
    finally:
        await stop(ed)


async def test_errors_name_the_fix(ed: Editor):
    await setup_cheats(ed)
    stopped = await ed.check_error("eval_lua", command="give gold", contains="start it with play")
    ed.check("Console commands run in the playing game" in stopped, stopped)
    await start(ed)
    try:
        r = await command(ed, "gve gold")
        ed.check(r.is_error, "an unknown command is an error")
        ed.check_eq(r.value["command"]["status"], "unknown", f"{r.value}")
        ed.check("did you mean 'give'?" in r.text and 'eval_lua {command: "help"} lists the commands' in r.text, r.text)

        r = await command(ed, "give ruby")
        ed.check(r.is_error, "a usage error is an error")
        ed.check_eq((r.value["command"]["status"], r.value["command"]["usage"]), ("usage", "give <item> [count=1]"), f"{r.value}")
        ed.check('give: item must be one of gold, gem (got "ruby"). Usage: give <item> [count=1]' in r.text, r.text)

        for args in ({"code": "return 1", "command": "help"}, {}):
            message = await ed.check_error("eval_lua", args, contains="not both")
            ed.check("Pass code (Lua) or command (a console line), not both." in message, message)
        ed.check_eq((await ed.call("eval_lua", code="return 6*7"))["values"], [42], "code alone still runs")
    finally:
        await stop(ed)

    await ed.call("set_project_settings", values={"debug.console_commands": "never"})
    await start(ed)
    try:
        state = await ed.call("get_play_state")
        ed.check(not state["debug_overlay"]["commands_enabled"], f"off: {state['debug_overlay']}")
        r = await command(ed, "give gold 1")
        ed.check(r.is_error and r.value["command"]["status"] == "disabled", f"{r.value}")
        ed.check("Project Settings > Debug > Console Commands" in r.text, r.text)
        ed.check('set_project_settings {values: {"debug.console_commands": "development"}}' in r.text, r.text)
        ok = (await ed.call("eval_lua", code="return DebugOverlay.executeCommand('give gold 1').ok"))["values"]
        ed.check_eq(ok, [True], "the game's own calls still run")
    finally:
        await stop(ed)
        await ed.call("set_project_settings", values={"debug.console_commands": "development"})


async def test_help_value(ed: Editor):
    await setup_cheats(ed)
    await start(ed)
    try:
        r = await command(ed, "help")
        ed.check(not r.is_error, r.text[:300])
        entries = {entry["name"]: entry for entry in r.value["command"]["value"]}
        ed.check("give" in entries and "help" in entries and "legacy" in entries, f"{sorted(entries)}")
        give = entries["give"]
        ed.check_eq((give["usage"], give["aliases"], give["cheat"], give["builtIn"]), ("give <item> [count=1]", ["g"], True, False), f"{give}")
        ed.check_eq([a["name"] for a in give["args"]], ["item", "count"], f"{give['args']}")
        one = (await command(ed, "help give")).value["command"]
        ed.check_eq((one["value"]["name"], one["lines"][1]), ("give", "Usage: give <item> [count=1]"), f"{one}")
    finally:
        await stop(ed)


async def test_legacy_command_lines(ed: Editor):
    await setup_cheats(ed)
    await start(ed)
    try:
        r = await command(ed, "legacy a \"b c\"")
        ed.check(not r.is_error, r.text[:300])
        ed.check_eq((r.value["command"]["lines"], r.value["command"]["cheat"]), (["legacy got a,b c"], False), f"{r.value}")
        state = await ed.call("get_play_state")
        ed.check(not state["debug_overlay"]["cheats_used"], "not a cheat")
    finally:
        await stop(ed)


async def build_and_run(runtime_flavour: str, development: bool, runs: list[list[str]]) -> list[tuple[int, str]]:
    """Builds the Cheats project with the run's prebuilt glcore runtime of that flavour and runs its
    player headless once per argument list: (exit code, log) each."""
    out: list[tuple[int, str]] = []
    async with Editor(create=True, name=f"Console Command Player {runtime_flavour}",
                      env=build_cache.player_env("glcore", runtime_flavour)) as ed:
        runtime = await build_cache.player_runtime(ed, "glcore", runtime_flavour)
        await setup_cheats(ed)
        saved = await ed.call("save_project", untitled_scene_dir="scenes")
        await ed.call("edit_build_scenes", action="set_start", scene=saved["scenes_in_build"][0]["id"])
        await ed.call("set_project_settings", values={"player.development_build": development})
        build_dir = ed.scratch.root / f"export-{runtime_flavour}"
        job = await ed.call("build_project", mode="desktop", output_dir=str(build_dir), method="prebuilt", timeout_seconds=600)
        deadline = time.monotonic() + TIMEOUT
        while job.get("status") == "running" and time.monotonic() < deadline:
            await asyncio.sleep(3)
            job = await ed.call("get_job", job_id=job["job_id"], log_lines=10)
        ed.check_eq(job["status"], "succeeded", f"{job}")
        ed.check_eq((job["result"]["method_used"], job["result"]["player_id"]), ("prebuilt", runtime.name), f"{job}")
        for args in runs:
            run = await asyncio.to_thread(
                subprocess.run, [str(REPO / "tools" / "with-xvfb.sh"), job["result"]["executable"], *args],
                cwd=build_dir, capture_output=True, text=True, timeout=600)
            log = run.stdout + run.stderr
            (ed.scratch.root / f"player-{runtime_flavour}-{len(out)}.log").write_text(log)
            out.append((run.returncode, log))
    return out


async def test_player_command_option():
    """AC20: --command in a development runtime runs after the ready line (exit 0, its result logged),
    a command that is not ok fails --fail-on-error (exit 3), and a release runtime refuses it (exit 2)."""
    if os.environ.get("JASPER_SLOW_BUILD", "") in ("", "0"):
        skip("set JASPER_SLOW_BUILD=1 to build and run prebuilt players (the run builds its glcore runtimes once)")
    base = ["--headless", "--after-ready", "--quit-after", "5", "--fail-on-error"]
    (ok_code, ok_log), (bad_code, bad_log) = await build_and_run(
        "development", True, [[*base, "--command", "give gold 10"], [*base, "--command", "nope"]])
    assert ok_code == 0, f"exit {ok_code}: {ok_log[-2000:]}"
    assert "[command] give gold 10: ok" in ok_log, ok_log[-2000:]
    assert bad_code == 3, f"exit {bad_code}: {bad_log[-2000:]}"
    [(release_code, release_log)] = await build_and_run("release", False, [[*base, "--command", "give gold 10"]])
    assert release_code == 2, f"exit {release_code}: {release_log[-2000:]}"
    assert "--command needs a development player or Project Settings > Debug > Console Commands: Always." in release_log, release_log[-2000:]
