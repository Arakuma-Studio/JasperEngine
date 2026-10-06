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

The editor reads `properties` by executing the file in the editor's Lua state, so keep the
top level free of side effects (only define the table and functions).

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
| `"entity"`, `"entitypointer"`, `"pointer"` | the first enabled Lua script instance on the referenced GameObject if it has one, otherwise an `EntityHandle`; `nil` if unassigned | none |
| any other string = a wrapper class name (see list below) | an instance of that wrapper class for the referenced entity; `nil` if unassigned | none |
| any other string = a Lua script class name (e.g. `"Enemy"`) | that GameObject's `Enemy` script instance table (its `self`) | none |

Wrapper class names accepted as `type` (case-insensitive): `GameObject`, `Camera`, `Light`,
`Light2D`, `ShadowCaster2D`, `Mesh`, `Primitive`, `Mirror`, `Terrain`, `Tilemap`, `Model`,
`MeshPolygon`, `Points`, `LineRenderer`, `Sprite`, `AudioSource`, `RectTransform`,
`LayoutGroup`, `Polygon`, `Text`, `Image`, `Button`, `Panel`, `Scrollbar`, `Progressbar`,
`InputField`, `Bone`, `Fog`, `Skybox`, `ReflectionProbe`, `Rigidbody2D`, `Joint2D`,
`Rigidbody`, `Joint3D`, `Action`, `Tween`, `AlphaTween`, `ColorTween`, `PositionTween`,
`RotationTween`, `ScaleTween`, `Animation`, `ParticleSystem`, `SpriteAnimation`,
`MorphTracks`, `RotateTracks`, `ScaleTracks`, `TranslateTracks`, `EntityHandle`.
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
| `interpolationAlpha` | ro | fixed-step interpolation factor |
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
| `Input.getTouches()` | list of `Touch` (Touch class is NOT bound; prefer `numTouches`/`getTouchPosition` or touch events) |
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
`getDirSeparator()`, `getAssetPath()`, `getUserDataPath()`, `getLuaPath()`,
`getShaderPath()`; ads/web: `initializeAdMob()`, `setMaxAdContentRating(AdMobRating.*)`,
`loadInterstitialAd(id)`, `isInterstitialAdLoaded()`, `showInterstitialAd()`,
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
| `ambientLight2DColor`, `ambientLight2DIntensity`, `globalIlluminationColor`, `globalIlluminationIntensity`, `shadowQuality`, `lightState`, `shadow2DQuality`, `enableUIEvents` | rw properties (partial list; also ssao*/ssr*/fixedResolution*/postProcess* rendering options) |
| `getPhysicsSystem()`, `getAudioSystem()`, `getActionSystem()`, `getMeshSystem()`, `getRenderSystem()`, `getUISystem()` | subsystem objects |

`PhysicsSystem` (from `self.scene:getPhysicsSystem()`): `gravity` / `gravity2D` /
`gravity3D` (rw), `pointsToMeterScale2D` (rw), event objects (section 1.3),
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

---

## 4. Object wrappers

Class hierarchy (methods/properties are inherited):

```
EntityHandle
 |- GameObject
 |   |- Camera, Light, Light2D, ShadowCaster2D, ReflectionProbe, Bone, Points, LineRenderer
 |   |- Mesh
 |   |   |- Primitive -> Mirror
 |   |   |- Sprite, Tilemap, Model, MeshPolygon, Terrain
 |   |- RectTransform
 |       |- Text, Polygon, LayoutGroup
 |       |- Image -> Button, Panel, Scrollbar, Progressbar, InputField
 |- AudioSource, Rigidbody2D, Rigidbody, Fog, Skybox
 |- Action -> Tween (AlphaTween, ColorTween, PositionTween, RotationTween, ScaleTween),
             ParticleSystem, SpriteAnimation, Animation, MorphTracks, RotateTracks,
             ScaleTracks, TranslateTracks
Joint2D, Joint3D (standalone classes)
```

