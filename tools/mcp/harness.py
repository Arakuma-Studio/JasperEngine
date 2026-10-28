"""Test harness for the JasperEngine editor's MCP server.

Run with:  uv run --no-project --with-requirements tools/mcp/requirements.txt python <script>

    from harness import Editor, ToolError

    async with Editor() as ed:                       # copy of build/fixture-unity
        status = await ed.call("editor_status")
        made = await ed.call("create_entity", name="Box", type="box")
        await ed.screenshot(view="game", path="game.png")

    async with Editor(create=True) as ed: ...        # a new empty project
    async with Editor(project="/path/proj") as ed: ...   # a copy of that project
    async with Editor(url="http://127.0.0.1:47615/mcp", token="...") as ed: ...   # attach over HTTP

Each stdio Editor gets a scratch directory (temp) holding:
    project/             the copied (or created) project
    home/                $HOME of the editor
    settings.yaml        editor settings (the editor reads/writes it in its cwd); its
                         mcp.http_token is HTTP_TOKEN, so `--http-port` editors take that token
                         (the editor moves it to mcp_http_token, an owner-only file, at start)
    editor.stderr.log    everything the editor wrote to stderr (stdout is the protocol)
The scratch directory is deleted on success and kept (path printed) on failure,
or always kept with keep=True / JASPER_KEEP_SCRATCH=1.

Environment:
    JASPER_EDITOR    editor command (shell-split; default: build/linux/editor-install/bin/jasperengine-editor
                     of this checkout, else of the main checkout)
    JASPER_FIXTURE   default fixture project (default: build/fixture-unity of this or the main
                     checkout, /fixture, ~/je/fixture-unity: default_fixture(); without one, the
                     tests that need it skip)
    JASPER_XVFB      editor | harness | none   (see Editor's xvfb argument)
    JASPER_FAKE=1    use tools/mcp/fake_server.py instead of the editor (harness self-test)
    JASPER_KEEP_SCRATCH=1   keep scratch directories
    JASPER_MCP_URL   when set, Editor() with no project/url attaches to this URL instead
    JASPER_MCP_PERF_SLACK   the factor wall-clock budgets are enforced with (over_budget); unset
                     or 0: budgets are printed only (the benchmark run on server E sets it)
    JASPER_MCP_TOKEN bearer token for url mode (the editor's MCP window copies it)
"""

from __future__ import annotations

import asyncio
import base64
import contextlib
import json
import os
import shlex
import shutil
import signal
import subprocess
import sys
import tempfile
import time
from dataclasses import dataclass, field
from pathlib import Path
from typing import Any

import yaml

from mcp import ClientSession, types
from mcp.client.stdio import StdioServerParameters, stdio_client
from mcp.shared.exceptions import MCPError

HERE = Path(__file__).resolve().parent
REPO = HERE.parent.parent

# The bearer token of the editors the harness starts (settings.yaml mcp.http_token): HTTP
# requires one, and an editor with none would make a random one
HTTP_TOKEN = "jasper-harness-http-token"


# ---------------------------------------------------------------------------
# Errors and skips


class HarnessError(Exception):
    """The harness itself could not do something (editor missing, Xvfb failed...)."""


class ToolError(Exception):
    """A tool returned isError: true. `.message` is the tool's text, `.result` the CallResult."""

    def __init__(self, tool: str, message: str, result: "CallResult | None" = None):
        super().__init__(f"{tool}: {message}")
        self.tool = tool
        self.message = message
        self.result = result


class ProtocolError(Exception):
    """The server answered with a JSON-RPC error (unknown tool, bad params shape...)."""

    def __init__(self, method: str, error: MCPError):
        super().__init__(f"{method}: JSON-RPC error {getattr(error, 'code', '?')}: {error}")
        self.method = method
        self.code = getattr(error, "code", None)
        self.error = error


class TestFailure(AssertionError):
    """A failed check. Carries the editor's stderr tail so the report shows what the editor said."""


class Skip(Exception):
    """Raised (via skip()) to skip the current test cleanly."""


def skip(reason: str) -> None:
    raise Skip(reason)


# ---------------------------------------------------------------------------
# Locations


def _main_checkout() -> Path | None:
    """The main checkout when this file lives in a git worktree (its build/ holds the editor)."""
    try:
        common = subprocess.run(
            ["git", "-C", str(REPO), "rev-parse", "--path-format=absolute", "--git-common-dir"],
            capture_output=True, text=True, timeout=10,
        ).stdout.strip()
    except (OSError, subprocess.SubprocessError):
        return None
    return Path(common).parent if common else None


def _first_existing(rel: str) -> Path | None:
    for root in (REPO, _main_checkout()):
        if root and (root / rel).exists():
            return root / rel
    return None


def editor_command() -> list[str]:
    """The command that starts the editor (without the `mcp ...` arguments)."""
    if _env_flag("JASPER_FAKE"):
        return fake_editor_command()
    env = os.environ.get("JASPER_EDITOR")
    if env:
        return shlex.split(env)
    found = _first_existing("build/linux/editor-install/bin/jasperengine-editor")
    if not found:
        raise HarnessError(
            "No editor found at build/linux/editor-install/bin/jasperengine-editor "
            "(run tools/build.sh linux) and JASPER_EDITOR is not set")
    return [str(found)]


def fake_editor_command() -> list[str]:
    """The fake MCP server (tools/mcp/fake_server.py) run by this Python."""
    return [sys.executable, str(HERE / "fake_server.py")]


