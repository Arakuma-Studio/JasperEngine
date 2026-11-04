# Title Art effects and generators

Generated from the effect and generator registry (`engine/core/titleart/TitleEffectRegistry.h`) by
`titleEffectDocsMarkdown` (`engine/core/titleart/TitleEffectCatalog.cpp`); `TitleEffectCatalogTest.DocsPageIsUpToDate`
fails when it is stale (`JASPER_TEST_UPDATE_GOLDEN=1` rewrites it). Do not edit by hand.

Units: `px` are canvas pixels multiplied by the render's px_scale; `deg` are clockwise on screen from +x,
except light angles (Photoshop's, counter-clockwise: where the light comes from). An effect is added with
`edit_title {ops: [{op: "add_effect", layer, type, params?}]}` and its params are the field paths
`fx:<type>.<param>` (`fx:outer_glow.size`), the names exposed parameters and Lua use. A generator is a layer:
`{op: "add_layer", type: "generator", generator: "<type>"}`, its params `gen.<param>`.

Classes: cost C0 (trivial) to C4 (bake only); 8-bit F0 to F2 (how closely the RGBA8 fallback agrees);
glyph atlas safe, approximate or unsafe (S81's per-glyph rendering); animation `rerender` (smooth) or `step`;
`light` marks a colour of light (colour schemes map it onto their light ramp).

## Effects

### Glow

#### Outer Glow (`outer_glow`)

A glow around the layer, behind it like Photoshop's or added over it as light; one colour, a gradient across the layer, a gradient out from the edge, or the layer's own colours.

Cost C1 · 8-bit F1 · glyph atlas approximate · live none · reach at defaults 40 px · adds light at defaults no

Also found as: glow, halo, outerGlow, 光彩 (外側), 後光.

| Param | Type | Default | Range | Unit | Animation | Shown when | Description |
|---|---|---|---|---|---|---|---|
| `fill` Fill | enum | solid | `solid`, `gradient`, `layer`, `distance` |  | step |  | What colours the glow: one colour, a gradient across the layer, the layer's own colours or a gradient out from the edge |
| `color` Color | color | #ffb000 |  |  | rerender, light | `fill` is `solid` | The glow's colour |
| `gradient` Gradient | gradient | #ff4ad8@0 #30e0ff@1 |  |  | rerender, light | `fill` is `gradient` or `distance` | The glow's colours across the layer or out from its edge |
| `angle` Angle | number | 90 | -180 .. 360 | deg | rerender | `fill` is `gradient` | Direction of the gradient across the layer, clockwise (90 = top to bottom) |
| `technique` Technique | enum | softer | `softer`, `precise` |  | step |  | Softer blurs the silhouette (round halo); Precise follows the exact distance from the edge |
| `size` Size | number | 40 | 1 .. 400 | px | rerender |  | How far the glow reaches past the edge |
| `intensity` Intensity | number | 1.6 | 0 .. 6 | x | rerender |  | How bright the glow is near the edge |
| `spread` Spread | number | 0.15 | 0 .. 1 |  | rerender |  | How much of the glow stays solid before it fades |
| `opacity` Opacity | number | 1 | 0 .. 1 |  | rerender |  | The glow's opacity |
| `mode` Mode | enum | behind | `behind`, `add` |  | step |  | Behind the layer like Photoshop, or added over it as light |
| `on_face` On Face | number | 0.15 | 0 .. 1 |  | rerender | `mode` is `add` | How much added light falls over the layer itself |

#### Inner Glow (`inner_glow`)

A glow spreading inward from the layer's edges (or outward from its centre), in one colour or a gradient in from the edge.

Cost C1 · 8-bit F1 · glyph atlas approximate · live none · reach at defaults 0 px · adds light at defaults no

Also found as: innerGlow, inner light, 光彩 (内側).

| Param | Type | Default | Range | Unit | Animation | Shown when | Description |
|---|---|---|---|---|---|---|---|
| `fill` Fill | enum | solid | `solid`, `distance` |  | step |  | One colour, or a gradient that runs in from the edge |
| `color` Color | color | #fff4a0 |  |  | rerender, light | `fill` is `solid` | The glow's colour |
| `gradient` Gradient | gradient | #ffffff@0 #fff36b@0.22 #ff8a00@0.55 #e01400@1 |  |  | rerender, light | `fill` is `distance` | The glow's colours from the edge inward |
| `technique` Technique | enum | softer | `softer`, `precise` |  | step |  | Softer blurs the silhouette (round halo); Precise follows the exact distance from the edge |
| `size` Size | number | 18 | 1 .. 200 | px | rerender |  | How far the glow reaches into the layer |
| `intensity` Intensity | number | 1 | 0 .. 4 | x | rerender |  | How bright the glow is |
| `choke` Choke | number | 0 | 0 .. 1 |  | rerender |  | How much of the glow stays solid before it fades |
| `opacity` Opacity | number | 1 | 0 .. 1 |  | rerender |  | The glow's opacity |
| `mode` Mode | enum | screen | `normal`, `screen`, `add` |  | step |  | How the glow combines with the layer's colour |
| `source` Source | enum | edge | `edge`, `center` |  | step |  | Glow in from the edges, or out from the centre |

### Shadow & Depth

#### Drop Shadow (`drop_shadow`)

An offset, blurred shadow behind the layer; with Use Global Light it falls away from the document's light.

Cost C1 · 8-bit F1 · glyph atlas approximate · live none · reach at defaults 24 px · adds light at defaults no · follows the global light (`angle`)

Also found as: dropShadow, shadow, ドロップシャドウ, 影.

| Param | Type | Default | Range | Unit | Animation | Shown when | Description |
|---|---|---|---|---|---|---|---|
| `angle` Angle | number | 60 | -180 .. 360 | deg | rerender |  | The direction the shadow falls, clockwise (90 = straight down) |
| `use_global_light` Use Global Light | bool | true |  |  | step |  | Take the angle from the document's global light, so one change relights every effect |
| `distance` Distance | number | 10 | 0 .. 500 | px | rerender |  | How far the shadow is offset |
| `size` Size | number | 14 | 0 .. 300 | px | rerender |  | How soft the shadow is (its blur) |
| `spread` Spread | number | 0.1 | 0 .. 0.95 |  | rerender |  | How much of the shadow stays solid before it fades |
| `color` Color | color | #000000 |  |  | rerender |  | The shadow's colour |
| `opacity` Opacity | number | 0.75 | 0 .. 1 |  | rerender |  | The shadow's opacity |

#### Inner Shadow (`inner_shadow`)

Photoshop's inner shadow: the edge facing the light casts a soft shadow into the layer.

Cost C1 · 8-bit F1 · glyph atlas approximate · live none · reach at defaults 0 px · adds light at defaults no · follows the global light (`angle`)

Also found as: innerShadow, シャドウ (内側).

| Param | Type | Default | Range | Unit | Animation | Shown when | Description |
|---|---|---|---|---|---|---|---|
| `angle` Angle | number | 60 | -180 .. 360 | deg | rerender |  | The direction the shadow falls, clockwise (90 = downward) |
| `use_global_light` Use Global Light | bool | true |  |  | step |  | Take the angle from the document's global light, so one change relights every effect |
| `distance` Distance | number | 6 | 0 .. 300 | px | rerender |  | How far the shadow is offset |
| `size` Size | number | 8 | 0 .. 200 | px | rerender |  | How soft the shadow is (its blur) |
| `choke` Choke | number | 0 | 0 .. 0.95 |  | rerender |  | How much of the shadow stays solid before it fades |
| `color` Color | color | #000000 |  |  | rerender |  | The shadow's colour |
| `opacity` Opacity | number | 0.6 | 0 .. 1 |  | rerender |  | The shadow's opacity |

### Stroke & Edge

#### Outline (`outline`)

A line traced from the layer's alpha just outside its silhouette, behind it: the thin dark keyline that separates a title's edge from the background, or a stroke; several following the stack make stacked strokes. Light is never traced and stays over the line.

Cost C1 · 8-bit F1 · glyph atlas approximate · live none · reach at defaults 1.5 px · adds light at defaults no · traces ink

Also found as: keyline, stroke, border, 縁取り, フチ.

| Param | Type | Default | Range | Unit | Animation | Shown when | Description |
|---|---|---|---|---|---|---|---|
| `width` Width | number | 1.5 | 0.5 .. 100 | px | rerender |  | How wide the line is |
| `color` Color | color | #0a0604 |  |  | rerender |  | The line's colour |
| `opacity` Opacity | number | 1 | 0 .. 1 |  | rerender |  | The line's opacity |
| `softness` Softness | number | 0 | 0 .. 1 |  | rerender |  | How much of the line's outer part fades |
| `threshold` Threshold | number | 0.5 | 0.05 .. 0.95 |  | rerender |  | The alpha level traced as the edge |
| `follow` Follow | enum | layer | `layer`, `stack` |  | step |  | Trace the layer's own silhouette, or everything the effects above drew (light left out either way) |

### Color

#### Color Overlay (`color_overlay`)

Fills the layer with one colour in a blend mode, keeping its alpha (Photoshop's Color Overlay).

Cost C0 · 8-bit F0 · glyph atlas safe · live none · reach at defaults 0 px · adds light at defaults no

Also found as: colour overlay, tint, fill color, カラーオーバーレイ.

| Param | Type | Default | Range | Unit | Animation | Shown when | Description |
|---|---|---|---|---|---|---|---|
| `color` Color | color | #ff0000 |  |  | rerender |  | The overlay colour |
| `opacity` Opacity | number | 1 | 0 .. 1 |  | rerender |  | How much of the colour covers the layer |
| `mode` Blend Mode | enum | normal | `normal`, `add`, `screen`, `multiply`, `overlay`, `soft_light`, `hard_light`, `darken`, `lighten`, `color_dodge`, `color_burn`, `linear_burn`, `difference`, `exclusion`, `hue`, `saturation`, `color`, `luminosity` |  | step |  | How the colour combines with the layer's own |

#### Gradient Overlay (`gradient_overlay`)

Recolours the layer with a gradient, keeping its alpha: Linear across its bounds (90 = top to bottom), Radial from its centre, or Sweep, a band of fixed length moved across by Position (a glint); Light and Shade add or filter in linear light.

Cost C0 · 8-bit F0 · glyph atlas unsafe · live none · reach at defaults 0 px · adds light at defaults no

Also found as: gradientOverlay, gradient, shine, グラデーションオーバーレイ.

| Param | Type | Default | Range | Unit | Animation | Shown when | Description |
|---|---|---|---|---|---|---|---|
| `gradient` Gradient | gradient | #e8a020@0 #fff6b0@0.2 #f0a820@0.4 #ffc840@0.6 #c05a08@0.8 #ffe070@1 |  |  | rerender |  | The colours laid over the layer |
| `style` Style | enum | linear | `linear`, `radial`, `sweep` |  | step |  | Across the layer's box, out from its centre, or a band moved across it |
| `angle` Angle | number | 90 | -180 .. 360 | deg | rerender | `style` is `linear` or `sweep` | The gradient's direction, clockwise (90 = top to bottom) |
| `scale` Scale | number | 1 | 0.1 .. 10 | x | rerender | `style` is `linear` or `radial` | How much of the box the gradient spans |
| `offset` Offset | number | 0 | -2 .. 2 |  | rerender | `style` is `linear` or `radial` | Moves the gradient along its direction |
| `length` Length | number | 200 | 2 .. 4000 | px | rerender | `style` is `sweep` | The band's length |
| `position` Position | number | 0 | -100 .. 100 |  | rerender | `style` is `sweep` | Where the band is, percent of the way across: -100 just off one side, 100 just off the other |
| `wrap` Wrap | enum | clamp | `clamp`, `repeat`, `mirror` |  | step |  | What the gradient does past its ends |
| `amount` Amount | number | 1 | 0 .. 1 |  | rerender |  | How much of the gradient covers the layer |
| `mode` Blend Mode | enum | normal | `normal`, `add`, `screen`, `multiply`, `overlay`, `soft_light`, `hard_light`, `darken`, `lighten`, `color_dodge`, `color_burn`, `linear_burn`, `difference`, `exclusion`, `hue`, `saturation`, `color`, `luminosity`, `light`, `shade` |  | step |  | How the gradient combines with the layer's colour |

#### Adjust (`adjust`)

One adjustment (Levels, Curves, Hue/Saturation, Gradient Map...) on just this layer, group or part: the same operation as an adjustment layer.

Cost C0 · 8-bit F0 · glyph atlas safe · live none · reach at defaults 0 px · adds light at defaults no

Also found as: adjustment, levels, curves, hue saturation, color correction, 色調補正.

| Param | Type | Default | Range | Unit | Animation | Shown when | Description |
|---|---|---|---|---|---|---|---|
| `adjustment` Adjustment | adjustment | {"type":"hue_saturation","hue":0,"saturation":25,"lightness":0,"colorize":false} |  |  | rerender |  | The adjustment and its settings (the fourteen types of an adjustment layer) |

### Texture & Material

#### Pattern Overlay (`pattern_overlay`)

Lays a procedural pattern (zebra, leopard, checker, 和柄...) or a tiled image over the layer in a blend mode, keeping its alpha (Photoshop's Pattern Overlay).

Cost C0 · 8-bit F0 · glyph atlas safe · live none · reach at defaults 0 px · adds light at defaults no

Also found as: pattern, texture overlay, 柄, パターンオーバーレイ.

| Param | Type | Default | Range | Unit | Animation | Shown when | Description |
|---|---|---|---|---|---|---|---|
| `pattern` Pattern | enum | stripes | `zebra`, `tiger`, `giraffe`, `leopard`, `checker`, `hazard`, `stripes`, `dots`, `sakura`, `seigaiha`, `asanoha`, `shippo`, `kikko`, `uroko`, `custom` |  | step |  | The pattern: a procedural kind, or a custom image tiled once per motif |
| `color` Color | color | #000000 |  |  | rerender |  | The marks' colour |
| `color2` Color 2 | color | #ffffff |  |  | rerender |  | The secondary colour of the kinds that have one (checker, hazard, leopard, sakura) |
| `scale` Scale | number | 40 | 2 .. 2000 | px | rerender |  | The size of one motif |
| `angle` Angle | number | 0 | -180 .. 360 | deg | rerender |  | The pattern's rotation, clockwise |
| `seed` Seed | integer | 1 | 0 .. 999 |  | step |  | Reshuffles the random kinds (the document's seed adds to it) |
| `image` Image | asset | none |  |  | step | `pattern` is `custom` | The image a custom pattern tiles |
| `opacity` Opacity | number | 1 | 0 .. 1 |  | rerender |  | How much of the pattern covers the layer |
| `mode` Blend Mode | enum | normal | `normal`, `add`, `screen`, `multiply`, `overlay`, `soft_light`, `hard_light`, `darken`, `lighten`, `color_dodge`, `color_burn`, `linear_burn`, `difference`, `exclusion`, `hue`, `saturation`, `color`, `luminosity` |  | step |  | How the pattern combines with the layer's colour |
| `align` Align | enum | layer | `layer`, `canvas` |  | step |  | Anchor the pattern at the layer's position (it moves with the layer) or at the canvas centre |

### Blur

#### Gaussian Blur (`gaussian_blur`)

Photoshop's Gaussian Blur: a smooth, even blur of the layer (or of everything below an adjustment layer). Radius is the standard deviation; the scales stretch it along one axis.

Cost C1 · 8-bit F1 · glyph atlas approximate · live none · reach at defaults 24 px · adds light at defaults no

Also found as: gaussianBlur, blur, soften, ぼかし (ガウス).

| Param | Type | Default | Range | Unit | Animation | Shown when | Description |
|---|---|---|---|---|---|---|---|
| `radius` Radius | number | 8 | 0 .. 500 | px | rerender |  | The blur's standard deviation |
| `scale_x` Horizontal Scale | number | 100 | 0 .. 200 |  | rerender |  | The horizontal blur, percent of Radius |
| `scale_y` Vertical Scale | number | 100 | 0 .. 200 |  | rerender |  | The vertical blur, percent of Radius |
| `repeat_edges` Repeat Edge Pixels | bool | true |  |  | step |  | Keep content at the canvas edge from fading to transparent |

## Generators

### Fill & Pattern

#### Fill (`fill`)

A solid colour or a gradient covering the canvas: a background for mockups and full-frame looks.

Cost C0 · 8-bit F0 · glyph atlas unsafe · live none · follow none · fills the canvas

Also found as: background, solid, gradient fill, 塗りつぶし, 背景.

| Param | Type | Default | Range | Unit | Animation | Shown when | Description |
|---|---|---|---|---|---|---|---|
| `kind` Kind | enum | solid | `solid`, `linear`, `radial`, `angle`, `reflected`, `diamond`, `shape_burst` |  | step |  | A solid colour or the gradient's shape over the canvas |
| `color` Color | color | #202020 |  |  | rerender | `kind` is `solid` | The fill's colour |
| `gradient` Gradient | gradient | #000000@0 #ffffff@1 |  |  | rerender | `kind` is `linear` or `radial` or `angle` or `reflected` or `diamond` or `shape_burst` | The fill's colours |
| `angle` Angle | number | 90 | -180 .. 360 | deg | rerender | `kind` is `linear` or `angle` or `reflected` | The gradient's direction, clockwise (90 = top to bottom) |
| `scale` Scale | number | 1 | 0.1 .. 10 | x | rerender | `kind` is `linear` or `radial` or `angle` or `reflected` or `diamond` or `shape_burst` | How much of the canvas the gradient spans |
| `offset` Offset | number | 0 | -1 .. 1 |  | rerender | `kind` is `linear` or `radial` or `angle` or `reflected` or `diamond` or `shape_burst` | Moves the gradient along itself |

#### Pattern Fill (`pattern_fill`)

A full-frame 柄: zebra, giraffe, leopard, tiger, checker, hazard and 和柄 over a background colour, optionally on a floor plane receding to a horizon; fade it with a gradient mask.

Cost C0 · 8-bit F0 · glyph atlas unsafe · live none · follow none · fills the canvas

Also found as: patternFill, pattern, animal print, 和柄, 柄.

| Param | Type | Default | Range | Unit | Animation | Shown when | Description |
|---|---|---|---|---|---|---|---|
| `kind` Pattern | enum | zebra | `zebra`, `tiger`, `giraffe`, `leopard`, `checker`, `hazard`, `stripes`, `dots`, `sakura`, `seigaiha`, `asanoha`, `shippo`, `kikko`, `uroko` |  | step |  | The procedural pattern |
| `color` Color | color | #121212 |  |  | rerender |  | The marks' colour |
| `color2` Color 2 | color | #ffffff |  |  | rerender |  | The secondary colour of the kinds that have one |
| `background` Background | color | #f2efe8 |  |  | rerender |  | The colour between the marks |
| `bg_opacity` Background Opacity | number | 100 | 0 .. 100 |  | rerender |  | The background's opacity, percent |
| `scale` Motif Size | number | 140 | 8 .. 2000 | px | rerender |  | The size of one motif |
| `rotation` Rotation | number | 0 | -180 .. 180 | deg | rerender |  | The pattern's rotation, clockwise |
| `tilt` Perspective Tilt | number | 0 | 0 .. 80 | deg | rerender |  | Lays the pattern on a floor tilted back by this much |
| `seed` Seed | integer | 1 | 0 .. 999 |  | step |  | Reshuffles the random kinds (the document's seed adds to it) |
