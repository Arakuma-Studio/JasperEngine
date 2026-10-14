# Phase 5: UI effects library

Binding design: `docs/html-ui/DESIGN.md`. Agent rules: `docs/html-ui/BRIEF.md`. This spec
details phase 5; where it extends DESIGN.md it says so (section 3.18).

## 1 Outcome

A game UI written in standard-looking CSS can use about 80 animated GPU effects (aurora and mesh
gradients, noise, glow, frosted and liquid glass over the backdrop, blur variants, chromatic
aberration, pixelate/dither/halftone/CRT/VHS, vignette, sheen, skeleton shimmer, ripples and
distortions, dissolves and wipes, fire, smoke, plasma, starfield, holographic foil, edge light,
cursor spotlight and trail). It uses one syntax that a person or an agent can guess:

```css
.hero   { background: #0b0d14; background-image: effect(aurora, speed 2, color-b #44ccff); }
.panel  { backdrop-filter: effect(frosted-glass, blur 14px, tint #ffffff22); border-radius: 16px; }
.title  { filter: effect(chromatic-aberration, strength 0.3) drop-shadow(0 2px 4px #0008); }
.button { background-image: effect(sheen, intensity 0); transition: background-image 300ms; }
.button:hover { background-image: effect(sheen, intensity 0.8); }
```

The editor validates every effect at load time and in `jasperengine-editor check --ui`. Each
effect is a GLSL 450 file in `engine/shaderlib/effects/`. The editor cross-compiles it like every
other engine shader, and exports ship it precompiled: shipped games compile nothing. Effects
can be animated in four ways: by time, with CSS transitions and `@keyframes` on the effect
value, with registered custom properties (`@property`), and with the Lua `anime` API. They also
react to the pointer and can read the backdrop.

Projects (and agents) can write their own effect as one `.frag` file, declared from CSS with
`@effect`. The catalogue page lists every effect with its parameters and a thumbnail rendered
by the golden harness. Effects ported from shaders.com carry its MIT notice.

## 2 Scope

### In
- Author syntax `effect(<name>, <param> <value>, …)` in `background-image`, `background`,
  `mask-image` (paint effects), `filter` and `backdrop-filter` (all effects). Layering with
  other background layers, `background-blend-mode` and `background-clip: text` for effect
  layers, and `var()` in arguments.
- Engine runtime: effect registry and manifests, RmlUi decorator and filter instancers, typed
  parameter parsing, the renderer hooks for paint draws and filter passes, a blur prepass,
  per-element state (integrated clocks, pointer smoothing, pointer trail, ripple events), a
  deterministic clock, element-local pointer and backdrop access.
- Shader system: `ShaderType::UI_EFFECT`, engine-library effect sources, fragment-only project
  effects, precompile, export, Build Settings, runtime compiler, and `shaders` CLI specs.
- Registered custom properties (`@property`), animatable with transitions and keyframes. This
  is an RmlUi fork patch plus compat.
- 83 built-in effects (Appendix A) with goldens, parse tests, docs entries and thumbnails.
- Project effects: `@effect` declaration, compile, hot reload, `check`, and MCP write-feedback.
- Lua/C++ API: the effect clock override and `getEffects()`. MCP tools: `list_ui_effects`,
  `set_ui_effect_time`, and an `inspect_ui` extension. CLI: `ui-effects --json|--validate`.
- Docs: the guide section, a generated catalogue page with thumbnails, support-matrix rows and
  diagnostics. Attribution: an MIT notice file in the source tree and in exports.
- Tests: unit, golden, invariants, animation, pointer, lifecycle, performance, MCP end-to-end,
  all-backend shader compile (GLSL ES 300 included), web build, and the Vulkan golden run.

### Out (with reason)
- **Stateful GPU simulations from shaders.com**: Boids, ChromaFlow, CursorTrail's fluid grid,
  DataMosh, Fog/Smoke/SmokeFill/SmokeFlow fluids, InkFlow, MagneticFilings, Particles,
  ParticleField, ParticleFlow, PixelSort, PixelThrow, ReactionDiffusion, Shatter and
  TimeTrail. These need per-element persistent render targets and compute passes. Phase-5
  effects are stateless on the GPU, and their state lives on the CPU: clocks, pointer history
  and ripple events. Cursor trail and cursor ripples are re-implemented on that model.
- **Media effects**: ImageTexture, VideoTexture, WebcamTexture, HTMLInCanvas and Text. HTML
  already has `<img>`, video is not part of the HTML UI, and text is text.
- **Shape generators and SDF shapes**: Circle, Star, Heart and the other shapes. The UI shape
  is the element box with its `border-radius`. Shape-driven effects (glass, neon, light edge,
  holographic, thin film) use the element's rounded box as their shape.
- **3D/raymarched shape effects**: Form3D, Surface3D, Voxels, Chrome, LiquidMetal, Plastic,
  Water, Goo, Nebula, Obsidian, Crystal, Frost, BrushedMetal, CarbonFiber, Irradiance, Heatmap.
  Each is a whole-product material renderer, too expensive per UI element and not idiomatic UI.
- **Utility nodes**: Group, Repeater, KeyFrames, ObjectTracker, CornerPin, Perspective,
  PolarCoordinates, RectangularCoordinates, Flip and Mirror. CSS transforms and layout cover
  these.
- **Effects that duplicate CSS filters RmlUi already has**: Blur, Grayscale, Invert, HueShift,
  Saturation, BrightnessContrast, Exposure and DropShadow. Use `blur()`, `grayscale()`,
  `invert()`, `hue-rotate()`, `saturate()`, `brightness()`/`contrast()` and `drop-shadow()`.
- `background-clip: text` for non-effect layers (`linear-gradient` text) belongs to
  phase 2/3. Phase 5 handles it for effect layers only.
- `mix-blend-mode` and `prefers-reduced-motion`.

## 3 Detailed design

### 3.1 Vocabulary
- **Effect**: a named GPU program with typed parameters, defined by one fragment file whose
  header is its **manifest** (3.9).
- **Paint effect** (`@kind paint`): produces an image for the element's box, like a gradient.
  It is allowed in `background-image`, `background` and `mask-image` (paint form). It is also
  allowed in `filter`/`backdrop-filter` (filter form, "source-in fill": the paint recolors the
  content inside the content's alpha, used for gradient text).
- **Filter effect** (`@kind filter`): processes pixels that are already rendered. In
  `filter` these are the element's own content, children included. In `backdrop-filter` they
  are what lies behind the element, clipped to its border box with radii. It is only allowed
  in `filter`/`backdrop-filter`.
- **Form**: how an effect is compiled. The paint form draws the element geometry. The filter
  form is a fullscreen pass over the RmlUi layer inside the scissor. Paint effects have both
  forms and filter effects only the filter form. Each form is one precompiled variant.
- **Effect area**: the rectangle the effect is defined on. For a decorator it is the
  decorator's paint area (`border-box` by default, from `background-clip`). In filter form it
  is the element's border box. Coordinates are CSS px with the origin at the top-left of the area.

### 3.2 Author syntax

#### Grammar
```
<effect()>     = effect( <effect-name> [ , <effect-arg> ]* )
<effect-name>  = <ident>                      lowercase kebab-case, ASCII-case-insensitive
<effect-arg>   = <param-name> <param-value>   name and value separated by whitespace
<param-name>   = <ident>                      an effect parameter or a universal parameter
<param-value>  = component values up to the next top-level comma, typed per 3.3;
                 may be or contain var( --x [, <fallback>] )
```
- Arguments are always named. Positional values are an error (`effect-positional-arg`).
- Commas inside parentheses do not split arguments (`color-a rgb(0, 0, 0)` is one argument).
- A repeated parameter is a warning, and the last one wins.
- `effect(aurora)` with no arguments uses every default.

#### Where `effect()` is allowed
| Property | Accepts | Notes |
|---|---|---|
| `background-image`, `background` | paint effects, as image layers | mixes with `linear-gradient()`, `url()` and other layers; the first layer is on top, as in CSS |
| `mask-image`, `-webkit-mask-image` | paint effects | the effect's alpha masks the element |
| `filter` | paint (filter form) and filter effects | mixes with standard filter functions, applied left to right |
| `backdrop-filter` | paint (filter form) and filter effects | effects marked `backdrop: yes` are designed for this |
| anything else (`color`, `border-image`, `list-style-image`, `content`, …) | — | `effect-wrong-context` error, declaration dropped |

#### Universal parameters (every effect)
| Param | Type | Default | Meaning |
|---|---|---|---|
| `opacity` | percentage | `1` | Paint: multiplies the output alpha. Filter: mixes the filtered result with the unfiltered input (0 = no effect). |
| `time-scale` | number `[-10, 10]` | `1` | multiplies the clock; `0` freezes the effect |
| `time-offset` | time `[-3600s, 3600s]` | `0s` | added to the clock (desynchronises repeated elements) |
| `blend` | enum `normal\|add\|screen\|multiply` | `normal` | Paint form: blend with what is below (fixed-function, 3.7). Filter form of a paint effect: how the paint combines with the content color. A filter effect: `effect-blend-on-filter` warning, ignored. |

#### Stacking and composition
- Effect layers are decorators in the same RmlUi decorator list as gradients and images.
  `background-color` is painted below all layers. Layer order follows CSS: the first listed is
  drawn on top. This holds because RmlUi renders decorators back to front, last to first
  (`ElementEffects.cpp:193-200`).
- Each effect layer follows the per-layer `background-clip` paint-area mapping that phase 2
  applies to every image layer (CSS default `border-box`). `background-size`,
  `background-position` and `background-repeat` do not apply to effect layers. Setting them
  gives an `effect-ignored-background-prop` warning: effects always cover their paint area, so
  use the effect's own `scale`/`center` params instead.
- `background-blend-mode` (per layer) sets the universal `blend` of effect layers. The mapping
  is `normal`, `multiply`, `screen`, and `plus-lighter`→`add` (also accepted: `lighter`). Any
  other mode on an effect layer gives an `effect-unsupported-blend` warning and uses `normal`.
  Phase 2 reports `background-blend-mode` as Unsupported; that report stays for non-effect
  layers only.
- **Gradient text.** The trigger is the same as phase 2's: `background-clip: text` (or
  `-webkit-`) together with a transparent text fill (`color: transparent` or
  `-webkit-text-fill-color: transparent`, in the same rule or the base rule). For effect
  layers it is exact instead of phase 2's solid-color approximation. Phase 2 keeps its
  approximation for gradient and `url()` layers. Without the transparent fill, phase 2 drops
  the clip value with an Info, as in CSS, where opaque text hides the clipped background. The
  effect layers go into the internal property `-jfx-text-fill` in filter form, bottom layer
  first so the top layer applies last. `-jfx-text-fill` is registered with RmlUi's `filter`
  parser, so it cascades and transitions on its own. A small fork patch in
  `ElementEffects::InstanceEffects` (`ElementEffects.cpp:94-123`) applies its list *before*
  the element's `filter` list. A `filter` set by another rule therefore never replaces the
  text fill, and `drop-shadow()` in `filter` shadows the filled text.
  - Compat replaces the transparent text color with `#ffffff` so the content alpha exists,
    with an `effect-text-fill` info diagnostic.
  - Unlike browsers, this masks all of the element's content, not only glyphs; the guide says
    so.
- Filters run in list order. Paint effects in `mask-image` stack as RmlUi masks do (alpha
  multiplied).

#### Project effects
```css
@effect my-glow { src: url("fx/my-glow.frag"); }
.card { background-image: effect(my-glow, intensity 0.7); }
```
`src` resolves relative to the stylesheet, like `url()`. The name must match
`[a-z][a-z0-9-]*`, must not be a built-in name, and is global for the process. The first
declaration wins, and a later different `src` gives `effect-declare-conflict`. Compat hoists
`@effect` (and `@property`) rules to the top of the RCSS output so their order in the file does
not matter.

#### Browser fallback (documented)
Browsers drop declarations containing `effect()` and ignore `@effect`. Authors put a plain
fallback first (`background: #0b0d14;` then `background-image: effect(…)`), which keeps the
same file previewable in a browser.

#### Examples (all go into the guide and tests)
```css
/* layered: effect over a gradient over a color, rounded */
.card { background: effect(grain, amount 0.08), #101218 linear-gradient(#223, #112); border-radius: 12px; }
/* gradient text */
.logo { color: #fff; background-image: effect(holographic, speed 0.6); background-clip: text; }
/* dissolve-out transition driven by keyframes */
@keyframes vanish { from { filter: effect(noise-dissolve, progress 0%) } to { filter: effect(noise-dissolve, progress 100%, edge-color #ff8a00) } }
.closing { animation: vanish 0.6s ease-in forwards; }
/* animated border via @property + mask */
@property --spin { syntax: '<angle>'; inherits: false; initial-value: 0deg; }
@keyframes turn { to { --spin: 360deg; } }
.ring { background-image: effect(conic-spin, rotation var(--spin), speed 0); animation: turn 4s linear infinite; }
/* cursor spotlight over a frosted panel */
.menu { backdrop-filter: effect(frosted-glass, blur 12px); background-image: effect(spotlight, radius 180px, intensity 0.25); }
```

### 3.3 Parameter types
Every value is parsed with full CSS semantics, using phase 2's value parsers (3.17), whether it
is written literally or comes from `var()` substitution. Out-of-range values are clamped with
`effect-out-of-range`. Ranges in Appendix A are hard limits.

