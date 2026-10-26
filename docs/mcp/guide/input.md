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