Constructors: every wrapper has `(Scene, Entity)` to wrap an existing entity. Most also have
`(Scene)` which creates a new entity with that component. Exceptions: `Rigidbody2D`,
`Rigidbody` and `Bone` have only `(Scene, Entity)`; `AudioSource` also has `(Scene, is3D)`.

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
| `visible` | rw bool | also `setVisibleOnly(b)` |
| `addChild(goOrEntity)`, `removeChild(goOrEntity)`, `removeParent()` | methods | |
| `moveToTop()`, `moveUp()`, `moveDown()`, `moveToBottom()` | methods | sibling order |
| `billboard`, `fakeBillboard`, `cylindricalBillboard` (rw), `billboardRotation` (rw), `setBillboard(a,b,c)`, `setBillboardRotation(x,y,z)` | | |
| `localMatrix` (rw), `modelMatrix` (ro), `normalMatrix` (ro), `updateTransform()` | | |
| `getRigidbody2D()` | method | returns `Rigidbody2D`, CREATING the component if missing |
| `removeRigidbody2D()` | method | |
| `getRigidbody()` / `removeRigidbody()` | method | 3D, same semantics |
| `getRay(direction)` | method | Ray from world position |
| `GameObject.instantiate`, `GameObject.destroy`, `GameObject.destroyImmediate` | static | section 2.6 |

There is no `getComponent`: wrap the same entity with the class you need
(`Sprite(self.scene, self.entity)`).

### 4.3 Mesh (base of Primitive, Sprite, Model, Tilemap, ...) - partial

| Member | Kind |
|---|---|
| `color` | rw `Vector4` (sRGB) |
| `setColor(r, g, b)`, `setColor(r, g, b, a)`, `setColor(Vector4)` | method |
| `alpha` | rw |
| `enabled` | rw |
| `setTexture(path)` | method |
| `material` | rw `Material`; `getMaterial([submesh])`, `setMaterial([submesh,] mat)` |
| `castShadows`, `receiveShadows`, `receiveLights`, `transparent`, `faceCulling`, `cullingMode`, `customShader` | rw |
| `getAABB()`, `getWorldAABB()`, `getNumSubmeshes()` | methods |
| `setShaderUniform(name, float/Vector2/3/4)`, `getShaderUniform(name)`, `removeShaderUniform(name)` | methods |
| instancing: `createInstancedMesh()`, `addInstance(...)`, `updateInstance(i, ...)`, `removeInstance(i)`, `getNumInstances()`, `clearInstances()` | methods |

`Material` (rw): `baseColorFactor`, `metallicFactor`, `roughnessFactor`, `alphaMode`,
`alphaCutoff`, `emissiveFactor`, `baseColorTexture`, `emissiveTexture`,
`metallicRoughnessTexture`, `occlusionTexture`, `normalTexture`.

### 4.4 Primitive (Mesh)

`createPlane(w, d [, tiles])`, `createWall(w, h [, tiles])`, `createBox(w, h, d [, tiles])`,
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
`findAnimation(name)`, `playAnimation(indexOrName [, fadeTime])`, `stopAnimations()`,
`getBone(indexOrName)`, `getMorphWeight(indexOrName)`, `setMorphWeight(indexOrName, w)`,
`resetToBindPose()`, `enabled` (rw).

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

### 4.10 Light (GameObject) / Light2D (GameObject)

Light rw: `type` (`LightType.*`), `enabled`, `direction` (Vector3), `color` (Vector3),
`range`, `intensity`, `innerConeAngle`, `outerConeAngle`, `shadows`, `cameraNear`,
`cameraFar`, `automaticShadowCamera`, `numCascades`, `cullingMask`; methods
`setDirection(x,y,z)`, `setColor(r,g,b)`, `setConeAngle(a)`, `setRange(r)`,
`setIntensity(i)`, `setShadows(b)`, `setShadowCameraNearFar(n, f)`.

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

