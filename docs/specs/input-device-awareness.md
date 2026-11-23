# Input Device Awareness: the current input scheme, live, with prompts and UI that follow it

Builds on S22 (Input Actions, landed) and the HTML UI input work (p4-input, ui-multitouch, landed).
Sibling of `text-entry-gamepad-cursor-back-button.md`, whose gamepad cursor shows only while this
spec's scheme is Gamepad. Paths are relative to the repository root.

## 1. Goal

A game that runs on a phone, a laptop and a console-style pad today cannot tell which of them the
player is holding right now. Touch controls stay on screen while the player uses a keyboard, button
prompts say "Space" to a gamepad player, `@media (hover: hover)` is frozen at load, and nothing
fires when the player puts the phone down and picks up a pad. This spec gives the engine one
**current input scheme** (Keyboard&Mouse, Gamepad, Touch, with the gamepad family for prompts),
switched by what the player last really used, with hysteresis so it never flickers, and makes the
UI, prompts, Lua, the editor and MCP follow it live. It is free when nobody reads it.

## 2. Scope

**In scope**
- A scheme tracker in `engine/core/input`: current scheme, last pointer kind (mouse or touch), last
  used gamepad and its family (Xbox, PlayStation, Nintendo, Generic), switch rules with hysteresis,
  a forced override, one change event per frame.
- Lua: `InputActions.currentScheme`, `currentPointer`, `gamepadFamily`, `lastGamepad`,
  `onSchemeChanged`, `setSchemeOverride`; scheme-filtered display names and glyph keys for actions
  and bindings; Nintendo labels in display names.
- HTML UI: `data-input-scheme` and `data-gamepad-family` on every document's `<html>`, live; a
  `touch-scheme` value for the on-screen controls' `visibility`; `hover`/`pointer` media features
  that follow the last pointer kind; an `<input-prompt>` element that shows an action's binding for
  the current scheme.
- Component UI: a `Touch Scheme` value for On-Screen Stick / Button / Touch Area `Visibility`.
- Editor: Game view toolbar "Input Scheme" menu (Auto or forced), the scheme in the Input Actions
  window's Play header.
- MCP: `get_input_state` reports the scheme, `configure_game_view` forces it; `send_input` switches it
  through the real path (no new tool).
- Platform tagging so fake fingers do not count as touch: Android mouse tool type and the
  emulator "keyboard device that also sends fingers" rule; iOS indirect pointer touches.

**Out of scope (default decisions)**
- Filtering bindings by scheme. Every binding of every enabled map stays live whatever the scheme
  (a local single-player game wants any device to work); the scheme is information, never a gate.
  Unity's per-player `PlayerInput` pairing and split-screen device assignment are not copied.
- Player-rearrangeable on-screen controls (not selected for this round). Nothing here fixes
  control positions or stores a layout, so a later spec can add a layout editor over the same
  elements and components.
- Glyph art. The engine returns stable glyph keys (`gamepad/playstation/cross`); the project
  supplies images. No controller artwork ships (trademark and IP risk).
- Persisting the scheme across launches. Each launch starts from the device (R3).
- Per-player schemes for local multiplayer (one current scheme per process; `lastGamepad` tells
  which pad moved last).
- `any-hover` / `any-pointer` stay what they are (the device's capability, evaluated at load).

## 3. Current state

All read from the code (not run):

- **Schemes exist only as binding groups.** A binding's `groups` are strings ("Keyboard&Mouse",
  "Gamepad", "Touch") (`engine/core/input/InputActionAsset.h:57`); a control path knows its group
  (`InputControlPath::deviceGroup`, `engine/core/input/InputControlPath.h:50-51`); rebinding can
  restrict to groups (`InputActions.h:177-182`). Nothing tracks which group the player is using:
  `InputActions` has no scheme query or event (`engine/core/input/InputActions.h:214-410`).
- **Display names.** `getBindingDisplayName` / `getControlDisplayName` (`InputActions.h:278-283`)
  name a control and switch to PlayStation labels when the pad's name matches
  (`isPlayStation`, `InputActions.cpp:787-794`; labels `:796-812`) for the map's pad or the first
  connected one (`displayPadName`, `:848-855`). There is no Nintendo or Xbox family, no notion of
  "the binding of this action for the current scheme", and no glyph key. Lua binds both functions
  (`engine/core/script/binding/GameplayClassesLua.cpp:250-251`).
- **Touch is a device constant.** `System::isTouchDevice()` (`engine/core/System.cpp:96`, platform
  overrides e.g. `engine/platform/android/JasperEngineAndroid.cpp:144`) is the only signal. In the
  editor, Play acts as a touch device while the Game view's Simulate Touch is on
  (`editor/Project.h:2304`, toggle `editor/window/GameWindow.cpp:1012-1018`, disabled during Play).
- **On-screen controls (HTML).** `visibility="touch-devices-only"` hides an `<onscreen-stick>`,
  `<onscreen-button>` or `<touch-area>` where `isTouchDevice()` is false; evaluated every frame in
  `UIOnScreenControl::OnUpdate` (`engine/core/htmlui/controls/OnScreenControls.cpp:146-162`), so it
  is live but reads a constant; a hidden control releases its pointers (`:157-161`). Documented in
  `docs/mcp/guide/html-ui.md:1636-1638`.
- **On-screen controls (component UI).** `OnScreenVisibility { ALWAYS, TOUCH_DEVICES_ONLY }`
  (`engine/core/component/OnScreenControlComponents.h:27`), applied each frame in
  `UISystem::updateControls` (`engine/core/subsystem/UISystemPointer.cpp:1797-1856`) from
  `isTouchDevice()`; enum stored as `touch_devices_only` (`editor/reflect/Enums.cpp:346-348`),
  Inspector "Touch Devices Only" (`editor/window/InspectorUpstream.cpp:87-101`).
