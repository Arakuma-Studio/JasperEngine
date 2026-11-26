---@meta _
-- HTML UI documents (UIDocument, UIElement): shapes the bindings cannot describe. Hand-written
-- fragment (engine/core/script/binding/stubs), appended to the generated stubs.

---Options of addEventListener / removeEventListener (a boolean means `capture`).
---@class UIEventListenerOptions
---@field capture boolean|nil listen in the capture phase (an ancestor hears the event first)
---@field once boolean|nil remove the listener after its first call

---A data model (UIDocument:createDataModel, UIDocument:getDataModel), bound by `data-model`,
---`{{ }}`, `data-for`, `data-value`, `data-event-*` in the document. Its fields read and write the
---model's data: `model.gold = 20` updates the document; a nested table reads as a proxy too
---(`model.items[1].price = 7`, `table.insert(model.items, {...})`, `#model.items`, `pairs`).
---Lua indices are 1-based; the `i` of `data-for="item, i : items"` is 0-based (RmlUi). The
---top-level keys are fixed when the model is created. Once the model is removed, any access but
---isValid() raises an error.
---@class UIDataModel
---@field [string] any
local UIDataModel = {}

---Calls `fn` with the new value after the document wrote the top-level variable `name`
---(data-value, data-checked), after the document update. Returns an id for removeOnChange.
---@param name string
---@param fn fun(value: any)
---@return integer
function UIDataModel:onChange(name, fn) end

---Removes an onChange callback.
---@param id integer
function UIDataModel:removeOnChange(id) end

---Marks the top-level variable `name` changed (the document updates it). Assignments through
---the proxy do this themselves.
---@param name string
function UIDataModel:dirty(name) end

---Marks every variable changed.
function UIDataModel:dirtyAll() end

---The model's name.
---@return string
function UIDataModel:getName() end

---False once the model was removed (UIDocument:removeDataModel, or its document's component).
---@return boolean
function UIDataModel:isValid() end

---A copy of the value at a path ("items[2].name", 0-based indices as in C++), or nil.
---@param path string
---@return any
function UIDataModel:get(path) end

---Writes the value at a path ("items[2].name"); a missing nested key is added.
---@param path string
---@param value any
function UIDataModel:set(path, value) end

---What HtmlUI.prewarmText makes ready: the font as CSS names it, and its font-effect.
---@class UIPrewarmTextOptions
---@field family string|nil a font-family list (default "sans-serif")
---@field size number|nil px (default 16)
---@field weight number|"normal"|"bold"|nil (default 400)
---@field style "normal"|"italic"|nil
---@field effect string|nil a font-effect value: "outline(15px #000)", "glow(4px 12px 0px 8px #000)"
---@field text string the characters to make ready

---The input scheme on every document's `<html>` (`data-input-scheme`), set the frame the player
---switches device (InputActions.currentScheme): `html[data-input-scheme="touch"] .hint` in CSS,
---`element:getAttribute("data-input-scheme")` in a script. A script's own value is overwritten at
---the next change.
---@alias HtmlInputScheme "keyboard-mouse"|"gamepad"|"touch"

---The pad's family on every document's `<html>` (`data-gamepad-family`, InputActions.gamepadFamily).
---@alias HtmlGamepadFamily "xbox"|"playstation"|"nintendo"|"generic"

---The attributes of `<input-prompt>`: the action's binding for the input scheme as its text
---(InputActions.getActionDisplayName), unless the author gave it children.
---@class HtmlInputPromptAttributes
---@field action string the action, "Map/Action" or an Input Manager name
---@field scheme "current"|"touch"|"gamepad"|"keyboard-mouse"|nil the scheme shown (default current)
---@field ["data-glyph"] string set by the engine: the glyph key ("gamepad/playstation/cross", "" without a binding)
---@field ["data-scheme"] HtmlInputScheme set by the engine: the scheme shown
---@field ["data-empty"] "true"|"false" set by the engine: "true" when the action has no binding for that scheme

---The `back` event (`doc:addEventListener("back", fn)`): Back (Android's Back button, Escape,
---gamepad B) reached the top Is Menu document (UIDocument.isMenu), at its focused element, else its
---root; it bubbles. `event:preventDefault()` takes it; otherwise an open select, else the topmost
---modal dialog closes, then Engine.onBack(source) gets it.
---@alias UIBackEventType "back"
