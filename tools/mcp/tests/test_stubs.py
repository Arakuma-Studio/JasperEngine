"""Engine API on disk for code tools: the Lua API stubs (.jasperengine/engine-api/lua) with the
project's .luarc.json for lua-language-server, and the compile database
(.jasperengine/clangd/compile_commands.json) with the project's .clangd for clangd.

The language server checks run lua-language-server and clangd for real and skip
when they are not installed:
  - zero diagnostics (Warning and above) on the authored known-good Lua scripts, and the
    deliberate API mistakes of a bad script reported;
  - the editor's C++ script templates parse with zero errors, a deliberate error is reported.
"""

import asyncio
import json
import re
import shutil
import subprocess
import tempfile
import time
from pathlib import Path

from harness import Editor, skip

EDITOR = {"create": True, "name": "CodeTools"}
TIMEOUT = 900
LUA_DIR = ".jasperengine/engine-api/lua"
CLANGD_DB = ".jasperengine/clangd/compile_commands.json"
# lua-language-server plugin (runtime.plugin) that has assignments to the engine classes' static
# fields through the global table (`Engine.timeScale = v`) checked against their types
LUALS_PLUGIN = ".jasperengine/engine-api/luals-plugin.lua.txt"

BAD_LUA = """---@class Bad : ScriptInstance
local Bad = { properties = {} }

function Bad:init()
    RegisterEngineEvent(self, "onUpdat")
    local v = Vector3(1, 2, 3)
    v:notAMethod()
    local d = v:dotProduct("x")
    UndefinedGlobalThing()
    self.scene:findEntityy("Hub")
    Input.isKeyPressed("space")
    print(d)
    local sprite = Sprite(self.scene, self.entity)
    sprite.width = "x"
    local go = GameObject(self.scene, self.entity)
    go.position = 5
    local engine = Engine
    engine.timeScale = "fast"
    Engine.timeScale = "fast"
    Engine.useDegrees = 1
    Input.KEY_SPACE = {}
    AudioSystem.globalVolume = true
    AudioMixer.setFloat("MusicVolume", "loud")
    Vector3.UNIT_X = 5
    SceneManager.currentSceneName = 3
    Engine.scene = "main"
    Engine["fixedDeltatime"] = false
    Http.maxConcurrentRequests = "many"
end

return Bad
"""
# line -> diagnostic code lua-language-server must report there
BAD_LUA_EXPECTED = {
    5: "param-type-mismatch",   # not an Engine event name
    7: "undefined-field",       # no such method
    8: "param-type-mismatch",   # string where a Vector3 goes
    9: "undefined-global",
    10: "undefined-field",      # no such Scene method
    11: "param-type-mismatch",  # string where a key code goes
    14: "assign-type-mismatch",  # instance property: string to integer
    16: "assign-type-mismatch",  # instance property: number to Vector3
    18: "assign-type-mismatch",  # static property through a local
    # static properties through the global class table (the stubs' LuaLS plugin, see
    # docs/mcp/guide/editor-integration.md)
    19: "assign-type-mismatch",  # number <- string
    20: "assign-type-mismatch",  # boolean <- integer
    21: "assign-type-mismatch",  # integer constant <- table
    22: "assign-type-mismatch",  # number <- boolean
    23: "param-type-mismatch",   # AudioMixer.setFloat: a string where the value (number) goes
    24: "assign-type-mismatch",  # Vector3 constant <- integer
    25: "assign-type-mismatch",  # string <- integer
    26: "assign-type-mismatch",  # Scene <- string
    27: "assign-type-mismatch",  # number <- boolean, indexed with a string
    28: "assign-type-mismatch",  # integer <- string (Http)
}

# A script without engine fields assigned: the plugin leaves the checks driven by the doc
# comments as they are (it must not walk the syntax tree in a way that hides them)
BAD_DOCS_LUA = """---@class (exact) BadPoint
---@field x number
local badPoint = { x = 1, y = 2 }
print(badPoint)
"""
BAD_DOCS_LUA_EXPECTED = {3: "inject-field"}  # not a field of the exact class

