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

#### Fractal Noise (`fractal_noise`)

Clouds, fog, plasma, caustics, static or a nebula: octaves of value noise (or cells) through a gradient, with a looping Evolution and a scrolling Offset to animate.

Cost C1 · 8-bit F0 · glyph atlas unsafe · live kernel · follow none · fills the canvas

Also found as: fractal noise, noise, clouds, fog, plasma, turbulence, フラクタルノイズ, 雲.

| Param | Type | Default | Range | Unit | Animation | Shown when | Description |
|---|---|---|---|---|---|---|---|
| `noise` Noise | enum | fbm | `fbm`, `turbulent`, `ridged`, `cells` |  | step |  | Soft billows, turbulent creases, sharp ridges or Worley cell edges |
| `scale` Scale | number | 240 | 4 .. 4000 | px | rerender |  | The size of the coarsest octave's features |
| `aspect` Aspect | number | 1 | 0.1 .. 10 | x | rerender |  | Stretches the noise horizontally |
| `complexity` Complexity | integer | 5 | 1 .. 8 |  | step |  | How many octaves add finer detail (the mobile tier draws at most 4) |
| `contrast` Contrast | number | 1 | 0 .. 4 | x | rerender |  | Spreads the values about the middle |
| `brightness` Brightness | number | 0 | -1 .. 1 |  | rerender |  | Shifts every value |
| `evolution` Evolution | number | 0 | 0 .. 100 |  | live |  | Moves the noise through its next shapes (keyed, it loops smoothly) |
| `offset` Offset | vec2 | vec2 |  | px | live |  | Scrolls the noise, canvas px |
| `rotation` Rotation | number | 0 | -180 .. 180 | deg | rerender |  | Turns the noise, clockwise |
| `gradient` Gradient | gradient | #000000@0 #ffffff@1 |  |  | rerender, light |  | Colours the values from low to high |
| `seed` Seed | integer | 1 | 0 .. 999 |  | step |  | Reshuffles the noise |

### Particles

#### Particles (`particles`)

Particle field: 金粉 gold dust, 火の粉 embers, light motes, confetti, sakura, star showers, bokeh, rain and fireworks. Emits from a point, ellipse, rectangle, ring, line or the whole frame (centred on the followed layer when following one), scattered, bursting out or streaming; following a layer it can also burst from that layer's edge or fill, coloured from it, in front of it or behind it. Depth makes near particles bigger, brighter and defocused. Time, gravity, spin and twinkle animate it live.

Cost C1 · 8-bit F2 · glyph atlas unsafe · live kernel · follow optional · fills the canvas

Also found as: particles, particle field, パーティクル, 金粉, gold dust, glitter, 火の粉, embers, 光の粒, light motes, sparkles, 紙吹雪, confetti, 桜吹雪, sakura, bokeh, ボケ, rain, 雨, 花火, fireworks.

