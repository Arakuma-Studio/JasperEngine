# Input: edge queries, touches with phases, input actions (virtual axes and buttons)

Feature `input` (ledger #19), branch `gaps/input`. Status: specced 2026-10-31.
Binding conventions: [AGENT-BRIEF.md](../../engine-gaps/AGENT-BRIEF.md) "Shared conventions". Source of the gap:
[GAP-REPORT.md](../../engine-gaps/GAP-REPORT.md) §3 (Input) and §8.

## 1. Goals

1. Unity-style polling with edges: `Input.getKeyDown/getKeyUp`, `getMouseButton/Down/Up`, gamepad button down/up,
   `anyKey`, `anyKeyDown`, `inputString`, per-frame mouse delta and scroll delta.
2. A bound `Touch` value type with Unity's fields (`fingerId`, `phase`, `position`, `deltaPosition`, `deltaTime`,
   `tapCount`, plus `startPosition`) and `Input.touchCount` / `Input.getTouch(i)` / `Input.touches`.
3. **Input Actions**: named virtual axes and buttons configured in Project Settings > Input, with Unity's Input
   Manager defaults (`Horizontal`, `Vertical`, `Fire1`..`Fire3`, `Jump`, `Mouse X`, `Mouse Y`, `Mouse ScrollWheel`,
   `Submit`, `Cancel`) and Unity's per-binding knobs (positive/negative and alt buttons, gravity, sensitivity, dead,
   snap, invert, gamepad axis/button, mouse delta). `Input.getAxis/getAxisRaw/getButton/getButtonDown/getButtonUp`
   plus Godot's `getVector`. The same objects are the "action map" of the modern mental model (one system, two
   vocabularies).
4. One feeding point for on-screen controls (feature `ui-multitouch`), scripts and MCP agents: **virtual sources**,
   so `Input.getAxis("Horizontal")` sums keyboard + gamepad + on-screen stick (brief, Input convention).
5. Runtime rebinding API (settings screens) with a JSON export/import of the user's overrides.
6. Agents can drive and observe actions in Play mode (`send_input` action events, input block in `get_play_state`).
7. Identical behaviour in editor Play and every exported platform; old projects load and behave unchanged.

## 2. Non-goals

- Action maps / mapping contexts with priorities (Unreal Enhanced Input, Unity action maps), interactions
  (hold/tap/multi-tap), processors chains, composite 3D vectors. A project can enable/disable single actions
  (`Input.setActionEnabled`), which covers the game's needs. Revisit if a port needs contexts.
- Per-player device assignment (local multiplayer). Bindings can name a gamepad index (0..15) or `any`.
- Accelerometer, gyro, pen pressure, touch radius (GAP-REPORT "not needed").
- A rebinding UI widget (the API is provided; games build their screen).
- Changing `Input.isKeyPressed` & co, or the existing `onKey*/onMouse*/onTouch*` events (kept as they are).

## 3. Users and stories

- Human porting from Unity: writes `if Input.getButtonDown("Jump") then` and `Input.getAxis("Horizontal")` and gets
  Unity's numbers (same defaults, same smoothing), on keyboard, gamepad and the on-screen stick, without any setup.
- Human building a settings screen: lists actions, captures "press a key", stores the overrides with `UserSettings`.
- Human editing Project Settings > Input: adds an action "Dash", binds `left_shift` and gamepad `right_bumper`, sees
  its live value while playing.
- Human writing touch gestures: iterates `Input.touches`, reads `phase == TouchPhase.Began`, `deltaPosition`,
  `tapCount` (double tap).
- Agent: `set_project_settings {"input.actions": [...]}` to author; in Play mode `send_input` with
  `{type:"action", action:"Jump"}` or `{type:"axis", action:"Horizontal", value:1, hold_frames:30}` and checks
  `get_play_state` (input block) or `eval_lua "return Input.getAxis('Horizontal')"`; `send_input` key/gamepad/touch
  events exercise the real bindings end to end.

## 4. Research summary

| Source | Take | Leave (why) |
|---|---|---|
| Unity Input Manager <https://docs.unity3d.com/Manual/class-InputManager.html>, `Input` <https://docs.unity3d.com/ScriptReference/Input.html> | Names (`GetAxis`, `GetAxisRaw`, `GetButton(Down/Up)`, `GetKey(Down/Up)`, `GetMouseButton*`, `anyKey(Down)`, `inputString`, `mouseScrollDelta`), the default axes, per-entry gravity/sensitivity/dead/snap/invert, button names in strings | Several entries sharing a name combined by max magnitude (we keep one action with several bindings; combine rule §6 P3); "joystick button N" numbering (we name pad buttons) |
| Unity `Touch` <https://docs.unity3d.com/ScriptReference/Touch.html>, `TouchPhase` | Fields and phases, an Ended/Canceled touch is listed in the frame it ends, `multiTouchEnabled`, `simulateMouseWithTouches` (= our `Engine.callMouseInTouchEvent`) | `pressure/radius/altitude` (no platform data path today) |
| Unity Input System actions <https://docs.unity3d.com/Packages/com.unity.inputsystem@1.7/manual/Actions.html>, on-screen controls <https://docs.unity3d.com/Packages/com.unity.inputsystem@1.7/manual/OnScreen.html> | Actions with bindings, press point 0.5 for analog-as-button, on-screen controls writing into a virtual device that actions read, runtime rebinding with JSON overrides | Action maps, interactions, processors, control schemes, the asset-generated C# class |
| Godot `Input` / `InputMap` <https://docs.godotengine.org/en/stable/classes/class_input.html>, <https://docs.godotengine.org/en/stable/classes/class_inputmap.html> | One action map in project settings; `get_vector` (circular dead zone, length clamped to 1); `action_press/action_release` for simulation; dead zone rescaled to 0..1; just-pressed per frame | `exact_match` modifiers matching (no modifier-qualified bindings in v1) |
| Unreal Enhanced Input <https://dev.epicgames.com/documentation/en-us/unreal-engine/enhanced-input-in-unreal-engine> | Value types (bool / axis), dead zone and negate as per-binding modifiers | Mapping contexts, triggers (non-goal) |
| The game (`/tmp/gamesrc/Minigames`) | `FPSInput.cs`: `clamp(joystick + GetAxis(...), -1, 1)`; `ItemManager.cs`: `GetButtonDown/Up("Jump")`, `GetButton("Vertical")`; `GetKeyDown(KeyCode.Mouse0)` in Garden/Button3D; `DriveCar.cs` `GetAxisRaw` | `TouchArea.cs` indexes `Input.touches[pointerId]` by finger id (a bug: an index is not a finger id); our API looks touches up by id |

