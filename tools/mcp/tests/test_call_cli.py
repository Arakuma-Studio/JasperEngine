"""`jasperengine-editor call` / `batch` / `list-tools` (S31 R1-R11, R15, R16, R29; AC3-AC7).

The harness editor serves HTTP on a free port with a bearer token; the CLI attaches with --port
and --token. Each CLI run gets a HOME of its own (no instance file of another editor can
answer) and an empty working directory, which must stay empty (R15).

- help offline: `call create_entity --help` with no target lists every property of the live
  tool and both examples; offline `list-tools --json` equals the live tools/list per tool;
- --dry-run prints the exact arguments, contacts no editor (no session, nothing created), and,
  like --help and list-tools, takes at most 300 ms (best of three);
- exit codes: create_entity 0 (and the --json envelope), a missing entity 1 (tool_error), a bad
  enum 2, a wrong token 3, a port nobody listens on 3, a server that answers garbage 4
  (fake_server.py --broken); screenshot writes a PNG named in images[0].path, no base64 out;
- --timeout 1 on a 5 s wait exits 3 and the editor logs the call cancelled;
- --wait-job follows a generate_shaders job to its end; SIGINT while waiting cancels the job
  (cancel_job) and exits 3; build_project --wait-job under JASPER_SLOW_BUILD=1;
- batch: 3 lines with the second failing print 2 envelopes and exit 1, with --continue 3;
  --atomic makes them one undo step; 201 lines with --atomic exit 2 before anything runs.
"""

from __future__ import annotations

import asyncio
import json
import os
import shutil
import signal
import subprocess
import sys
import tempfile
import time
from pathlib import Path

import build_cache
import harness
from harness import Editor, HttpSession, free_port, wait_for_port

CI = True
TIMEOUT = 1200
PORT = free_port()
TOKEN = "s31-call-token"
# test_wait_job_build's game: built with the run's prebuilt glcore runtime (tools/mcp/build_cache.py)
PREBUILT_PLAYERS = ["glcore-release"]
EDITOR = {"create": True, "name": "CallCli", "extra_args": ["--http-port", str(PORT)],
          "settings": {"mcp": {"http_token": TOKEN}},
          "env": {"JASPER_MCP_TRACE": "1", **build_cache.player_env("glcore", "release")}}
ATTACH = ["--port", str(PORT), "--token", TOKEN]


class Cli:
    """The editor binary as the CLI, in an empty working directory with a HOME of its own."""

    def __init__(self):
        self.root = Path(tempfile.mkdtemp(prefix="jasper-call-cli-"))
        self.cwd = self.root / "cwd"
        self.home = self.root / "home"
        self.cwd.mkdir()
        self.home.mkdir()

    def env(self, extra: dict[str, str] | None = None) -> dict[str, str]:
        env = {k: v for k, v in os.environ.items() if k not in ("JASPER_MCP_URL", "JASPER_MCP_TOKEN", "DISPLAY",
                                                              "XDG_CONFIG_HOME", "WAYLAND_DISPLAY")}
        env["HOME"] = str(self.home)
        env.update(extra or {})
        return env

    def run(self, *args: str, stdin: str | None = None, extra_env: dict[str, str] | None = None,
            timeout: float = 600) -> subprocess.CompletedProcess:
        return subprocess.run(harness.editor_command() + list(args), cwd=str(self.cwd), env=self.env(extra_env),
                              input=stdin, capture_output=True, text=True, timeout=timeout)

    async def arun(self, *args: str, **kw) -> subprocess.CompletedProcess:
        return await asyncio.to_thread(self.run, *args, **kw)

    def timed(self, *args: str) -> float:
        """Best wall time of three runs (R29)."""
        best = 1e9
        for _ in range(3):
            t0 = time.monotonic()
            self.run(*args)
            best = min(best, time.monotonic() - t0)
        return best

    def remove(self) -> None:
        shutil.rmtree(self.root, ignore_errors=True)


