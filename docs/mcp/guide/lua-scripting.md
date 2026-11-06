# Writing Lua scripts

The agent guide (`jasper://guide`) has the short version; this is the whole of it, with a
complete script. The bound classes, their properties and methods and the enums are in
`jasper://docs/lua-api` (an index: read only the sections you need); the project's own stubs
are in `.jasperengine/engine-api/lua/`.

## A script

A Lua script returns a module table; attach it to a GameObject through its Script component.
self.scene is the Scene, self.entity the GameObject's numeric id (NOT an object: never call
self.entity:...). Wrap it: GameObject(self.scene, self.entity), Sprite(...), Text(...),
Rigidbody2D(...), AudioSource(...); a constructor given only self.scene CREATES a new
GameObject. Declare the module `---@class Name : ScriptInstance` (Name = the file name): lua-language-server
then types self and its properties and catches typos.

```lua
---@class Player : ScriptInstance
local Player = {
    properties = {
        { name = "speed", displayName = "Speed", type = "float", default = 300.0 },
        { name = "label", displayName = "Label", type = "Text" },      -- GameObject reference
        { name = "bullet", displayName = "Bullet", type = "Prefab" },  -- PrefabAsset
    }
}

function Player:init()
    self.go = GameObject(self.scene, self.entity)
    RegisterEngineEvent(self, "onUpdate")
end

function Player:onUpdate()
    local dx = 0
    if Input.isKeyPressed(Input.KEY_RIGHT) then dx = dx + 1 end
    if Input.isKeyPressed(Input.KEY_LEFT) then dx = dx - 1 end
    local p = self.go.position                          -- a copy: assign it back
    self.go.position = Vector3(p.x + dx * self.speed * Engine.deltatime, p.y, p.z)
    if Input.isKeyPressed(Input.KEY_SPACE) and not self.fired then
        GameObject.instantiate(self.bullet, self.scene, self.go.position, Quaternion.IDENTITY)
        Log.print("fired at " .. tostring(p.x))
    end
    self.fired = Input.isKeyPressed(Input.KEY_SPACE)
end

return Player
```

## Rules

- Property types: bool, int, float, string, vector2/3/4, color3/color4, prefab, entity, any
  wrapper class name (Sprite, Text, Image, Button, AudioSource, Rigidbody2D, ...), or another
  Lua script's class name (you get that script's self). Values are read as self.<name>.
- Events: RegisterEngineEvent(self, "onUpdate" | "onFixedUpdate" | "onPostUpdate" |
  "onKeyDown" | "onMouseDown" | "onTouchStart" | ...); define the method first. Component
  events: RegisterEvent(self, Button(self.scene, self.entity):getButtonComponent().onPress,
  "onPress"). No manual unregister. onDestroy runs on GameObject.destroy.
- Bound getters are often PROPERTIES, not methods: go.position, sprite.width, body.mass,
  Engine.deltatime. A non-existent method fails at runtime ("attempt to call a nil value").
  Check jasper://docs/lua-api (an index: read only the sections you need), the Lua stubs or
  search_engine_api before using a name you have not seen there.
- Input.isKeyPressed (getKey) is the held state; Input.getKeyDown / getKeyUp are this frame's
  press and release (both true for a tap inside one frame; one fixed step sees each in
  onFixedUpdate). Mouse delta: Input.getMouseDelta(). More: jasper://docs/input. Log.print/warn/error take ONE string (use ..).
- Time: Engine.timeScale and Engine.fixedDeltatime (read/write), Engine.gameTime and
  Engine.frameCount (scaled game time and frames since start).
- Lua has no C++ default arguments: pass every argument (e.g. the wake flag).
- Lua has no on_start, getComponent or setProperty (timers: Timer.after/every). Never put
  editor property paths (submeshes[0]...) in scripts.
- During play a saved Lua script reloads in place and keeps its instances (the next result's
  external_changes.script_reload; a file that fails keeps its old code running). C++ waits for
  Stop unless set_script_settings reload_cpp_during_play.
