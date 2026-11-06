# UI components: correct Buttons and layouts, Unity Selectable navigation and transitions (design)

Date 2026-11-04. Branch `dc/ui-components`. Program: Doriax catch-up, ledger `docs/doriax-catchup/LEDGER.md`
(feature 7). Dossier: `dc-work/dossiers/ui-components.json`; owner notes: `dc-work/dossiers/EXTRA-FINDINGS.md`
("ui-components").

## 1. Goals

1. The component UI (uGUI-like: Image, Text, Button, Layout Group, InputField) draws and lays out correctly:
   - a Button state without its own texture shows the Normal texture (pressed, disabled, non-interactable
     Canvas Group), and a texture still loading asynchronously is never replaced;
   - a hidden or inactive child of a Layout Group never takes or overwrites a sibling's box;
   - a 9-patch Button never clips its label to the band between its patch margins;
   - `Engine.setCanvasSize` / `Engine.scalingMode` at runtime update the cameras, the view rect, the Canvas
     Scaler and the safe area at once;
   - raising a Text's `maxTextSize` in the Inspector or over MCP grows its GPU buffers, and no UI element ever
     uploads more bytes than its GPU buffer holds.
2. Edits are honest. Inspector, undo/redo and MCP edits of Button fields redraw the button. A texture dragged
   onto any texture row previews. The Canvas Renderer's Color and Texture rows are read-only where the Button
   sets them. MCP refuses writes the Button would overwrite, and the refusal names the field to edit
   (`set_properties`, `set_runtime_property`, `create_ui`). Lua `button.color` and `button:setTexture` write
   the Normal state.
3. Unity Selectable parity for Buttons:
   - keyboard, gamepad and TV-remote navigation with Unity's Navigation modes (None, Horizontal, Vertical,
     Automatic, Explicit, Wrap Around), Tab order, Submit and Cancel;
   - a selection per scene (Unity's `EventSystem.currentSelectedGameObject`), `Select On Enable` (HTML
     `autofocus`, Unity's `firstSelectedGameObject` for a menu shown later), and focus return when a submenu
     closes;
   - Unity's five states (Normal, Highlighted, Pressed, Selected, Disabled), transitions (Color Tint with a fade
     duration, Sprite Swap, both, None), and a scale per state;
   - `Button.onClick` (pointer click and Submit), and the script messages `onSelect`, `onDeselect`, `onMove`,
     `onSubmit`, `onCancel`.
4. **No input swallowing.** While no Button is selected, the game gets every key and the whole gamepad, exactly
   as today, whatever Buttons are visible. Doriax's flaw, where any visible HUD button takes the arrows, the
   d-pad and Space, cannot happen by design.
5. Input routing has no traps:
   - a text field being typed in, in any scene, keeps the keys it types with, and an upper document's focused
     button no longer takes Space or Enter from it;
   - a stick held while a menu closes goes back to the game at once, for HTML documents and for component UI;
   - a TV remote's centre key (`AKEYCODE_DPAD_CENTER`) submits.
6. Humans and agents get the same feature:
   - the Inspector has Unity's names and order and a Navigation **Visualize** overlay in the Scene view;
   - Lua and C++ APIs;
   - MCP: `create_ui` keys, resolved navigation in `get_entity` and `get_runtime_entity`, the selection in
     `get_play_state`;
   - docs and an agent-guide digest.

## 2. Non-goals

- **Animation transition** (Unity's `Transition.Animation` with Animator triggers). It needs Unity's "Auto
  Generate Animation" authoring flow for UI clips. Scripts can drive an Animator from `onSelect` or
  `onPointerEnter` today. Possible later as a fifth `transition` value.
- **InputField, Scrollbar, Panel and Progressbar as navigation targets.** The mandate is Buttons. An InputField
  target needs a Selected look the component lacks and an edit-mode entry on Submit. A Scrollbar target needs
  value stepping. The typing guard (goal 5) covers InputFields. A follow-up can add them on the same
  `isSelectable` hook (6.4).
- **Auto-scrolling a list to the selected Button.** Jasper's component UI has no ScrollRect. Fully clipped
  Buttons are not navigation targets (7.2).
- **Unity's `ColorBlock.colorMultiplier`.** Low value. Jasper's state colours replace the colour (they do not
  multiply it); see decision 2.
- **Changing HTML UI navigation** (`nav-*`, spatial navigation in documents). Only the typing guard and the
  stick hand-back touch the shared routing.
- **Android TV packaging** (leanback launcher, banner). That is mobile-shipping's area. This feature only makes
  the remote's keys work.
- **Doriax 34e87d375's dead-root part.** That is runtime-core's.

## 3. Users and stories

- **Human from Unity:**
  - builds a main menu with GameObject > UI > Button, ticks *Select On Enable* on "Play", and presses Play;
  - arrows, the d-pad, the left stick and a TV remote move the selection; Enter, Space, gamepad A and the remote's
    centre key click;
  - the Selected tint fades in over 0.1 s, as in Unity;
  - turning on Navigation > *Visualize* draws the arrows in the Scene view, as Unity's does.
- **Human with a HUD:** a "Pause" button sits in the HUD. The player clicks it with the mouse, then walks with
  the arrows and jumps with Space. The game gets every key, because a click does not select when nothing is
  selected (decision 4).
- **Human with submenus:**
  - Options opens a panel whose first button has *Select On Enable*;
  - closing the panel (`SetActive(false)`) returns the selection to the "Options" button with no code, because
    the lost selection falls back to the previous one (decision 6).
- **Human from Unity porting code:**
  - `EventSystem.current.SetSelectedGameObject(resume)` becomes `UI.setSelected(self.scene, resume)`;
  - `button.onClick.AddListener(f)` becomes `button.onClick:add("tag", f)`;
  - `OnSelect(BaseEventData)` becomes `function M:onSelect(e)`.
- **Agent:**
  - builds a menu with `create_ui`: buttons with `select_on_enable: true` on the first, and
    `navigation: "vertical"` + `wrap_around`;
  - checks the graph without playing: `get_entity` returns `selectable.navigation {up, down, left, right, next}`
    as GameObject refs;
  - then plays: `send_input` key `down` twice, then `enter`; `get_play_state.input.ui_selected` names the
    button, and `eval_lua` reads the click counter.
- **Agent editing a Button's look:** `set_properties ui.color` on a Button with the default transition is refused
  with: "UIComponent.color of GameObject 12 "Play" is set by its Button (transition Color Tint + Sprite Swap):
  set button.colorNormal (and colorHovered, colorPressed, colorSelected, colorDisabled), or set
  button.transition to sprite_swap or none first." The agent then edits `button.colorNormal`, and
  `get_entity` shows `ui.color` updated in edit mode (goal 2).
- **Player with a pad in a pause menu:** holds the stick right while pressing A on "Resume". The menu
  closes, and the character starts walking right in the next frame, without re-centering the stick (goal 5).

## 4. Findings

Line numbers are of `main` at `b4f7816ea` and were re-read for this spec; "verdict" is the dossier skeptic's.

| # | What is wrong or missing | Jasper evidence | Doriax ref |
|---|---|---|---|
| F1 | A textured Button without a Pressed or Disabled texture draws a flat rectangle while pressed, disabled, or under a non-interactable Canvas Group. Only Hovered falls back to Normal | `engine/core/subsystem/UISystem.cpp:762-788` (`applyButtonVisual`: `:768`, `:771` take the state texture unchecked, `:784-786` swap `ui.texture`); draw: an empty `ui.texture` becomes `emptyWhite` (`RenderSystem.cpp:11057-11063`) | 98aa06265 |
| F2 | A Layout Group child that gets no box (hidden, inactive) keeps its old `containerBoxIndex`. Its size overwrites the sibling now at that index (the reverse loop `:2872-2873` reaches it last). A child inactive from load reads `boxes[-1]` | `UISystem.cpp:2689-2700` (the only writer, no reset); `:1755-1756` unguarded read in `getAnchorReferenceRect`; default -1 at `RectTransformComponent.h:59` | b84e20aa6 |
| F3 | A 9-patch Button clips its label to the band inside its patch margins (thin or empty when the top and bottom margins come near the height) | `RenderSystem.cpp:12399` (children inherit), `:12417-12426` (any Image sets the patch-margin scissor), `:12406-12409` (an empty rect skips the draw); the label is made at `UISystem.cpp:560-582` | 4bc3bd536, 35e610ced |
| F4 | `Engine.setCanvasSize` / `scalingMode` at runtime recompute the canvas, but leave the scene cameras, the Canvas Scaler, `viewRect` and `Screen.safeArea` stale until the next window resize | `Engine.cpp:516-526`, `:541-547`; `calculateCanvas` discards the view rect (`:1669-1673`, `unusedViewRect`); the full path is `systemViewChanged` `:1931-1971`; Lua `CoreClassesLua.cpp:2170,2174` | 5462d849a |
| F5 | With Async Loading on, a Button's Normal texture still loading is replaced by `ui.texture` (`load()` is false while loading) | `UISystem.cpp:798-800`; `Texture::load` returns a `TextureLoadResult` with `ResourceLoadState` (`Texture.cpp:450`) | 34e87d375 |
| F6 | Raising `maxTextSize` from the Inspector or MCP does not reload the text's GPU buffers. A later text between the old and the new maximum uploads past the buffer: no clamp in sokol's path | `UISystem.cpp:482-497` (reload only when the text exceeds the max); `editor/Catalog.cpp:1078-1083` (`UpdateFlags_Text` sets only `needUpdateText`); `RenderSystem.cpp:10976-10981` (size at creation), `:11072-11073` (unclamped upload); Lua `Text::setMaxTextSize` reloads (`Text.cpp:96-106`) | a175f0c90 (verdict: uncertain per-backend symptom) |
| F7 | MCP `set_properties ui.color` / `ui.texture` on a Button reports success, then the Button overwrites the value. `create_ui`'s `texture` on a button writes only `ui.texture`. Lua `button.color` and `button:setTexture` (Image's) have the same flaw | `editor/mcp/McpValues.cpp:3783-3816` (no check); `editor/mcp/tools/WorldTools.cpp:4386`; `applyButtonVisual` callers `UISystem.cpp:815,826,3585`; `Image::setTexture` `Image.cpp:129` | 88198667b |
| F8 | Button fields have no update flags: MCP edits and their undo do not set `needUpdateButton`, the only trigger of `updateButton`. Inspector edits do, through `.edited` | `editor/reflect/components/UIReflect.cpp:74-85`; `.edited` `:347`; trigger `UISystem.cpp:2515` | 6d0b683f6 (verdict: narrower, tool edits only) |
| F9 | Dragging a texture over a Button texture row (or any row that is not a Mesh, UI or Skybox row) writes no preview | `editor/window/InspectorWindow.cpp:150-164` (`markTextureUpdate`), `:1449-1460` | 9628666fd |
| F10 | The Canvas Renderer's Color and Texture rows stay editable on a Button that overwrites them | `InspectorWindow.cpp:7961-7965` | f08e7a6d3 |
| F11 | The component UI has no keyboard or gamepad navigation: no focus field, gamepad buttons go to documents only, and `eventOnKeyDown` serves InputFields only | `component/ButtonComponent.h:12-35`; `Engine.cpp:3683-3692`; `UISystem.cpp:3290-3312`; HTML has it (`docs/mcp/guide/html-ui.md:1546-1582`) | 3c5c7fa85 (+4d2125e32, 01797b4b4 follow-ups) |
| F12 | No typing guard across scenes: keys go to documents top layer first, so an upper document's focused button takes Space and Enter while a lower scene's field is typed in. Keys typed into a component InputField also reach the game's `onKeyDown`, since that field gets keys only after the game | `Engine.cpp:3526-3530` (dispatch), `:3535-3545` (order: documents, game, component UI) | EXTRA-FINDINGS; 4d2125e32 |
| F13 | A stick held while a menu closes stays consumed until the stick changes direction or centers, contradicting `html-ui.md:1576` | `Engine.cpp:3111`; `consumed` written only at the press (`:3047-3050`); `updateGamepadNavigation` `:3740-3761` ignores it | 71a87b533 (verdict: needs a Jasper fix, re-send the axis) |
| F14 | A TV remote's centre key is dropped: `AKEYCODE_DPAD_CENTER` maps to `D_KEY_UNKNOWN` | `engine/platform/android/NativeEngine.cpp:52-187`, dropped at `:1663` | EXTRA-FINDINGS |
| F15 | Button state changes are instant: no fade, no Selected state, no scale | `UISystem.cpp:762-788` | 45966b13a (button part) |
| F16 | There is no `Button.onClick`: docs list `onPress`/`onRelease` (`onRelease` fires off the button too) and the UI `onClick` without saying which one is the click | `docs/mcp/guide/lua-api.md:274-282`, `cpp-scripting.md:151`; `UISystemPointer.cpp:722-776` | 1448b85a6 |
| F17 | The guide calls UI and sprite colours sRGB, but they are stored linear (`ColorLinear`; no field anywhere is `ColorSRGB`) | `docs/mcp/guide/editor-tools.md:15-16`; `UIReflect.cpp:80-83,310` | 643e27c52 |
| F18 | An animation track on `ui.color` of a Button is overwritten. Only a doc line warns | `docs/mcp/guide/animation.md:226` | (class of F7) |

