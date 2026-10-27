# Sprite Normals, Phase 1: 2D normal maps correct in the renderer

Status: binding for the Phase 1 implementer (wave 1, its own worktree). Conforms to
`2026-10-27-sprite-normals-contract.md` (section 7 is this phase's contract). Owner direction: "fix" the 2D
convention (OpenGL green), ship with every other phase.

Every file:line below was read on `main` at 9140e7ce0. Line numbers drift; the quoted code is the anchor.

## 1. Goals and non-goals

After this phase a tangent-space normal map in the OpenGL convention (red = right, green = up the image, blue = out),
assigned at `submeshes[n].material.normalTexture`, lights a 2D object correctly under Light 2Ds:

- on every 2D mesh type: Sprite (quad and subdivided grid), MeshPolygon, Tilemap, on world (y-up) and UI (y-down)
  cameras, with the built-in mesh shader and with `library:sprite`;
- under every mirroring: negative Transform scale on either axis, mirrored frame rects (negative texture-rect width or
  height, per object or per instance), manual `flipY`, instanced copies with negative scale;
- with a Light 2D whose `height > 0`; and objects without a normal map look the same whatever the height;
- scripts (Lua and C++) can assign it; a missing normal-map file falls back to a flat normal, not a tilted one.

Non-goals: a per-material normal strength on the 2D path (strength is baked by the generator); normal maps on UI
`Image` components (they draw through `ui.frag` with no 2D lights); frame-edge clamping of the normal sample
(the tools copy the colour texture's Nearest filter onto the normal map, contract section 7); changing how the
struct default of `Light2DComponent::height` (0) loads, serializes or exports; runtime negative-scale winding/culling
of 3D meshes (observed, section 3.8, left as is).

## 2. How 2D normal mapping works today (the facts the fixes rest on)

- Every Sprite, sprite grid, MeshPolygon and Tilemap is a `MeshComponent` whose vertices carry `NORMAL` (always +Z)
  and `TANGENT` `(1, 0, 0, w)` (`MeshSystem.cpp:409-450` prepare*, sprite quad `:592-600`, sprite grid `:673`,
  polygon `:716`, tilemap `:915-917`), with `w = flipY ? -1 : +1` and the comment "handedness (w) follows the V
  direction, which flipY inverts".
- `flipY` is set automatically per camera when `automaticFlipY` (default true; `Sprite::setFlipY` turns automatic
  off, `Sprite.cpp:107`): `MeshSystem::changeFlipY` (`:987-993`) makes it true for every non-UI camera. With flipY
  true the bottom vertices get v near 1 (image bottom) and the top ones v near 0 (images load top row first), so the
  sprite is upright in a y-up world; on a UI camera (y down) flipY is false and v grows with +y (screen down).
- The vertex shader builds the frame `v_tbn = mat3(T, B, N)` with `B = cross(N, T) * a_tangent.w`
  (`mesh.vert:268-270`); the library path writes `v_tangent`, `v_bitangent = cross(N, T) * a_tangent.w`, `v_normal`
  (`includes/surface_vs.glsl:441-458`).
- World camera: `w = -1`, `T = +X`, `N = +Z`, so `B = -Y` = the direction v grows = image-down. UI camera: `w = +1`,
  `B = +Y` = screen-down = again the direction v grows. **2D reads green along +v: the DirectX convention**, the
  opposite of 3D Lit (`includes/stochastic.glsl:60-69`: Lit's bitangent "points up the image",
  `STOCHASTIC_NORMAL_GL`), Unity and Godot.
- Texture rects (atlas frames) scale UVs in the vertex shader: `v_uv1 = a_texcoord1 * rect.zw + rect.xy`, uniform
  `spriteParams.textureRect` then per instance `i_textureRect` (`mesh.vert:286-296`, `surface_vs.glsl:271-285`).
  A mirrored frame has a negative width (`MeshSystem.cpp:563-566`, `changesSpriteInset` `:1039-1046`). Nothing turns
  the tangent frame with it.
- Transform mirroring: `tangentW = model * T` flips with a negative x scale and `B = cross(N, tangentW) * w` flips
  with it, although v did not turn: green inverts. A negative y scale leaves `T` and `B` alone although v turned:
  green inverts. Nothing multiplies `w` by the sign of the transform's determinant (`mesh.vert:268`,
  `surface_vs.glsl:446`).
- The 2D light term (`includes/lighting2d.glsl:102-130`, `getLight2DIntensity`): per light, radial attenuation, and
  `if (pr.z > 0.0) ndotl = clamp(dot(n, normalize(vec3(toLight, height))), 0, 1)`. So a height above 0 dims every
  flat sprite by `height / |L|`, normal map or not, and height 0 ignores normals. The normal comes from
  `getNormal2D()` (`:32-51`): the normal map through `v_tbn` when `HAS_NORMALS && HAS_TANGENTS && HAS_NORMAL_MAP`
  and a UV set, else `v_tbn[2]`/`v_normal`, else +Z. Callers: `mesh.frag:241-243` (unlit path, the sprite default),
  `mesh.frag:423-426` (lit path in mixed scenes, `n` from `getNormalInfo`), `library/sprite.frag:233-235`.
- `Light2DComponent::height` defaults to 0 (`Light2DComponent.h:21`), is always serialized (`Stream.cpp:6899`,
  read back only when present `:6920`). New lights come from `CreateEntityCmd` `POINT_LIGHT_2D`
  (`CreateEntityCmd.cpp:382-390`, sets range 200 and intensity 1 only), used by the Hierarchy menu
  (`HierarchyWindow.cpp:501`) and MCP `create_entity` `light_2d` (`EntityTools.cpp:208`); Add Component goes through
  `AddComponentCmd` (`EntityTools.cpp:685` for MCP `add_component`) and `ProjectUtils::addEntityComponent`
  (`ProjectUtils.cpp:913`, also the scene loader, so defaults must not change there).
- Variant selection (`RenderSystem.cpp:3720-3765`): with 2D lights, `HAS_NORMALS`, `HAS_TANGENTS` and
  `HAS_NORMAL_MAP` follow the mesh's inputs; **for any shader with a manifest `p_hasNormalMap = false`** (`:3764`) and
  normals/tangents only follow `@require`. `library/sprite.frag` requires `uv1 color` only, so it always lights with
  +Z. The normal texture is bound by `loadPBRTextures` for every shader, manifest or not
  (`RenderSystem.cpp:4860-4876`; `addTexture` no-ops without the slot), with fallback `emptyNormal`.
- `emptyNormal` is filled with `0xFF808080` (`RenderSystem.cpp:900-912`): little-endian bytes `80 80 80 FF` = RGB
  (128, 128, 128), which decodes to `(0.004, 0.004, 0.004)` and normalizes to `(0.577, 0.577, 0.577)`: a 55-degree
  tilt. `emptyFlatNormal` (`:964-985`) uses `0xFFFF8080` = (128, 128, 255), correct. `getRender(&emptyNormal)`
  returns the fallback when a set normal texture fails to load (`Texture.cpp:477-506`), so a missing file tilts the
  lighting of Lit meshes and of 2D lit sprites today.
- Library manifests: `@require` inputs map to `ShaderRequire_*` bits (`ShaderManifest.h:65-88`; parse
  `ShaderManifest.cpp:400-417`; defines `getRequireDefines` `:1126-1140` -> `SURFACE_*`). Bits 0-10 are used.
  Shaders compile at runtime from the embedded `shaderlib/` (`engine/CMakeLists.txt:1206-1250`); `engine/shaders/*.h`
  are stand-ins, nothing to regenerate.
- Inspector: the Normal Texture row is drawn only when `drawMaterialShaderRows` returns true, i.e. the drawn shader is
  not a library shader (`InspectorWindow.cpp:7174-7191`, `InspectorShaderLibrary.cpp:688-690`). Light 2D Height help:
  `InspectorWindow.cpp:9365-9366`.
- Lua: `Material.normalTexture` is a read-only property (`CoreClassesLua.cpp:1268, 1288`); `Mesh` binds only
  `setTexture` (base colour, submesh 0; `Mesh.cpp:41-63`, `GameObjectClassesLua.cpp:814-818`). LuaLS stubs and API
  suggestions are generated from the bindings at build time (`generate_lua_stubs.py`, checked by
  `tools/mcp/tests/test_stubs.py`); the guide `docs/mcp/guide/lua-api.md` section 4.3 is hand-written.

## 3. The fixes

### 3.1 F1: OpenGL green on every 2D mesh type

Rule (camera-independent, stated in terms of the generated UVs): the tangent `T` points where u grows, the bitangent
`B = cross(N, T) * w` points where **v shrinks** (up the image). With `N = +Z`, `T = +X`, `cross(N, T) = +Y`, so
`w = +1` when v shrinks toward +y and `-1` when v grows toward +y.

For the four generators v shrinks toward +y exactly when `flipY` is true (world camera, or manual flip on a UI camera),
so the change is the sign:

| Site | Before | After |
|---|---|---|
| sprite quad `MeshSystem.cpp:595` | `sprite.flipY ? -1.0f : 1.0f` | `sprite.flipY ? 1.0f : -1.0f` |
| sprite grid `:673` | `sprite.flipY ? -1.0f : 1.0f` | `sprite.flipY ? 1.0f : -1.0f` |
| polygon `:716` | `polygon.flipY ? -1.0f : 1.0f` | `polygon.flipY ? 1.0f : -1.0f` |
| tilemap `:917` | `tilemap.flipY ? -1.0f : 1.0f` | `tilemap.flipY ? 1.0f : -1.0f` |

- Factor the sign into one helper, `static float MeshSystem::flatTangentHandedness(bool flipY)` (header comment: "the
  bitangent points up the image, OpenGL normal maps, as Lit"), used by all four sites; rewrite the four comments to
  state the rule above.
- Tilemap tiles whose rect has a negative width or height (mirrored tiles, if the tileset data allows them) bake the
  mirror into their UVs: for such a tile the tangent becomes `(sx, 0, 0, w')` with `sx = sign(rect width)` and
  `w' = flatTangentHandedness(flipY) * sx * sy` (`sy = sign(rect height)`), so `T` follows u and `B` still points up
  the image. Unit test U1 checks this property on generated vertex data for all four generators.
- `includes/stochastic.glsl` and `lighting2d.glsl:37` already pass `STOCHASTIC_NORMAL_GL` on the 2D path (a polygon
  with Stochastic Tiling): after F1 that is true; before it the stochastic taps rotated slopes the wrong way. No code
  change there; the comment at `stochastic.glsl:60-69` gains "and the 2D meshes (Sprite, Polygon, Tilemap)".

Effect on existing content: every 2D normal map authored for the old (DirectX) behaviour now lights with green
inverted. Migration note (README "Engine" bullet, `docs/mcp/guide/lua-api.md` Light2D paragraph, release notes in
the merge message): "2D normal maps now use the OpenGL convention (green up), like 3D Lit, Unity and Godot. A map made
for the old behaviour: flip its green channel (the Sprite Normal Map generator's DirectX toggle off/on, or any image
editor)." Phase 3's `check --normals` heuristic `normals-green-convention` flags maps whose slopes disagree with the
silhouette.

### 3.2 F2: mirroring keeps lighting correct (2D and 3D)

In `mesh.vert` (after `bitangentW` is computed, `:269`) and in `surface_vs.glsl` (the `SURFACE_TANGENTS` block,
`:441-458`), with the frame computed from the un-mirrored inputs as today:

```glsl
// mirroring: a transform with a negative determinant turns the bitangent (v did not turn
// with the tangent), a mirrored texture rect turns the axis it mirrors
float frameSign = sign(determinant(mat3(pbrParams.modelMatrix)));
#ifdef HAS_INSTANCING
    frameSign *= sign(determinant(mat3(instanceMatrix)));
#endif
bitangentW *= (frameSign == 0.0 ? 1.0 : frameSign);
#if defined(HAS_UV_SET1) && (defined(HAS_TEXTURERECT) || defined(HAS_INSTANCING))
    vec2 rectSign = vec2(1.0);
    #ifdef HAS_TEXTURERECT
        rectSign *= sign(spriteParams.textureRect.zw);
    #endif
    #ifdef HAS_INSTANCING
        rectSign *= sign(i_textureRect.zw);
    #endif
    rectSign = mix(vec2(1.0), rectSign, notEqual(rectSign, vec2(0.0)));  // a zero size keeps the frame
    tangentW *= rectSign.x;
    bitangentW *= rectSign.y;
#endif
```

Write it as a shared function in a new include `includes/tangent_frame.glsl`
(`void mirrorTangentFrame(inout vec3 tangent, inout vec3 bitangent)`, reading the same uniforms/attributes the two
vertex paths already declare), included by both, so the two paths cannot drift. The lattice path (`HAS_LATTICE`) and
the VAT path compute their tangent before this point and are covered by the same call. In `surface_vs.glsl`, the
fallback tangent (mesh without tangents, `:449-454`) is exempt from the rect part (it is not tied to u) but takes the
determinant part.

Why also for 3D: the defect is identical for any mesh with tangents (a mirrored instance, a negatively scaled glTF node
moved at runtime, a mirrored prefab); the fix is what Unity does (`unity_WorldTransformParams.w`); for every
non-mirrored transform `frameSign == 1` and the output is bit-identical, so existing non-mirrored content (all goldens)
is unchanged. glTF import already bakes a node's negative determinant into vertex data and winding
(`MeshSystem.cpp:4925`), so imported mirrored meshes have positive model determinants at rest and are unaffected.

Effect: only mirrored objects with normal maps change, from wrong to right. Migration: none needed.

### 3.3 F3: Light 2D normal response and default height

Shader (`includes/lighting2d.glsl`):

- Define `LIGHT2D_NORMAL_MAP` (inside the include, before `getNormal2D`) exactly when `getNormal2D` samples the normal
  map: `defined(HAS_NORMAL_MAP) && (defined(HAS_UV_SET1) || defined(HAS_UV_SET2)) && ((defined(HAS_NORMALS) &&
  defined(HAS_TANGENTS)) || defined(SURFACE_NORMAL_MAP_ACTIVE))` (the last one from F4).
- Define `LIGHT2D_NORMAL_RESPONSE` when `LIGHT2D_NORMAL_MAP` is defined or the including shader defined
  `LIGHT2D_SURFACE_NORMALS` before the include.
- In `getLight2DIntensity`, the `if (pr.z > 0.0)` block runs only `#ifdef LIGHT2D_NORMAL_RESPONSE`; otherwise
  `ndotl = 1.0`. Signature unchanged (forks and library shaders keep compiling).
- `mesh.frag`: `#ifndef MATERIAL_UNLIT` -> `#define LIGHT2D_SURFACE_NORMALS` before including `lighting2d.glsl`, so the
  lit path (real 3D normals, mixed scenes, `mesh.frag:423-426`) keeps its N.L exactly as today. The unlit path (every
  sprite by default) responds only when the map is sampled.
- Header comment of `lighting2d.glsl` documents both macros.

Editor:

- `CreateEntityCmd.cpp` `POINT_LIGHT_2D` block: also `light.height = 64.0f;` (comment: "the virtual height normal
  maps respond to; sprites without one are not affected"). This covers the Hierarchy menu, the App menus that create
  through it, and MCP `create_entity light_2d`.
- `AddComponentCmd`: when it adds a `Light2DComponent` to an existing GameObject interactively (Inspector Add
  Component, MCP `add_component`), set `height = 64` after `addEntityComponent` and record it so undo/redo restore
  the same value. Do **not** change `ProjectUtils::addEntityComponent` or the struct default (scene loading, prefab
  instantiation, Lua `Light2D(scene)` and C++ `Light2D` keep 0).
- Inspector Light 2D Height help (`InspectorWindow.cpp:9365`): "Virtual height of the light above the 2D plane, in
  world units: how steeply it shines on normal-mapped sprites (64 for new lights). 0 makes normal maps flat. Sprites
  without a normal map are lit the same at any height."
- Inspector Normal Texture row (built-in material, `InspectorWindow.cpp:7191`): `settingsMaterial` copy with help
  "Tangent-space normal map, OpenGL convention (green up). On sprites, tilemaps and polygons it answers Light 2Ds
  whose Height is above 0; same size and frame layout as the Base Texture."

Effect on existing content (deliberate): unlit 2D objects (and unlit 3D meshes) without a normal map under a Light 2D
with `height > 0` were dimmed by `height / |L|`; they now get the full radial light, as with height 0 (Unity's 2D
lights behave this way). Lit materials are unchanged. Migration note: "Light 2D Height now only shapes normal-mapped
objects; scenes that used Height to darken plain sprites: lower the light's Intensity." The contract's "lit exactly as
before regardless of height" is read as "as with height 0, at any height".

### 3.4 F4: library shaders sample the material's normal map (`@require normalmap`)

Manifest:

- New input `normalmap` -> `ShaderRequire_NormalMap = 1 << 11` (`ShaderManifest.h`), parsed next to `tangents`
  (`ShaderManifest.cpp:407`; the unknown-input error lists it), named in `getRequireNames`, and mapped by
  `getRequireDefines` to `SURFACE_NORMAL_MAP`. Meaning: "this shader lights with Light 2Ds through `getNormal2D`, and
  reads the material's own Normal Texture there". Gated by `feature=` like any input.

Variant (`RenderSystem.cpp`, the `if (manifest)` block at `:3745-3765`): after the existing lines,

```cpp
// @require normalmap: the material's normal map on the 2D light path (tangent frame from the mesh)
if ((require & ShaderRequire_NormalMap) && useLight2D && inputs.normalTexture && inputs.normal && inputs.tangent){
    p_hasNormal = true;
    p_hasTangent = true;
    p_hasNormalMap = true;
}
```

and the depth-variant code (`:3823`) is unaffected (no normals needed there). Without 2D lights, without a normal
texture or on a mesh without tangents, the variant is exactly today's.

Shaders:

- `surface_vs.glsl`: near the top, `#if defined(SURFACE_NORMAL_MAP) && defined(HAS_NORMAL_MAP) &&
  defined(HAS_TANGENTS)` -> `#define SURFACE_NORMAL_MAP_ACTIVE` and, if not already, `#define SURFACE_TANGENTS`
  (which implies `SURFACE_NORMALS`, `:96-98`), so `v_normal`, `v_tangent`, `v_bitangent` are written (with F2).
- `lighting2d.glsl`: the same three-define test yields `SURFACE_NORMAL_MAP_ACTIVE` on the fragment side (both stages
  see the same defines); under it the include declares `in vec3 v_tangent; in vec3 v_bitangent;`, `in vec3 v_normal;`
  unless the shader requires normals itself (`SURFACE_NORMALS` from its manifest, in which case the shader declares
  it), and `uniform texture2D u_normalTexture; uniform sampler u_normal_smp;` unless already declared (guard macro
  `LIGHT2D_DECLARES_NORMAL_TEXTURE`). `getNormal2D` gains a first branch:

```glsl
#if defined(SURFACE_NORMAL_MAP_ACTIVE)
    vec3 ntex = texture(sampler2D(u_normalTexture, u_normal_smp), v_uv1).rgb * 2.0 - vec3(1.0);
    return normalize(mat3(normalize(v_tangent), normalize(v_bitangent), normalize(v_normal)) * ntex);
#elif defined(SURFACE_TANGENTS) ... (library shader that requires tangents, no map: normalize(v_normal))
#elif ... existing mesh.frag branches unchanged
```

  This also fixes a latent compile error: a library shader requiring `tangents` and lit by 2D lights reached the
  `v_tbn` branch, which the library path never declares.
- `library/sprite.frag`: `// @require uv1 color normalmap`; `sprite.vert` unchanged (it includes `surface_vs.glsl`).
  With OUTLINE or OVERLAY the normal map is sampled at the same in-frame clamped `uv` the colour uses: pass the
  clamped UV by sampling inside `sprite.frag` through a small helper `getNormal2DAt(vec2 uv)` that `lighting2d.glsl`
  provides alongside `getNormal2D()` (same branches, UV given); `sprite.frag` calls `getNormal2DAt(uv)`. Description
  line gains "normal map (Normal Texture) for 2D lights".
- No other library shader includes `lighting2d.glsl` (checked: `sprite.frag`, `sprite.vert` only), so none other
  changes. `simple_lit`, `glass`, `water` keep their own `@texture u_normalTexture` (custom texture, 3D) untouched.

Inspector: in `InspectorWindow.cpp` after `drawMaterialShaderRows` returns false, if the drawn shader's manifest
resolves `ShaderRequire_NormalMap` for the current features, draw one table with the "Normal Texture" row (same
property path `submeshes[s].material.normalTexture`, same `settingsMaterial` and help as F3). Implement as
`bool Catalog::shaderReadsMaterialNormalMap(const ShaderManifest*, const std::vector<std::string>& features)` so
Phase 3/4 code (apply, checks) can ask the same question.

Docs: `docs/mcp/guide/shaders.md` `@require` list gains "`normalmap` (2D lights read the material's Normal Texture
through `getNormal2D()`; `library:sprite` uses it)"; `docs/shaders/library.md` Sprite entry gains the normal map;
`docs/shaders/README.md` untouched unless it lists inputs.

### 3.5 F5: scripts assign normal maps

C++ (`engine/core/object/Mesh.h/.cpp`), next to `setTexture`, same style:

```cpp
// the normal map (tangent-space, OpenGL green up) of submesh 0 / of a submesh; "" removes it
void setNormalTexture(const std::string& path);
void setNormalTexture(unsigned int submesh, const std::string& path);
void setNormalTexture(const std::string& id, TextureData data);
std::string getNormalTexture() const;                     // submesh 0's path, "" when none
std::string getNormalTexture(unsigned int submesh) const;
```

- Out-of-range submesh: `Log::error` naming the submesh count and return (as other per-submesh setters do).
- Setting a path on a submesh that had none, or clearing it, changes the shader variant (`HAS_NORMAL_MAP`): mark the
  mesh exactly as the editor's property change of `material.normalTexture` does (find the update flags
  `Catalog::getPropertyUpdateFlags`/the `PropertyCmd` path for that property and apply the same `needReload` /
  `needUpdateTexture`). Changing one path to another only needs `needUpdateTexture`.
- `Sprite`, `Tilemap`, `MeshPolygon`, `Model` inherit them. For a Tilemap, submesh `i` is the tileset whose
  `baseColorTexture` the submesh holds; document "one normal map per tileset texture, same size".

Lua (`GameObjectClassesLua.cpp`, the `Mesh` class): `setNormalTexture` with the three overloads
(`luabridge::overload<const std::string&>`, `<unsigned int, const std::string&>`, `<const std::string&, TextureData>`),
`getNormalTexture` with two overloads, and a rw property `normalTexture` (submesh 0 path, getter/setter above).
`Material.normalTexture` stays read-only (the comment "Textures stay read-only" keeps applying to Material copies).

Docs: `lua-api.md` 4.3 Mesh table gains `normalTexture` (rw path, submesh 0) and
`setNormalTexture([submesh,] path)` / `getNormalTexture([submesh])` with "OpenGL convention; Light 2D Height > 0";
the Light2D paragraph (`:1021`) gains "height: virtual height for normal-mapped objects (editor default 64)".
`docs/mcp/guide/cpp-scripting.md` lists the C++ methods if it lists Mesh methods. Stubs/suggestions regenerate at
build; `test_stubs.py` must report no unresolved binding.

### 3.6 F6: flat fallback normal

`RenderSystem.cpp:900-901`: `pixelsNormal[i] = 0xFFFF8080;` with the comment "(128, 128, 255): the flat normal,
bytes in memory order R, G, B, A on little-endian, as pixelsFlatNormal". Better still, have `emptyNormal` reuse
`pixelsFlatNormal`'s data and delete `pixelsNormal` if nothing else reads it (grep first; keep the `emptyNormal` name,
`RenderSystem.h:573`). Effect: Lit meshes and 2D-lit objects whose normal map fails to load (missing file, bad
image, a framebuffer that is gone) now shade as flat instead of tilted 55 degrees.