| Type | Accepted CSS | Normalised to | Uniform member | RmlUi unit (interpolation) |
|---|---|---|---|---|
| `number` | `<number>` | float | `float` | NUMBER (linear) |
| `integer` | `<integer>` (numbers are rounded) | float | `float` | NUMBER (linear, rounded on use) |
| `percentage` | `<percentage>` or `<number>` in 0–1 | 0–1 | `float` | NUMBER |
| `length` | `px em rem vw vh vmin vmax %` (% of the effect area's shorter side), unitless `0` | CSS px at instancing | `float` | PX/EM/REM/VW/VH/PERCENT (RmlUi resolves) |
| `angle` | `deg rad grad turn`, or a bare number meaning degrees | radians | `float` | RAD |
| `time` | `s ms` | seconds | `float` | NUMBER (seconds) |
| `color` | any CSS color incl. `rgb(0 0 0 / 50%)`, `#rgb[a]`, `#rrggbb[aa]`, `hsl()`, `hwb()`, `oklch()`, named, `transparent`, `currentColor` (resolved from the element's computed `color` at instancing) | linear-sRGB, straight alpha | `vec4` | COLOUR |
| `position` | CSS `<position>`: 1–2 values or the 4-value keyword-offset form (`right 10px bottom 20%`) | 0–1 per axis in the area, y down | `vec2` | two NUMBER props `<p>-x`, `<p>-y` |
| `enum` | one of the listed idents | option index | `float` | KEYWORD (discrete) |
| `boolean` | `on off true false yes no 1 0` | 0/1 | `float` | KEYWORD (discrete) |
| `palette` | 2–8 entries `<color> [<percentage>]`, space-separated; missing positions are spread as in CSS gradients; positions must not decrease | colors linear + positions 0–1 | `vec4 <p>[8]; vec4 <p>Stops[2]; float <p>Count;` | COLORSTOPLIST (linear when the counts match) |

Uniform member names are the param name in camelCase (`color-a` → `colorA`,
`ray-density` → `rayDensity`). Universal parameters are not in the params block (3.9).

### 3.4 Animation
1. **Time.** Every effect reads `c.time` = clock × `time-scale` + `time-offset`. The clock is
   `EffectClock::now()`: its fixed time when one is set (item 8), else phase 1's
   `HtmlUIClock::now()` — the same seconds as RmlUi animations (`RmlSystem::GetElapsedTime`),
   Lottie and `anime`, so CSS animations and effects stay in step. It is the UI clock (unscaled
   frame time, deterministic under a fixed frame delta)
   and does not pause with the game's time scale.
2. **Integrated clocks.** A manifest `@time speed[, other]` lists up to 4 rate parameters. For
   each one, the runtime integrates `anim[i] += dt × rate × time-scale` per element and slot
   (3.6). Changing `speed` mid-animation, for example through a transition, then never jumps,
   matching shaders.com's `animatedTime` (`gpu/porters.ts:234-246`). With a fixed clock it
   computes `anim[i] = c.time × rate` instead, which is deterministic.
3. **Transitions and keyframes on effect values.** Compat emits fully typed, fully positional
   RCSS (3.5). RmlUi therefore interpolates two effect values of the same effect parameter by
   parameter: it requires the same instancer and property count (`ElementAnimation.cpp:104-165`,
   decorator lists `:271-325`, filter lists `:327-362`). Examples are `transition:
   background-image 300ms` (compat maps `background-image`/`background` to `decorator`) and
   `@keyframes` with `filter: effect(…)`. Different effects, or different numbers of layers,
   switch discretely, as RmlUi does.
4. **Custom properties.** `effect(aurora, speed var(--s))` is re-resolved whenever `--s`
   changes, through RmlUi's var dependency tracking (`ElementStyle.cpp:336-371`). Unregistered
   custom properties change discretely.
5. **Registered custom properties.** `@property --x { syntax: '<angle>'; inherits: false;
   initial-value: 0deg; }` makes `--x` a typed RmlUi property, so `transition: --x 1s` and
   `@keyframes { to { --x: 360deg } }` interpolate it, and every `var(--x)` follows (3.11).
6. **anime API.** `anime.animate(el, { '--glow': 0.8, duration: 400 })` animates a custom
   property per frame, using phase 1's custom-property support. This is the documented script
   route. Effects read it through `var()`.
7. **Cost of 4–6.** A change to a var-driven parameter re-instances that element's effect on
   that frame. This costs CPU (3.14) but never compiles a shader or creates a pipeline. Time
   (1–2) re-instances nothing.
8. **Determinism.** `EffectClock::setFixedTime(t)` (C++), `UIDocument.setEffectTime(t)` (Lua)
   and `set_ui_effect_time` (MCP) freeze every effect at `t` (effects only; the rest of the UI
   keeps running). Integrated clocks then use the fixed formula, `dt` reads 0, and pointer
   smoothing snaps. The golden harness uses it together with phase 1's UI clock override
   `HtmlUIClock::setFixedTime` (which freezes CSS animations, Lottie and `anime` too).

### 3.5 Compat-layer integration and the RCSS forms

#### Where P5 plugs into phase 2 (`docs/html-ui/phase-2-compat.md`)
Phase 2's `CssRewriter` (namespace `jasperengine::htmlui::compat`) dispatches property
rewrites through `css/props/*.cpp`. P5 adds a function-handler registry to
`css/props/Rewriters.h`; if phase 2 did not ship one, this is an additive change:

```cpp
using ImageFunctionHandler  = std::function<std::optional<std::string>(const ImageLayerContext&, DiagnosticSink&)>;  // RCSS decorator item
using FilterFunctionHandler = std::function<std::optional<std::string>(const FilterContext&, DiagnosticSink&)>;      // RCSS filter item
using AtRuleHandler         = std::function<void(const CssAtRule&, RewriteSession&, DiagnosticSink&)>;
void registerImageFunctionHandler(std::string_view functionName, ImageFunctionHandler);
void registerFilterFunctionHandler(std::string_view functionName, FilterFunctionHandler);
void registerAtRuleHandler(std::string_view atKeyword, AtRuleHandler);  // the rule is removed from the output, hoisting is the handler's job
```

The contexts carry:
- the function's component values and `SourceRef`;
- the property (`background`, `background-image`, `mask-image`, `filter`, `backdrop-filter`),
  plus the layer index, the layer's `background-clip`, `-size`, `-position`, `-repeat` and
  `-blend-mode`;
- the rule's text-fill state (for gradient text);
- `CompatOptions` (fork features).

P5 registers the `effect` image and filter handlers and the `@effect` at-rule handler in
`EffectRegistry::initialise`. P5-property replaces phase 2's `@property` branch in
`css/AtRules.cpp` (3.11). The other phase 2 mechanisms P5 relies on:
- `Background.cpp`'s layer list, mask layers through the same rewrite (as phase 2 implements
  it since p2-perf2: each declaration is read into `Layer` structures in the session's arena,
  the decorator list written once by `appendDecorators`; an effect layer is a `Layer` whose
  `image` text is the handler's RCSS item, dispatched from `layerImage`).
- `Effects.cpp` filter lists.
- `CustomPropertyIndex`: when `--bg: effect(aurora)` is used in `background: var(--bg)`, the
  declarations of `--bg` are rewritten through the same handler into the typed form.
- `CssRewriter::rewriteDeclarations` for Lua (hook H5).
- `CompatDocument::dependencies` for hot reload (hook H2).
- The diagnostics sink.

Phase 2 drops `paint()` layers with `HTMLUI-CSS-0015`. When the `paint()` name is a known
effect, P5 adds the suggestion "write effect(<name>)".

#### Conversion (`engine/core/htmlui/effects/EffectCompat.cpp`)
For each image layer or filter function named `effect`:
1. `EffectSyntax::parse` turns the function text into an `EffectCall` with the name and named
   arguments, each with its source offset for `line:col`.
2. `EffectRegistry::find(name)` looks up the effect. If it is unknown, the result is
   `effect-unknown` with suggestions: Levenshtein distance ≤ 3 over names, plus every
   manifest's `@suggest-for` words. The layer or filter is dropped.
3. The context is checked: the kind against the property, otherwise `effect-wrong-context`.
   This diagnostic names the paint/filter counterpart when one exists; for example, `sparkle`
   in `background-image` suggests `glints`.
4. Each argument is validated with `EffectValue::parse`. Unknown params give
   `effect-unknown-param` (warning, ignored) and bad values give `effect-bad-value` (warning,
   default used). Values that are or contain `var()` are kept verbatim; they are validated at
   runtime and warn to the Console.
5. The **typed RCSS form** is emitted:
   - `jfx-<name>(<slot> <slot> …)`, followed by the decorator paint-area keyword when the
     effect is a decorator.
   - There is one slot per manifest param, in manifest order, then `opacity`, `time-scale`,
     `time-offset` and, for paint form only, `blend`.
   - Slots are `v(<css text>)`. A `position` param takes two slots, `vx(<css text>)` and
     `vy(<css text>)`, which both carry the whole position text; the parser picks the axis.
     The wrapper keeps every slot one whitespace-separated token, even after `var()`
     substitution (the reason is in `PropertySpecification.cpp:513-620`).
   - Defaults are written out in canonical form: numbers shortest-round-trip, lengths with
     their unit, angles `deg`, times `s`, colors `#rrggbbaa`, palettes as space-separated
     `#rrggbbaa <p>%`.

   Example: `background: #0b0d14 effect(aurora, speed 2)` becomes
   `background-color: #0b0d14ff; decorator: jfx-aurora(v(#a533f8ff) v(#22ee88ff) v(#1694e8ff)
   v(linear) v(50) v(80) v(4) v(2) v(50) v(20) v(120) vx(50% 0%) vy(50% 0%) v(0) v(1) v(1)
   v(0s) v(normal)) border-box;`
6. **With phase 3's `ForkFeature::BackgroundLayers` present** (native `background-*` and CSS
   `mask-*`, `phase-3-fork-patches.md` §3.9.1), compat no longer turns `background` into
   `decorator`. The `effect` handler then emits the same typed `jfx-<name>(…)` function as a
   **layer inside the native `background-image` / `mask-image` list**, in its source position
   (the fork accepts registered decorator functions as layers, so mixed lists such as
   `effect(aurora), url(a.png), linear-gradient(…)` keep their order), and the per-layer
   `background-size/position/repeat` of an effect layer are ignored with `HTMLUI-FX-0009` as
   before. Without the feature (phase 2 alone) the `decorator` form above is used.
7. Phase 2's transition rewrite maps `background` to `background-color, decorator`
   (phase-2 3.9.7 "Transitions"). P5 checks that `background-image` and `mask-image` map to
   `decorator` and `mask-image`, and adds those entries to `css/props/Transition.cpp` if
   absent.

#### Runtime fallback form
P5 also registers RmlUi instancers named `effect`, one decorator and one filter. Their single
string property `value` sits behind the shorthand `value#` (type `RecursiveCommaSeparated`,
which joins the arguments; `PropertySpecification.cpp:389-430`, `StringUtilities.h:42`).
They catch `effect(…)` that reaches RmlUi without compat. One case is a whole effect inside a
custom property (`--bg: effect(aurora)`). Another is any script path that bypasses compat.
The instancer parses the value with `EffectSyntax` at instancing time and logs every
diagnostic to the Console with the element address. Values in this form change discretely.
The guide tells authors to write `effect()` directly in the property.

#### Lua `style`
Phase 2 hook H5 routes `element.style[prop] = v`, `style.cssText` and `setProperty` through
`CssRewriter::rewriteDeclarations`. `style.backgroundImage = "effect(…)"` therefore gets the
typed form and diagnostics (logged with the `[HTML UI] ` prefix). P5 adds a test for this.

### 3.6 Runtime architecture

All new files live in `engine/core/htmlui/effects/`, namespace `jasperengine::htmlui`. No
RmlUi types appear in public engine headers outside `engine/core/htmlui/` (DESIGN principle 3).

```cpp
// EffectTypes.h
enum class EffectKind { Paint, Filter };
enum class EffectParamType { Number, Integer, Percentage, Length, Angle, Time, Color, Position, Enum, Boolean, Palette };
enum class EffectCost { Low, Medium, High };
enum class EffectPointer { None, Position, Trail, Ripples };
struct EffectScaledParam { std::string param; float scale = 1.f; float addPx = 0.f; };   // "size*0.5 + 2px"; param empty = constant
struct EffectParamDef {
    std::string name;            // kebab-case CSS name
    std::string uniform;         // camelCase member name
    EffectParamType type;
    std::string defaultText;     // canonical CSS text
    float minValue = 0, maxValue = 0; bool hasRange = false;   // in normalised units (rad, s, px, 0-1…)
    std::vector<std::string> options;                           // enum
    std::string description;
};
struct EffectManifest {
    std::string name, summary, source, category;
    EffectKind kind; EffectCost cost; bool backdrop = false;
    std::vector<std::string> timeParams;                         // @time, ≤ 4
    EffectPointer pointer = EffectPointer::None;
    bool hasPrepass = false; EffectScaledParam prepassSigma; std::string prepassThreshold;   // @prepass blur
    EffectScaledParam margin;                                     // @margin (filter)
    bool boundsGeometry = false; EffectScaledParam boundsExtent;  // @geometry (paint)
    std::vector<std::string> suggestFor;
    std::vector<EffectParamDef> params;
    bool builtin = true;
    std::string sourcePath;      // builtin: "effects/<name>.frag" (library); project: project-relative .frag
    int line = 0;                // manifest start line, for diagnostics
};
using EffectDiagnostic = compat::Diagnostic;   // phase 2's diagnostic (code, severity, message, suggestion, file, line, column)
```

```cpp
// EffectManifest.h
class EffectManifestParser {
public:
    // parses the leading "// @" block of a fragment source; false on any error (diags filled)
    static bool parse(const std::string& source, const std::string& path, bool builtin, EffectManifest& out, std::vector<EffectDiagnostic>& diags);
    // the "#include main_paint/main_filter" line matches @kind, every param has a legal default, names unique, etc.
    static bool validate(const EffectManifest& m, const std::string& source, std::vector<EffectDiagnostic>& diags);
};

// EffectValue.h
struct EffectValue { EffectParamType type; float f = 0; Vector2 v2; Vector4 color; Length length; std::vector<std::pair<Vector4, float>> palette; bool isVar = false; std::string text; };
class EffectValueParser {
public:
    static bool parse(const EffectParamDef& def, std::string_view css, EffectValue& out, std::string& error);
    static std::string canonical(const EffectParamDef& def, const EffectValue& v);
    static float clampToRange(const EffectParamDef& def, float v, bool& clamped);
};

// EffectSyntax.h
struct EffectArg { std::string name, value; size_t nameOffset, valueOffset; };
struct EffectCall { std::string name; std::vector<EffectArg> args; size_t nameOffset; };
class EffectSyntax {
public:
    static bool parse(std::string_view functionText, EffectCall& out, std::vector<EffectDiagnostic>& diags);  // "effect(…)"
    static std::string toRcss(const EffectCall& call, const EffectManifest& m, bool filterForm, std::string_view paintArea, std::vector<EffectDiagnostic>& diags);
};

// EffectRegistry.h
class EffectRegistry {
public:
    static EffectRegistry& get();
    void initialise();                                        // parses BuiltinEffectManifests.gen.cpp, registers RmlUi parsers + instancers
    void shutdown();
    const EffectManifest* find(std::string_view name) const;
    std::vector<const EffectManifest*> all() const;           // builtin + project, sorted by name
    bool declareProjectEffect(const std::string& name, const std::string& fragPath, const std::string& declaredIn, int line, std::vector<EffectDiagnostic>& diags);
    void onProjectFileChanged(const std::string& projectRelativePath); // re-parse, re-register, invalidate shaders, mark documents for reload
    ShaderKey shaderKey(const EffectManifest& m, bool filterForm) const;    // 3.8
    std::vector<std::string> suggestions(std::string_view unknownName) const;
};

// EffectInstancers.h  (RmlUi-facing, private to htmlui)
class EffectDecoratorInstancer : public Rml::DecoratorInstancer;   // one per paint effect: name "jfx-<name>"
class EffectFilterInstancer    : public Rml::FilterInstancer;      // one per effect (paint effects too): "jfx-<name>"
class RawEffectDecoratorInstancer : public Rml::DecoratorInstancer; // name "effect"
class RawEffectFilterInstancer    : public Rml::FilterInstancer;    // name "effect"
class EffectDecorator : public Rml::Decorator;   // GenerateElementData / ReleaseElementData / RenderElement
class EffectFilter    : public Rml::Filter;      // CompileFilter(element) / ExtendInkOverflow
class EffectPropertyParser : public Rml::PropertyParser;  // registered as jfx-number, jfx-integer, jfx-percentage, jfx-length,
                                                          // jfx-angle, jfx-time, jfx-color, jfx-position-x, jfx-position-y,
                                                          // jfx-enum (parameters = options), jfx-boolean, jfx-palette
```
- Each typed instancer registers one RmlUi property per slot, named after the param
  (`speed`, `center-x`, `center-y`, …, `opacity`, `time-scale`, `time-offset`, `blend`). Each
  has the matching `jfx-*` parser and the canonical default. It registers the shorthand
  `decorator` (or `filter`) over all slots in order, with `ShorthandType::FallThrough`
  (`EffectSpecification.h:30-37`).
- Parsers strip `v(…)`/`vx(…)`/`vy(…)`, accept a bare value too, and produce typed properties.
- `InstanceDecorator`/`InstanceFilter` build an `EffectParamsSnapshot`. It holds the
  normalised values (lengths stay unresolved until instancing) and the param block bytes
  packed as far as possible.
- Decorators are cached in a 256-entry LRU keyed by (effect, canonical value text). Repeated
  values share one decorator object.

```cpp
// EffectRuntime.h
struct EffectElementState {            // per (Element*, effect name, ordinal), survives re-instancing
    double lastClock = -1; float anim[4] = {};
    Vector2 smoothPointer; float presence = 0;
    std::array<Vector4, 32> trail{};   // xy local CSS px, z age s, w valid; newest first
    std::array<Vector4, 8> ripples{};  // xy, z age s, w strength
    uint64_t lastFrame = 0;
};
struct EffectFrameBlock {              // mirrors u_fs_effectFrame (3.9) member for member, 14 × vec4 = 224 bytes
    Vector4 size, time, anim, pointer, pointerSmooth, pointerEx, layer, toLocalX, toLocalY, toLayerX, toLayerY, srcRect, radii, misc;
};
struct EffectParamsSnapshot {          // produced by an instancer from its typed RmlUi properties
    const EffectManifest* manifest; std::vector<EffectValue> values;   // manifest order
    float opacity = 1, timeScale = 1, timeOffset = 0; int blend = 0;
};
struct EffectInstance {                // one per element per effect use; its address (| 1) is the RmlUi handle
    const EffectManifest* manifest; bool filterForm; Rml::Element* element;
    EffectElementState* state; std::vector<uint8_t> params;   // packed u_fs_effectParams
    float opacity, timeScale, timeOffset; int blend;
    std::shared_ptr<ShaderRender> shader;                      // ShaderPool, shared
    EffectFrameBlock frame;                                    // filled by prepare()
};
class EffectClock { public: static double now(); static void setFixedTime(std::optional<double>); static bool isFixed(); };
struct EffectStats { uint64_t liveInstances, instancesCreated, compileCalls, paintDraws, filterPasses, prepassPasses, pipelinesCreated; };
class EffectRuntime : public Rml::Plugin {            // OnElementDestroy / OnContextDestroy purge state (Plugin.h:54)
public:
    static EffectRuntime& get();
    void beginFrame();                                 // called by UIDocumentSystem before each context render
    void onPointerMove(Rml::Context*, Vector2 contextPx);
    void onPointerButton(Rml::Context*, Vector2 contextPx, bool down);
    void onPointerLeave(Rml::Context*);
    EffectInstance* acquire(Rml::Element*, const EffectManifest&, bool filterForm, const EffectParamsSnapshot&);
    void release(EffectInstance*);
    void prepare(EffectInstance&, const Rml::RenderBox* area, Vector2 areaOffset);   // fills frame block, integrates state once per frame
    const EffectStats& stats() const; const EffectStats& frameStats() const;  // frame = since beginFrame
    void setTestPointer(Rml::Context*, std::optional<Vector2> px, bool down);   // tests only
};
```
- **State key.** A decorator does not know its list index, so state is keyed by (Element*,
  effect name, ordinal). The ordinal counts acquisitions of that name on that element within
  the current reload pass, and resets when the element's last acquisition frame differs. An
  element keeps at most 16 states, with the least recently used evicted. Lost state only
  resets clocks and trails.
- **Pointer.** Pointer positions are in context px and come from phase 1's input dispatch
  (3.17). `prepare` maps them to area-local CSS px with `Element::Project` (handles transforms;
  `Element.h:236`), then subtracts the area offset.
  - Smoothing: exponential, 80 ms half-life.
  - Presence: eased 0↔1 over 150 ms.
  - Velocity: from the last two samples.
  - Trail: one sample appended per frame while moving (≥ 1 px), aged by `dt`; entries older
    than 4 s are dropped.
  - Ripples: an event per press, and per 40 px of movement when the effect has `trigger move`.
- **Seed.** `frame.misc.z` = FNV-1a of `element->GetAddress()` mapped to [0,1), stable
  across runs.
- **Lifecycle.**
  - `EffectDecorator::GenerateElementData` calls `acquire`, generates geometry (3.7) and calls
    `RenderManager::CompileShader("jfx", {"instance": VOIDPTR})`. The renderer returns
    `(uintptr_t)instance | 1`.
  - `ReleaseElementData` releases both.
  - `EffectFilter::CompileFilter(element)` does the same through `CompileFilter`.
  - Filters are compiled once per data reload (`ElementEffects.cpp:150-160`), so filter passes
    call `prepare` themselves at draw time.

### 3.7 Renderer integration (hooks into phase 1's `RmlRenderer`)

**Handle tagging.** Effect handles have bit 0 set (instances are 8-aligned), and phase 1's
handles are untagged pointers. P5 adds these one-branch hooks to `RmlRenderer`:
`CompileShader` (name `jfx`), `RenderShader`, `ReleaseShader`, `CompileFilter` (name `jfx`),
`ReleaseFilter`, and the filter loop of `CompositeLayers`. Each forwards tagged handles to
`EffectRenderer`.

**Paint draw** (`EffectRenderer::drawPaint(EffectInstance&, Rml::CompiledGeometryHandle,
Rml::Vector2f translation)`):
- It uses phase 1's current transform, scissor and clip-mask (stencil) state and its geometry
  handle, exactly as phase 1's gradient `RenderShader` path does. RmlUi's GL3 backend is the
  model: `RmlUi_Renderer_GL3.cpp:1691-1745`, including the time uniform at `:1720-1733`.
- Pipeline: phase 1's pipeline cache keyed by (effect `ShaderRender`, blend mode, clip-mask
  state, target format and sample count).
- Blend factors (premultiplied):
  - normal: `ONE, ONE_MINUS_SRC_ALPHA`
  - add: `ONE, ONE`
  - screen: `ONE, ONE_MINUS_SRC_COLOR`
  - multiply: `DST_COLOR, ONE_MINUS_SRC_ALPHA`
- Uniforms: `u_vs_effectVertex` (phase 1's projection×transform and the translation), then
  `u_fs_effectFrame`, `u_fs_effectParams`, and `u_fs_effectTrail`/`u_fs_effectRipples` if
  declared.
- Geometry:
  - `@geometry fill` (default): `MeshUtilities::GenerateBackground` over the paint-area render
    box, with border radii and a vertex alpha of opacity×255. Tex coords are
    `(pos − fillOffset)/fillSize`, as `DecoratorShader.cpp:33-47` does.
  - `@geometry bounds <extent>`: a quad of the paint area expanded by the extent on all sides,
    with the same tex-coord mapping (uv outside [0,1]). The shader computes the rounded-box
    SDF itself (`jfx_box_sdf`).
- If the variant is not ready yet (editor async build) or is missing (shipped game), the draw
  is skipped. A missing variant is reported once through `ShaderPool`'s missing-shader list.

**Filter pass** (`EffectRenderer::applyFilter(EffectInstance&, RmlFilterContext&)`).
`RmlFilterContext` is the interface phase 1 provides (`phase-1-integration.md` §3.3.3): the postprocess
primary/secondary/tertiary targets, `swap()`, the current scissor, the layer size and
orientation, `blit(src,dst)`, `blurInPlace(target, temp, sigmaDevicePx)` and a fullscreen
draw with a pipeline. The sequence, after RmlUi's model (`RmlUi_Renderer_GL3.cpp:1760-1883`):
1. `prepare(instance, borderBox)`. The window↔local affine rows come from three
   `Element::Project` calls (window→local) and their inverse.
2. Prepass, if `@prepass blur`:
   - `blit(primary → tertiary)`.
   - With a `threshold` param, run the internal `@effects/_bright` variant (soft-knee
     bright-pass) from tertiary to secondary, then blit back.
   - `blurInPlace(tertiary, secondary, σ × dpr)`, with σ = scaled param in CSS px.
3. The effect pass reads primary (`u_effectSource`) and tertiary (`u_effectBlurred`) and writes
   secondary with blending off, as RmlUi's own filters do. Then `swap()`.
4. `ExtendInkOverflow` grows the region by `@margin` (CSS px × dpr) for `filter:`. For
   `backdrop-filter`, RmlUi uses the same margin for its read region
   (`ElementEffects.cpp:236-245`).

**State caching.** Consecutive draws with the same pipeline skip re-applying it. The params
block bytes are cached per instance and repacked only on re-instancing. The frame block
(224 bytes) is uploaded per draw. RmlUi's draw order is kept: effects are never reordered.

**Stats.** `EffectStats.pipelinesCreated` counts pipeline-cache misses for UI_EFFECT shaders,
read from phase 1's `HtmlUIRenderStats::pipelineCacheMisses` around effect draws (phase 1
§3.2.8). Blend modes map to phase 1's `BlendMode` values `PREMULTIPLIED` (normal),
`ADDITIVE_PREMULTIPLIED` (add), `SCREEN_PREMULTIPLIED` and `MULTIPLY_PREMULTIPLIED`
(phase 1 §3.1.1).

### 3.8 Shader system integration

**Type.** Append `UI_EFFECT` to `ShaderType` after every value phases 0–1 added (`HTMLUI`,
`HTMLUI_GRADIENT`, `HTMLUI_POST`; phase 1 has no effect shader type or effect registry)
(`Render.h:41-58`, "keep appending"). It has one property bit: bit 0 `Flt` ("Filter form").
The effect's identity rides in the existing custom-shader id field
(`ShaderPool.h:36-41`, bits 48–63).
- **Built-in effects** use *library ids*: `ShaderPool::registerCustomShader("@effects/<name>")`.
  A name starting with `@` means the source is in the engine library, not the project. Their
  sanitised basename is `fx_<name, '-'→'_'>`.
- **Project effects** register their project-relative `.frag` path
  (`"ui/fx/my-glow.frag"`). A custom name that ends in `.frag` and has no `|` is a
  *fragment-only* custom shader.
- Shader strings: `uieffect_fx_aurora`, `uieffect_fx_aurora_Flt`, `uieffect_ui_fx_my_glow_frag_Flt`.

