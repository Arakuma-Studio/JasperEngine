# Native UI: scaling, text outline and shadow, text effects, particles

The component UI of UI scenes (RectTransform, Image, Text, Button, Panel, ...), as Unity's uGUI
canvas: what this guide covers. HTML/CSS UI documents have their own guide (`jasper://docs/html-ui`:
the same Canvas Scaler, `text-effect` and `<particle-system>`).

## UI scaling (Canvas Scaler)

Two settings, as Unity separates camera framing from the CanvasScaler:

- **Scaling mode** (Project Settings > Graphics, `graphics.scaling_mode`, Lua `Engine.scalingMode`):
  the **game canvas**: 2D cameras, input coordinates (`Input.getMousePosition`, touches), the world.
- **UI Scale** (Project Settings > Graphics > UI Scale, `graphics.ui_scale`): the **Canvas Scaler**,
  the UI's own space: the native UI of UI scenes (their camera is a UI camera) and the screen HTML
  UI documents of every scene. A scene can override it (Inspector of the scene > Canvas Scaler,
  "Use Project Default" off; MCP scene setting `canvas_scaler`; Lua `scene:getCanvasScaler()`).

Modes (Unity's formulas; W, H = the game's view in pixels, Rw, Rh = the reference resolution):

| Mode (`mode`) | UI units | Use |
|---|---|---|
| Follow Game Canvas (`follow_game_canvas`, default) | the game canvas, as before the Canvas Scaler existed | old projects; UI in canvas pixels |
| Constant Pixel Size (`constant_pixel_size`) | one unit = `scale_factor` screen pixels | desktop tools |
| Scale With Screen Size (`scale_with_screen_size`) | designed at `reference` [w, h], scaled to the screen | **mobile games** |
| Constant Physical Size (`constant_physical_size`) | a physical length (`physical_unit`, points by default) at `Screen.dpi` (`fallback_dpi` when unknown) | touch targets of a real size |

Scale With Screen Size has three `match_mode`s: `match_width_or_height` (`match` 0 = by the width,
1 = by the height, between them a log-space blend: `s = 2^lerp(log2(W/Rw), log2(H/Rh), match)`),
`expand` (`s = min(W/Rw, H/Rh)`: the UI is never smaller than the reference in either axis, a
wider or taller screen adds room) and `shrink` (`s = max(...)`: never larger, part may be cut).
The UI size is `W / s` x `H / s` (fractional sizes are fine). Unity's `CanvasScaleAdjuster`
pattern (match 0 on 4:3, 1 on 20:9) is exactly `expand`.

```json
{"tool": "set_project_settings", "values": {"graphics.ui_scale": {"mode": "scale_with_screen_size", "reference": [1920, 1080], "match_mode": "expand"}}}
{"tool": "set_scene_settings", "settings": {"canvas_scaler": {"mode": "constant_pixel_size", "scale_factor": 2}}}
```

Setting fields on a scene that used the project default makes it override, starting from that
default (`use_project_default: true` goes back). Lua does the same:

```lua
local scaler = scene:getCanvasScaler()
scaler.uiScaleMode = UIScaleMode.ScaleWithScreenSize
scaler.referenceResolution = Vector2(1920, 1080)
scaler.screenMatchMode = ScreenMatchMode.Expand
-- next frame: scaler.canvasSize is the UI size (1920 x 1440 on a 4:3 screen), scaler.appliedScaleFactor the scale
```

