---@meta _
-- Hand-written: Dialogue is bound with the Lua C API
-- (engine/core/script/binding/DialogueClassesLua.cpp), which the generator does not read.
-- DialogueRunner is generated from the same file.

---An option of a line, after its condition (docs/dialogue.md)
---@class DialogueOption
---@field index integer # 1-based among the presented options: runner:choose(index)
---@field id string # "o2": stable across edits
---@field key string # the String Table key "<namespace>.<node>.<option>"
---@field text string # formatted in the game language
---@field enabled boolean # false: a when_false = disable option whose condition is false (shown greyed)
---@field style string # a free tag for the view: "", "accept", "danger", "exit"...
---@field tags string[]

---What a Dialogue Runner shows: a line and its choices (empty text with options: a choice menu)
---@class DialogueLine
---@field node string # "n7"
---@field key string # "<namespace>.<node>", or "<namespace>.<node>.<variant>"
---@field text string # formatted in the game language with the message arguments
---@field speaker string # as written in the dialogue
---@field speakerName string # the speakers.<speaker> text when a table has it, else speaker
---@field mood string
---@field tags string[]
---@field voice? string # an audio clip or Sound Event path
---@field variant? string # the variant shown ("v1"), nil for the line's own text
---@field options DialogueOption[]

---The game-wide dialogue variable store: what `$name` reads and writes in every dialogue
---@class DialogueVariables
---@field onChanged FunctionSubscribe_V_DialogueChanged # fun(name: string, value: boolean|number|string|nil) after a change; assign a function or :add(tag, fn)
local DialogueVariables = {}

---The value of a variable (nil when absent)
---@param name string
---@return boolean|number|string|nil
function DialogueVariables.get(name) end

---Sets a variable (onChanged fires when it changes)
---@param name string
---@param value boolean|number|string
function DialogueVariables.set(name, value) end

---The store as a table {variables = {...}, visits = {["<dialogue>#<node>"] = count}}: what a save keeps
---@return table
function DialogueVariables.toTable() end

---Replaces the store with a table toTable() made (an error, nothing changed, for another shape)
---@param state table
function DialogueVariables.fromTable(state) end

---Writes toTable() to a save slot: SaveGame.write with namespace "dialogue" unless the options name one
---@param slot string
---@param options? SaveWriteOptions
---@return boolean ok
---@return SaveError|nil err
function DialogueVariables.save(slot, options) end

---Reads the namespace save wrote (SaveGame.read, migrations included) and replaces the store with it
---@param slot string
---@param options? SaveReadOptions
---@return boolean ok
---@return SaveInfo|SaveError info
function DialogueVariables.load(slot, options) end

---Forgets every visit count (visited(), visits(), once options and variants start over)
function DialogueVariables.resetVisits() end

---Dialogue graphs (docs/dialogue.md): handlers by prefix for the actions and conditions dialogues
---call (`quest.start:intro`, `shop.canAfford(5)`), and the variable store. Handlers are game-wide
---and cleared at Stop; register them in a script's init.
---@class Dialogue
---@field variables DialogueVariables
Dialogue = {}

---Runs `prefix.verb(...)` actions: fn(runner, verb, args) with the evaluated arguments; returning
---"wait" pauses the runner until runner:resume(). One handler per prefix: a second replaces the first.
---@param prefix string # "quest" (a trailing "." is ignored)
---@param fn fun(runner: DialogueRunner|nil, verb: string, args: any[]): string|nil
function Dialogue.onAction(prefix, fn) end

---Answers `prefix.verb(...)` in conditions: fn(runner, verb, args) returns the value (truthy: true).
---An unregistered prefix makes the condition false and warns once per run.
---@param prefix string
---@param fn fun(runner: DialogueRunner|nil, verb: string, args: any[]): boolean|number|string|nil
function Dialogue.onCondition(prefix, fn) end

---Removes both handlers of a prefix
---@param prefix string
function Dialogue.removeHandler(prefix) end