def envelopes(proc: subprocess.CompletedProcess) -> list[dict]:
    return [json.loads(line) for line in proc.stdout.splitlines() if line.strip()]


def flag(key: str) -> str:
    return "--" + key.replace("_", "-")


S: dict = {}


async def test_setup(ed: Editor):
    await wait_for_port(PORT)
    S["cli"] = Cli()


async def test_help_offline(ed: Editor):
    cli: Cli = S["cli"]
    p = await cli.arun("call", "create_entity", "--help")
    ed.check(p.returncode == 0, f"call create_entity --help exits 0: {p.returncode} {p.stderr[-500:]}")
    props = ed.tool("create_entity").input_schema["properties"]
    missing = [k for k in props if flag(k) not in p.stdout]
    ed.check(not missing, f"help lists every property: missing {missing}\n{p.stdout}")
    ed.check(p.stdout.count("call create_entity") >= 2 and "call create_entity '{" in p.stdout, f"two examples:\n{p.stdout}")
    ed.check("Group: core" in p.stdout and "Allowed while playing:" in p.stdout and "offline" in p.stdout,
             f"group, play rule, offline note:\n{p.stdout[:600]}")
    p = await cli.arun("call", "--help")
    ed.check(p.returncode == 0 and "core (" in p.stdout and "create_entity" in p.stdout, f"call --help: {p.stdout[-800:]}")
    p = await cli.arun("call")
    ed.check(p.returncode == 2 and "no tool given" in p.stderr, f"call without a tool: {p.returncode} {p.stderr}")
    p = await cli.arun("call", "create-entity", "--help")
    ed.check(p.returncode == 0 and "create_entity" in p.stdout, "kebab-case tool names")
    p = await cli.arun("call", "creat_entity", "--name", "X")
    ed.check(p.returncode == 2 and "did you mean create_entity" in p.stderr, f"unknown tool: {p.returncode} {p.stderr}")
    p = await cli.arun("call", "--nope", "editor_status")
    ed.check(p.returncode == 2 and "unknown option --nope" in p.stderr, f"unknown global: {p.stderr}")
    ed.check(not any(cli.cwd.iterdir()), f"the working directory stays empty: {list(cli.cwd.iterdir())}")


async def test_offline_registry_matches_live(ed: Editor):
    cli: Cli = S["cli"]
    p = await cli.arun("list-tools", "--json")
    ed.check(p.returncode == 0, f"list-tools --json: {p.stderr[-400:]}")
    offline = {t["name"]: t for t in json.loads(p.stdout)}
    http = HttpSession(PORT, token=TOKEN)
    await asyncio.to_thread(http.initialize)
    a = await asyncio.to_thread(http.request, "tools/list", {"_meta": {"jasper/groups": "all"}})
    live = {t["name"]: t for t in a.response["result"]["tools"]}
    ed.check_eq(sorted(offline), sorted(live), "the same tools")
    differ = [n for n in live if json.dumps(live[n], sort_keys=True) != json.dumps(offline[n], sort_keys=True)]
    ed.check(not differ, f"offline definitions equal the live ones: {differ[:5]}")
    p = await cli.arun("list-tools")
    ed.check(p.returncode == 0 and "create_entity" in p.stdout and " tools, " in p.stdout, f"list-tools: {p.stdout[-300:]}")
    p = await cli.arun("list-tools", "--group", "diagnostics")
    ed.check(p.returncode == 0 and "get_memory_stats" in p.stdout and "create_entity" not in p.stdout, "--group")
    # attached: the live list
    p = await cli.arun("list-tools", *ATTACH, "--json")
    ed.check(p.returncode == 0 and len(json.loads(p.stdout)) == len(live), "list-tools against the editor")