### 3.7 F7: other defects found on this path, fixed here

1. The latent `v_tbn` compile error in library shaders requiring `tangents` under 2D lights (F4 branch).
2. `stochastic.glsl` comment corrected (F1).
3. Inspector help texts (F3).
4. The four "handedness follows the V direction" comments in `MeshSystem.cpp` described the old (wrong) frame;
   rewritten (F1).

### 3.8 Observed, out of scope

- A runtime negative Transform scale on a 3D mesh with face culling on does not swap the winding (only glTF import
  bakes it), so such meshes cull their front faces. Pre-existing, unrelated to normal maps, not changed here; noted in
  the merge message as a known issue.
- 2D lights do not reach UI `Image`/`Button` (by design of `ui.frag`).

## 4. Behaviour changes and migration notes (all in the merge message and README)

| Change | Who sees it | Note |
|---|---|---|
| 2D reads OpenGL green | 2D normal maps authored for the old behaviour light upside-down | Flip green (generator DirectX toggle, or an image editor) |
| Mirroring correct | Mirrored normal-mapped objects (2D and 3D) | None: they were wrong |
| Height shapes only normal-mapped unlit objects | Plain sprites under a Light 2D with height > 0 get brighter | Lower Intensity if the dimming was wanted |
| New editor Light 2Ds: height 64 | New lights only | Lua/C++/loaded lights keep their value |
| `library:sprite` reads Normal Texture | Sprites on `library:sprite` with a normal texture set under 2D lights | None: it was ignored |
| Flat fallback normal | Objects whose normal map fails to load | None: they were tilted |