- **Media queries.** `hover`, `any-hover`, `pointer`, `any-pointer` are rewritten to constants at
  compile from `CompatOptions::touchDevice` (`engine/core/htmlui/compat/css/MediaQuery.cpp:626-631`,
  option set at load `engine/core/htmlui/HtmlUIContext.cpp:300`; support table
  `docs/mcp/guide/html-ui-support.md:2197-2198, 2215, 2239`; guide `html-ui.md:1591`: "evaluated
  once at load"). `prefers-color-scheme` and `prefers-reduced-motion` are live through RmlUi themes
  (`HtmlUIContext.cpp:476-483`), but a block may carry only one theme: two themes in one condition
  become inexpressible (`MediaQuery.cpp:724-731`), so a third theme would break
  `(prefers-color-scheme: dark) and (hover: hover)`.
- **Root attributes and restyles.** A compile's selectors are banded by the *names* of the `<html>`
  element's attributes and classes, not their values (`rootSignatureOf`,
  `engine/core/htmlui/compat/DocumentLoader.cpp:110-134`). `updateRuntime` checks every document
  each update and recompiles only when the names change (`compat/runtime/RuntimeRegistry.cpp:162-171`,
  `DocumentLoader::restyleForRoot`, `DocumentLoader.cpp:1039-1056`). Compiles are cached by an
  options hash that includes `touchDevice` and, once a script changed the root, the root attribute
  pairs with their values (`DocumentLoader.cpp:137-160`). `restyleWith` / `restyleForViewport`
  recompile and swap sheets in place (`DocumentLoader.h:141-158, 190-194`).
- **Fake fingers and fake mice.** A finger can play the mouse (`Engine::isCallMouseInTouchEvent`,
  `engine/core/Engine.cpp:3712-3718`) and the editor's Simulate Touch makes the mouse a finger.
  Android delivers a mouse with a button held on the touch path; only hover moves are mouse events
  (`engine/platform/android/NativeEngine.cpp:1609-1625`). Windows handles `WM_POINTER` so no
  promoted mouse messages follow a touch (`engine/platform/win/WinInputRouter.cpp:327`); the web's
  touch handler returns true, which cancels the compatibility mouse events
  (`engine/platform/emscripten/JasperEngineWeb.cpp:1239-1240`, inferred for every branch).
- **Gamepad identity.** `Input::Gamepad` is `{id, name, ...}` (`engine/core/Input.h:247-252`);
  `getGamepadName(id)` is the platform's string (a browser id such as "... Vendor: 054c ...", an
  Android `InputDevice` name). No vendor field.
- **MCP.** `get_input_state` returns levels, edges, touches, gamepads and maps with `fields`
  raw/touches/gamepads/actions (`docs/mcp/TOOLS.md:4747-4759`); `send_input` drives keys, mouse,
  touch and gamepad through `Engine::system*` (`TOOLS.md:1463-1488`; `engine/core/Input.h:285-287`
  says MCP input goes through `Engine::system*`); `configure_game_view` holds the toolbar's
  editor-only state (`TOOLS.md:1537-1562`). Nothing reports or forces a scheme.

## 4. Reference

| Source | Take | Improve / note |
|---|---|---|
| Craftsman, `include/mcpe015/kbm_input.h:9-20` (modes Touch / Keyboard / Mouse) | The pattern: the HUD hides once a physical key or a mouse is used in play, and **a finger brings touch back, except while a movement key is held**, so looking with a finger while walking with W does not flicker the HUD | Generalised to three schemes and to "any non-modifier key held", genre-agnostic |
| Craftsman, `kbm_input.h:152-162` (`onFingerDown(deviceId)`, `noteKeyDevice`) | A "finger" from the **same input device that sends keys** is an emulator's PC mouse, not a finger | Kept as R6(c) for Android; plus the Android mouse tool type, which their code did not use |
| Craftsman, `include/mcpe015/gamepad_input.h:48-52` | Nothing changes for a touch player until a real pad button or an axis past the deadzone arrives; a finger hands everything back to touch | R5, R6 |
| Unity Input System: `PlayerInput.currentControlScheme`, `onControlsChanged`, auto-switch; `InputBinding.MaskByGroup`, `InputAction.GetBindingDisplayString`; `DualShockGamepad`, `SwitchProControllerHID`, `XInputController` | Names: control scheme, current scheme, display string masked by group, the family split | Docs only, no code. No per-player pairing (out of scope) |
| CSS Media Queries 4 `hover` / `pointer` (primary input) vs `any-hover` / `any-pointer` | The split: primary = what is in use, `any-*` = what exists | Followed: `hover`/`pointer` follow the last pointer, `any-*` stay the capability |

