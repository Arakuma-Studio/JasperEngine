"""HTTP client (spec 2026-10-31-http section 9, MCP e2e): a played Lua script GETs JSON, POSTs a
table, downloads a file and loads a texture into an Image from a local Python HTTP server
(127.0.0.1, so plain http is allowed); eval_lua reads the results, the screenshot shows the
downloaded image, get_play_state has the http block, configure_game_view {network: "offline"}
makes requests fail with "offline", stopping Play while a slow request runs leaves no error and
no late callback, and set_project_settings round-trips player.allow_insecure_http and
android.internet_access. No internet is needed."""

from __future__ import annotations

import asyncio
import json
import struct
import threading
import time
import zlib
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

from harness import Editor
from htmlui_pixels import decode_png

EDITOR = {"create": True, "name": "Http Test"}
CI = True
TIMEOUT = 600


def solid_png(width: int, height: int, rgb: tuple[int, int, int]) -> bytes:
    raw = b"".join(b"\x00" + bytes(rgb) * width for _ in range(height))

    def chunk(kind: bytes, data: bytes) -> bytes:
        return struct.pack(">I", len(data)) + kind + data + struct.pack(">I", zlib.crc32(kind + data) & 0xFFFFFFFF)

    return (b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", width, height, 8, 2, 0, 0, 0)) +
            chunk(b"IDAT", zlib.compress(raw)) + chunk(b"IEND", b""))


PNG = solid_png(64, 64, (255, 0, 0))
BLOB = bytes(i % 251 for i in range(200_000))


class Handler(BaseHTTPRequestHandler):
    def log_message(self, *args):
        pass

    def send(self, status: int, body: bytes, content_type: str = "text/plain"):
        self.send_response(status)
        self.send_header("Content-Type", content_type)
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def do_GET(self):
        if self.path == "/json":
            self.send(200, b'{"name":"jasper","coins":7,"list":[1,2,3]}', "application/json")
        elif self.path == "/icon.png":
            self.send(200, PNG, "image/png")
        elif self.path == "/blob.bin":
            self.send(200, BLOB, "application/octet-stream")
        elif self.path.startswith("/slow"):
            time.sleep(4)
            try:
                self.send(200, b"late")
            except OSError:
                pass
        else:
            self.send(404, b"no " + self.path.encode())

    def do_POST(self):
        length = int(self.headers.get("Content-Length", "0"))
        body = self.rfile.read(length)
        echo = {"method": "POST", "content_type": self.headers.get("Content-Type", ""), "body": body.decode("utf-8", "replace")}
        self.send(200, json.dumps(echo).encode(), "application/json")


def start_server() -> tuple[ThreadingHTTPServer, str]:
    server = ThreadingHTTPServer(("127.0.0.1", 0), Handler)
    server.daemon_threads = True
    threading.Thread(target=server.serve_forever, daemon=True).start()
    return server, f"http://127.0.0.1:{server.server_address[1]}"


PROBE = """---@class HttpProbe : ScriptInstance
local HttpProbe = { properties = {} }

function HttpProbe:init()
    local base = "%(base)s"
    httpDone = false
    httpResults = {}
    local pending = 4
    local function done()
        pending = pending - 1
        if pending == 0 then httpDone = true end
    end
    local image = Image(self.scene, self.entity)
    image:setSize(300, 300)
    Http.get(base .. "/json", {responseType = "json", owner = self}, function(res)
        httpResults.json = res.ok and res.data.name .. ":" .. res.data.coins .. ":" .. #res.data.list or res.error
        done()
    end)
    Http.post(base .. "/echo", {player = "p1", score = 3}, {owner = self}, function(res)
        local echo = res:json()
        httpResults.post = res.ok and echo.content_type .. "|" .. echo.body or res.error
        done()
    end)
    Http.download(base .. "/blob.bin", "data://downloads/blob.bin", {owner = self, onProgress = function(req, got, total)
        httpResults.progress = got .. "/" .. total
    end}, function(res)
        httpResults.download = res.ok and res.file .. ":" .. res.bytesReceived or res.error
        done()
    end)
    Http.getTexture(base .. "/icon.png", {owner = self}, function(res)
        if res.ok then image:setTexture(res.url, res.textureData) end
        httpResults.texture = res.ok and res.textureData:getWidth() .. "x" .. res.textureData:getHeight() or res.error
        done()
    end)
end

return HttpProbe
"""