## 5. Tests

All new tests must pass on OpenGL (llvmpipe under Xvfb) and Vulkan (lavapipe) where the harness supports the backend.

### 5.1 Unit (editor tests, CPU, `editor/tests/Sprite2DTangentFrameTest.cpp`, picked up by the GLOB)

- U1 tangent frame property: for each generator (sprite quad, sprite grid 4x4, polygon square, tilemap 2x1 with one
  normal and one negative-width tile rect), with `flipY` true and false: read the vertex buffer, estimate
  `du/dx`, `dv/dy` from two vertices, and assert `T.x == sign(du/dx)` and
  `(cross(N, T) * w).y == -sign(dv/dy)` (B points where v shrinks). Fails on `main`, passes after F1.
- U2 `flatTangentHandedness(true) == 1`, `(false) == -1`.
- U3 manifest: `// @require uv1 color normalmap` parses to `ShaderRequire_UV1|Color|NormalMap`, `getRequireDefines`
  contains `SURFACE_NORMAL_MAP`, `getRequireNames` round-trips `normalmap`; `@require normalmap feature=X` gates; the
  unknown-input error text lists `normalmap`. `library:sprite`'s embedded manifest resolves `ShaderRequire_NormalMap`.
- U4 `Catalog::shaderReadsMaterialNormalMap` true for `library:sprite`, false for `library:simple_lit`, false for the
  built-in (null manifest).