async def test_dry_run(ed: Editor):
    cli: Cli = S["cli"]
    before = await ed.call("editor_status")
    p = await cli.arun("call", "--dry-run", "create_entity", "--name", "DryBox", "--type", "cube", "--position", "1,2,3",
                       "--no-select")
    ed.check(p.returncode == 0, f"dry run: {p.returncode} {p.stderr}")
    ed.check_eq(json.loads(p.stdout), {"tool": "create_entity", "arguments": {
        "name": "DryBox", "type": "cube", "position": [1.0, 2.0, 3.0], "select": False}}, "the exact arguments")
    after = await ed.call("editor_status")
    ed.check_eq(len(after["mcp"]["sessions"]), len(before["mcp"]["sessions"]), "no session opened")
    found = await ed.call("find_entities", name="DryBox")
    ed.check(not harness.find_entities(found), f"nothing created: {found}")
    ed.check(not any(cli.cwd.iterdir()), "no settings.yaml or editor.log in the working directory")
    p = await cli.arun("call", "--dry-run", "create_entity", "--type", "nope")
    ed.check(p.returncode == 2 and "nope" in p.stderr, f"a bad enum in a dry run: {p.stderr}")
    times = {
        "--help": await asyncio.to_thread(cli.timed, "call", "create_entity", "--help"),
        "--dry-run": await asyncio.to_thread(cli.timed, "call", "--dry-run", "create_entity", "--name", "T"),
        "list-tools": await asyncio.to_thread(cli.timed, "list-tools"),
    }
    # R29's 300 ms is a wall-clock budget: the benchmark run enforces it (harness.over_budget)
    slow = harness.over_budget("R29 call --help, --dry-run, list-tools", {k: v * 1000 for k, v in times.items()}, 300, "ms")
    ed.check(not slow, f"R29: at most 300 ms each: {slow}")


async def test_exit_codes(ed: Editor):
    cli: Cli = S["cli"]
    p = await cli.arun("call", *ATTACH, "create_entity", "--name", "CliBox", "--type", "cube")
    ed.check(p.returncode == 0 and "CliBox" in p.stdout, f"create_entity: {p.returncode} {p.stdout} {p.stderr}")
    ed.check(harness.find_entities(await ed.call("find_entities", name="CliBox")), "the GameObject exists")
    p = await cli.arun("call", *ATTACH, "--json", "find_entities", "--name", "CliBox")
    env = envelopes(p)
    ed.check(p.returncode == 0 and len(env) == 1 and env[0]["ok"] is True and env[0]["code"] is None, f"envelope: {p.stdout}")
    for key in ("ok", "exit_code", "code", "tool", "text", "structured", "images", "error", "editor"):
        ed.check(key in env[0], f"envelope key {key}")
    ed.check(env[0]["editor"]["url"] == f"http://127.0.0.1:{PORT}/mcp" and env[0]["editor"]["spawned"] is False,
             f"editor: {env[0]['editor']}")
    # structured output
    p = await cli.arun("call", *ATTACH, "--structured", "editor_status")
    ed.check(p.returncode == 0 and json.loads(p.stdout)["project"]["name"], "--structured prints structuredContent")
    # 1: the tool says no
    p = await cli.arun("call", *ATTACH, "--json", "delete_entities", "--entities", "NoSuchThing")
    env = envelopes(p)
    ed.check(p.returncode == 1 and env[0]["code"] == "tool_error" and env[0]["ok"] is False, f"tool error: {p.stdout}")
    # 2: usage
    p = await cli.arun("call", *ATTACH, "create_entity", "--type", "nope")
    ed.check(p.returncode == 2 and "must be one of" in p.stderr, f"bad enum: {p.returncode} {p.stderr}")
    p = await cli.arun("call", *ATTACH, "--json", "create_entity", "--nmae", "x")
    ed.check(p.returncode == 2 and envelopes(p)[0]["code"] == "usage" and "did you mean --name" in envelopes(p)[0]["error"],
             f"unknown flag: {p.stdout}")
    p = await cli.arun("call", *ATTACH, "set_transform", "--entity", "CliBox", "--position", "1,2")
    ed.check(p.returncode == 2 and "position: expected 3 items" in p.stderr, f"schema validation: {p.stderr}")
    # 3: connection
    p = await cli.arun("call", "--port", str(PORT), "--token", "wrong", "editor_status")
    ed.check(p.returncode == 3 and "token" in p.stderr, f"wrong token: {p.returncode} {p.stderr}")
    p = await cli.arun("call", "--port", str(PORT), "editor_status")
    ed.check(p.returncode == 3, f"no token: {p.returncode} {p.stderr}")
    p = await cli.arun("call", "--port", str(PORT), "editor_status", extra_env={"JASPER_MCP_TOKEN": TOKEN})
    ed.check(p.returncode == 0, f"JASPER_MCP_TOKEN: {p.returncode} {p.stderr}")
    p = await cli.arun("call", "--port", str(free_port()), "editor_status")
    ed.check(p.returncode == 3 and "connection refused" in p.stderr, f"nothing listening: {p.returncode} {p.stderr}")
    p = await cli.arun("call", "--url", f"http://127.0.0.1:{PORT}/mcp", "--token", TOKEN, "editor_status")
    ed.check(p.returncode == 0, f"--url: {p.stderr}")
    p = await cli.arun("call", "editor_status", extra_env={"JASPER_MCP_URL": f"http://127.0.0.1:{PORT}/mcp",
                                                           "JASPER_MCP_TOKEN": TOKEN})
    ed.check(p.returncode == 0, f"JASPER_MCP_URL: {p.stderr}")


