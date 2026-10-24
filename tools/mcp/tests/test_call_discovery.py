"""How `jasperengine-editor call` finds an editor (S31 R12-R14; AC8, AC9).

Discovery: two harness editors serve HTTP on their own free ports with their own projects and
share one per-user config directory (XDG_CONFIG_HOME), so both write their instance files to
<config>/mcp/instances. `editors --json` lists both; `call --project A editor_status` reaches A
(its pid and project); with no target the CLI exits 2 naming both; an instance file holds no
token and has mode 0600; after A is SIGKILLed, `editors` drops its stale file. A request with
Origin: http://evil is still refused with 403 (regression of the HTTP transport).

Spawn: `call --project <scratch copy> get_scene_tree` with no editor running starts one headless
editor (in a scratch working directory with an empty settings.yaml), answers, and leaves no
process behind; the user-config settings.yaml is untouched. A project another editor has open
fails with exit 3, project_locked, naming that editor's pid (S03 R10).
"""

from __future__ import annotations

import asyncio
import contextlib
import http.client
import json
import os
import re
import shutil
import signal
import stat
import subprocess
import tempfile
import time
from pathlib import Path

import harness
from harness import Editor, RawEditor, TestFailure, free_port, wait_for_port

CI = True
TIMEOUT = 1200
FIXTURE = harness.REPO / "tools" / "fixtures" / "scene-inputs"


def run_cli(args: list[str], config: Path, home: Path, cwd: Path, timeout: float = 900) -> subprocess.CompletedProcess:
    env = {k: v for k, v in os.environ.items() if k not in ("JASPER_MCP_URL", "JASPER_MCP_TOKEN", "DISPLAY", "WAYLAND_DISPLAY")}
    env.update({"XDG_CONFIG_HOME": str(config), "HOME": str(home)})
    return subprocess.run(harness.editor_command() + args, cwd=str(cwd), env=env, capture_output=True, text=True,
                          timeout=timeout)


def pid_alive(pid: int) -> bool:
    try:
        os.kill(pid, 0)
    except ProcessLookupError:
        return False
    except PermissionError:
        return True
    # a zombie child of ours still "exists": read its state
    with contextlib.suppress(OSError):
        return Path(f"/proc/{pid}/stat").read_text().split()[2] != "Z"
    return True


async def test_discovery():
    root = Path(tempfile.mkdtemp(prefix="jasper-discovery-"))
    config, home, cwd = root / "config", root / "home", root / "cwd"
    for d in (config, home, cwd):
        d.mkdir()
    instances = config / "jasperengine" / "mcp" / "instances"
    port_a, port_b = free_port(), free_port()
    env = {"XDG_CONFIG_HOME": str(config)}
    a = Editor(create=True, name="DiscoveryA", extra_args=["--http-port", str(port_a)], env=env)
    b = Editor(create=True, name="DiscoveryB", extra_args=["--http-port", str(port_b)], env=env)
    try:
        async with a, b:
            await wait_for_port(port_a)
            await wait_for_port(port_b)
            # both have their projects open (initialize is answered before the project loads),
            # and the next frame rewrites the instance files with them
            await a.call("editor_status")
            await b.call("editor_status")
            deadline = time.monotonic() + 30
            while True:
                p = await asyncio.to_thread(run_cli, ["editors", "--json"], config, home, cwd)
                listed = json.loads(p.stdout)
                if (len(listed) == 2 and all(e["project"] for e in listed)) or time.monotonic() > deadline:
                    break
                await asyncio.sleep(0.5)
            by_port = {e["url"]: e for e in listed}
            a.check(len(listed) == 2 and f"http://127.0.0.1:{port_a}/mcp" in by_port, f"editors lists both: {listed}")
            entry_a = by_port[f"http://127.0.0.1:{port_a}/mcp"]
            a.check(Path(entry_a["project"]).resolve() == a.project.resolve(), f"A's project: {entry_a}")
            keys = {"pid", "url", "port", "project", "cwd", "settings_path", "token_required", "version", "started"}
            for file in instances.glob("*.json"):
                info = json.loads(file.read_text())
                a.check(set(info) == keys, f"the instance file has exactly {sorted(keys)} (no token): {info}")
                a.check(stat.S_IMODE(file.stat().st_mode) == 0o600, f"{file.name} has mode 0600")

            p = await asyncio.to_thread(run_cli, ["call", "--json", "--project", str(a.project), "editor_status"],
                                        config, home, cwd)
            env_line = json.loads(p.stdout.splitlines()[0])
            a.check(p.returncode == 0 and env_line["editor"]["pid"] == entry_a["pid"] and not env_line["editor"]["spawned"],
                    f"--project reaches A: {p.stdout} {p.stderr}")
            a.check(Path(env_line["structured"]["project"]["path"]).resolve() == a.project.resolve(), "A answered")

            p = await asyncio.to_thread(run_cli, ["call", "editor_status"], config, home, cwd)
            a.check(p.returncode == 2 and str(port_a) in p.stderr and str(port_b) in p.stderr,
                    f"no target with two editors: {p.returncode} {p.stderr}")

            # Origin of a web page: refused (DNS rebinding guard)
            conn = http.client.HTTPConnection("127.0.0.1", port_b, timeout=30)
            conn.request("POST", "/mcp", body=b"{}", headers={"Origin": "http://evil", "Content-Type": "application/json"})
            status = conn.getresponse().status
            conn.close()
            a.check(status == 403, f"Origin: http://evil gets 403, not {status}")

            os.kill(entry_a["pid"], signal.SIGKILL)
            deadline = time.monotonic() + 30
            while pid_alive(entry_a["pid"]) and time.monotonic() < deadline:
                await asyncio.sleep(0.2)
            a.check((instances / f"{entry_a['pid']}.json").exists(), "a killed editor leaves its file")
            p = await asyncio.to_thread(run_cli, ["editors", "--json"], config, home, cwd)
            listed = json.loads(p.stdout)
            a.check(len(listed) == 1 and listed[0]["url"].endswith(f":{port_b}/mcp"), f"only B is live: {listed}")
            a.check(not (instances / f"{entry_a['pid']}.json").exists(), "editors pruned A's stale file")
            p = await asyncio.to_thread(run_cli, ["call", "--quiet", "editor_status"], config, home, cwd)
            a.check(p.returncode == 0, f"one live editor: no target needed: {p.stderr}")
    finally:
        shutil.rmtree(root, ignore_errors=True)