async def lua(ed: Editor, code: str, **args):
    return (await ed.call("eval_lua", code=code, **args))["values"]


async def test_project_settings_keys(ed: Editor):
    ed.require_tools("get_project_settings", "set_project_settings")
    got = (await ed.call("get_project_settings", keys=["player.allow_insecure_http", "android.internet_access"]))["settings"]
    ed.check_eq(got, {"player.allow_insecure_http": False, "android.internet_access": "auto"})
    await ed.call("set_project_settings", values={"player.allow_insecure_http": True, "android.internet_access": "require"})
    got = (await ed.call("get_project_settings", keys=["player.allow_insecure_http", "android.internet_access"]))["settings"]
    ed.check_eq(got, {"player.allow_insecure_http": True, "android.internet_access": "require"})
    await ed.check_error("set_project_settings", values={"android.internet_access": "always"}, contains="require")
    await ed.call("save_project")
    project_yaml = (ed.project / "project.yaml").read_text()
    ed.check_in("allowInsecureHttp: true", project_yaml)
    ed.check_in("internetAccess: require", project_yaml)
    await ed.call("set_project_settings", values={"player.allow_insecure_http": False, "android.internet_access": "auto"})
    await ed.call("save_project")
    project_yaml = (ed.project / "project.yaml").read_text()
    ed.check("allowInsecureHttp" not in project_yaml and "internetAccess" not in project_yaml, "defaults are omitted")


async def test_play_requests_offline_and_stop(ed: Editor):
    ed.require_tools("play", "wait", "eval_lua", "configure_game_view", "get_play_state", "create_script")
    server, base = start_server()
    try:
        made = await ed.call("create_scene", name="HttpMain", type="2D", path="scenes/HttpMain")
        scene = made["scene"]["id"]
        ent = await ed.call("create_entity", scene=scene, type="image", name="Icon")
        await ed.call("create_script", kind="lua", name="HttpProbe", content=PROBE % {"base": base}, scene=scene,
                      entity=ent["entity_id"])
        await ed.call("save_scene", scene=scene)
        cursor = (await ed.call("read_console", limit=1))["next_cursor"]

        view = await ed.call("configure_game_view")
        ed.check_eq(view["network"], "online")

        await ed.call("play", action="start", scene=scene)
        try:
            waited = await ed.call("wait", **{"until": "httpDone", "timeout_seconds": 30})
            ed.check_eq(waited.get("reason"), "condition", f"{waited}")
            results = (await lua(ed, "return Json.encode(httpResults)"))[0]
            results = json.loads(results)
            ed.check_eq(results["json"], "jasper:7:3")
            ed.check_eq(results["post"], 'application/json|{"player":"p1","score":3}')
            ed.check_eq(results["download"], "data://downloads/blob.bin:200000")
            ed.check_eq(results["progress"], "200000/200000")
            ed.check_eq(results["texture"], "64x64")
            downloaded = ed.project / ".jasperengine" / "user" / "rundata" / "downloads" / "blob.bin"
            ed.check_eq(downloaded.read_bytes(), BLOB, "the downloaded file")
            ed.check(not downloaded.with_name("blob.bin.part").exists(), "no .part left")

            state = await ed.call("get_play_state")
            http = state.get("http")
            ed.check(isinstance(http, dict), f"get_play_state has http: {state}")
            ed.check_eq((http["finished"], http["failed"], http["active"], http["offline"]), (4, 0, 0, False))

            # the downloaded texture is on screen
            await ed.call("wait", frames=5)
            width, height, channels, rows = decode_png(await ed.screenshot("game"))
            red = sum(1 for y in range(0, height, 4) for x in range(0, width, 4)
                      if rows[y][x * channels] > 200 and rows[y][x * channels + 1] < 60 and rows[y][x * channels + 2] < 60)
            ed.check(red > 50, f"the red icon is drawn ({red} red samples)")

            # offline simulation, while playing
            off = await ed.call("configure_game_view", network="offline")
            ed.check_eq(off["network"], "offline")
            ed.check_in("network", off["changed"])
            await lua(ed, "offlineCode = nil; localOk = nil\n"
                          "Http.get('https://example.invalid/x', function(r) offlineCode = r.errorCode end)\n"
                          f"Http.get('{base}/json', function(r) localOk = r.ok end)")
            await ed.call("wait", **{"until": "offlineCode ~= nil and localOk ~= nil", "timeout_seconds": 15})
            ed.check_eq(await lua(ed, "return offlineCode, localOk"), ["offline", True])
            http = (await ed.call("get_play_state"))["http"]
            ed.check_eq(http["offline"], True)
            ed.check_eq(http["last_failures"][-1]["error_code"], "offline")
            ed.check_eq(http["last_failures"][-1]["url"], "https://example.invalid/x")
            await ed.call("configure_game_view", network="online")

            # plain http to another host is refused (player.allow_insecure_http is off)
            await lua(ed, "insecureCode = nil; Http.get('http://example.invalid/', function(r) insecureCode = r.errorCode end)")
            await ed.call("wait", **{"until": "insecureCode ~= nil", "timeout_seconds": 15})
            ed.check_eq(await lua(ed, "return insecureCode"), ["insecure_http"])

            # a slow request when Play stops: its callback never runs
            await lua(ed, f"Http.get('{base}/slow', function(r) error('late http callback after stop') end)")
            await ed.call("wait", frames=3)
            ed.check_eq((await ed.call("get_play_state"))["http"]["active"], 1)
        finally:
            await ed.call("play", action="stop")
        await asyncio.sleep(5)
        errors = (await ed.call("read_console", since=cursor, types=["error"]))["entries"]
        ed.check(not errors, f"no error, no late callback: {errors}")

        # the next Play starts clean
        await ed.call("play", action="start", scene=scene)
        try:
            await ed.call("wait", **{"until": "httpDone", "timeout_seconds": 30})
            http = (await ed.call("get_play_state"))["http"]
            ed.check_eq((http["finished"], http["failed"], http["cancelled"]), (4, 0, 0))
        finally:
            await ed.call("play", action="stop")
        errors = (await ed.call("read_console", since=cursor, types=["error"]))["entries"]
        ed.check(not errors, f"unexpected errors: {errors}")
    finally:
        server.shutdown()
        server.server_close()