| Param | Type | Default | Range | Unit | Animation | Shown when | Description |
|---|---|---|---|---|---|---|---|
| `emitter` Emitter | enum | ellipse | `ellipse`, `rect`, `ring`, `line`, `point`, `full`, `layer` |  | relayout |  | Where particles are born (Followed Layer: the followed layer's own pixels) |
| `source` Emit From | enum | fill | `fill`, `edge` |  | relayout | `emitter` is `layer` | The followed layer's whole fill or only its edge band |
| `edge_width` Edge Width | number | 4 | 1 .. 100 | px | relayout | `source` is `edge` | How deep the edge band particles are born in reaches |
| `direction` Direction | enum | normal | `normal`, `point`, `angle` |  | step | `emitter` is `layer` | Which way particles burst from the followed layer |
| `point` Point | vec2 | vec2 |  | px | rerender | `direction` is `point` | The point particles burst away from, canvas px (+y up) |
| `heading` Heading | number | -90 | -180 .. 180 | deg | rerender | `direction` is `angle` | The direction particles burst in, clockwise (-90 = up) |
| `radius` Radius | vec2 | vec2 |  | px | relayout | `emitter` is `ellipse` or `rect` or `ring` or `line` | The emitter's half width and half height, px |
| `distribution` Distribution | enum | burst | `scatter`, `burst`, `stream` |  | relayout | `emitter` is `ellipse` or `rect` or `ring` or `line` or `point` or `full` | Scattered over the emitter, bursting out of it or streaming in one direction |
| `falloff` Density Falloff | number | 60 | 0 .. 100 |  | relayout | `emitter` is `ellipse` or `rect` or `ring` or `line` or `point` or `full` | How strongly particles crowd the emitter's centre and the start of their travel, percent |
| `travel` Distance | number | 200 | 0 .. 4000 | px | relayout |  | How far particles travel from where they are born |
| `distance_random` Distance Random | number | 100 | 0 .. 100 |  | rerender | `emitter` is `layer` | How much each particle's travel varies, percent |
| `angle` Angle | number | 90 | -180 .. 180 | deg | rerender | `distribution` is `scatter` or `stream` | The stream's and streaks' direction, clockwise (90 = down) |
| `cone` Spread Angle | number | 20 | 0 .. 360 | deg | rerender |  | How far each particle's direction varies |
| `split` Particles | enum | all | `all`, `front`, `back` |  | step | `emitter` is `layer` | Only the half drawn in front of or behind the followed layer, for a pair of layers |
| `shape` Shape | enum | dot | `dot`, `glint4`, `glint6`, `ember`, `square`, `confetti`, `petal`, `star`, `heart`, `bokeh`, `rain` |  | step |  | Each particle's shape |
| `glint_mix` Glint Mix | number | 0 | 0 .. 100 |  | rerender | `shape` is `dot` or `square` | The share of dots or squares drawn as big 4-point glints, percent |
| `count` Count | integer | 400 | 1 .. 5000 |  | relayout |  | How many particles (the mobile tier draws the first 2,000) |
| `size` Size | number | 6 | 0.5 .. 200 | px | rerender |  | A particle's diameter |
| `size_random` Size Random | number | 60 | 0 .. 100 |  | rerender |  | How much smaller particles may be, percent |
| `depth` Depth | number | 50 | 0 .. 100 |  | rerender |  | Fakes depth: near particles bigger, brighter and defocused, percent |
| `streak` Streak Length | number | 40 | 0 .. 400 | px | rerender | `shape` is `ember` or `rain` | The length of ember and rain streaks |
| `ring` Bokeh Ring | number | 30 | 0 .. 100 |  | rerender | `shape` is `bokeh` | A bright rim on the bokeh discs, percent |
| `rotation_random` Rotation Random | number | 20 | 0 .. 180 | deg | rerender | `shape` is `glint4` or `glint6` or `square` or `confetti` or `petal` or `star` or `heart` | How far each particle turns either way |
| `tumble` Tumble | number | 0 | 0 .. 100 |  | rerender | `shape` is `square` or `confetti` or `petal` or `star` or `heart` | Particles turning in 3D: foreshortened, the back face darker, percent |
| `sheen` Foil Sheen | number | 50 | 0 .. 100 |  | rerender | `shape` is `confetti` | The glare sliding along the confetti foil, percent |
| `color_mode` Color | enum | radial | `solid`, `radial`, `palette`, `sample` |  | step |  | How each particle gets its colour (Sample Layer: from the followed layer's face) |
| `color` Solid Color | color | #ffd166 |  |  | rerender, light | `color_mode` is `solid` | Every particle's colour |
| `gradient` Gradient | gradient | #fff2c2@0 #ffc845@0.35 #c77a12@1 |  |  | rerender, light | `color_mode` is `radial` or `palette` | The colours by distance from the emitter, or the palette's stops |
| `opacity` Opacity | number | 100 | 0 .. 100 |  | rerender |  | The particles' opacity, percent |
| `opacity_random` Opacity Random | number | 40 | 0 .. 100 |  | rerender |  | How much fainter particles may be, percent |
| `seed` Seed | integer | 1 | 0 .. 999 |  | relayout |  | Reshuffles every particle |
| `time` Time | number | 1 | 0 .. 1 |  | live |  | Where the particles are along their travel: 0 at birth, 1 where they come to rest (the still) |
| `loop` Loop | bool | false |  |  | step |  | Each particle runs on its own offset, so animating Time gives a continuous stream |
| `gravity` Gravity | number | 0 | -2000 .. 2000 | px | live |  | How far particles fall by Time 1 (negative rises) |
| `spin` Spin | number | 0 | -1080 .. 1080 | deg | live |  | How far particles turn by Time 1, clockwise, each at its own rate |
| `twinkle` Twinkle | number | 0 | 0 .. 1 |  | live | `shape` is `glint4` or `glint6` or `dot` or `square` | How strongly glints flash, each on its own beat |
| `fade_in` Fade In | number | 0 | 0 .. 1 |  | rerender |  | The share of each particle's time its opacity ramps up over |
| `fade_out` Fade Out | number | 0 | 0 .. 1 |  | rerender |  | The share of each particle's time its opacity ramps down over |

#### Ink Splatter (`ink_splatter`)

墨飛沫: seeded ink drops thrown off the silhouette of the layer it follows, with satellite droplets and drag streaks; below that layer, or above it to spatter over it. Black by default; set the colour to red for 赤 titles. Growth animates the drops live.

Cost C1 · 8-bit F0 · glyph atlas unsafe · live kernel · follow required · fills the canvas

Also found as: inkSplatter, ink splatter, 墨飛沫, 墨, ink splash, spatter, splatter, paint splatter, 赤.

| Param | Type | Default | Range | Unit | Animation | Shown when | Description |
|---|---|---|---|---|---|---|---|
| `amount` Amount | number | 45 | 0 .. 100 |  | rerender |  | How many main drops (80 at 100 %), percent |
| `spread` Spread | number | 150 | 0 .. 1500 | px | rerender |  | How far from the silhouette drops land |
| `size` Drop Size | number | 22 | 1 .. 300 | px | rerender |  | The largest drop's radius |
| `satellites` Satellite Droplets | number | 60 | 0 .. 200 |  | rerender |  | Droplets thrown around each drop, percent |
| `streaks` Streaks | number | 35 | 0 .. 100 |  | rerender |  | The share of drops dragged into streaks, percent |
| `streak_length` Streak Length | number | 110 | 0 .. 1500 | px | rerender |  | How far a drag streak runs |
| `direction` Streak Direction | enum | radial | `radial`, `directional` |  | step |  | Streaks fly away from the silhouette's centre or all one way |
| `angle` Streak Angle | number | 10 | -180 .. 180 | deg | rerender | `direction` is `directional` | The streaks' direction, clockwise |
| `color` Ink Color | color | #0b0907 |  |  | rerender |  | The ink's colour |
| `growth` Growth | number | 1 | 0 .. 1 |  | live |  | Every drop and streak grows from nothing (0) to its full size (1) |
| `seed` Seed | integer | 1 | 0 .. 999 |  | relayout |  | Reshuffles every drop |

### Rings

#### Shockwave (`shockwave`)

衝撃波: concentric light rings with a hard, soft, sine, double or fibrous band, soft edges, squashed and tilted onto a floor, broken up by noise, smeared radially, with a chromatic fringe and a core flash. Set the layer to Add.

Cost C1 · 8-bit F1 · glyph atlas unsafe · live kernel · follow none · fills the canvas

Also found as: shockwave, 衝撃波, radio waves, impact ring, ripple rings, 当たり告知.

| Param | Type | Default | Range | Unit | Animation | Shown when | Description |
|---|---|---|---|---|---|---|---|
| `center` Center | vec2 | vec2 |  | px | rerender |  | The rings' centre, canvas px from the layer's position (+y up) |
| `radius` Radius | number | 380 | 10 .. 3000 | px | live |  | The leading ring's radius |
| `rings` Rings | integer | 2 | 1 .. 8 |  | step |  | How many rings trail inward from the leading one |
| `spacing` Ring Spacing | number | 60 | 0 .. 1000 | px | live |  | The distance between rings |
| `width` Width | number | 24 | 1 .. 600 | px | live |  | Each ring's band width |
| `profile` Profile | enum | soft | `hard`, `soft`, `sine`, `double`, `fibrous` |  | step |  | The brightness across the band |
| `soft_in` Inner Softness | number | 30 | 0 .. 800 | px | rerender |  | How far the band's light fades inward |
| `soft_out` Outer Softness | number | 6 | 0 .. 800 | px | rerender |  | How far the band's light fades outward |
| `squash` Squash | number | 100 | 5 .. 100 |  | rerender |  | The ellipse's height over its width, percent (a ring lying on a floor) |
| `tilt` Tilt | number | 0 | -90 .. 90 | deg | rerender |  | Turns the squashed ellipse, clockwise |
| `breakup` Breakup | number | 25 | 0 .. 100 |  | rerender |  | Gaps noise cuts across the rings, percent |
| `breakup_scale` Breakup Scale | number | 40 | 2 .. 1000 | px | rerender |  | The size of the breakup's gaps |
| `smear` Radial Smear | number | 0 | 0 .. 100 |  | rerender |  | Streaks of ring colour thrown inward and outward, percent |
| `split` Chromatic Split | number | 0 | 0 .. 60 | px | rerender |  | Red drawn further out and blue further in by this much |
| `flash` Core Flash | number | 15 | 0 .. 200 |  | live |  | A white-hot flash at the centre, percent |
| `inner` Inner Color | color | #ffffff |  |  | rerender, light |  | The colour at the inner edge and of the core flash |
| `outer` Outer Color | color | #d8ecff |  |  | rerender, light |  | The colour of the leading ring |
| `seed` Seed | integer | 1 | 0 .. 999 |  | step |  | Reshuffles the fibres, breakup and smear |

#### Magic Circle (`magic_circle`)

魔法陣 / HUD rings: up to six concentric rows (ring, dashes, ticks, text, polygon, star, chevrons, arcs, hex grid), rotated, optionally laid back on the floor (ホログラム台座), coloured centre to edge, with an inner fill. Set the layer to Add.

Cost C2 · 8-bit F0 · glyph atlas unsafe · live kernel · follow none · fills the canvas

Also found as: magicCircle, magic circle, 魔法陣, summoning circle, sigil, HUD rings, hologram base, ホログラム台座, rune circle.

| Param | Type | Default | Range | Unit | Animation | Shown when | Description |
|---|---|---|---|---|---|---|---|
| `center` Center | vec2 | vec2 |  | px | rerender |  | The circle's centre, canvas px from the layer's position (+y up) |
| `radius` Radius | number | 320 | 10 .. 3000 | px | rerender |  | The circle's radius: the rows' radii are percents of it |
| `rotation` Rotation | number | 0 | -180 .. 180 | deg | live |  | Spins the whole circle, clockwise |
| `tilt` Tilt | number | 0 | 0 .. 80 | deg | rerender |  | Lays the circle back onto a floor plane, degrees (a hologram base) |
| `color` Color | gradient | #fff6d0@0 #f0b030@1 |  |  | rerender, light |  | The colour from the centre to the edge |
| `fill` Inner Fill | number | 6 | 0 .. 100 |  | rerender |  | A disc to the outermost row, brightening toward its rim like a lit lens, percent |
| `glyphs` Glyphs | enum | runes | `runes`, `kuji`, `zodiac`, `trigrams`, `hud`, `custom` |  | step |  | The characters text rows repeat around the circle |
| `custom_glyphs` Custom Glyphs | string |  |  |  | step | `glyphs` is `custom` | The characters a custom set repeats (up to 64) |
| `font` Font | asset | none |  |  | step |  | The font of the text rows; none = the set's library font (Zen Old Mincho Black; HUD: Zen Kaku Gothic New Black) |
| `rows` Rows | rows | rows |  |  | rerender |  | Up to six concentric rows, each a type, a radius (percent of the circle's), a size and a count |
| `seed` Seed | integer | 1 | 0 .. 999 |  | step |  | Reshuffles the random arcs |

#### Brush Swirl (`brush_swirl`)

筆渦 / 墨: thick dry-brush strokes along an arc, an inward spiral, a 円相 ring or a 一閃 slash, tapered in and out, with bristle gaps opening toward the dry tail and splatter droplets; Reveal draws the strokes.

Cost C2 · 8-bit F0 · glyph atlas unsafe · live kernel · follow none · fills the canvas

Also found as: brushSwirl, brush swirl, 筆渦, 墨, 円相, enso, 一閃, ink brush, sumi stroke, brush stroke ring.

| Param | Type | Default | Range | Unit | Animation | Shown when | Description |
|---|---|---|---|---|---|---|---|
| `center` Center | vec2 | vec2 |  | px | rerender |  | The swirl's centre, canvas px from the layer's position (+y up) |
| `path` Path | enum | arc | `arc`, `spiral`, `slash`, `ring` |  | relayout |  | Strokes along an arc, an inward spiral, a slash through the centre or once round |
| `radius` Radius | number | 380 | 10 .. 3000 | px | relayout |  | The path's radius (the slash's half length) |
| `sweep` Sweep | number | 300 | 10 .. 1080 | deg | relayout | `path` is `arc` or `spiral` | How far the arc or spiral turns, degrees |
| `angle` Start Angle | number | -150 | -180 .. 180 | deg | relayout |  | Where the strokes start, clockwise from +x |
| `strokes` Strokes | integer | 4 | 1 .. 12 |  | relayout |  | Bristle clumps side by side; the middle one is the full, widest stroke |
| `spacing` Stroke Spacing | number | 26 | 0 .. 400 | px | relayout |  | The distance between the strokes |
| `width` Width | number | 70 | 1 .. 600 | px | relayout |  | The main stroke's width |
| `taper_in` Taper In | number | 15 | 0 .. 100 |  | relayout |  | How thin and how long the press-in is, percent |
| `taper_out` Taper Out | number | 85 | 0 .. 100 |  | relayout |  | How thin and how long the lift-off tail is, percent |
| `dry` Dry Brush | number | 45 | 0 .. 100 |  | rerender |  | Bristle gaps (かすれ) opening toward the tail and the rims, percent |
| `splatter` Splatter | number | 30 | 0 .. 100 |  | rerender |  | Droplets sprayed off the sides and flicked past the ends, percent |
| `color` Color | color | #0b0907 |  |  | rerender |  | The ink's colour |
| `reveal` Reveal | number | 1 | 0 .. 1 |  | live |  | How much of each stroke is drawn, 0 to 1: animate it to draw the strokes |
| `seed` Seed | integer | 1 | 0 .. 999 |  | relayout |  | Reshuffles the strokes' wobble, bristles and splatter |

### Lines

#### Speed Lines (`speed_lines`)

集中線 / 流線 / フラッシュ / ベタフラ: tapered wedges around a clean focus (an ellipse, a rectangle or the followed layer's silhouette; following a layer the lines converge on it). Length is a share of the way to the frame edge (focus, flow) or of the focus radius (flash, beta). Set the layer to Add for light-like lines.

Cost C1 · 8-bit F0 · glyph atlas unsafe · live kernel · follow optional · fills the canvas

Also found as: speedLines, speed lines, 集中線, 流線, フラッシュ, ベタフラ, manga lines, focus lines, action lines, flow lines.

| Param | Type | Default | Range | Unit | Animation | Shown when | Description |
|---|---|---|---|---|---|---|---|
| `type` Type | enum | focus | `focus`, `flow`, `flash`, `beta` |  | relayout |  | Lines from the focus to the frame edge, parallel lines past it, short spikes, or a fill cut by spikes |
| `center` Center | vec2 | vec2 |  | px | relayout |  | Where the lines converge, canvas px (an offset from the followed layer's centre while following one) |
| `shape` Focus Shape | enum | ellipse | `ellipse`, `rect`, `layer` |  | relayout |  | The clear area the lines start from (Layer Silhouette needs a followed layer; without one the ellipse is used) |
| `padding` Padding | number | 40 | 0 .. 1000 | px | relayout | `shape` is `layer` | Clear space around the followed layer's silhouette |
| `radius` Focus Radius | vec2 | vec2 |  | px | relayout | `shape` is `ellipse` or `rect` | The focus ellipse's or rectangle's half width and height |
| `angle` Flow Angle | number | 0 | -180 .. 180 | deg | relayout | `type` is `flow` | The flow lines' direction, clockwise |
| `side` Flow Side | enum | both | `both`, `front`, `back` |  | step | `type` is `flow` | Which side of the focus the flow lines run from |
| `count` Line Count | integer | 160 | 4 .. 600 |  | relayout |  | How many lines |
| `width` Width | number | 10 | 0.2 .. 200 | px | rerender |  | Each line's width at its widest |
| `width_random` Width Random | number | 60 | 0 .. 100 |  | rerender |  | How much thinner each line may be at random, percent |
| `length` Length | number | 100 | 1 .. 300 |  | relayout |  | The lines' length: a share of the way to the frame edge or of the focus radius, percent |
| `length_random` Length Random | number | 0 | 0 .. 100 |  | relayout |  | How much shorter each line may be at random, percent |
| `jitter` Base Jitter | number | 60 | 0 .. 1000 | px | relayout |  | How far past the focus each line may start at random |
| `jag_count` Jagged Teeth | integer | 0 | 0 .. 64 |  | step |  | Teeth of a jagged focus edge around the centre (0 = smooth) |
| `jag_height` Jag Height | number | 40 | 0 .. 1000 | px | relayout |  | How far the teeth's tips reach past their notches |
| `clump` Clumping | integer | 1 | 1 .. 20 |  | step |  | How many lines run side by side in a clump |
| `gap` Gap | number | 0 | 0 .. 10 |  | relayout |  | Empty line slots between clumps |
| `taper_in` Taper (Focus End) | number | 100 | 0 .. 100 |  | rerender |  | How much of the length the focus end takes to grow to full width, percent |
| `taper_out` Taper (Far End) | number | 0 | 0 .. 100 |  | rerender |  | How much of the length the far end takes to narrow to a point, percent |
| `curve` Curve | number | 0 | -100 .. 100 |  | relayout |  | Bends every line the same way (100 = 60 deg over half the frame diagonal), percent |
| `color_mode` Fill | enum | solid | `solid`, `length`, `around` |  | step |  | One colour, a gradient along each line, or a gradient around the centre |
| `color` Color | color | #ffffff |  |  | rerender, light | `color_mode` is `solid` | The lines' colour |
| `gradient` Gradient | gradient | #ffffff@0 #ffc83a@1 |  |  | rerender, light | `color_mode` is `length` or `around` | The lines' colours |
| `opacity` Opacity | number | 85 | 0 .. 100 |  | rerender |  | Each line's own opacity (overlapping lines build up, unlike the layer's), percent |
| `seed` Seed | integer | 1 | 0 .. 999 |  | live |  | Reshuffles the lines (a stepped key reseeds them live: every 2-3 frames strobes them) |

#### Light Streaks (`light_streaks`)

ストリーク光: white-cored light lines with a coloured edge and halo: anamorphic lens streaks through the origin, a bundle of beams fanning out of it, or arrow-tapered spears. Brightness above 100 % goes past white; set the layer to Add.

Cost C2 · 8-bit F2 · glyph atlas unsafe · live kernel · follow none · fills the canvas

Also found as: lightStreaks, light streaks, ストリーク光, anamorphic streak, lens streak, light beams, streak.

| Param | Type | Default | Range | Unit | Animation | Shown when | Description |
|---|---|---|---|---|---|---|---|
| `mode` Mode | enum | anamorphic | `anamorphic`, `bundle`, `spears` |  | step |  | Lens streaks stacked on the origin, beams fanning out of it, or spears flying along the angle |
| `origin` Origin | vec2 | vec2 |  | px | live |  | Where the streaks start or centre, canvas px from the layer's position (+y up) |
| `angle` Angle | number | 0 | -180 .. 180 | deg | live |  | The streaks' direction, clockwise |
| `spread` Spread | number | 3 | 0 .. 360 | deg | rerender |  | How far the streaks fan out around the angle |
| `count` Count | integer | 6 | 1 .. 200 |  | relayout |  | How many streaks |
| `length` Length | number | 1600 | 10 .. 6000 | px | live |  | The longest streak's length |
| `length_random` Length Random | number | 55 | 0 .. 100 |  | rerender |  | How much shorter each streak may be at random, percent |
| `thickness` Thickness | number | 16 | 0.5 .. 200 | px | rerender |  | The thickest streak's width |
| `thickness_random` Thickness Random | number | 60 | 0 .. 100 |  | rerender |  | How much thinner each streak may be at random, percent |
| `taper_head` Taper Head | number | 100 | 0 .. 100 |  | rerender |  | How much of the length the head end takes to narrow to a point, percent |
| `taper_tail` Taper Tail | number | 100 | 0 .. 100 |  | rerender |  | How much of the length the tail end takes to narrow to a point, percent |
| `core_color` Core Color | color | #ffffff |  |  | rerender, light |  | The hot line along each streak's middle |
| `edge_color` Edge Color | color | #3d8bff |  |  | rerender, light |  | The colour of the streaks' edges and halo |
| `core_width` Core Width | number | 25 | 0 .. 100 |  | rerender |  | The core's share of each streak's width, percent |
| `brightness` Brightness | number | 250 | 0 .. 400 |  | live |  | How bright the streaks are (above 100 the core burns out), percent |
| `glow` Halo | number | 140 | 0 .. 400 | px | rerender |  | How far the coloured halo spreads (0 = none) |
| `chroma` Chromatic Split | number | 0 | 0 .. 40 | px | rerender |  | Red and blue profiles drawn this far apart across each streak |
| `seed` Seed | integer | 1 | 0 .. 999 |  | relayout |  | Reshuffles the streaks' lengths, widths and places |

#### Sunburst (`sunburst`)

放射 / 集中線: rays from a centre over the whole frame, an opaque background. Sunburst: two colours (or rays coloured by a gradient along the radius) merging into a solid disc at the inner radius, with a centre glow; Twist curls the rays. Wedges, Focus Lines and Speed Rings: rays of varying width and length around a cleared centre. Following a layer the rays centre on it and can stay on its pixels.

Cost C1 · 8-bit F0 · glyph atlas unsafe · live kernel · follow optional · fills the canvas

Also found as: sunburst, 放射, rays, light burst, radial rays, jackpot background, starburst.

| Param | Type | Default | Range | Unit | Animation | Shown when | Description |
|---|---|---|---|---|---|---|---|
| `style` Style | enum | sunburst | `sunburst`, `wedges`, `focus`, `rings` |  | step |  | Alternating ray wedges with a glowing hub, or varying wedges, needles or broken rings around a clear centre |
| `center_mode` Center | enum | layer | `layer`, `point` |  | step |  | The followed layer's centre (the layer's position without one), or the Center Point |
| `on_layer` On the Layer | enum | all | `all`, `clip`, `over` |  | step |  | With a followed layer: rays everywhere, only on its pixels, or on them and around them |
| `center` Center Point | vec2 | vec2 |  | px | rerender | `center_mode` is `point` | The rays' centre, canvas px (an offset from the followed layer's centre while following one) |
| `rays` Rays | integer | 24 | 2 .. 300 |  | step |  | How many rays (speed rings: rings per 1,000 px) |
| `rotation` Rotation | number | 0 | -180 .. 180 | deg | live |  | Turns the rays, clockwise |
| `width` Ray Width | number | 50 | 2 .. 100 |  | rerender |  | Each ray's share of its period, percent |
| `width_variation` Width Variation | number | 40 | 0 .. 100 |  | rerender | `style` is `wedges` or `focus` or `rings` | How much each ray's width varies, percent |
| `length_variation` Length Variation | number | 60 | 0 .. 100 |  | rerender | `style` is `focus` or `rings` | How much each needle's start or arc's length varies, percent |
| `twist` Twist | number | 0 | -360 .. 360 | deg | live | `style` is `sunburst` | Curls the rays clockwise, degrees per 1,000 px of radius |
| `inner` Inner Radius | number | 140 | 0 .. 4000 | px | rerender |  | The solid disc (sunburst) or the cleared centre (the others) |
| `softness` Softness | number | 10 | 0 .. 100 |  | rerender | `style` is `sunburst` | How soft the ray edges and the disc's rim are, percent |
| `inner_softness` Inner Softness | number | 60 | 0 .. 100 |  | rerender | `style` is `wedges` or `focus` or `rings` | How soft the cleared centre's edge is, percent |
| `color_mode` Ray Color | enum | ab | `ab`, `radial` |  | step | `style` is `sunburst` | Rays in Color A, or coloured by the gradient from the centre out |
| `color_a` Color A | color | #ffc93c |  |  | rerender | `color_mode` is `ab` | The rays' colour |
| `alternate` Two Colors | bool | false |  |  | step | `style` is `wedges` or `focus` or `rings` | Alternate the rays (or rings) between Color A and Color B |
| `color_b` Color B | color | #c2121c |  |  | rerender |  | The colour between the rays |
| `gradient` Gradient | gradient | #fff3b0@0 #e3a82a@0.45 #7a4a0a@1 |  |  | rerender | `color_mode` is `radial` | The rays' colours from the centre to the farthest corner |
| `glow` Center Glow | number | 70 | 0 .. 300 |  | rerender | `style` is `sunburst` | A whitened glow of the ray colour at the centre, percent |
| `opacity` Ray Opacity | number | 70 | 0 .. 100 |  | rerender | `style` is `wedges` or `focus` or `rings` | The rays' opacity, blended like Photoshop's (in sRGB), percent |
| `seed` Seed | integer | 1 | 0 .. 999 |  | step | `style` is `wedges` or `focus` or `rings` | Reshuffles the rays' widths, starts and arcs |

### Light

#### Glints (`glints`)

キラキラ: star sparkles on the layer it follows: on its highlight peaks (a few large glints plus small ones, favouring the face over the outer rims), big flare stars on its hottest peaks, one long anamorphic hero glint across it, scattered over and around its shape, or one star at a fixed point; 4-ray, 6-ray snow-cross or 8-ray shapes, optionally with rainbow rays. Set the layer to Add.

Cost C1 · 8-bit F2 · glyph atlas unsafe · live kernel · follow required · fills the canvas

Also found as: glints, glint, キラキラ, きらきら, sparkle, sparkles, twinkle, star glints, highlight stars, hero glint, 光沢.

| Param | Type | Default | Range | Unit | Animation | Shown when | Description |
|---|---|---|---|---|---|---|---|
| `placement` Placement | enum | highlights | `highlights`, `peaks`, `hero`, `scatter`, `point` |  | relayout |  | Where the stars go: the followed layer's highlights, its hottest peaks, one hero glint, a scatter or one point |
| `large` Large Glints | integer | 5 | 0 .. 20 |  | relayout | `placement` is `highlights` | How many large glints dress the brightest highlights |
| `small` Small Glints | integer | 6 | 0 .. 40 |  | relayout | `placement` is `highlights` | How many small twinkles go on the other highlights |
| `stars` Star Count | integer | 5 | 1 .. 40 |  | relayout | `placement` is `peaks` | How many flare stars go on the hottest peaks |
| `count` Count | integer | 40 | 1 .. 400 |  | relayout | `placement` is `scatter` | How many stars scatter over and around the shape |
| `hero_width` Hero Streak Width | number | 80 | 20 .. 150 |  | rerender | `placement` is `hero` | The hero streak's length, percent of the layer's width |
| `hero_thickness` Hero Thickness | number | 100 | 20 .. 400 |  | rerender | `placement` is `hero` | The hero streak's thickness, percent of the star size |
| `hero_cross` Vertical Spike | number | 22 | 0 .. 100 |  | rerender | `placement` is `hero` | The hero's vertical spike, percent of its streak |
| `hero_angle` Streak Angle | number | 0 | -90 .. 90 | deg | rerender | `placement` is `hero` | Turns the hero streak, clockwise |
| `hero_stars` Small Stars | integer | 3 | 0 .. 8 |  | relayout | `placement` is `hero` | How many small stars sparkle beside the hero glint |
| `position` Position | vec2 | vec2 |  | px | rerender | `placement` is `point` | The star's centre, canvas px from the canvas centre (+y up) |
| `size` Star Size | number | 50 | 2 .. 800 | px | rerender |  | The radius of a full-size star |
| `size_variation` Size Variation | number | 60 | 0 .. 100 |  | rerender | `placement` is `highlights` or `peaks` or `scatter` | How much weaker highlights shrink their stars, percent |
| `threshold` Highlight Threshold | number | 30 | 0 .. 100 |  | relayout | `placement` is `highlights` or `peaks` or `hero` | Highlights dimmer than this share of the brightest get no star, percent |
| `spacing` Min. Spacing | number | 80 | 0 .. 600 | px | relayout | `placement` is `highlights` or `hero` | The least distance between stars |
| `face_bias` Face Priority | number | 70 | 0 .. 100 |  | relayout | `placement` is `highlights` or `hero` | Favours highlights deep in the face over those on the outer rims, percent |
| `jitter` Size / Angle Jitter | number | 50 | 0 .. 100 |  | rerender | `placement` is `highlights` or `hero` | A little size and angle variation from star to star, percent |
| `spread` Scatter Area | number | 60 | 0 .. 600 | px | relayout | `placement` is `scatter` | How far past the shape's edge stars scatter |
| `inside` Scatter Over Shape | bool | true |  |  | step | `placement` is `scatter` | Stars also land on the shape itself, not only around it |
| `rays` Shape | enum | four | `four`, `snow`, `eight` |  | step |  | The star's rays |
| `spectral` Rainbow Rays | bool | false |  |  | step |  | Fans each ray out into a rainbow fringe |
| `thickness` Ray Thickness | number | 100 | 20 .. 400 |  | rerender |  | The rays' width at the core, percent |
| `color` Color | color | #fff4c0 |  |  | rerender, light |  | The stars' colour: white at the core, this toward the ray tips |
| `intensity` Intensity | number | 130 | 0 .. 400 |  | live |  | The stars' brightness, percent |
| `rotation` Rotation | number | 0 | -180 .. 180 | deg | live |  | Turns every star, clockwise |
| `brightness_variation` Brightness Variation | number | 50 | 0 .. 100 |  | rerender | `placement` is `highlights` or `hero` or `scatter` | How much smaller stars are dimmed, percent |
| `seed` Seed | integer | 1 | 0 .. 999 |  | relayout | `placement` is `highlights` or `peaks` or `hero` or `scatter` | Reshuffles the jitter, the scatter and the twinkle phases |
| `twinkle` Twinkle | number | 0 | 0 .. 1 |  | live |  | How deeply each star dims and flashes over its twinkle cycle |
| `twinkle_phase` Twinkle Phase | number | 0 | 0 .. 1 |  | live |  | Where every star is in its twinkle cycle (wraps; animate it 0 to 1) |
| `twinkle_rotate` Twinkle Rotation | number | 0 | 0 .. 45 | deg | live |  | How far each star rocks back and forth over its twinkle cycle |

#### Lens Flare (`lens_flare`)

An anamorphic lens flare added as light: a hot core, a horizontal streak, a starburst, a halo ring and ghosts on the axis through the canvas centre. Following a layer, its position is an offset from that layer's centre and it moves with it. Set the layer to Add.

Cost C1 · 8-bit F2 · glyph atlas unsafe · live kernel · follow optional · fills the canvas

Also found as: lensFlare, lens flare, レンズフレア, flare, anamorphic flare, anamorphic streak, starburst, halo ring.

| Param | Type | Default | Range | Unit | Animation | Shown when | Description |
|---|---|---|---|---|---|---|---|
| `position` Position | vec2 | vec2 |  | px | live |  | The flare's centre, canvas px from the layer's position (+y up); following a layer, from its centre |
| `size` Size | number | 70 | 4 .. 1000 | px | rerender |  | The core's size |
| `streak` Streak Length | number | 700 | 0 .. 4000 | px | rerender |  | How far the horizontal streak reaches |
| `rotation` Streak Angle | number | 0 | -180 .. 180 | deg | rerender |  | Turns the streak, clockwise |
| `rays` Starburst | number | 60 | 0 .. 200 |  | rerender |  | The starburst spikes' strength, percent |
| `ghosts` Ghosts | bool | true |  |  | step |  | Draws the ghost discs along the axis through the canvas centre |
| `color` Color | color | #7cc4ff |  |  | rerender, light |  | The flare's tint |
| `intensity` Intensity | number | 100 | 0 .. 400 |  | live |  | The flare's brightness, percent |
| `seed` Seed | integer | 1 | 0 .. 999 |  | step |  | Reshuffles the starburst and the ghosts |

#### Backlight (`backlight`)

後光: god rays streaming out from behind the layer it follows, from its own silhouette (before its effects), from its centre or a chosen point; place it below that layer.

Cost C2 · 8-bit F2 · glyph atlas unsafe · live kernel · follow required · fills the canvas

Also found as: backlight, god rays, 後光, 逆光, light rays, volumetric light, 光線, jackpot rays.

| Param | Type | Default | Range | Unit | Animation | Shown when | Description |
|---|---|---|---|---|---|---|---|
| `center_mode` Light Center | enum | shape | `shape`, `manual` |  | step |  | Where the rays radiate from: the followed layer's centre or a chosen point |
| `center` Center | vec2 | vec2 |  | px | rerender | `center_mode` is `manual` | The rays' centre, canvas px from the canvas centre (+y up) |
| `length` Ray Length | number | 260 | 10 .. 2000 | px | rerender |  | How far the rays stream out from the silhouette |
| `intensity` Intensity | number | 240 | 0 .. 600 |  | live |  | The rays' brightness, percent |
| `color` Inner Color | color | #fff6d0 |  |  | rerender, light |  | The colour of the brightest rays |
| `outer_color` Outer Color | color | #ffa318 |  |  | rerender, light |  | The colour the rays fade to |
| `rays` Ray Count | integer | 36 | 2 .. 120 |  | step |  | About how many rays go around the circle |
| `noise` Ray Noise | number | 90 | 0 .. 100 |  | rerender |  | How strongly the rays break up into separate beams, percent |
| `rotation` Rotation | number | 0 | -180 .. 180 | deg | live |  | Turns the ray pattern, clockwise |
| `seed` Seed | integer | 1 | 0 .. 999 |  | step |  | Reshuffles the ray pattern |

### Energy

#### Fire (`fire`)

Flames rising from the edges of the layer it follows (Direction -90 = up), behind it, or over its edges where they clear out inside the shape.

Cost C3 · 8-bit F2 · glyph atlas unsafe · live kernel · follow required · fills the canvas

Also found as: fire, flames, burning, 炎, 火炎, ファイア, 激アツ.

| Param | Type | Default | Range | Unit | Animation | Shown when | Description |
|---|---|---|---|---|---|---|---|
| `over_edge` Over the Edges | bool | false |  |  | step |  | The flames also lick over the outer band of the shape and clear out inside it |
| `height` Flame Height | number | 150 | 5 .. 600 | px | rerender |  | How tall the flames rise from the edge |
| `width` Flame Width | number | 22 | 3 .. 200 | px | rerender |  | The size of the tongues across the flow |
| `turbulence` Turbulence | number | 60 | 0 .. 150 |  | rerender |  | How much the tongues differ in length and the flame body sways, percent |
| `angle` Direction | number | -90 | -180 .. 180 | deg | rerender |  | The direction the flames rise, clockwise from right (-90 = up) |
| `spread` Side Burn | number | 35 | 0 .. 100 |  | rerender |  | How tall the flames burn up the sides of the shape, percent of the tops |
| `inside` Over-Edge Depth | number | 12 | 0 .. 100 | px | rerender | `over_edge` is `true` | How far into the shape the flames lick before clearing out |
| `phase` Evolution | number | 0 | 0 .. 10 |  | live |  | Moves the flames through their flicker (key it to animate) |
| `intensity` Intensity | number | 110 | 0 .. 300 |  | rerender |  | The flames' brightness, percent |
| `gradient` Colors | gradient | #ffffff@0 #fff27a@0.18 #ffb000@0.4 #ff4800@0.65 #c40c00@0.85 #4a0000@1 |  |  | rerender, light |  | The flame colours from the core to the tips |
| `seed` Seed | integer | 1 | 0 .. 999 |  | step |  | Reshuffles the tongues, sway and flicker |

#### Smoke (`smoke`)

Soft billowing smoke rising from the edges of the layer it follows (Direction -90 = up); put it below that layer.

Cost C3 · 8-bit F0 · glyph atlas unsafe · live kernel · follow required · fills the canvas

Also found as: smoke, steam, billowing smoke, 煙, 湯気, スモーク.

| Param | Type | Default | Range | Unit | Animation | Shown when | Description |
|---|---|---|---|---|---|---|---|
| `height` Smoke Height | number | 220 | 10 .. 800 | px | rerender |  | How tall the smoke rises from the edge |
| `width` Billow Size | number | 40 | 5 .. 300 | px | rerender |  | The size of the billows |
| `angle` Direction | number | -90 | -180 .. 180 | deg | rerender |  | The direction the smoke rises, clockwise from right (-90 = up) |
| `spread` Side Spread | number | 35 | 0 .. 100 |  | rerender |  | How tall the smoke rises from the sides of the shape, percent of the tops |
| `softness` Softness | number | 60 | 5 .. 100 |  | rerender |  | How gradually the smoke fades out, percent |
| `color` Highlight Color | color | #d8d2e6 |  |  | rerender |  | The colour of the lit billows |
| `shadow_color` Shadow Color | color | #4a4458 |  |  | rerender |  | The colour between the billows |
| `phase` Evolution | number | 0 | 0 .. 10 |  | live |  | Drifts and churns the billows (key it to animate) |
| `seed` Seed | integer | 1 | 0 .. 999 |  | step |  | Reshuffles the billows and plumes |

#### Aura (`aura`)

Wispy energy tendrils radiating from the edges of the layer it follows, or an aura (オーラ, strong or weak) of flickering tongues hugging its silhouette, drawn as light. Below the layer it glows from behind; above it set to Add, it lights the layer too.

Cost C3 · 8-bit F2 · glyph atlas unsafe · live kernel · follow required · fills the canvas

Also found as: aura, corona, オーラ, オーラ強, オーラ弱, コロナ, energy tendrils, power aura.

| Param | Type | Default | Range | Unit | Animation | Shown when | Description |
|---|---|---|---|---|---|---|---|
| `mode` Style | enum | tendrils | `tendrils`, `rising` |  | step |  | Tendrils radiating outward, or an aura of tongues hugging the silhouette |
| `length` Tendril Length | number | 120 | 10 .. 600 | px | rerender | `mode` is `tendrils` | How far the tendrils reach from the edge |
| `wispiness` Wispiness | number | 60 | 0 .. 200 |  | rerender | `mode` is `tendrils` | How much the tendrils curl toward their tips, percent |
| `density` Tendril Density | number | 100 | 20 .. 400 |  | rerender | `mode` is `tendrils` | How many tendrils run along the edge, percent |
| `strength` Strength | enum | strong | `strong`, `weak` |  | step | `mode` is `rising` | Strong tongues, or weak ones: shorter, dimmer and more broken |
| `flow` Flow | enum | outward | `outward`, `up` |  | step | `mode` is `rising` | The tongues reach outward along the edge, or rise with the sides licking up too |
| `tongue` Tongue Length | number | 90 | 5 .. 600 | px | rerender | `mode` is `rising` | How far the tongues reach from the silhouette |
| `turbulence_scale` Turbulence Scale | number | 60 | 5 .. 400 | px | rerender | `mode` is `rising` | The size of the stretches of edge that share a tongue |
| `breakup` Breakup | number | 50 | 0 .. 100 |  | rerender | `mode` is `rising` | How much the outer half of the tongues breaks into holes, percent |
| `glow` Edge Glow | number | 60 | 0 .. 200 |  | rerender |  | A soft glow hugging the edge, percent |
| `intensity` Intensity | number | 140 | 0 .. 400 |  | rerender |  | The light's brightness, percent |
| `gradient` Colors | gradient | #ffffff@0 #c6f4ff@0.2 #3ab4ff@0.45 #6a2cff@0.75 #d01cff@1 |  |  | rerender, light |  | The light's colours from the edge to the tips |
| `phase` Evolution | number | 0 | 0 .. 10 |  | live |  | Moves the tendrils and tongues through their flicker (key it to animate) |
| `seed` Seed | integer | 1 | 0 .. 999 |  | step |  | Reshuffles the tendrils and tongues |

#### Lightning (`lightning`)

Electric arcs crawling over and shooting out from the edges of the layer it follows, with branching bolts and their own glow; Along Edge (Electrify) runs many arcs hugging the whole outline. Set the layer to Add.

Cost C2 · 8-bit F1 · glyph atlas unsafe · live kernel · follow required · fills the canvas

Also found as: lightning, electric arcs, electrify, bolts, 稲妻, 雷, 電撃, スパーク.

| Param | Type | Default | Range | Unit | Animation | Shown when | Description |
|---|---|---|---|---|---|---|---|
| `mode` Arcs | enum | mixed | `mixed`, `outward`, `crawl`, `edge` |  | relayout |  | Bolts shooting out, crawling along the edge, both, or arcs hugging the whole outline |
| `count` Bolt Count | integer | 12 | 1 .. 40 |  | relayout | `mode` is `mixed` or `outward` or `crawl` | How many bolts start from the edge |
| `arcs` Arc Count | integer | 24 | 1 .. 60 |  | relayout | `mode` is `edge` | How many arcs walk the outline |
| `length` Bolt Length | number | 240 | 10 .. 800 | px | relayout |  | How long each bolt or arc runs |
| `jaggedness` Jaggedness | number | 55 | 0 .. 150 |  | relayout | `mode` is `mixed` or `outward` or `crawl` | How far the bolts zigzag off their line, percent |
| `branches` Branching | number | 60 | 0 .. 100 |  | relayout | `mode` is `mixed` or `outward` or `crawl` | How often the bolts fork, percent |
| `wrinkle` Wrinkle | number | 60 | 0 .. 150 |  | relayout | `mode` is `edge` | How much the arcs wander off the edge and zigzag, percent |
| `branchiness` Branchiness | number | 40 | 0 .. 100 |  | relayout | `mode` is `edge` | How often the arcs fork outward, percent |
| `core_width` Core Width | number | 3.5 | 0.5 .. 12 | px | rerender |  | The width of the bright core |
| `glow_size` Glow Size | number | 18 | 0 .. 80 | px | rerender | `mode` is `mixed` or `outward` or `crawl` | How far the bolts' glow spreads |
| `glow_width` Glow Width | number | 16 | 0 .. 80 | px | rerender | `mode` is `edge` | How far the arcs' glow spreads |
| `glow` Glow Strength | number | 100 | 0 .. 300 |  | live |  | The glow's brightness, percent |
| `color` Glow Color | color | #4fd8ff |  |  | rerender, light |  | The colour of the glow; the core is whitened toward it |
| `intensity` Intensity | number | 100 | 0 .. 300 |  | live |  | The bolts' brightness, percent |
| `seed` Seed | integer | 1 | 0 .. 999 |  | live |  | Reshuffles the bolts (a stepped key strobes them) |