async def test_instance_file_removed_on_quit():
    root = Path(tempfile.mkdtemp(prefix="jasper-discovery-"))
    config = root / "config"
    config.mkdir()
    port = free_port()
    try:
        async with RawEditor(create=True, extra_args=["--http-port", str(port)], env={"XDG_CONFIG_HOME": str(config)}) as raw:
            await raw.initialize()
            await wait_for_port(port)
            deadline = time.monotonic() + 30
            files: list[Path] = []
            while not files and time.monotonic() < deadline:
                files = list((config / "jasperengine" / "mcp" / "instances").glob("*.json"))
                await asyncio.sleep(0.2)
            if len(files) != 1:
                raw.fail(f"one instance file while HTTP serves: {files}")
            code = await raw.close_stdin_and_wait(60)
            if code is None or files[0].exists():
                raw.fail(f"the editor removes its file when it quits (exit {code}, exists {files[0].exists()})")
    finally:
        shutil.rmtree(root, ignore_errors=True)


async def test_spawn():
    root = Path(tempfile.mkdtemp(prefix="jasper-spawn-"))
    config, home, cwd = root / "config", root / "home", root / "cwd"
    for d in (config, home, cwd):
        d.mkdir()
    project = root / "project copy"
    shutil.copytree(FIXTURE, project, ignore=shutil.ignore_patterns(".jasperengine"))
    user_settings = config / "jasperengine" / "settings.yaml"
    user_settings.parent.mkdir(parents=True)
    user_settings.write_text("recent_projects: []\n")
    before = user_settings.stat().st_mtime_ns
    try:
        t0 = time.monotonic()
        p = await asyncio.to_thread(run_cli, ["call", "--json", "--project", str(project), "get_scene_tree"],
                                    config, home, cwd)
        if p.returncode != 0:
            raise TestFailure(f"spawned call: {p.returncode}\n{p.stdout[-800:]}\n{p.stderr[-2000:]}")
        envelope = json.loads(p.stdout.splitlines()[0])
        editor = envelope["editor"]
        if not editor["spawned"] or editor["url"] is not None or not editor["pid"]:
            raise TestFailure(f"the envelope names the spawned editor: {editor}")
        print(f"     [spawn] {time.monotonic() - t0:.1f}s")
        started = re.search(r"started an editor \(pid (\d+)\) on (.*) in (.*?); it closes", p.stderr)
        scratch = started.group(3) if started else None
        if not scratch or not scratch.startswith(tempfile.gettempdir()) or Path(scratch).exists():
            raise TestFailure(f"a scratch working directory was used, then removed: {scratch}\n{p.stderr}")
        if pid_alive(editor["pid"]):
            raise TestFailure(f"no editor process remains (pid {editor['pid']})")
        if user_settings.stat().st_mtime_ns != before:
            raise TestFailure("the user's settings.yaml was rewritten")
        if any(cwd.iterdir()):
            raise TestFailure(f"nothing written to the working directory: {list(cwd.iterdir())}")

        # the project open in an editor without HTTP: project_locked, exit 3, its pid
        async with RawEditor(project=project, env={"XDG_CONFIG_HOME": str(config)}) as raw:
            await raw.initialize()
            # the project is open (and locked) once a main-thread call answers
            raw.send({"jsonrpc": "2.0", "id": 2, "method": "tools/call", "params": {"name": "editor_status", "arguments": {}}})
            await raw.response(2)
            holder = raw.proc.pid
            p = await asyncio.to_thread(run_cli, ["call", "--project", str(raw.scratch.project), "get_scene_tree"],
                                        config, home, cwd)
            if p.returncode != 3 or "project_locked" not in p.stderr:
                raw.fail(f"a locked project: exit 3, project_locked: {p.returncode}\n{p.stderr[-1500:]}")
            if holder not in [int(n) for n in re.findall(r"PID (\d+)", p.stderr)]:
                raw.fail(f"the message names the editor holding it (pid {holder}): {p.stderr[-800:]}")
    finally:
        shutil.rmtree(root, ignore_errors=True)
