# Phase 6: stateful UI effects (per-element simulations)

Binding design: `docs/html-ui/DESIGN.md`. Agent rules: `docs/html-ui/BRIEF.md`. This spec extends
phase 5 (`phase-5-effects.md`): everything not said here is phase 5's. Where it changes DESIGN.md it
says so (3.15).

## 1 Outcome

A game UI can use GPU simulations that remember their last frame, with the same one-line syntax as
every other effect:

```css
.hero    { background: #06070b; background-image: effect(fog, turbulence 1.4); }
.canvas  { background-image: effect(ink-flow, color-mode rainbow); }        /* drag to paint */
.title   { filter: effect(time-trail, trail-length 0.8s); }                 /* moving text leaves a trail */
.flock   { background-image: effect(boids, count 800, cursor-mode repel); }
.card    { filter: effect(pixel-sort, radius 0.3); }
```

The library gains 18 effects ported from shaders.com's stateful simulations: fluids (`fog`,
`fluid-smoke`, `smoke-fill`, `smoke-flow`, `ink-flow`, `fluid-cursor-trail`), particles and agents
(`boids`, `magnetic-filings`, `particles`, `particle-field`, `particle-flow`), grid simulations
(`reaction-diffusion`, `pixel-sort`, `pixel-throw`, `chroma-flow`, `shatter`) and content feedback
(`data-mosh`, `time-trail`).

Each effect instance owns persistent GPU state images, advanced by fragment passes on a fixed
step of the effect clock, so the effects work on every backend including GLES3/WebGL2, which has
no compute shaders. A pinned clock gives byte-identical frames. Scripts can inject into a
simulation (`element:effectSplat{...}`). The editor, `check`, the exporter, the catalogue and
`inspect_ui` know stateful effects, and project effects can declare state too.

## 2 Scope

### In
- Manifest directives for persistent state and its passes (`@state`, `@init`, `@step`,
  `@warmup`, `@rate`, `@replay`) and the cost class `stateful`.
- Two new shader forms per draw form: the **step form** (a fullscreen pass into a state image) and
  the **scatter form** (one quad per texel of a placement image, for particles). The shader
  contract: the include `lib/jfx_state.glsl`, `lib/main_step.glsl`, the vertex stages
  `effect_step.vert` and `effect_scatter.vert`.
- Float render targets in the render abstraction (`ColorFormat::RGBA16F`, `RGBA32F`, `R32F`)
  with capability queries.
- The runtime: per-element simulation state that survives re-instancing, an image pool, passes
  run at the element's draw, culling and pausing, the fixed-step clock with a substep cap, the
  pinned-clock replay, pointer history, splats, budgets and diagnostics, statistics.
- C++/Lua API (`UIElement::effectSplat`, `effectReset`, the state fields of `getEffects`), MCP
  (`inspect_ui` state fields; splats through `eval_lua`).
- Shader variants: `uieffect/<name>:Stp`, `:Sct` (with `Flt`), `uieffect/all`, collection and
  export, `check` for project effects with state, `ui-effects --json|--validate`.
- 18 effects (3.13) with manifests, goldens at two fixed times on glcore and Vulkan, catalogue
  entries, thumbnails and the MIT attribution.
- Tests (section 4), docs (guide, catalogue, CONTRACTS, DESIGN, support matrix).

### Out (with reason)
- **Compute shaders.** GLES3/WebGL2 have none; every pass is a fragment pass, and particles are
  scattered with vertexless quads (3.4).
- **Atomics and fixed-point accumulators.** The upstream agent renders accumulate energy with
  `atomicAdd` into `u32` buffers. Fragment passes accumulate with additive blending into
  `rgba16f` images instead (3.13, "agents"): the same sums, without the 1/255 quantisation.
- **Upstream idle gates** (a settled field stops dispatching). A visible stateful effect steps
  every frame it is drawn; an element that is not drawn does not step (3.5). Documented cost
  guidance replaces the gate.
- **Shape props of the shape effects** (`SmokeFill`, `Particles`: `shape`, `shapeSdfUrl`,
  `shapeType`, `center`, `scale`, `rotation`, `origin`). As in phase 5 (§2 "Out", shape-driven
  effects), the shape is the element's rounded box (`smoke-fill`), and the 3D swarm's container is
  the default preset, a sphere of radius 0.35 (`particles`).
- **Upstream device tiers** (`resolveRenderRes`, `countCap` for phones): one resolution and count
  per effect, documented per effect. Authors pick a lower `count` on mobile.