# Typical script code that must check clean: an annotated script module, events, physics
# events, operators, static members, the hand-written lua_CFunction signatures
GOOD_LUA = """---@class Good : ScriptInstance
---@field speed number
---@field target Sprite
local Good = {
    properties = {
        { name = "speed", displayName = "Speed", type = "float", default = 5.0 },
        { name = "target", type = "Sprite" },
    }
}

function Good:init()
    local initial <const> = 0  -- Lua 5.4 syntax: the workspace must use the engine's language version.
    RegisterEngineEvent(self, "onUpdate")
    RegisterEngineEvent(self, "onKeyDown")
    self.go = GameObject(self.scene, self.entity)
    self.counter = initial
    RegisterEvent(self, self.scene:getPhysicsSystem().beginContact2D, "onContact")
    local button = Button(self.scene, self.entity)
    button:getButtonComponent().onPress = function() Log.print("pressed") end
    Engine.timeScale = 0.5
    Engine.fixedDeltatime = 1 / 60
    Engine.useDegrees = false
    Engine.mouseCursor = CursorType.CROSSHAIR
    Engine.scene = Engine.getScene()
    AudioSystem.globalVolume = self.speed * 0.1
    SceneManager.loadingDelay = 0.5
    Engine.timeScale, Engine.fixedDeltatime = 1, 0
    Engine["timeScale"] = Engine.timeScale * 0.5
    Engine.onViewLoaded = function() Log.print("view") end
    Engine.gameDebugFlag = true  -- a field of the game's own on an engine table
    Log.print(tostring(Engine.gameDebugFlag))
    GameState = { level = 1 }  -- the game's own global table
    GameState.level = "boss"
    Log.print("frames " .. tostring(Engine.frameCount) .. " " .. tostring(Engine.gameTime))
    -- coroutines, timers and Time (the wait helpers are async: fine inside the coroutine's function)
    local co = self:startCoroutine(function()
        local elapsed = Coroutine.wait(0.5) + Coroutine.waitRealtime(0.1)
        Coroutine.nextFrame()
        Coroutine.waitFrames(2)
        local ok = Coroutine.waitUntil(function() return self.counter > 3 end, 2.0, true)
        local done, value = Coroutine.waitFor(Timer.after(1, function() end))
        Coroutine.waitForFixedUpdate()
        Coroutine.waitForEndOfFrame()
        print(elapsed, ok, done, value, Time.deltaTime, Time.unscaledDeltaTime, Time.fixedDeltaTime)
    end)
    print(co.status, co:isDone(), co.error, co.owner, co.id)
    self:startCoroutine("onUpdate")
    self:stopCoroutine(co)
    self:stopAllCoroutines()
    self:invokeRepeating("onUpdate", 1, 0.5)
    local timer = self:invoke(function() print("later") end, 2, true)
    print(timer.remaining, timer.interval, timer.unscaled, timer:isDone(), self:isInvoking(timer), self:isInvoking())
    self:cancelInvoke("onUpdate")
    Timer.every(0.5, function(t) return t.remaining > 0 end, {count = 3, unscaled = true, owner = self, delay = 0})
    Coroutine.start(function() Coroutine.wait(1) end)
    Time.timeScale = 0.5
    Time.captureDeltaTime = 0
    print(Engine.unscaledDeltatime, Engine.unscaledTime, Engine.maxDeltatime, Coroutine.running(), Coroutine.isInside())
    -- the audio mixer (audio-mixer): option tables, nil results, the group and effect handles
    local muted = AudioMixer.setFloat("MusicVolume", -80, 0.5, {unscaled = true})
    local cutoff = AudioMixer.getFloat("lowPassCutoff") or 22000
    AudioMixer.transitionTo("Gameplay", 0.65)
    AudioMixer.transitionToSnapshots({"Menu", "Gameplay"}, {0.5, 0.5}, 1)
    local music = AudioMixer.getGroup("Music")
    if music then
        music.volume = AudioMixer.linearToDb(0.5)
        music.mute = not muted
        local lowpass = music:getEffect(1) or music:getEffect("lowpass")
        if lowpass then lowpass:setParameter("cutoff", cutoff * 0.5, 0.9) end
        print(music:getMeter(), music:getEffectCount(), #AudioMixer.getGroupNames())
    end
    AudioListener.pause = false
    AudioListener.volume = 0.8
    local sound = AudioSource(self.scene, self.entity)
    sound.outputGroup = "SFX"
    sound.loadType = AudioLoadType.DecompressOnLoad
    sound.pitch = 1.2
    sound.priority = 64
    sound:playOneShot("sfx/coin.wav", 0.7)
    sound:fadeOut(1.5, {pause = true})
    sound:fadeTo(0.5, 2)
    AudioSource.playClipAtPoint(self.scene, "sfx/boom.wav", Vector3(0, 0, 0), 1, "SFX")
    AudioSystem.preload("music/theme.ogg", AudioLoadType.Streaming)
    -- the Video Player: fields, enums, events, methods and statics (video-mp4)
    local vp = self.go:getVideoPlayer()
    if vp and self.go:hasVideoPlayer() then
        vp.source = VideoPlayerSource.URL
        vp.url = System.getUserDataPath() .. "/ads/ad1.mp4"
        vp.timeUpdateMode = VideoTimeUpdateMode.UNSCALED_GAME_TIME
        vp.renderMode = VideoRenderMode.RENDER_TEXTURE
        vp.aspectRatio = VideoAspectRatio.FIT_INSIDE
        vp.audioOutputMode = VideoAudioOutputMode.DIRECT
        vp.isLooping = false
        vp.playbackSpeed = 1.5
        vp.targetCamera = nil
        vp.onPrepareCompleted = function(p) p:play() end
        vp.onErrorReceived = function(p, message) Log.print(message) end
        vp:prepare()
        local framebuffer = vp:getFramebuffer()
        print(framebuffer, vp.isPrepared, vp.frameCount, vp.length, vp.time, vp.frame, vp.backend, vp.texture)
        print(VideoPlayer.canPlay("ads/ad1.mp4"), VideoPlayer.clearCache(), VideoPlayer.clearCache("https://cdn.example.com/a.mp4"))
    end
end

function Good:onContact(bodyA, shapeA, bodyB, shapeB)
    print(bodyA.entity, shapeA, bodyB.entity, shapeB)
end

function Good:onKeyDown(key, isRepeat, mods)
    if key == Input.KEY_SPACE and not isRepeat and mods == 0 then
        self.target:setColor(1, 0, 0, 1)
    end
end

function Good:onUpdate()
    local dt = Engine.deltatime
    local p = self.go.position
    self.go.position = Vector3(p.x + self.speed * dt, p.y, p.z) + Vector3.UNIT_Y * 0.5
    self.go.position = p / 2 + 2 * Vector3.UNIT_X - p / Vector3(1, 2, 4) + Matrix4.translateMatrix(p) * Vector3.ZERO
    self.counter = self.counter + 1
    local hit = Physics2D.raycast(self.scene, Vector2(0, 0), Vector2(0, -1), 100)
    if hit.hit then print(hit.point.x) end
    print(LayerMask.getMask("Default", "Enemies"), self.scene:findEntity("Hub") ~= NULL_ENTITY)
    local b = GameObject.instantiate(PrefabAsset.load("enemies/Enemy"), Vector3(1, 2, 3), Quaternion.IDENTITY)
    if b then
        local copy = GameObject.instantiate(b, self.go)
        if copy then GameObject.destroyImmediate(copy) end
        GameObject.destroy(b, 2.0)
    end
    local variants = ShaderVariantCollection()
    variants:add("mesh:Ult,Vc3")
    variants:addObject(self.go)
    ---@type boolean
    local ready = variants:warmUp()
    ---@type integer
    local pending = ShaderPool.getPendingShaderCount()
    print(ready, pending, ShaderPool.isShaderReady("mesh:Ult,Vc3"))
    -- script data: PlayerPrefs, Json, File (the hand-written signatures and option classes)
    PlayerPrefs.setInt("coins", PlayerPrefs.getInt("coins", 0) + 1)
    if PlayerPrefs.hasKey("name") then print(PlayerPrefs.getString("name", "")) end
    PlayerPrefs.setFloat("volume", PlayerPrefs.getFloat("volume") * 0.5)
    PlayerPrefs.save()
    local text = Json.encode({coins = 1, list = Json.array(), none = Json.null}, {pretty = true, emptyTable = "object"})
    local value, err = Json.tryDecode(text, {null = "nil", allowComments = true})
    if not value then print(err) end
    print(Json.isNull(Json.null), Json.decode(text), Json.object({}))
    local ok, werr = Json.writeFile("data://save.json", {coins = 1})
    local saved, rerr = Json.readFile("data://save.json")
    local body, ferr = File.readText("data://save.json")
    print(ok, werr, saved, rerr, body, ferr, File.exists("save.json"), File.list("data://"))
    print(File.writeText("data://a.txt", "x"), File.appendText("a.txt", "y"), File.remove("a.txt"), File.makeDirs("data://d"))
    print(UserSettings.hasKey("coins"), System.syncFileSystem(), System.isUserDataLoaded())
    UserSettings.deleteKey("coins")
    UserSettings.deleteAll()
    -- http: the hand-written signatures and option classes
    local req = Http.get("https://example.com/data.json", {timeout = 10, responseType = "json", owner = self}, function(res)
        if res.ok then
            print(res.status, res.data, res.body, res.headers["content-type"], res:header("etag"), res.url, res.redirected)
        else
            print(res.errorCode, res.error, res.elapsed)
        end
    end)
    Http.post("https://example.com/api", {score = 3}, function(res) print(res.ok, res:json()) end)
    Http.post("https://example.com/raw", "a=1", {headers = {["Content-Type"] = "text/plain"}})
    Http.request{url = "https://example.com/x", method = "PUT", json = {a = 1}, onComplete = function(res) print(res.method) end}
    Http.download("https://example.com/a.mp4", "data://ads/a.mp4", {resume = true, onProgress = function(r, got, total)
        print(r.progress, r.bytesReceived, got, total)
    end})
    Http.getTexture("https://example.com/i.png", function(res) if res.textureData then print(res.textureData:getWidth()) end end)
    Http.head("https://example.com", {redirects = false, query = {a = 1, b = "x"}})
    print(req.state == HttpState.Done, req:isDone(), req.bytesTotal, req.response, Http.maxConcurrentRequests, Http.encodeUrl("a b"))
    Http.timeout = 20
    Http.logFailures = true
    req:cancel()
end

return Good
"""


