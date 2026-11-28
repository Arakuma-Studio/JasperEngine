# Atmosphere: height fog, volumetric fog and volumetric clouds

The scene's atmosphere is three features that share one medium and one lighting model, so they
agree where they meet (design record: `docs/specs/S12-atmosphere.md`):

- **Height fog**: a Fog of Type **Height**. Physically based: dense up to its Base Height,
  thinning above it, lit by its colour and by the main directional light. It works in
  perspective and orthographic cameras and covers the sky near the horizon (Sky Fill).
- **Volumetric fog**: an option of the height fog. Near the camera the same medium is
  raymarched with the sun's shadows (light shafts), Local Volumetric Fog volumes, a density
  noise and point and spot lights, shadowed when they cast shadows. Past its Distance the analytic height fog
  continues, so the two meet without a seam.
- **Fog Profiles and Fog Zones**: a fog look as an asset shared by every Fog linked to it, and
  places (a cave) that blend their own look in as the camera comes near.
- **Volumetric clouds**: a Volumetric Clouds component; raymarched clouds lit by the sun and the
  sky, in a Sky Map (every camera) or per pixel on screen (the main camera).

Everything costs nothing when it is off: no Fog or a distance fog runs no atmosphere pass, a
height fog without Sky Fill runs none, no clouds component runs no cloud pass, no texture and no
noise bake. Everything works on every backend (GL, GLES3/WebGL2, Metal, D3D11, Vulkan) with
fragment shaders only: no compute.

## Height fog (Fog, Type Height)

| Inspector | Field | Meaning |
|---|---|---|
| Color Mode | `colorMode` | Constant Color: the fog takes its Color. Sky Color: the sky's colour at the horizon, from a cube map Skybox (4 directions, the Skybox's colour and rotation applied) or a sky shader with a `horizonColor` property; any other sky uses the Color and the Inspector says so. Lit: see below |
| Color | `color` | linear colour (not in Lit) |
| Albedo, Ambient Multiplier, Emissive | `albedo`, `ambientMultiplier`, `emissive` | Lit only (below) |
| Attenuation Distance | `attenuationDistance` | metres light travels below the Base Height before 63% is absorbed (extinction = 1 / distance) |
| Base Height | `baseHeight` | uniform density below this height (m) |
| Maximum Height | `maximumHeight` | the height where the density has fallen to 1/1000 |
| Start Distance | `startDistance` | no fog nearer than this (m) |
| Max Fog Distance | `maxFogDistance` | no more fog past this (m) |
| Max Opacity | `maxOpacity` | the most the fog hides (0..1) |
| Directional Inscattering > Intensity | `directionalInscattering` | how much of the main directional light the fog scatters to the camera |
| Directional Inscattering > Anisotropy | `anisotropy` | -1 back towards the sun, 0 even, 1 forward (a glow around the sun) |
| Sky > Sky Fill | `skyFill` | the sky takes the fog's colour near the horizon (one Sky Composite pass over the sky's pixels; no sky shader changes) |
| Sky > Sky Fill Angle | `skyFillAngle` | degrees above the horizon the fill reaches |

The light of the medium is `L = fog colour + sun colour × Intensity × 4π HG(Anisotropy, cos θ) ×
visibility` (visibility is the shadow term in the volumetric fog, 1 elsewhere). A part of a ray
lets `T = exp(-optical depth)` through and adds `S = L (1 - T)`.

### Lit colour mode

With Color Mode **Lit** (`colorMode: "lit"`) the fog has no colour of its own: it scatters the
scene's light, so one look holds from noon to night as the sun and the ambient change. Its light
is `albedo × (sun colour × Intensity × 4π HG × visibility + scene ambient × ambientMultiplier) +
emissive`; the scene ambient is the Lighting's ambient colour × intensity, as meshes get it.

- `albedo` (linear, default white): the share of the light the fog scatters, per channel. Dark
  values make a murky fog; a tint colours the fog only, never the Local Volumetric Fog volumes
  inside it (each scatters by its own albedo).
- `ambientMultiplier` (default 1): the ambient's weight; 0 lights the fog by the sun alone.
- `emissive` (linear HDR, default black): light the fog gives off whatever lights it (a
  glowing, toxic or infernal fog). It is the only thing that shows with no sun and no ambient.

