#!/usr/bin/env python3
"""Run a web (Emscripten) build in headless Chrome and fail on what it shows.

    python3 tools/web/browser_gate.py --root build/web --page jasperengine-project.html \\
        --until-ready --baseline tools/web/gates/sample-project.json \\
        --screenshot build/test-results/web/sample-project.png

Serves the build directory like tools/web/serve.py (COOP/COEP, no-store, wasm MIME types), starts
Chrome headless on SwiftShader WebGL2 with a throwaway profile, opens the page with
?jasperengine_ready_signal=1 (the web platform then logs the engine's "Engine ready:" line, as
JASPERENGINE_READY_SIGNAL does on desktop) and drives it over the DevTools protocol. It prints the
console transcript, every line stamped with its arrival time (seconds since the first event), the
uncaught exceptions, crossOriginIsolated and the canvas's centre pixel, then the verdict.

Exit 1 on: an uncaught exception; a console message of level error; a console line matching
ERROR_MARKERS (WebGL errors, the engine's (ERROR) lines, Lua errors, missing shaders, Emscripten
aborts, wasm traps); no ready line before --timeout (with --until-ready); a missing --expect line;
a centre pixel outside the baseline's rgb +- tolerance; a --screenshot the page did not give
(Page.captureScreenshot timed out or returned nothing), unless --allow-no-capture REASON says why it
cannot be (a page without a WebGL canvas); crossOriginIsolated differing from the
baseline's isolated; a line of the baseline's "console" list ({"text", "level"}: log, info,
warning, error) missing, at another console level, or (with "ordered": true) out of order. An
error the "console" list names is expected and fails nothing. A baseline may also give "query" (items added to the
page URL's query), "seconds" (the run length when --seconds is not given), "capture_timeout" (seconds the screenshot may take), "init_script" (JavaScript run before the page's own scripts), "eval" with "eval_expect" (substrings
its value must contain), and "allow": substrings of console lines and uncaught exceptions that are expected (a
deliberate crash) and fail nothing. Exit 2 when the gate itself cannot run (no Chrome, no page target).

--until-line TEXT ends the run at the first console line containing TEXT (a game that prints when it
is done; --seconds is then the cap).

A baseline's "click": n clicks the canvas centre n times after the run (CDP Input.dispatchMouseEvent:
trusted events, so a page can lock the pointer), each followed by "after_click_seconds" (default 2)
of running; the console lines it causes are checked like the others.

A baseline's "keys": key presses after the clicks, each a {"code", "key", "key_code", "location"} object
(every field optional: an event without "code" exercises the legacy keyCode path) sent as a CDP
Input.dispatchKeyEvent rawKeyDown and keyUp pair (trusted: a key press is a user gesture), each followed
by "after_key_seconds" (default 0.5). "eval_before_input" with "eval_before_input_expect" is evaluated
after the run, before the clicks and keys (a state a gesture changes: the audio context). "chrome_flags"
adds Chrome flags (["--autoplay-policy=user-gesture-required"]: audio waits for a gesture as in a
desktop browser).

The transcript is printed before anything that needs the page's main thread (evaluation, the
screenshot), so a wedged page still yields its console. The only capture path is
Page.captureScreenshot: reading a WebGL canvas back from the page returns cleared pixels.

Chrome: JASPER_CHROME, else chrome-headless-shell (the web build image's, docker/web.Dockerfile),
else google-chrome, chromium or chrome on PATH, else the usual install paths. No third-party
packages: the DevTools client below speaks enough of RFC 6455 to send commands and collect events.

Adapted from OpenEngine's Tools/Web/browser_gate.py (MIT):
Copyright (c) 2026 Gamecrafters Guild Limited and contributors
"""

from __future__ import annotations

import argparse
import base64
import json
import os
import random
import re
import shutil
import socket
import struct
import subprocess
import sys
import tempfile
import threading
import time
import urllib.request
import zlib
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from serve import make_server  # noqa: E402