# ---------------------------------------------------------------------------
# helpers


async def wait_tooling(ed: Editor, timeout: float = 240) -> None:
    """The editor syncs the engine API snapshot when it opens a project, the tool files last."""
    deadline = time.monotonic() + timeout
    await ed.call("get_project_summary")
    while time.monotonic() < deadline:
        if (ed.project / LUA_DIR / "Globals.lua").exists() and (ed.project / CLANGD_DB).exists() \
                and (ed.project / ".clangd").exists():
            return
        await asyncio.sleep(0.5)
    ed.fail(f"tool files not written in {timeout:.0f}s ({LUA_DIR}, {CLANGD_DB}, .clangd)")


def find_tool(name: str) -> str | None:
    found = shutil.which(name)
    if found:
        return found
    local = Path("/var/lib/orquester/.local/bin") / name
    return str(local) if local.exists() else None


def luals_check(workspace: Path) -> list[dict]:
    """lua-language-server --check diagnostics (Warning and above) of a workspace, as dicts with file/line/code/message."""
    exe = find_tool("lua-language-server")
    if not exe:
        skip("lua-language-server is not installed (https://github.com/LuaLS/lua-language-server/releases); "
             "the Lua stub checks need it")
    with tempfile.TemporaryDirectory() as tmp:
        out = Path(tmp) / "check.json"
        proc = subprocess.run([exe, f"--check={workspace}", "--checklevel=Warning", "--check_format=json",
                               f"--check_out_path={out}"], capture_output=True, text=True, timeout=600)
        if not out.exists():
            raise AssertionError(f"lua-language-server --check wrote no report: {proc.stdout[-2000:]} {proc.stderr[-2000:]}")
        data = json.loads(out.read_text() or "[]") or {}
    diags = []
    for uri, items in data.items():
        for d in items:
            diags.append({"file": uri.split(str(workspace.name) + "/", 1)[-1], "line": d["range"]["start"]["line"] + 1,
                          "code": d.get("code"), "message": d["message"].replace("\n", " ")[:200]})
    return sorted(diags, key=lambda d: (d["file"], d["line"]))


