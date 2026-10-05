# Custom shaders

Served as `jasper://docs/shaders`.

## Forking

Custom shaders are per component on Mesh, UI, Points, Lines (Line Renderer) and Sky
(Skybox), through the `customShader` property. To customize, call `fork_shader`: it copies the
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
`.vert`/`.frag`/`.glsl` compiles the forks using it and returns the errors (line + message);
after editing on disk, call `sync_files` then `check_shader` (no arguments: every fork in use).

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
  with `inspect_shader_uniforms`. Scripts: `mesh:setShaderUniform("amplitude", 0.3)`,
  `scene:setPostProcessUniform(0, "amount", 0.5)`.
- The engine writes `time` (seconds since startup) and `resolution` (xy = render target size,
  zw = 1/size) every frame: declare them to animate, never set them.
- One scalar kind per block (do not mix int and float members: GL flattens the block after
  its first member). No matrix or array members (they stay zero): one scalar/vector member per
  value. An unset member reads zero: set it or give the effect a sensible fallback.
