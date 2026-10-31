# Shader globals: user-defined global shader uniforms (engine gaps #14, `gaps/shader-globals`)

Status: binding design. Read `docs/engine-gaps/AGENT-BRIEF.md` first: its shared conventions are binding.
Branch `gaps/shader-globals`.

## 1 Goals

1. Scripts set **global shader values** once and every shader that reads them sees them, like Unity's
   `Shader.SetGlobalFloat/Int/Vector/Color/Matrix/Texture/FloatArray/VectorArray` and `Shader.PropertyToID`,
   and Godot's `global uniform` + `RenderingServer.global_shader_parameter_set`.
2. Shaders read globals through **one engine-recognised uniform block per stage**, `u_vs_globals` /
   `u_fs_globals`, declared with only the members they need, filled **by member name**; global textures bind by
   texture name. Works in forks, custom shaders and library shaders, on every backend (std140 layout, GL's
   flattened blocks included).
3. **Library shaders and the built-in material path use globals without forking**: any `@property` of a
   material or component can be **bound to a global** (a Unity-like "this property follows the global").
4. **Project Settings > Graphics > Shader Globals** declares globals (name, type, default) visible in edit mode,
   in Play (live values, editable), and in exported builds.
5. Cheap: packed at most once per frame per shader program, pushed per draw like the frame block; zero cost for
   shaders that do not declare the block.
6. The game's use cases: the vertex-lighting updater (`S/VertexLighting/VertexLightsUpdater.cs:59-77`: four
   `_VertexLightColor0..3` colours and `_LightPositionX/Y/Z`, `_LightRadius` vectors set every frame, read by the
   Amplify shaders `Assets/Shaders/General.shader:120-127`, `Character.shader`, `UnlitLights.shader`,
   `SimpleWall.shader` in Menu/A/D/G/H/I), and a global fog/tint style colour.

## 2 Non-goals

- `MaterialPropertyBlock`/per-renderer overrides (already covered by component `shaderUniforms`).
- Global **keywords** (`Shader.EnableKeyword`): features are compile-time variants per material in this engine
  (`setShaderFeature`); a global keyword would rebuild every variant. Out of scope, documented.
- Integer-typed globals: Unity's `SetGlobalInt` historically stores a float; same here (§5.6). No `ivec` members.
- Compute buffers / `SetGlobalBuffer`: no storage buffers on WebGL2/GLES3.
- Changing built-in Lit's own uniforms (its frame block is engine data).

## 3 Users and stories

- **Unity porter**: keeps `VertexLightsUpdater` logic in Lua: `Shader.setGlobalColor("_VertexLightColor" .. i,
  c)` and `Shader.setGlobalVector("_LightPositionX", Vector4(...))` every `onUpdate`; forks `library:vertex_lit`
  (or writes a custom mesh shader) declaring `u_vs_globals { vec4 _LightPositionX; ... }`; it lights.
- **Artist**: Project Settings > Graphics > Shader Globals: adds `FogTint` (Color, grey); on every toon and
  unlit material, clicks the link icon next to **Tint** and picks `FogTint`; changes the default and every
  material follows in the Scene view; in Play drags the live value.
- **Scripter**: `local id = Shader.propertyToID("_Wind")` once, then `Shader.setGlobalVector(id, w)` per frame.
- **AI agent**: `set_project_settings {"graphics.shader_globals.FogTint": {"type": "color", "default": "#808080"}}`;
  `get_project_settings keys: ["graphics.shader_globals"]` returns the list and a ready GLSL block to paste;
  `fork_shader` + `write_file` with that block; `check_shader` reports members that are not declared globals;
  in Play `eval_lua "Shader.setGlobalColor('FogTint', Vector4(1,0,0,1))"` then `screenshot`.

## 4 Research summary