# Where the default fixture project (not in git: its licence is unknown) is looked for when
# JASPER_FIXTURE is not set: build/fixture-unity of this checkout or of the main checkout, the
# mount point of the linux build image (rb.sh --img, tools/build.sh mcp-tests: /fixture), and the
# build servers' copy (~/je/fixture-unity). tools/build.sh mcp-tests looks in the same places.
FIXTURE_HOME = Path("je") / "fixture-unity"
FIXTURE_MOUNT = Path("/fixture")


class FixtureMissing(HarnessError, Skip):
    """No default fixture project anywhere: the tests that need it skip (run_tests.py)."""


def fixture_candidates() -> list[Path]:
    found = [root / "build" / "fixture-unity" for root in (REPO, _main_checkout()) if root]
    found.append(FIXTURE_MOUNT)
    with contextlib.suppress(RuntimeError):
        found.append(Path.home() / FIXTURE_HOME)
    return found


def default_fixture() -> Path:
    """The default fixture project: JASPER_FIXTURE, else the first of fixture_candidates() that
    holds a project.yaml. Raises FixtureMissing (a Skip) when there is none."""
    env = os.environ.get("JASPER_FIXTURE")
    if env:
        if not (Path(env) / "project.yaml").exists():
            raise HarnessError(f"JASPER_FIXTURE={env} has no project.yaml")
        return Path(env)
    for candidate in fixture_candidates():
        if (candidate / "project.yaml").exists():
            return candidate
    raise FixtureMissing("needs the default fixture project, found nowhere: set JASPER_FIXTURE, or put it at "
                         + ", ".join(str(c) for c in fixture_candidates()))


# ---------------------------------------------------------------------------
# Xvfb


def _die_with_parent() -> None:
    """Linux: the child gets SIGTERM when the harness dies, even by SIGKILL (no stray Xvfb)."""
    with contextlib.suppress(Exception):
        import ctypes
        ctypes.CDLL("libc.so.6", use_errno=True).prctl(1, signal.SIGTERM)  # PR_SET_PDEATHSIG


class Xvfb:
    """A private X server: `Xvfb -displayfd` picks a free display number."""

    def __init__(self, size: str = "1600x900x24"):
        self.size = size
        self.proc: subprocess.Popen | None = None
        self.display: str | None = None

    def start(self, timeout: float = 15.0) -> str:
        if not shutil.which("Xvfb"):
            raise HarnessError("Xvfb is not installed")
        r, w = os.pipe()
        try:
            self.proc = subprocess.Popen(
                ["Xvfb", "-displayfd", str(w), "-screen", "0", self.size, "-nolisten", "tcp", "-noreset"],
                pass_fds=(w,), stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
                start_new_session=True, preexec_fn=_die_with_parent,
            )
        finally:
            os.close(w)
        deadline = time.monotonic() + timeout
        buf = b""
        os.set_blocking(r, False)
        try:
            while not buf.endswith(b"\n"):
                if time.monotonic() > deadline or self.proc.poll() is not None:
                    self.stop()
                    raise HarnessError("Xvfb did not report a display number")
                try:
                    chunk = os.read(r, 64)
                except BlockingIOError:
                    time.sleep(0.02)
                    continue
                if not chunk:
                    time.sleep(0.02)
                    continue
                buf += chunk
        finally:
            os.close(r)
        self.display = ":" + buf.decode().strip()
        return self.display

    def stop(self) -> None:
        if self.proc and self.proc.poll() is None:
            self.proc.terminate()
            try:
                self.proc.wait(5)
            except subprocess.TimeoutExpired:
                self.proc.kill()
                self.proc.wait()
        self.proc = None


# ---------------------------------------------------------------------------
# Scratch directory and launch spec


@dataclass
class Scratch:
    """A temp working dir for one editor: project/, home/, settings.yaml, editor.stderr.log."""

    root: Path
    project: Path
    home: Path
    stderr_log: Path

    @classmethod
    def make(cls, project: str | Path | None = None, create: bool = False,
             prefix: str = "jasper-mcp-", settings: dict[str, Any] | None = None) -> "Scratch":
        root = Path(tempfile.mkdtemp(prefix=prefix))
        home = root / "home"
        home.mkdir()
        proj = root / "project"
        if not create:
            src = Path(project) if project else default_fixture()
            if src.is_file():  # project.yaml given
                src = src.parent
            if not (src / "project.yaml").exists():
                raise HarnessError(f"{src} has no project.yaml")
            shutil.copytree(src, proj, symlinks=True, ignore=shutil.ignore_patterns(".git"))
        extra_settings = settings
        settings = {
            "window": {"width": 1600, "height": 900, "maximized": True},
            "mcp": {"http_token": HTTP_TOKEN},
        }
        if not create:
            settings["last_project_path"] = str(proj)
            settings["recent_projects"] = [str(proj)]
        settings.update(extra_settings or {})
        (root / "settings.yaml").write_text(yaml.safe_dump(settings, sort_keys=False))
        return cls(root=root, project=proj, home=home, stderr_log=root / "editor.stderr.log")

    def remove(self) -> None:
        shutil.rmtree(self.root, ignore_errors=True)