- U5 `CreateEntityCmd(POINT_LIGHT_2D)` gives `height == 64`, undo removes, redo restores 64; `AddComponentCmd` of
  `Light2DComponent` gives 64, undo/redo; loading a scene YAML with a Light2D lacking `height` gives 0; a Light2D made
  with `ProjectUtils::addEntityComponent` (loader path) gives 0.
- U6 `Mesh::setNormalTexture`: path set/get on submesh 0 and 1, `""` clears, out-of-range submesh logs an error and
  changes nothing, the reload flag is set when presence changes and not when only the path changes.

### 5.2 GPU render tests (`engine/tests/htmlui/TestLight2DNormalMaps.cpp`, the engine's headless GPU harness)

Fixture (`Light2DNormalMapTest : FullPathGoldenTest`-style, a scene from `harness/SceneFixture.h`): 640x360 target,
orthographic world camera at 1 world unit per pixel looking down -z (y up), 2D ambient 0, one Light 2D with
intensity 1, range 2000, falloff 0 (constant attenuation), shadows off. Textures made in code with `TextureData`:

- `white`: 64x64 RGBA (255, 255, 255, 255).
- `dome`: 64x64 OpenGL hemisphere: for pixel (x, y) (y down), `nx = (x + 0.5 - 32) / 32`,
  `ny = (32 - (y + 0.5)) / 32` (up), `r2 = nx*nx + ny*ny`; inside (`r2 < 1`) `n = (nx, ny, sqrt(1 - r2))`, else
  (0, 0, 1); bytes `round((n * 0.5 + 0.5) * 255)`, alpha 255.

