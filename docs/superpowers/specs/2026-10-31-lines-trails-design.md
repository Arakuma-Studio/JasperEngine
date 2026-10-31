# Lines and trails: thick Line Renderer and a Trail Renderer (engine gaps #13, `gaps/lines-trails`)

Status: binding design. Read `docs/engine-gaps/AGENT-BRIEF.md` first: its shared conventions are binding.
Branch `gaps/lines-trails`. Unity names first (`docs/unity-glossary.md`).

## 1 Goals

1. `LineRenderer` draws **thick lines** with a width in world units (or pixels): a constant width times a
   **width curve over the line's length**, a **colour gradient** over the length, **caps and corner
   joins** (round vertex counts like Unity), a **texture** in four Unity texture modes, **alignment**
   (camera-facing or the transform's Z) and **world or local space**, **loop**. It looks the same on
   every backend: GL 4.1, GLES3/WebGL2, Vulkan, Metal, D3D11 (none of which draws wide `LINES`).
2. A Unity-style **polyline** API (`positionCount`, `setPosition`, `setPositions`, `getPosition`) on top
   of the engine's existing list of independent segments, which keeps working.
3. A new **`TrailRenderer`** component: `time`, `minVertexDistance`, width and colour over the trail,
   `emitting`, `clear()`, `autodestruct`, the same caps/joins/texture/alignment, on any GameObject
   (also one that already draws a Mesh or Sprite).
4. **Existing scenes and scripts look and behave exactly as before**: a LineRenderer without a width is
   the current 1-pixel `LINES` primitive, same shader variant, same queue, same pixels.
5. Works in 2D scenes (orthographic cameras, sprite sorting) and 3D, in editor Play and exported builds on
   every platform. Covers the game's two cases: the Invaders bullet trail (`Prefabs/I - Invaders/Bullet.prefab`,
   child `Trail`: time 0.3, width 0.223 -> 0 over the trail, orange with alpha 1 -> 0, min vertex distance
   0.1) and BACK-MAN's lightning (`Scenes/B.unity`, `BRLightningCannon.cs:37-62`: a 2-point world-space line,
   width 0.35, additive `LightningBolt` shader with an 8-row flipbook of `lightning.png` at 30 fps,
   `SetPosition(0/1, ...)` every frame).