## 5. Engine today (what we build on)

- `engine/core/Input.h/.cpp`: held state in `std::map<int,bool>` (keys, mouse), `touches` vector of `{pointer,
  position}` (`Input.h:194`), gamepads, modifiers. Writers are private, called by `Engine::system*`
  (`Engine.cpp:1888-2330`): the device state is updated **before** UI consumption, so polling sees every press
  (Unity semantics).
- Events arrive between frames on every platform (GLFW/Win32 polling, Android looper, iOS UIKit, browser callbacks,
  the editor's `PlayInput`), then `Engine::runFrame` (`Engine.cpp:1335`) runs fixed steps, `onUpdate`, scene
  updates, `onPostUpdate`, `ObjectManager::update`.
- Lua `Input` class: `CoreClassesLua.cpp:1534-1725`; `getTouches()` returns unbound `Touch` (lua-api.md:357).
- Simulation flags `Engine.callMouseInTouchEvent` / `callTouchInMouseEvent` (`CoreClassesLua.cpp:719-721`,
  `Engine.cpp:81-82`, both default false): touch→mouse adds `Input::addMousePressed(D_MOUSE_BUTTON_1)`
  (`Engine.cpp:1917`), mouse→touch adds touch 0 (`Engine.cpp:2047`).
- Gamepad UI consumption: while a document has gamepad focus (`Engine::wantsGamepadNavigation`) the d-pad, left stick
  and A are the document's (`docs/mcp/guide/html-ui.md` "Consumed").
- The key-name table of MCP `send_input` (`editor/mcp/tools/PlayTools.cpp:463-560`, `parseKeyName/keyName`).

## 6. Product decisions

- **P1. One system: Input Actions.** Project Settings > Input holds an ordered list of actions. An action is what
  Unity calls a virtual axis/button and what Godot/Input System call an action. There is no second "action map"
  system. Rationale: two systems with overlapping semantics would split docs, tools and on-screen controls.
- **P2. A binding = one Unity Input Manager entry.** Each action has 1..n bindings; each binding carries its own
  gravity/sensitivity/dead/snap/invert, exactly as Unity's entries do (keyboard `Horizontal` gravity 3, joystick
  `Horizontal` dead 0.19). Binding kinds: `buttons` (keys, mouse buttons, gamepad buttons as positive/negative lists:
  Unity's "Key or Mouse Button"), `gamepadAxis` (Unity "Joystick Axis"), `mouseDelta` (Unity "Mouse Movement": x, y,
  wheel).
- **P3. Combine rule.** Bounded sources (button bindings after smoothing, gamepad axes, on-screen sticks/buttons,
  simulated axes) are **summed then clamped to [-1, 1]**; delta sources (mouse movement, touch area drags, simulated
  deltas) are **added unclamped** on top. Rationale: matches the game's `clamp(joystick + GetAxis)`, the brief, and
  Unity's in-entry rule (left+right held = 0); mouse look must not clamp. Documented difference: Unity combines
  same-named entries by max magnitude (identical results except opposite simultaneous inputs).
- **P4. Edges are per frame.** A down/up edge is visible from the next frame that runs the update until that frame
  ends, whatever the time scale. Frames that do not run the update (app paused, Play paused or held, loading hold)
  keep pending edges. A press and release inside one frame make `getKeyDown` and `getKeyUp` both true and `getKey`
  false (Unity). In `onFixedUpdate` an edge is seen by every fixed step of its frame (documented: read edges in
  `onUpdate`).
- **P5. Smoothing runs on unscaled time.** Gravity/sensitivity ramps use the unscaled frame delta (an action works in
  a pause menu at `timeScale` 0). This is input, not game time; documented on the Input page.
- **P6. Dead zone is rescaled** (Godot, Input System): `v' = sign(v)·(|v|-dead)/(1-dead)`, so the output starts at 0
  instead of jumping to `dead` (Unity Input Manager clips without rescaling; documented).
- **P7. Typing and menus win over actions.** While a text field has keyboard focus (native InputField or an HTML text
  control: `UISystem::isInputFieldFocused`, `UIDocumentSystem::hasFocusedTextInput`), keyboard controls do not drive
  actions. While `Engine::wantsGamepadNavigation()` is true, the d-pad, left stick and A do not drive actions (the
  HTML UI already consumes exactly these). Raw queries (`getKey`, `isGamepadButtonPressed`) still read the device.
  Rationale: the player must not walk while typing a name or using a menu.