Not applicable or already solved (no work, see 13 for owners): 3fb49fbba (Jasper repeats per held key,
`Engine.cpp:2985-2993,3058-3062`); the cascade-refit and MCP colour-formula parts of 35e610ced
(`CascadeFit.cpp:247-248,284-285`; `McpValues.cpp:219`).

Dossier coverage:

| Doriax commit | Where this spec handles it |
|---|---|
| 98aa06265 | F1, 6.3 |
| b84e20aa6 | F2, 6.7 |
| 4bc3bd536 | rejected in favour of 35e610ced's renderer rule (decision 14) |
| 35e610ced | button part in 6.7; the rest is already solved |
| 5462d849a | F4, 6.5 |
| 34e87d375 | Button part in 6.3; the dead root goes to runtime-core |
| a175f0c90 | F6, 6.7 (widened, decision 16) |
| 88198667b | F7, 6.10 (widened, decision 13) |
| 6d0b683f6 | F8, 6.2 |
| 9628666fd | F9, 6.8 |
| f08e7a6d3 | F10, 6.8 |
| 3c5c7fa85 | redesigned, 6.4-6.5 (decisions 1, 4) |
| 4d2125e32 (skip) | its rule is the typing guard (decision 17) |
| 01797b4b4 (skip) | replaced by decision 19 |
| 71a87b533 | F13, decision 18 |
| 3fb49fbba (skip) | already solved |
| 1448b85a6 | F16, `Button.onClick` |
| 643e27c52 | F17, 6.11 |
| 45966b13a, Button part | F15, 6.3 (the Action part goes to runtime-core) |

## 5. Product decisions

1. **The selection is explicit, per scene.**
   - What: each scene's `UISystem` holds at most one selected Button (Unity's EventSystem selection). Keys and
     the gamepad go to the component UI only while some scene taking input has a live selection.
   - How a selection starts: `Select On Enable`, `UI.setSelected`/`button:select()`, or navigation from an
     existing selection.
   - What does not start one: a key press. Doriax selects the first button on the first key press, which is its
     swallowing flaw.
   - Reason: the HTML UI has the same rule ("nothing focused means the game keeps the input",
     `html-ui.md:1574-1576`), so both UIs behave alike. Old projects keep their input untouched.
2. **Transition is Unity's enum plus Jasper's combined mode:**

   | Mode | The Button writes |
   |---|---|
   | `NONE` | nothing |
   | `COLOR_TINT` | `ui.color` |
   | `SPRITE_SWAP` | `ui.texture` |
   | `COLOR_TINT_AND_SPRITE_SWAP` | both |

   - The default is the combined mode: today's behaviour, so old scenes look the same.
   - State colours still replace `ui.color`; they do not multiply it as Unity's CanvasRenderer tint does.
     Multiplying would change every saved Button, whose `ui.color` already holds its last state colour.
   - The mode decides who owns `ui.color` and `ui.texture`. Ownership drives the read-only rows, the MCP
     refusals and the Lua redirects. A Unity user who animates the Image picks None, as in Unity.
3. **State priority (Unity's `currentSelectionState`):** Disabled (own `disabled` or a non-interactable Canvas
   Group), then Pressed, then Selected, then Highlighted (hovered), then Normal. A state without its own
   texture uses Normal's (fixes F1). A state texture still loading shows Normal's until it loads.
