# Input: edges, touches and input actions

Scripts read input the Unity way: held state and per-frame edges of keys, mouse and gamepad
buttons, Unity's `Touch` list with phases, and **input actions** (Unity's Input Manager virtual
axes and buttons, configured in Project Settings > Input). The keyboard, gamepad, on-screen
controls, scripts and agents all feed the same actions, so `Input.getAxis("Horizontal")` sums
the arrow keys, the left stick and an on-screen stick. Everything works the same in editor Play
and in every exported build. Lua reference: `jasper://docs/lua-api` §2.2.

## 1. Polling with edges

| Lua | Meaning |
|---|---|
| `Input.getKey(key)`, `getKeyDown(key)`, `getKeyUp(key)` | held / pressed this frame / released this frame. `key` is an `Input.KEY_*` code or a control name (`"space"`, `"a"`, `"left_shift"`, `"mouse_left"`, `"gamepad_a"`, Unity's `"left ctrl"`, `"joystick button 0"`) |
| `Input.getMouseButton(b)`, `getMouseButtonDown(b)`, `getMouseButtonUp(b)` | `b` 0 left, 1 right, 2 middle (`Input.MOUSE_BUTTON_*`) |
| `Input.getGamepadButtonDown(id, button)`, `getGamepadButtonUp(id, button)` | `id` -1 = any pad; `button` an `Input.GAMEPAD_BUTTON_*` or a name (`"gamepad_a"`, `"a"`, `"dpad_up"`) |
| `Input.anyKey`, `Input.anyKeyDown` | any key, mouse button or gamepad button held / pressed this frame |
| `Input.inputString` | UTF-8 text typed this frame (backspace `"\b"`, return `"\n"`) |
| `Input.mouseDelta` | mouse movement this frame, canvas px, y down; works with `Engine.mouseMode = MouseMode.CAPTURED` and outside the canvas |
| `Input.mouseScrollDelta` | wheel steps this frame (y positive up) |
| `Input.getPressedControl()` | the first control pressed this frame by name (`"space"`, `"mouse_left"`, `"gamepad_a"`, `"left_x+"`), nil when none: a "press a key" screen |

Rules:

- **An edge is seen by one frame**: the first frame that runs the update after the press, until
  that frame ends, whatever `Engine.timeScale` is. A press during a frame belongs to the next one.
  Frames that do not run the update (app paused, Play paused, a held simulation) keep it.
- A press and a release inside one frame make `getKeyDown` and `getKeyUp` both true and `getKey`
  false (Unity). Key repeat never makes an edge.
- `onFixedUpdate` runs 0..n times per frame: every fixed step of a frame sees that frame's
  edges, so read edges in `onUpdate`.
- `Input.isKeyPressed`, `isMousePressed`, `numTouches`, `getTouches` and the `onKey*`, `onMouse*`,
  `onTouch*` events are unchanged.
- App pause (a mobile app sent to the background, a web ad) and editor Play stop release
  everything held (the ups are seen by the next frame); in the editor, the Game view losing
  focus releases the keys. A disconnected gamepad releases its buttons.

## 2. Touches

`Input.touchCount`, `Input.getTouch(index)` (0-based, nil out of range), `Input.touches` (a
list) and `Input.getTouchById(fingerId)` (nil when that finger has no touch this frame) give
Unity's `Touch`: `fingerId`, `phase` (`TouchPhase.Began`, `Moved`, `Stationary`, `Ended`,
`Canceled`), `position`, `startPosition`, `deltaPosition` (canvas px, y down), `deltaTime`
(unscaled seconds since its last position change) and `tapCount`.

```lua
function Gestures:onUpdate()
    for _, touch in ipairs(Input.touches) do
        if touch.phase == TouchPhase.Began and touch.tapCount == 2 then
            Log.print("double tap at " .. tostring(touch.position.x))
        elseif touch.phase == TouchPhase.Moved then
            self:pan(touch.deltaPosition)
        end
    end
    -- one finger by id (never index Input.touches by a finger id)
    local held = self.fingerId and Input.getTouchById(self.fingerId)
end
```

- A touch that ended or was cancelled is listed in the frame it ends (its last `position` and
  `deltaPosition`); a touch that begins and ends between two frames is `Began` in one frame and
  `Ended` in the next. A finger id reused in one frame is listed twice, the ended one first.
