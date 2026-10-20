---@meta _

---Any CSS property, camelCase (`style.backgroundColor`; `style.webkitLineClamp` is `-webkit-line-clamp`) or hyphenated (`style["background-color"]`):
---reading gives the computed value as CSS text, assigning sets the inline style ("" or nil removes it).
---@class UIStyle
---The inline declarations as text; assigning replaces them all (phase 2)
---@field cssText string
---@field [string] string