Measure: mean luminance of the rendered object's left quarter vs right quarter of the disc's bounding box (and top vs
bottom quarter). "Lit from the left" passes when `left > 1.3 * right` and `left > 40` (8-bit); likewise for top. A
flat comparison passes when the two quarters differ by at most 2 levels.

| Test | Scene | Expectation |
|---|---|---|
| R1 SpriteLitFromLeft / R2 SpriteLitFromTop | Sprite 64x64, `white` + `dome`, light at (-300, 0, height 64) / (0, 300, 64) | lit side bright |
| R3 SpriteGrid | same, sprite subdivided (the grid path, `requestedSpriteGrid` > 1, e.g. a lattice bound or the grid field the engine exposes) | as R1/R2 |
| R4 Polygon | MeshPolygon square 64x64 with the two textures | as R1/R2 |
| R5 Tilemap | Tilemap one 64x64 tile with `white` tileset and `dome` normal on its submesh | as R1/R2 |
| R6 MirroredScaleX / R7 MirroredScaleY / R8 MirroredBoth | Sprite with scale (-1,1,1) / (1,-1,1) / (-1,-1,1) | as R1/R2 (the dome is symmetric: the lit side follows the light) |
| R9 MirroredFrameRect | Sprite with texture rect (1, 0, -1, 1) and then (0, 1, 1, -1) | as R1/R2 |
| R10 ManualFlipY | Sprite with `setFlipY(false)` on the world camera (image upside down) | as R1/R2 |
| R11 Rotated90 | Sprite rotated 90 degrees about z | as R1/R2 |
| R12 Instanced | Sprite with 3 instances: identity, scale (-1,1,1), instance texture rect mirrored | each instance lit from the light's side |
| R13 LibrarySprite | R1/R2 with `material.shader = "library:sprite"`; and with OUTLINE on | as R1/R2 |
| R14 FlatIgnoresHeight | Sprite `white`, no normal map, light height 64 vs height 0 (two renders) | identical within 1 level |
| R15 LibraryFlatIgnoresHeight | R14 with `library:sprite` | identical within 1 level |
| R16 UICamera | the R1/R2 sprite in a scene whose camera is `CAMERA_UI` (y down), light placed left/top in that space | as R1/R2; if the renderer does not evaluate 2D lights for that scene, the test asserts the render equals the unlit render and U1 (flipY false) is the UI acceptance |
| R17 LitMirrored3D | a normal-mapped sprite drawn by the lit path (`material.lighting` per pixel) with one directional light from the left, at scale (1,1,1) and (-1,1,1), no Light 2D | left brighter in both |
| R18 MissingNormalFallsBackFlat | R17's object with `normalTexture` set to a missing path vs no normal texture | identical within 2 levels |
| R19 Before/after guard | R1 with the `dome` green channel inverted (DirectX map) | lit from the left still, but top/bottom reversed: R2 fails on it (proves the test distinguishes conventions) |

