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
glyph atlas safe, approximate or unsafe (S81's per-glyph rendering); animation `live` (plays per frame through the
live variant), `rerender` (smooth), `step` or `relayout` (the layout re-runs);
`light` marks a colour of light (colour schemes map it onto their light ramp).

## Effects

### Glow

#### Outer Glow (`outer_glow`)

A glow around the layer, behind it like Photoshop's or added over it as light; one colour, a gradient across the layer, a gradient out from the edge, or the layer's own colours.

Cost C1 · 8-bit F1 · glyph atlas approximate · live scale_split · reach at defaults 40 px · adds light at defaults no

Also found as: glow, halo, outerGlow, 光彩 (外側), 後光.

Live (scale_split): `intensity`, `opacity`.

| Param | Type | Default | Range | Unit | Animation | Shown when | Description |
|---|---|---|---|---|---|---|---|
| `fill` Fill | enum | solid | `solid`, `gradient`, `layer`, `distance` |  | step |  | What colours the glow: one colour, a gradient across the layer, the layer's own colours or a gradient out from the edge |
| `color` Color | color | #ffb000 |  |  | rerender, light | `fill` is `solid` | The glow's colour |
| `gradient` Gradient | gradient | #ff4ad8@0 #30e0ff@1 |  |  | rerender, light | `fill` is `gradient` or `distance` | The glow's colours across the layer or out from its edge |
| `angle` Angle | number | 90 | -180 .. 360 | deg | rerender | `fill` is `gradient` | Direction of the gradient across the layer, clockwise (90 = top to bottom) |
| `technique` Technique | enum | softer | `softer`, `precise` |  | step |  | Softer blurs the silhouette (round halo); Precise follows the exact distance from the edge |
| `size` Size | number | 40 | 1 .. 400 | px | rerender |  | How far the glow reaches past the edge |
| `intensity` Intensity | number | 1.6 | 0 .. 6 | x | live |  | How bright the glow is near the edge |
| `spread` Spread | number | 0.15 | 0 .. 1 |  | rerender |  | How much of the glow stays solid before it fades |
| `opacity` Opacity | number | 1 | 0 .. 1 |  | live |  | The glow's opacity |
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

#### Light Glow (`light_glow`)

A physically based glow: the layer becomes a light source whose light scatters with a power-law falloff (like lens bloom) and burns toward white where it is over-bright; Radius and Falloff reshape the halo without changing how much light there is.

Cost C3 · 8-bit F2 · glyph atlas approximate · live scale_split · reach at defaults 200 px · adds light at defaults yes

Also found as: power glow, bloom, glare, light, deepGlow, deep glow light, 発光, 光.

Live (scale_split): `intensity`. Presets: `editor/resources/titleart/fx-presets/light_glow.yaml`.

| Param | Type | Default | Range | Unit | Animation | Shown when | Description |
|---|---|---|---|---|---|---|---|
| `exposure` Exposure | number | 1 | -4 .. 6 |  | rerender |  | Brightens the light, in stops (each stop doubles it) |
| `threshold` Threshold | number | 0 | 0 .. 2 |  | rerender |  | Only light brighter than this luminance glows (0 = all of it) |
| `knee` Threshold Knee | number | 0.25 | 0 .. 1 |  | rerender |  | How softly the threshold cuts in |
| `radius` Radius | number | 200 | 4 .. 1000 | px | rerender |  | How far the halo reaches |
| `falloff` Falloff | number | 50 | 0 .. 100 |  | rerender |  | Long haze (0) to a tight halo (100), in percent; the light's total stays the same |
| `intensity` Intensity | number | 1 | 0 .. 8 | x | live |  | How much of the light scatters into the glow |
| `chroma` Chromatic Scatter | number | 0 | -100 .. 100 |  | rerender |  | Red spreads wider than blue (+) or the reverse (-), in percent |
| `color_mode` Glow Color | enum | layer | `layer`, `custom` |  | step |  | The glow takes the layer's colours or one colour |
| `color` Color | color | #ffffff |  |  | rerender, light | `color_mode` is `custom` | The glow's colour |
| `tint_amount` Tint Amount | number | 0 | 0 .. 1 |  | rerender |  | How much the halo is tinted from Tint Inner to Tint Outer |
| `tint_inner` Tint Inner | color | #ffffff |  |  | rerender, light |  | The tint near the layer |
| `tint_outer` Tint Outer | color | #ff8a2a |  |  | rerender, light |  | The tint at the halo's edge |
| `highlight_shift` Highlight Shift | number | 60 | 0 .. 100 |  | rerender |  | Over-bright colour bleeds into its neighbours (blue to cyan to white), in percent |
| `core_white` Core Whiteness | number | 20 | 0 .. 100 |  | rerender |  | How quickly over-bright cores turn white, in percent |
| `on_face` Glow Over Face | number | 0 | 0 .. 1 |  | rerender |  | How much of the glow falls over the layer itself |

#### Contour Lines (`contour_lines`)

Light-guide panel lines (疑似導光板): glowing lines that follow the silhouette at even spacing, inside or around it, coloured by a gradient and added as light with a glow.

Cost C3 · 8-bit F2 · glyph atlas approximate · live kernel · reach at defaults 12 px · adds light at defaults yes · traces ink

Also found as: light guide, 導光板, 疑似導光板, contourLines, rings, offset lines, neon rings.

Live (kernel): `offset`, `intensity`, `glow`. Presets: `editor/resources/titleart/fx-presets/contour_lines.yaml`.

| Param | Type | Default | Range | Unit | Animation | Shown when | Description |
|---|---|---|---|---|---|---|---|
| `mode` Lines | enum | inside | `inside`, `outside`, `both` |  | step |  | Lines inside the shape, around it, or both |
| `count` Count | integer | 6 | 1 .. 40 |  | step |  | How many lines |
| `spacing` Spacing | number | 9 | 2 .. 200 | px | rerender |  | The distance between two lines |
| `width` Width | number | 2.5 | 0.5 .. 40 | px | rerender |  | How wide each line is |
| `offset` Offset | number | 4 | 0 .. 200 | px | live |  | How far the first line sits from the edge |
| `falloff` Falloff | number | 0.6 | 0 .. 1 |  | rerender |  | How much the farther lines fade |
| `gradient` Gradient | gradient | #ff2a2a@0 #ff9b00@0.17 #fff200@0.33 #2bff3a@0.5 #00c8ff@0.67 #3a3aff@0.83 #d22cff@1 |  |  | rerender, light |  | The lines' colours |
| `color_by` Color By | enum | across | `across`, `line` |  | step |  | The gradient runs across the layer, or from the first line to the last |
| `angle` Angle | number | -45 | -180 .. 360 | deg | rerender | `color_by` is `across` | Direction of the gradient across the layer, clockwise |
| `intensity` Intensity | number | 1.2 | 0 .. 6 | x | live |  | How bright the lines are |
| `glow_size` Glow Size | number | 12 | 0 .. 200 | px | rerender |  | How far the lines' glow reaches |
| `glow` Glow | number | 0.8 | 0 .. 4 | x | live |  | How bright the glow is |
| `dim` Dim Face | number | 0 | 0 .. 1 |  | rerender |  | Darkens the layer under the lines |
| `follow` Follow | enum | layer | `layer`, `stack` |  | step |  | Trace the layer's own silhouette, or everything the effects above drew (light left out either way) |

#### Neon Tube (`neon_tube`)

A glowing glass neon tube along the silhouette (or filling a thin layer), with a white-hot core, a light glow, broken-tube gaps and flicker.

Cost C3 · 8-bit F2 · glyph atlas approximate · live kernel · reach at defaults 44 px · adds light at defaults yes · follows the global light (`light_angle`)

