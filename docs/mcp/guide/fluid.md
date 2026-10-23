# Fluid simulation

The **Fluid Simulation** component (GameObject > Effects > Fluid Simulation; component
`FluidSimulationComponent`, MCP name `fluidsimulation`, alias `fluid`; Lua/C++ class
`FluidSimulation`) runs a GPU fluid on a GameObject: a port of Pavel Dobryakov's
[WebGL-Fluid-Simulation](https://github.com/PavelDoGreat/WebGL-Fluid-Simulation) (MIT) with his
solver (curl, vorticity confinement, divergence, Jacobi pressure, gradient subtract,
semi-Lagrangian advection), his look (dye-gradient shading, soft-knee HDR bloom, sunrays) and his
defaults. On top of his page: a fixed-step clock (`simulationRate` Hz), a seeded random source
(`seed`, for repeatable runs), strokes spread over a frame's steps, scripted splats and steps,
presets, and three places to show it. Exported games that contain one ship his licence
(`licenses/webgl-fluid-simulation-LICENSE.txt`).

The fluid lives in **fluid coordinates**: (0,0) is the bottom-left of its image, (1,1) the
top-right. Velocities are Pavel's force units (a pointer delta in fluid coordinates x
`splatForce`); colours are HDR dye (his pointer colours are about 0.15, his random bursts about
1.5). The same kernels drive the HTML UI `fluid` effect (`jasper://docs/html-ui-effects`).

## Outputs

`output` says where the image goes (MCP values `screen`, `material`, `texture`; Lua
`FluidOutput.SCREEN` / `MATERIAL` / `TEXTURE`):

| Output | What it draws | Pointer (`interactive`) | Use it for |
|---|---|---|---|
| `material` (default) | the GameObject's own Sprite, UI Image or Mesh (submesh 0's base colour) shows the fluid instead of its texture, lent at draw time (nothing saved, no prefab override) | drags over the Sprite or Image (a Mesh takes splats from scripts) | a fluid panel, a puddle, a magic surface in the world or in the UI |
| `screen` | over the main camera's whole view, after post-processing, at the view's aspect | drags anywhere on the view | backgrounds, title screens, cursor trails; set `transparent` for an overlay (opaque covers the game with `backColor`) |
| `texture` | nothing by itself: the output is a texture other components show through a `fluid\|<entity>` link | none (scripts splat) | one fluid shown in several places, on a mesh material, a UI Image elsewhere |

The image is RGBA8 at the dye's size. Opaque: the dye over `backColor`. `transparent`: no
background, alpha = max(r, g, b), premultiplied (dark dye is see-through).

## Properties

Fields are camelCase everywhere (set_properties, Lua, YAML). "Steps" are the fixed steps of
the clock; cost in fullscreen passes (see Performance).