Run (no Docker on this host; `source /root/.ccache-env` first):

```
cmake -S engine -B build/linux/htmlui-tests-glcore -G Ninja -DCMAKE_BUILD_TYPE=Release -DJASPERENGINE_HTMLUI_TESTS=ON \
  -DGRAPHIC_BACKEND=glcore -DCMAKE_C_COMPILER_LAUNCHER=ccache -DCMAKE_CXX_COMPILER_LAUNCHER=ccache
cmake --build build/linux/htmlui-tests-glcore --target jasperengine-htmlui-tests
xvfb-run -a -s '-screen 0 1600x900x24' build/linux/htmlui-tests-glcore/tests/htmlui/jasperengine-htmlui-tests \
  --gtest_filter='Light2DNormalMap*'
# Vulkan (lavapipe): same with -DGRAPHIC_BACKEND=vulkan into build/linux/htmlui-tests-vulkan and
VK_ICD_FILENAMES=/usr/share/vulkan/icd.d/lvp_icd.x86_64.json xvfb-run -a ... --gtest_filter='Light2DNormalMap*'
```

Also run the whole `jasperengine-htmlui-tests` suite on glcore (existing goldens must not change: F2/F6 keep
non-mirrored, normal-map-loaded content bit-identical) and the Lattice/VAT render tests on both backends.