- **P8. Unknown action names** log one error per name (`Input action "Fire4" does not exist (Project Settings >
  Input)`) and return 0/false. No Lua error (a typo must not kill the update), unlike Unity's exception.
- **P9. Touches.** New Unity-semantic list (`touchCount`, `getTouch`, `touches`) includes touches that ended or were
  cancelled this frame. The legacy `numTouches/getTouchPosition/getTouches/findTouchIndex/isTouch` keep "fingers down
  now". A touch that begins and ends between two frames is reported `Began` in frame N and `Ended` in frame N+1.
- **P10. Simulation flags stay as they are** (defaults false). The new edge queries read the same device state,
  so `getMouseButtonDown(0)` is true for a touch exactly when `callMouseInTouchEvent` is on, and
  `getTouch` reports the mouse as finger 0 exactly when `callTouchInMouseEvent` is on. In the editor's "Simulate
  touch" mode the mouse is finger 0 (`PlayInput.cpp:102`). Bindings never see a simulated device twice: the touch
  that simulates `mouse_left` drives `Fire1` once.
- **P11. Rebinding** is runtime-only state layered over the project defaults: `Input.setBindings` / `resetBindings`,
  `exportBindings()` returns the overrides as JSON text, `importBindings(text)` applies them. The game persists them
  with `UserSettings` (no implicit file).
- **P12. Control names** are the MCP `send_input` names, moved to the engine (`InputControlNames`) so the editor,
  MCP and Lua share one table: keys `a`..`z`, `0`..`9`, `space`, `enter`, `escape`, `left`, `left_shift`,
  `left_control`, `left_alt`, `kp_enter`, `f1`...; mouse `mouse_left`, `mouse_right`, `mouse_middle`, `mouse_4`..
  `mouse_8`; gamepad buttons `gamepad_a`, `gamepad_b`, `gamepad_x`, `gamepad_y`, `gamepad_left_bumper`, ...,
  `gamepad_dpad_up`; gamepad axes `left_x`, `left_y`, `right_x`, `right_y`, `left_trigger`, `right_trigger`. Unity
  spellings are accepted on input and normalised (`"left ctrl"`→`left_control`, `"mouse 0"`→`mouse_left`,
  `"joystick button 0"`→`gamepad_a`, `"return"`→`enter`, `"[1]"`→`kp_1`).
- **P13. Axis direction convention**: positive = right/up. Gamepad Y axes report up as -1 (`Input.h` GLFW
  convention), so the default `Vertical` gamepad binding has `invert: true` (Unity does the same). Canvas Y grows
  downwards, so `Mouse Y` is `-dy`.

## 7. Design: runtime C++

### 7.1 Input state (engine/core/Input.h/.cpp)

- Replace the held `std::map<int,bool>` with fixed arrays: `keyState[D_KEY_LAST+1]`, `mouseState[8]`, per gamepad
  `buttonState[15]`. Each `ControlState { bool held; uint64_t downFrame; uint64_t upFrame; }`.
- `static uint64_t inputFrame` (starts at 1). Writers stamp `downFrame/upFrame = inputFrame` (the frame that will see
  it). `getKeyDown(k) = keyState[k].downFrame == inputFrame`; same for up.
- `static Vector2 mouseDeltaAccum, scrollAccum` accumulated by every `systemMouseMove` (in canvas units **before**
  the outside-canvas rejection, from the raw platform position, so `MouseMode::CAPTURED` look works) and
  `systemMouseScroll`; published as `mouseDelta`, `mouseScrollDelta` at frame start.
- `static std::u32string inputChars` from `systemCharInput/systemTextInput` (published as `inputString`, UTF-8).
- Touch records: `struct Touch { int fingerId; TouchPhase phase; Vector2 position, startPosition, deltaPosition,
  rawPrevPosition; float deltaTime; int tapCount; double lastChangeTime, beginTime; bool endedPending;
  bool beganThisFrame; }` (rename the old `int pointer` to `fingerId`; keep `pointer` as a C++ alias accessor for
  the legacy getters). `TouchPhase { Began, Moved, Stationary, Ended, Canceled }`.
  - start: append (if the id is still listed as ended this frame, both entries exist for that frame); `Began`.
  - move: position updated, phase becomes `Moved` at publication if the position changed during the frame.
  - end/cancel: if `Began` not yet published, mark `endedPending` (reported `Ended` next frame), else `Ended`/
    `Canceled`; the legacy list drops the finger immediately as today.
  - `tapCount`: a touch that begins within `Input::multiTapTime` (0.3 s, same value as `UI_DOUBLE_CLICK_TIME`,
    `UISystem.cpp:21`) of the previous touch's end and within `multiTapDistance` (24 canvas px) of its start gets
    previous tapCount + 1 (the previous touch must have lasted < 0.3 s), else 1.
  - `deltaTime`: unscaled seconds since the touch's last position change; `deltaPosition`: position change in the
    published frame.
- `Input::beginFrame(double unscaledDt)` (new, called by `Engine::runFrame` right after `flushCharInput()` and before
  `SceneManager::applyPendingLoad()`, only on frames that run the update, P4): publishes deltas, touch phases, then
  `InputActions::update(unscaledDt)` and fires `Engine::onButtonDown/onButtonUp` for actions whose edge happened.
- `Input::endFrame()` (after `ObjectManager::update`, same condition): drops `Ended/Canceled` touches, sets
  `Moved`→`Stationary` candidates, clears deltas and `inputChars`, `inputFrame++`.
