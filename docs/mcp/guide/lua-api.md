# JasperEngine Lua scripting API

Reference for writing Lua gameplay scripts (scene scripts attached to GameObjects). Every
name below is taken from the LuaBridge bindings in `engine/core/script/binding/*.cpp` and
the glue in `engine/core/script/LuaBinding.cpp`. If a name is not here, assume it is NOT
bound; do not guess C++ names.

Notation used in the tables:

| Notation | Meaning | Lua call form |
|---|---|---|
| `prop` (rw) | `addProperty` with getter+setter | `obj.prop`, `obj.prop = v` |
| `prop` (ro) | `addProperty` getter only | `obj.prop` (assigning errors) |
| `fn(args)` | `addFunction` (method) | `obj:fn(args)` (colon!) |
| `Class.fn(args)` | `addStaticFunction` | `Class.fn(args)` (dot!) |
| `Class.NAME` | static property / namespace constant | `Class.NAME` (no parentheses) |

Lua 5.4, LuaBridge3. All standard libs are open (`math`, `string`, `table`, `os`, `io`, ...).

---

## 1. Script module structure

A Lua script is a file under the project's Lua directory that **returns a table** (the module).
The editor attaches it to a GameObject (Script component); its class name is the file base
name (`Player.lua` -> `Player`).

```lua
-- Player.lua
---@class Player : ScriptInstance
local Player = {
    properties = {
        { name = "speed",   displayName = "Speed",   type = "float", default = 200.0 },
        { name = "isActive", displayName = "Is Active", type = "bool", default = true },
        { name = "target",  displayName = "Target",  type = "Sprite" },     -- entity reference
        { name = "bullet",  displayName = "Bullet",  type = "Prefab" },     -- prefab asset
    }
}

function Player:init()
    -- self.scene (Scene) and self.entity (number) are set by the engine
    RegisterEngineEvent(self, "onUpdate")
end

function Player:onUpdate()
    -- per frame
end

return Player
```

Tools: `create_script` (kind `lua`, optional `content`, `entity` to attach) writes the file in
the Lua directory; `check_script` checks syntax (without running) and lists the properties the
Inspector will show; `set_script_properties` sets them on a GameObject. Edit scripts with your
own file tools: before your next tool call the editor reloads the code editor, re-parses
`properties` (new entries appear in the Inspector) and that call's result lists the file in
`external_changes` with syntax errors and their line numbers (during play: once play stops).
Runtime errors only appear when playing: read the Console.

The `---@class Player : ScriptInstance` line (the editor's templates write it; `Player` = the
file name) is a LuaLS annotation, a comment to the engine: it lets lua-language-server type
`self` (`self.scene`, `self.entity` and every property, whose types the editor reads from the
`properties` table) and report a typo such as `self.entiy`. The Lua API stubs it checks
against are in `.jasperengine/engine-api/lua/`, written whenever the editor creates or opens
the project (see [editor-integration.md](editor-integration.md)).

### 1.1 Lifecycle

Unity's MonoBehaviour lifecycle. The engine calls these methods **by name** when the script
defines them (no registration):

| Method | When | Unity |
|---|---|---|
| `init(self)` | once, the first time the GameObject is active in hierarchy, **whether or not the script is enabled**; after every instance and reference of the scene exists | `Awake` |
| `onEnable(self)` | the script becomes active and enabled: after `init` at load / instantiate / activation, and every time it is re-enabled or its GameObject re-activated | `OnEnable` |
| `start(self)` | once, at the start of the frame after its first `onEnable`, before that frame's `onFixedUpdate`/`onUpdate`; not run while disabled or inactive (it waits). A script's registered updates begin only after its `start`: one enabled during a frame gets its first `onUpdate` in the next frame, after `start` | `Start` |
| `onDisable(self)` | the script stops being active and enabled: `self.enabled = false`, its GameObject or a parent deactivated, destroy, scene unload, Play stop; always before `onDestroy` | `OnDisable` |
| `onDestroy(self)` | once: `GameObject.destroy` (end of frame) / `destroyImmediate`, scene unload (a `SceneManager.loadScene`), Play stop / application quit; only if `init` ran | `OnDestroy` |
| `onApplicationPause(self, paused)` | the app goes to the background (`true`) and returns (`false`); scripts of active GameObjects, enabled or not | `OnApplicationPause` |
| `onApplicationFocus(self, focused)` | the app lost / gained the focus (platforms that report it) | `OnApplicationFocus` |
| `onApplicationQuit(self)` | Play stop / app shutdown, before `onDisable`/`onDestroy` | `OnApplicationQuit` |

