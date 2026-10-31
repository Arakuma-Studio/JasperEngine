# UI multi-touch: per-pointer events in both UI systems, DOM Pointer Events, on-screen controls

Feature `ui-multitouch` (ledger #5), branch `gaps/ui-multitouch`. Status: specced 2026-10-31.
Binding conventions: [AGENT-BRIEF.md](../../engine-gaps/AGENT-BRIEF.md). Gap: [GAP-REPORT.md](../../engine-gaps/GAP-REPORT.md)
§8 (UI) and §3 (Input). Depends on feature `input` ([spec](2026-10-31-input-design.md)) for virtual sources.

## 1. Goals

1. Every pointer (mouse, each finger) is tracked independently through the native UI (RectTransform/Image/Button...)
   and the HTML/CSS UI: its own hover, press, capture, drag and click. Two fingers press two buttons; lifting one
   releases only its button.
2. Native UI gets Unity EventSystem semantics: a `PointerEventData` (pointerId, position, delta, pressPosition,
   button, clickCount, dragging...) and per-object script callbacks `onPointerDown/Up/Click/Enter/Exit`,
   `onInitializePotentialDrag`, `onBeginDrag/onDrag/onEndDrag`, `onDrop`, `onScroll`, dispatched up the hierarchy
   like `ExecuteEvents.ExecuteHierarchy`.
3. HTML UI gets DOM Pointer Events implemented in the RmlUi fork: `pointerdown/move/up/cancel/over/out/enter/
   leave`, `gotpointercapture/lostpointercapture`, `pointerId`, `pointerType`, `isPrimary`, `setPointerCapture`,
   `releasePointerCapture`, `hasPointerCapture`, CSS `touch-action`, per-pointer `:active`/`:hover`/drag.
4. On-screen controls feeding `input` actions, in both UI systems: **On-Screen Stick** (Fixed / Floating / Dynamic
   like Joystick Pack, handle range, dead zone, axis options, snap, move threshold), **On-Screen Button**, and
   **Touch Area** (drag delta for camera look) that works at the same time as the stick on another finger (the
   game's B/D/E/G scenes).
5. `Input.isPointerOverUI([pointerId])` (Unity `EventSystem.IsPointerOverGameObject`).
6. Agents can build these controls (`create_ui`, `add_component`, HTML elements) and test them with multi-finger
   `send_input`.

## 2. Non-goals

- DOM Touch Events (`touchstart`, `TouchList`): Pointer Events cover them; `touch*` listeners keep warning, now
  suggesting `pointer*`.
- Gestures (pinch, rotate) as built-ins: scripts compute them from two pointers (documented recipe).
- Pen pressure/tilt (`pressure` is 0.5 while pressed, 0 otherwise, as the DOM default for devices without it).
- Raycasting 3D world objects through the EventSystem (Unity's PhysicsRaycaster): `Physics.raycast` exists.
- Native UI keyboard/gamepad navigation changes.

## 3. Users and stories

- Porting the game: a Floating stick on the left half and a Touch Area on the right; the left thumb moves
  (`Input.getAxis("Horizontal/Vertical")`), the right thumb looks (`Input.getAxis("Mouse X/Y")`); a Fire button on
  the right works while both thumbs are down (Invaders).
- Inventory drag and drop (Garden) with `onBeginDrag/onDrag/onEndDrag/onDrop` on slot scripts.
- HTML HUD author: `<onscreen-stick mode="floating">` and `<onscreen-button action="Jump">Jump</onscreen-button>`,
  styled with CSS; a custom slider with `pointerdown` + `setPointerCapture`.
- Agent: `create_ui {type:"onscreen_stick", ...}`, then `send_input` two touches (`pointer` 0 on the stick, 1 on the
  area) and asserts `Input.getAxis` values with `eval_lua`.

## 4. Research summary

| Source | Take | Leave (why) |
|---|---|---|
| Unity EventSystem: `PointerEventData` <https://docs.unity3d.com/Packages/com.unity.ugui@1.0/api/UnityEngine.EventSystems.PointerEventData.html>, supported events <https://docs.unity3d.com/Packages/com.unity.ugui@1.0/manual/SupportedEvents.html> | Field names; pointerId -1/-2/-3 for mouse buttons, fingerId for touches; ExecuteHierarchy (first ancestor with a handler receives); pointerPress gets up and click; drag threshold; `Use()` | Selection/navigation events, Move/Submit/Cancel events (focus system is separate) |
| W3C Pointer Events 3 <https://www.w3.org/TR/pointerevents3/>, MDN `touch-action` <https://developer.mozilla.org/en-US/docs/Web/CSS/touch-action> | Event set, implicit capture of touch pointers, explicit capture API, `isPrimary`, `pointercancel` when a pan starts, `touch-action` values | `-1` reserved id semantics beyond "not a pointer"; `getCoalescedEvents` |
| Unity Input System on-screen controls <https://docs.unity3d.com/Packages/com.unity.inputsystem@1.7/manual/OnScreen.html> | Controls write into a virtual device read by actions; button 1 on down, 0 on up; stick movement range | Control paths (`<Gamepad>/leftStick`): we name actions instead (simpler, same effect) |
| Joystick Pack (game: `Assets/Joystick Pack/Scripts`) | The exact Fixed/Floating/Dynamic maths: input = (pointer - centre)/radius, dead zone, normalise past 1, axis options, 22.5°/67.5° snap, handle = input·radius·handleRange, Dynamic background follow past `moveThreshold` | `Joystick.Instance` singleton, canvas.scaleFactor maths (our stick works in UI units) |
| Godot `TouchScreenButton` <https://docs.godotengine.org/en/stable/classes/class_touchscreenbutton.html>, Virtual Joystick add-on <https://github.com/MarcoFazioRandom/Virtual-Joystick-Godot> | `passby_press` (press when a finger slides on), visibility "touchscreen only", clamp zone | Its own action quartet per direction (we drive two axes) |
| The game's `TouchArea.cs` | Press records pointerId; delta per frame | It indexes `Input.touches[pointerId]` by finger id, a bug; ours reads the finger by id |

## 5. Engine today

- Native UI: one pointer. `UISystem` keeps `lastUIFromPointer`, `lastUIFromPointerHover`, `pointerDragging`,
  `lastPointerDownPos` as singletons (`UISystem.h:31-43`); `eventOnPointerDown/Up/Move(x, y)` take no id
  (`UISystem.h:155-157`); a second finger's down replaces the first's target (`UISystem.cpp:2866-2873`); **every**
  live UI element gets `onPointerUp` and every pressed Button is released on any finger's up
  (`UISystem.cpp:2955-3017`). Hit test: the last `ImageComponent` in component order containing the point, not
  `ignoreEvents` (`UISystem.cpp:2890-2905`). Callbacks `FunctionSubscribe<void(float,float)>` with element-local
  x, y (`UIComponent.h:67-78`), bound at `ECSClassesLua.cpp:398-410`. Engine dispatch: `Engine.cpp:1888-2110`
  (touches call `eventOnPointerDown(x, y)` without id, `:1905`; mouse only for the left button, `:2041-2043`).
- HTML UI: `UIDocumentSystem` already passes the pointer id and keeps per-pointer capture sets
  (`UIDocumentSystemInput.cpp:29-35,243-345`), but RmlUi's `Context` has one hover chain, one active chain, one
  drag: every touch is "mouse button 0" (`engine/libs/rmlui/Source/Core/Context.cpp:904-1060`), so lifting any
  finger clears `:active` everywhere (`HtmlUIContextInput.cpp:76-77`, `docs/mcp/guide/html-ui.md:1329-1333`) and
  only the last pressed element can be clicked. `pointer*`/`touch*` listeners warn (`html-ui.md:628-629`);
  `touch-action` is accepted and ignored (`engine/core/htmlui/compat/tables/PropertyTable.cpp:787`). `UIEvent` has
  `clientX/Y`, `button` only (`engine/core/object/ui/UIEvent.h:40`).
- MCP `send_input` already sends multi-finger touches with `pointer` ids (`PlayTools.cpp:2671-2765`),
  `test_html_ui_input.py::test_two_touch_pointers` covers two `:active` targets.

## 6. Product decisions

- **D1. Pointer ids.** Engine-wide: touches use `Touch.fingerId` (>= 0, the platform id); the mouse uses
  `-1` (left), `-2` (right), `-3` (middle) in the **native UI** `PointerEventData.pointerId` (Unity). In the **HTML
  UI**, DOM rules: the mouse is one pointer `pointerId = 1` for all buttons, a finger is `fingerId + 2`;
  `event.fingerId` (engine extension) gives the Input finger id (-1 for the mouse) so scripts need no arithmetic.
- **D2. Native UI multi-pointer state.** `UISystem` keeps a `std::map<int, UIPointerState>` keyed by pointerId,
  created on down (touch) or first move (mouse), removed on up/cancel (touch). Built-in widgets keep per-pointer
  ownership: a Button is `pressed` while its set of pressing pointers is non-empty (`onPress` on the first,
  `onRelease` on the last); a Panel move/resize, a Scrollbar drag and an InputField selection belong to the pointer
  that started them and ignore others.
- **D3. Legacy callbacks keep their signatures** (`onPointerDown(x, y)` etc., Lua and C++) and fire per pointer
  with element-local coordinates, but only on the element that pointer pressed (bug fix: no more `onPointerUp` on
  every element). `onPointerEnter/Leave` (legacy) and `Button.hovered` fire on the first pointer entering / the last
  leaving. During a legacy callback `UIComponent:getPointerEventData()` returns the current `PointerEventData`.
- **D4. New per-object script callbacks** (brief convention, Unity message names in lowerCamelCase):
  `onPointerDown(e)`, `onPointerUp(e)`, `onPointerClick(e)`, `onPointerEnter(e)`, `onPointerExit(e)`,
  `onInitializePotentialDrag(e)`, `onBeginDrag(e)`, `onDrag(e)`, `onEndDrag(e)`, `onDrop(e)`, `onScroll(e)`.
  Dispatch = `ExecuteHierarchy`: from the hit element up to the root, the first GameObject with a Lua script
  defining that method (or a built-in handler component, §7.4) receives it. **Collision rule**: if a script
  instance subscribed a method of the same name to a `UIComponent` event with `RegisterEvent` (the old pattern,
  `lua-api.md:165`), the engine does not also call it with `e` for that GameObject. `LuaBinding::luaRegisterEventImpl`
  (`LuaBinding.cpp:386`) records `(instance, methodName)` when the event object is a UIComponent pointer event.
- **D5. Unity sequence rules.** Down: `onPointerEnter` first for a touch (it has no hover), then `onPointerDown`;
  the down handler becomes `pointerPress` (if none, the first ancestor with `onPointerClick`); the drag handler
  (first ancestor with `onDrag`/`onBeginDrag`) gets `onInitializePotentialDrag`. Move: enter/exit per pointer;
  past the drag threshold (`UI_DRAG_START_DISTANCE`, kept; `e.useDragThreshold=false` starts at once) →
  `onBeginDrag`, then `onDrag` per move. Up: `onPointerUp` to `pointerPress`; `onPointerClick` if the click handler
  under the release equals `pointerPress` and `eligibleForClick` (not cancelled, no drag started); `onDrop` to the
  drop handler under the pointer if dragging; then `onEndDrag`; a touch then gets `onPointerExit`. `clickCount`
  counts clicks of that pointer on the same element within 0.3 s (the legacy `onDoubleClick` stays).
- **D6. Cancel** (touch cancel, focus loss/app pause, the pressed element disabled/destroyed, scene unload, a
  covering loading scene): `onPointerUp` with `e.canceled = true`, `onEndDrag` if dragging, no click, no drop; a
  Button releases without `onRelease`'s click semantics changing (onRelease still fires, as today on cancel).
- **D7. Right/middle mouse buttons** reach the new script callbacks (`e.button`); built-in widgets (Button, Panel,
  Scrollbar, InputField) and the legacy callbacks keep reacting to the left button only (today's behaviour, Unity's
  Button too).
- **D8. HTML compat mouse events stay for every pointer** (`mousedown/up/move/over/out`, `click`, `dblclick`): the
  engine's docs told authors to use `mousedown` for touch, so a second finger keeps firing them (DOM fires
  compatibility events for the primary pointer only; documented deviation). Each event carries `pointerId`.
  `click` fires per pointer (modern DOM: `click` is a PointerEvent).
- **D9. Hover rules (HTML).** `:hover` is the union over pointers: the mouse always, a finger while it is down
  (cleared on its up: no sticky hover on touch). `:active` is reference-counted per element: an element is active
  while any pointer presses it or a descendant.
- **D10. Implicit capture (HTML)**: a touch pointer is captured by its `pointerdown` target until up/cancel (DOM);
  the mouse is not, unless `setPointerCapture`. Capture redirects `pointermove/up/cancel` (and compat mouse events)
  to the capturing element; `:hover` follows the real hit.
- **D11. touch-action (HTML)**: `auto | none | pan-x | pan-y | pan-x pan-y | manipulation` (non-inherited).
  Effective value = intersection of the values from the target up to (including) the nearest scroll container.
  When a finger moves past the touch slop (`TOUCH_CLICK_MAX_DISTANCE` × dp ratio) in an allowed pan direction, the
  scroll container pans, the pointer gets `pointercancel` (DOM) and loses `:active`; with `none` (or a disallowed
  direction) no pan happens and pointer events continue. On-screen control elements default to `none` (UA style).
- **D12. On-screen controls are input sources, not scripts.** They write into `input` actions through
  `InputActions::setSourceValue/addSourceDelta` keyed by their component/element, and clear them on release,
  disable, destroy and scene unload. Default actions: stick `Horizontal`/`Vertical`, button `Fire1`, touch area
  `Mouse X`/`Mouse Y` (so mouse-look scripts work unchanged on touch).
- **D13. Stick maths = Joystick Pack**, in UI units (the canvas scaler, feature ui-scaling-text, already scales
  them, so no `scaleFactor` term). Value y is up-positive.
- **D14. First pointer wins.** A stick, button press set, or touch area owned by a pointer ignores other pointers
  that start inside it while held (a button accepts several, D2). Topmost element first: a stick above a
  full-screen touch area takes touches on it.
- **D15. `Input.isPointerOverUI(id)`**: true when that pointer (or, without argument, the mouse or any finger) is
  over a native UI element that receives events, over HTML content, or captured by either. Unity name.

## 7. Design: runtime C++

### 7.1 Native UI EventSystem (engine/core/subsystem/UISystem.*)

```cpp
struct UIPointerState {
    int pointerId; bool touch; int button;            // D_MOUSE_BUTTON_* (touch: left)
    Vector2 position, delta, pressPosition;           // UI units of this scene (ui-scaling-text converts)
    Vector2 canvasPosition, canvasDelta, canvasPressPosition;   // Input/canvas px
    double pressTime = 0, clickTime = 0; int clickCount = 0;
    Entity hit = NULL_ENTITY;                         // current raycast target
    std::vector<Entity> enterChain;                   // pointerEnter path (exit/enter diffs)
    Entity rawPress = NULL_ENTITY, press = NULL_ENTITY, drag = NULL_ENTITY, lastClick = NULL_ENTITY;
    bool dragging = false, eligibleForClick = false, useDragThreshold = true, internalGesture = false;
    PanelEdge panelEdge; Entity panelHeader, scrollbar, inputFieldSelecting;   // built-in gestures
    Vector2 panelSizeAcc;
};
```

- Entry points become `eventOnPointerDown(int pointerId, int button, float x, float y, bool touch)`,
  `eventOnPointerUp(...)`, `eventOnPointerMove(int pointerId, float x, float y, bool touch)`,
  `eventOnPointerCancel(int pointerId)`, `eventOnScroll(float dx, float dy)` (mouse pointer target), all returning
  "consumed". The old signatures stay as inline wrappers (`pointerId` -1, left, not touch) for C++ callers.
- Hit test unchanged (topmost Image, `ignoreEvents`, panel fit), extracted to `Entity raycast(Vector2)`.
- `Engine.cpp` passes ids: touches `pointer`; mouse `-1 - button` with the button; mouse moves `-1`, also for
  buttons 2/3 held (one mouse position). Consumption per pointer: `Engine` keeps `std::set<int> uiCapturedPointers`
  (a pointer whose down went to UI keeps its moves/up consumed), replacing the single `uiEventReceived` decision
  for moves/ups.
- `onComponentRemoved` (`UISystem.cpp:3364`) and entity deactivation clear the entity from every pointer state
  (D6 cancel callbacks are sent before the entity goes, when it is still alive: deactivation; for destruction only
  the state is dropped).
- Script dispatch uses feature `script-lifecycle`'s message path: new `ScriptMessage` values `OnPointerDown,
  OnPointerUp, OnPointerClick, OnPointerEnter, OnPointerExit, OnInitializePotentialDrag, OnBeginDrag, OnDrag,
  OnEndDrag, OnDrop, OnScroll` (names as D4), sent with `ScriptRuntime::sendMessage(scene, handler, msg, args,
  DeliverRequireActive | DeliverRequireEnabled)` where `args` carries the `PointerEventData` (Lua: a userdata via
  `ScriptArg::pushLua`; C++: `cppPayload = const PointerEventData*`, virtual methods on the C++ script base). The
  handler is found by a new `ScriptRuntime::findHandler(scene, entity, msg)` (walks parents; first entity with an
  enabled script entry defining the method and not excluded by D4's collision rule). Until script-lifecycle lands,
  the fallback is `LuaBinding::callLuaScriptMethod` (`LuaBinding.h:131`, the animation-event path).

### 7.2 PointerEventData (C++ value type `jasperengine::PointerEventData`, Lua class `PointerEventData`)

| Field | Meaning |
|---|---|
| `pointerId` | D1 (native: -1/-2/-3 mouse, fingerId touch) |
| `button` | `Input.MOUSE_BUTTON_LEFT/RIGHT/MIDDLE` |
| `isTouch` | bool |
| `position`, `delta`, `pressPosition` | Vector2, canvas px (same space as `Input.getMousePosition`, `Touch.position`) |
| `localPosition` | Vector2, receiving element's local UI units (what legacy x, y were) |
| `uiPosition`, `uiDelta` | Vector2, the scene's UI units (differs from canvas px when a canvas scaler is set) |
| `clickCount`, `clickTime` | int, seconds (unscaled system time) |
| `dragging`, `eligibleForClick`, `canceled` | bool |
| `useDragThreshold` | rw, meaningful in `onInitializePotentialDrag` |
| `pointerEnter`, `pointerPress`, `pointerDrag`, `rawPointerPress`, `target` | `EntityHandle` or nil |
| `used`, `use()` | Unity `BaseEventData.Use()`: a used down event stops built-in widget handling of that press |

### 7.3 HTML UI: RmlUi fork patches (docs/html-ui/FORK.md, new rows G5-1..G5-6)

- **G5-1 multi-pointer Context.** `Context` keeps `SmallUnorderedMap<PointerId, PointerState>` (`PointerState {
  type (mouse/touch/pen), primary, position, buttons, hover chain, active chain, capture, press target, drag state
  (drag, drag_started, drag_verbose, drag_clone, drag_hover chain), touch scroll state (moved from TouchState),
  click count/time }`). New API: `ProcessPointerDown(const PointerInput&)`, `ProcessPointerMove`,
  `ProcessPointerUp`, `ProcessPointerCancel(PointerId)`, `ProcessPointerLeave(PointerId)` with `PointerInput {id,
  type, position, button, modifiers, pressure, width, height}`. `ProcessMouse*` become wrappers for pointer 1;
  `ProcessTouch*` wrappers for touch pointers. `GetHoverElement()` returns the mouse's (compat); new
  `GetHoverElement(PointerId)`. Pseudo-classes: `:hover`/`:active` reference-counted (D9).
- **G5-2 Pointer Events.** New `EventId`s `Pointerdown, Pointermove, Pointerup, Pointercancel, Pointerover,
  Pointerout, Pointerenter, Pointerleave, Gotpointercapture, Lostpointercapture` (+ `Mouseenter, Mouseleave`
  non-bubbling compat). Parameters: `pointer_id, pointer_type ("mouse"|"touch"|"pen"), is_primary, button,
  buttons, mouse_x, mouse_y, pressure, width, height` plus the key modifiers. Order per pointer (DOM):
  `pointerover → pointerenter → (mouseover)` on entering, `pointerdown → mousedown`, `pointermove → mousemove`,
  `pointerup → mouseup → click`, `pointercancel` (no compat event), `pointerout → pointerleave` on leaving (touch:
  after up). `preventDefault()` on `pointerdown` suppresses the compat `mousedown` and focus change (DOM).
- **G5-3 Pointer capture.** `Element::SetPointerCapture(PointerId)`, `ReleasePointerCapture`, `HasPointerCapture`;
  implicit capture of touch pointers (D10); capture released on up/cancel/element removal with
  `lostpointercapture`; `gotpointercapture` before the next pointer event.
- **G5-4 touch-action.** New property `touch-action` (non-inherited, values D11) in the style sheet specification;
  the touch scroll decision in the pointer path uses it.
- **G5-5 per-pointer drag.** `drag="drag|drag-drop|clone"` elements: each pointer drags its own element; `dragdrop`,
  `dragover`, `dragout` use that pointer's hover; drag events carry `pointer_id`.
- **G5-6 per-pointer click.** A click fires for a pointer released over its press target (or a descendant); two
  fingers click two buttons; double click per pointer.
- RmlUi unit tests in `engine/libs/rmlui/Tests/Source/UnitTests/PointerEvents.cpp` (new) for each row.

Engine side:
- `HtmlUIContext` (`HtmlUIContextInput.cpp`): `processPointerDown/Move/Up/Cancel(id, type, ...)` replacing the touch
  helpers (the `activeElement` workaround goes away); `UIDocumentSystem::eventOnPointer*` call them with the D1
  mapping (mouse pointer 1 with `button`; touch `fingerId + 2`); `dispatchWorldPointer` (texture-mode documents,
  `UIDocumentSystem.h:207`) passes the same ids.
- `UIEvent` (`UIEvent.h`) getters: `getPointerId`, `getPointerType`, `isPrimary`, `getButtons`, `getPressure`,
  `getWidth`, `getHeight`, `getFingerId`. `UIElement`: `setPointerCapture(id)`, `releasePointerCapture(id)`,
  `hasPointerCapture(id)`.
- Compat layer: `pointer*`, `gotpointercapture`, `lostpointercapture`, `mouseenter`, `mouseleave` become supported
  event names (the warning list in the Lua binding and `UIEventType` stub enum); `touch-action` row in
  `PropertyTable.cpp:787` becomes supported ("pointerevents-3"); `touch*` warning text suggests `pointer*`.

### 7.4 On-screen controls, native UI (new engine/core/component/OnScreenControlComponents.h)

```cpp
enum class OnScreenStickMode { Fixed, Floating, Dynamic };
enum class OnScreenStickAxes { Both, Horizontal, Vertical };
enum class OnScreenVisibility { Always, TouchDevicesOnly };
struct OnScreenStickComponent {
    bool enabled = true;
    OnScreenStickMode mode = OnScreenStickMode::Fixed;
    Entity background = NULL_ENTITY, handle = NULL_ENTITY;   // child Images; area = this element's rect
    float handleRange = 1.0f, deadZone = 0.0f, moveThreshold = 1.0f;   // radius units (pack defaults)
    OnScreenStickAxes axes = OnScreenStickAxes::Both;
    bool snapX = false, snapY = false, hideWhenReleased = true, clampToArea = true;
    std::string horizontalAction = "Horizontal", verticalAction = "Vertical";
    OnScreenVisibility visibility = OnScreenVisibility::Always;
    // runtime
    Vector2 value; bool pressed = false; int pointerId = INT_MIN; Vector2 backgroundRest;
    FunctionSubscribe<void(Vector2)> onValueChanged; FunctionSubscribe<void()> onPress, onRelease;
};
struct OnScreenButtonComponent {
    bool enabled = true; std::string action = "Fire1"; float value = 1.0f;   // e.g. -1 on "Horizontal"
    bool pressWhenPointerEnters = false, releaseWhenPointerLeaves = false;
    OnScreenVisibility visibility = OnScreenVisibility::Always;
    bool pressed = false; std::vector<int> pointers;                        // runtime
    FunctionSubscribe<void()> onPress, onRelease;
};
struct TouchAreaComponent {
    bool enabled = true; std::string horizontalAction = "Mouse X", verticalAction = "Mouse Y";
    float sensitivity = 0.1f;                         // = default Mouse X/Y sensitivity: touch and mouse feel alike
    bool invertX = false, invertY = false;
    Vector2 delta; bool pressed = false; int pointerId = INT_MIN;           // runtime: delta canvas px this frame, y down
    FunctionSubscribe<void(Vector2)> onDrag;
};
```

Behaviour (`UISystem`, handlers registered as built-in ExecuteHierarchy receivers for down/drag/up):
- **Stick.** Area = the stick element's rect (needs an Image to be hit; the menu creates a transparent one). Down
  (a free stick): Floating/Dynamic move the background centre to the pointer (clamped so the background stays
  inside the area when `clampToArea`), show it. Each down/move: `input = (pointer - centre) / r` with `r =
  background width / 2` in UI units; axes option zeroes the other axis; `m = |input|`: `m <= deadZone` → 0, `m > 1` →
  normalised; snap per Joystick Pack (`Joystick.cs:91-123`: horizontal 0 when the angle to up is < 22.5° or >
  157.5°, vertical 0 between 67.5° and 112.5°, else ±1; single-axis mode snaps to sign); Dynamic: if `m >
  moveThreshold` the centre moves by `normalised · (m - moveThreshold) · r`. Handle local position = `input · r ·
  handleRange`. Value y = -input.y (up positive). Writes `value.x/value.y` as bounded sources; `onValueChanged`
  when it changes. Up/cancel: value 0, sources cleared, handle centred, Floating/Dynamic hide the background
  (`hideWhenReleased`) and return it to `backgroundRest`.
- **Button.** Down adds the pointer, first pointer → pressed, source `value`; `pressWhenPointerEnters`: a finger
  sliding in while down presses it; `releaseWhenPointerLeaves`: leaving releases. Last pointer up/cancel → source
  0, `onRelease`. With a `ButtonComponent` on the same entity its visuals follow `pressed` (per-pointer set, D2).
- **Touch Area.** Down (free) takes the pointer; each frame (`UISystem::update`, after `Input::beginFrame`) `delta`
  = that pointer's position change this frame in canvas px (`Touch.deltaPosition` for fingers, `Input.mouseDelta`
  for the mouse); writes `delta.x·sensitivity` to `horizontalAction` and `-delta.y·sensitivity` to
  `verticalAction` as delta sources (× -1 when inverted); `onDrag(delta)` when non-zero. Up/cancel releases.
- `visibility = TouchDevicesOnly`: hidden and non-interactive when `System.isTouchDevice()` is false (the editor's
  Simulate Touch counts as touch).
- Disable/destroy/scene unload: `InputActions::clearSource(owner)` (no stuck axis).

### 7.5 On-screen controls, HTML UI (engine/core/htmlui/controls/OnScreenControls.cpp)

Custom elements registered with an `Rml::ElementInstancer` and known to the compat layer (no unknown-element
diagnostic):

```html
<onscreen-stick mode="floating" handle-range="1" dead-zone="0.1" axes="both" snap-x snap-y move-threshold="1"
                horizontal-action="Horizontal" vertical-action="Vertical" visibility="always"></onscreen-stick>
<onscreen-button action="Jump" value="1" press-on-enter release-on-leave>Jump</onscreen-button>
<touch-area horizontal-action="Mouse X" vertical-action="Mouse Y" sensitivity="0.1" invert-y></touch-area>
```

- Same maths as 7.4 in document px. A stick without children gets `<div class="stick-base"><div
  class="stick-knob"></div></div>` (authors may supply their own children with those classes). The element positions
  `.stick-base` (Floating/Dynamic) and `.stick-knob` with `transform: translate(...)` each update and sets
  `data-pressed` / `data-x` / `data-y` attributes; `:active` while held.
- UA styles: `onscreen-stick, onscreen-button, touch-area { display: block; touch-action: none; }`, a 160 px
  translucent circle base and 64 px knob (overridable).
- Events (bubble, `detail`): `stickstart`, `stickmove` (`{x, y}`), `stickend`; buttons fire normal pointer/click
  events; `touch-area` fires `areadrag` (`{dx, dy}` document px).
- They use G5 pointer events with implicit capture; sources keyed by the element, cleared on release, element
  removal, `display: none`, document hide/unload.

### 7.6 Serialization, catalog, exports

- New components in `editor/Stream.cpp` (encode/decode next to `encodeAudioSourceComponent`, `:7678`), `Catalog.cpp`
  property tables (so `set_properties` works), `Factory.cpp` code generation (exported scenes are generated C++,
  `Factory.cpp:2108` pattern), component add menu (UI section), `Generator` registration like the other UI
  components. Entity references (`background`, `handle`) serialized like `ButtonComponent.label`.
- Runtime state is never serialized. Old scenes have none of these components: nothing changes.

## 8. Lua API

- `PointerEventData` class (7.2). Script callbacks D4 (`function Slot:onDrop(e) ... end`).
- `UIComponent:getPointerEventData()` (valid during a legacy callback, nil otherwise).
- Wrappers: `OnScreenStick(scene, entity)` (rw fields of 7.4 by name, read-only `value`, `pressed`, `pointerId`;
  events `onValueChanged`, `onPress`, `onRelease`), `OnScreenButton(scene, entity)`, `TouchArea(scene, entity)`
  (read-only `delta`, `pressed`); `GameObject:getComponent`-style accessors follow the existing wrapper pattern
  (`OnScreenStick(self.scene, self.entity)`); `EntityHandle` type names for `"entity"` script properties.
- `Input.isPointerOverUI([pointerId])` (D15; pointerId per D1 native numbering: -1 mouse, fingerId).
- HTML: `event.pointerId`, `event.pointerType`, `event.isPrimary`, `event.buttons`, `event.pressure`,
  `event.width`, `event.height`, `event.fingerId`; `el:setPointerCapture(id)`, `el:releasePointerCapture(id)`,
  `el:hasPointerCapture(id)`.

## 9. Editor UX

- GameObject > UI menu (and the Hierarchy "+" menu): **On-Screen Stick** (submenu Fixed / Floating / Dynamic),
  **On-Screen Button**, **Touch Area**. The stick creates root (RectTransform 300×300 anchored bottom-left, transparent
  Image, OnScreenStick), "Background" (Image 160×160 circle) and "Handle" (Image 64×64 circle); circle textures are
  written once to `assets/ui/onscreen/stick_base.png`, `stick_knob.png` (generated, not bundled binaries). The
  Touch Area creates a right-half stretch rect with a transparent Image.
- Inspector sections with Unity/Joystick Pack labels (Mode, Handle Range, Dead Zone, Axis Options, Snap X/Y, Move
  Threshold, Horizontal/Vertical Action as dropdowns of the project's actions with free text allowed), live value
  while playing. The Scene view draws the stick's handle-range circle and the dead-zone circle as gizmos.
- Game view, "Simulate touch" on: the mouse is finger 0 (today). New: **Alt + left press pins finger 0** where it
  is (it stays down while Alt is held) and the next press-drag is finger 1, so a human can hold the stick and look
  with one mouse. Releasing Alt lifts the pinned finger. Documented in the Game view tooltip and input.md.

## 10. MCP / agent surfaces

- `create_ui` types `onscreen_stick` (`mode`, `actions` [h, v]), `onscreen_button` (`action`, `value`, text),
  `touch_area` (`actions`), with the default children (WorldTools.cpp:4841).
- `add_component` / `set_properties` with the catalog names; `list_component_types` lists them.
- `send_input` unchanged (multi-touch exists); docs gain the recipe: two `touch start` with pointers 0 and 1, moves
  over frames, `eval_lua` reading `Input.getAxis`. `inspect_ui`/`assert_ui` (HTML) see `data-pressed`, `data-x`.
- Guide: `docs/mcp/guide/input.md` (on-screen controls section) and html-ui.md updates; one line in
  `McpInstructions.cpp` ("Touch controls: create_ui onscreen_stick/onscreen_button/touch_area feed Input actions").

## 11. Per-platform notes

Finger ids come from the platforms as today (Android pointer ids from 0, iOS slots from 1, Web identifiers; Linux
desktops have no multi-touch: the Game view's Alt finger and MCP cover testing). Windows touch (`WinInputRouter.cpp:339`)
passes ids already. Nothing platform-specific is added.

## 12. Behaviour rules and edge cases

- Ordering: within a frame events are processed in arrival order; each pointer's sequence is independent. HTML
  documents first, then native UI, then world documents, then the game (unchanged), but per pointer: a finger
  captured by a document never reaches the native UI and vice versa.
- Two fingers down on the same Button: pressed until both are up; one `onPress`, one `onRelease`; each finger can
  produce its own `onClick`/`onPointerClick`.
- A finger pressed on a stick that slides over a Button: the button is not pressed unless
  `pressWhenPointerEnters`; the stick keeps the pointer.
- Element disabled or deactivated while pressed: D6 cancel, sources cleared, `pressed` false.
- Scene unload: every pointer state of that scene is dropped (no callbacks into a dying scene), sources cleared.
- Time scale 0: controls and events work (input is unscaled); `onDrag` deltas are per frame.
- App pause / focus loss: the platform cancels touches; `Input::releaseAll` cancels mouse presses → D6.
- Mouse right button dragging over UI: script callbacks get `button` right; built-in widgets ignore it.
- HTML: `pointercancel` on pan start; a finger on `touch-action: none` content never scrolls its container.
- `isPointerOverUI` during a frame: reflects the last processed event of that pointer.

## 13. Performance (mobile)

Per pointer one raycast per event (unchanged cost per event); hover chains small; pointer maps ≤ 10 entries.
On-screen controls: O(1) per event, a transform write per frame while held. HTML: per-pointer chains reuse RmlUi's
ElementSet; `:hover` refcounts avoid style recomputation churn (only transitions 0↔1 change pseudo-classes).

## 14. Tests

Editor unit tests (`editor/tests/UIMultiPointerTest.cpp`, scenes built in code like the existing UI tests):
- Two pointers press two Buttons; release pointer 0 → only button 0 `onRelease`; legacy `onPointerUp` fires only on
  the pressed element (regression of the every-element bug).
- Same Button, two fingers: one onPress/onRelease, two clicks.
- ExecuteHierarchy: handler on the parent receives `onPointerDown` from a child Image; collision rule (RegisterEvent
  method not double-called).
- Drag sequence: InitializePotentialDrag, BeginDrag after threshold, Drag, Drop on the slot under the pointer,
  EndDrag; no click after drag; `useDragThreshold=false`.
- Cancel paths: touch cancel, entity deactivated, scene unloaded.
- Stick maths table-driven against Joystick Pack formulas (dead zone, normalisation, snap angles 22.5/67.5,
  Dynamic follow, axis options, handle position), Floating show/hide/rest.
- Touch Area delta = finger delta × sensitivity into `Mouse X`, `-dy` into `Mouse Y`; coexistence with the stick on
  another finger (both actions non-zero in one frame).
- `isPointerOverUI` per pointer.
RmlUi unit tests (G5 rows): per-pointer `:active`/`:hover`, implicit capture, explicit capture events order,
`pointercancel` on pan with `touch-action: auto`, none with `none`, `pan-y` intersection, per-pointer drag, per-pointer
click, compat mouse events order, `preventDefault` on pointerdown.
HTML UI engine tests (`engine/tests/htmlui/TestInput.cpp`, `TestInputDocument.cpp`): `UIEvent` getters, D1 id mapping,
fingerId; `<onscreen-stick>` value and `data-x` after synthetic pointers; `<onscreen-button>` drives
`Input.getButton`; `<touch-area>` delta; element removal clears sources.
MCP e2e: extend `tools/mcp/tests/test_html_ui_input.py::test_two_touch_pointers` (lift finger 0 only → target A
loses `:active`, B keeps it; both clicks counted); new `tools/mcp/tests/test_onscreen_controls.py`: native scene with
`create_ui` stick (floating) + touch area + button; send pointer 0 start at the stick area, move 80 px right over 5
frames, pointer 1 start on the area and move 40 px up, pointer 2 tap on the button: `eval_lua` asserts
`Input.getAxis("Horizontal") > 0.9`, `Mouse Y` > 0 in the frame of the move, `getButtonDown("Fire1")` seen by the
probe; same scenario with an HTML document using the custom elements.

## 15. Acceptance criteria

1. The game's control layout (Floating stick + Touch Area + Fire button) works with three simultaneous fingers in
   both UI systems, verified by the MCP tests above.
2. Lifting one finger never releases or un-`:active`s another finger's control (both UIs).
3. `pointer*` events, capture API and `touch-action` work and are documented in `docs/mcp/guide/html-ui.md` (the
   1329-1333 paragraph and the event table at 609-629 rewritten); FORK.md has rows G5-1..G5-6 with tests.
4. Existing tests (`test_html_ui_input.py`, UI tests) pass unchanged except the documented bug fixes.
5. Components serialize, generate code for exports (Linux export plays the stick scene), appear in the add menu,
   Inspector and catalog; stubs list the new classes with `-- unresolved:` empty.
6. `docs/unity-glossary.md` rows: EventSystem interfaces → script callbacks, `PointerEventData`,
   `EventSystem.IsPointerOverGameObject` → `Input.isPointerOverUI`, Input System OnScreenStick/OnScreenButton,
   Joystick Pack Fixed/Floating/Variable(Dynamic) → On-Screen Stick modes.

## 16. Integration points

- `engine/core/subsystem/UISystem.h:31-43,155-160` and `UISystem.cpp:2700-3364` (pointer state, entry points,
  per-pointer gestures), `UIComponent.h:67-78` (legacy events unchanged), `ECSClassesLua.cpp:398-410`.
- `engine/core/Engine.cpp:1888-2110` (ids to UISystem, per-pointer consumption), `Engine.h` (`isPointerOverUI`).
- `engine/core/script/LuaBinding.cpp:386` (collision bookkeeping), `LuaBinding.h:131` (dispatch overload).
- `engine/libs/rmlui/Include/RmlUi/Core/Context.h`, `Source/Core/Context.cpp:872-1060` (G5), `ID.h:425` (EventIds),
  `Element.h` (capture API), style sheet spec (touch-action); `docs/html-ui/FORK.md` rows.
- `engine/core/htmlui/HtmlUIContextInput.cpp:31-96`, `HtmlUIContext.h:88-92`,
  `engine/core/subsystem/UIDocumentSystemInput.cpp:243-345`, `UIDocumentSystem.h:134-137,207`,
  `engine/core/object/ui/UIEvent.h`, `UIElement.h`, `engine/core/script/binding/HtmlUIClassesLua.cpp` (event names,
  getters), `engine/core/htmlui/compat/tables/PropertyTable.cpp:787`.
- New `engine/core/component/OnScreenControlComponents.h`, `engine/core/htmlui/controls/*`.
- Editor: `editor/Stream.cpp`, `Catalog.cpp`, `Factory.cpp`, `window/InspectorWindow.cpp`, component add dialog,
  GameObject > UI menu, `window/PlayInput.cpp:100-130` (Alt finger), `mcp/tools/WorldTools.cpp:3599-4860` (create_ui).

## 17. Cross-feature dependencies

- **input** (provider): virtual source API, `Touch.fingerId`, `Input.isPointerOverUI` lives on the `Input` class
  but is implemented here; on-screen buttons count as pressed through `pressPoint`.
- **ui-scaling-text**: provides the scene's UI-unit ↔ canvas-px conversion (`uiPosition` vs `position`); sticks
  use UI units so they scale with the canvas.
- **safe-area**: on-screen controls anchored with safe-area anchoring (their RectTransforms) stay clear of notches;
  nothing extra here.
- **script-lifecycle**: provides `ScriptRuntime::sendMessage` and the `ScriptMessage` enum this spec extends
  (11 pointer messages + `findHandler`); `onDisable` of a control's GameObject = D6 cancel + source clear.