| Property | Default | Range (Inspector) | What it does | Cost |
|---|---|---|---|---|
| `enabled` | true | | off: neither steps nor draws (state kept) | none while off |
| `output` | material | screen, material, texture | where it is drawn (Outputs) | |
| `interactive` | true | | pointer drags splat (mouse and touches) | |
| `aspectRatio` | 0 | 0-4 (0 auto) | width/height of the fluid; auto: the view (screen), the Sprite/Image (material), else 1 | |
| `simResolution` | 128 | 32, 64, 128, 256 | velocity grid, short side | every solver pass runs on it: the main step cost |
| `dyeResolution` | 1024 | 128, 256, 512, 1024 | dye image (and output), short side: the detail | most of the memory, 1 pass a step + the look |
| `densityDissipation` | 1 | 0-4 | dye fade, value / (1 + k dt) a step; 0 keeps it | |
| `velocityDissipation` | 0.2 | 0-4 | motion fade | |
| `pressure` | 0.8 | 0-1 | the previous pressure kept as the Jacobi warm start | |
| `pressureIterations` | 20 | 1-80 | Jacobi iterations a step: less compressible with more | 1 pass each |
| `curl` | 30 | 0-50 | vorticity confinement: small swirls put back | |
| `gradientScale` | 1 | 0.25-1.5 | gradient subtracted x this: 1 Pavel's (livelier), 0.5 the textbook projection | |
| `simulationRate` | 60 | 15-240 | fixed steps a second (same motion at any frame rate) | steps per second |
| `maxSubsteps` | 4 | 1-16 | steps a frame at most; a slow frame drops the rest | caps a frame |
| `timeScale` | 1 | 0-4 | slow motion / fast forward of the fluid's clock | |
| `paused` | false | | no clock steps; splats still paint, `step()` still steps | 0 passes while nothing changes |
| `splatRadius` | 0.25 | 0.01-1 | splat size (Pavel's SPLAT_RADIUS, used / 100) | |
| `splatForce` | 6000 | 0-20000 | push of a pointer's movement (delta x this) | |
| `colorful` | true | | pointer colours cycle through hues | |
| `colorUpdateSpeed` | 10 | 0-50 | how often they change | |
| `splatsOnStart` | true | | a random burst when it (re)starts | |
| `randomSplatCount` | 0 | 0-64 | a burst's splats (0: random 5-24, Pavel's) | 2 passes a splat |
| `seed` | 0 | | every random choice (bursts, pointer colours); 0: a new seed each start | |
| `shading` | true | | lights the dye by its gradient (relief) | in the display pass |
| `backColor` | [0,0,0] | colour | background (display values, `"#RRGGBB"` accepted) | |
| `transparent` | false | | no background: alpha = max(r, g, b) | |
| `bloom` | true | | soft-knee glow of the bright dye | 2 x levels + 1 passes |
| `bloomIterations` | 8 | 1-8 | blur levels (until the chain reaches 2 texels) | 2 passes a level |
| `bloomResolution` | 256 | 64-1024 | the chain's first level, short side | |
| `bloomIntensity` | 0.8 | 0.1-2 | glow strength | |
| `bloomThreshold` | 0.6 | 0-1 | brightness the glow starts from | |
| `bloomSoftKnee` | 0.7 | 0-1 | 0 hard cut, 1 wide curve | |
| `sunrays` | true | | light shafts from the dye | 4 passes |
| `sunraysResolution` | 196 | 98-512 | the rays' mask, short side | |
| `sunraysWeight` | 1 | 0.3-1 | ray strength | |

