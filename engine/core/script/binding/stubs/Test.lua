---@meta _
-- Hand-written: Test is bound with the Lua C API (engine/core/test/TestRunner.cpp), only inside
-- test chunks (*.test.lua under the project's Tests/ folder), never in _G (docs/game-tests.md).

---Options of a case (Test.case) or of a file's cases (Test.config)
---@class TestCaseOptions
---@field mode? "play"|"edit" # default "play": a fresh Play session; "edit": the editor, read-only, no waits
---@field scene? string # Build Settings name or path; default the start scene
---@field data? string # "clean" (default, an empty data:// folder) or a Tests/Data snapshot name
---@field replay? string # an Input/*.input.json recording
---@field fixedFps? integer # default tests.yaml fixed_fps (60)
---@field resolution? string # "WxH", default tests.yaml resolution
---@field timeoutFrames? integer # the case fails past it (default tests.yaml timeout_frames)
---@field tags? string[] # run_tests filter "tag:<t>"
---@field platforms? string[] # only on these targets (editor, linux, windows, macos, android, ios, web)
---@field skip? string # skip with this reason
---@field expectErrors? string[] # Lua patterns of error lines this case expects (each absorbs one)

---The running case (the argument of a case's function)
---@class TestCase
---@field name string # read-only
---@field id string # read-only: "<file>#<name>"
---@field frame integer # read-only: game frames since the case began
---@field gameTime number # read-only: game seconds since the case began

---Where the case runs
---@class TestTarget
---@field platform string # "editor", "linux", "android", "web"...
---@field backend string # "glcore", "vulkan", "metal", "d3d11", "gles3"
---@field editor boolean
---@field resolution string # "WxH"

---The test API of a test file
---@class Test
---@field target TestTarget # read-only
Test = {}

---Registers a case: `fn(t)` runs in its own session; waits resume it after game frames
---@param name string
---@param opts TestCaseOptions|fun(t: TestCase)
---@param fn? fun(t: TestCase)
function Test.case(name, opts, fn) end
---Defaults of every case of this file
---@param opts TestCaseOptions
function Test.config(opts) end
---Runs before each case of the file, in its session
---@param fn fun(t: TestCase)
function Test.setup(fn) end
---Runs after each case of the file, also after a failure
---@param fn fun(t: TestCase)
function Test.teardown(fn) end
---Records a failure when cond is false and goes on
---@param cond any
---@param message? string
---@return boolean
function Test.check(cond, message) end
---Records a failure when cond is false and ends the case
---@param cond any
---@param message? string
function Test.assert(cond, message) end
---Compares deeply (tables up to 8 levels); a failure prints both values and the first differing path
---@param actual any
---@param expected any
---@param message? string
---@return boolean
function Test.equal(actual, expected, message) end
---|a - b| <= epsilon (default 1e-6)
---@param a number
---@param b number
---@param epsilon? number
---@param message? string
---@return boolean
function Test.near(a, b, epsilon, message) end
---Records a failure and ends the case
---@param message? string
function Test.fail(message) end
---Ends the case as skipped (never counted as passed)
---@param reason? string
function Test.skip(reason) end
---Waits n game frames; returns the case's frame
---@param n integer
---@return integer frames
function Test.waitFrames(n) end
---Waits game seconds (scaled game time, never the wall clock)
---@param seconds number
---@return integer frames
function Test.waitGameSeconds(seconds) end
---Waits until predicate() is true, checked once per frame after the scenes' update; past its cap
---(frames, or gameSeconds, default tests.yaml wait_frames) the case fails with message
---@param predicate fun(): any
---@param opts? {frames?: integer, gameSeconds?: number, message?: string}
---@return integer frames
function Test.waitUntil(predicate, opts) end
---Reads the case's frame (loaded content only) and compares it with Tests/Golden/<case>/<name>.png;
---a capture never ends the case
---@param name string
---@param opts? {tolerance?: integer, maxFraction?: number, maxDiff?: integer, maxMean?: number, region?: {x: integer, y: integer, w: integer, h: integer}}
function Test.capture(name, opts) end
---An error line matching this Lua pattern is expected once (else the case fails at its end)
---@param pattern string
function Test.expectError(pattern) end
---Attaches a line to the case's result
---@param ... any
function Test.log(...) end
