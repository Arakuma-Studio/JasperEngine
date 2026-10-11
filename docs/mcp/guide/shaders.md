# Shaders

Served as `jasper://docs/shaders`.

## Library shaders (use these first)

The engine ships ready-made shaders, referenced as `"library:<name>"`. `list_shaders` lists
them (name, type, category, description, lighting modes); `list_shaders name="toon"` (or
`detail: true`) adds each one's parameters (member, kind, default, range, label, `feature`
gating), textures (uniform, default), features, keyword groups, the inputs it requires
(`require`, plus `require_rules`: inputs only while a feature is on, `"!X"` while off) and its
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
    (transparent order, lower first). `auto` takes the shader's default state, else the
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

## Forking

Custom (forked) shaders are set per component on Mesh, UI, Points, Lines (Line Renderer) and
Sky (Skybox), through the `customShader` property (a mesh material's `shader` also takes a fork). To customize, call `fork_shader`: it copies the
built-in `<type>.vert` / `<type>.frag` into a project directory (default `shaders`) and sets
`customShader` in one undoable step; `fork_includes` also copies the engine includes into an
editable `includes/` folder beside the fork.

- Never set `customShader` by hand to files that do not exist. The value is a base path with
  no extension (`base.vert` + `base.frag`), or two explicit paths joined by `|`
  (`"a.vert|b.frag"`) when the names differ. Both a .vert and a .frag are always required.
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
  position`; `rawcolor` gives `v_rawColor`, the vertex colour without the instance colour
  (`v_color` is their product).
- The engine defines the blend mode a variant draws with: `BLEND_NONE`, `BLEND_ALPHA`,
  `BLEND_PREMULTIPLIED`, `BLEND_ADDITIVE` or `BLEND_MULTIPLY` (`#ifdef BLEND_ADDITIVE`).
  `finalizeColor` fogs by it (additive fades to black, multiply to white);
  `finalizeStraightColor` (what library shaders call) also shapes a straight-alpha colour for
  it (premultiplied: times alpha; multiply: faded to white by alpha), so a shader needs no
  keyword of its own for the blend and draws right in every blend mode. Feature and keyword
  names `BLEND_*` are reserved.
- Library mesh shaders draw on a Terrain too (its height-mapped patches; `uv1` is the
  terrain's tiled UV, painted layers stay Lit's).