POLICY = """---@class PolicyProbe : ScriptInstance
local PolicyProbe = { properties = {} }

function PolicyProbe:init()
    policyCode = nil
    -- sent from init: the project's plain-HTTP policy is already the one of this Play
    Http.get("http://example.invalid/", {timeout = 5, owner = self}, function(res) policyCode = res.errorCode end)
end

return PolicyProbe
"""


async def test_policy_applies_before_init(ed: Editor):
    ed.require_tools("play", "wait", "eval_lua", "create_script", "set_project_settings")
    made = await ed.call("create_scene", name="PolicyMain", type="2D", path="scenes/PolicyMain")
    scene = made["scene"]["id"]
    ent = await ed.call("create_entity", scene=scene, type="empty", name="Probe")
    await ed.call("create_script", kind="lua", name="PolicyProbe", content=POLICY, scene=scene, entity=ent["entity_id"])
    await ed.call("save_scene", scene=scene)

    async def play_code() -> str:
        await ed.call("play", action="start", scene=scene)
        try:
            await ed.call("wait", **{"until": "policyCode ~= nil", "timeout_seconds": 20})
            return (await lua(ed, "return policyCode"))[0]
        finally:
            await ed.call("play", action="stop")

    await ed.call("set_project_settings", values={"player.allow_insecure_http": True})
    try:
        # allowed: the request goes out (example.invalid does not resolve), it is not refused
        code = await play_code()
        ed.check(code not in ("insecure_http", ""), f"allowed on the first Play: {code}")
        await ed.call("set_project_settings", values={"player.allow_insecure_http": False})
        ed.check_eq(await play_code(), "insecure_http", "refused after turning it off")
        await ed.call("set_project_settings", values={"player.allow_insecure_http": True})
        code = await play_code()
        ed.check(code not in ("insecure_http", ""), f"allowed again: {code}")
    finally:
        await ed.call("set_project_settings", values={"player.allow_insecure_http": False})