def clangd_check(path: Path) -> tuple[int, list[str]]:
    """(error count, error lines) of clangd --check on one file."""
    exe = find_tool("clangd")
    if not exe:
        skip("clangd is not installed (e.g. apt install clangd); the C++ language server checks need it")
    proc = subprocess.run([exe, f"--check={path}"], capture_output=True, text=True, timeout=600, cwd=path.parent)
    log = proc.stdout + proc.stderr
    m = re.search(r"All checks completed, (\d+) errors", log)
    if not m:
        raise AssertionError(f"clangd --check did not complete for {path}:\n{log[-3000:]}")
    errors = [l for l in log.splitlines() if re.match(r"E\[[\d:.]+\] \[", l)]
    return int(m.group(1)), errors


def lua_workspace(ed: Editor, scripts: dict[str, str] | None = None, dirs: list[Path] | None = None) -> Path:
    """A scratch workspace with the project's stubs, LuaLS plugin and .luarc.json plus the given scripts."""
    ws = Path(tempfile.mkdtemp(prefix="jasper-luals-")) / "ws"
    (ws / LUA_DIR).parent.mkdir(parents=True)
    shutil.copytree(ed.project / LUA_DIR, ws / LUA_DIR)
    shutil.copy(ed.project / LUALS_PLUGIN, ws / LUALS_PLUGIN)
    shutil.copy(ed.project / ".luarc.json", ws / ".luarc.json")
    for i, d in enumerate(dirs or []):
        shutil.copytree(d, ws / f"src{i}")
    for name, text in (scripts or {}).items():
        (ws / name).write_text(text, encoding="utf-8")
    return ws