- **Optional child layers** (`ReactionDiffusion`'s child drive) other than the element's own
  content: in filter form a stateful effect reads the content (3.9), which is how the child drive is
  ported.

## 3 Detailed design

### 3.1 Vocabulary
- **Simulation**: the state of one effect instance on one element: its state images, its step
  count and simulation time, its pointer tracking and pending splats. It lives in the element's
  effect state (phase 5 §3.6 key `(element, name, ordinal)`), so re-instancing (a `var()` change, a
  transition) keeps it.
- **State image**: a named persistent render target (`@state`). Ping-pong pairs unless every pass
  writing it blends (3.3).
- **Pass**: a fragment pass writing one state image (`@init` once, `@step` every step).
- **Step**: one fixed time step of the simulation: every `@step` pass in declaration order,
  repeated as declared.
- **Draw**: the effect's paint or filter pass (phase 5), which reads the current state images.
- **Replay**: with a pinned clock, the state at time `t` is rebuilt from scratch (3.6).

### 3.2 Manifest grammar (additions to phase 5 §3.9)
```
// @state   <name> <format> <size> [linear|nearest] [clamp|repeat] [clear|resample]
//          <format> := rgba8 | rgba16f | rgba32f | r32f
//          <size>   := <w>x<h> | box[*<scale>] [max <px>]
// @init    <pass> <state>                                  (once, after allocation and every reset)
// @step    <pass> <state> [x<count>|x<param>] [add] [scatter <state>]
// @warmup  <steps> <dt-seconds>                            (silent steps after @init; default none)
// @rate    <hz> [max <substeps>]                           (default 60 Hz, max 4)
// @replay  <seconds>                                       (pinned-clock history window; default 2)
// @reseed  <param> [<param>...]                            (a change of these restarts the simulation)
// @cost    stateful                                        (required with @state; forbidden without)
```
- **Formats.** `rgba8` (unsigned normalised), `rgba16f` (half float, filterable everywhere),
  `rgba32f` and `r32f` (full float: particle positions, Gray-Scott chemicals, sort offsets). The
  32-bit formats are not filterable on GLES3/WebGL2 without `OES_texture_float_linear`: they must
  be `nearest` (manifest error otherwise), and the compiler annotates their texture and sampler as
  unfilterable (`@image_sample_type`, `@sampler_type`, phase 0's annotation mechanism).
- **Sizes.** `<w>x<h>` is fixed (the upstream grids: `256x256` fluids, `512x512` Gray-Scott, agent
  tables). `box` follows the effect area: CSS px × scale, rounded, the larger side at
  most `max` (default `max 1024`; both sides scaled down together, the aspect kept), each side at
  least 1. An empty area (0 px on a side) allocates nothing and draws
  nothing.
- **Filter and wrap** apply to the state's sampler (default `linear clamp`).
- **Resize policy** (`box` states, 3.7): `resample` (default: the old image is redrawn into the new
  size with its filter) or `clear` (the simulation restarts from `@init`).
- **Passes.** A pass name is `[a-z][a-z0-9-]*`, unique; the compiler defines
  `JFX_PASS_<NAME>` (upper case, `-` as `_`) = its index (the `@init` and `@step` lines in
  declaration order, as written), and `JFX_PASSES` = their count.
  `x<count>` repeats a step pass (1 to 64); `x<param>` repeats it by an integer or number
  parameter's value, rounded and clamped to [1, 64] (Gray-Scott's `speed`). `add` blends the pass
  additively into a cleared target (accumulators). `scatter <state>` draws one quad per texel of
  that placement state instead of a fullscreen triangle (3.4).
- **Validation** (`EffectManifestParser::validate`, `HTMLUI-FX-0014`): every pass targets a
  declared state; a `scatter` placement state is declared and written by an earlier pass; at most
  8 states, 32 passes; `@step` needs at least one `@state`; `x<param>` names a number or integer
  parameter; `@warmup` needs `@init`; `@rate` 1-240 Hz, `max` 1-16; `@replay` 0-30 s; a 32-bit
  state is `nearest`; `@cost stateful` exactly when there is a `@state`.

Example (the fluid family, 3.13):
```glsl
// @state   vel rgba16f 256x256 linear
// @state   pd rgba16f 256x256 nearest
// @state   dye rgba16f 256x256 linear
// @step    splat-vel vel
// @step    splat-dye dye
// @step    curl vel
// @step    vorticity vel
// @step    divergence pd
// @step    jacobi pd x10
// @step    project vel
// @step    advect-vel vel
// @step    advect-dye dye
```

### 3.3 State images and the pool
- Each state of a simulation is one image, or a ping-pong pair: a pair unless every pass writing
  the state is `add` (an accumulator, cleared and summed every step). A pass reads every state's
  **current** image and writes the target's other image, which then becomes current. While a pass
  writes a single-image state, that state's sampler reads a black placeholder (no feedback loop).
- New images are cleared to transparent black. `@init` passes then seed them.
- **Pool** (`EffectStatePool`, one per process, on the main thread). Images are keyed by (format,
  width, height, filter, wrap). A released image goes back to the pool and is reused by the next
  allocation of its key; pooled images idle for 120 frames, or beyond 64 MB pooled, are destroyed
  (`trim(frames)`; tests call `trim(0)`). `releaseGpuResources` (view loss) destroys everything.
- **Memory** is reported per instance (`stateBytes`: the images' texels × bytes per texel,
  counting pairs twice) in `EffectStats`, `UIElement::getEffects` and `inspect_ui`.
- **Capabilities.** At allocation the formats are checked against `RenderFeatures`
  (`render<Format>`, `blend<Format>` for `add` targets; sokol `sg_query_pixelformat`), through
  `EffectStatePool::supports`. An unsupported format gives `HTMLUI-FX-0017
  effect-state-unsupported` once per simulation and the effect draws nothing (paint) or leaves the
  content (filter).

### 3.4 Forms, variants and passes
Phase 5 had one variant per effect per draw form (paint, `Flt`). A stateful effect adds a step
variant per draw form it is used in, and a scatter variant when it has `scatter` passes:

| Property bit | Name | Vertex stage | Defines |
|---|---|---|---|
| 0 | `Flt` (filter form) | phase 5 | `JFX_FILTER_FORM` |
| 1 | `Stp` (step form) | `effects/effect_step.vert` (fullscreen, vertexless) | `JFX_STEP_FORM`, `JFX_PASS_*` |
| 2 | `Sct` (scatter form) | `effects/effect_scatter.vert` | `JFX_STEP_FORM`, `JFX_SCATTER_FORM`, `JFX_PASS_*` |

- `Stp`/`Sct` combine with `Flt`: the step variant of a filter-form use is `Flt|Stp`, so steps of a
  filter use can read the element's content (`jfx_src`, 3.9). A paint use needs paint + `Stp`
  (+ `Sct`); a filter use `Flt` + `Flt|Stp` (+ `Flt|Sct`). Shader strings: `uieffect_fx_fog_Stp`,
  `uieffect_fx_time_trail_FltStp`; specs `uieffect/fog:Stp`, `uieffect/time-trail:Flt,Stp`.
  `uieffect/all` adds every built-in effect's step and scatter variants.
- One step variant runs every pass of the effect: the pass index is a uniform
  (`step.info.x`), compared with the `JFX_PASS_*` constants in the effect's
  `vec4 jfx_step(JfxStep s)`. Branching on a uniform is uniform control flow on every backend, and
  it keeps "one variant per form" (no variant per pass).
- **Step pass**: a fullscreen triangle into the target image; `gl_FragCoord.xy` is the texel
  (`s.cell`), in memory rows: texel `(x, y)` is row `y` of the image on every backend, as read by
  `texelFetch` and by `texture()` at `(x + 0.5, y + 0.5) / size`. Blending off (or `ONE, ONE` for
  `add`); the target is not cleared unless `add`.
- **Scatter pass**: `draw(0, 6 × W × H)` of the placement image (W × H texels); vertex `v` reads
  texel `v / 6` of the placement state with `texelFetch` in the vertex stage: `xy` the centre in
  target texels, `zw` the half extents (a non-positive `z` skips the quad). The fragment runs
  `vec4 jfx_scatter(JfxStep s, JfxPlaced a)` with the step context and the agent (`a.index`,
  `a.cell` its placement texel, `a.place`, `a.offset` from its centre); the pass blends additively (`add` is implied). This is the classic GPGPU particle
  pipeline without instancing (the effect pipeline has no instance buffers): it needs vertex
  texture fetch, which GLES3 guarantees (16 units).
- **Binding.** Every pass and the draw bind each state's current image as `u_state_<name>` (with
  sampler `u_state_<name>_smp`); missing names are skipped (reflection, as phase 5). Uniform blocks
  of the step forms: `u_fs_effectFrame` (phase 5, with the layer maps for filter uses),
  `u_fs_effectParams`, `u_fs_effectStep` (3.9); the scatter vertex stage has
  `u_vs_effectScatter`.