| File | Change |
|---|---|
| `Render.h` | `UI_EFFECT` appended |
| `ShaderPool.{h,cpp}` | `parseShaderTypeToken("uieffect")`; type names `uieffect`/`UI Effect`; property count 1, bit-0 names `Flt`/`Filter form`; `getValidPropertyMask` 1; `static bool isLibraryCustomShader(uint16_t)`; basename rule for `@effects/`; `parseShaderSpec("uieffect/<name>[:Flt]")`; new `static bool expandShaderSpec(const std::string&, std::vector<ShaderKey>&, std::string* error)` (handles `uieffect/all` = every built-in, paint effects in both forms, filter effects `Flt`); `parseShaderStr` for `uieffect_…` names through a resolver set by `EffectRegistry` (`static void setCustomShaderNameResolver(std::function<std::string(const std::string& basename)>)`), used by `getPrecompiledShaderKeys`; `destroyCustomShaders` skips library ids; new `static uint32_t getCustomShaderGeneration()`, incremented by `clearUnused` (EffectRegistry re-resolves ids when it changes) |
| `ShaderVariantCompiler.cpp` | `setupShaderArgs(UI_EFFECT)`: vert `effects/effect_filter.vert` if `Flt`, else `effects/effect_paint.vert`; defines `JFX=1`, plus `JFX_FILTER_FORM=1` if `Flt`. `compile`: a library id sets frag `effects/<name>.frag` (fails "Unknown UI effect" when absent) and needs no custom sources; a project id overlays fragment-only sources and keeps the library vertex. `getCustomShaderPaths`: the fragment-only rule (vert empty). `getCustomShaderIncludeRoot`: the fragment's directory when vert is empty. `resolveCustomShaderSources`: skips the empty vertex entry |
| `RuntimeShaderCompiler.cpp` (`:304`) | library ids do not resolve project sources |
| `editor/shader/ShaderBuilder.cpp` | library ids are treated as built-ins: no project sources, no staleness signature, not dropped by `invalidateCustomShaders` |
| `ShaderUniforms.{h,cpp}` | `CustomUniformBlock::writeUniformArray(block, member, const Vector4* values, count)` for palette arrays |
| `CoreClassesLua.cpp` | `ShaderType.UI_EFFECT` |
| `editor/Stream.cpp` | none: keys already persist `type` + `customShader` name (`Stream.cpp:38-90`) |

**Collection and export.** P5-tooling adds to `ShaderVariantCollector`, after the scene passes
(`ShaderVariantCollector.cpp:540-566`), the effect keys used by the scene's
`UIDocumentComponent`s and their prefabs:
- Each document is analysed through compat in analysis mode: HTML, linked and inline CSS, and
  `style` attributes. Paint uses add the paint form; filter-context uses add `Flt`.
- `@effect`-declared project effects are included.
- A regex scan of the project's `.lua` sources for `effect\(\s*([a-z][a-z0-9-]*)` adds both
  forms of any matching paint effect and `Flt` of filter effects.
- If an effect has a prepass with a threshold, `@effects/_bright:Flt` is added.

Anything else dynamic goes through Build Settings additions with spec `uieffect/<name>[:Flt]`.
The ExportWindow "Add Shader" modal (`ExportWindow.cpp:1236-1242`) lists `UI Effect` with a
combo of built-in and declared project effects.

### 3.9 Effect source format

#### Files
```
engine/shaderlib/effects/
  effect_paint.vert  effect_filter.vert           vertex stages (one per form)
  lib/jfx_common.glsl  lib/jfx_color.glsl  lib/jfx_hash.glsl  lib/jfx_noise.glsl
  lib/jfx_sdf.glsl  lib/jfx_geom.glsl  lib/jfx_sample.glsl  lib/jfx_blend.glsl  lib/jfx_tone.glsl
  lib/main_paint.glsl  lib/main_filter.glsl       main() per kind
  lib/fam_<package>_*.glsl                        family helpers (owned by that package)
  _bright.frag                                    internal prepass (not listed, name starts with '_')
  <name>.frag                                     one per effect (file stem == @effect name)
  LICENSE.shaders-com.txt                         MIT notice (3.16)
```
`engine/CMakeLists.txt:956-960` globs `shaderlib/**/*.{glsl,vert,frag}` recursively, so the
runtime compiler embeds these files with no CMake change.

`_bright.frag` is internal. It has no manifest, `EffectRegistry::all()` does not list it, and
`gen_effect_manifests.py` skips `_`-prefixed files. Its params block is
`{ float threshold; float knee; }`, and `EffectRenderer` drives it directly as library id
`@effects/_bright` (filter form only).

#### Manifest grammar
These are consecutive comment lines directly after `#version 450`:
```
// @effect     <name>
// @kind       paint | filter
// @category   gradient | noise | pattern | light | energy | space | interactive | glass | blur | stylize | color | distort | transition
// @summary    <one line, ≤ 100 chars>
// @source     shaders.com <Name>[ + <Name>] [(adapted: <text>)] | original [(<text>)]
// @cost       low | medium | high
// @backdrop   yes | no                                   (filter kind; default no)
// @time       <param>[, <param>]                          (≤ 4 rate params)
// @pointer    none | position | trail | ripples           (default none)
// @prepass    blur <scaled> [threshold <param>]           (filter kind)
// @margin     <scaled> | <number>px                       (filter kind; default 0px)
// @geometry   fill | bounds <scaled>                      (paint kind; default fill)
// @suggest-for <word> [<word> …]
// @param      <name> <type> <default> [<min> <max>] [<opt>|<opt>|…] -- <description>
<scaled> := <param>[*<number>][+<number>px] | <number>px
```
- Defaults containing spaces are double-quoted (`"50% 0%"`). Ranges carry units (`0px 128px`,
  `0deg 360deg`, `0s 10s`).
- An effect file ends with exactly one `#include "effects/lib/main_paint.glsl"` or
  `"…/main_filter.glsl"`, matching `@kind` (validated).
- Every param must have a member of the matching type in `u_fs_effectParams`, and the block
  must have no extra members. `jasperengine-editor ui-effects --validate` compiles every effect
  for glcore and checks the reflection.
- Built-in effect names: `[a-z][a-z0-9-]*`. They must not equal a CSS image or filter function
  name (`linear-gradient`, `radial-gradient`, `conic-gradient`, `url`, `image`, `blur`,
  `drop-shadow`, `brightness`, `contrast`, `grayscale`, `hue-rotate`, `invert`, `opacity`,
  `saturate`, `sepia`) and must not start with `_`.

#### Uniform interface (exact names; reflected by name like `postprocess.frag`)
```glsl
// effect_paint.vert   (attribute declarations copied from phase 1's RmlUi vertex shader verbatim)
uniform u_vs_effectVertex { mat4 transform; vec4 translate; } vtx;   // translate.xy = RmlUi translation
out vec2 v_uv;      // tex coord: 0..1 over the paint area (beyond for @geometry bounds)
out vec4 v_color;   // premultiplied vertex color (alpha = element opacity)

// effect_filter.vert  (vertexless fullscreen triangle as fullscreen.vert, same IS_GLSL/IS_VULKAN handling)
uniform u_vs_effectFilter { vec4 layer; vec4 flip; } fvtx;  // layer.xy size device px; flip.x = 1 when layer row 0 is the window bottom
out vec2 v_texcoord;   // samples u_effectSource at this fragment on every backend
out vec2 v_layerPx;    // fragment position in RmlUi window px (top-left origin, y down)

// lib/jfx_common.glsl
uniform u_fs_effectFrame {
    vec4 size;          // xy area size CSS px, zw 1/size
    vec4 time;          // x clock*time-scale+time-offset (s), y frame dt (0 when fixed), z dpr, w 1 when fixed
    vec4 anim;          // integrated @time clocks, declaration order
    vec4 pointer;       // xy area-local CSS px, z inside 0/1, w primary down 0/1
    vec4 pointerSmooth; // xy smoothed local px, z presence 0..1, w 0
    vec4 pointerEx;     // xy velocity CSS px/s, z s since last press inside (1e6 none), w s since enter (1e6 outside)
    vec4 layer;         // filter form: xy layer size device px, zw 1/size
    vec4 toLocalX;      // filter form: local.x = dot(toLocalX.xyz, vec3(layerPx, 1))
    vec4 toLocalY;
    vec4 toLayerX;      // filter form: layerPx.x = dot(toLayerX.xyz, vec3(localPx, 1))
    vec4 toLayerY;
    vec4 srcRect;       // filter form: valid source uv rect (scissor), xy min zw max
    vec4 radii;         // area border radii CSS px: tl tr br bl
    vec4 misc;          // x opacity, y blend index, z element seed [0,1), w 0
} frame;
uniform texture2D u_effectSource;  uniform sampler u_effectSource_smp;    // filter form
uniform texture2D u_effectBlurred; uniform sampler u_effectBlurred_smp;   // filter form, prepass

// optional, declared by the effect when @pointer trail / ripples
uniform u_fs_effectTrail   { vec4 points[32]; } trail;    // xy local px, z age s, w valid; newest first
uniform u_fs_effectRipples { vec4 events[8]; } ripples;   // xy local px, z age s, w strength

// in the effect file
uniform u_fs_effectParams { /* vec4 members first, then vec2, then float; float-only types */ } params;
```
Rules (the same as `docs/mcp/guide/shaders.md`): no int members, since GL flattens the block
after its first member; arrays only for palettes; all values float. Unused textures and blocks
are stripped by the compiler, and the binder skips names missing from reflection
(`ShaderData::getUniformBlockIndexByName`/`getTextureIndexByName`).

#### Include library API (`effects/lib/*.glsl`, all `jfx_` prefixed)
```glsl
// jfx_common.glsl
#define JFX_PI 3.14159265359
#define JFX_TAU 6.28318530718
struct JfxContext { vec2 uv; vec2 px; vec2 size; float aspect; float time; vec4 anim; float dt; float dpr;
                    vec2 pointer; vec2 pointerSmooth; float pointerInside; float pointerPresence; float pointerDown;
                    vec2 pointerVelocity; float sincePress; vec4 radii; float seed; };
JfxContext jfx_context();               // uv y down; px = uv*size; aspect guarded max(size.y,1e-6)
// jfx_color.glsl
vec3 jfx_srgb_to_linear(vec3); vec3 jfx_linear_to_srgb(vec3);       // kit tonemap.ts:182-188 and inverse
vec4 jfx_premultiply(vec4); vec4 jfx_unpremultiply(vec4);
vec4 jfx_encode(vec4 straightLinear);   // linear→sRGB, premultiply (shaders.com finalTail, composer.ts:1541-1549)
float jfx_luma709(vec3); float jfx_luma601(vec3);
vec3 jfx_rgb_to_oklab(vec3); vec3 jfx_oklab_to_rgb(vec3); vec3 jfx_oklab_to_oklch(vec3); vec3 jfx_oklch_to_oklab(vec3);
vec3 jfx_rgb_to_hsl(vec3); vec3 jfx_hsl_to_rgb(vec3); vec3 jfx_rgb_to_hsv(vec3); vec3 jfx_hsv_to_rgb(vec3);
vec3 jfx_rgb_to_lab(vec3); vec3 jfx_lab_to_rgb(vec3); vec3 jfx_lab_to_lch(vec3); vec3 jfx_lch_to_lab(vec3);
vec4 jfx_mix_colors(vec4 a, vec4 b, float t, float space);   // 0 linear 1 oklch 2 oklab 3 hsl 4 hsv 5 lch (colorMixing.ts:328-419)
vec4 jfx_palette(vec4 colors[8], vec4 stops[2], float count, float t, float space);  // clamped; jfx_palette_wrap for repeat
vec3 jfx_cosine_palette(float t, vec3 a, vec3 b, vec3 c, vec3 d);
vec3 jfx_hue_rotate(vec3 rgb, float radians); vec3 jfx_saturate(vec3 rgb, float s);
// jfx_hash.glsl  (bit hashes from kit noise.ts:385-445; MaterialX lookup3 from noise.ts:27-120)
float jfx_hash11(float); float jfx_hash12(vec2); vec2 jfx_hash22(vec2); vec3 jfx_hash32(vec2); float jfx_hash13(vec3); vec3 jfx_hash33(vec3);
uint jfx_mx_hash(int x, int y); uint jfx_mx_hash(int x, int y, int z); float jfx_mx_bits_to_01(uint);
// jfx_noise.glsl
float jfx_value12(vec2); float jfx_perlin12(vec2); vec3 jfx_perlin12d(vec2);      // noise.ts:447-480
float jfx_simplex12(vec2); float jfx_simplex13(vec3);
float jfx_mx_noise2(vec2); float jfx_mx_noise3(vec3); float jfx_mx_worley2(vec2 p, float jitter);   // MaterialX
vec2 jfx_curl22(vec2); float jfx_fbm_perlin(vec2 p, float octaves);                // noise.ts:589, :735-748; octaves ≤ 8 runtime loop
float jfx_fbm_value(vec2 p, float octaves); vec2 jfx_domain_warp2(vec2 p, float strength, float t);
vec3 jfx_worley(vec2 p, float jitter);   // x F1, y F2, z cell id hash
float jfx_blue12(vec2);
// jfx_sdf.glsl
float jfx_box_sdf(vec2 px, vec2 size, vec4 radii);           // per-corner rounded box, CSS px, negative inside
float jfx_rounded_rect_sdf(vec2 d, vec2 halfSize, float r);  // sdf.ts:157-166
float jfx_circle_sdf(vec2 p, float r); float jfx_segment_sdf(vec2 p, vec2 a, vec2 b);
float jfx_aa_fill(float sdf);            // coverage via fwidth
float jfx_aa_band(float sdf, float halfWidth);
vec2 jfx_box_normal(vec2 px, vec2 size, vec4 radii);         // SDF gradient (central difference, 0.5px)
// jfx_geom.glsl
vec2 jfx_rotate(vec2 p, float radians); vec2 jfx_to_polar(vec2 p); vec2 jfx_from_polar(vec2 rt);
float jfx_fmod(float x, float m); vec2 jfx_fmod(vec2 x, vec2 m);   // floored (geom.ts:39-42)
vec2 jfx_edge_uv(vec2 uv, float mode);   // 0 stretch(clamp) 1 transparent 2 mirror 3 wrap
float jfx_edge_alpha(vec2 uv, float mode);
// jfx_sample.glsl  (filter form only)
vec4 jfx_src_premul_here();              // exact texel at this fragment
vec4 jfx_src(vec2 localPx);              // straight linear, clamped to srcRect
vec4 jfx_src_edge(vec2 localPx, float mode);   // edge modes relative to the effect area
vec4 jfx_blurred(vec2 localPx);          // straight linear prepass result
vec2 jfx_local_to_layer_uv(vec2 localPx); vec2 jfx_layer_to_local(vec2 layerPx);
// jfx_blend.glsl  (blend.ts)
vec3 jfx_blend(vec3 base, vec3 top, float mode);  // 0 normal 1 multiply 2 screen 3 overlay 4 soft-light 5 add 6 difference 7 color-dodge
// jfx_tone.glsl
float jfx_bayer2(vec2 cell); float jfx_bayer4(vec2 cell); float jfx_bayer8(vec2 cell);
vec3 jfx_posterize(vec3 c, float levels); vec3 jfx_contrast(vec3 c, float k);
float jfx_grain(vec2 px, float t, float animated);   // colorOps.ts:267 filmGrain look
```

#### `main_*.glsl` contracts
- An effect defines `vec4 jfx_paint(JfxContext c)` or `vec4 jfx_filter(JfxContext c)`. The
  return value is straight-alpha linear RGBA.
- `main_paint.glsl`:
  - Paint form: `frag_color = jfx_encode(p) * (v_color.a * frame.misc.x)`.
  - Filter form: with `S` = the unpremultiplied source in sRGB and `P` = the encoded paint
    (alpha `pa`):
    - normal: rgb = `mix(S.rgb, P.rgb, pa)`
    - add: `S.rgb + P.rgb*pa`
    - screen: `1-(1-S.rgb)*(1-P.rgb*pa)`
    - multiply: `S.rgb*mix(vec3(1),P.rgb,pa)`
    - In every mode the alpha stays `S.a`. The result is premultiplied, then
      `mix(src, result, opacity)`.
- `main_filter.glsl`: `#error`s without `JFX_FILTER_FORM`, then
  `frag_color = mix(jfx_src_premul_here(), jfx_encode(jfx_filter(c)), frame.misc.x)`.
- Colors arrive as linear sRGB. CSS colors go sRGB→linear in C++, which is shaders.com's
  `setColorSpaceMode('srgb')` (`utilities/transformations.ts:10-17`). Their default is
  p3-linear, so hues differ slightly from their website, by design.
- `shaders.com` compile-time props (`compileTime: true`, `PRIMITIVES.md` C5) become runtime
  uniforms: enums and loop bounds. A loop runs to the type's maximum with
  `if (float(i) >= n) break;`. **There is exactly one variant per effect per form.**

### 3.10 Project effects
- **Authoring:**
  - The author writes `<dir>/<name>.frag` (any directory under the project's assets, as for
    documents).
  - The manifest's `@effect` must equal the CSS `@effect` name; a mismatch is the error
    `effect-manifest`.
  - Includes resolve against the file's directory, then the project root, then the engine
    library (`ShaderVariantCompiler.cpp:591-660`), so `#include
    "effects/lib/jfx_noise.glsl"` works.
- **Declaration:** `@effect` in CSS (3.2).
  - Compat calls `EffectRegistry::declareProjectEffect`, which reads the file through
    `FileData`, parses and validates the manifest, and registers both instancer kinds as
    applicable.
  - It also calls `ShaderPool::registerCustomShader(<project-relative .frag>)`.
  - Shipped games read the same `.frag` for its manifest. It ships as a project asset like the
    CSS, while the compiled variants ship as `.sdat`/headers.
- **Build:** the editor compiles project effects on first use, like forks (async, with a disk
  cache keyed by the source signature).
- **Hot reload:** the `@effect` handler adds the `.frag` to the document's
  `CompatDocument::dependencies`, so the hot-reload path (phase 1's
  `UIDocumentSystem::reloadChangedFiles`, called by phase 1's editor watcher and by phase 4's
  `w4::syncHtmlUi`; phase 2 hook H2 adds the dependencies) invalidates and reloads every
  document that declared it. P5-tooling calls `EffectRegistry::onProjectFileChanged` before
  that reload, in `reloadChangedFiles` (so both callers get it). It:
  - re-parses the manifest;
  - keeps old instancers alive in a graveyard until shutdown (RmlUi decorators may still
    reference them) and re-registers the instancers;
  - runs `ShaderPool::remove` on both forms plus `ShaderBuilder::invalidateCustomShaders`.
- **check:** `jasperengine-editor check` compiles every project `.frag` whose manifest block
  (the comment lines right after `#version 450`) starts with `// @effect`. It compiles them as
  UI effects, in each applicable form, and reports `file:line: message`.
  `ProjectChecks.cpp:557`/`:619` gain the UI_EFFECT detection. `check --ui` reports the CSS-side
  diagnostics.
- **MCP:** `write_file` on such a `.frag` compiles it and returns errors, as for forks.
- **Guide:** a "Writing an effect" subsection gives a complete minimal example, these rules,
  and the include API.

### 3.11 Registered custom properties (`@property`)
- **Compat** (`engine/core/htmlui/compat/CompatAtProperty.{h,cpp}`). It replaces phase 2's
  `@property` branch in `css/AtRules.cpp`, which rewrites to a `body#htmlui-root { --x: v }`
  rule with `HTMLUI-AT-0010` for `inherits: false`. That fallback stays only for unsupported
  syntaxes, and when the fork feature is absent. The new handling is gated on a new
  `ForkFeature::RegisteredProperties`: its `ForkFeatures.cpp` table entry is flipped by
  P5-property, and its probe registers `--htmlui-probe` as `<length>`, sets it to `3px` on a
  scratch element, and reads `var(--htmlui-probe)` back through a dependent `width`. It parses
  `@property --name { syntax; inherits; initial-value }` and calls
  `Rml::StyleSheetSpecification::RegisterProperty("--name", initial, inherits)` with a parser
  per syntax:
  - `<number>`/`<integer>` → `number`
  - `<length>` → `length`
  - `<percentage>`/`<length-percentage>` → `length_percent`
  - `<angle>` → `angle`
  - `<time>` → `number` (seconds; compat converts `ms`)
  - `<color>` → `color`

  It records the name with the new fork API, then drops the rule from the RCSS. Registration
  happens before the RCSS is parsed, and `ParsePropertyDeclaration` tries registered
  properties before the `--` custom path (`PropertySpecification.cpp:219-251`), so later
  declarations, transitions and keyframes are typed.
- **Errors in compat:**
  - Another syntax (including `*`) gives `at-property-unsupported-syntax` (warning) and falls
    back to phase 2's root rule.
  - Re-registering with a different definition gives `at-property-conflict` (warning, first
    wins, process-global).
  - A missing `initial-value` for a non-`*` syntax gives `at-property-invalid` (error, as in
    CSS).
- **Fork patch** `rmlui: registered custom properties (var() reads typed --props)`, recorded in
  `FORK.md`:
  1. `StyleSheetSpecification`: `static void MarkRegisteredCustomProperty(PropertyId)`,
     `static bool IsRegisteredCustomProperty(PropertyId)`.
  2. `ElementStyle::GetSpecifiedCustomProperty`/`SubstituteVariableOnce` (`ElementStyle.cpp:163-275`):
     for a registered name, take this element's `GetProperty(id)`. That value is the animated
     local value, else the inherited value (`inherits: true`), else the default, so
     non-inherited properties follow CSS semantics. It is serialised as follows: numbers
     shortest round-trip, lengths px, angles deg, colors `#rrggbbaa`.
  3. Dirtying: when a registered property is dirtied on an element (`ElementStyle.cpp:870`
     and the transition/animation setters), its name is inserted into `dirty_variables` for
     that element, and for descendants when it inherits. The existing dependency check
     (`:361-371`) then re-resolves dependents.
  4. `Element::GetProperty("--x")` already resolves registered names first (`Element.cpp:655-664`).
