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
  linked submeshes follow it. Setting a field of a linked submesh's material with
  `set_properties` makes it the GameObject's own material (unlinked, the file untouched: a
  linked submesh takes the file's values when its scene opens); `update_material` changes them all.
- **UI, Points, Lines and Sky shaders** go in the component's `customShader`; their
  parameters are its `shaderUniforms`. A Line Renderer and a Trail Renderer both take `lines`
  shaders (`list_shaders type=lines`, e.g. `library:line_flipbook`); with a width they draw a
  textured ribbon: the built-in line shader samples `shaderTextures.u_baseTexture` (the
  `texture` shortcut) times the line's colour, a library or forked one its own textures at
  `lineUV()` (`includes/lines_fs.glsl`). A hairline (width 0) has no texture coordinates
  (jasper://docs/lines-trails).
  A UI shader gets the element's colour with its Canvas Group alpha already applied in
  `uiParams.color` (jasper://docs/ui): one that ignores that uniform does not fade with its
  group, but the element is still not drawn at group alpha 0.
- Values (paths of the submesh's material, `submeshes[N].material.<key>`):
  - `shaderParams`: `{member: value}`, or one entry `shaderParams.<member>`. Numbers or
    `[x,y,z,w]` as stored: colours are **linear**; a `"#RRGGBB[AA]"` string is sRGB and is
    converted. A member left out uses the manifest default; `null` removes one.
    `set_shader_uniform` (entity + `submesh`) sets one and `get_shader_uniforms` lists them
    with labels and defaults.
  - `shaderTextures`: `{uniform: "assets/path.png" | null}`, or `shaderTextures.<uniform>`.
    Unset ones bind the declared default (white, black, gray, flat normal, or the engine's seamless `ripples` normal map, `noise` and soft `particle`), or the one a `default[<DEFINE>]=` rule picks while that feature is on (`list_shaders`: `default_with`). Images only: a
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
    engine's. UI, Points, Lines and Trails have `blendMode` and `depthWrite` (a thick line's or a trail's auto: alpha, no depth write).
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
- The engine writes `time` (seconds since startup), `sceneTime` (the scene clock: scaled game
  seconds, held while paused) and `resolution` (xy = render target size, zw = 1/size) every
  frame: declare them to animate, never set them.
- One scalar kind per block (do not mix int and float members: GL flattens the block after
  its first member). No matrix or array members (they stay zero): one scalar/vector member per
  value. An unset member reads zero: set it or give the effect a sensible fallback.
- A fork may declare the manifest lines library shaders use (`// @property`, `@texture`,
  `@feature`, `@state`, ...; read `engine/shaderlib/library/unlit.frag` with
  `read_engine_source`): its parameters then get labels and defaults, and it takes
  `shaderTextures` and `shaderFeatures` like a library shader.

## Global values

Values set once and read by every shader that declares them: Unity's `Shader.SetGlobalColor`
/ `SetGlobalVector` / `PropertyToID`, Godot's global uniforms. Wind, a fog tint, the player's
position, a game's own lights.

**Declare** them in Project Settings > Graphics > Shader Globals, or with
`set_project_settings {"graphics.shader_globals": [{name, type, default, array_size}]}`
(`graphics.shader_globals.<name>` upserts one: `{type, default}`, `name` renames it, `null`
removes it; undoable). The Scene and Game views show the defaults; Play starts and stops with
them; exported builds declare them before the first scene loads. Names are GLSL identifiers
(Unity's leading `_` is fine), not `u_...` or `gl_...`; at most 256.

| type | GLSL member | `default` |
|---|---|---|
| `float` | `float` | `0.5` |
| `vector2` / `vector3` / `vector4` | `vec2` / `vec3` / `vec4` | `[x, y, ...]` |
| `color` | `vec4` (linear) | `"#RRGGBB[AA]"` or `[r, g, b, a]`, sRGB |
| `matrix4` | `mat4` | 16 numbers, column-major |
| `float_array` / `vector4_array` | `vec4 x[N]` (a float array in `.x`) / `vec4 x[N]` (`array_size` N, 1..64) | N values |
| `texture2d` / `texturecube` | `uniform texture2D x;` + `uniform sampler x_smp;` | an asset path (`""`: white / black) |

**Read** them in a fork through one block per stage: `u_vs_globals` in the .vert,
`u_fs_globals` in the .frag, any instance name, members named like the globals, only the ones
the stage reads (a block of its own, beside `u_*_customParams`). `get_project_settings keys:
["graphics.shader_globals"]` returns `glsl_vs` / `glsl_fs`, the blocks for the whole list (the
Project Settings page copies them: Copy GLSL). A texture global binds to a texture uniform of
its name in either stage (its sampler is the one the shader pairs with it).

```glsl
uniform u_fs_globals {
    vec4 FogTint;
    vec4 _Wind[4];    // a float_array: x[i].x
} globals;
uniform texture2D _NoiseTex;
uniform sampler _NoiseTex_smp;
// ... texture(sampler2D(_NoiseTex, _NoiseTex_smp), uv) * globals.FogTint
```

**Set** them from Lua (main thread; a set in `onUpdate` shows in that frame):

```lua
Shader.setGlobalColor("FogTint", Vector4(1, 0.5, 0, 1))      -- sRGB in, linear in the shader
local wind = Shader.propertyToID("_Wind")                     -- once
Shader.setGlobalFloatArray(wind, {0.2, 0.4, 0.1, 0})          -- per frame
Shader.setGlobalTexture("_NoiseTex", "textures/noise2.png")  -- a Texture, a path, a Framebuffer; nil: the default
```

Also `setGlobalFloat`, `setGlobalInt` (stored as a float, like Unity), `setGlobalVector`,
`setGlobalMatrix`, `setGlobalVectorArray`, their `getGlobal*` (colours back as sRGB; nil when
unknown), `hasGlobal`, `getGlobalNames` and `resetGlobals` (back to the defaults; the names
scripts made are dropped). A name nobody declared exists from its first set (typed by that
setter; an array sized by it) until Play stops; `check_shader` warns about shader members that
are not declared. C++: `ShaderGlobals::propertyToID`, `setColor`, `setVector`...
(`shader/ShaderGlobals.h`). Values persist across scene loads, like Unity.

**Bind a library shader's parameter** (no fork): the link button at the end of a parameter row
in the Inspector, `set_properties submeshes[0].material.shaderGlobalBindings.tint: "FogTint"`
(a component: `shaderGlobalBindings.<member>`; `null` unbinds; a post-process pass:
`shaderGlobalBindings` in its `set_scene_settings post_process` entry), or Lua
`m:setShaderGlobalBinding("tint", "FogTint"); mesh:setMaterial(m)` (also on Image, Text,
Polygon, Points, LineRenderer, TrailRenderer, Skybox, and `scene:setPostProcessGlobalBinding`).
The parameter then takes the global's current value over its own and the manifest default; a
colour binds to colour and vector4 globals, a vector to one of its size, a float to a float.
`get_shader_uniforms` lists `globals` (what the shader reads, with the values) and
`global_bindings`.

Rules:
- A member no global feeds reads 0 (the bytes are zero). A member of another type reads the
  global's leading components (a `vec3` of a `vector4`: xyz; a `float` of a vector: x; a
  `vec4` of a float: (x, 0, 0, 0)); a `mat4` reads only a `matrix4`. An array longer than the
  global's reads 0 past it, a shorter one its first elements. Only float members (no `int`,
  `mat3`, `bool`). The OpenGL and WebGL targets flatten blocks: arrays only of `vec4` (a float
  array in `.x`: std140 strides it 16 bytes either way) or `mat4`, and no indexing of a block's
  vector by a variable (copy it to a local first, as below). `check_shader` reports `global-undeclared` and `global-type-mismatch`
  (warnings), `globals-block-too-large` (error: a block over 1024 bytes, 64 vec4, fails like
  any compile error and the object draws with the built-in shader) and `uniform-block-limit`
  (error: more than 8 blocks in a stage; a globals block counts).
- Both stages declaring a global read the same value. Depth and shadow forks that read globals
  in the vertex stage (wind) get them too, so shadows match.
- Cost: a shader without the blocks pays nothing. One that declares them gets its block pushed
  per draw like the frame block, packed once per change of any value (shared by every program
  with the same block). A bound parameter rewrites its object's block once per frame in which
  a global changed.
- Renaming a global, or removing one that bindings use, waits for Play to stop (Project
  Settings refuses it while playing, and its undo too; `set_project_settings` is refused while
  playing anyway).
- A texture global holding a render target (a camera's, a `Framebuffer` from Lua) that is
  destroyed takes its default texture again (a warning says so); a file that cannot be loaded
  binds the fallback until the value changes (warned once).
- Not here: global keywords (`Shader.EnableKeyword`; features are per-material variants), and
  integer globals.

**Vertex work at the world position**: a fork on the library vertex path defines
`SURFACE_VERTEX` and gets `surfaceVertex(worldPosition, worldNormal, color, uv)` called after
the engine's own vertex code, to write varyings of its own. The game's four vertex lights
(Unity `VertexLightsUpdater`: colours and the X/Y/Z positions and radii of four lights, as
vec4s, set every frame):

```glsl
// lights.vert
#version 450
#define SURFACE_VERTEX
#define SURFACE_NO_VERTEX_COLOR
#include "includes/surface_vs.glsl"
uniform u_vs_globals {
    vec4 _LightPositionX; vec4 _LightPositionY; vec4 _LightPositionZ; vec4 _LightRadius;
    vec4 _VertexLightColor0; vec4 _VertexLightColor1; vec4 _VertexLightColor2; vec4 _VertexLightColor3;
} globals;
out vec3 v_light;
void surfaceVertex(vec3 worldPosition, vec3 worldNormal, vec4 color, vec2 uv) {
    vec4 colors[4] = vec4[4](globals._VertexLightColor0, globals._VertexLightColor1,
                             globals._VertexLightColor2, globals._VertexLightColor3);
    // copies: the GL targets cannot index a block's vector by a variable
    vec4 px = globals._LightPositionX, py = globals._LightPositionY, pz = globals._LightPositionZ, radius = globals._LightRadius;
    v_light = vec3(0.0);
    for (int i = 0; i < 4; i++) {
        vec3 toLight = vec3(px[i], py[i], pz[i]) - worldPosition;
        float fade = clamp(1.0 - length(toLight) / max(radius[i], 0.0001), 0.0, 1.0);
        v_light += colors[i].rgb * fade * max(dot(worldNormal, normalize(toLight)), 0.0);
    }
}
```

```glsl
// lights.frag
#version 450
in vec3 v_light;
out vec4 g_finalColor;
#include "includes/output.glsl"
void main() { g_finalColor = finalizeColor(vec4(v_light, 1.0)); }
```

```lua
-- VertexLightsUpdater.lua: four light GameObjects in `lights`
function Updater:onUpdate()
    local x, y, z = {}, {}, {}
    for i = 1, 4 do
        local p = self.lights[i]:getWorldPosition()
        x[i], y[i], z[i] = p.x, p.y, p.z
        Shader.setGlobalColor("_VertexLightColor" .. (i - 1), self.colors[i])
    end
    Shader.setGlobalVector("_LightPositionX", Vector4(x[1], x[2], x[3], x[4]))
    Shader.setGlobalVector("_LightPositionY", Vector4(y[1], y[2], y[3], y[4]))
    Shader.setGlobalVector("_LightPositionZ", Vector4(z[1], z[2], z[3], z[4]))
    local r = self.radius
    Shader.setGlobalVector("_LightRadius", Vector4(r, r, r, r))
end
```

`tools/fixtures/shader-globals` has these shaders and a fork reading a colour and a texture
global.

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
  ripple normal map), `noise` (seamless grey value noise) and `particle` (a soft white disc, Unity's
  Default-Particle), the last three built by the engine, so a material can use them without image
  files (`library:water`'s `BUILTIN_TEXTURES`, `library:unlit`'s `PARTICLE_TEXTURE`: what Particle
  Systems draw with when no base map is set, jasper://docs/particles).
- `// @require <inputs> feature=<DEFINE>`: inputs required only while that define is on
  (`feature=!<DEFINE>`: while off). Inputs: `normals tangents uv1 uv2 color rawcolor
  position smoothnormals normalmap`; `rawcolor` gives `v_rawColor`, the vertex colour without the
  instance colour (`v_color` is their product); `smoothnormals` gives a `SURFACE_DISPLACE`
  hook `surfaceSmoothNormal`, the object-space normal averaged over the vertices sharing a
  position (the engine makes it), so an inverted hull pushed along it has no gaps at hard
  edges; `normalmap` (2D lights read the material's Normal Texture through `getNormal2D()`;
  `library:sprite` uses it): under Light 2Ds, with a normal texture, on a mesh with tangents
  and `uv1` (sprites, tilemaps, polygons), `includes/lighting2d.glsl` samples it in the OpenGL
  convention (green up) and the light's height shapes it; the shader must include that file.
  A Light 2D's height shapes only normals that mean something: `lighting2d.glsl`'s
  `getLight2DIntensity` applies N.L (with the light raised by its height) only when it sampled
  the normal map itself (`normalmap` above) or when the shader defines `LIGHT2D_SURFACE_NORMALS`
  before including it; a shader that passes its own normals (from `getNormal2D()`, a texture of
  its own, real 3D normals) must `#define LIGHT2D_SURFACE_NORMALS` first, otherwise its normal is
  ignored and the light falls as from height 0 (`mesh.frag`'s lit path does this).
  `waterexclusion` and `waterwake` (mesh shaders; they bring `position`): the Water Excluders
  over the draw and the scene's Water Wake map, through `includes/water_interaction.glsl` (put
  `WATER_INTERACTION_PARAMS`, from `includes/water_interaction_params.glsl`, last in your
  `u_fs_customParams`: the engine writes their values there; `waterExcluded(worldPosition)`: discard after every derivative read; `waterWake(worldXZ)`:
  foam and the wake waves' slope). The variant takes them ('Wex', 'Wwk') only while the scene
  has such components, so a scene without them compiles the shader as if it never asked.
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
- **The copy holds what was drawn before the first reader.** Every camera pass copies right
  before the first draw reading the colour: a 3D pass after its opaque queue, the sky and the
  transparent draws the queue puts first (those a water's surface covers, see Water below), a 2D
  pass (or `transparentSort` off) everything drawn before it in hierarchy order. Two glass panes
  at the same depth layer do not see each other (as Unity URP's opaque texture); a reader over
  one of a higher water rank (a refracting pool over a refracting lake) gets the copy again, at
  most four copies a pass (`get_render_stats` `scene_color_copies`). UI, lines and gizmos are
  not in it. A shader drawing the copy in place of what is behind it (library `glass`, `water`
  with REFRACTION, `heat_haze` where its mask is whole) hides what the copy lacks: transparents
  drawn after it in the queue, that is in front of it by distance. Compositing over the copy in
  a shader: mix display-encoded values (`encodeColor`, `finalizeStraightColorOverScene` in
  `includes/output.glsl`), as the hardware blend does. The copy replaces (no blend), so its
  colour is right where the target's alpha is under 1. Reflection probe faces draw the opaque
  queue only: such shaders are not in them.
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
`BUILTIN_TEXTURES`, `WAVES`, `GERSTNER`, `REFLECTION`, `UNDERSIDE`, `DEPTH_FOAM` and
`REFRACTION`, lit per pixel, and, when the scene has none, a `library:underwater` pass (off) at the end of its
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
| `GERSTNER` | with `WAVES`: four Gerstner waves (lengths 1, 0.6, 0.37, 0.23 of `waveLength`, shorter ones slower), sharp crests by `waveSteepness` (0..1, never folding), the surface rolling back and forth; foam on the squeezed crests | about thirty ALU a pixel and a varying; the physics, `Water.sample` (its `flowVelocity` rolls too) and the underwater switch follow them |
| `REFLECTION` | the sky's environment or the reflection probe around the water by Schlick's fresnel (f0 0.02), a GGX glint (`ggxSpecular`, `includes/lights.glsl`) | one cube map fetch |
| `PLANAR_REFLECTION` | with `REFLECTION`: the scene mirrored in the water instead of the environment, bent by the ripples (`reflectionDistortion`) | the scene drawn again per water height a camera sees (Planar Reflections below) |
| `UNDERSIDE` | both faces drawn (`@state[UNDERSIDE] cull=off`): from below, Snell's window (the sky only within 48 degrees of straight up), the depths mirrored around it | a branch |
| `DEPTH_FOAM` | foam along every shore and object crossing the surface | scene depth |
| `ABSORPTION` | colour by the water's thickness, a soft shore; with `REFRACTION` the bed loses each colour at its own rate (Beer-Lambert from the shallow colour) | scene depth |
| `REFRACTION` | the bed and what is under the surface bent by the ripples | scene colour and depth |
| `CAUSTICS` | with `REFRACTION`: sunlight (the main directional light) focused on the bed, faded with depth and where its cells would be under a few pixels; procedural, no texture | a dozen ALU instructions |
| `FLOW` | rivers: both ripple layers and the foam noise carried along the mesh's flow (vertex colour rg = 0.5 + 0.5 * direction over world XZ, b = share of `flowSpeed`), two-phase flow map over `flowCycle` seconds | twice the four texture fetches |
| `MESH_DEPTH` | shore foam and shallow to deep colour from the mesh's depth (uv0 = (metres along, depth in metres), as spline water writes it), no scene depth: the mobile shore | a few instructions |

Scene depth and colour cost what the previous section says (a depth pre-pass, a copy of the
screen); `PLANAR_REFLECTION` a camera pass; the others are a few instructions. On low-end mobile
keep to the base, `WAVES` and `REFLECTION` (or set the scene's planar reflections to `quarter` or
`off`).

**Planar reflections.** Each camera (the scene camera, the editor's views; not render-to-texture
cameras or probe faces) draws one reflection pass per water height it sees with
`PLANAR_REFLECTION` (tiles of one lake at one level share it; the nearest heights first, up to
`max_planar_reflections`, default 2; the others, and a camera under the surface, show the
environment). The scene settings (Inspector Scene settings > Planar Reflections,
`set_scene_settings`, Lua `scene.planarReflection*`): `planar_reflection_resolution` (`off`,
`quarter`, `half` the default, `full`: per side of the view; `off` draws none), `_layers` (with
the camera's culling mask), `_shadows` (the camera's own shadow maps; no map is drawn for it),
`_far_clip` (0: the camera's), `_clip_offset` (what is within it above the rest level is left
out, Unity's Clip Plane Offset). A reflection pass draws no reflecting water and no reader of
the scene (no glass in the reflection) and takes the camera's detail levels.
`get_render_stats` `planar_reflections` and `get_scene_settings` `planar_reflections_in_use`
say what the last frame drew. Made for flat water: `check` warns (`planar-reflection-not-flat`)
on a sloped mesh.

**Layering.** In a 3D pass, transparent draws are ranked around the visible waters: under a
surface the camera looks down through (or above one it looks up through) draws before the water,
on the camera's side after it, whatever the distances of their transforms (a fish under a large
lake no longer draws over it). The rank comes after `sortPriority`, which still wins.

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
on the same clock: the scene clock (`Water.getTime(scene)`, the engine-written `sceneTime` of
the shader), the seconds the scene has run scaled by the time scale. It holds while the game or
Play is paused and at `Time.timeScale = 0`, starts at 0 when the scene starts, and in a stopped
scene follows the Scene view's Animate water (on; `set_scene_camera display {animate_water}`).
`sample_water` returns it as `water_time`. Any shader can run on it by declaring a `sceneTime`
member in its custom block. A
dynamic Rigidbody with a `buoyancy` above 0 floats in it (physics guide, "Floating in water");
the material's `buoyancy` (on) and `buoyancyDepth` (0: no bottom) decide per water, the water
whose bottom is highest wins where they overlap. Lua: `Water.getHeight(scene, x, z)`,
`Water.getNormal(scene, x, z)` (nil where no water covers the point),
`Water.isUnderwater(scene, point)`.

**Boats.** A **Water Excluder** (Unity HDRP's name; `add_component component=WaterExcluder`)
keeps water out of a boat, a cabin or a dry tunnel: a Convex Hull of the GameObject's mesh (the
default; or of its model's mesh nodes, not its children), a Box or a Sphere (`center`, `size`,
local). Inside it no surface is drawn, a camera sees no underwater fade, `Water.isUnderwater` is
false and nothing floats but the Rigidbody it belongs to (the nearest Rigidbody on its
GameObject or above), so cargo in a boat stays put while the boat floats. Disabled, inactive or
hidden, it keeps nothing out. A water draw cuts at most the 8 excluders over it in the camera's
view, nearest first. A **Water Wake** leaves foam (stern churn, hull spray) and Kelvin waves on
the water while its GameObject moves through the surface (a running scene; frozen while paused):
`foam`, `waves` (0..2), `width` and `length` (0: the excluder's or the mesh's footprint). The
wakes live in one map around the main camera, scene settings `water_wake_resolution` (`auto`:
256, 128 on mobile and web), `water_wake_area` (64 m), `water_wake_fade` (8 s), and stay on the
water they were made on. Neither costs anything in a scene without them. Check with
`sample_water` (`excluded_by`, `wake` per point; `report: true` lists the hulls, the emitters and
which waters draw with them).

**Forks.** The engine treats a shader as water by its manifest's `// @role water` (and as the
underwater pass by `// @role underwater`), not by its name. Fork a library shader with
`fork_shader {source: "library:water", target: "material", entity}` (or `post_process` with
`library:underwater`), or the Inspector's Shader picker > Fork "Water"...: the copy keeps the
manifest, so its parameters, features and role carry over, and the bodies keep floating.
`check` warns (`water-fork-without-role`) about a hand-made copy without the line.

## Asynchronous creation and the warm-up list

Shaders and pipelines are created off the render thread (docs/rendering/async-shaders.md). While
a variant is pending, a mesh draws the nearest ready variant (fewer optional features), and the
exact one replaces it once it is ready. A pipeline being created is not drawn for those frames.

- `get_render_stats.shader_creation`: `mode` (`worker`, `parallel`, `deferred`, `thread`, `metal`,
  `sync`), `pending_shaders` / `pending_pipelines` / `pending_warm_draws`, `last_frame_main_ms`,
  `max_main_ms` (the render thread's creation work), and `warmup` {recording, shaders, pipelines}.
- `screenshot` (and tools that return one, and `sample_view_color`) waits for every `pending_*` to
  be 0 and no texture to be loading, up to its `timeout_seconds`, then captures; past it the call
  fails and names what is still loading. It never shows a stand-in. Other readings
  (`get_render_stats`, picking) do not wait: poll `pending_*` first. A stand-in frame is correct
  but not final.
- Settings (`set_project_settings`): `shader_creation.async_creation`,
  `shader_creation.main_thread_budget_ms`, `shader_creation.gl_warm_draws`, and for the warm-up
  list `shader_creation.record_during_play`, `include_in_builds`, `hold_first_scene`.
  `shader_creation.gl_warm_draws` is on by default for desktop GL on a GPU, off on GLES, and never
  runs on a software GL (llvmpipe), which also has no worker context (`mode` is `parallel` or `deferred` there). The list's entry
  count is a Project Settings row (`get_project_settings` `search` finds
  `shader_creation.warmup_entries`; the file is `ShaderWarmup.json`).
- The warm-up list (`ShaderWarmup.json` next to `project.yaml`): turn on Record During Play, play
  the scenes, stop. Exports then build those variants and the game creates them before its first
  scene. `run_command` `assets.clearShaderWarmup` empties it.
- Lua: `ShaderPool.getPendingShaderCount()`, `ShaderPool.getPendingPipelineCount()`,
  `ShaderPool.isGpuCreationIdle()`, `ShaderWarmup.isDone()`, `ShaderWarmup.getProgress()`.