def launch_spec(scratch: Scratch, *, create: bool = False, name: str = "Scratch",
                command: list[str] | None = None, display: str | None = None,
                headless: bool = True, extra_args: list[str] | None = None,
                inherit_display: bool = False, extra_env: dict[str, str] | None = None,
                ui_probe: bool = False) -> tuple[list[str], dict[str, str], Path]:
    """argv, environment and cwd of `<editor> mcp ...` for this scratch dir.

    Also used by tests that need a raw subprocess (e.g. the stdout purity test).
    """
    cmd = list(command or editor_command())
    args = ["mcp"]
    if create:
        args += ["--create", str(scratch.project), "--name", name]
    else:
        args += ["--project", str(scratch.project)]
    if headless:
        args.append("--headless")
    args += extra_args or []
    env = dict(os.environ)
    for k in ("XDG_CONFIG_HOME", "XDG_DATA_HOME", "XDG_CACHE_HOME", "XDG_STATE_HOME", "XDG_RUNTIME_DIR"):
        env.pop(k, None)
    if not inherit_display:
        env.pop("DISPLAY", None)
        env.pop("WAYLAND_DISPLAY", None)
    if display:
        env["DISPLAY"] = display
    env["HOME"] = str(scratch.home)
    env["LIBGL_ALWAYS_SOFTWARE"] = "1"
    # the engine's Lua state used off the main thread aborts the editor (LuaBinding::ThreadCheck)
    env.setdefault("JASPER_LUA_THREAD_CHECK", "abort")
    # test-only widget locator (editor/util/UIProbe.h): the editor writes the rectangles of the
    # widgets it names there (Editor.ui_rect)
    if ui_probe:
        env["JASPER_UI_PROBE"] = str(scratch.root / "ui-probe.json")
    else:
        env.pop("JASPER_UI_PROBE", None)
    env.update(extra_env or {})
    return cmd + args, env, scratch.root


# ---------------------------------------------------------------------------
# Results


@dataclass
class CallResult:
    tool: str
    is_error: bool
    texts: list[str] = field(default_factory=list)
    images: list[tuple[bytes, str]] = field(default_factory=list)  # (data, mime type)
    structured: Any = None
    raw: Any = None

    @property
    def text(self) -> str:
        return "\n".join(self.texts)

    @property
    def value(self) -> Any:
        """structuredContent if present, else the text parsed as JSON if it is JSON, else the text."""
        if self.structured is not None:
            return self.structured
        t = self.text
        s = t.strip()
        if s[:1] in "{[":
            with contextlib.suppress(ValueError):
                return json.loads(s)
        return t

    @classmethod
    def from_mcp(cls, tool: str, r: Any) -> "CallResult":
        out = cls(tool=tool, is_error=bool(getattr(r, "is_error", False)), raw=r,
                  structured=getattr(r, "structured_content", None))
        for c in getattr(r, "content", None) or []:
            kind = getattr(c, "type", None)
            if kind == "text":
                out.texts.append(c.text)
            elif kind == "image":
                out.images.append((base64.b64decode(c.data), c.mime_type))
        return out


# ---------------------------------------------------------------------------
# Wall-clock budgets

def perf_slack() -> float:
    """The factor wall-clock budgets are enforced with: JASPER_MCP_PERF_SLACK, 0 (not enforced)
    unless set. A shared build server's load alone misses them, so a functional run asserts the
    behaviour and prints the times; the benchmark run on the benchmark-only server E enforces them."""
    try:
        return max(0.0, float(os.environ.get("JASPER_MCP_PERF_SLACK", "") or 0))
    except ValueError:
        return 0.0


def over_budget(what: str, measured: dict[str, float], budget: float, unit: str = "s") -> dict[str, float]:
    """Prints `[bench] what: name value, ... (budget B)` and returns the measures over budget x
    perf_slack(): always empty in a functional run (perf_slack() 0), for the benchmark run to fail
    on. The measures and the budget are in `unit`."""
    shown = ", ".join(f"{name} {value:.4g} {unit}" for name, value in measured.items())
    slack = perf_slack()
    enforced = f", enforced x{slack:g}" if slack > 0 else ", not enforced"
    print(f"     [bench] {what}: {shown} (budget {budget:g} {unit}{enforced})", flush=True)
    if slack <= 0:
        return {}
    return {name: value for name, value in measured.items() if value > budget * slack}


# ---------------------------------------------------------------------------
# Editor


def _env_flag(name: str) -> bool:
    return os.environ.get(name, "") not in ("", "0", "false", "no")


# Tools that run game frames (wait, send_input, play step): the editor ends them when the game stops
# advancing, so the client only guards against a hung editor (Editor.call_raw)
FRAME_TOOLS = frozenset({"wait", "send_input", "play"})
FRAME_CALL_TIMEOUT = 3600.0