READY_LINE = "Engine ready:"
READY_QUERY = "jasperengine_ready_signal=1"

# console lines that fail the gate whatever their level: WebGL (Chrome's own "WebGL: INVALID_..."
# and ANGLE's GL_INVALID_*), the engine's error lines (System::platformLog "(ERROR): ", the
# RmlUi/script "[ERROR]" ones), Lua errors, a shader the build does not have, an Emscripten
# abort() and a wasm trap (RuntimeError: unreachable, memory access out of bounds, ...)
ERROR_MARKERS = re.compile(r"WebGL: INVALID_|GL_INVALID|\(ERROR\)|\[ERROR\]|Lua Error|[Mm]issing (precompiled )?shader|Aborted\(|RuntimeError")


def find_chrome() -> str:
    override = os.environ.get("JASPER_CHROME")
    if override:
        return override
    for name in ("chrome-headless-shell", "google-chrome", "chromium", "chromium-browser", "chrome"):
        found = shutil.which(name)
        if found:
            return found
    candidates = [Path("/opt/chrome-headless-shell/chrome-headless-shell")]
    if sys.platform == "darwin":
        candidates.append(Path("/Applications/Google Chrome.app/Contents/MacOS/Google Chrome"))
    elif sys.platform == "win32":
        for base in (os.environ.get("PROGRAMFILES", r"C:\Program Files"), os.environ.get("PROGRAMFILES(X86)", ""),
                     os.environ.get("LOCALAPPDATA", "")):
            if base:
                candidates.append(Path(base) / "Google" / "Chrome" / "Application" / "chrome.exe")
    for candidate in candidates:
        if candidate.is_file():
            return str(candidate)
    raise SystemExit("browser_gate: no Chrome here: set JASPER_CHROME to a Chrome or chrome-headless-shell binary")


def free_port() -> int:
    with socket.socket() as probe:
        probe.bind(("127.0.0.1", 0))
        return probe.getsockname()[1]


class Cdp:
    """Minimal DevTools client: one WebSocket, text frames, client-masked."""

    def __init__(self, url: str):
        _, _, rest = url.partition("://")
        hostport, _, path = rest.partition("/")
        host, _, port = hostport.partition(":")
        self.socket = socket.create_connection((host, int(port)))
        key = base64.b64encode(bytes(random.getrandbits(8) for _ in range(16))).decode()
        self.socket.sendall(
            (f"GET /{path} HTTP/1.1\r\nHost: {hostport}\r\nUpgrade: websocket\r\n"
             f"Connection: Upgrade\r\nSec-WebSocket-Key: {key}\r\n"
             "Sec-WebSocket-Version: 13\r\n\r\n").encode())
        self.buffer = b""
        while b"\r\n\r\n" not in self.buffer:
            chunk = self.socket.recv(4096)
            if not chunk:
                raise RuntimeError("DevTools socket closed during the handshake")
            self.buffer += chunk
        self.buffer = self.buffer.split(b"\r\n\r\n", 1)[1]
        self.next_id = 1
        self.events: list[dict] = []

    def _recv(self, count: int) -> bytes:
        while len(self.buffer) < count:
            chunk = self.socket.recv(65536)
            if not chunk:
                raise RuntimeError("DevTools socket closed")
            self.buffer += chunk
        out, self.buffer = self.buffer[:count], self.buffer[count:]
        return out

    def _read_frame(self) -> dict:
        while True:
            header = self._recv(2)
            opcode = header[0] & 0x0F
            length = header[1] & 0x7F
            if length == 126:
                length = struct.unpack(">H", self._recv(2))[0]
            elif length == 127:
                length = struct.unpack(">Q", self._recv(8))[0]
            payload = self._recv(length)
            if opcode == 1:
                return json.loads(payload)
            if opcode == 8:
                raise RuntimeError("DevTools socket closed by the browser")

    def send(self, method: str, timeout: float | None = 30.0, **params) -> dict:
        request_id = self.next_id
        self.next_id += 1
        data = json.dumps({"id": request_id, "method": method, "params": params}).encode()
        header = bytearray([0x81])
        if len(data) < 126:
            header.append(0x80 | len(data))
        elif len(data) < (1 << 16):
            header.append(0x80 | 126)
            header += struct.pack(">H", len(data))
        else:
            header.append(0x80 | 127)
            header += struct.pack(">Q", len(data))
        mask = bytes(random.getrandbits(8) for _ in range(4))
        header += mask
        self.socket.sendall(bytes(header) + bytes(b ^ mask[i % 4] for i, b in enumerate(data)))
        self.socket.settimeout(timeout)
        try:
            while True:
                frame = self._read_frame()
                if frame.get("id") == request_id:
                    return frame
                # a late reply to an earlier request that timed out is dropped
                if "id" not in frame:
                    self.events.append(frame)
        finally:
            self.socket.settimeout(None)

    def pump(self, seconds: float, until=None) -> bool:
        """Collects events for `seconds`, or until until(event) holds (then True)."""
        deadline = time.monotonic() + seconds
        self.socket.settimeout(0.25)
        try:
            while time.monotonic() < deadline:
                try:
                    frame = self._read_frame()
                except (socket.timeout, TimeoutError):
                    continue
                if "id" not in frame:
                    self.events.append(frame)
                    if until and until(frame):
                        return True
        finally:
            self.socket.settimeout(None)
        return False