- `tapCount`: a touch that begins within 0.3 s of the last touch's end and within 24 canvas px of
  its start counts on from it (the last one must have been shorter than 0.3 s).
- `fingerId`s are opaque small integers, stable while the finger is down (Android pointer ids
  from 0, iOS slots from 1, browser touch identifiers).
- `Input.multiTouchEnabled = false` reports only the first finger (in these touches and the
  simulated mouse); the legacy list and the UI still get every finger.
- `Input.touchSupported` is `System.isTouchDevice()`. `Engine.callMouseInTouchEvent` makes a
  finger the mouse (`getMouseButtonDown(0)`, `Fire1`, `Mouse X`); `Engine.callTouchInMouseEvent`
  makes the mouse finger 0. The editor's Simulate Touch mode is the mouse as finger 0.
- The legacy `Input.numTouches()` / `getTouches()` / `getTouchPosition(pointer)` list the fingers
  down now (a lifted finger goes at once). `getTouches()` items are `Touch` values too (`pointer`
  is their old name of `fingerId`).

## 3. Input actions

Project Settings > Input holds an ordered list of actions. A new or old project without an
`input` node in `project.yaml` has Unity's Input Manager defaults:

| Action | Type | Bindings |
|---|---|---|
| Horizontal | axis | keys left/a (negative), right/d (positive), gravity 3, sensitivity 3, dead 0.001, snap; gamepad left_x (dead 0.19) |
| Vertical | axis | keys down/s, up/w; gamepad left_y inverted (up is positive) |
| Fire1 | button | left_control, mouse_left; gamepad_a |
| Fire2 | button | left_alt, mouse_right; gamepad_b |
| Fire3 | button | left_shift, mouse_middle; gamepad_x |
| Jump | button | space; gamepad_y |
| Mouse X, Mouse Y | axis | mouse movement × 0.1 (Y up positive) |
| Mouse ScrollWheel | axis | wheel steps × 0.1 |
| Submit | button | enter, kp_enter, space; gamepad_a |
| Cancel | button | escape; gamepad_b |

| Lua | Meaning |
|---|---|
| `Input.getAxis(name)` | smoothed value: -1..1 (button 0..1); mouse movement adds unclamped |
| `Input.getAxisRaw(name)` | without smoothing (-1, 0, 1 for keys) |
| `Input.getButton(name)`, `getButtonDown(name)`, `getButtonUp(name)` | held / pressed / released this frame |
| `Input.getVector(xName, yName)` | `(getAxis(x), getAxis(y))` with the length clamped to 1 (Godot's `get_vector`; mouse movement left out) |
| `Input.hasAction(name)`, `Input.getActionNames()` | |
| `Input.setActionEnabled(name, enabled)`, `isActionEnabled(name)` | a disabled action reads 0 / false and keeps no edges; enabling starts from rest |
| `RegisterEngineEvent(self, "onButtonDown")` → `self:onButtonDown(action)`, also `onButtonUp` | fired before `onUpdate` for each action whose button went down / up |

### How a value is made

- A binding is one Unity Input Manager entry. **Key or Mouse Button** (`buttons`): any held
  positive control gives +1, negative −1 (both: 0). The value moves towards it at `sensitivity`
  per second and back to 0 at `gravity` per second (0: at once); below `dead` it reads 0; `snap`
  jumps to 0 when the other side is pressed. **Gamepad Axis** (`gamepad_axis`): the stick or
  trigger (triggers read 0..1), a dead zone that is *rescaled* (the output starts at 0 past
  `dead`: 0.6 with dead 0.19 reads 0.506), × `sensitivity`; no smoothing. **Mouse Movement**
  (`mouse_delta`): canvas px (wheel: steps) this frame × `sensitivity`. `invert` flips any of them.
- **Bounded** sources (key bindings, gamepad axes, on-screen sticks and buttons, simulated axes)
  are summed, then clamped to -1..1; **delta** sources (mouse movement, touch-area drags,
  `addAxisDelta`) add unclamped on top. Keyboard 1 + stick 0.5 = 1; keyboard 1 + stick -1 = 0.
- `getButton` is true while a binding's button is held (on an axis action, the positive or the
  negative one), a gamepad axis or a bounded source is past the action's `press_point` (0.5).