class Editor:
    """One editor driven over MCP. Use as `async with Editor(...) as ed:`.

    project   project dir or project.yaml to copy (default: JASPER_FIXTURE / build/fixture-unity)
    create    start with a new empty project (`mcp --create <scratch>/project --name <name>`)
    url       attach to a running editor over Streamable HTTP instead of launching one
    token     bearer token for url mode (default JASPER_MCP_TOKEN)
    scratch_prefix  name prefix of the scratch directory (e.g. non-ASCII, to test such paths)
    xvfb      "editor"  (default) pass --headless with DISPLAY unset: the editor spawns its Xvfb
              "harness" the harness spawns `Xvfb -displayfd` and sets DISPLAY
              "none"    inherit the caller's DISPLAY (a visible editor on a desktop)
    command   editor command (default: editor_command()); fake=True uses tools/mcp/fake_server.py
    extra_args  appended to the `mcp` command line
    env       extra environment variables of the editor (stdio mode)
    settings  extra top-level keys of the scratch settings.yaml (stdio mode), e.g. {"mcp": {...}}
    startup_timeout  seconds for the editor to answer initialize
    call_timeout     default seconds per tool call
    keep      keep the scratch dir even on success
    ui_probe  the editor writes the rectangles of the widgets it names (editor/util/UIProbe.h), read
              by ui_items() / ui_rect(): UI tests click widgets wherever the layout puts them
    """

    def __init__(self, project: str | Path | None = None, *, create: bool = False, name: str = "Scratch",
                 url: str | None = None, token: str | None = None, xvfb: str | None = None,
                 command: list[str] | None = None, fake: bool | None = None,
                 extra_args: list[str] | None = None, env: dict[str, str] | None = None,
                 settings: dict[str, Any] | None = None, startup_timeout: float = 180.0,
                 call_timeout: float = 300.0, keep: bool | None = None, label: str = "",
                 scratch_prefix: str | None = None, ui_probe: bool = False):
        if url is None and project is None and not create and os.environ.get("JASPER_MCP_URL"):
            url = os.environ["JASPER_MCP_URL"]
        if fake is None:
            fake = _env_flag("JASPER_FAKE")
        self.project_src = project
        self.create = create
        self.name = name
        self.url = url
        self.token = token or os.environ.get("JASPER_MCP_TOKEN")
        self.xvfb_mode = xvfb or os.environ.get("JASPER_XVFB", "editor")
        if self.xvfb_mode not in ("editor", "harness", "none"):
            raise HarnessError(f"xvfb must be editor, harness or none, not {self.xvfb_mode!r}")
        self.command = command or (fake_editor_command() if fake else None)
        self.extra_args = extra_args or []
        self.extra_env = env or {}
        self.extra_settings = settings or {}
        self.startup_timeout = startup_timeout
        self.call_timeout = call_timeout
        self.keep = _env_flag("JASPER_KEEP_SCRATCH") if keep is None else keep
        self.label = label
        self.scratch_prefix = scratch_prefix
        self.ui_probe = ui_probe

        self.scratch: Scratch | None = None
        self.session: ClientSession | None = None
        self.init_result: Any = None
        self.instructions: str = ""
        self.tool_list: list[Any] = []
        self.failed = False
        # the runner's hang guard (run_tests.py) measures the time without a tool call in flight
        self.in_flight = 0
        self.last_activity = time.monotonic()
        self._stack: contextlib.AsyncExitStack | None = None
        self._xvfb: Xvfb | None = None
        self._stderr_file = None
        self._console_cursor: Any = None

    # -- lifecycle ---------------------------------------------------------

    @property
    def project(self) -> Path | None:
        """The scratch project directory (stdio mode)."""
        return self.scratch.project if self.scratch else None

    async def __aenter__(self) -> "Editor":
        try:
            await self.start()
        except BaseException:
            self.failed = True
            await self.close()
            raise
        return self

    async def __aexit__(self, exc_type, exc, tb) -> None:
        if exc_type is not None and not issubclass(exc_type, Skip):
            self.failed = True
        await self.close()

    async def start(self) -> None:
        self._stack = contextlib.AsyncExitStack()
        if self.url:
            from mcp.client.streamable_http import create_mcp_http_client, streamable_http_client
            headers = {"Authorization": f"Bearer {self.token}"} if self.token else None
            http = await self._stack.enter_async_context(create_mcp_http_client(headers=headers))
            streams = await self._stack.enter_async_context(streamable_http_client(self.url, http_client=http))
        else:
            extra = {"prefix": self.scratch_prefix} if self.scratch_prefix else {}
            self.scratch = Scratch.make(self.project_src, create=self.create, settings=self.extra_settings, **extra)
            display = None
            inherit = self.xvfb_mode == "none"
            if self.xvfb_mode == "harness":
                self._xvfb = Xvfb()
                display = self._xvfb.start()
            argv, env, cwd = launch_spec(
                self.scratch, create=self.create, name=self.name, command=self.command,
                display=display, extra_args=self.extra_args, inherit_display=inherit, extra_env=self.extra_env,
                ui_probe=self.ui_probe)
            self.argv = argv
            self._stderr_file = open(self.scratch.stderr_log, "w", buffering=1)
            params = StdioServerParameters(command=argv[0], args=argv[1:], env=env, cwd=str(cwd))
            streams = await self._stack.enter_async_context(stdio_client(params, errlog=self._stderr_file))
        read, write = streams[0], streams[1]
        self.session = await self._stack.enter_async_context(
            ClientSession(read, write, read_timeout_seconds=self.call_timeout))
        try:
            self.init_result = await asyncio.wait_for(self.session.initialize(), self.startup_timeout)
        except asyncio.TimeoutError:
            raise HarnessError(
                f"editor did not answer initialize within {self.startup_timeout}s\n" + self._tail_block())
        except Exception as e:  # noqa: BLE001 - the editor died or wrote garbage
            raise HarnessError(f"initialize failed: {e!r}\n" + self._tail_block()) from e
        self.instructions = self.session.instructions or ""
        await self.tools()

    async def close(self) -> None:
        if self._stack:
            stack, self._stack = self._stack, None
            with contextlib.suppress(Exception):
                await stack.aclose()
        self.session = None
        if self._xvfb:
            self._xvfb.stop()
            self._xvfb = None
        if self._stderr_file:
            self._stderr_file.close()
            self._stderr_file = None
        if self.scratch:
            if self.failed or self.keep:
                print(f"[harness] scratch kept: {self.scratch.root}", file=sys.stderr)
            else:
                self.scratch.remove()

    # -- tools -------------------------------------------------------------

    async def tools(self, groups: str | list[str] | None = "all") -> list[Any]:
        """tools/list (all pages). Items are mcp Tool objects (.name, .description, .input_schema, .meta).

        groups: the tool groups to list (params._meta["jasper/groups"]): "all" (default: every
        registered tool, so tests never need tool_groups first), a list of group names, or None
        for what the session lists by default (core plus the groups it enabled)."""
        tools: list[Any] = []
        cursor = None
        meta = {"jasper/groups": groups} if groups is not None else None
        while True:
            params = types.PaginatedRequestParams(cursor=cursor, _meta=meta) if cursor or meta else None
            r = await self.session.list_tools(params=params)
            tools += list(r.tools)
            cursor = getattr(r, "next_cursor", None)
            if not cursor:
                break
        if groups == "all":
            self.tool_list = tools
        return tools

    @property
    def tool_names(self) -> set[str]:
        return {t.name for t in self.tool_list}

    def has_tool(self, name: str) -> bool:
        return name in self.tool_names

    def tool(self, name: str) -> Any:
        for t in self.tool_list:
            if t.name == name:
                return t
        return None

    def require_tools(self, *names: str) -> None:
        """Skip the test unless every tool exists (for features not built yet)."""
        missing = [n for n in names if not self.has_tool(n)]
        if missing:
            skip("tool(s) not available yet: " + ", ".join(missing))

    async def call_raw(self, tool: str, args: dict | None = None, *, timeout: float | None = None,
                       progress=None) -> CallResult:
        """tools/call; returns the CallResult whatever isError is. JSON-RPC errors raise ProtocolError.

        The tools that run game frames (FRAME_TOOLS) take as long as their frames take: the editor
        ends them itself when the game stops advancing (60 s without a frame), so their default
        client timeout is FRAME_CALL_TIMEOUT, a guard against a hung editor only. However slowly a
        loaded machine renders, a wait for N frames is never cut short here."""
        if not self.session:
            raise HarnessError("editor is not running")
        if timeout is None:
            timeout = max(self.call_timeout, FRAME_CALL_TIMEOUT) if tool in FRAME_TOOLS else self.call_timeout
        self.in_flight += 1
        try:
            r = await self.session.call_tool(
                tool, args or {}, read_timeout_seconds=timeout, progress_callback=progress)
        except MCPError as e:
            raise ProtocolError(f"tools/call {tool}", e) from e
        finally:
            self.in_flight -= 1
            self.last_activity = time.monotonic()
        return CallResult.from_mcp(tool, r)

    def idle_seconds(self) -> float:
        """Seconds since the last tool call ended; 0 while one is in flight."""
        return 0.0 if self.in_flight else time.monotonic() - self.last_activity

    @contextlib.contextmanager
    def busy(self):
        """Work with a timeout of its own that drives this editor some other way (a CLI run
        attached to it, polling a build for minutes) counts as a call in flight: the runner's hang
        guard (TIMEOUT) does not cut it short."""
        self.in_flight += 1
        try:
            yield
        finally:
            self.in_flight -= 1
            self.last_activity = time.monotonic()

    async def call(self, tool: str, _args: dict | None = None, *, expect_error: bool = False,
                   timeout: float | None = None, **args: Any) -> Any:
        """Call a tool and return its value (structuredContent, else JSON-parsed text, else text).

        Raises ToolError on isError (unless expect_error=True, which returns the error message and
        fails the test when the call succeeds). Arguments come as keywords or a dict (for names
        that are Python keywords or clash with `timeout`/`expect_error`).
        """
        merged = dict(_args or {})
        merged.update(args)
        r = await self.call_raw(tool, merged, timeout=timeout)
        if expect_error:
            if not r.is_error:
                self.fail(f"{tool} was expected to fail but returned: {r.text[:500]}")
            return r.text
        if r.is_error:
            raise ToolError(tool, r.text, r)
        return r.value

    async def batch(self, calls: list[tuple[str, dict]] | list[dict], *, undo_group: bool = False,
                    stop_on_error: bool = True) -> Any:
        """The `batch` tool: calls as [(tool, args)] or [{"tool":..., "arguments":...}]."""
        norm = [c if isinstance(c, dict) else {"tool": c[0], "arguments": c[1]} for c in calls]
        return await self.call("batch", calls=norm, undo_group=undo_group, stop_on_error=stop_on_error)

    async def screenshot(self, view: str = "game", path: str | Path | None = None, **args: Any) -> bytes:
        """Call the screenshot tool and save its first image to `path` (default scratch/<view>.png).

        Returns the image bytes. Fails the test when the result holds no image.
        """
        self.require_tools("screenshot")
        r = await self.call_raw("screenshot", {"view": view, **args})
        if r.is_error:
            raise ToolError("screenshot", r.text, r)
        if not r.images:
            self.fail(f"screenshot returned no image content: {r.text[:300]}")
        data, mime = r.images[0]
        if path is None:
            base = self.scratch.root if self.scratch else Path(tempfile.gettempdir())
            path = base / f"{view}.png"
        Path(path).parent.mkdir(parents=True, exist_ok=True)
        Path(path).write_bytes(data)
        if mime == "image/png" and not data.startswith(b"\x89PNG\r\n\x1a\n"):
            self.fail("screenshot said image/png but the data is not a PNG")
        return data

    def ui_items(self) -> dict[str, list[float]]:
        """The widgets the editor drew whole in its last frame, {name: [x0, y0, x1, y1]} in editor
        window pixels (Editor(ui_probe=True); {} before the first frame or without it)."""
        if not self.scratch:
            return {}
        path = self.scratch.root / "ui-probe.json"
        with contextlib.suppress(OSError, ValueError):
            return json.loads(path.read_text()).get("items", {})
        return {}

    async def ui_rect(self, name: str, timeout: float = 10.0) -> list[float] | None:
        """A widget's rectangle once the editor draws it whole (None after timeout seconds)."""
        deadline = time.monotonic() + timeout
        while True:
            rect = self.ui_items().get(name)
            if rect or time.monotonic() > deadline:
                return rect
            await asyncio.sleep(0.1)

    async def console_since(self, since: Any = "last", **args: Any) -> list[dict]:
        """Console entries newer than `since` (a cursor). "last" (default) continues from the previous
        console_since() call, None reads from the start. Returns a list of entries (dicts with at least
        `message`); remembers the returned cursor."""
        self.require_tools("read_console")
        if since == "last":
            since = self._console_cursor
        a = dict(args)
        if since is not None:
            a["since"] = since
        result = await self.call("read_console", a)
        self._console_cursor = result["next_cursor"]
        return result["entries"]

    async def ping(self) -> None:
        await self.session.send_ping()

    # -- stderr and assertions ----------------------------------------------

    def stderr_tail(self, lines: int = 40) -> str:
        """The last lines the editor wrote to stderr ("" when attached over HTTP)."""
        if not self.scratch or not self.scratch.stderr_log.exists():
            return ""
        if self._stderr_file:
            with contextlib.suppress(Exception):
                self._stderr_file.flush()
        text = self.scratch.stderr_log.read_text(errors="replace")
        return "\n".join(text.splitlines()[-lines:])

    def stderr_text(self) -> str:
        if not self.scratch or not self.scratch.stderr_log.exists():
            return ""
        return self.scratch.stderr_log.read_text(errors="replace")

    def _tail_block(self, lines: int = 40) -> str:
        tail = self.stderr_tail(lines)
        where = f" ({self.scratch.stderr_log})" if self.scratch else ""
        return f"--- editor stderr tail{where} ---\n{tail}\n--- end ---" if tail else "(no editor stderr)"

    def fail(self, message: str) -> None:
        self.failed = True
        raise TestFailure(message + "\n" + self._tail_block())

    def check(self, cond: Any, message: str = "check failed") -> None:
        if not cond:
            self.fail(message)

    def check_eq(self, actual: Any, expected: Any, message: str = "") -> None:
        if actual != expected:
            self.fail(f"{message + ': ' if message else ''}expected {expected!r}, got {actual!r}")

    def check_in(self, needle: Any, haystack: Any, message: str = "") -> None:
        if needle not in haystack:
            shown = repr(haystack)
            self.fail(f"{message + ': ' if message else ''}{needle!r} not in {shown[:800]}")

    async def check_error(self, tool: str, _args: dict | None = None, *, contains: str | None = None,
                          **args: Any) -> str:
        """Call a tool that must fail (isError or JSON-RPC error); returns the message."""
        try:
            msg = await self.call(tool, _args, expect_error=True, **args)
        except ProtocolError as e:
            msg = str(e)
        if contains and contains.lower() not in msg.lower():
            self.fail(f"{tool} error should mention {contains!r}: {msg[:500]}")
        return msg

    async def check_alive(self, timeout: float = 15.0) -> None:
        """Fail unless the editor still answers ping."""
        if self.session is None:
            self.fail("editor session is closed")
        try:
            await asyncio.wait_for(self.session.send_ping(), timeout)
        except Exception as e:  # noqa: BLE001
            self.fail(f"editor does not answer ping: {e!r}")


