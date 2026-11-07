# Native UI (`jasper://docs/ui`)

The component UI (Image, Text, Button, Panel, Scrollbar, Progressbar, InputField, Layout Group:
Unity's uGUI) built with `create_ui` or GameObject > UI. Layout rules: the `create_ui` and
`set_rect_layout` tool descriptions. Here: Canvas Group, UI scaling (Canvas Scaler), text outline and
shadow, text effects, particles in the UI. The HTML/CSS UI is a separate system (`jasper://docs/html-ui`)
that shares the Canvas Scaler and has `text-effect` and `<particle-system>`.

## Canvas Group

Unity's CanvasGroup (component key `canvasgroup`, "Canvas Group"): it affects the GameObject it
is on and every descendant.

| Field | Effect |
|---|---|
| `alpha` (0-1) | multiplied into every UI element's draw alpha; nested groups multiply. The elements' own colours are unchanged (`Image.color` reads back as set) |
| `interactable` | false: Buttons (no press, no click, the disabled look), Input Fields (no focus; a focused one loses it), Scrollbars (no drag) and Panels (no move, resize or bring-to-front) stop reacting. Plain pointer callbacks (`ui.onClick` of an Image) still fire, as Unity's EventTrigger |
| `blocksRaycasts` | false: pointers go through the elements below to whatever is behind |
| `ignoreParentGroups` | the groups above stop counting |
| `renderMode` | `PER_ELEMENT` (Unity) or `FLATTEN` (CSS opacity), below |

**The chain rule.** An element's chain is the enabled groups from the element itself up to the
root, stopping after (and including) the first one with `ignoreParentGroups`. Alpha is the
product over the chain, interactable and blocksRaycasts the AND. A disabled group (or one on an
inactive GameObject) does not count. Unlike Unity, a child with `ignoreParentGroups` under a
parent at alpha 0 still shows at its own alpha.

**Hiding a panel like Unity.** Alpha 0 still blocks clicks (Unity, CSS `opacity: 0`): set
`alpha = 0, interactable = false, blocksRaycasts = false` (the game's DelayedUI does it in
`onEnable`). Effective interactability: `rt:isInteractable()` (groups and the widget's own
`disabled`); `rt:getInheritedAlpha()`, `rt:getGroupsAllowInteraction()`,
`rt:getGroupsBlockRaycasts()` walk the chain on demand.

**Drag and drop (Garden inventory).** In `onDragStart` set `group.alpha = 0.6` and
`group.blocksRaycasts = false`: the dragged item keeps receiving `onDrag`/`onDragEnd` (a captured
press continues), and the element under the pointer at the release is the slot behind it.
Restore both on drop.

**Fades.** `CanvasGroup(self.scene, self.entity)` wraps the GameObject's group (adding one when
missing); `self.object:getCanvasGroup()` is nil without one.
```lua
local group = CanvasGroup(self.scene, self.entity)
group:fadeOut(0.25, {deactivate = true})              -- LeanTween.alphaCanvas(cg, 0, .25) + SetActive(false)
group:fadeTo(1, 0.3, {ease = EaseType.QUAD_OUT, unscaled = true, onComplete = function() print("shown") end})
group:stopFade(true)                                  -- jump to the end (completes it)
```
A fade is an AlphaTween on the group (`alphaTarget = CANVAS_GROUP`) from the current alpha; a new
fade stops the group's previous one. On completion the final alpha is written, then `onComplete`
runs (main thread), then the GameObject is deactivated when asked. `unscaled` keeps it running
at time scale 0 (pause menus). The tween goes with the group and with the scene. A plain
`AlphaTween` whose target has an enabled Canvas Group fades the group (`alphaTarget = AUTO`;
`COLOR` forces the colour alpha).

