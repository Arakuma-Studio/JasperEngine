# A doc with mistakes (tools/lua_api/tests: the Lua doc test must name them)

```lua
local v = Vector3(1, 2, 3)
local len = v:length()
print(len, Engine.deltatime)
Vector3:nope()
local m = Engine.nopeField
```

Not Lua: `Vector3:nope()` in prose is not checked.
