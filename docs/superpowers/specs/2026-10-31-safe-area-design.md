# `safe-area`: safe-area insets, Safe Area UI and device simulation (design)

Status: binding spec for branch `gaps/safe-area` (ledger #4). Read with
[AGENT-BRIEF.md](../../engine-gaps/AGENT-BRIEF.md) and [GAP-REPORT.md §2](../../engine-gaps/GAP-REPORT.md).
Creates or extends `engine/core/Screen.{h,cpp}` (shared with `platform-services`, which adds
orientation/sleep/dpi; whichever branch merges first creates the file).

## 1. Goals

1. Real safe-area insets on Android (display cutout, all API levels the engine supports: 21+;
   cutouts exist from 28), iOS (`safeAreaInsets`), Web (`env(safe-area-inset-*)` of the page),
   desktop (zero, plus simulation), editor (simulated device).
2. Lua: `Screen.safeArea` (canvas coordinates), insets, pixel rect, cutouts,
   `Engine.onSafeAreaChanged`, and app-defined **extra insets** (a banner ad's height) that every
   consumer includes.
3. Native UI: a **Safe Area** component that drives a RectTransform to the safe area, so
   anchored children respect it; GameObject > UI > Safe Area creates one.
4. HTML UI: `env(safe-area-inset-top|right|bottom|left[, fallback])` resolves to the live insets
   (today forced to 0 px, `engine/core/htmlui/compat/css/ValueNormalizer.cpp:592-595`), updated
   without reloading the document.
5. Editor Game view **device simulation**: phone/tablet presets with notch / Dynamic Island /
   punch hole, rounded corners and home indicator drawn over the game, portrait/landscape,
   simulated insets fed to the engine, the same overlay in MCP screenshots; Scene view UI layout
   follows the simulated safe area in edit mode too.

## 2. Non-goals

Android rounded-corner insets (`WindowInsets.getRoundedCorner`, API 31; Unity ignores them too),
system-gesture insets, keyboard (IME) insets, foldable hinge areas, iOS per-device cutout rects
(`Screen.cutouts` is empty on iOS, like Unity outside its device table), a full device "skin"
image around the Game view, simulating `Engine.platform`.

## 3. Users and stories

- **Port of `CanvasHelper.cs:111-243`.** The canvas has a "SafeArea" child panel: add the Safe Area
  component to it (Left/Top/Right/Bottom on). The banner margin becomes
  `Screen.setExtraInsets(0, 0, 0, bannerHeight)` (canvas units) or
  `Screen.setExtraInsets(0, 0, 0, bannerPx, "pixels")`; the panel, `Screen.safeArea` and HTML
  `env()` all shrink by it. The game's `androidRenderOutsideSafeArea: 1` is the engine's default
  (fullscreen, drawing under the cutout, `MainActivity.java:37-39`).
- **Script user:** `RegisterEngineEvent(self, "onSafeAreaChanged")` and read `Screen.safeArea` to
  place a HUD element manually.
- **HTML UI author:** `.hud { padding-top: env(safe-area-inset-top, 0px); }` works as in a mobile
  browser, live on rotation.
- **Designer:** picks "iPhone 15 Pro" in the Game view, rotates to landscape, sees the Dynamic
  Island and corners, sees the safe panel move in the Game view and in the Scene view.
- **Agent:** `configure_game_view {device: "iphone_15_pro", orientation: "landscape_left"}`,
  `screenshot` (overlay included, `safe_area_in_image` returned), `eval_lua "return Screen.safeArea"`,
  `create_ui {type: "safe_area"}`.

## 4. Research summary

| Engine | API | Taken / left |
|---|---|---|
| Unity `Screen.safeArea` (pixels, bottom-left origin), `Screen.cutouts`, no change event, community `SafeArea` scripts (Crystal SafeArea, the game's CanvasHelper), Device Simulator with notch overlay ([safeArea](https://docs.unity3d.com/ScriptReference/Screen-safeArea.html), [cutouts](https://docs.unity3d.com/ScriptReference/Screen-cutouts.html), [Device Simulator](https://docs.unity3d.com/Manual/device-simulator-introduction.html)), Android *Render outside safe area* | Take: names `Screen.safeArea`/`cutouts`, a component like the community script, a simulator with device presets. Leave: pixel/bottom-left coordinates (the engine's UI and input use canvas units, top-left), polling-only (add an event). |
| Godot `DisplayServer.get_display_safe_area()` (Rect2i pixels), `get_display_cutouts()` ([docs](https://docs.godotengine.org/en/stable/classes/class_displayserver.html#class-displayserver-method-get-display-safe-area)) | Confirms pixel rect + cutout list. |
| Defold `extension-safearea` (`safearea.get_insets()`, `set_background_color`) | Insets as four numbers -> `Screen.safeAreaInsets`. |
| Web: CSS `env(safe-area-inset-*)` with `viewport-fit=cover` ([MDN](https://developer.mozilla.org/en-US/docs/Web/CSS/env)) | HTML UI env() semantics and the Web backend's source of truth. |
| Android `WindowInsetsCompat.Type.displayCutout()`, GameActivity `GameActivity_getWindowInsets` + `onWindowInsetsChanged` ([docs](https://developer.android.com/reference/games/game-activity/group/game-activity)) | Native read of cutout insets, no Java needed except cutout rects. |
| iOS `UIView.safeAreaInsets`, `viewSafeAreaInsetsDidChange` | iOS backend. |

## 5. Product decisions

1. **Coordinates:** `Screen.safeArea` is a `Rect` in **canvas coordinates, top-left origin** (the
   space of `Input.getTouchPosition`, UI layout and `Engine.canvasWidth/Height`), intersected with
   the canvas. Pixel versions (`Screen.safeAreaPixels`) are in view pixels, top-left origin. The
   glossary states the difference from Unity.
2. **Both a component and nothing canvas-wide.** A Safe Area **component** on a full-stretch panel
   (children anchor inside it), not a global "UI respects safe area" switch: backgrounds must keep
   bleeding under the notch while buttons stay inside; that is exactly how Unity users structure it.
3. **Extra insets** (`Screen.setExtraInsets`) are additive per edge on top of the device insets,
   and included by default everywhere (`Screen.safeArea`, the component, HTML `env()`);
   `Screen.deviceSafeArea`/`deviceSafeAreaInsets` give the device-only values; the component has
   **Include Extra Insets** (default on).
4. **Android insets = display cutout only** while fullscreen (the default `android.fullscreen`,
   immersive with transient bars that overlay the game; including bars would make UI jump when the
   user swipes them in), Unity's behaviour. Non-fullscreen apps get cutout max'd with system bars.
5. **Event** `Engine.onSafeAreaChanged()` fires once whenever `Screen.safeArea` (with extras)
   changes value, after `onCanvasChanged` and `onOrientationChanged` of the same change.
6. **HTML `env()`** compiles to a runtime custom property: `var(--jasper-safe-area-inset-top, <fallback or 0px>)`;
   the document system sets the properties on each document root. Live, no reload.
7. **Device simulation is editor-only and never saved into builds**, applies in edit mode and Play,
   and is the same source for the Game view, Scene view UI layout, Lua and MCP screenshots. Desktop
   exports can simulate with `JASPERENGINE_SIMULATE_SAFE_AREA` for testing.

## 6. Design

### 6.1 Runtime C++

**`EdgeInsets`** (`engine/core/math/EdgeInsets.h`): `struct EdgeInsets { float left = 0, top = 0,
right = 0, bottom = 0; bool operator==; bool isZero() const; }`; bound to Lua as class `EdgeInsets`
with properties `left, top, right, bottom`, constructor `EdgeInsets(l, t, r, b)`, `__tostring`, `__eq`.

**`Screen`** (`engine/core/Screen.{h,cpp}`) safe-area part:
```cpp
// platform side (engine thread): view pixels, top-left, relative to the view the engine draws into
static void setPlatformSafeArea(const EdgeInsets& insetsPx, const std::vector<Rect>& cutoutsPx);
static EdgeInsets getPlatformInsetsPixels();
// app side
static void setExtraInsets(const EdgeInsets& insets, bool pixels = false);   // canvas units unless pixels; negatives -> 0
static EdgeInsets getExtraInsets();                                          // canvas units
static Rect getSafeArea();            // canvas, extras included
static Rect getDeviceSafeArea();      // canvas, no extras
static EdgeInsets getSafeAreaInsets(); static EdgeInsets getDeviceSafeAreaInsets();   // canvas
static Rect getSafeAreaPixels();      // view pixels, extras included
static std::vector<Rect> getCutouts();                                       // canvas
static uint64_t getSafeAreaRevision();   // increments on every value change
static void updateSafeArea();            // recompute; fires Engine::onSafeAreaChanged on change
```
`Engine::onSafeAreaChanged` (`FunctionSubscribe<void()>`, lifecycle-class event: cleared with
`clearAllSubscriptions(true)` and `removeSubscriptionsByTag`, not paused) and `Engine.onSafeAreaChanged`
binding (`CoreClassesLua.cpp:755-783`).

**Computation** (`updateSafeArea`, called by `Engine::systemViewChanged` (`Engine.cpp:1224`) after
`computeViewLayout`, by `setPlatformSafeArea`, by `setExtraInsets`):
- screen `W x H` = `System::getScreenWidth/Height`; device safe pixel rect
  `S = [insets.left, insets.top, W - insets.right, H - insets.bottom]` (clamped, empty -> whole screen
  with a warning: a platform bug).
- `viewRect` V and canvas `cw x ch` (from `computeViewLayout`): map with
  `x_c = (x_px - V.x) * cw / V.w`, `y_c = (y_px - V.y) * ch / V.h` (the inverse of
  `Engine::transformCoordPos`, `Engine.cpp:1603`); device safe canvas rect = map(S) ∩ `[0, cw] x [0, ch]`.
  Consequences (documented): LETTERBOX bars absorb insets that fall inside them; CROP adds the
  cropped part to the unsafe area; STRETCH scales per axis.
- extras (canvas units; pixel extras converted with the same scale at set time and **re-converted on
  every recompute**: stored with their unit) shrink the rect per edge; a rect smaller than 1x1 is kept
  at 1x1 centred on the remaining area.
- insets (canvas) = distances from the canvas edges to the rect; cutouts mapped likewise and
  clipped to the canvas (empty ones dropped).
- compare with the previous values (exact float compare after rounding to 1/1000 canvas unit);
  on change increment the revision and call `Engine::onSafeAreaChanged`.

### 6.2 Lua API (`Screen` class, `CoreClassesLua.cpp`)

| Lua | Kind | Meaning |
|---|---|---|
| `Screen.safeArea` | ro `Rect` | canvas, top-left, extras included |
| `Screen.safeAreaInsets` | ro `EdgeInsets` | canvas units |
| `Screen.deviceSafeArea`, `Screen.deviceSafeAreaInsets` | ro | without extras |
| `Screen.safeAreaPixels` | ro `Rect` | view pixels, extras included |
| `Screen.cutouts` | ro table of `Rect` | canvas; empty when none/unknown |
| `Screen.extraInsets` | ro `EdgeInsets` | canvas |
| `Screen.setExtraInsets(left, top, right, bottom [, unit])` | fn | `unit` `"canvas"` (default) or `"pixels"`; numbers required (Lua error otherwise); persists across scene loads; reset at Play stop |
| `Screen.clearExtraInsets()` | fn | |
| `Engine.onSafeAreaChanged` | event `()` | `RegisterEngineEvent(self, "onSafeAreaChanged")` |

Values are copies (changing the returned `Rect` changes nothing). Valid in `init()` (zero insets
until the platform reports them; the event follows).

### 6.3 Safe Area component (native UI)

`engine/core/component/SafeAreaComponent.h`:
```cpp
struct JASPERENGINE_API SafeAreaComponent {
    bool left = true; bool top = true; bool right = true; bool bottom = true;
    bool includeExtraInsets = true;
    uint64_t appliedRevision = 0;   // runtime, not serialized
};
```
Registered like `LayoutGroupComponent` (`engine/core/registry/EntityRegistry.cpp:17`), cloned by
`EntityCloner`, included in `JasperEngine.h`.

**UISystem rule** (`engine/core/subsystem/UISystem.cpp:2252` loop, before the anchor math of each
layout): for an entity with `SafeAreaComponent` + `RectTransformComponent` + `Transform`, active in
hierarchy:
1. `ref = getAnchorReferenceRect(layout, transform, true)` (world-axis rect of the anchor
   reference; for a root UI element the canvas rect, `UISystem.cpp:1557-1610`).
2. `safe = includeExtraInsets ? Screen::getSafeArea() : Screen::getDeviceSafeArea()` (canvas = UI
   world units for screen-space UI).
3. `anchorPointLeft = left ? clamp((safe.x - ref.x) / ref.w, 0, 1) : 0`, `anchorPointTop` likewise
   with y/h, `anchorPointRight = right ? clamp((safe.x + safe.w - ref.x) / ref.w, 0, 1) : 1`,
   `anchorPointBottom` likewise; `usingAnchors = true`, `anchorPreset = NONE`, all four
   `anchorOffset* = 0`, `positionOffset = (0,0)`, `needUpdateAnchorOffsets = false`.
   (`ref.w`/`ref.h` <= 0: skip.) Normal anchor processing then sizes and positions it, and children
   anchor inside it. Rotated/scaled parents: the rect is applied axis-aligned (documented).
Edit mode: UISystem runs in the editor scene with the simulated safe area (6.6), so the Scene view
shows the result.

**Inspector** (`editor/window/InspectorWindow.cpp`, new `drawSafeAreaComponent`): checkboxes Left,
Top, Right, Bottom (one row, Unity's "Conform X/Y" made per edge), Include Extra Insets; a help line
showing the current safe area (`Safe area: x 0, y 47, 390 x 763 (simulated: iPhone 13)`). The
RectTransform inspector draws its anchor/offset/size fields disabled with the note "Driven by Safe
Area" when the entity has the component (like Unity's driven-properties label). Undo through the
existing property-change commands.
**Component menu**: Add Component > UI > Safe Area (`ComponentAddDialog.cpp`); GameObject > UI >
Safe Area (`HierarchyWindow.cpp` create menu, `CreateEntityCmd`): a "Safe Area" GameObject with a
full-layout RectTransform (`AnchorPreset::FULL_LAYOUT`) and the component, parented to the selected
UI object if any.
**Serialization**: `editor/Stream.cpp` key `safearea:` with `left, top, right, bottom,
includeExtraInsets` (only non-defaults), prefabs too (`StreamPrefab.cpp` uses the same encoders);
`editor/Factory.cpp` generates the component for exported scenes; `editor/Catalog.{h,cpp}` adds
`ComponentType::SafeAreaComponent` with fast properties (names `left, top, right, bottom,
includeExtraInsets`), display name "Safe Area", icon `ICON_FA_MOBILE_SCREEN_BUTTON`; MCP values
(`editor/mcp/McpValues.cpp`). Lua: bind `SafeAreaComponent` fields in `ECSClassesLua.cpp` and a
`getSafeAreaComponent()` accessor wherever the UI wrappers expose their components (follow
`LayoutGroupComponent`).
**MCP**: `set_properties` on driven RectTransform fields of such an entity fails with "driven by
the Safe Area component (remove it or edit the Safe Area edges)". `create_ui` (`WorldTools.cpp`)
gains `type: "safe_area"`.

### 6.4 HTML UI `env()`

- `ValueNormalizer::env` (`ValueNormalizer.cpp:576-604`): for `safe-area-inset-{top,right,bottom,left}`
  emit `var(--jasper-safe-area-inset-<edge>, <normalized fallback or 0px>)` and report nothing.
  The `CustomPropertyIndex` / `resolveVar` must treat `--jasper-safe-area-*` (and
  `--jasper-device-safe-area-*`) as **dynamic** (no static value, never folded), so values reach
  RmlUi as `var()`, which the fork substitutes at runtime (`libs/rmlui/Source/Core/ElementStyle.cpp:381-560`).
  Inside `calc()` the calc-native path keeps the var (`NormalizeContext::calcNative`); where compat
  must fold a value statically (decorator arguments, token-by-token rewrites), it uses 0 px and
  reports `ApproximatedMinor` Info "env(safe-area-inset-*) is 0 here (static value)".
- Also accepted: `env(safe-area-max-inset-*)` -> same variables (iOS Safari name).
- `UIDocumentSystem` (`engine/core/subsystem/UIDocumentSystem.cpp`): after a document loads
  (`applyComponent` `:136-188`) and in `update` (`:190`) when `Screen::getSafeAreaRevision()` changed
  since the last apply, set on each screen-mode document's root element the custom properties
  `--jasper-safe-area-inset-{top,right,bottom,left}` (Screen insets, extras included) and
  `--jasper-device-safe-area-inset-*` (without), in CSS px of the document: canvas units times
  `layoutWidth / Engine::getCanvasWidth()` (x) and `layoutHeight / canvasHeight` (y) with the layout
  size of `getLayoutSize` (`:118`). Texture-mode documents (on meshes) get 0. Setter: the fork's
  custom property API (`ElementStyle::SetCustomProperty`, `ElementStyle.cpp:845`); expose it as
  `Rml::Element::SetCustomProperty(const String& name, const String& value)` in the fork if not public
  (one function, plus a comment pointing to this spec).
- Tables/docs: `compat/tables/UnitTable.cpp:107` row text "safe-area-inset-* -> the live safe area
  (Screen.safeAreaInsets); other variables -> the fallback or dropped (Info)"; regenerate
  `docs/mcp/guide/html-ui-support.md` (`tools/mcp/gen_html_ui_support.py`); `html-ui.md` gets a
  "Safe areas" subsection; `docs/html-ui/phase-2-compat.md:1280,4262` notes updated.

### 6.5 Platform implementations

**Android** (`engine/platform/android/NativeEngine.cpp`):
- On `APP_CMD_WINDOW_INSETS_CHANGED` (GameActivity's native-app-glue command for
  `onWindowInsetsChanged`; add the case in `handleCommand`, `:1354`) and whenever the surface size
  changes (`doFrame`, `:1090-1097`): `GameActivity_getWindowInsets(activity,
  GAMECOMMON_INSETS_TYPE_DISPLAY_CUTOUT, &cut)`; when `android.fullscreen` is off also
  `GAMECOMMON_INSETS_TYPE_SYSTEM_BARS` and take per-edge max. (The fullscreen flag reaches native
  code through a new `MainActivity.isImmersive()` JNI query, cached at setup.) Cutout rects:
  `JasperPlatform.getCutoutRects(activity)` (Java, API 28+: `getWindow().getDecorView().getRootWindowInsets().getDisplayCutout().getBoundingRects()`,
  flattened `int[]`; empty below 28), JNI on change only. Then `Screen::setPlatformSafeArea(...)`
  on the game thread (it is the engine thread).
- API 21-27: cutout insets are 0 (no cutout API); waterfall insets are part of the cutout insets.
- Insets are in window pixels; the surface fills the window in fullscreen mode, which the engine
  uses (`WindowCompat.setDecorFitsSystemWindows(false)`, `LAYOUT_IN_DISPLAY_CUTOUT_MODE_ALWAYS`).
- Verify the glue enum name against `android_native_app_glue.h` of games-activity 2.0.2 at
  implementation time; if the command is missing, poll `GameActivity_getWindowInsets` once per
  frame (cheap) and compare.

**iOS** (`engine/platform/apple/ios/ViewController.m`): override `viewSafeAreaInsetsDidChange` (and
call once after `viewDidLoad`'s renderer init): insets `self.view.safeAreaInsets` (points) times
`self.view.contentScaleFactor` -> drawable pixels (the engine's screen size is `drawableSize`,
`Renderer.mm:68-70`); `Screen::setPlatformSafeArea(insets, {})` on the main thread (engine
thread). `prefersHomeIndicatorAutoHidden` (`ios.hide_home_indicator`) does not remove the bottom
inset (iOS keeps it; documented). macOS: zero.

**Web** (`engine/platform/emscripten/JasperEngineWeb.cpp`): `EM_JS` creates a probe
`<div id="jasper-safe-area">` (`position:fixed; left:0; top:0; width:0; height:0; visibility:hidden;
pointer-events:none; padding: env(safe-area-inset-top,0px) env(safe-area-inset-right,0px)
env(safe-area-inset-bottom,0px) env(safe-area-inset-left,0px)`), reads `getComputedStyle` paddings
(CSS px of the viewport), intersects the viewport safe rect with the canvas
`getBoundingClientRect()` to get canvas-local CSS px (the engine's screen size on Web is the canvas
CSS size, `renderLoop` `:625-632`), re-reads on `resize`, `orientationchange` and
`visualViewport.resize`, and pushes through an `EMSCRIPTEN_KEEPALIVE` function
`jasper_web_safe_area(float l, float t, float r, float b)` -> `Screen::setPlatformSafeArea`.
Export (`Exporter::writeExportSettingsScript`, `editor/Exporter.cpp:2736`): new setting
`web.viewport_fit_cover` (bool, default true): in `apply.cmake`, if the page has a
`<meta name="viewport" ...>` add `viewport-fit=cover` to its `content` when missing, else insert
`<meta name="viewport" content="width=device-width, initial-scale=1, viewport-fit=cover">` into `extra`.

**Desktop** (Linux, Windows, macOS backends): zero, unless the environment variable
`JASPERENGINE_SIMULATE_SAFE_AREA="top,right,bottom,left"` (view pixels, integers) is set at start:
then those fixed pixel insets are pushed at `systemViewLoaded` and kept on resize (the mapping
to canvas units is recomputed by every `systemViewChanged`). Read in `Engine::systemViewLoaded`
(`Engine.cpp:1196`) through a base `System::getSimulatedSafeArea()` helper so every desktop gets it
without per-backend code. Documented for testing desktop builds (`run_build` with `args`/env).

**Editor**: `editor::Platform` never reports insets itself; the Game view (6.6) pushes the simulated
insets via `Screen::setPlatformSafeArea` before `Engine::systemViewChanged` whenever the device,
orientation or Game view size changes.

### 6.6 Editor: Game view device simulation

New `editor/window/DeviceProfiles.{h,cpp}`:
```cpp
enum class CutoutShape { None, Notch, Island, PunchHole };
struct DeviceProfile {
    const char* id; const char* name; const char* platform;  // "ios"|"android"
    int width, height;            // portrait pixels
    float dpi; float cornerRadius; // pixels
    CutoutShape cutout; Rect cutoutRect;   // portrait pixels (PunchHole: the circle's bounding box)
    EdgeInsets portrait, landscapeLeft, landscapeRight, portraitUpsideDown;   // pixels, in that orientation's frame
    bool homeIndicator;           // iOS bar: 134x5 pt, 8 pt from the bottom, scaled by the pt->px factor
    const char* model;            // System.getDeviceModel() in Play ("iPhone16,1")
};
const std::vector<DeviceProfile>& deviceProfiles();
EdgeInsets profileInsets(const DeviceProfile&, ScreenOrientation);   // landscape frames: width/height swapped
void rasterizeDeviceOverlay(uint8_t* rgba, int w, int h, const DeviceProfile&, ScreenOrientation, bool safeAreaOutline); // CPU, for screenshots
void drawDeviceOverlay(ImDrawList*, ImVec2 min, ImVec2 max, const DeviceProfile&, ScreenOrientation, bool safeAreaOutline);
```
Presets (binding defaults; pt values x scale; source: Apple HIG layout and device specs, Android
device cutout reports):

| id | name | px (portrait) | scale | corner px | cutout (portrait px) | portrait insets t/b | landscape insets l/r/b |
|---|---|---|---|---|---|---|---|
| `iphone_15_pro` | iPhone 15 Pro | 1179x2556 | 3 | 165 | Island 378x111 at y=33, centred | 177 / 102 | 177 / 177 / 63 |
| `iphone_15_pro_max` | iPhone 15 Pro Max | 1290x2796 | 3 | 165 | Island 378x111 at y=33 | 177 / 102 | 177 / 177 / 63 |
| `iphone_13` | iPhone 13 / 14 | 1170x2532 | 3 | 142 | Notch 486x99 at y=0 | 141 / 102 | 141 / 141 / 63 |
| `iphone_x` | iPhone X / XS / 11 Pro | 1125x2436 | 3 | 120 | Notch 627x90 at y=0 | 132 / 102 | 132 / 132 / 63 |
| `iphone_se` | iPhone SE (3rd gen) | 750x1334 | 2 | 0 | None | 0 / 0 | 0 / 0 / 0 |
| `ipad_pro_11` | iPad Pro 11" | 1668x2388 | 2 | 36 | None | 0 / 40 | 0 / 0 / 40 |
| `pixel_8` | Google Pixel 8 | 1080x2400 | 2.625 | 100 | PunchHole 84x84 at (498, 44) | 128 / 0 | LL: 128 / 0 / 0; LR: 0 / 128 / 0 |
| `pixel_3_xl` | Google Pixel 3 XL | 1440x2960 | 3.5 | 90 | Notch 560x171 at y=0 | 171 / 0 | LL: 171 / 0 / 0; LR: 0 / 171 / 0 |
| `galaxy_s23` | Samsung Galaxy S23 | 1080x2340 | 2.8125 | 90 | PunchHole 80x80 at (500, 34) | 110 / 0 | LL: 110 / 0 / 0; LR: 0 / 110 / 0 |
| `custom` | Custom | user W x H | 1 | user | None | user | user |

iOS landscape insets are symmetric left/right (both orientations); portrait upside-down: iOS
iPhones do not support it (profile allows it only on iPad: same as portrait); Android
upside-down: bottom = portrait top. LL = Unity LandscapeLeft (device rotated counter-clockwise: the
portrait top edge is on the left).

**Game view** (`editor/window/GameWindow.{h,cpp}`): toolbar combo **Device** (left of the aspect
combo; "None" + presets + "Custom..."), a **Rotate** button (cycles the profile's orientations:
portrait -> landscape left -> landscape right -> portrait), and toggles **Device Overlay** and
**Safe Area** outline (icons, tooltips). With a device: the aspect combo is disabled and shows the
device aspect; the game area is the largest rect of the device's aspect in the current orientation
(existing fit logic, `GameWindow.cpp:446-455`); scale factor `s = renderWidth / deviceWidthInOrientation`;
simulated insets = `profileInsets * s` (rounded to pixels), pushed with `Screen::setPlatformSafeArea`
(cutouts = the cutout rect transformed to the orientation and scaled) before
`Engine::systemViewChanged`; in edit mode the same insets are converted to canvas units for the
editor scene (the edit-mode UI lays out against the project canvas through
`UISystem::setAnchorReferenceSize(canvasWidth, canvasHeight)`, as `editor/Project.cpp:5880` restores at Play stop): `Screen` computes against the scene's anchor reference size via
`Screen::setEditorReference(cw, ch)` so Scene view and Game view both show the effect.
The overlay (`drawDeviceOverlay`) is drawn over the image: black fill outside the rounded corners,
the cutout in black, the home indicator (light gray, 60% opacity), and (toggle) a 1-px dashed green
safe-area rectangle. Input is unaffected. `GameViewSettings` (`editor/AppSettings.h:51`) gains
`std::string device = ""; int orientation = 1; bool deviceOverlay = true; bool safeAreaOutline = false;
CustomDevice custom{}` (saved in editor preferences, `AppSettings.cpp:808-815`), equality updated.
In Play with a device: `System.getDeviceModel()` returns the profile's model, `System.getDeviceType()`
Handheld, `Screen.dpi` the profile dpi (platform-services); `Screen.orientation` is the simulated
orientation and script requests rotate it (platform-services 6.5).
The Stats overlay adds `Safe area: x,y w x h` (canvas).

### 6.7 MCP / agent surfaces

- `configure_game_view` (`editor/mcp/tools/ViewTools.cpp:1355`): new args `device` (preset id,
  `"none"` or `"custom"`), `orientation` (`"portrait"|"landscape_left"|"landscape_right"|"portrait_upside_down"`),
  `device_overlay` (bool), `safe_area_outline` (bool), `custom_device` ({width, height,
  insets {top,right,bottom,left}, corner_radius}); its read output lists `devices: [ids]` and the
  current `safe_area` (canvas) and `safe_area_pixels`. Allowed while playing (rotates the device).
- `screenshot` (`ViewTools.cpp:1267`), `view: "game"`: when a device is active the PNG has the
  overlay rasterized (`rasterizeDeviceOverlay`, parameter `device_overlay` default true) and the
  result adds `safe_area_in_image` (rect in image pixels).
- `get_play_state`: `"safe_area": {"rect": [x,y,w,h], "insets": [l,t,r,b], "extra": [l,t,r,b], "device": "iphone_15_pro"|null}`.
- `create_ui` type `safe_area`; `set_properties`/`get_entity` for `SafeAreaComponent`.
- Guide line: "Safe areas: Screen.safeArea (canvas), the Safe Area component, CSS env(); simulate
  with configure_game_view device". Details in `docs/mcp/guide/safe-area.md`.
- TOOLS.md regenerated; tool budget passes (descriptions terse).

### 6.8 Exported-build path

Runtime insets come from the platform, nothing generated, except: `SafeAreaComponent` through
`Factory.cpp` (scenes and prefabs), `web.viewport_fit_cover` in `apply.cmake`, and the Android
fullscreen flag readable at runtime (`MainActivity.isImmersive()`; the exporter's non-fullscreen
patch, `Exporter.cpp:3128-3136`, also changes it to return false).

### 6.9 Docs to update

New `docs/mcp/guide/safe-area.md` (coordinates, extras, component, HTML env, devices, desktop env
var, letterbox/crop effects, iOS home indicator note); `lua-api.md` (`Screen` rows, `EdgeInsets`,
event); `html-ui.md` + support table; `docs/unity-glossary.md`: `Screen.safeArea` -> `Screen.safeArea`
(canvas units, top-left origin), `Screen.cutouts` -> `Screen.cutouts`, community SafeArea script ->
Safe Area component, Device Simulator -> Game view Device menu, *Render outside safe area* -> default
Android fullscreen; README ("safe areas and device simulation").

## 7. Behaviour rules and edge cases

1. Main thread only; every platform pushes on the engine thread (Android game thread in
   `handleCommand`/`doFrame`, iOS main, Web single thread, editor main).
2. Event order on rotation: `onCanvasChanged` -> `onOrientationChanged` -> `onSafeAreaChanged`;
   `onSafeAreaChanged` at most once per frame (coalesced: `updateSafeArea` marks dirty, the event
   fires at the end of `systemViewChanged` or, for pushes between frames, at the start of the next
   `runFrame` update).
3. Scene loads keep platform insets and extras; Play stop clears extras and simulation-free state is
   restored at the next Play start (editor pushes again).
4. Time scale / pause: unaffected; insets change while paused are applied on resume (no event while
   the app is backgrounded; one event after resume if the value differs).
5. The component on a non-root element whose parent is smaller than the safe area: anchors clamp to
   0..1 (it fills the parent).
6. Disabled component or inactive entity: the RectTransform keeps its last driven values and the
   fields become editable again only after removing the component (Unity-like).
7. Several SafeArea components (nested) each apply against their own reference: nested panels
   inside a safe panel end up with 0..1 anchors (already safe).
8. HTML documents loaded later get the current values at load; documents with `env()` in inline
   `style` attributes behave the same (inline values go through the same normalizer).
9. Extra insets larger than the safe area: the rect is 1x1 (logged once as a warning).
10. Simulation off ("None"): insets zero in the editor, exactly like a desktop build.

## 8. Performance

`updateSafeArea` is O(cutouts) and runs only on view changes/pushes. The component costs one
`getAnchorReferenceRect` per Safe Area entity per UISystem update. Document custom properties are
set only when the revision changes (4 + 4 `SetCustomProperty` calls per document). Overlay drawing:
a few dozen ImGui primitives.

## 9. Tests

**Unit (gtest, `editor/tests/SafeAreaTest.cpp`)** with a test `System` of a given screen size:
1. Mapping for FITWIDTH, FITHEIGHT, LETTERBOX (insets inside the bars -> 0, partially -> reduced),
   CROP (cropped part added), STRETCH, NATIVE: expected canvas rects for given pixel insets.
2. Extras additive, pixel extras re-converted after a canvas change, oversize extras -> 1x1 rect.
3. Event fired once per change, not for an identical push; revision increments; order after
   `onCanvasChanged`.
4. Cutout mapping/clipping.
5. SafeArea component: root element anchors equal the normalized safe rect; edges off -> 0/1;
   nested under a half-width parent -> clamped; `includeExtraInsets` false ignores extras; anchors
   update after a Screen change in the next `UISystem::update`.
6. Stream round trip of the component (non-defaults only), prefab instance override, `Factory`
   output compiles (generated code snippet compared to expected text).
7. `profileInsets` for each preset and orientation; `rasterizeDeviceOverlay` on a 200x400 white
   image: corner pixels black, centre white, cutout pixels black, home bar gray.
8. `JASPERENGINE_SIMULATE_SAFE_AREA` parsing (valid, malformed -> ignored with a warning).

**HTML UI tests** (standalone engine build, `engine/tests/htmlui`, new `TestSafeAreaEnv.cpp`):
normalizer output for `env(safe-area-inset-top)`, with fallback, inside `calc()`, unknown env name
unchanged behaviour; a document with `padding-top: env(safe-area-inset-top)` lays out with the
inset from `Screen`, and after `Screen::setPlatformSafeArea` changes, the next update re-lays out
without reloading (element offset changes; document pointer identical).

**MCP e2e (`tools/mcp/tests/test_safe_area.py`)**: `configure_game_view {device:"iphone_13", orientation:"portrait"}`;
`eval_lua` -> `Screen.safeArea` equals the scaled expected rect (tolerance 1 canvas unit); rotate to
landscape -> values swap, a Lua handler counted one `onSafeAreaChanged`; `create_ui {type:"safe_area"}`
with a child button anchored top-right -> `get_runtime_entity` shows the button inside the safe
rect; screenshot has black corner pixels and `safe_area_in_image`; `Screen.setExtraInsets(0,0,0,50)`
moves the panel; an HTML document with `env()` padding: `html_ui` query of computed `padding-top`
equals the inset; device `none` -> zero insets. Edit mode: the Scene view entity layout follows the
device (get_entity RectTransform values).

**Compile checks**: Android `assembleDebug` (glue command, JNI), Web build (probe, viewport meta),
iOS by review.

## 10. Acceptance criteria

- [ ] `Screen.safeArea/safeAreaInsets/deviceSafeArea/deviceSafeAreaInsets/safeAreaPixels/cutouts/extraInsets/setExtraInsets/clearExtraInsets`,
      `EdgeInsets`, `Engine.onSafeAreaChanged` bound; stubs clean.
- [ ] Android cutout insets (fullscreen rule), iOS safeAreaInsets, Web env probe + viewport-fit,
      desktop zero + env var simulation.
- [ ] Safe Area component with Inspector, menus, serialization, export (Factory), MCP; driven-field rule.
- [ ] HTML `env(safe-area-inset-*)` live via custom properties; support table regenerated.
- [ ] Game view Device menu, rotate, overlay, outline; edit-mode layout follows; MCP
      `configure_game_view`/`screenshot`/`get_play_state` additions.
- [ ] All unit, HTML UI and MCP tests pass; Android and Web compile; docs and glossary updated.

## 11. Integration points

| File | Change |
|---|---|
| `engine/core/Screen.{h,cpp}`, `engine/core/math/EdgeInsets.h` | new / extended. |
| `engine/core/Engine.{h,cpp}` (`systemViewChanged :1224`, `systemViewLoaded :1196`, event lists `:915-992`) | recompute, event, desktop env simulation. |
| `engine/core/component/SafeAreaComponent.h`, `engine/core/registry/{EntityRegistry,EntityCloner}.cpp`, `engine/core/JasperEngine.h` | component. |
| `engine/core/subsystem/UISystem.cpp:2252` | driving rule. |
| `engine/core/subsystem/UIDocumentSystem.cpp:136-225` | custom properties. |
| `engine/core/htmlui/compat/css/ValueNormalizer.cpp:576-604`, `CustomPropertyIndex`, `tables/UnitTable.cpp:107` | env() rewrite. |
| `engine/libs/rmlui` (Element) | public custom property setter if missing. |
| `engine/core/script/binding/{CoreClassesLua,ECSClassesLua,MathClassesLua}.cpp` | `Screen`, event, component, `EdgeInsets`. |
| `engine/platform/android/{NativeEngine.cpp, java/.../MainActivity.java, JasperPlatform.java}` | insets, cutouts, `isImmersive`. |
| `engine/platform/apple/ios/ViewController.m` | `viewSafeAreaInsetsDidChange`. |
| `engine/platform/emscripten/JasperEngineWeb.cpp` | probe. |
| `editor/window/{GameWindow,DeviceProfiles}.{h,cpp}`, `editor/AppSettings.{h,cpp}` | device simulation. |
| `editor/window/InspectorWindow.cpp`, `dialog/ComponentAddDialog.cpp`, `HierarchyWindow.cpp`, `command/type/CreateEntityCmd.cpp` | UI. |
| `editor/{Stream,StreamPrefab,Factory,Catalog}.cpp`, `editor/mcp/McpValues.cpp` | serialization, export, catalog. |
| `editor/mcp/tools/{ViewTools,PlayTools,WorldTools}.cpp` | MCP. |
| `editor/Exporter.cpp:2736-2830` | viewport meta. |

## 12. Cross-feature dependencies

- **platform-services**: `Screen` class, orientation (simulated rotation, `onOrientationChanged`
  ordering), device model/dpi in simulation. Coordinate the shared Game view toolbar (one Device
  menu holding device, rotate, language).
- **ui-scaling-text** (canvas match width/height): the safe area is computed from `computeViewLayout`;
  a new scaling mode must keep the pixel->canvas mapping in `Screen::updateSafeArea` consistent
  (use the same view rect).
- **native-bridge**: banner SDK wrappers call `Screen.setExtraInsets` (through Lua or C++).
- **debug-overlay**: should respect the safe area for its on-device console.
- **ui-multitouch**: on-screen sticks placed inside a Safe Area panel.

## Implementation notes

Decisions taken while implementing (branch `gaps/safe-area`, on `gaps/platform-services`), where the
spec was silent, ambiguous or did not fit the code:

1. **The first value is a baseline, not a change.** `Screen` computes the safe area lazily (the first
   getter or `updateSafeArea`), and that first value fires no event; every later change does. Editor Play
   start and stop (`Engine::resetApplicationState`) call `Screen::resetSafeAreaState()`: no extras, no
   editor reference, the next value a baseline again. The revision keeps counting across resets (the HTML
   documents compare it). So an exported game gets one `onSafeAreaChanged` when the view and the
   platform's insets arrive after `init()`, as the spec's "zero insets until the platform reports them;
   the event follows".
2. **Coalescing (rule 7.2).** `updateSafeArea` only marks the event pending; it fires at the end of
   `Engine::systemViewChanged` (after `onCanvasChanged` and `onOrientationChanged`) or at the start of
   the next `runFrame`, never while the app is paused (it waits for the first frame after `onResume`).
   A platform that reports the new insets and the new surface size in different frames (Android may) can
   give two events for one rotation; each carries a real value.
3. **Edit mode (6.6) needs the screen too.** `Screen::setEditorReference(canvasWidth, canvasHeight,
   screenWidth, screenHeight, scaling)`: the Game view passes the project canvas, its render size and the
   project's scaling every frame while stopped; `Screen` lays the canvas out as a Play at that size would
   (`computeViewLayout`) and scales the result to the project canvas (the same fractions of the canvas
   per edge), so LETTERBOX/CROP effects match Play. `Screen::getSafeAreaCanvasWidth/Height()` tell
   consumers which canvas the values are in.
4. **The Safe Area component and HTML use fractions of Screen's canvas**, not raw canvas units: the
   component maps the safe rectangle to the scene's root UI rect (the editor's anchor reference size, or
   the Engine canvas), the document system to the document's layout size. That is exact today and keeps
   working when `ui-scaling-text` makes UI units differ from canvas units (its boundary, §17 there: the
   insets are never part of the scale).
5. **2D (y-up) scenes.** The UI lays out top-down in layout space whatever the camera, so a scene whose
   camera is not `CAMERA_UI` shows layout "top" at the screen's bottom (`tools/mcp/tests/test_ui_anchors.py`).
   The component mirrors the vertical edges there: the notch (screen top) drives the layout's bottom
   anchor and the `top` field means the screen's top edge in every scene.
6. **`SafeAreaComponent.enabled`** (the Inspector checkbox, Unity's component toggle) was added: rule 7.6
   speaks of a disabled component. Off or inactive, the RectTransform keeps its last anchors and stays
   driven (its fields editable again only once the component is removed).
7. **Inspector edges row.** Left/Top/Right/Bottom are one row of four checkboxes (each an undoable
   `PropertyCmd`); because it is a custom row, those four do not show the bold "overridden" label of a
   prefab instance (the override itself works: `prefabinternal::diffComponentProperties` lists them).
   A missing RectTransform shows a warning (there is no RequireComponent in the editor).
8. **Driven fields.** The RectTransform inspector disables width, height, Use Anchors, the anchors, the
   offsets and the position offset under "Driven by Safe Area"; MCP refuses them in `set_properties`
   (`McpValues::safeAreaDrivenError`) and `set_rect_layout`, and `create_ui` refuses a `layout` on a
   `safe_area` node. Scene view drag handles are not blocked: the component puts the values back at the
   next UI update.
9. **HTML custom properties need no fork change.** `Rml::Element::SetProperty(name, value)` already
   parses `--custom` declarations into the element's local custom properties, so the document system sets
   `--jasper-safe-area-inset-*` and `--jasper-device-safe-area-inset-*` on each document element with it
   (values like `47px`, rounded to 1/1000). `CustomPropertyIndex::isDynamic` keeps these variables out of
   every static resolution (`staticValue`, `resolve`, `expandedValues`). env() inside a math function is
   rewritten to the variable first (StaticCalc has no `env()`); without the fork's calc() it folds with
   0px and an Info diagnostic. Reloaded documents get the properties before their first layout.
10. **Android: insets relative to the surface, polled.** GameActivity's insets are the window's; a
    windowed game's surface may not start at the window's corner, so `JasperPlatform` keeps (on the UI
    thread, from a layout listener) where the content view lies in the window and the cutout rectangles
    (`DisplayCutout.getBoundingRects()`, API 28+) in a volatile `int[]` with a revision; the native side
    subtracts the offsets. `NativeEngine::updateSafeArea` runs on `APP_CMD_WINDOW_INSETS_CHANGED` (present
    in games-activity 2.0.2's glue), with each new surface size and once per frame (two struct reads and
    one int field: nothing is pushed unless something changed).
11. **`MainActivity.isFullscreenGame()`**, not `isImmersive()`: `android.app.Activity.isImmersive()`
    already exists (a system flag) and must not be overridden. The exporter's non-fullscreen patch turns it
    to `return false`; the native side reads it once at setup.
12. **iOS** scales `safeAreaInsets` by the drawable/bounds ratio per axis (equal to `contentScaleFactor`
    when the drawable follows the view), on `viewSafeAreaInsetsDidChange`, after `viewDidLoad` and after a
    rotation transition.
13. **Web**: the probe is read again with every canvas size change (`changeCanvasSize`, the canvas resize
    callback) besides `resize`, `orientationchange` and `visualViewport.resize`; values are pushed only on
    a change. The viewport meta is edited by a regex in `apply.cmake` (a `content=` that lacks
    `viewport-fit` gets `viewport-fit=cover, ` prepended; a page without a viewport meta gets the spec's).
14. **Desktop simulation** is skipped in the editor (its Game view simulates devices); the parser accepts
    integers with spaces around them and rejects anything else (negative, fractional, missing or extra
    values) with one warning.
15. **Overlay API.** `rasterizeDeviceOverlay` / `drawDeviceOverlay` take two more (defaulted) arguments:
    the outlined rectangle in image pixels (`Screen.safeAreaPixels` in Play, extras included; stopped, the
    device's own safe rectangle) and `screen` (false: the outline only, for Device Overlay off and Safe
    Area on). Notches are rounded on their inner side only; the Dynamic Island is a pill, punch holes
    circles.
16. **Custom device**: `CustomDeviceSettings` (portrait size, insets, corner radius) lives in
    `AppSettings.h` with the other Game view preferences (`custom_device` in the editor settings); its
    profile (id `custom`) turns the portrait insets for each orientation, has no cutout shape, dpi 0
    (unknown) and allows all four orientations. `configure_game_view {custom_device}` alone selects it.
17. **`configure_game_view` waits three frames** before replying when it changed something, so the
    reply's `safe_area` / `safe_area_pixels` are those of the new layout (the Game view lays a new device
    out in its next frame). `get_play_state.safe_area` is reported stopped too (the edit-mode values).
    `create_entity` also gets `type: "safe_area"`.
18. **Guide budget.** The guide line shares one bullet with the platform-services line; four sentences
    elsewhere were shortened to stay within 16 KB.
19. **Merge note:** `ComponentType::SafeAreaComponent` is appended last (prefab override masks use the
    ordinal): a branch appending another type at the same place conflicts there and in
    `AnimationClipContractTest`. (The ECS `Signature` limit of 64 types was raised to 128 on `main`.)