# ---------------------------------------------------------------------------
# User-owned configuration


async def test_luarc_and_clangd_respect_user_files(ed: Editor):
    """A user's .luarc.json is merged into (keys kept, library added), a user .clangd is left alone."""
    await wait_tooling(ed)
    proj = Path(tempfile.mkdtemp(prefix="jasper-userfiles-")) / "UserFiles"
    shutil.copytree(ed.project, proj, ignore=shutil.ignore_patterns(".jasperengine", ".luarc.json", ".clangd"))
    (proj / ".luarc.json").write_text(
        '{\n  // the user\'s settings\n  "workspace": { "library": ["libs/mine"], "checkThirdParty": false },\n'
        '  "diagnostics": { "disable": ["lowercase-global"] }\n}\n')
    user_clangd = "CompileFlags:\n  Add: [-Wall]\n"
    (proj / ".clangd").write_text(user_clangd)
    try:
        await ed.call("open_project", path=str(proj), unsaved="discard")
        luarc = json.loads((proj / ".luarc.json").read_text())
        ed.check_eq(luarc["workspace"]["library"], ["libs/mine", LUA_DIR], "merged workspace.library")
        ed.check_eq(luarc["workspace"]["checkThirdParty"], False, "user key kept")
        ed.check_eq(luarc["diagnostics"], {"disable": ["lowercase-global"]}, "user diagnostics kept")
        ed.check_eq(luarc.get("runtime.version"), "Lua 5.4", "runtime.version added")
        ed.check_eq(luarc.get("runtime.plugin"), LUALS_PLUGIN, "runtime.plugin added")
        ed.check_eq((proj / ".clangd").read_text(), user_clangd, "user .clangd unchanged")
        ed.check((proj / CLANGD_DB).exists(), "compile database still written")
        ed.check((proj / LUA_DIR / "Vector3.lua").exists(), "stubs written")
        # merging again changes nothing
        before = (proj / ".luarc.json").read_text()
        await ed.call("open_project", path=str(ed.project), unsaved="discard")
        await ed.call("open_project", path=str(proj), unsaved="discard")
        ed.check_eq((proj / ".luarc.json").read_text(), before, ".luarc.json rewritten without a change")
        # a stale stub (a class the engine no longer binds) is removed
        (proj / LUA_DIR / "NoLongerBound.lua").write_text(
            "---@meta _\n---@class NoLongerBound\nNoLongerBound = {}\n")
        await ed.call("open_project", path=str(ed.project), unsaved="discard")
        await ed.call("open_project", path=str(proj), unsaved="discard")
        ed.check(not (proj / LUA_DIR / "NoLongerBound.lua").exists(), "stale stub not removed")
    finally:
        await ed.call("open_project", path=str(ed.project), unsaved="discard")
        shutil.rmtree(proj.parent, ignore_errors=True)