### 3.5 When the passes run
- **At the element's draw**, immediately before its paint draw (`EffectRenderer::renderShader`) or
  its filter pass (`applyFilter`). Paint draws happen inside a layer pass: the renderer ends it
  (without a resolve), runs the passes into the state images and binds the layer again with
  `LOAD` (colour and the shared stencil are stored, so the clip mask survives). Filter passes run
  inside `CompositeLayers`, where the content is the filter chain's primary target.
  - Why not before the context renders (the starting design): a filter-form simulation reads the
    element's rendered content, which exists only inside RmlUi's layer composite; and running at
    the draw gives the pause rule for free. The cost is one extra layer pass break per stateful
    paint draw (measured, 5.4).
- **Culling and pausing.** RmlUi draws elements whatever their position (`Element::Render` has no
  viewport test). Before the passes, the area's corners are projected with the draw's transform; if
  their bounds miss the current scissor and target, the instance is **clipped out**: no passes, no
  draw. Elements with `display: none`, `visibility: hidden`, off-screen, scrolled or
  clipped out, or in a document that is hidden, are not drawn and therefore do not step. A paused
  simulation keeps its images and step count; when it is drawn again it continues from where it
  stopped, without catching up the time it missed (3.6).
- An instance steps at most once per frame (several draws of one simulation in a frame, e.g. a
  context rendered twice, step once).

### 3.6 Clock, fixed step and determinism
- **Simulation time.** The simulation advances on the effect clock as the effect sees it:
  `T = clock × time-scale + time-offset` (phase 5 §3.4). The step is `h = 1 / @rate` seconds
  (default 1/60). `s.dt` is `h` (an effect with its own `speed` parameter scales it, as upstream).
  A negative or zero `time-scale` freezes the simulation.
- **Live clock.** Each frame the instance wants `n = floor((T − epoch) / h) − steps` steps
  (`epoch` anchors step 0). On the first frame, after a pause (it was not drawn the frame before),
  or when `T` went backwards, the epoch is re-anchored so `n = 0` would hold, then one step runs if
  a whole step elapsed since; no catch-up. **Substep cap**: at most `max` steps a frame (`@rate ...
  max`, default 4); the excess is dropped by moving the epoch (the simulation slows down below
  `rate / max` frames per second instead of spiralling). With phase 1's fixed frame delta (the
  fixture clock, `setFixedDeltatime`), the step sequence is the same in every run: byte-identical
  frames on one backend.
- **Pinned clock** (`EffectClock::setFixedTime`, `UIDocument.setEffectTime`,
  `set_ui_effect_time`). The state at `T` is defined, not accumulated: **reset, `@init`,
  `@warmup`, then the steps of `[max(0, T − @replay), T]`**, `N = floor(min(T, @replay) / h)` steps
  with times `T − (N − k) h`. The replay runs when the pinned time, the instance's area size, its
  parameters (a re-instance with other values), its pending input or the pointer changes, else
  the images are left alone: two frames at one pinned time are identical, whatever came before,
  and a test may render the same `T` in any order. During a replay the pointer at step time `t` is
  the recorded pointer history (3.8) at age `T − t` (the golden harness's synthetic trail moves the
  pointer across the replay); splats injected while pinned are logged with time `T` and replayed at
  the last step of `[.., T]`, so `set_ui_effect_time(t); splat; set_ui_effect_time(t + 0.5)` shows
  half a second of their evolution.
- **Seeds.** Every random choice is a hash of the effect's `seed` parameter (when it has one), the
  element's seed (phase 5 `frame.misc.z`) and the agent or cell index. Upstream's
  `Math.random()` start times (Fog) become such hashes.
- **Cross-backend.** The same goldens serve glcore and Vulkan with phase 0's tolerance; passes use
  `texelFetch` (no filtering differences) except where an upstream pass samples bilinearly
  (advection), and the replay windows are kept short enough that the two drivers' rounding does not
  diverge visibly (5.2 records the measured differences).

