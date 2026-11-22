# Text Entry, Gamepad Cursor and Back Button: the right soft keyboard per field, a pointer for pads, and one Back route with a menu gate

Builds on the HTML UI input work (p4-input, ui-multitouch, the IME handler), S22 (Input Actions)
and `input-device-awareness.md` (the current input scheme). Three parts, one spec, because they
share one question: *what is on screen and who gets the player's input right now*. Part A is text
entry, part B the gamepad cursor, part C Back and the pause gate. Paths are relative to the
repository root.

## 1. Goal

A game UI written in HTML gets a phone keyboard that ignores the field: an e-mail field shows
letters, a PIN field shows letters, the action key never says "Send", and suggestions cannot
replace a word cleanly. A pad player meets a menu with nothing focusable and cannot press anything.
Android's Back is only an Escape key, and nothing tells a game whether a menu is up, so it cannot
pause correctly when the app loses focus. This spec makes each text field drive the platform
keyboard (type, action key, capitalisation, correction, length) with the engine's field as the one
source of truth; adds an opt-in gamepad cursor that clicks, drags and scrolls through the same
paths as a pointer; and gives Back one route (top menu document, then the game) plus an "Is Menu"
flag that gates input release and an optional pause request. All three cost nothing when unused.

## 2. Scope

**In scope**
- **A. Text entry.** `TextEntryTraits` derived per field from HTML `type`, `inputmode`,
  `enterkeyhint`, `autocapitalize`, `autocorrect`, `maxlength`, `<textarea>`; mapped to Android
  `EditorInfo`, iOS `UITextInputTraits`, the web's hidden textarea. The action key acts as Enter
  with the browser's per-hint default action (next field, previous, blur, submit). The engine
  sends the field's real text back to the platform whenever it differs from what the IME believes
  (filters, `maxlength`, scripts), on Android and iOS; one shared, host-tested edit diff. Lua
  `System.showVirtualKeyboard(text, options)`. Component InputField: Keyboard Type and Character
  Limit. Game view keyboard chip; `inspect_ui` reports the focused field's traits.
- **B. Gamepad cursor.** UIDocument "Gamepad Cursor": while the current scheme is Gamepad and such
  a document is shown, the left stick moves a drawn pointer, A clicks and drags, the right stick
  scrolls, the d-pad moves focus and the cursor jumps to it. Project settings for speed,
  acceleration, scroll speed, graphic and size. Lua `GamepadCursor`, MCP placement and report.
- **C. Back and the pause gate.** `Engine::systemBack()` fed by Android Back, keyboard Escape and
  gamepad B; a cancelable `back` DOM event to the top menu document, then `Engine.onBack`; the
  Android Escape compatibility kept. UIDocument "Is Menu"; `Engine.isMenuShowing`; a menu
  appearing releases what the game held; Project Settings > Player > "Pause On Focus Loss" firing
  `Engine.onPauseRequested(reason)` only while no menu is showing. MCP `send_input` `app: back`.

**Out of scope (default decisions)**
- On-screen keyboards drawn by the engine (desktop with only a pad, TV, Steam Deck game mode). A
  focused field with no keyboard device stays focused; games that need one build it in HTML.
  Store overlays (Steam's floating keyboard) belong to their plugins.
- `autocomplete` / `textContentType` (password autofill, one-time codes), `spellcheck` (stays
  ignored), the web's mirrored text (the web keeps its clear-after-commit textarea: no context for
  suggestions, `autocorrect` forced off there, documented), input validation of `email`/`url`.
- Character filtering by Keyboard Type in the component InputField (Unity's Content Type filters):
  traits only; Character Limit is enforced.
- A gamepad cursor in the 3D world or in Texture-mode documents' own space (it is a screen pointer;
  world documents receive it as they receive the mouse), several cursors for several pads (one
  cursor, any pad drives it), S17's gamepad pick-up/put-down drag (still out; the cursor's A-held
  drag is a pointer drag).
- Browser Back (`popstate`): hijacking history breaks portal iframes and the page's own navigation;
  the web gets Escape and B only. iOS has no Back button; the edge swipe is the system's.
- Automatic pausing (time scale, audio): the engine requests, the game pauses.
- A menu stack data structure: "the top menu" is the shown Is Menu document that receives input
  first (D10). Games stack menus by showing documents.

## 3. Current state

All read from the code (not run):

- **One keyboard for the whole app on Android.** `GameActivity_setImeEditorInfo(TYPE_CLASS_TEXT,
  IME_ACTION_NONE, IME_FLAG_NO_FULLSCREEN)` is called once in `NativeEngine`'s constructor
  (`engine/platform/android/NativeEngine.cpp:543-549`). `System::showVirtualKeyboard(std::wstring
  text)` takes only text (`engine/core/System.h:249`), called by the document handler with the
  field's value (`engine/core/htmlui/RmlTextInputHandler.cpp:67-82`) and by the component
  InputField (`engine/core/subsystem/UISystemPointer.cpp:502-504`). Lua: `System.showVirtualKeyboard(text)`
  (`engine/core/script/binding/CoreClassesLua.cpp:4012`).
- **Attributes dropped.** `inputmode`, `enterkeyhint`, `autocapitalize`, `autocomplete`,
  `spellcheck` are "ignored ... dropped (Info) (HTMLUI-HTML-0004)" (`docs/mcp/guide/html-ui-support.md:229-230, 278, 313, 427`);
  `email`, `url`, `tel`, `search` are "text input; no validation or keyboard hints", `number` a text
  field with numeric filtering (`html-ui-support.md:190, 195, 200, 202, 205`); `maxlength` is kept
  (`:355`).
- **Android edits are already diffed.** For a focused document field the GameTextInput state is
  diffed against the engine's copy (`imeText`): removals become Backspace key presses after arrow
  keys move the caret, additions `systemTextInput`, the composing region `systemImeComposition`
  (`NativeEngine.cpp:341-503`); the component InputField has its own diff
  (`engine/platform/android/AndroidTextInput.h:156`). The field's text is pushed to the IME only
  when the keyboard opens (`NativeEngine.cpp:766-786`): when the field rejects a character (a
  `maxlength`, a number filter) or a script changes the value, the IME keeps text the field does
  not have, and later diffs are computed against the wrong base. The conversions are host-tested
  (`engine/tests/core/input/AndroidTextInputTest.cpp`); the document diff is not (it is a static
  function of `NativeEngine.cpp`).
- **Android editor actions.** The build uses `androidx.games:games-activity:2.0.2`
  (`engine/workspaces/androidstudio/app/build.gradle:134`); its `GameActivity.h` has no editor
  action callback (verified in the Gradle cache copy), while 3.0.5 declares
  `bool (*onEditorAction)(GameActivity*, int action)` (its `GameActivity.h:289`).
- **iOS keeps a sentinel, not the text.** A nearly transparent `UITextField` holds a sentinel
  string; corrections are off "Corrections would rewrite text the engine already has"
  (`engine/platform/apple/ios/EngineView.mm:117-137`); edits are read as a backspace on the
  sentinel or new text after it (`:217-241`), Return through `textFieldShouldReturn:` (`:243-247`).
  The keyboard type and return key are UIKit's defaults; no traits come from the field.
