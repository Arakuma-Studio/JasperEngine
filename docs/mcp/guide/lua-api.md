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

| Step | What happens |
|---|---|
| Instance creation | The file's chunk is executed once **per instance** (compiled once, run per attached script). The returned module table becomes the `__index` prototype of a fresh instance table. So file-level `local` variables are per instance; `require`d modules are shared. |
| Fields set | `self.__name` (class name), `self.scene` (Scene userdata), `self.entity` (integer Entity id), then every property `self.<name>` = Inspector value. |
| References | After all instances of the scene exist, entity-reference properties are resolved (second pass). So in `init()` all references are valid, but the referenced script's own `init()` may not have run yet. |
| `init(self)` | Called once (third pass) for every enabled Lua script. Register events here. |
| Events | Methods registered with `RegisterEngineEvent` / `RegisterEvent` are called by the engine. |
| `onDestroy(self)` | Called (if defined) when the GameObject is destroyed at runtime (`GameObject.destroy` / `destroyImmediate`), before the instance is released. It is NOT called when the scene/play mode simply stops. Not a registered event, just define the method. |
| Release | All subscriptions made through `RegisterEngineEvent`/`RegisterEvent` for this instance are removed automatically. |

`init` and `onDestroy` are the only methods the engine calls by name. Everything else
(`onUpdate`, `onFixedUpdate`, ...) is called **only if registered**.

Engine events registered with `RegisterEngineEvent` are skipped while the script's
GameObject is inactive or the script entry is disabled. `RegisterEvent` (component/physics
events) is not guarded.

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
| `"entity"`, `"entitypointer"`, `"pointer"` | the first enabled Lua script instance on the referenced GameObject if it has one, otherwise an `EntityHandle`; `nil` if unassigned | none |
| any other string = a wrapper class name (see list below) | an instance of that wrapper class for the referenced entity; `nil` if unassigned | none |
| any other string = a Lua script class name (e.g. `"Enemy"`) | that GameObject's `Enemy` script instance table (its `self`) | none |

Wrapper class names accepted as `type` (case-insensitive): `GameObject`, `Camera`, `Light`,
`Light2D`, `ShadowCaster2D`, `Mesh`, `Primitive`, `Mirror`, `Terrain`, `Tilemap`, `Model`,
`MeshPolygon`, `Points`, `LineRenderer`, `Sprite`, `AudioSource`, `RectTransform`,
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
| `RegisterEvent(self, eventObject, "methodName" [, tag])` | Subscribes `self:methodName(...)` to any event object (component or physics event). |

Re-registering the same method (same tag) replaces the previous subscription. There is no
Lua-side unregister; subscriptions end when the instance is released.

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

`self.entity` is a plain integer. Wrap it with a bound constructor `(Scene, Entity)`:

```lua
local go     = GameObject(self.scene, self.entity)
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

Edge detection (pressed this frame): `Input.isKeyPressed` returns the held state, so either
keep the previous value in `self`, or register `onKeyDown(key, isRepeat, mods)`:

```lua
function Mover:init() RegisterEngineEvent(self, "onKeyDown") end
function Mover:onKeyDown(key, isRepeat, mods)
    if key == Input.KEY_SPACE and not isRepeat then Log.print("jump") end
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
| `Input.anyKeyDown()` | bool, a key, mouse or gamepad button pressed this frame |
| `Input.getMouseDelta()` | `Vector2`, canvas pixels moved this frame |
| `Input.getMouseScrollDelta()` | `Vector2`, wheel offsets of this frame |
| `Input.getTouchCount()`, `Input.getTouch(i)` | this frame's touches (0-based), also those lifted in the frame: `{pointer, position, phase, delta, startPosition, time, began}` |
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

### 2.3 Log (static, one string argument each)