Action (EntityHandle): `start()`, `pause()`, `stop()`, `isRunning()`, `isPaused()`,
`isStopped()`, `getTimeCount()`, `getActionComponent()`; rw `target` (Entity),
`ownedTarget`, `speed`, `weight`; `setTarget(gameObjectOrEntity)`.

Tween (Action): rw `duration`, `loop`; `setFunctionType(EaseType.*)`, `getTime()`,
`getValue()`, `getTweenComponent()`.

| Tween | `setAction(...)` (pass every argument; C++ defaults do not apply) |
|---|---|
| `PositionTween` | `(startV3, endV3, duration, loop)` |
| `RotationTween` | `(startQuat, endQuat, duration, loop)` |
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
`ScaleTracks`, `MorphTracks` with `setTimes`, `setValues`, `setEasings`, `setEasing`.

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
(Component getter is misspelled in the binding: `getParticlesomponent()`.)

### 4.17 Other wrappers (partial)

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
- `Fog`: rw `type` (`FogType.*`), `color`, `density`, `linearStart`, `linearEnd`.
- `Skybox`: `setTextures(...)`, `setTexture(...)`, rw `color`, `alpha`, `rotation`.
- `Terrain`, `ReflectionProbe`, `Mirror`, `Bone`: editor-configured; see bindings.

---

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
| `CursorType` | `ARROW`, `IBEAM`, `CROSSHAIR`, `POINTING_HAND`, `RESIZE_EW`, `RESIZE_NS`, `RESIZE_NWSE`, `RESIZE_NESW`, `RESIZE_ALL`, `NOT_ALLOWED` |
| `MouseMode` | `NORMAL`, `HIDDEN`, `CAPTURED`, `CONFINED` |
| `UIEventState` | `NOT_SET`, `ENABLED`, `DISABLED` |
| `TextureFilter` | `NEAREST`, `LINEAR`, `NEAREST_MIPMAP_NEAREST`, `NEAREST_MIPMAP_LINEAR`, `LINEAR_MIPMAP_NEAREST`, `LINEAR_MIPMAP_LINEAR` |
| `TextureWrap` | `REPEAT`, `MIRRORED_REPEAT`, `CLAMP_TO_EDGE`, `CLAMP_TO_BORDER` |
| `PrimitiveType` | `TRIANGLES`, `TRIANGLE_STRIP`, `POINTS`, `LINES` |
| `MaterialAlphaMode` | `AUTO`, `ALPHA_OPAQUE`, `MASK`, `BLEND` |
| `LightState` | `OFF`, `ON`, `AUTO` |
| `ShadowQuality` | `NONE`, `LOW`, `MEDIUM`, `HIGH` |
| `ShadowCaster2DShape` | `AUTO_QUAD`, `POLYGON` |
| `ReflectionProbeMode` | `STATIC`, `DYNAMIC` |
| `ReflectionProbeUpdateMode` | `ON_LOAD`, `ON_MOVE`, `INTERVAL`, `MANUAL` |
| `FrustumPlane` | `FRUSTUM_PLANE_NEAR`, `_FAR`, `_LEFT`, `_RIGHT`, `_TOP`, `_BOTTOM` |
| `GraphicBackend` | `GLCORE`, `GLES3`, `D3D11`, `METAL`, `WGPU` |
| `AdMobRating` | `General`, `ParentalGuidance`, `Teen`, `MatureAudience` |
| `ResourceLoadState` | `NotStarted`, `Loading`, `Finished`, `Failed` |

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
21. **`onDestroy` is only for runtime destruction**; to clean up when play stops nothing is
    called. Event subscriptions are cleaned up automatically.
22. **Colors** on wrappers (`setColor`, `color`, Button colors) are sRGB 0..1 values;
    raw component fields (`UIComponent.color`, `ButtonComponent.colorNormal`) are linear.
23. **Integers**: entity ids, indices and enum values are integers; do not pass fractional
    numbers to them (`math.floor` first).