Ideas only: no code, data, assets or Minecraft-specific mechanics are copied from Craftsman (a port
reconstructed from Mojang's retail binary). Nothing here is genre-specific: a "movement key" is any
held non-modifier key, a menu is any document.

## 5. Requirements

### Functional: the scheme

- **R1 Schemes.** `InputScheme { KeyboardMouse, Gamepad, Touch }`, named in Lua and MCP by the
  binding group strings `"Keyboard&Mouse"`, `"Gamepad"`, `"Touch"` (the same strings
  `InputControlPath::deviceGroup` returns, so `binding:getGroups()` compares directly), and in
  HTML by `keyboard-mouse`, `gamepad`, `touch`.
- **R2 Pointer kind and pad.** Besides the scheme the tracker keeps `currentPointer`
  (`KeyboardMouse` or `Touch`: the last pointer device used; a gamepad never changes it),
  `lastGamepad` (id of the pad that last switched or held the Gamepad scheme, -1 before any) and
  `gamepadFamily` of that pad (R9), else of the first connected pad, else `Generic`.
- **R3 Initial value.** At engine start, Play start and `InputActions.resetSession`: Touch with
  pointer Touch when `System::isTouchDevice()` (the editor's Simulate Touch included), else
  Keyboard&Mouse with pointer Keyboard&Mouse. A pad connected at start does not switch until used.
  No change event fires for the initial value.
- **R4 Keyboard&Mouse evidence.** Any of: a key down that is not a repeat, not a modifier alone
  (Shift, Ctrl, Alt, Super, Caps Lock), not synthesised by the engine (the Android Back's Escape,
  `NativeEngine.cpp:1516-1519`; soft-keyboard edit keys `NativeEngine.cpp:360-375`), and not
  typed while a text field has keyboard focus (`Engine::isTextInputFocused()`: typing is
  scheme-neutral, so a phone's soft keyboard never switches); a mouse button down from a real mouse;
  a wheel event; mouse motion of at least 12 canvas px accumulated within 0.25 s (jitter and a
  table bump do not count).
- **R5 Gamepad evidence.** A gamepad button down, or an axis whose magnitude crosses 0.5 upward
  (sticks; triggers past half way), the same thresholds the UI navigation uses
  (`html-ui.md:1569`). Resting drift never switches.
- **R6 Touch evidence.** A finger down (`Engine::systemTouchStart`), except:
  (a) while the scheme is Keyboard&Mouse and a non-modifier key is held (the hysteresis rule of the
  Reference): the finger works but the scheme stays;
  (b) a pointer the platform tagged as a mouse (R16) counts as mouse evidence instead;
  (c) on Android, a finger from an input device id that sent a key in the last 10 s counts as mouse
  evidence (emulators that send the PC mouse as fingers);
  (d) a mouse event within 300 ms and 24 canvas px of a touch end never counts (a compatibility
  mouse event of a platform that sends one).
- **R7 Switching.** Evidence of another scheme switches at once (no dwell time) except as R6 says.
  The current pointer follows R4 mouse evidence and R6 touch evidence. Evidence is collected at the
  `Engine::system*` entry points before the UI routes the event, so an event a document consumed
  still counts (tapping a menu with a finger is using touch).
- **R8 One change per frame.** Evidence lands in the pending window; `Input::beginFrame` applies
  the last scheme of the window before `InputActions::evaluate` and fires `onSchemeChanged(scheme,
  previous)` once if it differs from the frame before (A to B to A in one window: no event). The
  HTML root attributes and media restyles (R17-R19) apply in the same place, so a script and the
  UI see one scheme per frame.
- **R9 Gamepad family.** `GamepadFamily { Generic, Xbox, PlayStation, Nintendo }` from the pad's
  name: a browser `Vendor: xxxx` field first (045e Xbox, 054c PlayStation, 057e Nintendo), then
  name words (case-insensitive): "xbox", "xinput", "microsoft" → Xbox; the current PlayStation
  words (`InputActions.cpp:787-794`) → PlayStation; "nintendo", "switch", "pro controller",
  "joy-con" → Nintendo; else Generic. Cached per pad at connect.
- **R10 Override.** `setSchemeOverride(scheme)` forces the scheme (and the pointer for Touch and
  Keyboard&Mouse); evidence is still tracked underneath; `setSchemeOverride(nil)` returns to the
  tracked value (one change event when it differs). The Game view's Input Scheme menu (R24) and
  `configure_game_view` (R27) use it. Cleared at Play stop.

### Functional: prompts

- **R11 Action display for a scheme.** `InputActions.getActionDisplayName(action, scheme?)`
  returns the display name of the first binding of the action (asset order, overrides applied) that
  belongs to `scheme` (default: current), "" when none. A binding belongs to a scheme when its
  `groups` contain it, or, with no groups, when its control's `deviceGroup()` equals it; a composite
  belongs when its first part does. Virtual controls belong to every scheme. Names follow the
  family of `lastGamepad` (else the map's pad, as today).
- **R12 Nintendo and Xbox labels.** `getControlDisplayName` gains Nintendo labels by position
  (south "B", east "A", west "Y", north "X", "L", "R", "ZL", "ZR", "−", "+", "Home", "L Stick
  Press", "R Stick Press") and Xbox labels ("LB", "RB", "LT", "RT", "View", "Menu", "Xbox"). Generic
  keeps today's names. PlayStation names are unchanged.
- **R13 Glyph keys.** `InputActions.getActionGlyph(action, scheme?)` and
  `getBindingGlyph(bindingId)` return a stable lowercase key: `keyboard/<control name>`
  (`keyboard/space`, `keyboard/w`), `mouse/left|right|middle|scroll|delta`,
  `gamepad/<family>/<control>` (`gamepad/playstation/cross`, `gamepad/xbox/a`,
  `gamepad/nintendo/b`, `gamepad/generic/south`; sticks `.../left_stick`), `touch/tap|position|delta`,
  `virtual/<name>`. A composite returns its parts' keys joined by `+` (`keyboard/w+keyboard/a+...`).
  The key list is fixed and documented in `docs/mcp/guide/input.md`; projects map keys to art.

### Functional: UI follows the scheme

- **R14 Root attributes.** Every HTML document (`.html` through the compat layer; `.rml` documents
  get them too) carries `data-input-scheme` (`keyboard-mouse|gamepad|touch`) and
  `data-gamepad-family` (`xbox|playstation|nintendo|generic`) on its `<html>` element from its
  first compile, updated live by R8. Author CSS `html[data-input-scheme="touch"] .kbd-hint {display:
  none}` works with no script. A script that sets either attribute is overwritten at the next change.
- **R15 `visibility="touch-scheme"`.** On-screen controls take a third value: shown while the
  current scheme is Touch. Hidden, the control is inert and releases what it wrote (the existing
  release path, `OnScreenControls.cpp:157-161`). `touch-devices-only` keeps its meaning.
- **R16 Component UI.** `OnScreenVisibility::TOUCH_SCHEME` ("Touch Scheme", stored `touch_scheme`)
  on On-Screen Stick, On-Screen Button and Touch Area, applied by `UISystem::updateControls`; hidden
  controls release their pointers and clear their sources (as on disable).
- **R17 Live `hover` and `pointer`.** These two features follow `currentPointer`: Touch →
  `hover: none`, `pointer: coarse`; Keyboard&Mouse → `hover: hover`, `pointer: fine`. At load the
  value is today's (touch device → coarse), so a document that never sees another pointer compiles
  exactly as now. `any-hover` / `any-pointer` do not change.
- **R18 `<input-prompt>`.** `<input-prompt action="Map/Action" scheme="current|touch|gamepad|keyboard-mouse">`
  (default `current`) renders the action's display name (R11) as its text, and sets `data-glyph`
  (R13), `data-scheme` and `data-empty` (no binding for that scheme). It updates on scheme change,
  pad family change and rebinding (`setOverride`, `setBindings`, `resetOverrides`). Children given by
  the author replace the text (the attributes still update), so `<input-prompt action="Jump"><img
  class="glyph"></input-prompt>` with CSS on `[data-glyph]` shows art.

### Platform & backends

- **R19 Every target.** Linux, Windows, macOS, Android, iOS, HTML5 and the editor. Evidence comes
  from the existing `Engine::system*` calls, so platforms change only for R20.
- **R20 Platform tags.** Android: pointers whose tool type is `AMOTION_EVENT_TOOL_TYPE_MOUSE` are
  tagged mouse (they stay on today's touch path, so behaviour is unchanged), and key and pointer
  events pass their `deviceId` (R6(c)). iOS: `UITouchTypeIndirectPointer` touches are tagged mouse
  (inferred from UIKit; verified on device). Other platforms pass nothing (unknown device, untagged).
  The tag travels as `Engine::setInputSource(InputSourceTag)` set before the event and reset after
  it (no signature change of `systemTouch*`).
- **R21 No rendering change.** No backend work; the restyles are CPU work in RmlUi.

### Performance (release; mobile and web first)

- **R22 Tracker cost.** Per input event at most one comparison chain and a store (≤ 0.2 µs on a
  2020 phone core); per frame without a change ≤ 1 µs; zero heap allocations after start, also on a
  change (scheme strings are static). With no Lua subscriber, no `<input-prompt>`, no document and
  no on-screen control, a change costs only the stores.
- **R23 Change cost.** A scheme change: setting two attributes on each shown document plus RmlUi's
  restyle of the elements they affect: ≤ 0.5 ms desktop, ≤ 2 ms on the mobile reference device for
  `engine/tests/htmlui/fixtures/bench/input-scheme.html` (a HUD of 120 elements in 3 documents, new). A pointer change of a
  document whose compile read `hover`/`pointer`: a cache hit (the variant was prewarmed, D4) is a
  sheet swap ≤ 1 ms desktop / 4 ms mobile; documents that never read those features are not
  touched. Neither cost recurs while the scheme stays.

### Editor UX (human)

- **R24 Game view.** The toolbar gets an "Input Scheme" button (gamepad/keyboard/hand icon showing
  the current scheme during Play) whose menu is Auto (default), Keyboard & Mouse, Gamepad, Touch,
  plus a disabled line "Current: Gamepad (PlayStation)". Forcing works in edit mode (applies at Play
  start) and during Play; it is a workspace preference like Simulate Touch (`Workspace.cpp`),
  never written into builds, no undo (view state, as the other toolbar toggles). Tooltip: "Input
  Scheme: what the game treats as the device in use (Auto follows the last device used)".
- **R25 Simulate Touch interplay.** Simulate Touch stays the device capability (`isTouchDevice`);
  the Input Scheme menu forces the scheme. Both on: Touch scheme with mouse-as-finger, as today.
- **R26 Input Actions window.** During Play its header shows "Scheme: Touch · Pointer: Touch · Pad:
  Xbox (id 0)", and each binding row of the debugger dims bindings outside the current scheme.
  Component Inspector: the Visibility rows of the three on-screen components list "Touch Scheme"
  (help text: "Touch Scheme: shown while the player is using touch (InputActions.currentScheme)").

### Agent UX (MCP, Lua, CLI)

- **R27 MCP.** `get_input_state` always adds `scheme: {current, pointer, gamepad, gamepad_family,
  override}` (strings as R1, `override` null when Auto) and `changes` (count since Play start).
  `configure_game_view` gains `input_scheme: auto|keyboard_mouse|gamepad|touch`. `send_input`
  needs no change: its gamepad, key, mouse and touch events reach `Engine::system*` and switch the
  scheme by R4-R7 (documented in the guide). The CLI gets both through `jasperengine-editor call`.
  Tool text growth is about 90 characters across both tools; `tool_budget.py --check` passes and
  each new string has an eval case (`tools/mcp/eval/cases.py`) that fails without it.
- **R28 Lua API** (`InputActions`, bound in `GameplayClassesLua.cpp`, documented from the header
  through `tools/lua_api`):
  - read-only static properties `currentScheme`, `currentPointer`, `gamepadFamily`, `lastGamepad`;
  - `onSchemeChanged = function(scheme, previous) end` (static `FunctionSubscribe`, cleared at Stop,
    `tools/check_event_clears.py`);
  - `setSchemeOverride(scheme|nil)`, `getSchemeOverride()`;
  - `getActionDisplayName(action, scheme?)`, `getActionGlyph(action, scheme?)`,
    `getBindingGlyph(id)`; `InputScheme` and `GamepadFamily` string constants
    (`InputScheme.Touch == "Touch"`).
  The hand-written stub `engine/core/script/binding/stubs/HtmlUI.lua` documents `<input-prompt>`'s
  attributes and the `data-input-scheme` root attribute for completion in HTML-side scripts.
- **R29 Docs.** `docs/mcp/guide/input.md` gets "Input scheme" (rules R3-R10, Lua, glyph keys,
  testing with `send_input`); `docs/mcp/guide/html-ui.md` updates the media sentence (1591), the
  on-screen control table (1636-1638) and adds `<input-prompt>`; the support tables
  (`gen_html_ui_support.py`) say `hover`/`pointer` "follow the last pointer used (live)";
  `docs/mcp/TOOLS.md` regenerated.

### Data & compatibility

- **R30 No version bump.** New stored values only: the `touch_scheme` enum value (additive; old
  scenes never contain it), the workspace's `inputScheme` (default `auto`). No `.inputactions` change.
  Old projects load and behave as before until they use a new value.
- **R31 Behaviour change, deliberate.** On a touch device with a mouse or keyboard attached, a
  document using `@media (hover: hover)` now switches when the mouse is used. Release note; the
  tests pinning load-time evaluation (`test_html_ui_*` media cases) are updated in the same commit
  and say so.
- **R32 Exports.** Nothing new is exported: the tracker is engine code in every player, and the
  override is editor-only.

## 6. Design constraints & decisions

**D1 Where it lives.** `engine/core/input/InputScheme.{h,cpp}`, a static tracker beside
`InputActions` (not in `Input`, which stays device levels and edges). `Input::beginFrame(dt, ...)`
calls `InputSchemeTracker::latch()` right after the window latch and before
`InputActions::evaluate`. `Engine::system*` call the `note*` functions first thing.

```cpp
namespace jasperengine {
    enum class InputScheme : uint8_t { KeyboardMouse = 0, Gamepad = 1, Touch = 2 };
    enum class GamepadFamily : uint8_t { Generic = 0, Xbox, PlayStation, Nintendo };
    enum class InputSourceTag : uint8_t { None = 0, Mouse, Synthetic };

    class JASPERENGINE_API InputSchemeTracker {
    public:
        // ---- evidence (Engine::system*; tests)
        static void noteKey(int key, bool repeat, int deviceId = -1);
        static void noteMouseButton();
        static void noteMouseScroll();
        static void noteMouseMove(float dx, float dy, double now);
        static void noteTouchStart(float x, float y, int deviceId = -1);
        static void noteTouchEnd(float x, float y, double now);
        static void noteGamepadButton(int gamepad);
        static void noteGamepadAxis(int gamepad, int axis, float previous, float value);
        static void noteGamepadConnected(int gamepad, const std::string& name);
        // ---- the frame (Input::beginFrame): applies the window, fires the change once
        static void latch();
        static void reset(bool touchDevice);   // R3: engine start, Play start, resetSession
        // ---- state
        static InputScheme getCurrent();
        static InputScheme getPointer();       // KeyboardMouse or Touch
        static int getLastGamepad();
        static GamepadFamily getGamepadFamily();
        static GamepadFamily familyOf(const std::string& padName);
        static void setOverride(std::optional<InputScheme> scheme);
        static std::optional<InputScheme> getOverride();
        static uint32_t getChangeCount();
        static const char* groupName(InputScheme);   // "Keyboard&Mouse"...
        static const char* htmlName(InputScheme);    // "keyboard-mouse"...
        static FunctionSubscribe<void(const std::string&, const std::string&)> onSchemeChanged;
        // engine listeners (HTML UI, component UI), no allocation, before onSchemeChanged
        using Listener = void (*)(InputScheme scheme, InputScheme pointer, GamepadFamily family);
        static void addListener(Listener listener);
    };
}
```

`InputActions` re-exports the Lua-facing members (R28) so scripts keep one class.

**D2 Synthetic and tagged events.** `Engine::setInputSource(InputSourceTag)` (engine-internal,
reset to None after each `system*` call) marks the Android Back's Escape and the soft-keyboard
edit keys `Synthetic` (never evidence) and Android/iOS mouse pointers `Mouse`. The Simulate Touch
mouse arrives as touch and counts as touch (it is meant to). A finger playing the mouse
(`isCallMouseInTouchEvent`) never reaches `noteMouse*` because the simulation calls `Input::*`
directly, not `Engine::systemMouse*` (`Engine.cpp:3712-3718`).

**D3 HTML root attributes without recompiles.** The compat HTML transform writes both attributes on
`<html>` at compile with the values at load, so `rootSignatureOf` (names only) already contains them
and a later value change never triggers `restyleForRoot`'s recompile; R8 sets them with
`ElementDocument::SetAttribute` and RmlUi restyles what their selectors match. The two attributes
are excluded from `optionsHash`'s value hashing (names only), so the compile cache is not split per
scheme. `.rml` documents get the attributes set after load (no compile involved).

**D4 Live `hover`/`pointer` by variant swap, not themes.** A third RmlUi theme would make every
block combining it with `prefers-color-scheme` inexpressible (`MediaQuery.cpp:724-731`). Instead
`CompatOptions` gets `coarsePointer` (initial `touchDevice`), used by `hover`/`pointer` only
(`any-*` keep `touchDevice`), and the compile records `dependsOnPointer` when it evaluated either
feature (as `dependsOnViewport`). On a pointer change, documents with `dependsOnPointer` are
restyled with `DocumentLoader::restyleWith(options with coarsePointer flipped)`. On a touch device,
right after a document with `dependsOnPointer` loads, the other variant is compiled on a background
job (`DocumentLoader::compile` is thread-safe) so the first switch is a cache hit (R23). Non-touch
devices never prewarm (zero cost); a switch there is a synchronous compile, as a viewport resize is.

**D5 `<input-prompt>`.** A compat custom element registered like the on-screen controls
(`engine/core/htmlui/controls/InputPrompt.{h,cpp}`), with a process-wide revision counter bumped
by scheme, family and override changes; each element compares its stored revision in `OnUpdate`
(one integer compare per frame per element) and rebuilds its text only on a change.

**D6 Component UI.** `UISystem::updateControls` reads `InputSchemeTracker::getCurrent()` once per
frame beside `isTouchDevice()` (`UISystemPointer.cpp:1802`); `TOUCH_SCHEME` uses it. A control
turned hidden calls the existing release path (pointer ownership cleared, `InputActions::clearSource`).

**D7 Not a filter.** `InputActions::evaluate` never reads the scheme (Out of scope). A game that
wants scheme-specific behaviour reads `currentScheme`.

**D8 Editor.** The Game view keeps the override in `Workspace` (`inputScheme`, beside
`simulateTouch`, `editor/Workspace.cpp:207-208, 343`) and applies it at Play start through
`InputSchemeTracker::setOverride`; changing it during Play applies at once.

## 7. Acceptance criteria

| AC | Req | Verification (red: what fails without the change) |
|---|---|---|
| AC1 | R3-R7 | `InputSchemeTest.SwitchesOnEachDeviceKind` (`engine/tests/core/input/InputSchemeTest.cpp`): key → Keyboard&Mouse, pad button → Gamepad, finger → Touch, wheel → Keyboard&Mouse, each visible after the next latch. Red: no tracker |
| AC2 | R6(a) | `InputSchemeTest.FingerWhileKeyHeldKeepsKeyboard`: W held, finger down and drag: scheme stays Keyboard&Mouse, pointer becomes Touch; W released, next finger switches to Touch |
| AC3 | R6(b,c), R20 | `InputSchemeTest.TaggedAndSameDeviceFingersAreMouse`: a finger with `InputSourceTag::Mouse`, and a finger from device 7 after a key from device 7, both give Keyboard&Mouse; device 8 gives Touch |
| AC4 | R4, R6(d) | `InputSchemeTest.NoiseDoesNotSwitch`: 11 px of mouse motion, a mouse event 100 ms after a touch end at the same place, a key repeat, Shift alone, a key typed with a text field focused, a `Synthetic` Escape, a stick at 0.3: none switches |
| AC5 | R8 | `InputSchemeTest.OneChangePerFrame`: Touch → Gamepad → Touch inside one window fires nothing; Touch → Gamepad fires once with ("Gamepad", "Touch") before `InputActions::evaluate` (an action callback sees the new scheme) |
| AC6 | R10 | `InputSchemeTest.OverrideWinsAndResumes`: override Gamepad, a finger arrives (tracked Touch), current stays Gamepad; clearing fires one change to Touch |
| AC7 | R22 | `InputSchemeTest.ZeroAllocations`: counting allocator over 10,000 mixed events and 600 latches with 40 changes: 0 allocations; `InputBenchTest` gains `scheme_event_ns` and `scheme_latch_ns` lines gated in `tools/perf/bench-thresholds.json` (0.2 µs, 1 µs at desktop × 5 slack) |
| AC8 | R9, R12 | `InputActionsTest.GamepadFamilyAndLabels`: names "Xbox Wireless Controller", "DualSense Wireless Controller", "Pro Controller", "Logitech Dual Action", a browser id with `Vendor: 057e` map to Xbox, PlayStation, Nintendo, Generic, Nintendo; south button reads "A", "Cross", "B", "A"; `gamepad/right_trigger` reads "RT", "R2", "ZR", "Right Trigger". Red: Nintendo/Xbox read Generic |
| AC9 | R11, R13 | `InputActionsTest.ActionDisplayForScheme`: "Gameplay/Jump" with keyboard/space (group Keyboard&Mouse), gamepad/a (no groups) and touch press: per scheme "Space", "Cross" (PlayStation pad last used), "Touch"; glyphs `keyboard/space`, `gamepad/playstation/cross`, `touch/tap`; an action without a touch binding gives "" and `data-empty` in AC12; an override changes the result |
| AC10 | R14, D3 | `InputSchemeDocumentTest.RootAttributeFollowsScheme` (`engine/tests/htmlui/TestInputScheme.cpp`): a document with `html[data-input-scheme="gamepad"] .pad {display: block}` shows `.pad` only after a pad press; `DocumentLoader::compiledOf` returns the same compile before and after (no recompile); a `.rml` document gets the attributes too |
| AC11 | R15, R16 | `InputSchemeDocumentTest.TouchSchemeVisibility`: an `<onscreen-stick visibility="touch-scheme">` held by a finger hides and releases `Horizontal` to 0 when a key switches the scheme; `UIMultiPointerTest.TouchSchemeVisibility` (`editor/tests/UIMultiPointerTest.cpp`): the same for the component stick, button and area |
| AC12 | R18 | `InputSchemeDocumentTest.InputPromptUpdates`: `<input-prompt action="Gameplay/Jump">` text "Space" → after a pad press "Cross" with `data-glyph="gamepad/playstation/cross"`; `setOverride` to `keyboard/e` while in Keyboard&Mouse → "E" next frame; author children are kept |
| AC13 | R17, D4 | `InputSchemeDocumentTest.HoverFollowsPointer`: with `touchDevice` on, `@media (hover: hover) {.tip {display: block}}` hidden at load, shown after mouse evidence, hidden after a finger; the second switch reuses the prewarmed compile (cache size unchanged, `restyleWith` returns a cached pointer); a document without hover/pointer queries keeps its compile. `MediaQueryTest` (compat suite) asserts `any-hover` still reads the capability and that `(prefers-color-scheme: dark) and (hover: hover)` stays expressible |
| AC14 | R23 | `HtmlUIRenderPerf.SchemeSwitch` (Release, `fixtures/bench/input-scheme.html`): scheme change ≤ 0.5 ms, pointer cache-hit swap ≤ 1 ms, through `perfSlack()`; mobile numbers recorded by the S15 device-lab run in `docs/html-ui/render-benchmark.md` |
| AC15 | R27 | `tools/mcp/tests/test_input.py` (existing, `CI = True`; its created project gains a HUD document with a `touch-scheme` stick): `test_scheme_follows_send_input`: in Play, `send_input` gamepad a → `get_input_state.scheme.current == "Gamepad"`, touch tap → "Touch", key → "Keyboard&Mouse", and `inspect_ui` shows the HUD stick hidden then shown; `test_forced_scheme`: `configure_game_view {input_scheme: "gamepad"}` before Play → Gamepad at start and `override == "Gamepad"`, `auto` clears it; `test_lua_api`: `eval_lua` reads `InputActions.currentScheme`, an `onSchemeChanged` marker appears in the console once per switch |
| AC16 | R24, R26 | `GameWindowTest.InputSchemeMenu` (`editor/tests/window/`): choosing Gamepad stores `inputScheme: gamepad` in the workspace and sets the override at Play start; Simulate Touch stays independent; `tools/editor-shot.sh` screenshot of the menu and of the Input Actions window header in the PR |
| AC17 | R30 | `InputSchemeEditorTest.OldScenesUnchanged`: a scene with `touch_devices_only` controls loads and saves byte-identical; `touch_scheme` round-trips; catalog snapshot and `ReflectParity` updated with the one enum value |
| AC18 | R19, R20 | S22's test project `tools/test-projects/input` logs `[gate] scheme <name>` on each change: on the Android emulator (`tools/android-emulator-test.sh`, new lines in `android-expect.txt`) an `adb shell input mouse tap` logs Keyboard&Mouse and `input touchscreen tap` Touch; its web gate (`GATE=input`, baseline `console` lines) sends CDP touch, mouse and key events and expects the three lines in order; `tools/build.sh web` and the Android build pass |
| AC19 | R28, R29 | `python3 -m lua_api doctest` passes on the new doc blocks; `jasper://lua-api?class=InputActions` lists the new members; `test_docs.py` passes; `gen_html_ui_support.py --check` passes with the new `hover`/`pointer` rows |

Each red control is recorded by running the test on the base commit first; the PR names the run.

## 8. Dependencies & conflicts

- **Depends on:** S22 (InputActions, display names), p4-input / ui-multitouch (on-screen controls,
  gamepad routing), the compat layer's restyle machinery (S17's live patch keeps `rebind`).
- **Used by:** `text-entry-gamepad-cursor-back-button.md` (the gamepad cursor is visible only while
  `currentScheme == Gamepad`; its cursor clicks are not evidence, D2 of that spec);
  `input-recorder-and-input-scripts.md` (a recording replays through `Engine::system*`, so it
  reproduces scheme switches; it should record the override too); `game-ui-localization.md`
  (display names are English control names; localising them is that spec's, through a table keyed
  by glyph key, R13); `dev-console-for-agents.md` (may print the scheme).
- **Merge hotspots:** `engine/core/input/InputActions.{h,cpp}` (display names), `engine/core/Input.cpp`
  (`beginFrame`), `engine/core/Engine.cpp` (`system*` entry points, also touched by the sibling
  spec's Back and cursor work), `engine/core/htmlui/compat/css/MediaQuery.cpp`,
  `compat/DocumentLoader.{h,cpp}`, `controls/OnScreenControls.cpp`,
  `engine/core/subsystem/UISystemPointer.cpp`, `engine/platform/android/NativeEngine.cpp` (also
  the sibling's text entry and Back), `editor/window/GameWindow.cpp`, `editor/mcp/tools/GameplayTools.cpp`,
  `editor/mcp/tools/ViewTools.cpp`, `docs/mcp/guide/input.md`, `docs/mcp/guide/html-ui.md`,
  `tools/mcp/budget.json`.

## 9. Risks & defaults

| Risk / open question | Default taken |
|---|---|
| A player resting a palm on a touch laptop while typing flips to Touch | R6(a): a held key keeps Keyboard&Mouse; a palm without a key held does switch (documented; the HUD reappears, nothing breaks) |
| Steam Deck / handhelds with both a touch screen and pads | Initial Touch (touch device), first pad press switches; documented |
| A pad that reports constant axis noise above 0.5 | Treated as use; the pad's own deadzone calibration is the platform's; `setSchemeOverride` is the escape for a game |
| Prewarming doubles a touch device's compile memory for documents with hover queries | Only documents with `dependsOnPointer` (usually few); the compile cache already holds viewport variants. Measured in AC14 |
| Gamepad family guessed wrong for an unknown pad | Generic labels (today's names); `gamepadFamily` is readable so a game can offer a manual choice |
| Display names are English | Localisation belongs to `game-ui-localization.md`, keyed by glyph key |
| `hover` changing at runtime surprises a document written for load-time evaluation | R31 release note; the old behaviour is what a non-touch device and a never-switching touch device still see |
| iOS indirect pointer tagging untested in CI | Verified on device for the PR (manual step listed in AC18's Android/iOS notes); without the tag a trackpad counts as touch, today's behaviour |

## Deviations

- **D1 API.** The evidence functions take what the rules need: `noteKey(key, repeat, typing, now)`
  (typing: a focused text field, R4), `noteKeyUp(key)` (R6(a) needs the held keys),
  `noteMouseButton(x, y, now)` and `noteMouseMove(x, y, now)` (positions, not deltas: R6(d) compares
  them with the last touch end), `noteTouchStart(x, y, now)`, `noteGamepadDisconnected(id)`. The
  device id travels with the tag: `Engine::setInputSource(tag, deviceId)` (R20), read back with
  `InputSchemeTracker::getSourceTag/getSourceDevice`. There is no `addListener`: the HTML UI
  (`HtmlUIContext::update`), `<input-prompt>` and the component UI compare
  `InputSchemeTracker::getRevision()` or read `getCurrent()` once per frame (D5, D6), which costs a
  compare when nothing changed and keeps R8's one scheme per frame. Added: `familyOfPad`,
  `familyName`, `familyKey`, `parseScheme`. `reset` starts on the forced scheme when one is set (no
  event), so a forced Play starts on it (AC15).
- **D3.** The compile adds the two attribute names to the root's selector banding
  (`rewriteContext.rootAttributes`) instead of writing them into the RML, and `DocumentLoader::load`
  sets their live values before the first layout: the root signature holds the names as D3 wants,
  and no existing RML snapshot changes. `restyleForRoot` counts the names as present, so a script
  that removes one does not recompile.
- **R11.** A map pinned to a pad (`setGamepad`) names its controls after that pad; otherwise after
  the pad last used, then the first connected one (the order R11 gives), so a second player's map
  never shows the first player's labels.
- **R9.** Firefox's gamepad ids (`054c-0ce6-...`) are read as a vendor id too.
- **R13.** Also `touch/count`; a binding with required modifiers prefixes them
  (`keyboard/ctrl+keyboard/s`); a stick's single axis gives the stick's key.
- **R26.** The debugger's rows are actions, so a Bindings column lists each action's bindings,
  those outside the current scheme dimmed.
- **AC7.** `InputBench.SchemeEventAndLatch` prints `scheme_event_ns` and `scheme_latch_ns` and gates them
  with `perfSlack()` like the suite's other budgets; `tools/perf/bench-thresholds.json` reads the JSON of
  benchmark executables only, which InputBench is not.
- **AC14.** The cost is measured by `InputSchemeDocumentTest.SchemeSwitchCost` (`TestInputScheme.cpp`,
  three documents of `fixtures/bench/input-scheme.html`), gated with `JASPER_HTMLUI_PERF_SLACK` as the
  other HTML UI budgets near a build server's noise; the render benchmark (`HtmlUIRenderPerf`) runs one
  document through FixtureRunner. Mobile numbers need the device lab and are not recorded.
- **D4.** Prewarming the other variant's compile was not enough for R23: a restyle parsed the
  document's sheets again (9.5 ms for the three documents). Each document now keeps the sheets it
  parsed per compile (`DocumentLoader::restyleSheets`, the last four compiles), and on a touch device
  both pointer variants' sheets are parsed, compiled and resolved for the document as soon as the
  background compile is in (`HtmlUIContext::update`, `DocumentLoader::prewarmSheets`).
- **R23 on box A.** On box A (a 2.4 GHz Haswell) the first pointer switch after the load took
  1.21-1.33 ms in every run, the later ones 0.73-0.96. Two things were the first switch's own: the
  other scheme's rules (`html[data-input-scheme=...]`) match new combinations under the other sheet,
  whose definitions RmlUi built then (6), and the prompts hidden since the load (the `.kbd` texts)
  were measured for the first time (9 strings shaped). The prewarm now resolves the other variant's
  definitions with the root's scheme attribute set to the one its pointer comes with (put back
  after) and measures each text never laid out with the font it has. Every switch also dirtied
  every variable of every element: a sheet swap marked each custom property of the old and new
  definitions changed, though the variants declare them alike (four `--html-*` ones per element from
  the user-agent sheet, about sixty on the root), so all 246 elements computed their values again;
  RmlUi now leaves out the custom properties both definitions declare alike (fork patch IDA-1 in
  `docs/html-ui/FORK.md`), and a switch computes 87. The first switch now does the same work as the
  later ones (the same definition, layout and allocation counts). Twelve quiet runs: the first
  switch 0.69-0.94 ms, a pointer switch 0.58-0.68 ms, a scheme switch 0.31-0.36 ms.
- **AC16.** `GameWindowTest.InputSchemeMenu` is in `editor/tests/PlayStartupScenesTest.cpp` (it needs
  that file's play-session access) and drives `GameWindow::chooseInputScheme`, which the menu calls.
- **AC17.** `InputSchemeEditorTest.OldScenesUnchanged` checks the three components' codecs (old values
  read and written back byte-identical, `touch_scheme` round-trips) in `editor/tests/UIMultiPointerTest.cpp`.
- **AC18.** CDP cannot send a gamepad: the web gate's tap and key give two lines (`Touch`, then
  `Keyboard&Mouse`; the page starts on Keyboard&Mouse). The Android mouse and touchscreen taps run
  after the two-finger tap in `tools/android-two-finger-tap.sh`.
- **R19 iOS (Mac pass).** The iOS builds ran on an arm64 Mac (the Xcode project for the simulator, the
  prebuilt iOS player, `tools/ci/ios_packaging.sh`); the indirect-pointer tagging did not: the
  simulator turns the Mac's pointer into touches and `simctl` sends no trackpad pointer, so it still
  needs a device.
- **R27 tool text.** `get_input_state`'s description no longer mentions the input scheme: its eval
  case `guard_input_scheme_read`, sharpened so the Game view's forced `input_scheme` is in reach and
  the prompt names no tool, passed 4/4 without the sentence (the tool's name and its `scheme` field
  decide), and AGENTS.md cuts MCP text no case needs. The `scheme` field and `changes` stay.
