---@meta _
-- Hand-written: InputRecording is bound with Lua C functions
-- (engine/core/script/binding/InputRecordingClassesLua.cpp), which the generator does not read.

---Options of InputRecording.start
---@class InputRecordingStartOptions
---@field checkpointFrames? integer # a state checkpoint every N frames (default 60; 0: none)
---@field probes? string[] # GameObject paths whose positions the checkpoints keep (up to 16; default: chosen)
---@field maxBytes? integer # the recording's cap (default 1 MB, at most 16 MB): it stops there, truncated

---Options of InputRecording.play
---@class InputRecordingPlayOptions
---@field loop? boolean # play it again from the start when it ends (default false)
---@field blockRealInput? boolean # device input the replay did not make does not reach the game (default true)
---@field stopOnRealInput? boolean # the first real key, button or touch ends it and reaches the game (default false)
---@field onFinished? fun(replay: InputReplay) # called once when it ends

---Records the input that reaches the game into a .input.json file, and replays recordings and
---scripts through the engine's input entry points (attract modes, game tools)
---@class InputRecording
---@field frame integer # read-only: the recording's frame, else the replay's; -1 when idle
InputRecording = {}

---Starts recording from the next frame (a start: "now" recording); refused while one runs
---@param opts? InputRecordingStartOptions
---@return boolean|nil ok
---@return string? err
function InputRecording.start(opts) end

---Stops the recording and writes it on a job (default data://recordings/<yyyymmdd-hhmmss>.input.json;
---lua:// and asset:// are read-only)
---@param path? string
---@return string|nil path
---@return string? err
function InputRecording.stop(path) end

---Plays a recording or a script: a lua://, asset:// or data:// path, or a decoded file table
---@param source string|table
---@param opts? InputRecordingPlayOptions
---@return InputReplay|nil replay
---@return string? err
function InputRecording.play(source, opts) end

---@return boolean
function InputRecording.isRecording() end

---@return boolean
function InputRecording.isReplaying() end

---Stops every replay
function InputRecording.stopReplay() end

---A game value the checkpoints sample and a replay compares (numbers, booleans, strings, vectors;
---at most 64 names)
---@param name string
---@param value number|boolean|string|Vector2|Vector3
---@return boolean|nil ok
---@return string? err
function InputRecording.checkpoint(name, value) end