USER_PLUGIN = """-- the user's own lua-language-server plugin: USER_PLUGIN_MACRO(...) reads as print(...)
function OnSetText(uri, text)
    local diffs = {}
    for s, f in text:gmatch('()USER_PLUGIN_MACRO()') do
        diffs[#diffs + 1] = { start = s, finish = f - 1, text = 'print' }
    end
    return diffs
end
"""


async def test_luarc_keeps_user_plugin(ed: Editor):
    """A .luarc.json with a plugin of its own (nested keys): the engine's plugin is listed first,
    and both run (the user's rewrites a macro, the engine's reports a wrong-type static field)."""
    await wait_tooling(ed)
    proj = Path(tempfile.mkdtemp(prefix="jasper-userplugin-")) / "UserPlugin"
    shutil.copytree(ed.project, proj, ignore=shutil.ignore_patterns(".jasperengine", ".luarc.json", ".clangd"))
    (proj / ".luarc.json").write_text('{ "runtime": { "version": "Lua 5.4", "plugin": "luals/my-plugin.txt" } }\n')
    (proj / "luals").mkdir()
    (proj / "luals/my-plugin.txt").write_text(USER_PLUGIN)
    script = proj / "scripts/PluginProbe.lua"
    script.parent.mkdir(parents=True, exist_ok=True)
    script.write_text('USER_PLUGIN_MACRO("macro")\nEngine.timeScale = "fast"\nEngine.timeScale = 0.5\n')
    try:
        await ed.call("open_project", path=str(proj), unsaved="discard")
        await wait_tooling(ed)
        luarc = json.loads((proj / ".luarc.json").read_text())
        ed.check_eq(luarc["runtime"], {"version": "Lua 5.4", "plugin": [LUALS_PLUGIN, "luals/my-plugin.txt"]},
                    "runtime.plugin: the engine's first, the user's kept")
        ed.check("runtime.plugin" not in luarc, f"nested style kept: {luarc!r}")
        before = (proj / ".luarc.json").read_text()
        if find_tool("lua-language-server"):
            diags = [d for d in luals_check(proj) if d["file"].endswith("PluginProbe.lua")]
            ed.check_eq([(d["line"], d["code"]) for d in diags], [(2, "assign-type-mismatch")],
                        f"both plugins ran: {diags}")
        await ed.call("open_project", path=str(ed.project), unsaved="discard")
        await ed.call("open_project", path=str(proj), unsaved="discard")
        ed.check_eq((proj / ".luarc.json").read_text(), before, ".luarc.json rewritten without a change")
    finally:
        await ed.call("open_project", path=str(ed.project), unsaved="discard")
        shutil.rmtree(proj.parent, ignore_errors=True)


# ---------------------------------------------------------------------------
# lua-language-server


PROBE_CONTROLLER = """---@class ProbeController : ScriptInstance
local ProbeController = { properties = {{ name = "strength", type = "float", default = 2.0 }} }
return ProbeController
"""

PROPERTY_ERRORS = """
function TypedProps:wrongValues()
    self.count = "two"
    self.on = "yes"
    self.label = false
    self.offset = "not a vector"
    self.tint = false
    self.body:pauseAnimation()
    self.controller:notARealMethod()
end
"""
PROPERTY_ERROR_CODES = {2: "assign-type-mismatch", 3: "assign-type-mismatch", 4: "assign-type-mismatch",
                        5: "assign-type-mismatch", 6: "assign-type-mismatch", 7: "undefined-field", 8: "undefined-field"}