async def test_screenshot_image_file(ed: Editor):
    cli: Cli = S["cli"]
    shots = cli.root / "shots"
    p = await cli.arun("call", *ATTACH, "--json", "--image-dir", str(shots), "screenshot", "--view", "scene")
    env = envelopes(p)
    ed.check(p.returncode == 0 and env and env[0]["images"], f"screenshot: {p.returncode} {p.stdout[:300]} {p.stderr}")
    image = env[0]["images"][0]
    path = Path(image["path"])
    ed.check(path.parent == shots and path.name == "screenshot-1.png" and image["mime_type"] == "image/png", f"{image}")
    ed.check(path.read_bytes().startswith(b"\x89PNG") and image["bytes"] == path.stat().st_size, "a PNG file")
    ed.check(len(p.stdout) < 20000 and "iVBORw0KGgo" not in p.stdout, "no base64 on stdout")
    p = await cli.arun("call", *ATTACH, "--image-dir", str(shots), "screenshot", "--view", "scene")
    ed.check(p.returncode == 0 and (shots / "screenshot-2.png").exists(), "no clobbering: screenshot-2.png")
    ed.check(str(shots / "screenshot-2.png") in p.stderr, f"the path goes to stderr: {p.stderr}")


async def test_view_asset_font_image_file(ed: Editor):
    """S51 AC16 (R31): view_asset of a font with text writes the PNG through the CLI's image rule"""
    cli: Cli = S["cli"]
    shots = cli.root / "fonts"
    p = await cli.arun("call", *ATTACH, "--json", "--image-dir", str(shots), "view_asset", "--path", "builtin:pixel", "--text", "HP 99")
    env = envelopes(p)
    ed.check(p.returncode == 0 and env and env[0]["images"], f"view_asset: {p.returncode} {p.stdout[:300]} {p.stderr}")
    path = Path(env[0]["images"][0]["path"])
    ed.check(path.name == "view_asset-1.png" and path.read_bytes().startswith(b"\x89PNG"), f"a PNG file: {path}")


