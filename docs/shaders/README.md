# Shaders, lighting and post-processing

Where to read about the engine's shader library, its lighting and its post-processing.

## Using them

| Document | What it covers |
|---|---|
| [library.md](library.md) | Reference of every library shader (`library:<name>`): parameters, textures, keywords, lighting modes and blends each supports. Generated from the manifests: `jasperengine-editor shaders --library all --doc docs/shaders/library.md` |
| [library.md#lighting-modes](library.md#lighting-modes) | Lighting modes: per pixel, per vertex, mixed (each light's **Shading**) and none; **Max Lights Per Object** (1-16, compile tiers 4/8/16, the rest folded into the ambient) |
| [library.md#blend-modes](library.md#blend-modes) | Blend modes and how a shader's output follows the blend drawn |
| [../mcp/guide/shaders.md](../mcp/guide/shaders.md) | Working with shaders: library shaders on materials and components, presets, stencil and the extra pass (outlines), forking built-in shaders, editing rules, the custom parameter block, the scene default shaders' values (inherited by every object without a shader of its own), manifests, scene depth and colour |
| [../mcp/guide/shaders.md#stochastic-tiling](../mcp/guide/shaders.md#stochastic-tiling) | Stochastic Tiling: hiding texture repetition on the built-in Lit material (`stochasticTiling`, variant bit `Sto`), on terrain layers and with the library's `STOCHASTIC` feature; the include `engine/shaderlib/includes/stochastic.glsl` (Mikkelsen's hex-tiling, MIT) documents its API |
| [../mcp/guide/shaders.md#water](../mcp/guide/shaders.md#water) | Water: `library:water`'s features and their costs (reflection, the underside, refraction, caustics), 3D Object > Water, the automatic `library:underwater` pass, buoyancy and the Lua surface queries (`Water`, `engine/core/object/environment/Water.h`) |
| [../mcp/guide/materials.md](../mcp/guide/materials.md) | The built-in Lit's materials and lighting: the extended lobes (clear coat, sheen, anisotropy, iridescence, transmission and volume, specular and IOR, emission intensity) from glTF `KHR_materials_*`, the extension map budget, hex tiling of materials and terrain layers, Lit Quality, photometric light units and colour temperature, the Mobile PBR Tier |
| [../mcp/guide/shaders.md#global-values](../mcp/guide/shaders.md#global-values) | Global values: Project Settings > Graphics > Shader Globals, the `u_vs_globals` / `u_fs_globals` blocks and texture globals, Lua `Shader.setGlobal*`, bindings of library shader parameters to globals, the `SURFACE_VERTEX` hook (four vertex lights), `check_shader` codes, costs ([spec](../superpowers/specs/2026-10-31-shader-globals-design.md)) |
| [../mcp/guide/post-processing.md](../mcp/guide/post-processing.md) | Post-processing of the main camera: cost tiers (in-shader colour, uber pass, bloom/blur chain, custom passes), presets, the cost readout, mobile advice, every field, Lua, measuring with frame stats, limits |
| [../mcp/guide/html-ui.md#effects](../mcp/guide/html-ui.md#effects), [../mcp/guide/html-ui-effects.md](../mcp/guide/html-ui-effects.md) | HTML/CSS UI effects: `effect(<name>, ...)` in a document's CSS, the effects library (shader type `UI_EFFECT`, ids `@effects/<name>`, project effects declared with `@effect`), separate from `library:` shaders; its frame order with post-processing (`docs/html-ui/DESIGN.md`, "Integration with origin/main") |
| [../mcp/guide/lua-api.md](../mcp/guide/lua-api.md) | Lua: `material.shader`, `setShaderParam` / `setShaderTexture` / `setShaderFeature`, the lobes (`material.clearcoatEnabled` ...), `light.shading`, `light.intensityUnit` / `colorTemperature`, `scene.maxLightsPerObject`, `scene.litQuality`, `scene.postProcessing` (custom passes: the post-processing guide) |

The same guides are served to coding agents by the editor's MCP server as
`jasper://docs/shaders`, `jasper://docs/materials` and `jasper://docs/post-processing`
(`list_shaders`, `check_shader`, `get_material_variant`, `set_light_unit`,
`list_post_processing_presets` and `apply_post_processing_preset` are the matching tools).

## Design

The shader library was built in four phases; each spec ends with implementation notes that
record the decisions, deviations, measurements and design limits of the shipped code.

| Phase | Spec | Contents |
|---|---|---|
| 1 | [shader library and unified lighting](../superpowers/specs/2026-10-10-shader-library-phase1-design.md) | Manifests (`@shader`, `@property`, `@texture`, keywords, `@state`, `@require`), the shared includes, the first library shaders, unified lighting (unlimited lights, per-object list and cap, pixel/vertex/mixed modes) |
| 2 | [extra passes and stencil](../superpowers/specs/2026-10-10-shader-library-phase2-design.md) | Stencil state, the material extra pass (outlines, silhouettes, masks), more library shaders |
| 3 | [scene inputs](../superpowers/specs/2026-10-10-shader-library-phase3-design.md) | Scene depth and colour for mesh shaders (soft particles, depth-fade water, glass, heat haze, projected decals), the scene-colour copy's resolution, runtime variant switches without stalls |
| 4 | [fast post-processing](../superpowers/specs/2026-10-10-shader-library-phase4-design.md) | The post-processing stack: in-shader colour (LUT), the uber pass, bloom and blur, custom passes with library post shaders, render scale, MSAA, the per-frame pass list |
| S11 | [materials and lighting](../specs/S11-materials-lighting.md) | The built-in Lit's extended lobes as `builtin:lit#...` variants (zero cost unused), glTF `KHR_materials_*`, Lit Quality, photometric units, hex tiling |

## Checking

- `jasperengine-editor shaders --library all --backend all` compiles every library shader's
  variant matrix on the six backends and validates the manifests, and the built-in Lit's own
  matrix (`--library builtin_lit` alone): every lobe, map, hex tiling and Lit Quality variant,
  each checked against the 16 fragment textures of GLES3 and WebGL2.
- Fixtures under `tools/fixtures/` (`shader-library`, `scene-inputs`, `stencil`,
  `extra-pass`, `unified-lighting`, `post-processing`, `shader-library-export`...) run with
  `tools/smoke-test.sh <fixture>`; `JASPERENGINE_READY_SIGNAL=1 JASPERENGINE_FRAME_STATS=1`
  logs each frame's passes, draws and pass list.
