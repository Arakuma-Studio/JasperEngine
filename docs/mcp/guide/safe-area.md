# Safe areas: notches, rounded corners, banners

Phones draw the game under their notch, Dynamic Island, punch-hole camera, rounded corners and
the iOS home indicator. The **safe area** is the part of the screen none of them covers. The
engine reports it on every platform (Android display cutout, iOS `safeAreaInsets`, the web page's
`env(safe-area-inset-*)`; zero on desktops), keeps the UI inside it with the **Safe Area**
component or CSS `env()`, and simulates phones in the editor's Game view.

## Coordinates

| Lua | Kind | Meaning |
|---|---|---|
| `Screen.safeArea` | ro `Rect` | canvas units, top-left origin, extras included, clipped to the canvas |
| `Screen.safeAreaInsets` | ro `EdgeInsets` | distances from the canvas edges to `safeArea` |
| `Screen.deviceSafeArea`, `Screen.deviceSafeAreaInsets` | ro | the same without the app's extra insets |
| `Screen.safeAreaPixels` | ro `Rect` | view pixels (`System.getScreenWidth/Height()`), top-left, extras included |
| `Screen.cutouts` | ro `Rect[]` | the cutouts in canvas units (Android 9+ and simulated devices; empty on iOS, like Unity) |
| `Screen.extraInsets` | ro `EdgeInsets` | the app's extra insets, canvas units |
| `Screen.setExtraInsets(left, top, right, bottom [, unit])` | fn | `unit` `"canvas"` (default) or `"pixels"` |
| `Screen.clearExtraInsets()` | fn | |
| `Engine.onSafeAreaChanged` | event `()` | once per change, after `onCanvasChanged` and `onOrientationChanged` |

`EdgeInsets(left, top, right, bottom)` has `left top right bottom` (rw), `isZero()`, `==` and
`tostring`. The values are copies: changing a returned `Rect` changes nothing.

Unity's `Screen.safeArea` is in pixels with a bottom-left origin; here it is in **canvas units with
a top-left origin**, the space of `Input.getTouchPosition`, UI layout and `Engine.canvasWidth/Height`.
`Screen.safeAreaPixels` is the pixel version (still top-left).

How the project's canvas scaling maps the insets:

- FITWIDTH / FITHEIGHT / NATIVE: scaled to the canvas (an inset of 141 pixels on a 1170-pixel-wide
  phone with a 390-wide canvas is 47 units).
- LETTERBOX: insets that fall inside the black bars are absorbed (a notch above a letterboxed
  canvas leaves the canvas safe).
- CROP: the cropped part of the canvas is outside the screen and so counts as unsafe.
- STRETCH: each axis scales on its own.

The insets never change the canvas scale (Unity: the scaler uses the whole screen).

## The Safe Area component (native UI)

Put it on a full-stretch panel under the canvas: backgrounds stay full-screen, everything anchored
inside the panel keeps clear of the notch. GameObject > UI > Safe Area creates one (a stretched
RectTransform with the component) under the selected UI object; Add Component > UI > Safe Area adds
it to an existing UI element. MCP: `create_ui {type: "safe_area", children: [...]}` or
`create_entity {type: "safe_area"}`.

| Field | Default | Meaning |
|---|---|---|
| `left`, `top`, `right`, `bottom` | on | the edges that follow the safe area; an edge off stays at the parent's edge (Unity's community SafeArea "Conform X/Y", per edge) |
| `includeExtraInsets` | on | `Screen.setExtraInsets` shrinks the panel too |
| `enabled` | on | off (or the GameObject inactive): the RectTransform keeps the anchors last applied |