- Smoothing runs on unscaled time: actions work in a pause menu at `Engine.timeScale = 0`.
- **Typing and menus win**: while a text field has keyboard focus (an InputField or an HTML text
  control) the keyboard does not drive actions; while an HTML document has gamepad focus
  (`Engine.wantsGamepadNavigation()`) its d-pad, left stick and A do not. A button held when the
  field took focus is released (an up edge). `getKey` and `isGamepadButtonPressed` still read the device.
- An unknown action name logs one error (`Input action "Fire4" does not exist (Project Settings >
  Input)`) and reads 0 / false; no Lua error.

### Differences from Unity

- Unity lists several entries with one name and takes the largest; here an action has several
  bindings that add up (then clamp), so the same results except opposite inputs at once.
- Unity's Input Manager clips the joystick dead zone; here it is rescaled (no jump to `dead`).
- `GetButton` on an axis action is true for both sides here (positive or negative held).
- No action maps / contexts, interactions (hold, tap) or per-player devices; bindings name a pad
  0-15 or any.

## 4. Simulated input and on-screen controls

Virtual sources add to an action like a device. A source has an owner; it lasts until set to 0,
cleared, or its owner goes:

| Lua | Meaning |
|---|---|
| `Input.simulateButton(name, pressed [, source])` | holds the button (value 1); seen by `getButton*` next frame |
| `Input.simulateAxis(name, value [, source])` | a bounded value -1..1; 0 removes it |
| `Input.addAxisDelta(name, delta [, source])` | a delta for this frame only |
| `Input.clearSimulated([source])` | removes a source (without one: every script source) |

`source` is a name (default `"script"`; global, survives scene loads) or a script instance
(`self`): then the source goes when the instance is released (GameObject destroyed, scene
unloaded), so nothing stays pressed. Values count at once; presses and edges at the next frame.

On-screen controls are sources owned by their component or element: a stick writes
`Horizontal` / `Vertical`, a button `Fire1`, a touch area `Mouse X` / `Mouse Y` deltas (section
4.1). C++: `InputActions::setSourceValue(owner, action, value, InputSourceKind::Control)`,
`addSourceDelta`, `clearSource(owner)` with any unique address as owner (a C++ script's `this` is
cleared when the script is deleted).

### 4.1 On-screen controls (touch sticks, buttons, look areas)

Three components of the component UI (GameObject > UI, `create_ui`, `add_component`), and the
same three as HTML elements (`<onscreen-stick>`, `<onscreen-button>`, `<touch-area>`:
`jasper://docs/html-ui`). They feed the input actions, so `Input.getAxis("Horizontal")` sums the
keyboard, a gamepad and the stick, and mouse-look code reading `Mouse X` / `Mouse Y` works on touch
unchanged. Each takes the first pointer that presses it (several fingers at once: a stick on one, a
touch area on another, a button on a third); the rest of the screen is unaffected.

| Component (Lua wrapper) | Writes | Settings |
|---|---|---|
| On-Screen Stick (`OnScreenStick`) | `horizontalAction` / `verticalAction` (default `Horizontal` / `Vertical`), -1..1, y up | `mode` (`OnScreenStickMode.FIXED`, `FLOATING`: the base appears under the finger, `DYNAMIC`: it also follows the finger past `moveThreshold`), `handleRange`, `deadZone`, `axes` (`OnScreenStickAxes.BOTH/HORIZONTAL/VERTICAL`), `snapX` / `snapY`, `hideWhenReleased`, `clampToArea`, `background` / `handle` (child Images) |
| On-Screen Button (`OnScreenButton`) | `action` (default `Fire1`) = `value` (1; -1 on an axis) while any finger holds it | `pressWhenPointerEnters` (a finger sliding on presses it), `releaseWhenPointerLeaves` |
| Touch Area (`TouchArea`) | `horizontalAction` / `verticalAction` (default `Mouse X` / `Mouse Y`): the drag delta x `sensitivity` (0.1, the Mouse X/Y default), y up | `invertX`, `invertY` |

- The stick follows Joystick Pack: input = (finger - base centre) / base radius (half the
  `background` width), axes zeroed per `axes`, 0 within `deadZone`, normalised past 1; snap gives
  -1/0/1 (horizontal 0 within 22.5 degrees of vertical, vertical 0 within 22.5 degrees of
  horizontal); the knob sits at input x radius x `handleRange`. The area a press may start in is
  the stick's own rect (it needs an Image, transparent from the menu). Released: value 0, actions
  cleared, knob centred, a Floating/Dynamic base hidden (`hideWhenReleased`) back where it rests.
