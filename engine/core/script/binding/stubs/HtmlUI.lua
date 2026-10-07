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