Also found as: neon, neon sign, tube, ネオン, ネオン管.

Live (kernel): `core`, `glow`, `seed`. Presets: `editor/resources/titleart/fx-presets/neon_tube.yaml`.

| Param | Type | Default | Range | Unit | Animation | Shown when | Description |
|---|---|---|---|---|---|---|---|
| `source` Source | enum | outline | `outline`, `fill` |  | step |  | A tube along the edge, or the layer's own thin shape as the tube |
| `width` Width | number | 8 | 1 .. 100 | px | rerender |  | The tube's thickness |
| `offset` Offset | number | 0 | -100 .. 100 | px | rerender | `source` is `outline` | The tube's centre from the edge (+ outside) |
| `color` Color | color | #ff2bd6 |  |  | rerender, light |  | The tube's colour |
| `core_color` Core Color | color | #ffffff |  |  | rerender, light |  | The hot core's colour |
| `core` Core | number | 0.6 | 0 .. 1 |  | live |  | How bright the white-hot core is |
| `glow_size` Glow Size | number | 40 | 0 .. 400 | px | rerender |  | How far the glow reaches |
| `glow` Glow | number | 1 | 0 .. 4 | x | live |  | How bright the glow is |
| `gaps` Gaps | number | 0 | 0 .. 1 |  | rerender |  | Breaks along the tube (a broken sign) |
| `flicker` Flicker | number | 0 | 0 .. 1 |  | rerender |  | How much some sections dim; animate Seed to strobe |
| `light_angle` Light Angle | number | 120 | -180 .. 360 | deg | rerender |  | Where the light on the glass comes from (Photoshop's angle) |
| `use_global_light` Use Global Light | bool | true |  |  | step |  | Take the angle from the document's global light, so one change relights every effect |
| `seed` Seed | integer | 1 | 0 .. 999 |  | live |  | The gaps' and flicker's random pattern; reseed to flicker |

### Shadow & Depth

#### Drop Shadow (`drop_shadow`)

An offset, blurred shadow behind the layer; with Use Global Light it falls away from the document's light.

Cost C1 · 8-bit F1 · glyph atlas approximate · live scale_split · reach at defaults 24 px · adds light at defaults no · follows the global light (`angle`)

Also found as: dropShadow, shadow, ドロップシャドウ, 影.

Live (scale_split): `opacity`.

| Param | Type | Default | Range | Unit | Animation | Shown when | Description |
|---|---|---|---|---|---|---|---|
| `angle` Angle | number | 60 | -180 .. 360 | deg | rerender |  | The direction the shadow falls, clockwise (90 = straight down) |
| `use_global_light` Use Global Light | bool | true |  |  | step |  | Take the angle from the document's global light, so one change relights every effect |
| `distance` Distance | number | 10 | 0 .. 500 | px | rerender |  | How far the shadow is offset |
| `size` Size | number | 14 | 0 .. 300 | px | rerender |  | How soft the shadow is (its blur) |
| `spread` Spread | number | 0.1 | 0 .. 0.95 |  | rerender |  | How much of the shadow stays solid before it fades |
| `color` Color | color | #000000 |  |  | rerender |  | The shadow's colour |
| `opacity` Opacity | number | 0.75 | 0 .. 1 |  | live |  | The shadow's opacity |

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

#### Extrude (`extrude`)

A 2D extrusion: the shape stacked toward a direction behind the layer with shaded, gradient sides; with Shading and Edge at 0, a one-colour gradient and some Fade it is a flat long shadow.

Cost C3 · 8-bit F0 · glyph atlas unsafe · live none · reach at defaults 22 px · adds light at defaults no · follows the global light (`light_angle`)

Also found as: extrude2d, 3d text, block letters, depth, long shadow, 立体, 押し出し.

Presets: `editor/resources/titleart/fx-presets/extrude.yaml`.

| Param | Type | Default | Range | Unit | Animation | Shown when | Description |
|---|---|---|---|---|---|---|---|
| `angle` Angle | number | 75 | -180 .. 360 | deg | rerender |  | The direction the stack recedes, clockwise (90 = down) |
| `length` Length | number | 22 | 1 .. 3000 | px | rerender |  | How deep the extrusion is |
| `gradient` Gradient | gradient | #d97a00@0 #4a1600@1 |  |  | rerender |  | The sides' colours from the front to the back |
| `light_angle` Light Angle | number | 120 | -180 .. 360 | deg | rerender |  | Where the light shading the sides comes from (Photoshop's angle) |
| `use_global_light` Use Global Light | bool | true |  |  | step |  | Take the angle from the document's global light, so one change relights every effect |
| `shading` Shading | number | 0.7 | 0 .. 1 |  | rerender |  | How much the light shades the sides |
| `edge` Edge Darkening | number | 0.55 | 0 .. 1 |  | rerender |  | A dark contact line at the face and at the back |
| `fade` Fade | number | 0 | 0 .. 1 |  | rerender |  | Thins the extrusion toward its back |
| `opacity` Opacity | number | 1 | 0 .. 1 |  | rerender |  | The extrusion's opacity |

#### Perspective Shadow (`perspective_shadow`)

A shadow cast onto a floor at the base of the layer: sheared, squashed, blurring and fading with distance; up casts it behind the layer, down in front.

Cost C2 · 8-bit F0 · glyph atlas unsafe · live kernel · reach at defaults 252 px · adds light at defaults no · follows the global light (`angle`)

Also found as: perspectiveShadow, cast shadow, floor shadow, ground shadow, 影 (遠近).

Live (kernel): `angle`, `length`, `opacity`. Presets: `editor/resources/titleart/fx-presets/perspective_shadow.yaml`.

| Param | Type | Default | Range | Unit | Animation | Shown when | Description |
|---|---|---|---|---|---|---|---|
| `angle` Angle | number | -55 | -180 .. 180 | deg | live |  | The direction the shadow falls, clockwise (-90 = straight back) |
| `use_global_light` Use Global Light | bool | true |  |  | step |  | Take the angle from the document's global light, so one change relights every effect |
| `length` Length | number | 230 | 5 .. 2000 | px | live |  | From the base to the shadow's tip |
| `perspective` Perspective | number | 0.35 | 0 .. 0.95 |  | rerender |  | How much the far end narrows |
| `blur_near` Blur Near | number | 2 | 0 .. 100 | px | rerender |  | The blur at the base |
| `blur_far` Blur Far | number | 22 | 0 .. 300 | px | rerender |  | The blur at the tip |
| `fade` Fade | number | 0.75 | 0 .. 1 |  | rerender |  | How much the shadow fades toward its tip |
| `offset` Floor Offset | number | 0 | -300 .. 300 | px | rerender |  | Moves the floor line up or down |
| `color` Color | color | #000000 |  |  | rerender |  | The shadow's colour |
| `opacity` Opacity | number | 0.7 | 0 .. 1 |  | live |  | The shadow's opacity |

#### Floor Reflection (`floor_reflection`)

A mirrored copy of the layer below its base, as if standing on a glossy floor, fading and blurring with distance.

Cost C3 · 8-bit F0 · glyph atlas unsafe · live scale_split · reach at defaults 168 px · adds light at defaults no

Also found as: floorReflection, mirror, reflection, glossy floor, 映り込み, 反射.

Live (scale_split): `opacity`. Presets: `editor/resources/titleart/fx-presets/floor_reflection.yaml`.

| Param | Type | Default | Range | Unit | Animation | Shown when | Description |
|---|---|---|---|---|---|---|---|
| `gap` Gap | number | 4 | -100 .. 300 | px | rerender |  | The space between the layer and its reflection |
| `length` Fade Length | number | 150 | 5 .. 2000 | px | rerender |  | How far the reflection reaches before it fades out |
| `opacity` Opacity | number | 0.45 | 0 .. 1 |  | live |  | The reflection's opacity |
| `blur` Blur Growth | number | 14 | 0 .. 200 | px | rerender |  | The blur at the reflection's far end |
| `falloff` Falloff | number | 1.4 | 0.3 .. 5 |  | rerender |  | How quickly it fades |
| `scale` Vertical Scale | number | 1 | 0.2 .. 2 | x | rerender |  | Squashes or stretches the reflection |

### Stroke & Edge

#### Outline (`outline`)

A line traced from the layer's alpha just outside its silhouette, behind it: the thin dark keyline that separates a title's edge from the background, or a stroke; several following the stack make stacked strokes. Light is never traced and stays over the line.

Cost C1 · 8-bit F1 · glyph atlas approximate · live scale_split · reach at defaults 1.5 px · adds light at defaults no · traces ink

Also found as: keyline, stroke, border, 縁取り, フチ.

Live (scale_split): `opacity`.

| Param | Type | Default | Range | Unit | Animation | Shown when | Description |
|---|---|---|---|---|---|---|---|
| `width` Width | number | 1.5 | 0.5 .. 100 | px | rerender |  | How wide the line is |
| `color` Color | color | #0a0604 |  |  | rerender |  | The line's colour |
| `opacity` Opacity | number | 1 | 0 .. 1 |  | live |  | The line's opacity |
| `softness` Softness | number | 0 | 0 .. 1 |  | rerender |  | How much of the line's outer part fades |
| `threshold` Threshold | number | 0.5 | 0.05 .. 0.95 |  | rerender |  | The alpha level traced as the edge |
| `follow` Follow | enum | layer | `layer`, `stack` |  | step |  | Trace the layer's own silhouette, or everything the effects above drew (light left out either way) |

#### Ink Brush Stroke (`brush_stroke`)

An ink brush stroke along the silhouette (outside, centred or inside) whose width breathes with pressure, split into bristle lanes with dry gaps and ragged edges: sumi-e, marker, calligraphy.

Cost C1 · 8-bit F0 · glyph atlas approximate · live none · reach at defaults 12 px · adds light at defaults no · traces ink

Also found as: brush, ink, sumi, calligraphy, marker, 筆, 墨, 毛筆.

Presets: `editor/resources/titleart/fx-presets/brush_stroke.yaml`.

| Param | Type | Default | Range | Unit | Animation | Shown when | Description |
|---|---|---|---|---|---|---|---|
| `position` Position | enum | outside | `outside`, `center`, `inside` |  | step |  | Where the stroke sits on the edge |
| `width` Width | number | 10 | 0.5 .. 200 | px | rerender |  | The stroke's width |
| `pressure` Pressure | number | 0.6 | 0 .. 1 |  | rerender |  | How much the width varies along the contour |
| `bristles` Bristles | number | 0.5 | 0 .. 1 |  | rerender |  | How visible the bristle lanes are |
| `dry` Dry | number | 0.3 | 0 .. 1 |  | rerender |  | Dry-brush gaps along the stroke |
| `roughness` Roughness | number | 0.4 | 0 .. 1 |  | rerender |  | How ragged the stroke's edges are |
| `color` Color | color | #111111 |  |  | rerender |  | The ink's colour |
| `opacity` Opacity | number | 1 | 0 .. 1 |  | rerender |  | The stroke's opacity |
| `seed` Seed | integer | 1 | 0 .. 999 |  | step |  | The stroke's random pattern |

#### Stitch (`stitch`)

Embroidery: thread stitches along the contour (running, cross, zigzag or a satin border), evenly fitted and shaded as round threads with needle holes, with an optional satin or tatami fill (badges, patches, sports logos).

Cost C3 · 8-bit F0 · glyph atlas approximate · live none · reach at defaults 3 px · adds light at defaults no · traces ink · follows the global light (`light_angle`)

Also found as: embroidery, sewing, thread, patch, badge, seam, 刺繍, ステッチ.

Presets: `editor/resources/titleart/fx-presets/stitch.yaml`.

| Param | Type | Default | Range | Unit | Animation | Shown when | Description |
|---|---|---|---|---|---|---|---|
| `style` Style | enum | running | `running`, `cross`, `zigzag`, `satin_border` |  | step |  | The stitch |
| `offset` Offset | number | -6 | -200 .. 200 | px | relayout |  | The stitch line's distance from the edge (- inside) |
| `stitch_length` Stitch Length | number | 12 | 2 .. 100 | px | relayout |  | How long a stitch is |
| `gap` Gap | number | 6 | 0 .. 100 | px | relayout |  | The space between two stitches |
| `thread_width` Thread Width | number | 3 | 0.5 .. 20 | px | rerender |  | How thick the thread is |
| `color` Color | color | #ffffff |  |  | rerender |  | The thread's colour |
| `shade` Shade | number | 0.5 | 0 .. 1 |  | rerender |  | How round the thread looks |
| `holes` Holes | number | 0.4 | 0 .. 1 |  | rerender |  | How dark the needle holes are |
| `fill` Fill | enum | none | `none`, `satin`, `tatami` |  | step |  | Thread lines over the layer inside the stitches |
| `fill_angle` Fill Angle | number | 45 | -180 .. 360 | deg | rerender | `fill` is `satin` or `tatami` | The fill threads' direction, clockwise |
| `fill_density` Fill Density | number | 3 | 1 .. 20 | px | rerender | `fill` is `satin` or `tatami` | The distance between two fill threads |
| `light_angle` Light Angle | number | 120 | -180 .. 360 | deg | rerender |  | Where the light on the threads comes from (Photoshop's angle) |
| `use_global_light` Use Global Light | bool | true |  |  | step |  | Take the angle from the document's global light, so one change relights every effect |
| `seed` Seed | integer | 1 | 0 .. 999 |  | relayout |  | The stitches' jitter |

### Color

#### Color Overlay (`color_overlay`)

Fills the layer with one colour in a blend mode, keeping its alpha (Photoshop's Color Overlay).

Cost C0 · 8-bit F0 · glyph atlas safe · live kernel · reach at defaults 0 px · adds light at defaults no

Also found as: colour overlay, tint, fill color, カラーオーバーレイ.

Live (kernel): `opacity`, `color`.

| Param | Type | Default | Range | Unit | Animation | Shown when | Description |
|---|---|---|---|---|---|---|---|
| `color` Color | color | #ff0000 |  |  | live |  | The overlay colour |
| `opacity` Opacity | number | 1 | 0 .. 1 |  | live |  | How much of the colour covers the layer |
| `mode` Blend Mode | enum | normal | `normal`, `add`, `screen`, `multiply`, `overlay`, `soft_light`, `hard_light`, `darken`, `lighten`, `color_dodge`, `color_burn`, `linear_burn`, `difference`, `exclusion`, `hue`, `saturation`, `color`, `luminosity` |  | step |  | How the colour combines with the layer's own |

#### Gradient Overlay (`gradient_overlay`)

Recolours the layer with a gradient, keeping its alpha: Linear across its bounds (90 = top to bottom), Radial from its centre, or Sweep, a band of fixed length moved across by Position (a glint); Light and Shade add or filter in linear light.

Cost C0 · 8-bit F0 · glyph atlas unsafe · live kernel · reach at defaults 0 px · adds light at defaults no

Also found as: gradientOverlay, gradient, shine, グラデーションオーバーレイ.

Live (kernel): `position`, `amount`, `angle`, `offset`, `scale`.

| Param | Type | Default | Range | Unit | Animation | Shown when | Description |
|---|---|---|---|---|---|---|---|
| `gradient` Gradient | gradient | #e8a020@0 #fff6b0@0.2 #f0a820@0.4 #ffc840@0.6 #c05a08@0.8 #ffe070@1 |  |  | rerender |  | The colours laid over the layer |
| `style` Style | enum | linear | `linear`, `radial`, `sweep` |  | step |  | Across the layer's box, out from its centre, or a band moved across it |
| `angle` Angle | number | 90 | -180 .. 360 | deg | live | `style` is `linear` or `sweep` | The gradient's direction, clockwise (90 = top to bottom) |
| `scale` Scale | number | 1 | 0.1 .. 10 | x | live | `style` is `linear` or `radial` | How much of the box the gradient spans |
| `offset` Offset | number | 0 | -2 .. 2 |  | live | `style` is `linear` or `radial` | Moves the gradient along its direction |
| `length` Length | number | 200 | 2 .. 4000 | px | rerender | `style` is `sweep` | The band's length |
| `position` Position | number | 0 | -100 .. 100 |  | live | `style` is `sweep` | Where the band is, percent of the way across: -100 just off one side, 100 just off the other |
| `wrap` Wrap | enum | clamp | `clamp`, `repeat`, `mirror` |  | step |  | What the gradient does past its ends |
| `amount` Amount | number | 1 | 0 .. 1 |  | live |  | How much of the gradient covers the layer |
| `mode` Blend Mode | enum | normal | `normal`, `add`, `screen`, `multiply`, `overlay`, `soft_light`, `hard_light`, `darken`, `lighten`, `color_dodge`, `color_burn`, `linear_burn`, `difference`, `exclusion`, `hue`, `saturation`, `color`, `luminosity`, `light`, `shade` |  | step |  | How the gradient combines with the layer's colour |

#### Adjust (`adjust`)

One adjustment (Levels, Curves, Hue/Saturation, Gradient Map...) on just this layer, group or part: the same operation as an adjustment layer.

Cost C0 · 8-bit F0 · glyph atlas safe · live none · reach at defaults 0 px · adds light at defaults no

Also found as: adjustment, levels, curves, hue saturation, color correction, 色調補正.

| Param | Type | Default | Range | Unit | Animation | Shown when | Description |
|---|---|---|---|---|---|---|---|
| `adjustment` Adjustment | adjustment | {"type":"hue_saturation","hue":0,"saturation":25,"lightness":0,"colorize":false} |  |  | rerender |  | The adjustment and its settings (the fourteen types of an adjustment layer) |

#### Grain (`grain`)

Film grain over the layer, monochrome or colour, strongest in the midtones; transparency is left untouched.

Cost C0 · 8-bit F0 · glyph atlas approximate · live kernel · reach at defaults 0 px · adds light at defaults no

Also found as: film grain, noise, grit, 粒子, ノイズ.

Live (kernel): `amount`, `seed`. Presets: `editor/resources/titleart/fx-presets/grain.yaml`.

| Param | Type | Default | Range | Unit | Animation | Shown when | Description |
|---|---|---|---|---|---|---|---|
| `amount` Amount | number | 0.14 | 0 .. 1 |  | live |  | How strong the grain is |
| `size` Size | number | 1.2 | 0.3 .. 20 | px | rerender |  | The grain's size |
| `color` Color Grain | bool | false |  |  | step |  | Coloured grain instead of grey |
| `seed` Seed | integer | 1 | 0 .. 999 |  | live |  | The grain's random pattern; reseed every frame for moving film grain |

#### Holographic Foil (`holographic`)

A holographic foil: thin-film rainbow colours that shift with the view tilt and across the layer, with a diffraction, glitter or star micro pattern.

Cost C1 · 8-bit F0 · glyph atlas approximate · live kernel · reach at defaults 0 px · adds light at defaults no

Also found as: hologram, holo, foil, iridescent, rainbow foil, ホログラム, 箔.

Live (kernel): `tilt`. Presets: `editor/resources/titleart/fx-presets/holographic.yaml`.

| Param | Type | Default | Range | Unit | Animation | Shown when | Description |
|---|---|---|---|---|---|---|---|
| `amount` Amount | number | 1 | 0 .. 1 |  | rerender |  | How strongly the foil covers the layer |
| `tilt` Tilt | vec2 | vec2 |  | px | live |  | The simulated view tilt (-1..1 each way): animate it for the shimmer |
| `film_thickness` Film Thickness | number | 450 | 100 .. 1200 |  | rerender |  | The film's thickness in nanometres: which colours it shows |
| `film_ior` Film IOR | number | 1.5 | 1 .. 2.5 |  | rerender |  | The film's refractive index |
| `relief` Relief | number | 0.5 | 0 .. 1 |  | rerender |  | A bevel and wobble that shift the colours |
| `pattern` Pattern | enum | glitter | `none`, `diffraction`, `glitter`, `stars` |  | step |  | The foil's micro pattern |
| `pattern_scale` Pattern Scale | number | 6 | 1 .. 100 | px | rerender |  | The micro pattern's size |
| `mode` Blend | enum | overlay | `normal`, `add`, `screen`, `multiply`, `overlay`, `soft_light`, `hard_light`, `darken`, `lighten`, `color_dodge`, `color_burn`, `linear_burn`, `difference`, `exclusion`, `hue`, `saturation`, `color`, `luminosity` |  | step |  | How the foil blends over the layer |
| `seed` Seed | integer | 1 | 0 .. 999 |  | step |  | The pattern's random layout |

### Texture & Material

#### Pattern Overlay (`pattern_overlay`)

Lays a procedural pattern (zebra, leopard, checker, 和柄...) or a tiled image over the layer in a blend mode, keeping its alpha (Photoshop's Pattern Overlay).

Cost C0 · 8-bit F0 · glyph atlas safe · live none · reach at defaults 0 px · adds light at defaults no

Also found as: pattern, texture overlay, 柄, パターンオーバーレイ.

| Param | Type | Default | Range | Unit | Animation | Shown when | Description |
|---|---|---|---|---|---|---|---|
| `pattern` Pattern | enum | stripes | `zebra`, `tiger`, `giraffe`, `leopard`, `checker`, `hazard`, `stripes`, `dots`, `sakura`, `seigaiha`, `asanoha`, `shippo`, `kikko`, `uroko`, `wood`, `marble`, `granite`, `concrete`, `brushed_metal`, `diamond_plate`, `carbon_fiber`, `rust`, `leather`, `brick`, `camo`, `circuit`, `custom` |  | step |  | The pattern: a procedural kind, or a custom image tiled once per motif |
| `color` Color | color | #000000 |  |  | rerender |  | The marks' colour |
| `color2` Color 2 | color | #ffffff |  |  | rerender |  | The secondary colour of the kinds that have one (checker, hazard, leopard, sakura) |
| `scale` Scale | number | 40 | 2 .. 2000 | px | rerender |  | The size of one motif |
| `angle` Angle | number | 0 | -180 .. 360 | deg | rerender |  | The pattern's rotation, clockwise |
| `seed` Seed | integer | 1 | 0 .. 999 |  | step |  | Reshuffles the random kinds (the document's seed adds to it) |
| `image` Image | asset | none |  |  | step | `pattern` is `custom` | The image a custom pattern tiles |
| `opacity` Opacity | number | 1 | 0 .. 1 |  | rerender |  | How much of the pattern covers the layer |
| `mode` Blend Mode | enum | normal | `normal`, `add`, `screen`, `multiply`, `overlay`, `soft_light`, `hard_light`, `darken`, `lighten`, `color_dodge`, `color_burn`, `linear_burn`, `difference`, `exclusion`, `hue`, `saturation`, `color`, `luminosity` |  | step |  | How the pattern combines with the layer's colour |
| `align` Align | enum | layer | `layer`, `canvas` |  | step |  | Anchor the pattern at the layer's position (it moves with the layer) or at the canvas centre |

#### Distress (`distress`)

Wears the layer away: grunge, a dry brush (かすれ), rough edges, scratches, speckle or spray paint, inside a band along the edge or over the whole layer, the rest darkened by dirt.

Cost C1 · 8-bit F0 · glyph atlas approximate · live none · reach at defaults 0 px · adds light at defaults no

Also found as: grunge, worn, かすれ, weathered, dry brush, rough, scratched, spray paint, 汚し.

Presets: `editor/resources/titleart/fx-presets/distress.yaml`.

| Param | Type | Default | Range | Unit | Animation | Shown when | Description |
|---|---|---|---|---|---|---|---|
| `pattern` Pattern | enum | grunge | `grunge`, `dry_brush`, `rough_edges`, `scratches`, `speckle`, `spray` |  | step |  | How the layer wears |
| `amount` Amount | number | 0.5 | 0 .. 1 |  | rerender |  | How much wears away |
| `edge` Edge | number | 12 | 0 .. 400 | px | rerender |  | The band in from the edge that wears (0 = the whole layer) |
| `scale` Scale | number | 24 | 1 .. 1000 | px | rerender |  | The wear's size (the dry brush: its stroke length) |
| `angle` Angle | number | 0 | -180 .. 360 | deg | rerender | `pattern` is `dry_brush` or `scratches` | The brush or scratch direction, clockwise |
| `contrast` Contrast | number | 0.6 | 0 .. 1 |  | rerender |  | How crisp the worn edges are |
| `wear` Wear | number | 0 | 0 .. 1 |  | rerender |  | Darkens what remains with dirt |
| `wear_color` Wear Color | color | #3a2a1a |  |  | rerender |  | The dirt's colour |
| `outside` Outside | number | 0 | 0 .. 1 |  | rerender | `pattern` is `speckle` or `spray` | Speckle or spray beyond the edge, within Edge |
| `seed` Seed | integer | 1 | 0 .. 999 |  | step |  | The wear's random pattern |

#### Drips (`drips`)

Paint, slime, blood or icicles dripping from the edges that face gravity, merged smoothly with the shape and shaded with a gloss streak.

Cost C3 · 8-bit F1 · glyph atlas approximate · live kernel · reach at defaults 115.6 px · adds light at defaults no · follows the global light (`light_angle`)

Also found as: drip, dripping, melting, slime, blood, icicles, horror, 垂れ, したたり.

Live (kernel): `length`. Presets: `editor/resources/titleart/fx-presets/drips.yaml`.

| Param | Type | Default | Range | Unit | Animation | Shown when | Description |
|---|---|---|---|---|---|---|---|
| `style` Style | enum | paint | `paint`, `slime`, `blood`, `icicles` |  | step |  | The drips' look (icicles taper to a point) |
| `count` Count | integer | 14 | 0 .. 200 |  | relayout |  | How many drips |
| `length` Length | number | 90 | 0 .. 2000 | px | live |  | How long the drips run |
| `length_random` Length Random | number | 60 | 0 .. 100 |  | rerender |  | How much the drips' lengths vary, in percent |
| `width` Width | number | 14 | 1 .. 200 | px | rerender |  | How wide the drips are |
| `width_random` Width Random | number | 40 | 0 .. 100 |  | rerender |  | How much the drips' widths vary, in percent |
| `head` Head | number | 1.4 | 1 .. 3 | x | rerender |  | The rounded head's size against the drip's width |
| `angle` Gravity | number | 90 | -180 .. 360 | deg | rerender |  | The direction the drips run, clockwise (90 = down) |
| `edge_bias` Edge Bias | number | 70 | 0 .. 100 |  | rerender |  | Prefer edges facing gravity squarely, in percent |
| `merge` Merge | number | 6 | 0 .. 60 | px | rerender |  | How smoothly the drips join the shape |
| `color_mode` Color Mode | enum | layer | `layer`, `custom` |  | step |  | The drips take the ink above them, or one colour |
| `color` Color | color | #c8102e |  |  | rerender | `color_mode` is `custom` | The drips' colour |
| `gloss` Gloss | number | 0.6 | 0 .. 1 |  | rerender |  | How glossy the drips are |
| `light_angle` Light Angle | number | 120 | -180 .. 360 | deg | rerender |  | Where the light on the drips comes from (Photoshop's angle) |
| `use_global_light` Use Global Light | bool | true |  |  | step |  | Take the angle from the document's global light, so one change relights every effect |
| `seed` Seed | integer | 1 | 0 .. 999 |  | relayout |  | Where the drips fall |

#### Frost (`frost`)

Ice crystals growing in from the edges as Amount rises, rime crystals reaching outward, facets glinting in the light.

Cost C1 · 8-bit F1 · glyph atlas approximate · live kernel · reach at defaults 6 px · adds light at defaults no

Also found as: ice, frozen, icy, rime, winter, 霜, 氷.

Live (kernel): `amount`. Presets: `editor/resources/titleart/fx-presets/frost.yaml`.

| Param | Type | Default | Range | Unit | Animation | Shown when | Description |
|---|---|---|---|---|---|---|---|
| `amount` Amount | number | 0.7 | 0 .. 1 |  | live |  | How far the frost has grown |
| `reach` Reach | number | 18 | 0 .. 400 | px | rerender |  | How far in from the edge the frost grows |
| `outside` Outside | number | 6 | 0 .. 200 | px | rerender |  | How far the rime crystals reach outward |
| `crystal_size` Crystal Size | number | 10 | 1 .. 200 | px | rerender |  | The crystals' size |
| `color` Color | color | #dff6ff |  |  | rerender |  | The frost's colour |
| `tint` Tint | color | #7fc8ff |  |  | rerender |  | The tint mixed into the frost |
| `tint_amount` Tint Amount | number | 0.35 | 0 .. 1 |  | rerender |  | How much of the tint |
| `sparkle` Sparkle | number | 0.4 | 0 .. 1 |  | rerender |  | How much the facets glint in the global light |
| `seed` Seed | integer | 1 | 0 .. 999 |  | step |  | The crystals' random layout |

#### Chalk (`chalk`)

The shape drawn in chalk: strokes along an angle over a paper grain, a broken edge and dust falling around it.

Cost C2 · 8-bit F0 · glyph atlas approximate · live none · reach at defaults 12 px · adds light at defaults no

Also found as: chalkboard, blackboard, pastel, crayon, チョーク, 黒板.

Presets: `editor/resources/titleart/fx-presets/chalk.yaml`.

| Param | Type | Default | Range | Unit | Animation | Shown when | Description |
|---|---|---|---|---|---|---|---|
| `grain` Grain | number | 3 | 0.5 .. 40 | px | rerender |  | The paper grain's size |
| `coverage` Coverage | number | 0.7 | 0 .. 1 |  | rerender |  | How much of the shape the chalk covers |
| `stroke_angle` Stroke Angle | number | -30 | -180 .. 360 | deg | rerender |  | The strokes' direction, clockwise |
| `stroke_length` Stroke Length | number | 40 | 1 .. 400 | px | rerender |  | How long the strokes are |
| `edge_rough` Edge Roughness | number | 0.5 | 0 .. 1 |  | rerender |  | How broken the edge is |
| `edge_width` Edge Width | number | 4 | 0 .. 50 | px | rerender |  | How far the broken edge reaches |
| `dust` Dust | number | 0.3 | 0 .. 1 |  | rerender |  | Chalk dust around the shape |
| `dust_reach` Dust Reach | number | 12 | 0 .. 200 | px | rerender |  | How far the dust falls |
| `color_mode` Color Mode | enum | layer | `layer`, `custom` |  | step |  | The layer's colours, or one chalk colour |
| `color` Color | color | #f4f1e8 |  |  | rerender | `color_mode` is `custom` | The chalk's colour |
| `seed` Seed | integer | 1 | 0 .. 999 |  | step |  | The strokes' random pattern |

#### Material (`material`)

Bevel, chrome, metal grain or emboss on the shape: a title Material (fill, bevel and emboss, finish, pattern, satin, texture, sheen, inner glow, gloss) over the layer's own colours or the material's fill.

Cost C3 · 8-bit F1 · glyph atlas safe · live none · reach at defaults 0 px · adds light at defaults no

Also found as: bevel, emboss, deboss, chrome, metal, gold, bevel and emboss, ベベル, エンボス, 金属.

Presets: `editor/resources/titleart/fx-presets/material.yaml`.

| Param | Type | Default | Range | Unit | Animation | Shown when | Description |
|---|---|---|---|---|---|---|---|
| `color_mode` Color | enum | layer | `layer`, `fill` |  | step |  | Shade the layer's own colours, or the material's fill |
| `size` Size | number | 200 | 10 .. 2000 | px | rerender |  | The length the material's percentages are of |
| `material` Material | material | material |  |  | rerender |  | The material: every Material member but glass and refraction; a reflection without its map is a sky and ground horizon |

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

#### Motion Blur (`motion_blur`)

Smears the layer evenly along a direction over Distance, centred on it, or one-sided as a trail left behind the motion.

Cost C2 · 8-bit F1 · glyph atlas unsafe · live none · reach at defaults 20 px · adds light at defaults no

Also found as: motionBlur, speed blur, directional blur, streak, 動き, モーションブラー.

Presets: `editor/resources/titleart/fx-presets/motion_blur.yaml`.

| Param | Type | Default | Range | Unit | Animation | Shown when | Description |
|---|---|---|---|---|---|---|---|
| `angle` Angle | number | 0 | -180 .. 360 | deg | rerender |  | The motion's direction, clockwise (0 = right) |
| `distance` Distance | number | 40 | 0 .. 2000 | px | rerender |  | How far the layer smears |
| `mode` Type | enum | centered | `centered`, `trail` |  | step |  | Centred on the layer, or a one-sided trail |

#### Lens Blur (`lens_blur`)

An out-of-focus camera blur through an iris (circle, hexagon or octagon) in which bright points bloom into hard-edged bokeh discs; Noise adds grain back so the smooth blur does not band.

Cost C4 · 8-bit F1 · glyph atlas approximate · live none · reach at defaults 20 px · adds light at defaults no

Also found as: lensBlur, bokeh, defocus, depth of field, ぼかし (レンズ), ボケ.

Presets: `editor/resources/titleart/fx-presets/lens_blur.yaml`.

| Param | Type | Default | Range | Unit | Animation | Shown when | Description |
|---|---|---|---|---|---|---|---|
| `radius` Radius | number | 20 | 0 .. 300 | px | rerender |  | How out of focus the layer is |
| `shape` Iris Shape | enum | hexagon | `circle`, `hexagon`, `octagon` |  | step |  | The shape of the bokeh |
| `rotation` Blade Rotation | number | 0 | 0 .. 360 | deg | rerender |  | Turns the iris |
| `brightness` Highlight Brightness | number | 15 | 0 .. 100 |  | rerender |  | How much the highlights take over their disc |
| `threshold` Highlight Threshold | number | 235 | 0 .. 255 |  | rerender |  | The brightness (0-255) a highlight starts at |
| `noise` Noise Amount | number | 2 | 0 .. 100 |  | rerender |  | Grain added back, in 8-bit levels |
| `distribution` Noise Distribution | enum | uniform | `uniform`, `gaussian` |  | step |  | The grain's distribution |
| `mono` Monochromatic | bool | true |  |  | step |  | Grey grain instead of coloured |
| `seed` Seed | integer | 1 | 0 .. 999 |  | step |  | The grain's random pattern |

#### Tilt-Shift Blur (`tilt_shift`)

A sharp focus band through a centre point, blurring smoothly more and more beyond it up to Blur: the miniature look.

Cost C3 · 8-bit F1 · glyph atlas unsafe · live kernel · reach at defaults 24 px · adds light at defaults no

Also found as: tiltShift, miniature, field blur, focus band, ミニチュア.

Live (kernel): `center`, `angle`, `width`, `falloff`. Presets: `editor/resources/titleart/fx-presets/tilt_shift.yaml`.

| Param | Type | Default | Range | Unit | Animation | Shown when | Description |
|---|---|---|---|---|---|---|---|
| `blur` Blur | number | 16 | 0 .. 200 | px | rerender |  | The most blur, at the far edges |
| `center` Center | vec2 | vec2 |  | px | live |  | The focus band's centre, canvas px from the canvas centre |
| `angle` Angle | number | 0 | -180 .. 360 | deg | live |  | Tilts the band, clockwise (0 = horizontal) |
| `width` Sharp Width | number | 160 | 0 .. 4000 | px | live |  | How wide the sharp band is |
| `falloff` Falloff | number | 240 | 0 .. 4000 | px | live |  | How far the blur takes to reach its most |

#### Radial Blur (`radial_blur`)

A zoom blur streaking toward a centre point (an impact cut-in), or a spin blur around it.

Cost C3 · 8-bit F1 · glyph atlas unsafe · live kernel · reach at defaults 0 px · adds light at defaults no

Also found as: radialBlur, zoom blur, spin blur, impact, 集中ブラー, 放射状ぼかし.

Live (kernel): `amount`, `spin`. Presets: `editor/resources/titleart/fx-presets/radial_blur.yaml`.

| Param | Type | Default | Range | Unit | Animation | Shown when | Description |
|---|---|---|---|---|---|---|---|
| `type` Type | enum | zoom | `zoom`, `spin` |  | step |  | Streaks toward the centre, or around it |
| `amount` Zoom Amount | number | 0.25 | 0 .. 0.95 |  | live | `type` is `zoom` | How long the zoom streaks are |
| `spin` Spin Angle | number | 12 | 0 .. 180 | deg | live | `type` is `spin` | How far the spin smears |
| `center` Center | vec2 | vec2 |  | px | rerender |  | The blur's centre, canvas px from the canvas centre |
| `keep_original` Original On Top | bool | true |  |  | step |  | Keeps the sharp layer over its blur |

### Distort

#### Wave Warp (`wave_warp`)

A sine displacement: a linear wave like a flag, circular ripples from a centre, or a turbulent heat-haze field.

Cost C0 · 8-bit F0 · glyph atlas approximate · live kernel · reach at defaults 8 px · adds light at defaults no

Also found as: wave, ripple, flag, heat haze, warp, 波, 揺らぎ.

Live (kernel): `amplitude`, `phase`, `evolution`. Presets: `editor/resources/titleart/fx-presets/wave_warp.yaml`.

| Param | Type | Default | Range | Unit | Animation | Shown when | Description |
|---|---|---|---|---|---|---|---|
| `type` Type | enum | linear | `linear`, `radial`, `turbulent` |  | step |  | The wave's shape |
| `amplitude` Amplitude | number | 8 | 0 .. 200 | px | live |  | How far pixels move |
| `wavelength` Wavelength | number | 140 | 4 .. 3000 | px | rerender |  | The distance between two crests |
| `phase` Phase | number | 0 | 0 .. 1 |  | live |  | Moves the wave along by a fraction of a wavelength (wraps) |
| `angle` Direction | number | 0 | -180 .. 360 | deg | rerender | `type` is `linear` | The direction the wave travels, clockwise |
| `center` Center | vec2 | vec2 |  | px | rerender | `type` is `radial` | The ripples' centre, canvas px from the canvas centre |
| `complexity` Complexity | integer | 3 | 1 .. 6 |  | step | `type` is `turbulent` | Octaves of the turbulent field |
| `evolution` Evolution | number | 0 | 0 .. 100 |  | live | `type` is `turbulent` | Drifts the turbulent field |

#### Motion Trail (`motion_trail`)

A smeared trail of the shape behind the layer, fading and tapering toward its end, or Echo (残像): separate fading copies stepped along the direction.

Cost C4 · 8-bit F0 · glyph atlas unsafe · live kernel · reach at defaults 160 px · adds light at defaults no

Also found as: motionTrail, trail, echo, afterimage, smear, 残像.

Live (kernel): `offset`, `opacity`. Presets: `editor/resources/titleart/fx-presets/motion_trail.yaml`.

| Param | Type | Default | Range | Unit | Animation | Shown when | Description |
|---|---|---|---|---|---|---|---|
| `mode` Mode | enum | smear | `smear`, `echo` |  | step |  | One continuous smear, or separate copies |
| `angle` Angle | number | 180 | -180 .. 360 | deg | rerender |  | The trail's direction, clockwise (180 = to the left) |
| `length` Length | number | 160 | 1 .. 1500 | px | rerender | `mode` is `smear` | How long the smear is |
| `fade` Fade | number | 1.4 | 0 .. 6 |  | rerender | `mode` is `smear` | How quickly the smear fades |
| `taper` Taper | number | 0.25 | 0 .. 0.95 |  | rerender | `mode` is `smear` | How much the smear narrows toward its end |
| `copies` Copies | integer | 4 | 1 .. 12 |  | step | `mode` is `echo` | How many echoes |
| `offset` Copy Offset | number | 70 | 1 .. 600 | px | live | `mode` is `echo` | The distance between two echoes |
| `scale_step` Scale Step | number | 0 | -30 .. 30 |  | rerender | `mode` is `echo` | Each echo grows (+) or shrinks (-) by this percent |
| `decay` Opacity Decay | number | 0.4 | 0 .. 0.95 |  | rerender | `mode` is `echo` | How much each echo fades |
| `echo_blur` Blur Per Copy | number | 0 | 0 .. 40 | px | rerender | `mode` is `echo` | Each echo blurs this much more |
| `opacity` Opacity | number | 0.8 | 0 .. 1 |  | live |  | The trail's opacity |
| `tint` Tint | color | #ff7a00 |  |  | rerender | `mode` is `smear` | The smear's tint |
| `tint_start` Start Color | color | #ff7a00 |  |  | rerender | `mode` is `echo` | The first echo's tint |
| `tint_end` End Color | color | #7a2cff |  |  | rerender | `mode` is `echo` | The last echo's tint |
| `tint_amount` Tint Amount | number | 0.5 | 0 .. 1 |  | rerender |  | How much the tint replaces the layer's colours |

#### Shatter (`shatter`)

Breaks the layer into glass shards, Voronoi cells or triangles and blasts the pieces near the impact outward, turning, falling and fading, each cut edge catching a highlight; Progress animates it from assembled (0) to blown (1).

Cost C3 · 8-bit F0 · glyph atlas unsafe · live kernel · reach at defaults 181.4498 px · adds light at defaults no

Also found as: break, broken glass, shards, explode, smash, ガラス割れ, 破砕.

Live (kernel): `progress`, `strength`, `rotation`, `fall`, `fade`. Presets: `editor/resources/titleart/fx-presets/shatter.yaml`.

| Param | Type | Default | Range | Unit | Animation | Shown when | Description |
|---|---|---|---|---|---|---|---|
| `pattern` Pattern | enum | glass | `glass`, `voronoi`, `triangles` |  | relayout |  | How the layer breaks |
| `pieces` Pieces | integer | 60 | 10 .. 400 |  | relayout |  | About how many pieces |
| `impact` Impact | vec2 | vec2 |  | px | relayout |  | Where it breaks, canvas px from the canvas centre |
| `radius` Radius | number | 300 | 0 .. 4000 | px | relayout |  | How far from the impact the pieces fly |
| `progress` Progress | number | 1 | 0 .. 1 |  | live |  | 0 assembled, 1 blown apart: animate it |
| `strength` Strength | number | 80 | 0 .. 1000 | px | live |  | How far the pieces are thrown |
| `rotation` Rotation | number | 25 | 0 .. 180 | deg | live |  | How much the pieces turn at most |
| `fall` Fall | number | 0 | 0 .. 4000 | px | live |  | How far the pieces drop as they fly |
| `fade` Fade | number | 0 | 0 .. 1 |  | live |  | Pieces fade as they move |
| `gap` Gap | number | 1 | 0 .. 40 | px | relayout |  | The cracks' width |
| `edge_color` Edge Color | color | #ffffff |  |  | rerender, light |  | The cut edges' highlight colour |
| `edge_highlight` Edge Highlight | number | 100 | 0 .. 300 |  | rerender |  | How bright the cut edges are, in percent |
| `lost` Lost | number | 10 | 0 .. 100 |  | relayout |  | The share of pieces near the impact blown away, in percent |
| `cracks_only` Cracks Only | bool | false |  |  | step |  | Keeps every piece in place: only the cracks show |
| `seed` Seed | integer | 1 | 0 .. 999 |  | relayout |  | The break's random layout |

### Stylize

#### Chromatic Aberration (`chromatic_aberration`)

An RGB split: red and blue pushed apart radially from a centre or along a direction, smeared through the spectrum.

Cost C2 · 8-bit F0 · glyph atlas approximate · live kernel · reach at defaults 12 px · adds light at defaults no

Also found as: chromaticAberration, rgb split, color fringe, prism, 色収差.

Live (kernel): `amount`. Presets: `editor/resources/titleart/fx-presets/chromatic_aberration.yaml`.

| Param | Type | Default | Range | Unit | Animation | Shown when | Description |
|---|---|---|---|---|---|---|---|
| `amount` Amount | number | 6 | 0 .. 200 | px | live |  | How far the colours split |
| `type` Type | enum | radial | `radial`, `directional` |  | step |  | Split outward from a centre, or along one direction |
| `angle` Direction | number | 0 | -180 .. 360 | deg | rerender | `type` is `directional` | The split's direction, clockwise |
| `smooth` Spectral Smear | bool | true |  |  | step |  | Smears through the spectrum instead of three hard copies |
| `center` Center | vec2 | vec2 |  | px | rerender | `type` is `radial` | The split's centre, canvas px from the canvas centre |

#### Glitch (`glitch`)

A digital glitch: horizontal slices (or rectangular blocks) of the layer jump sideways, each with its own RGB split.

Cost C1 · 8-bit F0 · glyph atlas unsafe · live kernel · reach at defaults 35 px · adds light at defaults no

Also found as: digital, vhs, datamosh, corrupt, グリッチ, 逆押し.

Live (kernel): `amount`, `seed`. Presets: `editor/resources/titleart/fx-presets/glitch.yaml`.

| Param | Type | Default | Range | Unit | Animation | Shown when | Description |
|---|---|---|---|---|---|---|---|
| `mode` Mode | enum | slices | `slices`, `blocks` |  | step |  | Whole horizontal slices, or rectangular blocks |
| `amount` Amount | number | 0.35 | 0 .. 1 |  | live |  | The share of slices that jump |
| `slices` Slices | integer | 14 | 1 .. 80 |  | step | `mode` is `slices` | How many slices across the layer's height |
| `offset` Max Offset | number | 28 | 0 .. 600 | px | rerender |  | How far a slice jumps at most |
| `split` RGB Split | number | 7 | 0 .. 100 | px | rerender |  | How far the colour channels split |
| `block_size` Block Size | number | 40 | 4 .. 400 | px | rerender | `mode` is `blocks` | The blocks' width |
| `seed` Seed | integer | 1 | 0 .. 999 |  | live |  | The glitch's random pattern; reseed every few frames to animate it |

#### Halftone (`halftone`)

A comic dot screen inside the shape (round, square, diamond, line or cross dots), its dot size from a ramp across the layer, its darkness or uniform; Dissolve breaks the layer itself into a dot screen toward its soft parts.

Cost C1 · 8-bit F0 · glyph atlas approximate · live kernel · reach at defaults 0 px · adds light at defaults no

Also found as: comic dots, ben-day dots, screen tone, dot screen, dissolve, 網点, トーン, スクリーントーン.

Live (kernel): `dot_scale`, `fade`. Presets: `editor/resources/titleart/fx-presets/halftone.yaml`.

| Param | Type | Default | Range | Unit | Animation | Shown when | Description |
|---|---|---|---|---|---|---|---|
| `size` Cell Size | number | 9 | 2 .. 120 | px | rerender |  | The screen's cell size |
| `angle` Screen Angle | number | 45 | -180 .. 360 | deg | rerender |  | Turns the screen |
| `dot` Dot | enum | round | `round`, `square`, `diamond`, `line`, `cross` |  | step |  | The dots' shape |
| `dot_scale` Dot Scale | number | 0.8 | 0 .. 2 |  | live | `source` is `ramp` or `luminance` or `uniform` | Scales every dot |
| `color` Dot Color | color | #8a1c00 |  |  | rerender | `source` is `ramp` or `luminance` or `uniform` | The dots' colour |
| `amount` Amount | number | 0.6 | 0 .. 1 |  | rerender | `source` is `ramp` or `luminance` or `uniform` | How strongly the dots recolour the layer |
| `source` Dot Size From | enum | ramp | `ramp`, `luminance`, `uniform`, `alpha` |  | step |  | What sets each dot's size |
| `fade` Fade | number | 28 | 0 .. 400 | px | live | `source` is `alpha` | How far in from the edge Dissolve reaches |
| `ramp_angle` Ramp Angle | number | 90 | -180 .. 360 | deg | rerender | `source` is `ramp` | The ramp's direction across the layer, clockwise |
| `mode` Blend | enum | multiply | `normal`, `add`, `screen`, `multiply`, `overlay`, `soft_light`, `hard_light`, `darken`, `lighten`, `color_dodge`, `color_burn`, `linear_burn`, `difference`, `exclusion`, `hue`, `saturation`, `color`, `luminosity` |  | step | `source` is `ramp` or `luminance` or `uniform` | How the dots blend over the layer |

#### Scanlines (`scanlines`)

Retro screen lines over the layer: soft scanlines, a CRT with an aperture grille, interlaced lines or LCD sub-pixels.

Cost C0 · 8-bit F0 · glyph atlas approximate · live kernel · reach at defaults 0 px · adds light at defaults no

Also found as: crt, lcd, interlace, retro screen, tv lines, 走査線, スキャンライン.

Live (kernel): `intensity`, `offset`, `seed`. Presets: `editor/resources/titleart/fx-presets/scanlines.yaml`.

| Param | Type | Default | Range | Unit | Animation | Shown when | Description |
|---|---|---|---|---|---|---|---|
| `style` Style | enum | lines | `lines`, `crt`, `interlace`, `lcd` |  | step |  | The screen's look |
| `spacing` Spacing | number | 4 | 1 .. 64 | px | rerender |  | The distance between two lines |
| `thickness` Thickness | number | 0.5 | 0.05 .. 0.95 |  | rerender |  | The share of a period the dark line takes |
| `intensity` Intensity | number | 0.45 | 0 .. 1 |  | live |  | How dark the lines get |
| `angle` Angle | number | 0 | -90 .. 90 | deg | rerender |  | The lines' direction, clockwise (0 = horizontal) |
| `mask_strength` Mask Strength | number | 0.5 | 0 .. 1 |  | rerender | `style` is `crt` or `lcd` | How strong the RGB stripes are |
| `brightness` Brightness | number | 1.15 | 0.5 .. 2 | x | rerender |  | Brightens the layer to make up for the lines |
| `offset` Offset | number | 0 | -4000 .. 4000 | px | live |  | Scrolls the lines |
| `flicker` Flicker | number | 0 | 0 .. 1 |  | rerender |  | Dims the screen by a random amount; reseed to flicker |
| `seed` Seed | integer | 1 | 0 .. 999 |  | live |  | The flicker's random amount |

#### Pixelate (`pixelate`)

Big pixels: the layer averaged into cells drawn as squares, discs, diamonds or LED dots, optionally posterized or snapped to a palette with an ordered dither (retro 8-bit, LED boards, mosaics).

Cost C1 · 8-bit F0 · glyph atlas approximate · live kernel · reach at defaults 8 px · adds light at defaults no

Also found as: pixel, 8-bit, 8bit, mosaic, led board, retro, モザイク, ドット絵.

Live (kernel): `size`. Presets: `editor/resources/titleart/fx-presets/pixelate.yaml`.

| Param | Type | Default | Range | Unit | Animation | Shown when | Description |
|---|---|---|---|---|---|---|---|
| `size` Cell Size | number | 8 | 1 .. 256 | px | live |  | The pixels' size |
| `shape` Shape | enum | square | `square`, `round`, `diamond`, `led` |  | step |  | Each pixel's shape |
| `gap` Gap | number | 0 | 0 .. 0.9 |  | rerender |  | Space between the pixels, a share of a cell |
| `palette` Palette | asset | none |  |  | step |  | A palette image (one colour per pixel, up to 256): each cell snaps to its nearest colour |
| `levels` Levels | integer | 0 | 0 .. 64 |  | step |  | Posterize each channel to this many levels when there is no palette (0 = off) |
| `dither` Dither | enum | none | `none`, `bayer2`, `bayer4`, `bayer8` |  | step |  | An ordered dither before the colours snap |
| `dither_amount` Dither Amount | number | 0.5 | 0 .. 1 |  | rerender |  | How strong the dither is |
| `alpha_mode` Alpha | enum | hard | `hard`, `soft` |  | step |  | Cells are fully in or out, or keep their coverage |
| `alpha_threshold` Alpha Threshold | number | 0.5 | 0 .. 1 |  | rerender | `alpha_mode` is `hard` | The coverage a cell needs to be drawn |
| `align` Align | enum | canvas | `canvas`, `layer` |  | step |  | The grid sits on the canvas, or moves with the layer |

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
| `kind` Pattern | enum | zebra | `zebra`, `tiger`, `giraffe`, `leopard`, `checker`, `hazard`, `stripes`, `dots`, `sakura`, `seigaiha`, `asanoha`, `shippo`, `kikko`, `uroko`, `wood`, `marble`, `granite`, `concrete`, `brushed_metal`, `diamond_plate`, `carbon_fiber`, `rust`, `leather`, `brick`, `camo`, `circuit` |  | step |  | The procedural pattern |
| `color` Color | color | #121212 |  |  | rerender |  | The marks' colour |
| `color2` Color 2 | color | #ffffff |  |  | rerender |  | The secondary colour of the kinds that have one |
| `background` Background | color | #f2efe8 |  |  | rerender |  | The colour between the marks |
| `bg_opacity` Background Opacity | number | 100 | 0 .. 100 |  | rerender |  | The background's opacity, percent |
| `scale` Motif Size | number | 140 | 8 .. 2000 | px | rerender |  | The size of one motif |
| `rotation` Rotation | number | 0 | -180 .. 180 | deg | rerender |  | The pattern's rotation, clockwise |
| `tilt` Perspective Tilt | number | 0 | 0 .. 80 | deg | rerender |  | Lays the pattern on a floor tilted back by this much |
| `seed` Seed | integer | 1 | 0 .. 999 |  | step |  | Reshuffles the random kinds (the document's seed adds to it) |