- Read-only state: `value` (Vector2), `pressed`, `pointerId`; Touch Area `delta` (this frame,
  canvas px). Events: stick `onValueChanged(x, y)`, `onPress`, `onRelease`; button `onPress`,
  `onRelease`; touch area `onDrag(dx, dy)` once a frame while it moves.
- `visibility = OnScreenVisibility.TOUCH_DEVICES_ONLY` hides a control (and makes it ignore
  pointers) where `System.isTouchDevice()` is false; the Game view's Simulate Touch counts as touch.
- A Touch Area pressed with the mouse adds nothing to an action bound to mouse movement (the mouse
  already moves it).
- Disabling, deactivating or destroying a control, unloading its scene, a touch cancel, a finger
  or button released outside the canvas, app pause and focus loss release it: no axis stays held.
- An On-Screen Button with a Button on the same GameObject (the menu's) shows the pressed visuals.

```lua
-- the game's layout: Floating stick left, look area right, Fire button
function Player:onUpdate()
    local move = Input.getVector("Horizontal", "Vertical")      -- keyboard, pad or stick
    self.yaw = self.yaw + Input.getAxis("Mouse X")              -- mouse or touch area
    if Input.getButtonDown("Fire1") then self:fire() end        -- mouse, pad or the button
end
```

Testing without a touch screen: the Game view's Simulate Touch makes the mouse finger 0; release
the left button with Alt held to keep that finger down where it is, then the next press-drag is
finger 1 (hold the stick, look around); releasing Alt lifts the first finger.

## 5. Rebinding at run time (settings screens)

```lua
-- "press a key" for Jump
function Settings:onUpdate()
    if self.waiting then
        local control = Input.getPressedControl()
        if control then
            local ok, err = Input.setBindings("Jump", {{kind = "buttons", positive = {control}}})
            if not ok then Log.warn(err) end
            UserSettings.setStringForKey("bindings", Input.exportBindings())
            self.waiting = false
        end
    end
end

function Settings:init()
    local saved = UserSettings.getStringForKey("bindings")
    if saved ~= "" then Input.importBindings(saved) end
end
```

- `Input.getBindings(name)` returns the bindings in use as tables (the shape of §6).
- `Input.setBindings(name, bindings)` → `true` or `false, error`; `Input.resetBindings([name])`.
- `Input.exportBindings()` returns the overrides only, as JSON text; `Input.importBindings(json)`
  replaces them (`false, error` when invalid; actions the game no longer has are skipped).
- Overrides are runtime state: nothing is saved implicitly. Editor Play stop drops them.

## 6. Authoring (Project Settings > Input, MCP)

**Editor**: Edit > Project Settings > Input. The list on the left: + adds, the copy button
duplicates, the bin deletes, drag to reorder, search. The right side: name, type, description,
press point, and the bindings with Unity's labels (Gravity, Dead, Sensitivity, Snap, Invert); the
`+` of a control list picks a name, **Listen** takes the next key, mouse button or gamepad control
you press (Esc cancels). Ctrl+Z / Ctrl+Y undo inside the page; OK makes the edit one undo step of
the project history. While playing, each action shows a live value bar and a pressed dot.
**Reset to Unity Defaults** restores the table above.

**MCP**: `get_project_settings {"keys": ["input.actions"]}` and `set_project_settings
{"values": {"input.actions": [...]}}` (the whole list, validated: get it first to add one; the
string `"defaults"` restores the defaults; undoable with `undo {scope: "project"}`):

```json
[{"name": "Dash", "type": "button", "bindings": [
    {"kind": "buttons", "positive": ["left_shift", "gamepad_right_bumper"]}]},
 {"name": "Steer", "type": "axis", "bindings": [
    {"kind": "buttons", "negative": ["a"], "positive": ["d"], "gravity": 3, "sensitivity": 3, "dead": 0.001, "snap": true},
    {"kind": "gamepad_axis", "axis": "left_x", "gamepad": "any", "dead": 0.19, "sensitivity": 1}]},
 {"name": "Look X", "bindings": [{"kind": "mouse_delta", "axis": "x", "sensitivity": 0.1}]}]
```

- Fields: action `name` (unique, case-sensitive), `type` `axis` (default) or `button`,
  `description`, `press_point` (0..1, default 0.5), `bindings`. Binding `kind` `buttons`
  (`positive`, `negative`, `gamepad`, `gravity`, `sensitivity`, `dead`, `snap`, `invert`),
  `gamepad_axis` (`axis` left_x / left_y / right_x / right_y / left_trigger / right_trigger,
  `gamepad` "any" or 0-15, `dead`, `sensitivity`, `invert`), `mouse_delta` (`axis` x / y /
  wheel, `sensitivity`, `invert`). Missing numbers take Unity's defaults of the kind (keys
  3/3/0.001, joystick dead 0.19 sensitivity 1, mouse 0.1). Unknown fields are errors.