`Log.print(s)` (= `Log.info(s)`), `Log.verbose(s)` (trace), `Log.debug(s)` (debug builds
only), `Log.warn(s)`, `Log.error(s)`.
Pass ONE string: `Log.print("score " .. tostring(n))` or `Log.print(string.format("%d", n))`.
Plain Lua `print(...)` goes to the engine log too, as an info line (the editor's Console, in
Play and at edit time; a game's platform log in an exported build): it takes any number of
values of any type and joins them with tabs, like standard Lua. `Log.*` add the levels.

Lua's records are in the `lua` category and carry the chunk and line they were logged from
(the Console's details pane, `read_console`'s `source`). More than 200 identical messages a
second are held back and summed up in one `suppressed N similar messages` line.

- `Log.category(name)`: a category of your own, created on first use, with
  `:trace(s) :debug(s) :info(s) :warn(s) :error(s)` (`local net = Log.category("net")`,
  `net:warn("lag")`). The Console filters by category.
- `Log.setLevel(level [, category])`: the lowest level logged (`"trace"`, `"debug"`, `"info"`,
  `"warn"`, `"error"`, `"fatal"`, `"off"`) for `lua` or the named category; false for an
  unknown level name. `Log.getLevel([category])` reads it back.

### 2.4 UserSettings (static, persistent key/value)

Getters (with optional default as 2nd arg): `UserSettings.getBoolForKey(k [, def])`,
`getIntegerForKey`, `getLongForKey`, `getFloatForKey`, `getDoubleForKey`,
`getStringForKey`, `getDataForKey`.
Setters: `UserSettings.setBoolForKey(k, v)`, `setIntegerForKey`, `setLongForKey`,
`setFloatForKey`, `setDoubleForKey`, `setStringForKey`, `setDataForKey`.
`UserSettings.removeKey(k)`.

### 2.5 System (static)

`System.getScreenWidth()`, `getScreenHeight()`, `isTouchDevice()`, `isFullscreen()`,
`requestFullscreen()`, `exitFullscreen()`, `isWindowMaximized()`, `maximizeWindow()`,
`restoreWindow()`, `setWindowSize(w, h)`, `isWindowResizable()`, `setWindowResizable(b)`,
`setWindowTitle(s)`, `quit()`, `showVirtualKeyboard(text)`, `hideVirtualKeyboard()`,
`setClipboardText(s)`, `getClipboardText()` (the system clipboard, UTF-8; on Web the text of
the last copy or paste), `getDirSeparator()`, `getAssetPath()`, `getUserDataPath()`, `getLuaPath()`,
`getShaderPath()`; ads/web: `initializeAdMob()`, `setMaxAdContentRating(AdMobRating.*)`,
`loadInterstitialAd(id)`, `isInterstitialAdLoaded()`, `showInterstitialAd()` (Android/iOS; the
export compiles Google Mobile Ads in only with the project settings `android.admob_enabled` /
`ios.admob_enabled` and their `admob_app_id`, otherwise these do nothing),
`initializeCrazyGamesSDK()`, `showCrazyGamesAd(type)`, `happytimeCrazyGames()`,
`gameplayStartCrazyGames()`, `gameplayStopCrazyGames()`, `loadingStartCrazyGames()`,
`loadingStopCrazyGames()`.

Files: `File()` / `File(path, write)` with `:open`, `:flush`, `:close`, `:readString()`,
`:writeString(s)`, `:read8/16/32()`, `:eof()`, `:length()`, `:seek(n)`, `:pos()`;
`Data(path)` reads asset data; `FileData.getBaseDir(p)`, `FileData.getFilePathExtension(p)`,
`FileData.getSystemPath(p)`. Constants `FileErrors.FILEDATA_OK`, `INVALID_PARAMETER`,
`FILE_NOT_FOUND`, `OUT_OF_MEMORY`.

`require("folder.module")` loads `folder/module.lua` from the Lua directory (or its `lua/`
subfolder). Use it for shared helper modules.

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
| `ambientLight2DColor`, `ambientLight2DIntensity`, `globalIlluminationColor`, `globalIlluminationIntensity`, `maxLightsPerObject` (1-16; the other lights reaching an object go into its ambient), `litQuality` (`LitQuality.STANDARD` / `HIGH`: the built-in Lit's specular anti-aliasing and direct-light energy compensation; new scenes are High, scenes saved before it Standard), `physicalLightEV100` (the exposure, -6..24, lights in lux, candela or lumen are drawn at: Sunny 15, Overcast 12, Interior 7, Night 2), `shadowQuality` (`ShadowQuality` AUTO/NONE (hard only)/LOW/MEDIUM/HIGH/ULTRA, the shadow quality tier), `shadowDistance` (metres, 0: the tier's), `shadowFadeFraction`, `shadowCascadeBlend`, `effectiveShadowTier` (read-only `ShadowTier` LOW/MEDIUM/HIGH/ULTRA; `jasper://docs/shadows`), `lightState`, `shadow2DQuality`, `sceneColorScale` (`SceneColorScale`: resolution of the scene-colour copy that shaders requiring scenecolor sample; `HALF` default), `postProcessing` (a `PostProcessingSettings` copy: edit its fields, then assign it back, `local p = scene.postProcessing; p.tonemap = TonemapMode.ACES; p.fxaaEnabled = true; scene.postProcessing = p`; enums `TonemapMode` NONE/ACES/NEUTRAL/REINHARD/AGX, `MonochromeMode` GRAYSCALE/SEPIA; fields as `get_scene_settings` `post_processing.<field>` in camelCase; `scene.postProcessing = PostProcessingSettings()` turns everything off; costs and presets: `jasper://docs/post-processing`), `postProcessPasses` (a list of `PostProcessPass`: `shader`, `enabled`, `scale` 0.1-1, `setUniform`/`getUniform`, `setTexture(uniform, path)`/`getTexture`; assign the list back; `jasper://docs/post-processing`), `enableUIEvents` | rw properties (partial list; also ssao*/ssr*/fixedResolution*/postProcess* rendering options) |
| `getPhysicsSystem()`, `getAudioSystem()`, `getActionSystem()`, `getMeshSystem()`, `getRenderSystem()`, `getUISystem()` | subsystem objects |
| `findEntitiesWith({"Transform", "MeshComponent"}[, {"UIComponent"}])` | -> table of Entity with every listed component and none of the excluded ones, in the order of the first one's array (parents before children for `Transform`); names are the component class names |
| `getChangeTick()` | -> integer: the scene's change tick; a change made after the call is stamped with a newer one |
| `findChangedEntities("Transform", sinceTick)` | -> table of Entity whose component changed after `sinceTick` (added, set through a wrapper setter, a component field, the editor or `markChanged`) |
| `markChanged(e, "MeshComponent")` | -> bool: stamps the component changed after a write the engine cannot see; false when `e` has none |
| `getSignature(e)` | -> 256-character string of `0`/`1`, MSB first (bit 0 is the last character); `EntityManager:setSignature` also takes the 64-character strings of older scripts |

`PhysicsSystem` (from `self.scene:getPhysicsSystem()`): `gravity` / `gravity2D` /
`gravity3D` (rw), `interpolation` (rw, as `scene.physicsInterpolation`), `pointsToMeterScale2D` (rw), event objects (section 1.3),
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

### 2.11 Angle (static)

`Angle.degToRad(d)`, `Angle.radToDeg(r)`, `Angle.defaultToRad(a)`, `Angle.defaultToDeg(a)`,
`Angle.radToDefault(r)`, `Angle.degToDefault(d)` ("default" = degrees unless
`Engine.useDegrees = false`). Lua's `math.sin/cos` take radians.

### 2.12 Color (static)

`Color.linearTosRGB(...)`, `Color.sRGBToLinear(...)`: accept `Vector3`, `Vector4`, `(r,g,b)`
or `(r,g,b,a)`.

### 2.13 Timer (static): delayed calls, repeats, coroutines

```lua
local h = Timer.after(2.0, function() self:explode() end, { owner = self })   -- once, in 2 s of game time
Timer.every(0.5, function() self:blink() end, { count = 10, owner = self })    -- 10 times
Timer.nextFrame(fn --[[, opts]]);  Timer.afterFrames(3, fn --[[, opts]]);  Timer.afterFixedSteps(1, fn --[[, opts]])
local co = Timer.run(function()              -- a coroutine the scheduler drives
    Timer.wait(1.0)                          -- game seconds (or the run's clock)
    Timer.waitRealtime(0.5)                  -- wall-clock seconds, even while paused
    Timer.waitFrames(2)                      -- simulated frames
    Timer.waitFixedStep()                    -- one fixed update
    Timer.waitUntil(function() return self.ready end)   -- checked once per frame
end, { owner = self })
h:cancel(); h:isActive(); co:cancel(); co:isActive(); Timer.cancelAll(self)
```

- Options: `owner` (cancelled when that script goes away), `clock` (`"game"`: scaled by
  `Engine.timeScale`, the default; `"unscaled"`; `"real"`), `count` (every), `catchUp` (every:
  after a stall, up to 8 missed periods at once; by default a repeat fires at most once a frame
  and keeps its phase).
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

---

## 4. Object wrappers

Class hierarchy (methods/properties are inherited):

```
EntityHandle
 |- GameObject
 |   |- Camera, Light, Light2D, ShadowCaster2D, ReflectionProbe, LocalVolumetricFog, Bone, Points, LineRenderer, Lattice,
 |   |  FluidSimulation, ParticleSystem
 |   |- Mesh
 |   |   |- Primitive -> Mirror
 |   |   |- Sprite, Tilemap, Model, MeshPolygon, Terrain
 |   |- RectTransform
 |       |- Text, Polygon, LayoutGroup
 |       |- Image -> Button, Panel, Scrollbar, Progressbar, InputField
 |- AudioSource, PoseAnimator, AnimationPlayer, VertexAnimator, Rigidbody2D, Rigidbody, Fog, VolumetricClouds, Skybox
 |- Action -> Tween (AlphaTween, ColorTween, PositionTween, RotationTween, ScaleTween),
             SpriteAnimation, Animation,
             KeyframeTracks (MorphTracks, LatticeTracks, RotateTracks, ScaleTracks, TranslateTracks)
Joint2D, Joint3D, AnimationState, AnimationEvent, VertexAnimatorSocketPose (standalone classes)
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
| `material` | rw `Material`; `getMaterial([submesh])`, `setMaterial([submesh,] mat)` |
| `castShadows`, `receiveShadows`, `staticShadowCaster` (cached in the static shadow layer, `jasper://docs/shadows`), `receiveLights`, `transparent`, `faceCulling`, `cullingMode`, `customShader` | rw |
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
`"GROUP_VALUE"`), `isShaderFeatureEnabled(define)`. A material with `shader = ""` feeds the
shader its submesh draws with (the mesh's `customShader`, else the scene default): a copy from
`getMaterial` / `material` resolves defaults and features against that one. UI (Image, Text,
Polygon), Points, LineRenderer and Skybox objects have the same parameter/texture/feature
methods for their `customShader` (but `setShaderCubeTexture`: set a cube uniform of theirs from
the editor or MCP), and (but Skybox) `blendMode` and `depthWrite`.
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
`specularTexture`. E.g. `local m = mesh:getMaterial(); m.clearcoatEnabled = true;
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
### 4.8.4 Animator (EntityHandle)

Unity's Animator: plays an Animator Controller (`.controller`: parameters, layers, state
machines, blend trees, transitions) on the clips of the Animation Player's kinds (`.anim`, model
clips), with Avatar Masks, montages (`.montage`), an IK stack and root motion. `Animator(scene,
entity)` adds the component when missing; `GameObject:getAnimator()` returns `nil` without one;
`GameObject:hasAnimator()`. It runs in play mode and in games. Calls before the controller loads
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
| `getCurrentState(layer)`, `getNextState(layer)` | an `AnimatorStateInfo`: `valid`, `name`, `fullPath`, `tag`, `normalizedTime` (laps + fraction), `length` (s), `loop` |
| `isInTransition(layer)` | |
| `getLayerCount()`, `getLayerName(i)`, `getLayerIndex(name)`, `setLayerWeight(i, w)`, `getLayerWeight(i)` | the first layer always weighs 1 |
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
`requestShadowUpdate()` redraws an On Demand light's maps.

Light2D rw: `enabled`, `color` (Vector3), `intensity`, `range`, `falloff`, `height`,
`shadows`, `shadowBias`, `shadowSoftness`, `mapResolution`; methods `setColor(r,g,b)`,
`setIntensity(i)`, `setRange(r)`, `setFalloff(f)`, `setHeight(h)`, `setShadows(b)`.

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
| `linearDamping`, `angularDamping`, `gravityScale` | rw | |
| `type` | rw | `BodyType.STATIC`, `BodyType.KINEMATIC`, `BodyType.DYNAMIC` |
| `fixedRotation`, `bullet`, `awake`, `enableSleep`, `enabled` | rw bool | |
| `pointsToMeterScale` | ro | default 64 |
| `getMass()` | method | kg (NOT a property) |
| `getRotationalInertia()`, `applyMassFromColliders()` | methods | |
| `applyForce(forceV2, pointV2, wake)` | method | point in world points |
| `applyForceToCenter(forceV2, wake)` | method | |
| `applyTorque(t, wake)` | method | |
| `applyLinearImpulse(impulseV2, pointV2, wake)` | method | |
| `applyLinearImpulseToCenter(impulseV2, wake)` | method | |
| `applyAngularImpulse(i, wake)` | method | |
| `createBoxCollider(w, h)` | method | box from local (0,0) to (w,h); returns collider index |
| `createCenteredBoxCollider(w, h [, centerV2, angle])` | method | centered box |
| `createRoundedBoxCollider(w, h, radius)`, `createCircleCollider(centerV2, radius)`, `createCapsuleCollider(c1, c2, radius)`, `createSegmentCollider(p1, p2)`, `createPolygonCollider({Vector2...})`, `createChainCollider({Vector2...}, loop)` | methods | |
| `removeAllColliders()`, `getNumColliders()`, `getColliderType(i)` | methods | |
| `load()` | method | rebuilds the body after changing colliders from script |
| `shapeDensity`, `shapeFriction`, `shapeRestitution`, `shapeSensor`, `shapeContactEvents`, `shapeSensorEvents`, `shapeEnableHitEvents`, `shapePreSolveEvents` | rw | first collider; per collider: `setColliderX(index, v)` / `getColliderX(index)` (`Density`, `Friction`, `Restitution`), `setColliderSensor(i, b)`, `isColliderSensor(i)` ... |
| `categoryBitsFilter`, `maskBitsFilter`, `groupIndexFilter` | rw | also `setBitsFilter(category, mask)` |
| `getBodyContacts()`, `getColliderContacts(i)` | methods | tables of `Contact2D` |
| `getAttachedObject()` | method | GameObject |

`Contact2D`: `getBodyA()`, `getBodyB()` (Rigidbody2D), `getBodyEntityA()`,
`getBodyEntityB()`, `getColliderIndexA()`, `getColliderIndexB()`, `getManifold()`.
`Manifold2D`: `getNormal()`, `getPointCount()`, `getManifoldPointPosition(i)`, ... .
`Joint2D(scene [, entity])`: `setDistanceJoint(bodyA, bodyB [, anchorA, anchorB, ...])`,
`setRevoluteJoint`, `setPrismaticJoint`, `setMouseJoint`, `setWheelJoint`, `setWeldJoint`,
`setMotorJoint`, `getType()` (partial; arguments follow the C++ headers).

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

### 4.16 ParticleSystem

A GameObject with a Particle System (`ParticleSystem(scene)` makes one; a script property of
type ParticleSystem, or `object(ref, 'ParticleSystem')` in eval_lua). Methods: `play()`,
`pause()`, `stop([clear])` (true also removes the particles), `restart()`,
`emit(count [, {position, velocity, size, color, lifetime}])` (born at the system's next tick), `seek(t)` (restart and simulate to
t seconds), `step(ticks)`, `sendTrigger(name)` (the effect's Trigger sub-emitters named so),
`setFloat(name, v)` / `setVector3(name, v)` / `setColor(name, v)` and `getFloat` / `getVector3` /
`getColor` for the effect's exposed parameters (a set returns false for a name it does not expose),
`setInlineEffect(json)` / `getInlineEffect()`, `addCollisionPlane(gameObject)` /
`clearCollisionPlanes()`. rw: `enabled`, `playOnAwake`, `playbackSpeed`, `seed` (0: a new seed
at every play), `effect` (a .particles path; "" plays the inline effect). ro: `isPlaying`,
`isPaused`, `isEmitting`, `particleCount`, `time`.

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
`emitterShape` (`ParticleEmitterShape.*`), `running`; `start()`, `reset()`, `setTarget(go)`,
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
deprecated). More: jasper://docs/particles.

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
- `LineRenderer`: `addLine(aV3, bV3 [, colorV4 [, colorB]])`, `updateLine(i, ...)`,
  `removeLine(i)`, `clearLines()`, `getNumLines()`, rw `maxLines`, `enabled`.
- `Polygon` (UI): `addVertex(x, y)` / `(Vector3)`, rw `color`, `alpha`, `flipY`;
  `setColor(r,g,b[,a])`, `setTexture(path)`.
- `MeshPolygon`: `addVertex(...)`, ro `width`, `height`, rw `flipY`.
- `LayoutGroup` (RectTransform): rw `type` (`LayoutGroupType.*`), `useAllWrapSpace`,
  `wrapCellWidth`, `wrapCellHeight`; ro `contentWidth`, `contentHeight`; `resize()`.
- `ShadowCaster2D`: rw `shape` (`ShadowCaster2DShape.AUTO_QUAD` / `POLYGON`), `closed`,
  `enabled`; `addVertex(x, y)`, `clearVertices()`, `getVertexCount()`.
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
| `Collider2DType` | `POLYGON`, `CIRCLE`, `CAPSULE`, `SEGMENT`, `CHAIN` |
| `Collider3DType` | `SPHERE`, `BOX`, `CAPSULE`, `TAPERED_CAPSULE`, `CYLINDER`, `CONVEX_HULL`, `MESH`, `HEIGHTFIELD` |
| `RigidbodyMotionQuality` | `DISCRETE`, `LINEAR_CAST` |
| `Joint2DType` | `DISTANCE`, `REVOLUTE`, `PRISMATIC`, `MOUSE`, `WHEEL`, `WELD`, `MOTOR` |
| `Joint3DType` | `FIXED`, `DISTANCE`, `POINT`, `HINGE`, `CONE`, `PRISMATIC`, `SWINGTWIST`, `SIXDOF`, `PATH`, `GEAR`, `RACKANDPINON`, `PULLEY` |
| `CameraType` | `CAMERA_UI`, `CAMERA_ORTHO`, `CAMERA_PERSPECTIVE` |
| `LightType` | `DIRECTIONAL`, `POINT`, `SPOT` |
| `JobLane` | `High`, `Normal`, `Background` |
| `EaseType` | `LINEAR`, `STEP`, `QUAD_IN/OUT/IN_OUT`, `CUBIC_*`, `QUART_*`, `QUINT_*`, `SINE_*`, `EXPO_*`, `CIRC_*`, `ELASTIC_*`, `BACK_*`, `BOUNCE_*` (each `_IN`, `_OUT`, `_IN_OUT`), `CUSTOM` |
| `ActionState` | `Running`, `Paused`, `Stopped` |
| `AudioSourceState` | `Playing`, `Paused`, `Stopped` |
| `AudioSourceAttenuation` | `NO_ATTENUATION`, `INVERSE_DISTANCE`, `LINEAR_DISTANCE`, `EXPONENTIAL_DISTANCE` |
| `AnchorPreset` | `NONE`, `TOP_LEFT`, `TOP_RIGHT`, `BOTTOM_LEFT`, `BOTTOM_RIGHT`, `CENTER_LEFT`, `CENTER_TOP`, `CENTER_RIGHT`, `CENTER_BOTTOM`, `CENTER`, `LEFT_WIDE`, `TOP_WIDE`, `RIGHT_WIDE`, `BOTTOM_WIDE`, `VERTICAL_CENTER_WIDE`, `HORIZONTAL_CENTER_WIDE`, `FULL_LAYOUT` |
| `PivotPreset` | `CENTER`, `TOP_CENTER`, `BOTTOM_CENTER`, `LEFT_CENTER`, `RIGHT_CENTER`, `TOP_LEFT`, `BOTTOM_LEFT`, `TOP_RIGHT`, `BOTTOM_RIGHT` |
| `LayoutGroupType` | `VERTICAL`, `HORIZONTAL`, `VERTICAL_WRAP`, `HORIZONTAL_WRAP` |
| `ScrollbarType`, `ProgressbarType` | `VERTICAL`, `HORIZONTAL` |
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
15. **Held vs pressed**: `Input.isKeyPressed` / `isMousePressed` report the held state.
    For "pressed this frame" track the previous value or use `onKeyDown` / `onMouseDown`.
16. **Physics events are global** for the scene: compare `bodyA.entity` / `bodyB.entity`
    with `self.entity`. 2D contact events require the per-collider event flags.
17. **Unassigned references are `nil`**: an entity-reference or Prefab property not set in
    the Inspector is `nil`; guard (`if self.target then ... end`).
18. **No `getComponent`, no script lookup by entity**: to talk to another script, expose a
    property typed with that script's class name (`type = "Enemy"`) and assign it in the
    Inspector; `GameObject.instantiate(scriptSelf, ...)` also returns the copy's script.
19. **Canvas size in `init`**: `Engine.canvasWidth/Height` can be 0 in exported builds during
    `init`; use `Engine.preferredCanvasWidth/Height` there.
20. **`Log.*` take a single string**; build messages with `..`/`tostring`/`string.format`.
21. **Animation Player callbacks take one value**: `player.onEvent = function(event) ... end`
    (`event.functionName`, `event.args[1]`), not `function(name, args)`; `onFinish` gets the
    state name. `GameObject:getAnimationPlayer()` is `nil` without the component (unlike
    `getRigidbody2D`, it never adds one).
22. **`onDestroy` is only for runtime destruction**; to clean up when play stops nothing is
    called. Event subscriptions (`RegisterEvent`/`RegisterEngineEvent`, with or without a tag)
    are removed automatically when the script goes away.
23. **Colors** on wrappers (`setColor`, `color`, Button colors) are sRGB 0..1 values;
    raw component fields (`UIComponent.color`, `ButtonComponent.colorNormal`) are linear.
24. **Integers**: entity ids, indices and enum values are integers; do not pass fractional
    numbers to them (`math.floor` first).
24. **Pass `owner = self` to `Timer.*`**: a timer without an owner outlives its script (until
    Play stops) and calls into a script that is gone. With `owner = self` it dies with the
    script, like the script's event subscriptions.

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