Order, deterministic: a scene load runs all `init` (parents before children, siblings in order,
scripts in component order), then all `onEnable`, then the scene's HTML documents start;
`start` follows at the next frame. A scene load first unloads the old scenes (their
`onDisable`, then `onDestroy`), then the new ones `init`: a singleton clearing
`Manager.instance` in `onDestroy` does not clobber the new one. `GameObject.instantiate`
returns after the copies' `init` and `onEnable` ran. `go:setActive(true)` runs `init` (first
time) and `onEnable` before it returns; `setActive(false)` runs `onDisable`. Play stop (and
an exported build's shutdown): `onApplicationQuit` to every script, then `onDisable`, then
`onDestroy`. A mobile OS may end an app without the shutdown: save state in
`onApplicationPause(true)`.

| Step | What happens |
|---|---|
| Instance creation | Every Script component entry has an instance, enabled or not (Unity components exist when disabled), also on inactive GameObjects. The file's chunk is executed once **per instance** (compiled once, run per attached script); the instance gets its methods from the returned module table. So file-level `local` variables are per instance; `require`d modules are shared. |
| Fields set | `self.__name` (class name), `self.scene` (Scene userdata), `self.entity` (integer Entity id), then every property `self.<name>` = Inspector value. |
| References | After all instances of the scene exist, entity-reference properties are resolved (second pass). So in `init()` all references are valid, but the referenced script's own `init()` may not have run yet. |
| Lifecycle | `init`, `onEnable`, `start`, ... as above. Register events in `init` (or `onEnable`, unregistering in `onDisable`). |
| Events | Methods registered with `RegisterEngineEvent` / `RegisterEvent` are called by the engine. |
| Release | After `onDestroy` (or for a GameObject never active, without it): subscriptions made through `RegisterEngineEvent`/`RegisterEvent` and `Event` listeners the instance owns are removed. |

Script messages besides the lifecycle (defined methods, no registration; every script of the
GameObject that defines it gets the call, enabled or not, after the fixed step that caused it):

| Method | When |
|---|---|
| `onJointBreak2D(self, joint)` | a `Joint2D` of this GameObject, or whose bodyA is it, broke (its `breakForce` / `breakTorque` passed; `joint` still exists during the call, its `breakAction` decides what follows) |

Every instance also has (Unity's MonoBehaviour members):

| Member | Kind | Meaning |
|---|---|---|
| `self.enabled` | rw boolean | the Script entry's enabled flag (the Inspector checkbox); assigning calls `onEnable`/`onDisable` at once, inside the assignment (as Unity), and `start` at the next frame the first time. Only `true`/`false` (else error). |
| `self.isActiveAndEnabled` | ro boolean | GameObject active in hierarchy and the script enabled |
| `self.gameObject` | ro `GameObject` | the script's GameObject (cached) |
| `self:getScript(className)` ... | methods | the scripts of the same GameObject, its children or parents (4.1) |

`enabled`, `isActiveAndEnabled` and `gameObject` are reserved: a `properties` entry with one
of these names is an error (the script does not run until it is renamed).

Update methods (`onUpdate`, `onFixedUpdate`, `onPostUpdate`, input events) are called **only if
registered** with `RegisterEngineEvent`, and are skipped while the script's GameObject is
inactive or the script disabled. `RegisterEvent` (component/physics events) is not guarded.
Disabling a script does not stop what it started (tweens, sounds); deactivating its GameObject
pauses its updates. Unity names (`Awake`, `Start`, `Update`, `OnEnable`, ...) are never called:
`check_script` warns about them.

Migration (projects from before this lifecycle):

- A **disabled** script's `init` now runs (Unity's Awake): check `self.enabled` in `init` or
  remove the script to keep it fully off. A script on a GameObject that starts **inactive** gets
  its `init` when the GameObject is first activated (it used to run at load). `onDestroy` now
  also runs on scene unload and Play stop.
- Methods named `start`, `onEnable`, `onDisable`, `onApplicationPause/Focus/Quit` are now called
  by the engine; a script that called its own `start()` from `init` now runs it twice.
- `self.gameObject = ...`, `self.isActiveAndEnabled = ...` and a non-boolean `self.enabled`
  now raise an error; a `properties` entry or a field of the script's table named `enabled`,
  `isActiveAndEnabled` or `gameObject` stops the script (renamed, it runs). The script's table
  also holds these three members and the `getScript*` methods (it is per instance).
- C++: `ScriptBase` methods named `init`, `start`, `onEnable`, `onDisable`, `onDestroy`,
  `onApplicationPause/Focus/Quit` are now overrides the engine calls (twice if the script also
  calls them itself, e.g. `init()` from its constructor); one with another signature (`bool
  init()`) no longer compiles: rename it. Disabled C++ scripts are compiled into the build too
  (they can be enabled at runtime).

The editor reads `properties` by executing the file in an isolated Lua state of its own
(engine globals there are inert stand-ins, see [checks](../checks.md)), and the game runs it
again for every instance, so keep the top level free of side effects (only define the table
and functions) and the defaults plain values.

### 1.2 `properties` entries

Each entry is a table: `name` (field name on `self`, required), `displayName` (Inspector
label, defaults to `name`), `type` (string, case-insensitive; default `float` if missing),
`default` (optional).

| `type` string(s) | Lua value on `self` | `default` format |
|---|---|---|
| `"bool"`, `"boolean"` | boolean | `true` / `false` |
| `"int"`, `"integer"` | integer | `3` |
| `"float"`, `"number"` | number | `5.0` |
| `"string"` | string | `"text"` |
| `"vector2"`, `"vec2"` | `Vector2` | `{1, 2}` (array table) |
| `"vector3"`, `"vec3"` | `Vector3` | `{1, 2, 3}` |
| `"vector4"`, `"vec4"` | `Vector4` | `{1, 2, 3, 4}` (w defaults 1) |
| `"color3"` | `Vector3` | `{r, g, b}` |
| `"color4"` | `Vector4` | `{r, g, b, a}` (a defaults 1) |
| `"prefab"`, `"prefabasset"` | `PrefabAsset`, or `nil` when left None | no default (assigned in Inspector) |
| `"entity"`, `"entitypointer"`, `"pointer"` | the first Lua script instance (enabled or not) on the referenced GameObject if it has one, otherwise an `EntityHandle`; `nil` if unassigned | none |
| any other string = a wrapper class name (see list below) | an instance of that wrapper class for the referenced entity; `nil` if unassigned | none |
| any other string = a Lua script class name (e.g. `"Enemy"`) | that GameObject's `Enemy` script instance table (its `self`) | none |

Wrapper class names accepted as `type` (case-insensitive): `GameObject`, `Camera`, `Light`,
`Light2D`, `ShadowCaster2D`, `Mesh`, `Primitive`, `Mirror`, `Terrain`, `Tilemap`, `Model`,
`MeshPolygon`, `Points`, `LineRenderer`, `TrailRenderer`, `Sprite`, `AudioSource`, `RectTransform`,
`LayoutGroup`, `Polygon`, `Text`, `Image`, `Button`, `Panel`, `Scrollbar`, `Progressbar`,
`InputField`, `Bone`, `Fog`, `Skybox`, `ReflectionProbe`, `FluidSimulation`, `Rigidbody2D`, `Joint2D`,
`Rigidbody`, `Joint3D`, `Action`, `Tween`, `AlphaTween`, `ColorTween`, `PositionTween`,
`RotationTween`, `ScaleTween`, `Animation`, `ParticleSystem`, `SpriteAnimation`,
`KeyframeTracks`, `MorphTracks`, `LatticeTracks`, `RotateTracks`, `ScaleTracks`, `TranslateTracks`, `EntityHandle`.
An unknown name falls back to `EntityHandle` (with a warning in the log).

The wrapper only wraps the entity; it does not check the entity really has that component.
Pick the type that matches the referenced object (a sprite -> `"Sprite"`, a label -> `"Text"`,
a sound -> `"AudioSource"`).

### 1.3 Event registration (globals)

| Function | Meaning |
|---|---|
| `RegisterEngineEvent(self, "methodName" [, tag])` | Subscribes `self:methodName(...)` to `Engine.<methodName>`. The method name must equal the Engine event name and must already exist on `self` (error otherwise). |
| `RegisterEvent(self, eventObject, "methodName" [, tag])` | Subscribes `self:methodName(...)` to any event object (component or physics event, or an `Event`). |
| `UnregisterEngineEvent(self, "methodName" [, tag])` | Removes that `RegisterEngineEvent` subscription (same tag); `true` if it existed. Unity's `-=`. |
| `UnregisterEvent(self, eventObject, "methodName" [, tag])` | Removes that `RegisterEvent` subscription; `true` if it existed. |

Re-registering the same method (same tag) replaces the previous subscription. Subscriptions
also end when the instance is released (destroy, scene unload, Play stop). The usual Unity
pattern works: register in `onEnable`, unregister in `onDisable`. Every event object
(`Engine.onUpdate`, `button.onPress`, ...) also has `ev:add(tag, fn)`, `ev:remove(tag) -> bool`
and `ev:has(tag) -> bool`.

**`Event()`** (Unity's UnityEvent / a C# `event`): an event a script owns and invokes.

```lua
function Health:init()
    self.onDied = Event()                          -- other scripts listen to it
end
function Health:hit(n) if n >= self.hp then self.onDied:invoke(self) end end

function Hud:onEnable()
    self.health = GameObject.findObjectOfType("Health")
    self.health.onDied:addListener(self, "showGameOver")   -- owned by self: dropped when self goes
end
function Hud:onDisable() self.health.onDied:removeListener(self, "showGameOver") end
```

| Member | Meaning |
|---|---|
| `Event()` | a new event |
| `ev:addListener(fn)` / `ev:addListener(self, "method")` | adds a listener (the second form is owned by `self`: removed once that script is released); adding the same one twice is ignored |
| `ev:removeListener(fn)` / `ev:removeListener(self, "method")` | `true` if it was there |
| `ev:removeAllListeners()` | |
| `ev:invoke(...)` | calls the listeners in add order with these arguments; a listener's error is logged (with its traceback) and the others still run; listeners added during `invoke` run from the next one, removed ones are skipped at once |
| `ev.listenerCount` | ro |
| `ev:add(tag, fn)`, `ev:remove(tag)`, `ev:has(tag)` | the event object protocol: `RegisterEvent(self, ev, "method")` / `UnregisterEvent` work on it |

Engine events (names valid for `RegisterEngineEvent`; handler arguments in order):

| Event | Handler args | When |
|---|---|---|
| `onUpdate` | none | every frame (variable step; use `Engine.deltatime`) |
| `onFixedUpdate` | none | every fixed step (`Engine.updateTime`, default 1/60 s), 0..n times per frame, before physics steps |
| `onPostUpdate` | none | every frame after scenes update |
| `onDraw` | none | every frame before drawing |
| `onViewLoaded` | none | view ready |
| `onCanvasChanged` | none | view/canvas size changed (C++ name is onViewChanged; Lua name is `onCanvasChanged`) |
| `onViewDestroyed` | none | view destroyed |
| `onPause`, `onResume`, `onShutdown` | none | application lifecycle |
| `onSceneLoaded` | none | a scene loaded by SceneManager is ready |
| `onKeyDown`, `onKeyUp` | `key, isRepeat, mods` | key event (`key` is an `Input.KEY_*` value) |
| `onCharInput` | `codepoint` | text char typed |
| `onMouseDown`, `onMouseUp` | `button, x, y, mods` | mouse button (`Input.MOUSE_BUTTON_*`) |
| `onMouseMove` | `x, y, mods` | mouse moved |
| `onMouseScroll` | `xoffset, yoffset, mods` | wheel |
| `onMouseEnter`, `onMouseLeave` | none | pointer enters/leaves window |
| `onTouchStart`, `onTouchMove`, `onTouchEnd`, `onTouchCancel` | `pointer, x, y` | touch |
| `onGamepadConnect`, `onGamepadDisconnect` | `gamepad` | gamepad |
| `onGamepadButtonDown`, `onGamepadButtonUp` | `gamepad, button` | gamepad button |
| `onGamepadAxisMove` | `gamepad, axis, value` | axis -1..1 |
| `onButtonDown`, `onButtonUp` | `action` (name) | an input action's button went down / up this frame (Project Settings > Input), before `onUpdate` |

Component events (use `RegisterEvent`; this is exactly the form the editor generates):

```lua
RegisterEvent(self, Button(self.scene, self.entity):getButtonComponent().onPress, "onPress")
RegisterEvent(self, Image(self.scene, self.entity):getUIComponent().onClick, "onClick")
RegisterEvent(self, self.scene:getPhysicsSystem().beginContact2D, "onBeginContact")
```

| Source (wrapper + getter) | Event field | Handler args |
|---|---|---|
| `Button(...):getButtonComponent()` | `onPress`, `onRelease` | none |
| `Image(...):getUIComponent()` (any UI element) | `onClick`, `onDoubleClick`, `onPointerDown`, `onPointerUp`, `onPointerMove`, `onPointerEnter`, `onPointerLeave`, `onDragStart`, `onDrag`, `onDragEnd` | `x, y` |
| same | `onGetFocus`, `onLostFocus` | none |
| `Scrollbar(...):getScrollbarComponent()` | `onChange` | `step` |
| `Panel(...):getPanelComponent()` | `onMove` / `onResize` | none / `width, height` |
| `InputField(...):getInputFieldComponent()` | `onChange`, `onSubmit` | none |
| `Action(...):getActionComponent()` (any Action/Tween/Animation entity) | `onStart`, `onPause`, `onStop`, `onStep` | none |
| `AudioSource(...):getAudioSourceComponent()` | `onStart`, `onPause`, `onStop` | none |
| `PoseAnimator(...):getPoseAnimatorComponent()` | `onFinish` | none |
| `self.scene:getPhysicsSystem()` (2D) | `beginContact2D`, `endContact2D` | `bodyA, shapeA, bodyB, shapeB` (Rigidbody2D, index, Rigidbody2D, index) |
| same | `beginSensorContact2D`, `endSensorContact2D` | `sensorBody, sensorShape, visitorBody, visitorShape` |
| same | `hitContact2D` | `bodyA, shapeA, bodyB, shapeB, point (Vector2), normal (Vector2), speed` |
| same | `preSolve2D` (return false to disable contact) | `bodyA, shapeA, bodyB, shapeB, manifold (Manifold2D)` |
| same | `shouldCollide2D` (return false to skip) | `bodyA, shapeA, bodyB, shapeB` |
| `self.scene:getPhysicsSystem()` (3D) | `onContactAdded3D`, `onContactPersisted3D` | `bodyA, bodyB, contact (Contact3D)` |
| same | `onContactRemoved3D` | `bodyA, bodyB, shapeA, shapeB` |
| same | `onBodyActivated3D`, `onBodyDeactivated3D` | `body` (Rigidbody) |
| same | `shouldCollide3D` (return false to skip) | `bodyA, bodyB, baseOffset (Vector3), result (CollideShapeResult3D)` |

Physics events are scene-wide: they fire for ANY pair of bodies. Filter in the handler, e.g.
`if bodyA.entity == self.entity or bodyB.entity == self.entity then ... end`.
2D contact events need "Contact Events" (sensor events: sensor + "Sensor Events", hit: "Hit
Events") enabled on the Collider 2D (Inspector, or `body.shapeContactEvents = true`,
`body.shapeSensorEvents = true`, `body.shapeEnableHitEvents = true`).

### 1.4 Getting wrappers for your own GameObject

`self.gameObject` is the script's `GameObject`. `self.entity` is a plain integer: wrap it with a
bound constructor `(Scene, Entity)` for the other wrappers:

```lua
local go     = self.gameObject                         -- same as GameObject(self.scene, self.entity)
local sprite = Sprite(self.scene, self.entity)
local text   = Text(self.scene, self.entity)
local body2d = Rigidbody2D(self.scene, self.entity)   -- body must exist (or use go:getRigidbody2D())
local sound  = AudioSource(self.scene, self.entity)
```

Wrappers are cheap value handles; caching them in `init` (`self.go = GameObject(...)`) is fine.
Constructors with only `(Scene)` (e.g. `GameObject(self.scene)`, `Sprite(self.scene)`)
**create a new entity**.

### 1.5 Example: move with arrow keys

```lua
local Mover = {
    properties = {
        { name = "speed", displayName = "Speed", type = "float", default = 300.0 },
    }
}

function Mover:init()
    self.go = GameObject(self.scene, self.entity)
    RegisterEngineEvent(self, "onUpdate")
end

function Mover:onUpdate()
    local dt = Engine.deltatime
    local dx, dy = 0, 0
    if Input.isKeyPressed(Input.KEY_LEFT)  then dx = dx - 1 end
    if Input.isKeyPressed(Input.KEY_RIGHT) then dx = dx + 1 end
    if Input.isKeyPressed(Input.KEY_UP)    then dy = dy + 1 end
    if Input.isKeyPressed(Input.KEY_DOWN)  then dy = dy - 1 end

    local p = self.go.position                  -- Vector3 copy
    self.go.position = Vector3(p.x + dx * self.speed * dt, p.y + dy * self.speed * dt, p.z)
    -- equivalent: self.go:setPosition(x, y, z)  or  self.go:setPosition(x, y)
end

return Mover
```

The same with input actions (Project Settings > Input; keyboard, gamepad and on-screen stick
add up, smoothed like Unity's Input Manager):

```lua
function Mover:onUpdate()
    local move = Input.getVector("Horizontal", "Vertical")    -- length <= 1
    local p = self.go.position
    self.go.position = Vector3(p.x + move.x * self.speed * Engine.deltatime, p.y + move.y * self.speed * Engine.deltatime, p.z)
end
```

Edge detection (pressed this frame): `Input.getKeyDown(Input.KEY_SPACE)` (or
`Input.getKeyDown("space")`), `Input.getButtonDown("Jump")`, `Input.getMouseButtonDown(0)`;
`Input.isKeyPressed` / `getKey` are the held state. The `onKeyDown(key, isRepeat, mods)` event
works too:

```lua
function Mover:onUpdate()
    if Input.getButtonDown("Jump") then Log.print("jump") end
end
```

### 1.6 Example: 2D physics (Rigidbody2D)

```lua
local Ball = {
    properties = {
        { name = "pushForce", displayName = "Push Force", type = "float", default = 20.0 },
        { name = "jumpImpulse", displayName = "Jump Impulse", type = "float", default = 5.0 },
    }
}

function Ball:init()
    self.body = Rigidbody2D(self.scene, self.entity)  -- Rigidbody 2D set up in the editor
    self.jumpHeld = false
    RegisterEngineEvent(self, "onFixedUpdate")
    RegisterEngineEvent(self, "onUpdate")
end

function Ball:onFixedUpdate()
    -- forces: every fixed step, no deltatime factor (Newtons, Box2D meters)
    local dir = 0
    if Input.isKeyPressed(Input.KEY_RIGHT) then dir = dir + 1 end
    if Input.isKeyPressed(Input.KEY_LEFT)  then dir = dir - 1 end
    if dir ~= 0 then
        self.body:applyForceToCenter(Vector2(dir * self.pushForce, 0), true)
    end
end

function Ball:onUpdate()
    local down = Input.isKeyPressed(Input.KEY_SPACE)
    if down and not self.jumpHeld then
        self.body:applyLinearImpulseToCenter(Vector2(0, self.jumpImpulse), true)
    end
    self.jumpHeld = down

    if self.body.position.y < -500 then           -- position is in points (pixels)
        self.body.position = Vector2(0, 200)       -- teleport
        self.body.linearVelocity = Vector2(0, 0)
    end
end

return Ball
```

Mass: `self.body:getMass()` (method, kg). To get a target acceleration `a` in points/s^2:
`F = mass * a / self.body.pointsToMeterScale` (default scale 64 points per meter).

3D equivalent: `local rb = Rigidbody(self.scene, self.entity)`; in `onFixedUpdate`:
`rb:applyForce(Vector3(0, 10, 0))`; mass is a property: `rb.mass`.

---

## 2. Globals and singletons

### 2.1 Engine (all static: `Engine.x`, `Engine.fn()`)

| Name | Kind | Notes |
|---|---|---|
| `deltatime` | ro | seconds since last frame (clamped, times `timeScale`). Same value inside `onFixedUpdate`. |
| `updateTime` | rw | fixed step in seconds (default 1/60). Also `Engine.setUpdateTimeMS(ms)` |
| `timeScale` | rw | game speed (Unity's `Time.timeScale`): scales the frame delta, physics steps, actions, animations, tweens, particles. 1 = real time, 0 = frozen (frames still draw, `onUpdate` runs with delta 0). Clamped to [0, 100] |
| `fixedDeltatime` | rw | when > 0, every frame advances exactly this many seconds (before `timeScale`) whatever the wall clock measured: reproducible runs. 0 (default) = measured frame time |
| `gameTime` | ro | scaled seconds the update loop has run since start (not while paused or held) |
| `frameCount` | ro | frames the update loop has run since start (not while paused or held) |
| `interpolationAlpha` | ro | fixed-step interpolation factor: how far the frame is between the last fixed step and the next (bodies are drawn this far from their pose before the last step) |
| `framerate` | ro | fps |
| `systemTime` | ro | |
| `canvasWidth`, `canvasHeight` | ro | current canvas; can be 0 in `init()` of exported builds |
| `preferredCanvasWidth`, `preferredCanvasHeight` | ro | project canvas size; safe in `init()` |
| `setCanvasSize(w, h)` | static fn | |
| `viewRect` | ro | `Rect` |
| `scalingMode` | rw | `Scaling.*` |
| `textureStrategy` | rw | `TextureStrategy.*` |
| `useDegrees` | rw | default `true`: all engine angles are degrees |
| `scene` | rw | main scene (also `Engine.getScene()`, `Engine.setScene(s)`) |
| `getMainScene()`, `getLastScene()`, `addSceneLayer(s)`, `removeScene(s)`, `removeAllSceneLayers()`, `removeAllScenes()`, `isSceneRunning(s)`, `executeSceneOnce(s)`, `hasScenesToExecuteOnce()` | static fn | multi-scene control |
| `pauseGameEvents(bool)` | static fn | |
| `mouseCursor` | rw | `CursorType.*` |
| `mouseMode` | rw | `MouseMode.*` |
| `setMousePosition(x, y)` | static fn | |
| `callMouseInTouchEvent`, `callTouchInMouseEvent`, `allowEventsOutCanvas`, `ignoreEventsHandledByUI` | rw | input routing flags |
| `isUIEventReceived()` | static fn | |
| `platform` | ro | `Platform.*` |
| `graphicBackend` | ro | `GraphicBackend.*` |
| `openGL` | ro | bool |
| `asyncLoading` | rw | |
| `isViewLoaded()`, `getQueuedResourceCount()`, `setMaxResourceLoadingThreads(n)`, `startAsyncThread()`, `commitThreadQueue()`, `endAsyncThread()`, `isAsyncThread()` | static fn | |
| `framebuffer` | rw | |
| `clearAllSubscriptions()`, `clearComponentSubscriptions()` | static fn | do not call from gameplay scripts |
| event objects (`onUpdate`, `onFixedUpdate`, ... see 1.3) | static props | use via `RegisterEngineEvent` |

### 2.2 Input (all static)

Functions:

| Function | Returns |
|---|---|
| `Input.isKeyPressed(key)` | bool, key currently held |
| `Input.isMousePressed(button)` | bool, button held |
| `Input.isTouch()` | bool, any touch active |
| `Input.isMouseEntered()` | bool |
| `Input.getMousePosition()` | `Vector2` |
| `Input.getMouseScroll()` | `Vector2` |
| `Input.getTouchPosition(pointer)` | `Vector2` |
| `Input.numTouches()` | integer |
| `Input.findTouchIndex(pointer)` | integer |
| `Input.getTouches()` | list of `Touch`: the fingers down now (`touch.pointer` is the old name of `fingerId`) |
| `Input.isGamepadConnected(id)`, `Input.getGamepadName(id)`, `Input.isGamepadButtonPressed(id, button)`, `Input.getGamepadAxis(id, axis)`, `Input.numGamepads()`, `Input.getGamepadId(index)` | gamepad |
| `Input.getModifiers()` | bitmask of `Input.MODIFIER_*` |

Unity-style polling with edges, touches and input actions (details: `jasper://docs/input`).
`key` is an `Input.KEY_*` code or a control name (`"space"`, `"a"`, `"left_shift"`,
`"mouse_left"`, `"gamepad_a"`; Unity spellings like `"left ctrl"`, `"joystick button 0"` too):

| Function / property | Returns |
|---|---|
| `Input.getKey(key)`, `Input.getKeyDown(key)`, `Input.getKeyUp(key)` | bool: held / pressed this frame / released this frame |
| `Input.getMouseButton(b)`, `getMouseButtonDown(b)`, `getMouseButtonUp(b)` | bool; `b` 0 left, 1 right, 2 middle |
| `Input.getGamepadButtonDown(id, button)`, `getGamepadButtonUp(id, button)` | bool; `id` -1 any pad; `button` `Input.GAMEPAD_BUTTON_*` or a name |
| `Input.anyKey`, `Input.anyKeyDown` (properties) | bool: any key, mouse or gamepad button held / pressed this frame |
| `Input.inputString` (property) | UTF-8 text typed this frame (`"\b"` backspace, `"\n"` return) |
| `Input.mouseDelta`, `Input.mouseScrollDelta` (properties) | `Vector2`: movement this frame (canvas px, y down; works captured), wheel steps (y up) |
| `Input.getPressedControl()` | string or nil: the first control pressed this frame (`"space"`, `"mouse_left"`, `"gamepad_a"`, `"left_x+"`) |
| `Input.touchCount`, `Input.touches` (properties), `Input.getTouch(index)` (0-based), `Input.getTouchById(fingerId)` | Unity's touches this frame (ended/cancelled ones included in their last frame): `Touch` or nil |
| `Input.multiTouchEnabled` (rw, default true), `Input.touchSupported` | false: only the first finger is reported / `System.isTouchDevice()` |
| `Input.getAxis(name)`, `Input.getAxisRaw(name)` | number: the input action's smoothed / raw value (-1..1, button 0..1; mouse movement unclamped) |
| `Input.getButton(name)`, `getButtonDown(name)`, `getButtonUp(name)` | bool |
| `Input.getVector(xName, yName)` | `Vector2`, length <= 1 |
| `Input.hasAction(name)`, `Input.getActionNames()` | bool / string list |
| `Input.setActionEnabled(name, enabled)`, `Input.isActionEnabled(name)` | a disabled action reads 0 / false |
| `Input.simulateButton(name, pressed [, source])`, `Input.simulateAxis(name, value [, source])`, `Input.addAxisDelta(name, delta [, source])`, `Input.clearSimulated([source])` | virtual sources (like an on-screen control); `source` a name (default `"script"`) or `self` (cleared with the instance) |
| `Input.getBindings(name)` | list of binding tables (`kind`, `positive`, ...) |
| `Input.setBindings(name, bindings)`, `Input.resetBindings([name])` | `true` or `false, error` / runtime rebinding over Project Settings |
| `Input.exportBindings()`, `Input.importBindings(json)` | the overrides as JSON text / `true` or `false, error` |

Default actions (Unity's Input Manager): `Horizontal`, `Vertical`, `Fire1`, `Fire2`, `Fire3`,
`Jump`, `Mouse X`, `Mouse Y`, `Mouse ScrollWheel`, `Submit`, `Cancel`. An unknown action name logs
one error and reads 0 / false.

`Touch` (read-only fields): `fingerId`, `phase` (`TouchPhase.Began`, `Moved`, `Stationary`,
`Ended`, `Canceled`), `position`, `startPosition`, `deltaPosition` (canvas px, y down),
`deltaTime` (unscaled s), `tapCount`, `pointer` (= `fingerId`).

Constants (static properties, integers, no parentheses):

- Keys: `Input.KEY_A` .. `Input.KEY_Z`, `Input.KEY_0` .. `Input.KEY_9`, `KEY_SPACE`,
  `KEY_ENTER`, `KEY_ESCAPE`, `KEY_TAB`, `KEY_BACKSPACE`, `KEY_INSERT`, `KEY_DELETE`,
  `KEY_LEFT`, `KEY_RIGHT`, `KEY_UP`, `KEY_DOWN`, `KEY_PAGE_UP`, `KEY_PAGE_DOWN`, `KEY_HOME`,
  `KEY_END`, `KEY_CAPS_LOCK`, `KEY_SCROLL_LOCK`, `KEY_NUM_LOCK`, `KEY_PRINT_SCREEN`,
  `KEY_PAUSE`, `KEY_F1` .. `KEY_F25`, `KEY_KP_0` .. `KEY_KP_9`, `KEY_KP_DECIMAL`,
  `KEY_KP_DIVIDE`, `KEY_KP_MULTIPLY`, `KEY_KP_SUBTRACT`, `KEY_KP_ADD`, `KEY_KP_ENTER`,
  `KEY_KP_EQUAL`, `KEY_LEFT_SHIFT`, `KEY_LEFT_CONTROL`, `KEY_LEFT_ALT`, `KEY_LEFT_SUPER`,
  `KEY_RIGHT_SHIFT`, `KEY_RIGHT_CONTROL`, `KEY_RIGHT_ALT`, `KEY_RIGHT_SUPER`, `KEY_MENU`,
  `KEY_APOSTROPHE`, `KEY_COMMA`, `KEY_MINUS`, `KEY_PERIOD`, `KEY_SLASH`, `KEY_SEMICOLON`,
  `KEY_EQUAL`, `KEY_LEFT_BRACKET`, `KEY_BACKSLASH`, `KEY_RIGHT_BRACKET`, `KEY_GRAVE_ACCENT`,
  `KEY_WORLD_1`, `KEY_WORLD_2`, `KEY_UNKNOWN`, `KEY_LAST`.
  On Android the system Back button or gesture arrives as a `KEY_ESCAPE` press (released on
  the next frame); volume and other device keys stay with the system.
- Mouse: `Input.MOUSE_BUTTON_LEFT`, `MOUSE_BUTTON_RIGHT`, `MOUSE_BUTTON_MIDDLE`,
  `MOUSE_BUTTON_1` .. `MOUSE_BUTTON_8`, `MOUSE_BUTTON_LAST`.
- Modifiers: `Input.MODIFIER_SHIFT`, `MODIFIER_CONTROL`, `MODIFIER_ALT`, `MODIFIER_SUPER`,
  `MODIFIER_CAPS_LOCK`, `MODIFIER_NUM_LOCK`.
- Gamepad buttons: `Input.GAMEPAD_BUTTON_A`, `_B`, `_X`, `_Y`, `_LEFT_BUMPER`,
  `_RIGHT_BUMPER`, `_BACK`, `_START`, `_GUIDE`, `_LEFT_THUMB`, `_RIGHT_THUMB`, `_DPAD_UP`,
  `_DPAD_RIGHT`, `_DPAD_DOWN`, `_DPAD_LEFT`, `_LAST`, `_CROSS`, `_CIRCLE`, `_SQUARE`,
  `_TRIANGLE` (full names `Input.GAMEPAD_BUTTON_<X>`).
- Gamepad axes: `Input.GAMEPAD_AXIS_LEFT_X`, `_LEFT_Y`, `_RIGHT_X`, `_RIGHT_Y`,
  `_LEFT_TRIGGER`, `_RIGHT_TRIGGER`, `_LAST`.

### 2.3 Log (static, one string argument each)

`Log.print(s)`, `Log.verbose(s)`, `Log.debug(s)`, `Log.warn(s)`, `Log.error(s)`.
Pass ONE string: `Log.print("score " .. tostring(n))` or `Log.print(string.format("%d", n))`.
Plain Lua `print(...)` goes to the engine log too, as an info line (the editor's Console, in
Play and at edit time; a game's platform log in an exported build): it takes any number of
values of any type and joins them with tabs, like standard Lua. `Log.*` add the warn / error
levels.

### 2.4 PlayerPrefs / UserSettings (static, persistent key/value)

Unity's PlayerPrefs, lowerCamel: `PlayerPrefs.getInt(k [, default=0])`, `setInt(k, v)`,
`getFloat(k [, default=0.0])`, `setFloat(k, v)`, `getString(k [, default=""])`,
`setString(k, v)`, `hasKey(k)`, `deleteKey(k)`, `deleteAll()`, `save()`.

```lua
PlayerPrefs.setInt("Minigame_Money", PlayerPrefs.getInt("Minigame_Money") + 10)
if not PlayerPrefs.hasKey("OnboardingDone") then showOnboarding() end
PlayerPrefs.setString("progress", Json.encode(progress))   -- a table: through Json
PlayerPrefs.save()                                          -- after an important write
```

`UserSettings` is the same store with more types (PlayerPrefs reads what it wrote):
getters with an optional default `UserSettings.getBoolForKey(k [, def])`, `getIntegerForKey`,
`getLongForKey`, `getFloatForKey`, `getDoubleForKey`, `getStringForKey`, `getDataForKey`;
setters `setBoolForKey(k, v)`, `setIntegerForKey`, `setLongForKey`, `setFloatForKey`,
`setDoubleForKey`, `setStringForKey`, `setDataForKey`; `hasKey(k)`, `deleteKey(k)` (=
`removeKey(k)`), `deleteAll()`, `save()`.

- Read a key with the type you wrote it with (as in Unity): another type gives the default
  (Android keeps the types, desktop and web store text). Getters never raise: a stored value
  that does not read as the type asked for gives the default, with a warning once per key.
- `hasKey` is true for a stored empty string, and `getString` returns that `""`.
- `setInt` takes an integer within 32 bits (`6 / 2` is fine, `3.5` raises "integer expected";
  larger: `UserSettings.setLongForKey`); `setString` takes a string (`Json.encode` tables).
- Numbers are written the same in every locale (`0.5`, never `0,5`).
- `deleteKey` of a missing key does nothing; `deleteAll` removes every key of this game (on
  iOS/macOS the keys the game wrote or read back, never other SDKs' entries such as ad consent).
- Keys are non-empty strings without zero bytes (`PlayerPrefs` raises, `UserSettings` logs and
  ignores them); values may hold any bytes.

When values reach the disk (call `save()` after an important write; a crash loses at most
the last second on desktop):

| Platform | Store | Written |
|---|---|---|
| Editor (Play) | `<project>/.jasperengine/user/rundata/UserSettings.xml` (Edit > Clear User Settings (PlayerPrefs)..., MCP `user_settings`) | a second after a change, on `save()`, on Play stop |
| Desktop exports | `UserSettings.xml` in the user data folder (`data://`, below) | a second after a change, on `save()`, app pause, quit |
| Android | SharedPreferences `UserSettings` | at once in the background; `save()` waits for it |
| iOS / macOS app | NSUserDefaults | by the system; `save()` synchronizes |
| Web | localStorage, keys `<identifier>/<key>` | at once |

### 2.4.1 Json (static)

`Json.encode(value [, opts])` -> string, `Json.decode(text [, opts])` -> value; both raise
on errors with the place (`Json.encode: cannot encode a function at $.player.onHit`,
`Json.decode: syntax error at line 3, column 14: unexpected '}'; expected string literal`).
`Json.tryEncode` / `Json.tryDecode` return `nil, message` instead (a valid `null` gives
`Json.null`, so `nil` always means failure). `Json.readFile(path [, opts])` and
`Json.writeFile(path, value [, opts])` are below (2.4.2).

```lua
local t = Json.decode(response)           -- JsonConvert.DeserializeObject
local ts = t.utc_timestamp
local body = Json.encode({playerID = id}) -- JsonConvert.SerializeObject
local ok, err = Json.tryDecode(text)       -- untrusted text: no raise
Log.print(Json.encode(state, {pretty = true}))
```

| Lua | JSON |
|---|---|
| table with keys exactly `1..n` (n >= 1) | array |
| empty table | `[]` (`{}` with `Json.object(t)` or `{emptyTable = "object"}`) |
| any other table | object (integer keys as `"1"`; other key types are an error) |
| `Json.array(t)` / `Json.object(t)` (marks `t`, returns it) | always array / object (a marked array with holes is an error) |
| integer / float | integer / number with a decimal point (`1` / `1.0`); NaN and infinities are an error |
| string | string (must be UTF-8; `invalidUtf8 = "replace"` or `"ignore"`) |
| `Json.null` (`Json.isNull(v)`) | `null` |
| `Vector2/3/4`, `Quaternion` | `{"x":..,"y":..[,"z"][,"w"]}` |
| a value whose metatable has `__tojson(self)` | what it returns |
| function, thread, other userdata, a cycle, nesting deeper than 512 | error with the path |

Decoding: objects and arrays are tables (empty ones marked, so `Json.encode(Json.decode(s))`
keeps `[]` and `{}`), integers are Lua integers (`1.0`, `1e2`, and integers beyond int64 are
floats), `null` is `Json.null` (`{null = "nil"}`: omitted from objects, holes in arrays).
Object keys are written sorted. Options: encode `pretty` (indent 2), `indent` (0..64),
`invalidUtf8` (`"error"`/`"replace"`/`"ignore"`), `emptyTable` (`"array"`/`"object"`);
decode `null` (`"sentinel"`/`"nil"`), `allowComments`. Unknown option names raise (typos).

### 2.4.2 Files and saves (File.*, data://)

| Function | Returns |
|---|---|
| `File.readText(path)` | the whole file (bytes as they are), or `nil, message` |
| `File.writeText(path, text)` | `true`, or `nil, message`; atomic (a crash leaves the old or the new file), makes missing folders |
| `File.appendText(path, text)` | `true`, or `nil, message` (not atomic; makes the file) |
| `File.exists(path)` | boolean (files and folders) |
| `File.remove(path)` | `true` (also when nothing was there), or `nil, message`; files only |
| `File.makeDirs(path)` | `true`, or `nil, message` |
| `File.list(dir)` | sorted names of the files and folders of a `data://` folder, or `nil, message` |
| `Json.readFile(path [, opts])` | the decoded value, or `nil, "not found: data://save.json"` / `nil, "data://save.json: syntax error at ..."` |
| `Json.writeFile(path, value [, opts])` | `true`, or `nil, message` (encode error or write error) |

Paths: `data://x` is the user data folder (Unity's `Application.persistentDataPath`), the
only writable one; `asset://x` (the assets folder; Unity `Resources`/`StreamingAssets`) and
`lua://x` (the Lua folder) are read-only and work inside an Android APK and a packed export;
a bare relative path means `data://`; `..` segments and unknown `scheme://` are refused;
absolute paths are read on desktop and in the editor only. Wrong argument types raise;
file problems return `nil, message` ("not found: ...", "permission denied: ...",
"read-only path: asset://...").

```lua
-- save / load a table
local ok, err = Json.writeFile("data://garden.json", {coins = 120, plots = plots})
if not ok then Log.warn("save failed: " .. err) end
local state = Json.readFile("data://garden.json") or {coins = 0, plots = {}}
```

Where `data://` is: the editor's Play mode `<project>/.jasperengine/user/rundata`; desktop
exports a per-user folder named after Project Settings `application.identifier` (the project
name when it is the default): Linux `$XDG_DATA_HOME/<id>` (`~/.local/share/<id>`), Windows
`%APPDATA%\<id>`, macOS `~/Library/Application Support/<id>` (older builds saved in the
working directory: `UserSettings.xml` is copied over once; other files they wrote there are
not); Android the app's internal files; iOS Documents; Web `/datafs` (IndexedDB).

Web: files persist in IndexedDB. `File.writeText`, `Json.writeFile` and `File.remove` store
within half a second (and when the tab is hidden); `System.syncFileSystem()` stores now.
The first scene starts once the saved files are restored (`System.isUserDataLoaded()`; at
most 3 s), so `init` reads them; if the restore takes longer, `data://` writes return
`nil, "the saved files are not restored yet ..."` until it is done (the restore would replace
them). Plain Lua `io` works with `FileData.getSystemPath("data://x")`
paths, but on Web call `System.syncFileSystem()` after writing.

Data assets (Unity ScriptableObjects): a JSON file with the Lua scripts or the assets
(`Json.readFile("lua://data/items.json")`, `Json.readFile("asset://levels/1.json",
{allowComments = true})`), exported with them; or a Lua data module shared by every
script that requires it:

```lua
-- lua/data/items.lua
return { sword = { damage = 12, price = 30 }, shield = { armor = 5, price = 25 } }
-- in a script
local Items = require("data.items")
local price = Items.sword.price
```

### 2.5 System (static)

`System.getScreenWidth()`, `getScreenHeight()`, `isTouchDevice()`, `isFullscreen()`,
`requestFullscreen()`, `exitFullscreen()`, `isWindowMaximized()`, `maximizeWindow()`,
`restoreWindow()`, `setWindowSize(w, h)`, `isWindowResizable()`, `setWindowResizable(b)`,
`setWindowTitle(s)`, `quit()`, `showVirtualKeyboard(text)`, `hideVirtualKeyboard()`,
`setClipboardText(s)`, `getClipboardText()` (the system clipboard, UTF-8; on Web the text of
the last copy or paste), `getDirSeparator()`, `getAssetPath()`, `getUserDataPath()`, `getLuaPath()`,
`getShaderPath()`, `syncFileSystem()` (Web: stores the `data://` files in IndexedDB now; true),
`isUserDataLoaded()` (Web: the saved files are restored; elsewhere true); ads/web: `initializeAdMob()`, `setMaxAdContentRating(AdMobRating.*)`,
`loadInterstitialAd(id)`, `isInterstitialAdLoaded()`, `showInterstitialAd()` (Android/iOS; the
export compiles Google Mobile Ads in only with the project settings `android.admob_enabled` /
`ios.admob_enabled` and their `admob_app_id`, otherwise these do nothing),
`initializeCrazyGamesSDK()`, `showCrazyGamesAd(type)`, `happytimeCrazyGames()`,
`gameplayStartCrazyGames()`, `gameplayStopCrazyGames()`, `loadingStartCrazyGames()`,
`loadingStopCrazyGames()`.

Files (for saves prefer `File.readText` / `File.writeText` / `Json.readFile`, 2.4.2):
`File()` / `File(path, write)` with `:open`, `:flush`, `:close`, `:readString()`,
`:writeString(s)`, `:read8/16/32()`, `:eof()`, `:length()`, `:seek(n)`, `:pos()`;
`Data(path)` reads asset data; `FileData.getBaseDir(p)`, `FileData.getFilePathExtension(p)`,
`FileData.getSystemPath(p)`. Constants `FileErrors.FILEDATA_OK`, `INVALID_PARAMETER`,
`FILE_NOT_FOUND`, `OUT_OF_MEMORY`.

`require("folder.module")` loads `folder/module.lua` from the Lua directory (or its `lua/`
subfolder). Use it for shared helper modules.

### 2.6 GameObject.instantiate / destroy (static, Unity-style)

```lua
GameObject.instantiate(original)                              -- -> GameObject or nil
GameObject.instantiate(original, parent [, worldSpace])       -- parent: GameObject or a script self
GameObject.instantiate(original, position, rotation [, parent]) -- Vector3, Quaternion (world)
GameObject.instantiate(prefab, scene [, position, rotation])  -- prefab into a given scene
GameObject.destroy(obj [, delaySeconds])                      -- deferred to end of frame
GameObject.destroyImmediate(obj)
```

- `original`: a `PrefabAsset`, a prefab path/name string, a prefab id number, a GameObject
  (any wrapper), or a script instance (`self` of some script).
- A rotation is REQUIRED with a position: use `Quaternion.IDENTITY` or `Quaternion(0, 0, deg)`.
- Returns a `GameObject` wrapper of the new root. If `original` was a script instance, returns
  the copy's script instance of the same class instead.
- Prefabs go to the active scene unless a parent or scene is given.
- `obj` for destroy: any wrapper or a script `self` (destroys its GameObject). `nil` is ignored.

```lua
local b = GameObject.instantiate(self.bullet, Vector3(x, y, 0), Quaternion.IDENTITY)
if b then GameObject.destroy(b, 2.0) end
```

Finding objects (Unity's `GameObject.Find`, `FindWithTag`, `FindObjectOfType`): they search
**every running scene** (the main scene, then its layers bottom to top; ascending entity ids
within a scene; while a scene starts, its own objects too, so `init` finds them). They are
linear scans: call them in `init`/`start` and keep the result, not every frame.

```lua
GameObject.find("Player")                     -- first ACTIVE GameObject named so, or nil
GameObject.find("Canvas/Panel/Score")          -- a Score below Panel below Canvas
GameObject.find("/Canvas/Panel/Score")         -- ... with Canvas a root
GameObject.findWithTag("Player")               -- first active GameObject with the tag, or nil
GameObject.findGameObjectsWithTag("Enemy")     -- list (possibly empty)
GameObject.findObjectOfType("ScoreManager")    -- a script instance (active and enabled), or nil
GameObject.findObjectsOfType("Enemy", true)    -- every Enemy script, also inactive / disabled
GameObject.findObjectOfType("Camera")          -- a wrapper name: a Camera for a GameObject with a camera
```

`className` is a Lua script class (the file name) or a wrapper class name of 1.2 that has a
component (`"Camera"`, `"AudioSource"`, `"Rigidbody"`, `"Sprite"`, ...; `"GameObject"` is an
error). Without `includeInactive`, only active GameObjects (and, for scripts, enabled ones)
count. A tag the project does not define logs "Tag: X is not defined." and finds nothing.

### 2.7 PrefabAsset / PrefabManager

| Name | Kind |
|---|---|
| `PrefabAsset.load(nameOrPathOrGuid)` / `PrefabAsset.load(id)` | static fn, e.g. `PrefabAsset.load("enemies/Enemy")` (".prefab" optional) |
| `PrefabAsset()` | empty constructor |
| `id`, `valid`, `guid`, `path`, `name` | ro properties |
| `isValid()` | method |
| `==`, `tostring()` | metamethods |
| `PrefabManager.findPrefab(...)`, `getPrefabId(...)`, `getPrefabName(...)`, `getPrefabGuid(...)`, `getPrefabNames()`, `PrefabManager.prefabCount` | static |

A `"Prefab"` script property left at None is `nil`: check before instantiating.

### 2.8 SceneManager (static)

`SceneManager.loadScene(idOrName)`, `addChildScene(idOrName)`, `removeChildScene(idOrName)`,
`preloadScene(idOrName)`, `getPreloadProgress(idOrName)`, `cancelPreload(idOrName)`,
`setLoadingScene(idOrName)`, `holdLoading()`, `releaseLoading()`, `isCoveredByLoading(...)`,
`getSceneId(name)`, `getSceneName(id)`, `getSceneNames()`, `getScenePtr(id)`,
`clearAll()`. Properties (ro unless noted): `sceneCount`, `currentSceneId`,
`currentSceneName`, `loadPending`, `loadingSceneId`, `loading`, `loadingProgress`,
`loadingDelay` (rw), `loadingTimeout` (rw).

### 2.9 Scene (`self.scene`) - methods from EntityRegistry and Scene

Entities are plain integers (`0` = none).

| Method / property | Notes |
|---|---|
| `findEntity(name)` / `findEntity(name, parent)` | -> Entity (0 if not found) |
| `findByTag(tag)` / `findEntityByTag(tag)` | -> Entity |
| `findAllByTag(tag)` / `findEntitiesByTag(tag)` | -> table of Entity |
| `getEntityName(e)`, `setEntityName(e, name)` | |
| `isEntityCreated(e)` | |
| `setEntityActive(e, b)`, `isEntityActive(e)`, `isEntityActiveInHierarchy(e)` | |
| `setEntityTag(e, tag)` (tag must be defined in project), `getEntityTag(e)` | |
| `setEntityLayer(e, n)`, `getEntityLayer(e)` | |
| `addEntityChild(parent, child, changeTransform)` | |
| `getEntityList()`, `getLastEntity()`, `isParentOf(a, b)`, `findOldestParent(e)` | |
| `createEntity()`, `destroyEntity(e)` | low level; prefer `GameObject(self.scene)` / `GameObject.destroy` |
| `camera` | rw property, Entity of the active camera; `setCamera(cameraWrapperOrEntity)` |
| `backgroundColor` (Vector4, rw); `setBackgroundColor(r,g,b[,a])` | |
| `gravity2D` (Vector2, rw); `setGravity2D(x, y)` | |
| `gravity3D` (Vector3, rw); `setGravity3D(x, y, z)` | |
| `physicsInterpolation` (bool, rw) | bodies drawn between their last two fixed steps (default true); the scene setting `physics_interpolation` |
| `physics2DSubSteps` (integer, rw) | Box2D sub-steps of each fixed step, 1-16 (default 4); the scene setting `physics_2d_sub_steps` |
| `ambientLight2DColor`, `ambientLight2DIntensity`, `globalIlluminationColor`, `globalIlluminationIntensity`, `maxLightsPerObject` (1-16; the other lights reaching an object go into its ambient), `shadowQuality`, `lightState`, `shadow2DQuality`, `sceneColorScale` (`SceneColorScale`: resolution of the scene-colour copy that shaders requiring scenecolor sample; `HALF` default), `postProcessing` (a `PostProcessingSettings` copy: edit its fields, then assign it back, `local p = scene.postProcessing; p.tonemap = TonemapMode.ACES; p.fxaaEnabled = true; scene.postProcessing = p`; enums `TonemapMode` NONE/ACES/NEUTRAL/REINHARD/AGX, `MonochromeMode` GRAYSCALE/SEPIA; fields as `get_scene_settings` `post_processing.<field>` in camelCase; `scene.postProcessing = PostProcessingSettings()` turns everything off; costs and presets: `jasper://docs/post-processing`), `postProcessPasses` (a list of `PostProcessPass`: `shader`, `enabled`, `scale` 0.1-1, `setUniform`/`getUniform`, `setTexture(uniform, path)`/`getTexture`; assign the list back; `jasper://docs/post-processing`), `enableUIEvents` | rw properties (partial list; also ssao*/ssr*/fixedResolution*/postProcess* rendering options) |
| `getPhysicsSystem()`, `getAudioSystem()`, `getActionSystem()`, `getMeshSystem()`, `getRenderSystem()`, `getUISystem()` | subsystem objects |

`PhysicsSystem` (from `self.scene:getPhysicsSystem()`): `gravity` / `gravity2D` /
`gravity3D` (rw), `interpolation` (rw, as `scene.physicsInterpolation`), `pointsToMeterScale2D` (rw),
`subSteps2D` (rw, 1-16, as `scene.physics2DSubSteps`), event objects (section 1.3),
`createRigidbody2D(e)`, `removeRigidbody2D(e)`, `createRigidbody(e)`, `removeRigidbody(e)`
(partial).

`AudioSystem` (static): `AudioSystem.stopAll()`, `pauseAll()`, `resumeAll()`,
`AudioSystem.globalVolume` (rw).

### 2.10 Physics / Physics2D / LayerMask (static)

| Name | Notes |
|---|---|
| `LayerMask.nameToLayer(name)`, `LayerMask.layerToName(n)` | layers 0..31 from Project Settings |
| `LayerMask.getMask("A", "B")` or `LayerMask.getMask({"A", "B"})` | -> integer mask |
| `Physics2D.raycast(scene, origin, direction, maxDistance [, mask])` | Vector2 origin/dir -> `RayReturn` |
| `Physics2D.raycast(scene, ray [, mask])` | `Ray` (Vector3) form |
| `Physics2D.raycastAll(...)` | same args -> table of `RayReturn` |
| `Physics.raycast(scene, origin, direction, maxDistance [, mask])`, `Physics.raycastAll(...)` | 3D, Vector3 origin/dir |
| `ignoreLayerCollision(l1, l2 [, ignore=true])`, `getIgnoreLayerCollision(l1, l2)`, `getLayerCollisionMask(l)`, `setLayerCollisionMask(l, mask)`, `resetLayerCollisionMatrix()` | on both `Physics` and `Physics2D` |
| `ALL_LAYERS`, `DEFAULT_RAYCAST_LAYERS`, `IGNORE_RAYCAST_LAYER` | static props on both |

`RayReturn` (ro): `hit` (bool), `distance`, `point` (Vector3), `normal` (Vector3), `body`
(Entity), `colliderIndex`.

```lua
local hit = Physics2D.raycast(self.scene, Vector2(x, y), Vector2(0, -1), 100)
if hit.hit then local e = hit.body end
```

### 2.10.1 Water (static)

The water surfaces of a scene are its meshes drawn with `library:water` (enabled, active), each
resting at the top of its world bounds and covering their XZ extent; with `WAVES` the heights
follow the same waves the shader draws, on its clock (`Water.getTime()`, `frame.time.x`:
seconds since the engine started, wall-clock, running in the editor and while the game is
paused or time-scaled), so a script and the screen agree. Where waters overlap the one whose
bottom (the material's `buoyancyDepth` below its surface; none at 0) is highest wins. Each call
looks through the scene's meshes: for many points a frame, keep the results.

| Name | Notes |
|---|---|
| `Water.getHeight(scene, x, z)` | surface height (world Y, waves included) of the topmost water over (x, z); `nil` where none covers it |
| `Water.getNormal(scene, x, z)` | its normal (`Vector3`; up without `WAVES`); `nil` where none |
| `Water.isUnderwater(scene, point)` | `point` (`Vector3`) is under the surface of the water it is in |
| `Water.getTime()` | the shader library's clock, seconds |

```lua
local y = Water.getHeight(self.scene, pos.x, pos.z)
if y and pos.y < y then -- swimming
end
```

Buoyancy needs no script: a dynamic `Rigidbody` with `buoyancy > 0` floats (4.14). The
`library:underwater` post-process pass switches itself on under a surface (shaders guide,
"Water").

### 2.10.2 Shader (static): global shader values

Unity's `Shader.SetGlobal*` / `Shader.PropertyToID`: values every shader declaring them reads
(a member of its `u_vs_globals` / `u_fs_globals` block, or a texture uniform of the name), and
library shader parameters bound to them follow (the shaders guide, "Global values").
Project Settings > Graphics > Shader Globals declares them with their defaults; Play starts and
stops with those. `name` is the global's name or the id `propertyToID` gave. Setters return
`false` (and log once) when the global is of another kind; wrong argument types raise an error.

| Name | Notes |
|---|---|
| `Shader.propertyToID(name)` | a stable integer for this run |
| `Shader.setGlobalFloat(name, n)`, `setGlobalInt(name, i)` | an int is stored as a float (Unity's SetGlobalInt) |
| `Shader.setGlobalVector(name, Vector4 \| Vector3 \| Vector2)` | missing components 0 |
| `Shader.setGlobalColor(name, Vector4 \| Vector3)` | sRGB in, stored linear (alpha as is) |
| `Shader.setGlobalMatrix(name, Matrix4)` | only a `matrix4` global |
| `Shader.setGlobalFloatArray(name, {n...})`, `setGlobalVectorArray(name, {Vector4...})` | 1..64; a first set fixes an undeclared array's size |
| `Shader.setGlobalTexture(name, Texture \| "path" \| Framebuffer)` | `nil`: the default texture again |
| `Shader.getGlobalFloat/Int/Vector/Color/Matrix/FloatArray/VectorArray/Texture(name)` | `getGlobalColor` is sRGB; `nil` when unknown or of another kind |
| `Shader.hasGlobal(name)`, `Shader.getGlobalNames()` | declared first, then the ones scripts made |
| `Shader.resetGlobals()` | back to the defaults; the names scripts made are dropped |

```lua
local tint = Shader.propertyToID("FogTint")
function Fog:onUpdate()
    Shader.setGlobalColor(tint, Vector4(1, 0.5 + 0.5 * math.sin(Engine.gameTime), 0, 1))
end
```

Values outlive scene loads (like Unity). A material or component parameter can follow one:
`setShaderGlobalBinding(member, global)` (4.3).

### 2.11 Angle (static)

`Angle.degToRad(d)`, `Angle.radToDeg(r)`, `Angle.defaultToRad(a)`, `Angle.defaultToDeg(a)`,
`Angle.radToDefault(r)`, `Angle.degToDefault(d)` ("default" = degrees unless
`Engine.useDegrees = false`). Lua's `math.sin/cos` take radians.

### 2.12 Color (static)

`Color.linearTosRGB(...)`, `Color.sRGBToLinear(...)`: accept `Vector3`, `Vector4`, `(r,g,b)`
or `(r,g,b,a)`.

No timer API is bound: accumulate `Engine.deltatime` in `self`, or use
`GameObject.destroy(obj, delay)` for delayed destruction, or Tweens/Actions.

---

## 3. Math

### 3.1 Vector2 / Vector3 / Vector4

| | Vector2 | Vector3 | Vector4 |
|---|---|---|---|
| Constructors | `()`, `(x, y)`, `(Vector2)`, `(Vector3)`, `(Vector4)` | `()`, `(x, y, z)`, `(Vector2, z)`, `(Vector3)`, `(Vector4)` | `()`, `(x, y, z, w)`, `(Vector2, z, w)`, `(Vector3, w)`, `(Vector4)` |
| Fields (rw) | `x y` | `x y z` | `x y z w` |
| Constants | `ZERO UNIT_X UNIT_Y NEGATIVE_UNIT_X NEGATIVE_UNIT_Y UNIT_SCALE` | `ZERO UNIT_X UNIT_Y UNIT_Z UNIT_SCALE` | `ZERO UNIT_X UNIT_Y UNIT_Z UNIT_W UNIT_SCALE` |
| `+ -` (vector) | yes | yes | yes |
| `*` | `v * number`, `number * v`, `v * v` | `v * number`, `number * v`, `v * v` | `v * number`, `number * v`, `v * v` |
| `/` | `v / number`, `number / v`, `v / v` | `v / number`, `number / v`, `v / v` | `v / number`, `number / v`, `v / v` |
| unary `-`, `==`, `<`, `<=`, `>`, `>=`, `tostring` | yes | yes | yes |
| Methods | `length() squaredLength() distance(v) squaredDistance(v) dotProduct(v) normalize() normalized() normalizeL() midPoint(v) makeFloor(v) makeCeil(v) perpendicular() crossProduct(v) normalizedCopy() reflect(n) swap(v) isValid()` | `length() squaredLength() dotProduct(v) absDotProduct(v) distance(v) squaredDistance(v) normalize() normalized() normalizeL() crossProduct(v) midPoint(v) makeFloor(v) makeCeil(v) perpendicular() reflect(n) isValid()` | `dotProduct(v) divideByW() swap(v) isNaN() isValid()` |

Constants are static properties returning copies: `Vector3.ZERO` (no parentheses).
`v * v`, `v / v` and `number / v` are component-wise. `<` is true when every component is
smaller, `<=` when every component is smaller or equal (so `a < b` and `b < a` can both be
false). `==` with a value of another class is `false`. There is no `v + number`.

### 3.2 Quaternion

| Item | Notes |
|---|---|
| `Quaternion()` | identity (w=1); same as `Quaternion.IDENTITY` |
| `Quaternion(w, x, y, z)` | raw components (w first) |
| `Quaternion(xDeg, yDeg, zDeg)` | Euler angles in default units (degrees), ZYX order |
| `Quaternion(x, y, z, RotationOrder.XYZ)` | Euler with explicit order |
| `Quaternion(angle, axisVector3)` | angle-axis |
| `Quaternion(Matrix3)`, `Quaternion(Matrix4)`, `Quaternion(xAxis, yAxis, zAxis)` | |
| `Quaternion.IDENTITY` | static |
| fields `w x y z` (rw); `* (Quaternion)`, `* (Vector3)` rotates a vector, `* number` / `number *` scales; `+ - unary- == tostring` | |
| methods | `fromEulerAngles(x, y, z, order)`, `getEulerAngles(order)`, `fromAngle(a)` (Z axis), `fromAngleAxis(a, axis)`, `fromAxes(...)`, `fromRotationMatrix(m)`, `getRotationMatrix()`, `xAxis() yAxis() zAxis()`, `dot(q) norm() inverse() unitInverse() exp() log()`, `normalize() normalized() normalizeL()`, `getRoll() getPitch() getYaw()` (default units) |
| static | `Quaternion.slerp(t, a, b [, shortestPath])`, `Quaternion.nlerp(t, a, b [, shortestPath])`, `Quaternion.squad(...)`, `Quaternion.slerpExtraSpins(...)`, `Quaternion.lookRotation(forward [, up])` |

`RotationOrder.XYZ`, `XZY`, `YXZ`, `YZX`, `ZXY`, `ZYX`.

### 3.3 Other math types (partial)

| Class | Key members |
|---|---|
| `Rect(x, y, w, h)` | rw `x y width height`; `contains(...)`, `setRect(...)`, `fitOnRect(r)`, `getVector()`, `isZero()`, `isNormalized()` |
| `Ray(origin, direction)` (Vector3) | rw `origin direction`; `getPoint(t)`; `intersects(AABB/OBB/Plane/Rigidbody2D/Rigidbody/...)`; `Ray.NO_HIT` |
| `Matrix3`, `Matrix4` | `+ - == tostring`; `*` by a matrix, a number (either side) or a vector (`m * Vector3`, Matrix4 also `m * Vector4`; `Matrix4 * Vector3` applies the translation); `row column get set identity inverse transpose`; Matrix4 static `translateMatrix rotateMatrix rotateXMatrix rotateYMatrix rotateZMatrix scaleMatrix lookAtMatrix orthoMatrix perspectiveMatrix frustumMatrix` |
| `Plane`, `AABB`, `OBB`, `Sphere` | geometry helpers (`intersects`, `contains`, `getCenter`, `getSize`, `merge`, ...) |
| `AnimationCurve()` | Unity's curve. Static `constant(v)`, `linear(t0, v0, t1, v1)`, `easeInOut(t0, v0, t1, v1)`; `evaluate(t)`; `addKey(time, value [, inTangent, outTangent])` / `addKey(CurveKey)` -> 1-based index (-1 when a key is at that time); `removeKey(i)`; `moveKey(i, time, value)` / `moveKey(i, CurveKey)` -> its new index; `getKey(i)` -> `CurveKey` (nil out of range); ro `keyCount`; rw `keys` (table of `CurveKey`), `preWrapMode`, `postWrapMode` (`CurveWrap.*`) |
| `CurveKey(time, value [, inTangent, outTangent])` | rw `time value inTangent outTangent` (slopes; `math.huge` is a step) |
| `Gradient()` | Unity's gradient (white, opaque). `setKeys(colorKeys, alphaKeys)`: `{{time, Vector3 sRGB}, ...}` (or `GradientColorKey`s), `{{time, alpha}, ...}`; `evaluate(t)` -> Vector4 **linear**; rw `mode` (`GradientMode.*`), `colorKeys`, `alphaKeys`; ro `colorKeyCount`, `alphaKeyCount`; `getColorKey(i)`, `getAlphaKey(i)` (1-based) |
| `GradientColorKey(time, colorV3)`, `GradientAlphaKey(time, alpha)` | rw `time`, `color` (**sRGB**: given and read as pickers show it, stored linear) / `alpha` |

---

## 4. Object wrappers

Class hierarchy (methods/properties are inherited):

```
EntityHandle
 |- GameObject
 |   |- Camera, Light, Light2D, ShadowCaster2D, ReflectionProbe, Bone, Points, LineRenderer, Lattice,
 |   |  FluidSimulation, TrailRenderer
 |   |- Mesh
 |   |   |- Primitive -> Mirror
 |   |   |- Sprite, Tilemap, Model, MeshPolygon, Terrain
 |   |- RectTransform
 |       |- Text, Polygon, LayoutGroup
 |       |- Image -> Button, Panel, Scrollbar, Progressbar, InputField
 |- AudioSource, PoseAnimator, AnimationPlayer, VertexAnimator, Rigidbody2D, Joint2D, Rigidbody, Fog, Skybox
 |- Action -> Tween (AlphaTween, ColorTween, PositionTween, RotationTween, ScaleTween),
             ParticleSystem, SpriteAnimation, Animation,
             KeyframeTracks (MorphTracks, LatticeTracks, RotateTracks, ScaleTracks, TranslateTracks)
Joint3D, AnimationState, AnimationEvent, VertexAnimatorSocketPose, PhysicsMaterial (standalone classes)
```

Constructors: every wrapper has `(Scene, Entity)` to wrap an existing entity. Most also have
`(Scene)` which creates a new entity with that component. Exceptions: `Rigidbody2D`,
`Rigidbody`, `Bone`, `PoseAnimator`, `AnimationPlayer` and `VertexAnimator` have only `(Scene, Entity)`; `AudioSource` also has `(Scene, is3D)`.

A wrapper made with `(Scene)` owns its new entity (`entityOwned` is true): when the garbage
collector takes it, the entity is destroyed. Keep it (e.g. in `self`) as long as the object
should exist, or set `entityOwned = false` to leave the entity to the scene. A wrapper collected
after its scene ended (play stop, scene change) does nothing: the scene destroyed the entity.

### 4.1 EntityHandle (base of everything)

| Member | Kind |
|---|---|
| `scene` | ro, Scene |
| `entity` | ro, integer |
| `name` | rw |
| `tag` | rw (tag must exist in Project Settings) |
| `layer` | rw integer |
| `activeSelf`, `activeInHierarchy` | ro |
| `entityOwned` | rw |
| `setActive(bool)`, `isActive()`, `isActiveInHierarchy()`, `compareTag(tag)` | methods |
| `getScript(className)` | method: the Lua script instance of that class on this GameObject (enabled or not; Unity's `GetComponent<Enemy>()`), or nil |
| `getScripts([className])` | method: a list of its script instances (all, or of that class), component order |
| `getScriptInChildren(className [, includeInactive])` | method: this GameObject first, then its descendants (pre-order); inactive GameObjects only with `includeInactive` |
| `getScriptsInChildren(className [, includeInactive])` | method: list |
| `getScriptInParent(className [, includeInactive])` | method: this GameObject, then its parents upward |

Script instances (`self`) have the same five methods for their own GameObject:
`self:getScript("Weapon")`. Lua scripts only (C++ scripts are not reachable from Lua); a
non-string `className` errors. Annotate the result for LuaLS: `local enemy = go:getScript("Enemy") ---@type Enemy`.

### 4.2 GameObject

| Member | Kind | Notes |
|---|---|---|
| `position` | rw `Vector3` | local position |
| `setPosition(x, y, z)`, `setPosition(x, y)` | method | |
| `getWorldPosition()` | method | Vector3 |
| `rotation` | rw `Quaternion` | |
| `setRotation(xDeg, yDeg, zDeg)` | method | Euler, default units (degrees); 2D: `setRotation(0, 0, angle)` |
| `getWorldRotation()` | method | Quaternion |
| `scale` | rw `Vector3` | |
| `setScale(s)` | method | uniform float only; for non-uniform assign `obj.scale = Vector3(...)` |
| `getWorldScale()` | method | |
| `visible` | rw bool | setting it copies the value to the children (next frame); `setVisibleOnly(b)` changes only this one. A parent hidden in the editor does not hide its children until a script hides it; to hide a whole subtree reliably use `setActive(false)` |
| `addChild(goOrEntity)`, `removeChild(goOrEntity)`, `removeParent()` | methods | |
| `moveToTop()`, `moveUp()`, `moveDown()`, `moveToBottom()` | methods | sibling order = 2D draw order (below) |
| `billboard`, `fakeBillboard`, `cylindricalBillboard` (rw), `billboardRotation` (rw), `setBillboard(a,b,c)`, `setBillboardRotation(x,y,z)` | | |
| `localMatrix` (rw), `modelMatrix` (ro), `normalMatrix` (ro), `updateTransform()` | | |
| `getRigidbody2D()` | method | returns `Rigidbody2D`, CREATING the component if missing |
| `removeRigidbody2D()` | method | |
| `getJoint2D()` | method | the GameObject's `Joint2D`, `nil` when it has none |
| `getRigidbody()` / `removeRigidbody()` | method | 3D, same semantics |
| `getRay(direction)` | method | Ray from world position |
| `getAnimationPlayer()` | method | the GameObject's `AnimationPlayer` (4.8.2), `nil` when it has none (it never adds one) |
| `hasAnimationPlayer()` | method | bool |
| `getVertexAnimator()` | method | the GameObject's `VertexAnimator` (4.8.3), `nil` when it has none (it never adds one) |
| `hasVertexAnimator()` | method | bool |
| `GameObject.instantiate`, `GameObject.destroy`, `GameObject.destroyImmediate` | static | section 2.6 |

There is no `getComponent`: wrap the same entity with the class you need
(`Sprite(self.scene, self.entity)`).

**2D draw order.** A 2D or UI scene's camera draws without a depth test, in Hierarchy order: a
parent before its children, and among siblings the later one on top (a GameObject created at
runtime goes last, so on top). Position Z does not change the order (it only has to stay inside
the camera's near/far range, -10..10 by default); there is no sorting layer, and `layer` only
selects the cameras that render a GameObject. Reorder with `moveToTop()`/`moveUp()`/`moveDown()`/
`moveToBottom()` (among siblings) or in the Hierarchy. A camera with `depthTest` on (3D, and
cameras created with `Camera(scene)`) draws opaque meshes by depth, then transparent ones (most
sprites) sorted by their distance to the camera position, not by Z.

### 4.3 Mesh (base of Primitive, Sprite, Model, Tilemap, ...) - partial

| Member | Kind |
|---|---|
| `color` | rw `Vector4` (sRGB) |
| `setColor(r, g, b)`, `setColor(r, g, b, a)`, `setColor(Vector4)` | method |
| `alpha` | rw |
| `enabled` | rw |
| `setTexture(path)` | method |
| `normalTexture` | rw path of submesh 0's normal map (`""` none; assigning `""` removes it) |
| `setNormalTexture([submesh,] path)`, `setNormalTexture(id, TextureData)`, `getNormalTexture([submesh])` | methods: the normal map of submesh 0 or of a submesh (tangent-space, OpenGL convention, green up; on sprites, tilemaps and polygons it answers Light 2Ds whose `height` is above 0; same size and frame layout as the colour texture; a Tilemap's submesh `i` is the tileset of its colour texture `i`) |
| `material` | rw `Material`; `getMaterial([submesh])`, `setMaterial([submesh,] mat)` |
| `castShadows`, `receiveShadows`, `receiveLights`, `transparent`, `faceCulling`, `cullingMode`, `customShader` | rw |
| `getAABB()`, `getWorldAABB()`, `getNumSubmeshes()` | methods |
| `setShaderUniform(name, float/Vector2/3/4)`, `getShaderUniform(name)`, `removeShaderUniform(name)` | methods |
| instancing: `createInstancedMesh()`, `addInstance(...)`, `updateInstance(i, ...)`, `removeInstance(i)`, `getNumInstances()`, `clearInstances()` | methods |

`Material` (rw): `baseColorFactor`, `metallicFactor`, `roughnessFactor`, `alphaMode`,
`alphaCutoff`, `emissiveFactor`, `baseColorTexture`, `emissiveTexture`,
`metallicRoughnessTexture`, `occlusionTexture`, `normalTexture`, `lighting`
(`MaterialLighting.*`: the stages evaluating the object's lights; set it on the copy
`getMaterial(i)` returns, then `setMaterial(i, m)`), `stochasticTiling` (Stochastic Tiling of
the built-in Lit's maps) with `stochasticCellSize`, `stochasticBlendSharpness`, `stochasticContrast`
and `stochasticRotation` (see the shaders guide). Shader library (same copy-then-set rule):
`shader` (`""` built-in Lit, `"library:<name>"`, or a fork), `blendMode`
(`MaterialBlendMode.*`), `cullMode`, `depthWrite`, `depthTest`, `depthOffsetFactor`,
`depthOffsetUnits`, `sortPriority` (draw order within its queue, opaque or transparent, lower
first, before distance); methods `setShaderParam(name, value)` (colours linear; a
`Vector3` colour is opaque), `getShaderParam(name)` (unset: the manifest default),
`removeShaderParam(name)`, `setShaderTexture(uniform, path)` (`""` back to the default; on a
`@texture ... cube` uniform it sets a 2D texture: use the cube form),
`setShaderCubeTexture(uniform, path)` (a `@texture ... cube` uniform: one image holding the six
faces, the layouts a Skybox takes) or `setShaderCubeTexture(uniform, front, back, left, right,
up, down)` (six images), `getShaderTexture(uniform)` (a cube map's single image or front
face), `setShaderFeature(define, on)` (a `@keywords` value is
`"GROUP_VALUE"`), `isShaderFeatureEnabled(define)`, `setShaderGlobalBinding(member, global)`
(the parameter follows a global shader value, 2.10.2, over its own; `nil` unbinds) and
`getShaderGlobalBinding(member)` (`nil` unbound). A material with `shader = ""` feeds the
shader its submesh draws with (the mesh's `customShader`, else the scene default): a copy from
`getMaterial` / `material` resolves defaults and features against that one. UI (Image, Text,
Polygon), Points, LineRenderer, TrailRenderer and Skybox objects have the same
parameter/texture/feature/global binding methods for their `customShader` (but
`setShaderCubeTexture`: set a cube uniform of theirs from the editor or MCP), and (but Skybox)
`blendMode` and `depthWrite`; a post-process pass: `scene:setPostProcessGlobalBinding(index,
member, global)`.
`stencil` (`MaterialStencil()`: `compare` (`MaterialStencilCompare.*`: `AUTO` takes the shader's
rule, `DISABLED`, `NEVER` ... `ALWAYS`), `ref`, `readMask`, `writeMask` (0-255), `pass`, `fail`,
`depthFail` (`MaterialStencilOp.*`)) and `colorWrite` (`MaterialColorWrite.*`: `AUTO`, `NONE`,
`RGB`, `ALL`, or the bits `R`, `G`, `B`, `A` summed): on the material (`m.stencil.ref = 2`
edits the copy's) and on UI, Points and LineRenderer objects (a copy there: set it back,
`local s = image.stencil; s.compare = MaterialStencilCompare.EQUAL; image.stencil = s`).
`extraPass` (`MaterialPass`, edited in place on the copy, then `setMaterial`): one more draw
of the submesh after its mesh's main draws (`order = MaterialPassOrder.BEFORE`: ahead of
them; on a mesh of several submeshes every After pass draws after all of its submeshes and
every Before pass ahead of all of them, in submesh order) with its own
`shader` (`""`: none), the same state fields as the material plus `stencil` and `colorWrite`,
and the same parameter/texture/feature methods (`setShaderCubeTexture` included), resolved
against its own shader; e.g.
`m.extraPass.shader = "shaders/outline"; m.extraPass:setShaderParam("width", 0.05)`.

### 4.4 Primitive (Mesh)

`createPlane(w, d [, tiles])`, `createGrid(w, d, segments [, tiles])` (the plane as segments x
segments quads, 1..255, for `library:water` WAVES), `createWall(w, h [, tiles])`, `createBox(w, h, d [, tiles])`,
`createSphere(r)` / `createSphere(r, slices, stacks)`, `createCylinder(r, h)` /
`(baseR, topR, h)` / `(r, h, slices, stacks)` / `(baseR, topR, h, slices, stacks)`,
`createCapsule(...)` (same overload shapes as cylinder), `createTorus(r, ringR [, sides, rings])`.
Color/texture via Mesh: `Primitive(self.scene, self.entity):setColor(1, 0, 0, 1)`.

### 4.5 Sprite (Mesh)

| Member | Kind |
|---|---|
| `width`, `height` | rw (points) |
| `setSize(w, h)` | method |
| `flipY` | rw (there is no `flipX` binding) |
| `textureRect` | rw `Rect`; `setTextureRect(x, y, w, h)` |
| `textureScaleFactor` | rw |
| `pivotPreset` | rw `PivotPreset.*` |
| `enabled` | rw |
| frames: `addFrame(...)`, `removeFrame(idOrName)`, `setFrame(idOrName)` | methods |
| `startAnimation(framesTable, framesTimeTable, loop)`, `startAnimation(startFrame, endFrame, intervalMs, loop)`, `startAnimation(name, intervalMs, loop)` | methods |
| `pauseAnimation()`, `stopAnimation()` | methods |
| `getShadowCaster2D()`, `removeShadowCaster2D()` | methods |
| plus Mesh: `color`, `alpha`, `setColor`, `setTexture`, ... | |

### 4.6 SpriteAnimation (Action)

`setAnimation(framesTable, framesTimeTable, loop)` / `setAnimation(startFrame, endFrame,
intervalMs, loop)`, plus Action: `start()`, `pause()`, `stop()`, `isRunning()`,
`isPaused()`, `isStopped()`, `speed` (rw), `target` (rw).

### 4.7 Tilemap (Mesh) - partial

`addRect(...)`, `removeRect(idOrName)`, `getRect(idOrName)`, `clearRects()`,
`addTile(...)`, `removeTile(idOrName)`, `getTile(idOrName)`, `clearTiles()`,
`findRectByString(s)`, `findTileByString(s)`, `getWidth()`, `getHeight()` (methods),
`textureScaleFactor`, `reserveTiles`, `enabled` (rw), `clearAll()`.

### 4.8 Model (Mesh)

`loadModel(path)`, `loadGLTF(path)`, `loadOBJ(path)`, `getAnimation(i)`,
`findAnimation(name)`, `playAnimation(indexOrName [, fadeTime])`, `stopAnimations(fadeTime)`,
`getBone(indexOrName)`, `getMorphWeight(indexOrName)`, `setMorphWeight(indexOrName, w)`,
`resetToBindPose()`, `enabled` (rw), `getPoseAnimator()` (the model's PoseAnimator, added when missing).

With an Animation Player on the Model's GameObject (4.8.2), `playAnimation(name [, fadeTime])`
crossfades to the player's state of that name (over the player's `defaultFadeTime` when no fade
is given) and fades out the model's running entity animations; an index is a player state, in
list order (0-based); an index past the player's states is the entity animation of that index. A
name the player lacks falls back to the entity animation of that name (the player's
states fade out). `stopAnimations(fadeTime)` fades the player out too. Without a player,
`playAnimation` uses the model's entity animations (glTF animations loaded as Animation
entities), as before; `getAnimation` / `findAnimation` always return entity animations (none
after the Inspector's Use Animation Player). Two glTF animations with the same name are both
named like that as entity animations, but without a player `playAnimation` and `findAnimation`
take the names the Inspector shows: `"Wave"` is the first one and `"Wave 2"` the second. With a
player, its states are `"Wave"` and `"Wave 2"`.

### 4.8.1 PoseAnimator (EntityHandle)

Plays a pose animation (`.poseanim`, path relative to the assets folder) live on a humanoid
Model; it lives on the Model's GameObject. `PoseAnimator(scene, entity)` adds the component when
missing, `model:getPoseAnimator()` does the same. It runs in play mode and in games, not in the
editor's edit mode. Models load asynchronously: until `isLoaded()` the animator waits (calls made
before are kept: `play`, `goToPose`...). A missing or invalid file logs one error and leaves the bones
alone; the animator tries the file again every second (and at once on `setAnimation`), so a fixed
file plays without a restart.

`setAnimation(path)` / `getAnimation()` (loads again), `play()`, `pause()`, `stop()` (time 0,
shows pose 0), `isPlaying()`, `setTime(t)` / `getTime()` / `getLength()` (seconds),
`setSpeed(s)` / `getSpeed()` (negative plays backwards), `setWeight(w)` / `getWeight()` (0..1
blend over the Model's clips or rest pose), `autoplay` (rw), `getPoseCount()`, `getPoseName(i)`,
`findPose(name)` (0-based; -1 when absent), `goToPose(indexOrName [, duration [, overshoot [,
anticipation]]])` (live spring transition from the pose shown, then holds that pose; playback
stops; returns false for an unknown pose), `isLoaded()`, `onFinish` (a function called when a
one-shot animation ends), `getPoseAnimatorComponent()`.

```lua
local anim = Model(self.scene, self.entity):getPoseAnimator()
anim.onFinish = function() anim:goToPose("Idle", 0.2) end
anim:play()
```

Crossfade over the Model's clip by moving the weight each frame, e.g. in `onUpdate`:
`anim:setWeight(anim:getWeight() + (target - anim:getWeight()) * math.min(1, Engine.deltatime * 8))`.
Ready-made animations: `get_pose_animation {templates: true}` (`jasper://docs/pose-animation`).

`loadModel` / `loadGLTF` also take an `.fbx` path: it loads the glTF the editor converted the FBX
into (`.jasperengine/imported/<path>.fbx.glb` in the editor, `<path>.fbx.glb` next to it in an
exported game, which ships only that). Every FBX of the assets is converted before Play and export.

### 4.8.2 AnimationPlayer (EntityHandle)

Unity's legacy Animation component: plays Animation Clips (`.anim` files, or a model's read-only
clips `"models/hero.gltf#Walk"`, paths relative to the assets folder) with fades, blends, queues
and wrap modes, and fires clip events. `AnimationPlayer(scene, entity)` adds the component when
missing; `GameObject:getAnimationPlayer()` returns `nil` without one. Players run in play mode
and in games (edit mode previews clips in the Animation window). Calls made before the clips load
are kept. Clip tracks bind by path below the player's GameObject; renames and reparents rebind
on the next frame. User guide: `docs/animation-clips.md`; agents: `jasper://docs/animation`.

| Member | Notes |
|---|---|
| `play(name)` | instant: the other states stop (rewound), `name` plays from 0 at weight 1 (from its end when the speed is negative); clears the queue |
| `play(name, fade)` | `fade` > 0: same as `crossFade(name, fade)` |
| `crossFade(name [, fade])` | fades the others out and `name` in; no fade = `defaultFadeTime` |
| `blend(name, weight [, fade])` | moves one state's weight, the others untouched; total weight under 1 is filled by the rest (scene) pose |
| `queue(name [, fade])` | plays after the playing states finish (loop / ping_pong: at the end of the current cycle; clamp_forever: on reaching its end); with a fade it starts `fade` s earlier and crossfades; queuing the playing state restarts it; nothing playing = play |
| `stop()`, `stop(name)` | stop and rewind, the pose stays; clear the queue |
| `rewind()`, `rewind(name)` | time 0 |
| `sample()` | applies the current times once (e.g. paused) |
| `isPlaying()`, `isPlaying(name)` | bool; false for a `once` state that ended (held) |
| `getState(name)` | an `AnimationState` (below) |
| `addClip(path [, name])` | appends an entry (`name` empty = the file stem / model clip name); false when the name is taken |
| `removeClip(name)`, `getClipCount()`, `getClipName(i)` | `getClipName` is 0-based, `""` out of range |
| `speed`, `defaultClip`, `defaultFadeTime`, `playAutomatically`, `enabled` | rw; `speed` multiplies every state (negative plays backwards); `enabled = false`: time stops, nothing is written |
| `isLoaded()` | every clip loaded and bound |
| `onEvent` | rw, a function called with ONE `AnimationEvent` (below) per clip event |
| `onFinish` | rw, a function called with the state name when a `once` state ends, and once when a `clamp_forever` state first reaches its end (never for loop / ping_pong) |
| `getAnimationPlayerComponent()` | the ECS component: `enabled`, `defaultClip`, `playAutomatically`, `speed`, `defaultFadeTime`, `needsLoad`, `onEvent`, `onFinish` |

A `once` state that reaches its end fires `onFinish`, stops advancing and **holds its last pose**
at its weight until a fade (crossFade / play with a fade / blend to 0) or `stop` releases it, so
`onFinish` then `crossFade("Idle", 0.2)` blends from the end pose; with nothing else playing the
pose stays. `play(name)` or `blend(name, w > 0)` of a held state restarts it; `blend(name, 0,
fade)` fades it out. Events and `onFinish` run after the frame's animation update, in time order; player
calls made inside them take effect next frame.

`AnimationState` (by player + state name, looked up on every access; works across clip list
changes): rw `time`, `normalizedTime`, `speed` (the state's own), `weight`, `wrap` (`"once"`,
`"loop"`, `"ping_pong"`, `"clamp_forever"`; `"default"` = the entry's), `enabled` (false pauses);
ro `name`, `clip`, `length`; `isValid()`. An unknown name reads defaults and writes nothing.

`AnimationEvent` (a copy): `functionName`, `state`, `time` (seconds in the clip), `args` (a
1-based table of numbers, strings and booleans), `getArgCount()`, `getArgType(i)` (`"number"`,
`"string"`, `"boolean"`), `getNumberArg(i)`, `getStringArg(i)`, `getBoolArg(i)` (0-based `i`).
An event fires for every state with weight > 0 that is not fading out (in a crossfade, only the
incoming clip), forward, backward and across loops, once per crossing. It calls every `onEvent`
subscriber, then the method named `functionName` on each enabled Lua script of the player's own
GameObject with the args spread as parameters. An event named like a method the engine calls
itself (`init`, `onDestroy`) or an engine event (`onUpdate`, `onKeyDown`...) calls that method
again with the event's args (as Unity's `SendMessage`; the clip's binding report warns): give
events their own names.

```lua
function Hero:init()
    self.player = GameObject(self.scene, self.entity):getAnimationPlayer()
    self.player.onEvent = function(event) print(event.functionName, event.args[1]) end
    self.player.onFinish = function(state)
        if state == "Attack" then self.player:crossFade("Idle", 0.2) end
    end
    self.player:play("Walk")
end

function Hero:onFootstep(side, strength) end    -- clip event {function: "onFootstep", args: ["left", 1]}
```

### 4.8.3 VertexAnimator (EntityHandle)

Plays a baked Vertex Animation (`.vat`, path relative to the assets folder) on the GPU: on the
GameObject, and on every instance of its Instanced Mesh, each with its own clip, time, speed and
crossfade. It lives on the GameObject whose Model is the `.vat`'s mesh (`<name>.vat.glb`).
`VertexAnimator(scene, entity)` adds the component when missing. `GameObject:getVertexAnimator()`
returns `nil` without one. It runs in play mode and in games. In the editor's edit mode it runs
only with `preview` on. Calls made before the `.vat` loads are kept and applied on load. An
unknown clip name or instance index logs one warning and does nothing. Instances count from 0,
like `Mesh:getInstance`. User guide: `docs/vertex-animation.md`; agents:
`jasper://docs/vertex-animation`.

| Member | Notes |
|---|---|
| `play(name)` | starts `name` now (from its end when the speed is negative and it doesn't loop); clears the queue |
| `play(name, fade)` | `fade` > 0: same as `crossFade(name, fade)` |
| `crossFade(name [, fade])` | fades linearly from the current clip to `name`; no fade = `defaultFadeTime`; at its end `name` is the current clip; a crossfade during a crossfade continues from the dominant clip (weight >= 0.5) |
| `blend(a, b, weight)` | holds clips `a` and `b` at a fixed weight of `b` (0..1), `b`'s phase following `a`'s (walk/run by speed); again with the same pair only moves the weight |
| `queue(name)` | plays `name` when the current clip ends (once / clamp_forever) or ends its cycle (loop / ping_pong); nothing playing = play now |
| `stop()` | holds the current pose (`isPlaying()` false) until the next play; clears the queue |
| `isPlaying()`, `isPlaying(name)` | bool; false after `stop` and for a `once` clip that ended |
| `time` | rw, seconds in the current clip |
| `normalizedTime` | ro, 0..1 in the current clip |
| `speed` | rw, multiplies every clip's speed (negative plays backwards); clips keep their phase when it changes |
| `currentClip` | ro, the clip playing (the one faded to during a crossfade, `a` in a blend); `""` before the `.vat` loads |
| `getClipNames()` | the `.vat`'s clip names (a table; empty until loaded) |
| `getClipLength(name)` | seconds; 0 for an unknown clip |
| `getSocketPose(name)`, `getSocketPose(name, i)` | a `VertexAnimatorSocketPose` now: `position` (Vector3), `rotation` (Quaternion), `valid` (false for an unknown socket or before load). Through crossfades. In the GameObject's space; with an instance index, that instance's clip and time, before the instance's own position, rotation and scale |
| `playInstance(i, name [, fade])` | instance `i` plays `name`; `fade` > 0 crossfades |
| `crossFadeInstance(i, name, fade)` | instance `i` fades to `name` over `fade` seconds |
| `setInstanceSpeed(i, speed)`, `setInstanceTime(i, seconds)` | that instance's speed (times the animator's `speed`) and time in its clip |
| `getInstanceClip(i)`, `getInstanceNormalizedTime(i)` | `""` / 0 for an unknown index |
| `getInstanceCount()` | the Instanced Mesh's instances the animator plays |
| `playAll(name [, fade])` | the GameObject and every instance |
| `randomizeTimes(seed)` | every instance (and the GameObject) at a seeded time of its clip: the same seed gives the same times |
| `animation` | rw, the `.vat` (asset-relative); setting it loads the file |
| `defaultClip`, `playAutomatically`, `randomStart`, `defaultFadeTime`, `instanceEvents`, `enabled` | rw; `randomStart`: each instance (and the GameObject) starts at a random time of its first clip; `instanceEvents`: per-instance events and finishes (costs work per instance each frame) |
| `preview` | rw, editor: animate in edit mode (the Inspector's Preview) |
| `isLoaded()` | the `.vat` is loaded and plays on this mesh |
| `getLoadError()` | why it doesn't play (a load error, or `"mesh_mismatch: ..."` when the Model isn't the `.vat`'s mesh); `""` when it plays |
| `onEvent` | rw, a function called with ONE `AnimationEvent` (4.8.2; `state` is the clip name) per clip event; GameObjects without an Instanced Mesh only |
| `onFinish` | rw, a function called with the clip name when a `once` clip ends, and once when a `clamp_forever` clip first reaches its end |
| `onInstanceEvent` | rw, a function called with `(i, event)` per instance event when `instanceEvents` is on |
| `onInstanceFinish` | rw, a function called with `(i, clip)` when `instanceEvents` is on |
| `getVertexAnimatorComponent()` | the ECS component: `enabled`, `animation`, `defaultClip`, `playAutomatically`, `speed`, `randomStart`, `defaultFadeTime`, `instanceEvents`, `preview`, `needsLoad`, ro `loadError`, ro `gpuActive`, `onEvent`, `onFinish`, `onInstanceEvent`, `onInstanceFinish` |

Events fire in play mode only, from the dominant clip: in a crossfade, the outgoing clip until
the midpoint, then the incoming one; in a blend, the clip with weight >= 0.5. A GameObject without
an Instanced Mesh calls every `onEvent` subscriber, then the method named `functionName` on each
enabled Lua script of the GameObject, with the args spread as parameters (as the Animation
Player). Instances call only `onInstanceEvent` / `onInstanceFinish`, and only with
`instanceEvents` on. Events and finishes run after the frame's update, in time order; calls made
inside them take effect next frame. Attachments (GameObjects following a socket) are set in the
Inspector. To attach something to an instance, move it with `getSocketPose(name, i)`.

```lua
function Crowd:init()
    self.vat = GameObject(self.scene, self.entity):getVertexAnimator()
    self.vat.instanceEvents = true
    self.vat.onInstanceFinish = function(i, clip)
        if clip == "Wave" then self.vat:crossFadeInstance(i, "Idle", 0.3) end
    end
    RegisterEngineEvent(self, "onUpdate")
end

function Crowd:onUpdate()
    if not self.started and self.vat:isLoaded() then      -- clip names are known once loaded
        self.started = true
        local clips = self.vat:getClipNames()
        for i = 0, self.vat:getInstanceCount() - 1 do
            self.vat:playInstance(i, clips[math.random(#clips)])
            self.vat:setInstanceSpeed(i, 0.8 + 0.4 * math.random())
        end
        self.vat:randomizeTimes(42)
    end
end
```

### 4.9 Camera (GameObject) - partial

rw: `type` (`CameraType.*`), `enabled`, `nearClip`, `farClip`, `leftClip`, `rightClip`,
`bottomClip`, `topClip`, `aspect`, `yfov`, `target` (Vector3), `up` (Vector3), `autoResize`,
`renderToTexture`, `transparentSort`, `depthTest`, `cullingMask`.
Methods: `activate()`, `setOrtho(l, r, b, t, near, far)`, `setPerspective(yfov, aspect, near,
far)`, `setType(t)`, `setTarget(x, y, z)`, `disableTarget()`, `isUsingTarget()`, `setUp(x,y,z)`,
`getDirection()`, `getRight()`, `getUp()`, `getWorldDirection()`, `getWorldTarget()`,
`getWorldUp()`, `getWorldRight()`, `screenToRay(x, y)`, `rotateView(a)`,
`rotatePosition(a)`, `elevateView(a)`, `elevatePosition(a)`, `walkForward(d)`, `zoom(d)`,
`slide(d)`, `slideForward(d)`, `slideUp(d)`, `getViewMatrix()`, `getProjectionMatrix()`,
`getViewProjectionMatrix()`, `getDistanceFromTarget()`, `updateCamera()`.
A camera looks at its target by default (`isUsingTarget()`): the GameObject's rotation is
ignored until `disableTarget()`. Orbit: position = target + (cos(pitch)sin(yaw), sin(pitch),
cos(pitch)cos(yaw)) * distance, then `setTarget(target)`.

### 4.10 Light (GameObject) / Light2D (GameObject)

Light rw: `type` (`LightType.*`), `enabled`, `direction` (Vector3), `color` (Vector3),
`range`, `intensity`, `innerConeAngle`, `outerConeAngle`, `shadows`, `cameraNear`,
`cameraFar`, `automaticShadowCamera`, `numCascades`, `cullingMask`, `shading`
(`LightShading.*`, the stage evaluating it on MIXED-lighting materials); methods
`setDirection(x,y,z)`, `setColor(r,g,b)`, `setConeAngle(a)`, `setRange(r)`,
`setIntensity(i)`, `setShadows(b)`, `setShadowCameraNearFar(n, f)`.

Light2D rw: `enabled`, `color` (Vector3), `intensity`, `range`, `falloff`, `height`,
`shadows`, `shadowBias`, `shadowSoftness`, `mapResolution`; methods `setColor(r,g,b)`,
`setIntensity(i)`, `setRange(r)`, `setFalloff(f)`, `setHeight(h)`, `setShadows(b)`.
`height`: the light's virtual height above the 2D plane for normal-mapped objects (editor
default 64; `Light2D(scene)` keeps 0, which makes normal maps flat); unlit objects without a
normal map (sprites by default) are lit the same at any height; Lit materials keep their N.L. 2D normal maps use the OpenGL convention (green up), like
3D Lit, Unity and Godot: a map made for the engine's old 2D (DirectX) behaviour needs its green
channel flipped.

### 4.11 UI: RectTransform, Text, Image, Button, Panel, Scrollbar, Progressbar, InputField

RectTransform (GameObject) rw: `width`, `height`, `anchorPreset` (`AnchorPreset.*`),
`anchorPointLeft/Top/Right/Bottom`, `anchorOffsetLeft/Top/Right/Bottom`,
`positionXOffset`, `positionYOffset`, `positionOffset` (Vector2), `pivot` (Vector2),
`usingAnchors`, `ignoreScissor`, `enabled`; methods `setSize(w, h)`,
`setAnchorPoints(l, t, r, b)`, `setAnchorOffsets(l, t, r, b)`, `setPivot(x, y)`.
UI position: use `positionXOffset`/`positionYOffset` (as in the bundled example), or
`position`.

| Class | rw properties | methods |
|---|---|---|
| `Text` | `text`, `font`, `fontSize`, `color` (Vector4), `alpha`, `multiline`, `fixedWidth`, `fixedHeight`, `maxTextSize`, `flipY`, `pivotBaseline`, `pivotCentered` | `setColor(r,g,b[,a])`, `setFont(path)`, `setFixedSize(w,h)`, `getNumChars()`, `getCharPosition(i)`, `getLineHeight()` |
| `Image` | `color` (Vector4), `alpha`, `textureScaleFactor`, `flipY`, `patchMarginLeft/Right/Top/Bottom` | `setTexture(path)`, `setColor(r,g,b[,a])`, `setPatchMargin(m)` / `(l,r,t,b)`, `getUIComponent()` |
| `Button` (Image) | `label`, `labelColor`, `labelFont`, `labelFontSize`, `colorNormal`, `colorHovered`, `colorPressed`, `colorDisabled`, `disabled` | `getButtonComponent()`, `getLabelObject()`, `setTextureNormal/Hovered/Pressed/Disabled(path)`, `setColorNormal(r,g,b[,a])` (and Hovered/Pressed/Disabled), `setLabelColor(r,g,b,a)` |
| `Panel` (Image) | `title`, `titleColor`, `titleFont`, `titleFontSize`, `headerColor`, `minWidth`, `minHeight`, `canMove`, `canResize`, `canBringToFront` | `getPanelComponent()`, `setMinSize(w,h)` |
| `Scrollbar` (Image) | `type` (`ScrollbarType.*`), `barSize`, `step`, `barColor`, `barAlpha` | `getScrollbarComponent()`, `setBarTexture(path)` |
| `Progressbar` (Image) | `type`, `value` (0..1), `fillColor`, `fillAlpha` | `getProgressbarComponent()`, `setFillTexture(path)` |
| `InputField` (Image) | `text`, `disabled`, `textColor`, `textFont`, `fontSize`, `maxTextSize`, `placeholder`, `placeholderColor`, `password`, `passwordChar`, `cursorIndex`, `cursorColor`, `selectionColor`, `cursorBlink`, `cursorWidth` | `getInputFieldComponent()`, `getTextObject()`, `setSelection(...)` |

Component structs returned by `get*Component()` expose the event objects of section 1.3 and
some raw fields (`ButtonComponent.pressed`, `hovered` ro; `disabled` rw;
`ProgressbarComponent.value` rw; `InputFieldComponent.text` ro). Prefer the wrapper
properties for changes; raw component writes need the matching `needUpdate*` flag.

### 4.12 AudioSource (EntityHandle)

Methods: `play()`, `pause()`, `stop()`, `seek(t)`, `loadSound(path)`, `destroySound()`,
`isPlaying()`, `isPaused()`, `isStopped()`, `getLength()`, `getPlayingTime()`,
`getGameObject()`, `setMinMaxDistance(min, max)`, `setInaudibleBehavior(mustTick, kill)`,
`getAudioSourceComponent()`.
rw: `enabled`, `volume`, `speed`, `pan`, `looping`, `loopingPoint`, `sound3D`,
`clockedSound`, `protectVoice`, `minDistance`, `maxDistance`, `attenuationModel`
(`AudioSourceAttenuation.*`), `attenuationRolloffFactor`, `dopplerFactor`.
Restart a one-shot: `snd:stop(); snd:play()`.

### 4.13 Rigidbody2D (EntityHandle) - 2D physics (Box2D)

| Member | Kind | Notes |
|---|---|---|
| `position` | rw `Vector2` | points (pixels); setting teleports and wakes the body |
| `angle` | rw number | default units (degrees) |
| `linearVelocity` | rw `Vector2` | points per second |
| `angularVelocity` | rw | |
| `linearDamping`, `angularDamping`, `gravityScale` | rw | saved fields too (Unity drag, gravity scale) |
| `type` | rw | `BodyType.STATIC`, `BodyType.KINEMATIC`, `BodyType.DYNAMIC` |
| `fixedRotation`, `bullet`, `awake`, `enableSleep`, `enabled` | rw bool | `bullet` = `collisionDetection` CONTINUOUS, `enableSleep` false = NEVER_SLEEP |
| `collisionDetection` | rw | `CollisionDetectionMode2D.DISCRETE` / `CONTINUOUS` (never tunnels) |
| `sleepingMode` | rw | `RigidbodySleepMode2D.NEVER_SLEEP` / `START_AWAKE` / `START_ASLEEP` |
| `mass` | rw number | kg; setting it turns `useAutoMass` off (also `getMass()`) |
| `useAutoMass` | rw bool | true: mass from the colliders (area x density) |
| `centerOfMass`, `autoCenterOfMass` | rw `Vector2`, bool | body-local, collider units (points); setting the centre turns auto off |
| `inertia`, `autoInertia` | rw number, bool | kg m^2 about the centre of mass; setting it turns auto off |
| `material` | rw | default material of the colliders without one: a `.physicsmaterial` path, a `PhysicsMaterial` or nil; reads a `PhysicsMaterial` or nil |
| `pointsToMeterScale` | ro | default 64 |
| `getRotationalInertia()`, `applyMassFromColliders()` | methods | |
| `applyForce(forceV2, pointV2, wake)` | method | point in world points |
| `applyForceToCenter(forceV2, wake)` | method | |
| `applyTorque(t, wake)` | method | |
| `applyLinearImpulse(impulseV2, pointV2, wake)` | method | |
| `applyLinearImpulseToCenter(impulseV2, wake)` | method | |
| `applyAngularImpulse(i, wake)` | method | |
| `createBoxCollider(w, h)` | method | box from local (0,0) to (w,h); returns collider index |
| `createCenteredBoxCollider(w, h [, centerV2, angle])` | method | centered box |
| `createRoundedBoxCollider(w, h, radius)`, `createCircleCollider(centerV2, radius)`, `createCapsuleCollider(c1, c2, radius)`, `createSegmentCollider(p1, p2)`, `createPolygonCollider({Vector2...})`, `createChainCollider({Vector2...}, loop)` | methods | any number of colliders; a polygon of any number of points, concave too (split in convex pieces); a chain of 2+ points (2-3: segments) |
| `removeAllColliders()`, `getNumColliders()`, `getColliderType(i)` | methods | |
| `removeCollider(i)` | method | -> bool; later indices shift down |
| `setColliderPoints(i, {Vector2...})`, `getColliderPoints(i)` | methods | polygon or chain, any count |
| `setColliderLoop(i, b)`, `isColliderLoop(i)`, `setColliderRadius(i, r)`, `getColliderRadius(i)`, `setColliderCenter(i, v2)` (circle), `getColliderCenter(i)`, `setColliderSegment(i, a, b)` (segment, capsule) | methods | rebuild that collider only at the next step: the body keeps its velocity, joints and other contacts |
| `setColliderMaterial(i, material)`, `getColliderMaterial(i)` | methods | a `.physicsmaterial` path, a `PhysicsMaterial` or nil; the getter returns the one in use (the collider's, else the body's) or nil |
| `setColliderTrigger(i, b)`, `isColliderTrigger(i)` | methods | Unity `isTrigger` (the sensor flag) |
| `getColliderPieceCount(i)` | method | the Box2D shapes of the collider (convex pieces of a polygon, segments of a chain) |
| `load()` | method | rebuilds the whole body (collider edits do not need it) |
| `shapeDensity`, `shapeFriction`, `shapeRestitution`, `shapeSensor`, `shapeContactEvents`, `shapeSensorEvents`, `shapeEnableHitEvents`, `shapePreSolveEvents` | rw | first collider; per collider: `setColliderX(index, v)` / `getColliderX(index)` (`Density`, `Friction`, `Restitution`), `setColliderSensor(i, b)`, `isColliderSensor(i)` ... Friction / restitution of a collider with a material read the material's; setting them gives the collider its own copy of it (Unity `collider.material`) |
| `categoryBitsFilter`, `maskBitsFilter`, `groupIndexFilter` | rw | also `setBitsFilter(category, mask)` |
| `getBodyContacts()`, `getColliderContacts(i)` | methods | tables of `Contact2D` |
| `getAttachedObject()` | method | GameObject |

`Contact2D`: `getBodyA()`, `getBodyB()` (Rigidbody2D), `getBodyEntityA()`,
`getBodyEntityB()`, `getColliderIndexA()`, `getColliderIndexB()`, `getManifold()`.
`Manifold2D`: `getNormal()`, `getPointCount()`, `getManifoldPointPosition(i)`, ... .
`PhysicsMaterial` (shared with 3D): `PhysicsMaterial.load(path)` (the asset's runtime copy, shared
by every collider naming it until the scene ends; nil when it cannot be read),
`PhysicsMaterial.new{friction = 0.4, bounciness = 0, frictionCombine =
PhysicsMaterialCombine.MINIMUM, bounceCombine = ...}` (a material of the script's own); rw
`friction`, `bounciness`, `frictionCombine`, `bounceCombine`, ro `path` ("" for a new one).
Editing one changes every collider using it at the next step.

### 4.13.1 Joint2D (EntityHandle) - 2D joints

`go:getJoint2D()` (nil when none), `Joint2D(scene, entity)`, `Joint2D(scene)` (a new GameObject
with a joint). Units: points, degrees (the engine's angle unit), degrees/s or points/s, N, N m, Hz.
Unity mapping: WheelJoint2D = `Joint2DType.WHEEL`, HingeJoint2D = `REVOLUTE`, SliderJoint2D =
`PRISMATIC`, DistanceJoint2D = `DISTANCE` (`rope` = maxDistanceOnly), SpringJoint2D = `DISTANCE`
with `enableSpring`, FixedJoint2D = `WELD`, RelativeJoint2D = `MOTOR`, TargetJoint2D = `MOUSE`.

| Member | Kind | Notes |
|---|---|---|
| `type` | rw `Joint2DType` | changing it rebuilds the joint (fields at the old type's defaults take the new type's) |
| `bodyA`, `bodyB` (alias `connectedBody`) | rw GameObject or nil | set with a GameObject or Rigidbody2D that has a Rigidbody 2D (else a Lua error), or nil (bodyB nil: the world) |
| `anchorSpace` | rw `JointAnchorSpace.LOCAL` / `WORLD` | LOCAL: anchors in each body's space, axis in bodyA's |
| `anchorA` (alias `anchor`), `anchorB` (alias `connectedAnchor`), `autoAnchors`, `axis` | rw | changing them rebuilds the joint before the next step |
| `enableCollision` | rw bool | the connected bodies collide |
| `breakForce`, `breakTorque` | rw number | N, N m; `math.huge` never breaks (default) |
| `breakAction` | rw `JointBreakAction.IGNORE` / `CALLBACK_ONLY` / `DISABLE` / `DESTROY` | default DESTROY; the callback is `onJointBreak2D(joint)` |
| `useLimits`, `lowerAngle`, `upperAngle`, `lowerTranslation`, `upperTranslation` | rw | revolute angles; prismatic and wheel translations |
| `useMotor`, `motorSpeed`, `maxMotorTorque`, `maxMotorForce` | rw | live: change them every frame (a car's throttle) |
| `enableSpring`, `frequency`, `dampingRatio` | rw | wheel suspension, revolute / prismatic / distance spring, mouse stiffness |
| `length`, `autoLength`, `minLength`, `maxLength`, `rope` | rw | distance (setting `length` turns `autoLength` off) |
| `linearFrequency`, `angularFrequency`, `linearDampingRatio`, `angularDampingRatio` | rw | weld (0 = rigid) |
| `linearOffset`, `angularOffset`, `autoOffset`, `maxForce`, `maxTorque`, `correctionFactor` | rw | motor joint (setting an offset turns `autoOffset` off); mouse `maxForce` (0: 1000 x mass) |
| `target` | rw `Vector2` | mouse joint world point |
| `enabled` | rw bool | a disabled joint leaves the simulation (a broken DISABLE joint stays so until enabled) |
| `reactionForce` (Vector2), `reactionTorque`, `jointAngle`, `jointSpeed`, `jointTranslation`, `jointLinearSpeed`, `motorTorque`, `motorForce`, `currentLength`, `limitState` (`JointLimitState2D`), `referenceAngle`, `broken` | ro | the live joint (0 without one) |
| `setDistanceJoint(bodyA, bodyB [, anchorA, anchorB, rope])`, `setRevoluteJoint(a, b, anchor)`, `setPrismaticJoint(a, b, anchor, axis)`, `setMouseJoint(a, b, target)`, `setWheelJoint(a, b, anchor, axis)`, `setWeldJoint(a, b, anchor)`, `setMotorJoint(a, b)` | methods | bodies are entities; build a joint as before local anchors: WORLD anchors and Box2D's defaults for every other field (set properties after) |

```lua
local joint = self.frontWheel:getJoint2D()
joint.useMotor = true
joint.motorSpeed = -720      -- deg/s
joint.maxMotorTorque = 10000
function Car:onJointBreak2D(joint) Log.print("snap") end
```

### 4.14 Rigidbody (EntityHandle) - 3D physics (Jolt)

| Member | Kind | Notes |
|---|---|---|
| `position` | rw `Vector3` | teleport |
| `rotation` | rw `Quaternion` | |
| `linearVelocity`, `angularVelocity` | rw `Vector3` | also `setLinearVelocityClamped(v)`, `setAngularVelocityClamped(v)` |
| `mass` | rw property | (2D uses `getMass()` method) |
| `type` | rw `BodyType.*` | |
| `motionQuality` | rw | `RigidbodyMotionQuality.DISCRETE` / `LINEAR_CAST` |
| `friction`, `restitution`, `gravityFactor`, `sensor`, `allowSleeping`, `enabled`, `shapeDensity`, `collisionGroupID`, `categoryBitsFilter`, `maskBitsFilter`, `collideKinematicVsNonDynamic` | rw | |
| `buoyancy`, `waterDrag`, `waterAngularDrag` | rw numbers | floating in a `library:water` (2.10.1): Jolt's buoyancy at the centre of mass each fixed step of a dynamic body, 1 neutral, more floats, less sinks, 0 (default) ignores water; drags default 0.5 and 0.01 |
| `applyForce(f)`, `applyForce(f, point)` | method | f in Vector3; single-arg = at center (no `applyForceToCenter` in 3D) |
| `applyImpulse(i)`, `applyImpulse(i, point)`, `applyTorque(t)`, `applyAngularImpulse(a)` | methods | |
| `activate()`, `deactivate()` | methods | |
| `createBoxCollider(w, h, d)`, `createSphereCollider(r)`, `createCapsuleCollider(halfH, r)`, `createCylinderCollider(halfH, r)`, `createTaperedCapsuleCollider(...)`, `createConvexHullCollider(...)`, `createMeshCollider(...)`, `createHeightFieldCollider(...)` | methods | each also with `(Vector3 pos, Quaternion rot, ...)` prefix overload |
| `getNumColliders()`, `getColliderType(i)`, `load()`, `getAttachedObject()` | methods | |
| `setAllowedDOFsAll()`, `setAllowedDOFs2DPlane()`, `setAllowedDOFs(...)`, `setOverrideMassAndInertia(...)` | methods | |
| `getPointVelocity(p)`, `getAccumulatedForce()`, `getCenterOfMassPosition()` | methods | |

`Contact3D`: `getWorldSpaceNormal()`, `getPenetrationDepth()`, `getBaseOffset()`,
`getColliderIndex1()`, `getColliderIndex2()`, rw `combinedFriction`,
`combinedRestitution`, `sensor`. `Joint3D(scene [, entity])`: `setFixedJoint`,
`setDistanceJoint`, `setPointJoint`, `setHingeJoint`, `setConeJoint`, `setPrismaticJoint`,
`setSwingTwistJoint`, `setSixDOFJoint`, `setPathJoint`, `setGearJoint`,
`setRackAndPinionJoint`, `setPulleyJoint`, `getType()` (partial).

### 4.15 Actions and Tweens

Action (EntityHandle): `start()`, `pause()`, `stop()`, `restart()` (stops a running or paused
action and starts it from the beginning, in the same update), `isRunning()`, `isPaused()`,
`isStopped()`, `getTimeCount()`, `getActionComponent()`; rw `target` (Entity),
`ownedTarget`, `speed` (scales the action's own time: 2 plays twice as fast, a negative speed
plays back to the start and holds there), `weight`; `setTarget(gameObjectOrEntity)`.
`stop()` also cancels a `start()` asked for earlier in the same frame.

Tween (Action): rw `duration`, `loop`, `pingPong` (forward and then back, in twice the
duration); `setFunctionType(EaseType.*)`, `getTime()`, `getValue()`, `getTweenComponent()`.
A non-looping tween stops on the update it reaches its end, after applying the end value.

| Tween | `setAction(...)` (pass every argument; C++ defaults do not apply) |
|---|---|
| `PositionTween` | `(startV3, endV3, duration, loop)` |
| `RotationTween` | `(startQuat, endQuat, duration, loop)`; `setSpinAction(startQuat, axisV3, angle, duration, loop)` turns by `angle` (any size, so full turns) around `axis` |
| `ScaleTween` | `(startV3, endV3, duration, loop)` |
| `ColorTween` | `(startV3, endV3, duration, loop)` or `(startV4, endV4, duration, loop)` |
| `AlphaTween` | `(startAlpha, endAlpha, duration, loop)` |

```lua
local tw = PositionTween(self.scene)                    -- new action entity
tw:setTarget(GameObject(self.scene, self.entity))
tw:setAction(Vector3(0, 0, 0), Vector3(200, 0, 0), 1.0, false)
tw:setFunctionType(EaseType.QUAD_OUT)
tw:start()
```

Animation (Action): rw `loop`, `ownedActions`, `blendWeight`, `defaultFadeTime`;
`fadeIn(t)`, `fadeOut(t)`, `addActionFrame(...)`, `getActionFrameSize()`,
`getActionFrame(i)` (partial). Keyframe tracks: `TranslateTracks`, `RotateTracks`,
`ScaleTracks`, `MorphTracks` (all `KeyframeTracks`) with `setTimes`, `setValues`, `setEasings`,
`setEasing`; rw `loop` (starts over after the last key) and `relative` (position, rotation
and scale tracks: values are offsets from the target's pose when the track starts: position
plus value, rotation times value, scale times value; an anchored UI element moves by its
RectTransform position offset).
`LatticeTracks(scene, times, points, values)` (a `KeyframeTracks` too) keys lattice offsets:
`points` are flat indices (`i + rx*(j + ry*k)`), `values[key][n]` the offset of `points[n]`;
also `setPoints`. Its target needs a LatticeComponent; points it does not animate keep their
offsets (`relative` does not apply to it).

`Ease(EaseType.X)` / `Ease.linear(t)`, `Ease.easeInQuad(t)`, `Ease.easeOutQuad(t)`,
`Ease.easeInOutQuad(t)`, ... `Ease.easeOutBounce(t)` (static helpers, t in 0..1).

### 4.16 ParticleSystem (Action) - partial

rw: `enabled`, `rate`, `maxPerUpdate`, `emitter`, `loop`, `localSpace`, `emitterShape`
(`ParticleEmitterShape.*`). Methods: `reset()`, `addBurst(time, count)` / `addBurst(time, minCount, maxCount)`,
`setBursts(...)`, `clearBursts()`, `setLifeInitializer(min [, max])`, `setPositionInitializer(minV3 [, maxV3])`,
`setSpherePositionInitializer(r [, r2])`, `setCirclePositionInitializer(...)`,
`setVelocityInitializer(minV3 [, maxV3])`, `setAccelerationInitializer(...)`,
`setColorInitializer(minV3 [, maxV3])`, `setAlphaInitializer(...)`,
`setSizeInitializer(...)`, `setRotationInitializer(...)`, `setScaleInitializer(...)`, and
`set*Modifier(fromTime, toTime, from, to [, EaseType])` for Position/Velocity/Acceleration/
Color/Alpha/Size/Rotation/Scale/Sprite. Plus Action `start()`, `stop()`, `pause()`.
Component: `getParticleSystemComponent()` (the old misspelt `getParticlesomponent()` still works, deprecated).

### 4.17 FluidSimulation (GameObject)

Pavel Dobryakov's WebGL fluid (GameObject > Effects > Fluid Simulation): a GPU stable-fluids
solve with his look (shading, bloom, sunrays) drawn into a texture. `FluidSimulation(scene)`
makes one; `output` (`FluidOutput.*`) says where it goes: `SCREEN` (over the main camera),
`MATERIAL` (the entity's Sprite/Image/Mesh texture), `TEXTURE` (only the `fluid|<entity>`
texture link and `getTexture()`). Positions are fluid coordinates: (0,0) bottom-left, (1,1)
top-right. Full guide (presets, costs, deterministic runs, MCP): `jasper://docs/fluid`.

rw (the component's fields, Pavel's defaults): `enabled`, `output`, `interactive` (pointer drags
splat), `aspectRatio` (0 auto); solver `simResolution` (128), `dyeResolution` (1024),
`densityDissipation` (1), `velocityDissipation` (0.2), `pressure` (0.8), `pressureIterations`
(20), `curl` (30), `gradientScale` (1); clock `simulationRate` (60 steps/s), `maxSubsteps` (4),
`timeScale`, `paused`; splats `splatRadius` (0.25), `splatForce` (6000), `colorful`,
`colorUpdateSpeed`, `splatsOnStart`, `randomSplatCount` (0: random 5-24), `seed` (0: new each
start; set one for repeatable runs); look `shading`, `backColor` (Vector3, or
`setBackColor(r, g, b)`), `transparent` (alpha = max(r, g, b)), `bloom`, `bloomIterations`,
`bloomResolution`, `bloomIntensity`, `bloomThreshold`, `bloomSoftKnee`, `sunrays`,
`sunraysResolution`, `sunraysWeight`. ro `preset` (the matching preset id, `""` when custom).
Setters log an error and keep the old value for NaN/infinite or negative numbers (and 0 for
resolutions, `simulationRate`, `maxSubsteps`, `splatRadius`).

| Method | Meaning |
|---|---|
| `splat(x, y, dx, dy, r, g, b)` | a splat at the next step (also while paused): velocity in Pavel's force units (a pointer delta x `splatForce`), HDR colour (his pointer colours ~0.15) |
| `splat(posV2, velV2, colorV3 [, radius])` | the same with vectors; `radius` in `splatRadius` units (0: the component's) |
| `randomSplats([count])` | a seeded random burst (none/0: 5-24 splats) |
| `step([n])` | queues `n` (1) fixed steps for the next frame, also while paused (deterministic stepping) |
| `reset()` | restarts at the next frame: zero fields, the seed, the start burst |
| `applyPreset(id)` / `getPreset()` | `"default"`, `"mobile"`, `"smoke"`, `"ink"`, `"calm-water"`, `"psychedelic"`; sets the solver and look fields only (false: unknown id) |
| `FluidSimulation.getPresetIds()` | static: the preset ids, in order |
| `getStats()` | `FluidStats`: `steps`, `time`, `splats`, `simWidth`, `simHeight`, `dyeWidth`, `dyeHeight`, `gpuBytes`, `passesLastFrame`, `seed`, `shadingActive`, `bloomActive`, `sunraysActive`, `manualFiltering`, `compactFormats` (the device fallbacks in use), `outputWidth`, `outputHeight`, `targetFailures` (bloom or sunrays targets the device could not make: that feature is off, `getError()` says why), `drawn` (the last frame drew the output) |
| `getTexture()` | the output `Texture` (RGBA8) |
| `getFluidSimulationComponent()` | the raw component (no validation) |

```lua
function Fluid:init()
    self.fluid = FluidSimulation(self.scene)   -- keep it: the wrapper owns the entity
    self.fluid.output = FluidOutput.SCREEN
    self.fluid.transparent = true
    self.fluid:applyPreset("ink")
    self.fluid.seed = 42
    RegisterEngineEvent(self, "onUpdate")
end

function Fluid:onUpdate()
    if Input.isKeyPressed(Input.KEY_SPACE) then
        self.fluid:splat(0.5, 0.2, 0, 800, 0.1, 0.4, 1.0)   -- while held: an upward blue jet
    end
end
```

### 4.18 Other wrappers (partial)

- `Points`: `addPoint(...)`, `updatePoint(i, ...)`, `removePoint(i)`, `clearPoints()`,
  `getNumPoints()`, `setTexture(path)`, rw `maxPoints`, `enabled`.
- `LineRenderer` (Unity's LineRenderer, jasper://docs/lines-trails): the polyline:
  rw `positionCount` (growing repeats the last point), `setPosition(index, V3)` /
  `getPosition(index)` (**0-based** like Unity; out of range is an error naming the count;
  `setPosition(x, y[, z])` with numbers still moves the GameObject), `setPositions(table)`,
  `getPositions()`, rw `loop`, `useWorldSpace`; the width: rw `widthMultiplier` (0 = the 1-pixel
  hairline), `startWidth`, `endWidth` (set one: the curve becomes linear between them, multiplier
  1), `widthCurve` (an `AnimationCurve` copy: edit it, assign it back), `widthUnit`
  (`LineWidthUnit.*`); rw `colorGradient` (a `Gradient` copy), `startColor`, `endColor` (sRGB
  Vector4, the gradient's ends), `numCornerVertices`, `numCapVertices` (0..90), `alignment`
  (`LineAlignment.*`), `textureMode` (`LineTextureMode.*`), `textureScale` (Vector2), `texture`
  (path: the built-in shader's base map), `sortPriority`, `mode` (`LineRendererMode.*`: the
  polyline API switches to `Polyline`, the segment API to `Segments`). The segments (debug lines):
  `addLine(aV3, bV3 [, colorV4 [, colorB]])`, `updateLine(i, ...)`, `removeLine(i)`,
  `clearLines()`, `getNumLines()`, rw `maxLines`, `enabled`; the shader methods as Points.
- `TrailRenderer` (Unity's TrailRenderer; `TrailRenderer(scene)` makes a GameObject with one,
  `object:addTrailRenderer()` adds one to a GameObject (AddComponent; its own when it has one),
  `object:getTrailRenderer()` is nil without one, `object:hasTrailRenderer()`): rw `time`,
  `minVertexDistance`, `emitting`, `autodestruct`, `enabled`, the width and colour properties
  of LineRenderer (`widthMultiplier`, `startWidth`, `endWidth`, `widthCurve`, `widthUnit`,
  `colorGradient`, `startColor`, `endColor`, `numCornerVertices`, `numCapVertices`, `alignment`,
  `textureMode`, `textureScale`, `texture`, `sortPriority`: curve and gradient run from the head,
  0, to the tail, 1); `clear()` (after a teleport); ro `positionCount`; `getPosition(i)`,
  `setPosition(i, V3)` (0-based, the oldest first; the live head is not one), `getPositions()`,
  `addPosition(V3)`, `addPositions(table)`; the shader methods (`customShader`,
  `setShaderParam`, `setShaderTexture`, `setShaderFeature`, `blendMode`, ...).
- `Polygon` (UI): `addVertex(x, y)` / `(Vector3)`, rw `color`, `alpha`, `flipY`;
  `setColor(r,g,b[,a])`, `setTexture(path)`.
- `MeshPolygon`: `addVertex(...)`, ro `width`, `height`, rw `flipY`.
- `LayoutGroup` (RectTransform): rw `type` (`LayoutGroupType.*`), `useAllWrapSpace`,
  `wrapCellWidth`, `wrapCellHeight`; ro `contentWidth`, `contentHeight`; `resize()`.
- `ShadowCaster2D`: rw `shape` (`ShadowCaster2DShape.AUTO_QUAD` / `POLYGON`), `closed`,
  `enabled`; `addVertex(x, y)`, `clearVertices()`, `getVertexCount()`.
- `Fog`: rw `type` (`FogType.*`), `color`, `density`, `linearStart`, `linearEnd`.
- `Skybox`: `setTextures(positiveX, negativeX, positiveY, negativeY, positiveZ, negativeZ)` (six
  images; `+Z` is the front), `setTexture(path)` (one image holding the six faces),
  `setTexturePositiveX(path)` ... `setTextureNegativeZ(path)` (one face), rw `color`, `alpha`, `rotation`.
- `Terrain`, `ReflectionProbe`, `Mirror`, `Bone`: editor-configured; see bindings.
- `ObjectRender.getLivePipelineCount()` (static, diagnostics): the render pipelines objects
  made and did not destroy yet (a leak check: the count before creating objects and after
  destroying them, once their loads settled).

### 4.19 HTML/CSS documents: UIDocument, UIElement, UIEvent, data models

Full guide: `jasper://docs/html-ui` ("Scripting"). Browser names and semantics.

- `UIDocument` (GameObject; `UIDocument(scene)` adds the component, `UIDocument(scene, entity)`):
  rw `filename`, `title`, `visible`, `sortOrder`; ro `loaded`, `body`, `documentElement` (the
  `<html>`); `getDiagnostics()` (what the engine does not draw like a browser: a list of
  `{severity, code, name, message, suggestion, file, line, column, text}`),
  `setColorScheme("dark"|"light")`, `setReducedMotion(b)` (the `prefers-*` media queries);
  `load(file)`,
  `reload()`, `show()`, `hide()`, `getElementById(id)`, `querySelector(sel)`,
  `querySelectorAll(sel)`, `createElement(tag)`, `createTextNode(text)`,
  `addEventListener(type, fn[, capture|{capture=, once=}])`, `removeEventListener(...)`,
  `createDataModel(name, table)` (returns the model proxy), `getDataModel(name)`,
  `removeDataModel(name)`, `getTargetTexture()` (TEXTURE mode: a `Framebuffer`, else nil),
  `getUIDocumentComponent()` (rw `filename`, `sortOrder`, `enabled`, `interactive`,
  `renderMode` (`UIDocumentRenderMode.SCREEN`/`TEXTURE`, applied at the next load: set
  `needReload = true`), `textureWidth`, `textureHeight`, `inputSurface`, `pixelRatio`,
  `needReload`; ro `loaded`; events `onLoad`, `onUnload`); events `onLoad`, `onUnload`;
  static `UIDocument.setDebuggerVisible(b)`, `isDebuggerVisible()`, `getLiveLuaRefCount()`.
  Getters load a document that is due, so `init` can query elements.
- `UIElement` (weak handle; `nil` when a query finds nothing; any use but `isValid()` of a
  dead one raises): rw `id`, `className`, `textContent`, `innerHTML`, `outerHTML` (HTML in an
  `.html` document), `value`, `checked`, `disabled`, `scrollTop`, `scrollLeft`; ro `tagName`
  (`DIV` in an `.html` document, as in the DOM; lower case in `.rml`), `localName`, `content`
  (a `<template>`'s fragment), `parentElement`, `children`,
  `firstElementChild`, `lastElementChild`, `nextElementSibling`, `previousElementSibling`,
  `childElementCount`, `scrollWidth`, `scrollHeight`, `clientWidth`, `clientHeight`,
  `offsetWidth`, `offsetHeight`, `offsetLeft`, `offsetTop`, `classList`, `style`, `dataset`;
  methods `getAttribute`, `setAttribute`, `hasAttribute`, `removeAttribute`, `getAttributes`,
  `querySelector`, `querySelectorAll`, `closest`, `matches`, `contains`, `appendChild`,
  `insertBefore`, `removeChild`, `replaceChild`, `remove`, `cloneNode([deep])`, `focus`,
  `blur`, `click`, `insertAdjacentHTML(position, html)`, `show()`, `showModal()`,
  `close([returnValue])` (a `<dialog>`), `scrollIntoView`, `scrollTo`, `scrollBy`,
  `getBoundingClientRect` (UIRect),
  `addEventListener`, `removeEventListener`, `dispatchEvent(type[, detail[, bubbles]])`.
  `<lottie>` elements also: `play()`, `pause()`, `stop()`, `seek(seconds)`, `setFrame(f)`,
  `setSpeed(s)`, `setDirection(1|-1)`, `setLoop(b)`, `setLoopCount(n)`,
  `setSegment(from, to)`, `setMarker(name)`, `isPlaying()`, `getCurrentFrame()`,
  `getTotalFrames()`, `getDuration()` (events `load`, `error`, `play`, `pause`, `loop`,
  `complete`).
- `classList:add(...)`, `remove(...)`, `toggle(name[, force])`, `contains(name)`;
  `style.fontSize` / `style["font-size"]` (computed value; assign the inline style, `""`/nil
  removes; standard CSS values in an `.html` document), `style.cssText`;
  `dataset.userName` (`data-user-name`).
- A document's own `<script type="text/lua">` (inline or `src`) runs while playing after every
  load and reload of the document, with `document` (its `UIDocument`) and `anime` bound and its
  own environment (set globals through `_G`).
- `UIEvent` (valid while the listener runs): `type`, `target`, `currentTarget`, `eventPhase`,
  `key`, `code`, `keyCode`, `["repeat"]`, `ctrlKey`, `shiftKey`, `altKey`, `metaKey`,
  `clientX`, `clientY`, `button` (0 left, 1 middle, 2 right), `deltaX`, `deltaY`, `data`,
  `detail`, `defaultPrevented`; `stopPropagation()`, `stopImmediatePropagation()`,
  `preventDefault()`.
- Data model proxy: fields read/write the model (`model.gold = 1`, `model.items[1].name`,
  `table.insert(model.items, t)`); top-level keys fixed; methods `onChange(name, fn)`,
  `removeOnChange(id)`, `dirty(name)`, `dirtyAll()`, `get(path)`, `set(path, v)`,
  `getName()`, `isValid()`. `data-for` indices are 0-based.
- `anime` (global table, Anime.js v4): `anime.animate(targets, params)`,
  `createTimeline(params)`, `createTimer(params)`, `createSpring(params)`,
  `stagger(value[, params])`, `cubicBezier`, `steps`, `linear`, `irregular`, `anime.eases.*`,
  `anime.utils.*`; targets are selectors (documents of every scene of the game), `UIElement`s,
  plain tables or lists; animations have `play`, `pause`, `resume`, `restart`, `reverse`,
  `seek(ms)`, `cancel`, `revert`, `complete`, `andThen(fn)` and `currentTime`, `progress`,
  `paused`, `completed`, `cancelled`. Full reference: `jasper://docs/html-ui` ("Animation").

---

### 4.19 Lattice (GameObject) - free-form deformation

A box cage of control points that bends, squashes or wobbles the meshes bound to it
(meshes, models, sprites, polygons, tilemaps). Local space is the unit cube -0.5..0.5,
sized and placed by the Transform (`scale` = size). Point `(i, j, k)` counts from 0 per axis,
flat index `i + rx * (j + ry * k)`; offsets are local units from the rest positions, 0 at
rest. Concepts, selectors, presets and recipes: `jasper://docs/lattice`.

```lua
local cage = Lattice(self.scene)           -- entity + Transform + LatticeComponent
cage.entityOwned = false                   -- or keep the wrapper alive in self
cage.position = Vector3(0, 1, 0); cage.scale = Vector3(2, 2, 2)
cage:bind(self.entity)                     -- this mesh follows the cage (weight 1)
cage:setOffset(1, 2, 1, Vector3(0, 0.3, 0)) -- pull the top centre up
cage:savePose("tall"); cage:resetOffsets()
cage:tweenToPose("tall", 0.4, EaseType.BACK_OUT)
```

| Member | Notes |
|---|---|
| `enabled`, `interpolation` (`LatticeInterpolation`), `outside` (`LatticeOutside`), `fadeDistance` | rw; `setInterpolation`, `setOutside`, `setFadeDistance` too |
| `setResolution(x, y, z)` | each 2..16 (default 3); resamples offsets, poses and a running tween |
| `getResolution()` (Vector3), `getResolutionX/Y/Z()`, `getPointCount()`, `getPointIndex(i, j, k)` | |
| `getOffset(i, j, k)`, `setOffset(i, j, k, v)`, `getOffsetAt(index)`, `setOffsetAt(index, v)` | local units; out of range: logged, ignored |
| `getRestPosition(i, j, k)`, `getPointPosition` / `setPointPosition(i, j, k, v)` | local space (rest + offset) |
| `getPointWorldPosition` / `setPointWorldPosition(i, j, k, v)` | world space (last frame's Transform) |
| `resetOffsets()` | all zero |
| `applyPreset(name, axis, amount [, toward])` | `"squash"`, `"stretch"`, `"taper"`, `"twist"`, `"bend"`, `"bulge"`, `"shear"`, `"noise"`; axis `"x"`/`"y"`/`"z"`, toward also `"-x"`...; added to the offsets; bend/twist angles are true in the world (the cage's proportions) |
| `savePose(name)`, `hasPose(name)`, `removePose(name)`, `applyPose(name [, weight])` | `applyPose`: offsets = pose * weight |
| `tweenToPose(name, seconds [, EaseType])`, `tweenToRest(seconds [, EaseType])`, `isTweening()`, `stopTween()` | runs while playing; ends exactly on the target |
| `jiggle` (`setJiggle(b)`), `jiggleStiffness`, `jiggleDamping`, `jiggleInertia`, `jiggleAnchor` (`LatticeJiggleAnchor`), `jiggleMaxOffset` | rw; springs driven by the cage's acceleration, while playing |
| `poke(i, j, k, impulseV3)` (local units/s), `pokeAt(worldPoint, worldImpulse, radius)` | wake the jiggle; `isJiggleSleeping()`, `getJiggleOffset(i, j, k)`, `resetJiggle()` (after a teleport) |
| `deformPoint(worldPoint)` | world in, world out |
| `bind(goOrEntity [, weight])`, `unbind(goOrEntity)` | binding in the target's `LatticeModifierComponent` (added when missing, with `includeChildren` when the target has no mesh); weight 0..2; a child's own binding replaces the one it inherits (weight 0 excludes it) |
| `addFollower(follower, rest [, applyPosition, applyRotation, applyScale, weight])`, `removeFollower(follower)` | the follower is placed where `rest` ends up through the cage each frame (a prop on a squashed surface); refused when the follower is the rest or an ancestor of it |
| `getLatticeComponent()`, `getModifierComponent(entity)`, `getFollowerComponent(entity)` | raw components (nil when missing); their `std::vector` fields read as copies |

Writing offsets stops a running tween. Tweens and jiggle do not run in the editor's edit mode;
offsets, poses and followers show there too. Scripts must not edit `comp.offsets[i]` (a copy):
use the methods. Physics colliders keep the rest shape.

## 5. Enums (exact Lua spelling; values are integers)

| Namespace | Values |
|---|---|
| `BodyType` | `STATIC`, `KINEMATIC`, `DYNAMIC` |
| `Collider2DType` | `POLYGON`, `CIRCLE`, `CAPSULE`, `SEGMENT`, `CHAIN` |
| `Collider3DType` | `SPHERE`, `BOX`, `CAPSULE`, `TAPERED_CAPSULE`, `CYLINDER`, `CONVEX_HULL`, `MESH`, `HEIGHTFIELD` |
| `RigidbodyMotionQuality` | `DISCRETE`, `LINEAR_CAST` |
| `Joint2DType` | `DISTANCE`, `REVOLUTE`, `PRISMATIC`, `MOUSE`, `WHEEL`, `WELD`, `MOTOR` |
| `JointAnchorSpace` | `WORLD`, `LOCAL` |
| `JointBreakAction` | `IGNORE`, `CALLBACK_ONLY`, `DISABLE`, `DESTROY` |
| `JointLimitState2D` | `INACTIVE`, `LOWER`, `UPPER`, `EQUAL` |
| `CollisionDetectionMode2D` | `DISCRETE`, `CONTINUOUS` |
| `RigidbodySleepMode2D` | `NEVER_SLEEP`, `START_AWAKE`, `START_ASLEEP` |
| `PhysicsMaterialCombine` | `AVERAGE`, `GEOMETRIC_MEAN`, `MINIMUM`, `MULTIPLY`, `MAXIMUM` |
| `Joint3DType` | `FIXED`, `DISTANCE`, `POINT`, `HINGE`, `CONE`, `PRISMATIC`, `SWINGTWIST`, `SIXDOF`, `PATH`, `GEAR`, `RACKANDPINON`, `PULLEY` |
| `CameraType` | `CAMERA_UI`, `CAMERA_ORTHO`, `CAMERA_PERSPECTIVE` |
| `LightType` | `DIRECTIONAL`, `POINT`, `SPOT` |
| `EaseType` | `LINEAR`, `STEP`, `QUAD_IN/OUT/IN_OUT`, `CUBIC_*`, `QUART_*`, `QUINT_*`, `SINE_*`, `EXPO_*`, `CIRC_*`, `ELASTIC_*`, `BACK_*`, `BOUNCE_*` (each `_IN`, `_OUT`, `_IN_OUT`), `CUSTOM` |
| `ActionState` | `Running`, `Paused`, `Stopped` |
| `AudioSourceState` | `Playing`, `Paused`, `Stopped` |
| `AudioSourceAttenuation` | `NO_ATTENUATION`, `INVERSE_DISTANCE`, `LINEAR_DISTANCE`, `EXPONENTIAL_DISTANCE` |
| `AnchorPreset` | `NONE`, `TOP_LEFT`, `TOP_RIGHT`, `BOTTOM_LEFT`, `BOTTOM_RIGHT`, `CENTER_LEFT`, `CENTER_TOP`, `CENTER_RIGHT`, `CENTER_BOTTOM`, `CENTER`, `LEFT_WIDE`, `TOP_WIDE`, `RIGHT_WIDE`, `BOTTOM_WIDE`, `VERTICAL_CENTER_WIDE`, `HORIZONTAL_CENTER_WIDE`, `FULL_LAYOUT` |
| `PivotPreset` | `CENTER`, `TOP_CENTER`, `BOTTOM_CENTER`, `LEFT_CENTER`, `RIGHT_CENTER`, `TOP_LEFT`, `BOTTOM_LEFT`, `TOP_RIGHT`, `BOTTOM_RIGHT` |
| `LayoutGroupType` | `VERTICAL`, `HORIZONTAL`, `VERTICAL_WRAP`, `HORIZONTAL_WRAP` |
| `ScrollbarType`, `ProgressbarType` | `VERTICAL`, `HORIZONTAL` |
| `ParticleEmitterShape` | `Box`, `Sphere`, `Hemisphere`, `Circle`, `Cone` |
| `FogType` | `LINEAR`, `EXPONENTIAL`, `EXPONENTIALSQUARED` |
| `RotationOrder` | `XYZ`, `XZY`, `YXZ`, `YZX`, `ZXY`, `ZYX` |
| `RayFilter` | `BODY_2D`, `BODY_3D`, `BOUNDS` |
| `Scaling` | `FITWIDTH`, `FITHEIGHT`, `LETTERBOX`, `CROP`, `STRETCH`, `NATIVE` |
| `Platform` | `MacOS`, `iOS`, `Web`, `Android`, `Linux`, `Windows` |
| `CursorType` | `ARROW`, `IBEAM`, `CROSSHAIR`, `POINTING_HAND`, `RESIZE_EW`, `RESIZE_NS`, `RESIZE_NWSE`, `RESIZE_NESW`, `RESIZE_ALL`, `NOT_ALLOWED`, `WAIT`, `PROGRESS`, `HELP`, `GRAB`, `GRABBING` |
| `MouseMode` | `NORMAL`, `HIDDEN`, `CAPTURED`, `CONFINED` |
| `UIEventState` | `NOT_SET`, `ENABLED`, `DISABLED` |
| `TouchPhase` | `Began`, `Moved`, `Stationary`, `Ended`, `Canceled` |
| `UIDocumentRenderMode` | `SCREEN`, `TEXTURE` |
| `TextureFilter` | `NEAREST`, `LINEAR`, `NEAREST_MIPMAP_NEAREST`, `NEAREST_MIPMAP_LINEAR`, `LINEAR_MIPMAP_NEAREST`, `LINEAR_MIPMAP_LINEAR` |
| `TextureWrap` | `REPEAT`, `MIRRORED_REPEAT`, `CLAMP_TO_EDGE`, `CLAMP_TO_BORDER` |
| `PrimitiveType` | `TRIANGLES`, `TRIANGLE_STRIP`, `POINTS`, `LINES` |
| `MaterialAlphaMode` | `AUTO`, `ALPHA_OPAQUE`, `MASK`, `BLEND` |
| `MaterialLighting` | `AUTO`, `PIXEL`, `VERTEX`, `MIXED`, `NONE` |
| `LightShading` | `BOTH`, `PIXEL`, `VERTEX` |
| `LightState` | `OFF`, `ON`, `AUTO` |
| `ShadowQuality` | `NONE`, `LOW`, `MEDIUM`, `HIGH` |
| `SceneColorScale` | `FULL`, `HALF` |
| `TonemapMode` | `NONE`, `ACES`, `NEUTRAL`, `REINHARD`, `AGX` (`PostProcessingSettings.tonemap`) |
| `MonochromeMode` | `GRAYSCALE`, `SEPIA` (`PostProcessingSettings.monochromeMode`) |
| `MaterialBlendMode` | `AUTO`, `NONE` (opaque), `ALPHA`, `PREMULTIPLIED`, `ADDITIVE`, `MULTIPLY` |
| `MaterialCullMode` | `AUTO`, `BACK`, `FRONT`, `OFF` |
| `MaterialDepthWrite` | `AUTO`, `ENABLED`, `DISABLED` |
| `MaterialDepthTest` | `AUTO`, `LESS`, `LESS_EQUAL`, `EQUAL`, `GREATER_EQUAL`, `GREATER`, `NOT_EQUAL`, `ALWAYS` |
| `MaterialPassOrder` | `AFTER`, `BEFORE` |
| `MaterialStencilCompare` | `AUTO`, `DISABLED`, `NEVER`, `LESS`, `EQUAL`, `LESS_EQUAL`, `GREATER`, `NOT_EQUAL`, `GREATER_EQUAL`, `ALWAYS` |
| `MaterialStencilOp` | `KEEP`, `ZERO`, `REPLACE`, `INCR_CLAMP`, `DECR_CLAMP`, `INVERT`, `INCR_WRAP`, `DECR_WRAP` |
| `MaterialColorWrite` | `AUTO` (-1), `NONE` (0), `R` (1), `G` (2), `B` (4), `A` (8), `RGB` (7), `ALL` (15); any sum of `R`, `G`, `B`, `A` |
| `ShadowCaster2DShape` | `AUTO_QUAD`, `POLYGON` |
| `ReflectionProbeMode` | `STATIC`, `DYNAMIC` |
| `ReflectionProbeUpdateMode` | `ON_LOAD`, `ON_MOVE`, `INTERVAL`, `MANUAL` |
| `FluidOutput` | `SCREEN`, `MATERIAL`, `TEXTURE` (`FluidSimulation.output`) |
| `FrustumPlane` | `FRUSTUM_PLANE_NEAR`, `_FAR`, `_LEFT`, `_RIGHT`, `_TOP`, `_BOTTOM` |
| `GraphicBackend` | `GLCORE`, `GLES3`, `D3D11`, `METAL`, `WGPU` |
| `AdMobRating` | `General`, `ParentalGuidance`, `Teen`, `MatureAudience` |
| `ResourceLoadState` | `NotStarted`, `Loading`, `Finished`, `Failed` |
| `LatticeInterpolation` | `SHARP`, `SMOOTH`, `CUBIC` |
| `LatticeOutside` | `EXTEND`, `FADE` |
| `LatticeJiggleAnchor` | `NONE`, `NEGATIVE_X`, `POSITIVE_X`, `NEGATIVE_Y`, `POSITIVE_Y`, `NEGATIVE_Z`, `POSITIVE_Z` |
| `LatticeStage` | `BEFORE_SKINNING`, `AFTER_SKINNING` |
| `LatticeMaskChannel` | `NONE`, `RED`, `GREEN`, `BLUE`, `ALPHA`, `INVERTED_RED`, `INVERTED_GREEN`, `INVERTED_BLUE`, `INVERTED_ALPHA` |
| `LineRendererMode` | `Segments`, `Polyline` (`LineRenderer.mode`) |
| `LineWidthUnit` | `World`, `Pixels` |
| `LineAlignment` | `View`, `TransformZ` |
| `LineTextureMode` | `Stretch`, `Tile`, `DistributePerSegment`, `RepeatPerSegment` |
| `CurveWrap` | `Clamp`, `Loop`, `PingPong` (`AnimationCurve.preWrapMode` / `postWrapMode`) |
| `GradientMode` | `Blend`, `Fixed` |

There is no separate Rigidbody2D type enum: both Rigidbody2D and Rigidbody use `BodyType`.
There is no `Collider2D`/`Collider3D` class: colliders are created/queried through the
Rigidbody wrappers by index.

---

## 6. Pitfalls

1. **Colon vs dot.** Methods use `:` (`sound:play()`, `go:setPosition(1, 2)`). Static
   functions and properties use `.` (`GameObject.instantiate(...)`, `Engine.deltatime`,
   `Input.isKeyPressed(...)`, `Log.print(...)`, `Vector3.ZERO`). `Engine:deltatime()` or
   `Input:isKeyPressed(k)` are wrong.
2. **Properties are not methods.** A getter bound with `addProperty` is a field:
   `sprite.width`, `go.position`, `text.text`, `body.linearVelocity`, `Engine.canvasWidth`.
   `sprite:getWidth()`, `go:getPosition()`, `text:getText()`, `Engine.getCanvasWidth()`
   do NOT exist and raise "attempt to call a nil value". Conversely, some things are only
   methods: `body2d:getMass()`, `sound:isPlaying()`, `sound:getLength()`,
   `tilemap:getWidth()`, `go:getWorldPosition()`.
3. **Mass spelling differs.** 2D: `rb2d:getMass()` (method, read-only). 3D: `rb.mass`
   (property, read/write).
4. **Property values are copies.** `go.position.x = 5` changes a temporary copy and has no
   effect. Read, modify, assign back: `local p = go.position; p.x = 5; go.position = p`.
5. **`self.entity` is a number**, not an object. Wrap it: `GameObject(self.scene, self.entity)`,
   `Sprite(self.scene, self.entity)`, ... Calling `self.entity:setPosition` errors.
6. **One-argument constructors create entities.** `Sprite(self.scene)` makes a NEW sprite
   entity every call. Do not call it in `onUpdate` to "get" an existing object.
7. **`GameObject:getRigidbody2D()` creates the component** if it is missing; use
   `Rigidbody2D(self.scene, self.entity)` to wrap an existing body.
8. **Angles are degrees** by default (`Engine.useDegrees == true`): `go:setRotation(0, 0, 45)`,
   `Quaternion(0, 0, 45)`, `rb2d.angle`, `Quaternion:getRoll()`. Lua `math.sin/cos/atan`
   use radians: convert with `math.rad`/`math.deg` or `Angle.degToRad`/`Angle.radToDeg`.
9. **Forces in `onFixedUpdate`, without deltatime.** Forces are applied per physics step;
   do not multiply by `Engine.deltatime` (which is the frame delta even inside
   `onFixedUpdate`; the fixed step is `Engine.updateTime`). Impulses are one-off and can be
   applied anywhere. 2D forces/impulses are in Box2D units (kg, meters); positions and
   velocities read/written through Rigidbody2D are in points (`pointsToMeterScale`, default 64).
10. **Pass every argument.** LuaBridge ignores C++ default arguments. A missing trailing
    `bool` silently becomes `false` (e.g. `wake` in `applyForceToCenter(f)` -> body not woken);
    a missing number/vector raises an error. Write `applyForceToCenter(f, true)`,
    `tween:setAction(a, b, 1.0, false)`.
11. **Teleporting physics bodies**: set the body (`rb2d.position = Vector2(...)`,
    `rb.position = Vector3(...)`) rather than the GameObject transform; also reset
    `linearVelocity` if needed.
12. **Vector arithmetic**: `v * 2`, `2 * v`, `v / 2` and `v + w` work; `v + 1` does not (no
    vector + number). Use the bound `Vector3(...)` constructor, not plain tables, when an API
    needs a vector (plain tables are only for `default` values in `properties`).
13. **Event methods must exist before registering**, and the name passed to
    `RegisterEngineEvent` must be an Engine event name (`"onUpdate"`, not `"update"`).
    Define the handler on the module table (`function M:onUpdate() ... end`).
14. **Do not assign Engine events** (`Engine.onUpdate = function() end`): it bypasses the
    per-script subscription (not removed when the script is destroyed, survives play
    sessions). Always use `RegisterEngineEvent`.
15. **Held vs pressed**: `Input.isKeyPressed` / `isMousePressed` / `getKey` report the held
    state; "pressed this frame" is `Input.getKeyDown`, `getMouseButtonDown`, `getButtonDown`
    (read edges in `onUpdate`: every fixed step of a frame sees them). Prefer input actions
    (`Input.getAxis("Horizontal")`) to raw keys: they add the gamepad and on-screen controls.
16. **Physics events are global** for the scene: compare `bodyA.entity` / `bodyB.entity`
    with `self.entity`. 2D contact events require the per-collider event flags.
17. **Unassigned references are `nil`**: an entity-reference or Prefab property not set in
    the Inspector is `nil`; guard (`if self.target then ... end`).
18. **Scripts by class name, not `getComponent`**: `go:getScript("Enemy")` (same GameObject:
    `self:getScript(...)`), `getScriptInChildren/InParent`, or across scenes
    `GameObject.findObjectOfType("Enemy")`. A property typed with the script's class name
    (`type = "Enemy"`) assigned in the Inspector is the cheapest; `GameObject.instantiate(scriptSelf, ...)`
    also returns the copy's script.
19. **Canvas size in `init`**: `Engine.canvasWidth/Height` can be 0 in exported builds during
    `init`; use `Engine.preferredCanvasWidth/Height` there.
20. **`Log.*` take a single string**; build messages with `..`/`tostring`/`string.format`.
21. **Animation Player callbacks take one value**: `player.onEvent = function(event) ... end`
    (`event.functionName`, `event.args[1]`), not `function(name, args)`; `onFinish` gets the
    state name. `GameObject:getAnimationPlayer()` is `nil` without the component (unlike
    `getRigidbody2D`, it never adds one).
22. **`onDestroy` runs on every end**: `GameObject.destroy`, a scene unload (`SceneManager.loadScene`,
    also of the same scene) and Play stop / quit (after `onApplicationQuit` and `onDisable`), only
    for scripts whose `init` ran. Do not create objects there. Event subscriptions are cleaned up
    automatically.
23. **Colors** on wrappers (`setColor`, `color`, Button colors) are sRGB 0..1 values;
    raw component fields (`UIComponent.color`, `ButtonComponent.colorNormal`) are linear.
24. **Integers**: entity ids, indices and enum values are integers; do not pass fractional
    numbers to them (`math.floor` first).
25. **Json: an empty table is `[]`.** Use `Json.object()` (or `{emptyTable = "object"}`) for an
    empty object. A table with `nil` in the middle is not an array (`{1, nil, 3}` encodes as an
    object `{"1":1,"3":3}`): store `Json.null` to keep a place. `Json.decode` gives integers for
    `1` and floats for `1.0`; compare JSON nulls with `Json.null`, never `nil`.
26. **Saves go to `data://`.** `asset://` and `lua://` are read-only (and inside the APK on
    Android); `File.*` and `Json.*File` return `nil, message` on file problems instead of
    raising: check the first result. On Web, Lua `io` writes need `System.syncFileSystem()`.
27. **`self.enabled` and the reserved names**: `self.enabled = false` disables the script
    (`onDisable`, its registered updates stop) but not what it started; it takes only `true` /
    `false`. `enabled`, `isActiveAndEnabled` and `gameObject` cannot be `properties` names, and
    `isActiveAndEnabled` / `gameObject` cannot be assigned. A disabled script's `init` still runs.
28. **Unity method names are not called**: `Awake` -> `init`, `Start` -> `start`, `OnEnable` ->
    `onEnable`, `Update` -> `onUpdate` registered with `RegisterEngineEvent(self, "onUpdate")`
    (`FixedUpdate` -> `onFixedUpdate`, `LateUpdate` -> `onPostUpdate`). `check_script` warns.
