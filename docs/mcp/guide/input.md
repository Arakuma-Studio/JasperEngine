# Input

Served as `jasper://docs/input`. How a game reads input (`Input` in Lua and C++), how frames
see presses, Input Actions (action maps in an `.inputactions` asset) with Unity's Input Manager
on top (`Input.getAxis("Horizontal")`, `getButtonDown("Jump")`), on-screen controls, and how an
agent drives and checks it with `send_input`, `get_input_state` (group `gameplay`: enable it with
`tool_groups`) and `get_play_state`. Everything works the same in editor Play and every export.

## Levels and frame edges

| Lua | Meaning |
|---|---|
| `Input.getKey(key)` (`isKeyPressed`) | held now. `key`: an `Input.KEY_*` code or a control name (`"space"`, `"a"`, `"left_shift"`, `"mouse_left"`, `"gamepad_a"`, Unity's `"left ctrl"`, `"joystick button 0"`) |
| `Input.getKeyDown(key)` / `Input.getKeyUp(key)` | pressed / released in this frame's window |
| `Input.getMouseButton(b)`, `getMouseButtonDown(b)`, `getMouseButtonUp(b)` | the same for mouse buttons (`Input.MOUSE_BUTTON_LEFT`...) |
| `Input.getGamepadButtonDown(id, b)`, `getGamepadButtonUp(id, b)` | per pad, `id` -1 any; `b` a code or a name (`"gamepad_a"`, `"a"`, `"dpad_up"`) (`isGamepadButtonPressed` is the level) |
| `Input.anyKeyDown` (also `Input.anyKeyDown()`), `Input.anyKey` | a key, mouse button or gamepad button pressed this frame / held now |
| `Input.inputString` | UTF-8 text typed this frame (backspace `"\b"`, return `"\n"`) |
| `Engine.onCharInput(char)` | one call per typed character, a one-character UTF-8 string (`char32_t` in C++): a character beyond U+FFFF (an emoji) is one call on every platform, although Windows and Android deliver it as two UTF-16 halves |
| `Input.getPressedControl()` | the first control pressed this frame by name (`"space"`, `"mouse_left"`, `"gamepad_a"`, `"left_x+"`, `"right_trigger"`), nil when none: "press a key" screens |
| `Input.getMouseDelta()` (`Input.mouseDelta`) | canvas pixels the pointer moved this frame (raw motion while the pointer is locked; moves outside the canvas count) |
| `Input.getMouseScrollDelta()` (`Input.mouseScrollDelta`) | the wheel offsets of this frame (y positive up) |
| `Input.getTouchCount()` (`Input.touchCount`), `Input.getTouch(i)` (0-based, nil out of range), `Input.touches`, `Input.getTouchById(fingerId)`, `getTouchDown(pointer)`, `getTouchUp(pointer)` | this frame's touches with phases |
| `Input.getPointerLockState()` | `Input.POINTER_UNLOCKED`, `POINTER_PENDING`, `POINTER_LOCKED` |

The events that arrive between two frames form one *window*. A frame that runs its update
starts by making the pending window its own, so `getKeyDown` is true for the whole frame
(every script, `onUpdate` and `onPostUpdate`), and false again in the next frame. A press and
its release inside one window are **both** seen: `getKeyDown` and `getKeyUp` are true and
`getKey` is false. A tap is never lost, however short. Key repeat is never a press.

A paused game (or one held by `auto_pause`) latches nothing: the events wait for the next
frame that runs, so `play step` sees what `send_input` sent while paused.

**Fixed steps.** Inside `onFixedUpdate` the edges are those of the fixed window: each one is
seen by exactly one fixed step, the first one at or after its frame. A frame with 0 steps
keeps it for the next frame's first step; a frame with 3 steps shows it only in the first.
Levels are the same in both. Deltas (`getMouseDelta`, scroll) are the frame's in every step:
read deltas in `onUpdate`.

```lua
function onUpdate()
    if Input.getKeyDown(Input.KEY_SPACE) then jump() end
    local look = Input.getMouseDelta()
    yaw = yaw - look.x * 0.1
end
```

`Input.getMouseScroll()` is deprecated: it keeps the last wheel event forever. Use
`getMouseScrollDelta()`.

## Touches

`getTouch(i)` returns `{pointer, position, phase, delta, startPosition, time, began}` and Unity's
names of a `Touch`: `fingerId` (= `pointer`), `deltaPosition` (= `delta`), `deltaTime` (unscaled
seconds since it last moved) and `tapCount`. `phase` is `Input.TOUCH_BEGAN` (`TouchPhase.Began`),
`TOUCH_MOVED` (`Moved`), `TOUCH_STATIONARY` (`Stationary`), `TOUCH_ENDED` (`Ended`) or
`TOUCH_CANCELED` (`Canceled`). The frame list holds the fingers down, then the ones lifted or
cancelled in the window: a finger that touched and lifted in one frame appears once, with phase
`TOUCH_ENDED` and `began = true`. `Input.getTouches()` still lists only the fingers down now.

```lua
for _, touch in ipairs(Input.touches) do
    if touch.phase == TouchPhase.Began and touch.tapCount == 2 then Log.print("double tap") end
    if touch.phase == TouchPhase.Moved then pan(touch.deltaPosition) end
end
local held = fingerId and Input.getTouchById(fingerId)   -- never index Input.touches by a finger id
```

- `tapCount`: a touch that begins within 0.3 s of the last touch's end and within 24 canvas px of
  its start counts on from it (the last one shorter than 0.3 s).
- `fingerId`s are opaque small integers, stable while the finger is down (Android pointer ids from
  0, iOS slots from 1, browser identifiers).
- `Input.multiTouchEnabled = false` reports only the first finger (these touches, the bindings,
  the simulated mouse); the fingers-down list and the UI still get every finger.
  `Input.touchSupported` is `System.isTouchDevice()`.

## Pointer lock

`Engine.mouseMode = MouseMode.CAPTURED` locks the pointer for mouse-look. Desktop players lock
at once (`POINTER_LOCKED`). A browser locks only after a click or key press: the state is
`POINTER_PENDING` until then (and again after Esc, until the next click). In the editor the
pointer is locked while the Game view has the focus. Esc in the focused Game view during Play is
the game's key (`Input.getKeyDown(Input.KEY_ESCAPE)`, a pause menu) and the view keeps the focus, as
in Unity. Losing the window focus (or an app pause)
releases every held key, button and gamepad button (their `getKeyUp` fires), cancels the fingers
and makes a lock `POINTER_PENDING`.

## Keys on the web

Browsers report every key the desktop players do: the Command and Windows keys are
`LEFT_SUPER` / `RIGHT_SUPER` (`MetaLeft`, `MetaRight`, and the old `OSLeft` / `OSRight`), the
context-menu key is `MENU`, the ISO `<>` key `WORLD_2`, F13-F24 their keys. An event without a
`KeyboardEvent.code` (Android soft keyboards, old browsers) is mapped from its `keyCode`, the right
Shift, Ctrl, Alt and Meta from its `location`. A hidden or minimized desktop window releases the
gamepads too (platform-services.md, Hidden windows).

## Driving and checking input (MCP)

- `send_input` `{type:"key", key:"space", hold_frames: 0}` presses and releases in the same
  frame: the way to test that a script reacts to a tap. The default `hold_frames` (2) holds it.
  `hold_frames: 0` also works for mouse clicks and gamepad presses.
- A mouse-look move: `{type:"mouse", action:"move", dx: 40, dy: 0}` (no x, y) moves the pointer
  40 canvas pixels from where it is; with `CAPTURED`, `Input.getMouseDelta().x` sums to 40.
- `get_input_state {fields?: ["raw", "touches", "gamepads"]}` returns what the game read at the
  last frame that ran: held keys and buttons, this frame's `down`/`up` edges, the mouse delta and
  scroll delta, the lock state, the touches with phases, the pads. To see the edges of an
  input, freeze the game between calls (`play {action: "set", auto_pause: true}`) and send it
  with `frames_after: 0`: the last frame that ran is then the one the input landed in (with
  the default `frames_after: 1` one more frame runs, and its window is empty).
- To count edges from a script, keep counters in a Lua script and read them with `eval_lua`.
- `send_input {type:"text", text:"a😀b"}` delivers the string at once (a paste or an IME commit);
  with `per_unit: true` each UTF-16 unit is its own character event, as Windows (`WM_CHAR`) and
  Android's keys deliver typing: the way to check on Linux that a field, `onCharInput` or
  `Input.inputString` gets an emoji whole.

## Input Actions

An `.inputactions` asset (JSON, `"version": 1`, edited in Window > Input Actions or with
`edit_input_actions`) holds **action maps** of **actions** whose values come from **bindings** to
controls. Project Settings > Input names the asset Play and the built game load
(`InputActions.loadDefault`), and the default deadzone; a script can `InputActions.load(path)`
another (a path in the assets directory).

```lua
local move = InputActions.get("Gameplay/Move"):readVector2()      -- VECTOR2: length <= 1
if InputActions.get("Gameplay/Jump"):wasPressedThisFrame() then jump() end
InputActions.get("Gameplay/Jump").onPerformed = function(e) vy = 5 end
InputActions.getMap("Menu"):enable()                               -- an exclusive map: Gameplay stops
```

- **Action types.** `BUTTON` (0..1, pressed at `pressPoint`, default 0.5; `holdTime` > 0 performs
  after holding that long), `AXIS` (the bindings' sum, -1..1), `VECTOR2` (the sum, radial deadzone,
  length at most 1). Mouse and touch deltas are neither deadzoned nor clamped (read them in
  `onUpdate`: in `onFixedUpdate` every step sees the frame's delta).
- **Control paths.** `keyboard/<key>` (the send_input names: `space`, `a`, `left_shift`, `f1`...),
  `mouse/left|right|middle|button4..8`, `mouse/delta`, `mouse/delta/x|y`, `mouse/scroll`,
  `mouse/scroll/x|y`, `gamepad/a|b|x|y|left_bumper|start|dpad_up...`, `gamepad/left_x..right_trigger`
  (triggers 0..1; the sticks' y is up-positive), `gamepad/left_stick|right_stick`,
  `touch/primary/press|position|delta`, `touch/count`, `virtual/<name>`.
- **Composites.** `dpad2d` (parts up, down, left, right: W/A/S/D gives (0, 1) for W, diagonals
  normalized) and `axis1d` (negative, positive). A part may be a list (`"positive": [{"path":
  "keyboard/right"}, {"path": "keyboard/d"}]`: any of them; none reads 0).
- **Input Manager smoothing.** A binding's `smoothing {gravity, sensitivity, dead, snap}` (Unity's
  Key or Mouse Button entries): its value moves to the target at `sensitivity`/s, back at
  `gravity`/s (0: at once), reads 0 under `dead`, `snap` jumps through 0. Unscaled time.
- **Priority and consumption.** Maps are read highest `priority` first; a control a `consume` map
  uses is unavailable to lower maps that frame, an enabled `exclusive` map blocks every lower map
  (modal menus). In one priority level a binding that needs modifiers and has them (`Ctrl+S`)
  suppresses the plain binding of the same key; `forbiddenMods` refuses one. With
  `ignoreWhenUIConsumes` (default) a press an HTML document or the component UI took does not
  count. `event:consume()` in a callback keeps the action's controls from lower maps.
- **Several players.** `InputActions.setGamepad("P2", 1)` (or `map:setGamepad(1)`): -1 any pad. A
  binding's own `gamepad` (0-15) reads that pad whatever its map's.
- **Edges.** `wasPressedThisFrame`/`wasReleasedThisFrame` follow Input's windows: a tap in one
  frame is pressed and released (callbacks: started, performed, canceled), one fixed step sees it.
- **Virtual controls.** `InputActions.bindVirtualStick(element, "move")` and
  `bindVirtualButton(element, "jump")` make HTML UI elements feed `virtual/move` and
  `virtual/jump`; `InputActions.setVirtual(name, value)` does it from a script. (The on-screen
  controls below feed actions directly.)
- **Typing and menus win.** In a map with `ignoreWhenUIConsumes`, a focused text field takes the
  keyboard, a document with gamepad focus or a selected component Button the d-pad, left stick and
  A, and a selected Button the arrows, Tab, Enter and Space (a key held when the field took focus is
  released; jasper://docs/ui). `getKey` and `isGamepadButtonPressed` still read the device.
- **Rebinding.** `InputActions.startRebind("Gameplay/Jump", bindingId, {groups = {"Gamepad"},
  exclude = {"mouse/delta*"}, cancel = "keyboard/escape", timeout = 5}, function(r) ... end)`
  takes the next key, button or axis past 0.5 (maps are suspended meanwhile) as an override;
  `getConflicts`, `saveOverrides` (UserSettings, per asset), `resetOverrides`.
  `binding:getDisplayName()` gives "Space", "Ctrl+S", "Left Stick", "Cross" on a PlayStation pad.

## Unity's Input Manager (`Input.getAxis`, `getButton`)

The actions of a map named **Input Manager** are Unity's virtual axes and buttons. A game or Play
without an actions asset has Unity's defaults in that map; Project Settings > Input's **Input
Manager** section (or `add_input_manager`, Window > Input Actions' pad button) puts it in an asset.

| Action | Type | Bindings |
|---|---|---|
| Horizontal | axis | keys left/a (negative), right/d (positive), gravity 3, sensitivity 3, dead 0.001, snap; gamepad left_x (dead 0.19) |
| Vertical | axis | keys down/s, up/w; gamepad left_y (up is positive) |
| Fire1 / Fire2 / Fire3 | button | left_control, mouse_left; gamepad_a / left_alt, mouse_right; gamepad_b / left_shift, mouse_middle; gamepad_x |
| Jump | button | space; gamepad_y |
| Mouse X, Mouse Y | axis | mouse movement × 0.1 (Y up positive) |
| Mouse ScrollWheel | axis | wheel steps × 0.1 |
| Submit / Cancel | button | enter, kp_enter, space; gamepad_a / escape; gamepad_b |

| Lua | Meaning |
|---|---|
| `Input.getAxis(name)` | smoothed: -1..1 (button 0..1); mouse movement adds unclamped |
| `Input.getAxisRaw(name)` | without smoothing (-1, 0, 1 for keys) |
| `Input.getButton(name)`, `getButtonDown(name)`, `getButtonUp(name)` | held (a key of either side of an axis, an axis or source past the press point) / pressed / released this frame |
| `Input.getVector(xName, yName)` | the two axes, length clamped to 1, deltas left out (Godot's `get_vector`) |
| `Input.hasAction(name)`, `getActionNames()`, `setActionEnabled(name, on)`, `isActionEnabled(name)` | a disabled action reads 0 and keeps no edges |
| `RegisterEngineEvent(self, "onButtonDown")` → `self:onButtonDown(action)`, `onButtonUp` | before `onUpdate`, per action whose button went down / up |

- `name` is a flat name (the Input Manager map's action, else the first action of that name in
  priority order) or `"Map/Action"`: `Input.getButtonDown("Gameplay/Jump")` reads an S22 action
  with Unity's semantics. An unknown name logs once and reads 0 (no Lua error).
- Bounded values (keys after smoothing, gamepad axes, on-screen sticks and buttons, simulated
  axes) are summed, then clamped; deltas (mouse movement, touch-area drags) add unclamped:
  keyboard 1 + stick 0.5 = 1; keyboard 1 + stick -1 = 0. A gamepad dead zone is rescaled (0.6 with
  0.19 reads 0.506). Smoothing runs on unscaled time (`Engine.timeScale = 0` menus work).
- Differences from Unity: same-named entries add up (Unity takes the largest); the dead zone is
  rescaled; `getButton` on an axis is true for both sides; a click or key the UI consumed does not
  count; an edge is seen by one fixed step.

### Simulated input and on-screen controls

| Lua | Meaning |
|---|---|
| `Input.simulateButton(name, pressed [, source])` | holds the action (value 1) |
| `Input.simulateAxis(name, value [, source])` | a bounded value -1..1; 0 removes it |
| `Input.addAxisDelta(name, delta [, source])` | a delta for this frame only |
| `Input.clearSimulated([source])` | removes a source (none: every script source) |

`source` is a name (default `"script"`; survives scene loads) or a script instance (`self`: it
goes with the instance). Values count at once in `getAxis`; presses and edges from the next frame.
C++: `InputActions::setSourceValue(owner, action, value, InputSourceKind::Control)`,
`addSourceDelta`, `clearSource(owner)` (any unique address).

On-Screen Stick, On-Screen Button and Touch Area (GameObject > UI, `create_ui`; HTML
`<onscreen-stick>`, `<onscreen-button>`, `<touch-area>`, jasper://docs/html-ui) are sources owned
by their component or element: a stick writes `horizontalAction` / `verticalAction` (default
`Horizontal` / `Vertical`, y up), a button `action` (default `Fire1`) = `value` while a finger holds
it, a touch area `Mouse X` / `Mouse Y` deltas × `sensitivity` (0.1). An action field takes a flat
name or `Map/Action`. Each takes the first pointer that presses it (a stick, a look area and a
button at once); releasing, disabling, destroying, unloading, a cancel, pause or focus loss let go.
A touch area dragged with the mouse adds nothing to an action bound to mouse movement. The Game
view's Simulate Touch makes the mouse finger 0 (Alt on release keeps it down; the next drag is
finger 1).

```lua
function Player:onUpdate()
    local move = Input.getVector("Horizontal", "Vertical")      -- keyboard, pad or stick
    self.yaw = self.yaw + Input.getAxis("Mouse X")              -- mouse or touch area
    if Input.getButtonDown("Fire1") then self:fire() end        -- mouse, pad or the button
end
```

### Rebinding the Input Manager at run time

`Input.getBindings(name)` (the flat shape below), `Input.setBindings(name, bindings)` → `true` or
`false, error`, `Input.resetBindings([name])`, `Input.exportBindings()` (the replacements as JSON
text, for `UserSettings`), `Input.importBindings(json)`. Runtime state: Play stop drops it.
(S22's per-binding rebinding, `InputActions.startRebind`, persists by itself.)

### Agents

- `get_input_actions {path}` reads an asset (ids, paths, display names); `edit_input_actions {path,
  ops}` applies ops as one undo step (`add_map`, `add_input_manager`, `add_action`, `add_binding`,
  `add_composite`, `add_part`, `set`, `remove`, `rename`, `move`, `duplicate`; keys snake_case),
  live during Play. The asset's own JSON keys are camelCase (`pressPoint`, `ignoreWhenUIConsumes`).
- `get_project_settings {"keys": ["input.manager_actions"]}` / `set_project_settings` with it (the
  whole list, or `"defaults"`; one undo step, scope project) edit the Input Manager map in the flat
  shape, making `InputManager.inputactions` when the project has none:

```json
[{"name": "Dash", "type": "button", "bindings": [
    {"kind": "buttons", "positive": ["left_shift", "gamepad_right_bumper"]}]},
 {"name": "Steer", "type": "axis", "bindings": [
    {"kind": "buttons", "negative": ["a"], "positive": ["d"], "gravity": 3, "sensitivity": 3, "dead": 0.001, "snap": true},
    {"kind": "gamepad_axis", "axis": "left_x", "gamepad": "any", "dead": 0.19, "sensitivity": 1}]},
 {"name": "Look X", "bindings": [{"kind": "mouse_delta", "axis": "x", "sensitivity": 0.1}]}]
```

  Kinds `buttons` (`positive`, `negative`, `gamepad`, `gravity`, `sensitivity`, `dead`, `snap`,
  `invert`), `gamepad_axis` (`axis` left_x..right_trigger, `gamepad` any or 0-15, `dead`,
  `sensitivity`, `invert`), `mouse_delta` (`axis` x / y / wheel, `sensitivity`, `invert`), and
  `binding` (`{"binding": {an .inputactions binding}}` for any other); actions `name`, `type`
  axis / button / vector2, `description`, `press_point`, `hold_time`, `deadzone`. Missing numbers
  take Unity's defaults of the kind. Control names: keys `a`..`z`, `0`..`9`, `space`, `enter`,
  `left_shift`, `kp_0`, `f1`...; `mouse_left`, `mouse_right`, `mouse_middle`, `mouse_4`..`mouse_8`;
  `gamepad_a`.. `gamepad_dpad_left`; Unity spellings normalised (`"left ctrl"`, `"mouse 0"`,
  `"joystick button 0"`, `"return"`, `"[1]"`, `"LeftArrow"`).
- A project saved by an older editor with `input: {version: 1, actions: [...]}` loads with those
  actions as the Input Manager map of `InputManager.inputactions` (written in the assets directory;
  project.yaml points to it once saved).
- Set the asset Play loads with `set_project_settings {"values": {"input.actions": "assets/..."}}`.
- `get_input_state {fields: ["actions"]}`: each map's priority, enabled and blocked state and each
  action's value, phase, edges and consumer.
- `send_input {type: "input_action", name: "Gameplay/Jump", action: "press", hold_frames: 0}`
  forces an action at top priority (press, down, up, value with `value: [x, y]`): the way to test a
  script's reaction to an action without knowing its bindings.
- `send_input {type: "action", name: "Jump", phase: "press", hold_frames: 3}` (`phase` press, down,
  up), `{type: "axis", name: "Horizontal", value: 1, hold_frames: 30}` (without hold_frames it
  stays until `value: 0`, `release_all` or Play stop) and `{type: "axis_delta", name: "Mouse X",
  delta: 5}` (one frame) add to an action through the virtual source `"mcp"`, like an on-screen
  control. `get_play_state` shows `input` (the Input Manager's actions not at rest:
  `{"Horizontal": 1.0, "Jump": "pressed"}`, touches, keys held) and `held_input` (`actions`, `axes`).
- On-screen controls with several fingers: `touch` events with different `pointer` ids spread
  over frames (`gap_frames`, `wait` events), then `eval_lua "return Input.getAxis('Horizontal')"`;
  `Input.isPointerOverUI(id)` tells whether a finger is on UI.