**Animation clips.** Property tracks `component: "canvasgroup"`, `property: "alpha"` (Unity
`m_Alpha`), and the bools `interactable`, `blocksRaycasts`, `ignoreParentGroups`, `enabled`
(jasper://docs/animation, Property tracks).

**Render modes.** `PER_ELEMENT` multiplies each element's alpha at no extra draw cost: overlapping
children show through each other (darker overlaps). `FLATTEN` draws the subtree into a layer and
composites it once at the group's alpha, so overlaps look like the front element (CSS `opacity`,
Godot CanvasGroup). Its cost, only while its alpha is strictly between 0 and 1 and the subtree has
two or more UI elements: per camera that draws it, one offscreen pass before the camera's own (a
target the size of the screen, cleared; flattened siblings elsewhere on screen share one) and one
textured quad over the subtree's bounds; the colour pass is not broken. Keep at most two flattened
groups fading at once on mobile. Trade-offs: additive and multiply children blend
against the layer, not the background; `ignoreParentGroups` children inside a flattened group are
still composited at its alpha; UI particles and sprites in the subtree are faded per element and drawn
over the layer; a stencil mask above a flattened group does not clip inside it (put the mask inside
the group, or use a Panel's scissor); at alpha 0 the whole subtree is skipped (masks included); more
than 4 nested layers fall back to Per Element.

**What it does not affect.** HTML UI documents (use CSS `opacity` / `pointer-events`), sprites,
meshes and line renderers (as in Unity, only UI elements). UI particles (Points under a
RectTransform) are faded. A custom UI shader receives the factor in `uiParams.color`; one that
ignores it does not fade, but still hides at alpha 0.

**MCP.** `create_ui` nodes take `canvas_group: true | {alpha, interactable, blocks_raycasts,
ignore_parent_groups, render_mode}`; `add_component canvasgroup` + `set_properties` /
`set_runtime_property canvasgroup.alpha`; `get_entity` and `get_runtime_entity` of an element
under a group return `canvas_group_state {inherited_alpha, interactable, blocks_raycasts,
group}`. Check the look with `screenshot` pixels and clicks with `send_input` + `eval_lua`.

**Behaviour changes in scenes without Canvas Groups** (intended, Unity-like): an InputField whose own
`disabled` is set no longer takes the focus on a press (and typing does not reach it); a Button that
is pressed when it becomes disabled is released (`onRelease`, no click) instead of staying pressed;
new Buttons' `colorDisabled` defaults to Unity's grey (sRGB 200, 200, 200, 128): saved scenes keep
the colour they stored, buttons made from Lua or MCP get the grey (set `colorDisabled` for another
look).

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

## The Game view (configure_game_view)

`configure_game_view` (group editor_ux) with no arguments reads the Game view toolbar; its
arguments change it. They are editor preferences, not undoable, and not saved with the project:
- `aspect` (`project` = the canvas size, a project setting) and `scale` (1-5); the game renders
  at `render_size`. `resolution`: `"1920x1080"`, `"WxH@dpi"`, a name from the resolutions menu,
  `free` (back to the aspect) or `{width, height, dpi, name}` (a name saves it); the game and
  screenshots render at it and its dpi is `Screen.dpi`.
- `device` (an id of the devices list, `custom` or `none`), `orientation`, `language` (`es-MX`...,
  or `system`), `device_overlay`, `safe_area_outline`, `custom_device` `{width, height, insets
  {top, right, bottom, left}, corner_radius}` in pixels with `dpi`: a simulated phone and system
  language, editor only; `safe_area` in the reply is `Screen.safeArea` (jasper://docs/safe-area).
- `simulate_touch`: the mouse is one finger (not while playing); `simulate_textures`: every view
  shows the cooked textures of `desktop`, `mobile`, `android_etc2` or `web`.
- `atmosphere_quality`: the preview's Atmosphere Quality (`project` = the project's desktop tier;
  jasper://docs/atmosphere); `physics_debug` (collider and joint wireframes in Play) and
  `physics_debug_contacts` (jasper://docs/physics); `network: "offline"` makes Http to any host
  but localhost fail with `offline`, also while playing (jasper://docs/networking);
  `maximize_on_play`, `show_stats`.

## Input Field text in any language

An Input Field takes any character, emoji and other characters beyond U+FFFF included, on every
platform: Windows and Android type those as two UTF-16 halves, which the engine joins before the
field sees them (one character, one backspace to erase). `maxTextSize` counts characters. Whether
a character shows is up to the Text's font: the default font has Latin and Arabic; a game that
shows Chinese, Japanese, Korean or emoji gives the Text a font with those glyphs. Check typing as
Windows delivers it with `send_input {type:"text", text:"a😀", per_unit:true}` (`jasper://docs/input`).

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
| FadeIn | duration, spread (0 one after the other, 1 all at once) | one-shot |
| Grow, Unfold | duration, spread, amplitude (TMP's Amplitude: 1, the default, is the whole change; above overshoots) | one-shot (Grow from the bottom edge, Unfold from the middle line, as TMP Effect) |
| Rainbow | duration, phaseStep, saturation, lightness, mix | looping colour |
| Gradient | duration, spread, gradient stops (sRGB), mix (multiply, add, replace) | looping colour |

Component fields: `autoPlay` (starts when shown), `loop` (one-shot layers start again once they all
finished; `onComplete` fires at each end), `useUnscaledTime` (default true: runs at
`Engine.timeScale = 0`, pause menus), `startIndex` / `length` (characters, -1 to the end) or
`forWords` (whole words, space separated). One-shot timing is TMP Effect's: the stagger counts the
characters with their spaces. `UI.reducedMotion = true` skips the motion types and keeps colour and
reveal ones (an effect of only motion one-shots ends at once). Changing a layer's type moves an
amplitude left at the old type's default to the new one's (0.15 em, 1 for Grow and Unfold). The
Inspector's Preview plays it in edit mode.

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
spread = ratio, amplitude A); Sketch is Shake (amplitude `0.1 x A`); Repeat is `loop`, AutoPlay `autoPlay`, ForWords
`forWords`. TMP Effect runs on unscaled time, as the default here.

## Particles in the UI

A Particle System under a UI element of a UI scene draws in Hierarchy order (between its siblings),
in UI units (scaling with the canvas), clipped by the nearest Panel or scissor above it, never hit
by pointer events, hidden with a hidden or inactive ancestor. Create one with GameObject > UI >
Particle System (one GameObject: unscaled time, local space, the soft round image, a sparkle in UI
units) or `create_particles {parent: "<UI element>"}` in a UI scene (presets in UI units, turned y
down, local space, unscaled time, the soft image; its reply says `ui: true`). `useUnscaledTime`
(Inspector "Unscaled Time", Lua `ps.useUnscaledTime`; the effect's own Unscaled Time does the same)
plays it while the game is paused (`Engine.timeScale = 0`). A Canvas Group's alpha does not fade a
Particle System (it fades points). Unity's Coffee UIParticle is this: a Particle System under the
element, no extra component.