- **Tests:** a transition on `:hover` sampled at 50% (UI clock pinned with `HtmlUIClock::setFixedTime`), infinite keyframes
  driving `conic-spin rotation var(--spin)`, inherited vs non-inherited lookups, an
  unregistered fallback, and a conflict diagnostic. Goldens: `golden/property/*`.

### 3.12 Lua and C++ API
- C++ (`engine/core/object/ui/UIDocument.h`, static): `static void setEffectTime(std::optional<double> seconds)`,
  `static double getEffectTime()`.
- Lua: `UIDocument.setEffectTime(seconds|nil)`, `UIDocument.getEffectTime()`.
- `UIElement:getEffects()` (Lua) and `std::vector<UIEffectInfo> UIElement::getEffects() const`
  (C++). Each entry is
  `{ property = "background-image"|"mask-image"|"filter"|"backdrop-filter", index = 1.., name = "aurora",
  form = "paint"|"filter", params = { ["speed"] = "2", … } }` (canonical CSS text, universal
  params included).
- Bindings are added to `HtmlUIClassesLua.cpp` (phase 1 file, additive). The Lua stubs are
  regenerated with `generate_lua_stubs.py`, with `_PARAM_TYPE_OVERRIDES`/stub fragments
  (phase 1 §3.9.5 mechanism) giving `setEffectTime(seconds: number?)`, `getEffectTime():
  number` and `getEffects(): UIEffectInfo[]` precise types, so phase 4's LuaLS test stays
  clean.

### 3.13 Tooling, diagnostics and docs

**Diagnostic codes.** These are added to phase 2's code registry (`DiagnosticCodes.cpp`,
phase-2 3.4) under a new area `FX`, as `DiagArea::FX`. IDs are `HTMLUI-FX-0001`… in the order
of this table, and the kebab-case names are the registry `name`s. The `at-property-*` codes
are appended to area `AT` with the next free numbers at implementation time (after phase 2's
`HTMLUI-AT-0011` and any AT code phase 3 added); tests look them up by name
(`findDiagnosticCode("at-property-conflict")`).
- Each code has a `help` paragraph for the generated docs.
- CSS-side codes are reported at load time (logged with the `[HTML UI] ` prefix) and by
  `check --ui`, in phase 2's `toText` format
  (`ui/menu.css:12:5: warning: unknown parameter 'speeed' (fix: speed) [HTMLUI-FX-0004]`;
  `check --ui` and MCP print the path project-relative). The `code` field of `check --ui`
  findings and MCP `problems` is the id (`HTMLUI-FX-0004`).
- Runtime-form problems go to the Console with the element address.
- `EffectDiagnostic` (3.6) is an alias of `compat::Diagnostic`; reports go through
  `compat::DiagnosticSink` with `SourceRef`s.

| Code (registry name) | Severity | When | Suggestion text |
|---|---|---|---|
| `HTMLUI-FX-0001` `effect-unknown` | error | unknown effect name; layer/filter dropped | nearest names + `@suggest-for` matches + "see jasper://docs/html-ui-effects" |
| `HTMLUI-FX-0002` `effect-wrong-context` | error | paint/filter effect in a property that does not accept it, or `effect()` in any other property | where it is allowed; counterpart effect |
| `HTMLUI-FX-0003` `effect-positional-arg` | error | argument without a param name | the param list |
| `HTMLUI-FX-0004` `effect-unknown-param` | warning | param not in the manifest; ignored | nearest param |
| `HTMLUI-FX-0005` `effect-bad-value` | warning | value does not parse for the type; default used | expected type and range |
| `HTMLUI-FX-0006` `effect-out-of-range` | warning | clamped | the range |
| `HTMLUI-FX-0007` `effect-duplicate-param` | warning | repeated; last wins | — |
| `HTMLUI-FX-0008` `effect-blend-on-filter` | warning | `blend` on a filter effect | remove it |
| `HTMLUI-FX-0009` `effect-ignored-background-prop` | warning | `background-size/position/repeat` on an effect layer | use the effect's params |
| `HTMLUI-FX-0010` `effect-unsupported-blend` | warning | `background-blend-mode` value other than normal/multiply/screen/plus-lighter on an effect layer | — |
| `HTMLUI-FX-0011` `effect-text-fill` | info | `background-clip: text` forced text color to `#fff` | — |
| `HTMLUI-FX-0012` `effect-declare-conflict` | error | `@effect` name is built-in or declared with another `src` | rename |
| `HTMLUI-FX-0013` `effect-file-missing` | error | `@effect` `src` not found | path checked |
| `HTMLUI-FX-0014` `effect-manifest` | error | project effect manifest invalid (reported at the `.frag` line) | the grammar rule |
| `HTMLUI-FX-0015` `effect-shader-compile` | error | editor: project effect failed to compile (log lines mapped to `.frag` lines) | — |
| `HTMLUI-FX-0016` `effect-cost` | info | more than 4 `high`-cost effects in one document | cost guidance link |
| `at-property-unsupported-syntax` / `at-property-conflict` / `at-property-invalid` | warning / warning / error | 3.11 | — |

**CLI** (`editor/cli/CommandLine.cpp`, owned by phase 4's p4-checks; additive):
`jasperengine-editor ui-effects [--json] [--validate] [--project <dir>]`, dispatched from
`editor/main.cpp` next to phase 4's `ui-support` and listed in the main help. `--json` prints the catalogue with every manifest field, an example and the
thumbnail path. `--validate` compiles every effect (both forms) for glcore, checks the
manifest against the reflection, and exits non-zero on any problem. The existing `shaders`
command accepts `--shader uieffect/<name>[:Flt]` and `uieffect/all` through `expandShaderSpec`.

**MCP tools** (`editor/mcp/tools/UIEffectTools.cpp`, regenerated in `docs/mcp/TOOLS.md` by
`tools/mcp/gen_tool_docs.py`):
- `list_ui_effects { category?, kind?, name? }` → the `--json` entries, filtered.
- `set_ui_effect_time { time: number | null }` → freezes or unfreezes the effect clock in the
  edit and play views.
- `inspect_ui` (phase 4) element output gains `effects: [getEffects() entries]` (omitted when
  empty; one `fx:<name>` token per effect in the text outline).
- Budget (phase 4 §3.6.7, `test_budget`): `list_ui_effects` ≤ 900 chars, `set_ui_effect_time`
  ≤ 500, `inspect_ui` growth ≤ 150; p5-tooling raises the total cap by the measured sum (at
  most 1,550) and updates the README sentence.

**Docs:**
- `docs/mcp/guide/html-ui.md` gains an "Effects" section, owned by P5-infra (subsections in
  the work packages); P5-infra adds its title to the section list of phase 4's
  `test_guide_served`, and its snippets must pass phase 4's guide tests (names exist, Lua
  parses, good CSS/HTML gives zero `check --ui` warnings). It covers: quick reference; where `effect()` goes; layering; parameter
  types; universal params; animation (time, transitions/keyframes, `@property`, anime,
  determinism); gradient text; backdrop effects; project effects ("Writing an effect");
  performance guidance (3.14); diagnostics table; browser fallback.
- `docs/mcp/guide/html-ui-effects.md` is generated by `tools/htmlui/gen_effects_docs.py` from
  `ui-effects --json`. Per category, each effect gets a heading, kind, summary, source,
  cost, backdrop flag, a params table (name, type, default, range/options, description), one
  CSS example, and its thumbnail `img/effects/<name>.png`. The thumbnails are copied from the
  goldens (`golden/effects/<name>.t1.png`). It is served as `jasper://docs/html-ui-effects`
  (`CMakeLists.txt:345` globs `*.md` only; images are not embedded).
  `gen_effects_docs.py --check` fails when the page is stale.
- Support matrix (`html-ui-support.md`, phase 4 generator): compat property-table rows for
  `effect()` in the five properties, `@effect`, `@property`, `background-clip: text` (effect
  layers only), and `background-blend-mode` (effect layers only).
- `docs/mcp/guide/README.md` gets a row for the new page.

### 3.14 Performance design and cost guidance
- One `ShaderRender` per (effect, form) from `ShaderPool`, shared by every element. One
  pipeline per (shader, blend, clip state, target format), shared. Nothing is compiled or
  created per element or per frame.
- Per frame, per element: one frame-block upload and one draw (paint), or one fullscreen pass
  limited by the scissor to the element's region (filter). A prepass adds 1 blit + 2 blur
  passes, plus 2 for threshold.
- Consecutive same-pipeline draws skip pipeline binds. Draw order is never changed, so there
  is no cross-element batching.
- Time-only animation: zero re-instancing. Var/transition/keyframe-driven parameter changes:
  one decorator re-instance (LRU-cached), one `acquire`, one 4–30-vertex mesh, and no
  compile.
- **Cost classes** are measured (5.4): render time at 1024×1024 relative to a
  `background-color` fill of the same size. `low` ≤ 4×, `medium` ≤ 16×, `high` above that.
- **Guidance** (in the guide):
  - Low effects: use freely.
  - Medium: up to about half the screen area.
  - High: at most 2 elements or a quarter of the screen.
  - `backdrop-filter` effects pay one extra layer copy.
  - Prepass effects (`frosted-glass`, `liquid-glass` with blur, `tilt-shift`,
    `progressive-blur`, `glow`) are the costliest per pixel; keep them on panels, not
    full-screen.
  - Prefer time-driven params (`speed`) to per-frame `var()` animation for many elements.

### 3.15 Porting guide: shaders.com TypeGPU DSL → GLSL 450

Sources: `/tmp/htmlui-src/shaders/packages/core/src/`. Each effect is
`shaders/<Name>/index.ts` (props + `paint:`/`effect:`/`map:`/`shape:`/`fragment:`), plus
`cover.jpg` as a visual reference. Effects compose *words* from `std/` (builders that emit
`Expr` trees). The words call GPU functions in `gpu/kit/` (`tgpu.fn` bodies with `'use gpu'`,
TypeScript transpiled to WGSL). `gpu/kit/CATALOG.md` indexes the kit, and `PRIMITIVES.md`
states its rules.

**Procedure per effect.**
1. Read `index.ts`. Copy the props into `@param` lines: camelCase → kebab, the same defaults,
   and slider ranges as hard ranges, except where Appendix A adapts them.
2. Follow each word to its kit function and port that function into the include library if
   it is shared, or into a family include. Keep constants bit-exact: the "salt" tables and
   `fround` literals are the look.
3. Write `jfx_paint`/`jfx_filter`.
4. Render the golden at defaults and compare it side by side with `cover.jpg`. The golden
   generator writes `build/effects-compare/<name>.png` (golden | cover). The orientation,
   palette and structure must match. Covers may use non-default presets: if so, also render
   the cover's look by setting params in a second, review-only golden, and note it in the
   report.

**Translation table.**

| TypeGPU / std | GLSL |
|---|---|
| `tgpu.fn([d.vec2f, d.f32], d.f32)((p, k) => { 'use gpu' … })` | `float name(vec2 p, float k) { … }` |
| `d.f32`, `d.u32`, `d.i32`, `d.vec2f/3f/4f`, `d.vec2u` | `float`, `uint`, `int`, `vec2/3/4`, `uvec2` |
| `a.add(b)`, `.sub`, `.mul`, `.div` on vectors | `a + b`, `-`, `*`, `/` |
| `std.select(f, t, cond)` (**false value first**) | `cond ? t : f`, or `mix(f, t, vec(cond))` for vectors |
| `std.fract/floor/mix/smoothstep/clamp/pow/exp` | same names |
| `std.radians(x)` | `radians(x)` |
| WGSL `bitcast<vec2u>(v)` (`noise.ts:385`) | `floatBitsToUint(v)` (GLSL ES 3.00 OK) |
| u32 arithmetic (wraps) | `uint` arithmetic (wraps in GLSL too) |
| `x % m` on floats (WGSL truncated remainder) | `x - m * trunc(x / m)`, **not** `mod` (floored); the kit's own `flooredMod1` (`geom.ts:39-42`) is `jfx_fmod` |
| `d.struct({...})` return | a GLSL `struct` |
| `uniformOf(p('x'), params)` | `params.x` |
| `surfaceOf(params)` → `{uv, viewport, aspect}` (uv y down, `std/frames.ts:22-41`) | `c.uv`, `c.size`, `c.aspect` |
| `animatedTime(params)` (`gpu/porters.ts:234-246`) | `c.anim.x` (manifest `@time speed`) |
| `compileTime: true` prop | runtime uniform + bounded loop/branch (3.9) |
| `transformColor` (p3-linear in their default mode) | `vec4` linear sRGB from C++ |
| `transformPosition` / `centerPropConfig` | `vec2` 0–1, y down (`params.center`) |
| `transformAngle` (degrees) | radians in the uniform (`angle` type); drop `radians()` calls |
| `edgesPropConfig` | `enum stretch\|transparent\|mirror\|wrap` + `jfx_src_edge` |
| `colorStopsPropConfig` / `stops` | `palette` type + `jfx_palette` |
| `colorSpace` select (`transformations.ts:279-286`) | `enum linear\|oklch\|oklab\|hsl\|hsv\|lch` + `jfx_mix_colors` (same 0–5 numbering) |
| `gatherStack(stage, [])`, child RTT reads | `jfx_src()` / `jfx_src_edge()` (already unpremultiplied and linear) |
| compute blur / `bloom(...)` / `tiltShift(...)` | `@prepass blur` + `jfx_blurred()` |
| `simulate.grid(...)`, pointer sims | out of scope; use `@pointer trail/ripples` (CPU state) |
| final `tonemap → linearToSrgb → premultiply` (`composer.ts:1541-1549`) | done by `main_*.glsl` (`jfx_encode`) |

**Example 1, a value-level kit function** (`std/effects/lens.ts:29-37`, `chromaticOffsetUV`):
```ts
(uv, angle, strength, aspect, channelOffset) => { 'use gpu'
    const angleRad = std.radians(angle)
    const direction = d.vec2f(std.cos(angleRad) / aspect, std.sin(angleRad))
    return uv.add(direction.mul(strength * 0.1).mul(channelOffset)) }
```
```glsl
// angle arrives in radians (angle type), so no radians() here
vec2 jfx_chromatic_offset_uv(vec2 uv, float angle, float strength, float aspect, float channelOffset) {
    vec2 direction = vec2(cos(angle) / aspect, sin(angle));
    return uv + direction * (strength * 0.1) * channelOffset;
}
// chromatic-aberration.frag: three taps, alpha from green (ChromaticAberration/index.ts:89-95)
vec4 jfx_filter(JfxContext c) {
    vec4 r = jfx_src(jfx_chromatic_offset_uv(c.uv, params.angle, params.strength, c.aspect, params.redOffset) * c.size);
    vec4 g = jfx_src(jfx_chromatic_offset_uv(c.uv, params.angle, params.strength, c.aspect, params.greenOffset) * c.size);
    vec4 b = jfx_src(jfx_chromatic_offset_uv(c.uv, params.angle, params.strength, c.aspect, params.blueOffset) * c.size);
    return vec4(r.r, g.g, b.b, g.a);
}
```
The kit does this in premultiplied space, then unpremultiplies. `jfx_src` returns straight
linear samples, so recombining channels gives the same result except at partially transparent
edges, which are accepted.

**Example 2, select argument order** (`gpu/kit/noise.ts:128-136`, `mxGradientFloat2`):
`std.select(y, x, h < 4u)` means `h < 4u ? x : y`, and
`std.select(u, -u, (h & 1u) !== 0u)` means `(h & 1u) != 0u ? -u : u`.

**Example 3, a composition-level look** (`shaders/Aurora/index.ts:142-224` → `aurora.frag`):

| Aurora word (file:line) | Emits | GLSL |
|---|---|---|
| `ARC_WAVES`, `LAYERS`, `RAY_SALT` (`Aurora/index.ts:19-32`) | constants | `const` arrays, bit-exact |
| `wavyLine({along, time, waves})` (`std/paint/noise.ts:125-135` → `waves` `:80-109`) | `Σ amount·sin(x·freq + t·speed + phase)` | 3 `sin` terms |
| `hashPhase(seed, {k, big})` (`std/math.ts:424-425`) | `fract(sin(seed·k)·big)·τ` | `fract(sin(s*k)*big)*JFX_TAU` |
| `volumeNoiseAt(vec3)` (`std/paint/materials.ts:945-947`) | `mxNoiseFloat3` (`kit/noise.ts:347`) | `jfx_mx_noise3` |
| `rayBands({...})` (`std/paint/light.ts:46-58`) | `pow(sin(a·f + t·s + φ)·0.5 + 0.5, sharp)` | inline |
| `softBand({distance, below, above})` (`std/mask.ts:35-44`) | `smoothstep(b0,b1,d)·(1−smoothstep(a0,a1,d))` | inline |
| `layered(LAYERS.slice(0,count), …)` (`std/paint/compose.ts:53-70`) | Σ value·weight, Σ weight | loop of 4, `break` at `curtains` |
| `colorLadder3({a,b,c,space})` (`std/paint/fields.ts:128-134`) | two `mixColors` in a space | `jfx_mix_colors(jfx_mix_colors(A,B,t1,s),C,t2,s)` |
| `animatedTime(params)` | integrated clock | `c.anim.x` with `@time speed` |

```glsl
vec4 jfx_paint(JfxContext c) {
    const float OFFS[4] = float[4](0.0, 3.1, 6.7, 9.4);
    const float W[4]    = float[4](1.0, 0.65, 0.45, 0.3);
    float cx = params.center.x, cy = 1.0 - params.center.y;      // as Aurora/index.ts:149-150
    float wav = params.waviness * 0.01, rayAmt = params.rayDensity * 0.01, h = params.height * 0.01;
    float sum = 0.0, total = 0.0;
    for (int i = 0; i < 4; i++) {
        if (float(i) >= params.curtains) break;
        float tSlow = c.anim.x * 0.06 + OFFS[i];
        float lSeed = params.seed + float(i) * 31.7;
        float wx = (c.uv.x - cx) * c.aspect * 2.0;
        float arc = 0.15 * sin(wx * 0.7 + tSlow * 0.4  + aurora_phase(lSeed, 12.9, 4758.5))
                  + 0.08 * sin(wx * 1.6 + tSlow * 0.65 + aurora_phase(lSeed, 78.2, 2847.1))
                  + 0.04 * sin(wx * 2.9 + tSlow * 0.3  + aurora_phase(lSeed, 41.6, 1593.7));
        float n = jfx_mx_noise3(vec3(wx * 0.5, tSlow * 0.25, lSeed * 0.1));
        // … rays, envelope, colorT, alpha exactly as :173-200 …
        sum += colorT * alpha * W[i]; total += alpha * W[i];
    }
    // … balance remap, ladder, brightness and alpha as :215-223 …
}
```

**Example 4, glass on the element box** (`gpu/kit/effects/glass.ts:105-157`). The shape SDF
is `jfx_box_sdf(c.px, c.size, c.radii)` in CSS px. The gradient is `jfx_box_normal`, which
replaces the forward difference at `:105-110`. Normalise `thickness` in px rather than in shape
units: `depthNorm = clamp(-sdf / max(thickness, 0.5), 0, 1)`. The lens UVs follow
`glassLensUVs` (`:126-157`) with offsets in CSS px: `offset = -normal * refraction * 0.15 *
min(size.x,size.y) * refrStrength`, so the refraction scales with the element as it did with
the canvas.

**Gotchas checklist (part of every family package's review).**
- uv is y down. Where the source flips (`1 - center.y`), port the flip literally and verify
  the orientation against `cover.jpg`.
- Guard every aspect divide (`max(size.y, 1e-6)`; PRIMITIVES.md D-1).
- Use 601 vs 709 luma exactly as the source file does (PRIMITIVES.md D-2).
- No `int` uniforms. No dynamic indexing of constant arrays with non-loop indices in ES 3.00
  fragment shaders; use loop indices or unrolled selects.
- `highp` is implied (SPIRV-Cross emits `precision highp float/int` for ES). The hashes need it.
- Never `discard` in paint form, which would break the clip-mask path; output alpha 0
  instead.

### 3.16 Attribution
- `engine/shaderlib/effects/LICENSE.shaders-com.txt`: the full MIT text with "Copyright (c)
  2026 Shader Effects Inc." (from `/tmp/htmlui-src/shaders/LICENSE`), the upstream URL, and the
  list of ported effects with their source names. It is regenerated by
  `gen_effects_docs.py` from the `@source` lines.
- Each ported `.frag` has the header comment `// Ported from shaders.com <Name> (MIT,
  Copyright (c) 2026 Shader Effects Inc.); see effects/LICENSE.shaders-com.txt`.
- `NOTICE.md` gains one paragraph pointing to it.
- The exporter (`editor/Exporter.cpp`) copies it to `<export>/licenses/shaders.com-LICENSE.txt`
  whenever the exported keys contain a built-in UI_EFFECT whose `@source` starts with
  `shaders.com`.

### 3.17 Contracts required from phases 0–4
Each item names the phase that provides it, by the provider's name (reconciled with the
phase 0–4 specs; `docs/html-ui/CONTRACTS.md` lists them). When one turns out absent in the
merged code, P5 adds it as a small additive change in that phase's file and lists it in its
report.