TYPED_PROPS = """---@class TypedProps : ScriptInstance
---@field speed number
local TypedProps = {
    properties = {
        { name = "speed", type = "float", default = 1.0 },
        { name = "count", type = "int", default = 2 },
        { name = "on", type = "bool", default = true },
        { name = "label", type = "string", default = "x" },
        { name = "offset", type = "vector3", default = {1, 2, 3} },
        { name = "tint", type = "color4", default = {1, 1, 1, 1} },
        { name = "bullet", type = "Prefab" },
        { name = "target", type = "entity" },
        { name = "body", type = "rigidbody2d" },
        { name = "controller", type = "ProbeController" },
    }
}

function TypedProps:init()
    local n = self.speed * self.count + self.offset.x + self.tint.w
    if self.on and self.label ~= "" then print(n) end
    self.body:applyForceToCenter(Vector2(0, 1), true)
    ---@type integer
    local count = self.count
    ---@type boolean
    local on = self.on
    ---@type string
    local label = self.label
    ---@type Vector3
    local offset = self.offset
    ---@type Vector4
    local tint = self.tint
    ---@type number
    local strength = self.controller.strength
    GameObject.instantiate(self.bullet)
    GameObject(self.scene, self.target.entity)
    print(count, on, label, offset, tint, strength)
end

return TypedProps
"""

async def test_luals_script_class_property_types(ed: Editor):
    """Declared property types must support ordinary typed code and reject wrong caller values."""
    await wait_tooling(ed)
    controller = await ed.call("create_script", kind="lua", name="ProbeController", content=PROBE_CONTROLLER)
    made = await ed.call("create_script", kind="lua", name="TypedProps", content=TYPED_PROPS)
    path = ed.project / made["path"]
    stub = ed.project / LUA_DIR / "scripts" / "TypedProps.lua"
    try:
        diags = [d for d in luals_check(ed.project) if d["file"].endswith(("TypedProps.lua", "ProbeController.lua"))]
        ed.check(not diags, f"diagnostics on independently typed property uses: {diags}")
        body, ret = TYPED_PROPS.rsplit("return TypedProps", 1)
        first = body.count("\n") + 1
        path.write_text(body + PROPERTY_ERRORS.lstrip("\n") + "\nreturn TypedProps" + ret)
        await ed.call("sync_files")
        diags = [d for d in luals_check(ed.project) if d["file"].endswith("TypedProps.lua")]
        got = {d["line"] - first + 1: d["code"] for d in diags}
        ed.check_eq(got, PROPERTY_ERROR_CODES, f"property types reject wrong values and methods: {diags}")
    finally:
        path.unlink(missing_ok=True)
        (ed.project / controller["path"]).unlink(missing_ok=True)
        await ed.call("sync_files")
    ed.check(not stub.exists(), "deleting a script removes its generated class")


async def test_luals_zero_diagnostics_on_known_good_scripts(ed: Editor):
    await wait_tooling(ed)
    ws = lua_workspace(ed, {"Good.lua": GOOD_LUA})
    try:
        diags = luals_check(ws)
        ed.check(not diags, f"lua-language-server diagnostics on known-good scripts: {diags}")
    finally:
        shutil.rmtree(ws.parent, ignore_errors=True)


async def test_luals_reports_api_mistakes(ed: Editor):
    await wait_tooling(ed)
    ws = lua_workspace(ed, {"Bad.lua": BAD_LUA, "BadDocs.lua": BAD_DOCS_LUA})
    try:
        all_diags = luals_check(ws)
        for name, text, expected in (("Bad.lua", BAD_LUA, BAD_LUA_EXPECTED), ("BadDocs.lua", BAD_DOCS_LUA, BAD_DOCS_LUA_EXPECTED)):
            diags = [d for d in all_diags if d["file"].endswith(name)]
            got = {d["line"]: d["code"] for d in diags}
            for line, code in expected.items():
                ed.check_eq(got.get(line), code, f"{name}:{line} ({text.splitlines()[line - 1].strip()}); all: {diags}")
            ed.check_eq(len(diags), len(expected), f"unexpected diagnostics in {name}: {diags}")
    finally:
        shutil.rmtree(ws.parent, ignore_errors=True)


