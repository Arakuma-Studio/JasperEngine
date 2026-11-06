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
| `start(self)` | once, at the start of the frame after its first `onEnable`, before that frame's `onFixedUpdate`/`onUpdate`; not run while disabled or inactive (it waits), nor for a GameObject `GameObject.destroy` is about to remove (destroyed in `init`, a singleton's duplicate). A script's registered updates begin only after its `start`: one enabled during a frame gets its first `onUpdate` in the next frame, after `start` | `Start` |
| `onDisable(self)` | the script stops being active and enabled: `self.enabled = false`, its GameObject or a parent deactivated, destroy, scene unload, Play stop; always before `onDestroy` | `OnDisable` |
| `onDestroy(self)` | once: `GameObject.destroy` (end of frame) / `destroyImmediate`, scene unload (a `SceneManager.loadScene`), Play stop / application quit; only if `init` ran | `OnDestroy` |
| `onApplicationPause(self, paused)` | the app goes to the background (`true`) and returns (`false`); scripts of active GameObjects, enabled or not | `OnApplicationPause` |
| `onApplicationFocus(self, focused)` | the app lost / gained the focus (platforms that report it). Order: focus `false` before pause `true`, pause `false` before focus `true`; the same moments as `Engine.onFocus` / `onPause` / `onResume` ([platform-services.md](platform-services.md)) | `OnApplicationFocus` |
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
| Coroutines, timers | `self:startCoroutine(fn)` and `self:invoke(...)` belong to the instance: deactivating the GameObject stops its coroutines (not its timers), destroying it or unloading its scene ends both (2.17). |
| Release | After `onDestroy` (or for a GameObject never active, without it): subscriptions made through `RegisterEngineEvent`/`RegisterEvent` and `Event` listeners the instance owns are removed. |

Script messages besides the lifecycle (defined methods, no registration; every script of the
GameObject that defines it gets the call, enabled or not, after the fixed step that caused it):

| Method | When |
|---|---|
| `onCollisionEnter(self, collision)`, `onCollisionStay`, `onCollisionExit` | a collider of this GameObject's Rigidbody started / keeps / stopped touching another (`Collision`, seen from this side; 4.14.1) |
| `onTriggerEnter(self, other)`, `onTriggerStay`, `onTriggerExit` | a trigger pair (either side a trigger / sensor): `other` is the other `Collider` |
| `onCollisionEnter2D(self, collision)`, `...Stay2D`, `...Exit2D`, `onTriggerEnter2D(self, other)`, `...Stay2D`, `...Exit2D` | the same for Rigidbody 2D (`Collision2D`, `Collider2D`; 4.13.2) |
| `onJointBreak2D(self, joint)` | a `Joint2D` of this GameObject, or whose bodyA is it, broke (its `breakForce` / `breakTorque` passed; `joint` still exists during the call, its `breakAction` decides what follows) |
| `onParticleSystemStopped(self)` | a version 2 `ParticleSystem` of this GameObject with `main.stopAction = ParticleSystemStopAction.Callback` finished (stopped and its last particle gone; never in an editor preview) |

The contact callbacks come once per fixed step that steps (2D first, then 3D; triggers, then
collisions; Enter, Stay, Exit), only while the GameObject is active; a collider's `receiver`
("Send Callbacks To") also gets them. Sleeping bodies get no Stay and no Exit; when the other side
is destroyed, deactivated or its collider removed you still get the Exit. Rules and data:
`jasper://docs/physics` (Collision and trigger callbacks). They cannot yield.

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
| `"asset"`, `"assetref"` (with `assetType = "texture"`, `"model"`, `"audio"`, ...) | `AssetRef`, or `nil` when left None | a path (`"textures/hero.png"`), or none |
| `"entity"`, `"entitypointer"`, `"pointer"` | the first Lua script instance (enabled or not) on the referenced GameObject if it has one, otherwise an `EntityHandle`; `nil` if unassigned | none |
| any other string = a wrapper class name (see list below) | an instance of that wrapper class for the referenced entity; `nil` if unassigned | none |
| any other string = a Lua script class name (e.g. `"Enemy"`) | that GameObject's `Enemy` script instance table (its `self`) | none |

Wrapper class names accepted as `type` (case-insensitive): `GameObject`, `Camera`, `Light`,
`Light2D`, `ShadowCaster2D`, `CapsuleShadowCaster`, `Mesh`, `Primitive`, `Mirror`, `Terrain`, `Tilemap`, `Model`,
`MeshPolygon`, `Points`, `LineRenderer`, `TrailRenderer`, `Sprite`, `AudioSource`, `RectTransform`,
`LayoutGroup`, `Polygon`, `Text`, `Image`, `Button`, `Panel`, `Scrollbar`, `Progressbar`,
`InputField`, `Bone`, `Fog`, `LocalVolumetricFog`, `VolumetricClouds`, `Skybox`, `ReflectionProbe`, `FluidSimulation`, `Rigidbody2D`, `Joint2D`, `CharacterController2D`,
`Rigidbody`, `Joint3D`, `CharacterController`, `Action`, `Tween`, `AlphaTween`, `ColorTween`, `PositionTween`,
`RotationTween`, `ScaleTween`, `Animation`, `ParticleSystem`, `SpriteAnimation`,
`KeyframeTracks`, `MorphTracks`, `LatticeTracks`, `RotateTracks`, `ScaleTracks`, `TranslateTracks`, `Spline`, `SplineExtrude`,
`SplineInstantiate`, `SplineAnimate`, `EntityHandle`.
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
| `onPause`, `onResume`, `onShutdown` | none | application lifecycle (pause = to the background; idempotent) |
| `onFocus` | `hasFocus` | the app gained or lost focus (a dialog, the notification shade, another window); `onFocus(false)` comes before `onPause`, `onFocus(true)` after `onResume` |
| `onOrientationChanged` | `orientation` (`ScreenOrientation`) | the interface rotated, after `onCanvasChanged` |
| `onSafeAreaChanged` | none | `Screen.safeArea` changed (rotation, platform insets, `Screen.setExtraInsets`), after `onCanvasChanged` / `onOrientationChanged` |
| `onLowMemory` | none | the system is short of memory (Android, iOS); `Device.onLowMemory` follows at the next frame, after the engine cleared its caches |
| `onSceneLoaded` | none | a scene loaded by SceneManager is ready |
| `onKeyDown`, `onKeyUp` | `key, isRepeat, mods` | key event (`key` is an `Input.KEY_*` value) |
| `onCharInput` | `char` | a character was typed: a one-character UTF-8 string, one call per character (an emoji is one call on every platform) |
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
| `AudioSource(...):getAudioSourceComponent()` | `onStart` (play, playOnAwake, a resume), `onPause`, `onStop` (`stop()`, `fadeOut`, the natural end of a non-looping clip, a voice stolen) | none |
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

Prefer the per-object callbacks (`onCollisionEnter`, `onTriggerEnter2D`..., 1.1): they come only
to the GameObjects involved. These physics events are scene-wide: they fire for ANY pair of
bodies. Filter in the handler, e.g.
`if bodyA.entity == self.entity or bodyB.entity == self.entity then ... end`. They run on the main
thread after the step (2D: also `stayContact2D`, `staySensorContact2D`, same arguments as the
begin ones); 3D gets a `Contact3D` snapshot whose setters apply from the next step; with
`physicsSystem.synchronousContactEvents3D = true` (a Lua `shouldCollide3D` turns it on) they run
inside the step with live contacts.
2D contact events need "Contact Events" (sensor events: sensor + "Sensor Events", hit: "Hit
Events") enabled on the Collider 2D (Inspector, or `body.shapeContactEvents = true`,
`body.shapeSensorEvents = true`, `body.shapeEnableHitEvents = true`).
Contact and activation events (2D and 3D) are delivered on the main thread right after the
physics step that found them, inside that fixed step, in the same order on every run: a handler
may move, create or destroy bodies. The filters (`preSolve2D`, `shouldCollide2D`,
`shouldCollide3D`) answer inside the step; with a `shouldCollide3D` handler the 3D step runs on the
main thread only (slower for big worlds).

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


### 1.7 UI pointer callbacks (Unity's EventSystem)

The component UI (RectTransform elements with an Image) sends Unity's pointer messages to scripts
by method name, `e` a `PointerEventData`. Each finger and mouse button is its own pointer: two
fingers press two buttons, lifting one releases only its own. A message goes to the element under
the pointer, else its first ancestor whose script defines the method (ExecuteHierarchy).

| Method | When |
|---|---|
| `onPointerEnter(e)` / `onPointerExit(e)` | the pointer comes over / leaves the element or a child (every GameObject on the way gets it; a finger enters on its down and leaves with its up) |
| `onPointerDown(e)` / `onPointerUp(e)` | pressed; released or cancelled (`e.canceled`): the up goes to the GameObject that got the down |
| `onPointerClick(e)` | released over the pressed GameObject without a drag (`e.clickCount`: 2 for a double click within 0.3 s) |
| `onInitializePotentialDrag(e)` | on the down, to the drag handler (set `e.useDragThreshold = false` to start the drag at once) |
| `onBeginDrag(e)`, `onDrag(e)`, `onEndDrag(e)` | past 4 px; each move; the release or cancel |
| `onDrop(e)` | released while dragging, to the handler under the pointer (`e.pointerDrag` is what was dragged) |
| `onScroll(e)` | the wheel over it (`e.scrollDelta`) |