| # | From | Needed | Provided as |
|---|---|---|---|
| C1 | P0, P1 | A fixed UI clock for goldens; a golden harness that renders an HTML/CSS file at W×H to RGBA and compares against `engine/tests/htmlui/golden/**` | `HtmlUIClock::setFixedTime` (phase 1 §3.0; `RmlSystem::GetElapsedTime` returns `HtmlUIClock::now()`); phase 0 `GpuEnvironment`/`SceneFixture`/`compareWithGolden`/`EXPECT_GOLDEN` with `JASPER_HTMLUI_UPDATE_GOLDEN=1` (phase 0 §3.8.2) |
| C2 | P1 | `RmlRenderer` `CompileShader/RenderShader/ReleaseShader/CompileFilter/ReleaseFilter/CompositeLayers` on the render abstraction, pipeline cache keyed by shader + blend + clip state; P5 adds the tagged-handle branches | phase 1 §3.2, `SokolPipelineCache` + `PipelineState` (§3.1.1–3.1.3, blend modes incl. screen/multiply), bit-0 handle rule (§3.3.3) |
| C3 | P1 | `RmlFilterContext` | `engine/core/htmlui/RmlFilterContext.h` (phase 1 §3.3.3): `primary/secondary/tertiary`, `swap`, `blit`, `blurInPlace`, `getScissor`, `getLayerSize`, `drawFullscreen` |
| C4 | P1 | RmlUi vertex layout and the projection × transform it uploads | phase 0 `htmlui.vert` layout (`a_position`, `a_color`, `a_texcoord1`, stride 20) and `u_vs_htmluiParams { mat4 transform; vec4 translation; }`; phase 1 `RmlRenderer::getCurrentTransform()`/`getCurrentScissor()`/`isClipMaskEnabled()`/`getStencilRef()` |
| C5 | P1 | Pointer move/button/leave reaching a document context | phase 0 `HtmlUIContext::processMouse*` (extended by phase 1 §3.6.2); P5 adds `EffectRuntime::onPointer*` there |
| C6 | P0, P1 | A per-frame hook before a context renders; init/shutdown hooks after `Rml::Initialise()` | `HtmlUIContext::render` (phase 0 §3.5.3) before `Context::Render()` → `EffectRuntime::beginFrame()`; `HtmlUI::acquire()`/`release()` (phase 0 §3.3.1) → `EffectRegistry::initialise/shutdown`, `Rml::RegisterPlugin(&EffectRuntime::get())` |
| C7 | P1 | `anime` animating custom properties (`'--x'`) per frame | phase 1 §3.11 `AnimProperty` (custom properties written with `Element::SetProperty("--name", text)`) |
| C8 | P2 | `CssRewriter` (`css/props/Background.cpp`, `Effects.cpp`, `Transition.cpp`), `css/AtRules.cpp`, `CustomPropertyIndex`, `DiagnosticSink`/`SourceRef`/`DiagnosticCodes.cpp`, `ColorValue`/`Units`/`ValueNormalizer`, `CompatOptions::fork`/`ForkFeatures`, `checkDocument`, `CssRewriter::rewriteDeclarations` (H5), `CompatDocument::dependencies` + H2, support tables with `Support::Emulated` rows; P5 adds the handler registry to `css/props/Rewriters.h` | phase 2 §3.3–3.12, §3.16 under these names |
| C9 | P4 | `check --ui`; the support-matrix generator; `inspect_ui`; the auto-sync file-change hook; `html-ui.md` | `jasperengine-editor check --ui` → `ProjectChecks::checkUiFiles`; `jasperengine-editor ui-support` → `tools/mcp/gen_html_ui_support.py` → `docs/mcp/guide/html-ui-support.md`; `inspect_ui` (`HtmlUiTools.cpp`, `HtmlUIInspector`); `w4::syncHtmlUi` → `UIDocumentSystem::reloadChangedFiles`; `docs/mcp/guide/html-ui.md` (p4-guide) |

`ForkFeature::RegisteredProperties` and `ForkFeature::TextFillFilter` are appended to phase 2's
enum after the values phase 3 appended (`phase-3-fork-patches.md` Appendix A). `@property` is
phase 5's: phase 3 does not implement it (DESIGN).

### 3.18 Deviations from DESIGN.md (recorded here, to be folded into DESIGN.md at merge)
1. Effects use RmlUi **filters** (custom `FilterInstancer`s, filter form) as well as shader
   decorators. Filter/backdrop processing and gradient text are impossible with decorators
   alone.
2. **`@property`** (registered custom properties) is added in phase 5, not phase 3, because
   effect animation needs it. It is a fork patch plus compat. It supersedes phase 2's
   `body#htmlui-root` rewrite for the supported syntaxes, gated by
   `ForkFeature::RegisteredProperties`.
3. **One `ShaderType::UI_EFFECT`** for all effects, with identity in the custom-shader id
   (library ids `@effects/<name>`), instead of one `ShaderType` per effect. This avoids about
   80 enum values and table rows, and reuses fork persistence and export.
4. **Stateful simulations are excluded** (section 2). Cursor trail and ripples are rebuilt on
   CPU state.
5. **Colors are linear sRGB**, not shaders.com's default p3-linear.
6. **Two small RmlUi fork patches** in phase 5: `-jfx-text-fill` (gradient text) and the
   `@property` var lookup (item 2). Both are recorded in `FORK.md`.
7. **Phase 2 behaviors extended**, not contradicted: `background-blend-mode` and gradient text
   (`background-clip: text`) become exact for effect layers. Phase 2's reports and
   approximations remain for gradient and `url()` layers.

## 4 Work packages

Worktrees: `/var/lib/orquester/workspaces/jaspersito/JasperEngine-2-wt/<slug>`, branch
`hui/<slug>`, following BRIEF.md.
- **Wave 1:** `p5-infra` and `p5-property`, in parallel.
- **Wave 2:** `p5-tooling`, `p5-fx-paint`, `p5-fx-light` and `p5-fx-filters`, in parallel,
  after both wave-1 branches are merged into `html-ui`.

Generated files are `BuiltinEffectManifests.gen.cpp`, `html-ui-effects.md`, `img/effects/*`
and `LICENSE.shaders-com.txt`. They are never hand-merged: on conflict, rerun
`tools/htmlui/gen_effect_manifests.py` and `tools/htmlui/gen_effects_docs.py`.

### 4.1 `p5-infra` (wave 1)
**Goal.** Build the complete effect system end to end, plus five reference effects that
exercise every path:
- paint: `stripes`
- pointer: `spotlight`
- trail: `cursor-trail`
- filter: `pixelate`
- backdrop + prepass: `frosted-glass`

**Owned.**
- `engine/core/htmlui/effects/**`: `EffectTypes.h`, `EffectManifest.{h,cpp}`,
  `EffectValue.{h,cpp}`, `EffectSyntax.{h,cpp}`, `EffectRegistry.{h,cpp}`,
  `EffectInstancers.{h,cpp}`, `EffectRuntime.{h,cpp}`, `EffectRenderer.{h,cpp}`,
  `EffectCompat.{h,cpp}`, `BuiltinEffectManifests.gen.cpp`.
- `engine/shaderlib/effects/`: `effect_paint.vert`, `effect_filter.vert`, `lib/jfx_*.glsl`
  (the complete API of 3.9), `lib/main_paint.glsl`, `lib/main_filter.glsl`, `_bright.frag`,
  `stripes.frag`, `spotlight.frag`, `cursor-trail.frag`, `pixelate.frag`, `frosted-glass.frag`,
  `LICENSE.shaders-com.txt`.
- `tools/htmlui/gen_effect_manifests.py`, `gen_effects_docs.py`, `gen_testcard.py`,
  `kit_reference.py`.
- `engine/tests/htmlui/effects/**`: unit tests, the golden driver, fixtures (`paint.html`,
  `filter.html`, `backdrop.html`, `text.html`, `testcard.png` from `gen_testcard.py`) and RCSS
  expectation files.
- `engine/tests/htmlui/golden/effects/` for the five reference effects.
- `docs/mcp/guide/html-ui.md` "Effects" section, all subsections except "Effects in the
  editor and MCP" (P5-tooling); `docs/mcp/guide/html-ui-effects.md`; `docs/mcp/guide/img/effects/`.
- `ui-effects` subcommand in `editor/cli/CommandLine.cpp`.

**Additive edits (listed in the report).**
- `Render.h`, `ShaderPool.{h,cpp}`, `ShaderVariantCompiler.{h,cpp}`,
  `RuntimeShaderCompiler.cpp`, `editor/shader/ShaderBuilder.cpp`, `ShaderUniforms.{h,cpp}`,
  `CoreClassesLua.cpp` (all per 3.8).
- `RmlRenderer` hooks (3.7), and C2–C6 where missing.
- Phase 2 files (C8):
  - the handler registry in `css/props/Rewriters.h` and its dispatch in `Background.cpp` and
    `Effects.cpp` (including the gradient-text hand-off for effect layers and the
    `background-blend-mode` exception);
  - `Transition.cpp` entries;
  - the `paint()` suggestion on `HTMLUI-CSS-0015`;
  - `DiagArea::FX` and the `HTMLUI-FX-*` rows in `DiagnosticCodes.cpp`;
  - the `ForkFeatures` entry `TextFillFilter`;
  - support-table rows.
- `HtmlUIClassesLua.cpp`, `UIDocument.{h,cpp}` and `UIElement` for 3.12.
- `NOTICE.md`.
- `engine/CMakeLists.txt` source list and the `jasperengine-htmlui-tests` source list.
- `docs/mcp/guide/README.md` row.

**Deliverables.**
1. Everything in 3.2–3.10, 3.12, 3.13 (diagnostics, CLI, docs generator) and 3.16 (notice
   file and headers).
2. The runtime fallback `effect` instancers.
3. The golden driver, which auto-enumerates `EffectRegistry::all()` and enforces 5.2 and 5.3
   for every registered effect.
4. A guide section with the six examples from 3.2, all rendered in tests.
5. `tools/htmlui/kit_reference.py`: a pure-Python port of the kit's reference math
   (`hash22`, `perlin12`, MaterialX `mxNoiseFloat3`, `linearToSrgb`, `rgbToOklab`, `fbmPerlin`).
   `EffectLibTest` renders a test shader that evaluates the include library at fixed inputs
   and compares the results with these values (tolerance 1/255 after 8-bit encoding). This
   proves the library ports before the families depend on it.
6. The fork patch `rmlui: -jfx-text-fill applies before filter` in `ElementEffects.cpp`
   (InstanceEffects and the `has_filter` check), recorded in `FORK.md`, plus the registration
   of `-jfx-text-fill`. It is gated by a new `ForkFeature::TextFillFilter`: its table entry is
   flipped by P5-infra, and its probe checks that a `-jfx-text-fill: jfx-pixelate(…)` element
   reports one compiled filter. Without the feature, compat falls back to phase 2's
   solid-color approximation.

**Tests.** 5.1 (everything except `RegisteredPropertyTest`), 5.2/5.3 for the five reference
effects, 5.6 for the five, a Vulkan golden run, and `test_protocol.py` + `test_ui_anchors.py`.

#### p5-infra as implemented

Built as specified (3.2–3.10, 3.12, 3.13, 3.16, deliverables 1–6), with these differences. Wave 2 builds on them.