### 3.7 Lifecycle
| Event | What happens |
|---|---|
| First draw | Images allocated from the pool, cleared, `@init` passes, `@warmup` (silent steps), then the clock rule of 3.6 |
| Re-instancing (`var()`, transition, keyframes) | Nothing: the simulation belongs to the element state |
| Resize of the area (`box` states) | Live clock: `resample` states are redrawn into images of the new size, `clear` states restart the simulation; fixed states keep their images. Pinned clock: a replay at the new size (deterministic) |
| Parameters change | Live: the passes read the new values next step. Pinned: replay |
| Element removed, document unloaded | `EffectRuntime::OnElementDestroy` drops the element's states; their images return to the pool |
| The effect changes (another effect, or `effect()` removed) | The old key's state has no live instance: at the next frame start the runtime releases its images (the CPU state stays in the element's 16-entry LRU); a new effect gets a new simulation |
| Hidden / clipped / off-screen | Paused (3.5), images kept |
| Editor play / stop | Play loads its own documents (new elements, new simulations); stop destroys them; the edit-mode documents keep theirs |
| Hot reload | A full reload creates new elements (new simulations); a stylesheet-only reload keeps elements and simulations, except that an effect whose manifest changed (its states or passes, a project effect's `.frag`) restarts (the simulation stores its layout signature) |
| Device lost / view recreated | `releaseGpuResources` destroys every image; each simulation restarts at its next draw (`@init`, then the clock rule; a pinned clock replays) |
| `UIElement::effectReset(name)` | The named (or every) simulation of the element restarts |

### 3.8 Inputs
- **Per-step pointer.** For each step the runtime computes the pointer in the effect area's local
  CSS px: live, the frame's pointer movement is spread over the frame's steps (linear
  interpolation between the previous and the current position); pinned, the history. The step
  block carries the position, the previous step's position, the raw velocity (px/s), whether the
  pointer is inside the area, the primary button, and press/release edges. Smoothing, teleport
  guards and minimum drags are the effect's (upstream's trackers are per effect): an effect keeps
  them in a 1×1 state image (`ptr`, 3.13), so they are part of the simulation and replay with it.