Changing `seed`, `splatsOnStart` or `randomSplatCount` in edit mode restarts the fluid; at
runtime a new seed takes effect at the next `reset()`. A new resolution or aspect recreates the
targets at the next frame, resampling the dye and the velocity into them (Pavel's resize). Lua setters refuse NaN,
infinite and negative values (and 0 for resolutions, `simulationRate`, `maxSubsteps`,
`splatRadius`); set_properties checks the types only.

## Presets

A preset sets the solver and look fields only (resolutions, dissipations, pressure,
`pressureIterations`, `curl`, `gradientScale`, `splatRadius`, `splatForce`, `colorful`,
`colorUpdateSpeed`, `shading`, the bloom and sunrays fields); `output`, `transparent`,
`backColor`, `interactive`, `aspectRatio`, the clock, the start burst and `seed` keep their
values. Every preset sets the same fields, so applying one after another gives the same result.
Unlisted fields below are Pavel's defaults.

| Preset | Look | Differs from Pavel's | Cost |
|---|---|---|---|
| `default` | Pavel's page | nothing | 27 passes a step, about 20 look passes, 1024 dye |
| `mobile` | his weak-GPU fallback | `dyeResolution` 512, `shading`/`bloom`/`sunrays` false | 27 a step, 1 look pass, a quarter of the dye memory |
| `smoke` | soft billowing smoke | dye 512, `densityDissipation` 0.4, `velocityDissipation` 0.1, `curl` 20, `splatRadius` 0.5, `splatForce` 4000, `colorful` false, `bloom` false, `sunraysWeight` 0.6 | 27 a step, about 5 look passes |
| `ink` | thin lasting ink strokes | `densityDissipation` 0.15, `velocityDissipation` 0.8, `pressureIterations` 30, `curl` 8, `splatRadius` 0.12, `splatForce` 3000, `colorful` false, `bloom`/`sunrays` false | 37 a step, 1 look pass |
| `calm-water` | gentle settling water | `densityDissipation` 0.5, `velocityDissipation` 0.5, `pressureIterations` 30, `curl` 5, `splatRadius` 0.4, `splatForce` 2500, `colorUpdateSpeed` 2, `bloomIterations` 6, `bloomIntensity` 0.3, `bloomThreshold` 0.7, `sunrays` false | 37 a step, about 14 look passes |
| `psychedelic` | swirling neon | `simResolution` 256, `densityDissipation` 0.5, `velocityDissipation` 0.1, `curl` 50, `splatRadius` 0.3, `splatForce` 8000, `colorUpdateSpeed` 30, `bloomIntensity` 1.4, `bloomThreshold` 0.3, `bloomSoftKnee` 0.9 | 27 a step on 4x the cells, about 20 look passes |

Apply one: set_properties `{"fluid": {"preset": "ink"}}` (edit mode: one undo step; it expands
to the fields above, and other values in the same object override it:
`{"fluid": {"preset": "ink", "curl": 12}}`); the same works in create_entity and add_component
`values`. While playing: eval_lua `object("Fluid", "FluidSimulation"):applyPreset("ink")`. The
Inspector's Preset combo is the same list. get_entity's `fluid_stats.preset` names the preset
the values match (`null`: custom).

## MCP recipes

```json
{"tool": "create_entity", "arguments": {"type": "fluid_simulation", "name": "Fluid",
  "values": {"fluid": {"preset": "calm-water", "seed": 7}}}}
```

`fluid_simulation` makes a Sprite showing the fluid (output `material`): 512 px in a 2D scene,
4 units in a 3D one, placed and scaled like any sprite. For a screen overlay or a texture-only
fluid start from an empty GameObject (no sprite to draw):

```json
{"tool": "create_entity", "arguments": {"type": "empty", "name": "Background Fluid",
  "components": ["fluid"],
  "values": {"fluid": {"output": "screen", "transparent": true, "preset": "smoke", "seed": 3}}}}
```

- Tune: set_properties `{"fluid": {"curl": 12, "backColor": "#101020"}}`; read with
  get_entity (`components: ["fluid"]`); while playing set_runtime_property changes them live.
- On a UI Image: add the component to the Image's GameObject (output `material`).
- A Lua script attached to it can wrap it: `FluidSimulation(self.scene, self.entity)`.
- Runtime state: get_entity and get_runtime_entity return `fluid_stats` (read only): `preset`,
  `supported` (+ `unsupported_reason`), `running`, `steps`, `time`, `splats`, `seed`,
  `sim_size`, `dye_size`, `output_size` ([w, h]), `gpu_bytes`, `passes_last_frame`,
  `shading_active`, `bloom_active`, `sunrays_active` (after fallbacks), `manual_filtering`,
  `compact_formats`, `error`.
- In edit mode the fluid is drawn still (its start burst); the Inspector's preview bar runs it
  and its Paint Dye button lets a human stir it in the Scene view. eval_lua with
  `allow_edit_mode` can call `step(n)` on it to look at a later state (transient, not undoable).

## Lua

```lua
---@class FluidStir : ScriptInstance
local FluidStir = {}

function FluidStir:init()
    self.fluid = FluidSimulation(self.scene, self.entity)   -- the component on this GameObject
    self.fluid.seed = 42                                     -- taken at the next reset()
    self.fluid:applyPreset("ink")
    self.fluid:reset()
    RegisterEngineEvent(self, "onUpdate")
end

function FluidStir:onUpdate()
    if Input.isKeyPressed(Input.KEY_SPACE) then
        -- x, y (0..1, y up), velocity (force units), HDR colour
        self.fluid:splat(0.5, 0.1, 0, 900, 0.1, 0.4, 1.0)
    end
end

return FluidStir
```

`FluidSimulation(self.scene)` (no entity) creates a new GameObject with the component (keep the
wrapper; give it `output = FluidOutput.SCREEN` or a texture link, it has no sprite). Methods:
`splat(x, y, dx, dy, r, g, b)` or `splat(posV2, velV2, colorV3 [, radius])` (applied at the
next frame, also while paused), `randomSplats([count])` (seeded burst; 0 or none: 5-24),
`step([n])` (n fixed steps at the next frame, also while paused), `reset()` (zero fields, the
seed, the start burst), `applyPreset(id)` / `getPreset()`, `FluidSimulation.getPresetIds()`,
`getStats()` (`steps`, `time`, `splats`, `simWidth`, `simHeight`, `dyeWidth`, `dyeHeight`,
`gpuBytes`, `passesLastFrame`, `seed`, `shadingActive`, `bloomActive`, `sunraysActive`),
`getTexture()`; every field above is a read/write property. Full list:
`jasper://docs/lua-api` (FluidSimulation).

## Deterministic verification

A fluid with a non-zero `seed` replays exactly: the same bursts, pointer colours and steps give
the same image on the same machine and backend (half floats round differently across GPUs:
compare loosely there). Wall-clock timing is the only thing to remove.

1. Edit mode: `seed` non-zero; for a fully scripted run also `paused: true` (no clock steps) and
   `interactive: false`; save the scene.
2. `play {"action": "start", "fixed_fps": 60, "auto_pause": true}`: every frame advances
   exactly 1/60 s and the game only runs inside your calls.
3. Drive it from a known state: eval_lua
   `local f = object("Fluid", "FluidSimulation"); f:reset(); f:step(60); return true`
   (reset and steps happen together at the next frame, whenever its shaders are ready).
4. Run until it happened: wait
   `{"until": "object('Fluid', 'FluidSimulation'):getStats().steps >= 60", "timeout_seconds": 60}`.
5. Look: screenshot the Game view (`canvas_rect_in_image` maps image pixels to canvas pixels);
   read get_runtime_entity `fluid_stats` (`steps` 60, `seed`, `splats`).
6. Stop and repeat: the image is byte-identical; another seed gives another image.

With the clock running instead (`paused: false`), `simulationRate` 60 and `fixed_fps` 60 give
one step a frame: wait on `getStats().steps` as above rather than on frames, since the fluid
starts once its shaders are built (the first play compiles them).

Checking a splat lands where you meant (the y axis is up in fluid coordinates, down in screen
pixels): eval_lua `f:splat(0.25, 0.75, 0, 0, 1, 0.3, 0.1)` paints the top-left quarter; on a
`screen` fluid it is at canvas pixel (0.25 W, 0.25 H). With `shading`, `bloom` and `sunrays` off
the pixel shows the dye colour itself.

## Pointer input

An `interactive` fluid takes the mouse (pointer 0) and touches (each its own pointer) as
Pavel's page does: a press on the fluid starts a stroke, moves splat along it (spread over the
frame's steps; one splat a frame while paused), the colour cycles when `colorful`. A drag that
starts outside the fluid does nothing.

- `screen`: anywhere on the view; fluid (x, y) = (canvas x / W, 1 - canvas y / H).
- `material` on a Sprite or Image: through the scene camera onto the quad's plane (any camera
  angle); a stroke starts on the quad and follows the pointer beyond it until released.