# Appended to the Lua template: self is typed through its `---@class X : ScriptInstance`
# (line of the probe, 1 = its `function` line -> diagnostic code)
TEMPLATE_PROBE = """
function StubTemplateCheck:probe()
    print(self.entiy)
    self.scene:findEntityy("Hub")
    ---@type string
    local entity = self.entity
    ---@type string
    local active = self.isActive
    print(entity, active, self.scene:findEntity("Hub"))
end
"""
TEMPLATE_PROBE_EXPECTED = {
    2: "undefined-field",       # a typo of self.entity
    3: "undefined-field",       # self.scene is a Scene
    5: "assign-type-mismatch",  # self.entity is an integer
    7: "assign-type-mismatch",  # the isActive property is a boolean
}


async def test_luals_new_lua_script_template(ed: Editor):
    """The editor's Lua template (create_script kind lua) checks clean, and its class
    annotation types self: a typo or a wrong type through self is reported."""
    await wait_tooling(ed)
    ed.check(".jasperengine/" in (ed.project / ".gitignore").read_text(),
             "generated tooling artifacts are ignored in a new project")
    made = await ed.call("create_script", kind="lua", name="StubTemplateCheck")
    path = ed.project / made["path"]
    try:
        text = path.read_text()
        diags = [d for d in luals_check(ed.project) if d["file"].endswith("StubTemplateCheck.lua")]
        ed.check(not diags, f"diagnostics on the Lua template: {diags}")

        body, ret = text.rsplit("return StubTemplateCheck", 1)
        first = body.count("\n") + 1  # file line of the probe's `function` line
        path.write_text(body + TEMPLATE_PROBE.lstrip("\n") + "\nreturn StubTemplateCheck" + ret)
        diags = [d for d in luals_check(ed.project) if d["file"].endswith("StubTemplateCheck.lua")]
        got = {d["line"] - first + 1: d["code"] for d in diags}
        ed.check_eq(got, TEMPLATE_PROBE_EXPECTED, f"diagnostics through the typed self: {diags}")
    finally:
        path.unlink(missing_ok=True)
        await ed.call("sync_files")


# ---------------------------------------------------------------------------
# clangd


async def test_clangd_cpp_templates_parse(ed: Editor):
    await wait_tooling(ed)
    await ed.call("set_project_settings", values={"scripting.cxx_standard": 20})
    # Tooling is regenerated on project open/build, rather than on each settings edit.
    # beside the current project: create_project over MCP makes a project there, not in an unrelated folder
    with tempfile.TemporaryDirectory(prefix="jasper-tooling-reopen-", dir=ed.scratch.root) as other:
        await ed.call("create_project", path=str(Path(other) / "Other"), name="ToolingReload", unsaved="discard")
        await ed.call("open_project", path=str(ed.project), unsaved="discard")
    await wait_tooling(ed)
    made = await ed.call("create_entity", name="StubCppHost", type="box")
    eid = made["entity_id"]
    script = await ed.call("create_script", kind="cpp_script", name="StubScriptCheck")
    sub = await ed.call("create_script", kind="cpp_subclass", name="StubSubclassCheck", entity=eid)
    source = ed.project / script["path"]
    source.write_text(source.read_text() + "\n// The configured C++20 standard must reach the IDE.\n"
                      "template<class T> concept AcceptsOne = requires(T value) { value + 1; };\n"
                      "static_assert(AcceptsOne<int>);\n")
    # Each source includes its own header first, so these parses also check the headers.
    files = [source, ed.project / sub["path"]]
    for f in files:
        n, errors = clangd_check(f)
        ed.check_eq(n, 0, f"clangd errors in {f.name}: {errors}")

    bad = ed.project / "scripts" / "StubBadCheck.cpp"
    bad.write_text('#include "StubScriptCheck.h"\n\nvoid stubBad() {\n'
                   '    jasperengine::Vector3 v(1, 2, 3);\n    v.lengthh();\n    jasperengine::Engine::getDeltaTimeX();\n}\n')
    try:
        n, errors = clangd_check(bad)
        ed.check(n >= 2, f"clangd should report the two mistakes, got {n}: {errors}")
        for missing_method in ("lengthh", "getDeltaTimeX"):
            ed.check(any(missing_method in e for e in errors), f"no error for {missing_method}: {errors}")
    finally:
        bad.unlink(missing_ok=True)
