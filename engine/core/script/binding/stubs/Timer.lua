---@meta _
-- Hand-written: Timer is bound with the Lua C API (engine/core/script/binding/UtilClassesLua.cpp),
-- which the generator does not read.

---Options of the Timer functions
---@class TimerOptions
---@field owner? table # a script instance (self): the timer is cancelled when that script goes away
---@field clock? "game"|"unscaled"|"real" # default "game" (scaled by Time.timeScale, stopped while paused)
---@field unscaled? boolean # the same as clock = "unscaled"
---@field count? integer # Timer.every: how many calls (default: until cancelled)
---@field delay? number # Timer.every: the first call's delay (default: the interval)
---@field catchUp? boolean # Timer.every: after a stall, up to 8 missed periods at once

---A pending call (Timer.after, every, nextFrame, afterFrames, afterFixedSteps); tostring gives
---"Timer(5, 0.75 s left, every 0.50 s)". Coroutine.waitFor(timer) waits for it.
---@class TimerHandle
---@field id integer # read-only
---@field remaining number # read-only: seconds to the next call (0 when done)
---@field interval number # read-only: seconds between calls (0 for a one-shot)
---@field unscaled boolean # read-only: it counts unscaled seconds
local TimerHandle = {}
---Cancels it (a repeating one stops); false when it already ran or was cancelled
---@return boolean
function TimerHandle:cancel() end
---@return boolean
function TimerHandle:isActive() end
---It will not be called again (it ran out, returned false or was cancelled)
---@return boolean
function TimerHandle:isDone() end

---A coroutine started by Timer.run
---@class TimerCoroutine
local TimerCoroutine = {}
---Stops it at its current wait (from inside: at its next wait)
---@return boolean
function TimerCoroutine:cancel() end
---@return boolean
function TimerCoroutine:isActive() end

---Delayed and repeating calls and coroutines on the engine's scheduler
---@class Timer
Timer = {}

---Calls fn once, delay seconds from now
---@param delay number
---@param fn fun(timer: TimerHandle)
---@param opts? TimerOptions
---@return TimerHandle
function Timer.after(delay, fn, opts) end

---Calls fn every period seconds (fn returning false stops it)
---@param period number
---@param fn fun(timer: TimerHandle): boolean?
---@param opts? TimerOptions
---@return TimerHandle
function Timer.every(period, fn, opts) end

---Calls fn after the next simulated frame
---@param fn fun()
---@param opts? TimerOptions
---@return TimerHandle
function Timer.nextFrame(fn, opts) end

---@param frames integer
---@param fn fun()
---@param opts? TimerOptions
---@return TimerHandle
function Timer.afterFrames(frames, fn, opts) end

---@param steps integer
---@param fn fun()
---@param opts? TimerOptions
---@return TimerHandle
function Timer.afterFixedSteps(steps, fn, opts) end

---Runs fn as a coroutine at once, up to its first Timer.wait*
---@param fn fun()
---@param opts? TimerOptions
---@return TimerCoroutine
function Timer.run(fn, opts) end

---Inside Timer.run: waits seconds on the run's clock
---@param seconds number
function Timer.wait(seconds) end

---Inside Timer.run: waits wall-clock seconds (also while paused)
---@param seconds number
function Timer.waitRealtime(seconds) end

---Inside Timer.run: waits simulated frames (default 1)
---@param frames? integer
function Timer.waitFrames(frames) end

---Inside Timer.run: waits one fixed update
function Timer.waitFixedStep() end

---Inside Timer.run: waits until predicate returns true (checked once per frame)
---@param predicate fun(): boolean
function Timer.waitUntil(predicate) end

---Cancels every timer and coroutine of owner; returns how many
---@param owner any
---@return integer
function Timer.cancelAll(owner) end