6. The shared value types **`AnimationCurve`** and **`Gradient`** (Unity semantics), their Inspector fields
   and their Lua/MCP/YAML forms, used here and by the `particles` feature (#22).

## 2 Non-goals

- Lit lines (`generateLightingData`), shadow casting/receiving by lines and trails, `shadowBias`: lines are
  unlit (Unity's default line materials are unlit too). Documented.
- `LineRenderer.Simplify`, `BakeMesh`, `TrailRenderer.BakeMesh`: not needed; `BakeMesh` can follow later.
- Particle trails (Unity's Trails module of the ParticleSystem): out of scope (`particles` non-goal).
- Sprite mask interaction (`maskInteraction`): the component already has stencil fields; documented as the way.
- Geometry shaders / compute: not available on GLES3/WebGL2; not used.

## 3 Users and stories

- **Unity porter (human)**: "My bullets have a Trail Renderer of 0.3 s that thins to 0. I add Trail Renderer
  to the bullet's child, set Time 0.3, edit the width curve and the gradient in the Inspector exactly as in
  Unity, and it looks the same."
- **Gameplay scripter**: `line.positionCount = 2; line:setPosition(0, a); line:setPosition(1, b)` every frame
  for a laser; `line.widthMultiplier = 0.35`; `line.textureMode = LineTextureMode.Stretch`.
- **2D developer**: draws a rope with 20 points in a pixel-unit 2D scene; width 6 world units, round joins,
  sorted with sprites by its sort priority.
- **Tools/debug author**: wants 2-pixel debug lines regardless of distance: `widthUnit = LineWidthUnit.Pixels`.
- **AI agent**: `create_entity type=trail_renderer parent=<bullet>`, then `set_properties` with
  `{"time":0.3,"widthCurve":[[0,0.223],[1,0]],"colorGradient":{"colors":[...],"alphas":[[0,1],[1,0]]}}`;
  reads back the same shapes with `get_entity`; checks the result with `screenshot`.
- **Existing project**: opens a scene with old `LineRenderer` debug lines: nothing changes.

## 4 Research summary

- Unity LineRenderer (<https://docs.unity3d.com/Manual/class-LineRenderer.html>,
  <https://docs.unity3d.com/ScriptReference/LineRenderer.html>): positions + loop + useWorldSpace;
  `widthCurve` over the normalized length times `widthMultiplier`; `colorGradient` over the length;
  `numCornerVertices`, `numCapVertices`; `alignment` View / TransformZ; `textureMode` Stretch / Tile /
  DistributePerSegment / RepeatPerSegment (+ `textureScale` since 2022); mesh rebuilt on the CPU.
  **Take** all of these names and semantics (the YAML seen in the game, §1, uses exactly these fields).
  **Leave** lighting data, shadow bias, `Simplify`, `BakeMesh`.
- Unity TrailRenderer (<https://docs.unity3d.com/Manual/class-TrailRenderer.html>): `time`,
  `minVertexDistance`, `widthCurve`/`colorGradient` where 0 is the head (newest point) and 1 the tail,
  `emitting`, `autodestruct` (destroy the GameObject once it has had no points for `time`), `Clear()`,
  `AddPosition(s)`, `GetPosition(s)`, `positionCount`. Trails always live in world space. **Take** all.
- Godot 4 `Line2D` (width curve, gradient, joint mode sharp/bevel/round, cap modes, texture tile/stretch)
  and the absence of a built-in 3D line in Godot. **Take** "sharp joint with a miter limit falling back to
  bevel" (Line2D `sharp_limit`) for `numCornerVertices = 0`, which Unity leaves unspecified (Unity's sharp
  corners spike); **leave** separate cap modes (Unity's single cap-vertex count covers box and round).
- Unreal Niagara ribbons: camera-facing expansion in the vertex shader from a centre line with a tangent,
  the standard technique for multiple views. three.js `MeshLine`/`Line2` (screen-space width variant).
  **Take** GPU expansion: the engine draws the same buffer for several cameras per frame (render-to-texture,
  mirrors, the editor's views while playing) and sokol allows one `sg_update_buffer` per buffer per frame
  (`RenderSystem.cpp` comment near 15586), so per-camera CPU geometry is not possible without one buffer per
  camera. The CPU builds a camera-independent strip once per change; the vertex shader turns it to each camera.
- `glLineWidth > 1` is unsupported on core profiles, WebGL, GLES (most drivers), Metal and D3D11: rejected.

## 5 Product decisions

1. **One ribbon renderer, GPU-expanded.** Thick lines and trails are triangle strips built on the CPU from
   the centre line (positions, colours, u coordinate, width, the incoming and outgoing tangents) and
   expanded in the vertex shader for the camera that draws them. Works on every backend, every camera,
   one buffer upload per change. Rationale: §4.
2. **`width == 0` is the hairline** (today's 1-pixel `LINES`, built-in variant `lines:Vc4,...` unchanged, the
   opaque queue as today). Existing scenes have no `widthMultiplier` key, so they decode `0` and keep the
   exact current rendering. New Line Renderers created from the menu / `create_entity` get width 0.1 (3D) or
   4 (2D, pixel units) and the Polyline mode; the C++/Lua constructor keeps the struct defaults (0,
   Segments) so existing scripts are unchanged.
3. **Two data modes on LineRenderer**: `Segments` (the existing `lines` list of independent A-B segments with
   per-end colours: debug lines, gizmo-like drawings) and `Polyline` (Unity's positions). The Unity API
   (`positionCount`, `setPosition(s)`) switches the mode to Polyline, the segment API (`addLine`,
   `updateLine`, `clearLines`) to Segments; both lists are kept and serialized, only the active one draws.
   In Segments mode width/curve/caps apply per segment (the curve over each segment), the gradient is ignored
   (the segment colours are used); joins do not exist.
4. **TrailRenderer coexists with other renderers** on its GameObject (unlike the engine's other renderables,
   which are one per entity): trails are world-space and do not use the entity's mesh, so the render system
   draws them from their own loop over `TrailRendererComponent`s, not from the per-entity `else if` chain.
   Unity users put trails on the moving object itself; this keeps that working.
5. **Width units**: `widthUnit = World` (default, Unity) or `Pixels` (screen pixels of the render target,
   constant on screen; debug and UI-like lines). In Pixels mode the curve still shapes it.
6. **Joins**: `numCornerVertices = 0` is a miter joint limited to 4x the half width, falling back to a bevel
   beyond it (no spikes); `> 0` is a round joint with that many extra vertices. **Caps**: `numCapVertices = 0`
   is a flat butt cap (Unity), `> 0` a round cap. Both capped at 90 (Unity's limit).
7. **Queue**: ribbons draw in the **transparent queue** (sorted by distance, `sortPriority` first like
   meshes) when their resolved blend is not opaque, else in the opaque queue. Default blend AUTO for a
   ribbon is **alpha** (the current lines' AUTO state) with **depth write off** for ribbons (alpha ribbons
   writing depth cut each other); hairlines keep AUTO = alpha + depth write on as today.
8. **Texture**: the built-in line shader gains a texture variant (`u_baseTexture`, white when unset) and a
   `tiling` offset; the line component's existing `shaderTextures`/`customShader` path is used. A new library
   shader **`library:line_flipbook`** (type `lines`) covers the lightning case: base map, tint, flipbook
   grid (columns, rows, fps), UV scroll speed, additive/alpha via the component's `blendMode`.
9. **Trail time** follows the scaled game clock (`Engine.timeScale`), like Unity; a trail freezes at
   timeScale 0. The trail samples its transform after scripts and animation ran (post-update), so the head
   follows the object the frame it moves.
10. **Trail on deactivation**: when its GameObject becomes inactive in the hierarchy or the component is
    disabled, the trail is **cleared** (pooled bullets reactivated elsewhere would otherwise draw a streak
    from their old position; Unity users call `Clear()` for this). Documented in the guide.
11. **Shared types** `AnimationCurve` and `Gradient` are defined here (§6.1) and implemented by whichever of
    `lines-trails` / `particles` lands first; the second rebases onto them. They live in
    `engine/core/math/AnimationCurve.h` and `engine/core/math/Gradient.h` (no dependency on components).

## 6 Design

### 6.1 Shared value types (normative for `lines-trails` and `particles`)

```cpp
namespace jasperengine {
    struct JASPERENGINE_API CurveKey {
        float time = 0, value = 0;
        float inTangent = 0, outTangent = 0;   // slopes, value units per time unit; +-inf = step
        bool operator==(const CurveKey&) const;
    };
    enum class CurveWrap : uint8_t { Clamp = 0, Loop = 1, PingPong = 2 };
    struct JASPERENGINE_API AnimationCurve {
        std::vector<CurveKey> keys;              // sorted by time (normalize() sorts, dedups equal times)
        CurveWrap preWrap = CurveWrap::Clamp, postWrap = CurveWrap::Clamp;
        float evaluate(float t) const;           // empty: 0; one key: its value
        static AnimationCurve constant(float v);              // one key (0, v)
        static AnimationCurve linear(float t0, float v0, float t1, float v1);
        void normalize();
        bool operator==(const AnimationCurve&) const;
    };
    struct JASPERENGINE_API GradientColorKey { float time = 0; Vector3 color = Vector3(1,1,1); };  // linear RGB
    struct JASPERENGINE_API GradientAlphaKey { float time = 0; float alpha = 1; };
    enum class GradientMode : uint8_t { Blend = 0, Fixed = 1 };
    struct JASPERENGINE_API Gradient {
        std::vector<GradientColorKey> colorKeys;   // 0..8 keys (Unity's limit), times in [0,1]
        std::vector<GradientAlphaKey> alphaKeys;   // 0..8 keys
        GradientMode mode = GradientMode::Blend;
        Vector4 evaluate(float t) const;           // linear RGBA; no keys: white / alpha 1
        void normalize();                          // clamp times, stable sort, cap at 8 keys
        bool operator==(const Gradient&) const;
    };
}
```

- `evaluate` between two keys k0,k1 is the cubic Hermite with `dt = k1.time - k0.time`,
  `m0 = k0.outTangent * dt`, `m1 = k1.inTangent * dt` (Unity's non-weighted keys); an infinite tangent on
  either side makes the segment constant at `k0.value`. Outside the keys: wrap modes (Clamp holds the end
  value). Unity's weighted keys (`weightedMode != 0`) are converted by the importer to non-weighted (not used
  by the game; reported).
- Gradient colours are **linear** in memory (the engine's convention); YAML and MCP write colours as
  `"#RRGGBB"` sRGB strings (converted) or linear `[r,g,b]` arrays, as `docs/mcp/guide/shaders.md` already
  describes for shader params. `Fixed` mode returns the first key whose time >= t (Unity's step mode).
- YAML form (Stream helpers `encodeCurve/decodeCurve`, `encodeGradient/decodeGradient` in `editor/Stream.cpp`):
  `widthCurve: {keys: [[0, 1], [1, 0, -2, -2]], pre: clamp, post: clamp}` (a key is `[time, value]` or
  `[time, value, inTangent, outTangent]`; `.inf`/`-.inf` allowed); a bare number is a constant curve.
  `colorGradient: {mode: blend, colors: [[0, "#FF9600"], [1, "#FF9600"]], alphas: [[0, 1], [1, 0]]}`.
  MCP uses the same JSON shapes (arrays for keys), and also accepts a number for a constant curve and a
  single colour for a constant gradient.
- Factory (exported builds) emits `AnimationCurve{{{t,v,in,out},...}, pre, post}` and
  `Gradient{{...},{...},mode}` literals via new `Factory::formatCurve` / `formatGradient`.
- Lua: classes `AnimationCurve` (`AnimationCurve()`, `AnimationCurve.constant(v)`,
  `AnimationCurve.linear(t0,v0,t1,v1)`, `AnimationCurve.easeInOut(t0,v0,t1,v1)`, `:addKey(time, value[, in,
  out])` -> index, `:removeKey(i)` (1-based), `:moveKey(i, time, value)`, `:evaluate(t)`, `keyCount`,
  `:getKey(i)` -> `CurveKey` {time, value, inTangent, outTangent}, `preWrapMode`/`postWrapMode`
  (`CurveWrap.Clamp|Loop|PingPong`)), `Gradient` (`Gradient()`, `:setKeys(colorKeys, alphaKeys)` with
  tables `{ {time, color(Vector3, sRGB) }, ... }` and `{ {time, alpha}, ... }`, `:evaluate(t)` -> Vector4
  linear, `mode` (`GradientMode.Blend|Fixed`), `colorKeyCount`, `alphaKeyCount`, `getColorKey(i)`,
  `getAlphaKey(i)`), `GradientColorKey`, `GradientAlphaKey`. Colours given to Lua setters are sRGB (as
  pickers show them) and converted to linear; getters return sRGB. Component properties of these types are
  **copies** (Unity semantics: `local c = line.widthCurve; c:addKey(...); line.widthCurve = c`).
- Editor widgets (new, `editor/window/widget/CurveField.{h,cpp}`, `GradientField.{h,cpp}`):
  `CurveField`: an inline preview (the curve drawn in the row, the value range on hover) that opens a popup
  curve editor (drag keys, add on double-click, delete key, tangent handles with "Auto/Linear/Constant/Free"
  per key, presets: constant, linear up/down, ease in/out, zoom to fit). `GradientField`: Unity's bar with
  colour key markers below, alpha markers above, a checkerboard behind; click to add, drag to move, delete
  key, colour picker for the selected key, Blend/Fixed toggle, presets. Both commit one undo step per drag or
  edit through `PropertyCmd` on the property (types `PropertyType::Curve`, `PropertyType::Gradient`, new in
  `editor/Catalog.h`), support multi-selection (shown mixed when different) and the prefab override marker.
  Pure parts (hit testing, key insertion, tangent auto-smoothing) are ImGui-free and gtested.

### 6.2 LineRendererComponent (engine/core/component/LineRendererComponent.h)

New fields (defaults keep old scenes identical):

| Field | Type | Default | Meaning |
|---|---|---|---|
| `mode` | `LineRendererMode {Segments, Polyline}` | `Segments` | which data draws |
| `positions` | `std::vector<Vector3>` | empty | polyline points |
| `loop` | bool | false | close the polyline (last to first, joined) |
| `useWorldSpace` | bool | false | points are world positions (the transform is ignored); Unity default is true: new components from the editor menu set true for Polyline |
| `widthMultiplier` | float | 0 | 0 = hairline (§5.2) |
| `widthCurve` | AnimationCurve | constant 1 | over the normalized length |
| `widthUnit` | `LineWidthUnit {World, Pixels}` | World | |
| `colorGradient` | Gradient | white, alpha 1 | over the normalized length (Polyline) |
| `numCornerVertices` | uint8 | 0 | 0..90 |
| `numCapVertices` | uint8 | 0 | 0..90 |
| `alignment` | `LineAlignment {View, TransformZ}` | View | |
| `textureMode` | `LineTextureMode {Stretch, Tile, DistributePerSegment, RepeatPerSegment}` | Stretch | |
| `textureScale` | Vector2 | (1,1) | u scale (Tile: repeats per world unit), v scale |
| `texture` | Texture | empty | the built-in shader's base map (a shortcut for `shaderTextures["u_baseTexture"]`) |
| `sortPriority` | int | 0 | transparent queue order (Unity's sorting order) |
| `maxVertices` | runtime | | grown by powers of two like `maxLines` |

Runtime-only: `ribbon` (vertex vector), `needUpdateRibbon`, `ribbonVertexCount`, `ribbonIndexCount`,
`lengthCache`.

Ribbon vertex (`LineRibbonVertex`, 20 floats, `std140`-free plain buffer):
`position` (vec3, centre, model space or world), `color` (vec4, linear), `uv` (vec2: u along per texture mode,
v 0 on the left side, 1 on the right), `tangentIn` (vec3, unit direction of the incoming segment, = out at
the first point), `tangentOut` (vec3), `params` (vec4: x side -1/+1, y half width in its unit, z join/cap
kind code 0 body, 1 corner fan, 2 cap fan, w fan parameter in [0,1]). Indexed triangle list (uint32 indices
when > 65535 vertices, else uint16).

Build (`engine/core/render/LineRibbonBuilder.{h,cpp}`, pure, gtested): from points P0..Pn-1 (+ loop), the
cumulative length L, t = l / L, width w(t) = widthMultiplier x widthCurve(t), colour gradient(t). End points:
two body vertices (side -1/+1). Interior points: four body vertices, an "in" and an "out" one per side
(params.z 0 = in, 3 = out), plus, when `numCornerVertices > 0`, a fan of `numCornerVertices` vertices per side
between them (params.z 1, w = k/(n+1)); the indices always include the in-out wedge and the fan triangles on
both sides. Which side is the outer side of the turn depends on the view, so it is decided in the vertex
shader (§6.3): on the inner side every in/out/fan vertex collapses onto the miter point (degenerate, zero-area
triangles), on the outer side they spread into the bevel or round joint. Ends with `numCapVertices > 0` add
that many cap vertices (params.z 2) whose fan parameter sweeps (0,1) (angle 0..pi around the end point).
Duplicate consecutive points (distance < 1e-6) are skipped; fewer than
2 distinct points draw nothing. Texture u: Stretch u = t x scale.x; Tile u = l x scale.x (the texture
repeats every `1 / textureScale.x` world units, Unity's rule; in Pixels width unit l is still world length);
DistributePerSegment u = (segment index + fraction) / segments x scale.x; RepeatPerSegment u = (segment
index + fraction) x scale.x. v = (side x 0.5 + 0.5) x scale.y. For trails l is measured from the head.

### 6.3 Shaders (engine/shaderlib/lines.vert, lines.frag)

New LINES property bits (ShaderPool, `engine/core/pool/ShaderPool.cpp:1596`, names `:682`, count `:526` -> 6):
bit 3 `Rib` (Ribbon), bit 4 `Tex` (Texture), bit 5 `Pxw` (pixel widths). Post colour stays bit 2. The hairline
variant is unchanged (`getLinesProperties(false, true)`), so its cache entry, forks and pixels are unchanged.

`lines.vert` under `#ifdef LINE_RIBBON`:

```glsl
uniform u_vs_linesParams {
    mat4 mvpMatrix;          // unchanged first member (hairline layout preserved)
    mat4 modelMatrix;        // identity when useWorldSpace
    mat4 viewProjection;
    vec4 cameraPosition;     // xyz world, w = 1 orthographic
    vec4 alignAxis;          // xyz world normal for TransformZ, w = 1 TransformZ / 0 View
    vec4 viewport;           // xy target size in pixels (pixel widths)
} linesParams;
in vec3 a_position; in vec4 a_color; in vec2 a_texcoord1; in vec3 a_tangentIn; in vec3 a_tangentOut; in vec4 a_lineParams;
```

World unit: P = model x position; Tin/Tout = normalize(mat3(model) x tangent); facing normal N = alignAxis
(TransformZ) or normalize(cameraPosition - P) (perspective) / camera forward (orthographic, derived from
`viewProjection` inverse-free: pass forward in `cameraPosition.xyz` when w = 1). Perpendiculars
Sin = normalize(cross(Tin, N)), Sout = normalize(cross(Tout, N)) (fallback to any perpendicular when the
tangent is parallel to N). The outer side is the side s with s x dot(cross(Tin, Tout), N) < 0 (the turn
bends away from it). Miter M = normalize(Sin + Sout), miter length m = 1 / max(dot(M, Sin), 1e-4). Inner side
(and both sides when the joint is straight within 1e-4): every in/out/fan vertex goes to M x m (clamped to
4) x halfWidth x side. Outer side: if `numCornerVertices == 0` and m <= 4, in and out both go to the miter
point; otherwise "in" uses Sin, "out" uses Sout (a bevel) and fan vertex w uses normalize(mix(Sin, Sout, w))
(a round joint when there are fan vertices; with 0 fan vertices beyond the limit it is a plain bevel). Cap
fan: direction = cos(pi w) x S + sin(pi w) x (-Tin at the start / +Tout at the end), S the end's
perpendicular on the +1 side (w = 0 is the +1 body vertex, w = 1 the -1 one; the cap vertices and the end
point's two body vertices form a triangle fan around the centre point, which the builder adds as an extra
vertex with half width 0). Offset = direction x halfWidth. Pixel unit (`LINE_PIXEL_WIDTH`): the same directions are
built in clip space (screen-space extrusion: project P and P + T, take the screen perpendicular, offset by
halfWidth x 2 / viewport x clip.w). `gl_Position = viewProjection x vec4(P + offset, 1)` (+ the Vulkan
depth fixup kept). Varyings `v_color`, `v_uv1`.

`lines.frag`: `#ifdef HAS_TEXTURE` samples `u_baseTexture`/`u_base_smp` (sRGB to linear) times `v_color`,
with `customParams.tiling` when the fork/library declares it; else `v_color` as today.

`engine/shaderlib/library/line_flipbook.{vert,frag}` (manifest `@shader lines`, `@require uv1 color`,
properties `tint` color, `columns` float 1, `rows` float 8, `fps` float 30, `scroll` vector4 (0,0,0,0),
`@texture u_baseTexture white`, `@feature VERTEX_COLOR` on by default, `@state blend=additive zwrite=off`).
`list_shaders type=lines` lists it; `jasperengine-editor shaders --library all --backend all` compiles it on six
backends. `docs/shaders/library.md` regenerated.

### 6.4 TrailRendererComponent (new, engine/core/component/TrailRendererComponent.h)

| Field | Default | Unity |
|---|---|---|
| `enabled` | true | |
| `time` | 5.0 s | `time` |
| `minVertexDistance` | 0.1 | |
| `widthMultiplier` | 1.0 (menu: 0.2 3D / 8 2D) | |
| `widthCurve` | constant 1 | 0 = head |
| `widthUnit` | World | |
| `colorGradient` | white | 0 = head |
| `numCornerVertices`, `numCapVertices` | 0, 0 | |
| `alignment` | View | |
| `textureMode`, `textureScale`, `texture` | Stretch, (1,1), empty | |
| `emitting` | true | |
| `autodestruct` | false | |
| `customShader`, `shaderUniforms`, `shaderTextures`, `shaderFeatures`, `blendMode` (AUTO = alpha), `depthWrite` (AUTO = off), `stencil`, `colorWrite`, `sortPriority` | as LineRenderer | |

Runtime: `points` (`std::vector<TrailPoint{Vector3 position; float birthTime;}>`, a ring with capacity grown
by powers of two, not serialized), `idleTime`, the same render members as LineRenderer (`ObjectRender`,
buffers, shader, bindings).

Simulation (`engine/core/subsystem/TrailSystem.{h,cpp}`, a new SubSystem updated in the post-update phase
after scripts' `onPostUpdate`, animations, physics interpolation and transform updates; `Scene` registers it
like the other systems): `now` = the scene's scaled game time. For each enabled, active trail: drop points with
`now - birthTime > time`; if `emitting` and the head moved more than `minVertexDistance` from the last stored
point, append (position = transform world position); the live head (current position) is always the first
vertex of the ribbon. Width/colour t = 0 at the head, 1 at the oldest point **by age** (`(now-birth)/time`,
Unity's trail fade). `autodestruct`: when the trail has no points and the object has not moved for `time`
seconds, `GameObject.destroy` the entity (end of frame, as `destroy` does). Inactive / disabled: cleared
(§5.10). `clear()` empties it immediately. Scene load: trails are scene objects, cleared with them.
Ribbon rebuilt when points changed (every frame while moving); uploaded once per frame.

### 6.5 Render system integration

- `RenderSystem::loadLines` (`RenderSystem.cpp:10512`): for `widthMultiplier > 0` load the Ribbon variant
  (`PrimitiveType::TRIANGLES`, indexed, `LineRibbonVertex` attributes), else today's code unchanged. A change
  of `widthMultiplier` between 0 and > 0, `widthUnit`, or the texture presence flags `needReload`.
- `drawLines` (`:10726`): ribbon path applies the extended `linesParams` per camera (objectMVP, model,
  viewProjection, camera, align axis, viewport), the custom blocks, the frame block and the base texture.
  The per-entity draw site (`:15600`) queues ribbons with a non-opaque resolved blend into `transparentRenders`
  as a new `TransparentRenderType::LINES` with `objectDistance(transform)` (world-space lines: distance of the
  centre of their bounds) and `sortPriority`; hairlines and opaque ribbons draw in place as today.
- Trails: a loop over `TrailRendererComponent` in the update (load/reload like lines, `:13743` pattern) and in
  the camera pass after the opaque entity loop (queued as `TransparentRenderType::TRAIL`, or drawn in place
  when opaque). Layer culling (`isLayerVisible`) and `isEntityActiveInHierarchy` apply to both.
- 2D: cameras drawing in hierarchy order (2D, `transparentSort` off) draw ribbons and trails in hierarchy
  order like sprites; a trail draws at its entity's position in that order.
- Bounds for culling: the ribbon's AABB grown by the max half width (pixel widths: not culled).
- Shader warmup (`RenderSystem.cpp:3394` `collectSceneShaders`): add the ribbon/trail variants so exports
  precompile them; `ShaderVariantCollector.cpp` likewise. Ready checks (`:4218`) include trails.
- `destroyLines` / reload on view loss: as today; trails the same.

### 6.6 Lua API (engine/core/script/binding/GameObjectClassesLua.cpp:1301)

`LineRenderer` gains properties: `mode` (`LineRendererMode.Segments|Polyline`), `positionCount` (rw; growing
fills with the last point or zero), `loop`, `useWorldSpace`, `widthMultiplier`, `startWidth`/`endWidth` (rw,
Unity shortcuts: read the curve's first/last key; writing makes the curve linear between them with the
multiplier set to 1), `widthCurve` (AnimationCurve copy), `widthUnit` (`LineWidthUnit.World|Pixels`),
`colorGradient` (Gradient copy), `startColor`/`endColor` (Unity shortcuts on the gradient's ends, sRGB),
`numCornerVertices`, `numCapVertices`, `alignment` (`LineAlignment.View|TransformZ`), `textureMode`
(`LineTextureMode.Stretch|Tile|DistributePerSegment|RepeatPerSegment`), `textureScale` (Vector2), `texture`
(path string), `sortPriority`. Methods: `setPosition(index, Vector3)` (0-based like Unity; out of range ->
Lua error naming the count), `getPosition(index)`, `setPositions(table of Vector3)` (sets the count),
`getPositions()` (table). Existing methods unchanged. `TrailRenderer(scene)` / `gameObject:getTrailRenderer()`
(nil without one) with the shared fields above plus `time`, `minVertexDistance`, `emitting`, `autodestruct`,
`positionCount` (read-only), `clear()`, `addPosition(Vector3)`, `addPositions(table)`, `getPosition(i)`,
`setPosition(i, v)`, `getPositions()` (Unity order: index 0 is the oldest stored point, the live head is
not included). All setters mark the
ribbon dirty; values are clamped (counts 0..90, widths >= 0, time >= 0) silently.

### 6.7 Serialization and exports

- `Stream::encode/decodeLineRendererComponent` (`editor/Stream.cpp:9776`): new keys written only when not at
  their defaults (`mode`, `positions`, `loop`, `useWorldSpace`, `widthMultiplier`, `widthCurve`, ...). Old files
  decode to defaults -> identical. `Factory::createLineRendererComponent` (`editor/Factory.cpp:4028`) emits them.
- `TrailRendererComponent`: `ComponentType::TrailRendererComponent` appended at the end of the enum
  (`editor/Catalog.h:83`, keep the prefab override bit order), `Catalog` property surface (all fields; curves
  and gradients as `PropertyType::Curve/Gradient`), `Stream` encode/decode, `Factory::createTrailRendererComponent`,
  `EntityRegistry` registration (`engine/core/registry/EntityRegistry.cpp:35`), `EntityCloner` (runtime points not
  copied), prefab override support through the catalog, `ComponentAddDialog` entry ("Draws a fading ribbon
  behind a moving object").
- The texture shortcut field is stored as `shaderTextures.u_baseTexture` (one storage).

### 6.8 Editor UX

- Inspector `drawLineRendererComponent` (`editor/window/InspectorWindow.cpp:11388`): a **Mode** combo; in
  Polyline mode a **Positions** list (count field, rows with Vector3 drags, Add/Remove, "Edit points in Scene
  view" toggle: draggable handles in the Scene view, Ctrl-click inserts on the line, Delete removes), Loop,
  Use World Space; **Width** (field + `CurveField`, a "Hairline (1 px)" hint when 0), Width Unit, **Color**
  (`GradientField`), Corner Vertices, End Cap Vertices, Alignment, Texture Mode, Texture Scale, Texture slot;
  the existing shader block. Segments mode keeps the current list UI. Order and labels follow Unity's
  Inspector.
- Trail Renderer Inspector: Width, Width Curve, Time, Min Vertex Distance, AutoDestruct, Emitting, Color,
  Corner/Cap vertices, Alignment, Texture mode/scale/texture, shader block. In Play mode a read-only "Points: N".
  In edit mode the Scene view previews the trail of a selected object being dragged (cleared on release).
- Menus: GameObject > Effects > **Line Renderer** (Polyline, 2 points (0,0,0)-(0,0,1) / (0,0)-(100,0) in 2D,
  width 0.1 / 4, world space) and **Trail Renderer** (new, child-capable: created on the selected GameObject
  when it has no TrailRenderer, else as a child).
- `jasperengine-editor check`: warn `line-texture-no-uv` when a fork samples a texture on a hairline line,
  `trail-zero-time`.

### 6.9 MCP / agent surfaces

- `create_entity`: types `line_renderer` (now Polyline defaults, above) and new `trail_renderer`
  (`EntityTools.cpp:203/248` tables; alias `trail`). `parent` puts a trail under its object.
- `set_properties` / `get_entity`: all fields through the catalog; curves/gradients in the §6.1 JSON shapes.
  `get_entity` adds `ribbon: {vertices, indices, length}` for lines and `points` count for trails in Play.
- No new tool (budget). `docs/mcp/TOOLS.md` regenerated (`tools/mcp/gen_tool_docs.py`); `tool_budget.py`
  stays within budget (only enum values grow).
- Agent guide: one line in `McpInstructions.cpp` pointing at `jasper://docs/lines-trails`
  (`docs/mcp/guide/lines-trails.md`, new: data modes, width/units, joins/caps, texture modes, trails, 2D, the
  lightning and bullet recipes, costs).

### 6.10 Per platform

Pure engine C++ and GLSL: identical on Linux, Windows, macOS, iOS, Android, Web. The shader variant set is
cross-compiled by the existing pipeline (glsl410, glsl300es, spirv, msl, hlsl5). D3D11/Metal have no hairline
width issue either (hairlines stay 1 px as today). uint32 indices are supported by sokol on all backends
(WebGL2 included).

### 6.11 Docs to update

`docs/mcp/guide/lines-trails.md` (new), `docs/mcp/guide/lua-api.md` (LineRenderer, TrailRenderer,
AnimationCurve, Gradient, enums in §5), `docs/unity-glossary.md` (TrailRenderer, AnimationCurve, Gradient,
LineAlignment, LineTextureMode rows), `docs/shaders/library.md` (line_flipbook), `docs/mcp/guide/shaders.md`
(lines take textures; `type=lines` library), README feature list ("thick lines and trails").

## 7 Behaviour rules and edge cases

- Width 0 in Polyline mode: the polyline draws as connected hairline segments (n-1 `LINES` segments built
  from the positions with gradient colours), so a Unity-ported line with width 0 is still visible.
- One point or all points equal: nothing drawn, no error. NaN/inf positions: that point is skipped and a
  warning logged once per component.
- Loop with 2 points: draws the segment twice (Unity does); with < 2 nothing.
- Very sharp turns (U-turn): miter limit -> bevel; tangents antiparallel: perpendicular from the incoming one.
- Camera looking along the line (tangent parallel to the view): the ribbon degenerates to its width (Unity
  too); no NaN (fallback perpendicular).
- Negative or zero curve values: width clamped at 0 (degenerate triangles, not flipped).
- Scaled transforms with `useWorldSpace = false`: the points follow the scale, the width does not (Unity's
  rule): offsets are applied in world space after the model transform.
- Trail with `time = 0`: only the live head; nothing drawn. Teleport: call `clear()`; documented.
- Trail and `timeScale = 0`: no points added or removed; the head still follows the object.
- `minVertexDistance` 0: one point per frame (capped at 4096 points per trail; oldest dropped beyond, warning
  once).
- Entity destroyed: trail freed with it. Prefab instantiate: runtime points start empty.
- Editor Play stop: runtime state discarded with the play scene (standard).

## 8 Performance (mobile budgets)

- CPU: building a ribbon is O(points + fan vertices); a 100-point polyline with 4 corner vertices is ~1300
  vertices x 80 bytes = 104 KB, under 0.05 ms on a mid phone. Lines rebuild only when dirty; trails rebuild
  while moving. Budget: 50 active trails of 64 points under 0.5 ms CPU per frame on a 2019 mid-range Android
  (measured by a benchmark scene `tools/fixtures/lines-trails/bench` with `JASPERENGINE_FRAME_STATS=1`).
- GPU: vertex shader ~40 ALU ops; fill is the ribbon area. One draw per line/trail.
- Memory: capacity doubling; buffers shrink back when the count stays below a quarter for 5 s.

## 9 Tests and acceptance

Unit (gtest, `editor/tests`, `EditorTests.cmake`):
- `AnimationCurveTest`: Hermite values against hand-computed points; step tangents; wrap modes; Unity sample
  curves from the game (`Bullet.prefab` width curve: evaluate(0)=0.22307944, evaluate(1)=0, evaluate(0.5)
  = 0.11153972 for zero tangents); normalize sorting and duplicate times.
- `GradientTest`: blend and fixed modes, colour/alpha keys at different times (the lightning gradient of
  `B.unity`), 8-key cap, no-key defaults, sRGB parsing round trip.
- `LineRibbonBuilderTest`: vertex/index counts for (points, corner, cap, loop) combinations; u for each
  texture mode; widths from the curve; skipped duplicates; NaN point.
- `LineRendererStreamTest`: an old scene file (fixture copied from today's format) decodes to
  `widthMultiplier 0, mode Segments` and re-encodes byte-identically; round trip of every new field;
  Factory output compiles (existing Factory compile test pattern).
- `TrailSimulationTest` (engine-free TrailSystem core with an injected clock): points added by distance,
  removed by age, emitting false, clear, autodestruct after idle `time`, timeScale 0 freeze, deactivation clears.
- Curve/Gradient widget pure logic tests (hit testing, key insert/delete, tangent auto).

Pixel tests (MCP, `tools/mcp/tests/test_lines_trails.py`, screenshots decoded with `htmlui_pixels.py`):
- Hairline regression: an old-format scene screenshot equals a stored reference made from `main` before the
  change (`assert image_difference < 0.001`).
- A 0.5-wide red Polyline across a black view: the column under its middle has red pixels over the expected
  pixel span (± 1 px) at a known camera distance; rotate the camera 90 degrees around the line: still the same
  width (View alignment); TransformZ alignment seen edge-on: under 2 px.
- Pixel width 6: same 6 ± 1 px at two camera distances.
- Round caps: pixels past the end within halfWidth are covered; butt caps: not.
- Texture Stretch vs Tile with a 2-colour texture: colour changes at the expected x positions.
- Trail: Play, move the object along x at a known speed for 1 s with time 0.3: lit pixels span the last 0.3 s
  of path ± 10 %, alpha falls towards the tail; after `clear()` nothing.
- 2D scene: a trail behind a sprite sorted by hierarchy order (drawn before a later sprite).
- Backends: run the line fixture with `SMOKE_BACKEND=gles3` and `vulkan` (`tools/smoke-test.sh`,
  `tools/fixtures/lines-trails`) and compare to glcore within tolerance.

Lua/stubs: `tools/mcp/tests/test_stubs.py` (`-- unresolved:` empty); a Lua script test setting positions,
gradient, curve copies (`eval_lua` round trips).

Builds: Linux editor + tests; `cmake -S engine` standalone; Android (Gradle/NDK) and Web (emsdk) compile the
engine with the new files; `jasperengine-editor shaders --library all --backend all` passes.

Acceptance (checkable):
1. Old scenes with LineRenderers render pixel-identically (test above) and old Lua scripts run unchanged.
2. The Invaders bullet trail and the B lightning are reproducible from the Inspector and from Lua with the
   values in §1; screenshots in the fixture show them.
3. Width, curve, gradient, joins, caps, 4 texture modes, View/TransformZ, World/Pixels, world/local space and
   loop work on glcore, gles3 and Vulkan (fixture runs) and compile for all backends.
4. TrailRenderer coexists with a Mesh on the same GameObject.
5. Every field is editable in the Inspector with undo, serialized, exported (Factory), settable by MCP
   `set_properties`, readable by `get_entity`, bound in Lua with stubs, documented.

## 10 Integration points

- `engine/core/component/LineRendererComponent.h` (fields), `engine/core/object/LineRenderer.{h,cpp}` (API).
- `engine/core/subsystem/RenderSystem.cpp`: `collectSceneShaders` 3394, ready check 4218, reload 11654,
  load/destroy queues 12306/12346, update 13743, draw 15600 + transparent list, `loadLines` 10512, `drawLines`
  10726, `destroyLines` 10755, `isRenderActive` 12534 (add TrailRenderer).
- `engine/core/pool/ShaderPool.cpp` 511, 526, 682, 1596; `engine/shaderlib/lines.vert/.frag`; new library files.
- `engine/core/registry/EntityRegistry.cpp:33-35,144`; `EntityCloner.cpp`.
- `editor/Catalog.{h,cpp}` (3546 lines props, enum 83, names 4083, icons 4150), `editor/Stream.cpp` 4940/5689/9776,
  `editor/Factory.cpp` 3022/4028, `editor/window/InspectorWindow.cpp` 11388/16205,
  `editor/window/HierarchyWindow.cpp:494`, `editor/command/type/CreateEntityCmd.cpp:646`,
  `editor/window/dialog/ComponentAddDialog.cpp:80`, `editor/mcp/tools/EntityTools.cpp:203,248`,
  `editor/shader/ShaderVariantCollector.cpp`.
- `engine/core/script/binding/GameObjectClassesLua.cpp:1301`, `MathClassesLua.cpp` (AnimationCurve, Gradient).

## 11 Cross-feature dependencies

- `particles` (#22) uses §6.1 types and widgets (first to land adds them).
- `anim-properties` (#12) may animate `widthMultiplier`/`startColor` through the catalog.
- `script-lifecycle` (#17): trail clearing on `onDisable` order is independent (engine side).
- `persistence` (#20): a persistent object's trail is cleared on scene load (its transform jumps).
- `ui-scaling-text` (#21): none (trails in UI are not in scope).

## Implementation notes

Decisions taken while implementing (branch `gaps/lines-trails`), where the spec was ambiguous,
wrong or in conflict with the code:

1. **Ribbon vertex size**: `LineRibbonVertex` is 19 packed floats (76 bytes): the fields listed in
   6.2 add up to 3+4+2+3+3+4 = 19, not 20. The w of an in or out vertex at a joint is 1 when the
   joint is round (the shader may not miter it), the fan parameter on fan vertices. Indices are
   uint16 while the vertex capacity fits them, uint32 past 65536 (capacity, not count, decides,
   so the pipeline's index type only changes on a reload).
2. **Joint geometry**: the wedge of each side fans around the *other side's* in vertex (the inner
   miter point), not around the centre point: with the centre a bevel or round joint left two
   uncovered slivers between the body quads. Inner-side vertices all collapse onto the inner miter
   point (limit 4 half widths), so their triangles have no area. `LineRibbonBuilder::vertexOffset`
   is a C++ mirror of `lineOffset` in `includes/lines_vs.glsl`, used by the tests (areas of
   straight lines, miters, bevels, round joints and caps, loops, U-turns, end-on views) and kept
   identical to the shader. U-turns treat both sides as outer (bevel or round through the front).
   Pixel widths (`Pxw`) build the same offsets in screen space; Transform Z alignment does not
   apply to them.
3. **Shared GLSL**: the ribbon expansion lives in `includes/lines_vs.glsl` (the built-in
   `lines.vert` and library `.vert`s include only it) and `includes/lines_fs.glsl`
   (`lineVertexColor()`, `lineUV()`); the hairline path of the include is the old `lines.vert`,
   so the hairline variant's output is the same. A fork made before this keeps drawing hairlines;
   with a width it needs the include (documented in the guide).
4. **Variant bits**: `Tex` is set only for a shader without a manifest (the built-in shader or a
   fork of it) with a non-empty `u_baseTexture`; a library shader declares its own textures.
   `Rib`, `Tex` and `Pxw` are never set on a hairline. `RenderSystem::getLinesVariantProperties` /
   `getTrailVariantProperties` are the single source for loads, warmup (`collectSceneShaders`),
   the editor's variant collector and the library matrix (which now builds ribbon, hairline and
   pixel-width variants).
5. **Render state**: AUTO depth write is off for ribbons (an explicit `depthWrite` or a manifest
   `zwrite=` still wins); face culling is off (which side a camera sees depends on the view).
   The transparent queue order is `sortPriority` plus the shader's `@state priority=` (applied
   without a reload). Thick lines and trails with world widths are frustum-culled by their bounds
   grown by the largest half width.
6. **Trail timing**: `TrailSystem` is registered right after `RenderSystem`, whose update makes the
   frame's world transforms; each trail is simulated there and its ribbon built and loaded in the
   same update (`RenderSystem::updateTrail`), so the head is drawn where the object is drawn that
   frame. Lua `onPostUpdate` (Engine::onPostUpdate) runs after the scene update: a move made there
   reaches every renderer's world transform, the trail's included, next frame, so meshes and trails
   stay in step (the spec's "after scripts' onPostUpdate" cannot hold for any renderer without a
   second transform pass). The first point of a trail is where the head was when it started moving
   (born a frame earlier), so a stationary object keeps no points and autodestruct can fire.
7. **Texture shortcut**: one storage, as the spec says: `shaderTextures.u_baseTexture`. `texture`
   is a Lua/C++ accessor (`setTexture`/`getTexture`) and an MCP alias (`set_properties` rewrites
   `texture` to `shaderTextures.u_baseTexture`); it is not a Catalog property (a Texture property
   pointing into the list would have mutated it on read and given prefab overrides two names for one
   value). The Inspector shows a Texture slot for the built-in shader; a library shader shows its
   own slots.
8. **useWorldSpace** also applies to Segments mode (the transform is ignored for every point).
9. **Editor defaults**: the Unity presets (Polyline, world space, (0,0,0)-(0,0,1), width 0.1 /
   2D (0,0)-(100,0), width 4; trails width 0.2 / 8) are applied by the menu, `create_entity` and
   also Add Component / `add_component` (AddComponentCmd, like the existing Light 2D height),
   so every way of adding one in the editor gives a visible line. A new 2D trail also gets
   `minVertexDistance` 2 (pixel units), and new trails thin 1 -> 0 and fade out. The structs keep
   their defaults (hairline, Segments; trail time 5, width 1) for scenes and scripts.
10. **Position API vs GameObject**: `setPosition(index, Vector3)` / `getPosition(index)` live next
    to GameObject's own `setPosition(x, y[, z])` (C++ `using` declarations; the Lua function
    dispatches on its arguments), so old scripts moving a LineRenderer by coordinates keep working.
11. **Curves and gradients in files**: tangents are written only when not flat; wraps only when not
    clamp. Gradient colours are written as `"#RRGGBB"` only when the linear value is exactly an
    8-bit sRGB value read back (else linear `[r, g, b]`), so a round trip is lossless. MCP writes
    infinite tangents as `"inf"` / `"-inf"` (JSON has no infinity); YAML uses `.inf`. A new
    `Gradient` is Unity's (white at both ends, opaque); "no keys" evaluates white and opaque.
    Lua curve keys are 1-based (`addKey` returns -1 when a key is at that time).
12. **Curve editor tangent modes**: a key has no stored tangent mode (the spec's `CurveKey`):
    a key counts as Auto when its tangents equal the auto value and is recomputed when neighbours
    move; Free is a per-key flag of the open editor (only needed when a free key's two tangents
    happen to be equal). The widgets also take a second curve (particles' Two Curves).
13. **Scene view point editing**: the polyline's point handles show and drag like the segments'
    endpoints always did (Move or 2D tool); the Inspector's Edit Points toggle enables Ctrl+click
    insertion on the line; Delete removes the selected point (instead of the GameObject).
14. **shader-globals**: `TrailRendererComponent::shaderGlobalBindings` exists, is serialized,
    copied and exported (Factory); it is not editable yet: the shader-globals feature wires it
    (and the same field on the other components) when it lands.
15. **get_entity**: `ribbon` is `{vertices, indices, length, loaded}` for a thick line and
    `{hairline: true, segments}` for a hairline; `trail` is `{points, vertices, length}`.
16. **A trail on a mesh**: `RenderSystem::isRenderActive` (every renderer on the entity must be
    enabled) is not extended with the Trail Renderer, so disabling a trail does not hide the mesh
    or sprite it follows; trails use their own `isTrailRenderActive` (enabled, active in the
    hierarchy, layer, editor overlay rules).
17. **check_shader without custom shaders**: the MCP tool returned early ("No custom shader is
    used") before adding the scene findings; it now reports the Stochastic Tiling and the new
    `line-texture-no-uv` / `trail-zero-time` findings in that case too (the CLI `check` already
    did).
18. **Bench**: the 50-trail bench is the fixture run with `LINES_TRAILS_BENCH=1`, not a separate
    fixture folder. Measured on the shared Linux build host (8 cores, load average ~28, so the
    numbers are inflated): the CPU side of 50 trails of 64 points (simulation, centre line and
    ribbon build, outside the editor) took 0.6-0.65 ms per frame on average after writing the
    vertices field by field and reserving the lists (0.7-0.97 ms before). Not measured on an
    Android device.
19. **MCP tool budget**: no new tool; `create_entity`'s `trail_renderer` type (+20 characters) is
    paid by a shorter `create_entity` description (total 106093 of 106100).