Drive it with send_input over the running game (canvas pixels from the top-left):

```json
{"tool": "send_input", "arguments": {"events": [
  {"type": "mouse", "action": "drag", "x": 200, "y": 360, "to": [1080, 360], "steps": 20}],
  "frames_after": 2, "screenshot": true}}
```

`steps` is one move a frame. Touches: `{"type": "touch", "action": "swipe", "x": ..., "y": ...,
"to": [...]}`. With `paused: true` the strokes paint without moving: an exact trail to check.

## Texture links

Any texture slot that takes an image can show a fluid's output: set the slot to
`{"fluid": <entity>}` or the link id `"fluid|<entity id>"`, e.g. set_properties
`{"mesh": {"submeshes[0].material.baseColorTexture": {"fluid": "Fluid"}}}` or
`{"ui": {"texture": {"fluid": "Fluid"}}}`. The fluid is usually output `texture` (any output
works). The link is saved, survives copy/paste, duplicate and prefabs (it follows a copied fluid,
is cleared in another scene) and is bound in exports. Shader texture slots
(`shaderTextures.<uniform>`) take image files only. In C++ `FluidSimulation::getTexture()` is
the output texture (its version changes when it is recreated).

## The HTML UI effect

The same look as one line of CSS on any UI element: `background-image: effect(fluid)` (catalogue:
`jasper://docs/html-ui-effects`, guide: the "Stateful effects" section of `jasper://docs/html-ui`).
Its parameters are this component's in kebab case (`density-dissipation`, `curl`, `splat-radius`,
`splat-force`, `bloom-intensity`, `sunrays-weight`, `seed`, `splats-on-start`, ...) with the same
defaults; `preset` takes the ids of the presets above and gives the parameters you leave at their
defaults the preset's values (the resolutions excepted: the effect keeps a 128 velocity grid, the
element's px for the dye, 256 bloom). It is transparent where there is no dye (the element's
`background` is `backColor`) and paints while the pointer drags over it (`hover on`: any movement).