`PointerEventData` (read-only unless noted): `pointerId` (-1 / -2 / -3 the mouse's left / right /
middle button, a finger's `Touch.fingerId`), `button` (`Input.MOUSE_BUTTON_*`), `isTouch`,
`position`, `delta`, `pressPosition` (canvas px, y down, as `Input.getMousePosition`),
`localPosition` (the receiving element's own), `uiPosition`, `uiDelta`, `scrollDelta`,
`clickCount`, `clickTime`, `dragging`, `eligibleForClick`, `canceled`, `useDragThreshold` (rw),
`used`, `use()` (a used down is not handled by the built-in Button, Panel, Scrollbar and
InputField), and the GameObjects (or nil) `pointerEnter`, `pointerPress`, `rawPointerPress`,
`pointerDrag`, `target` (the one receiving the call).

```lua
---@class Slot : ScriptInstance
local Slot = {}
function Slot:onBeginDrag(e) self.start = self.gameObject.position end
function Slot:onDrag(e) local p = self.gameObject.position; self.gameObject.position = Vector3(p.x + e.delta.x, p.y + e.delta.y, p.z) end
function Slot:onDrop(e) Log.print(e.pointerDrag.name .. " dropped on " .. self.gameObject.name) end
return Slot
```

- The right and middle mouse buttons reach these callbacks (`e.button`); Button, Panel, Scrollbar
  and InputField react to the left button and fingers only.
- A press ends cancelled (`onPointerUp` with `e.canceled`, `onEndDrag`, no click, no drop) when the
  touch is cancelled, released outside the canvas, the app pauses or loses focus, or the pressed
  element is deactivated or disabled (`Button.disabled` too).
- The legacy component events (`ui.onPointerDown(x, y)`, `onClick`, `onDrag`...) still fire per
  pointer, on the element the pointer pressed only; `ui:getPointerEventData()` gives the event of
  the one running. A method subscribed with `RegisterEvent` to such an event (the old pattern) is
  not also called with `e` under the same name while subscribed.
- `Input.isPointerOverUI([pointerId])`: the pointer (none: the mouse or any finger) is over UI that
  takes it, or a press there holds it; check it before world clicks (Unity's
  `EventSystem.IsPointerOverGameObject`).
- C++ scripts override `onPointerDown(PointerEventData& e)` ... on `ScriptBase`.
- Gestures (pinch, rotate) are two pointers: keep each finger's `position` by `pointerId`.

---

## 2. Globals and singletons

### 2.1 Engine (all static: `Engine.x`, `Engine.fn()`)

| Name | Kind | Notes |
|---|---|---|
| `deltatime` | ro | seconds since last frame (clamped, times `timeScale`). Same value inside `onFixedUpdate` (`Time.deltaTime` gives the fixed step there, 2.16). |
| `unscaledDeltatime`, `unscaledTime` | ro | the frame delta before `timeScale` (0 while paused or held) and its sum: `Time.unscaledDeltaTime` / `Time.unscaledTime` |
| `maxDeltatime` | rw | cap of a frame's delta (default 0.25 s; values <= 0 are ignored with a warning): `Time.maximumDeltaTime` |
| `updateTime` | rw | fixed step in seconds (default 1/60), Unity's `Time.fixedDeltaTime`. Also `Engine.setUpdateTimeMS(ms)` |
| `timeScale` | rw | game speed (Unity's `Time.timeScale`): scales the frame delta, physics steps, actions, animations, tweens, particles. 1 = real time, 0 = frozen (frames still draw, `onUpdate` runs with delta 0). Clamped to [0, 100] |
| `fixedDeltatime` | rw | when > 0, every frame advances exactly this many seconds (before `timeScale`) whatever the wall clock measured: reproducible runs. 0 (default) = measured frame time. NOT the fixed step (that is `updateTime`): Unity's `Time.captureDeltaTime` |
| `gameTime` | ro | scaled seconds the update loop has run since start (not while paused or held) |
| `frameCount` | ro | frames the update loop has run since start (not while paused or held) |
| `interpolationAlpha` | ro | fixed-step interpolation factor: how far the frame is between the last fixed step and the next (bodies are drawn this far from their pose before the last step) |
| `framerate` | ro | fps (measured) |
| `targetFrameRate` | rw | Unity's `Application.targetFrameRate`: -1 (default) = the platform's rate (60 on mobile, every animation frame on the web, vsync-paced on desktops); desktops and the editor ignore it while `vSyncCount > 0`. Kept across scene loads |
| `vSyncCount` | rw | Unity's `QualitySettings.vSyncCount` 0..4 (present every Nth vblank); starts from the project's VSync; mobile and web ignore it |
| `isFocused` | ro | Unity's `Application.isFocused` (true at start) |
| `isDevelopmentBuild` | ro | Unity's `Debug.isDebugBuild`: true in the editor's Play and in exports built with the Debug build type |
| `systemTime` | ro | |
| `canvasWidth`, `canvasHeight` | ro | current canvas; can be 0 in `init()` of exported builds |
| `preferredCanvasWidth`, `preferredCanvasHeight` | ro | project canvas size; safe in `init()` |
| `setCanvasSize(w, h)` | static fn | |
| `viewRect` | ro | `Rect` |
| `scalingMode` | rw | `Scaling.*` |
| `setAtmosphereQuality(q)`, `getAtmosphereQuality()`, `resetAtmosphereQuality()` | fn | the volumetric fog and clouds tier (`AtmosphereQuality.*`): overrides the project's per-platform one until reset |
| `textureStrategy` | rw | `TextureStrategy.*` |
| `setTextureQuality(q)`, `getTextureQuality()` | static fn | 0 (full) to 3 (an eighth): top mips dropped from streamable textures at load (Unity's Global Mipmap Limit); sprites and UI keep theirs. The export sets the platform's from Project Settings > Texture |
| `setTextureMemoryBudget(mb)`, `getTextureMemoryBudget()`, `getTextureMemoryUsage()` | static fn | MB of resident textures (0 = unlimited); over it the largest streamable textures drop mips (each keeps 64 px). Usage is in bytes |
| `setTextureLowMemoryFallback(bool)`, `isTextureLowMemoryFallback()` | static fn | RGB565 / RGBA4444 instead of RGBA8 when a GL device samples none of a texture's formats |
| `isTextureFormatSupported(name)` | static fn | `"BC7"`, `"ASTC_6x6"`, `"ETC2_RGBA8"`...: the GPU samples it with filtering (`jasper://docs/textures`) |
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
| `isViewLoaded()`, `startAsyncThread()`, `commitThreadQueue()`, `endAsyncThread()`, `isAsyncThread()` | static fn | |
| `getQueuedResourceCount()` | static fn | asset jobs queued or running (reads included): 0 only once loading is idle |
| `setMaxResourceLoadingThreads(n)` | static fn | the most workers asset (Background) jobs take at once, applied live; 0 = derived from the reserved frame workers |
| `framebuffer` | rw | |
| `clearAllSubscriptions()`, `clearComponentSubscriptions()` | static fn | do not call from gameplay scripts |
| event objects (`onUpdate`, `onFixedUpdate`, ... see 1.3) | static props | use via `RegisterEngineEvent` |

### 2.2 Input (all static)

Functions:

| Function | Returns |
|---|---|
| `Input.isKeyPressed(key)`, `Input.getKey(key)` | bool, key currently held |
| `Input.getKeyDown(key)`, `Input.getKeyUp(key)` | bool, pressed / released this frame (both true for a tap inside one frame; the fixed step's window in `onFixedUpdate`) |
| `Input.isMousePressed(button)`, `Input.getMouseButton(button)` | bool, button held |
| `Input.getMouseButtonDown(button)`, `Input.getMouseButtonUp(button)` | bool, this frame |
| `Input.getGamepadButtonDown(id, button)`, `Input.getGamepadButtonUp(id, button)` | bool, this frame |
| `Input.getMouseDelta()` | `Vector2`, canvas pixels moved this frame |
| `Input.getMouseScrollDelta()` | `Vector2`, wheel offsets of this frame |
| `Input.getTouchCount()`, `Input.getTouch(i)` | this frame's touches (0-based, nil out of range): the fingers down, then those lifted in the frame: `{pointer, position, phase, delta, startPosition, time, began}` plus Unity's fields (below) |
| `Input.getTouchDown(pointer)`, `Input.getTouchUp(pointer)` | bool, this frame |
| `Input.getPointerLockState()` | `Input.POINTER_UNLOCKED` / `POINTER_PENDING` / `POINTER_LOCKED` |
| `Input.isTouch()` | bool, any touch active |
| `Input.isMouseEntered()` | bool |
| `Input.getMousePosition()` | `Vector2` |
| `Input.getMouseScroll()` | `Vector2`, deprecated: the last wheel event, never reset (use `getMouseScrollDelta`) |
| `Input.getTouchPosition(pointer)` | `Vector2` |
| `Input.numTouches()` | integer |
| `Input.findTouchIndex(pointer)` | integer |
| `Input.getTouches()` | list of the fingers down now, each a table `{pointer, position, phase, delta, startPosition, time, began}` |
| `Input.isGamepadConnected(id)`, `Input.getGamepadName(id)`, `Input.isGamepadButtonPressed(id, button)`, `Input.getGamepadAxis(id, axis)`, `Input.numGamepads()`, `Input.getGamepadId(index)` | gamepad |
| `Input.getModifiers()` | bitmask of `Input.MODIFIER_*` |
| `Input.deviceOrientation` (property) | `DeviceOrientation.*`: how the device is held; `Unknown` on desktops and flat Android devices |

Unity-style polling with edges, touches and input actions (details: `jasper://docs/input`).
`key` is an `Input.KEY_*` code or a control name (`"space"`, `"a"`, `"left_shift"`,
`"mouse_left"`, `"gamepad_a"`; Unity spellings like `"left ctrl"`, `"joystick button 0"` too):

| Function / property | Returns |
|---|---|
| `Input.getKey(key)`, `Input.getKeyDown(key)`, `Input.getKeyUp(key)` | bool: held / pressed this frame / released this frame |
| `Input.getMouseButton(b)`, `getMouseButtonDown(b)`, `getMouseButtonUp(b)` | bool; `b` 0 left, 1 right, 2 middle |
| `Input.getGamepadButtonDown(id, button)`, `getGamepadButtonUp(id, button)` | bool; `id` -1 any pad; `button` `Input.GAMEPAD_BUTTON_*` or a name |
| `Input.anyKey`, `Input.anyKeyDown` (properties) | bool: any key, mouse or gamepad button held / pressed this frame. `Input.anyKeyDown()` works too (S22's spelling: a boolean called without arguments gives itself back) |
| `Input.inputString` (property) | UTF-8 text typed this frame (`"\b"` backspace, `"\n"` return) |
| `Input.mouseDelta`, `Input.mouseScrollDelta` (properties) | `Vector2`: movement this frame (canvas px, y down; works captured), wheel steps (y up) |
| `Input.getPressedControl()` | string or nil: the first control pressed this frame (`"space"`, `"mouse_left"`, `"gamepad_a"`, `"left_x+"`) |
| `Input.touchCount`, `Input.touches` (properties), `Input.getTouch(index)` (0-based), `Input.getTouchById(fingerId)` | Unity's touches this frame (ended/cancelled ones included in their last frame): `Touch` or nil |
| `Input.multiTouchEnabled` (rw, default true), `Input.touchSupported` | false: only the first finger is reported / `System.isTouchDevice()` |
| `Input.isPointerOverUI([pointerId])` | bool: the pointer (-1 the mouse, a finger's `fingerId`; none: the mouse or any finger) is over UI that takes events (component UI or HTML content) or held by it (1.7) |
| `Input.getAxis(name)`, `Input.getAxisRaw(name)` | number: the action's smoothed / raw value (-1..1, button 0..1; mouse movement unclamped). `name`: an action of the actions asset's "Input Manager" map, else the first of that name by map priority, or `"Map/Action"` |
| `Input.getButton(name)`, `getButtonDown(name)`, `getButtonUp(name)` | bool |
| `Input.getVector(xName, yName)` | `Vector2`, length <= 1 |
| `Input.hasAction(name)`, `Input.getActionNames()` | bool / string list |
| `Input.setActionEnabled(name, enabled)`, `Input.isActionEnabled(name)` | a disabled action reads 0 / false |
| `Input.simulateButton(name, pressed [, source])`, `Input.simulateAxis(name, value [, source])`, `Input.addAxisDelta(name, delta [, source])`, `Input.clearSimulated([source])` | virtual sources (like an on-screen control); `source` a name (default `"script"`) or `self` (cleared with the instance) |
| `Input.getBindings(name)` | list of binding tables (`kind`, `positive`, ...) |
| `Input.setBindings(name, bindings)`, `Input.resetBindings([name])` | `true` or `false, error` / runtime rebinding over the asset's bindings (dropped at Play stop) |
| `Input.exportBindings()`, `Input.importBindings(json)` | the overrides as JSON text / `true` or `false, error` |

Default actions (Unity's Input Manager, the "Input Manager" map of a project without an actions
asset): `Horizontal`, `Vertical`, `Fire1`, `Fire2`, `Fire3`, `Jump`, `Mouse X`, `Mouse Y`,
`Mouse ScrollWheel`, `Submit`, `Cancel`. An unknown action name logs one error and reads 0 / false.

`Touch` (a table): `fingerId` (= `pointer`), `phase` (`TouchPhase.Began`, `Moved`, `Stationary`,
`Ended`, `Canceled`: the values of `Input.TOUCH_*`), `position`, `startPosition`, `deltaPosition`
(= `delta`, canvas px, y down), `deltaTime` (unscaled s since it last moved), `tapCount`, `time`
(s since it touched), `began`.

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
- Touch phases: `Input.TOUCH_BEGAN`, `TOUCH_MOVED`, `TOUCH_STATIONARY`, `TOUCH_ENDED`,
  `TOUCH_CANCELED`. Pointer lock: `Input.POINTER_UNLOCKED`, `POINTER_PENDING`, `POINTER_LOCKED`.
  Frames and fixed steps: `jasper://docs/input`.
- Gamepad buttons: `Input.GAMEPAD_BUTTON_A`, `_B`, `_X`, `_Y`, `_LEFT_BUMPER`,
  `_RIGHT_BUMPER`, `_BACK`, `_START`, `_GUIDE`, `_LEFT_THUMB`, `_RIGHT_THUMB`, `_DPAD_UP`,
  `_DPAD_RIGHT`, `_DPAD_DOWN`, `_DPAD_LEFT`, `_LAST`, `_CROSS`, `_CIRCLE`, `_SQUARE`,
  `_TRIANGLE` (full names `Input.GAMEPAD_BUTTON_<X>`).
- Gamepad axes: `Input.GAMEPAD_AXIS_LEFT_X`, `_LEFT_Y`, `_RIGHT_X`, `_RIGHT_Y`,
  `_LEFT_TRIGGER`, `_RIGHT_TRIGGER`, `_LAST`.

### 2.2.2 InputActions (static) and InputAction

Maps of actions bound to controls, from an `.inputactions` asset (`jasper://docs/input`).

| Function | Notes |
|---|---|
| `InputActions.load(path)`, `loadDefault(path)`, `unload()`, `isLoaded()`, `getAssetPath()` | a path in the assets directory |
| `InputActions.get("Map/Action")` | `InputAction` or nil |
| `InputActions.getMap(name)` | `InputActionMap`: `enabled` (rw), `priority` (rw), `gamepad` (rw, -1 any), `enable()`, `disable()`, `setGamepad(id)`, `getActions()` |
| `InputActions.enableMap(name, on)`, `isMapEnabled`, `setMapPriority`, `getMapPriority`, `setGamepad(map, id)`, `getGamepad` | |
| `InputActions.setVirtual(name, number or Vector2)`, `getVirtual(name)` | `virtual/<name>` controls |
| `InputActions.bindVirtualStick(element, name)`, `bindVirtualButton(element, name)`, `unbindVirtualControl(element)` | HTML UI on-screen controls |
| `InputActions.inject(action, number or Vector2, windows)`, `releaseInjected(action)` | 0 windows: a tap, -1: until released |
| `InputActions.startRebind(action, bindingId, {groups, exclude, cancel, timeout}, fn(result))`, `cancelRebind()`, `isRebinding()` | `result.status` completed, canceled or timed_out; `result.path` |
| `InputActions.getConflicts(action, bindingId)` | list of `{action, id, path, groups}` |
| `InputActions.getBindingPath(id)`, `setOverride(id, path)`, `removeOverride(id)`, `saveOverrides()`, `loadOverrides()`, `resetOverrides()` | overrides persist in UserSettings |
| `InputActions.getBinding(id)` | `InputBinding`: `id`, `path`, `displayName`, `composite`, `overridden`, `getDisplayName()`, `getGroups()`, `getParts()` |
| `InputActions.getBindingDisplayName(id)`, `getControlDisplayName(path)`, `isControlBound(path)` | "Space", "Ctrl+S", "Left Stick" |
| `InputActions.defaultDeadzone` | rw, 0.15 |

`InputAction`: `readValue()`, `readVector2()`, `isPressed()`, `wasPressedThisFrame()`,
`wasReleasedThisFrame()`, `getHeldTime()`, `getPhase()` / `phase` (`InputActionPhase.WAITING`,
`STARTED`, `PERFORMED`, `CANCELED`), `name`, `mapName`, `path`, `type` (`InputActionType.BUTTON`,
`AXIS`, `VECTOR2`), `consumedBy`, `getBindings()`, and the events `onStarted`, `onPerformed`,
`onCanceled` (assign a function; it gets an `InputActionEvent`: `action`, `phase`, `value`,
`vector2`, `consume()`).

### 2.2.1 Jobs (static, read-only)

The engine's job system runs asset loading and engine work on worker threads. Lua states are
single-threaded, so scripts never submit jobs; these only report.

| Function | Returns |
|---|---|
| `Jobs.getWorkerCount()` | worker threads (0 in single-thread mode: web without threads) |
| `Jobs.isSingleThreaded()` | bool |
| `Jobs.getQueuedCount(lane)` | jobs queued in `JobLane.High`, `JobLane.Normal` or `JobLane.Background` |
| `Jobs.getRunningCount(lane)` | jobs running in that lane |

`ThreadPoolManager` is deprecated (one release): `ThreadPoolManager.getInstance():getQueueSize()`
returns the Background lane's queue; `initialize` and `shutdown` warn once and do nothing.

### 2.3 Log (static) and DebugOverlay

`Log.print(...)` (= `Log.info(...)`), `Log.verbose(...)` (trace), `Log.debug(...)` (debug builds
only), `Log.warn(...)`, `Log.error(...)`: any values, joined with tabs (`tostring`), like Lua's
`print`, which logs an info line too. Every line goes to the engine log: the editor's Console (Play
and edit time), the platform log of an exported game (logcat, the terminal), the on-device console
and its ring buffer. Lines of Lua carry a Lua traceback per Project Settings > Debug > Log Stack
Traces (default: warnings and errors); a Lua error's `stack traceback:` is split off into
`stackTrace`.

Lua's records are in the `lua` category and carry the chunk and line they were logged from
(the Console's details pane, `read_console`'s `source`). More than 200 identical messages a
second are held back and summed up in one `suppressed N similar messages` line.

- `Log.category(name)`: a category of your own, created on first use, with
  `:trace(s) :debug(s) :info(s) :warn(s) :error(s)` (`local net = Log.category("net")`,
  `net:warn("lag")`). The Console filters by category.
- `Log.setLevel(level [, category])`: the lowest level logged (`"trace"`, `"debug"`, `"info"`,
  `"warn"`, `"error"`, `"fatal"`, `"off"`) for `lua` or the named category; false for an
  unknown level name. `Log.getLevel([category])` reads it back. A line below it reaches nothing,
  the buffer included.

The buffer and its listeners see every record of the engine log (C++ `JE_LOG_*` and `Log::*` of
any category too, from any thread):

| Name | Kind | Notes |
|---|---|---|
| `Log.onMessageReceived` | event | Unity's `Application.logMessageReceived`: `RegisterEvent(self, Log.onMessageReceived, "onLog")`, `function M:onLog(message, stackTrace, level)`; main thread (worker-thread lines at the next frame start); lines logged inside a handler are not re-dispatched |
| `Log.getEntries([maxCount [, levels]])` | fn | buffered lines, oldest first (the newest `maxCount`; `levels` a list of `LogLevel`): `{id, category, level, message, stackTrace, time, frame, mainThread, fromLua}` |
| `Log.count(level)` | fn | lines of that level since the last clear |
| `Log.clear()` | fn | empties the buffer and the counts |
| `Log.bufferSize` | rw | lines kept (default 500, Project Settings > Debug; 0 = none, at most 100000) |
| `LogLevel.TRACE` (= `VERBOSE`), `DEBUG`, `INFO`, `WARN`, `ERROR`, `FATAL` | enum | 0 - 5, the engine log's one level scale: compare with the names, not numbers |

`DebugOverlay` (static), the on-device console and stats overlay (Unity's IngameDebugConsole and
Graphy), details in `jasper://docs/debugging`:

| Name | Kind | Notes |
|---|---|---|
| `enabled` | rw | master switch (initial value: Project Settings > Debug > Overlay and the build type); works in any build |
| `showConsole()`, `hideConsole()`, `toggleConsole()` | fn | no-ops (warned once) while not enabled |
| `consoleVisible` | ro | |
| `statsMode` | rw | `DebugOverlay.STATS_OFF`, `STATS_FPS`, `STATS_FULL` (another value raises an error) |
| `getStats()` | fn | `{fps, frameMs, frameMsAvg, frameMsMin, frameMsMax, frameMs1PercentLow, drawCalls, instances, triangles, processMemoryBytes, luaMemoryBytes, audioVoices, audioVoicesMax, bodies2D, bodies3D, entities, scenes, coroutines, timers, htmlUiDrawCalls}`; works while not enabled |
| `addCommand(name, description, fn [, owner])` | fn | console command `name args...`: `fn(args)` gets a list of strings; `[a-z0-9_-]+`; removed with its script (`owner`, default the calling method's `self`) |
| `removeCommand(name)`, `executeCommand(line)` | fn | |

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
```

```lua
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

Platform services ([platform-services.md](platform-services.md) has the per-platform details):

| Function | Returns |
|---|---|
| `System.openURL(url)` | bool: the system took it (browser, store, `mailto:`); `javascript:`/`data:`/`file:`/`vbscript:` and scheme-less URLs refused. Web: a blocked popup opens on the next click (false) |
| `System.getSystemLanguage()` | `SystemLanguage.*` (Unity's `Application.systemLanguage`) |
| `System.getLanguageCode()`, `getRegionCode()`, `getLocale()` | `"es"`, `"MX"` (the platform's region setting first; `""` unknown), `"es-MX"` |
| `System.getPreferredLanguages()` | `{"es-MX", "en-US"}` (BCP 47, most preferred first) |
| `System.getAppVersion()`, `getAppBuild()`, `getAppIdentifier()`, `getAppName()` | Player version, build (integer: Android versionCode, iOS `CFBundleVersion`), identifier, name of the running app |
| `System.getOperatingSystem()` | Unity's `SystemInfo.operatingSystem` format (`"Android OS 14 / API-34 (...)"`, `"iOS 17.1"`) |
| `System.getOperatingSystemVersion()`, `getAndroidApiLevel()` | `"14"`, 34 (0 off Android) |
| `System.getDeviceModel()`, `getDeviceType()` | `"Google Pixel 8"`, `DeviceType.*` |
| `System.getSystemMemorySize()`, `getProcessorCount()` | MiB (0 unknown), logical processors |
| `System.isEditor()` | true only in the editor's Play mode |
| `System.getInternetReachability()` | `NetworkReachability.*` (a network is there, not that a server answers; not every frame) |
| `System.vibrate([ms])` | bool; 400 ms by default, iOS ignores the length |
| `System.hapticFeedback(HapticFeedback.*)` | bool |

### 2.5.1 Screen (static)

| Name | Kind | Notes |
|---|---|---|
| `Screen.orientation` | rw | `ScreenOrientation.*`: get = how the interface is shown now (never `AutoRotation`); set = a fixed orientation or `AutoRotation` |
| `Screen.autorotateToPortrait`, `autorotateToPortraitUpsideDown`, `autorotateToLandscapeLeft`, `autorotateToLandscapeRight` | rw | bools: where `AutoRotation` may turn (the last one cannot be turned off) |
| `Screen.sleepTimeout` | rw | `SleepTimeout.NeverSleep` (-1, keeps the screen on) or `SleepTimeout.SystemSetting` (-2) |
| `Screen.dpi`, `Screen.refreshRate` | ro | dots per inch (0 unknown), Hz |
| `Screen.safeArea` | ro `Rect` | the part no notch, rounded corner or home indicator covers: canvas units, top-left origin, extra insets included |
| `Screen.safeAreaInsets` | ro `EdgeInsets` | canvas units from each canvas edge |
| `Screen.deviceSafeArea`, `Screen.deviceSafeAreaInsets` | ro | without the extra insets |
| `Screen.safeAreaPixels` | ro `Rect` | view pixels, top-left, extras included |
| `Screen.cutouts` | ro `Rect[]` | canvas units; empty when none or unknown (iOS) |
| `Screen.extraInsets` | ro `EdgeInsets` | canvas units |
| `Screen.setExtraInsets(l, t, r, b [, unit])` | fn | space kept free (a banner): `unit` `"canvas"` (default) or `"pixels"`; persists across scene loads |
| `Screen.clearExtraInsets()` | fn | |

Safe areas: the Safe Area component, HTML `env(safe-area-inset-*)`, editor device simulation:
jasper://docs/safe-area.

The pixel size is `System.getScreenWidth()` / `getScreenHeight()`; the canvas `Engine.canvasWidth` /
`canvasHeight`. `LandscapeLeft` = the device turned counter-clockwise from portrait.

Files (for saves prefer `File.readText` / `File.writeText` / `Json.readFile`, 2.4.2):
`File()` / `File(path, write)` with `:open`, `:flush`, `:close`, `:readString()`,
`:writeString(s)`, `:read8/16/32()`, `:eof()`, `:length()`, `:seek(n)`, `:pos()`;
`Data(path)` reads asset data; `FileData.getBaseDir(p)`, `FileData.getFilePathExtension(p)`,
`FileData.getSystemPath(p)`. Constants `FileErrors.FILEDATA_OK`, `INVALID_PARAMETER`,
`FILE_NOT_FOUND`, `OUT_OF_MEMORY`.

`require("folder.module")` loads `folder/module.lua` from the Lua directory (or its `lua/`
subfolder). Use it for shared helper modules.

### 2.5.2 Device (static): Device Tier, thermal state, memory warnings

| Name | Kind | Notes |
|---|---|---|
| `Device.tier` | ro | `DeviceTier.Low`, `Medium`, `High`, `Ultra`: the tier in effect (set, simulated or detected) |
| `Device.gpuName`, `Device.tierReason` | ro | the renderer name, and why it got its tier |
| `Device.tierApplied` | ro | false in the editor until the Game view simulates a tier or a script sets one |
| `Device.thermalState` | ro | `ThermalState.Nominal`, `Fair`, `Serious`, `Critical` (desktops and the web: Nominal) |
| `Device.setTier(tier)`, `Device.resetTier()` | fn | a graphics menu's tier for the rest of the run, and back |
| `Device.onThermalStateChanged` | event | `function(state)` |
| `Device.onLowMemory` | event | a low-memory warning was handled (caches cleared, streamed textures a mip lower) |

```lua
Device.onThermalStateChanged = function(state)
    if state == ThermalState.Critical then
        Device.setTier(DeviceTier.Low)
    end
end
```

Details: jasper://docs/device.

### 2.6 GameObject.instantiate / destroy (static, Unity-style)

```lua
GameObject.instantiate(original)                              -- -> GameObject or nil
GameObject.instantiate(original, parent --[[, worldSpace]])    -- parent: GameObject or a script self
GameObject.instantiate(original, position, rotation --[[, parent]]) -- Vector3, Quaternion (world)
GameObject.instantiate(prefab, scene --[[, position, rotation]]) -- prefab into a given scene
GameObject.destroy(obj --[[, delaySeconds]])                   -- deferred to end of frame
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

### 2.7.1 Asset / AssetRef (asset identity)

Every asset has a GUID that survives renames and moves (`jasper://docs/assets`). `Asset` answers
from the editor's asset database in Play and from the build's `assetmap.bin` in exported games.
Paths are relative to the assets directory for textures, models, audio, fonts, UI documents and
data files, and to the project for scenes, prefabs, materials, shaders and scripts.

| Name | Kind |
|---|---|
| `Asset.path(guid)` | static fn: the current path, or `nil` (a former GUID or a sub-asset's resolves too) |
| `Asset.guid(path)` | static fn: the GUID, or `nil` |
| `Asset.exists(guidOrPath)` | static fn: boolean |
| `Asset.ref(guidOrPath)` | static fn: an `AssetRef` |
| `guid`, `path`, `type` (`"texture"`, `"model"`, ...), `valid` | `AssetRef` ro properties |
| `isValid()`, `==`, `tostring()` | `AssetRef` method, metamethods |

Component path fields (`AudioSource.filename`, `Model.filename`...) stay plain strings in Lua.

### 2.8 SceneManager (static)

`SceneManager.loadScene(idOrName)`, `addChildScene(idOrName)`, `removeChildScene(idOrName)`,
`preloadScene(idOrName)`, `getPreloadProgress(idOrName)`, `cancelPreload(idOrName)`,
`setLoadingScene(idOrName)`, `holdLoading()`, `releaseLoading()`, `isCoveredByLoading(...)`,
`getSceneId(name)`, `getSceneName(id)`, `getSceneNames()`, `getScenePtr(id)`,
`clearAll()`. Properties (ro unless noted): `sceneCount`, `currentSceneId`,
`currentSceneName`, `loadPending`, `loadingSceneId`, `loading`, `loadingProgress`,
`loadingDelay` (rw), `loadingTimeout` (rw), `persistentScene` (the DontDestroyOnLoad scene, nil
until the first `DontDestroyOnLoad`). `moveGameObjectToScene(obj, scene)` (2.8.1).

### 2.8.1 Persistent objects (DontDestroyOnLoad)

```lua
function AudioManager:init()
    DontDestroyOnLoad(self)          -- or GameObject.dontDestroyOnLoad(self) / (self.gameObject)
end

-- a manager placed in every scene (Unity's `if (instance == null) ... else Destroy(gameObject)`)
function AdsManager:init()
    if GameObject.persistSingleton(self) ~= self then return end
    -- the one manager from here on
end
```

> **Cameras draw their own scene.** Do not make a level's camera persistent: in the persistent
> scene it draws only the persistent objects, and the level falls back to a default camera (a
> warning says so). Keep a Camera in every level and persist a manager that finds and positions
> it after each load (`GameObject.findObjectOfType("Camera")` in its `onSceneLoaded` engine event, or
> the level's camera script asking the manager). A camera brought by a persistent GameObject
> becomes the persistent scene's camera (for persistent world objects or UI).

| Function | Semantics |
|---|---|
| `DontDestroyOnLoad(obj)` / `GameObject.dontDestroyOnLoad(obj)` | `obj`: a script instance (its GameObject) or any GameObject wrapper. Its **root** GameObject, with its children, components and running state, moves into the persistent scene and survives every `SceneManager.loadScene`. Returns true. A GameObject with a parent: warning "DontDestroyOnLoad only works for root GameObjects", nothing happens (false). `nil`: ignored. Already persistent: nothing. In the editor outside Play: nothing (false). |
| `GameObject.isPersistent(obj)` | in the persistent scene, or marked to move there (true from the call on) |
| `GameObject.persistSingleton(self)` | the instance of `self`'s class that survives: another one already on a persistent GameObject (or marked earlier, also in the same start pass) is returned and `self`'s GameObject is destroyed (at the end of the frame); else `self` is made persistent and returned |
| `SceneManager.persistentScene` | the persistent `Scene` (named "DontDestroyOnLoad"), or nil before the first `DontDestroyOnLoad`; `GameObject.instantiate(prefab, SceneManager.persistentScene)` instantiates straight into it (check for nil) |
| `SceneManager.moveGameObjectToScene(obj, scene)` | moves a root GameObject to a running scene (`Engine.scene`, a layer) or to the persistent scene; out of the persistent scene it stops persisting (the next load destroys it). Error logged, false: not a root, "the target scene is not loaded" |

Rules:
- **The move is a real move**: the same script instance tables (fields, other scripts' references
  to them), the same Lua state, sounds play on (the voice is kept), Rigidbody / Rigidbody2D keep
  their velocities (the body is made again in the persistent scene's world), running actions,
  tweens, particles, Animation Players keep going, subscriptions to its component events stay.
  No lifecycle method runs for it (Unity). Entity ids are kept when free; otherwise the
  GameObject gets a new id and `self.entity` changes with it. Reloaded as when copied: the GPU
  state (one frame), UI documents (their DOM and data models start again), fluids, Vertex
  Animators and Trail Renderers restart (a trail's points are cleared).
- **When it moves**: never inside a callback. Called in `init`/`onEnable` of a scene being
  loaded: at the end of that scene's start, before its HTML documents start (the usual case,
  nothing is lost). From `onUpdate`, coroutines, timers, an instantiated object's `init`: after
  this frame's update. Between frames (input callbacks, tools): at the start of the next frame.
  Always before a scene load tears the scenes down. `self.scene` and `self.entity` change when
  it moves; `self.gameObject` follows.
- **Wrappers** a script made before the move for its own GameObjects (in its fields:
  `self.body = Rigidbody2D(self.scene, self.entity)`) follow the move; wrappers kept elsewhere
  (locals, other tables, other scripts) still name the old scene: use `self.gameObject` or wrap
  again.
- **Drawing**: the persistent scene is a scene layer above the loaded scenes (and their layers
  and child scenes) and below the loading scene: persistent UI covers the game, the loading
  screen covers everything. It draws with a default UI camera, or the first Camera a persistent
  GameObject brings (see the camera note above). `Engine.scene` is always the loaded main scene,
  never the persistent one (also in a new level's `init`). It clears its own depth: persistent world objects draw on top of the level
  (move them into the level with `moveGameObjectToScene` to depth-sort them with it).
- **Physics**: the persistent scene has its own worlds (default gravity, set it with
  `SceneManager.persistentScene:setGravity2D(...)`): persistent bodies collide only with
  persistent bodies. A joint to a body that stays behind is removed (warning).
- **Audio**: 2D sounds (music) are untouched; 3D sounds are heard by the loaded main scene's
  camera (the persistent scene places the listener there every frame while one plays).
- **Subscriptions**: what a level script subscribes to on a persistent GameObject (its Button's
  `onPress`...) goes when that script is released (its scene unloads), Lua and C++ alike.
- **Events**: subscriptions to the old scene's events (`self.scene:getPhysicsSystem()...`) stay
  on that scene until it unloads: register scene-wide events on `self.scene` after the move (or
  use the per-object callbacks). Engine events (`RegisterEngineEvent`) keep working and follow
  the GameObject's active state.
- **Play stop / quit** destroys the persistent objects after the quit pass (`onApplicationQuit`,
  `onDisable`, `onDestroy`); nothing persists into edit mode. The persistent scene stays (empty)
  until then once made. `Engine.removeAllScenes()` (low level) takes it off the screen until
  the next load adds it again.
- **Finds** (`GameObject.find*`, `findObjectOfType`) see persistent objects; the editor's
  Hierarchy lists them under a "DontDestroyOnLoad" row in Play, MCP as the running scene
  "DontDestroyOnLoad" (`persistent: true`).

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
| `ambientLight2DColor`, `ambientLight2DIntensity`, `globalIlluminationColor`, `globalIlluminationIntensity`, `maxLightsPerObject` (1-16; the other lights reaching an object go into its ambient), `litQuality` (`LitQuality.STANDARD` / `HIGH`: the built-in Lit's specular anti-aliasing and direct-light energy compensation; new scenes are High, scenes saved before it Standard), `physicalLightEV100` (the exposure, -6..24, lights in lux, candela or lumen are drawn at: Sunny 15, Overcast 12, Interior 7, Night 2), `shadowQuality` (`ShadowQuality` AUTO/NONE (hard only)/LOW/MEDIUM/HIGH/ULTRA, the shadow quality tier), `shadowDistance` (metres, 0: the tier's), `shadowFadeFraction`, `shadowCascadeBlend`, `capsuleShadowsEnabled`, `capsuleShadowIntensity` (0..1), `capsuleShadowSoftness` (0..2; `jasper://docs/lighting`), `effectiveShadowTier` (read-only `ShadowTier` LOW/MEDIUM/HIGH/ULTRA; `jasper://docs/shadows`), `lightState`, `shadow2DQuality`, `sceneColorScale` (`SceneColorScale`: resolution of the scene-colour copy that shaders requiring scenecolor sample; `HALF` default), `postProcessing` (a `PostProcessingSettings` copy: edit its fields, then assign it back, `local p = scene.postProcessing; p.tonemap = TonemapMode.ACES; p.fxaaEnabled = true; scene.postProcessing = p`; enums `TonemapMode` NONE/ACES/NEUTRAL/REINHARD/AGX, `MonochromeMode` GRAYSCALE/SEPIA; fields as `get_scene_settings` `post_processing.<field>` in camelCase; `scene.postProcessing = PostProcessingSettings()` turns everything off; costs and presets: `jasper://docs/post-processing`), `postProcessPasses` (a list of `PostProcessPass`: `shader`, `enabled`, `scale` 0.1-1, `setUniform`/`getUniform`, `setTexture(uniform, path)`/`getTexture`; assign the list back; `jasper://docs/post-processing`), `enableUIEvents` | rw properties (partial list; also ssao*/ssr*/fixedResolution*/postProcess* rendering options) |
| `getPhysicsSystem()`, `getAudioSystem()`, `getActionSystem()`, `getMeshSystem()`, `getRenderSystem()`, `getUISystem()` | subsystem objects (below) |
| `findEntitiesWith({"Transform", "MeshComponent"}[, {"UIComponent"}])` | -> table of Entity with every listed component and none of the excluded ones, in the order of the first one's array (parents before children for `Transform`); names are the component class names |
| `getChangeTick()` | -> integer: the scene's change tick; a change made after the call is stamped with a newer one |
| `findChangedEntities("Transform", sinceTick)` | -> table of Entity whose component changed after `sinceTick` (added, set through a wrapper setter, a component field, the editor or `markChanged`) |
| `markChanged(e, "MeshComponent")` | -> bool: stamps the component changed after a write the engine cannot see; false when `e` has none |
| `getSignature(e)` | -> 256-character string of `0`/`1`, MSB first (bit 0 is the last character); `EntityManager:setSignature` also takes the 64-character strings of older scripts |
| `getCanvasScaler()` | the scene's `CanvasScaler` (UI scale, `jasper://docs/ui`): rw `uiScaleMode` (`UIScaleMode.FollowGameCanvas`/`ConstantPixelSize`/`ScaleWithScreenSize`/`ConstantPhysicalSize`), `scaleFactor`, `referenceResolution` (Vector2), `screenMatchMode` (`ScreenMatchMode.MatchWidthOrHeight`/`Expand`/`Shrink`), `matchWidthOrHeight` (0 width..1 height), `physicalUnit` (`PhysicalUnit.Centimeters`/`Millimeters`/`Inches`/`Points`/`Picas`), `fallbackScreenDPI`, `useProjectDefault`; ro `canvasSize` (Vector2, UI units now), `appliedScaleFactor`. Reads give what applies; setting a field of a scene on the project default overrides from it. Applies from the next frame |

`PhysicsSystem` (from `self.scene:getPhysicsSystem()`): `gravity` / `gravity2D` /
`gravity3D` (rw), `interpolation` (rw, as `scene.physicsInterpolation`), `pointsToMeterScale2D` (rw),
`subSteps2D` (rw, 1-16, as `scene.physics2DSubSteps`), event objects (section 1.3),
`createRigidbody2D(e)`, `removeRigidbody2D(e)`, `createRigidbody(e)`, `removeRigidbody(e)`,
`removeSubscriptionsByTag(substring)` (the scene's physics event callbacks whose tag holds it),
`clearSubscriptions()` (all of them, every script's) (partial).

The other subsystems (from `self.scene:getActionSystem()`, `getMeshSystem()`, ...):

| Object | Members |
|---|---|
| `ActionSystem` | `actionStart(e [, newRun])` (newRun default true: a stopped action starts a new run; false resumes inside its old one), `actionStop(e)`, `actionPause(e)`, `getDuration(e)` (seconds: a tween, an animation's frames, a keyframe track), `getFrameDuration(frame)` (an `ActionFrame` of an animation) |
| `AudioSystem` (on the scene) | `stopSceneSounds()`: this scene's sounds (`AudioSystem.stopAll()`: every scene's) |
| `MeshSystem` | `loadGLTF(e, path [, async [, skipEntities [, changeRootTransform [, bakeSceneRoot]]]])` (changeRootTransform default true: the entity takes the file's root transform), `hasPendingAsyncModelLoads()`, `isAsyncModelLoadPending(e, path)`, `cancelAsyncModelLoad(e, path)`, `cancelAsyncModelLoads()` |
| `RenderSystem` | `isAllLoaded()` (everything the scene waits for has loaded), `getLoadCount()` -> `loaded, total` (a loading screen's progress) |
| `UISystem` | `parentToLayoutSpace(e, v2)`, `layoutToParentSpace(e, v2)` (an anchored element's offset is in its layout parent's space, its position in its parent's), `hasPendingImages()`, `resetButtonStates()` (every button unpressed, pointers released) |

`AudioSystem` (static): `AudioSystem.stopAll()`, `pauseAll()` / `resumeAll()` (the mixer's Master
bus holds, as in the app pause but apart from it; a source a script paused stays paused), `AudioSystem.globalVolume` (rw,
= `AudioListener.volume`), `preload(path [, loadType])` (decode or open a clip ahead),
`isOutputAvailable()` (false without an audio device: sounds still mix, meters still read),
`getVoiceCount()`.

`Audio` (static, free voices with no AudioSource): `Audio.play(eventOrClip [, opts])` -> `SoundHandle`
(a Sound Event by GUID or `.soundevent` path, or a clip path; `opts = {target = GameObject (3D, follows
it, stops when it is destroyed), position = Vector3 (3D, fixed), scene = Scene (the owner: it holds
and stops with it; default the target's, else the main scene), volume = 1, pitch = 1, group = "SFX"
(default the event's, else Master), delay = 0 (s), unscaled = false (the delay's time)}`; an unknown
name gives an invalid handle and one warning per name per Play), `Audio.stopAll([group])` -> integer
(the voices of `Audio.play`, triggers and one-shots, all or those on `group` or below it;
`AudioSystem.stopAll()` also stops the sources' main voices). Inside a physics callback they run at
the frame's audio update: the handle is valid at once, `stopAll` answers 0.
`Audio.mark(label)` writes a marker line (at most 200 bytes) at this sample of an audio capture and does
nothing without one; `Audio.isCapturing()` -> boolean. Both exist in every build (a release player: no-op
and false), so a script behaves the same everywhere.

```lua run
Audio.mark("doc test")
assert(type(Audio.isCapturing()) == "boolean")
```

The music (one track that survives scene loads; `jasper://docs/sound` Music): `Audio.playMusic(eventOrClip [, opts])`
-> bool (a clip, or a Sound Event played as a playlist; `opts = {crossfade = 0 (s, equal power), sync = "now" | "beat"
| "bar" | "section_end" (the playing track's next one), snapshot = "Combat" (reached at the switch), snapshotSeconds
(default the crossfade), group (default the event's output, else `audio.music_group`), volume = 1,
ignoreListenerPause = false}`; the playing track again does nothing; false for a `layer` event),
`Audio.stopMusic([fade [, {sync}]])`, `Audio.advanceMusic()` -> bool (the section ends at its next loop end),
`Audio.playStinger(eventOrClip [, sync = "beat"])` -> `SoundHandle` (on the next beat or bar, owned by the music),
`Audio.getMusic()` -> string, `Audio.getMusicTime()` -> seconds, `Audio.getMusicPosition()` -> `{section, bar, beat,
beatFraction, bpm, beatsPerBar}` or nil (no music or no tempo). `Audio.onMusicBeat` (bar, beat) and
`Audio.onMusicSection` (section, clip) are called in the frame whose mix holds them: frame-accurate; a stinger or a
synced switch is the sample-exact way. A time that is negative or not a number, or an unknown `sync`, is refused
with one warning.

```lua run
assert(Audio.getMusic() == "")
assert(Audio.getMusicPosition() == nil)
assert(Audio.advanceMusic() == false)
Audio.onMusicBeat:add("doc", function(bar, beat) end)
Audio.onMusicBeat:remove("doc")
```

`SoundHandle` (a value naming one play): `isValid()`, `isPlaying()`, `stop()`, `pause()`, `resume()`,
`fadeOut(seconds [, opts])` (`opts.pause`, `opts.unscaled`, as `AudioSource:fadeOut`),
`fadeTo(volume, seconds [, opts])`; properties `volume`, `pitch`, `pan`, `position` (rw; setting it
stops following), `time`, `voiceCount` (ro). Getters of an invalid handle give 0 or the zero vector,
setters do nothing; a value that is not a number is refused with a warning.

`AudioPlayOn` (flags, add them): `UIClick` 1, `UIPress` 2, `UIHover` 4, `Collision2D` 8, `Collision3D`
16, `Sensor2D` 32, `Sensor3D` 64: what plays an AudioSource by itself (`AudioSource.playOn`).

```lua
-- a footstep that follows this object, a fade of one play, and a Button that clicks by itself
local step = Audio.play("events/footstep.soundevent", {target = self, volume = 0.8, group = "SFX"})
if step:isValid() then
    step:fadeOut(0.2)
end
local sound = AudioSource(self.scene, self.entity)
sound.event = "events/click.soundevent"
sound.playOn = AudioPlayOn.UIClick + AudioPlayOn.UIHover
Audio.stopAll("SFX")
```

`AudioListener` (static, Unity's): `pause` (rw: every source but the `ignoreListenerPause` ones
holds; a pause menu), `volume` (rw, 0..1, everything).

`AudioMixer` (static, the project's Audio Mixer, Project Settings > Audio; `jasper://docs/audio`):
`setFloat(name, value [, seconds [, opts]])` -> bool (an exposed parameter: a group volume in dB, an
effect value; it overrides snapshots until `clearFloat(name)`), `getFloat(name)` -> number or nil,
`clearFloat(name)`, `hasParameter(name)`, `getParameterNames()`, `transitionTo(snapshot, seconds
[, opts])`, `transitionToSnapshots(names, weights, seconds [, opts])`, `getSnapshotNames()`,
`getCurrentSnapshot()`, `getGroup(name)` -> `AudioMixerGroup` or nil, `getGroupNames()`,
`getMeter(group)` -> dB (-80 silent), `saveParameters()` / `loadParameters()` (the `remember`
ones, UserSettings), `dbToLinear(db)`, `linearToDb(v)`. `opts = {unscaled = true}`: unscaled
time (default: the mixer's update mode, scaled). `AudioMixerGroup`: `name`, `id` (ro), `volume`
(dB), `mute`, `solo`, `bypassEffects` (rw), `setVolume(db, seconds [, opts])`, `getEffect(index
(1-based) or type)` -> `AudioMixerEffect` (`type`, `id` ro; `bypass`, `wet` rw;
`setParameter(name, value [, seconds [, opts]])` -> bool, `getParameter(name)`), `getEffectCount()`,
`getMeter()`.

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
| `ignoreCollision(a, b [, ignore=true])`, `getIgnoreCollision(a, b)` | on both: two GameObjects / bodies (3D: Rigidbody, 2D: Rigidbody 2D) of one scene never collide; runtime only. Error if one has no body of that dimension |
| `ALL_LAYERS`, `DEFAULT_RAYCAST_LAYERS`, `IGNORE_RAYCAST_LAYER` | static props on both |
| `Physics.debugDraw`, `Physics.debugDrawContacts` | rw static bools: draw the colliders and joints (and contact points) of every playing scene, 3D and 2D (the Game view's Physics toggle) |

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
seconds since the engine started, wall-clock latched once a frame, running in the editor and
while the game is paused or time-scaled; with a fixed frame delta it advances by exactly that a
frame), so a script and the screen agree. Where waters overlap the one whose
bottom (the material's `buoyancyDepth` below its surface; none at 0) is highest wins. Each call
looks through the scene's meshes: for many points a frame, keep the results.

| Name | Notes |
|---|---|
| `Water.getHeight(scene, x, z)` | surface height (world Y, waves included) of the topmost water over (x, z); `nil` where none covers it |
| `Water.getNormal(scene, x, z)` | its normal (`Vector3`; up without `WAVES`); `nil` where none |
| `Water.isUnderwater(scene, point)` | `point` (`Vector3`) is under the surface of the water it is in |
| `Water.getTime()` | the shader library's clock, seconds (one value a frame) |
| `Water.sample(scene, point)` | a `WaterSample`: `inWater`, `surfaceHeight`, `normal`, `flowVelocity` (the volume's flow + the surface rising), `volume` (entity), `hasWater`; Water Volumes first, then the meshes above |
| `Water.sampleHeight(scene, x, z)` | the surface height there, Water Volumes first; `nil` where no water is |

A Water Volume (`go:getWaterVolume()`, `WaterVolume(scene, entity)`): `size` (Vector3, local: XZ
extent, Y depth), `surfaceOffset`, `infinite`, `density`, `flow`, `linearDrag`, `angularDrag`,
`waves` (`WaterVolumeWaves.FROM_MATERIAL` / `NONE`), `enabled`. A Buoyancy (`go:getBuoyancy()`,
`Buoyancy(scene, entity)`): `columns` (`BuoyancyColumns.AUTO_2x2` / `AUTO_3x3` / `CUSTOM`),
`customPoints` ({Vector3...}), `volumeScale`, `dragScale`, `enabled`.

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

### 2.12.1 UI (static)

`UI.canvasToUI(scene, Vector2)` / `UI.uiToCanvas(scene, Vector2)`: game canvas pixels (`Input`,
touches) to a scene's UI units and back (they differ when its Canvas Scaler scales the UI);
`UI.reducedMotion` (rw, default false): Text Effects skip their motion layers.

### 2.13 Timer (static): delayed calls, repeats, coroutines

```lua
local h = Timer.after(2.0, function() self:explode() end, { owner = self })   -- once, in 2 s of game time
Timer.every(0.5, function(t) self:blink() end, { count = 10, owner = self })   -- 10 times (fn gets the Timer)
Timer.every(1, function(t) return self.alive end, { delay = 0, unscaled = true }) -- false stops it
Timer.nextFrame(fn --[[, opts]]);  Timer.afterFrames(3, fn --[[, opts]]);  Timer.afterFixedSteps(1, fn --[[, opts]])
local co = Timer.run(function()              -- a coroutine the scheduler drives
    Timer.wait(1.0)                          -- game seconds (or the run's clock)
    Timer.waitRealtime(0.5)                  -- wall-clock seconds, even while paused
    Timer.waitFrames(2)                      -- simulated frames
    Timer.waitFixedStep()                    -- one fixed update
    Timer.waitUntil(function() return self.ready end)   -- checked once per frame
end, { owner = self })
local _ = h:cancel(), h:isActive(), h:isDone(), h.remaining, h.interval, h.unscaled, h.id
co:cancel(); co:isActive(); Timer.cancelAll(self)
```

- Options: `owner` (a script instance, `self`: cancelled when that script goes away), `clock`
  (`"game"`: scaled by `Time.timeScale`, the default; `"unscaled"`; `"real"`), `unscaled = true`
  (the same as `clock = "unscaled"`), `count` (every: how many calls), `delay` (every: the first
  call's delay, the interval by default), `catchUp` (every: after a stall, up to 8 missed periods
  at once; by default a repeat fires at most once a frame and keeps its phase).
- The callback gets the timer; a repeating one returning `false` stops (`isDone()` then true).
- A timer: `cancel()` (true when it stopped it), `isActive()`, `isDone()`, read-only `id`,
  `remaining` (seconds to the next call, 0 when done), `interval` (0 for a one-shot), `unscaled`;
  `tostring` gives `Timer(5, 0.75 s left, every 0.50 s)`. `Coroutine.waitFor(timer)` waits for it
  (true when it ran out, false when cancelled; a repeating timer without a count raises).
  `self:invoke` / `invokeRepeating` (2.17) return timers with the same fields.
- The game and unscaled clocks run only while the game runs: not in the editor's Pause (Step
  advances one frame), not while loading or held. Timers run after the scenes' update, before
  `onPostUpdate`; fixed-step ones after each fixed update.
- `Timer.run` starts at once and runs up to its first wait. `Timer.wait*` outside `Timer.run`
  raises `Timer.wait must be called inside Timer.run`.
- An error in a callback or coroutine is reported with its traceback (in the editor it pauses
  Play, like an error in an event) and cancels that timer or coroutine.
- Without an owner a timer lives until Play stops. `list_timers` (MCP) shows what is pending.

For delayed destruction `GameObject.destroy(obj, delay)` also works; tweens are Actions.

### 2.14 Profiler (static): samples, counters, frame statistics

```lua
Profiler.beginSample("AI think")             -- a scope in Window > Analysis > Profiler (CPU Usage)
self:think()
Profiler.endSample()
local path = Profiler.sample("Pathfind", function() return self:findPath() end)   -- fn's results
Profiler.counter("Enemies alive", #self.enemies)   -- a monitor (the Monitors module, get_frame_times)
Profiler.enabled = true                      -- start recording (read it to know whether it records)
local s = Profiler.frameStats()              -- {drawCalls, instances, triangles, cpuMs, fps, gpuMs?}
local r = scene:getRenderStats()             -- by pass: mainDrawCalls, shadowDrawCalls, mainTriangles, instancedDraws...
```

- Samples nest. An `endSample` without its `beginSample` is ignored (logged once). An error in
  `Profiler.sample`'s function is rethrown after the sample closes.
- Every Lua callback runs inside a `Lua` scope (the CPU chart's Scripts); with Deep Profile Lua
  (the Profiler's toolbar, `capture_profile deep_lua`) each callback gets a scope of its own,
  named by its chunk and line.
- `frameStats().gpuMs` is there only while GPU timing runs (the Profiler records, an agent called
  `get_gpu_passes`); GPU times arrive a few frames late.
- In a release game (no profiler) the functions do nothing, `enabled` stays false, and
  `frameStats()` still works.

### 2.15 Crash (static): reports of earlier sessions, the report hook, tags

```lua
if Crash.lastSessionEndedAbnormally() then Log.warn("the last session did not end cleanly") end
Crash.setTag("level", "forest-2")                -- a line of this session's crash.txt (16 tags)
for _, report in ipairs(Crash.getPendingReports()) do
    Log.print(report.kind .. " " .. report.id .. ": " .. report.reason)
end
Crash.setReportHook(function(report)             -- once per pending report, now
    -- {id, kind = "crash"|"hang"|"abnormal_exit", reason, time, version, product, handled, files}
    return true                                  -- true marks it handled: not offered again
end)
Crash.markHandled("20261026T143200Z-4242")       -- the same by id (false: unknown)
```

- The engine never sends a report anywhere: the hook decides. Reports hold file paths and log
  lines; ask the player before sending one. `files` are paths to read with the file API.
- Pending means from an earlier session and not handled yet; the current session's reports
  appear at the next start.
- Editor Play reads the editor's own reports (docs/crash-diagnostics.md).
### 2.16 Time (static, Unity's Time)

The game clocks under their Unity names. `Engine.deltatime` and the other `Engine` names stay;
prefer `Time` in new code: its names are Unity's and they avoid the `Engine.updateTime` /
`Engine.fixedDeltatime` trap.

| Name | Kind | Notes |
|---|---|---|
| `deltaTime` | ro | game seconds of this frame (times `timeScale`); inside `onFixedUpdate` (a fixed step) the fixed step `fixedDeltaTime` |
| `unscaledDeltaTime` | ro | the frame delta before `timeScale` (runs at time scale 0: pause menus); inside a fixed step `fixedUnscaledDeltaTime`; 0 while the app or the editor's Play is paused |
| `fixedDeltaTime` | rw | game seconds per fixed step (physics, `onFixedUpdate`), default 1/60 |
| `fixedUnscaledDeltaTime` | ro | `fixedDeltaTime / timeScale` (0 at time scale 0) |
| `time` | ro | game seconds since start (inside a fixed step, `fixedTime`) |
| `fixedTime` | ro | sum of the fixed steps run (inside a step: the time at its start) |
| `unscaledTime` | ro | sum of `unscaledDeltaTime` |
| `realtimeSinceStartup` | ro | wall clock seconds since the engine started (pauses included) |
| `timeSinceLevelLoad` | ro | game seconds since the last `SceneManager.loadScene` (or Play start) |
| `frameCount` | ro | frames the update loop has run |
| `timeScale` | rw | game speed, [0, 100]: scales the frame delta, physics, actions, tweens, animations, particles, `Coroutine.wait` and timers (not `waitRealtime`, unscaled timers, audio, HTML UI) |
| `maximumDeltaTime` | rw | cap of a frame's delta (default 0.25; values <= 0 are ignored with a warning) |
| `captureDeltaTime` | rw | when > 0 every frame advances exactly this many seconds (before `timeScale`); 0 = measured frames |
| `smoothDeltaTime` | ro | exponential average of `deltaTime` |
| `inFixedTimeStep` | ro | true inside `onFixedUpdate`, the physics step and the coroutines waiting for it |

| Old name | `Time` name |
|---|---|
| `Engine.deltatime` | `Time.deltaTime` (but the frame delta inside `onFixedUpdate` too) |
| `Engine.updateTime` | `Time.fixedDeltaTime` (the fixed step) |
| `Engine.fixedDeltatime` | `Time.captureDeltaTime` (a fixed frame delta, NOT the fixed step) |
| `Engine.timeScale` | `Time.timeScale` |
| `Engine.gameTime` | `Time.time` |
| `Engine.frameCount` | `Time.frameCount` |
| `Engine.unscaledDeltatime`, `Engine.unscaledTime` | `Time.unscaledDeltaTime`, `Time.unscaledTime` |
| `Engine.maxDeltatime` | `Time.maximumDeltaTime` |
| `Engine.systemTime` | `Time.realtimeSinceStartup` (from engine start, not an absolute clock) |

### 2.17 Coroutines and invoke (Coroutine, self:startCoroutine, self:invoke)

Engine coroutines are Lua functions the engine resumes: a wait helper suspends the running one
until its condition holds, then it continues on a later frame (Unity's `StartCoroutine` /
`yield return`). Lua yields through plain calls, so helpers compose (`self:moveTo(p)` may wait).

```lua
function Spawner:init()
    self.loop = self:startCoroutine(function()
        Coroutine.wait(2)                         -- WaitForSeconds(2): game time
        while true do
            self:spawn()
            Coroutine.wait(self.interval)
        end
    end)
    self:invokeRepeating("blink", 1, 0.5)          -- InvokeRepeating("blink", 1, 0.5)
end
function Spawner:openDoor()                        -- `yield return StartCoroutine(Open())`
    self:startCoroutine(function()
        local ok = Coroutine.waitFor(self:startCoroutine(self.animateOpen, self))
        Coroutine.waitUntil(function() return self.player.ready end, 5)   -- false after 5 s
    end)
end
```

| Script instance method | What it does |
|---|---|
| `self:startCoroutine(fn, ...)` / `self:startCoroutine("method", ...)` | runs `fn(...)` (or `self:method(...)`) in a new coroutine owned by this script, now until its first wait, and returns its `Coroutine`. Raises when the GameObject is inactive or the method is missing; allowed on a disabled script. An error inside is logged with the coroutine's traceback (the handle gets status `"error"`), never raised in the caller |
| `self:stopCoroutine(co)` / `self:stopCoroutine("method")` | stops that coroutine (it must be this script's) / every coroutine of this script started with that method name; ended or unknown: nothing |
| `self:stopAllCoroutines()` | every coroutine this script started (no other script's) |
| `self:invoke(fn, delay [, unscaled])` / `self:invoke("method", delay [, unscaled])` | calls `fn()` / `self:method()` once after `delay` seconds; returns a `Timer`. A missing method is logged once when it fires |
| `self:invokeRepeating(fnOrName, delay, interval [, unscaled])` | first after `delay`, then every `interval` (> 0) seconds |
| `self:cancelInvoke([timerOrName])` | no argument: every timer of this script; a method name: its timers; a Timer: that one |
| `self:isInvoking([timerOrName])` | a matching timer is pending |

| Wait helper (inside an engine coroutine) | Resumes | Returns |
|---|---|---|
| `Coroutine.wait(seconds)` | after `seconds` of game time (scaled; never at `timeScale` 0; <= 0: next frame) | elapsed game seconds |
| `Coroutine.waitRealtime(seconds)` | after unscaled seconds (runs at time scale 0) | elapsed unscaled seconds |
| `Coroutine.nextFrame()` (= `coroutine.yield()`) | next frame (Unity's `yield return null`) | nothing |
| `Coroutine.waitFrames(n)` | after `n` frames (n >= 1) | nothing |
| `Coroutine.waitForFixedUpdate()` | after the next fixed step's physics (none at time scale 0) | nothing |
| `Coroutine.waitForEndOfFrame()` | after this frame's draw | nothing |
| `Coroutine.waitUntil(pred [, timeout [, unscaled]])` | when `pred()` is truthy, checked every frame | `true`, or `false` on timeout |
| `Coroutine.waitWhile(pred [, timeout [, unscaled]])` | when `pred()` is falsy | same |
| `Coroutine.waitFor(x)` (= `coroutine.yield(x)`) | when `x` is done | see below |

`waitFor(x)`: a `Coroutine` -> `true, ...what it returned` / `false, error` (`false, "stopped"`),
at once when it already ended; a `Timer` -> `true` when it fired, `false` when cancelled (an
endless `every` raises "never done"); a Tween or any Action, an `AudioSource`, an
`AnimationPlayer` -> `true` when stopped / not playing, `false` when its GameObject is gone; an
anime animation or timer -> `true` completed, `false` cancelled; anything else with an
`isDone()` method (the HTTP request handle) -> `true, x` once `x:isDone()` is truthy.

Statics: `Coroutine.start(fn, ...)` (unowned: survives scene loads, ends at Play stop; for
`require`d modules and HTML UI document scripts), `Coroutine.running()` (the handle or nil),
`Coroutine.isInside()`. A `Coroutine` handle: `status` (`"running"`, `"suspended"`,
`"finished"`, `"stopped"`, `"error"`), `error` (string with traceback, or nil), `owner` (the
script instance or nil), `id`, `isDone()`, `stop()`. Lua's own `coroutine` library is untouched:
`coroutine.wrap` generators work inside engine coroutines (the wait helpers do not, inside them);
`coroutine.resume` of an engine coroutine (`coroutine.running()` inside one) is refused with an error
and the engine ends it. Two coroutines waiting for each other (`waitFor`) raise "they would wait for
each other forever".

Timers outside scripts: the global `Timer` (2.13: `Timer.after`, `Timer.every`, the frame and
fixed-step timers, `Timer.run`), on the engine's scheduler (`list_timers` shows them). HTML UI
animation timing stays `anime.createTimer` (milliseconds, the UI clock); for gameplay use
`self:invoke` / `Timer.after`.

Frame order: fixed steps (`onFixedUpdate`, physics, then the coroutines waiting
`waitForFixedUpdate`), `onUpdate`, then `invoke` timers and the coroutines whose wait is over (seconds,
frames, predicates, `waitFor`; in the order they were started), the scenes' update,
`onPostUpdate`, destroys, the draw, then the coroutines waiting `waitForEndOfFrame`. Every wait
resumes on a later frame, except `waitForEndOfFrame` (same frame) and a `waitFor` of an ended
coroutine (at once).

Lifetime:

| Event | Coroutines of the script | Its timers | Unowned (`Coroutine.start`, `Timer.after`) |
|---|---|---|---|
| `self.enabled = false` (script disabled) | continue | continue | - |
| GameObject (or a parent) deactivated | stopped (to-be-closed variables close) | continue | - |
| GameObject destroyed, scene unloaded (`loadScene`), Play stop | stopped | cancelled | continue on loads; end at Play stop |
| `Time.timeScale = 0` | scaled waits wait | scaled timers wait | same |
| App in background, editor Play paused | nothing runs, no clock advances (realtime too) | same | same |

A coroutine that stops itself (`self:stopAllCoroutines()`, `Coroutine.running():stop()`) runs
on to its next wait, which ends it. Errors read `Lua coroutine error in Spawner (GameObject
'Spawner', scene 'Level1'), started at Spawner.lua:12: ... stack traceback: ...` (timers:
`Lua timer error in ..., created at ...`). In the editor such an error stops Play, like an
error in an event callback ("Script crash" in the Console). MCP `get_runtime_entity` lists each
script's coroutines (what each waits for) and timers; `get_play_state` has the totals.

### 2.18 Http (static, HTTP(S) requests)

Full reference, options, error codes, platform notes and examples: `jasper://docs/networking`.
The callback runs once, on the main thread, right before `onUpdate` of a later frame; errors are
values (`res.ok`, `res.errorCode`, `res.error`); pass `owner = self` so the request ends with
the script (no callback). Plain `http://` works only to localhost unless
`player.allow_insecure_http`.

| Call | Meaning |
|---|---|
| `Http.get(url [, options] [, callback])` | GET; `head`, `delete` likewise |
| `Http.post(url, body [, options] [, callback])` | POST: string body as is, table as JSON; `put`, `patch` likewise |
| `Http.request{url = ..., method = ..., onComplete = ...}` | any method, every option |
| `Http.download(url, "data://x" [, options] [, callback])` | into a file (`<path>.part`, renamed on success) |
| `Http.getTexture(url [, options] [, callback])` | `res.textureData` for `image:setTexture(res.url, res.textureData)` |
| `Http.cancelAll()`, `Http.isAvailable()`, `Http.encodeUrl(s)`, `Http.decodeUrl(s)` | |
| `Http.maxConcurrentRequests`, `Http.userAgent`, `Http.timeout`, `Http.logFailures` (rw) | 8, `JasperEngine/<version> (<platform>)`, 30 s, false |

Options: `headers`, `query`, `body`, `json`, `form`, `timeout`, `connectTimeout`, `redirects`,
`verifyTls`, `responseType` (`"text"`, `"binary"`, `"json"`: `res.data`), `maxBodySize`,
`onProgress = function(req, received, total)`, `owner`, `file`, `resume`, `keepPartial`.
`HttpRequest` (returned): `id`, `url`, `method`, `state` (`HttpState.Queued/Running/Done/Cancelled`),
`progress`, `bytesReceived`, `bytesTotal`, `response`, `req:isDone()`, `req:cancel()`.
`HttpResponse`: `ok`, `status`, `errorCode`, `error`, `headers`, `body`, `data`, `textureData`,
`file`, `url`, `redirected`, `method`, `elapsed`, `bytesReceived`, `res:json()`, `res:header(name)`.

```lua
Http.get("https://backend.example.com/Now", {responseType = "json", timeout = 10, owner = self}, function(res)
    if res.ok then self.serverTime = res.data.time else Log.warn(res.error) end
end)
```

### 2.14 Native (static, native code of the platform)

Full reference, every platform's conventions, mocks and SDK settings: `jasper://docs/native-bridge`.
A plugin name answers from, in order: a C++ plugin, the platform's class/object (Java/Kotlin on
Android, Objective-C/Swift on iOS/macOS, JavaScript on the Web), a Lua mock
(`<Lua folder>/native_mocks/<Plugin>.lua` or `Native.setMock`). Values are JSON (the `Json` rules);
errors are values; callbacks and events run on the main thread before `onUpdate`.

| Call | Meaning |
|---|---|
| `Native.call(plugin, method, ...)` | sync: `value, nil, nil` or `nil, errMessage, errCode` |
| `Native.callAsync(plugin, method [, args] [, callback] [, options])` | `callback(result, errMessage, errCode)`; options `thread` (`"main"`/`"background"`), `owner`, `timeout`; returns a `NativeCall` |
| `Native.on(plugin, event, handler [, owner])` / `Native.off(id)` | native events (`handler(payload)`) |
| `Native.has(plugin [, method])` | an implementation exists here |
| `Native.setMock(plugin, table)`, `Native.clearMock(plugin)`, `Native.emitMock(plugin, event, payload)`, `Native.pending()` | Lua mocks |

`NativeCall`: `id`, `plugin`, `method`, `result`, `errorCode`, `errorMessage`, `call:isDone()`
(`Coroutine.waitFor(call)`), `call:cancel()`, and for mocks `call:resolve(value)`,
`call:reject(code, message)`. Codes: `not_found`, `no_method`, `bad_args`, `exception`,
`unsupported_type`, `invalid_utf8`, `cancelled`, `timeout`, `async_only`, `not_ready`, `unavailable`.

```lua
local version, err = Native.call("Device", "getCode")
Native.callAsync("Ads", "showRewarded", {"unit-1"}, function(result, err, code)
    if err then Log.warn(code .. ": " .. err) else self.coins = self.coins + result.amount end
end, {owner = self})
```

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
| Unity's moves | `moveTowards(target, maxDelta) lerp(target, t) lerpUnclamped(target, t)` | same | same |

Constants are static properties returning copies: `Vector3.ZERO` (no parentheses).
`v * v`, `v / v` and `number / v` are component-wise. `<` is true when every component is
smaller, `<=` when every component is smaller or equal (so `a < b` and `b < a` can both be
false). `==` with a value of another class is `false`. There is no `v + number`.
`lerp` is Unity's `Lerp`: `t` is clamped to 0..1 (`lerpUnclamped` extrapolates); `moveTowards`
steps at most `maxDelta` toward the target and never past it (a negative delta moves away).
`a:lerp(b, t)` and `Vector3.lerp(a, b, t)` are the same call:

```lua run
local a, b = Vector3(0, 0, 0), Vector3(10, 0, 0)
assert(Vector3.lerp(a, b, 0.25) == Vector3(2.5, 0, 0))
assert(a:lerp(b, 2) == b and a:lerpUnclamped(b, 2) == Vector3(20, 0, 0))
assert(Vector2(0, 0):moveTowards(Vector2(3, 4), 2.5) == Vector2(1.5, 2))
```

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
| `EdgeInsets(left, top, right, bottom)` | rw `left top right bottom`; `isZero()`, `==`, `tostring` (Screen.safeAreaInsets) |
| `Ray(origin, direction)` (Vector3) | rw `origin direction`; `getPoint(t)`; `intersects(AABB/OBB/Plane/Rigidbody2D/Rigidbody/...)`; `Ray.NO_HIT` |
| `Matrix3`, `Matrix4` | `+ - == tostring`; `*` by a matrix, a number (either side) or a vector (`m * Vector3`, Matrix4 also `m * Vector4`; `Matrix4 * Vector3` applies the translation); `row column get set identity inverse transpose`; Matrix4 static `translateMatrix rotateMatrix rotateXMatrix rotateYMatrix rotateZMatrix scaleMatrix lookAtMatrix orthoMatrix perspectiveMatrix frustumMatrix` |
| `Plane`, `AABB`, `OBB`, `Sphere` | geometry helpers (`intersects`, `contains`, `getCenter`, `getSize`, `merge`, ...); `AABB`/`OBB` `getCorner(c)` (`AABB.CornerEnum.*`), `getCorners()` -> table of 8 `Vector3` copies, `corners[i]` = `getCorner(i - 1)` (empty for a null or infinite box) |
| `AnimationCurve()` | Unity's curve. Static `constant(v)`, `linear(t0, v0, t1, v1)`, `easeInOut(t0, v0, t1, v1)`; `evaluate(t)`; `addKey(time, value [, inTangent, outTangent])` / `addKey(CurveKey)` -> 1-based index (-1 when a key is at that time); `removeKey(i)`; `moveKey(i, time, value)` / `moveKey(i, CurveKey)` -> its new index; `getKey(i)` -> `CurveKey` (nil out of range); ro `keyCount`; rw `keys` (table of `CurveKey`), `preWrapMode`, `postWrapMode` (`CurveWrap.*`) |
| `CurveKey(time, value [, inTangent, outTangent])` | rw `time value inTangent outTangent` (slopes; `math.huge` is a step) |
| `Gradient()` | Unity's gradient (white, opaque). `setKeys(colorKeys, alphaKeys)`: `{{time, Vector3 sRGB}, ...}` (or `GradientColorKey`s), `{{time, alpha}, ...}`; `evaluate(t)` -> Vector4 **linear**; rw `mode` (`GradientMode.*`), `colorKeys`, `alphaKeys`; ro `colorKeyCount`, `alphaKeyCount`; `getColorKey(i)`, `getAlphaKey(i)` (1-based) |
| `GradientColorKey(time, colorV3)`, `GradientAlphaKey(time, alpha)` | rw `time`, `color` (**sRGB**: given and read as pickers show it, stored linear) / `alpha` |

### 3.4 Curves and gradients (AnimationCurve, Keyframe, Gradient, MinMaxCurve, MinMaxGradient)

Unity's types over the engine's `Curve` (cubic Hermite keys; the curve fields of components and
particle effects hold the same values). **Key indices are 1-based** (C++ and MCP: 0-based); an
index out of range gives `nil`, `false` or `-1`, never an error.

| Class | Members |
|---|---|
| `Keyframe(time, value[, inTangent, outTangent])` | rw `time value inTangent outTangent` (a step: `math.huge`), `inTangentMode outTangentMode` (`TangentMode.*`), `broken` |
| `AnimationCurve()` | `evaluate(t)`; `addKey(t, v)` / `addKey(keyframe)` -> index; `removeKey(i)` -> bool; `moveKey(i, keyframe)` -> new index; `getKey(i)` -> Keyframe or nil; `smoothTangents(i, weight)`; ro `length` (keys); rw `preWrapMode postWrapMode` (`WrapMode.*`); static `AnimationCurve.linear(t0, v0, t1, v1)`, `AnimationCurve.constant(t0, t1, v)`, `AnimationCurve.easeInOut(t0, v0, t1, v1)` |
| `GradientColorKey(colorVector3, time)`, `GradientAlphaKey(alpha, time)` | rw `color`/`alpha`, `time` |
| `Gradient()` | `evaluate(t)` -> Vector4 (sRGB 0-1); `setKeys(colorKeys, alphaKeys)` (Lua lists, at most 8 each); ro `colorKeys alphaKeys` (lists); rw `mode` (`GradientMode.Blend`/`Fixed`), `linear` (blend in linear space) |
| `MinMaxCurve([constant])`, `MinMaxCurve(min, max)`, `MinMaxCurve(multiplier, curve)`, `MinMaxCurve(multiplier, minCurve, maxCurve)` (Unity's) | `evaluate(t[, random])` (random 0-1 picks between the two); rw `mode` (`MinMaxCurveMode.*`), `constant`/`constantMin`, `constantMax`, `curve`/`curveMin`, `curveMax`, `multiplier`; static `MinMaxCurve.twoConstants(a, b)`, `fromCurve(curve, multiplier)`, `twoCurves(min, max, multiplier)` |
| `MinMaxGradient([color])` (Vector4, or Vector3 opaque; sRGB), `MinMaxGradient(min, max)` (two colours), `MinMaxGradient(gradient)`, `MinMaxGradient(minGradient, maxGradient)` (Unity's) | `evaluate(t[, random])` -> Vector4; rw `mode` (`MinMaxGradientMode.*`), `color`/`colorMin`, `colorMax`, `gradient`/`gradientMin`, `gradientMax` |

```lua
local curve = AnimationCurve.linear(0, 0, 1, 2)
local i = curve:addKey(0.5, 1.5)              -- 2: the new key's 1-based index
curve.postWrapMode = WrapMode.PingPong
local y = curve:evaluate(1.25)                -- as at 0.75
local fade = Gradient()
fade:setKeys({ GradientColorKey(Vector3(1, 1, 1), 0) }, { GradientAlphaKey(1, 0), GradientAlphaKey(0, 1) })
local half = fade:evaluate(0.5).w             -- 0.5
```

---

## 4. Object wrappers

Class hierarchy (methods/properties are inherited):

```
EntityHandle
 |- GameObject
 |   |- Camera, Light, Light2D, ShadowCaster2D, ReflectionProbe, LocalVolumetricFog, Bone, Points, LineRenderer, Lattice,
 |   |  FluidSimulation, ParticleSystem, TrailRenderer
 |   |- Mesh
 |   |   |- Primitive -> Mirror
 |   |   |- Sprite, Tilemap, Model, MeshPolygon, Terrain
 |   |- RectTransform
 |       |- Text, Polygon, LayoutGroup
 |       |- Image -> Button, Panel, Scrollbar, Progressbar, InputField
 |- AudioSource, PoseAnimator, AnimationPlayer, VertexAnimator, Rigidbody2D, Joint2D, Rigidbody, Fog, VolumetricClouds, Skybox
 |- Action -> Tween (AlphaTween, ColorTween, PositionTween, RotationTween, ScaleTween),
             SpriteAnimation, Animation,
             KeyframeTracks (MorphTracks, LatticeTracks, RotateTracks, ScaleTracks, TranslateTracks)
Joint3D, AnimationState, AnimationEvent, AnimatorStateInfo, VertexAnimatorSocketPose, PhysicsMaterial (standalone classes)
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
| `addChild(goOrEntity)`, `removeChild(goOrEntity)`, `removeParent()` | methods | keep the child's local values |
| `parent` | rw `GameObject` or nil | Unity's `transform.parent`: assigning keeps the world placement (nil: to the root) |
| `setParent(goOrScriptOrNil [, worldPositionStays])` | method | Unity's `SetParent` (default true keeps the world placement, false the local values); refused (logged) for itself, one of its children, another scene's GameObject |
| `childCount`, `getChild(i)`, `getChildren()` | ro / methods | the direct children in Hierarchy order; `getChild` is 0-based (nil out of range), `getChildren` a table |
| `findChild(path)` | method | Unity's `Transform.Find`: a child by name, or a path of names below it (`"Arm/Hand"`); nil when missing |
| `moveToTop()`, `moveUp()`, `moveDown()`, `moveToBottom()` | methods | sibling order = 2D draw order (below) |
| `billboard`, `fakeBillboard`, `cylindricalBillboard` (rw), `billboardRotation` (rw), `setBillboard(a,b,c)`, `setBillboardRotation(x,y,z)` | | |
| `localMatrix` (rw), `modelMatrix` (ro), `normalMatrix` (ro), `updateTransform()` | | |
| `getRigidbody2D()` | method | returns `Rigidbody2D`, CREATING the component if missing |
| `removeRigidbody2D()` | method | |
| `getJoint2D()` | method | the GameObject's `Joint2D`, `nil` when it has none |
| `getRigidbody()` / `removeRigidbody()` | method | 3D, same semantics |
| `getJoint3D()`, `getRagdoll()` | method | the GameObject's `Joint3D` / `Ragdoll`, `nil` when it has none |
| `getRay(direction)` | method | Ray from world position |
| `getAnimationPlayer()` | method | the GameObject's `AnimationPlayer` (4.8.2), `nil` when it has none (it never adds one) |
| `hasAnimationPlayer()` | method | bool |
| `getAnimator()` | method | the GameObject's `Animator` (4.8.4); without one its `AnimationPlayer`; `nil` without either |
| `getVertexAnimator()` | method | the GameObject's `VertexAnimator` (4.8.3), `nil` when it has none (it never adds one) |
| `hasVertexAnimator()` | method | bool |
| `GameObject.instantiate`, `GameObject.destroy`, `GameObject.destroyImmediate` | static | section 2.6 |

```lua run
local holder = GameObject(scene)
holder.name = "Holder"
local arm = GameObject(scene)
arm.name = "Arm"
arm:setParent(holder)
assert(arm.parent.entity == holder.entity and holder.childCount == 1)
assert(holder:findChild("Arm").entity == arm.entity and holder:getChild(0).entity == arm.entity)
arm.parent = nil
assert(arm.parent == nil and holder.childCount == 0)
GameObject.destroy(holder)
GameObject.destroy(arm)
```

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
| `castShadows`, `receiveShadows`, `staticShadowCaster` (cached in the static shadow layer, `jasper://docs/shadows`), `receiveLights`, `transparent`, `faceCulling`, `cullingMode`, `customShader` | rw |
| `allowGpuInstancing` (its draws may batch with those of the same mesh and material, `jasper://docs/meshes`) | rw |
| `getAABB()`, `getWorldAABB()`, `getNumSubmeshes()` | methods (a model made of parts: 4.8) |
| `vertexCount` | ro (Unity's `Mesh.vertexCount`) |
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
Built-in Lit extended lobes (glTF `KHR_materials_*`; the same copy-then-set rule; each lobe
compiles in only while its `...Enabled` is true, so animating a factor never recompiles):
`emissiveStrength` (multiplies `emissiveFactor`), `clearcoatEnabled`, `clearcoatFactor`,
`clearcoatRoughness`, `clearcoatNormalScale`; `sheenEnabled`, `sheenColor` (Vector3, linear),
`sheenRoughness`; `anisotropyEnabled`, `anisotropyStrength`, `anisotropyRotation` (radians);
`iridescenceEnabled`, `iridescenceFactor`, `iridescenceIor`, `iridescenceThicknessRange`
(Vector2, nm); `transmissionEnabled`, `transmissionFactor`; `volumeEnabled`, `thicknessFactor`,
`attenuationColor`, `attenuationDistance` (`math.huge`: no absorption); `specularEnabled`,
`specularFactor`, `specularColor`, `ior`; `hexTilingEnabled`, `hexTilingRotation`,
`hexTilingContrast`. Their maps read only: `clearcoatTexture`, `clearcoatNormalTexture`,
`sheenTexture`, `anisotropyTexture`, `iridescenceTexture`, `transmissionTexture`,
`specularTexture`. The Shading Model: `shadingModel` (`ShadingModel.STANDARD` / `HAIR`), and the
hair's `hairPrimaryColor`, `hairPrimaryShift`, `hairPrimaryWidth`, `hairSecondaryColor`,
`hairSecondaryShift`, `hairSecondaryWidth`, `hairRotation` (radians), read only `hairFlowTexture`.
E.g. `local m = mesh:getMaterial(); m.clearcoatEnabled = true;
m.clearcoatFactor = 1; mesh:setMaterial(m)`. More: `jasper://docs/materials`.
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
`resetToBindPose()`, `enabled` (rw), `filename` (ro, the file it loaded), `getAnimationCount()`,
`getPoseAnimator()` (the model's PoseAnimator, added when missing),
`getHumanoidInfo()` (`{humanoid, animationType ("generic"/"humanoid"), humanScale, missing = {slot names},
extraMapped, error?}`: its Avatar; nil without bones).

**A model made of parts.** A glTF with several mesh nodes loads each into a child GameObject
(a part; the Model's own mesh stays empty), and the Mesh and Model API reaches the parts: a mesh-wide
setter (`castShadows`, `receiveLights`, `customShader`, `setShaderUniform`, ...) writes every part
and its getter reads the first (lowest node index); calls on "submesh 0" (`setColor`, `color`,
`setTexture`, `material`, `setNormalTexture(path)`) act on each part's first submesh;
an index (`setMaterial(i, m)`, `getMaterial(i)`) counts submeshes part after part
(`getNumSubmeshes()` is their sum; past the end: logged, nothing changes); `getAABB()` and
`vertexCount` cover all parts; `setMorphWeight(name, w)` goes to every part with a target of that
name, `setMorphWeight(i, w)` to every part with that many targets. Instancing and `setAsMirror` are
refused there (logged): call them on a part (`model:findChild("Body")`).

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
A clip's audio cues play by themselves (no Lua API; author them with `edit_animation {clip, audio}`):
to play a sound from code use `Audio.play`, and react to clip events as below.

| Member | Notes |
|---|---|
| `play(name [, fade [, mode]])` | instant: the other states of `name`'s layer stop (rewound), `name` plays from 0 at weight 1 (from its end when the speed is negative); clears that layer's queue. `mode`: `PlayMode.STOP_SAME_LAYER` (default) or `PlayMode.STOP_ALL` (every layer). `fade` > 0: same as `crossFade` |
| `crossFade(name [, fade [, mode]])` | fades the other states of the layer (or all with `STOP_ALL`) out and `name` in; no fade = `defaultFadeTime` |
| `blend(name, weight [, fade])` | moves one state's weight, the others untouched; total weight under 1 is filled by the rest (scene) pose |
| `queue(name [, fade])` | plays after the playing states of `name`'s layer finish (loop / ping_pong: at the end of the current cycle; clamp_forever: on reaching its end); with a fade it starts `fade` s earlier and crossfades; queuing the playing state restarts it; nothing playing on the layer = play. One queue per layer |
| `stop()`, `stop(name)` | stop and rewind, the pose stays; `stop()` clears every queue, `stop(name)` the state's layer queue |
| `rewind()`, `rewind(name)` | time 0 |
| `sample()` | applies the current times once (e.g. paused) |
| `isPlaying()`, `isPlaying(name)` | bool; false for a `once` state that ended (held) |
| `getState(name)` | an `AnimationState` (below) |
| `addClip(path [, name [, options]])` | appends an entry (`name` empty or nil = the file stem / model clip name); false when the name is taken. `options`: `{retarget = bool (play a generic clip as a humanoid clip, from its source model), mirror = bool, footIK = bool, layer = int}` (other keys or types are an error) |
| `removeClip(name)`, `getClipCount()`, `getClipName(i)` | `getClipName` is 0-based, `""` out of range |
| `speed`, `defaultClip`, `defaultFadeTime`, `playAutomatically`, `enabled` | rw; `speed` multiplies every state (negative plays backwards); `enabled = false`: time stops, nothing is written |
| `isLoaded()` | every clip loaded and bound |
| `getAnimatedProperties(name)` | the state's bound property tracks, `{"path:component.property", ...}` (debugging; empty while its clip loads). A clip animating a script field sets `self.<field>` (read it in `onUpdate`; a write of yours there is overwritten by the clip in the same frame, as in Unity) |
| `onEvent` | rw, a function called with ONE `AnimationEvent` (below) per clip event |
| `onFinish` | rw, a function called with the state name when a `once` state ends, and once when a `clamp_forever` state first reaches its end (never for loop / ping_pong); fires on a zero-weight layer too |
| `getLayerCount()` | >= 1 (layer 0 is the base layer) |
| `getLayerName(i)`, `getLayerIndex(name)` | `""` / -1 when unknown |
| `addLayer(name [, weight [, blend [, mask]]])` | appends an authored layer (`blend` `"override"` or `"additive"`, `mask` an asset-relative `.mask`); its index, or -1 when the name is taken |
| `removeLayer(layer)` | not the base layer; its clip entries move to layer 0 (warning); bool |
| `setLayerWeight(layer, w [, fade])`, `getLayerWeight(layer)` | runtime weight 0..1, fading linearly over `fade` s (scaled time); the getter returns the current, possibly fading, value; layer 0 is always 1 |
| `setLayerMask(layer, path)`, `getLayerMask(layer)` | runtime Avatar Mask (`""` clears it); a layer whose mask is missing contributes nothing |
| `setLayerBlendMode(layer, mode)`, `getLayerBlendMode(layer)` | runtime `"override"` / `"additive"` (layer 0: always override) |
| `stopLayer(layer [, fade])` | fades every state of the layer out (instant without a fade) and clears its queue |
| `controller` | rw, the controller of the GameObject's Animator (asset-relative `.controller`, `""` = none); setting one adds an Animator playing it and disables the player, which then hands over |
| `hasController()` | bool: the player hands over to an Animator with a controller |
| `setFloat(name, v [, dampTime])`, `getFloat(name)` | a controller parameter; `dampTime` > 0 eases toward `v` with SmoothDamp, a set without it snaps |
| `setInteger(name, v)`, `getInteger(name)`, `setBool(name, b)`, `getBool(name)` | the other parameter types; a wrong type warns once and converts (`setBool` on a trigger sets it, `setFloat` on an int rounds) |
| `setTrigger(name)`, `resetTrigger(name)` | a trigger stays set until a transition reading it fires |
| `hasParameter(name)`, `getParameterType(name)` | bool; `"float"`, `"int"`, `"bool"`, `"trigger"` (`""` when unknown) |
| `getCurrentStateName([layer])` | the layer's current state, without the layer prefix (`""` when none) |
| `getCurrentStateInfo([layer])`, `getNextStateInfo([layer])` | a `ControllerStateInfo` (below); the next state is valid only during a transition |
| `isInTransition([layer])` | bool |
| `playInLayer(name, layer [, normalizedTime])` | Unity's `Animator.Play`: with a controller, jumps to `name` (`"Layer.State"` or `"State"`) on `layer` (-1: any) at `normalizedTime` (default 0); without one, plays the clip state from that normalized time |
| `crossFadeInLayer(name, seconds, layer)` | Unity's `CrossFadeInFixedTime`: a transition of `seconds` to `name` on `layer` (-1: any) |
| `onStateEnter`, `onStateExit` | rw, functions called with `(layer, name)` when a controller state is entered / exited |
| `getAnimationPlayerComponent()` | the ECS component: `enabled`, `defaultClip`, `playAutomatically`, `speed`, `defaultFadeTime`, `needsLoad`, `onEvent`, `onFinish`, `onStateEnter`, `onStateExit` |

`layer` arguments are a 0-based index or a layer name; an unknown layer warns once and does
nothing (getters return 0, `""` or -1). Layers (Unity's animation layers): each clip entry plays on
one; a layer has a weight, a blend (Override replaces what is below as much as its weight says,
Additive adds the clip's change from its `additive_reference` time, default its first frame) and an
optional Avatar Mask (`.mask`). Events fire only on layers with weight > 0 (the base always fires).

**Handover to an Animator.** A *disabled* player on a GameObject with an Animator (what a scene
saved when the player played Animator Controllers loads as, and what Create Animator Controller
from Clips makes) forwards to the Animator: `play(name)` jumps to a state (`"Layer.State"` or
`"State"`), `play(name, fade)` and `crossFade(name, seconds)` are a transition of that many seconds
(a `PlayMode` has no effect), `playInLayer` / `crossFadeInLayer` add the layer and the normalized
start time, `stop()` stops it until the next `play` or until the player is enabled again, `blend`
and `queue` warn once and do nothing, `isPlaying(name)` takes controller state names, and
`setEnabled` / the layer weights / the parameter calls / the state reads are the Animator's (an
Animator without a controller: the parameter calls warn once and return defaults). After the
frame's pose is written, `onStateEnter(layer, name)` / `onStateExit(layer, name)` (the state name
without its layer) call the player's subscribers, then the methods of those names on the
GameObject's Lua scripts.

A `once` state that reaches its end fires `onFinish`, stops advancing and **holds its last pose**
at its weight until a fade (crossFade / play with a fade / blend to 0) or `stop` releases it, so
`onFinish` then `crossFade("Idle", 0.2)` blends from the end pose; with nothing else playing the
pose stays. `play(name)` or `blend(name, w > 0)` of a held state restarts it; `blend(name, 0,
fade)` fades it out. Events and `onFinish` run after the frame's animation update, in time order; player
calls made inside them take effect next frame.

`AnimationState` (by player + state name, looked up on every access; works across clip list
changes): rw `time`, `normalizedTime`, `speed` (the state's own), `weight`, `wrap` (`"once"`,
`"loop"`, `"ping_pong"`, `"clamp_forever"`; `"default"` = the entry's), `enabled` (false pauses),
`layer` (a runtime move: time and weight are kept, the tracks re-mask), `mirror` and `footIK` (humanoid
clips: runtime overrides of the entry's Mirror / Foot IK; `resetMirror()` / `resetFootIK()` go back to
the entry's); ro `name`, `clip`, `length`, `isHumanoid` (a humanoid clip, or a generic one retargeted);
`isValid()`; `addMixingTransform(path [, recursive=true])`, `removeMixingTransform(path)`,
`clearMixingTransforms()` (Unity's legacy AddMixingTransform: with any, the state animates only
those paths, below them too unless `recursive` is false; multiplied with the layer's mask). An
unknown name reads defaults and writes nothing.

`PlayMode`: `PlayMode.STOP_SAME_LAYER` (0), `PlayMode.STOP_ALL` (1).

`ControllerStateInfo` (a copy, the player's and the Animator's `getCurrentStateInfo`): `isValid`,
`name`, `fullName` (`"Layer.State"`), `normalizedTime` (whole loops counted), `length` (seconds),
`speed`, `loop`.

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

```lua
-- layers: the right arm holds a can over the walk (the "Holding" layer has an arm mask)
local w = self.player:getLayerWeight("Holding")
self.player:setLayerWeight("Holding", w + ((self.holding and 1 or 0) - w) * math.min(1, Engine.deltatime * 15))
self.player:crossFade("Hit", 0.05)          -- on an additive layer: over whatever plays

-- an Animator Controller
local animator = GameObject(self.scene, self.entity):getAnimator()
animator:setFloat("Speed", speed)
animator:setTrigger("Jump")
if animator:getCurrentStateInfo(0).name == "Land" then --[[ ... ]] end
function Hero:onStateEnter(layer, name) print("entered", layer, name) end
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

### 4.8.4 Animator (EntityHandle)

Unity's Animator: plays an Animator Controller (`.controller`: parameters, layers, state
machines, blend trees, transitions) on the clips of the Animation Player's kinds (`.anim`, model
clips), with Avatar Masks, montages (`.montage`), an IK stack and root motion. `Animator(scene,
entity)` adds the component when missing; `GameObject:getAnimator()` returns it (the Animation
Player without one, `nil` without either); `GameObject:hasAnimator()`. It runs in play mode and in games. Calls before the controller loads
are kept. User guide: `docs/animator.md`; agents: `jasper://docs/animator`. Names that the
controller does not have warn once and do nothing.

| Member | Notes |
|---|---|
| `controller` | rw, the `.controller` path (assets-relative); a change reloads it, parameters and states of the same names carry over |
| `setFloat(name, v [, dampTime, deltaTime])`, `getFloat` | with damping: Unity's SmoothDamp towards `v` over `dampTime` s |
| `setInteger`, `getInteger`, `setBool`, `getBool` | |
| `setTrigger(name)`, `resetTrigger(name)` | a trigger stays set until a transition uses it |
| `getParameterIndex(name)`, `getParameterCount()`, `getParameterName(i)` | 0-based; -1 / `""` when unknown |
| `play(state [, layer, normalizedTime])` | at once; `state` a name or `"Layer.Machine.State"`; `normalizedTime` < 0 keeps the current time when it is the current state |
| `crossFade(state, duration [, layer, offset])` | `duration` in the current state's lengths; `crossFadeInFixedTime` in seconds |
| `getCurrentState(layer)`, `getNextState(layer)` | an `AnimatorStateInfo`: `valid`, `name`, `fullPath`, `tag`, `normalizedTime` (laps + fraction), `length` (s), `loop`, `speed`, `mirror` and `footIK` (the state's humanoid playback) |
| `isInTransition([layer])` | layer 0 by default |
| `getLayerCount()`, `getLayerName(i)`, `getLayerIndex(name)`, `setLayerWeight(layer, w [, fade])`, `getLayerWeight(layer)` | `layer` an index or a name; `fade` seconds to reach `w`; the first layer always weighs 1 |
| `getCurrentStateName([layer])`, `getCurrentStateInfo([layer])`, `getNextStateInfo([layer])` | the names of the Animation Player's controller calls (scripts written for them run as they are): the state's name, a `ControllerStateInfo` |
| `playInLayer(state, layer [, normalizedTime])`, `crossFadeInLayer(state, seconds, layer)` | `play` and `crossFadeInFixedTime` by those names |
| `hasController()`, `hasParameter(name)` | bool |
| `playMontage(path [, rate, section])`, `stopMontage([blendOut])`, `isMontagePlaying()`, `getMontage()`, `getMontageSection()` | a montage plays on the layer whose slot it names, blending in and out |
| `montageJumpToSection(name)`, `montageSetNextSection(from, to)` | |
| `setIKTarget(constraint, position [, weight])`, `clearIKTarget(constraint)`, `setIKWeight(constraint, w)` | world space; a script target wins over the constraint's GameObject |
| `setLookAt(position [, weight])` | the first look_at constraint |
| `hasBone(slotOrPath)`, `getBonePosition(slotOrPath)`, `getBoneRotation(slotOrPath)` | world space, from the last evaluated pose; humanoid slots (`"left_hand"`) or paths |
| `speed`, `applyRootMotion` | rw |
| `getDeltaPosition()`, `getDeltaRotation()` | the last update's root motion (world space) |
| `isHumanoid()`, `isLoaded()` | |
| `onEvent` | a function called with one `AnimationEvent` per clip or montage event (leader clip per layer, or all with `eventMode = all`); then the scripts' methods named like the event |
| `onStateEnter`, `onStateExit` | functions called with `(layer, state)`: the layer's name and the state's full path (`"Base Layer.Combat.Attack"`) |
| `onMontageEnded` | `(path, interrupted)` |
| `onAnimatorMove` | `(deltaPosition, deltaRotation)`: set, it takes the root motion over (nothing moves the GameObject) |
| `getAnimatorComponent()` | the ECS component |

```lua
function Hero:init()
    self.animator = GameObject(self.scene, self.entity):getAnimator()
    self.animator.onStateEnter = function(layer, state) print("enter", layer, state) end
    RegisterEngineEvent(self, "onUpdate")
end

function Hero:onUpdate()
    local speed = Input.isKeyPressed(Input.KEY_W) and 1 or 0
    self.animator:setFloat("Speed", speed, 0.1, Engine.deltatime)
    if Input.isKeyPressed(Input.KEY_SPACE) then self.animator:setTrigger("Jump") end
end
```

### 4.9 Camera (GameObject) - partial

rw: `type` (`CameraType.*`), `enabled`, `nearClip`, `farClip`, `leftClip`, `rightClip`,
`bottomClip`, `topClip`, `aspect`, `yfov`, `target` (Vector3), `up` (Vector3), `autoResize`,
`renderToTexture`, `transparentSort`, `depthTest`, `cullingMask`, `transparencyMode`
(`TransparencyMode.SORTED` / `ORDER_INDEPENDENT`: weighted blended OIT for alpha-blended materials),
`oitWeightScale` (`jasper://docs/lighting`).
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

### 4.9.1 CameraShake, CameraShaker, ShakeInstance, CameraFollow

Camera shake (MilkShake's Shaker) and a small follow component. On a camera the shake is a view
offset: `position` and `rotation` are never shaken, so scripts and Camera Follow move the camera
freely while it shakes. Without a camera, a Camera Shaker moves its Transform additively (the
base under it is whatever else writes it). Both run after `onUpdate`, before the frame is drawn.
Agents: `jasper://docs/camera`; users: `docs/camera.md`.

A **preset** is a built-in name (`CameraShake.getPresetNames()`: `explosion`, `light_explosion`,
`earthquake`, `punch`, `shock`, `electrocute`, `drunk`, `offroad`, `jumpscare_2d`, `eh_shake`,
`entity_shake`, `invader_drunk`, `invaders_explosion`, `handheld`, `vibration`, `bump`), a
`.shake` path relative to the assets folder, or a table `{type = "one_shot"|"sustained",
strength, roughness, fadeIn, fadeOut, positionInfluence = Vector3, rotationInfluence = Vector3}`
(missing keys: the `.shake` defaults). An unknown name logs one warning and shakes nothing.
**options** (all optional): `{strength, roughness, seed, unscaled, fadeIn, fadeOut}`:
`strength` / `roughness` scale the preset, the same `seed` shakes the same way, `unscaled`
overrides the shaker's `unscaledTime`, the fades override the preset's.

| `CameraShake` (static, a dot) | Notes |
|---|---|
| `shake(preset [, options])` | a `ShakeInstance` on every global Camera Shaker (Global on, enabled, active, every running scene); invalid with a warning when there is none |
| `shakeAt(position, radius, preset [, options])` | the same, strength and roughness scaled by `max(0, 1 - d / radius)` (d from the shaker, x/y only in 2D); none at d >= radius |
| `addTrauma(amount)` | every global shaker; trauma in [0, 1], shake = trauma ^ power, decaying linearly |
| `stopAll([fade])` | every global shaker's shakes fade out over `fade` (default: their fade out; 0: now) |
| `getPresetNames()` | the built-in names (a table) |

| `CameraShaker` (EntityHandle) | Notes |
|---|---|
| `CameraShaker(scene, entity)` | adds the component when missing; `GameObject:getCameraShaker()` returns `nil` without one, `hasCameraShaker()` |
| `shake(preset [, options])`, `shakeAt(position, radius, preset [, options])` | this shaker, whatever its `global`; a disabled shaker keeps the shake for when it is enabled |
| `addTrauma(amount)`, `stopAll([fade])`, `getInstanceCount()` | |
| `trauma` | rw, 0..1 |
| `enabled`, `global`, `unscaledTime` | rw |
| `mode` | rw, `"auto"` (2D for orthographic / UI cameras), `"3d"`, `"2d"` (x/y in tenths of the view height and roll only) |
| `positionScale`, `rotationScale` | rw, gains (>= 0) |
| `startPreset` | rw, played on start and on an enable unless the shake it started still plays (`""` = none) |
| `traumaDecay`, `traumaPower` (1..4), `traumaMaxOffset` (Vector3, units), `traumaMaxAngle` (Vector3, degrees), `traumaFrequency` | rw |
| `offsetPosition`, `offsetRotation` | ro, the offset applied this frame (Vector3; rotation in Euler degrees) |

| `ShakeInstance` (a handle) | Notes |
|---|---|
| `stop([fade])` | fades out over `fade` (default: the preset's fade out; 0: now) |
| `pause([fade])`, `resume([fade])` | the noise slows to a stop over `fade` (default 0) and back; a paused sustained shake holds its offset |
| `isPaused()`, `isFinished()`, `isValid()` | invalid once every instance is gone (finished, its shaker destroyed, its scene unloaded): its calls do nothing |
| `strengthScale`, `roughnessScale` | rw; on a handle from a static call they set every shaker's instance, and read the first live one |
| `state` | ro, `"fading_in"`, `"sustained"`, `"fading_out"`, `"paused"`, `"stopped"` |
| `count` | ro, the live instances (one per shaker reached) |

| `CameraFollow` (EntityHandle) | Notes |
|---|---|
| `CameraFollow(scene, entity)` | adds the component when missing; `GameObject:getCameraFollow()` returns `nil` without one, `hasCameraFollow()` |
| `target` | rw, the GameObject followed (this scene's, or one of the persistent DontDestroyOnLoad scene) or `nil`; another scene's is refused with a warning |
| `offset` (Vector3), `offsetSpace` (`"world"`, `"target"`, `"target_yaw"`) | rw |
| `damping` (Vector3, SmoothDamp seconds per world axis; 0 = rigid), `maxSpeed` (0 = unlimited) | rw |
| `lookAt`, `lookAtOffset` (Vector3), `rotationDamping` | rw; a `useTarget` camera gets its `target` set, others are turned |
| `deadZone` (Vector2, world x/y; orthographic and UI cameras), `useBounds`, `boundsMin`, `boundsMax` | rw |
| `unscaledTime`, `enabled` | rw |
| `snap()` | the next update places it without damping |

Camera Follow owns its Transform while enabled: a script that also moves it is overwritten
(warned once). Writes in `onPostUpdate` reach the camera a frame late.

```lua
-- MilkShake's CameraShaker.cs, ported
function Hazard:onExplode()
    CameraShake.shake("explosion")
end
function Rush:start()  self.heavy = CameraShake.shake("earthquake")  end
function Rush:finish() if self.heavy then self.heavy:stop(0.2) end end

-- a hit: trauma piles up and settles
function Player:onHit(damage)
    CameraShake.addTrauma(math.min(0.6, damage / 50))
end

-- a pause menu that still shakes (Time.timeScale = 0)
CameraShake.shake("shock", {unscaled = true})
```

### 4.10 Light (GameObject) / Light2D (GameObject)

Light rw: `type` (`LightType.*`), `enabled`, `direction` (Vector3; ro `worldDirection`, as of the
last transform update), `color` (Vector3),
`range`, `intensity`, `innerConeAngle`, `outerConeAngle`, `shadows`, `cameraNear`,
`cameraFar`, `automaticShadowCamera`, `numCascades` (0: the quality tier's), `cullingMask`, `shading`
(`LightShading.*`, the stage evaluating it on MIXED-lighting materials), `intensityUnit`
(`LightUnit.ENGINE`, `LUX` for a directional light, `CANDELA` or `LUMEN` for a point or spot;
setting the property keeps the number, which then means the new unit),
`useColorTemperature`, `colorTemperature` (Kelvin, 1000-20000, 6500 white; `color` filters
it); methods `setDirection(x,y,z)`, `setColor(r,g,b)`, `setConeAngle(a)`, `setRange(r)`,
`setIntensity(i)`, `setIntensityUnit(unit, preserveBrightness)` (true converts the intensity so
the light stays as bright, through `scene.physicalLightEV100`), `setShadows(b)`,
`setShadowCameraNearFar(n, f)`.
Shadows (`jasper://docs/shadows`) rw: `shadowType` (`ShadowType.NONE/HARD/SOFT`), `shadowStrength`
(0-1), `shadowBias` and `shadowNormalBias` (shadow texels), `shadowResolution` (0: the tier's),
`cascadeSplitMode` (`CascadeSplitMode.AUTO/MANUAL`), `cascadeSplits` (Vector3, fractions of the
distance), `shadowDistance` (metres, 0: the scene's or the tier's), `shadowAngularDiameter`
(degrees), `shadowSourceRadius` (metres), `shadowUpdateMode` (`ShadowUpdateMode.AUTO/EVERY_FRAME/ON_DEMAND`);
`requestShadowUpdate()` redraws an On Demand light's maps. Contact shadows (directional and spot
lights with shadows): `contactShadows`, `contactShadowLength` (metres), `contactShadowThickness`
(metres), `contactShadowSampleCount` (8..16; `jasper://docs/lighting`).

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
`setAnchorPoints(l, t, r, b)`, `setAnchorOffsets(l, t, r, b)`, `setPivot(x, y)`,
`getSafeAreaComponent()` (the Safe Area component driving it, or nil: `SafeAreaComponent`
rw `left`, `top`, `right`, `bottom`, `includeExtraInsets`, `enabled`; its anchors, offsets and
size are driven while it is there).
Canvas Groups over the element (4.11.1), walked on demand: `getInheritedAlpha()` (Unity
`CanvasRenderer.GetInheritedAlpha`), `getGroupsAllowInteraction()`, `getGroupsBlockRaycasts()`,
`isInteractable()` (the groups and the Button / InputField's own `disabled`; Unity
`Selectable.IsInteractable`).
UI position: use `positionXOffset`/`positionYOffset` (as in the bundled example), or
`position`.

| Class | rw properties | methods |
|---|---|---|
| `Text` | `text`, `font`, `fontSize`, `color` (Vector4), `alpha`, `multiline`, `fixedWidth`, `fixedHeight`, `maxTextSize`, `flipY`, `pivotBaseline`, `pivotCentered`; outline/shadow (TMP underlay; UI units, colours sRGB): `outlineWidth`, `outlineColor`, `shadowEnabled`, `shadowColor`, `shadowOffset` (Vector2, right-down), `shadowSoftness`, `shadowDilate`; `visibleCharacters` (-1 all) | `setColor(r,g,b[,a])`, `setFont(path)`, `setFixedSize(w,h)`, `getNumChars()`, `getCharPosition(i)`, `getLineHeight()` |
| `TextEffect(scene, entity)` (adds the component) | `enabled`, `autoPlay`, `loop`, `useUnscaledTime` (default true), `startIndex`, `length` (-1 to the end), `forWords`; events `onComplete`, `onCharacterRevealed(index)` | `getLayer(i)` (1..4, a `TextEffectLayerRef` whose fields write through: `type` (`TextEffectType.Wave`/`Bounce`/`Shake`/`Pulse`/`Typewriter`/`FadeIn`/`Grow`/`Unfold`/`Rainbow`/`Gradient`/`None`), `enabled`, `amplitude`, `duration`, `spread`, `phaseStep`, `frequency`, `charactersPerSecond`, `fadeTime`, `gradient` (list of `TextEffectGradientStop` {`color` sRGB Vector4, `position`}), `mix` (`TextEffectMix.Multiply`/`Add`/`Replace`), `saturation`, `lightness`), `setLayer(i, layerOrTable)`, `play()`, `stop()` (plain text), `finish()`, `restart()`, `isFinished()`, `isPlaying()` (`jasper://docs/ui`) |
| `Image` | `color` (Vector4), `alpha`, `textureScaleFactor`, `flipY`, `patchMarginLeft/Right/Top/Bottom` | `setTexture(path)`, `setColor(r,g,b[,a])`, `setPatchMargin(m)` / `(l,r,t,b)`, `getUIComponent()` |
| `Button` (Image) | `label`, `labelColor`, `labelFont`, `labelFontSize`, `colorNormal`, `colorHovered`, `colorPressed`, `colorDisabled`, `disabled` | `getButtonComponent()`, `getLabelObject()`, `setTextureNormal/Hovered/Pressed/Disabled(path)`, `setColorNormal(r,g,b[,a])` (and Hovered/Pressed/Disabled), `setLabelColor(r,g,b,a)` |
| `Panel` (Image) | `title`, `titleColor`, `titleFont`, `titleFontSize`, `headerColor`, `minWidth`, `minHeight`, `canMove`, `canResize`, `canBringToFront` | `getPanelComponent()`, `setMinSize(w,h)` |
| `Scrollbar` (Image) | `type` (`ScrollbarType.*`), `barSize`, `step`, `barColor`, `barAlpha` | `getScrollbarComponent()`, `setBarTexture(path)` |
| `Progressbar` (Image) | `type`, `value` (0..1), `fillColor`, `fillAlpha` | `getProgressbarComponent()`, `setFillTexture(path)` |
| `InputField` (Image) | `text`, `disabled`, `textColor`, `textFont`, `fontSize`, `maxTextSize`, `placeholder`, `placeholderColor`, `password`, `passwordChar`, `cursorIndex`, `cursorColor`, `selectionColor`, `cursorBlink`, `cursorWidth` | `getInputFieldComponent()`, `getTextObject()`, `setSelection(...)` |
| `OnScreenStick` (Image) | `enabled`, `mode` (`OnScreenStickMode.*`), `background`, `handle` (entities), `handleRange`, `deadZone`, `moveThreshold`, `axes` (`OnScreenStickAxes.*`), `snapX`, `snapY`, `hideWhenReleased`, `clampToArea`, `horizontalAction`, `verticalAction`, `visibility`; ro `value` (Vector2, y up), `pressed`, `pointerId`; events `onValueChanged(x, y)`, `onPress`, `onRelease` | `getOnScreenStickComponent()` |
| `OnScreenButton` (Image) | `enabled`, `action`, `value`, `pressWhenPointerEnters`, `releaseWhenPointerLeaves`, `visibility`; ro `pressed`; events `onPress`, `onRelease` | `getOnScreenButtonComponent()` |
| `TouchArea` (Image) | `enabled`, `horizontalAction`, `verticalAction`, `sensitivity`, `invertX`, `invertY`, `visibility`; ro `delta` (this frame, canvas px), `pressed`, `pointerId`; event `onDrag(dx, dy)` | `getTouchAreaComponent()` |

The three on-screen controls feed input actions (`jasper://docs/input` 4.1); wrapping a GameObject
without the component adds it.

Component structs returned by `get*Component()` expose the event objects of section 1.3 and
some raw fields (`ButtonComponent.pressed`, `hovered` ro; `disabled` rw;
`ProgressbarComponent.value` rw; `InputFieldComponent.text` ro). Prefer the wrapper
properties for changes; raw component writes need the matching `needUpdate*` flag.
A Button's `colorDisabled` defaults to Unity's disabledColor (sRGB 200, 200, 200, 128): it also
shows under a non-interactable Canvas Group.

#### 4.11.1 CanvasGroup (EntityHandle)

Unity's CanvasGroup: fades, disables or makes click-through a UI subtree. Rules, the Hide
pattern, drag and drop, render modes: `jasper://docs/ui`.

- `CanvasGroup(scene, entity)` wraps the GameObject's group and ADDS one when it has none (Unity
  `GetComponent ?? AddComponent`); `CanvasGroup(scene)` makes a new GameObject: a RectTransform
  stretched to its parent with a group.
- rw `enabled`, `alpha` (clamped to 0..1; NaN rejected with a warning), `interactable`,
  `blocksRaycasts`, `ignoreParentGroups`, `renderMode` (`CanvasGroupRenderMode.PER_ELEMENT` |
  `FLATTEN`).
- `fadeTo(alpha, duration [, opts])`, `fadeIn(duration [, opts])`, `fadeOut(duration [, opts])`
  return the running `AlphaTween`; `opts` = `{ease = EaseType.*, unscaled = false, deactivate =
  false, onComplete = function}`. A new fade stops the previous one; on completion the final
  alpha is written, then `onComplete` runs, then `deactivate` calls `setActive(false)`.
- `stopFade([jumpToEnd = false])` (jumpToEnd writes the end alpha and completes the fade),
  `isFading()`, `getGameObject()`, `getCanvasGroupComponent()`.
- GameObject: `getCanvasGroup()` (nil without one), `hasCanvasGroup()`, `addCanvasGroup()`,
  `removeCanvasGroup()`.

```lua
function DelayedUI:onEnable()
    local group = CanvasGroup(self.scene, self.entity)
    group.alpha = 0; group.interactable = false; group.blocksRaycasts = false   -- hidden, click-through
    group:fadeTo(1, 0.3, {onComplete = function() group.interactable = true; group.blocksRaycasts = true end})
end
```

### 4.12 AudioSource (EntityHandle)

Clips that vary (footsteps, hits: no repeats, random pitch and volume) need no script: a Sound Event does it as data (`edit_sound_event`, group audio; then `event` on the AudioSource, `jasper://docs/sound`).

Methods: `play()`, `pause()`, `stop()`, `seek(t)`, `loadSound(path)`, `destroySound()`,
`isPlaying()`, `isPaused()`, `isStopped()`, `getLength()`, `getPlayingTime()`,
`getGameObject()`, `setMinMaxDistance(min, max)`, `setInaudibleBehavior(mustTick, kill)`,
`getAudioSourceComponent()`.
rw: `enabled`, `volume`, `speed` (`pitch` is the same), `pan`, `looping`, `loopingPoint`, `sound3D`,
`clockedSound`, `protectVoice`, `minDistance`, `maxDistance`, `attenuationModel`
(`AudioSourceAttenuation.*`), `attenuationRolloffFactor`, `dopplerFactor`, `outputGroup` (the
mixer group by name, "" = Master), `loadType` (`AudioLoadType.*`, Auto streams files of 1 MiB and
more), `priority` (0 most important .. 256, default 128), `maxVoices` (1..64, default 8),
`ignoreListenerPause`.
Sound Events and triggers: `event` (rw: its path; assigning a path or GUID resolves it, `""` clears it;
set, the source plays the event and its clip is ignored), `playOn` (rw, `AudioPlayOn` flags),
`minImpactSpeed`, `fullVolumeImpactSpeed` (contact triggers: below the first nothing plays; below the
second the volume scales with the speed, 0 = no scaling), `pitchRandomSemitones` (0..24),
`volumeRandomDb` (0..60): clip mode's variation of each start.

Mixer and voices: `playOneShot(path [, volumeScale])` (an extra voice over what plays, a clip or a
Sound Event's pick by its `.soundevent` path or GUID, Unity's
PlayOneShot; the oldest one-shot is stolen past `maxVoices`), `fadeIn(seconds [, opts])`,
`fadeOut(seconds [, opts])` (`opts.pause` pauses instead of stopping; the volume comes back for
the next play), `fadeTo(volume, seconds [, opts])`, `isFading()`, `getVoiceCount()`,
`getResolvedLoadType()`; static `AudioSource.playClipAtPoint(scene, path, position [, volume
[, group]])` (stops with `scene`). `isPlaying()` follows the voice: true for a looping clip at any
pitch, and right after `play()`. `play()` on a playing source starts it over; two sources play
one clip at once.

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
| `applyForce(forceV2, pointV2 [, wake])` | method | point in world points; `wake` (all six) defaults to true: a sleeping body wakes, as Unity's `AddForce` (false: Box2D skips a sleeping body) |
| `applyForceToCenter(forceV2 [, wake])` | method | |
| `applyTorque(t [, wake])` | method | |
| `applyLinearImpulse(impulseV2, pointV2 [, wake])` | method | |
| `applyLinearImpulseToCenter(impulseV2 [, wake])` | method | |
| `applyAngularImpulse(i [, wake])` | method | |
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

### 4.13.2 Collision2D, ContactPoint2D, Collider2D (the 2D callbacks' values, read-only)

| Class | Members |
|---|---|
| `Collision2D` | `gameObject`, `collider`, `rigidbody` (the other side's), `thisCollider`, `thisRigidbody`, `otherCollider` / `otherRigidbody` (**this** side: Unity's names), `relativeVelocity` (Vector2, points/s, other minus this; Enter: before the step), `enabled`, `contactCount`, `getContact(i)`, `contacts` |
| `ContactPoint2D` | `point`, `normal` (toward this one), `separation` (points), `normalImpulse`, `tangentImpulse`, `relativeVelocity`, `thisCollider`, `otherCollider` |
| `Collider2D` | `gameObject`, `rigidbody` / `attachedRigidbody`, `index`, `type` (`Collider2DType`), `isTrigger` (sensor), `tag`, `compareTag(tag)`, `receiver`, `isValid()` |

```lua
function UpsideDown:onCollisionEnter2D(c)
    if c.gameObject.tag == "Ground" and c.thisCollider.index == self.detectorCollider then
        GameOver(self)   -- the game's own function
    end
end
```

#### 4.13.1 CharacterController2D (EntityHandle)

`go:getCharacterController2D()` (adds one with defaults if missing), `go:removeCharacterController2D()`,
or `CharacterController2D(scene, entity)`. Points and points per second, as Rigidbody2D.

| Member | Kind | Notes |
|---|---|---|
| `move(motionV2)` | method | slides by motion now (no gravity), writes the Transform; returns `CollisionFlags` bits |
| `simpleMove(speedV2)` | method | speed.y ignored, own gravity (`Physics` 2D gravity); returns `isGrounded` |
| `center` | rw `Vector2` | offset from the Transform position |
| `height`, `radius`, `slopeLimit`, `stepOffset`, `skinWidth`, `minMoveDistance`, `pushStrength` | rw number | defaults: height 128, radius 32 (2 m x 0.5 m at 64 points a metre) |
| `pushBodies`, `movingPlatforms`, `enabled` | rw bool | |
| `setBitsFilter(category, mask)`, `categoryBits`, `maskBits` | method / ro | |
| `isGrounded`, `isOnSteepSlope`, `collisionFlags`, `velocity`, `groundNormal`, `groundEntity`, `groundVelocity` | ro | the last move and physics step |

`CollisionFlags.NONE`, `SIDES` (1), `ABOVE` (2), `BELOW` (4): test with `flags & CollisionFlags.SIDES ~= 0`.

### 4.14 Rigidbody (EntityHandle) - 3D physics (Jolt)

| Member | Kind | Notes |
|---|---|---|
| `position` | rw `Vector3` | teleport |
| `rotation` | rw `Quaternion` | |
| `linearVelocity`, `angularVelocity` | rw `Vector3` | also `setLinearVelocityClamped(v)`, `setAngularVelocityClamped(v)` |
| `mass` | rw property | kg; setting it turns `automaticMass` off (2D uses `getMass()` method) |
| `automaticMass`, `centerOfMass`, `automaticCenterOfMass`, `inertiaTensor`, `inertiaTensorRotation`, `automaticInertiaTensor` | rw | setting a value turns its automatic off; `resetCenterOfMass()`, `resetInertiaTensor()` |
| `linearDamping`, `angularDamping` | rw numbers | Unity drag / angularDrag, 0.05 by default (Jolt's) |
| `maxLinearVelocity`, `maxAngularVelocity` | rw numbers | m/s, rad/s |
| `interpolation` | rw `RigidbodyInterpolation.*` | `SCENE` (default), `NONE`, `INTERPOLATE`, `EXTRAPOLATE` |
| `type` | rw `BodyType.*` | switches live (same body, joints and contacts); `isKinematic` (rw) is Unity's; `setType(t)` returns false for a body with a mesh / heightfield collider |
| `isSleeping()`, `sleep()`, `wakeUp()` | methods | |
| `material` | rw `PhysicsMaterial` | default material of the colliders without one |
| `motionQuality` | rw | `RigidbodyMotionQuality.DISCRETE` / `LINEAR_CAST` |
| `friction`, `restitution`, `gravityFactor`, `sensor`, `allowSleeping`, `enabled`, `shapeDensity`, `collisionGroupID`, `categoryBitsFilter`, `maskBitsFilter`, `collideKinematicVsNonDynamic` | rw | `friction` / `restitution`: the surface of colliders without a material |
| `buoyancy`, `waterDrag`, `waterAngularDrag` | rw numbers | floating in a `library:water` (2.10.1): Jolt's buoyancy at the centre of mass each fixed step of a dynamic body, 1 neutral, more floats, less sinks, 0 (default) ignores water; drags default 0.5 and 0.01 |
| `applyForce(f)`, `applyForce(f, point)` | method | f in Vector3; single-arg = at center (no `applyForceToCenter` in 3D) |
| `applyImpulse(i)`, `applyImpulse(i, point)`, `applyTorque(t)`, `applyAngularImpulse(a)` | methods | |
| `activate()`, `deactivate()` | methods | |
| `createBoxCollider(w, h, d)`, `createSphereCollider(r)`, `createCapsuleCollider(halfH, r)`, `createCylinderCollider(halfH, r)`, `createTaperedCapsuleCollider(...)`, `createConvexHullCollider(...)`, `createMeshCollider(...)`, `createHeightFieldCollider(...)` | methods | each also with `(Vector3 pos, Quaternion rot, ...)` prefix overload; any number of colliders, vertices and indices |
| `getNumColliders()` / `getColliderCount()`, `getColliderType(i)`, `removeCollider(i)` (later indices shift), `removeAllColliders()`, `load()`, `getAttachedObject()` | methods | |
| `setColliderPosition(i, v)`, `setColliderRotation(i, q)`, `setColliderBoxSize(i, v)`, `setColliderRadius(i, r)`, `setColliderHalfHeight(i, h)`, `setColliderTaperedRadii(i, top, bottom)`, `setColliderVertices(i, vertices [, indices])` and their getters | methods | swap the shape at the next step: the body keeps its id, joints and contacts |
| `setColliderTrigger(i, on)`, `isColliderTrigger(i)` | methods | Unity Is Trigger per collider: passes through, sensor contacts, no mass |
| `setColliderMaterial(i, m)` (a `PhysicsMaterial`, a path, or nil), `getColliderMaterial(i)`, `setColliderFriction(i, f)`, `setColliderBounciness(i, b)` and getters | methods | friction / bounciness give the collider its own copy of its material (Unity `collider.material`) |
| `setAllowedDOFsAll()`, `setAllowedDOFs2DPlane()`, `setAllowedDOFs(...)`, `setOverrideMassAndInertia(...)` | methods | |
| `getAllowedDOFs()` | method | -> translationX, translationY, translationZ, rotationX, rotationY, rotationZ (booleans, `setAllowedDOFs` order) |
| `overrideMassProperties` | ro bool | `setOverrideMassAndInertia` was called |
| `getPointVelocity(p)`, `getAccumulatedForce()`, `getCenterOfMassPosition()` | methods | |

`Contact3D` (global events): `getApproachSpeed()` (how fast the bodies came together along the
normal before the solver), `getWorldSpaceNormal()`, `getPenetrationDepth()`, `getBaseOffset()`,
`getColliderIndex1()`, `getColliderIndex2()`, `getRelativeContactPointsOnA(i)` / `OnB(i)`,
`numContactPoints`, `snapshot` (true after the step: setters apply from the next step), rw `combinedFriction`,
`combinedRestitution`, `sensor`.

#### Collision, ContactPoint, Collider (the 3D callbacks' values, read-only)

| Class | Members |
|---|---|
| `Collision` | `gameObject`, `rigidbody`, `collider` (the other side's), `thisCollider`, `thisRigidbody`, `impulse` (Vector3, estimated N s on this body), `relativeVelocity` (Vector3, other minus this), `contactCount`, `getContact(i)` (0-based, error out of range), `contacts` (new 1-based table each read) |
| `ContactPoint` | `point`, `normal` (from the other collider toward this one), `separation` (< 0 penetrating), `impulse`, `thisCollider`, `otherCollider` |
| `Collider` | `gameObject`, `rigidbody` / `attachedRigidbody`, `index`, `type` (`Collider3DType`), `isTrigger`, `tag`, `compareTag(tag)`, `receiver` (GameObject or nil), `isValid()`; `==` compares GameObject and index |

Values are copies taken at the step (safe to keep); a GameObject gone since reads as nil.

**Joint3D** (EntityHandle; `go:getJoint3D()`, `Joint3D(scene, entity)`, `Joint3D(scene)` a new
GameObject): rw `enabled`, `type` (`Joint3DType.*`), `bodyA`, `bodyB` (alias `connectedBody`;
Entity or GameObject, nil = the world), `anchorSpace` (`JointAnchorSpace.LOCAL` for new joints),
`anchor`, `anchorB` (alias `connectedAnchor`), `autoAnchors` (alias
`autoConfigureConnectedAnchor`), `anchorA`, `axis`, `normal` (alias `secondaryAxis`), `twistAxis`,
`planeAxis` (alias `swingAxis`), `axisX`, `axisY`, `enableCollision`, `breakForce`, `breakTorque`,
`breakAction`, `useLimits`, `limitsMin`, `limitsMax`, `limitsSpringFrequency`,
`limitsSpringDamping`, `useMotor`, `motorTargetVelocity`, `motorMaxForce`, `useSpring`,
`springStiffness`, `springDamper`, `springTargetPosition`, `maxFriction`, `minDistance`,
`maxDistance`, `coneHalfAngle`, `twistMinAngle` / `twistMaxAngle` (aliases `lowTwistLimit` /
`highTwistLimit`), `normalHalfConeAngle` (alias `swing1Limit`: about the swing axis),
`planeHalfConeAngle` (alias `swing2Limit`: about the normal of twist and swing axes), `swingType` (`SwingType.CONE` / `PYRAMID`), `motorTargetRotation`,
`motorSpringFrequency`, `motorSpringDamping`, `xMotion` ... `angularZMotion` (`JointMotion.LOCKED` /
`LIMITED` / `FREE`), `linearLimitMin/Max`, `angularLimitMin/Max`, `useDrive`, `driveStiffness`,
`driveDamping`, `angularDriveStiffness`, `angularDriveDamping`, `driveMaxForce`, `driveMaxTorque`,
`targetPosition`, `targetRotation`, `targetVelocity`, `targetAngularVelocity`. Read-only:
`currentForce`, `currentTorque` (Vector3, N and N m), `angle`, `velocity` (hinge), `position`,
`speed` (prismatic), `currentLength` (distance), `broken`, `live`. Angles in the engine's unit,
bodyA's relative to the connected body from the pose the joint is made in. Breaking calls
`onJointBreak(force, joint)` on the joint's GameObject and bodyA's scripts. The old helpers
`setFixedJoint`, `setHingeJoint`, ... `setPulleyJoint` (world points) still work.

```lua
local hinge = Joint3D(self.scene)          -- a door on its frame
hinge.type = Joint3DType.HINGE
hinge.bodyA = self.door                    -- Unity's "this"
hinge.connectedBody = self.frame
hinge.anchor = Vector3(-0.5, 0, 0)         -- in the door's local space
hinge.axis = Vector3(0, 1, 0)
hinge.useLimits = true
hinge.limitsMin, hinge.limitsMax = 0, 110
```

**Ragdoll** (EntityHandle; `go:getRagdoll()`): rw `enabled`, `simulated` (false: the parts are
kinematic and follow the animation; true: dynamic, the Animation Player paused), `inheritVelocity`,
`selfCollision`, `totalMass`, `root`, `animator`; ro `partCount`; `getParts()` (table of
`Rigidbody`), `applyImpulse(impulse, point)` (the part nearest the point; after `simulated = true`
in the same frame too), `applyForce(force)` (every part by its mass); event
`onBeforeAnimatedRestore` (`RegisterEvent(self, ragdoll.onBeforeAnimatedRestore, "fn")`, fn(entity)
before a ragdoll goes back to animated). Made by the Ragdoll wizard (`create_ragdoll`).

#### 4.14.1 CharacterController (EntityHandle)

`go:getCharacterController()` (adds one with defaults if missing), `go:removeCharacterController()`,
or `CharacterController(scene, entity)`. World units (metres).

| Member | Kind | Notes |
|---|---|---|
| `move(motionV3)` | method | sweeps by motion now (no gravity), writes the Transform; returns `CollisionFlags` bits |
| `simpleMove(speedV3)` | method | speed.y ignored, own gravity; returns `isGrounded` |
| `trySetSize(height, radius, centerV3)` | method | false (old size kept) when it does not fit there |
| `center` | rw `Vector3` | setting `center`, `height`, `radius` goes through `trySetSize` |
| `height`, `radius`, `slopeLimit`, `stepOffset`, `skinWidth`, `minMoveDistance`, `mass`, `pushStrength` | rw number | Unity defaults: 2, 0.5, 45, 0.3, 0.02, 0.001; mass 70, push 100 N |
| `pushBodies`, `movingPlatforms`, `inheritPlatformYaw`, `detectCollisions`, `characterCollisions`, `enhancedEdgeRemoval`, `enabled` | rw bool | |
| `setBitsFilter(category, mask)`, `categoryBits`, `maskBits` | method / ro | |
| `isGrounded`, `isOnSteepSlope`, `collisionFlags`, `velocity`, `groundNormal`, `groundEntity`, `groundVelocity` | ro | the last move and physics step |

Script hook `function MyScript:onControllerColliderHit(hit)`: `hit.controller`, `hit.entity`,
`hit.point`, `hit.normal`, `hit.moveDirection`, `hit.moveLength`.

### 4.15 Actions and Tweens

Action (EntityHandle): `start()`, `pause()`, `stop()`, `restart()` (stops a running or paused
action and starts it from the beginning, in the same update), `isRunning()`, `isPaused()`,
`isStopped()`, `getTimeCount()`, `getActionComponent()`; rw `target` (Entity),
`ownedTarget`, `speed` (scales the action's own time: 2 plays twice as fast, a negative speed
plays back to the start and holds there), `weight`; `setTarget(gameObjectOrEntity)`.
`stop()` also cancels a `start()` asked for earlier in the same frame.

Tween (Action): rw `duration`, `loop`, `pingPong` (forward and then back, in twice the
duration), `useUnscaledTime` (advances at time scale 0, default false: scaled like Unity), `curve` (an `AnimationCurve`: the ease when the function type is `EaseType.CURVE`, the value over the
normalized time 0-1); `setFunctionType(EaseType.*)`, `getTime()`, `getValue()`, `getTweenComponent()` (its `curve` too).
A non-looping tween stops on the update it reaches its end, after applying the end value.

| Tween | `setAction(...)` (pass every argument; C++ defaults do not apply) |
|---|---|
| `PositionTween` | `(startV3, endV3, duration, loop)` |
| `RotationTween` | `(startQuat, endQuat, duration, loop)`; `setSpinAction(startQuat, axisV3, angle, duration, loop)` turns by `angle` (any size, so full turns) around `axis` |
| `ScaleTween` | `(startV3, endV3, duration, loop)` |
| `ColorTween` | `(startV3, endV3, duration, loop)` or `(startV4, endV4, duration, loop)` |
| `AlphaTween` | `(startAlpha, endAlpha, duration, loop)`; rw `alphaTarget`: `AlphaTweenTarget.AUTO` (default: the target's enabled Canvas Group when it has one, else its colour alpha), `COLOR`, `CANVAS_GROUP` |

```lua
local tw = PositionTween(self.scene)                    -- new action entity
tw:setTarget(GameObject(self.scene, self.entity))
tw:setAction(Vector3(0, 0, 0), Vector3(200, 0, 0), 1.0, false)
tw:setFunctionType(EaseType.QUAD_OUT)
tw:start()
```

Audio actions (an action GameObject with an `AudioSourceComponent`) and Mixer Snapshot actions start
and stop with the Action API above (their sound follows; a standalone audio action stops itself when its
sound ends, so `onStop` fires). `GameObject:getAudioSnapshotComponent()` (nil without one) gives the
`AudioSnapshotComponent`: rw `snapshot` (a mixer snapshot name), `seconds`, `unscaled`, `revert` (back to
the previous snapshot on stop, while the mixer still shows this one), `revertSeconds` (-1: `seconds`).

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

### 4.16 ParticleSystem

A GameObject with a Particle System (`ParticleSystem(scene)` makes one, as does origin/main's
`ParticleSystem(scene, true)`; a script property of
type ParticleSystem, or `object(ref, 'ParticleSystem')` in eval_lua). Methods: `play()`,
`pause()`, `stop([clear])` (true also removes the particles), `restart()`,
`emit(count [, {position, velocity, size, color, lifetime}])` (born at the system's next tick), `seek(t)` (restart and simulate to
t seconds), `step(ticks)`, `sendTrigger(name)` (the effect's Trigger sub-emitters named so),
`setFloat(name, v)` / `setVector3(name, v)` / `setColor(name, v)` and `getFloat` / `getVector3` /
`getColor` for the effect's exposed parameters (a set returns false for a name it does not expose),
`setInlineEffect(json)` / `getInlineEffect()`, `addCollisionPlane(gameObject)` /
`clearCollisionPlanes()`. rw: `enabled`, `playOnAwake`, `playbackSpeed`, `seed` (0: a new seed
at every play), `effect` (a .particles path; "" plays the inline effect), `useUnscaledTime` (the
GameObject's own: it ticks with the unscaled frame time and plays at `Engine.timeScale` 0, a pause
menu or UI particles; the effect's `ps.main.useUnscaledTime` does the same for every system playing
it). ro: `isPlaying`, `isPaused`, `isEmitting`, `particleCount`, `time`.

Events, one batch per frame on the main thread after the simulation (plain tables, 1-based):
`onCollision(fn)` (position, normal, velocity, particleId, killed; at most the Collision
module's maxCollisionEvents per tick), `onParticleDeath(fn)` (position, velocity, particleId),
`onSubEmit(fn)` (position, entry, count, trigger: "birth", "death", "collision" or "trigger").

```lua
local ps = ParticleSystem(scene)
ps.effect = "fx/sparks.particles"
ps:play()
ps:onCollision(function(events)
    print(#events .. " hits, first at " .. events[1].position.y)
end)
```

Deprecated (the old Action API, kept for old scripts: they edit the inline effect and warn
once): rw `maxParticles`, `rate`, `maxPerUpdate`, `emitter`, `loop`, `localSpace`,
`emitterShape` (`ParticleEmitterShape.*`), `running`, `target` (the system's own GameObject: it
lives on the old target now), `speed` (= `playbackSpeed`); `isRunning()`, `isStopped()`,
`getTimeCount()` (= `time`); `start()`, `reset()`, `setTarget(go)`,
`addBurst(time, count)` / `addBurst(time, minCount, maxCount)`, `setBursts(...)`,
`clearBursts()`, `setLifeInitializer(min [, max])`, `setPositionInitializer(minV3 [, maxV3])`,
`setSpherePositionInitializer(...)`, `setHemispherePositionInitializer(...)`,
`setCirclePositionInitializer(...)`, `setConePositionInitializer(...)`,
`setVelocityInitializer(...)`, `setAccelerationInitializer(...)`, `setColorInitializer(...)`,
`addColorGradientStop(...)`, `setColorGradient(...)`, `clearColorGradient()`,
`setAlphaInitializer(...)`, `setSizeInitializer(...)`, `setSpriteIntializer(...)`,
`setRotationInitializer(...)`, `setScaleInitializer(...)` and
`set*Modifier(fromTime, toTime, from, to [, EaseType])` for Position/Velocity/Acceleration/
Color/Alpha/Size/Sprite/Rotation/Scale. Component: `getParticleSystemComponent()` (the old misspelt `getParticlesomponent()` still works,
deprecated). The old Action's `isPaused()` method is the property `isPaused` now: write
`ps.isPaused` (calling it fails). More: jasper://docs/particles.

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

### 4.17.1 VideoPlayer (EntityHandle)

Unity's VideoPlayer (Add Component > Video > Video Player; jasper://docs/video).
`self.gameObject:getVideoPlayer()` (nil without one; `hasVideoPlayer()`), `VideoPlayer(scene)`
makes a GameObject with one.

- Settings (read/write): `enabled`, `source` (`VideoPlayerSource.*`), `clip` (asset path), `url`
  (`https://...`, `data://...`, absolute or asset path), `playOnAwake`, `waitForFirstFrame`,
  `isLooping`, `skipOnDrop`, `playbackSpeed` (0.0625..16), `timeUpdateMode`
  (`VideoTimeUpdateMode.*`), `renderMode` (`VideoRenderMode.*`), `targetCamera` (a GameObject or
  nil: the main camera), `targetCameraAlpha`, `aspectRatio` (`VideoAspectRatio.*`),
  `targetMaterialRenderer` (GameObject or nil: this one), `targetMaterialProperty`
  (`"baseColorTexture"`, a shader `@texture` uniform), `targetSubmesh` (-1 all),
  `renderTextureWidth`/`renderTextureHeight` (0: the video's), `audioOutputMode`
  (`VideoAudioOutputMode.*`), `directVolume`, `directMute`, `targetAudioSource`.
- `time` (seconds; setting it seeks), `frame` (setting it seeks), read only: `canSetTime`,
  `frameCount`, `frameRate`, `length`, `width`, `height`, `isPrepared`, `isPlaying`, `isPaused`,
  `prepareProgress` (0..1), `texture` (a `Texture`, nil before prepared), `backend`, `videoCodec`,
  `audioCodec`, `error`.
- `prepare()`, `play()`, `pause()`, `stop()`, `stepForward()`, `getFramebuffer()`
  (`image:setTexture(vp:getFramebuffer())`), `VideoPlayer.canPlay(pathOrUrl)` ("probably", "maybe",
  ""), `VideoPlayer.clearCache([url])`, `getVideoPlayerComponent()` (the raw fields).
- Events (function properties, called with the player): `onPrepareCompleted`, `onStarted`,
  `onLoopPointReached`, `onErrorReceived(player, message)`, `onSeekCompleted`,
  `onFrameReady(player, frameIndex)`.

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
- `CapsuleShadowCaster` (capsule shadows, `jasper://docs/lighting`): rw `enabled`, `castShadows`
  (`CapsuleCastShadows.ON` / `CAPSULES_ONLY`); ro `capsuleCount`; `addCapsule(bone, radius, length)`
  (its index, -1 past 8), `removeCapsule(i)`, `clearCapsules()`, `get/setCapsuleBone(i)`,
  `get/setCapsuleRadius(i)`, `get/setCapsuleLength(i)`, `get/setCapsuleOffset(i)`,
  `get/setCapsuleDirection(i)` (`CapsuleDirection.X_AXIS/Y_AXIS/Z_AXIS`), `fitToSkeleton()` (false:
  no humanoid bones under the GameObject).
- `Fog`: rw `type` (`FogType.*`), `color`, `density`, `linearStart`, `linearEnd`. Height fog
  (`type = FogType.HEIGHT`, see `jasper://docs/atmosphere`): rw `colorMode` (`FogColorMode.*`),
  `attenuationDistance`, `baseHeight`, `maximumHeight`, `startDistance`, `maxFogDistance`,
  `maxOpacity`, `directionalInscattering`, `anisotropy`, `skyFill`, `skyFillAngle`; volumetric
  fog: rw `volumetric`, `volumetricDistance`, `lightShafts`, `noiseIntensity`, `noiseScale`,
  `wind` (Vector3, also `setWind(x, y, z)`), `qualityOverride` (`AtmosphereQualityOverride.*`);
  `applyPreset(id)` ("morning_haze", "ground_mist", "mountain_valley", "horizon_haze", "night";
  false for an unknown id). Colours are sRGB here, linear in the component.
- `LocalVolumetricFog` (GameObject, `LocalVolumetricFog(scene)`): rw `shape`
  (`FogVolumeShape.*`), `size` (Vector3, before the Transform's scale; also `setSize(x, y, z)`),
  `attenuationDistance`, `albedo` (also `setAlbedo(r, g, b)`), `blendDistance`, `falloff`
  (`FogVolumeFalloff.*`), `noiseIntensity`, `noiseScale`, `scrollSpeed`. Shows only through a
  height fog's volumetric fog.
- `VolumetricClouds` (`VolumetricClouds(scene)`; a scene draws its first): rw `preset`
  (`CloudsPreset.*`: setting it applies the look), `rendering` (`CloudsRendering.*`), `coverage`,
  `density`, `shapeScale`, `shapeFactor`, `erosionFactor`, `erosionScale`, `bottomAltitude`,
  `altitudeRange`, `earthCurvature`, `windDirection`, `windSpeed`, `weatherMap` (image path, ""
  for the built-in map), `weatherMapScale`, `sunLightDimmer`, `ambientLightDimmer`,
  `maxDistance`, `qualityOverride`.
- `Light`: also rw `volumetricIntensity` (point and spot lights in High quality volumetric fog).
- `Skybox`: `setTextures(positiveX, negativeX, positiveY, negativeY, positiveZ, negativeZ)` (six
  images; `+Z` is the front), `setTexture(path)` (one image holding the six faces),
  `setTexturePositiveX(path)` ... `setTextureNegativeZ(path)` (one face), rw `color`, `alpha`, `rotation`.
- `Texture` (`Texture(path)`, or a component's): `load()`, ro `width`, `height` (the source's size:
  layout never changes when Max Size, the texture quality or the budget shrink the GPU image),
  `getStoredWidth()`, `getStoredHeight()` (what the GPU holds), `isCompressed()`,
  `getGpuFormat()` (`"BC7"`, `"ASTC_6x6"`, `"RGBA8"`...), `getData()` (the pixels: empty, with a
  warning once, for a GPU-compressed texture; set Read/Write in its import settings, or reference
  it from a CPU slot such as a terrain height map).
- `Terrain`, `ReflectionProbe`, `Mirror`, `Bone`: editor-configured; see bindings. A Terrain's painted layers (`TerrainSurfaceLayer`) also have `hexTiling` (anti-repetition of the flat projection) and `hexRotation` (0..1).
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
  `reload([{mode = "patch"|"rebuild"}])`, `show()`, `hide()`, `getElementById(id)`, `querySelector(sel)`,
  `querySelectorAll(sel)`, `createElement(tag)`, `createTextNode(text)`,
  `addEventListener(type, fn[, capture|{capture=, once=}])`, `removeEventListener(...)`,
  `createDataModel(name, table)` (returns the model proxy), `getDataModel(name)`,
  `removeDataModel(name)`, `getTargetTexture()` (TEXTURE mode: a `Framebuffer`, else nil),
  `getUIDocumentComponent()` (rw `filename`, `sortOrder`, `enabled`, `interactive`,
  `renderMode` (`UIDocumentRenderMode.SCREEN`/`TEXTURE`, applied at the next load: set
  `needReload = true`), `textureWidth`, `textureHeight`, `inputSurface`, `pixelRatio`,
  `needReload`; ro `loaded`; events `onLoad`, `onUnload`); events `onLoad`, `onUnload`,
  `onReload` (after an in-place patch, with the report); ro `lastReloadReport` (`{mode, file,
  kept, updated, inserted, removed, replaced, runtimeKept, scriptsRerun, fallbackReason, ms,
  causes}`, nil before a reload); `tooltip` (`visible`, `source`, `element`, `text`,
  `show(el)`, `hide()`, `configure{enabled, delay, hopWindow}`); static
  `UIDocument.markdownToHtml(text)`;
  static `UIDocument.setDebuggerVisible(b)`, `isDebuggerVisible()`, `getLiveLuaRefCount()`.
  Getters load a document that is due, so `init` can query elements.
- `<particle-system>` elements (`jasper://docs/html-ui`): `el:play()`, `el:pause()`, `el:stop()`,
  `el:isPlaying()`, `el:clear()` (its live particles go), `el:getParticleSystem()` (the
  `ParticleSystem` of its hidden entity: every particle API works; `nil` before the element's first
  update or when its `src` failed; play/pause/stop before that are queued); event `complete`. Text effects:
  `el.style.textEffect = "wave(0.15em 1.5s)"`, `el:restartTextEffect()`, event `texteffectend`.
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
  Every element: `setMarkdown(text)`, `setTooltip(textOrElementOrFunction[, {placement, delay,
  arrow, interactive}])`, `clearTooltip()`; ro `itemsView` (a `<list-view>`, `<grid-view>`,
  `<tree-view>` or `<table-view>`'s `UIItemsView`, else nil).
- `UIItemsView` (1-based indices): ro `element`, `kind`, `count`; rw `activeIndex`;
  `setProvider{count, bind = fn(row, i), unbind, key = fn(i), height = fn(i), children =
  fn(key) (nil: the roots), hasChildren = fn(key)}`, `clearProvider()`, `setCount(n)`, `refresh()`,
  `refreshItems{...}`, `scrollToIndex(i[, "start"|"center"|"end"|"nearest"])`,
  `getVisibleRange()` (first, last), `setSelection{...}`, `getSelection()`,
  `getSelectedKeys()`, `activate(i)`, `keyOf(i)`, `indexOfKey(key)`, `getRow(i)`,
  `indexOf(el)`, `expand(key)`, `collapse(key)`, `toggle(key)`, `isExpanded(key)`,
  `getNode(i)` (`{key, depth, expanded, hasChildren, parent}`), `getColumns()`,
  `setColumnWidth(key, w)`, `sortBy(key[, "asc"|"desc"])`, `getState()`. Events on the view:
  `select` (`detail = {indices, keys}`), `activate`, `expand`, `collapse` (`detail = {index,
  key}`), `sort` (`key`, `direction`), `columnresize`.
- `UIDataTransfer` (`event.dataTransfer` of the drag events, valid until `dragend`):
  `setData(type, value)` (any Lua value), `getData(type)`, ro `types`, `clearData([type])`,
  rw `effectAllowed`, `dropEffect`, `setDragImage(elOrNil[, x, y])`, `isValid()`. `UIDrag`:
  `begin(el[, {data = {type = value}, effectAllowed}])`, `over(el)`, `drop(el)`, `cancel()`,
  `isActive()`, `getDataTransfer()`.
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
  `detail`, `defaultPrevented`, `dataTransfer`, `dropIndex`, `dropPosition` (drag events
  over a virtual view: `"before"`, `"after"`, `"on"`); `stopPropagation()`, `stopImmediatePropagation()`,
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

### 4.20 Spline, SplineExtrude, SplineInstantiate, SplineAnimate (EntityHandle)

A GameObject's curve and what it builds (`jasper://docs/splines`). `self.entity:getSpline()`,
`getSplineExtrude()`, `getSplineInstantiate()`, `getSplineAnimate()` add the component when it
is missing. Spline queries are world space: distances and radii are local values times the
GameObject's largest axis scale. Knot indices are 0-based; returned lists are Lua arrays.

```lua
local road = self.scene:findEntity("Road")
local s = GameObject(self.scene, road):getSpline()
local f = s:evaluateAtDistance(12.5)          -- SplineFrame: position, forward, right, up, radius, roll
local hit = s:getClosestPoint(player.position) -- SplineHit: t, distance, position, segment
s:addKnot(Vector3(10, 0, 4)); s:setKnotPosition(1, Vector3(4, 0, 2)); s:removeKnot(3)
local a = cart:getSplineAnimate(); a.speed = 6; a.loop = SplineLoop.PingPong; a:play()
a:setOnComplete(function() Log.print("arrived") end)   -- Once only
```

| Member | Notes |
|---|---|
| Spline `type` (`SplineType`), `closed`, `is3D` (false: up axis Z, the XY plane), `enabled` | rw |
| `getKnotCount()`, `getKnotPositions()`, `getKnotPosition(i)` / `setKnotPosition(i, v)` | world; edits regenerate the consumers within the frame budget |
| `addKnot(v)`, `insertKnot(i, v)`, `removeKnot(i)` (two stay), `clearKnots()` | |
| `getKnotRadius/setKnotRadius(i, r)`, `getKnotRoll/setKnotRoll(i, deg)`, `getKnotTangentIn/Out(i)`, `setKnotTangents(i, in, out)`, `getKnotMode/setKnotMode(i, SplineTangentMode)` | handles local |
| `getLength()`, `evaluate(t)` (t 0-1), `evaluateAtDistance(d)`, `getPosition(t)`, `tToDistance(t)`, `distanceToT(d)` | |
| `getClosestPoint(v)`, `getClosestPointXZ(x, z)`, `getSignedDistance(v)` (to the tube: < 0 inside), `getSignedDistanceXZ(x, z)` (a closed spline's inside is < 0), `getSignedDistanceBandXZ(x, z)` (a ring, never a disc) | |
| `sampleUniform(n)` (frames), `resample(spacing)`, `simplify(tolerance)` (knots removed) | |
| SplineExtrude `shape` (`SplineExtrudeShape`), `widthSource`, `width`, `edgeDrop`, `edgeInset`, `crownRise`, `shoulderWidth`, `shoulderDrop`, `thickness`, `height`, `grade` (`SplineGrade`), `stepHeight`, `tubeSides`, `maxHalfWidth`, `waterEdgeDrop`, `flow`, `flowSpeed`, `tilesPerMetreU/V`, `levelCourseV`, `corners` (`SplineCornerStyle`), `endCaps`, `endTaper`, `lateralOffset`, `verticalOffset`, `conform` (`SplineConform`), `slopeBlend`, `stationSpacing`, `collision` (`SplineCollision`) | rw; `vertexCount`, `chunkCount`, `pending` ro; `rebuild()` |
| SplineInstantiate `mode` (`SplineInstantiateMode`), `spacing`, `fit` (`SplineSpacingFit`), `postPitch`, `spanMaxStretch`, `fenceGrade`, `crestPitch`, `lateralOffset`, `verticalOffset`, `spacingJitter`, `yawJitter`, `lateralJitter`, `scaleRange` (Vector2), `dropout`, `seed`, `endTrim`, `plantMode` (`SplinePlantMode`), `maxTilt`, `conform`, `slopeBlend`, `stationSpacing` | rw; `addItem(path, weight)`, `clearItems()`, `itemCount`, `instanceCount`, `pending`, `rebuild()` |
| SplineAnimate `spline` (the GameObject with the Spline), `method` (`SplineAnimateMethod`), `duration`, `speed`, `loop` (`SplineLoop`), `easing` (`SplineEasing`), `alignment` (`SplineAlignment`), `startOffset` (0-1), `lateralOffset`, `verticalOffset`, `playOnAwake` | rw |
| `play()`, `pause()`, `restart()`, `playing`, `completed`, `normalizedTime`, `distance` (rw: moves the follower) | constant world speed along the curve; a 2D spline turns the follower about Z |
| `onComplete` (`FunctionSubscribe`), `setOnComplete(fn)` | Once: called at the end |

## 5. Enums (exact Lua spelling; values are integers)

| Namespace | Values |
|---|---|
| `BodyType` | `STATIC`, `KINEMATIC`, `DYNAMIC` |
| `VideoPlayerSource` | `CLIP`, `URL` |
| `VideoTimeUpdateMode` | `GAME_TIME`, `UNSCALED_GAME_TIME` |
| `VideoRenderMode` | `CAMERA_FAR_PLANE`, `CAMERA_NEAR_PLANE`, `RENDER_TEXTURE`, `MATERIAL_OVERRIDE`, `API_ONLY` |
| `VideoAspectRatio` | `NO_SCALING`, `FIT_VERTICALLY`, `FIT_HORIZONTALLY`, `FIT_INSIDE`, `FIT_OUTSIDE`, `STRETCH` |
| `VideoAudioOutputMode` | `NONE`, `AUDIO_SOURCE`, `DIRECT` |
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
| `JointMotion` | `LOCKED`, `LIMITED`, `FREE` |
| `SwingType` | `CONE`, `PYRAMID` |
| `RigidbodyInterpolation` | `SCENE`, `NONE`, `INTERPOLATE`, `EXTRAPOLATE` |
| `CameraType` | `CAMERA_UI`, `CAMERA_ORTHO`, `CAMERA_PERSPECTIVE` |
| `LightType` | `DIRECTIONAL`, `POINT`, `SPOT` |
| `JobLane` | `High`, `Normal`, `Background` |
| `EaseType` | `LINEAR`, `STEP`, `QUAD_IN/OUT/IN_OUT`, `CUBIC_*`, `QUART_*`, `QUINT_*`, `SINE_*`, `EXPO_*`, `CIRC_*`, `ELASTIC_*`, `BACK_*`, `BOUNCE_*` (each `_IN`, `_OUT`, `_IN_OUT`), `CUSTOM`, `CURVE` (the Tween's `curve`) |
| `WrapMode` | `Clamp` (also `ClampForever`, `Constant`), `Loop`, `PingPong`, `Linear`, `LoopWithOffset` (also `CycleWithOffset`) |
| `TangentMode` | `Free`, `Auto`, `Linear`, `Constant`, `ClampedAuto` |
| `GradientMode` | `Blend`, `Fixed` |
| `MinMaxCurveMode` | `Constant`, `TwoConstants`, `Curve`, `TwoCurves` |
| `MinMaxGradientMode` | `Color`, `Gradient`, `TwoColors`, `TwoGradients` |
| `ActionState` | `Running`, `Paused`, `Stopped` |
| `AudioSourceState` | `Playing`, `Paused`, `Stopped` |
| `AudioSourceAttenuation` | `NO_ATTENUATION`, `INVERSE_DISTANCE`, `LINEAR_DISTANCE`, `EXPONENTIAL_DISTANCE` |
| `AudioLoadType` | `Auto`, `DecompressOnLoad`, `CompressedInMemory`, `Streaming` |
| `AnchorPreset` | `NONE`, `TOP_LEFT`, `TOP_RIGHT`, `BOTTOM_LEFT`, `BOTTOM_RIGHT`, `CENTER_LEFT`, `CENTER_TOP`, `CENTER_RIGHT`, `CENTER_BOTTOM`, `CENTER`, `LEFT_WIDE`, `TOP_WIDE`, `RIGHT_WIDE`, `BOTTOM_WIDE`, `VERTICAL_CENTER_WIDE`, `HORIZONTAL_CENTER_WIDE`, `FULL_LAYOUT` |
| `PivotPreset` | `CENTER`, `TOP_CENTER`, `BOTTOM_CENTER`, `LEFT_CENTER`, `RIGHT_CENTER`, `TOP_LEFT`, `BOTTOM_LEFT`, `TOP_RIGHT`, `BOTTOM_RIGHT` |
| `LayoutGroupType` | `VERTICAL`, `HORIZONTAL`, `VERTICAL_WRAP`, `HORIZONTAL_WRAP` |
| `CanvasGroupRenderMode` | `PER_ELEMENT`, `FLATTEN` |
| `AlphaTweenTarget` | `AUTO`, `COLOR`, `CANVAS_GROUP` |
| `ScrollbarType`, `ProgressbarType` | `VERTICAL`, `HORIZONTAL` |
| `OnScreenStickMode` | `FIXED`, `FLOATING`, `DYNAMIC` |
| `OnScreenStickAxes` | `BOTH`, `HORIZONTAL`, `VERTICAL` |
| `OnScreenVisibility` | `ALWAYS`, `TOUCH_DEVICES_ONLY` |
| `ParticleEmitterShape` | `Box`, `Sphere`, `Hemisphere`, `Circle`, `Cone` |
| `FogType` | `LINEAR`, `EXPONENTIAL`, `EXPONENTIALSQUARED`, `HEIGHT` |
| `FogColorMode` | `COLOR`, `SKY` |
| `FogVolumeShape` | `BOX`, `SPHERE` |
| `FogVolumeFalloff` | `LINEAR`, `EXPONENTIAL` |
| `CloudsPreset` | `CUSTOM`, `SPARSE`, `CLOUDY`, `OVERCAST`, `STORMY` |
| `CloudsRendering` | `AUTO`, `SKY_MAP`, `SCREEN` |
| `AtmosphereQuality` | `OFF`, `LOW`, `MEDIUM`, `HIGH` |
| `AtmosphereQualityOverride` | `PROJECT`, `LOW`, `MEDIUM` |
| `RotationOrder` | `XYZ`, `XZY`, `YXZ`, `YZX`, `ZXY`, `ZYX` |
| `RayFilter` | `BODY_2D`, `BODY_3D`, `BOUNDS` |
| `Scaling` | `FITWIDTH`, `FITHEIGHT`, `LETTERBOX`, `CROP`, `STRETCH`, `NATIVE` |
| `Platform` | `MacOS`, `iOS`, `Web`, `Android`, `Linux`, `Windows` |
| `CursorType` | `ARROW`, `IBEAM`, `CROSSHAIR`, `POINTING_HAND`, `RESIZE_EW`, `RESIZE_NS`, `RESIZE_NWSE`, `RESIZE_NESW`, `RESIZE_ALL`, `NOT_ALLOWED`, `WAIT`, `PROGRESS`, `HELP`, `GRAB`, `GRABBING` |
| `MouseMode` | `NORMAL`, `HIDDEN`, `CAPTURED`, `CONFINED` |
| `UIEventState` | `NOT_SET`, `ENABLED`, `DISABLED` |
| `TouchPhase` | `Began`, `Moved`, `Stationary`, `Ended`, `Canceled` |
| `LogLevel` | `INFO`, `VERBOSE`, `DEBUG`, `WARN`, `ERROR` |
| `UIDocumentRenderMode` | `SCREEN`, `TEXTURE` |
| `TextureFilter` | `NEAREST`, `LINEAR`, `NEAREST_MIPMAP_NEAREST`, `NEAREST_MIPMAP_LINEAR`, `LINEAR_MIPMAP_NEAREST`, `LINEAR_MIPMAP_LINEAR` |
| `TextureWrap` | `REPEAT`, `MIRRORED_REPEAT`, `CLAMP_TO_EDGE`, `CLAMP_TO_BORDER` |
| `PrimitiveType` | `TRIANGLES`, `TRIANGLE_STRIP`, `POINTS`, `LINES` |
| `MaterialAlphaMode` | `AUTO`, `ALPHA_OPAQUE`, `MASK`, `BLEND` |
| `MaterialLighting` | `AUTO`, `PIXEL`, `VERTEX`, `MIXED`, `NONE` |
| `LightShading` | `BOTH`, `PIXEL`, `VERTEX` |
| `LightState` | `OFF`, `ON`, `AUTO` |
| `ShadowQuality` | `NONE`, `LOW`, `MEDIUM`, `HIGH` |
| `LitQuality` | `STANDARD`, `HIGH` (`scene.litQuality`) |
| `LightUnit` | `ENGINE`, `LUX`, `CANDELA`, `LUMEN` (`light.intensityUnit`) |
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
| `TransparencyMode` | `SORTED`, `ORDER_INDEPENDENT` |
| `ShadingModel` | `STANDARD`, `HAIR` |
| `CapsuleCastShadows` | `ON`, `CAPSULES_ONLY` |
| `CapsuleDirection` | `X_AXIS`, `Y_AXIS`, `Z_AXIS` |
| `LightMode` | `AUTOMATIC`, `PER_OBJECT`, `TILED` |
| `ReflectionProbeMode` | `STATIC`, `DYNAMIC` |
| `ReflectionProbeUpdateMode` | `ON_LOAD`, `ON_MOVE`, `INTERVAL`, `MANUAL` |
| `FluidOutput` | `SCREEN`, `MATERIAL`, `TEXTURE` (`FluidSimulation.output`) |
| `FrustumPlane` | `FRUSTUM_PLANE_NEAR`, `_FAR`, `_LEFT`, `_RIGHT`, `_TOP`, `_BOTTOM` |
| `GraphicBackend` | `GLCORE`, `GLES3`, `D3D11`, `METAL`, `WGPU` |
| `AdMobRating` | `General`, `ParentalGuidance`, `Teen`, `MatureAudience` |
| `SystemLanguage` | Unity's names and values: `Afrikaans` (0), `Arabic`, `Basque`, `Belarusian`, `Bulgarian`, `Catalan`, `Chinese`, `Czech`, `Danish`, `Dutch`, `English` (10), `Estonian`, `Faroese`, `Finnish`, `French`, `German`, `Greek`, `Hebrew`, `Hungarian`, `Icelandic`, `Indonesian`, `Italian`, `Japanese`, `Korean`, `Latvian`, `Lithuanian`, `Norwegian`, `Polish`, `Portuguese`, `Romanian`, `Russian`, `SerboCroatian`, `Slovak`, `Slovenian`, `Spanish` (34), `Swedish`, `Thai`, `Turkish`, `Ukrainian`, `Vietnamese`, `ChineseSimplified` (40), `ChineseTraditional`, `Unknown` (42), `Hindi` |
| `ScreenOrientation` | `Portrait` (1), `PortraitUpsideDown` (2), `LandscapeLeft` (3), `LandscapeRight` (4), `AutoRotation` (5) |
| `DeviceOrientation` | `Unknown` (0), `Portrait`, `PortraitUpsideDown`, `LandscapeLeft`, `LandscapeRight`, `FaceUp`, `FaceDown` |
| `DeviceType` | `Unknown`, `Handheld`, `Console`, `Desktop` |
| `NetworkReachability` | `NotReachable`, `ReachableViaCarrierDataNetwork`, `ReachableViaLocalAreaNetwork` |
| `HapticFeedback` | `Light`, `Medium`, `Heavy`, `Selection`, `Success`, `Warning`, `Error` |
| `SleepTimeout` | `NeverSleep` (-1), `SystemSetting` (-2) |
| `ResourceLoadState` | `NotStarted`, `Loading`, `Finished`, `Failed` |
| `LatticeInterpolation` | `SHARP`, `SMOOTH`, `CUBIC` |
| `LatticeOutside` | `EXTEND`, `FADE` |
| `LatticeJiggleAnchor` | `NONE`, `NEGATIVE_X`, `POSITIVE_X`, `NEGATIVE_Y`, `POSITIVE_Y`, `NEGATIVE_Z`, `POSITIVE_Z` |
| `LatticeStage` | `BEFORE_SKINNING`, `AFTER_SKINNING` |
| `LatticeMaskChannel` | `NONE`, `RED`, `GREEN`, `BLUE`, `ALPHA`, `INVERTED_RED`, `INVERTED_GREEN`, `INVERTED_BLUE`, `INVERTED_ALPHA` |
| `SplineType` | `Linear`, `CatmullRom`, `CubicBezier` |
| `SplineTangentMode` | `Mirrored`, `Aligned`, `Broken` |
| `SplineExtrudeShape` | `Flat`, `Bevel`, `Crown`, `Rectangle`, `Tube`, `Custom`, `Fill`, `WaterBanks` |
| `SplineWidthSource` | `Radius`, `Constant` |
| `SplineCornerStyle` | `None`, `Mitre`, `Round` |
| `SplineGrade` | `Racked`, `Stepped` |
| `SplineConform` | `None`, `TerrainHeight`, `TerrainHeightAndSlope` |
| `SplineCollision` | `None`, `Mesh`, `Chain2D` |
| `SplineInstantiateMode` | `Spacing`, `Tile`, `Fence` |
| `SplineSpacingFit` | `FitToLength`, `FixedPitch` |
| `SplinePlantMode` | `BoundsMin`, `Pivot` |
| `SplineAnimateMethod` | `Duration`, `Speed` |
| `SplineLoop` | `Once`, `Loop`, `PingPong` |
| `SplineEasing` | `None`, `EaseIn`, `EaseOut`, `EaseInOut` |
| `SplineAlignment` | `None`, `SplineTangent`, `SplineTangentAndUp`, `World` |
| `LineRendererMode` | `Segments`, `Polyline` (`LineRenderer.mode`) |
| `LineWidthUnit` | `World`, `Pixels` |
| `LineAlignment` | `View`, `TransformZ` |
| `LineTextureMode` | `Stretch`, `Tile`, `DistributePerSegment`, `RepeatPerSegment` |

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
   `onFixedUpdate`; the fixed step is `Engine.updateTime`, `Time.fixedDeltaTime`, and
   `Time.deltaTime` is the fixed step there). Impulses are one-off and can be
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
16. **Per-object contact callbacks** (`onCollisionEnter`, `onTriggerEnter2D`...) need no flag and
    come only to the GameObjects involved; the global events (`beginContact2D`,
    `onContactAdded3D`...) fire for every pair of the scene (compare `bodyA.entity` /
    `bodyB.entity` with `self.entity`) and 2D ones need the collider's event flags. A filter that
    was a Lua `shouldCollide3D` is `Physics.ignoreCollision(a, b)` (or layers) now.
17. **Unassigned references are `nil`**: an entity-reference or Prefab property not set in
    the Inspector is `nil`; guard (`if self.target then ... end`).
18. **Scripts by class name, not `getComponent`**: `go:getScript("Enemy")` (same GameObject:
    `self:getScript(...)`), `getScriptInChildren/InParent`, or across scenes
    `GameObject.findObjectOfType("Enemy")`. A property typed with the script's class name
    (`type = "Enemy"`) assigned in the Inspector is the cheapest; `GameObject.instantiate(scriptSelf, ...)`
    also returns the copy's script.
19. **Canvas size in `init`**: `Engine.canvasWidth/Height` can be 0 in exported builds during
    `init`; use `Engine.preferredCanvasWidth/Height` there.
20. **`Log.*` join their arguments with tabs** like `print` (`Log.warn("hp", hp)`); use
    `string.format` for other layouts.
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
29. **Singletons: module tables are per instance.** A Unity `static Instance` has no direct
    equivalent (each attached script runs its file again): use `GameObject.persistSingleton(self)`
    for a persistent manager, or keep shared state in a `require`d module table.
30. **After `DontDestroyOnLoad`**, wrappers of the object kept outside the script's own fields
    name the old scene once it moved: use `self.gameObject` or wrap again (2.8.1).
31. **Event handlers cannot wait.** `onUpdate`, input events, `init`, `onEnable`, `onDisable`
    and `onDestroy` are called from C: `Coroutine.wait` there raises "not inside an engine
    coroutine" (`start` may wait: it runs as a coroutine). Start a coroutine
    (`self:startCoroutine(function() ... end)`). Waits inside a `table.sort` comparator, a
    metamethod or another C callback raise too ("waits cannot cross a C function").
32. **Realtime stops when paused.** `Coroutine.waitRealtime` and unscaled timers run at
    `timeScale = 0` but not while the app is in the background or the editor's Play is paused
    (unlike Unity's wall clock).
33. **Timers outlive deactivation; coroutines do not.** `self:invoke` keeps firing while the
    GameObject is inactive (Unity); coroutines stop when it is deactivated and do not come back
    on reactivation: start them again (in `onEnable`, or where you reactivate).
34. **Pass `owner = self` to `Timer.*`**: a timer without an owner outlives its script (until
    Play stops) and calls into a script that is gone. With `owner = self` it dies with the
    script, like the script's event subscriptions.
35. **An omitted boolean is false** unless the reference gives a default (`[, wake]` defaults to true,
    `[, newRun]` too): `go:setActive()` deactivates, `rb2d:applyForceToCenter(f)` wakes the body.
36. **Inspector fields come from the saved file.** A script whose saved file no longer has a
    `properties` table has no fields; while you edit it in the code editor, a missing table keeps
    them until you save (cutting and pasting the table loses no value).

## 7. Hot reload and editor scripts

Saving a script while the game plays reloads it in place (Preferences > Scripting > Script
Changes While Playing, MCP `set_script_settings changes_while_playing`). Every live instance keeps
its table and fields. Its functions become the new ones, and events registered by name are
registered again. `init` does not run again, and anonymous callbacks and timers created by the
old code are dropped. A file that does not compile or fails at load keeps the old code running
(`reload_scripts` `kept_old` with `{path, line}`). Define `onReload` to redo what `init`
derived:

```lua
local Spinner = { properties = { { name = "speed", type = "float", default = 90 } } }

function Spinner:init()
    self.angle = 0
    RegisterEngineEvent(self, "onUpdate")
end

function Spinner:onUpdate()
    self.angle = self.angle + self.speed * Engine.deltatime
end

function Spinner:onReload(info)
    Log.print("reloaded " .. info.path)
end

return Spinner
```

Properties: a property that keeps its type keeps its runtime value. int <-> float, bool,
string and vector types convert. A property whose type changed in another way, and a new
property, gets its default. A removed property is cleared. Modules from `require` are patched in
place, so their local variables start over: keep state in fields.

Editor scripts (`---@menu` / `---@toolbar` / `---@validate` annotations in a `.lua` under a
folder named `Editor`) run in an editor-only state with the math classes and the `Editor`
table (`Editor.call(tool, args)` -> `{ok, text, data}`, `Editor.selection()`,
`Editor.select(ids)`, `Editor.log(...)`). They are never part of the game, so `require`
refuses them and exports leave them out. MCP: `list_editor_menus`, `run_editor_menu` (one undo
step per run). User docs: `docs/scripting.md`.

The math classes, checked at runtime by the doc test (a ```` ```lua run ```` block runs in a Play
fixture through `eval_lua`):

```lua run
local a = Vector3(1, 2, 3)
local b = a * 2 + Vector3.UNIT_X
assert(b.x == 3 and b.y == 4 and b.z == 6)
assert(math.abs(Vector3(3, 4, 0):length() - 5) < 1e-6)
local q = Quaternion(0, 0, 45)
assert(math.abs(q:getRoll() - 45) < 1e-3)
```
