# Atmosphere: height fog, volumetric fog and volumetric clouds

The scene's atmosphere is three features that share one medium and one lighting model, so they
agree where they meet (design record: `docs/specs/S12-atmosphere.md`):

- **Height fog**: a Fog of Type **Height**. Physically based: dense up to its Base Height,
  thinning above it, lit by its colour and by the main directional light. It works in
  perspective and orthographic cameras and covers the sky near the horizon (Sky Fill).
- **Volumetric fog**: an option of the height fog. Near the camera the same medium is
  raymarched with the sun's shadows (light shafts), Local Volumetric Fog volumes, a density
  noise and, at High quality, point and spot lights. Past its Distance the analytic height fog
  continues, so the two meet without a seam.
- **Volumetric clouds**: a Volumetric Clouds component; raymarched clouds lit by the sun and the
  sky, in a Sky Map (every camera) or per pixel on screen (the main camera).

Everything costs nothing when it is off: no Fog or a distance fog runs no atmosphere pass, a
height fog without Sky Fill runs none, no clouds component runs no cloud pass, no texture and no
noise bake. Everything works on every backend (GL, GLES3/WebGL2, Metal, D3D11, Vulkan) with
fragment shaders only: no compute.

## Height fog (Fog, Type Height)

| Inspector | Field | Meaning |
|---|---|---|
| Color Mode | `colorMode` | Constant Color: the fog takes its Color. Sky Color: the sky's colour at the horizon, from a cube map Skybox (4 directions, the Skybox's colour and rotation applied) or a sky shader with a `horizonColor` property; any other sky uses the Color and the Inspector says so |
| Color | `color` | linear colour |
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

Presets (Inspector > Preset, MCP `apply_atmosphere_preset`, Lua `fog:applyPreset(id)`) make the
Fog a height fog of that look and keep its Color Mode and volumetric settings:
`morning_haze`, `ground_mist`, `mountain_valley`, `horizon_haze`, `night`.

## Volumetric fog (Fog > Volumetric Fog)

| Inspector | Field | Meaning |
|---|---|---|
| Enabled | `volumetric` | the main camera raymarches the fog near it (Atmosphere Quality Low and up) |
| Distance | `volumetricDistance` | how far it marches (m); the analytic fog continues beyond |
| Light Shafts | `lightShafts` | the main directional light's shadows darken the fog (it must cast shadows) |
| Noise Intensity, Noise Scale, Wind | `noiseIntensity`, `noiseScale`, `wind` | a drifting 3D density noise, faded out over the last fifth of the Distance |
| Quality Override | `qualityOverride` | caps this fog's tier below the project's (never raises it) |

The Inspector's last line says what runs, e.g. "Running: Medium · 960×540 · 32 steps · RGBA16F
· depth pre-pass" (the pre-pass is drawn when neither SSAO, SSR nor a post pass made the depth).

How it renders: the scene's depth is reduced to the fog's resolution (the farthest of each block),
the march runs per low-resolution texel with steps spread quadratically and offset by a static
64×64 blue noise (no frame index: a still scene renders the same image every frame), a 3×3
depth-aware denoise removes the pattern, and a joint bilateral upsample composites it over the
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

Point and spot lights with **Volumetric Intensity** above 0 scatter in the volumetric fog at High
quality (the 4 nearest, without shadows).

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
| Local lights / volumes | 0 / 2 | 0 / 4 | 4 / 8 |
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

- `create_entity` types `fog`, `local_volumetric_fog`, `volumetric_clouds`; every field above is
  a `set_properties` property (enums by name: `type: "height"`, `rendering: "sky_map"`), one undo
  step per call.
- `list_atmosphere_presets {kind: "fog" | "clouds"}`, `apply_atmosphere_preset {entity, preset}`
  (one undo step).
- `get_atmosphere_info {view: "game" | "scene"}`: the tier and platform default, the fog's type
  and volumetric target (size, steps, format, volumes used and culled, local lights, whether it
  forced a depth pre-pass), the clouds' mode, noise state and Sky Map, the `Atmosphere/*` passes
  of the last frame and their VRAM.
- `configure_game_view {atmosphere_quality}`, the Scene view's `disable_fog` and
  `disable_clouds` display settings (`set_scene_camera display`), and the project settings
  `quality.atmosphere_quality_desktop`, `_mobile`, `_web`.

## Limits

- The fog and cloud composites blend in the engine's display space, as transparent surfaces do:
  exact where the fog is opaque or absent, an approximation in between.
- Sky pixels behind volumetric fog take the marched fog in front of them on top of the sky fill.
- Transparent surfaces take the analytic fog: no light shafts or volumes on them.
- Volumetric fog and Screen clouds draw for the main camera only; other cameras get the analytic
  fog, the sky fill and the Sky Map clouds.