### 5.3 Editor end-to-end (`tools/mcp/tests/test_light2d_normals.py`, real editor, Xvfb)

- `create_entity {type: light_2d}` -> `get_properties` height 64; `add_component` Light2D on an empty GameObject ->
  64; `undo` restores.
- A 2D scene with a Sprite (`white`/`dome` PNGs written by the test with `pngdiff.encode`), `set_properties`
  `submeshes[0].material.normalTexture`, a Light 2D at the sprite's left, `ambient_light_2d_intensity` 0:
  `screenshot {view: "game"}` -> the left quarter is brighter (decode with `htmlui_pixels.decode_png`).
- Same with `submeshes[0].material.shader = "library:sprite"`.
- `get_properties` of the sprite after setting `material.shader` to `library:sprite` still exposes
  `submeshes[0].material.normalTexture` (Inspector row data path).
- A Lua script in play mode: `self.gameObject:setNormalTexture("dome.png")` (via the existing script test
  pattern in `test_script.py`) then `getNormalTexture()` returns the path; screenshot lit from the left.

Run: `tools/mcp/run-tests.sh tools/mcp/tests/test_light2d_normals.py` with `JASPER_EDITOR` pointing at the
worktree's `build/linux/editor-install/bin/jasperengine-editor` (`cmake --install` after building
`jasperengine-editor`). Also run `test_stubs.py`, `test_shader_library*.py`, `test_scene.py`, `test_world.py`,
`test_stochastic_tiling.py`, `test_docs.py`, and `editor-tests` (all must stay green).