# ---------------------------------------------------------------------------
# Raw stdio access (protocol-level tests)


class RawEditor:
    """The editor launched as a plain subprocess speaking newline-delimited JSON-RPC on stdio,
    with no MCP client in between. For protocol tests (stdout purity, version negotiation).

        async with RawEditor() as raw:
            raw.send({"jsonrpc": "2.0", "id": 1, "method": "initialize", "params": {...}})
            msg = await raw.response(1)          # waits for the response with id 1
        raw.lines   # every stdout line seen, raw.bad_lines: those that are not JSON-RPC 2.0
    """

    def __init__(self, project: str | Path | None = None, *, create: bool = False, name: str = "Scratch",
                 xvfb: str | None = None, command: list[str] | None = None, keep: bool | None = None,
                 extra_args: list[str] | None = None, env: dict[str, str] | None = None,
                 settings: dict[str, Any] | None = None):
        self.project_src, self.create, self.name = project, create, name
        self.extra_env = env or {}
        self.extra_settings = settings or {}
        self.xvfb_mode = xvfb or os.environ.get("JASPER_XVFB", "editor")
        self.command = command
        self.extra_args = extra_args or []
        self.keep = _env_flag("JASPER_KEEP_SCRATCH") if keep is None else keep
        self.scratch: Scratch | None = None
        self.proc: asyncio.subprocess.Process | None = None
        self.lines: list[str] = []
        self.bad_lines: list[str] = []
        self.messages: list[dict] = []
        self.failed = False
        self._xvfb: Xvfb | None = None
        self._stderr = None
        self._reader: asyncio.Task | None = None
        self._new = asyncio.Event()

    async def __aenter__(self) -> "RawEditor":
        self.scratch = Scratch.make(self.project_src, create=self.create, settings=self.extra_settings)
        display = None
        if self.xvfb_mode == "harness":
            self._xvfb = Xvfb()
            display = self._xvfb.start()
        argv, env, cwd = launch_spec(self.scratch, create=self.create, name=self.name, command=self.command,
                                     display=display, extra_args=self.extra_args,
                                     inherit_display=self.xvfb_mode == "none", extra_env=self.extra_env)
        self._stderr = open(self.scratch.stderr_log, "w")
        self.proc = await asyncio.create_subprocess_exec(
            *argv, stdin=asyncio.subprocess.PIPE, stdout=asyncio.subprocess.PIPE, stderr=self._stderr,
            env=env, cwd=str(cwd), start_new_session=True, limit=64 * 1024 * 1024)
        self._reader = asyncio.create_task(self._read())
        return self

    async def _read(self) -> None:
        assert self.proc and self.proc.stdout
        while True:
            line = await self.proc.stdout.readline()
            if not line:
                break
            text = line.decode("utf-8", errors="replace").rstrip("\n")
            self.lines.append(text)
            try:
                msg = json.loads(text)
                if not isinstance(msg, dict) or msg.get("jsonrpc") != "2.0":
                    raise ValueError("not a JSON-RPC 2.0 object")
                self.messages.append(msg)
            except ValueError:
                self.bad_lines.append(text)
            self._new.set()
        self._new.set()

    def send(self, msg: dict | str) -> None:
        data = msg if isinstance(msg, str) else json.dumps(msg)
        assert self.proc and self.proc.stdin
        self.proc.stdin.write(data.encode() + b"\n")

    async def response(self, id: Any, timeout: float = 180.0) -> dict:
        """Wait for the JSON-RPC response with this id."""
        deadline = time.monotonic() + timeout
        while True:
            for m in self.messages:
                if m.get("id") == id and ("result" in m or "error" in m):
                    return m
            if self.proc.stdout.at_eof() and self._reader.done():
                raise HarnessError(f"editor closed stdout before answering id {id}\n{self.stderr_tail()}")
            left = deadline - time.monotonic()
            if left <= 0:
                raise HarnessError(f"no response to id {id} within {timeout}s\n{self.stderr_tail()}")
            self._new.clear()
            with contextlib.suppress(asyncio.TimeoutError):
                await asyncio.wait_for(self._new.wait(), min(left, 1.0))

    async def initialize(self, version: str = "2025-11-25", id: Any = 1, timeout: float = 180.0) -> dict:
        self.send({"jsonrpc": "2.0", "id": id, "method": "initialize", "params": {
            "protocolVersion": version, "capabilities": {},
            "clientInfo": {"name": "jasper-harness-raw", "version": "1"}}})
        r = await self.response(id, timeout)
        self.send({"jsonrpc": "2.0", "method": "notifications/initialized"})
        return r

    async def close_stdin_and_wait(self, timeout: float = 30.0) -> int | None:
        """Close stdin (the MCP shutdown signal) and wait for the editor to exit; None if it didn't."""
        assert self.proc and self.proc.stdin
        with contextlib.suppress(Exception):
            self.proc.stdin.close()
        try:
            return await asyncio.wait_for(self.proc.wait(), timeout)
        except asyncio.TimeoutError:
            return None

    def stderr_tail(self, lines: int = 40) -> str:
        if not self.scratch or not self.scratch.stderr_log.exists():
            return ""
        if self._stderr:
            with contextlib.suppress(Exception):
                self._stderr.flush()
        text = self.scratch.stderr_log.read_text(errors="replace")
        return "--- editor stderr tail ---\n" + "\n".join(text.splitlines()[-lines:]) + "\n--- end ---"

    def fail(self, message: str) -> None:
        self.failed = True
        raise TestFailure(message + "\n" + self.stderr_tail())

    async def __aexit__(self, exc_type, exc, tb) -> None:
        if exc_type is not None and not issubclass(exc_type, Skip):
            self.failed = True
        if self.proc and self.proc.returncode is None:
            with contextlib.suppress(Exception):
                self.proc.stdin.close()
            try:
                await asyncio.wait_for(self.proc.wait(), 10)
            except asyncio.TimeoutError:
                with contextlib.suppress(ProcessLookupError):
                    os.killpg(self.proc.pid, signal.SIGTERM)
                try:
                    await asyncio.wait_for(self.proc.wait(), 5)
                except asyncio.TimeoutError:
                    with contextlib.suppress(ProcessLookupError):
                        os.killpg(self.proc.pid, signal.SIGKILL)
                    await self.proc.wait()
        if self._reader:
            with contextlib.suppress(Exception):
                await asyncio.wait_for(self._reader, 5)
        if self._xvfb:
            self._xvfb.stop()
        if self._stderr:
            self._stderr.close()
        if self.scratch:
            if self.failed or self.keep:
                print(f"[harness] scratch kept: {self.scratch.root}", file=sys.stderr)
            else:
                self.scratch.remove()