- Control names: keys `a`..`z`, `0`..`9`, `space`, `enter`, `escape`, `tab`, `backspace`,
  `left`, `right`, `up`, `down`, `left_shift`, `left_control`, `left_alt`, `left_super` (and
  `right_*`), `kp_0`..`kp_9`, `kp_enter`, `f1`..`f25`, `page_up`, `home`...; mouse `mouse_left`,
  `mouse_right`, `mouse_middle`, `mouse_4`..`mouse_8`; gamepad `gamepad_a`, `gamepad_b`,
  `gamepad_x`, `gamepad_y`, `gamepad_left_bumper`, `gamepad_right_bumper`, `gamepad_back`,
  `gamepad_start`, `gamepad_guide`, `gamepad_left_thumb`, `gamepad_right_thumb`,
  `gamepad_dpad_up` / `_right` / `_down` / `_left`. Unity spellings are accepted and normalised:
  `"left ctrl"` → `left_control`, `"mouse 0"` / `KeyCode.Mouse0` → `mouse_left`, `"joystick
  button 0"` → `gamepad_a`, `"return"` → `enter`, `"[1]"` → `kp_1`, `"LeftArrow"` → `left`.
- `project.yaml` stores the same shape under `input: {version: 1, actions: [...]}` (written only
  once the actions differ from the defaults). A node the editor cannot read is reported in the
  Console and kept as it is until the actions are edited. Exported builds load the project's
  actions at startup (`InputActions::loadJson` in the generated `main.cpp`).

## 7. Testing input as an agent

While playing, `send_input` drives the real devices and the actions:

- `{"type": "key", "key": "space"}` (press), `{"type": "gamepad", "axis": "left_x", "value": 0.6}`,
  `{"type": "touch", "action": "start", "pointer": 0, "x": 100, "y": 200}`: the bindings react as
  to a player.
- `{"type": "action", "name": "Jump", "phase": "press", "hold_frames": 3}` (`phase` press, down or
  up), `{"type": "axis", "name": "Horizontal", "value": 1, "hold_frames": 30}` (without
  hold_frames / hold_ms it stays until `value: 0`, `release_all` or Play stop),
  `{"type": "axis_delta", "name": "Mouse X", "delta": 5}` (one frame). These use the virtual source
  `"mcp"`, which adds to the bindings like an on-screen control.
- `get_play_state` shows `input`: `actions` not at rest (`{"Horizontal": 1.0, "Jump": "pressed"}`),
  `touches` (count) and `keys_held`; `held_input` lists what send_input holds.
- Check values with `eval_lua "return Input.getAxis('Horizontal')"`, or a probe script that
  counts `Input.getButtonDown("Jump")` per frame and logs it (`read_console`).
- On-screen controls with several fingers: `touch` events with different `pointer` ids, moves
  spread over frames (`gap_frames`, `wait` events), e.g. pointer 0 `start` on the stick then `move` 80 px right
  over 5 frames, pointer 1 `start` / `move` on the touch area, pointer 2 a tap on the button; then
  `eval_lua` reads `Input.getAxis("Horizontal")` (> 0.9) and `Input.getAxis("Mouse Y")` in the frame
  of the move, and a probe counts `getButtonDown("Fire1")`. `Input.isPointerOverUI(id)` tells whether
  a finger is on UI.

## 8. C++ scripts

The same statics: `Input::getKeyDown(D_KEY_SPACE)`, `Input::getTouchCount()`, `Input::getTouch(i)`,
`InputActions::getAxis("Horizontal")`, `InputActions::getButtonDown("Jump")`,
`InputActions::setSourceValue(reinterpret_cast<uintptr_t>(this), "Horizontal", 1.0f,
InputSourceKind::Script)`. Include `"Input.h"` and `"input/InputActions.h"`.