async def test_broken_server(ed: Editor):
    cli: Cli = S["cli"]
    port = free_port()
    fake = subprocess.Popen([sys.executable, str(harness.HERE / "fake_server.py"), "mcp", "--http-port", str(port), "--broken"],
                            stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    try:
        await wait_for_port(port)
        p = await cli.arun("call", "--port", str(port), "editor_status")
        ed.check(p.returncode == 4 and "malformed JSON-RPC" in p.stderr, f"garbage JSON-RPC: {p.returncode} {p.stderr}")
    finally:
        fake.kill()
        fake.wait()


async def test_timeout_cancels(ed: Editor):
    cli: Cli = S["cli"]
    t0 = time.monotonic()
    p = await cli.arun("call", *ATTACH, "--timeout", "1", "wait", "--seconds", "5")
    ed.check(p.returncode == 3 and "timed out" in p.stderr, f"--timeout 1: {p.returncode} {p.stderr}")
    ed.check(time.monotonic() - t0 < 4.5, "it did not wait for the tool")
    deadline = time.monotonic() + 10
    lines: list[str] = []
    while time.monotonic() < deadline and not lines:
        lines = [l for l in ed.stderr_text().splitlines() if "mcp-trace" in l and "done wait" in l]
        await asyncio.sleep(0.2)
    ed.check(lines and lines[-1].endswith(" cancelled"), f"the editor logs the call cancelled: {lines[-3:]}")


async def test_wait_job(ed: Editor):
    cli: Cli = S["cli"]
    # generate_shaders writes only to the export folder, the default export location or a folder of the
    # project (upstream's output_dir rule): a project folder
    out = ed.project / "call-cli-shaders"
    p = await cli.arun("call", *ATTACH, "--json", "--wait-job", "generate_shaders", "--output-dir", str(out),
                       "--wait=false", timeout=900)
    env = envelopes(p)
    ed.check(p.returncode == 0 and env and env[-1]["structured"]["status"] == "succeeded",
             f"--wait-job: {p.returncode} {p.stdout[-500:]} {p.stderr[-500:]}")
    ed.check("waiting for job" in p.stderr, "it waited for the job")
    # Ctrl-C while waiting: cancel_job, exit 3
    proc = await asyncio.create_subprocess_exec(
        *harness.editor_command(), "call", *ATTACH, "--wait-job", "generate_shaders", "--output-dir", str(ed.project / "call-cli-shaders2"),
        "--wait=false", "--backends", "all", cwd=str(cli.cwd), env=cli.env(),
        stdout=asyncio.subprocess.PIPE, stderr=asyncio.subprocess.PIPE)
    seen = ""
    while "waiting for job" not in seen:
        line = await asyncio.wait_for(proc.stderr.readline(), 120)
        if not line:
            break
        seen += line.decode()
    ed.check("waiting for job" in seen, f"the CLI waits for the job: {seen}")
    job = seen.split("waiting for job", 1)[1].split()[0]
    proc.send_signal(signal.SIGINT)
    rest = await asyncio.wait_for(proc.communicate(), 120)
    ed.check(proc.returncode == 3 and "cancel job" in rest[1].decode(), f"SIGINT: {proc.returncode} {rest[1].decode()[-400:]}")
    ed.check(await ed.call("get_job", job_id=job) is not None, "the job is known")
    deadline = time.monotonic() + 120
    status = None
    while time.monotonic() < deadline:
        status = (await ed.call("get_job", job_id=job))["status"]
        if status != "running":
            break
        await asyncio.sleep(0.5)
    ed.check(status in ("cancelled", "succeeded"), f"the job ended after cancel_job: {status}")


async def test_wait_job_build(ed: Editor):
    if os.environ.get("JASPER_SLOW_BUILD") != "1":
        harness.skip("JASPER_SLOW_BUILD=1 builds a real game (the run builds its prebuilt player runtime once)")
    cli: Cli = S["cli"]
    await ed.call("save_scene", path="scenes/CallCli")
    # the game is built with the prebuilt player (what --wait-job waits for is the build job; the
    # compile path is test_build.py's)
    runtime = await build_cache.player_runtime(ed, "glcore", "release")
    # a loaded host can make the build slow: the CLI's own timeout bounds it, not the hang guard
    with ed.busy():
        p = await cli.arun("call", *ATTACH, "--json", "--timeout", "7200", "--wait-job", "build_project",
                           '{"mode": "desktop", "build_type": "Release", "method": "prebuilt", "wait": false, "output_dir": "' +
                           str(ed.scratch.root / "call-cli-build") + '"}',
                           timeout=7300)
    env = envelopes(p)
    ed.check(p.returncode == 0 and env[-1]["structured"]["status"] == "succeeded", f"build: {p.stdout[-800:]}")
    result = env[-1]["structured"].get("result") or {}
    ed.check_eq((result.get("method_used"), result.get("player_id")), ("prebuilt", runtime.name), f"build: {p.stdout[-800:]}")


def batch_lines(*calls: tuple[str, dict]) -> str:
    return "\n".join(json.dumps({"tool": t, "arguments": a}) for t, a in calls) + "\n"


async def test_batch(ed: Editor):
    cli: Cli = S["cli"]
    lines = batch_lines(("create_entity", {"name": "B1"}), ("delete_entities", {"entities": ["NoSuchB"]}),
                        ("create_entity", {"name": "B3"}))
    p = await cli.arun("batch", *ATTACH, stdin="# a comment\n\n" + lines)
    env = envelopes(p)
    ed.check(p.returncode == 1 and len(env) == 2 and env[0]["ok"] and not env[1]["ok"], f"stops at the failure: {p.stdout}")
    p = await cli.arun("batch", *ATTACH, "--continue", stdin=lines)
    env = envelopes(p)
    ed.check(p.returncode == 1 and len(env) == 3 and env[2]["ok"], f"--continue: {p.stdout}")
    # a malformed line: nothing runs
    p = await cli.arun("batch", *ATTACH, stdin=batch_lines(("create_entity", {"name": "NeverMade"})) + "{oops\n")
    ed.check(p.returncode == 2 and "line 2" in p.stdout and
             not harness.find_entities(await ed.call("find_entities", name="NeverMade")), f"malformed line: {p.stdout}")
    p = await cli.arun("batch", *ATTACH, stdin=batch_lines(("create_entity", {"name": "NeverMade"}),
                                                          ("create_entity", {"name": "NeverMade2", "type": "nope"})))
    ed.check(p.returncode == 2 and not harness.find_entities(await ed.call("find_entities", name="NeverMade")),
             f"invalid arguments are checked before anything runs: {p.stdout}")
    # --atomic: one undo reverts every line
    atomic = batch_lines(("create_entity", {"name": "A1"}), ("create_entity", {"name": "A2"}),
                         ("create_entity", {"name": "A3"}))
    p = await cli.arun("batch", *ATTACH, "--atomic", stdin=atomic)
    env = envelopes(p)
    ed.check(p.returncode == 0 and len(env) == 3 and all(e["ok"] for e in env), f"--atomic: {p.stdout} {p.stderr}")
    for name in ("A1", "A2", "A3"):
        ed.check(harness.find_entities(await ed.call("find_entities", name=name)), f"{name} made")
    await ed.call("undo")
    for name in ("A1", "A2", "A3"):
        ed.check(not harness.find_entities(await ed.call("find_entities", name=name)), f"one undo removed {name}")
    many = batch_lines(*[("editor_status", {}) for _ in range(201)])
    p = await cli.arun("batch", *ATTACH, "--atomic", stdin=many)
    ed.check(p.returncode == 2 and envelopes(p)[0]["code"] == "usage" and "at most 200" in envelopes(p)[0]["error"],
             f"201 lines: {p.returncode} {p.stdout} {p.stderr}")


async def test_teardown(ed: Editor):
    S["cli"].remove()