# ---------------------------------------------------------------------------
# Raw Streamable HTTP access (transport-level tests)


def free_port() -> int:
    """A TCP port on 127.0.0.1 that nothing listens on right now."""
    import socket
    with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as s:
        s.bind(("127.0.0.1", 0))
        return s.getsockname()[1]


@dataclass
class HttpAnswer:
    status: int
    headers: dict[str, str]           # lower-case names
    messages: list[dict]              # JSON-RPC messages of the body (one, or the SSE events)
    body: str

    @property
    def sse(self) -> bool:
        return self.headers.get("content-type", "").startswith("text/event-stream")

    @property
    def response(self) -> dict | None:
        """The JSON-RPC response (the last message with an id)."""
        for m in reversed(self.messages):
            if "id" in m and ("result" in m or "error" in m):
                return m
        return None

    def notifications(self, method: str | None = None) -> list[dict]:
        return [m for m in self.messages if "id" not in m and (method is None or m.get("method") == method)]


class HttpSession:
    """The editor's MCP over Streamable HTTP with no client library in between: one JSON-RPC
    message per POST (http.client), the Mcp-Session-Id kept after initialize.

        http = HttpSession(port)
        http.initialize()
        a = http.request("tools/call", {"name": "tool_groups", "arguments": {}}, sse=True)
        a.sse, a.notifications("notifications/tools/list_changed"), a.response["result"]
    """

    def __init__(self, port: int, *, token: str | None = None, host: str = "127.0.0.1"):
        self.port, self.token, self.host = port, token, host
        self.session_id: str | None = None
        self._next_id = 0

    def post(self, message: dict | str, *, sse: bool = True, headers: dict[str, str] | None = None,
             timeout: float = 120.0) -> HttpAnswer:
        import http.client
        conn = http.client.HTTPConnection(self.host, self.port, timeout=timeout)
        try:
            h = {"Content-Type": "application/json",
                 "Accept": "application/json, text/event-stream" if sse else "application/json"}
            if self.token:
                h["Authorization"] = f"Bearer {self.token}"
            if self.session_id:
                h["Mcp-Session-Id"] = self.session_id
            h.update(headers or {})
            body = message if isinstance(message, str) else json.dumps(message)
            conn.request("POST", "/mcp", body=body.encode(), headers=h)
            r = conn.getresponse()
            text = r.read().decode("utf-8", errors="replace")
            hdrs = {k.lower(): v for k, v in r.getheaders()}
        finally:
            conn.close()
        msgs: list[dict] = []
        if hdrs.get("content-type", "").startswith("text/event-stream"):
            for event in text.replace("\r\n", "\n").split("\n\n"):
                data = "\n".join(line[5:].lstrip() for line in event.split("\n") if line.startswith("data:"))
                if data:
                    msgs.append(json.loads(data))
        elif text.strip():
            with contextlib.suppress(ValueError):
                value = json.loads(text)
                if isinstance(value, dict):
                    msgs.append(value)
        return HttpAnswer(r.status, hdrs, msgs, text)

    def request(self, method: str, params: dict | None = None, *, sse: bool = True,
                timeout: float = 120.0) -> HttpAnswer:
        self._next_id += 1
        msg: dict[str, Any] = {"jsonrpc": "2.0", "id": self._next_id, "method": method}
        if params is not None:
            msg["params"] = params
        return self.post(msg, sse=sse, timeout=timeout)

    def notify(self, method: str, params: dict | None = None) -> HttpAnswer:
        msg: dict[str, Any] = {"jsonrpc": "2.0", "method": method}
        if params is not None:
            msg["params"] = params
        return self.post(msg)

    def initialize(self, version: str = "2025-11-25") -> dict:
        a = self.request("initialize", {"protocolVersion": version, "capabilities": {},
                                        "clientInfo": {"name": "jasper-harness-http", "version": "1"}})
        if a.status != 200 or not a.response or "result" not in a.response:
            raise HarnessError(f"HTTP initialize failed: {a.status} {a.body[:300]}")
        self.session_id = a.headers.get("mcp-session-id")
        self.notify("notifications/initialized")
        return a.response["result"]