- **Effects are image layers of the fork's native lists.** With `ForkFeature::BackgroundLayers` (present), compat emits an
  effect layer into the native `background-image`/`mask-image` list (`jfx-<name>(…)` among gradients and `url()`
  layers, its paint area from the layer's `background-clip`); the `decorator` property path with a paint-area keyword is
  kept for a fork without the feature (`EffectCompatTest.LegacyDecoratorsWithPaintAreas`). `transition: background-image`
  keeps its name (no mapping to `decorator`). Two fork patches follow from this, recorded in `FORK.md`: **P5-2**
  `Decorator::PaintsOwnClip` (the fork clip-masked rounded decorator layers in a pushed layer; effects clip themselves in
  the shader, so their blend sees what is below and `@geometry bounds` glows are kept) and **P5-3**
  `DecoratorInstancer::InterpolatesAsImageLayer` (image lists were discrete; opted-in decorator layers interpolate
  property by property, and the interpolated layer's text carries the values). P5-1 is as specified; `-jfx-text-fill` is
  registered after `ReserveUnnamed`.
- **Shader interface.** `effect_filter.vert` is vertexless and has no `u_vs_effectFilter` block and no `v_layerPx`
  varying: `v_texcoord` is top-down on every backend (the vertex shader flips it under `IS_GLSL`) and the filter form
  computes the fragment's layer px as `v_texcoord * frame.layer.xy` (`jfx_layer_px()`). `frame.misc.w` is 1 when the
  paint form clips to the area's rounded box (the spec left it 0): `main_paint.glsl` multiplies the coverage of
  `jfx_box_sdf` over `radii`. `JfxContext` also has `sinceEnter` (`frame.pointerEx.w`).
- **Palettes and reflection names.** SPIRV-Cross flattens uniform blocks on GL, and an array member cannot be passed to
  a function whole: an effect with a palette param `p` writes `JFX_DECLARE_PALETTE(p)` after its params block, which
  defines `jfx_palette_p(t, space)` (copies the array). SPIRV-Cross renames a member named like a GLSL builtin
  (`noise` becomes `_noise`): `checkReflection` and `EffectRenderer` match a member with or without the `_` prefix.
- **ShaderPool.** `expandShaderSpec("uieffect/all")` scans the embedded library (`effects/*.frag`, the kind from the
  `// @kind` line), so the CLI expands it without the registry; `resolveUiEffect` falls back to the library too.
  Library ids are `@effects/<name>`, basenames `fx_<name with - as _>` (`fx__bright`); `ShaderUniforms::writeUniformArray`
  packs std140 arrays (palettes, the trail).
- **RmlRenderer.** The `jfx` shader and filter handles are the instance address with bit 0 set (the handle owns the
  instance); `EffectRenderer` (one per `RmlRenderer`) draws them, the filter passes through `RmlFilterContext`.
  `EffectRuntime::beginFrame` runs from `RmlRenderer::beginFrame`, keyed on `SystemRender::getFrameCount()`; an animated
  effect calls `RequestNextUpdate(0)` while the clock is not fixed.
- **Runtime details the spec left open.** Trail: a sample per frame when the pointer moved 1 px or more, at most 32,
  dropped after 4 s; ripples: a press inside adds one (strength 1), `trigger move` adds one per 40 px moved inside
  (strength 0.6, the movement counted from where the pointer entered), at most 8, dropped after 4 s; a new ripple is as
  old as its press. A long frame counts 0.25 s at most for integrated clocks, trails and ripples.
- **Compat.** The function and at-rule handlers are registered on first use (`registerEffectCompatHandlers`, called by
  the registry lookups in `Rewriters.cpp`), so `check --ui` works without RmlUi. `@effect` is handled in the flatten
  pass (an `@effect` inside a style rule is a warning). A vendor-prefixed image property (`-webkit-mask-image`) takes
  effects. A missing `@effect` file reads `'<src>' not found (no file at <asset path>)` (FX-0013).
  `UIElement::getEffects` reports a gradient-text effect as property `background-image`, form `filter`.
  `ForkFeature::RegisteredProperties` is left to p5-property.
- **CLI.** `ui-effects --json` prints `{"effects": [...]}` (with `--validate`, also `"problems"`); an entry has every
  manifest field (`name kind category summary source cost backdrop pointer time suggestFor params builtin path example
  thumbnail`, and `prepass`/`margin`/`geometry` when set); a param has `name type default range description uniform`
  (`min`/`max` normalised, `options`). `--project <dir>` adds every `.frag` of the project whose manifest starts with
  `// @effect` (declared or not). `shaders`/`export --shader` take `uieffect/<name>[:Flt]` and `uieffect/all`.
- **Docs generator.** `gen_effects_docs.py` reads the manifests from the `.frag` files with the registry's grammar
  instead of running `ui-effects --json` (its `--check` needs no build); it also copies the thumbnails and writes
  `LICENSE.shaders-com.txt` (which lists `original` effects that follow the kit, `frosted-glass`, and the include
  library too).
- **Guide examples** are the six of 3.2 adapted to the reference effects (`grain` and `holographic` become `stripes`,
  `noise-dissolve` becomes `pixelate`, `conic-spin` becomes `stripes` with `angle var(--spin)`). The gradient-text
  example uses `color: transparent`: by 3.2's own trigger, `color: #fff` drops the clip. `EffectGuideTest` renders
  every css block of the section; the project effect of "Writing an effect" is the fixture
  `fixtures/effects/fx/my-glow.frag`, which must equal the guide's block. `test_html_ui_docs.py` writes a `.frag` a
  guide snippet declares from that block.
- **Tests added beyond 5.1:** `EffectGuideTest`, `EffectShaderBuildTest` (5.6 for every built-in effect, in process),
  and the fork's `EffectHooks.*` (run by `run-rmlui-tests.sh`). `EffectLifecycleTest` counts the renderer's live
  geometry, textures, geometry buffers and sokol buffers and images. The golden driver's gradient-text goldens are
  listed in `kTextEffects` of `EffectGoldenTest.cpp` (`stripes`; wave 2 adds its own).
- **Tools not in the owned list:** `tools/htmlui/xcode_sync_effects.py` (the effects sources in the Xcode project, as
  `xcode_sync_compat.py` does for compat).

### 4.2 `p5-property` (wave 1)
**Goal.** Registered custom properties (3.11), so that transitions and keyframes animate
typed custom properties and every `var()` follows.

**Owned.**
- RmlUi fork patch (`engine/libs/rmlui/Source/Core/ElementStyle.{h,cpp}`,
  `Include/RmlUi/Core/StyleSheetSpecification.h`, `Source/Core/StyleSheetSpecification.cpp`)
  as a separate `rmlui:` commit, with a `docs/html-ui/FORK.md` entry.
- `engine/core/htmlui/compat/CompatAtProperty.{h,cpp}` and its rows in the compat at-rule
  table and support matrix.
- Additive edits:
  - phase 2's `css/AtRules.cpp` `@property` branch, which delegates to `CompatAtProperty` when
    the feature is present;
  - the `ForkFeatures.{h,cpp}` entry and probe for `RegisteredProperties`;
  - the three `HTMLUI-AT-*` codes in `DiagnosticCodes.cpp`.
- `engine/tests/htmlui/property/**` and `engine/tests/htmlui/golden/property/**`.
- The `docs/mcp/guide/html-ui.md` subsection "Animating custom properties (@property)".

**Deps.** Phases 0–4. Not P5-infra: its tests use `conic-gradient(from var(--spin), …)` and
custom properties on standard properties (`opacity: var(--o)`). The `conic-spin` golden is
added by P5-fx-paint.

**Deliverables.** 3.11 complete: compat registration and diagnostics, the fork patch with var
lookup, serialisation and dirty propagation, and hoisting.

**Tests.** `RegisteredPropertyTest`: parse/registration, conflict, unsupported syntax,
inherited and non-inherited var lookup, transition midpoint value, keyframes at fixed times,
anime-driven registered property, unregistered fallback; plus goldens.

#### 4.2.1 `p5-property` as implemented

Registered custom properties as Chromium 153 has them (each behaviour below was read from the pinned
Chromium with `getComputedStyle` before it was built). Where the code differs from 3.11 (also in
`CONTRACTS.md`):

- **Where compat reads `@property`.** Phase 2's branch is in the flatten pass of
  `css/CssRewriter.cpp` (not `css/AtRules.cpp`); with `ForkFeature::RegisteredProperties` it calls
  `CompatAtProperty::take`, which records the rule, drops it, or hands it back to the root-rule
  emulation (`Outcome::Fallback`). `CompatAtProperty::finish` (after the flatten pass, before the
  pre-pass) puts the registrations in the rewrite result and their initial values in the custom
  property index. Every `@property` of every sheet is known before any declaration is rewritten
  (hoisting: a rule after the declarations that use the name applies to them, as in CSS).
- **Registration at load, not in the compile.** The compile is pure (no RmlUi; `check --ui` and the
  cache run it off the main thread), so it records `CompatDocument::registeredProperties` and
  `DocumentLoader::load` (and `restyle`, and `check --ui`'s RmlUi verification) registers them
  before RmlUi parses the sheets and the `style` attributes (`CompatAtProperty::registerAll`).
  Conflicts are found there: `at-property-conflict` is a load-time diagnostic of the document
  (`UIDocument::getDiagnostics`, the Console), not a `check --ui` finding.
- **Fork API.** `StyleSheetSpecification::RegisterCustomProperty(name, RegisteredPropertySyntax,
  inherits, initial)` registers, adds the parser and marks in one call;
  `MarkRegisteredCustomProperty(id, registration)` takes the registration (syntax, inherits, initial
  value) besides the id. Each syntax has a parser of its own (`registered_<syntax>`), stricter than
  the spec's mapping: `<percentage>` takes percentages only (not `length_percent`), `<angle>` no
  unitless zero, `<integer>` integer literals (an expression is rounded), one token or one math
  function (RmlUi's number parser read `1px 2px` as `1px`). `<time>` is a fork parser reading `s`
  and `ms` into seconds: compat writes the value as declared (no `ms` conversion).
- **`*` is registered** (3.11 sent it to the fallback): Chromium honours `inherits: false` and the
  optional `initial-value` for it, and transitions it discretely with `allow-discrete`.
  `<transform-function>`, `<transform-list>`, combinations (`|`) and lists (`+`, `#`) fall back with
  `at-property-unsupported-syntax`.
- **Values that do not match** (not in 3.11): as in CSS, a declaration of a registered property is
  valid at parse time whatever its value; one that does not match the syntax (declared, or after
  `var()` substitution) is invalid at computed-value time: `unset`, the initial value (`inherits:
  false`) or the parent's value. The fork keeps it as written (`Unit::VAR_EXPRESSION` without
  `var(`). Compat reports the ones it can check statically with `at-property-invalid` as a
  **Warning** (the code is "by case": Error for an invalid rule).
- **Invalid rules** (3.11 named only a missing `initial-value`): as Chromium, also a missing or
  unquoted `syntax`, an invalid syntax string (data type names are case-sensitive), a missing or
  non-boolean `inherits`, an initial value with `var()`, with font-relative units, or of another
  type, a name that is not `--*`, and `@property` nested in a style rule. Viewport units and
  `currentcolor` are accepted as initial values (Chromium does).
- **Several rules of one name in a document**: the last valid one wins (Chromium); `at-property-
  conflict` is only for a name registered before with another definition (process-global, the first
  wins, until RmlUi shuts down: in the editor, which keeps it while any document is loaded, until
  the editor restarts). A hot reload that changes a definition gets the conflict: the guide says to
  rename the property while iterating on its definition.
- **Conditional group rules**: Chromium registers a rule inside `@media`/`@supports` while the
  condition holds. Compat evaluates `@supports` statically (as for every rule) and `@media` against
  the compile's viewport, resolution and themes, and sets `dependsOnViewport` so a resize compiles
  again; a registration is never undone (process-global).
- **Serialisation for `var()`**: numbers in the shortest decimal text that reads back as the same
  float (no exponent below 1e9), lengths in px against the element that declares the value (the
  inherited value is the parent's pixels), percentages with `%`, angles in deg, times in s, integers
  rounded (halves up), colours `#rrggbbaa`, `currentcolor` kept (it resolves where it is used, as in
  Chromium). `Element::GetProperty("--x")` returns the computed value too.
- **Found in the fork while building it** (all in P5-4, FORK.md):
  - `transition: all` and the `all` shorthand leave custom properties alone (Chromium; CSS Cascade
    4): a registered property transitions only when the list names it.
  - transitions' after-change values read registered properties without animation layers
    (the scoped `ElementStyle::declared_only`), so a keyframes-animated `--x` never starts the transitions
    of the properties that read it (phase 3's rule for ancestors' animations);
  - a transition of a registered property that starts, reverses or ends recomputes, in the same
    style pass, the properties whose `var()` read it (the second `rmlui:` commit; a discrete `*`
    transition showed its end value until it flipped);
  - a `var()` in `font-size` or `line-height` whose variable changed makes their pre-pass compute
    them (they were computed only when dirty themselves; unregistered variables too);
  - `var()` cycles through registered properties make every member invalid at computed-value time
    (an unregistered member is the guaranteed-invalid value), and a `var()` of a custom property that
    is guaranteed-invalid (in a cycle, or whose own `var()` fails) takes its fallback, as in CSS
    (before, the property that read it failed with it; unregistered variables too).
- **Gradients with `var()`** (compat, `css/props/Background.cpp`, additive): with the fork's
  background layers, `conic-gradient(from var(--spin), …)`, a `linear-gradient(var(--a), …)` whose
  variable is a registered `<angle>`, and `var()` stop positions are written for the fork to
  substitute; they were dropped as gradients compat could not write (`HTMLUI-CSS-0004`).
- **`transition-property` names** keep their case for custom properties (`css/props/Transition.cpp`,
  additive; `--Spin` is not `--spin`).
- **Capacity**: registered properties take ids from RmlUi's 256 property ids (the engine uses the
  others: 59 are free, `RegisteredPropertyTest.PropertyIdCapacity`); a registration past the last free
  id stays unregistered with a warning
  (`at-property-unsupported-syntax`). `PropertyIdSet` was not widened: it is in every style hot path.
- **No Lua `CSS.registerProperty`**: 3.12 has none; `anime` animates registered properties as typed
  values (C++ `Anime`, Lua `anime.animate(el, { ["--x"] = … })`).
- **Fixtures and goldens**: the browser-parity fixtures are `fixtures/p3/property/*` (area `property`
  of `fixtures/p3/manifest.json`, owner `p5-property`), because `FixtureRunner`, `BrowserParity` and
  `render-reference.mjs --fixtures property` read that tree; Chromium's references are
  `golden/browser/property/`, the engine's goldens of the same captures `golden/property/`
  (`RegisteredPropertyGolden`). The `conic-spin` effect golden stays P5-fx-paint's.

### 4.3 `p5-tooling` (wave 2)
**Goal.** Editor and shipping integration, end-to-end and performance verification.

**Owned.**
- `editor/shader/ShaderVariantCollector.cpp` (UI-effect collection, 3.8).
- `editor/Exporter.cpp` (license copy, 3.16).
- `editor/window/dialog/ExportWindow.cpp` (UI Effect in Add Shader).
- `editor/util/ProjectChecks.cpp` (project effects in `check`, 3.10).
- `editor/mcp/tools/UIEffectTools.cpp` (new: `list_ui_effects`, `set_ui_effect_time`) and its
  registration.
- Additive edits to phase 4's `inspect_ui` tool (`effects` key, `InspectElement::effects`),
  phase 1's `UIDocumentSystem::reloadChangedFiles` (calls
  `EffectRegistry::onProjectFileChanged` for changed `.frag` files, 3.10), phase 4's
  `w4::syncHtmlUi` trigger extensions (`.frag`), phase 4's
  `test_budget` caps (3.13), `gen_tool_docs.py` `MODULES` (`UIEffectTools`), the HTML-UI
  section of `McpInstructions.cpp` (one line naming the two tools), and the MCP `write_file`
  shader-compile feedback.
- `tools/mcp/tests/test_html_ui_effects.py`.
- `engine/tests/htmlui/effects/EffectPerfTest.cpp`.
- `tools/htmlui/check_effect_shaders.sh`.
- `docs/mcp/TOOLS.md` (regenerated).
- The `html-ui.md` subsection "Effects in the editor and MCP".

**Deps.** P5-infra merged.

**Deliverables.** Export of a fixture project with UI documents ships exactly the effect
variants it uses (both forms where used), plus the notice. The remaining deliverables are
build-settings round-trip of `uieffect/<name>` additions, hot reload of project effects, and
5.4–5.7.

**Tests.** 5.4, 5.5, 5.6, an export test (a CLI `export` of a fixture with effects; asserts
the `.sdat` list and the license file), `test_checks.py`, `test_autosync.py`,
`test_protocol.py` and `test_ui_anchors.py`.

### 4.4 `p5-fx-paint` (wave 2)
**Goal.** Port or author the 28 paint effects of Appendix A.2: gradients, noise, patterns,
energy and space.

**Owned.** `engine/shaderlib/effects/<name>.frag` for those effects,
`engine/shaderlib/effects/lib/fam_paint_*.glsl`, their goldens, and their regenerated generated
files.

**Deliverables.** Per effect:
- the manifest exactly as Appendix A;
- a faithful port (3.15 procedure);
- the `.t1` golden, the `.t2` golden when animated, and the `.text` golden for `aurora`,
  `conic-spin`, `gradient-pan` and `plasma`;
- its compare image reviewed against `cover.jpg`;
- a measured cost class consistent with the declaration (5.4 cost table).

**Tests.** 5.2/5.3 (auto-enumerated), 5.6 for its effects, the 5.4 cost table for its effects,
and the `--validate` CLI.

### 4.5 `p5-fx-light` (wave 2)
**Goal.** The 24 effects of Appendix A.3: light, shine, holographic, edge, light filters,
cursor ripples and transitions.

**Owned.** Their `.frag` files, `lib/fam_light_*.glsl`, goldens and regenerated files.

**Deliverables.** As in 4.4. Also `.text` goldens for `holographic` and `sheen`, and the
transition invariants I4 for all 8 transitions.

**Tests.** As in 4.4.

### 4.6 `p5-fx-filters` (wave 2)
**Goal.** The 26 effects of Appendix A.4: blur, stylize, color, glass and distortion filters.

**Owned.** Their `.frag` files, `lib/fam_filters_*.glsl`, goldens and regenerated files.

**Deliverables.** As in 4.4. Backdrop goldens for every `backdrop: yes` effect. Edge modes are
verified on one distortion per mode (`wave-distortion` with stretch, transparent, mirror and
wrap goldens).

**Tests.** As in 4.4.

## 5 Test plan

Commands:
- C++: `HTMLUI_TEST_ARGS="--gtest_filter=Effect*:RegisteredProperty*" JOBS=3 tools/build.sh htmlui-tests`
  (phase 0's runner, Docker + Xvfb; `HTMLUI_BACKEND=vulkan` for the Vulkan run). Test files in
  `engine/tests/htmlui/effects/` and `property/` need one glob line each in
  `engine/tests/htmlui/CMakeLists.txt` (phase 0's glob is not recursive).
- Goldens: the same command with `JASPER_HTMLUI_UPDATE_GOLDEN=1` (glcore only) creates them;
  names carry the subdirectory (`effects/<name>.t1`), compared by phase 0's
  `compareWithGolden` with its default tolerance.
- MCP: `tools/mcp/run-tests.sh tools/mcp/tests/test_html_ui_effects.py` with
  `JASPER_FIXTURE=/var/lib/orquester/workspaces/jaspersito/JasperEngine-2/build/fixture-unity`.

### 5.1 Unit tests (`engine/tests/htmlui/effects/`)
- **`EffectSyntaxTest`**
  - names and arguments, case folding, whitespace, commas inside `rgb()`/`var()`;
  - trailing comma, empty `effect()`;
  - positional argument, duplicate param, offsets for `line:col`.
- **`EffectValueTest`**, every type in 3.3:
  - all angle units and bare degrees, `s`/`ms`, every length unit including `%` of the
    shorter side;
  - colors in every CSS form, including `rgb(0 0 0 / 50%)`, `#rgba`, `hsl()`, `oklch()`,
    `transparent` and `currentColor`;
  - positions in 1, 2 and 4-value forms;
  - enums (case-insensitive) and booleans;
  - palettes: implicit positions, decreasing positions rejected, 1 entry and 9 entries
    rejected;
  - clamping with a diagnostic, canonical output round-trips, `var()` passthrough.
- **`EffectManifestTest`**:
  - every directive and default;
  - errors with line numbers: unknown directive, bad type, a default out of range, a missing
    `@kind`, a `main_*` include that does not match the kind, duplicate params, a reserved
    name, `_` prefix;
  - `@scaled` grammar.
- **`EffectCompatTest`**. Exact RCSS text for:
  - single and multi-layer backgrounds mixing effect, gradient and url layers (order);
  - per-layer `background-clip` → paint area;
  - `background-blend-mode` mapping;
  - `background-clip: text` lifting and the forced text color;
  - `mask-image`;
  - `filter`/`backdrop-filter` lists mixing standard functions;
  - `transition-property: background-image` → `decorator`;
  - `@effect` hoisting.

  It also covers every diagnostic code in 3.13: code, severity, `line:col` and suggestion
  text, including `glassmorphism` → `frosted-glass`, `sparkle` in `background-image` →
  `glints`, and `speeed` → `speed`. Plus the Lua `style.backgroundImage` path.
- **`EffectRegistryTest`**:
  - `BuiltinEffectManifests.gen.cpp` equals the generator output over the current shaderlib
    (stale → fail);
  - unique names;
  - project declaration: ok, conflict, missing file, manifest error at the `.frag` line, name
    mismatch;
  - graveyard on re-register;
  - `getCustomShaderGeneration` re-resolution.
- **`ShaderKeyTest`**:
  - `getShaderStr`/`parseShaderStr` round-trip for library and project ids, `Flt`
    bit/name/mask;
  - `parseShaderSpec`/`expandShaderSpec` (`uieffect/all` count = 2×paint + filter + 1 internal);
  - fragment-only custom paths and include root;
  - `isLibraryCustomShader`;
  - Stream encode/decode of `@effects/…` keys.
- **`EffectRuntimeTest`**:
  - the fixed clock and the integrated-clock formulas;
  - no jump when `speed` changes live: `anim` is continuous within dt·max rate;
  - state keying across re-instancing, the 16-state LRU, purge on element destroy and on
    document unload;
  - pointer mapping under translate, scale and rotate transforms (±0.5 px);
  - smoothing and presence timings;
  - trail ordering, ageing and 4 s expiry;
  - ripple events by press and movement;
  - seed stability.
- **`EffectAnimationTest`**:
  - `transition: background-image 1s` midpoint gives `getEffects()` params at 50% (fixed
    RmlUi clock);
  - `@keyframes` with filter effects;
  - `var()` change → one re-instance and zero compile calls;
  - anime animating `--x` → params follow;
  - a different effect switches discretely.
- **`EffectLibTest`**: include-library functions against `tools/htmlui/kit_reference.py`
  values (4.1 deliverable 5).
- **`EffectLifecycleTest`**: 100 cycles of loading and unloading a document with all five
  reference effects. Afterwards live instances are 0, phase 1's GPU object counters are back
  to baseline and the element-state map is empty.
- **`RegisteredPropertyTest`**: 4.2.

### 5.2 Golden images (`engine/tests/htmlui/golden/effects/`)
The driver auto-enumerates all built-in effects and pins the fixed clock (C1 + `EffectClock`).

| Fixture | Used for | Layout |
|---|---|---|
| `paint.html` | paint effects | canvas 360×240, body `#101218`, `#fx` 320×200 at (20,20), `border-radius: 12px`, `background-image: effect(<name>)` |
| `filter.html` | filter effects with `backdrop: no` | `#fx` 320×200 test card: "Effects 123" 28px bold white, four 40px squares `#e74c3c #f1c40f #2ecc71 #3498db`, a gradient bar, and `testcard.png` 160×100; `filter: effect(<name>)` |
| `backdrop.html` | filter effects with `backdrop: yes` | full-canvas test card behind a 220×140 panel at (70,50), radius 16px, `backdrop-filter: effect(<name>)` |
| `text.html` | listed paint effects in filter form | 48px bold white "JASPER" with `background-image: effect(<name>); background-clip: text` |

- Files are `<name>.t1.png` (t = 1.5 s, every effect) and `<name>.t2.png` (t = 4.0 s, effects
  whose t1 and t2 renders differ).
- Pointer effects use a test pointer at (70%, 40%) of `#fx`, pressed 0.3 s before the capture.
  Trail effects use a synthetic 12-point arc over the last 0.5 s.
- Transitions use `progress 40%`.
- Tolerance is phase 0's. Every changed image is inspected before regenerating (BRIEF).

### 5.3 Invariants (auto-enumerated, every effect)
- **I1.** `opacity 0` equals the render without the effect (max channel diff ≤ 1).
- **I2.** Two renders at the same fixed time are identical.
- **I3.** If a `.t2` golden exists, the effect is animated (t1 ≠ t2). Otherwise it is static
  (t1 == t2).
- **I4** (transitions). `progress 0%` equals no effect (≤ 1). `progress 100%` makes the area
  alpha ≤ 1/255. `invert on` swaps the two.
- **I5.** 0×0, 1×1, 1×400 and 400×1 elements render with no renderer error.
- **I6.** A paint effect in filter form over fully transparent content leaves alpha 0.
- **I7.** Every effect has a manifest, a golden, a catalogue entry and a `LICENSE` list entry
  when `@source` is shaders.com.

### 5.4 Performance smoke (`EffectPerfTest`, Linux glcore build)
- **Document A.** 1280×720 canvas, 64 elements of 140×90 in an 8×8 grid. 48 are paint
  effects (`stripes`, `spotlight`, `cursor-trail` in rotation) and 16 are filters (12
  `pixelate`, 4 `frosted-glass` backdrop).
- **Document B.** The same layout with only `background-color`.
- **Run.** 30 warm-up frames, then 120 measured frames with a live clock. The metric is the
  median CPU time of `UIDocumentSystem` update + render submission.
- **Asserts:**
  - in steady state, per frame: `instancesCreated == 0`, compile calls == 0,
    `pipelinesCreated == 0`;
  - CPU(A) ≤ 1.5 × CPU(B) + 1.0 ms;
  - Variant A2 (every element's param driven by `var(--k)` updated each frame): exactly 64
    re-instances per frame, 0 compiles, and CPU(A2) ≤ 3 × CPU(B) + 4.0 ms.
- **Cost table.** Every registered effect renders 1024×1024, 20 times, through the golden
  harness (render + readback). Baseline is a `background-color` fill. The test writes
  `build/linux/htmlui-effects-cost.json` and asserts ratio ≤ 6 for `low` and ≤ 24 for
  `medium` (class bound × 1.5); `high` is unbounded.

### 5.5 MCP end-to-end (`tools/mcp/tests/test_html_ui_effects.py`)
1. Write `ui/effects.html` and `ui/effects.css` into a fixture copy, and create a GameObject
   with a `UIDocument` for it. The CSS contains `aurora`, `frosted-glass` backdrop, `pixelate`
   filter, `holographic` text, `sheen` with a hover transition, and a project effect
   `@effect tint-wave { src: url(fx/tint-wave.frag) }` (written by the test).
2. Call `set_ui_effect_time { time: 1.5 }` and screenshot the game view. For each effect
   region, the stdev of the region exceeds a threshold and its mean color is within tolerance
   of the expected value recorded in the test, so a blank or wrong-region render fails.
3. Screenshot again and require it identical (determinism). Then call
   `set_ui_effect_time {time: null}`, wait 0.5 s, screenshot, and require the aurora region
   to have changed.
4. `inspect_ui` lists `effects` for `.hero` with the param values. `list_ui_effects` returns
   ≥ 83 entries, and `category: "glass"` returns `frosted-glass`, `liquid-glass`,
   `fluted-glass` and `glass-tiles`.
5. Write CSS with `effect(glassmorphism)`, `effect(aurora, speeed 2)` and
   `color: effect(plasma)`. Phase 4's `inspect_ui {file}` (the MCP equivalent of
   `check --ui`, sharing `ProjectChecks::checkUiFiles`) returns problems with codes
   `HTMLUI-FX-0001` effect-unknown (suggesting `frosted-glass`), `HTMLUI-FX-0004`
   effect-unknown-param (suggesting `speed`) and `HTMLUI-FX-0002` effect-wrong-context, each
   with its line:col.
6. Hot reload: change `tint-wave.frag`'s color constant and require the next screenshot to
   change. Introduce a GLSL error: `write_file` returns `effect-shader-compile` with the
   `.frag` line. Fix it and the render recovers.
7. Always also run `test_protocol.py` and `test_ui_anchors.py`.

### 5.6 Shader builds for every backend
`tools/htmlui/check_effect_shaders.sh` runs `jasperengine-editor shaders --out <tmp> --backend
all --shader uieffect/all`. It requires exit code 0 and one file per key per backend (GLSL
410, **GLSL 300 es**, HLSL 5, MSL 2.1 macOS/iOS, SPIR-V), and `jasperengine-editor ui-effects
--validate` passes. `JOBS=3 tools/build.sh web` must also build, and the web export of the MCP
fixture project with effects must contain the `glsl300es` variants.

### 5.7 Vulkan
Configure a Linux `GRAPHIC_BACKEND=vulkan` build and run the golden suite. It must pass with
the same goldens and phase 0's tolerance. The filter-form orientation golden (an asymmetric
`pixelate` on the test card) must pass on both glcore and Vulkan.

## 6 Acceptance criteria
- [ ] All 83 effects of Appendix A are registered, with manifests exactly as listed, goldens
      (t1, t2 when animated), docs entries with thumbnails, and cost classes consistent with
      the measured table.
- [ ] `effect()` works in `background-image`, `background`, `mask-image`, `filter` and
      `backdrop-filter`, layered with gradients, url images and standard filters, in CSS
      order. `background-blend-mode` and `background-clip: text` work for effect layers.
- [ ] Every diagnostic code in 3.13 is produced with file:line:col and its suggestion, at load
      (Console) and in `check --ui`. Nothing is dropped silently, including runtime-form
      warnings.
- [ ] Transitions and keyframes interpolate effect parameters. `var()` and the anime API
      drive them. `@property`-registered custom properties transition and animate, and every
      `var()` follows.
- [ ] The fixed effect clock makes renders deterministic (C++, Lua, MCP). Live speed changes
      never jump.
- [ ] Pointer data (position, smoothed, presence, velocity, press, trail, ripples) is
      element-local and correct under transforms.
- [ ] Backdrop effects read and refract what is behind the element, clipped to its rounded
      border box. The blur prepass works.
- [ ] `ShaderType::UI_EFFECT` variants are precompiled and exported: exactly the used ones
      plus Build Settings additions. Shipped games compile nothing. `uieffect/all` builds for
      every backend, including GLSL ES 300. The web build passes.
- [ ] Project effects: `@effect` + `.frag` compile in the editor, report errors with `.frag`
      lines, hot reload, export, and run in an exported game.
- [ ] Performance: 5.4 counters and budgets pass. No GPU object leaks across 100
      load/unload cycles.
- [ ] MCP: `list_ui_effects`, `set_ui_effect_time` and `inspect_ui` effects work.
      `test_html_ui_effects.py`, `test_protocol.py` and `test_ui_anchors.py` pass, and the
      screenshots were looked at.
- [ ] Goldens pass on Linux glcore and Vulkan.
- [ ] Docs: the guide's Effects section and the generated `html-ui-effects.md`
      (`gen_effects_docs.py --check` clean), the support-matrix rows, `TOOLS.md` regenerated,
      and the Lua stubs regenerated.
- [ ] Attribution: `LICENSE.shaders-com.txt` lists every ported effect, ported files carry
      the header, `NOTICE.md` is updated, and exports include the license when they ship
      shaders.com-derived effects.
- [ ] `FORK.md` records the `@property` patch. Each package's report lists its additive edits
      outside its ownership.

## Appendix A: Effect catalogue

Notation:
- Parameters are written `name type default [min, max]`. Types: `n` number, `i` integer, `%`
  percentage, `len` length, `ang` angle, `time`, `col` color, `pos` position, `bool`, `pal`
  palette, `enum{…}` (the default is listed first).
- `(t)` marks an `@time` rate param. B = `@backdrop yes`. Ptr = `@pointer`. Cost = low (L),
  medium (M), high (H).
- `cs` means `color-space enum{linear|oklch|oklab|hsl|hsv|lch}`, with the default given.
- Angles of ported effects keep the shaders.com meaning. For original effects, `direction`
  angles use the CSS gradient convention: 0deg points up, clockwise.
- Universal params (3.2) are not repeated here.
- Sources are folder names under `packages/core/src/shaders/`.

### A.1 Reference effects (P5-infra)
| Effect | Kind | Category | Source | Parameters | B | Ptr | Cost | Extra manifest |
|---|---|---|---|---|---|---|---|---|
| `stripes` | paint | pattern | Stripes | color-a col #000000; color-b col #ffffff; angle ang 45deg [-180deg,180deg]; density n 5 [1,30]; balance n 0.5 [0,1]; softness n 0 [0,1]; speed n 0.2 [-1,1] (t); offset n 0 [0,1]; cs linear | – | none | L | |
| `spotlight` | paint | interactive | original | color col #ffffff; radius len 160px [8px,2000px]; softness n 0.6 [0,1]; intensity n 0.35 [0,1]; idle-position pos "50% 50%"; idle-intensity n 0 [0,1] (eased by presence) | – | position | L | `@suggest-for cursor-light flashlight` |
| `cursor-trail` | paint | interactive | CursorTrail (adapted: CPU trail buffer replaces the fluid grid) | colors pal "#00aaff #ff00aa"; radius len 18px [2px,128px]; lifetime time 0.5s [0.1s,2s] (source `length`); shrink n 1 [0,1]; softness n 0 [0,1]; cs linear | – | trail | L | |
| `pixelate` | filter | stylize | Pixelate (adapted: cell size in px) | size len 8px [1px,256px]; gap n 0 [0,0.95]; roundness n 0 [0,1] | – | none | L | `@suggest-for pixel mosaic` |
| `frosted-glass` | filter | glass | original (Glass kit `glassTint`/`glassBorderHighlight`, `gpu/kit/effects/glass.ts:170-199`) | blur len 12px [0px,64px]; tint col #ffffff1f; saturation n 1.2 [0,3]; brightness n 1 [0,2]; noise n 0.04 [0,0.3]; highlight n 0.35 [0,1]; light-angle ang 300deg [0deg,360deg] | yes | none | M | `@prepass blur blur`; `@margin blur*3`; `@suggest-for glass glassmorphism frost blur-glass acrylic` |

### A.2 `p5-fx-paint` (28 paint effects)
| Effect | Category | Source | Parameters | Ptr | Cost | Extra manifest |
|---|---|---|---|---|---|---|
| `aurora` | gradient | Aurora | color-a col #a533f8; color-b col #22ee88; color-c col #1694e8; cs linear; balance n 50 [0,100]; intensity n 80 [0,100]; curtains i 4 [1,4]; speed n 5 [-10,10] (t); waviness n 50 [0,200]; ray-density n 20 [0,100]; height n 120 [10,200]; center pos "50% 0%"; seed n 0 [0,100] | none | M | `@suggest-for northern-lights borealis` |
| `mesh-gradient` | gradient | MeshGradient | colors pal "#1a0533 0% #6d2fd1 26% #e04b9e 52% #ff8c42 76% #ffdf8e 100%"; cs oklab; points i 5 [2,8]; smoothness n 2 [0,5]; variation n 0.35 [0,1]; swirl n 0.3 [-1,1]; drift n 0.5 [0,1]; wrapping n 0 [0,1]; speed n 1 [0,10] (t); seed n 0 [0,100] | none | M | `@suggest-for mesh gradient-mesh` |
| `flowing-gradient` | gradient | FlowingGradient | color-a col #0a0015; color-b col #6b17e6; color-c col #ff4d6a; color-d col #ff6b35; cs oklch; speed n 1 [0,10] (t); distortion n 0.5 [0,2]; seed n 0 [0,100] | none | M | `@suggest-for liquid-gradient silk` |
| `gradient-pan` | gradient | LinearGradient (adapted: speed scrolls the stops) | colors pal "#1aff00 #0000ff"; angle ang 0deg [0deg,360deg]; speed n 0.2 [-5,5] (t) (axis lengths/s); edges enum{mirror\|stretch\|wrap}; cs linear | none | L | `@suggest-for animated-gradient linear-gradient` |
| `radial-pulse` | gradient | RadialGradient (adapted: speed moves rings outward) | colors pal "#ff0000 #0000ff"; center pos "50% 50%"; radius n 1 [0,2]; repeat n 1 [1,20]; aspect n 1 [0.1,4]; speed n 0 [-5,5] (t); cs linear | none | L | `@suggest-for radial-gradient pulse` |
| `conic-spin` | gradient | ConicGradient (adapted: speed in turns/s) | colors pal "#ff0080 #00bfff"; center pos "50% 50%"; rotation ang 0deg [-3600deg,3600deg]; repeat n 1 [1,24]; speed n 0.1 [-5,5] (t); cs linear | none | L | `@suggest-for conic-gradient spin rotating-border` |
| `diamond-gradient` | gradient | DiamondGradient | colors pal "#4ffb4a #4f1238"; center pos "50% 50%"; size n 0.7 [0.01,2]; rotation ang 0deg [0deg,360deg]; repeat n 1 [1,16]; roundness n 0 [0,1]; cs linear | none | L | |
| `multi-point-gradient` | gradient | MultiPointGradient | color-a col #4776e6; position-a pos "20% 20%"; color-b col #c44dff; position-b pos "80% 20%"; color-c col #1abc9c; position-c pos "20% 80%"; color-d col #f8bbd9; position-d pos "80% 80%"; color-e col #ff8c42; position-e pos "50% 50%"; smoothness n 2 [0,5]; cs linear | none | L | |
| `color-wheel` | gradient | ColorWheel | mode enum{rainbow\|custom}; color-a col #ff0000; color-b col #00ff88; color-c col #0066ff; scale n 1 [0.1,10]; angle ang 0deg [-180deg,180deg]; speed n 0.05 [-1,1] (t); cs oklch | none | L | `@suggest-for rainbow hue-cycle` |
| `plasma` | energy | Plasma | density n 2 [0,4]; speed n 2 [0,5] (t); intensity n 1.5 [0.1,3]; warp n 0.4 [0,1]; contrast n 1 [0,3]; balance n 50 [0,100]; color-a col #7018be; color-b col #000000; cs linear | none | M | |
| `swirl` | gradient | Swirl (adapted: source `blend` renamed `balance`) | color-a col #1275d8; color-b col #e19136; speed n 1 [0,5] (t); detail n 1 [0,5]; balance n 50 [0,100]; cs linear | none | M | |
| `marble` | pattern | Marble | color-a col #ffffff; color-b col #3a2d54; color-c col #0f0f0f; scale n 2 [0.1,10]; turbulence n 10 [0,50]; speed n 0.05 [0,0.25] (t); seed n 0 [0,100]; cs linear | none | M | |
| `strands` | light | Strands | colors pal "#00e5ff 0% #5b8cff 34% #b06bff 67% #ff5fa2 100%"; cs oklab; speed n 0.5 [0,2] (t); amplitude n 2 [0,5]; frequency n 0.3 [0,5]; line-count i 8 [1,16]; line-width n 0.05 [0,1]; softness n 0.05 [0,1]; spread n 0.2 [0,1]; pin-edges bool false; color-scale n 1 [1,6]; color-variance n 1 [0,1]; color-speed n 1 [0,2] (t); start pos "0% 50%"; end pos "100% 50%" | none | M | `@suggest-for ribbons light-strands` |
| `perlin-noise` | noise | PerlinNoise | color-a col #ffffff; color-b col #000000; cs linear; scale n 2 [-2,5]; contrast n 0 [-1,5]; balance n 0 [-1,1]; seed n 0 [0,100]; speed n 1 [0,5] (t) | none | L | `@suggest-for noise` |
| `simplex-noise` | noise | SimplexNoise | as `perlin-noise` | none | L | |
| `fractal-noise` | noise | FractalNoise | color-a col #000000; color-b col #ffffff; octaves i 4 [1,8]; detail n 2 [1,4]; contrast n 0.5 [0.1,1]; speed n 0.15 [0,1] (t); angle ang 0deg [-180deg,180deg]; seed n 0 [0,100]; cs linear | none | M | `@suggest-for fbm clouds` |
| `curl-noise` | noise | CurlNoise | color-a col #ffffff; color-b col #000000; cs linear; scale n 2 [-2,5]; contrast n 0 [-1,5]; balance n 0 [-1,1]; seed n 0 [0,100]; speed n 1 [0,5] (t) | none | M | |
| `voronoi` | noise | Voronoi | color-a col #3186cf; color-b col #fc02dd; color-border col #000000; scale n 6 [1,20]; speed n 0.5 [0,5] (t); seed n 0 [0,100]; edge-intensity n 0.5 [0,1]; edge-softness n 0.05 [0,0.4]; cs linear | none | M | `@suggest-for cells worley` |
| `grain` | noise | original (FilmGrain look, `gpu/kit/colorOps.ts:267`) | amount n 0.12 [0,1]; scale n 1 [0.5,4]; animated bool true; colored bool false; color col #ffffff | none | L | `@suggest-for noise-overlay film-grain-overlay` |
| `dot-grid` | pattern | DotGrid | color col #ffffff; density n 30 [1,200]; dot-size n 0.3 [0,1]; offset n 0 [0,1]; speed n 0 [-3,3] (t); speed-variance n 0.3 [0,1]; twinkle n 0 [0,1] | none | L | `@suggest-for dots polka` |
| `grid` | pattern | Grid | color col #ffffff; cell-color col transparent; cells n 10 [1,50]; thickness n 1 [0,20]; rotation ang 0deg [0deg,360deg]; softness n 0 [0,1]; variation n 0 [0,1]; cs linear | none | L | |
| `hex-grid` | pattern | HexGrid | color-a col #000000; color-b col #ffffff; cells n 8 [1,40]; thickness n 1 [0,10]; rotation ang 0deg [0deg,360deg]; softness n 0 [0,1]; variation n 0 [0,1]; cs linear | none | L | `@suggest-for honeycomb hexagons` |
| `ripple-rings` | pattern | Ripples | center pos "50% 50%"; color-a col #ffffff; color-b col #000000; speed n 1 [-5,5] (t); frequency n 20 [1,80]; softness n 0 [0,3]; thickness n 0.5 [0,1]; phase n 0 [0,6.28] | none | L | `@suggest-for ripples rings` |
| `sine-wave` | pattern | SineWave | color col #ffffff; amplitude n 0.15 [0,1]; frequency n 1 [0.1,20]; speed n 1 [-5,5] (t); angle ang 0deg [0deg,360deg]; position pos "50% 50%"; thickness n 0.2 [0,2]; softness n 0.4 [0,1] | none | L | `@suggest-for wave` |
| `falling-lines` | pattern | FallingLines | color-a col #ffffff; color-b col #ffffff00; cs linear; angle ang 90deg [0deg,360deg]; speed n 0.5 [0,3] (t); speed-variance n 0.3 [0,1]; density n 15 [1,60]; trail-length n 0.35 [0.01,1]; balance n 0.5 [0,1]; stroke-width n 0.15 [0.02,1]; rounding n 1 [0,1] | none | L | `@suggest-for rain matrix` |
| `fire` | energy | original (domain-warped fbm flame; palette after GradientMap `fire`) | colors pal "#00000000 0% #8a1500 30% #ff5a00 60% #ffd27a 85% #ffffff 100%"; speed n 1 [0,5] (t); scale n 1 [0.25,4]; turbulence n 1 [0,3]; height n 0.8 [0.1,1.5]; direction ang 0deg [-180deg,180deg]; seed n 0 [0,100]; cs linear | none | M | `@suggest-for flames burn` |
| `smoke` | energy | original (stateless fbm smoke; look and params after shaders.com Fog) | color-a col #e0e0e0; color-b col #888888; density n 0.6 [0,1]; scale n 1 [0.25,4]; turbulence n 1 [0,3]; speed n 1 [0.1,3] (t); direction ang 0deg [-180deg,180deg]; seed n 0 [0,999]; cs linear | none | M | `@suggest-for fog mist` |
| `starfield` | space | original | color col #ffffff; density n 1 [0.1,4]; layers i 3 [1,4]; speed n 0.05 [-1,1] (t); direction ang 90deg [0deg,360deg]; twinkle n 0.5 [0,1]; size n 1 [0.5,3]; warp n 0 [0,1] (streaks toward the center) | none | M | `@suggest-for stars space galaxy` |

### A.3 `p5-fx-light` (24 effects)
| Effect | Kind | Category | Source | Parameters | B | Ptr | Cost | Extra manifest |
|---|---|---|---|---|---|---|---|---|
| `godrays` | paint | light | Godrays | center pos "0% 0%"; density n 0.3 [0,1]; intensity n 0.8 [0,1]; spotty n 1 [0,1]; speed n 0.5 [0,2] (t); ray-color col #4283fb; base-color col transparent (source `backgroundColor`) | – | none | M | `@suggest-for light-rays crepuscular` |
| `sunburst` | paint | light | SunBurst | color col #ffdd88; base-color col #000000 (source `background`); center pos "50% 50%"; ray-count i 12 [3,64]; softness n 0.3 [0,1]; radius n 0.8 [0,1.2]; feather n 0.5 [0,5]; speed n 0.2 [-2,2] (t) | – | none | L | `@suggest-for rays reward-rays` |
| `light-edge` | paint | light | LightEdge (adapted: shape = element rounded box, sizes in px) | colors pal "#7b2ff7 #00e0ff"; cs linear; thickness len 6px [1px,64px]; softness n 0.5 [0,1]; intensity n 0.6 [0,1]; bloom n 0.3 [0,1]; spots i 3 [1,5]; spot-size n 0.5 [0,1]; pulse n 0 [0,1]; smoke n 0.3 [0,1]; smoke-size n 0.5 [0,1]; speed n 1 [0,4] (t); seed n 0 [0,100]; extent len 24px [0px,128px] | – | none | M | `@geometry bounds extent`; `@suggest-for border-glow glow-border edge-light` |
| `neon-border` | paint | light | Neon (adapted: tube along the border, sizes in px) | color col #00ddff; secondary-color col #ff00aa; secondary-blend n 0.5 [0,1]; glow-color col #00ddff; tube-thickness len 4px [1px,32px]; intensity n 1.5 [0.5,4]; hot-core n 0.6 [0,1]; glow-intensity n 0.6 [0,2]; glow-radius len 16px [1px,96px]; light-angle ang 300deg [0deg,360deg]; specular n 0.5 [0,2]; flicker-speed n 0 [0,5] (t); flicker-amount n 0.2 [0,1]; flow-speed n 0 [0,5] (t); flow-amount n 0.3 [0,1]; extent len 32px [0px,128px] | – | none | M | `@geometry bounds extent`; `@suggest-for neon` |
| `sheen` | paint | light | original | color col #ffffff; angle ang 20deg [-90deg,90deg]; width n 0.25 [0.02,1]; softness n 0.6 [0,1]; intensity n 0.6 [0,1]; duration time 0.8s [0.05s,10s]; period time 3s [0.1s,60s]; delay time 0s [0s,60s] | – | none | L | `@suggest-for shine glint-sweep` |
| `shimmer` | paint | light | original (skeleton loading) | base-color col #d9dce1; highlight-color col #f5f6f8; angle ang 15deg [-90deg,90deg]; width n 0.4 [0.05,1]; duration time 1.4s [0.2s,10s] | – | none | L | `@suggest-for skeleton loading placeholder` |
| `glints` | paint | light | original (paint counterpart of Sparkle) | color col #ffffff; density n 20 [1,200] (per 10 000 CSS px²); size len 8px [2px,64px]; rays n 1 [0,1]; twinkle-speed n 1 [0,5] (t); colorize n 0.3 [0,1]; seed n 0 [0,100] | – | none | M | `@suggest-for sparkles sparkle twinkle` |
| `holographic` | paint | light | Holographic (adapted: shape = element box; tilt follows the pointer) | hue-shift ang 0deg [0deg,360deg]; foil-scale n 1.5 [0.3,6]; saturation n 0.75 [0,1]; roughness n 0.25 [0,1]; speed n 1 [0,2] (t); crinkle n 0.5 [0,1]; crinkle-scale n 1 [0.3,6]; sparkle n 0.4 [0,1]; tilt-follows-pointer bool true | – | position | M | `@suggest-for foil holo iridescent` |
| `thin-film` | paint | light | ThinFilm (adapted: rim along the element edges) | intensity n 1 [0,4]; rim-width n 1 [0,3]; edge-softness n 0.3 [0,1]; thickness n 0.5 [0,1]; dispersion n 0.5 [0,1]; saturation n 1 [0,1]; hue-shift n 0 [0,1]; light-angle ang 300deg [0deg,360deg]; mode enum{rainbow\|custom}; color-a col #2b6fff; color-b col #ffffff; color-c col #ff7a21; cs oklch; speed n 0.1 [-1,1] (t) | – | none | M | `@suggest-for soap-bubble iridescence` |
| `glow` | filter | light | Glow | intensity n 1 [0,50]; threshold n 0.5 [0,1]; size len 25px [0px,100px] | – | none | M | `@prepass blur size*0.5 threshold threshold`; `@margin size*1.5`; `@suggest-for bloom` |
| `sparkle` | filter | light | Sparkle | size n 100 [10,200]; intensity n 10 [0,20]; threshold n 0.2 [0,1]; expand n 0 [0,100]; ray-length n 5 [0,20]; colorize n 0.45 [0,1]; speed n 1 [0,5] (t); seed n 0 [0,100] | – | none | M | |
| `hologram` | filter | stylize | Hologram (adapted: applied to the element's content instead of a shape) | color col #5ec8ff; brightness n 1.5 [0.2,3]; fill n 0.45 [0,1]; edge-glow n 1 [0,2.5]; scanlines n 0.55 [0,1]; scanline-scale n 130 [20,400]; scanline-speed n 1 [0,5] (t); sweep n 0.1 [0,1]; flicker n 0.3 [0,1]; distortion n 0.1 [0,1]; grain n 0.7 [0,1]; speed n 1 [0,3] (t) | – | none | M | `@suggest-for projection sci-fi` |
| `outline` | filter | stylize | original | color col #ffffff; width len 2px [0px,16px]; softness n 0 [0,1]; position enum{outside\|inside\|center} | – | none | M | `@margin width+2px`; `@suggest-for stroke text-stroke` |
| `emboss` | filter | stylize | Emboss (adapted: relief from the content alpha) | depth n -0.5 [-1,1]; light-angle ang 260deg [0deg,360deg]; light-intensity n 0.6 [0,2]; shadow-intensity n 0.3 [0,1]; bevel len 3px [0.5px,16px] | – | none | L | `@suggest-for bevel relief` |
| `light-leak` | filter | light | LightLeak | position pos "95% 40%"; spread n 0.55 [0.1,1.5]; intensity n 0.15 [0,1]; streaks n 0.6 [0,1]; color-hot col #fff3c4; color-mid col #ff7a2f; color-fringe col #a63d8f; flicker n 0.35 [0,1]; speed n 1 [0,4] (t); seed n 0 [0,100] | – | none | L | |
| `cursor-ripples` | filter | interactive | CursorRipples (adapted: analytic ripple events replace the wave grid, `CursorRipples/index.ts:12-26`) | intensity n 10 [0,20]; decay n 3 [0.5,20] (1/s); wavelength len 28px [4px,200px]; wave-speed n 300 [50,2000] (CSS px/s); chromatic-split n 1 [0,3]; trigger enum{move\|press}; edges enum{stretch\|transparent\|mirror\|wrap} | – | ripples | M | |
| `noise-dissolve` | filter | transition | NoiseDissolve (+ original burn edge) | progress % 50%; scale n 3 [0.5,20]; softness n 0.25 [0,1]; seed n 0 [0,100]; invert bool false; edge-color col transparent; edge-width n 0.05 [0,0.3] | – | none | L | `@suggest-for dissolve burn` |
| `block-dissolve` | filter | transition | BlockDissolve | progress % 50%; block-size n 0.08 [0.01,0.5]; softness n 0.15 [0,1]; invert bool false | – | none | L | |
| `linear-wipe` | filter | transition | LinearWipe | progress % 50%; angle ang 0deg [0deg,360deg]; feather n 0.1 [0,1]; invert bool false | – | none | L | `@suggest-for wipe` |
| `radial-wipe` | filter | transition | RadialWipe | progress % 50%; start-angle ang 0deg [0deg,360deg]; direction enum{cw\|ccw\|both}; center pos "50% 50%"; feather n 0.1 [0,1]; invert bool false | – | none | L | `@suggest-for clock-wipe` |
| `iris-wipe` | filter | transition | IrisWipe | progress % 50%; center pos "50% 50%"; feather n 0.1 [0,1]; invert bool false | – | none | L | `@suggest-for circle-wipe` |
| `diamond-wipe` | filter | transition | DiamondWipe | progress % 50%; size n 0.15 [0.02,0.5]; feather n 0.1 [0,1]; invert bool false | – | none | L | |
| `venetian-blinds` | filter | transition | VenetianBlinds | progress % 50%; angle ang 0deg [0deg,360deg]; strip-count i 5 [2,60]; feather n 0.15 [0,1]; invert bool false | – | none | L | `@suggest-for blinds` |
| `ripple-wipe` | filter | transition | RippleWipe | progress % 50%; center pos "50% 50%"; rings i 8 [2,40]; feather n 0.2 [0,1]; invert bool false | – | none | L | |

Transition semantics, which every port must follow and I4 verifies: `progress 0%` leaves the
content fully visible, and `100%` removes it entirely. `invert` swaps the two. If the
shaders.com source uses the opposite convention, the port flips it.

### A.4 `p5-fx-filters` (26 filter effects)
| Effect | Category | Source | Parameters | B | Ptr | Cost | Extra manifest |
|---|---|---|---|---|---|---|---|
| `zoom-blur` | blur | ZoomBlur | intensity n 30 [0,100]; center pos "50% 50%" | – | none | M | `@suggest-for radial-blur` |
| `angular-blur` | blur | AngularBlur | intensity n 20 [0,100]; center pos "50% 50%" | – | none | M | `@suggest-for spin-blur rotational-blur` |
| `motion-blur` | blur | LinearBlur | intensity n 30 [0,100]; angle ang 0deg [0deg,360deg] | – | none | M | `@suggest-for linear-blur directional-blur` |
| `tilt-shift` | blur | TiltShift | intensity n 50 [0,100]; width n 0.3 [0,1]; falloff n 0.3 [0,1]; angle ang 0deg [0deg,360deg]; center pos "50% 50%" | yes | none | M | `@prepass blur intensity*0.2` |
| `progressive-blur` | blur | ProgressiveBlur | intensity n 50 [0,100]; angle ang 0deg [0deg,360deg]; center pos "0% 50%"; falloff n 1 [0,1] | yes | none | M | `@prepass blur intensity*0.2`; `@suggest-for gradient-blur fade-blur` |
| `chromatic-aberration` | stylize | ChromaticAberration | strength n 0.2 [0,1]; angle ang 0deg [0deg,360deg]; red-offset n -1 [-2,2]; green-offset n 0 [-2,2]; blue-offset n 1 [-2,2] | – | none | L | `@suggest-for rgb-split rgb-shift chroma` |
| `dither` | stylize | Dither (adapted: Floyd–Steinberg omitted, it needs sequential error diffusion) | pattern enum{bayer4\|bayer2\|bayer8\|clustered-dot\|blue-noise\|white-noise}; pixel-size len 4px [1px,20px]; threshold n 0.5 [0,1]; spread n 1 [0,1]; color-mode enum{custom\|source}; color-a col transparent; color-b col #ffffff | – | none | L | `@suggest-for bayer 1-bit` |
| `halftone` | stylize | Halftone | style enum{classic\|cmyk}; frequency n 100 [10,300]; angle ang 45deg [0deg,360deg]; cyan-angle ang 15deg [0deg,360deg]; magenta-angle ang 75deg [0deg,360deg]; yellow-angle ang 0deg [0deg,360deg]; black-angle ang 45deg [0deg,360deg]; misprint n 0 [0,0.01]; misprint-angle ang 0deg [0deg,360deg]; paper-color col #ffffff; cyan-color col #00ffff; magenta-color col #ff00ff; yellow-color col #ffff00; black-color col #000000 | – | none | M | `@suggest-for comic print` |
| `scanlines` | stylize | original (CRTScreen's scanline term) | intensity n 0.3 [0,1]; spacing len 3px [1px,16px]; thickness n 0.5 [0.1,1]; speed n 0 [-100,100] (t) (CSS px/s scroll); flicker n 0 [0,1] | – | none | L | |
| `crt` | stylize | CRTScreen | pixel-size n 128 [8,128]; color-shift n 1 [0,10]; scanline-intensity n 0.3 [0,1]; scanline-frequency n 200 [100,800]; brightness n 1 [0.5,2]; contrast n 1 [0.5,2]; vignette-intensity n 1 [0,1]; vignette-radius n 0.5 [0,1] | – | none | M | `@suggest-for crt-screen retro monitor` |
| `vhs` | stylize | VHS | wobble n 1 [0,5]; scanline-noise n 0.6 [0,1]; smear n 0.2 [-2,2]; speed n 1 [0.1,3] (t) | – | none | M | `@suggest-for tape analog` |
| `glitch` | stylize | Glitch | intensity n 0.5 [0,1]; speed n 1 [0.1,5] (t); rgb-shift n 5 [0,20]; block-density n 10 [2,50]; color-bar-intensity n 0.2 [0,1]; mirror-amount n 0.3 [0,1]; scanline-intensity n 0.2 [0,1] | – | none | M | `@suggest-for datamosh corrupt` |
| `film-grain` | stylize | FilmGrain | strength n 0.5 [0,1]; bias n 2 [0,10]; animated bool false | – | none | L | `@suggest-for grain noise` |
| `vignette` | stylize | Vignette | color col #000000; center pos "50% 50%"; radius n 0.5 [0,1.5]; falloff n 0.5 [0.01,1.5]; intensity n 1 [0,1] | – | none | L | |
| `posterize` | color | Posterize | levels n 5 [2,20] (source `intensity`) | – | none | L | |
| `duotone` | color | Duotone | color-a col #ff0000; color-b col #023af4; amount n 0.5 [0,1] (source `blend`); cs linear | – | none | L | |
| `gradient-map` | color | GradientMap | preset enum{rainbow\|sunset\|ocean\|fire\|neon\|pastel\|custom} (source `palette`); color-low col #1a0b2e; color-mid col #e94560; color-high col #f9ed69; speed n 0.15 [-2,2] (t); contrast n 1 [0,3]; black-point n 0 [0,1]; white-point n 1 [0,1]; strength n 1 [0,1]; cs oklch | – | none | L | `@suggest-for color-grade lut` |
| `liquid-glass` | glass | Glass (adapted: shape = element rounded box, thickness in px) | refraction n 1 [0,2]; thickness len 24px [1px,200px]; blur len 0px [0px,20px]; aberration n 0.5 [0,1]; inner-zoom n 1 [0.5,3]; light-angle ang 300deg [0deg,360deg]; highlight n 0.05 [0,2]; highlight-color col #ffffff; highlight-softness n 0.5 [0,1]; fresnel n 0.1 [0,1]; fresnel-softness n 0.1 [0,1]; fresnel-color col #ffffff; edge-softness n 0.1 [0,1]; tint-color col #ffffff; tint-intensity n 0 [0,1]; tint-preserve-luminosity bool true | yes | none | H | `@prepass blur blur`; `@margin thickness*0.5+8px`; `@suggest-for refraction lens apple-glass` |
| `fluted-glass` | glass | FlutedGlass | shape enum{bars\|rounded\|waves}; angle ang 0deg [0deg,360deg]; frequency n 10 [1,20]; softness n 0.5 [0,1]; wave-amplitude n 0.06 [0,0.5]; wave-frequency n 1.5 [0.1,10]; speed n 0 [-1,1] (t); refraction n 1.5 [0,4]; aberration n 0.2 [0,1]; light-angle ang 30deg [-90deg,90deg]; highlight n 0.2 [0,2]; highlight-softness n 0.3 [0,1]; highlight-color col #ffffff; edges enum{mirror\|stretch\|transparent\|wrap} | yes | none | M | `@suggest-for reeded-glass ribbed-glass` |
| `glass-tiles` | glass | GlassTiles | intensity n 2 [0,10]; tile-count n 20 [5,50]; rotation ang 0deg [0deg,360deg]; roundness n 0 [0,1] | yes | none | L | |
| `wave-distortion` | distort | WaveDistortion | strength n 0.3 [0,1]; frequency n 1 [0.1,10]; speed n 1 [0,5] (t); angle ang 0deg [0deg,360deg]; wave-type enum{sine\|triangle\|square\|sawtooth\|bounce}; edges enum{stretch\|transparent\|mirror\|wrap} | – | none | L | `@suggest-for wave wobble` |
| `water-ripple` | distort | original (Ripples' ring math as a displacement; CursorRipples' chromatic split) | center pos "50% 50%"; amplitude len 6px [0px,64px]; wavelength len 40px [4px,400px]; speed n 1 [-5,5] (t); decay n 0.5 [0,5]; chromatic-split n 0 [0,3]; edges enum{stretch\|transparent\|mirror\|wrap} | – | none | L | `@suggest-for ripple water` |
| `twirl` | distort | Twirl | intensity n 1 [-5,5]; center pos "50% 50%"; radius n 1 [0,2]; edges enum{stretch\|transparent\|mirror\|wrap} | – | none | L | `@suggest-for swirl-distort vortex` |
| `bulge` | distort | Bulge (+ original `follow-pointer`) | center pos "50% 50%"; strength n 1 [-1,1]; radius n 1 [0,5]; falloff n 0.5 [0,1]; follow-pointer bool false; edges enum{stretch\|transparent\|mirror\|wrap} | – | position | L | `@suggest-for magnify fisheye pinch` |
| `flow-field` | distort | FlowField | strength n 0.15 [0,0.5]; detail n 2 [0.5,5]; speed n 0 [0,20] (t); evolution-speed n 0 [0,20] (t); seed n 0 [0,100]; edges enum{mirror\|stretch\|transparent\|wrap} | – | none | M | `@suggest-for liquify warp` |
| `heat-haze` | distort | original (FlowField-derived) | strength len 3px [0px,32px]; scale n 1 [0.25,4]; speed n 1 [0,5] (t); direction ang 0deg [-180deg,180deg] (0deg = rising) | – | none | L | `@suggest-for heat shimmer-distort mirage` |

Total: 5 + 28 + 24 + 26 = 83 effects.

## Appendix B: Verified references

**RmlUi 6.3** (`/tmp/htmlui-src/rmlui-6.3`):
- `Source/Core/DecoratorShader.cpp:27-65`: the `shader` decorator compiles `CompileShader("shader", {value, dimensions})`, builds `GenerateBackground` geometry with tex coords over the paint area and renders with the shader. `:67-71`: instancer with one string property and a `decorator` shorthand.
- `Include/RmlUi/Core/Decorator.h:23-44, 68-80`: Decorator and DecoratorInstancer API.
- `Include/RmlUi/Core/Filter.h:16-47`: `Filter::CompileFilter(Element*)`, `ExtendInkOverflow`, `FilterInstancer`.
- `Include/RmlUi/Core/EffectSpecification.h:30-37`: shorthand names `decorator` and `filter`.
- `Source/Core/PropertyParserDecorator.cpp:33-134`: decorator list parsing. Instancer lookup by type name is at `:101`, the shorthand parse at `:113`, paint-area keywords at `:11-15`.
- `Source/Core/PropertyParserFilter.cpp:14-89`: the filter list is split on whitespace outside parentheses (`:32`). Instancer lookup is at `:56`.
- `Source/Core/PropertySpecification.cpp:219-251`: property, then shorthand, then the `--` custom path. `:284-301`: var detection gives a var shorthand. `:389-430`: `RecursiveCommaSeparated` with optional and repeating items. `:513-620`: value splitting (`;`, quotes, parentheses, `EndsWithVariable`).
- `Source/Core/ElementAnimation.cpp:104-165`: `InterpolateEffectProperties`. `:271-325` and `:327-362`: decorator and filter interpolation. `:365-388`: color-stop lists. `:641`: animatable units.
- `Source/Core/ElementEffects.cpp:94-123`: filter instancing. `:127-167`: `ReloadEffectsData` (`GenerateElementData`, `CompileFilter(element)`). `:185-316`: `RenderEffects` (decorators back to front at `:193-200`, the backdrop path at `:247-281`, the filter and mask path at `:283-315`, the backdrop scissor with ink overflow at `:236-245`).
- `Include/RmlUi/Core/RenderInterface.h:95-140`: layer, filter and shader API.
- `Backends/RmlUi_Renderer_GL3.cpp:1610-1745`: `CompileShader`/`RenderShader`; the `creation` shader reads `GetElapsedTime` at `:1720-1733`. `:1760-1883`: `RenderFilters` ping-pong. `:1896-1927`: `CompositeLayers`.
- `Source/Core/Element.cpp:655-664`: `GetProperty("--x")`.
- `Source/Core/ElementStyle.cpp:163-275`: var substitution. `:336-371`: dirty-variable propagation. `:663-676`: `GetCustomProperty`.
- `Include/RmlUi/Core/Element.h:236`: `Project`. `Include/RmlUi/Core/Plugin.h:54`: `OnElementDestroy`. `Include/RmlUi/Core/Variant.h:47`: `VOIDPTR`. `Include/RmlUi/Core/StringUtilities.h:42`: `JoinString`. `Include/RmlUi/Core/StyleSheetSpecification.h:24,36,56`: `RegisterParser`/`RegisterProperty`/`RegisterShorthand`. `Include/RmlUi/Core/MeshUtilities.h:54`: `GenerateBackground`. `Include/RmlUi/Core/RenderBox.h:23-40`: fill size, offset and radii.

**shaders.com** (`/tmp/htmlui-src/shaders`, MIT, `LICENSE`):
- `packages/core/src/shaders/` holds 199 folders, each with `index.ts` and `cover.jpg`. Kinds: `paint` (generator), `effect` (filter), `map` (warp), `shape` (shapeEffect).
- `shaders/Aurora/index.ts:16-32` (look data), `:50-137` (props), `:142-224` (paint).
- `shaders/ChromaticAberration/index.ts:89-95`. `shaders/MeshGradient/index.ts:48-54` (default stops). `shaders/CursorRipples/index.ts:12-26` (grid simulation).
- `std/effects/lens.ts:29-37` (`chromaticOffsetUV`). `std/frames.ts:22-41` (`surfaceOf`, uv y down). `std/paint/noise.ts:80-109,125-135` (`waves`, `wavyLine`). `std/paint/light.ts:46-58` (`rayBands`). `std/mask.ts:35-44` (`softBand`). `std/math.ts:424-425` (`hashPhase`). `std/paint/materials.ts:945-947` (`volumeNoiseAt`). `std/paint/fields.ts:128-134` (`colorLadder3`). `std/paint/compose.ts:53-70` (`layered`).
- `gpu/porters.ts:234-246` (`animatedTime`).
- `gpu/kit/noise.ts:27-120` (MaterialX lookup3 hash), `:128-136` (`mxGradientFloat2`, select order), `:347` (`mxNoiseFloat3`), `:385` (bitcast), `:411-417` (`hash22`), `:447-480` (`value12`, `perlin12`), `:589` (`curl22`), `:735-748` (`fbmPerlin`).
- `gpu/kit/tonemap.ts:182-188` (`linearToSrgb`). `gpu/composer.ts:1541-1549` (final tail: tonemap, sRGB, premultiply). `gpu/root.ts:281-292` (premultiplied output).
- `gpu/kit/colorMixing.ts:52-419` (color spaces, `mixColors` variants). `gpu/kit/sdf.ts:157-166` (`roundedRectSdf`). `gpu/kit/geom.ts:39-42` (`flooredMod1`). `gpu/kit/effects/glass.ts:105-157,170-199` (glass lens UVs, tint, highlight). `gpu/kit/colorOps.ts:267` (`filmGrain`).
- `utilities/transformations.ts:10-17` (p3-linear default), `:36-58` (color parsing), `:279-286` (`colorSpaceOptions`). `utilities/noiseColor.ts:15-90` (shared noise props). `utilities/propConfigs.ts:250-375` (glass shell props).
- `gpu/kit/PRIMITIVES.md` (C5 compile-time vs runtime, D-1 aspect guard, D-2 luma).

**Engine** (this repository):
- `engine/core/render/Render.h:41-58`: `ShaderType`, "keep appending".
- `engine/core/pool/ShaderPool.h:36-41` (key layout), `:100-108` (custom registry).
- `engine/core/pool/ShaderPool.cpp:53-110` (type tokens), `:388-425` (names, property counts), `:510-537` (`getShaderStr`, `getShaderKey`), `:620-630` (`loadPrecompiledShader`).
- `engine/core/shader/ShaderVariantCompiler.cpp:367-449` (`setupShaderArgs`), `:527-549` (custom paths, include root), `:591-660` (`resolveCustomShaderSources`), `:669-778` (`compile`, custom overlay at `:713-726`).
- `engine/core/shader/RuntimeShaderCompiler.cpp:304-311`. `editor/shader/ShaderBuilder.cpp:110-140` (`buildShader` custom-aware disk cache), `:235-262` (`compileShader` custom sources), `:386-416` (`isCustomCacheStale`), `:418-440` (`invalidateCustomShaders`).
- `engine/core/shader/ShaderUniforms.h:15-52` (`CustomUniformBlock`).
- `engine/shaderlib/postprocess.frag` (named block and texture conventions). `engine/shaderlib/fullscreen.vert` (flip conventions).
- `editor/shader/ShaderVariantCollector.cpp:520-567`. `editor/Exporter.cpp:611-636, 3135-3250`. `editor/Stream.cpp:38-90`. `editor/window/dialog/ExportWindow.cpp:1236-1242`. `editor/util/ProjectUtils.cpp:267`. `editor/util/ProjectChecks.cpp:557,619`.
- `engine/CMakeLists.txt:956-960` (recursive shaderlib embed). `CMakeLists.txt:345` (guide `*.md` embed). `docs/mcp/guide/shaders.md` (custom block rules). `NOTICE.md`.