4. **A pointer press selects only where a selection exists:**
   - In a scene that has a selection, a primary press on a Button selects it (Unity's
     `Selectable.OnPointerDown`). A press on anything else deselects (Unity's `DeselectIfSelectionChanged`) and
     clears the history.
   - A press on an InputField leaves the selection alone: the field types, and the typing guard keeps the keys.
   - In a scene with no selection, a press selects nothing.
   - Deviation from Unity, documented in the glossary. In Unity a clicked HUD button keeps Space and Enter, the
     classic "jump re-clicks the button" bug.
5. **Submit clicks on the down (Unity's `OnSubmit` → `Press()`, HTML's Enter):**
   - Down: the Button shows Pressed while the key is held; `onPress`, `onClick` and the legacy `ui.onClick`
     (with the element's centre) fire.
   - Up: `onRelease` fires.
   - Repeats do nothing. The pointer keeps clicking on release.
6. **A lost selection returns to the previous one:**
   - Each scene keeps a history of its last 8 selections.
   - When the selection is lost (destroyed, deactivated, hidden, UIComponent disabled, Navigation set to None),
     the most recent entry that is still navigable becomes the selection without a fade (HTML `<dialog>` focus
     return). With none left, the selection clears and the game gets the keys back.
   - An explicit deselect (pointer press elsewhere, `UI.setSelected(scene, nil)`) clears the history too.
7. **Select On Enable** (per Button, default off):
   - When such a Button becomes live (active in hierarchy, visible, UIComponent enabled), it becomes its scene's
     selection, the previous one going to the history.
   - When several become live in one update, the first in Hierarchy order wins (HTML `autofocus`).
   - At scene start it is Unity's First Selected. The glossary maps `EventSystem.firstSelectedGameObject` to it.
     One mechanism covers both start menus and menus shown later.
8. **Keys and buttons** (Unity's StandaloneInputModule plus HTML's Tab):

   | Command | Keyboard | Gamepad | Android TV remote |
   |---|---|---|---|
   | Move | arrows | d-pad, left stick | its arrows |
   | Next / Previous | Tab / Shift+Tab (Hierarchy order, wraps) | none | none |
   | Submit | Enter, keypad Enter, Space | A | centre key |
   | Cancel | Escape | B | Back (Escape) |

   - Only plain keys navigate: Ctrl, Alt or Super held pass the key to the game. Shift is allowed only on Tab.
   - Gamepad repeat uses the HTML constants (0.4 s delay, then every 0.1 s); keys repeat at the OS rate.
   - Move, Next, Previous and Submit are consumed while a selection exists, also at the menu's edge where
     nothing is that way (HTML's rule).
   - Cancel goes as `onCancel` to the selected GameObject and its ancestors. It is consumed only when a script
     defines `onCancel` there; otherwise the game gets Escape or B to close its menu.
9. **Navigation modes are Unity's:** `NONE`, `HORIZONTAL`, `VERTICAL`, `AUTOMATIC` (default) and `EXPLICIT`
   with `selectOnUp/Down/Left/Right`, plus `wrapAround`. Wrap Around applies to Horizontal, Vertical and
   Automatic.
   - The automatic search is Unity's `FindSelectable`: score by `dot / sqrDistance` from the edge point (7.2).
   - On-screen Buttons (with an On-Screen Button component) are never navigation targets: they are touch
     controls.
10. **Transitions run on the UI clock** (`HtmlUIClock`: unscaled, keeps running in a paused Play and at time
    scale 0), so pause menus fade (Unity's `ignoreTimeScale` tweens).
    - Fade Duration: a struct default of 0 (instant, today's), so old scenes do not change.
    - Unity's look on new Buttons: GameObject > UI > Button and `create_ui` write Highlighted (245),
      Pressed (200) and Selected (245) in sRGB, and a Fade Duration of 0.1.
    - Lua and C++ constructors keep the struct defaults (documented).
11. **The Selected colour follows Highlighted when not set:** at the first load, a `colorSelected` still at the
    default white takes `colorHovered`, the same seeding Jasper uses for `colorHovered` and `colorNormal`
    (`UISystem.cpp:801-807`). A coloured button therefore never turns white when selected.
12. **The scale transition:**
    - Fields: `scaleHovered`, `scalePressed`, `scaleSelected` (1 = off).
    - It multiplies the Transform's scale by the change of factor (new/old). A script's own scale change during
      a transition is kept; Doriax's snapshot of a "rest scale" would lose it.
    - It eases with the fade. The label scales along as a child. Hit testing follows the scaled rect (as Unity's
      Animation transition on `localScale`).
    - It is never applied in edit mode. It is undone when a scene leaves the layers or Play stops (7.6).
13. **The class fix for overwritten Button fields** (F7, F10, F18). Every writer of a property the transition
    owns either redirects to the Button field or is refused with the field to use:

    | Writer | Behaviour |
    |---|---|
    | Lua `Button.color` / `setColor` / `setTexture` | redirect to Normal: the Button object overrides them |
    | MCP `set_properties`, `set_runtime_property`, `create_ui` | refused with the field to use (create_ui writes both) |
    | Inspector rows | read-only with a note |
    | Animation bindings | a binding warning |

    Raw ECS access (`UIComponent.color` from Lua or C++) stays raw, and the docs say the Button owns it.
14. **A Button label is clipped by its ancestors, never by its own Button's patch margins.** The rule lives in
    the renderer (Doriax's final form, 35e610ced), not in a per-label `ignoreScissor` flag (4bc3bd536):
    - the flag would also drop every ancestor's clip, and a label in a scrolled panel would draw outside it;
    - the flag would need a migration of saved labels;
    - a long label overflows the button as in Unity.

    The anchor box keeps its patch-margin inset (`UISystem.cpp:1757-1762`), so labels stay centred.
15. **Runtime canvas changes do everything a view change does except the framebuffer:** cameras of every scene and
    the overlay, `viewRect`, `Screen.safeArea`; then `onCanvasChanged` fires when the canvas size or view rect
    really changed (a handler that sets the canvas again does not recurse).
16. **The GPU buffer guard is a class fix, not just `maxTextSize`.** A UI element whose CPU buffer outgrew the GPU
    buffer it was created with is never uploaded: it reloads (bigger) instead, on every backend and for every
    UI type (Text, Image patches, Polygon).
17. **Typing guard:** a component InputField or a document text field being typed in gets the keys first, from
    any scene. While a component InputField is typed in, every key down except Escape and F1-F25 is consumed,
    and the game's `onKeyDown`/`onCharInput` no longer see typing. This is the HTML rule (`html-ui.md:1536-1538`)
    and Input Actions' `suppression.keyboard` already behaves so (`Input.cpp:801`).
18. **Stick hand-back:**
    - When no UI wants the gamepad any more (no document gamepad focus, no component selection), every axis the
      UI took is handed back in the next frame: the game gets the current value once, and the axis stops
      being marked consumed (`Input::setGamepadAxisConsumedByUI(pad, axis, false)`).
    - Buttons held through the change stay the UI's until released, so the game never gets an up without a
      down.
19. **Selection survives a scene leaving the layers.** A scene removed with `Engine.removeScene` keeps its
    selection (shown again when re-added, as a reopened Unity menu). Its submit press ends there (`onRelease`).
    Doriax's "hide the focus until the next press" (01797b4b4) has no meaning when a selection is explicit.

## 6. Design

### 6.1 Data: `ButtonComponent` (`engine/core/component/ButtonComponent.h`)

New enums in `engine/core/component/UISelectable.h` (new header, also used by Lua and reflection):

```cpp
enum class ButtonTransition : uint8_t { NONE, COLOR_TINT, SPRITE_SWAP, COLOR_TINT_AND_SPRITE_SWAP };
enum class NavigationMode   : uint8_t { NONE, HORIZONTAL, VERTICAL, AUTOMATIC, EXPLICIT };
enum class SelectionState   : uint8_t { NORMAL, HIGHLIGHTED, PRESSED, SELECTED, DISABLED };
enum class MoveDirection    : uint8_t { LEFT, UP, RIGHT, DOWN, NONE };   // Unity's order
```

New `ButtonComponent` members (saved unless marked runtime):

| Member | Default | Unity name |
|---|---|---|
| `ButtonTransition transition` | `COLOR_TINT_AND_SPRITE_SWAP` | Transition |
| `Texture textureSelected` | empty | SpriteState.selectedSprite |
| `Vector4 colorSelected` (linear) | white (seeded, decision 11) | ColorBlock.selectedColor |
| `float fadeDuration` (s, >= 0) | 0 | ColorBlock.fadeDuration |
| `float scaleHovered, scalePressed, scaleSelected` (0.01..10) | 1 | (Animation transition) |
| `NavigationMode navigationMode` | `AUTOMATIC` | Navigation.mode |
| `bool wrapAround` | false | Navigation.wrapAround |
| `Entity selectOnUp, selectOnDown, selectOnLeft, selectOnRight` | `NULL_ENTITY` | Navigation.selectOnUp... |
| `bool selectOnEnable` | false | EventSystem.firstSelectedGameObject |
| `FunctionSubscribe<void()> onClick` | | Button.onClick |
| runtime: `bool selected`, `bool submitPressed`, `bool wasLive`, `SelectionState state` | | |
| runtime: `Vector4 fadeFrom`, `double fadeStart`, `float appliedScale = 1`, `ButtonTransition appliedTransition` | | |

The flat fields (not a nested struct) keep reflection on its proven path (`label` is a top-level entity field).
The size grows by about 220 bytes per Button (two vectors, a Texture, the entities), which is fine at UI counts.

### 6.2 Reflection, serialization, codegen (`editor/reflect/components/UIReflect.cpp`)

- `kButtonFields` gets every saved member above, in Unity's Inspector order (`uiOrder`). Groups: "Transition",
  "Navigation".
  - `visibleIf`: colours when the transition has a tint, textures when it swaps, the four explicit targets when
    `navigationMode == EXPLICIT`, `wrapAround` unless `NONE`/`EXPLICIT`.
  - Labels: `colorHovered` "Highlighted Color", `colorSelected` "Selected Color", `textureHovered` "Highlighted
    Texture", and so on. The keys stay (no format change).
  - Enum descriptors `enums::kButtonTransition`, `enums::kNavigationMode` (YAML names lower snake case, as the
    other enums).
  - Clamps: `fadeDuration` `.min = 0`, scales `.min = 0.01f, .max = 10.0f` with `Clamped`.
- **Every** Button field gets `.updateFlags = UpdateFlags_Button`. That is a new bit in `editor/Catalog.h`, the
  next free one: 53 today, to be checked at merge (see 13). `Catalog::updateEntity` sets
  `button->needUpdateButton = true` (fixes F8).
- No version bump: absent keys take the struct defaults, which reproduce today's look. Generated scene code goes
  through the reflected codegen. The entity fields use `ops::Entity` (like `label`), so ids are remapped for
  Prefabs and codegen variable names.
- `kTextFields.maxTextSize` keeps `UpdateFlags_Text`. The reload decision moves into the engine (6.9), where
  every path meets it.

### 6.3 Button visuals (`UISystem.cpp`, Button section)

Replace `applyButtonVisual(ButtonComponent&, UIComponent&)` with:

```cpp
SelectionState buttonState(Entity entity, const ButtonComponent& b, const UIComponent& ui) const; // decision 3
void applyButtonVisual(Entity entity, ButtonComponent& b, UIComponent& ui, bool animate);
void updateButtonTransitions();             // advances fades/scales on the UI clock; from update()
static bool buttonOwnsUIColor(const ButtonComponent& b);    // transition has a tint
static bool buttonOwnsUITexture(const ButtonComponent& b);  // transition swaps
```

`applyButtonVisual`:

1. `state = buttonState(...)`; store `b.state`.
2. Sprite swap: the state's texture when it is non-empty and loaded; otherwise Normal's. The non-empty test is
   `hasButtonTexture`: non-empty, with an id or a framebuffer. The loaded test is `load().state == Finished`.
   Set `ui.texture` and `needUpdateTexture` only on a change.
3. Colour tint: the target colour of the state. When `animate && fadeDuration > 0 && ui.color != target`, set
   `fadeFrom = ui.color` and `fadeStart = HtmlUIClock::now()`, add the entity to `transitioningButtons`, and let
   the update write `ui.color`. Otherwise write it now.
4. Scale: the target factor (Hovered, Pressed or Selected; 1 for Normal and Disabled). The scale eases the same
   way. `applyScaleFactor` does `transform.scale *= f / appliedScale; appliedScale = f; transform.needUpdate`.
   The scale applies only while the scene runs (`!paused`). The editor's edit mode never hovers, presses or
   selects.
5. When `transition != appliedTransition`, the properties that just stopped being owned get their Normal value
   once (no state colour stuck on `ui.color`), and `appliedTransition = transition`.

`updateButtonTransitions` walks `std::vector<Entity> transitioningButtons`:
- An entity that is not live, has no Button, or whose fade ended is dropped.
- Otherwise `t = clamp((now - fadeStart) / fadeDuration, 0, 1)` and `ui.color = lerp(fadeFrom, target, t)`; the
  scale eases the same way.
- An empty vector costs one `empty()` per update.

`updateButton` (first load):

- `textureNormal`: seeded from `ui.texture` only when it is empty or its load state is `Failed`. A texture still
  `Loading` keeps its path (fixes F5).
- `colorSelected` seeded per decision 11.
- `applyButtonVisual(..., animate=false)`.

### 6.4 Selection and navigation (`engine/core/subsystem/UISystemNavigation.cpp`, new; `UISystem.h`)

UISystem state: `Entity selected = NULL_ENTITY; std::vector<Entity> selectionHistory /*max 8*/;
Entity submitButton = NULL_ENTITY; int submitSource = 0; std::vector<Entity> transitioningButtons;`

Public API:

```cpp
// Unity's EventSystem selection (ui-components spec 6.4)
Entity getSelected() const;
// any live Button that is not an on-screen control (interactable or not, any mode, as Unity's Select());
// NULL_ENTITY deselects and clears the history. false when it cannot be selected
bool setSelected(Entity entity);
// the selection is live (6.4 "live"); a lost one is replaced from the history (decision 6) first
bool hasLiveSelection();
bool isSelectable(Entity entity) const;   // a Button, not on-screen, live
bool isNavigable(Entity entity) const;    // isSelectable, mode != NONE, interactable, non-empty hit rect
Entity findSelectableOnDirection(Entity from, MoveDirection dir) const;  // its mode's rule, 7.2
Entity findNextSelectable(Entity from, bool backwards) const;            // Hierarchy order, wraps
// input from Engine; each returns whether it consumed
bool eventOnMove(MoveDirection dir, Vector2 moveVector);
bool eventOnTab(bool backwards);
bool eventOnSubmitDown(int source);   // source: a key code, or 1000 + gamepad slot * 32 + button
bool eventOnSubmitUp(int source);
bool eventOnCancel();
void cancelSubmit();                  // press ends without a click (onRelease only)
```

Private API: `loseSelection()`, `changeSelection(Entity next, bool fromHistory)` (sends `onDeselect` then
`onSelect` and re-applies both visuals), `navRect(Entity)`.

- **live:** `isLiveUIEntity`, `isUIActive` (active in hierarchy, UIComponent enabled), and `transform.visible`.
- **`navRect`:** the rect `raycastHit` uses (`UISystemPointer.cpp:92-120`): world position times size,
  `fitOnPanel` when in a panel, intersected with the UI canvas (`getCanvasSize`). Navigation and clicks agree on
  what is reachable.
- **History:** `changeSelection` pushes the old selection (unique, max 8, oldest dropped). `loseSelection` pops
  until it finds a navigable entry.
- **Updates (`UISystem::update`, after the `paused` return):**
  1. the Select On Enable pass: in the existing loop over layouts, for each Button,
     `live = isUIActive && visible`, and `if (live && !wasLive && selectOnEnable && !pickedThisUpdate)` it is
     selected; then `wasLive = live`;
  2. `hasLiveSelection()` (loss and restore);
  3. `updateButtonTransitions()`.
- **Removal:** `onComponentRemoved` for Button, UIComponent or Transform of `selected` calls `loseSelection()`.
  For `submitButton` it clears it without callbacks (the GameObject is going).
- **Submit:**
  - `eventOnSubmitDown`: needs a live selection. With an existing `submitButton`, the down is consumed and
    ignored.
  - Otherwise: script message `Submit` to the selected GameObject. If `e:use()` was called there, the built-in
    action is skipped.
  - Built-in action, if the Button is interactable: `submitButton = selected`, `refreshButtonPressed(selected)`
    (it now also counts `submitButton`, so pointer and submit holds combine: `onPress` once), then
    `button.onClick`, then the legacy `ui.onClick(width/2, height/2)` with `getPointerEventData()` nil.
  - `eventOnSubmitUp(source)`: if the source matches, clear `submitButton` and call `refreshButtonPressed`
    (`onRelease`).
- **Move:**
  - `eventOnMove`: script message `Move` (an `AxisEventData`) to the selected GameObject. If not used:
    `next = findSelectableOnDirection(selected, dir)`, and `setSelected(next)` when found.
  - Consumed whenever a selection exists.
  - `eventOnTab` is the same with `findNextSelectable`, and has no script message.
- **Cancel:** script message `Cancel` with ExecuteHierarchy from the selected GameObject (decision 8). Consumed
  when a handler ran.
- **Pointer (`UISystemPointer.cpp` `eventOnPointerDown`, primary press, after the raycast):**
  - when `selected != NULL_ENTITY`: `h` = the hit or its nearest ancestor with a Button (Unity's
    `GetEventHandler<ISelectHandler>`; the label's press selects its Button);
  - an on-screen Button, or the hit or an ancestor having an InputField: the selection is unchanged;
  - else if `h` is selectable and not `NONE`: `setSelected(h)`;
  - else: deselect (decision 4).
- **Pointer click (`legacyPointerUp`):** after the legacy `ui.onClick`, a Button also gets `button.onClick`
  (F16), in the same interactable check `UISystemPointer.cpp:758-761`.
- **Scene leaving the layers (`resetButtonStates`, `UISystemPointer.cpp:1332`):** `cancelSubmit()`, end
  transitions at their target and restore the scales. The selection is kept (decision 19).

### 6.5 Engine routing (`engine/core/Engine.cpp`, `Engine.h`)

New, in the anonymous namespace next to the gamepad code:

```cpp
// the component UI of the top scene (layer order, canSendUIEvents) whose selection is live; or null
Scene* selectionScene(const std::vector<Scene*>& scenes);
// a component InputField typed in (top scene first), or null
Scene* componentTypingScene(const std::vector<Scene*>& scenes);
bool keyToNavigation(const std::vector<Scene*>& scenes, int key, bool repeat, int mods); // decision 8
```

**`Engine::systemKeyDown`** (`:3515`), new order:

1. **Typing guard.** `typing = componentTypingScene(scenes)`. If there is one, its
   `UISystem::eventOnKeyDown` runs (InputField editing). The key is consumed unless it is Escape or F1-F25
   (decision 17).
2. **Documents**, unless consumed. A document with a focused text input is offered the key first
   (`dispatchToDocuments` gains a "typing first" pass: the scene whose `UIDocumentSystem::hasFocusedTextInput()`
   is true), then the others top layer first. This fixes F12's upper-button case.
3. **Component navigation**, unless consumed or something is typed in: `keyToNavigation`. It works only on a
   live selection in `selectionScene`.
4. `setPressConsumedByUI` when consumed and not a repeat. The game's `onKeyDown` unless consumed (with
   `ignoreEventsHandledByUI`).

The old step "component UI after the game" goes; step 1 replaces it.

**`Engine::systemKeyUp`:** Enter, keypad Enter and Space call `eventOnSubmitUp(key)` on every scene (only the
holder matches). Consumed when it matched, so the game never gets an up without its down.

**`Engine::systemCharInput` / `systemTextInput`:** with a typing component InputField, its scene's
`eventOnCharInput` runs first and consumes (decision 17); then documents and the game as today. unicode-text
rewrites these functions too (13).

**Gamepad:**
- `GamepadUIHeld` gains `enum class Owner : uint8_t { NONE, DOCUMENTS, COMPONENT } owner` (instead of the
  `consumed` bool; `consumed == (owner != NONE)`).
- `gamepadKeyDown`: documents first (as today); if none took it, the component UI takes it for a live selection:

  | Key | Component command |
  |---|---|
  | d-pad and stick directions | `eventOnMove` |
  | A | `eventOnSubmitDown(1000 + slot * 32 + button)` |
  | B | `eventOnCancel` |

  The owner is recorded.
- `gamepadKeyUp` sends the ups to their owner: documents as today; for the component UI,
  `eventOnSubmitUp` for A.
- `updateGamepadNavigation` repeats a held direction to its owner. For the component UI it calls `eventOnMove`
  again; when that is no longer consumed (the selection is gone), the owner becomes `NONE` and the repeat stops.
- `gamepadAxisToDocuments` (renamed `gamepadAxisToUI`) returns
  `(held && held->owner != NONE) || documentsHaveGamepadFocus(scenes) || selectionScene(scenes)`.
  `getInputScenes(false)` gives the scenes for `selectionScene`.

**Stick hand-back (decision 18):**
- `GamepadUIState` gains `std::set<std::pair<int,int>> takenAxes`, filled where `systemGamepadAxisMove` reports
  `consumed`.
- In `updateGamepadNavigation`, which no longer returns early when `held` is empty but `takenAxes` is not: when
  `!wantsGamepadNavigation()`, each taken axis is handed back:
  - `v = Input::getGamepadAxis(pad, axis)`; if `gameAxes[id] != v`, set `gameAxes[id] = v` and call
    `onGamepadAxisMove(pad, axis, v)`;
  - `Input::setGamepadAxisConsumedByUI(pad, axis, false)`;
  - that pad's held stick-direction entries get `owner = NONE`.
- Then `takenAxes.clear()`.
- Cost when nothing is held or taken: two `empty()` checks.

**Queries:**
- `Engine::wantsGamepadNavigation()` (`:3726`): documents with gamepad focus **or** a live component selection in
  an input scene.
- `Input.cpp:801-802` suppression gains `suppression.keyboardNavigation = selection exists`.
  `InputActions::suppressedByUI` then suppresses, for maps with `ignoreUI`, the arrows, Tab, Enter, keypad Enter
  and Space only.
- `Engine::isTextInputFocused` is unchanged.

**`Engine::removeScene`** (`:349-379`): `resetButtonStates()` already runs and now ends the submit (6.4). The
pad needs no call here: the next `updateGamepadNavigation` hands the axes back if nothing else wants the pad.

**Canvas (decision 15):**
- `void Engine::refreshCanvas(bool notify)` is the body of `systemViewChanged` without the framebuffer part.
  It runs `computeViewLayout` with `viewRect`, then `updateCameraSize` for every scene and the overlay, then
  `Screen::updateSafeArea()`.
- When `notify` and the canvas or view rect changed: `onViewChanged`, `systemOrientationChanged()`, and
  `Screen::dispatchSafeAreaEvent()` unless paused.
- `setCanvasSize` and `setScalingMode` call `refreshCanvas(true)` when `viewLoaded`, instead of
  `calculateCanvas()`.
- A static `inCanvasRefresh` guard: a nested call recomputes without notifying.
- `systemViewChanged` = framebuffer part + `refreshCanvas` with its current unconditional events (unchanged for
  the platforms). The editor's own sequence (`editor/App.cpp:4305-4309, 4328-4329`) still works: `setCanvasSize`
  now refreshes and the following `systemViewChanged` refreshes again.

### 6.6 Android TV (`engine/platform/android/NativeEngine.cpp`)

- `getJasperEngineKey`: `AKEYCODE_DPAD_CENTER` → `D_KEY_ENTER`, with a `static_assert` beside the others
  (`:225-252`). No character is sent for it: Enter's `'\r'` comes only from `AKEYCODE_ENTER`
  (`:1654`).
- Gamepads that send `DPAD_CENTER` with a gamepad source also land in the keyboard path (`getJasperEngineGamepadButton`
  returns -1), so they submit too.
- This builds in the Android image (`tools/build.sh android`). Its runtime is checked by the static asserts and
  by the shared Engine path tested on Linux.
- Other platforms need no platform code: Windows, macOS, iOS and web keys already map arrows, Enter, Space, Tab
  and Escape.

### 6.7 Layout, label clip, text buffers (`UISystem.cpp`, `RenderSystem.cpp`)

- **F2:** in `UISystem::update`'s first loop, set `layout.containerBoxIndex = -1` before
  `findNearestLayoutParent` (`:2689`). `getAnchorReferenceRect` (`:1754-1756`) reads the box only when
  `containerBoxIndex >= 0 && < container.numBoxes`.
- **F3:** in `RenderSystem::applyPassScissor` (`:12417`), an Image **with a ButtonComponent** passes its
  parent's scissor on unchanged:
  - `*scissor.rect = parentScissor`, `*scissor.active = inheritsScissor`;
  - no patch-margin rect.

  Children of a Button are clipped by the Button's ancestors only. `passScissor` (`:13990`) follows the same
  rule wherever it recomputes a parent's scissor.
- **F6 / decision 16:**
  - New runtime `UIComponent` members: `size_t gpuBufferBytes = 0, gpuIndicesBytes = 0`, set in `loadUI`
    (`RenderSystem.cpp:10976-11006`) to the sizes passed to `createBuffer`, and zeroed on destroy.
  - In the draw's buffer update (`:11072-11076`), when `ui.buffer.getSize() > ui.gpuBufferBytes` (or the same
    for the indices), there is no upload: `ui.needReload = true` and the update stays pending (next frame
    rebuilds).
  - `UISystem::createText` also sets `ui.needReload` when `ui.loaded && maxTextSize * 4 * stride >
    gpuBufferBytes` (Doriax a175f0c90, against the real capacity). The Inspector and MCP path therefore reloads
    before a longer text ever arrives.

### 6.8 Editor (`editor/window`, `editor/render`, `editor/command`)

- **Button Inspector, `InspectorWindow::drawButtonComponent`** (new file `editor/window/InspectorButton.cpp`,
  registered in `InspectorDefault.cpp`'s switch):
  - Unity's layout: **Interactable** (a checkbox for `!disabled`, through `PropertyCmd` on `disabled`, undoable),
    then **Transition**.
  - Then the colour rows and **Fade Duration**, and the texture rows, each shown by mode.
  - Then the **Scale** rows (Highlighted, Pressed, Selected), **Navigation** with **Wrap Around** or the four
    Select On rows, a **Visualize** toggle button, **Select On Enable**, and **Label**.
  - Rows go through `propertyRow`, so every edit is a `PropertyCmd` with the field's `UpdateFlags_Button`, and
    undo and redo refresh.
- **Visualize** (Unity's Navigation Visualize):
  - an editor preference `AppSettings::uiNavigationVisualize` (settings.yaml, not undoable, not saved in the
    scene);
  - `SceneRender2D` keeps a `LineRenderer* navigationLines`. While on, it draws for every navigable Button of
    the open UI scene an arrow from the edge point toward each resolved neighbour (yellow, as Unity's), with the
    arrowhead at the target's edge;
  - rebuilt when the UI layout or a Button changes (the scene's layout revision), else reused.
- **Canvas Renderer rows (F10):** `drawUIComponent` (`:7961`) draws Color and/or Texture disabled (`BeginDisabled`)
  when `buttonOwnsUIColor`/`buttonOwnsUITexture`, with a dim line:
  "Set by the Button's transition: edit its Normal color / texture". The values still seed Normal at load
  (decision 11's seeding), and the line says so in its tooltip.
- **Texture drag preview (F9):** `markTextureUpdate` and the preview at `:1449-1460` use
  `Catalog::updateEntity(scene, entity, Catalog::findProperty(cpType, id).updateFlags)` instead of the per-type
  branches. Mesh, UI and Skybox keep working through their own flags; Button rows get `UpdateFlags_Button`.
- **Creation (decision 10):**
  - `CreateEntityCmd.cpp:169-190` (BUTTON) writes Unity's state colours and `fadeDuration = 0.1`;
  - ONSCREEN_BUTTON (`:286-315`) writes `navigationMode = NONE` for clarity. It is excluded anyway.
- **Hierarchy:** none.

### 6.9 Lua and C++ API

- **`Button` object** (`engine/core/object/ui/Button.{h,cpp}`, bound in `GameObjectClassesLua.cpp:3195-3223`):
  - properties: `transition`, `colorSelected`, `fadeDuration`, `scaleHovered`, `scalePressed`, `scaleSelected`,
    `navigationMode`, `wrapAround`, `selectOnUp/Down/Left/Right` (GameObject or nil), `selectOnEnable`;
    `interactable` (= not `disabled`); `selected` and `state` (read-only); `onClick`;
  - methods: `setColorSelected(r, g, b[, a])`, `setTextureSelected(path)`, `select()`,
    `findSelectableOnUp/Down/Left/Right()` (GameObject or nil; Unity's names);
  - overrides: `color` / `setColor` write `colorNormal`, and `setTexture(path)` writes `textureNormal`
    (decision 13);
  - every setter sets `needUpdateButton` and `markChanged` (the existing pattern).
- **`ButtonComponent`** (`ECSClassesLua.cpp:777-793`): the new fields raw, plus `onClick`, `selected` and `state`
  read-only.
- **`UI` statics** (`CoreClassesLua.cpp:1447`):

  | Function | Returns |
  |---|---|
  | `UI.getSelected(scene)` | GameObject or nil |
  | `UI.setSelected(scene, objOrEntityOrNil)` | bool |

- **Enums:** `ButtonTransition`, `NavigationMode`, `SelectionState`, `MoveDirection`.
- **Event data** (`engine/core/object/ui/UIEventData.h`, new, beside `PointerEventData.h`):

  | Type | Members |
  |---|---|
  | `BaseEventData` | `target`, `selectedObject` (GameObjects), `used`, `use()` |
  | `AxisEventData : BaseEventData` | `moveDir`, `moveVector` (unit, y down) |

- **Script messages** (`script/ScriptMessages.h`): append `Select, Deselect, Move, Submit, Cancel` before
  `Count` (39 < 64). Method names `onSelect`, `onDeselect`, `onMove`, `onSubmit`, `onCancel`.
  - `ScriptBase` virtuals: `onSelect(BaseEventData&)`, `onDeselect(BaseEventData&)`, `onMove(AxisEventData&)`,
    `onSubmit(BaseEventData&)`, `onCancel(BaseEventData&)`.
  - Delivery: Select, Deselect, Move and Submit go to the GameObject only (Unity's `Execute`); Cancel goes up the
    hierarchy (decision 8).
  - The `RegisterEvent` collision rule (`noteLegacySubscription`) also covers `onSubmit` subscribed to an
    `InputFieldComponent.onSubmit`.
- **C++:**
  - `UISystem` public API as in 6.4; `Button::select()`, `isSelected()`, `getState()`;
  - `REGISTER_BUTTON_EVENT(onClick, method)` works as is (generic member).
- **`Engine.wantsGamepadNavigation()`** documents its wider meaning.
- **Lua API data and stubs:** regenerated by the build from `tools/lua_api`. Add descriptions in
  `tools/lua_api/manual.py` where it holds them.

### 6.10 MCP (`editor/mcp`)

- **Refusals (F7, decision 13).** `McpValues::appendChangeCommands` (per entity; `checkReflectedChanges` has no
  entity) refuses a `UIComponent` `color` or `texture` change on a GameObject whose Button owns it, with 3's
  message. `PlayTools` `set_runtime_property` does the same.
- **`create_ui`** (`WorldTools.cpp` `buildUiNode`, `:4232-4400`):
  - `texture` on a button writes `button.textureNormal` too (fixes `:4386`);
  - `colors` gains `selected`;
  - new button keys:

    | Key | Value |
    |---|---|
    | `textures` | `{normal, hovered, pressed, selected, disabled}` |
    | `transition` | `none` / `color_tint` / `sprite_swap` / `color_tint_and_sprite_swap` |
    | `fade_duration` | seconds |
    | `scale` | `{hovered, pressed, selected}` |
    | `navigation` | a mode string, or `{mode, wrap_around, up, down, left, right}`; targets are node names of the same call or entity refs |
    | `select_on_enable` | bool |

  - defaults per decision 10 when the node sets no colours;
  - the description names the keys (+~120 characters, within `tool_default` 1500; `tool_budget.py --check`).
- **`get_entity`** (`ComponentTools.cpp`, beside `canvas_group_state` `:397`): a Button gets
  `selectable {navigable, navigation {up, down, left, right, next}}`, the resolved targets as `{id, path}` or
  null, computed from the edit-mode layout.
- **`get_runtime_entity`** (`PlayTools.cpp`, beside `:2508`): `selectable {state, selected, navigable,
  navigation {...}}`.
- **`get_play_state`:** `input.ui_selected: [{scene, entity: {id, path}}]` for each running scene with a live
  selection, plus a text line. Description +~25 characters.
- **`send_input`:** unchanged. Keys (`down`, `tab`, `enter`, `space`, `escape`) and gamepad (`dpad_down`, `a`,
  `b`, axis `left_x`) already reach the routing.
- **Docs and guide:**
  - `docs/mcp/guide/ui.md`: new sections "Buttons: states and transitions" and "Keyboard, gamepad and TV
    navigation" (rules of 7, Lua, MCP checks);
  - `docs/mcp/TOOLS.md` regenerated (`gen_tool_docs.py`);
  - agent guide (`editor/mcp/McpInstructions.cpp`, the "UI widgets" bullet): "Menus: one Button with
    select_on_enable gives keyboard/gamepad/TV navigation (jasper://docs/ui)", about 90 bytes within the guide
    budget.

### 6.11 Docs

- `docs/mcp/guide/lua-api.md`:
  - 1.7 gains the five messages and the event data;
  - the component-events table gains `onClick`, marked "the click: pointer released over it, or Submit",
    `onRelease` "every release, also off the button" (F16);
  - the `UI` statics.
- `cpp-scripting.md:151`: `REGISTER_BUTTON_EVENT(onClick, method)` as the click.
- `input.md`: routing order, typing guard, suppression.
- `html-ui.md:1574-1576`: the stick hand-back is now true; and "a text field being typed in, in any scene, keeps
  its keys".
- `animation.md:226`: "with a transition that tints".
- `editor-tools.md:15-16`: "UI colours are linear like the 3D ones; `get_entity` shows what is stored" (F17).
- `docs/unity-glossary.md`: a "UI (Selectable)" table with `Selectable.Select` / `EventSystem.SetSelectedGameObject`,
  `firstSelectedGameObject`, `Navigation`, `ColorBlock`, `SpriteState`, `Transition`, `Button.onClick`,
  `ISelectHandler`... and the deviation of decision 4.

## 7. Behaviour rules and edge cases

### 7.1 Who gets a key

1. A text field typed in (any scene, the component field first, top layer first) gets it. Consumed except
   Escape and F1-F25.
2. HTML documents (the one typing first, then top layer first), as today.
3. With a live selection in the top selection scene: arrows → Move, Tab/Shift+Tab → Next/Previous,
   Enter/keypad Enter/Space → Submit (consumed), Escape → Cancel (consumed only when handled). Ctrl, Alt or Super
   held: not navigation.
4. The game (`onKeyDown`) when nothing consumed it, or always when `ignoreEventsHandledByUI` is false.
5. `Input.getKey*` and `isKeyPressed` always read the device. Input Actions with `ignoreWhenUIConsumes` skip
   consumed presses and, while a selection exists, the navigation keys.

### 7.2 Finding the next Button

Space is the UI's, y down: Up = (0,-1).

- **Candidates:** navigable Buttons of the same scene other than `from`, in Hierarchy order.
- **Automatic** (Unity's `FindSelectable`):
  - `pos` = the edge point of `navRect(from)` in `dir`;
  - for each candidate, `v = centre(c) - pos`, `along = dot(dir, v)`; skip `along <= 0`;
    `score = along / |v|²`;
  - the highest wins; ties go to the earlier in Hierarchy order.
- **Horizontal / Vertical:** as Automatic for Left/Right or Up/Down only; the other two directions find nothing
  (consumed, no move).
- **Explicit:** the field's target if it is navigable, else nothing (no fallback search).
- **Wrap Around**, when nothing was found (not Explicit): among candidates with `along < 0`, those overlapping
  `from` on the perpendicular axis first; the one farthest along `-dir` wins.
- **Next / Previous:** the next navigable Button in Hierarchy order after (before) `from`, wrapping. All modes
  except None take part.
- **From a non-navigable selection** (disabled, or selected by a script): the same search from its rect. A
  selection whose rect is empty searches from the canvas centre.

### 7.3 Selection lifetime

- **Lost:** the selected GameObject is destroyed or deactivated, its UIComponent is disabled, it is hidden, it
  loses its Button, or its mode is set to None. It is then replaced from the history (decision 6) at the next
  update or input event, whichever comes first. `onDeselect` is not sent to a destroyed GameObject.
- **Kept:** a selection that only became non-interactable stays and shows Disabled. Navigation moves away from
  it; Submit is consumed and does nothing.
- **Covered:** a loading scene covering the selection scene, or `UIEventState::DISABLED`, removes it from
  `selectionScene`. The selection stays and is used again when uncovered.
- **Multiple scenes:** only the top scene with a live selection navigates. Other scenes' selections stay and
  keep their Selected look (as several Unity EventSystems would not; documented: deselect what you hide).
- **Re-entrancy:**
  - `onSelect`, `onDeselect`, `onPress`, `onClick`, `onMove`, `onSubmit` and `onCancel` may select, destroy or
    deactivate anything;
  - every step re-reads components after a callback (the `UISystemPointer.cpp` pattern) and runs under
    `StructuralDeferScope`;
  - a `setSelected` inside `onSelect`/`onDeselect` wins: a request counter, as Doriax's `focusRequest`;
  - a Submit whose `onClick` selected another Button releases the original on the up.

### 7.4 Submit and Cancel sources

- One Submit hold at a time per scene. A second source's down while one is held is consumed and ignored.
- The up of a source that did not press is not consumed.
- App pause, focus loss and `Input::releaseAll` call `cancelSubmit` on every scene (`onRelease`, no second click).

### 7.5 Pointer, hover and touch

- Hover still gives Highlighted, also on a Button other than the selected one; both looks show at once (Unity).
- Touch has no hover.
- On-screen Buttons: never selected and never navigation targets. Their own press logic is unchanged and they
  get `onClick` like any Button.

### 7.6 Transitions

- **Instant changes:** the first apply, edit mode, `fadeDuration == 0`, a restore from history, and Disabled
  set at load.
- **Texture swaps** are instant (Unity's). **Colour and scale** ease linearly over `fadeDuration` (Unity's
  `ColorTween` is linear).
- **A new state during a fade** starts from the current colour and scale.
- **Scale clamping:** the factor is clamped to 0.01..10, and `appliedScale` is never 0.
- **Restore:** `resetButtonStates` and Play stop end every transition at Normal and restore the factor to 1
  before the play-change snapshot. `editor/PlayChangeReview` reads the scene after `UISystem::resetButtonStates`;
  if not, call `UISystem::endTransitions()` there.
- **Canvas Groups:** a fading Button under a fading group: the group's alpha multiplies independently
  (`groupAlpha`); `ui.color` is untouched by groups.

### 7.7 Layout, clip, text

- **A Layout Group child inactive from load:** never reads `boxes[-1]`. It anchors to the group's full rect
  until it gets a box.
- **A Button inside a clipping Panel:** its label is still clipped by the Panel. A Button with no clipping
  ancestor: the label is unclipped.
- **Text growth:** a text that outgrows the GPU buffer for one frame keeps its previous geometry for that frame,
  then reloads. A `Log::warn` once per element when that happens outside the maxTextSize path (a bug elsewhere
  became visible).

### 7.8 Canvas

- `setCanvasSize` before the view loads only stores (as today).
- Equal values: cameras refresh, but no events fire.
- A Lua `onCanvasChanged` that calls `setCanvasSize` again refreshes without a second event.

## 8. Performance and cost

| Path | Cost when unused | Cost when used |
|---|---|---|
| `UISystem::update` | one `wasLive` compare per Button (inside the existing per-element loop); `hasLiveSelection` O(1) with no selection; `transitioningButtons.empty()` | fades: O(transitioning Buttons) |
| Key events | `componentTypingScene` O(scenes x InputFields) per key, only on input events; `selectionScene` O(scenes) | a Move: O(Buttons in the scene) rect math, once per press or repeat |
| Gamepad per frame | `held.empty() && takenAxes.empty()` | O(held + taken) |
| Render | one size compare per UI buffer upload | a reload when a buffer outgrew (rare) |
| Memory | about 220 bytes per Button; 16 per UIComponent; 3 vectors per UISystem | history of 8 |
| GPU | nothing: no shader, pipeline, pass or texture change | Visualize: one LineRenderer in the editor only |

This is safe for mobile and web: no allocation per frame in the steady state, and no work while nothing is
selected or fading. Every graphics backend and export target behaves the same, since there is no GPU feature.

## 9. Compatibility

No scene format change, so there is no version bump: every new field has a default reproducing today's
behaviour. `editor/tests/reflect/catalog_snapshot.json`, `tools/fixtures/reflection-parity/golden/inspector_layouts.json`
and the legacy-codec parity list (`editor/tests/reflect/legacy/LegacyCodecs.cpp`, the new keys being
reflection-only) are updated.

Intended behaviour changes (listed in `ui.md` "Behaviour changes", as Canvas Group did):
1. **Button looks:** a pressed, disabled or non-interactable Button without its own texture keeps its Normal
   texture instead of a flat rectangle.
2. **Labels:** a Button's label is no longer clipped to its 9-patch band.
3. **Typing guard:** keys typed into a component InputField no longer reach the game's `onKeyDown` /
   `onCharInput` (with `ignoreEventsHandledByUI`, the default), except Escape and F1-F25. A focused upper
   document no longer takes Space or Enter from a field typed in elsewhere.
4. **Runtime canvas:** `Engine.setCanvasSize` / `scalingMode` at runtime update cameras at once and fire
   `onCanvasChanged` when something changed.
5. **Lua on a Button object:** `button.color = ...` and `button:setTexture(...)` set the Normal state (before,
   they were lost at the next hover).
6. **Stick hand-back:** a stick held while a menu closes reaches the game in the next frame.
7. **New Buttons** from the editor menu or `create_ui` get Unity's state colours and a 0.1 s fade. Existing
   Buttons are unchanged.
8. **Android:** the TV remote's centre key arrives as `KEY_ENTER` (it was dropped).
9. **The shipped MCP tests and fixtures** that read a created button's hovered or pressed colour, or count
   `onKeyDown` while typing, are updated in this branch, and each update is noted in the commit.

Navigation itself is opt-in: no old project selects anything until it uses Select On Enable or the API.

## 10. Tests

Suites per `docs/testing.md`. **Red** marks the controls that must fail on `main` and pass with the change. Each
red is run on the parent commit first and the run is recorded.

**`jasperengine-editor-tests`, new `editor/tests/UIComponentsTest.cpp`** (harness of `UIMultiPointerTest.cpp`:
`ScriptTest`, scene with a camera, `element()`/`button()` helpers):

| Test | Asserts | Red |
|---|---|---|
| `PressedWithoutTextureKeepsNormal` | Button `textureNormal = a.png`, no pressed or disabled texture: during a pointer press `ui.texture == textureNormal`; same with `disabled`, and under a Canvas Group `interactable = false` | red |
| `LoadingNormalTextureIsNotReplaced` | with the texture's pool entry in `Loading` (test hook `TextureDataPool::setStateForTest`, or a pure `resolveNormalTexture(state, ...)` when the pool cannot be held) the first `updateButton` keeps `textureNormal`'s path; with `Failed` it seeds from `ui.texture` | red |
| `HiddenLayoutChildTakesNoBox` | horizontal Layout Group, expand off, children 50/80/30 wide: deactivate the middle one, update twice; the third child's box rect is 30 wide at x = 50 + spacing, the hidden child's `containerBoxIndex == -1` | red |
| `ChildInactiveFromLoadReadsNoBox` | a child inactive before the first update: `getAnchorReferenceRect` returns the group's rect (and UBSan reports nothing in the sanitizer run) | red under UBSan |
| `MaxTextSizeGrowthReloads` | loaded text (`ui.loaded = true`, `gpuBufferBytes` set as `loadUI` would), `maxTextSize` 10 → 100 through `Catalog::updateEntity(UpdateFlags_Text)`, update: `ui.needReload` | red |
| `McpStyleEditRefreshesButton` | a `MultiPropertyCmd` on `button.colorNormal` (no Inspector callback), execute + update: `ui.color` equals it; undo + update: back | red |
| `DragPreviewMarksButton` | the preview helper on `ButtonComponent.textureHovered` sets `needUpdateButton` | red |
| `RuntimeCanvasSizeUpdatesCameras` | view loaded (the SafeAreaTest setup), `Engine::setCanvasSize(800, 600)` at runtime: the scene camera's size and `Engine::getViewRect()` follow at once; `onCanvasChanged` fired once; a handler that calls it again does not recurse | red |
| `NoSelectionNoSwallow` | three visible Buttons, nothing selected: `systemKeyDown(DOWN/SPACE/ENTER/TAB)` and gamepad d-pad/A reach the game's `onKeyDown`/`onGamepadButtonDown`; no `onClick`, no selection (the Doriax-flaw control: a "first press selects" implementation fails it) | green on main, guards the design |
| `ClickedHudButtonDoesNotTakeKeys` | click a Button with the mouse in a scene without selection, then Space: the game gets it and the Button is not clicked again | guard |
| `SelectOnEnableAtStartAndOnShow` | the first `selectOnEnable` Button in Hierarchy order is selected after the first update; activating a hidden panel with one selects it; deactivating that panel restores the previous selection (history) | |
| `AutomaticGridNavigation` | 2x2 grid: right, down, left, up visit the four; nothing beyond the edges (consumed, unchanged) | |
| `HorizontalVerticalExplicitWrap` | Horizontal ignores up/down; Explicit follows targets and skips a non-navigable one; Wrap Around on a vertical list goes from last to first | |
| `TabFollowsHierarchyOrder` | Tab and Shift+Tab with wrap; Ctrl+Tab not consumed | |
| `SubmitClicksOnDown` | Enter: Pressed state, `onPress`, `onClick` and `ui.onClick` on the down; `onRelease` on the up; repeat downs do nothing; Space and gamepad A the same | |
| `SubmitReleasedWhenSceneLeaves` | Enter held, `Engine::removeScene`: `onRelease` once, selection kept; re-added: still selected | |
| `CancelGoesToHandlerElseGame` | Escape with `onCancel` on the panel's script: consumed and handled; without: the game's `onKeyDown(ESCAPE)` | |
| `PointerPressSelectionRules` | with a selection: clicking another Button selects it, clicking the label selects its Button, clicking the background deselects and clears history, clicking an InputField keeps it; without: nothing selected | |
| `NonInteractableAndCanvasGroup` | a disabled Button and one under a non-interactable group are skipped by navigation; a selected Button made non-interactable shows Disabled and Submit does nothing | |
| `TopSceneWithSelectionTakesKeys` | two stacked scenes with selections: the top navigates; covered by a loading scene: the lower one does | |
| `TypingGuardAcrossScenes` | lower scene InputField focused, upper scene with a selection: arrows and Space go to the field (cursor moves, space typed), the selection does not move, the game's `onKeyDown('W')` is not called | red (game gets W today) |
| `InputActionsSuppressedWhileSelected` | an `ignoreUI` action bound to Space does not fire while a Button is selected, fires when none is | |
| `ColorTintFadesOnUIClock` | fade 0.2: after `HtmlUIClock::advance(0.1)` `ui.color` is half way; at time scale 0 too | |
| `SpriteSwapAndNoneOwnership` | SPRITE_SWAP leaves `ui.color` as set; NONE leaves both; COLOR_TINT leaves `ui.texture`; switching mode restores Normal once | |
| `ScaleTransitionKeepsScriptScale` | `scaleHovered = 1.1`, hover, a script sets the scale ×2 meanwhile, unhover: the final scale is ×2 | |
| `ScriptMessagesLuaAndCpp` | Lua and C++ scripts receive `onSelect`, `onDeselect`, `onMove(e.moveDir)`, `onSubmit` (`e:use()` skips the click), `onCancel` | |
| `LuaButtonColorWritesNormal` | `Button(...).color = red`, hover, unhover: Normal is red | red |
| `GamepadStickHandBackComponentUI` | stick right held while a Button is selected (game got 0), the Button is destroyed: the next frame's game gets the current value, and an `ignoreUI` input action bound to the stick reads it again | |

**`jasperengine-htmlui-tests`, `engine/tests/htmlui/TestInputGamepad.cpp` / `TestInput.cpp`:**

| Test | Asserts | Red |
|---|---|---|
| `StickHeldWhenMenuHidesGoesBackToTheGame` | a focused document button, stick right 0.9 (game sees 0), hide the document, one frame: the game's `onGamepadAxisMove` gets 0.9 | red |
| `TypingFieldKeepsSpaceFromUpperDocument` | lower scene document with a focused text field, upper scene document with an autofocused button: Space types a space and the button is not clicked | red |

**`jasperengine-core-tests`, render, `*Gpu` suite (`engine/tests/core/render/UIBufferGrowthGpuTest.cpp`):**
`TextOutgrowingItsBufferReloads`. A loaded UI text gets a buffer larger than `gpuBufferBytes` without the reload
path (direct write); one draw makes no upload and sets `needReload`, and the sokol validation layer stays silent.
Red: a validation error or an overrun on `main`. If the headless device cannot load UI text, the test drives a
Polygon instead (same path).

**Android:** `static_assert(getJasperEngineKey(AKEYCODE_DPAD_CENTER, 0) == D_KEY_ENTER)` (compile-time, red on
`main` by construction), built with `tools/build.sh android`.

**MCP, `tools/mcp/tests/test_ui_buttons.py` (new):**

| Test | Asserts | Red |
|---|---|---|
| `test_nine_patch_label_not_clipped` | a button with a 9-patch texture (top and bottom margins 18, height 40) and label "Play": `screenshot` pixels of the label area contain the label colour | red |
| `test_set_properties_ui_color_on_button_refused` | default transition → error naming `button.colorNormal`; with `transition = sprite_swap` the same edit succeeds and persists after `get_entity` | red |
| `test_set_properties_button_field_refreshes` | `set_properties button.colorNormal` → `get_entity ui.color` updated in edit mode; `undo` → back | red |
| `test_create_ui_button_texture_sets_normal` | `create_ui` button with `texture`: `button.textureNormal` equals it | red |
| `test_create_ui_navigation_keys` | `textures`, `transition`, `fade_duration`, `scale`, `navigation` (names) and `select_on_enable` land in the right fields; a bad mode fails with the valid list | |
| `test_inspector_rows_read_only` | `get_inspector_layout` of a Button's Canvas Renderer shows Color and Texture disabled with the note | red |

**MCP, `tools/mcp/tests/test_ui_navigation.py` (new):**

| Test | Asserts |
|---|---|
| `test_get_entity_resolves_navigation` | a vertical menu of three, `wrap_around`: `selectable.navigation` down/up/next as expected, without Play |
| `test_keyboard_menu` | play; `get_play_state.input.ui_selected` is the `select_on_enable` button; `send_input` keys down, down, enter: the third is selected and its Lua `onClick` counter is 1; a game `onKeyDown` counter saw none of them |
| `test_gamepad_menu_and_hand_back` | d-pad and A drive the same; with the stick held right, closing the menu from `onClick` gives the game's axis handler the held value the next frame |
| `test_no_selection_hud` | a HUD button clicked with the mouse, then key space: the game's handler counts it, `ui_selected` empty |
| `test_selected_tint_screenshot` | `colors.selected` red: the Game view pixel at the selected button is red after the fade (`wait` 200 ms) |
| `test_get_runtime_entity_state` | `selectable.state` reads `selected`, then `pressed` while a key is held (`hold_frames`), then `disabled` after `set_runtime_property button.disabled` |

**Gates:**
- `tools/check-changed` on each commit;
- `tools/mcp/tool_budget.py --check tools/mcp/budget.json` and `tools/mcp/gen_tool_docs.py --check`;
- the Lua doc test of the docs' Lua blocks;
- `SANITIZE=address,undefined UNIT_TEST_ARGS=--gtest_filter='UIComponents*' tools/build.sh unit-tests`.

## 11. Acceptance criteria

1. **State textures:** `PressedWithoutTextureKeepsNormal` and `LoadingNormalTextureIsNotReplaced` were red on
   the parent commit and pass.
2. **Layout boxes:** `HiddenLayoutChildTakesNoBox` was red and passes. `ChildInactiveFromLoadReadsNoBox`
   passes under `SANITIZE=address,undefined`, and was flagged by UBSan before the change (or the absence of a
   report on `main` is recorded).
3. **Label clip:** `test_nine_patch_label_not_clipped` was red and passes. A Button in a clipping Panel still has
   its label clipped (asserted in the same test with a second button).
4. **Runtime canvas:** `RuntimeCanvasSizeUpdatesCameras` was red and passes.
5. **Text buffers:** `MaxTextSizeGrowthReloads` was red and passes. `TextOutgrowingItsBufferReloads` passes on
   glcore and Vulkan (lavapipe); its red run on `main` is recorded.
6. **Button edits:** `McpStyleEditRefreshesButton`, `DragPreviewMarksButton`,
   `test_set_properties_button_field_refreshes`, `test_set_properties_ui_color_on_button_refused`,
   `test_create_ui_button_texture_sets_normal`, `test_inspector_rows_read_only` and `LuaButtonColorWritesNormal`
   were red and pass.
7. **No swallowing:** `NoSelectionNoSwallow` and `ClickedHudButtonDoesNotTakeKeys` pass. `test_no_selection_hud`
   passes.
8. **Navigation:** every navigation test of 10 passes, including Navigation modes, Tab, Submit on the down,
   Cancel rules, pointer selection rules, history, scene stacking and Canvas Groups.
9. **Typing guard:** `TypingGuardAcrossScenes` and `TypingFieldKeepsSpaceFromUpperDocument` were red and pass.
10. **Stick hand-back:** `StickHeldWhenMenuHidesGoesBackToTheGame` was red and passes.
    `GamepadStickHandBackComponentUI` and `test_gamepad_menu_and_hand_back` pass.
11. **Android:** the DPAD_CENTER static assert compiles in `tools/build.sh android`.
12. **Transitions:** the transition tests pass. A Scene view screenshot from an isolated editor shows the
    Visualize arrows of a 2x2 menu. A Game view screenshot shows a selected button's tint. Both are kept outside
    the repository and linked in the report.
13. **Inspector:** shows Interactable, Transition, the state rows by mode, Fade Duration, Scale, Navigation (with
    Wrap Around or the Select On rows), Visualize and Select On Enable; every row undoes (checked with
    `get_inspector_layout` and `undo` in `test_ui_buttons.py`).
14. **Lua:** the Lua API data lists the new Button members, `UI.getSelected/setSelected`, the enums and the event
    data. LuaLS stubs regenerate. The docs' Lua blocks pass the doc test.
15. **Gates:** `tool_budget.py --check` passes. `gen_tool_docs.py --check` is clean after regenerating
    `TOOLS.md`. The agent guide stays within its budget. `tools/check_agent_docs.py` is clean if guide files
    changed.
16. **Suites:** `tools/check-changed` is green on the branch. The full suite (WERROR build, every unit test, MCP
    shard) is green at the batch landing.
17. **Docs:** `ui.md` documents 5 and 7, with the behaviour-change list of 9; the glossary has the Selectable
    table; lua-api, cpp-scripting, input, html-ui, animation and editor-tools are updated as in 6.11.

## 12. Files likely touched

Engine:
- `engine/core/component/ButtonComponent.h`, `UIComponent.h`; new `engine/core/component/UISelectable.h`;
- `engine/core/subsystem/UISystem.{h,cpp}`, `UISystemPointer.cpp`; new `UISystemNavigation.cpp`;
- **`engine/core/subsystem/RenderSystem.cpp`** (`applyPassScissor`, `passScissor`, `loadUI`, the UI buffer
  update);
- **`engine/core/Engine.{h,cpp}`** (key, char and gamepad routing, `updateGamepadNavigation`, `refreshCanvas`,
  `setCanvasSize`, `setScalingMode`, `systemViewChanged`, `removeScene` comment);
- `engine/core/Input.cpp` and `Input.h` (suppression), `engine/core/input/InputActions.cpp`
  (`suppressedByUI`);
- `engine/core/object/ui/Button.{h,cpp}`; new `engine/core/object/ui/UIEventData.h`;
- `engine/core/script/ScriptMessages.{h,cpp}`, `ScriptBase.{h,cpp}`;
- `engine/core/script/binding/GameObjectClassesLua.cpp`, `ECSClassesLua.cpp`, `CoreClassesLua.cpp`;
- `engine/core/animation/PropertyBindings.cpp` (`colorSelected` row, binding warning);
- `engine/platform/android/NativeEngine.cpp`;
- `engine/CMakeLists.txt` and the Xcode project (new sources).

Editor:
- `editor/reflect/components/UIReflect.cpp` and the enums file it uses;
- **`editor/Catalog.{h,cpp}`** (`UpdateFlags_Button`);
- **`editor/window/InspectorWindow.cpp`** (F9, F10), `InspectorDefault.cpp`; new `InspectorButton.cpp`;
  `InspectorWindow.h`;
- `editor/render/SceneRender2D.{h,cpp}`, `editor/AppSettings.{h,cpp}`;
- `editor/command/type/CreateEntityCmd.cpp`;
- **`editor/mcp/McpValues.cpp`**, `editor/mcp/tools/ComponentTools.cpp`, **`WorldTools.cpp`**,
  **`PlayTools.cpp`**;
- **`editor/mcp/McpInstructions.cpp`**;
- `editor/CMakeLists.txt`.

Tests and docs:
- `editor/tests/UIComponentsTest.cpp` (new), `editor/tests/reflect/catalog_snapshot.json`,
  `editor/tests/reflect/legacy/LegacyCodecs.cpp`, `tools/fixtures/reflection-parity/golden/inspector_layouts.json`;
- `engine/tests/htmlui/TestInputGamepad.cpp`, `TestInput.cpp`; `engine/tests/core/render/UIBufferGrowthGpuTest.cpp`;
- `tools/mcp/tests/test_ui_buttons.py`, `test_ui_navigation.py` (new), and existing MCP tests per 9.9;
- docs: `docs/mcp/guide/ui.md`, `lua-api.md`, `cpp-scripting.md`, `input.md`, `html-ui.md`, `animation.md`,
  `editor-tools.md`, `docs/unity-glossary.md`, `docs/mcp/TOOLS.md` (generated), `tools/lua_api/manual.py`.

Hot files are in bold: Engine.cpp, RenderSystem.cpp, InspectorWindow.cpp, Catalog, the MCP tools, and the agent
guide.

## 13. Cross-feature dependencies

| Feature | Overlap | Plan |
|---|---|---|
| `unicode-text` | rewrites `Engine::systemCharInput`, `UISystem::eventOnCharInput` (to `uint32_t`), InputField char handling | Land it first if both are ready. This branch then puts the typing guard on top of its combined code points; otherwise unicode-text keeps the guard's order (typing component field first) |
| `platform-lifecycle` | Engine.cpp input (web key map, web audio unlock on the first key in `systemKeyDown`, pads suspended when hidden) | Hidden games suspend pads: the hand-back must not send axis values to a hidden game. Its suspend path calls `Input::releaseAll`, which ends submits here. Merge the `systemKeyDown` order by hand |
| `scene-integrity` | Engine scene list (`removeScene`, scene replacement during Play, captured-cursor hand-back) | Keep `resetButtonStates()` in `removeScene`; a replaced scene's UISystem dies with its selection |
| `sprite-batching` | RenderSystem UI draw (batching UI quads) | The buffer guard of 6.7 and the Button scissor rule must hold in the batched path: a batch never spans a Button-label scissor change, and batched buffers keep a capacity check |
| `draw-perf` | `applyPassScissor` and the bindings and pipeline apply skipping | Same function; scissor results unchanged except the Button rule |
| `render-correctness` | RenderSystem | No shared function expected |
| `project-window` | InspectorWindow.cpp texture rows ("texture slots show file names") and drag-drop | F9's preview change is in the same drop code: merge by hand, keep the flag-driven mark |
| `agent-authoring` | agent guide budget; MCP `set_properties` path (edits held during a human save) | The refusal of 6.10 sits in `appendChangeCommands`, before any hold logic; the guide line is about 90 bytes |
| `scripting` | Lua bindings (GameObjectClassesLua, CoreClassesLua), subsystem bindings, Lua-only statics in C++ completion | `UI.getSelected/setSelected` are Lua statics: keep them out of C++ completion if that feature filters them |
| `runtime-core` | `FunctionSubscribe` exception reporting (`onClick` calls go through it); ScriptMessages | Use its guarded call when it lands |
| `mesh-memory` | possibly `Catalog` update flags | `UpdateFlags_Button` takes the next free bit at merge time |
| `mobile-shipping` | Android sources | `NativeEngine.cpp` key map only; AdMob removal does not touch it |
| `export-pipeline`, `build-toolchain`, `water-*`, `ssr-probes`, `code-editor-git` | none | |


## Deviations (implementation, 2026-11-06)

Recorded while implementing on `dc/ui-components` (main had moved: unicode-text's `char32_t` input, the
lean MCP rules, the sound layer).

1. **Submit hold when a scene leaves the layers** (6.4, 7.4, test `SubmitReleasedWhenSceneLeaves`): the hold ends
   without `onRelease`, as pointer presses already do there ("no callbacks into a scene being removed",
   `resetButtonStates`). The test is `SubmitHoldEndsWhenTheSceneLeaves`. App pause and focus loss
   (`cancelAllPointers`) still send `onRelease`.
2. **A lost selection** is one that is no longer selectable (destroyed, inactive, hidden, UIComponent off, no
   Button). Navigation set to None does not lose it: `UI.setSelected` selects any Button, as Unity's `Select()`.
3. **New Buttons keep white state colours and Fade Duration 0** (decision 10 reversed). Jasper's state colours
   replace the image colour; Unity's tint defaults (245/200 grey) would turn a coloured button grey on hover.
   `ui.md` gives Unity's 0.1 s fade as the suggestion; the `create_ui` keys set any of them.
4. **No animation binding warning** for a `ui.color` track on a Button (decision 13, last row): it needs a new
   `TrackBindingStatus`; the docs (animation.md), the read-only rows and Transition None cover it. A
   `colorSelected` property row was added.
5. **Agent guide:** no line added. The guide steers menus to the HTML UI (`create_ui_document`, eval case
   `guard_html_ui_template`); the component UI's navigation is in `jasper://docs/ui` and named by `create_ui`'s
   description. New eval cases: `guard_ui_button_gamepad_menu`, `guard_ui_button_color_refusal`.
6. **`Engine::wantsKeyboardNavigation()`** (C++ and Lua) added: the Input actions' suppression of the arrows,
   Tab, Enter and Space (`InputSuppression::keyboardNavigation`) and games that poll keys need it.
7. **Edit mode** is `UISystem::setEditMode`, called by `Project::pauseEngineScene`, which is also the editor's
   Play pause: while paused no Select On Enable runs and no scale applies; what shows stays.
8. **Tests:** the drag-preview test checks the update flags the preview now uses (`ButtonFieldEditsRefreshFromAnyPath`;
   the helper is file-static in `InspectorWindow.cpp`). `LoadingNormalTextureIsNotReplaced` and the GPU test
   `TextOutgrowingItsBufferReloads` are not written: the unit harness cannot hold a texture in `Loading`, and
   no GPU harness loads component UI; both fixes are covered by reading (inferred). The UBSan run of the
   inactive layout child is not run (no sanitizer tree built, per the brief's build rules). The MCP tests are one
   file, `tools/mcp/tests/test_ui_buttons.py`.
9. **A pointer press on a Button whose Navigation is None** (with a selection elsewhere) deselects, as a press on
   any non-selectable element.
10. **`get_entity` / `get_runtime_entity`** return `selectable {navigable, navigation {up, down, left, right, next}}`
    (+ `state`, `selected` in Play) with `{id, path}` targets; `get_play_state.input.ui_selected` is
    `[{scene, entity {id, path}}]`.