async def wait_for_port(port: int, timeout: float = 60.0) -> None:
    """Until something accepts connections on 127.0.0.1:port."""
    import socket
    deadline = time.monotonic() + timeout
    while True:
        with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as s:
            s.settimeout(1.0)
            if s.connect_ex(("127.0.0.1", port)) == 0:
                return
        if time.monotonic() > deadline:
            raise HarnessError(f"nothing listens on port {port} after {timeout}s")
        await asyncio.sleep(0.2)


def tool_meta(tool: Any) -> dict:
    """A listed tool's _meta as a dict ({"jasper/group", "jasper/module"} from this editor)."""
    meta = getattr(tool, "meta", None)
    if meta is None:
        return {}
    if isinstance(meta, dict):
        return meta
    with contextlib.suppress(Exception):
        return dict(meta.model_dump(by_alias=True))
    return {}


# ---------------------------------------------------------------------------
# Result helpers (tolerant of legacy vs new result shapes)


def exported_asset_ships(assets: Path, rel: str, family: str = "desktop") -> bool:
    """Whether an export's assets directory ships the asset `rel` (assets-relative): the file
    itself, or for a texture source the export cooked, its entry in je-cooked/<family>/index.json
    with the cooked file (S14 R9: the source then stays out)."""
    if (assets / rel).is_file():
        return True
    index_path = assets / "je-cooked" / family / "index.json"
    if not index_path.is_file():
        return False
    entry = json.loads(index_path.read_text(encoding="utf-8"))["textures"].get(rel)
    return bool(entry) and (index_path.parent / entry["file"]).is_file()


def find_key(value: Any, *keys: str) -> Any:
    """First value of any of `keys` found walking a JSON value depth-first (None if absent)."""
    if isinstance(value, dict):
        for k in keys:
            if k in value:
                return value[k]
        for v in value.values():
            r = find_key(v, *keys)
            if r is not None:
                return r
    elif isinstance(value, list):
        for v in value:
            r = find_key(v, *keys)
            if r is not None:
                return r
    return None


def find_entities(value: Any) -> list[dict]:
    """Every dict in a JSON value that looks like a GameObject entry (has an id/entity_id and a name)."""
    out: list[dict] = []
    if isinstance(value, dict):
        if ("id" in value or "entity_id" in value) and "name" in value:
            out.append(value)
        for v in value.values():
            out += find_entities(v)
    elif isinstance(value, list):
        for v in value:
            out += find_entities(v)
    return out


def entity_id_of(entry: dict) -> Any:
    return entry.get("entity_id", entry.get("id"))