- **History.** Every stateful effect records the pointer (context px, 32 samples, 4 s, newest
  first; phase 5's trail rule) whatever its `@pointer`; the golden harness's `setTestTrail`
  replaces it.
- **Splats.** `element:effectSplat{ x=, y=, radius=, dx=, dy=, color=, strength=, effect= }` (Lua)
  and `UIElement::effectSplat(const UIEffectSplat&)` (C++) queue an injection on the element's
  stateful effects (all of them, or the one named `effect`); `x`, `y`, `radius` in the area's CSS
  px (default radius 24 px), `dx`, `dy` a velocity in px/s, `color` any CSS colour (linear in the
  shader, default white), `strength` default 1. At most 8 are queued per simulation (the oldest
  dropped); the next step consumes them (`s.splats`), each effect applying them as its own
  emitter does (dye and momentum for fluids, a kick for agents, reagent for Gray-Scott, a push for
  the displacement grids). Returns the number of simulations that took it. `effectReset(name?)`
  restarts them.
- **MCP.** No new tool (the tool budget is unchanged): `eval_lua` runs `effectSplat` in play;
  `inspect_ui` reports the state (3.10); `set_ui_effect_time` pins the clock as in phase 5.

### 3.9 Shader contract
`lib/jfx_state.glsl` (included by every stateful effect, after `jfx_common.glsl`):
```glsl
#define JFX_STATE(name)   // declares u_state_<name> + sampler and:
                          //   vec4 name##_fetch(ivec2 texel)   (clamped to the image)
                          //   vec4 name##_sample(vec2 uv)      (uv 0..1 over the image, its filter)
                          //   vec2 name##_size()
uniform u_fs_effectStep {             // step and scatter forms only
    vec4 target;      // xy target texels, zw 1/size
    vec4 info;        // x pass index, y iteration (repeated passes), z steps since reset, w 1 during @warmup
    vec4 clock;       // x simulation time s, y dt s, z step index in this frame, w steps this frame
    vec4 pointer;     // xy area-local CSS px, z inside 0/1, w primary down 0/1
    vec4 pointerPrev; // xy previous step's position, z 1 when the pointer is known, w press edge (1 on the step of a press)
    vec4 pointerVel;  // xy raw velocity px/s, z release edge, w 0
    vec4 splatInfo;   // x splat count (0..8)
    vec4 splatPos[8]; // xy CSS px, z radius px, w strength
    vec4 splatVel[8]; // xy px/s
    vec4 splatColor[8]; // linear RGBA
} jstep;
struct JfxStep {
    int pass, iteration; ivec2 cell; vec2 uv; vec2 size;   // the target texel, its uv, the target size
    float dt, time, steps; bool warm; JfxContext c;       // c: the area context at this texel (uv = s.uv)
    vec2 pointer, pointerPrev, pointerVelocity; bool pointerInside, pointerKnown, pointerDown, pressed, released;
    int splatCount;
};
JfxStep jfx_step_context();
JfxSplat jfx_splat(int i);           // {vec2 pos; float radius; float strength; vec2 vel; vec4 color}
JfxPlaced jfx_placed();              // scatter form: {int index; ivec2 cell; vec4 place; vec2 offset}
```
- `main_step.glsl` (included last by a stateful effect, after its `main_paint`/`main_filter`
  include, which it disables in the step forms) writes `jfx_step(s)` (step form) or
  `jfx_scatter(s)` (scatter form) as is: state values are raw floats, not colours.
- In a filter use, `jfx_src(localPx)` reads the content (the phase-5 layer map of the frame
  block) in the step forms too.
- The draw reads the states through the same `JFX_STATE` helpers; `jfx_paint`/`jfx_filter` are
  unchanged.
- The family includes `lib/fam_sim_*.glsl` hold the shared simulation kernels (the Stable-Fluids
  passes, the agent shapes and splat windows, the colour ramps), ported line for line from the
  scaffolds (3.13).

### 3.10 Cost, budgets, diagnostics, statistics
- **Cost class `stateful`.** A stateful effect costs per frame what its passes cost (mostly
  independent of the element's size for fixed grids) plus its draw. `EffectPerfTest.CostTable`
  measures, per stateful effect, one frame of passes and the draw of a 512×512 instance against the
  1024×1024 fill (phase 5's interleaved method), and gates it against the effect's measured budget
  ×1.5 (the table in 3.13 lists the measured cost; the budget is fixed in the test).
- **Budgets.** At most 8 active stateful simulations per document and 32 in the process
  (soft: `HTMLUI-FX-0018 effect-stateful-budget`, warning, once per document while over, naming the
  count and the memory); 64 MB of state images per document (soft, same code). Hard cap: 64
  simulations in the process: beyond it a new simulation does not start (`HTMLUI-FX-0018` as an
  error) and the effect draws nothing until one ends. "Active" = stepped this frame or the frame
  before. `EffectRuntime::setStatefulBudget(perDocument, global, bytesPerDocument)` changes the
  soft budgets. `check --ui` reports `HTMLUI-FX-0016 effect-cost` (info) when a document uses more
  than 4 stateful effects (phase 5's rule for `high`, extended).
- **Diagnostics** (appended to area `FX`):

| Code | Severity | When |
|---|---|---|
| `HTMLUI-FX-0017` `effect-state-unsupported` | error | a state format is not renderable (or not blendable for `add`) on this device; the effect is not drawn |
| `HTMLUI-FX-0018` `effect-stateful-budget` | warning (soft), error (hard cap) | too many active stateful simulations, or too much state memory |

- **Statistics** (`EffectStats`): `statefulActive`, `stateImages`, `stateBytes`, `stepPasses`,
  `scatterPasses`, `stepsRun`, `replays`, `stateImagesCreated` (pool misses), `statePoolBytes`.
- **`UIElement::getEffects`** entries gain `stateful`, `stateBytes`, `steps`, `simTime`,
  `paused` (Lua: same names); `inspect_ui` prints them in the effect entry (`state: {bytes, steps,
  time, paused}`) and adds `~` to the `fx:<name>` outline token of a paused simulation.

### 3.11 Project effects and `check`
A project `.frag` may declare state like a built-in. `check` compiles its step (and scatter)
variants as well, reports manifest errors at their line (`HTMLUI-FX-0014`), compile errors
(`HTMLUI-FX-0015`), and checks the reflection: a `u_state_<name>` texture per state some form
reads, and `u_fs_effectStep` in the step forms.

### 3.12 Export
`ShaderVariantCollector::addUiEffectKeys` adds, for a stateful effect used in a form, that form's
step variant and, with `scatter` passes, its scatter variant. Build Settings take
`uieffect/<name>:Stp` etc. `check_effect_shaders.sh` compiles `uieffect/all` (now with the step and
scatter variants) on all six backends.

### 3.13 The effects
Names follow the catalogue (kebab-case of the upstream name). Two upstream names collide with
phase-5 stateless effects; their stateful versions are named for what they simulate:
`fluid-smoke` (`Smoke`; the stateless `smoke` stays, a cheap fbm look) and `fluid-cursor-trail`
(the stateless `cursor-trail` is a faithful port of upstream's CursorTrail, which today is a CPU
polyline, 3.13.1). Making state an option of the stateless effects was rejected: the two are
different programs with different parameters, costs and variants.

| Effect | Upstream | Kind | Category | State (per instance) | Passes per step |
|---|---|---|---|---|---|
| `fog` | Fog | paint | interactive | fluid 256² + seeded dye | force, solve, restore |
| `fluid-smoke` | Smoke | paint | interactive | fluid 256² | cone emitter, solve |
| `smoke-fill` | SmokeFill | paint | interactive | fluid 256², confined to the rounded box | masked emitter, masked solve |
| `smoke-flow` | SmokeFlow | paint | interactive | fluid 256² | cursor puff ribbon, solve |
| `ink-flow` | InkFlow | paint | interactive | fluid 256² RGB dye, colour cycle | brush ribbon, solve |
| `fluid-cursor-trail` | CursorTrail + SmokeFlow | paint | interactive | fluid 256² | stroke ribbon, solve |
| `particle-flow` | ParticleFlow | paint | interactive | fluid 256² + 16384 particles + trail canvas | force ribbon, solve, advect, place, scatter, resolve |
| `boids` | Boids | paint | interactive | 4096 agents + trail canvas | flock, place, scatter, resolve |
| `magnetic-filings` | MagneticFilings | paint | interactive | 12288 directors | torque, place, scatter |
| `particles` | Particles | paint | interactive | 16000 particles + 32³ density grid | density scatter, forces, place, scatter |
| `particle-field` | ParticleField | filter | stylize | 256² particle grid over the content | spring, place, scatter |
| `reaction-diffusion` | ReactionDiffusion | paint | interactive | Gray-Scott 512² | brush + reaction × speed |
| `pixel-sort` | PixelSort | filter | interactive | sort offsets (box) + luma | luma, compare-swap × passes |
| `pixel-throw` | PixelThrow | filter | interactive | displacement 128² | pointer, advect + inject |
| `chroma-flow` | ChromaFlow | paint | interactive | flow + density 128² | pointer, advect + inject |
| `shatter` | Shatter | filter | interactive | 16 shards | pointer, shard springs |
| `data-mosh` | DataMosh | filter | stylize | decoded frame (box) | macroblock hold/refresh |
| `time-trail` | TimeTrail | filter | stylize | history + previous frame (box) | decay, stamp, keep |

Per-effect port notes (sources, adaptations, the measured cost and the goldens) are recorded in
"as implemented" (6.1) once built.

#### 3.13.1 Families
- **Fluids** (`fam_sim_fluid.glsl`, from `gpu/scaffolds/fluids.ts` and `std/sim/fluids.ts`). The
  Stable-Fluids chain: curl → vorticity → divergence (pressure warm start × 0.8) → Jacobi × 10 →
  gradient subtract → advect velocity → advect dye, with the clamped (free-slip) and toroidal
  boundaries, the velocity cap, the mask, `densityAge` and `rgb` dyes. Upstream's in-place
  compute passes become ping-pong passes: curl and vorticity write a new velocity image; divergence
  writes `(pressure × decay, divergence)` into the `pd` image, which Jacobi relaxes (upstream relaxes
  in place, a race between threads; the ping-pong is the textbook Jacobi the code describes); the
  copy passes disappear. A splat that writes velocity and dye is two passes, the velocity pass first
  (it reads the dye before the splat, as upstream's gravity term does). Cursor ribbons (upstream:
  one dispatch per stamp, up to 16 a frame) are one pass that loops over the step's stamps in
  order; per-cell updates are independent, so the result is the same.
- **Agents** (`fam_sim_agents.glsl`, from `std/sim/agentForces.ts`, `agentRender.ts`,
  `gpu/kit/agents.ts`). Agent tables are `rgba32f` images (one texel per agent). Upstream updates
  agents in place (Gauss-Seidel: a neighbour may be this frame's or the last); here every agent
  reads the last step's table (Jacobi), which upstream's own comment calls invisible in a smooth
  flock. The splat writes into fixed-point atomic accumulators become a placement pass (each agent's
  window in target texels) plus a scatter pass blending the shape's coverage additively into an
  `rgba16f` accumulator; the resolve (energy → alpha, the rest→excited ramp, the trail canvas) is a
  step pass (or the draw, when there is no trail). Accumulator gains (1/255 fixed point) are kept as
  scale factors so the resolved alpha is the same.
- **Host grids** (ChromaFlow, PixelThrow, Shatter, CursorTrail's recorder): upstream runs these on
  the CPU and uploads a texture each frame. They are fragment passes here (each cell's update reads
  the previous field only), so they are simulations like the others, with the pointer bookkeeping
  in a 1×1 `ptr` state.
- **Feedback** (`fam_sim_feedback.glsl`, from `std/sim/feedback.ts` and
  `gpu/scaffolds/feedbackSim.ts`): the content (filter form) is the "child"; state images are box
  sized (CSS px), premultiplied like the layer.

### 3.14 Performance design
- Per frame and stateful instance: one pass per step pass and iteration (fluids: 18 at 256²), the
  scatter draws, the draw; one extra layer pass break for a paint use. No compile, no pipeline, no
  image creation in the steady state (counted, 5.4).
- Pipelines: one per (variant, target format, blend), from phase 1's cache.
- **Guidance** (the guide): stateful effects are the costliest of the library: at most 2 or 3 on
  screen, prefer fixed-grid ones for large elements, lower `count` for agents on mobile, and hide
  (not just cover) an element whose simulation should stop.

### 3.15 Deviations from DESIGN.md (folded into DESIGN.md "Recorded deviations" at acceptance)
1. Deviation 20 (stateful simulations excluded) is reversed: phase 6 adds them, as per-element
   persistent render targets advanced by fragment passes at draw time.
2. Float render targets (`RGBA16F`, `RGBA32F`, `R32F`) join the render abstraction's
   `ColorFormat`, appended after `SRGB8A8`.
3. `ShaderType::UI_EFFECT` gains two property bits (`Stp`, `Sct`); a stateful effect has up to
   three variants per draw form.

## 4 Test plan

Commands as phase 5 (5 "Commands"); every run on the remote test host (BRIEF, the assignment).

### 4.1 Unit tests (`engine/tests/htmlui/effects/EffectStatefulTest.cpp`)
- **Manifest**: every directive and default; errors with lines (unknown state in a pass, a 32-bit
  `linear` state, `@cost stateful` without `@state` and the reverse, `x<param>` of a colour, a
  scatter of an unwritten state, too many states).
- **Allocation and pooling**: first draw allocates the declared images (sizes, formats, pairs vs
  single); a second element of the same effect reuses pooled images after the first is removed;
  `trim(0)` destroys pooled images; statistics (`stateImages`, `stateBytes`).
- **Resize**: `box` states resample (content kept, size changed), `clear` states restart; fixed
  states untouched; pinned clock replays.
- **Pause/resume**: `display: none`, `visibility: hidden`, off-screen (`left: -1000px`), clipped by an
  `overflow: hidden` parent: no steps while hidden, the same step count after, no catch-up.
- **Removal and leaks**: 100 cycles of load/unload of a document with every stateful effect
  (and 100 effect switches on one element): live simulations 0, pool trimmed, phase 1's GPU object
  counters and the renderer's images back to the baseline.
- **Determinism**: two scenes at the same pinned time are byte-identical, in either order and after
  other times; a fixed frame delta gives the same frames in two runs.
- **Substep cap**: a 1 s frame runs `max` steps; `rate`/`max` from the manifest; `time-scale 0`
  freezes, `time-scale 2` doubles the steps.
- **Splat input**: a splat changes the next frame; queued at most 8; named effect only; pinned
  clock: logged and replayed; `effectReset`.
- **Budgets and diagnostics**: 9 stateful elements in a document give `HTMLUI-FX-0018` once;
  the hard cap stops the 65th; an unsupported format gives `HTMLUI-FX-0017` (simulated with a
  test hook).
- **Shader keys**: `Stp`/`Sct` string, spec and storage round trips; `uieffect/all` counts.

### 4.2 Goldens (`golden/effects/<name>.t1`, `.t2`)
The phase-5 driver enumerates the new effects too (I1-I7). Pointer effects get the harness's test
pointer and synthetic trail, which drive the replay (3.6). Every stateful golden passes on glcore
and Vulkan with the same files.

### 4.3 Shaders
`check_effect_shaders.sh` (every variant, six backends, `ui-effects --validate`).

### 4.4 Performance (`EffectPerfTest`)
- `StatefulSteadyState`: a document with 6 stateful effects (two fluids, one agent, one grid, two
  filter feedbacks) after warm-up: per frame no instance, image, compile or pipeline creation; the
  expected pass counts; the engine's own CPU work of the frame within its budget.
- `CostTable` rows for every stateful effect (3.10).
- The existing grid documents A, A2, B, C are unchanged: non-stateful pages must not regress.

### 4.5 MCP (`tools/mcp/tests/test_html_ui_effects.py`)
A document with `fog`, `ink-flow` and `time-trail`: pinned screenshots identical; the live clock
moves the fog; in play, `eval_lua` splats into `ink-flow` and the region changes; `inspect_ui`
reports the state fields; `list_ui_effects` lists the 18 effects with `cost: stateful`.

### 4.6 Export (slow, `test_html_ui_effects_export.py`)
A project using `fog` and `time-trail` exports with their step variants and renders in the
Desktop player (screenshot regions animate).

## 5 Acceptance
- [ ] The 18 effects registered with manifests, goldens (t1, t2) passing on glcore and Vulkan,
      catalogue entries, thumbnails, `stateful` cost rows within budget, MIT attribution.
- [ ] Step/scatter variants compile on all six backends; `ui-effects --validate` passes.
- [ ] State images: allocation, pool, resize policy, release on removal/effect change/unload, no
      leaks over 100 cycles.
- [ ] Pause while not drawn, resume without catch-up; substep cap; pinned clock byte-identical.
- [ ] Splats (C++, Lua, MCP via `eval_lua`); `effectReset`.
- [ ] Budgets and diagnostics FX-0017/0018; statistics; `inspect_ui` state fields.
- [ ] Project effects with state: compile, `check`, hot reload restart.
- [ ] Export ships the step variants; the Desktop player draws a stateful effect.
- [ ] Every suite of the assignment passes on the remote host; non-stateful perf unchanged.
- [ ] Docs: guide, catalogue, CONTRACTS, DESIGN, support matrix.

## 6 As implemented

Built by `p6-stateful-fx` (branch `hui/p6-stateful-fx`, from `main` at `4958e5f7b`).

### 6.1 Design as built (differences from sections 3-5)
- **Passes at draw time, as 3.5.** A paint use ends the layer pass, runs its passes into the state
  images and binds the layer again (`LOAD`); a filter use runs them inside `applyFilter` with the
  content as the filter chain's primary. `EffectRenderer::advanceSimulation`
  (`EffectSimulator.cpp`) does the format check (FX-0017), the culling (`clippedOut`: the area's
  projected corners against the scissor and the context, paint uses), the admission (hard cap), the
  wait for the pass variants, the view-loss restart, `@reseed`, allocation and resize, then the
  pinned replay or the live clock of 3.6.
- **Pass index.** `JFX_PASS_<NAME>` numbers the `@init` and `@step` lines as written (not "@init
  first"); `JFX_PASSES` is the count (`JFX_PASS_COUNT` would collide with a pass named `count`).
  The compiler also defines `JFX_STATEFUL` and annotates the 32-bit states unfilterable through a
  generated include (`effects/__state_annotations__.glsl`).
- **`@reseed <param>...`** (new directive): a change of these parameters' canonical values
  restarts the simulation (`seed`, Gray-Scott's `preset`); other parameter changes act on the next
  step (live) or replay (pinned), as 3.7.
- **Box sizes** keep the aspect: the larger side is at most `max`, both scaled together.
- **Scatter.** The placement texel is `(cx, cy, halfX, halfY)` in target texels; the fragment
  function is `vec4 jfx_scatter(JfxStep s, JfxPlaced a)`. A single-image (additive) state reads an
  empty black 1×1 image while its own pass writes it, so a scatter pass takes the target's size from
  `s.size`.
- **Accumulators** hold upstream's integer quanta (`floor(w × gain)`) summed by additive blending
  into `rgba16f` box-sized targets (the area's CSS px; upstream a 1024² target), so the resolves
  divide as upstream does; ParticleField's RGBW accumulator sums floats (no fixed-point factor).
- **Agent sizes** (`agent_body`): upstream's texels of its 1024 reference over the area's height,
  as upstream draws them over areas at least 410 px tall; smaller areas keep that 410 px size in CSS
  px, so agents stay legible on cards (a pure CSS-px size crowded small areas).
- **Element seed.** `EffectRuntime::seedOf` hashes the element's address in its document (up to
  the document element). Phase 5 hashed the full address, whose last part is the context's root,
  named after the context: the seed depended on which context drew the element, and the goldens of
  seed-hashed simulations (`fog`'s start time) on the test order. Every phase-5 golden is unchanged.
- **Pointer.** Live: the frame's movement is spread over its steps; press and release edges on the
  first step. Pinned: the history at age `T − t` (the harness's synthetic arc); press edges are not
  replayed (the effects ported here react to movement, not clicks).
- **Inputs from scripts.** `UIElement::effectSplat` / `el:effectSplat{...}`, `effectReset`;
  `EffectRuntime::splat` queues at most 8 a simulation (live) or logs them with the pinned time
  (64 kept), as 3.8.
- **Statistics and inspection** as 3.10; `inspect_ui` writes `state: {bytes, steps, time,
  paused}` per stateful effect and `fx:<name>~` for a paused one; `ui-effects --json` and
  `list_ui_effects` carry `state` (images, passes, rate, substeps, replay, reseed, warm-up).
- **Golden invariants added**: I8, a stateful effect's `<name>.t0.png` at 0.25 s (inside the
  harness's 0.5 s pointer arc) differs from t1; I9, t1 reached from t2 in the same document (a
  replay) is t1's render byte for byte.

### 6.2 The effects
All 18 effects of 3.13 are built, MIT-attributed in `LICENSE.shaders-com.txt`, with catalogue
entries and thumbnails; goldens: `.t0` and `.t1` for all, `.t2` where t1 and t2 differ (all but
`chroma-flow`, `fluid-cursor-trail`, `pixel-throw`, `shatter` and `smoke-flow`, which draw only
what the pointer's arc leaves, the same at 1.5 s and 4 s). Port notes are in each `.frag`'s
header; the adaptations:

| Effect | State (per instance) | Steps | Port notes |
|---|---|---|---|
| `fog` | vel, pd, dye 256² rgba16f | seed (init), force, curl, vorticity, divergence, Jacobi ×10, project, advect velocity and dye, restore; `@warmup 50 0.1s` | toroidal; the random start time is a hash of `seed` and the element |
| `fluid-smoke` | vel, pd, dye 256² | cone emitter, solve (Jacobi ×10), advect density+age | the stateless `smoke` stays |
| `smoke-fill` | vel, pd, dye 256² | masked emitter and solve | the shape is the element's rounded box |
| `smoke-flow` | ptr 1², vel, pd, dye 256² | tracker, puff ribbon (16 stamps), solve | tracker smoothing in `ptr` |
| `ink-flow` | vel, pd, dye 256², cycle 1² | brush ribbon, solve, RGB dye | colour cycle in `cycle` |
| `fluid-cursor-trail` | ptr 1², vel, pd, dye 256² | stroke ribbon, solve | CursorTrail's look (`lifetime`, colours) over SmokeFlow's fluid; upstream has no fluid CursorTrail |
| `chroma-flow` | ptr 1², field 128² | advect + inject | upstream's CPU host grid as a pass |
| `pixel-throw` | ptr 1², flow 128² | advect + inject | CPU host grid as a pass; B-spline taps |
| `shatter` | shards 16×1 rgba32f | shove | CPU springs as a pass; the Voronoi field per fragment |
| `pixel-sort` | luma, offset 512² r32f, tick 1² | key, swap ×5, count | parity and frame count in `tick` |
| `reaction-diffusion` | rd 512² rgba32f, pattern 512² rgba16f | seed (init), react × `speed`, publish | child drive = the content in filter form; `@reseed preset` |
| `data-mosh` | state 768² rgba16f | macroblock hold/refresh | upstream `blend` = the universal `opacity` |
| `time-trail` | history, display, prev-live 640² rgba16f | decay, stamp, keep | upstream's one kernel as three passes |
| `boids` | agents, accel, place 64² rgba32f, agit 64² r32f, accum, trail (box) | spawn (init), steer, move, excite, place, splat (scatter), resolve | Jacobi update (upstream Gauss-Seidel); brute-force neighbours (4096 max) |
| `magnetic-filings` | ptr 1², agents 128×96 rgba32f, agit, place, accum, field (box) | torque, place, splat, resolve | jittered-grid start |
| `particles` | pos, vel, place 128² rgba32f, dplace 1024×128, dens 256×128 (32³ atlas), accum, field (box) | dplace, density (scatter), forces, move, place, splat (scatter), resolve | the default sphere container; trilinear density as 8 one-texel quads a particle |
| `particle-field` | pos, vel, place 256² rgba32f, col 256² rgba16f, accum, field (box) | color, forces, move, place, splat (scatter), resolve | upstream's one integrator as three passes; float RGBW accumulation; field stored premultiplied |
| `particle-flow` | vel, pd 256², agents, place 128² rgba32f, accum, trail (box) | force ribbon, solve, advect agents, place, splat (scatter), resolve | idle freeze not ported |

### 6.3 Measured cost and memory
glcore on llvmpipe, the remote test host (4 threads), `EffectPerfTest` Release:
- **CostTable** (a 512×512 instance on the live clock, one step a capture, against the 1024×1024
  fill; render + readback, process CPU; budget = ratio × 1.5): `boids` ×1.43, `chroma-flow` ×0.97,
  `data-mosh` ×1.58, `fluid-cursor-trail` ×2.57, `fluid-smoke` ×2.57, `fog` ×2.47, `ink-flow`
  ×2.68, `magnetic-filings` ×1.33, `particle-field` ×2.16, `particle-flow` ×3.38, `particles`
  ×1.44, `pixel-sort` ×2.09, `pixel-throw` ×1.65, `reaction-diffusion` ×1.85, `shatter` ×1.82,
  `smoke-fill` ×2.62, `smoke-flow` ×2.72, `time-trail` ×2.13 (the fill: 180-200 ms a capture).
- **State memory per instance** (images × texels × bytes, pairs counted twice; box states at
  512²): `fog`, `fluid-smoke`, `smoke-fill`, `smoke-flow`, `ink-flow`, `fluid-cursor-trail` 3.0 MB;
  `chroma-flow`, `pixel-throw` 0.25 MB; `shatter` 512 B; `pixel-sort` 4.0 MB;
  `reaction-diffusion` 12.0 MB; `data-mosh` 9.0 MB; `time-trail` 18.75 MB; `boids` 6.4 MB;
  `magnetic-filings` 6.8 MB; `particles` 11.75 MB; `particle-field` 13.0 MB; `particle-flow` 9.0 MB.
- **StatefulSteadyState** (six effects of 300×200 on the 1280×720 canvas, a step a frame): no
  instance, state image, variant or pipeline created; 53 step passes and 1 scatter pass a frame as
  the manifests list them; the engine's own CPU work 1.41 ms a frame against document B's 0.20 ms
  (budget B + 4 ms), the backend (llvmpipe rendering on the thread) 322 ms; 47.5 MB of state.
- Non-stateful pages: `GridSteadyStateAndBudgets` A 1.219 ms (budget 1.288), A2 3.528 ms (budget
  4.575); the phase-5 rows of the cost table within their bounds (highest low ×3.17, medium ×9.32).