Each UI update sets the RectTransform's four anchor points to the safe area relative to its parent
(its anchor reference), offsets and position offset 0. While the component is on the GameObject,
the RectTransform's size, anchors and offsets are driven: the Inspector shows them disabled with
"Driven by Safe Area", and `set_properties` / `set_rect_layout` refuse them ("driven by the Safe Area
component"). Remove the component to edit them again.

- Under a parent smaller than the safe area the anchors clamp to 0..1 (it fills the parent).
- Nested Safe Areas each apply against their own parent: inside a safe panel they get 0..1.
- A rotated or scaled parent gets the safe area as an axis-aligned rectangle.
- It needs a RectTransform (a UI element); without one the Inspector warns.

Lua: `RectTransform(self.scene, self.entity):getSafeAreaComponent()` returns the
`SafeAreaComponent` (`left`, `top`, `right`, `bottom`, `includeExtraInsets`, `enabled`, rw) or nil.

## Extra insets (a banner ad)

```lua
-- CanvasHelper.cs: the banner's height kept free at the bottom
Screen.setExtraInsets(0, 0, 0, bannerHeightPx, "pixels")
-- the ad closed
Screen.clearExtraInsets()
```

Extras add to the device insets per edge, everywhere: `Screen.safeArea`, the Safe Area component,
HTML `env()`. Pixel extras are kept in pixels and converted again on every canvas change; canvas
extras stay in canvas units. Negative values count as 0; extras larger than the safe area leave a
1x1 rectangle (a warning once). They persist across scene loads; the editor's Play stop clears them.

## Reacting to changes

```lua
function Hud:init()
    RegisterEngineEvent(self, "onSafeAreaChanged")
    self:place()
end

function Hud:onSafeAreaChanged()
    self:place()
end

function Hud:place()
    local safe = Screen.safeArea
    self.coins.position = Vector3(safe.x + safe.width - 120, safe.y + 16, 0)
end
```

`Screen.safeArea` is valid in `init()` (zero insets until the platform reports them; the event
follows). The event fires once per change of the value, at most once per frame, after
`onCanvasChanged` and `onOrientationChanged` of the same rotation; not while the app is paused
(one event after `onResume` if the value changed meanwhile).

## HTML/CSS UI

```css
.hud { padding-top: env(safe-area-inset-top, 0px); }
.bottom-bar { bottom: max(12px, env(safe-area-inset-bottom)); }
.panel { padding-bottom: calc(16px + env(safe-area-inset-bottom)); }
```

`env(safe-area-inset-top|right|bottom|left[, fallback])` (and Safari's `safe-area-max-inset-*`) is
the live inset, as in a mobile browser with `viewport-fit=cover`: it compiles to
`var(--jasper-safe-area-inset-<edge>, <fallback or 0px>)`, and the document system sets those
custom properties on every screen document from `Screen.safeAreaInsets` (extras included) in the
document's CSS px. A rotation or `Screen.setExtraInsets` lays the document out again without a
reload. `--jasper-device-safe-area-inset-*` give the insets without the extras. Inside `calc()`,
`min()`, `max()` and `clamp()` the variable is kept; where the compat layer folds a value at load
(a value a rewrite reads token by token, or decorator arguments) it is `0px` with an Info
diagnostic. Documents on meshes (texture mode) get 0. Inline `style` attributes work the same.

## Platforms

| Platform | Source | Notes |
|---|---|---|
| Android | GameActivity's display cutout insets (`APP_CMD_WINDOW_INSETS_CHANGED`, checked every frame), cutout rectangles from `DisplayCutout.getBoundingRects()` | Player > Android > Fullscreen (default): cutout only, the system bars come and go over the game (Unity's *Render outside safe area*). Windowed: cutout and system bars. API 21-27: no cutout API, zero |
| iOS | `UIView.safeAreaInsets` (`viewSafeAreaInsetsDidChange`) in drawable pixels | the home indicator's inset stays with Hide Home Indicator (iOS keeps it); no cutout rectangles |
| Web | a hidden probe padded by the page's `env(safe-area-inset-*)`, intersected with the canvas | export setting `web.viewport_fit_cover` (default on) adds `viewport-fit=cover` to the page's viewport meta (or adds the meta) |
| Linux, Windows, macOS | zero | `JASPERENGINE_SIMULATE_SAFE_AREA="top,right,bottom,left"` (integer pixels) at start gives fixed insets, to test a desktop build (`run_build` with the variable set); malformed values are ignored with a warning |
| Editor | the Game view's simulated device | none simulated: zero, as a desktop build |

## Simulating phones in the editor

The Game view's Device menu (left of the aspect) picks a phone or tablet: iPhone 15 Pro and Pro Max
(Dynamic Island), iPhone 13 / 14 and X (notch), iPhone SE, iPad Pro 11", Pixel 8 and Galaxy S23
(punch hole), Pixel 3 XL (notch), or **Custom...** (your own size, portrait insets and corner
radius). Rotate turns it (portrait, landscape left, landscape right, upside down where the device
has it); the toolbar's phone icon toggles the **Device Overlay** (black rounded corners and cutout,
the home indicator) and the square icon the **Safe Area** outline (green, dashed). With a device:

- the game image has the device's aspect; the simulated insets are scaled to the rendered size and
  fed to the engine, so `Screen.safeArea`, Safe Area panels and HTML `env()` behave as on the phone,
  in Play and in edit mode (the Scene view's UI follows the device too);
- in Play, `System.getDeviceModel()`, `Screen.dpi` and `Screen.orientation` are the device's
  (jasper://docs/platform-services);
- input is unaffected by the overlay; the Stats overlay shows `Safe area: x,y wxh` (canvas).

Nothing of the simulation reaches a build.

MCP:

```json
{"tool": "configure_game_view", "arguments": {"device": "iphone_15_pro", "orientation": "landscape_left"}}
{"tool": "configure_game_view", "arguments": {"custom_device": {"width": 1080, "height": 2400, "insets": {"top": 120, "bottom": 60}, "corner_radius": 80}}}
{"tool": "screenshot", "arguments": {"view": "game"}}
{"tool": "eval_lua", "arguments": {"code": "return Screen.safeArea"}}
```

- `configure_game_view` returns `devices` (the ids, `custom` last), `safe_area` (canvas) and
  `safe_area_pixels` (render pixels) as the game sees them now; `device_overlay` and
  `safe_area_outline` toggle the drawing. It works while playing (rotating the device).
- `screenshot` (game view) draws the device over the image (`device_overlay: false` to leave it
  out) and returns `safe_area_in_image`: the safe rectangle in image pixels (in Play
  `Screen.safeAreaPixels`, extras included; stopped, the device's).
- `get_play_state` has `safe_area`: `rect` and `insets` (canvas, extras included), `extra`, `device`.

## Recipes

Port of a Unity `SafeArea` panel: add the Safe Area component to the "SafeArea" child of the
canvas (all edges on), keep the background image a sibling outside it, anchor buttons inside it.

Check a HUD on a notch, by agent:

1. `configure_game_view {device: "iphone_13", orientation: "portrait"}`
2. `create_ui {type: "safe_area", name: "Safe", children: [{type: "button", text: "Pause", layout: {anchor: "top_right", size: [120, 48], position: [-16, 16]}}]}`
3. `play`, `screenshot` (the button below the notch, inside `safe_area_in_image`)
4. `configure_game_view {orientation: "landscape_left"}`, `screenshot` again.

## Limits

Android rounded-corner insets (API 31), system-gesture and keyboard insets, foldable hinges and iOS
per-device cutout rectangles are not reported (Unity does not either). The simulator draws no
device frame around the image and does not change `Engine.platform`.
