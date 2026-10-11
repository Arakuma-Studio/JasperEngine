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
models draw with mesh shaders).

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
    Unset ones bind the declared default (white, black, gray or flat normal). Images only: a
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
    (`list_shaders` gives them, the default first; the built-in Lit takes pixel, vertex and
    mixed). Choosing another shader puts a mode it does not list back to auto. `mixed`
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
  turns them off too). Other keys: `blend`, `cull`, `zwrite`, `ztest`, `offset`, `queue`.
- `// @state[<DEFINE>] key=value...`: state applied while a `@feature` or `@keywords`
  value (`<GROUP>_<VALUE>`) is on; `[!<DEFINE>]` while it is off. Order: material value,
  then the matching `@state[...]` lines (later wins), then `@state`, then the engine's.
  Example: `// @state[MODE_ALPHA] blend=alpha zwrite=off`.
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

- Manifest: `// @require scenedepth`, `// @require scenecolor`, gated like any input
  (`// @require scenedepth feature=SOFT`: only the variants with `SOFT` read it and pay for
  it). `list_shaders` gives `scene_inputs` (`["scenedepth", "scenecolor"]`, what some variant
  may read) for every shader: show it as a cost.
- In the `.frag`: `#include "includes/scene.glsl"` (it declares the engine's block and
  textures for the variants that require them, nothing for the others). Functions:
  `screenUV()` (0..1, y up, the current fragment), `sceneLinearDepth(uv)` (view-space
  distance in world units of the opaque surface there; the far clip distance over the sky),
  `fragmentLinearDepth()` (the same for the fragment being drawn), `sceneWorldPosition(uv)`
  (world position of the opaque surface) and `sceneColor(uv)` (linear colour of the opaque
  scene and sky, fog included, bilinear). The first three and `sceneWorldPosition` need
  `scenedepth`, `sceneColor` needs `scenecolor`. Example, soft fade:
  `alpha *= clamp((sceneLinearDepth(screenUV()) - fragmentLinearDepth()) / softness, 0.0, 1.0);`
- The draw must be in the transparent queue (`@state queue=transparent` or a blend other
  than opaque): it reads what the opaque queue drew. A material that puts such a shader in
  the opaque queue is an error (`check_shader` without arguments, `check --shaders`, the
  material inspector): it would read its own depth and a black colour. Transparent objects
  are not in the colour copy, so they do not see each other through it.
- Cost, only in the camera passes (scene camera, render-to-texture and mirror cameras,
  editor views) where such a draw is visible: the scene depth is a depth pre-pass of the
  opaque meshes (none extra when SSAO, SSR or a depth-reading post pass already draws one);
  the scene colour is one copy of the pass after the opaque queue and the sky, at the scene
  setting `scene_color_scale` (`half`, the default, or `full`; linear filtering). While the
  scene has a depth reader, every mesh keeps a depth program. A camera drawing in hierarchy
  order (2D, `transparentSort` off) copies right before the first draw reading the colour,
  so the copy holds everything drawn before it. Reflection probe faces draw the opaque
  queue only: such shaders are not in them.