## 6. Acceptance criteria

1. U1-U6, R1-R19 (both backends, R16 per its rule) and the e2e file pass; the full htmlui GPU suite (glcore), the
   editor tests and the listed MCP test files pass with no golden updated.
2. A normal map produced by the upstream tool with default (OpenGL) settings, on a sprite in the editor, is brighter
   on the light's side for lights left, right, above and below (R1/R2 and the e2e prove the mechanism).
3. No shader compile error or new warning on any backend for: built-in mesh (every 2D variant), `library:sprite`
   (all features), library shaders requiring `tangents` under 2D lights.
4. Docs: README Engine bullet ("2D lights, normal maps (OpenGL convention), and 2D shadow casters"), lua-api,
   shaders.md, library.md updated; migration notes from section 4 in the merge message text the implementer drafts in
   its report.
5. `-Wall -Wextra` clean in touched C++.

## 7. Files (ownership for wave 1)

Engine: `engine/core/subsystem/MeshSystem.{h,cpp}`, `engine/core/subsystem/RenderSystem.cpp`,
`engine/core/shader/ShaderManifest.{h,cpp}`, `engine/core/object/Mesh.{h,cpp}`,
`engine/core/script/binding/GameObjectClassesLua.cpp`, `engine/shaderlib/mesh.vert`, `engine/shaderlib/mesh.frag`,
`engine/shaderlib/includes/{surface_vs,lighting2d,stochastic}.glsl`, new `engine/shaderlib/includes/tangent_frame.glsl`,
`engine/shaderlib/library/sprite.frag`, new `engine/tests/htmlui/TestLight2DNormalMaps.cpp`.
Editor: `editor/command/type/{CreateEntityCmd,AddComponentCmd}.cpp`, `editor/window/InspectorWindow.cpp` (only the
Light 2D help and the Normal Texture row/help: Phase 4 edits other parts of this file), `editor/Catalog.{h,cpp}`
(`shaderReadsMaterialNormalMap`), new `editor/tests/Sprite2DTangentFrameTest.cpp`, new
`tools/mcp/tests/test_light2d_normals.py`.
Docs: `README.md` (Engine bullet only; Phase 3 adds the feature bullet), `docs/mcp/guide/lua-api.md`,
`docs/mcp/guide/shaders.md`, `docs/shaders/library.md`, `docs/mcp/guide/cpp-scripting.md` if applicable.
Not touched: `editor/mcp/McpInstructions.cpp` (Phase 3 owns the agent guide line and its 16 KB budget).

## 8. Task breakdown (one implementer, in order, one commit or more per task, prefix "Sprite normals P1:")

1. Tests first: write U1/U2 and R1/R2/R14 so they fail on the current code (R1 passes, R2 fails today: record it).
2. F1 (MeshSystem helper + four sites + comments) -> U1, U2, R1-R5, R10, R11 pass on glcore.
3. F2 (`tangent_frame.glsl`, both vertex paths) -> R6-R9, R12, R17 pass.
4. F3 shader macros + `mesh.frag` define -> R14 passes; F3 editor (CreateEntityCmd, AddComponentCmd, help texts) -> U5.
5. F4 (manifest bit, variant, `surface_vs`/`lighting2d` declarations and branches, `getNormal2DAt`, `sprite.frag`,
   `Catalog::shaderReadsMaterialNormalMap`, Inspector row) -> U3, U4, R13, R15.
6. F5 (Mesh C++ + Lua + docs) -> U6.
7. F6 -> R18.
8. R16, R19; Vulkan build and the whole new test file on lavapipe; full regression runs (section 5.2-5.3); docs;
   report with the migration notes, test commands and outputs.