Use Lit for a scene whose lighting changes (day-night cycles, a sun that sets) or a fog that must
match its lighting; use Constant Color for a fixed look. The analytic fog, the sky fill and the
volumetric fog all take it.

## Fog Profiles

A Fog Profile (`.fogprofile`) is a fog look shared by every Fog and Fog Zone linked to it. Each
look field is either set by the profile (ticked) or left to the Fog's own value, as in Unity's
Volume Profiles. The look fields are `color`, `density`, `linearStart`, `linearEnd`, `colorMode`,
`attenuationDistance`, `baseHeight`, `maximumHeight`, `startDistance`, `maxFogDistance`,
`maxOpacity`, `directionalInscattering`, `anisotropy`, `skyFill`, `skyFillAngle`,
`noiseIntensity`, `noiseScale`, `wind`, `albedo`, `ambientMultiplier` and `emissive`. Fields that
set cost or technique (`type`, `volumetric`, `volumetricDistance`, `lightShafts`,
`qualityOverride`) are never in a profile: they stay on each Fog.

The built-ins are `morning_haze`, `ground_mist`, `mountain_valley`, `horizon_haze`, `night` (the
old presets' looks) and the Lit looks `bright_haze`, `murky`, `infernal`, `toxic`. Linking one
copies it to `FogProfiles/<id>.fogprofile` in the assets folder the first time, in the same undo step; later
links reuse that copy. Linking a profile that sets height fog fields makes a distance fog a Height
fog in the same step.

The file is JSON: `{"version": 1, "name", "description", "fields": {<field>: <value>}}`, colours as
linear `[r, g, b]`, `colorMode` as `"color"`, `"sky"` or `"lit"`. A missing or unreadable profile
counts as an empty one (`profile_missing`).

Editing a profile changes every Fog and Fog Zone using it, in every scene. To change one scene
only, duplicate it (`edit_fog_profile {path: <new>, from: {profile: <path>}}`) and link the copy
there.

## Fog Zones

A Fog Zone (GameObject > Effects > Fog Zone; `create_entity {type: "fog_zone"}`) is a place with a
look of its own: a cave, a swamp, a building. It blends its profile's ticked fields over the
scene's Fog as the camera comes near: weight × clamp(1 − d / Blend Distance, 0, 1), d the camera's
distance to its shape (0 inside). The scene still needs one Fog; zones change it, they draw nothing
themselves and cost nothing on the GPU.

| Field | Meaning |
|---|---|
| `shape`, `size` | a box or a sphere, before the Transform's scale; the Transform places and turns it |
| `blendDistance` | how far outside the shape the look fades in (0: a hard switch at its surface) |
| `priority` | zones apply in ascending priority over the Fog (ties by entity id): the higher wins |
| `weight` | how much of the look applies inside (0..1) |
| `profile` | the Fog Profile whose ticked fields it blends in; none: no effect |

The look of a view, field by field: the Fog's own fields, then its profile, then a scripted blend,
then the zones by priority, then Lua and animation writes in Play. The Game view and the game take
the main camera's position, the Scene view its own camera's.

## Blending at run time (Lua)

`fog:blendTo(profile, seconds)` cross-fades the look to another profile over game time (`nil`:
back to the Fog's own fields), from wherever it is now; `fog.blending` is true meanwhile.
`fog:getResolved(field)` reads the look the main camera sees. Profiles from `FogProfile.load(path)`
and `FogProfile.builtin(id)` are read-only; `profile:clone()` makes a copy the script may `set`
and `clear`, never saved. A script's write to a Fog field in Play shows over the profile and the
zones, and goes at Stop (`fog:clearOverride(field)` and `fog:clearOverrides()` drop it sooner).
`fog:applyPreset(id)` still writes the Fog's own fields. Details: `jasper://docs/lua-api`.

## Volumetric fog (Fog > Volumetric Fog)

| Inspector | Field | Meaning |
|---|---|---|
| Enabled | `volumetric` | the main camera raymarches the fog near it (Atmosphere Quality Low and up) |
| Distance | `volumetricDistance` | how far it marches (m); the analytic fog continues beyond |
| Light Shafts | `lightShafts` | shadows darken the fog: the main directional light's and the fog lights' (each must cast shadows) |
| Noise Intensity, Noise Scale, Wind | `noiseIntensity`, `noiseScale`, `wind` | a drifting 3D density noise, faded out over the last fifth of the Distance |
| Quality Override | `qualityOverride` | caps this fog's tier below the project's (never raises it) |

The Inspector's last line says what runs, e.g. "Running: Medium · 960×540 · 32 steps · RGBA16F
· depth pre-pass" (the pre-pass is drawn when neither SSAO, SSR nor a post pass made the depth).

How it renders: the scene's depth is reduced to the fog's resolution (the farthest of each block),
the march runs per low-resolution texel with steps spread quadratically and offset by a static
64×64 blue noise (no frame index: a still scene renders the same image every frame), a
depth-aware denoise (5×5 at Low, 3×3 at Medium and High) removes the pattern, and a joint bilateral upsample composites it over the
opaque scene. Every surface takes the analytic fog over its whole ray; the composite then puts
the marched fog in place of the analytic fog's near part (up to the Distance), so where the two
are the same medium (no shadow, volume or noise) the image is exactly the analytic fog's, and
nothing is fogged twice. Transparent surfaces keep the analytic fog (no light shafts or volumes on
them), and refractive shaders see the fogged scene.

### Local Volumetric Fog

A box or sphere of extra fog (GameObject > Effects > Local Volumetric Fog), placed, turned and
scaled by its Transform: `shape`, `size`, `attenuationDistance`, `albedo` (its colour),
`blendDistance` (the density fades in over it inside the edge), `falloff` (Linear,
Exponential), `noiseIntensity`, `noiseScale`, `scrollSpeed`. The Scene view shows the outer and
inner (blend) shape of a selected volume. It shows only through volumetric fog (the Inspector
warns otherwise). Per frame the volumes in view and within the Distance are sorted by distance
and the nearest are used: 2 at Low, 4 at Medium, 8 at High.

Point and spot lights with Intensity and **Volumetric Intensity** above 0 scatter in the volumetric
fog: those whose range reaches the view and comes within the Distance, the nearest first, 1 at
Low, 2 at Medium, 4 at High. With Light Shafts on, a light with Shadows on casts them in the fog
(one shadow tap per step), as the sun does; the sun takes two taps per step at Medium and High
(softer shafts). A light set to Volumetric Intensity 0 costs nothing.

## Volumetric clouds

A Volumetric Clouds component (GameObject > Effects > Volumetric Clouds; a scene draws its first
one). Presets: Custom, Sparse, Cloudy, Overcast, Stormy (the Preset menu applies the look; Custom
leaves the fields). Fields: `rendering`, `coverage`, `density`, `shapeScale`, `shapeFactor`,
`erosionFactor`, `erosionScale`, `bottomAltitude`, `altitudeRange`, `earthCurvature`,
`windDirection`, `windSpeed`, `weatherMap` (R coverage, G cloud type; empty: a built-in tiling
map), `weatherMapScale` (km), `sunLightDimmer`, `ambientLightDimmer`, `maxDistance` (km),
`qualityOverride`.

The density is a weather map plus baked 3D noise (Perlin-Worley shapes 64³, Worley detail 32³,
baked once per process on the job system: ≤250 ms, never blocking the main thread; time-sliced on
web builds without threads; the clouds fade in over 0.5 s when it is ready). The light is the sun
(Beer's law with a powder term, a dual-lobe phase, three multiple-scattering octaves; it fades
out when the sun sets) and the scene's ambient, from the cloud base to the top. The height fog
lies in front of the clouds (aerial perspective).

Rendering:

- **Sky Map** (Automatic at Low and Medium): a map of the sky's directions marched from the
  camera, refreshed a slice per frame (8 frames at Low, 4 at Medium) and crossfaded; it is only
  re-marched when something changed. Every camera sees it (mirrors, render textures, reflection
  probes), behind all geometry. Seen from below: the clouds fade out over 200 m as the camera
  rises to 100 m below their base. Flight games use Screen.
- **Screen** (Automatic at High): a quarter-resolution march per pixel, stopped at the scene's
  surfaces, so clouds can hide far mountains. The main camera only (other cameras use the Sky
  Map).

## Quality

Project Settings > Quality > Atmosphere Quality has a row per platform group: Desktop (default
High; also the editor's views), Mobile (Low) and Web (Low). A game overrides it with
`Engine.setAtmosphereQuality(AtmosphereQuality.MEDIUM)` (Lua) or
`Engine::setAtmosphereQuality` (C++); the Game view's toolbar previews a tier (Project, Off, Low,
Medium, High). Off turns volumetric fog and clouds off; the height fog and the sky fill are never
tiered. Components cap it downward with Quality Override.

| | Low | Medium | High |
|---|---|---|---|
| Volumetric fog resolution / steps | 1/4 per axis, 16 | 1/2, 32 | 1/2, 64 |
| Local lights / volumes | 1 / 2 | 2 / 4 | 4 / 8 |
| Sun shadow taps per step / denoise | 1 / 5×5 | 2 / 3×3 | 2 / 3×3 |
| Clouds (Automatic) | Sky Map 256², 8 frames, 32 + 3 steps, no erosion | Sky Map 512², 4 frames, 64 + 4 steps | Screen ¼ resolution, 96 + 6 steps |

Low-resolution targets are RGBA16F where the device renders and filters them, else RGBA8 with the
scattering range-compressed (WebGL2 without `EXT_color_buffer_float`).

## Budgets

GPU time per feature (1920×1080; mobile at 1280×720) and memory at 1080p, as the reference
hardware must meet them (mid desktop: GTX 1660 / RX 5500 class; mobile: Adreno 650 / Mali-G78
class):

| Feature | Desktop | Mobile |
|---|---|---|
| Height fog over a linear fog | ≤ 0.10 ms | ≤ 0.3 ms |
| Sky Composite | ≤ 0.15 ms | |
| Volumetric fog (downsample, march, denoise, composite) | Medium ≤ 1.0 ms, High ≤ 1.8 ms | Low ≤ 1.5 ms |
| Clouds | Medium ≤ 0.8 ms, High ≤ 2.5 ms | Low ≤ 0.6 ms |
| VRAM (all atmosphere targets and noise) | Medium ≤ 16 MB, High ≤ 24 MB | Low ≤ 4 MB |
| CPU main thread (volumes, uniforms) | ≤ 0.1 ms | |

The depth pre-pass the volumetric fog draws when nothing else made the depth is reported apart.
`tools/performance/atmosphere_bench.py` measures them (300 frames of
`tools/fixtures/atmosphere/bench.scene`; `--gpu` times each `Atmosphere/*` pass with S15's GPU
timers at exactly 1920x1080). Measured on an RTX 3080 Ti (GL, docs/perf/gpu-campaign.md): height
fog 0.003 ms, Sky Composite 0.008, volumetric Low 0.094 / Medium 0.391 / High 0.761, clouds Low
0.067 / Medium 0.184 / High 0.307: within every budget (the reference-class card is 4-5x slower).

### Measured on the benchmark server E (2026-10-25)

E (4 dedicated vCPU, nothing else running) renders in software (Mesa llvmpipe), so its
milliseconds say nothing about a GPU. Method (`atmosphere_bench.py --runs 3 --relative`): each
configuration's frame time (median of 300 frames, Game view 904×509, no vsync), median of 3 runs
in new editors (runs within 1%); a feature's cost is its frame minus its baseline's, taken as a
share of the frame without any atmosphere (248.8 ms), and compared with the budget's share of a
60 fps frame (budget / 16.67 ms: the reference GPU is taken to draw the scene without atmosphere
in that frame). "Sky Composites" is the cost in units of one trivial full-screen pass on E. VRAM is
scaled to 1080p (view-sized targets only). Raw data:
`tools/performance/results/atmosphere_bench_E_2026-10-25.json`.

| Feature | Cost on E | Share | Budget share | Sky Composites | VRAM @1080p | Result |
|---|---|---|---|---|---|---|
| Height fog (over linear) | 0.85 ms | 0.3% | 0.6% (desktop), 1.8% (mobile) | 0.6 | 1.4 MB | within |
| Sky Composite | 1.46 ms | 0.6% | 0.9% | 1.0 | 1.4 MB | within |
| Volumetric fog Low | 45.1 ms | 18.1% | 9.0% (mobile) | 31 | 3.4 MB (≤ 4) | over 2.0× |
| Volumetric fog Medium | 153.6 ms | 61.7% | 6.0% | 105 | 11.8 MB (≤ 16) | over 10× |
| Volumetric fog High | 268.2 ms | 107.8% | 10.8% | 184 | 11.8 MB (≤ 24) | over 10× |
| Clouds Low (Sky Map) | 7.9 ms | 3.2% | 3.6% (mobile) | 5.4 | 2.9 MB (≤ 4) | within |
| Clouds Medium (Sky Map) | 107.5 ms | 43.2% | 4.8% | 74 | 7.4 MB (≤ 16) | over 9× |
| Clouds High (Screen) | 57.7 ms | 23.2% | 15.0% | 40 | 3.9 MB (≤ 24) | over 1.5× |

Every VRAM budget holds. By this method the marches (volumetric fog at every tier, clouds Medium
and High) exceed their time budgets. The method weighs per-pixel work against a frame whose cost
on a CPU rasteriser is mostly elsewhere, and a software 3D-texture fetch costs many times a
GPU's, so it overstates fragment-bound passes; it does not prove a GPU budget either way. AC10's
GPU budgets still need the reference-GPU run with S15's pass timings.

## Agents

- `create_entity` types `fog`, `fog_zone`, `local_volumetric_fog`, `volumetric_clouds`; every field
  above is a `set_properties` property (enums by name: `type: "height"`, `rendering: "sky_map"`),
  one undo step per call.
- `list_atmosphere_presets {kind: "fog"}`: the built-in profiles (id, name, description, number of
  fields set, users, the project's copy) and the project's profiles (path, the same).
  `kind: "clouds"`: the Volumetric Clouds presets.
- `apply_atmosphere_preset {entity, preset}` links a Fog or a Fog Zone to a profile (a built-in id
  or a `.fogprofile` path) or sets a clouds preset; `unlink: true` turns a Fog's profile into its
  own fields (the look stays) and clears the link. One undo step. When the profile has other users,
  `next` says how to change them all or this one only.
- `edit_fog_profile {path, from?, set?, clear?}` (group `rendering_fx`) edits a profile as one undo
  step: `from` (`{builtin}`, `{entity}`: a Fog's look or a zone's profile, `{profile}`) creates the
  file, `set` writes fields as the file holds them and ticks them (also `name`, `description`),
  `clear` unticks. It returns the fields, the users and the open scene's resolved look. Refused in
  Play.
- `set_properties` on a Fog field its profile sets is refused, naming both fixes: `edit_fog_profile
  {set}` for every user, or a copy for this scene only. Other fields write as before.
- `get_atmosphere_info {view: "game" | "scene", resolved?}`: the tier and platform default, the
  fog's type, `profile` and volumetric target (size, steps, format, volumes used and culled, local
  lights, whether it forced a depth pre-pass), `zones` (entity, priority, the weight at the view's
  camera, distance, profile), `blend` (target, progress) in Play, the clouds' mode, noise state and
  Sky Map, the `Atmosphere/*` passes of the last frame and their VRAM, the `diagnostics` and the
  `opacity` estimate (below). `resolved: true` adds each look field's value and the layer it comes
  from: `fog`, `profile`, `blend`, `zone:<entity>` or `runtime`.
- `configure_game_view {atmosphere_quality}`, the Scene view's `disable_fog` and
  `disable_clouds` display settings (`set_scene_camera display`), and the project settings
  `quality.atmosphere_quality_desktop`, `_mobile`, `_web`.

## Diagnostics

When fog does not show, looks too thin, hides everything or ignores a volume or a light, call
`get_atmosphere_info` before changing the look. Its `diagnostics` list each cause found:
`{code, severity, message, entity?, fix}` (`entity`: the GameObject it is about; severity `info`,
`warning` or `error`), the same text as the Inspector's help boxes on Fog, Local Volumetric Fog and
Light. Apply the fix of each `warning`, by the route below (`next` names the first one's), then
call it again. `info` items explain a choice (a light past the tier's cap, a volume out of view)
and need nothing. The checks and the estimate read the look the main camera sees (its profile,
zones and blend included).

| Code | Seen when | Route |
|---|---|---|
| `fog_missing` | Local Volumetric Fog or Fog Zones in a scene with no Fog | `create_entity {type: "fog"}`; for volumes also `set_properties` fog `type: "height"`, `volumetric: true` |
| `fog_second_ignored` | a second Fog (on `entity`): only the first is used | `delete_entities` one of them |
| `profile_missing` | the Fog's or a Fog Zone's (on `entity`) profile file is missing or does not read: the Fog shows its own values, the zone nothing | restore the file, or `apply_atmosphere_preset` an existing profile |
| `profile_no_effect` | the profile sets only fields the Fog's Type ignores (height fog fields on a distance fog, or the reverse) | `set_properties` fog `type`, or another profile |
| `zone_no_effect` | a Fog Zone (on `entity`) with no profile, a profile that sets nothing, or Weight 0 | `apply_atmosphere_preset` the zone a profile, `edit_fog_profile {set}`, or `set_properties` fogzone `weight` |
| `volumetric_needs_height` | Volumetric on a distance fog, or volumes with the volumetric fog off | `set_properties` fog `type: "height"`, `volumetric: true` |
| `volumetric_quality_off` | the view's Atmosphere Quality is Off; the message names the setting | that setting: `configure_game_view {atmosphere_quality: "project"}`, `quality.atmosphere_quality_desktop` (`_mobile`, `_web`), the script's `Engine.setAtmosphereQuality`, or the device tier's Volumetrics cell |
| `volumetric_main_camera_only` | the view, or a render-to-texture camera, is not the main camera | read `view: "game"`; the volumetric fog draws for the main camera only |
| `shafts_off` | Light Shafts off while the sun casts shadows | `set_properties` fog `lightShafts: true` |
| `shafts_sun_no_shadows` | Light Shafts on, but no sun shadow map was drawn | `set_properties` the named light `shadows: true` |
| `light_not_scattered` | a point or spot light (on `entity`) the fog left out: intensity, Volumetric Intensity, distance, out of view, past the tier's cap | its `volumetricIntensity`, the Fog's `volumetricDistance`, or the quality |
| `volume_culled` | a Local Volumetric Fog (on `entity`) left out: distance, out of view, past the tier's cap | the Fog's `volumetricDistance`, or the quality |
| `scene_view_fog_disabled` | the Scene view's Disable fog display setting is on | `set_scene_camera {display: {disable_fog: false}}` |
| `sky_mode_no_horizon` | Color Mode Sky with a sky that has no horizon colour | `set_properties` fog `colorMode: "color"`, or a cube map Skybox |
| `lit_no_light` | Color Mode Lit with no sun, no ambient light and no emissive: black fog | a directional light, or `set_scene_settings` `global_illumination_intensity` |
| `fog_invisible` | `opacity.at200` below 0.02 | Height: lower `attenuationDistance`; Linear: lower `linearEnd`; Exponential: raise `density` |
| `fog_opaque` | `opacity.at10` above 0.95 | Height: raise `attenuationDistance`; Linear: raise `linearStart`; Exponential: lower `density` |

`opacity {at10, at50, at200, includes: "analytic"}` is the share (0..1) of the scene the fog hides
10, 50 and 200 m along the main camera's forward ray: the analytic fog exactly (the volumetric
fog's medium without shadows), not Local Volumetric Fog or the noise. It is `null` without a Fog
or a main camera. Compare it after a change instead of a screenshot.

## Limits

- The fog and cloud composites blend in the engine's display space, as transparent surfaces do:
  exact where the fog is opaque or absent, an approximation in between.
- Sky pixels behind volumetric fog take the marched fog in front of them on top of the sky fill.
- Transparent surfaces take the analytic fog: no light shafts or volumes on them.
- Volumetric fog and Screen clouds draw for the main camera only; other cameras get the analytic
  fog, the sky fill and the Sky Map clouds.
