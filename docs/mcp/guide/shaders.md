# Shaders

Served as `jasper://docs/shaders`.

## Library shaders (use these first)

The engine ships ready-made shaders, referenced as `"library:<name>"`. `list_shaders` lists
them (name, type, category, description, lighting modes); `list_shaders name="toon"` (or
`detail: true`) adds each one's parameters (member, kind, default, range, label, `feature`
gating: shown while that define is on, `"!X"` while it is off), textures (uniform, default), features, keyword groups, the inputs it requires
(`require`, plus `require_rules`: inputs only while a feature is on, `"!X"` while off; every
entry also has `scene_inputs`, see "Scene depth and colour" below) and its
default render state (`state`, including `shadows: "off"` for shaders that cast none, plus
`state_rules`: state a feature or keyword value turns on, e.g. hologram's
`BLEND_MODE_ALPHA` makes it alpha blended).
`type` filters by shader type: `mesh`, `ui`, `points`, `lines` or `sky` (sprites, tilemaps and
models draw with mesh shaders). `ui` is the component UI's (Image, Text, Polygon). HTML/CSS UI
documents have their own effects library instead, `effect(<name>, ...)` in CSS (shader type
`UI_EFFECT`; `list_ui_effects`, `jasper://docs/html-ui-effects`); a document's GPU work is not a
`library:` shader and takes no material.

- **Mesh shaders go on a material**, per submesh: `set_properties` on the mesh with
  `submeshes[0].material.shader = "library:toon"` (`""` is the built-in Lit shader). A
  `.material` file takes the same keys (`create_material` / `update_material` `values`), and
  linked submeshes follow it.
- **UI, Points, Lines and Sky shaders** go in the component's `customShader`; their
  parameters are its `shaderUniforms`.