- Unity `Shader.SetGlobal*` / `Shader.PropertyToID` (<https://docs.unity3d.com/ScriptReference/Shader.SetGlobalVector.html>):
  global values apply to every shader property of that name **that the material does not define** (not in the
  shader's `Properties` block); ids are process-wide ints; colours are converted to linear in Linear colour
  space; arrays are fixed in size by the first set. No editor UI to declare them. **Take**: names, id API, colour
  conversion, array setters, "unknown names are allowed". **Leave**: the implicit "material wins unless absent"
  rule as the only mechanism, which is invisible in the Inspector; we make binding explicit (§5.3).
- Godot 4 global uniforms (<https://docs.godotengine.org/en/stable/tutorials/shaders/shader_reference/shading_language.html#global-uniforms>):
  declared in Project Settings > Shader Globals with type and default, read with `global uniform vec4 x;`,
  set with `RenderingServer.global_shader_parameter_set`, stored in one big buffer (`limits/global_shader_variables/buffer_size`).
  **Take**: the Project Settings list with defaults shown in the editor, live in edit mode. **Leave**: a new GLSL
  keyword (the engine compiles standard GLSL 450 through glslang/spirv-cross; a block declaration is standard
  and keeps forks valid GLSL).
- Unreal Material Parameter Collections (named scalar/vector sets, 1024 scalars + 1024 vectors cap, referenced by
  materials, set from Blueprint): **take** the per-material binding idea and caps; **leave** separate assets
  (one project list is enough here).
- sokol-gfx (`engine/libs/sokol/sokol_gfx.h`): uniforms are per-draw pushes (`sg_apply_uniforms`) copied into a
  per-frame ring (Metal/D3D11/Vulkan) or `glUniform4fv` on the flattened block (GL); at most 8 uniform blocks per
  stage (`_SG_MAX_UNIFORMBLOCK_BINDINGS_PER_STAGE`). There is no persistent UBO binding across draws, so "one UBO
  per frame" is realised as "one packing per frame, the same bytes pushed per draw" (the frame block's model,
  `RenderSystem::applyFrameBlock`, `RenderSystem.cpp:4759`).

## 5 Product decisions

1. **Declaration in GLSL is a standard block** named `u_vs_globals` (vertex) / `u_fs_globals` (fragment), any
   instance name, members whose names are global names. The engine discovers it by name at load
   (`loadShaderLibraryBindings`, `RenderSystem.cpp:3886`) like the frame block. A shader declares only what it
   reads; each stage independently. It is not the "one custom block per stage" (`u_*_customParams`): both can
   coexist. It counts towards sokol's 8 blocks per stage (`check_shader` reports `uniform-block-limit`).
2. **Project Settings is the source of declarations and defaults**; scripts may also set names that are not
   declared (Unity compatibility): they exist from the first set until `Shader.resetGlobals()` / Play stop, and
   `check_shader` warns `global-undeclared` for shader members not in Project Settings (they read 0 until set).
3. **Bindings make library shaders global-aware**: a material (submesh) or a UI/Points/Lines/Sky/Trail
   component maps any of its shader's custom-block members to a global (`shaderGlobalBindings: {member:
   globalName}`); a bound member takes the global's current value, overriding the stored value and the manifest
   default. Explicit, visible in the Inspector, works with every library shader without forking.
4. **Values persist across scene loads** (process-wide, like Unity) and are **reset to the Project Settings
   defaults** at app start, at editor Play start and at Play stop.
5. **Types**: `float`, `vector2`, `vector3`, `vector4`, `color` (a vector4 shown and set as sRGB, stored linear),
   `matrix4`, `float_array[N]`, `vector4_array[N]` (N 1..64), `texture2d`, `texturecube`. GLSL member types:
   float, vec2, vec3, vec4, mat4, `float x[N]`, `vec4 x[N]`; texture globals bind to `texture2D`/`textureCube`
   uniforms of the same name with a sampler named `<name>_smp` (the engine's sampler pairing convention).
6. `Shader.setGlobalInt` stores a float (Unity's historical behaviour); documented.
7. **Caps**: 256 declared globals per project; a globals block at most 64 vec4 slots (1024 bytes) per stage
   (GLES3 guarantees 256 vertex uniform vectors in total, shared with the engine's own blocks); arrays up to 64.
   Exceeding: `check_shader` error `globals-block-too-large`, the shader falls back like any failed fork.
8. **Names**: GLSL identifiers `[A-Za-z_][A-Za-z0-9_]{0,63}`, not starting with `gl_` or `u_` (engine
   uniforms), no `__`; Unity's leading underscore (`_VertexLightColor0`) is valid and recommended for ported
   shaders.
9. **Main thread only** (Lua and C++ setters), the values a frame draws with are those set before its render.

## 6 Design

### 6.1 Runtime C++ (`engine/core/shader/ShaderGlobals.{h,cpp}`, new)

```cpp
namespace jasperengine {
    enum class ShaderGlobalType : uint8_t { Float, Vector2, Vector3, Vector4, Color, Matrix4,
                                            FloatArray, Vector4Array, Texture2D, TextureCube };
    struct JASPERENGINE_API ShaderGlobalDecl {   // a Project Settings entry
        std::string name; ShaderGlobalType type = ShaderGlobalType::Vector4;
        unsigned int arraySize = 0;              // arrays: 1..64
        std::vector<Vector4> defaultValue;       // linear; 1 element (matrix: 4 columns; arrays: N)
        std::string defaultTexture;              // asset path, "" = white / black cube
    };
    class JASPERENGINE_API ShaderGlobals {       // static, main thread
    public:
        static int propertyToID(const std::string& name);    // stable for the process, creates the name
        static const std::string& nameOf(int id);
        static void declare(const std::vector<ShaderGlobalDecl>& decls); // replaces declarations, resets values
        static void resetToDefaults();
        static bool setFloat(int id, float v);  static bool setVector(int id, const Vector4& v);
        static bool setColor(int id, const Vector4& srgb);   // rgb sRGB -> linear, alpha as is
        static bool setMatrix(int id, const Matrix4& m);
        static bool setFloatArray(int id, const float* v, size_t n);
        static bool setVectorArray(int id, const Vector4* v, size_t n);
        static bool setTexture(int id, const Texture& t);  // path, framebuffer, data
        static bool get(int id, ShaderGlobalValue& out);    // type + values + texture
        static uint64_t getVersion();      // +1 per change of any value (textures included)
        static uint64_t getNamesVersion(); // +1 when a name is created/declared/removed
        static std::vector<std::string> getNames();
        static bool isDeclared(int id);
    };
}
```

Value storage: `std::vector<Vector4>` per global (16-byte slots: float in .x, matrix as 4 columns, arrays N
slots) plus a `Texture` for texture types. A setter of another kind converts: float -> (x,0,0,0) for vector
types; vector -> x for float; a matrix global only takes `setMatrix` (others: false + `Log::error` once per name
"global '<n>' is Matrix4"); arrays take arrays (a vector setter writes element 0). An undeclared name takes the
type of its first setter (like Unity's arrays: the first set fixes the array size; a larger later set is
truncated with a warning). Version bumps only when bytes change.

### 6.2 Shader layout and packing

- `struct GlobalsBlockLayout { int slot; unsigned size; struct Member { int globalId; unsigned offset;
  ShaderUniformType type; unsigned arrayCount; }; std::vector<Member> members; std::vector<uint8_t> bytes;
  uint64_t packedVersion = ~0; uint64_t namesVersion; }` resolved from `ShaderData` reflection
  (`getUniformBlockMembers("u_fs_globals", size)`) **once per shader program**: cached by `ShaderRender*` in
  `RenderSystem::globalsLayouts` (an `unordered_map`, entries removed in `ShaderPool::remove` / shader destroy).
  Every member name is turned into an id with `propertyToID` (so a member whose global does not exist yet
  resolves as soon as a script sets it).
- `ShaderLibraryBindings` (`engine/core/shader/ShaderUniforms.h:77`) gains `GlobalsBlockLayout* vsGlobals`,
  `fsGlobals` (non-owning) and `std::vector<GlobalTextureSlot> globalTextures`.
- `RenderSystem::applyGlobals(ObjectRender&, const ShaderLibraryBindings&)` is called from `applyFrameBlock`
  (`RenderSystem.cpp:4759`), so every draw path that applies the frame block (meshes, UI, points, lines, sky,
  trails, terrain, instanced meshes, extra passes, depth programs, post-process passes) also applies globals:
  if `layout->packedVersion != ShaderGlobals::getVersion()` repack (each member's slots copied at its std140
  offset; vec3 members take xyz; float arrays one float per 16-byte element; mat4 four columns) then
  `render.applyUniformBlock(slot, size, bytes)`. Packing happens at most once per program per frame-version.
- Built-in Lit (`mesh.vert/frag`, `uber.*`) does not declare the block (no change, no cost); a fork of it may.
- Global textures: in `loadShaderLibraryBindings` (`RenderSystem.cpp:3941`), a texture uniform not claimed by a
  manifest `@texture` or `shaderTextures` entry whose name is a global name (any existing id, declared or not)
  gets a `GlobalTextureSlot{slot, id, cube, boundVersion}`, bound to the global's texture or the white/black-cube
  fallback; `applyGlobals` rebinds when the global's texture changed (`render.addTexture`). Textures loaded by
  `setTexture(path)` go through `TexturePool` like component textures; a framebuffer texture (a camera's render
  texture) is allowed.
- Material/component bindings: `Material::shaderGlobalBindings` (`std::vector<std::pair<std::string,std::string>>`,
  member -> global name) and the same field on `UIComponent`, `PointsComponent`, `LineRendererComponent`,
  `SkyboxComponent`, `TrailRendererComponent` (if landed) and post-process passes. `CustomUniformBlock::writeValues`
  (`ShaderUniforms.h`) takes an optional bindings list: a bound member's value comes from `ShaderGlobals`
  (colour/vector/float conversions as in §6.1; a texture global cannot be bound to a value member: refused by the
  setter and the Inspector). Components with bindings remember `globalsVersionSeen`; `applyComponentUniforms`
  (and the mesh material upload) rewrite their custom blocks when `ShaderGlobals::getVersion()` moved, otherwise
  nothing changes in their per-draw cost.
- Shader cache / variants: unaffected (globals are data, not defines); no cache version bump.

### 6.3 GLSL usage (documented in `docs/mcp/guide/shaders.md`, new section "Global values")

```glsl
// vertex stage of a fork: the game's four vertex lights
uniform u_vs_globals {
    vec4 _LightPositionX;   // x,y,z,w = light 0..3
    vec4 _LightPositionY;
    vec4 _LightPositionZ;
    vec4 _LightRadius;
    vec4 _VertexLightColor0;
    vec4 _VertexLightColor1;
    vec4 _VertexLightColor2;
    vec4 _VertexLightColor3;
} globals;
uniform texture2D _NoiseTex;    // a texture2d global
uniform sampler _NoiseTex_smp;
```

There is no engine include for the block, because each project has its own list and a shader should declare only
what it reads; `get_project_settings` and the Project Settings page give the exact block for the current list ("Copy GLSL block" button: both stages, all globals or the selected ones). Members of the same
name and type in both stages are filled identically.

### 6.4 Lua API (`engine/core/script/binding/CoreClassesLua.cpp`, new static class `Shader`)

| Function | Semantics |
|---|---|
| `Shader.propertyToID(name)` -> integer | stable id for the process (creates the name) |
| `Shader.setGlobalFloat(nameOrId, number)` / `setGlobalInt(nameOrId, integer)` | int stored as float |
| `Shader.setGlobalVector(nameOrId, Vector4 \| Vector3 \| Vector2)` | missing components 0 |
| `Shader.setGlobalColor(nameOrId, Vector4 \| Vector3)` | sRGB in, stored linear (Unity in Linear space) |
| `Shader.setGlobalMatrix(nameOrId, Matrix4)` | |
| `Shader.setGlobalFloatArray(nameOrId, {numbers})`, `setGlobalVectorArray(nameOrId, {Vector4})` | 1..64 |
| `Shader.setGlobalTexture(nameOrId, Texture \| "path" \| Framebuffer)` | `nil` restores the default |
| `Shader.getGlobalFloat/Vector/Color/Matrix/FloatArray/VectorArray/Texture(nameOrId)` | Color returns sRGB; unknown name -> nil |
| `Shader.hasGlobal(name)` -> bool; `Shader.getGlobalNames()` -> table | |
| `Shader.resetGlobals()` | back to Project Settings defaults; undeclared names dropped |

Errors: wrong argument types raise a Lua error naming the function; a matrix/array mismatch logs once and returns
false. Material bindings from Lua: `material:setShaderGlobalBinding(member, globalName)` (`nil` unbinds) and
`getShaderGlobalBinding(member)`, and the same on UI/Points/LineRenderer/Skybox/TrailRenderer objects
(`setShaderGlobalBinding`). C++ scripts use `ShaderGlobals` directly (`docs/mcp/guide/cpp-scripting.md`).

### 6.5 Serialization, project settings and exports

- Project file (`project.yaml`, written by `Project`): `shaderGlobals:` list of
  `{name, type, arraySize?, default}` (colour defaults written as `"#RRGGBBAA"` sRGB, vectors as arrays,
  matrices as 16 numbers column-major, textures as asset paths). `editor/Project.h` gains
  `std::vector<ShaderGlobalDecl> shaderGlobals` in the graphics settings with get/set + dirty flag.
- Editor runtime: `Project` calls `ShaderGlobals::declare(...)` on project open and whenever the list changes
  (edit mode and Play), and `resetToDefaults()` at Play start/stop (`editor/Project.cpp` play session start/stop).
- Exported builds: `editor/Generator.cpp` (main content, next to the tags and layers block ~2323) emits
  `ShaderGlobals::declare({...});` with literal decls before any scene loads (`Factory::formatShaderGlobalDecl`).
  Texture defaults are added to the export's resource list (the asset collector already follows scene texture
  paths; add the project list to it).
- Materials and components: `shaderGlobalBindings: {member: global}` key in `Stream` material/component
  encoders (`editor/Stream.cpp` `encodeComponentShaderLibrary` helpers, written only when non-empty) and
  `Factory` (`formatComponentShaderLibrary`), `.material` files (`editor/material`), prefab overrides via the
  catalog (`PropertyType::Custom` with an `UpdateFlags_Shader_Uniforms` flag: no reload).

### 6.6 Editor UX

- **Project Settings > Graphics > Shader Globals** (`editor/window/dialog/ProjectSettingsWindow.cpp`
  `drawGraphicsSettings` ~1293, keywords added to `projectCategories` for search): a table Name | Type | Default
  | (Play: Live) | Used by. "+ Add" (name field validated live with the §5.8 rule and duplicates), type combo
  (changing type resets the default), default editor per type (float drag, vector drags, colour picker (sRGB),
  4x4 matrix grid, array size + element list, texture slot with asset picker), remove (asks when used by
  bindings: lists them; removal leaves bindings dangling and reported), rename (updates material/component
  bindings in all open scenes and project materials through one undo step; GLSL is not rewritten: a notice says
  so). "Copy GLSL block" (vertex/fragment, all or selected). **Live column in Play**: shows current values each
  frame (scripts' writes) and edits them (not saved; a "Copy live values to defaults" button saves them). Undo
  through the project settings command path.
- **Used by**: the shaders (forks, library, loaded) whose reflection declares the member, and the bindings; from
  `ShaderGlobalsUsage` scanning loaded shader programs and project materials on demand (not every frame).
- **Inspector**: every shader parameter row of a material / UI / Points / Lines / Sky / Trail shader block gains a
  small link button: "Bind to global..." popup listing compatible globals (same component count; colour <->
  vector4 allowed); a bound row shows `@FogTint` and the live value read-only, with "Unbind". Undoable.
- **Shader errors**: `check_shader` and the Console show `global-undeclared` (warning), `global-type-mismatch`
  (warning: GLSL vec3 vs a declared vector4 reads xyz; float vs vector reads x), `globals-block-too-large`
  (error), `uniform-block-limit` (error, any block).

### 6.7 MCP / agent surfaces

- `get_project_settings` / `set_project_settings` (`editor/mcp/tools/ProjectTools.cpp`): key
  `graphics.shader_globals` (the whole list, replace) and `graphics.shader_globals.<name>` (`{type, default,
  array_size}` upsert, `null` removes). `describe: true` documents them. The read result adds `glsl_vs` and
  `glsl_fs` (the declaration blocks for the list).
- `get_shader_uniforms` (AssetTools) adds `globals: [{member, stage, global, type, declared, value}]` for the
  entity's shader and `global_bindings` of the material/component; `set_properties` paths
  `submeshes[N].material.shaderGlobalBindings` / `shaderGlobalBindings` (`{member: global | null}`).
- Runtime values in Play: `eval_lua` (no new tool). `get_play_state` is unchanged.
- No new tool; `docs/mcp/TOOLS.md` regenerated, budget unchanged except descriptions (+~200 chars).
- Agent guide (`editor/mcp/McpInstructions.cpp`): one line "global shader values: Project Settings
  `graphics.shader_globals`, GLSL block `u_vs_globals`/`u_fs_globals`, Lua `Shader.setGlobal*`; details in
  jasper://docs/shaders#global-values".

### 6.8 Per platform

All in engine C++ and standard GLSL: identical on Linux, Windows, macOS, iOS, Android, Web. Cross-compilation:
the block is std140 in GLSL 450 source; spirv-cross emits a UBO (GL 4.1, GLES3/WebGL2 as uniform blocks or
flattened arrays as sokol configures today), a cbuffer (HLSL5), a buffer argument (MSL). Only float members
(no int mixing: the GL flattening rule in `shaders.md`). The packer always uses reflection offsets, never its own
layout assumptions.

### 6.9 Docs to update

`docs/mcp/guide/shaders.md` (new "Global values" section: declaration, types, bindings, caps, cost, the vertex
lights example), `docs/mcp/guide/lua-api.md` (§2 new `Shader` static class; material/component
`setShaderGlobalBinding`), `docs/unity-glossary.md` (`Shader.SetGlobal*` -> `Shader.setGlobal*`, `Shader.PropertyToID`
-> `Shader.propertyToID`; Godot-style Project Settings list), `docs/shaders/README.md` (row), `docs/mcp/guide/cpp-scripting.md`,
README feature list.

## 7 Behaviour rules and edge cases

- A shader member with no global of that name (never declared, never set) reads 0 (zero bytes), like Unity.
- A member's GLSL type differs from the global's: the slot's leading components are used (vec3 of a vector4:
  xyz; float of a vector: x; vec4 of a float: (x,0,0,0)); a mat4 member of a non-matrix global reads 0;
  warnings as above.
- An array member longer than the global's array: extra elements 0; shorter: truncated.
- Same global read in both stages: same values. Stage-only blocks are fine.
- Setting a global in `onUpdate` of frame N affects frame N's draws; setting from an HTTP callback (main thread,
  shared convention) affects the next draw.
- Scene load: values persist; Play stop: reset. Hot reload of a fork: layouts re-resolved with the new program.
- Removing a declared global that scripts still set: it becomes undeclared (runtime-only) on the next set.
- Depth/shadow programs of a fork that reads globals in the vertex stage (e.g. wind displacement): their globals
  block is applied too (applyFrameBlock path), so shadows match.
- Edit mode: defaults apply in Scene and Game views (Lua does not run in edit mode); Unity's `[ExecuteAlways]`
  scripts' edit-time previews are replaced by defaults or bindings.

## 8 Performance

- Packing: O(members) per program when the version changed (once per frame for per-frame updates): 8 vec4 =
  128 bytes, negligible. Per draw: one `applyUniformBlock` of the block size for programs that declare it
  (same as the frame block). A material with bindings: one custom-block rewrite per frame it is drawn while the
  version moves.
- Budget on a mid-range Android phone: 500 draws reading a 16-vec4 globals block cost < 0.2 ms CPU per frame
  extra (measured with the `tools/fixtures/shader-globals` bench scene and `JASPERENGINE_FRAME_STATS=1`), and
  zero for draws not declaring it.
- sokol's per-frame uniform ring (default size) is ample: 1024 bytes x 1000 draws = 1 MB < 4 MB.

## 9 Tests and acceptance

Unit (`editor/tests`):
- `ShaderGlobalsTest`: ids stable; declare/reset; setters and conversions (float->vector, colour sRGB->linear
  exactness for 0.5 -> 0.21404114); matrix mismatch refused; arrays truncation; version bumps only on change;
  undeclared names typed by first set; reset drops them.
- `GlobalsBlockPackTest`: pack into reflection offsets for a block with float, vec2, vec3, vec4, mat4,
  `float[4]`, `vec4[4]` (a synthetic `ShaderData` and one compiled with the real pipeline for glsl410, glsl300es,
  spirv, hlsl5, msl: offsets from reflection equal the packer's writes).
- `ShaderGlobalBindingTest`: a library shader's `tint` bound to a colour global takes it, over the stored value
  and the manifest default; unbinding restores; version-driven rewrite happens once per change.
- `ProjectShaderGlobalsStreamTest`: project.yaml round trip; Generator output contains the declare call and
  compiles (existing generator compile test pattern); name validation rules.

Pixel / end-to-end (MCP, `tools/mcp/tests/test_shader_globals.py`):
- Fixture `tools/fixtures/shader-globals`: a quad with a fork of `library:unlit` whose fragment stage declares
  `u_fs_globals { vec4 TestColor; }` and outputs it. Edit mode: default red -> screenshot centre red; change the
  default via `set_project_settings` -> green within one frame. Play: `eval_lua Shader.setGlobalColor('TestColor',
  Vector4(0,0,1,1))` -> blue; stop -> back to the default.
- Vertex-stage global (the vertex lights example): a sphere lit by 4 lights from globals; moving a light via Lua
  changes the brightness on the expected side (pixel comparison left/right).
- A `library:toon` material with `tint` bound to `FogTint`: changing the global recolours it.
- Texture global: a 2x2 checker texture global sampled by a fork.
- Backends: the fixture run with `SMOKE_BACKEND=gles3` and `vulkan` (`tools/smoke-test.sh`) shows the same
  colours; the fork compiles on all six backends (`jasperengine-editor shaders` / `check_shader`).
- `check_shader` diagnostics for undeclared, mismatched and too-large blocks.
- Stubs: `test_stubs.py` passes with the `Shader` class.

Acceptance:
1. The game's `VertexLightsUpdater` logic ports to Lua with the same calls and a fork reading `u_vs_globals`
   renders the four lights (fixture screenshot).
2. Globals declared in Project Settings show their defaults in edit mode, are live-editable in Play, reset on
   stop, and are present in exported builds (smoke test of the fixture export shows the default colour).
3. A library shader property bound to a global follows it without forking.
4. Shaders without the block have unchanged cost and output (frame-stats diff on the shader-library fixture).
5. Lua, Inspector, Project Settings, MCP keys, docs and stubs cover every type.

## 10 Integration points

- `engine/core/shader/ShaderUniforms.{h,cpp}` (`ShaderLibraryBindings` :77, `CustomUniformBlock::writeValues`).
- `engine/core/subsystem/RenderSystem.cpp`: `loadShaderLibraryBindings` 3886 (block + textures), `applyFrameBlock`
  4759 (call `applyGlobals`), `applyComponentUniforms` (bindings), the mesh material uniform upload, post-process
  pass upload; `RenderSystem.h:1595` declarations.
- `engine/core/pool/ShaderPool.cpp` (layout cache removal on shader destroy).
- `engine/core/texture/Material.h` (`shaderGlobalBindings`), components listed in §6.2.
- `engine/core/script/binding/CoreClassesLua.cpp` (`Shader`), `GameObjectClassesLua.cpp` (bindings on objects).
- `editor/Project.{h,cpp}` (settings, Play start/stop), `editor/window/dialog/ProjectSettingsWindow.cpp:1293`,
  `editor/window/InspectorShaderLibrary.cpp` (link button), `editor/Generator.cpp:2323`, `editor/Factory.cpp`,
  `editor/Stream.cpp`, `editor/mcp/tools/ProjectTools.cpp`, `editor/mcp/tools/AssetTools.cpp` (`get_shader_uniforms`,
  `check_shader`), `editor/shader/*` (diagnostics).

## 11 Cross-feature dependencies

- `lines-trails` (#13): TrailRendererComponent gets `shaderGlobalBindings` if it lands first; otherwise lines-trails
  adds the field when rebasing.
- `anim-properties` (#12): animating a global from a clip is not in scope (scripts set globals).
- `script-data` / `coroutines`: none. `persistence` (#20): globals already persist across loads.

## Implementation notes

Decisions taken while implementing (branch `gaps/shader-globals`, on `gaps/lines-trails` with
`main` merged in).

1. **Colour conversion**: `setGlobalColor`, colour defaults and the Project Settings picker use
   the engine's `Color::sRGBToLinear` (gamma 2.2), the conversion every material colour, the
   Inspector's pickers and Gradient keys use, so a global bound to a material colour renders
   exactly as the same colour typed into the material. 0.5 therefore becomes 0.2176376, not the
   piecewise sRGB curve's 0.21404114 named in section 9 (the unit test checks 0.2176376).
2. **Layout cache**: the packed globals blocks are cached by their reflected layout (block name,
   size, members' names, types, offsets, counts), not by `ShaderRender*`: programs with the same
   block share one packing, a layout lives while a loaded object holds it (`shared_ptr` in
   `ShaderLibraryBindings`, `weak_ptr` in the cache), and a hot-reloaded or destroyed program
   cannot leave a stale entry, so `ShaderPool::remove` needs no hook. The slot stays per
   program (`slotVSGlobals` / `slotFSGlobals`).
3. **declare() keeps unchanged values**: a global whose declaration (type, size, default) is
   unchanged keeps its current value, a new or changed one takes its default, a removed one is
   dropped. Editing Project Settings during Play therefore does not wipe what scripts set into
   the other globals (section 6.1 said "resets values").
4. **Texture globals**: any texture uniform the manifest and `shaderTextures` do not claim and
   whose name is a valid global name (engine textures are all `u_*`) binds to the global of its
   name, in either stage, every draw (fallback white, black for a cube, also while loading or
   when the kind differs). This also covers globals made after the object loaded. The sampler is
   whatever the shader pairs with the texture (`<name>_smp` is the documented convention).
5. **Vertex hook**: to write a vertex-lighting fork without copying the library's vertex code,
   `includes/surface_vs.glsl` gained `SURFACE_VERTEX` / `surfaceVertex(worldPosition,
   worldNormal, color, uv)`, called after the varyings are written. The fixture's
   `vertex_lights` fork and the docs use it; no library shader defines it.
6. **Bindings scope**: materials (submeshes), UI (Image, Text, Polygon), Points, Line Renderer,
   Trail Renderer (the field lines-trails added, now editable everywhere), Skybox and
   post-process passes. A material's extra pass and a mesh component's own `shaderUniforms`
   (a fork set on the mesh) have none; Unity's implicit "material wins unless absent" rule is not
   reproduced (explicit bindings only). Bindings refuse texture and matrix globals; a binding to
   a global that does not exist reads 0 until a script sets it.
7. **Rename** in Project Settings (OK) is one project-history step that renames the bindings of
   the open scenes (`ShaderGlobalsCmd`); scenes that are not open keep the old name (they read 0,
   `check_shader` / "Used by" show it). MCP renames with `graphics.shader_globals.<old>:
   {name: <new>}`. There are no `.material` asset files in this engine, so "project materials"
   are the scenes' materials.
8. **Project Settings page**: the edited list is declared to the engine on every valid edit, so
   the Scene and Game views follow while the dialog is open; Cancel (or closing it) declares the
   project's list again. The Live column and "Copy Live Values to Defaults" appear while a scene
   plays. "Used by" is scanned when the page opens and on its refresh button (bindings and the
   loaded programs of the open scenes' objects); post-process programs are not scanned.
9. **Export**: `Generator` emits `ShaderGlobals::declare({ShaderGlobalDecl{...}, ...})` into
   main.cpp (`Generator::shaderGlobalsInitSource`, `Factory::formatShaderGlobalDecl`). Exports
   already copy the whole assets directory, so texture defaults need no resource-list entry.
10. **Diagnostics**: `ShaderGlobals::checkShader` (engine) feeds `check_shader` (findings with
    `code` global-undeclared / global-type-mismatch as warnings) and the Console (a warning when
    a fork is built for the editor's backend). `globals-block-too-large` fails the compile in
    `ShaderVariantCompiler::compile` (editor and runtime alike), so the object falls back like
    any failed fork; `uniform-block-limit` is a `check_shader` error.
11. **project.yaml**: `shaderGlobals: [{name, type, arraySize (arrays), default}]`; a colour
    default is `"#RRGGBBAA"` when it is exactly an 8-bit sRGB colour, else `[r, g, b, a]` sRGB
    numbers (lossless); an entry that cannot be read is left out with an error, the rest kept.
12. **Agent guide**: the merged tree's guide was over its 16 KB budget (lines-trails' line); the
    shader-globals line was added and a few sentences elsewhere were shortened to fit (16333
    bytes).
13. **Not done**: the frame-stats bench of section 8 (500 draws on a mid-range Android phone) was
    not measured: no device here. The `SMOKE_BACKEND=gles3/vulkan` smoke runs were not run;
    every backend's compile and reflection offsets are checked by the unit test
    `GlobalsBlockPackTest.CompiledBlocksMatchThePackerOnEveryBackend` (and the four vertex lights
    test) instead.
14. **GL targets**: the OpenGL / GLES3 / WebGL2 targets flatten uniform blocks (SPIRV-Cross),
    which takes only `vec4` and `mat4` arrays and no indexing of a block's vector by a variable.
    So a `float_array` global is declared (and generated: Copy GLSL, `glsl_vs` / `glsl_fs`) as
    `vec4 x[N]` read in `.x` (std140 gives `float x[N]` the same 16-byte stride, so the packer
    fills either), and `check_shader` warns about float/vec2/vec3 arrays in a globals block. The
    vertex-lights shaders copy the block's vectors to locals before indexing them per light.
