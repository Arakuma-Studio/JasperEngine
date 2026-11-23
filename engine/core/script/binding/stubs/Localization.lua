---@meta _
-- Hand-written: Localization is bound with the Lua C API
-- (engine/core/script/binding/LocalizationClassesLua.cpp), which the generator does not read.

---A text Localization.scan() found on screen that no key produced
---@class LocalizationLiteral
---@field where string # the HTML element ref or the GameObject
---@field text string

---A lookup that did not find a translation this run
---@class LocalizationMiss
---@field key string # "ns.key"
---@field language string
---@field kind "untranslated"|"unknown_key"|"stale"
---@field where string # the first element or GameObject that asked ("" from Lua)
---@field count integer

---@class LocalizationScan
---@field literal LocalizationLiteral[]
---@field missing LocalizationMiss[]

---String Tables and the game language (docs/localization.md). Keys are "<namespace>.<key>", or a
---key alone for the Default Table (Project Settings > Localization); texts
---are ICU messages: {name}, {n, number}, {n, plural, one {# coin} other {# coins}}, {g, select, ...}.
---@class Localization
---@field language string # the resolved game language, e.g. "pt-BR" ("" without String Tables); assigning it is setLanguage, a bad tag raises
---@field languages string[] # read-only: the languages the game ships, the default first
---@field onLanguageChanged FunctionSubscribe_V_S # fun(code: string), once per change; assign a function or :add(tag, fn)
Localization = {}

---The message of a key in the game language. A key no table has returns `fallback`, else "" in a
---release build and "⟦key⟧" in the editor and Development Builds. A string argument "@ns.key" is
---replaced by that key's text.
---@param key string
---@param args? table<string|integer, string|number|boolean> # names fill {name}; a list fills {0} {1} ...
---@param fallback? string
---@return string
function Localization.get(key, args, fallback) end

---Formats an ICU message written in code (default: the game language's plural rules and numbers)
---@param pattern string
---@param args? table<string|integer, string|number|boolean> # names fill {name}; a list fills {0} {1} ...
---@param language? string
---@return string
function Localization.format(pattern, args, language) end

---Whether some table has the key
---@param key string
---@return boolean
function Localization.has(key) end

---Sets the game language (the same value as TitleArt.setLanguage), remembered across runs with
---Remember Language Choice; "" forgets the choice and follows the device again. False for an invalid tag.
---@param code string
---@return boolean
function Localization.setLanguage(code) end

---The language's own name ("Deutsch"), for a language menu
---@param code string
---@return string
function Localization.getLanguageName(code) end

---Whether the language (default: the game language) is written right to left
---@param code? string
---@return boolean
function Localization.isRightToLeft(code) end

---Development Builds and the editor: the texts on screen no key produced, and the misses so far
---(empty lists in a release build)
---@return LocalizationScan
function Localization.scan() end