- `Input::releaseAll()`: releases every held key, mouse button, pad button (up edges stamped) and cancels touches;
  called on app pause, focus loss (feature `platform-services` adds `onFocus`; until then the existing
  `systemPause`), and by the editor at Play stop/focus loss (`PlayInput::releaseAll`, `PlayInput.cpp:171`).
- Device ids: `getGamepadButtonDown(id, b)` with `id = -1` = any pad.

### 7.2 Input actions (new engine/core/input/InputActions.h/.cpp, InputControlNames.h/.cpp)

```cpp
enum class InputActionType { Axis, Button };            // Button: value 0..1 (negative list ignored)
enum class InputBindingKind { Buttons, GamepadAxis, MouseDelta };
enum class MouseDeltaAxis { X, Y, Wheel };
struct InputBinding {
    InputBindingKind kind = InputBindingKind::Buttons;
    std::vector<InputControl> positive, negative;          // Buttons (InputControl = {device, code})
    int gamepad = -1;                                      // -1 any, else 0..15 (Buttons on pads, GamepadAxis)
    int axis = D_GAMEPAD_AXIS_LEFT_X;                      // GamepadAxis
    MouseDeltaAxis mouseAxis = MouseDeltaAxis::X;          // MouseDelta
    float gravity = 3.0f, sensitivity = 3.0f, dead = 0.001f;
    bool snap = false, invert = false;
};
struct InputAction {
    std::string name, description;
    InputActionType type = InputActionType::Axis;
    float pressPoint = 0.5f;                               // |value| at which an analog source counts as pressed
    std::vector<InputBinding> bindings;
};
class JASPERENGINE_API InputActions {                     // static facade, one map per process
public:
    static void loadDefaults();                            // Unity defaults (7.3)
    static bool loadJson(const std::string& json, std::string* error);   // project definition
    static std::string toJson();
    static void update(double unscaledDt);                 // from Input::beginFrame
    static float getAxis(const std::string& name);  static float getAxisRaw(const std::string& name);
    static bool getButton(const std::string&); static bool getButtonDown(const std::string&); static bool getButtonUp(const std::string&);
    static Vector2 getVector(const std::string& x, const std::string& y);
    // virtual sources (on-screen controls, scripts, MCP); key identifies the owner
    static void setSourceValue(uintptr_t owner, const std::string& action, float value, InputSourceKind kind);
    static void addSourceDelta(uintptr_t owner, const std::string& action, float delta);
    static void clearSource(uintptr_t owner);              // all actions of that owner
    // rebinding (runtime overrides on top of the definition)
    static bool setBindings(const std::string& action, const std::vector<InputBinding>&);
    static void resetBindings(const std::string& action /* "" = all */);
    static std::string exportOverrides(); static bool importOverrides(const std::string& json, std::string* error);
    static void setActionEnabled(const std::string& action, bool enabled);
};
```

Evaluation per action per frame (`update`):
1. For each `Buttons` binding: `pos = any positive control held (not suppressed, P7)`, `neg = any negative held`;
   `target = (pos?1:0) - (neg?1:0)` (× -1 if `invert`). Raw value = target. Smoothed value `s` (per binding state):
   if `snap` and `target != 0` and `sign(target) != sign(s)`: `s = 0`; if `target != 0`: `s = moveTowards(s, target,
   sensitivity·dt)`; else `s = moveTowards(s, 0, gravity·dt)`; `|s| < dead` → 0 for output. `gravity <= 0` or
   `sensitivity <= 0` mean instant (Unity: large values). Button pressed = `pos` (Unity: the positive button).
2. `GamepadAxis`: `v` = pad axis (`gamepad` −1: the pad with the largest |v|), triggers remapped from [-1,1] rest -1
   to [0,1]; dead-zone rescale (P6); × `sensitivity`; × -1 if `invert`; raw = same (no smoothing, Unity). Pressed
   when `|v| >= pressPoint`.
3. `MouseDelta`: delta (canvas px this frame; Y as `-dy`, wheel as scroll y steps) × `sensitivity` (× -1 invert).
   Never pressed.
4. Virtual sources: bounded values summed with bounded bindings; deltas added (P3). A bounded virtual source counts
   as pressed when `|v| >= pressPoint` (an on-screen button writes 1).
5. `value = clamp(Σ bounded, -1, 1) + Σ delta`; `raw` likewise from raw values. Button type clamps to [0, 1].
   `pressed = any binding pressed || any bounded virtual source pressed`; down/up edges from `pressed` transitions,
   plus the edge rule of P4 applied to control edges (a press and release within one frame produce down and up).
6. Disabled actions (`setActionEnabled(false)`) report 0/false and keep no edges; re-enabling starts from rest.

`getVector(x, y)`: `(getAxis(x), getAxis(y))`, length clamped to 1 (Godot). Delta sources included unclamped is
nonsense for a vector: `getVector` uses only bounded sources.

### 7.3 Defaults (match Unity's Input Manager)

| Action | Type | Bindings (gravity, sensitivity, dead, snap, invert) |
|---|---|---|
| Horizontal | Axis | buttons −[left, a] +[right, d] (3, 3, 0.001, snap); gamepadAxis left_x any (0, 1, 0.19) |
| Vertical | Axis | buttons −[down, s] +[up, w] (3, 3, 0.001, snap); gamepadAxis left_y any (0, 1, 0.19, invert) |
| Fire1 | Button | buttons +[left_control, mouse_left] (1000, 1000, 0.001); buttons +[gamepad_a] |
| Fire2 | Button | buttons +[left_alt, mouse_right]; +[gamepad_b] |
| Fire3 | Button | buttons +[left_shift, mouse_middle]; +[gamepad_x] |
| Jump | Button | buttons +[space]; +[gamepad_y] |
| Mouse X | Axis | mouseDelta x (sensitivity 0.1) |
| Mouse Y | Axis | mouseDelta y (sensitivity 0.1) |
| Mouse ScrollWheel | Axis | mouseDelta wheel (sensitivity 0.1) |
| Submit | Button | buttons +[enter, kp_enter, space]; +[gamepad_a] |
| Cancel | Button | buttons +[escape]; +[gamepad_b] |