- **Web.** A hidden `<textarea id="jasper-ime">` with `autocorrect`/`autocapitalize` off, cleared
  after each commit (`engine/platform/emscripten/JasperEngineWeb.cpp:259-326`); focused by
  `showVirtualKeyboard`, which ignores its text (`:408-414`).
- **Gamepad reaches documents only through focus.** "While an element Tab reaches ... has the
  focus, the gamepad drives the document like a keyboard ... With no such element focused
  (nothing focused, or a clicked panel), the gamepad is the game's" (`docs/mcp/guide/html-ui.md:1550-1583`);
  routing in `gamepadKeyDown` (`engine/core/Engine.cpp:3566-3586`) to
  `UIDocumentSystem::eventOnGamepadKeyDown` (`engine/core/subsystem/UIDocumentSystem.h:175-185`). B
  is Escape (closes an open select or a modal `<dialog>`, `UIDocumentSystemInput.cpp:184, 619`). S17
  excludes a gamepad drag (`docs/specs/S17-htmlui-widgets.md:39`). There is no gamepad pointer.
- **Back is Escape.** `MainActivity` registers an `OnBackPressedCallback(true)` that calls
  `onBackPressedNative` unless a native plugin consumed it
  (`engine/platform/android/java/com/jaspergameengine/jasperengine/MainActivity.java:82-89`);
  `handleGameActivityInput` turns it into an Escape character and key press, released next frame
  (`NativeEngine.cpp:1510-1520`). There is no `back` event, no Lua hook and no menu notion.
- **Focus loss.** `systemFocus(false)` / `systemPause()` fire `onFocus(false)` then `onPause`, release
  every held key, button and finger and cancel UI pointers (`engine/core/Engine.h:979-992,
  1046-1049`; `docs/mcp/guide/input.md:84-87`; `docs/mcp/guide/platform-services.md:108-127`).
  Nothing asks the game to pause, and nothing knows whether a menu already covers the game.
- **UIDocument.** Fields `filename, sortOrder, enabled, interactive, renderMode, textureWidth,
  textureHeight, inputSurface, pixelRatio` (`engine/core/component/UIDocumentComponent.h:25-37`;
  reflection `editor/reflect/components/UIReflect.cpp:323-351`); Lua class bound in
  `engine/core/script/binding/HtmlUIWidgetsLua.cpp:751`.
- **Component InputField.** `placeholder`, `passwordChar`, `password`, `disabled`, colours, events
  (`engine/core/component/InputFieldComponent.h:13-41`); no keyboard type or character limit.
- **MCP.** `send_input` has `app: pause|resume|focus_lost|focus_gained|low_memory|hide|show|orientation`
  (`docs/mcp/TOOLS.md:1479`; handler `editor/mcp/tools/PlayTools.cpp:3585-3604`), gamepad events
  by button or axis; `inspect_ui` returns the element tree, diagnostics and boxes
  (`TOOLS.md:4052-4078`) with no focus or keyboard information.

## 4. Reference

| Source | Take | Improve / note |
|---|---|---|
| Craftsman `include/mcpe015/ime_bridge.h:15-28, 49-104` | A field's **kind and action key** reach the keyboard; every IME change is an **ordered edit** "delete N code points, insert text" applied through the field's own input path (so its length limit and filter stay its own); the engine sends the field's **real text** back whenever it differs, and the keyboard is reset to it | Our edit model already exists on Android (§3); we take the "real text back" half and the per-field traits, and share the diff with iOS |
| Craftsman `include/mcpe015/gamepad_input.h:31-36, 137-165` | Menus get a **pointer** moved by the stick or d-pad; A taps where it is through the same path as a finger, holding A drags, B is Back, the right stick scrolls; a screen that owns the d-pad moves focus and the **pointer jumps to the focused element** so A hits it; repeat 0.40 s / 0.12 s | Our cursor uses the mouse path (hover works), our existing focus navigation and repeat (0.4 s / 0.1 s) |
| Craftsman `include/mcpe015/touch_router.h:17-19` | Opening a screen with fingers down **releases what they held** (fire, joystick, buttons): nothing stays stuck | R27 |
| Craftsman `include/mcpe015/json_ui.h:405-413` (`is_showing_menu`, default true, false only for HUD screens) | Back and app focus loss use **"am I already in a menu?"** as the gate before pushing the pause screen | Our default is false (old documents are HUDs or unknown); a menu opts in |
| Unity: `TouchScreenKeyboard.Open(text, TouchScreenKeyboardType, autocorrection, multiline, secure, ..., characterLimit)`, `InputField.keyboardType`, `characterLimit`; Input System sample `VirtualMouseInput` (cursor speed, scroll speed, cursor graphic); Android Back = `KeyCode.Escape`; `OnApplicationFocus` / `OnApplicationPause` | Names: Keyboard Type, Character Limit, Cursor Speed, Scroll Speed, Cursor Graphic; Back stays Escape for compatibility | Docs only, no code |
| WHATWG HTML: `inputmode`, `enterkeyhint`, `autocapitalize`, `autocorrect`, `maxlength` | Attribute names, values and the action defaults of `next`/`previous`/`done` | As browsers on Android/iOS behave |

