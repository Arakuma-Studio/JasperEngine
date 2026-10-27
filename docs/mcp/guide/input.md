# Input

Served as `jasper://docs/input`. How a game reads input (`Input` in Lua and C++), how frames
see presses, and how an agent drives and checks it with `send_input` and `get_input_state`
(group `gameplay`: enable it with `tool_groups`).

## Levels and frame edges

| Lua | Meaning |
|---|---|
| `Input.getKey(key)` (`isKeyPressed`) | held now |
| `Input.getKeyDown(key)` / `Input.getKeyUp(key)` | pressed / released in this frame's window |
| `Input.getMouseButton(b)`, `getMouseButtonDown(b)`, `getMouseButtonUp(b)` | the same for mouse buttons (`Input.MOUSE_BUTTON_LEFT`...) |
| `Input.getGamepadButtonDown(id, b)`, `getGamepadButtonUp(id, b)` | per pad (`isGamepadButtonPressed` is the level) |
| `Input.anyKeyDown()` | a key, mouse button or gamepad button was pressed this frame |
| `Input.getMouseDelta()` | canvas pixels the pointer moved this frame (raw motion while the pointer is locked) |
| `Input.getMouseScrollDelta()` | the wheel offsets of this frame |
| `Input.getTouchCount()`, `Input.getTouch(i)` (0-based), `getTouchDown(pointer)`, `getTouchUp(pointer)` | this frame's touches with phases |
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

`getTouch(i)` returns `{pointer, position, phase, delta, startPosition, time, began}`; `phase`
is `Input.TOUCH_BEGAN`, `TOUCH_MOVED`, `TOUCH_STATIONARY`, `TOUCH_ENDED` or `TOUCH_CANCELED`.
The frame list also holds the fingers lifted or cancelled in the window: a finger that touched
and lifted in one frame appears once, with phase `TOUCH_ENDED` and `began = true`.
`Input.getTouches()` still lists only the fingers down now.

## Pointer lock

`Engine.mouseMode = MouseMode.CAPTURED` locks the pointer for mouse-look. Desktop players lock
at once (`POINTER_LOCKED`). A browser locks only after a click or key press: the state is
`POINTER_PENDING` until then (and again after Esc, until the next click). In the editor the
pointer is locked while the Game view has the focus. Losing the window focus releases every
held key and button (their `getKeyUp` fires) and makes a lock `POINTER_PENDING`.

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
  normalized) and `axis1d` (negative, positive).
- **Priority and consumption.** Maps are read highest `priority` first; a control a `consume` map
  uses is unavailable to lower maps that frame, an enabled `exclusive` map blocks every lower map
  (modal menus). In one priority level a binding that needs modifiers and has them (`Ctrl+S`)
  suppresses the plain binding of the same key; `forbiddenMods` refuses one. With
  `ignoreWhenUIConsumes` (default) a press an HTML document or the component UI took does not
  count. `event:consume()` in a callback keeps the action's controls from lower maps.
- **Several players.** `InputActions.setGamepad("P2", 1)` (or `map:setGamepad(1)`): -1 any pad.
- **Edges.** `wasPressedThisFrame`/`wasReleasedThisFrame` follow Input's windows: a tap in one
  frame is pressed and released (callbacks: started, performed, canceled), one fixed step sees it.
- **On-screen controls.** `InputActions.bindVirtualStick(element, "move")` and
  `bindVirtualButton(element, "jump")` make HTML UI elements feed `virtual/move` and
  `virtual/jump`; `InputActions.setVirtual(name, value)` does it from a script.
- **Rebinding.** `InputActions.startRebind("Gameplay/Jump", bindingId, {groups = {"Gamepad"},
  exclude = {"mouse/delta*"}, cancel = "keyboard/escape", timeout = 5}, function(r) ... end)`
  takes the next key, button or axis past 0.5 (maps are suspended meanwhile) as an override;
  `getConflicts`, `saveOverrides` (UserSettings, per asset), `resetOverrides`.
  `binding:getDisplayName()` gives "Space", "Ctrl+S", "Left Stick", "Cross" on a PlayStation pad.

### Agents

- `get_input_actions {path}` reads an asset (ids, paths, display names); `edit_input_actions {path,
  ops}` applies ops as one undo step (`add_map`, `add_action`, `add_binding`, `add_composite`,
  `set`, `remove`, `rename`, `move`, `duplicate`; keys snake_case), live during Play. The asset's
  own JSON keys are camelCase (`pressPoint`, `ignoreWhenUIConsumes`).
- Set the asset Play loads with `set_project_settings {"values": {"input.actions": "assets/..."}}`.
- `get_input_state {fields: ["actions"]}`: each map's priority, enabled and blocked state and each
  action's value, phase, edges and consumer.
- `send_input {type: "input_action", name: "Gameplay/Jump", action: "press", hold_frames: 0}`
  forces an action at top priority (press, down, up, value with `value: [x, y]`): the way to test a
  script's reaction to an action without knowing its bindings.