def console_text(event: dict) -> str:
    params = event.get("params", {})
    method = event.get("method")
    if method == "Runtime.consoleAPICalled":
        return " ".join(str(a.get("value", a.get("description", ""))) for a in params.get("args", []))
    if method == "Log.entryAdded":
        entry = params.get("entry", {})
        text = entry.get("text", "")
        return f"{text} ({entry['url']})" if entry.get("url") and entry.get("level") == "error" else text
    if method == "Runtime.exceptionThrown":
        details = params.get("exceptionDetails", {})
        frames = details.get("stackTrace", {}).get("callFrames", [])
        thrown = details.get("exception", {})
        what = thrown.get("description") or thrown.get("className") or ""
        if not what and "value" in thrown:
            what = repr(thrown["value"])
        return f"EXCEPTION {details.get('text', '')} :: {what}" + "".join(
            f"\n    {f.get('functionName') or '<anonymous>'} ({f.get('url', '')}:{f.get('lineNumber')})" for f in frames[:20])
    return ""


def console_level(event: dict) -> str:
    params = event.get("params", {})
    if event.get("method") == "Runtime.consoleAPICalled":
        return "error" if params.get("type") in ("error", "assert") else params.get("type", "log")
    if event.get("method") == "Log.entryAdded":
        return params.get("entry", {}).get("level", "info")
    if event.get("method") == "Runtime.exceptionThrown":
        return "exception"
    return ""


def console_time(event: dict) -> float | None:
    """The event's own timestamp in milliseconds since the epoch."""
    params = event.get("params", {})
    if event.get("method") == "Log.entryAdded":
        return params.get("entry", {}).get("timestamp")
    return params.get("timestamp")


def stamped(text: str, seconds: float | None) -> str:
    stamp = f"[{seconds:8.3f}] " if seconds is not None else "[   --.---] "
    return stamp + text