Rules: RectTransform sizes, positions and anchors are UI units; pointer events reach the UI in UI
units (`UI.canvasToUI(scene, point)` / `UI.uiToCanvas(scene, point)` convert game canvas pixels,
e.g. `Input.getMousePosition()`, to and from a scene's UI units); `Engine.onViewChanged` fires
after the new scale is computed (rotation, resize); safe-area insets do not change the scale
(Unity); the scale comes from the game's view rect (letterbox bars excluded). Text is rasterized
at the scale (crisp when the UI is scaled up). Under Follow Game Canvas nothing changes.

**Preview**: the Game view's resolution menu has fixed resolutions (1920x1080, 2400x1080 at 400 dpi,
2732x2048 at 264 dpi and their portrait sizes, "Add custom..." with a dpi); the game renders at that
size, fitted to the view, and `Screen.dpi` is the preset's. Agents: `configure_game_view
{resolution: "2400x1080"}` (or `"WxH@dpi"`, `{width, height, dpi, name}`; `"free"` back to the aspect),
then `screenshot`. The Scene view lays a UI scene out at the UI size of the Game view's resolution.

## Text outline and shadow

`Text` (TMP's outline and underlay): `outlineWidth` (UI units outside the glyph edge), `outlineColor`,
`shadowEnabled`, `shadowColor`, `shadowOffset` (UI units, right and down: (2, 2)), `shadowSoftness`
(blur radius), `shadowDilate` (the shadow grows). Either one draws the text from a signed-distance
atlas (one draw call, the face, outline and shadow in one shader pass); plain texts keep their bitmap
atlas. Inspector: Text > Outline and Shadow foldouts. Lua `text.outlineWidth = 3`, colours sRGB as
`text.color`; MCP `create_ui` text props `outline: {width, color}` and `shadow: {offset, color,
softness, dilate}` (colours linear like every MCP colour), or `set_properties` on TextComponent.
`visibleCharacters` (TMP `maxVisibleCharacters`, -1 all) draws only the first N characters, the
rest keeping their place.

## Text effects

The **Text Effect** component (Add Component > UI > Text Effect; MCP `add_component TextEffect`)
animates the Text on its GameObject per character: up to 4 layers applied in order (positions add
up, colours compose). Types and their fields:

| Type | Fields | |
|---|---|---|
| Wave | amplitude (em), duration (period, s), phaseStep (rad per character) | looping |
| Bounce | amplitude (em), duration, spread | looping hop |
| Shake | amplitude (em), frequency (Hz) | looping jitter |
| Pulse | amplitude (scale change), duration, spread | looping |
| Typewriter | charactersPerSecond (0: the whole text over duration), fadeTime | one-shot, `onCharacterRevealed(index)` |
| FadeIn, Grow, Unfold | duration, spread (0 one after the other, 1 all at once) | one-shot (Grow from the bottom edge, Unfold from the middle line, as TMP Effect) |
| Rainbow | duration, phaseStep, saturation, lightness, mix | looping colour |
| Gradient | duration, spread, gradient stops (sRGB), mix (multiply, add, replace) | looping colour |

Component fields: `autoPlay` (starts when shown), `loop` (one-shot layers start again once they all
finished; `onComplete` fires at each end), `useUnscaledTime` (default true: runs at
`Engine.timeScale = 0`, pause menus), `startIndex` / `length` (characters, -1 to the end) or
`forWords` (whole words, space separated). `UI.reducedMotion = true` skips the motion types and keeps
colour and reveal ones. The Inspector's Preview plays it in edit mode.

```lua
local fx = TextEffect(scene, title.entity)        -- adds the component when missing
fx:setLayer(1, {type = TextEffectType.Wave, amplitude = 0.06, duration = 1.5})
fx:setLayer(2, {type = TextEffectType.Gradient, mix = TextEffectMix.Add,
    gradient = {{color = Vector4(1, 0.8, 0.2, 1), position = 0}, {color = Vector4(1, 1, 1, 1), position = 1}}})
fx:getLayer(1).amplitude = 0.1                     -- a layer's fields change the running effect
local typing = TextEffect(scene, dialog.entity)
typing:setLayer(1, {type = TextEffectType.Typewriter, charactersPerSecond = 40})
typing.onCharacterRevealed = function(index) clickSound:play() end
typing.onComplete = function() showContinueArrow() end
typing:restart()                                   -- also play(), stop() (plain text), finish(), isFinished()
```

TextMesh Pro Effect: Waves with `Amplitude A`, `DurationInSeconds T`, `CharacterDurationRatio r` and a
gradient with Mix Add is a Wave layer (amplitude `0.3 x A` em, duration T, phaseStep 0.3) and a
Gradient layer (duration T, spread r, mix add, its stops); Grow and Unfold map one to one (duration,
spread = ratio); Sketch is Shake (amplitude `0.1 x A`); Repeat is `loop`, AutoPlay `autoPlay`, ForWords
`forWords`. TMP Effect runs on unscaled time, as the default here.

## Particles in the UI

A Particle System under a UI element of a UI scene draws in Hierarchy order (between its siblings),
in UI units (scaling with the canvas), clipped by the nearest Panel or scissor above it, never hit
by pointer events, hidden with a hidden or inactive ancestor. Create one with GameObject > UI >
Particle System (a Points emitter with a soft round image and its Particle System child: unscaled
time, local space, a sparkle in UI units) or `create_particles {parent: "<UI element>"}` in a UI
scene (presets in UI units, y down, unscaled time). `useUnscaledTime` (Inspector "Unscaled Time",
Lua `ps.useUnscaledTime`) plays it while the game is paused (`Engine.timeScale = 0`). Unity's Coffee
UIParticle is this: a Particle System under the element, no extra component.