```css
.title-screen { background: #000; background-image: effect(fluid, preset psychedelic, seed 7); }
```

Deterministic captures without Lua: `set_ui_effect_time {time: 1.0, selector: ".title-screen",
pointer_path: [[x, y, t], ...], splats: [{x, y, dx, dy, radius, color, strength, t}]}` replays a
drag (element CSS px, effect seconds) and splats exactly at the pinned time; the same call twice
gives the same screenshot. `inspect_ui` reports the simulation (`state`: bytes, steps, time,
images, replays, splats, path). Scripts use `el:effectSplat{...}`.

## Performance

Fullscreen passes (Pavel's pipeline; all half-float targets):

- A step: `7 + pressureIterations` passes, all on the velocity grid but one dye advection (27
  by default). `simulationRate` steps a second, at most `maxSubsteps` a frame.
- A splat: 2 passes (pointer strokes: one a step while moving; a burst: 5-24 splats once).
- The look, redrawn only when something changed (a step, a splat, a look field): display 1 +
  bloom `2 x levels + 1` (15 at 256) + sunrays 4, about 20 by default. A paused fluid nobody
  touches costs nothing.
- `fluid_stats.passes_last_frame` is the measured count of the last frame.

Memory (`fluid_stats.gpu_bytes` is exact): 20 bytes a dye texel (two RGBA16F images and the
RGBA8 output), 16 bytes a grid cell (RG16F velocity pair, R16F pressure pair, divergence, curl;
48 without RG16F/R16F), bloom about 13 x `bloomResolution`^2 bytes, sunrays 4 x
`sunraysResolution`^2. Short side x aspect is the long side:

| | square (1:1 sprite) | 16:9 screen |
|---|---|---|
| dye 1024 | 20 MiB | 35.5 MiB |
| dye 512 | 5 MiB | 8.9 MiB |
| dye 256 | 1.25 MiB | 2.2 MiB |
| dye 128 | 0.3 MiB | 0.6 MiB |
| grid 128 | 0.25 MiB | 0.45 MiB |
| grid 256 | 1 MiB | 1.8 MiB |

Mobile: start from the `mobile` preset (dye 512, display pass only); on low-end GPUs also
`simResolution` 64, `pressureIterations` 10-20 and `simulationRate` 30 (half the solver passes a
second; motion stays the same speed). A small `material` fluid needs a small dye (256 for a
sprite a quarter of the screen). Several fluids each pay their own passes: share one through
texture links. Pause (`paused`) or disable fluids that are off screen.

## Troubleshooting

- **Black / empty image**: nothing splatted yet (`splatsOnStart` false and no pointer or script
  splats), the dye faded (`densityDissipation` high), or `transparent` over a black scene.
  `fluid_stats.splats` counts splats since the last reset.
- **Nothing drawn at all**: `fluid_stats.running` false: the component is disabled, the
  GameObject inactive, the shaders still compiling (the first play), or `error` says why.
  `supported: false`: the device cannot render to half-float (RGBA16F) targets (a WebGL 2 or
  GLES 3 device without a float colour-buffer extension); nothing can run there.
- **No shading, bloom or sunrays on a device**: `manual_filtering: true`: half floats are not
  filterable there, so the advection filters by hand and the look passes that need filtering
  are skipped (`*_active` false). `compact_formats: false`: RG16F/R16F are missing and RGBA16F
  is used (3x the grid memory).
- **The game is hidden**: an opaque `screen` fluid covers the whole view with `backColor`; set
  `transparent: true`.
- **A sprite shows white**: output `screen` or `texture` on a GameObject made by
  create_entity `fluid_simulation` leaves its untextured Sprite; remove the Sprite or use
  output `material`.
- **The pointer does nothing**: `interactive` false, output `texture` or a Mesh, a press that
  started outside the fluid, or the game not running (edit mode: the Inspector's Paint Dye).
- **Runs differ**: `seed` 0 (a new seed each start), the wall clock (play without `fixed_fps`),
  or waiting on frames while the shaders build (wait on `getStats().steps`).
- **Splat in the wrong place**: fluid y is up; screen pixels are y down
  (fluid y = 1 - canvas y / H).
