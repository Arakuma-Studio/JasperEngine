# Post-processing

The engine post-processes the image of the scene's **main camera** (tone mapping, grading,
vignette, FXAA, bloom, CRT...). It is a scene setting, `post_processing`, edited in the
Inspector (nothing selected: scene settings > Post-processing) or with `set_scene_settings`.
Other cameras (render to texture, mirrors), layer scenes (a HUD added with `set_child_scene` /
`Engine.addSceneLayer`) and UI scenes are never post-processed. The Game view always shows it;
the Scene view shows it only with its display toggle (the wand button left of Gizmos, or
`set_scene_camera` `display: {show_post_processing: true}`; saved per scene in the workspace).

## Cost tiers

Every effect is off by default and costs nothing while off. What an effect costs depends on
its tier, not on its maths:

| Tier | Effects | Cost |
|---|---|---|
| T0, in the shaders | exposure, tone mapping (`tonemap`: none, aces, neutral, reinhard, agx), colour grading (white balance, contrast, saturation, lift/gamma/gain, LUT strip), monochrome (grayscale/sepia), vignette, dither | **0 passes**: one LUT tap at the end of every scene shader (baked on the CPU when a value changes) |
| T1, the uber pass | FXAA, chromatic aberration, pixelate, CRT, edge detect, film grain, sharpen, `render_scale` below 1 | **1 fullscreen pass** shared by all of them; with fixed resolution it is the upscale blit drawn anyway (no pass added). With it on, the grade, vignette and dither move into it (still no pass of their own) |
| T2, the chain | bloom, blur | small passes from a quarter of the resolution plus the uber pass that composites them: bloom **7** (about 0.17 of a screen), blur **1 + 2 x ceil(`blur_radius`)** (3 at radius 1, 5 to radius 2, 7 above) |
| T3, custom passes | `post_process` passes (library `@shader postprocess` shaders or your forks) | **1 pass each** at the scene's resolution times the pass's `scale` squared, before the uber pass. Without the uber pass an exported game ends the chain with a blit to the screen (with fixed resolution, a copy after a last pass below full size) |

MSAA (`msaa_samples` 2 or 4) is part of the scene pass: the samples resolve at its end (in tile
memory on tile-based GPUs) into the offscreen target the effects read. In an exported game with
nothing else offscreen (no uber pass, custom pass or fixed resolution) the scene resolves into a
target blitted to the screen: **1 more full-resolution pass**. FXAA and MSAA can both be on.