Ideas only: no code, data, assets or Minecraft-specific mechanics are copied from Craftsman (a port
reconstructed from Mojang's retail binary). Everything is genre-agnostic: "menu" is any document
the author marks.

## 5. Requirements

### Functional A: text entry

- **R1 Traits.** `TextEntryTraits { keyboard, enterKey, capitalize, autocorrect, secure, multiline,
  maxLength, signedNumber }` with `KeyboardType { Text, Number, Decimal, Phone, Email, Url, Search,
  None }`, `EnterKey { Default, Enter, Done, Go, Next, Previous, Search, Send }`,
  `AutoCapitalize { Off, Sentences, Words, Characters }`.
- **R2 From HTML.** For the focused `<input>` / `<textarea>` (compat layer; `.rml` inputs read
  the same attributes):
  - `type`: `email` → Email, `url` → Url, `tel` → Phone, `search` → Search with enter Search,
    `number` → Number when `step` is an integer and `min` ≥ 0, else Decimal with `signedNumber`
    when `min` is absent or negative; `password` → `secure` (and Number keyboard when
    `inputmode="numeric"`); `<textarea>` → `multiline`, enter Enter; other text types → Text.
  - `inputmode` (`none text decimal numeric tel search email url`) overrides the keyboard; `none`
    shows no keyboard (the field still takes hardware keys).
  - `enterkeyhint` (`enter done go next previous search send`) overrides the enter key.
  - `autocapitalize` (`off none on sentences words characters`, `on` = sentences) and
    `autocorrect` (`on off`) apply; absent: Off and off (today's behaviour), except Search
    keyboards which also stay off.
  - `maxlength` → `maxLength` (code points; -1 none).
  Attribute changes while focused re-send the traits (R5). The support tables move these four
  attributes from "ignored" to "native" with the platform notes, and the input-type rows lose
  "no keyboard hints".
- **R3 Action key.** The platform's action key arrives as an Enter key press on the focused field
  (`keydown` with `key == "Enter"`, plus `event.enterKeyHint`), cancelable. Unless prevented, the
  default action follows the hint: `next` / `previous` focus the next / previous focusable text
  field of the document in tab order (none: blur), `done` blurs (the keyboard closes), `go` /
  `search` / `send` / `enter` / Default on an `<input>` do implicit form submission (the field's
  form's `submit`, as Enter does today) and blur; on a `<textarea>` Enter / Default inserts a line
  break. Hardware Enter keeps today's behaviour.
- **R4 One source of truth.** The engine's field value is authoritative. Edits from the platform
  arrive in order as `delete N code points before the caret, insert text` (plus caret moves and
  composition), applied through the field's normal input path, so `maxlength`, the number filter and
  `input`/`beforeinput` listeners that change the value apply. After applying a batch, and whenever
  the value or caret changes for another reason (a script sets `value`, a reset, focus moves to a
  field with other text), if the value or caret differs from what the platform was last told,
  the engine sends `System::updateVirtualKeyboardText(text, selection)` once that frame; the
  platform replaces its buffer and resets its composition. Android and iOS; the web has nothing to
  resync (its textarea holds no text between commits).
- **R5 Lifecycle.** Focusing a field calls `System::showVirtualKeyboard(text, traits)`; moving focus
  to a field with other traits while the keyboard is open applies the new traits (Android restarts
  input; iOS reloads input views; the web rewrites the textarea attributes) without hiding it;
  blurring hides it as today. `inputmode="none"` hides it.
- **R6 Lua.** `System.showVirtualKeyboard(text, options?)` with `options = {keyboard, enterKey,
  autocapitalize, autocorrect, secure, multiline, maxLength}` (strings as the HTML values);
  without options exactly today's call. `System.updateVirtualKeyboardText(text, caret?)`.
  Hand-written stub `engine/core/script/binding/stubs/TextEntry.lua` declares
  `VirtualKeyboardOptions`; `HtmlUI.lua` documents `event.enterKeyHint`.
- **R7 Component InputField.** New fields Keyboard Type (Unity's `TouchScreenKeyboardType` subset:
  Default, Number Pad, Decimal Pad, Phone Pad, Email Address, URL, Search; maps to R1) and Character
  Limit (0 = none; enforced on typing and paste, as Unity). `password` sets `secure`. Its Submit
  (`onSubmit`) fires on the action key.

### Functional B: gamepad cursor

- **R8 Activation.** The cursor is **active** while all hold: the current input scheme is Gamepad
  (`input-device-awareness.md`), a shown, interactive, Screen-mode UIDocument with Gamepad Cursor on
  exists in an input scene, and `GamepadCursor.enabled` (default true). It becomes visible at the
  focused element's centre if one is focused, else at its last position this session, else the
  canvas centre. Inactive, it is hidden, and a press it holds is cancelled (`pointercancel`, no click).
- **R9 Movement.** Left stick past the deadzone (default 0.2, radial) moves it at
  `speed × curve(|stick|)` canvas px/s with `curve(t) = t²` (fine aim near the centre), ramping from
  0 to full speed over Acceleration Time while the stick stays deflected; over an element that
  takes pointer input (a button, link, form control, `[onclick]`/listener for `click`/`pointerdown`,
  a component UI Button) the speed is multiplied by Slow Over Controls. Clamped to the canvas.
  Movement is frame-rate independent (unscaled frame time) and works at time scale 0.
- **R10 Clicks and drags.** A down → pointer down at the cursor, A up → pointer up (click when on the
  same element), so holding A and moving drags (ranges, scroll boxes, S17 drag-and-drop sources,
  `setPointerCapture` sliders). Events reach UI documents, the component UI and documents on meshes
  in the mouse's order with `pointerType "mouse"` and the mouse's pointer id, `:hover` follows the
  cursor. They never reach `Input` (no `Input.getMouseButton`, no mouse position change), the
  game's `Engine.onMouse*` / `onTouch*` events, or the scheme tracker.
- **R11 Scroll.** Right stick past the deadzone scrolls at the cursor: vertical and horizontal wheel
  deltas at `scrollSpeed × curve(|axis|)` lines/s, through the wheel path.
- **R12 D-pad and focus.** While active, d-pad presses (with today's repeat 0.4 s / 0.1 s) run the
  existing spatial navigation, starting from the focused element or, when none, from the element
  under the cursor; the cursor then jumps to the new focus's centre (no animation) and `:focus-visible`
  shows. With nothing in that direction, nothing moves. A focus set by a script or `autofocus`
  while active also moves the cursor.
- **R13 Consumption.** While active, the left stick, right stick, d-pad, A and B are the UI's: the
  game sees their rest values and no `onGamepadButtonDown` for them, and input actions with
  `ignoreWhenUIConsumes` skip them (`InputSuppression::gamepadCursor`, new). X, Y, shoulders,
  triggers, Start, Back and stick presses go on as today (to a focused element as keys, else the
  game). B produces Back (part C).
- **R14 Drawing.** The cursor is the project's Cursor Graphic (a texture; default a built-in 32 px
  arrow, engine-authored, CC0) at Cursor Size CSS px with its hot spot at the graphic's top-left
  (Cursor Hot Spot setting, default (0,0)), drawn above every document and the component UI in the
  screen pass, never hit-tested.
- **R15 Lua.** `GamepadCursor.isActive()`, `GamepadCursor.position` (get; set warps, also while
  inactive for the next activation), `GamepadCursor.enabled`, `GamepadCursor.speed` and
  `scrollSpeed` (runtime overrides of the project values, reset at Play stop), `UIDocument.gamepadCursor`.

### Functional C: Back and the pause gate

- **R16 Sources.** `Engine::systemBack(source)`: Android Back (the existing callback, after native
  plugins), and on every platform the keyboard Escape key down and gamepad B down (any pad), plus
  `send_input app: back`. Repeats do not produce Back.
- **R17 Route.** On Back:
  0. If the document the key would reach today (the one holding the keyboard or gamepad focus,
     menu or not) has an open `<select>` or modal `<dialog>`, today's Escape / B handling closes it
     and the press ends there: it is not a Back (no `back` event, no `Engine.onBack`).
  1. The **top menu document** (D10) gets a `back` event (`Event`, bubbles, cancelable) at its
     focused element, else its root. If no listener prevents it, its default action closes an open
     `<select>`, else the topmost open modal `<dialog>` of that document. Prevented, or a default
     action that closed something: **consumed**.
  2. Not consumed: `Engine.onBack(source)` fires (Lua `Engine.onBack = function(source) end`;
     source `"back_button"`, `"escape"`, `"gamepad"`, `"agent"`). With at least one subscriber,
     Back is consumed by the game.
  3. Then the source's own path runs unless consumed: Escape and B keep today's routing (the
     focused document's keydown, then the game); Android Back presses Escape as today
     (`NativeEngine.cpp:1510-1520`), so a game written for "Back = Escape" keeps working.
  A consumed Escape or B is not delivered as a key to documents or the game for that press (its
  up neither).
- **R18 Is Menu.** UIDocument "Is Menu" (`isMenu`, default false). `Engine.isMenuShowing()` is true
  while any shown, interactive UIDocument with Is Menu exists in an input scene (cached, updated
  when a document's visibility, `enabled`, `interactive` or `isMenu` changes).
- **R19 Menu opening releases the game.** When `isMenuShowing` turns true, in the next input latch:
  every key, mouse button and gamepad button the game holds comes up (their up edges in the next
  window), fingers the game holds are reported Canceled, pointers held by non-menu documents and
  the component UI are cancelled (on-screen controls release their actions), and virtual sources of
  kind Control whose owner is not a menu document are cleared. Presses held by the menu document
  itself are untouched. Nothing is replayed when the menu closes: a key still physically held
  needs a new press (as after a focus loss, `input.md:84-87`).
- **R20 Pause On Focus Loss.** Project Settings > Player > "Pause On Focus Loss" (default off).
  When on and `isMenuShowing()` is false, `Engine.onPauseRequested(reason)` fires once on each of:
  focus loss (`"focus_lost"`), an app pause not preceded by a focus loss (`"app_paused"`), and the
  disconnect of the pad that holds the Gamepad scheme (`"gamepad_disconnected"`). It fires after
  `onFocus(false)` / `onPause` (or after the disconnect's own event), on the main thread. With the
  setting on and no subscriber at Play start, one warning names the event. A game opens its pause
  menu there; the engine changes no time scale.
- **R21 Lua.** `Engine.onBack`, `Engine.onPauseRequested`, `Engine.isMenuShowing()`,
  `UIDocument.isMenu`; the `back` event documented in `HtmlUI.lua` and `html-ui.md`. Static events
  are cleared at Stop (`tools/check_event_clears.py`).

### Platform & backends

- **R22 Android.** Per-field `EditorInfo` (D4) through `GameActivity_setImeEditorInfo` and an input
  restart; the action key through games-activity 3.0's `onEditorAction` (the dependency moves from
  2.0.2 to the 3.0 line), delivered as R3's Enter; resync through `GameActivity_setTextInputState`.
  Back unchanged in Java; `handleGameActivityInput` calls `Engine::systemBack(BackButton)`, which
  presses Escape when not consumed.
- **R23 iOS.** The hidden `UITextField` holds the field's real text and caret (sentinel removed),
  traits mapped (D4), edits diffed by the shared diff (D3); `secure` uses `secureTextEntry`;
  Return goes through R3. Hardware keyboards keep `pressesBegan`. No Back button.
- **R24 Web.** The textarea gets `inputmode`, `enterkeyhint`, `autocapitalize` per field
  (`autocorrect`/`spellcheck` stay off: no mirrored text, documented); `insertLineBreak` from a soft
  keyboard with a non-Enter hint is R3's action key. Back: Escape and B only.
- **R25 Desktop.** Windows, macOS, Linux: traits are inert (no soft keyboard; R3 applies to
  hardware Enter only as today); the cursor and Back work. All five graphics backends draw the cursor
  through the existing UI pass (one textured quad, no new shader).

### Performance (release; mobile and web first)

- **R26 Budgets.**
  - Text entry: no per-frame cost while no field is focused; per edit batch O(field length):
    ≤ 50 µs for a 4,096 code point field on the mobile reference device; at most one resync per
    frame; zero allocations per frame while a focused field is idle.
  - Cursor: off (no Gamepad Cursor document, or scheme not Gamepad): one cached boolean per frame,
    0 allocations, nothing drawn. Active and moving: ≤ 20 µs per frame desktop, ≤ 60 µs mobile (one
    hit test, one pointer move, one transform), one extra draw call.
  - Back: work only on a press. `isMenuShowing`: O(1) read; the cache updates on visibility change
    events, never by scanning per frame. R19's release: on the transition only.
  - Binary size: ≤ 24 KB added to the release player on Android arm64 and wasm (measured by the
    S34 size report).

### Editor UX (human)

- **R27 Inspector.** UI Document gets "Is Menu" ("Back goes here first; opening it releases the
  game's held input; Pause On Focus Loss does not fire while it shows") and "Gamepad Cursor" ("While
  the player uses a gamepad, the stick moves a pointer over this document"), both undoable through
  the reflection rows (`PropertyCmd`). InputField gets "Keyboard Type" and "Character Limit".
- **R28 Project Settings.** Input > "Gamepad Cursor": Cursor Speed (canvas px/s at a 1080 px tall
  canvas, scaled by canvas height; default 1000), Acceleration Time (0.15 s), Slow Over Controls
  (0.5), Scroll Speed (lines/s, 20), Deadzone (0.2), Cursor Graphic (texture, default built-in),
  Cursor Size (32 CSS px), Cursor Hot Spot ((0, 0)). Player > "Pause On Focus Loss". Each row is in
  the settings search table (`SettingsRows.cpp` pattern) and undoable as other project settings.
- **R29 Game view.** During Play, while a field is focused, a chip at the bottom of the Game view
  reads "Keyboard: Email · Send · max 32 · Sentences" (the traits the device would get), clickable to
  copy the traits as JSON. Escape in the focused Game view is Back (R16), so the Android Back
  route is testable on the desktop.

### Agent UX (MCP, Lua, CLI)

- **R30 MCP, no new tool.**
  - `inspect_ui` adds, when present, `focus: {ref, keyboard, enter_key, autocapitalize, autocorrect,
    secure, multiline, max_length, keyboard_open}` and `gamepad_cursor: {active, x, y, over}` (`over`
    = ref of the element under it) to its result; no schema change.
  - `send_input`: `app` takes `back` (result per event adds `back: "document"|"game"|"escape"|"key"`
    naming who consumed it); a `gamepad` event takes `x, y` or `target` to warp the cursor before its
    button or axis (error when no Gamepad Cursor document is shown: "no document has Gamepad Cursor
    on: set_properties UIDocument gamepadCursor true").
  - `get_play_state` adds `menu_showing`.
  - Each new string has an eval case in `tools/mcp/eval/cases.py`; `tool_budget.py --check` passes;
    `TOOLS.md` regenerated.
- **R31 Docs.** `docs/mcp/guide/html-ui.md` "Touch, text fields and the cursor" gets the traits
  table and R3; a "Gamepad cursor" subsection under the gamepad section; a "Back and menus"
  section. `docs/mcp/guide/input.md` and `platform-services.md` (focus table) get Back, Is Menu
  and `onPauseRequested`. Support tables regenerated (`gen_html_ui_support.py`).

### Data & compatibility

- **R32 Additive only, no version bump.** `UIDocumentComponent.isMenu`, `.gamepadCursor`
  (OmitIfDefault, default false); `InputFieldComponent.keyboardType` (default Default),
  `.characterLimit` (0); project keys `input.gamepad_cursor.*` and `player.pause_on_focus_loss`
  (defaults above). Old scenes and projects load with defaults and save unchanged (keys omitted
  at default). Exports carry the settings as the deadzone is (`editor/Project.cpp:8498, 10712`).
- **R33 Behaviour changes, deliberate.** HTML fields with `type=email|url|tel|number|search` or the
  four attributes get the matching keyboard on Android, iOS and the web (they had a text keyboard).
  An Escape or B press that a `back` listener prevents no longer reaches `keydown` listeners or the
  game (only when the author added such a listener). Tests pinning "attribute ignored" diagnostics
  (`test_html_ui_checks.py`, the support table snapshots) change in the same commit and say so.

## 6. Design constraints & decisions

**D1 Traits live in core.** `engine/core/input/TextEntry.h`:

```cpp
namespace jasperengine {
    enum class KeyboardType : uint8_t { Text, Number, Decimal, Phone, Email, Url, Search, None };
    enum class EnterKey : uint8_t { Default, Enter, Done, Go, Next, Previous, Search, Send };
    enum class AutoCapitalize : uint8_t { Off, Sentences, Words, Characters };
    struct TextEntryTraits {
        KeyboardType keyboard = KeyboardType::Text;
        EnterKey enterKey = EnterKey::Default;
        AutoCapitalize capitalize = AutoCapitalize::Off;
        bool autocorrect = false;
        bool secure = false;
        bool multiline = false;
        bool signedNumber = false;
        int maxLength = -1;
        bool operator==(const TextEntryTraits&) const = default;
    };
    // the HTML names ("email", "send", "words"): Lua options, MCP, the web textarea
    const char* keyboardTypeName(KeyboardType); bool keyboardTypeFromName(std::string_view, KeyboardType&);
    const char* enterKeyName(EnterKey);         bool enterKeyFromName(std::string_view, EnterKey&);
    const char* autoCapitalizeName(AutoCapitalize); bool autoCapitalizeFromName(std::string_view, AutoCapitalize&);
}
```

`System` gains (base implementations call the old ones, so platforms without support compile):

```cpp
virtual void showVirtualKeyboard(const std::wstring& text, const TextEntryTraits& traits);
virtual void updateVirtualKeyboardText(const std::wstring& text, int selectionStart, int selectionEnd);
```

`RmlTextInputHandler::flushKeyboard` derives the traits with
`htmlui::textEntryTraitsOf(Rml::Element*)` (`engine/core/htmlui/TextEntryTraits.cpp`, the R2 table)
and calls the new overload; it also tracks the last sent text/selection for R4.

**D2 The action key is Enter.** No new DOM event type: the platform calls
`Engine::systemEditorAction(EnterKey)`, which presses Enter on the focused field with
`event.enterKeyHint` set; the default action of R3 lives in the compat text-input behaviour, so a
hardware Enter on a field with `enterkeyhint="next"` also moves focus (as Chrome on Android does
with a Bluetooth keyboard; documented).

**D3 One diff, host-tested.** The Android document diff (`NativeEngine.cpp:378-503`) moves to
`engine/core/input/TextInputSync.{h,cpp}`: a class holding the engine-side text, caret and
composition that turns a platform snapshot `(text UTF-16, selection, composing region)` into calls
on an `Output` interface (`moveCaret`, `backspace`, `insertText`, `compose`, `cancelComposition`)
and exposes `reset(text, caret)` for R4. Android and iOS call it; `AndroidTextInput.h` keeps only
the modified UTF-8 conversions. Behaviour is kept bit-for-bit for the cases the Android code
handles (AC2 pins them before the move).

**D4 Platform mappings are pure functions with tests on Linux.**
`engine/platform/android/AndroidTextInput.h`: `editorInfoOf(traits) -> {inputType, imeOptions}`
(`TYPE_CLASS_TEXT|NUMBER|PHONE`, variations `EMAIL_ADDRESS`, `URI`, `PASSWORD`,
`NUMBER_PASSWORD`; flags `CAP_SENTENCES|WORDS|CHARACTERS`, `AUTO_CORRECT` or `NO_SUGGESTIONS` for
secure, `MULTI_LINE`, `NUMBER_FLAG_DECIMAL|SIGNED`; actions `UNSPECIFIED`/`NONE`/`DONE`/`GO`/`NEXT`/
`PREVIOUS`/`SEARCH`/`SEND`, always `IME_FLAG_NO_FULLSCREEN | IME_FLAG_NO_EXTRACT_UI`).
`engine/platform/apple/ios/TextTraitsMap.h` (no UIKit include; the UIKit enum values as constants
with a `static_assert` in `EngineView.mm`): `keyboardType`, `returnKeyType`,
`autocapitalizationType`, `autocorrectionType`, `secureTextEntry`; `KeyboardType::None` sets an
empty `inputView`. `engine/platform/emscripten/WebKeyMap.h` style header `WebTextEntry.h`:
the textarea attribute strings.

**D5 Resync guard.** The engine tells the platform only when the field and the platform's last
snapshot disagree after a batch, never echoes an edit the platform just made (compare, then send),
and never while a composition is active unless the field rejected composed text (then the
composition is cancelled and the text sent). This stops the IME-reset loop the Craftsman bridge
guards with sequence numbers; GameTextInput and UITextField deliver state snapshots, so sequence
numbers are not needed (decided).

**D6 Cursor home.** `engine/core/subsystem/GamepadCursor.{h,cpp}`: a static state updated in
`Engine::systemDraw` after `Input::beginFrame` (it needs the latched axes and the scheme) and before
the scenes' UI updates. Its pointer events go through the engine's pointer dispatch
(`dispatchToDocuments`, the component UI, `dispatchWorldPointer`) with a `fromCursor` flag that
skips `Input::*`, `Engine.onMouse*` and the scheme tracker (R10). A, B and the sticks reach it
before `gamepadKeyDown` routes them (`Engine.cpp:3566`), so while active they never become document
keys except through R12's navigation.

**D7 Cursor drawing.** The screen context gets an engine overlay document
(`@htmlui/gamepad-cursor.rml`, virtual file) with `z-index` above every document's sort order and
`pointer-events: none`, holding one `<img>` positioned by `transform`; loaded the first time the
cursor activates in a session, hidden when inactive. The component UI is drawn before the screen
documents in the camera pass, so one overlay covers both (inferred from the draw order of
`UIDocumentSystemRender.cpp`; verified by AC15's golden).

**D8 The cursor drives the mouse pointer of documents.** Hover needs a pointer that exists while
not pressed: touch pointers have hover only while down (`html-ui.md:1588`). Sharing the documents'
mouse pointer is safe because the cursor is visible only in the Gamepad scheme; a real mouse
movement switches the scheme, deactivates the cursor (its press cancelled) and the mouse continues
from where it is.

**D9 Back entry point.**

```cpp
enum class BackSource : uint8_t { BackButton, Escape, Gamepad, Agent };
// Engine.h
static bool systemBack(BackSource source);   // true: consumed (document or Engine.onBack)
static FunctionSubscribe<void(const std::string& source)> onBack;
static FunctionSubscribe<void(const std::string& reason)> onPauseRequested;
static bool isMenuShowing();
```

`Engine::systemKeyDown` (Escape, not repeat) and `systemGamepadButtonDown` (B) call
`systemBack` first; when it returns true they record the press as UI-held and stop (their up is
swallowed too). Android's `handleGameActivityInput` calls `systemBack(BackButton)` and presses
Escape only when it returns false. Escape synthesised for Android Back is tagged `Synthetic`
(`input-device-awareness.md` D2) and does not call `systemBack` again.

**D10 Top menu document.** Among input scenes in input order (top layer first), the first scene
with a shown, interactive Is Menu document; in it, the highest `sortOrder` (ties: the later loaded).
Texture-mode documents can be menus (a diegetic screen) and count for `isMenuShowing`; they get
`back` only when they hold the keyboard focus (`getTextureFocus`).

**D11 Pause request ordering.** `Engine::systemFocus(false)` → `onFocus(false)` → (setting on, no
menu) `onPauseRequested("focus_lost")`; `systemPause()` without a preceding focus loss →
`onPause` → `onPauseRequested("app_paused")`; a later pause after a focus loss does not fire again
until focus came back. The gamepad disconnect fires from `systemGamepadDisconnect` when that pad was
`lastGamepad` and the scheme is Gamepad.

## 7. Acceptance criteria

| AC | Req | Verification (red: what fails without the change) |
|---|---|---|
| AC1 | R1, R2 | `TextEntryTest.TraitsFromAttributes` (`engine/tests/htmlui/TestTextEntry.cpp`): a table of 24 fields (each `type`, `inputmode`, `enterkeyhint`, `autocapitalize`, `autocorrect`, `maxlength`, textarea, number with negative min) → expected `TextEntryTraits`; `TestSystem` records the traits passed to `showVirtualKeyboard` on focus. Red: the old call has no traits |
| AC2 | D3 | `TextInputSyncTest` (`engine/tests/core/input/TextInputSyncTest.cpp`), written first against the current Android function (moved verbatim): suggestion "Wh" → "What" inserts "at" only; "teh" → "the" deletes 2, inserts "he"; a composition updated, committed, cancelled; an edit away from the caret moves it; emoji and surrogate pairs count one code point. Passes before and after the move; `AndroidTextInputTest` unchanged |
| AC3 | R4, D5 | `TextEntryTest.ResyncAfterRejectedText`: `maxlength=5`, platform snapshot "abcdefg" → field "abcde", one `updateVirtualKeyboardText("abcde", 5, 5)`; a script sets `value = "x"` → one resync; an accepted edit → none (no echo). Red: no resync calls |
| AC4 | R3, D2 | `TextEntryTest.EnterKeyHintActions`: `systemEditorAction(Next)` moves focus to the next text field and re-sends its traits without hiding; Previous back; Done blurs and hides; Send submits the form once (`submit` listener marker); `preventDefault` on keydown stops all; textarea Enter inserts "\n" |
| AC5 | R5 | `TextEntryTest.TraitsChangeWithoutHiding`: focus email then number field: `TestSystem` sees show(email), show(number), no hide between; `inputmode="none"` hides |
| AC6 | D4, R22-R24 | `AndroidTextInputTest.EditorInfoOfTraits`, `IosTextTraitsTest.MapOfTraits` (`engine/tests/core/input/`), `WebKeyMapTest.TextEntryAttributes`: every traits combination of AC1's table maps to the documented platform values (e.g. Email+Send → `0x21`, `IME_ACTION_SEND|NO_FULLSCREEN|NO_EXTRACT_UI`) |
| AC7 | R22 | `tools/android-emulator-test.sh --ime` with S22's `tools/test-projects/input` extended by a form document: `adb shell dumpsys input_method` shows `inputType=0x21 imeOptions=0x12000004` for the e-mail field, `adb shell input text` beyond `maxlength` leaves the field (logged `[gate] value`) and the IME buffer (`dumpsys`) at 5 characters; the IME action logs `[gate] submit` |
| AC8 | R7 | `UIComponentsTest.InputFieldKeyboardTypeAndLimit` (`editor/tests/UIComponentsTest.cpp`): Keyboard Type Email Address reaches `showVirtualKeyboard` as Email; Character Limit 4 keeps "abcd" after typing and pasting "abcdef"; action key fires `onSubmit` once; old scenes load with defaults |
| AC9 | R8, R9, R13 | `InputGamepadTest.CursorMovesAndConsumes` (`engine/tests/htmlui/TestInputGamepad.cpp`): Gamepad scheme + a Gamepad Cursor document: stick at 1.0 for 1 s of frames moves 1000 px (× canvas/1080) ± 2 %, at 0.5 moves a quarter as fast, ramps over 0.15 s, halves over a button; the game's `onGamepadAxisMove` sees rest and `Input.getGamepadAxisForGame` 0; scheme Keyboard&Mouse → cursor hidden. Red: no cursor |
| AC10 | R10, D8 | `InputGamepadTest.CursorClicksHoversDrags`: hover class on a button under the cursor, A click fires `click` once, A held drags a range thumb to a new value and a `setPointerCapture` slider; `Input.getMousePosition()` and `Input.getMouseButton(0)` unchanged; the scheme stays Gamepad; `Engine.onMouseDown` never fires |
| AC11 | R11, R12 | `InputGamepadTest.CursorScrollsAndJumpsToFocus`: right stick scrolls a list by `scrollSpeed` lines/s ± 5 %; d-pad down moves focus to the next button and the cursor to its centre; `el:focus()` from Lua moves the cursor too |
| AC12 | R8 | `InputGamepadTest.CursorCancelsOnDeactivate`: A held over a button, mouse moved 20 px (scheme → Keyboard&Mouse): `pointercancel`, no `click`, cursor hidden; document hidden: same |
| AC13 | R10 | `UIMultiPointerTest.GamepadCursorPressesComponentButton` (`editor/tests/UIMultiPointerTest.cpp`): the cursor clicks a component UI Button (`onClick` once) and drags an On-Screen Stick with A held |
| AC14 | R26 | `InputGamepadTest.CursorZeroCostWhenOff`: counting allocator over 600 frames with a pad moving and no Gamepad Cursor document: 0 allocations, overlay document never loaded; `InputBenchTest` line `cursor_frame_us` gated in `tools/perf/bench-thresholds.json` (20 µs desktop with slack) |
| AC15 | R14, D7 | `InputGamepadTest.CursorDrawnOnTop` with golden `gamepad_cursor.png` (glcore, matched on Vulkan; built-in graphic, then a project texture at 48 px with hot spot (24, 24)) drawn over a document and a component Image |
| AC16 | R16-R17, D9 | `DocumentInputTest.BackRouting*` (`engine/tests/htmlui/TestInputDocument.cpp`): an open `<select>` in a non-menu document closes on Escape with no `back` event (step 0); `back` reaches the top menu's focused element and bubbles; `preventDefault` → consumed, no Escape keydown in any document, `Engine.onBack` not called; an open modal `<dialog>` closes and consumes; no `back` listener: with an `Engine.onBack` subscriber it gets `"escape"` and no document or game sees the keydown, without one today's Escape keydown runs; B with no menu and no subscriber → today's routing exactly (existing `InputGamepadTest.CancelWithB` and `BClosesAModalDialog` pass unchanged) |
| AC17 | R17, R22 | `DocumentInputTest.BackRoutingAndroidKeepsEscape`: `systemBack(BackButton)` with no menu and no `Engine.onBack` subscriber presses Escape (key down then up next frame, as `NativeEngine.cpp:1510-1520`); with a subscriber it does not; the synthetic Escape does not route Back twice |
| AC18 | R18, R19 | `PlatformServicesTest.MenuOpeningReleasesGameInput` (`editor/tests/PlatformServicesTest.cpp`): W held and a finger on an HTML on-screen stick of a HUD document; showing an Is Menu document → next frame `getKeyUp(W)`, the finger Canceled, `Horizontal` back to 0; a button held in the menu document keeps its press; `isMenuShowing` follows show/hide/`enabled`/`interactive` |
| AC19 | R20, D11 | `PlatformServicesTest.PauseRequestedOnlyWithoutMenu`: setting on: `systemFocus(false)` → `onPauseRequested("focus_lost")` once after `onFocus(false)`; the following `systemPause` adds nothing; resume + focus, then `systemPause` alone → `"app_paused"`; with a menu showing → nothing; setting off → nothing; disconnecting the Gamepad-scheme pad → `"gamepad_disconnected"`; one warning without a subscriber |
| AC20 | R30 | `tools/mcp/tests/test_html_ui_input.py` (existing, `CI = True`): `test_text_entry_traits`: focus an e-mail field via `send_input` tap → `inspect_ui.focus.keyboard == "email"`, `enter_key == "send"`, `max_length == 32`; `test_html_ui_gamepad.py` (existing): `test_gamepad_cursor`: `send_input` gamepad axis moves `inspect_ui.gamepad_cursor.x`, a `gamepad` a event with `target` clicks (console marker), the error text when no document has Gamepad Cursor; `test_back_route`: `send_input app back` returns `back == "document"` with a listener, `"game"` with `Engine.onBack`, `"escape"` otherwise; `get_play_state.menu_showing` follows. `tool_budget.py --check`, `gen_tool_docs.py --check` pass |
| AC21 | R27-R29, R32 | `UIDocumentEditorTest.MenuAndCursorRows` (`editor/tests/`): Is Menu and Gamepad Cursor through `PropertyCmd` set and undo; old scenes load and save byte-identical; catalog snapshot and `ReflectParity` golden gain the two keys; the project settings rows round-trip through `set_project_settings` and exports (`ExportSettingsTest` reads them in the generated game); editor screenshots of the Inspector rows, the settings group and the keyboard chip (`tools/editor-shot.sh`) in the PR |
| AC22 | R25, R26 | Web gate `GATE=input` gains a form: CDP focus of the e-mail field sets the textarea's `inputmode="email"` and `enterkeyhint="send"` (`eval`), Escape with a `back` listener logs `[gate] back`; `tools/build.sh web`, the Android build and an iOS simulator build pass; the S34 size report shows ≤ 24 KB growth |
| AC23 | R6, R21, R31 | `python3 -m lua_api doctest` passes; `jasper://lua-api?class=GamepadCursor` and the new `Engine`/`System`/`UIDocument` members are listed; `test_docs.py` and `gen_html_ui_support.py --check` pass |

Red controls are run on the base commit first and named in the PR.

## 8. Dependencies & conflicts

- **Depends on:** `input-device-awareness.md` for the current scheme (R8), the `Synthetic` source tag
  (D9) and the scheme tracker's rule that cursor events are not evidence. Part A and part C can
  land before it; part B cannot. The HTML UI IME handler and focus navigation (landed).
- **Related:** `game-ui-localization.md` (right-to-left text and IME languages use part A's sync;
  the keyboard chip text is editor-only English); `input-recorder-and-input-scripts.md` (records
  `systemBack`, editor actions and the IME edits as events so replays reproduce them);
  `savegame.md` (games typically save in `onPauseRequested`); `shipped-game-diagnostics.md` (may log
  Back routes); `dialogue-graphs.md` (dialogue boxes are natural Is Menu documents with Gamepad
  Cursor off and focus navigation on).
- **Merge hotspots:** `engine/core/Engine.{h,cpp}` (`systemKeyDown`, `systemGamepad*`,
  `systemFocus`, `systemPause`, pointer dispatch; also the sibling spec's evidence hooks),
  `engine/platform/android/NativeEngine.cpp` (text input, Back, the sibling's device ids),
  `engine/platform/apple/ios/EngineView.mm`, `engine/platform/emscripten/JasperEngineWeb.cpp`,
  `engine/core/htmlui/RmlTextInputHandler.cpp`, `engine/core/subsystem/UIDocumentSystemInput.cpp`,
  `engine/core/component/UIDocumentComponent.h`, `editor/reflect/components/UIReflect.cpp`,
  `editor/settings/SettingsRows.cpp`, `editor/mcp/tools/PlayTools.cpp`, `editor/mcp/tools/HtmlUiTools.cpp`,
  `engine/workspaces/androidstudio/app/build.gradle` (games-activity), `docs/mcp/guide/html-ui.md`,
  `tools/mcp/budget.json`, `editor/tests/reflect/catalog_snapshot.json`.

## 9. Risks & defaults

| Risk / open question | Default taken |
|---|---|
| games-activity 3.0 changes other behaviour (insets, input buffers) | The bump lands first as its own commit in this spec's PR, with the emulator suite (`tools/android-emulator-test.sh`) green before part A builds on it. If it cannot land, R3 on Android falls back to the Enter key the IME sends (Deviation) |
| Restarting Android input on every focus change flickers the keyboard | Restart only when the traits differ (`operator==`); same traits keep the connection and only reset the text |
| iOS mirror model regresses CJK composition handled by the sentinel code | AC2's composition cases run through the shared diff; an iOS simulator manual pass with Japanese and Chinese keyboards is listed in the PR; the sentinel code is removed only after it passes |
| Autocorrect replaces text across the caret in ways the diff turns into many arrow keys | The diff already moves the caret per code point (`NativeEngine.cpp:367-376`); fields up to 4,096 code points stay within R26; longer fields are rare in games |
| A `back` listener on a HUD document | Only Is Menu documents get `back`; a HUD that wants Back uses `Engine.onBack` |
| Games that rely on Escape reaching them while a menu with a back listener is open | Only when the author prevents `back`; documented in R33 |
| Gamepad Cursor and focus navigation both on one document | Both work: stick moves the cursor, d-pad moves focus and the cursor follows (R12); a document wanting only focus navigation leaves Gamepad Cursor off |
| Cursor too fast on small or large canvases | Speed scales with canvas height (R28); `GamepadCursor.speed` for a settings slider |
| Pause On Focus Loss on desktop when the player alt-tabs | Fires (that is its purpose); off by default |
| A pad disconnect while a menu shows | No request (the menu already covers the game) |

## Deviations

- Parts B/C: `send_input` reports who took an `app: back` in one top-level `back` field (the last
  such event of the call), not inside a per-event list, which `send_input` does not have.
- Part C (R19): a menu appearing releases the keys, mouse and gamepad buttons whose press went to
  the game (a press a document or the component UI took stays held in `Input`); virtual sources of
  kind Control are released through their pointers' cancel (every on-screen control clears its
  source on `pointercancel`), so a Control source no pointer holds stays.
- Part C: `Engine.isMenuShowing()` and `GamepadCursor.isActive()` are Lua functions, as the spec
  names them, although other bound getters are properties.
- Part B (D6): the cursor updates after the input latch of a frame that runs its update (the
  latched sticks and scheme); while the app or the editor's Play is paused it neither moves nor
  activates.
- Part B (AC13): the component-UI test is `InputGamepadCursorTest.GamepadCursorPressesComponentButton`
  in the htmlui suite (a Gamepad Cursor document must show, and the editor test binary has no HTML
  documents): A clicks a component Button once and drags a component On-Screen Stick.
- Part B (AC14): the gtest bench lines print `cursor_off_frame_ns` (core `InputBench.GamepadCursorOffFrame`,
  htmlui `CursorZeroCostWhenOff`: 0 allocations), `cursor_frame_us` (`CursorFrameBench`) and
  `text_entry_idle_frame_ns` (`TextEntryTest.IdleFocusedFieldDoesNotAllocate`: 0 allocations), enforced
  with JASPER_TEST_PERF_SLACK like the other InputBench lines; `tools/perf/bench-thresholds.json`
  gates the je-bench-* binaries only, so no row there. Active and moving, the cursor's frame measured
  9.0 us (budget 20 us desktop) with 26.8 allocations a frame: those of RmlUi's own mouse move (26.9
  along the same path, recorded in `docs/html-ui/FORK.md`); the cursor adds none (checked), and a
  still or inactive cursor makes none.
- Part B (AC15): goldens `gamepad_cursor.png` and `gamepad_cursor_texture.png` (glcore,
  `FullPathGoldenTest.GamepadCursorDrawnOnTop`); not compared on Vulkan.
- Tests: the Back tests are `DocumentBackTest.*` (an HTML fixture of their own in
  `TestInputDocument.cpp`), the cursor tests `InputGamepadCursorTest.*`, AC18 and the menu clause
  of AC19 are in the htmlui suite (`DocumentBackTest.MenuOpeningReleasesGameInput`,
  `PauseRequestedOnlyWithoutMenu`, `PauseOnFocusLossWarnsWithoutSubscriber`; the editor tests have
  no HTML documents), the rest of AC19 is `PlatformServicesTest.PauseRequestedOnlyWithoutMenu`;
  AC20's cursor and Back cases are one MCP test, `test_gamepad_cursor_and_back_route`; AC21's
  persistence is `StreamComponent.UIDocumentMenuAndCursorRoundTrip` and
  `PresentationSettings.PauseOnFocusLossAndGamepadCursorRoundTrip` (the two Inspector rows use the
  generic reflection `PropertyCmd` path of their neighbours; not tested on their own).
- Part A: iOS keeps its sentinel text field (R23 in part). The risk table removes the sentinel code
  only after an iOS simulator pass with Japanese and Chinese keyboards, which no machine of this
  implementation could run. The hidden field gets the field's keyboard type, Return key,
  capitalisation, secure entry and `inputmode="none"` (an empty `inputView`), and the keyboard's
  Return is R3's action key. The field holds no text, so iOS has no resync
  (`keepsVirtualKeyboardText` is false) and autocorrect stays off, as on the web. The shared diff
  (`TextInputSync`) is ready for the mirror model.
- Part A: the shared diff's state lives in core, not in each platform: `Engine::systemTextInputState`
  (a platform keyboard's snapshot) and `Engine::resetTextInputState` (the keyboard was given
  text) hold one `TextInputSync`, and `RmlTextInputHandler` compares the field with the snapshot.
  `System::keepsVirtualKeyboardText()` (Android true) gates R4, so platforms whose keyboard keeps no
  text (desktop, web, iOS here) never resync. `Engine::getTextEntryFocus` serves the Game view chip.
- Part A: a blur hides the soft keyboard at the document system's next update in the same frame,
  not inside the blur, so focus moving between fields (a click, `el:focus()`, Next) shows the next
  field's traits without a hide in between (R5). A field that goes away hides it at once.
- Part A: a hardware Enter on a field without `enterkeyhint` keeps today's behaviour exactly
  (implicit submission, the field keeps the focus, no `event.enterKeyHint`). With `enterkeyhint`,
  and for every action key, the `keydown` carries the hint and the default action runs after the
  listeners, so `preventDefault()` stops it (D2).
- Part A: `event.enterKeyHint` is documented through `tools/lua_api`'s field override of the bound
  `UIEvent` class, not in `HtmlUI.lua`: the generator owns `UIEvent`'s fields, and the fragment
  redeclaring one would duplicate it.
- Part A: names in Lua, MCP and the chip are the `inputmode` values (`numeric` for Number, `tel` for
  Phone); `inspect_ui`'s `focus` is present only while a text field of that document is focused;
  `enter_key` is `"default"` without a hint and `max_length` is null without a limit.
- Part A: games-activity moves to 3.0.5 (the version in the build's Gradle cache). The Android
  build (`ANDROID_ABIS=x86_64 tools/build.sh android`) and the web build compile with it and with
  parts A and C's platform code; the emulator suite (`tools/android-emulator-test.sh --ime`, AC7),
  the web gate's form (AC22) and an iOS build did not run.
- **AC7, AC22 (integration pass).** The form is `tools/test-projects/input`'s `assets/form/form.html` with
  `lua/FormUI.lua`: the game focuses the e-mail field (and makes its document a menu) at the input
  scheme's second switch, since neither the web gate nor `adb` can click inside the canvas's document.
  `GATE=input` then reads the IME textarea's `inputmode="email"` and `enterkeyhint="send"` and presses
  Escape (`[gate] back`). On the emulator, `tools/android-emulator-test.sh --ime` reads the field's
  EditorInfo from `dumpsys input_method` (`inputType=0x21 imeOptions=0x12000004`), types eight
  characters with `input text` (the field keeps five) and presses Enter: `adb` has no way to press
  an input method's action key, so the submit is the hardware Enter path, not `onEditorAction`, and
  the IME's own buffer is not read.
- **iOS (Mac pass).** The iOS builds ran on an arm64 Mac with Xcode 26.6: `tools/ci/macos.sh xcode ios`
  (the engine's Xcode project for the simulator), the prebuilt iOS player (`tools/ci/players-apple.sh ios
  development`, device and simulator slices) and `tools/ci/ios_packaging.sh`, whose game runs on the
  simulator. Typing into a field on the simulator did not run: `simctl` has no way to type or tap, and
  the Simulator window takes keys only from a person (or UI scripting, which needs Accessibility rights
  on that Mac); the Japanese and Chinese keyboard pass that would remove the sentinel did not run either.