def png_pixel(png: bytes, x: int, y: int) -> tuple[int, int, int] | None:
    """The RGB of pixel (x, y) of an 8-bit RGB/RGBA PNG (standard library only)."""
    if png[:8] != b"\x89PNG\r\n\x1a\n":
        return None
    pos, width, height, depth, colour = 8, 0, 0, 0, 0
    idat = b""
    while pos < len(png):
        length = struct.unpack(">I", png[pos:pos + 4])[0]
        kind = png[pos + 4:pos + 8]
        body = png[pos + 8:pos + 8 + length]
        if kind == b"IHDR":
            width, height, depth, colour = struct.unpack(">IIBB", body[:10])
        elif kind == b"IDAT":
            idat += body
        pos += 12 + length
    if depth != 8 or colour not in (2, 6) or not (0 <= x < width and 0 <= y < height):
        return None
    channels = 3 if colour == 2 else 4
    raw = zlib.decompress(idat)
    stride = width * channels
    previous = bytearray(stride)
    for row_index in range(y + 1):
        filter_type = raw[row_index * (stride + 1)]
        row = bytearray(raw[row_index * (stride + 1) + 1:(row_index + 1) * (stride + 1)])
        for i in range(stride):
            left = row[i - channels] if i >= channels else 0
            up = previous[i]
            up_left = previous[i - channels] if i >= channels else 0
            if filter_type == 1:
                row[i] = (row[i] + left) & 0xFF
            elif filter_type == 2:
                row[i] = (row[i] + up) & 0xFF
            elif filter_type == 3:
                row[i] = (row[i] + (left + up) // 2) & 0xFF
            elif filter_type == 4:
                p = left + up - up_left
                pa, pb, pc = abs(p - left), abs(p - up), abs(p - up_left)
                row[i] = (row[i] + (left if pa <= pb and pa <= pc else up if pb <= pc else up_left)) & 0xFF
        previous = row
    offset = x * channels
    return (previous[offset], previous[offset + 1], previous[offset + 2])


def png_size(png: bytes) -> tuple[int, int]:
    if png[:8] != b"\x89PNG\r\n\x1a\n":
        return (0, 0)
    width, height = struct.unpack(">II", png[16:24])
    return (width, height)


def evaluate(cdp: Cdp, expression: str, timeout: float = 30.0):
    reply = cdp.send("Runtime.evaluate", timeout=timeout, expression=expression, returnByValue=True, awaitPromise=True)
    return reply.get("result", {}).get("result", {}).get("value")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--root", type=Path, required=True, help="the web build directory (served)")
    parser.add_argument("--page", default="jasperengine-project.html", help="page under --root")
    parser.add_argument("--until-ready", action="store_true", help=f"wait for the engine's '{READY_LINE}' line")
    parser.add_argument("--timeout", type=float, default=120.0, help="seconds to wait for the ready line (120)")
    parser.add_argument("--seconds", type=float,
                        help="seconds to run: after the ready line with --until-ready (default 2), else in all (default 10)")
    parser.add_argument("--screenshot", type=Path, help="write the captured page here (PNG); no capture fails the gate")
    parser.add_argument("--capture-timeout", type=float,
                        help="seconds the page may take to give its screenshot (default: the baseline's capture_timeout, "
                             "else 30; a heavy scene on software WebGL needs more while the GPU process catches up)")
    parser.add_argument("--allow-no-capture", metavar="REASON",
                        help="a run whose page cannot be captured (no WebGL canvas, ...): a missing --screenshot does not "
                             "fail it; REASON is printed with the result")
    parser.add_argument("--transcript", type=Path, help="write the console transcript here too")
    parser.add_argument("--baseline", type=Path, help="JSON: rgb, tolerance (the canvas centre pixel), isolated")
    parser.add_argument("--expect", action="append", default=[], help="fail unless a console line contains this (repeatable)")
    parser.add_argument("--until-line", help="end the run as soon as a console line contains this (--seconds is then the cap)")
    parser.add_argument("--query", action="append", default=[],
                        help="extra name=value added to the page URL's query (repeatable), e.g. jasperengine_watch_probe=1")
    parser.add_argument("--eval", dest="evaluate", help="JS expression evaluated in the page after the run; its value is printed")
    parser.add_argument("--no-isolation", action="store_true",
                        help="serve without COOP/COEP (a threaded build cannot start then: the red control)")
    parser.add_argument("--chrome-flag", action="append", default=[], help="extra Chrome flag (repeatable)")
    args = parser.parse_args()

    baseline = json.loads(args.baseline.read_text()) if args.baseline else {}
    # a baseline's "seconds" is its own default run length (a scene that needs a while: S19's bakes)
    default_seconds = (2.0 if args.until_ready else 10.0)
    seconds = args.seconds if args.seconds is not None else float(baseline.get("seconds", default_seconds))
    if args.capture_timeout is None:
        args.capture_timeout = float(baseline.get("capture_timeout", 30.0))
    if not (args.root / args.page).is_file():
        print(f"browser_gate: no {args.page} in {args.root}", file=sys.stderr)
        return 2

    server = make_server(args.root, 0, cross_origin_isolation=not args.no_isolation, quiet=True)
    threading.Thread(target=server.serve_forever, daemon=True).start()
    page_url = f"http://127.0.0.1:{server.server_port}/{args.page}?" + "&".join([READY_QUERY] + baseline.get("query", []) + args.query)

    chrome_path = find_chrome()
    cdp_port = free_port()
    profile = tempfile.mkdtemp(prefix="jasper-browser-gate-")
    headless = "--headless" if "headless-shell" in Path(chrome_path).name else "--headless=new"
    chrome = subprocess.Popen(
        [chrome_path, headless, f"--remote-debugging-port={cdp_port}", f"--user-data-dir={profile}",
         "--use-angle=swiftshader", "--enable-unsafe-swiftshader", "--ignore-gpu-blocklist",
         # a container has neither the user namespaces of Chrome's sandbox nor a large /dev/shm
         "--no-sandbox", "--disable-dev-shm-usage",
         "--window-size=1280,720", "--no-first-run", "--no-default-browser-check", "--mute-audio",
         *baseline.get("chrome_flags", []), *args.chrome_flag, "about:blank"],
        stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)

    try:
        target = None
        for _ in range(120):
            try:
                with urllib.request.urlopen(f"http://127.0.0.1:{cdp_port}/json/list", timeout=2) as response:
                    target = next((t for t in json.load(response) if t.get("type") == "page"), None)
                if target:
                    break
            except OSError:
                pass
            if chrome.poll() is not None:
                break
            time.sleep(0.5)
        if target is None:
            print(f"browser_gate: {chrome_path} exposed no page target (exit {chrome.poll()})", file=sys.stderr)
            return 2

        cdp = Cdp(target["webSocketDebuggerUrl"])
        cdp.send("Runtime.enable")
        cdp.send("Runtime.setMaxCallStackSizeToCapture", size=50)
        cdp.send("Log.enable")
        cdp.send("Page.enable")
        if baseline.get("init_script"):
            cdp.send("Page.addScriptToEvaluateOnNewDocument", source=baseline["init_script"])
        print(f"browser_gate: {Path(chrome_path).name} opens {page_url}")
        cdp.send("Page.navigate", url=page_url)

        ready = False
        if args.until_ready:
            ready = cdp.pump(args.timeout, until=lambda event: READY_LINE in console_text(event))
        cdp.pump(seconds, until=(lambda event: args.until_line in console_text(event)) if args.until_line else None)

        before_input = None
        if baseline.get("eval_before_input"):
            before_input = evaluate(cdp, baseline["eval_before_input"])
            print(f"browser_gate: eval before input {baseline['eval_before_input']} = {json.dumps(before_input)[:20000]}")

        # user gestures the page waits for (pointer lock, audio): trusted clicks at the canvas centre
        for _ in range(int(baseline.get("click", 0))):
            rect = evaluate(cdp, "(() => { const c = document.querySelector('canvas'); if (!c) return null;"
                                 " const r = c.getBoundingClientRect(); return [r.left + r.width / 2, r.top + r.height / 2]; })()")
            x, y = (rect[0], rect[1]) if rect else (640, 360)
            for kind in ("mouseMoved", "mousePressed", "mouseReleased"):
                cdp.send("Input.dispatchMouseEvent", type=kind, x=x, y=y, button="left" if kind != "mouseMoved" else "none",
                         buttons=1 if kind == "mousePressed" else 0, clickCount=1 if kind != "mouseMoved" else 0)
            cdp.pump(float(baseline.get("after_click_seconds", 2.0)))

        # key presses (trusted, so a gesture: web audio starts on one)
        for key in baseline.get("keys", []):
            event = {"key": key.get("key", ""), "windowsVirtualKeyCode": int(key.get("key_code", 0)),
                     "nativeVirtualKeyCode": int(key.get("key_code", 0)), "location": int(key.get("location", 0))}
            if key.get("code"):
                event["code"] = key["code"]
            for kind in ("rawKeyDown", "keyUp"):
                cdp.send("Input.dispatchKeyEvent", type=kind, **event)
            cdp.pump(float(baseline.get("after_key_seconds", 0.5)))

        # the transcript first: everything after this needs the page's main thread
        records = [(console_time(e), console_level(e), console_text(e)) for e in cdp.events]
        records = [r for r in records if r[2]]
        origin = next((t for t, _, _ in records if t is not None), None)
        transcript = []
        for timestamp, level, text in records:
            elapsed = (timestamp - origin) / 1000.0 if timestamp is not None and origin is not None else None
            transcript.append(stamped(f"{level}: {text}" if level not in ("log", "info") else text, elapsed))
        print("\n".join(transcript))
        sys.stdout.flush()
        if args.transcript:
            args.transcript.parent.mkdir(parents=True, exist_ok=True)
            args.transcript.write_text("\n".join(transcript) + "\n")

        if args.evaluate:
            print(f"browser_gate: eval {args.evaluate} = {json.dumps(evaluate(cdp, args.evaluate))[:20000]}")
        baseline_eval = None
        if baseline.get("eval"):
            baseline_eval = evaluate(cdp, baseline["eval"])
            print(f"browser_gate: eval {baseline['eval']} = {json.dumps(baseline_eval)[:20000]}")
        isolated = evaluate(cdp, "self.crossOriginIsolated === true")
        canvas = evaluate(cdp, "(() => { const c = document.querySelector('canvas'); if (!c) return null;"
                               " const r = c.getBoundingClientRect(); return [r.left + r.width / 2, r.top + r.height / 2,"
                               " r.width, r.height]; })()")

        png = b""
        capture_problem = ""
        try:
            reply = cdp.send("Page.captureScreenshot", timeout=args.capture_timeout, format="png")
            png = base64.b64decode(reply.get("result", {}).get("data", "") or "")
            if not png:
                capture_problem = "Page.captureScreenshot returned no image: " + json.dumps(reply.get("error", reply))[:500]
        except (socket.timeout, TimeoutError):
            capture_problem = (f"Page.captureScreenshot did not answer within {args.capture_timeout:g} s: the page's main thread "
                               "or the browser's GPU process is blocked")
        if capture_problem:
            print(f"browser_gate: {capture_problem}")
        if args.screenshot and png:
            args.screenshot.parent.mkdir(parents=True, exist_ok=True)
            args.screenshot.write_bytes(png)
        width, height = png_size(png)
        if canvas and canvas[2] > 0 and canvas[3] > 0:
            centre_x, centre_y = int(canvas[0]), int(canvas[1])
        else:
            centre_x, centre_y = width // 2, height // 2
        centre = png_pixel(png, centre_x, centre_y) if png else None

        # the baseline's required console lines, at their level (and in order)
        required = baseline.get("console", [])
        expected_errors = [item["text"] for item in required if item.get("level") == "error"]

        allowed = baseline.get("allow", [])

        def expected(text: str) -> bool:
            return any(e in text for e in expected_errors) or any(a in text for a in allowed)

        console_failures = []
        position = 0
        for item in required:
            start = position if baseline.get("ordered") else 0
            found = next((i for i in range(start, len(records)) if item["text"] in records[i][2]), None)
            if found is None:
                earlier = any(item["text"] in r[2] for r in records)
                console_failures.append(f"'{item['text']}' " + ("is out of order" if earlier else "is missing"))
                continue
            if records[found][1] != item.get("level", records[found][1]):
                console_failures.append(f"'{item['text']}' is a console {records[found][1]}, not {item['level']}")
            position = found + 1

        exceptions = [text for _, level, text in records if level == "exception" and not any(a in text for a in allowed)]
        errors = [text for _, level, text in records if level == "error" and not expected(text)]
        marked = [text for _, level, text in records
                  if level not in ("exception", "error") and ERROR_MARKERS.search(text) and not expected(text)]
        lines = [text for _, _, text in records]
        missing = [text for text in args.expect + baseline.get("expect", []) if not any(text in line for line in lines)]

        failures = []
        if exceptions:
            failures.append(f"{len(exceptions)} uncaught exceptions")
        if errors:
            failures.append(f"{len(errors)} console errors")
        if marked:
            failures.append(f"{len(marked)} console lines with error markers: " + "; ".join(m.splitlines()[0] for m in marked[:5]))
        if args.until_ready and not ready:
            failures.append(f"no '{READY_LINE}' line within {args.timeout:g} s")
        for text in missing:
            failures.append(f"no console line contains '{text}'")
        if baseline.get("eval_before_input"):
            value = json.dumps(before_input) if not isinstance(before_input, str) else before_input
            for text in baseline.get("eval_before_input_expect", []):
                if text not in (value or ""):
                    failures.append(f"eval before input {baseline['eval_before_input']} does not contain '{text}' (got {value})")
        if baseline.get("eval"):
            value = json.dumps(baseline_eval) if not isinstance(baseline_eval, str) else baseline_eval
            for text in baseline.get("eval_expect", []):
                if text not in (value or ""):
                    failures.append(f"eval {baseline['eval']} does not contain '{text}' (got {value})")
        failures += [f"console: {failure}" for failure in console_failures]
        if "rgb" in baseline:
            tolerance = int(baseline.get("tolerance", 0))
            if centre is None:
                failures.append("no centre pixel (no screenshot)")
            elif any(abs(c - b) > tolerance for c, b in zip(centre, baseline["rgb"])):
                failures.append(f"centre pixel {centre} is outside the baseline {tuple(baseline['rgb'])} +- {tolerance}")
        # a screenshot asked for and not taken is a failure, unless the run says why it cannot be
        if args.screenshot and not png:
            if args.allow_no_capture:
                print(f"browser_gate: no screenshot, allowed: {args.allow_no_capture}")
            else:
                failures.append(f"no screenshot ({capture_problem or 'the capture is empty'}); --allow-no-capture REASON "
                                "for a page that cannot be captured")
        if "isolated" in baseline and bool(isolated) != bool(baseline["isolated"]):
            failures.append(f"crossOriginIsolated is {bool(isolated)}, the baseline says {bool(baseline['isolated'])}")

        print(f"\nbrowser_gate: {len(lines)} console lines, {len(exceptions)} exceptions, {len(errors)} errors, "
              f"ready {'yes' if ready else 'no' if args.until_ready else 'not awaited'}, crossOriginIsolated {bool(isolated)}, "
              f"centre pixel {centre} at ({centre_x}, {centre_y}) of {width}x{height}")
        if args.screenshot and png:
            print(f"browser_gate: screenshot {args.screenshot}")
        for failure in failures:
            print(f"browser_gate: FAIL: {failure}")
        if not failures:
            print("browser_gate: PASS")
        return 1 if failures else 0
    finally:
        chrome.terminate()
        try:
            chrome.wait(timeout=15)
        except subprocess.TimeoutExpired:
            chrome.kill()
        server.shutdown()
        shutil.rmtree(profile, ignore_errors=True)


if __name__ == "__main__":
    sys.exit(main())