- Values (paths of the submesh's material, `submeshes[N].material.<key>`):
  - `shaderParams`: `{member: value}`, or one entry `shaderParams.<member>`. Numbers or
    `[x,y,z,w]` as stored: colours are **linear**; a `"#RRGGBB[AA]"` string is sRGB and is
    converted. A member left out uses the manifest default; `null` removes one.
    `set_shader_uniform` (entity + `submesh`) sets one and `get_shader_uniforms` lists them
    with labels and defaults.
  - `shaderTextures`: `{uniform: "assets/path.png" | null}`, or `shaderTextures.<uniform>`.
    Unset ones bind the declared default (white, black, gray, flat normal, or the engine's seamless `ripples` normal map and `noise`), or the one a `default[<DEFINE>]=` rule picks while that feature is on (`list_shaders`: `default_with`). Images only: a
    camera's render texture goes in a built-in material texture (`baseColorTexture`, ...).
  - `shaderFeatures`: the enabled `@feature` defines and chosen `@keywords` values
    (`"<GROUP>_<VALUE>"`), e.g. `["RIM", "SPEC_MODE_HARD"]`; `"!RIM"` turns off a feature that
    is on by default. Writes also take `{"RIM": true}`, applied to the current list. A feature
    is a compile-time variant: changing one builds a new variant (no cost per pixel).
  - Render state: `blendMode` (auto, opaque, alpha, premultiplied, additive, multiply),
    `cullMode` (auto, back, front, off), `depthWrite` (auto, on, off), `depthTest` (auto,
    less, less_equal, equal, greater_equal, greater, not_equal, always), `depthOffsetFactor` /
    `depthOffsetUnits` (polygon offset; negative pulls towards the camera), `sortPriority`
    (draw order within the opaque or the transparent queue, lower first, before distance). `auto` takes the shader's default state, else the
    engine's. UI, Points and Lines have `blendMode` and `depthWrite`.
  - `lighting`: auto, pixel, vertex, mixed or none (unlit), only the modes the shader lists
    (`list_shaders` gives them, the default first; the built-in Lit takes pixel, vertex, mixed
    and none, which draws the base colour only: a glTF material with `KHR_materials_unlit`
    loads as none). Choosing another shader puts a mode it does not list back to auto. `mixed`
    follows each light's `shading` (both, pixel, vertex). The scene setting
    `max_lights_per_object` (1-16, default 8) caps the lights per object; the rest reaching
    it fold into its ambient.
- A submesh material with `shader = ""` draws with the mesh's `customShader`, else the scene's
  default mesh shader: its `shaderParams`, `shaderTextures` and `shaderFeatures` then follow that
  shader's manifest (set both in one `set_properties` call and the values follow the new one).
- Values set for another shader stay stored after a shader change (switching back finds them)
  and are ignored by a shader that does not declare them; an edit only checks the entries it
  names. `null` removes one.
- `check_shader shader="library:toon#RIM"` compiles a library shader with features.

Fork only when no library shader does what you need.

## Stochastic Tiling

Hides the repetition of a tiled texture at run time (Mikkelsen's hex-tiling,
`includes/stochastic.glsl`): the UV plane is cut into hexagonal cells that each read the maps at
their own random offset (and turn), blended at the cell borders. No precomputation; up to three
reads per map where cells meet, one or two over most of a cell. Suits organic surfaces (grass,
rock, sand, dirt); bricks, tiles and planks lose their grid.

- The built-in Lit material: `stochasticTiling: true` on the material (Inspector: the
  **Stochastic Tiling** checkbox at the end of the material's maps, its four values indented
  below while it is on), with `stochasticCellSize`, `stochasticBlendSharpness`,
  `stochasticContrast` and `stochasticRotation` (the ranges and defaults below; `set_properties`
  `submeshes[N].material.stochasticTiling`... or a material object, `.material` files, Lua
  `material.stochasticTiling` then `mesh:setMaterial(m)`). Every map the variant samples goes
  through the cells: base colour first (its taps' luminance gives the weights), then
  metallic-roughness, occlusion, emissive and the normal map with the same weights, one lattice
  per UV set. The depth pass (shadows, the depth pre-pass; an alpha-masked material's cutout) and
  the SSR G-buffer read the same taps, so cutouts, shadows and reflections match what is drawn.
  The variant bits: `Sto` in mesh (bit 5), depth (bit 14) and gbuffer (bit 10) variants, e.g.
  `mesh:Uv1,Nor,Puc,Sto`. Not applied (the `check` command warns, `stochastic-no-effect`): a
  material without textures, sprites and tilemaps (an atlas frame's neighbours would show),
  terrains (their layers have their own), shaders with a manifest (use their `STOCHASTIC`
  feature) and forks of Lit that do not opt in (see Forking: none of the submesh's passes take
  it then, so its shadows, cutouts and SSR match its plain colour). Unlit, vertex lights, 2D lights (the normal map), instancing, skinning and mirrors
  (the other maps) keep it; keep it off for instances that draw atlas frames
  (`InstanceData.textureRect`), which the variant cannot see.
- Library shaders: the `STOCHASTIC` feature ("Stochastic Tiling") of `simple_lit`, `unlit`,
  `toon`, `vertex_lit`, `fake_lit` and `triplanar` (each projection), e.g. `shaderFeatures:
  {"STOCHASTIC": true}`. Parameters: `stochasticCellSize` (Cell Size: the cell width in texture
  repeats, 0.25-4, default 1), `stochasticBlendSharpness` (Blend Sharpness 0-1, default 0.4),
  `stochasticContrast` (Contrast 0-1, default 0.45; 0 blends linearly, high shows the cell
  borders), `stochasticRotation` (Rotation 0-1: a random turn per cell, up to +-180 degrees;
  default 0). The material's other maps (normal, emission) reuse the base map's weights;
  `unlit`'s alpha clip and its depth program read the same taps.
- Terrain layers: `set_terrain_maps` `layers: [{index, stochastic_tiling: true, cell_size,
  blend_sharpness, contrast, rotation}]` (same ranges and defaults), or the paths
  `surfaceLayers[N].stochasticTiling`, `.stochasticCellSize`, `.stochasticBlendSharpness`,
  `.stochasticContrast`, `.stochasticRotation` (Lua: the same `TerrainSurfaceLayer` fields).
  The layer's colour taps weight its normal and surface maps too, in every projection (flat and
  the triplanar sides); its far tiling break is left out. A layer with it draws with the PBR
  terrain path.
- Rotation is for textures without a direction: normal maps are turned back per cell, but
  stripes, planks or baked light visibly turn. So does a normal map whose average normal leans
  off +Z (baked on a slope, or with a bias): each turned cell then leans its own way. The
  Inspector warns under the Rotation row (with "Set Rotation to 0") when the map's mean normal is
  3 degrees or more off +Z, and `jasperengine-editor check` reports it
  (`stochastic-tilted-normal-map`, a warning) for materials, terrain layers and library shader
  materials.
- In a fork: `#include "includes/stochastic.glsl"`; its header documents the API
  (`stochasticSetup`, `stochasticTextureLuminance`, `stochasticTexture`, `stochasticNormal`...).
  The texture must wrap (REPEAT).

## Presets, stencil and the extra pass

Outlines, see-through silhouettes, stencil masks (portals, windows, sprite masks) and
draw-once transparency are set up in one step by a **material preset**:
`list_material_presets` lists them (id, name, description and the material fields each
sets); `apply_material_preset preset="outline_hull" entity="Hero"` applies one to every
submesh (or `submesh`) of the given GameObjects as one undo step, or to a `.material` file
(`path`; linked submeshes follow). A preset changes only the fields it sets; a submesh
linked to a `.material` file becomes its own material. Tune the result with the paths below.

- `stencil` (material, extra pass, UI, Points and Lines): `{compare, ref, readMask,
  writeMask, pass, fail, depthFail}`, or one field `stencil.<field>`. `compare`: `auto` (the
  shader's `@state` stencil rule, else none), `disabled` (off), `never`, `less`, `equal`,
  `less_equal`, `greater`, `not_equal`, `greater_equal`, `always`; any test but auto makes
  the whole block explicit. `ref` and the masks are 0-255; ops `keep`, `zero`, `replace`,
  `incr_clamp`, `decr_clamp`, `invert`, `incr_wrap`, `decr_wrap` (the manifest's `lequal`,
  `incr`... work too). A block merges the keys given; `null` resets it. The stencil is
  cleared at the start of each camera pass: a mask and its readers draw in the same camera,
  the mask first (opaque queue before transparent; `sortPriority` orders within a queue).
  A test other than `always` leaves the depth pre-pass; SSAO and SSR ignore the stencil (a
  masked object darkens and reflects at its whole shape).
- `colorWrite`: `"auto"` (the shader's `colorwrite=`, else all), `"none"` (depth/stencil
  only: invisible masks) or channels of `"rgba"` (`"rgb"`); a mask 0-15 works too.
- `extraPass` (a material's): one more draw of the submesh before or after its mesh's main
  draws (Before passes ahead of all the mesh's submeshes, After passes after all of them), with
  its own mesh shader: `{shader, shaderParams, shaderTextures, shaderFeatures, blendMode,
  cullMode, depthWrite, depthTest, depthOffsetFactor, depthOffsetUnits, stencil, colorWrite,
  order}` (`order`: `after` or `before`), or one field `submeshes[0].material.extraPass.<key>`
  (`.shaderParams.<member>`, `.shaderTextures.<uniform>`). Its values follow its own shader's
  manifest (not the material's), with the same rules as the material's; its `auto` state is
  its shader's `@state`. `shader: ""` draws no pass; a whole object merges, `null` removes
  the pass. An inverted hull (`library:outline`) is for opaque objects (`after`: their depth
  hides the inner hull); its back faces fill a blended object's shape on either side, so the
  `outline_hull` preset gives a blended material (its blend, else the shader it draws with)
  stencil bit 1 and `library:outline_stencil` Visible only instead. A mask reader preset masks
  the pass too. The pass casts no shadows and skips the depth pre-pass.
- A manifest declares stencil and colour state with `@state stencil=<test> ref= readmask=
  writemask= pass= fail= zfail= colorwrite=` (also in `@state[...]` rules); `list_shaders`
  reports them in `state.stencil` / `state.colorWrite`.

## Forking

Custom (forked) shaders are set per component on Mesh, UI, Points, Lines (Line Renderer) and
Sky (Skybox), through the `customShader` property (a mesh material's `shader` also takes a fork). To customize, call `fork_shader`: it copies the
built-in `<type>.vert` / `<type>.frag` into a project directory (default `shaders`) and sets
`customShader` in one undoable step; `fork_includes` also copies the engine includes into an
editable `includes/` folder beside the fork.

- Never set `customShader` by hand to files that do not exist. The value is a base path with
  no extension (`base.vert` + `base.frag`), or two explicit paths joined by `|`
  (`"a.vert|b.frag"`) when the names differ. Both a .vert and a .frag are always required.
- Paths are project-relative (`assets/shaders/x` when the assets directory is `assets`); the
  assets-relative spelling (`shaders/x`) also resolves, in the editor, checks, exports and game.
- Clear `customShader` (empty string) to go back to the built-in shader.
- A Mesh fork drives the color pass only. When it displaces vertices or discards fragments,
  also fork the depth shader (`fork_shader` with shader type `depth`, which sets
  `customDepthShader`) so shadow maps follow, and SSAO/post-process depth while SSR is off
  (with SSR on they read the G-buffer, which keeps the built-in shader). It reads the same
  `shaderUniforms`.
- A mesh bound to lattices compiles with `HAS_LATTICE` (variant bit `Ltc`): `mesh.vert` and
  `depth.vert` run the vertex through `latticeDeform` from `includes/lattice.glsl`. A fork
  without that call (one made before lattices existed) draws the mesh undeformed; to follow
  them, include the file after the attributes and call, under `#ifdef HAS_LATTICE`,
  `latticeDeform(1, pos.xyz, normal, tangent, stretch);` after the instance matrix and before
  the model matrix (`latticeDeform(0, ...)` before skinning for bind-pose lattices), as
  `mesh.vert` does (`read_engine_source`), in the depth fork too.
- A mesh played by a Vertex Animator compiles with `HAS_VAT` (variant bits `Vat`, Bones mode
  `Vab` with `HAS_VAT_BONES`), without skinning or morph targets: `mesh.vert` and `depth.vert`
  call `vatEvaluate()` first and read the rest attributes through `vatPosition(a_position)`,
  `vatNormal(a_normal)` and `vatTangent(a_tangent.xyz)` from `includes/vat.glsl`. A fork without
  them draws the mesh at rest; to play it, include the file after the attributes and make the
  same calls (they return their argument without `HAS_VAT`), in the depth fork too.
- Stochastic Tiling (the material's `stochasticTiling`) in a Mesh fork: a fork opts in by
  naming the material's four values for `includes/pbr.glsl`, as the engine's `mesh.frag` does.
  A fork made before the feature (October 2026) does not: its submeshes then draw without it in
  every pass (the colour, depth and G-buffer variants carry no `Sto`), and `check` warns
  (`stochastic-no-effect`, "... predates Stochastic Tiling"). To opt in, in the fork's
  `.frag` (the entry point itself, not an include: the editor and the exported game read it
  there), end its `u_fs_pbrParams` block with the member and define the name after the block:

  ```glsl
  uniform u_fs_pbrParams {
      ...                          // the members it has, unchanged
      #ifdef USE_STOCHASTIC_TILING
          #ifdef MATERIAL_UNLIT
              // up to the offset of the values in the CPU Material prefix (48 bytes)
              #ifndef ALPHA_MASK
                  vec4 stochasticPadding0;
              #endif
              vec4 stochasticPadding1;
          #endif
          vec4 stochastic;         // cell size, blend sharpness, contrast, rotation
      #endif
  } pbrParams;

  #ifdef USE_STOCHASTIC_TILING
      #define LIT_STOCHASTIC_PARAMS pbrParams.stochastic
  #endif
  ```

  (copy the block from `read_engine_source` `mesh.frag` when the fork's members differ). The
  `#define LIT_STOCHASTIC_PARAMS` line is the signal: with it the submesh takes the `Sto`
  variants in its colour, depth and G-buffer passes; the fork must then include the engine's
  `includes/pbr.glsl` (or a copy of it from after the feature) before its `main()`. A fork with
  `fork_includes` copies of the includes made before the feature needs them copied again.

A fork of the built-in Mesh shader keeps the material's extended lobes and hex tiling
(clear coat, sheen, ... `jasper://docs/materials`) and the High Lit Quality when it keeps
their hooks: the `#ifdef PBR_EXT` members at the end of `u_fs_pbrParams` (and the `PBR_EXT`
define before it), `#include "includes/pbr_ext_slots.glsl"` before `includes/pbr.glsl`,
`includes/specular_aa.glsl` and `includes/pbr_ext.glsl` after `includes/ibl.glsl`, and the
`#ifdef PBR_*` / `USE_LIT_QUALITY` blocks of `main()` (`pbrExtSurface`, `pbrExtView`,
`pbrExtEnvironment`, `pbrExtDirect`, `pbrExtTransmission`, `pbrExtComposite`). The engine adds
their defines to the fork's variants (`<fork>#PBR_CLEARCOAT,...`); a fork written before them
compiles them as unused defines and draws without the lobes (the Inspector says "This shader
does not implement advanced surface"): fork again to get them.

## Editing a fork

The forked files are complete engine shaders with a strict structure: a `#version` line,
`#include "includes/..."` lines (resolved against the engine library), `#ifdef` variant guards,
uniform blocks, texture and sampler declarations, in/out varyings and a specific fragment
output variable.

1. Read the forked file first (always).
2. Make minimal, targeted changes. PRESERVE: `#version`, every `#include`, the engine uniform
   blocks, ALL texture and sampler declarations (`uniform sampler2D/samplerCube`,
   `textureN`/`samplerN`) and their `#ifdef` guards, the in/out declarations, and the output
   variable name.
3. Keep every texture/sampler declaration even if your code no longer samples it: the engine
   binds them by fixed slot, so removing or renaming one breaks the bindings.
4. Keep the original `main()` code that READS the in varyings (`v_position`, `v_normal`,
   `v_uv1`, `v_color`, `v_lightProjPos`, ...). If a varying's use disappears the GLSL
   compiler strips that fragment input and cross-compilation fails with a vertex/fragment
   interface mismatch ("vertex shader output X does not exist in fragment shader inputs").
   Compute the original color, then layer your effect on it (posterize, tint, override).
5. Change the shading math, usually near the end of `main()`. Never rewrite a shader from
   scratch, drop `#version`/`#include`, or invent sampler, texture, output or engine uniform
   names.
6. A stylized shader (toon, cel) may replace the lighting model and stop sampling shadow or
   IBL maps; keep their declarations (unused bindings are skipped) and say that the shader no
   longer receives shadows, if so.

If a custom shader fails to compile, the GameObject falls back to the built-in shader and the
error is in the Console: read it and the forked source, then fix minimally. `write_file` on a
`.vert`/`.frag`/`.glsl` compiles the forks using it and returns the errors (line + message).
Edits made on disk with your own tools are compiled before your next tool call, whose result
lists the errors in `external_changes.problems`; `check_shader` (no arguments: every fork in
use) checks on demand.

Targets of `fork_shader`: `component` (a GameObject's renderable), `depth` (a Mesh's depth
pass), `scene_default` (with `shader_type`: every component of that type without its own fork)
and `post_process` (a scene post-process pass: `pass_index`, or omit it to add a new pass).
A pass may also be a library post-process shader (`list_shaders` type `postprocess`), take
`textures` {uniform: image} and a `scale` (0.1..1, its resolution), and read
`u_originalSceneTexture` (the scene before the chain) besides `u_sceneColorTexture`.

## Parameters: the custom block

The one uniform block a fork may add is its own: `u_vs_customParams` in the .vert and/or
`u_fs_customParams` in the .frag (a post-process pass uses `u_fs_postParams`):

```glsl
uniform u_fs_customParams {
    float time;
    float amplitude;
    vec4 tint;
} customParams;
```

- Members are filled BY NAME from the component's `shaderUniforms` (or the pass uniforms):
  set them with `set_shader_uniform` (undoable, no shader rebuild); list the declared members
  with `get_shader_uniforms`. Scripts: `mesh:setShaderUniform("amplitude", 0.3)`,
  `scene:setPostProcessUniform(0, "amount", 0.5)`.
- The engine writes `time` (seconds since startup) and `resolution` (xy = render target size,
  zw = 1/size) every frame: declare them to animate, never set them.
- One scalar kind per block (do not mix int and float members: GL flattens the block after
  its first member). No matrix or array members (they stay zero): one scalar/vector member per
  value. An unset member reads zero: set it or give the effect a sensible fallback.
- A fork may declare the manifest lines library shaders use (`// @property`, `@texture`,
  `@feature`, `@state`, ...; read `engine/shaderlib/library/unlit.frag` with
  `read_engine_source`): its parameters then get labels and defaults, and it takes
  `shaderTextures` and `shaderFeatures` like a library shader.

## Manifest: conditional state, requires and blend

Manifest lines (library shaders and forks that declare them) beyond the basic ones:

- `// @state shadows=off`: the submesh casts no shadows (the mesh's `castShadows` still
  turns them off too). Other keys: `blend`, `cull`, `zwrite`, `ztest`, `offset`, `queue`,
  `priority`. A shader can offset its queue order with `@state priority=N`, added to the
  material's sort priority.
- `// @state[<DEFINE>] key=value...`: state applied while a `@feature` or `@keywords`
  value (`<GROUP>_<VALUE>`) is on; `[!<DEFINE>]` while it is off. Order: material value,
  then the matching `@state[...]` lines (later wins), then `@state`, then the engine's.
  Example: `// @state[MODE_ALPHA] blend=alpha zwrite=off`.
- `// @texture <uniform> <default> ... default[<DEFINE>]=<default>`: while that `@feature` (or
  `@keywords` value; `[!<DEFINE>]`: while it is off) is on, an unset slot binds the second
  default instead. Defaults: `white`, `black`, `gray`, `normal` (flat), `ripples` (a seamless
  ripple normal map) and `noise` (seamless grey value noise), the last two built by the engine,
  so a material can use them without image files (`library:water`'s `BUILTIN_TEXTURES`).
- `// @require <inputs> feature=<DEFINE>`: inputs required only while that define is on
  (`feature=!<DEFINE>`: while off). Inputs: `normals tangents uv1 uv2 color rawcolor
  position smoothnormals`; `rawcolor` gives `v_rawColor`, the vertex colour without the
  instance colour (`v_color` is their product); `smoothnormals` gives a `SURFACE_DISPLACE`
  hook `surfaceSmoothNormal`, the object-space normal averaged over the vertices sharing a
  position (the engine makes it), so an inverted hull pushed along it has no gaps at hard
  edges.
- The engine defines the blend mode a variant draws with: `BLEND_NONE`, `BLEND_ALPHA`,
  `BLEND_PREMULTIPLIED`, `BLEND_ADDITIVE` or `BLEND_MULTIPLY` (`#ifdef BLEND_ADDITIVE`).
  `finalizeColor` fogs by it (additive fades to black, multiply to white);
  `finalizeStraightColor` (what library shaders call) also shapes a straight-alpha colour for
  it (premultiplied: times alpha; multiply: faded to white by alpha), so a shader needs no
  keyword of its own for the blend and draws right in every blend mode. Feature and keyword
  names `BLEND_*` are reserved.
- Library mesh shaders draw on a Terrain too (its height-mapped patches; `uv1` is the
  terrain's tiled UV, painted layers stay Lit's).

## Scene depth and colour

A mesh or points shader can read the opaque scene behind it: soft particles, depth-fade
water and fog cards, intersection glows, projected decals (depth); glass, refraction, heat
haze (colour).

**When to use it.** Only for effects that need what is behind the surface: a fade where it
meets geometry, a distortion of the background. Each costs extra GPU work in every camera that
sees it (below); prefer a cheaper look when one exists (an additive glow instead of a soft
particle on low-end mobile, a tinted alpha surface instead of refraction).

- Manifest: `// @require scenedepth`, `// @require scenecolor`, gated like any input
  (`// @require scenedepth feature=SOFT`: only the variants with `SOFT` read it and pay for
  it).
- In the `.frag`: `#include "includes/scene.glsl"` (it declares the engine's block and
  textures for the variants that require them, nothing for the others). Functions:
  `screenUV()` (0..1, y up, the current fragment), `sceneLinearDepth(uv)` (view-space
  distance in world units of the opaque surface there; the far clip distance over the sky),
  `fragmentLinearDepth()` (the same for the fragment being drawn), `sceneWorldPosition(uv)`
  (world position of the opaque surface) and `sceneColor(uv)` (linear colour of the opaque
  scene and sky, fog included, bilinear). `screenUV` and `fragmentLinearDepth` are declared
  with either input, `sceneLinearDepth` and `sceneWorldPosition` need `scenedepth`,
  `sceneColor` needs `scenecolor`. Example, soft fade:
  `alpha *= clamp((sceneLinearDepth(screenUV()) - fragmentLinearDepth()) / softness, 0.0, 1.0);`
- **Transparent queue only.** A mesh draw reading the scene must be in the transparent queue
  (`@state queue=transparent` or a blend other than opaque): it reads what the opaque queue
  drew. A material that puts such a shader (or its extra pass, which draws at the submesh's
  queue position) in the opaque queue is an error (`check_shader` without arguments,
  `check --shaders`, the material inspector, `get_entity` below): it would read its own depth
  and a black colour. The fix is a blend other than opaque on the submesh
  (`submeshes[N].material.blendMode = "alpha"`), or, on a mesh whose Auto Transparency is off
  (its `transparent` then decides the queue, whatever the blend), `transparent = true`; the
  inspector's error row has a button for the one that applies, `get_entity`'s `fix` is it.
  Points have no queue: they always draw after the copy.
- **The copy holds the opaque scene and the sky only.** Transparent objects are not in it, so
  two glass panes do not see each other through it (as Unity URP's opaque texture), and UI,
  lines and gizmos are not either. A shader drawing the copy in place of what is behind it
  (library `glass`, `water` with REFRACTION, `heat_haze` where its mask is whole) hides every
  transparent object behind it (particles, alpha meshes, other water): keep those in front
  of it or opaque. `water` REFRACTION at strength 0 draws exactly the water without it.
  Compositing over the copy in a shader: mix display-encoded values (`encodeColor`,
  `finalizeStraightColorOverScene` in `includes/output.glsl`), as the hardware blend does. Reflection probe faces draw the opaque queue only: such
  shaders are not in them. A camera drawing in hierarchy order (2D, `transparentSort` off)
  copies right before the first draw reading the colour, so the copy holds everything drawn
  before it.
- **Where the copy comes from.** The colour is copied out of an offscreen target (the screen
  cannot be read). An exported game drawing its main scene straight to the screen (no
  post-process, SSR or fixed resolution) draws it offscreen and blits it to the screen instead,
  while a colour reader is visible (and some frames after): those frames have no MSAA of the
  screen. A layer scene (a child scene or `Engine.addSceneLayer`, drawn over the main
  scene) drawn straight to the screen has no copy: its colour readers read black (its depth
  readers see the layer's own opaque meshes). Render-to-texture cameras and the editor's
  views always have one.
- **Cost**, only in the camera passes (scene camera, render-to-texture and mirror cameras,
  editor views) where such a draw is visible; a scene without one pays nothing:
  - scene depth: one more geometry pass of the opaque meshes (none extra when SSAO, SSR or a
    depth-reading post pass already draws one), and while the scene has a depth reader every
    mesh keeps a depth program;
  - scene colour: one copy of the pass after the opaque queue and the sky, and a split of its
    render pass (a store and load of colour, depth and stencil on tile-based mobile GPUs). Its
    resolution is the scene setting `scene_color_scale`: `half` (the default, a quarter of the
    pixels, slightly blurred) or `full` (sharp, four times the bandwidth and memory).
  Avoid them on low-end mobile; a few readers on screen cost the same as one (the pass is per
  camera, not per object).
- **What the tools say.** `list_shaders`: `scene_inputs` on every shader (what some variant may
  read) and `cost_note` on those that read something (which features turn it on and the cost;
  one line in the list, the whole cost with `name` / `detail`); in detail each feature and
  keyword value that turns an input on has `scene_inputs` (`scene_inputs_when_off` for a
  `feature=!X` rule). `get_entity`: `scene_inputs`, one entry per reading submesh
  (`reads`, `extra_pass_reads` of their current variants, `queue`, and on the opaque-queue
  error `error` and `fix`, the `set_properties` values that fix it) or points component, with
  `scene_inputs_cost`. `get_shader_uniforms`: `scene_inputs` and `cost_note` of that variant.
  The inspector marks the same shaders, features and keyword values in its picker with a gauge
  and shows a "Reads scene ..." row under a reading material, extra pass or points shader.

## Water

`library:water` (reference: `docs/shaders/library.md#librarywater`) is a stylised, mobile-first
water: everything beyond the base look is a compile-time feature, off by default. The quickest
start is the GameObject menu's **3D Object > Water** (`create_entity type=water`): a 20 x 20
grid of 64 x 64 quads (`Primitive:createGrid`, fine enough for the waves) with a material of
`BUILTIN_TEXTURES`, `WAVES`, `REFLECTION`, `UNDERSIDE`, `DEPTH_FOAM` and `REFRACTION`, lit per
pixel, and, when the scene has none, a `library:underwater` pass (off) at the end of its
post-processing chain; one undo step removes both. Unset, the ripple normal map is flat and the
foam noise grey (no ripples, no noise foam: water made before these features looks as it did);
`BUILTIN_TEXTURES` makes unset slots bind the engine's seamless `ripples` and `noise` instead
(no image files; the shader code is the same), and a texture set on the material wins over
either.

| Feature | What it adds | Cost |
|---|---|---|
| (none) | shallow to deep colour by the view angle, two ripple layers, foam, Blinn-Phong glint (pixel lighting); distant waves and ripples fade by the pixel's footprint (no shimmer) | two normal and two noise fetches |
| `BUILTIN_TEXTURES` | the engine's ripples and foam noise for unset texture slots | none (a different default binding) |
| `WAVES` | two sine waves raising the vertices (tessellate the mesh) | vertex sines; the physics and `Water.getHeight` follow them |
| `REFLECTION` | the sky's environment or the reflection probe around the water by Schlick's fresnel (f0 0.02), a GGX glint (`ggxSpecular`, `includes/lights.glsl`) | one cube map fetch |
| `UNDERSIDE` | both faces drawn (`@state[UNDERSIDE] cull=off`): from below, Snell's window (the sky only within 48 degrees of straight up), the depths mirrored around it | a branch |
| `DEPTH_FOAM` | foam along every shore and object crossing the surface | scene depth |
| `ABSORPTION` | colour by the water's thickness, a soft shore; with `REFRACTION` the bed loses each colour at its own rate (Beer-Lambert from the shallow colour) | scene depth |
| `REFRACTION` | the bed and what is under the surface bent by the ripples | scene colour and depth |
| `CAUSTICS` | with `REFRACTION`: sunlight (the main directional light) focused on the bed, faded with depth and where its cells would be under a few pixels; procedural, no texture | a dozen ALU instructions |
| `FLOW` | rivers: both ripple layers and the foam noise carried along the mesh's flow (vertex colour rg = 0.5 + 0.5 * direction over world XZ, b = share of `flowSpeed`), two-phase flow map over `flowCycle` seconds | twice the four texture fetches |
| `MESH_DEPTH` | shore foam and shallow to deep colour from the mesh's depth (uv0 = (metres along, depth in metres), as spline water writes it), no scene depth: the mobile shore | a few instructions |

Scene depth and colour cost what the previous section says (a depth pre-pass, a copy of the
screen); the others are a few instructions. On low-end mobile keep to the base, `WAVES` and
`REFLECTION`.

**Seen from below.** `UNDERSIDE` draws the surface from under it; the water between the camera
and everything it sees is the post-process pass `library:underwater` (each colour fading at
its own rate with the distance through the water, the view leaving the water at its surface
fading only by the part below it). It is switched by the engine, not by hand: in a running
scene (Play, an exported game; never the editor's Scene view of a stopped scene) the scene's
`library:underwater` passes are on while the main camera is under the surface of the water it
is in and off above it (`Water::updateUnderwater`, `engine/core/object/environment/Water.h`,
called by the RenderSystem's update), with `waterLevel` set to the surface over the camera and,
with `followWater` on (the default), that water's `shallowColor` and `deepColor`; a band of
0.05 world units above the surface keeps waves from switching it every frame.

**Physics and scripts.** The water's surface is known outside the shader (`Water.h`): its rest
height is the top of the mesh's world bounds, its extent their XZ, its heights the same waves
on the same clock (`frame.time.x`, wall-clock seconds since the engine started: the waves keep
moving in the editor and while a game is paused, and so do the heights scripts read). A
dynamic Rigidbody with a `buoyancy` above 0 floats in it (physics guide, "Floating in water");
the material's `buoyancy` (on) and `buoyancyDepth` (0: no bottom) decide per water, the water
whose bottom is highest wins where they overlap. Lua: `Water.getHeight(scene, x, z)`,
`Water.getNormal(scene, x, z)` (nil where no water covers the point),
`Water.isUnderwater(scene, point)`.
