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
| T2, the chain | bloom, blur | **7 small passes** from a quarter of the resolution (about 0.17 of a screen) plus the uber pass that composites them |
| T3, custom passes | `post_process` passes (library `@shader postprocess` shaders or your forks) | **1 pass each** at the scene's resolution, before the uber pass |

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
- Avoid stacking custom `post_process` passes: each one is a full pass. Use FXAA or MSAA,
  not both.
- Measure: run with `JASPERENGINE_FRAME_STATS=1` (editor or exported game) and read the
  "Frame stats ... pass list:" lines in the log.

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