(Unity's "joystick button 0..3" are A, B, X, Y in our layout.) "Reset to defaults" restores this table.

### 7.4 Settings format and serialization

`project.yaml` gets an `input` node (absent = defaults, nothing written until the user edits; `Stream.cpp:2612`
encode, `:2948` decode):

```yaml
input:
  version: 1
  actions:
    - name: Horizontal
      type: axis            # axis | button
      press_point: 0.5      # omitted when default
      bindings:
        - {kind: buttons, negative: [left, a], positive: [right, d], gravity: 3, sensitivity: 3, dead: 0.001, snap: true}
        - {kind: gamepad_axis, axis: left_x, gamepad: any, gravity: 0, sensitivity: 1, dead: 0.19}
```

Validation (editor and `loadJson`): names unique (case-sensitive, trimmed, non-empty), unknown control names are
an error naming the binding, numbers finite and >= 0 (`dead` < 1). JSON is the same shape (used by the Generator,
MCP and `exportBindings`). Old projects: no node → defaults; no migration.

### 7.5 Exported builds

`Generator.cpp` (init block, next to `Engine::setScalingMode`, `Generator.cpp:2301-2311`) emits
`InputActions::loadJson(R"jinput(<json>)jinput", nullptr);` when the project has an `input` node, nothing otherwise
(the engine calls `loadDefaults()` at startup). Play mode: `Project` applies the definition to `InputActions` when the
project loads and when settings change (like `applyLayerCollisionMatrices`, `Project.cpp:2563`).

## 8. Lua API (engine/core/script/binding/CoreClassesLua.cpp, class `Input`)

Keys accept an `Input.KEY_*` integer or a control name string. All functions static.

| Name | Semantics |
|---|---|
| `Input.getKey(key)`, `getKeyDown(key)`, `getKeyUp(key)` | held / pressed this frame / released this frame (`getKey` = `isKeyPressed`) |
| `Input.getMouseButton(b)`, `getMouseButtonDown(b)`, `getMouseButtonUp(b)` | `b` 0 left, 1 right, 2 middle (= `Input.MOUSE_BUTTON_*`) |
| `Input.getGamepadButtonDown(id, button)`, `getGamepadButtonUp(id, button)` | `id` -1 = any pad |
| `Input.anyKey`, `Input.anyKeyDown` (properties) | any key, mouse button or pad button held / pressed this frame |
| `Input.inputString` | UTF-8 text typed this frame |
| `Input.mouseDelta` (Vector2) | mouse movement this frame, canvas px, y down; works with `MouseMode.CAPTURED` |
| `Input.mouseScrollDelta` (Vector2) | wheel steps this frame (y positive up, x positive right) |
| `Input.touchCount`, `Input.getTouch(index)` (0-based), `Input.touches` (array), `Input.getTouchById(fingerId)` (Touch or nil) | P9 |
| `Input.multiTouchEnabled` (rw, default true) | false: only the first finger is reported by the new list and by bindings |
| `Input.touchSupported` | `System.isTouchDevice()` |
| `Input.getAxis(name)`, `getAxisRaw(name)` | number (7.2) |
| `Input.getButton(name)`, `getButtonDown(name)`, `getButtonUp(name)` | bool |
| `Input.getVector(xName, yName)` | Vector2, length <= 1 |
| `Input.hasAction(name)`, `Input.getActionNames()` | bool / array of strings |
| `Input.setActionEnabled(name, enabled)`, `Input.isActionEnabled(name)` | |
| `Input.simulateButton(name, pressed [, source])` | a virtual bounded source (value 1/0) keyed by `source` (default `"script"`) |
| `Input.simulateAxis(name, value [, source])` | bounded virtual value, clamped to [-1, 1]; 0 removes it |
| `Input.addAxisDelta(name, delta [, source])` | delta source for this frame only |
| `Input.clearSimulated([source])` | removes a source (all script sources without argument) |
| `Input.getBindings(name)` → array of tables | the shape of 7.4 (`kind`, `positive`, ...) |
| `Input.setBindings(name, bindings)` → bool, `Input.resetBindings([name])` | runtime override (P11) |
| `Input.exportBindings()` → string, `Input.importBindings(json)` → bool, error | overrides only |
| `Input.getPressedControl()` → string or nil | first control that went down this frame (keys, mouse, pad buttons, pad axes past 0.5 as `"left_x+"`): "press a key" screens |

`Touch` (value class, read-only fields): `fingerId`, `phase` (`TouchPhase.Began|Moved|Stationary|Ended|Canceled`),
`position`, `startPosition`, `deltaPosition` (Vector2, canvas px, y down), `deltaTime` (s, unscaled), `tapCount`.
Engine events (`RegisterEngineEvent`): `onButtonDown(actionName)`, `onButtonUp(actionName)`, fired in
`Input::beginFrame` (before `onUpdate`). Script-created virtual sources keyed by a script are cleared when the
script instance is released (scene unload), like its subscriptions.

C++ scripting: the same statics on `jasperengine::Input` / `InputActions` (headers are public).

## 9. Editor UX

- **Project Settings > Input** (new category in `ProjectSettingsWindow.cpp:120-148`, keywords "input axes actions
  buttons keys gamepad bindings horizontal vertical jump fire"). Left: action list (add, duplicate, delete, drag to
  reorder, search). Right: name, type, description, press point, bindings as collapsible rows; a binding row shows
  its kind and fields (control pickers with a "Listen" button that captures the next key/mouse/pad control in the
  editor; axis dropdowns; numeric fields with Unity's labels Gravity/Dead/Sensitivity/Snap/Invert). Buttons:
  "Reset to Unity Defaults". During Play: each action shows a live value bar and a pressed dot.
- Edits are project settings changes recorded in the project command history (undo/redo like the Layer Collision
  Matrix) and saved with `project.yaml`.
- Inspector: nothing new. Game view: nothing new.

## 10. MCP / agent surfaces

- `get_project_settings` / `set_project_settings` key `input.actions` (array, the JSON shape; set replaces the list,
  validated, one undo step) in `editor/mcp/tools/ProjectTools.cpp` (next to `physics.layer_collision_matrix`,
  `:1402`).
- `send_input` (`PlayTools.cpp:2251+`) new event types, using a virtual source `"mcp"` released by `release_all`,
  play stop and restart (`releaseHeldInput`):
  - `{type:"action", name:"Jump", phase: "press"|"down"|"up", hold_frames, hold_ms}` (press = down, hold, up;
    `phase`, not `action`, because the other event types use `action` for their own verbs).
  - `{type:"axis", name, value (-1..1), hold_frames, hold_ms}`; `value: 0` releases.
  - `{type:"axis_delta", name, delta}` (one frame).
  Description stays inside the tool budget (`tools/mcp/tool_budget.py`): one line `action: name, phase
  press|down|up; axis: name, value; axis_delta: name, delta`.
- `get_play_state` gains a compact `input` object: `actions` (non-zero values and pressed ones only, e.g.
  `{"Horizontal": 1.0, "Jump": "pressed"}`), `touches` (count), `keys_held` (names, at most 16).
- Agent guide (`McpInstructions.cpp`): one line "Input: Input.getAxis/getButtonDown('Jump') read Project Settings >
  Input actions; test them with send_input action/axis events (jasper://docs/input)". Details in a new
  `docs/mcp/guide/input.md`.

## 11. Per-platform notes

All logic is in `engine/core`, platform layers keep calling `Engine::system*`. Checks per platform: Linux/Windows/
macOS keyboard+mouse+gamepad (GLFW/Win32/Cocoa), Android/iOS touches (finger ids: Android pointer ids from 0,
iOS slot ids from 1 (`EngineView.mm:461`): fingerIds are opaque small integers, stable while down; documented),
Web (touch identifiers, Gamepad API). Mouse delta in captured mode: Linux accumulates warps
(`LinuxInputRouter.cpp:198-201`), Web pointer lock gives movement; both reach `systemMouseMove`, which feeds the
accumulator before the canvas test.

## 12. Behaviour rules and edge cases

- Ordering inside a frame: platform events (stamped `inputFrame`) → `beginFrame` (publish, actions,
  `onButtonDown/Up`) → fixed steps → `onUpdate` → scenes → `onPostUpdate` → destroy → `endFrame`.
- Same key pressed twice in one frame (down, up, down): `getKeyDown` true, `getKeyUp` true, `getKey` true.
- Key repeat (`isRepeat`) never creates edges.
- Gamepad disconnect: its held buttons get up edges (`Engine.cpp:2287`), its axes return to rest; `gamepad: any`
  bindings move to the remaining pads.
- Focus loss / app pause: `Input::releaseAll()` (up edges are seen by the first frame after resume).
- Scene load: actions, touches and edges are global and survive; sources owned by destroyed on-screen controls or
  released scripts are removed in the same frame (no stuck axis).
- Time scale 0: actions still update (P5); edges still work.
- Editor Play stop: `releaseAll`, MCP source cleared, rebinding overrides reset (`InputActions::resetBindings("")`).
- `multiTouchEnabled = false`: fingers after the first are invisible to the new list and bindings (legacy list and
  UI still get them).
- Touch ended and same id begun in the same frame: both entries listed for that frame (Ended first).
- Text field focused: action keyboard controls suppressed (P7); edges that happened before focus finish normally
  (an up edge still fires so nothing stays pressed).

## 13. Performance (mobile)

Fixed arrays instead of maps (no allocation per query); action lookup by name through an
`unordered_map<string,int>` built at load, with a one-entry per-call-site friendly cache not needed (lookups are
~20 per frame). Action evaluation O(actions × bindings) ≈ 11 × 2 per frame. Touch list ≤ 10 entries. No allocations
per frame in steady state (vectors reserved).

## 14. Tests

Editor unit tests (`editor/tests`, `EditorTests.cmake`; pure engine code linked in the test target):
- `InputEdgesTest`: down/up stamping, both edges in one frame, held frames, no edge on repeat, edges kept across a
  frame that did not run the update, `anyKeyDown`, `inputString` UTF-8 (astral), mouse delta accumulation including
  outside-canvas moves, scroll delta.
- `InputTouchTest`: Began→Moved→Stationary→Ended with deltas and deltaTime; begin+end in one frame → Began then
  Ended; cancel → Canceled; tapCount 1,2,3 and reset after 0.3 s or 24 px; id reuse in one frame; legacy list
  unchanged (numTouches drops the finger at once); `multiTouchEnabled=false`.
- `InputActionsTest`: Unity numbers: Horizontal with `right` held reaches 0.05 after one 1/60 s frame (sensitivity
  3 → 3/60) and 1 after 20 frames, falls at gravity 3; snap jumps through 0; dead zone rescale (0.19 → 0, 0.595 →
  0.5); invert; triggers 0..1; sum+clamp (keyboard 1 + stick 0.5 = 1; keyboard 1 + stick -1 = 0); deltas
  unclamped; button pressPoint; disabled action; unknown name logs once; P7 suppression with a focused field and
  with gamepad navigation; JSON load/validation errors; overrides export/import round trip; defaults table equals
  §7.3.
- `GeneratorInputTest`: the generated init contains `InputActions::loadJson` with the project JSON (only when the
  node exists); the literal survives `)"` inside descriptions (delimiter chosen to be absent).
MCP end-to-end (`tools/mcp/tests/test_input.py`, new, fixture project):
- probe script counts `getKeyDown/getKeyUp("space")` and `getButtonDown("Jump")` per frame: `send_input` key press
  with hold_frames 3 → exactly one down, one up, `getKey` true for 3 frames.
- gamepad `left_x` 0.6 → `getAxis("Horizontal")` 0.506 (rescaled), `getAxisRaw` same.
- `{type:"axis", name:"Horizontal", value:1}` + keyboard `left` held → 0; release_all → 0.
- touch start/move/end with two pointers → probe records phases and fingerIds (Began, Moved, Ended).
- `set_project_settings input.actions` adds "Dash" bound to `left_shift`; play; key press → `getButtonDown("Dash")`.
- `get_play_state` shows the input block while an axis is held.
- Stubs: `tools/mcp/tests/test_stubs.py` passes (`-- unresolved:` empty; `Touch`, `TouchPhase` typed).
- Export: `tools/mcp/tests/test_build.py` (Linux build) runs a scene whose script prints `Input.getActionNames()`.

## 15. Acceptance criteria

1. Every Lua name in §8 exists, is in the generated stubs and in `docs/mcp/guide/lua-api.md` §2.2.
2. A new project and an old project without an `input` node report the 11 default actions; old scripts using
   `isKeyPressed`/`onKey*`/`onTouch*` behave identically (existing tests green).
3. The numbers in §14 `InputActionsTest` pass; the MCP tests pass under xvfb.
4. A Linux export and the Web build (emsdk compile) contain the project's actions; Android compiles.
5. Project Settings > Input edits are undoable and persist; live values show while playing.
6. With a focused text field, typing `d` does not move `Horizontal`; with a focused HTML button, the d-pad does not.
7. `docs/mcp/guide/input.md` exists; `docs/unity-glossary.md` maps Input Manager → Project Settings > Input,
   `GetAxis` → `Input.getAxis`, `Touch`/`TouchPhase`, `KeyCode.Mouse0` → `mouse_left`.

## 16. Integration points

- `engine/core/Input.h:194` (Touch), `Input.cpp` (state), new `engine/core/input/InputActions.*`,
  `InputControlNames.*` (moved from `editor/mcp/tools/PlayTools.cpp:463-560`).
- `engine/core/Engine.cpp:1335-1440` (`runFrame`: `beginFrame` after `flushCharInput` ~1360, `endFrame` after
  `ObjectManager::update` ~1438, only when `!paused && !simulationHeld`), `:1580-1590` (pause → `releaseAll`),
  `:1888-2330` (writers; mouse delta before `transformCoordPos` rejection), `Engine.h` (`onButtonDown/Up`
  subscriptions next to `onKeyDown` ~687; clear/removeByTag lists `Engine.cpp:920-990`; `pauseGameEvents` `:373`).
- `engine/core/script/binding/CoreClassesLua.cpp:1534-1725` (Input), `:768-790` (engine events).
- `editor/Project.cpp` (apply on load/change), `editor/Stream.cpp:2612/2948`, `editor/Generator.cpp:2301`,
  `editor/window/dialog/ProjectSettingsWindow.cpp:120-148,1012`, `editor/window/PlayInput.cpp:171`,
  `editor/mcp/tools/ProjectTools.cpp:1402`, `editor/mcp/tools/PlayTools.cpp:2251-2840`, `editor/mcp/McpInstructions.cpp`.
- Docs: `docs/mcp/guide/lua-api.md` (§1.3 events table, §1.5 "Edge detection" paragraph lines 242-250 rewritten to
  `getKeyDown`, §2.2, pitfall 15 at line 1487), new `docs/mcp/guide/input.md`, `docs/unity-glossary.md`, README
  feature list ("Input actions").

## 17. Cross-feature dependencies

- **ui-multitouch** (consumer): uses `InputActions::setSourceValue/addSourceDelta/clearSource` with the control's
  entity/element as owner; on-screen buttons are bounded sources, touch areas delta sources. Pointer ids in UI
  events equal `Touch.fingerId` (ui-multitouch §6). `Input.isPointerOverUI(id)` is specified there.
- **platform-services**: its focus-lost event calls `Input::releaseAll()`.
- **script-lifecycle**: script-owned virtual sources are cleared when the instance is released; `onDisable` of an
  on-screen control clears its source (ui-multitouch).
- **debug-overlay** may show the input block of `get_play_state` (optional).

## Implementation notes (2026-10-31, branch `gaps/input`)

Implemented as specified, with these decisions where the spec was open, ambiguous or did not fit
the code:

1. **`getButton` on an axis action** is true while the positive *or* the negative side is held
   (the spec said "Button pressed = pos"). Unity's `GetButton("Horizontal")` reacts to both
   sides and the game's `GetButton("Vertical")` expects it. A Button action ignores negatives.
2. **Touch internals stay private.** The public `Touch` has Unity's fields plus `startPosition`
   and `pointer()` (the old name); the bookkeeping fields the spec listed in it (raw previous
   position, change/begin times, pending end) live in a private record in `Input.cpp`. Touch
   times (tapCount, deltaTime) run on the input frame clock (the unscaled deltas `beginFrame`
   gets), so they have frame resolution and are deterministic under a fixed frame time.
3. **Writers are public.** `Input::addKeyPressed` & co were private to `Engine`; they are public
   (documented as the platform/engine entry points) so tests and tools can drive the state.
4. **Releases outside the canvas.** A mouse button released or a finger lifted outside the canvas
   (the events are dropped as before) now still clears the held state: before, `isMousePressed`
   / `numTouches` stayed stuck, which would have stuck `Fire1` or an on-screen stick. A
   cancelled finger that simulated the mouse (`callMouseInTouchEvent`) releases it too.
5. **The simulated mouse moves.** With `callMouseInTouchEvent` a finger's movement is added to
   `mouseDelta` (so `Mouse X/Y` look works on touch, Unity's `simulateMouseWithTouches`). A finger
   hidden by `multiTouchEnabled = false` does not press the simulated mouse.
6. **Virtual sources count at once, presses next frame.** `getAxis` adds the sources' values at
   query time (a delta added during the frame, e.g. by a touch area in `UISystem::update`, is seen
   in that frame); `getButton*` and the edges come from `update` in `beginFrame`.
7. **Source owners.** `InputSourceKind` is the owner's kind: `Control` (on-screen controls, the
   default), `Script`, `Agent` (MCP). Added: `clearSources(kind)`, `clearAllSources()`,
   `namedSource(name)` (a stable owner for a name, never a real address) and a `kind` argument on
   `addSourceDelta`. A script instance's sources are cleared when it is released through the
   existing tag path: `Engine::removeSubscriptionsByTag("_<address>_")` clears the owner with that
   address, which covers Lua `self` (`Input.simulateAxis(name, v, self)`) and C++ scripts' `this`.
   Lua sources named by a string (default `"script"`) are global and survive scene loads.
8. **Undo.** Project Settings is a modal dialog with OK / Cancel and the Layer Collision Matrix is
   in fact not undoable, so: the Input page has its own undo (Ctrl+Z / Ctrl+Y, Undo / Redo
   buttons) while the dialog is open, and OK records the whole Input edit as one step of the
   project history (`InputActionsCmd`, also used by MCP `input.actions`; `undo {scope: project}`).
9. **MCP.** `input.actions` is left out of the no-argument `get_project_settings` listing (it is
   long; the note says so) and accepts the string `"defaults"`. `send_input` `action` / `axis`
   events also take `action` as the name when `name` is absent. The tool definitions grew by 76
   characters (descriptions trimmed elsewhere to pay for most of it); the agent guide shrank.
10. **JSON shape.** Missing numbers take the defaults of the binding's kind (keys 3/3/0.001,
    joystick dead 0.19 sensitivity 1, mouse 0.1), `type` defaults to axis, unknown fields are
    errors, `gravity`/`snap` on a gamepad axis are accepted and ignored. `press_point` must be in
    (0, 1]. Output keeps fields in reading order. Unity's "joystick N button M" (one joystick) is
    rejected with a hint to use the binding's `gamepad`.
11. **project.yaml.** Actions equal to the defaults are stored as "not customized" (no node). An
    `input` node that cannot be read is reported in the Console, the defaults are used, and the
    node is written back verbatim until the actions are edited (a hand-edit typo loses nothing).
12. **Exports.** The generated line splits the JSON into adjacent raw literals of 8000 bytes
    (MSVC's limit) and `JasperEngine.h` now includes `input/InputActions.h` and
    `input/InputControlNames.h`, so the generated `main.cpp` (and C++ scripts) see them.
13. **Play start resets too** (`Project::resetPlayInput`: `Input::reset` and
    `InputActions::resetRuntimeState`), not only Play stop, so a session never inherits held
    input, overrides, disabled actions or simulated sources.
14. **Small choices.** `getPressedControl` names a trigger without a sign (`"left_trigger"`);
    `inputString` keeps `\b` and `\n` (`\r` becomes `\n`) and drops other control characters;
    the mouse-movement reference resets on mouse enter and when a finger moves the simulated
    mouse; `Input.getTouch(i)` returns nil out of range (Unity throws); Listen in the editor takes
    keys, gamepad buttons (ImGui's or the playing game's), sticks/triggers for a gamepad axis
    binding, and mouse buttons clicked on empty space (a click on the dialog is not a binding).

Verification: editor + tests build with 0 warnings; `jasperengine-editor-tests` 981 passed,
8 skipped (the known ones), 0 failed; MCP `test_input.py`, `test_input_ui.py` (acceptance 6:
focused text field and gamepad navigation), `test_stubs.py`, `test_docs.py`, `test_protocol.py`,
`test_play.py`, `test_project.py`, `test_html_ui_input.py`, `test_html_ui_gamepad.py` pass;
`test_build.py` checks the exported `main.cpp` (the real Desktop player run needs
`JASPER_SLOW_BUILD=1`). The changed engine sources compile with Emscripten. Not verified here:
Apple builds (Xcode project updated with `tools/xcode_sync_engine.py`) and Android (no NDK on
this host); no platform code changed.