Reference set (tonemap + grade + vignette + chromatic aberration + bloom + grain + FXAA at
1080p, an estimate from the renderers' source, not a measurement): 8 passes, 1 at full
resolution, about 1.17 screens shaded here; Unity URP 14 runs 19 passes, 2 at full resolution,
about 2.77 screens for the same effects.

## Presets

`list_post_processing_presets` lists them with the fields each sets and their estimated cost;
`apply_post_processing_preset {preset}` replaces the whole `post_processing` of the scene as
one undo step (the Inspector's Preset menu does the same). Tune afterwards with
`set_scene_settings {"post_processing.<field>": value}`.

| Preset | What it sets | Estimated passes (URP) |
|---|---|---|
| `none` | everything off | 0 (0) |
| `mobile` | neutral tone mapping, light grade with a 16^3 LUT, FXAA, faint bloom | 8 (19) |
| `console` | ACES, grade, vignette, dither, FXAA, bloom, subtle chromatic aberration and grain | 8 (19) |
| `stylised` | pixelate, punchy posterized-looking grade, vignette | 1 (3) |
| `crt_retro` | CRT (scanlines, mask, curvature), pixelate, chromatic aberration, grain, warm grade | 1 (4) |

The list's `current` is the scene's preset (`null` once a field differs: custom), its settings
and the estimated cost readout of the Inspector: `{engine, unity_urp: {passes,
full_resolution_passes, screens, pass_list}, tiers: {in_shader, uber_pass, bloom_chain},
scene_shading, notes}`; while playing also `applied` (what the renderer runs: `in_shader`,
`uber_pass`, `uber_features`).

## Mobile advice

- Prefer T0: tone mapping, grading, monochrome, vignette and dither cost no pass at all.
- Lower `render_scale` (0.5-1) before anything else on high-density screens: the 3D scene
  shades `render_scale^2` of the pixels and the uber pass upscales it; add `sharpen` if it
  looks soft. A HUD in a layer scene stays at full resolution.
- One T1 effect turns the uber pass on; more T1 effects then cost only their few taps.
- Bloom is cheap here (quarter resolution); keep `bloom_intensity` modest.
- Avoid stacking custom `post_process` passes: each one is a full pass (a `scale` below 1
  makes it cheaper). One of FXAA or MSAA is usually enough.
- Measure: run with `JASPERENGINE_READY_SIGNAL=1 JASPERENGINE_FRAME_STATS=1` (editor or
  exported game) and read the "Frame stats ... pass list:" lines in the log: frames 2, 30 and
  90 after everything loaded (`JASPERENGINE_FRAME_STATS=all`: every frame; in the editor each
  extra view, e.g. the Game view beside the Scene view, logs its own "view N" line).

## Limits

Design limits of this stack, each a trade for mobile cost:

- 8-bit scene (LDR). Exposure and the tone mapping curve run in every shader before the
  8-bit write (the scene image cannot hold highlights for a later curve), so transparent
  objects blend after the curve, in display values (as Godot's mobile renderer and Unreal's
  mobile LDR mode). Bloom reads that image too: a full-white surface above `bloom_threshold`
  glows like a light.
- UI. UI components the main camera draws are part of its image: the uber pass and
  `render_scale` reach them (as fixed resolution and custom passes always did); UI shaders
  never take the in-shader colour. Put a HUD that must stay sharp and untouched in a layer
  scene. Point sprites keep their size in pixels of the scaled image.
- Frosted panels. The blurred scene (`blur_enabled`, `u_blurredSceneTexture`) exists for
  layer scenes drawn after the post-processed scene; in that scene itself it is black.
- MSAA applies to the main camera's scene pass only: not to mirrors, other render-to-texture
  cameras, layer scenes or the edit-mode Scene view without its post-processing toggle.
  Changing `msaa_samples` reloads the objects. On GLES it resolves with a blit (no
  `EXT_multisampled_render_to_texture`); a device without 2x (lavapipe) runs without MSAA.
- Chromatic aberration with FXAA: where the shift is under a pixel every channel stays
  antialiased; a stronger fringe at the screen edges is a shifted, not re-antialiased, copy.
- Colour LUT size: `neutral` at `lut_size` 16 is up to about 3/255 off its curve in the
  shoulder; use 32 where that shows.
- A painted terrain (PBR layers) with any in-shader colour effect leaves its base material's
  emissive texture out (16-texture budget; the emissive factor still applies).
- Runtime switches: an uber feature or chain a script turns on that the export did not
  precompile builds its variant at runtime; the previous image is kept meanwhile.
- The editor's cost readout is an estimate; the measured pass list is in the log (above).

## Fields

Every field is a setting `post_processing.<field>` (get_scene_settings `detail=true` gives its
type, default, range and options); the `post_processing` object lists the fields away from
their defaults, setting an object changes the fields it names, `null` resets all. Colours are
sRGB `[r,g,b]` or `"#RRGGBB"`; `lut_texture` is a project-relative image path (an N*N x N
strip, Unity's layout).

- Colour: `exposure` (stops), `tonemap`, `color_grading_enabled`, `temperature`, `tint`,
  `contrast`, `saturation`, `lift`, `gamma`, `gain`, `lut_texture`, `lut_contribution`,
  `lut_size` (16 or 32), `monochrome_enabled`, `monochrome_mode` (grayscale, sepia),
  `monochrome_intensity`, `dither_enabled`.
- Lens: `vignette_enabled`, `vignette_intensity`, `vignette_smoothness`, `vignette_roundness`,
  `vignette_color`, `chromatic_aberration_enabled`, `chromatic_aberration_intensity`,
  `film_grain_enabled`, `film_grain_intensity`, `film_grain_response`.
- Anti-aliasing: `fxaa_enabled`, `msaa_samples` (1, 2, 4).
- Stylised: `pixelate_enabled`, `pixelate_size`, `crt_enabled`, `crt_scanlines`, `crt_mask`,
  `crt_curvature`, `crt_vignette`, `edge_detect_enabled`, `edge_detect_threshold`,
  `edge_detect_intensity`, `edge_detect_color`, `sharpen_enabled`, `sharpen_intensity`.
- Bloom and blur: `bloom_enabled`, `bloom_threshold`, `bloom_knee`, `bloom_intensity`,
  `bloom_tint`, `bloom_scatter`, `blur_enabled`, `blur_radius`, `blur_intensity`.
- Resolution: `render_scale` (0.5-1).

## Custom passes

`post_process` (a scene setting, a list) runs fullscreen passes on the scene image before the
uber pass, in order. A pass is a library post shader (`library:crt`, `pixelate`, `posterize`,
`edge_neon`, `gameboy`, `blur`: `list_shaders type=postprocess`) or a fork
(`fork_shader target=post_process`). Each item: `shader`, `enabled`, `uniforms` (the
manifest's `@property` names; unset ones take its defaults), `textures` ({uniform: path}),
`scale` (0.1-1: the pass's output resolution). A shader may also declare
`u_originalSceneTexture` to read the scene before the chain.

```json
{"post_process": [{"shader": "library:pixelate", "uniforms": {"pixelSize": 6}, "scale": 0.5},
                  {"shader": "library:crt", "enabled": false}]}
```

From Lua, `scene.postProcessPasses` is the list:

```lua
local pass = PostProcessPass()
pass.shader = "library:pixelate"
pass.scale = 0.5
pass:setUniform("pixelSize", 4.0)
scene.postProcessPasses = { pass }
```

`pass:setTexture(uniform, path)` / `getTexture(uniform)` set a texture the pass's shader
declares; `scene:setPostProcessUniform(index, name, value)` and
`setPostProcessPassEnabled(index, on)` change a pass of the running chain.

## Lua

`scene.postProcessing` is a copy: edit it, then assign it back (fields in camelCase).

```lua
local p = scene.postProcessing          -- self.scene in a script
p.tonemap = TonemapMode.ACES            -- NONE, ACES, NEUTRAL, REINHARD, AGX
p.vignetteEnabled = true
p.vignetteIntensity = 0.4
p.monochromeMode = MonochromeMode.SEPIA -- GRAYSCALE, SEPIA
p.fxaaEnabled = true
scene.postProcessing = p
scene.postProcessing = PostProcessingSettings()   -- everything off
```

Turning a T1/T2 effect on at runtime switches the camera to the uber pass at once; an export
compiles the variants the saved scene uses (and both colour states when a script mentions
`postProcessing`), others compile when first used.
