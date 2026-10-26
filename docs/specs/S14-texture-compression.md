# S14 — Texture compression pipeline (per-platform cook, KTX2/Basis, cooked cache, import settings)

Wave 2. Report: `docs/openengine-analysis/01-rendering.md` §1.9, §2.7, item 4 of the port list;
`00-SUMMARY.md` row 27.

## 1. Goal

Every texture a game ships is cooked at export into the GPU-native compressed format of its target
(BC on desktop, ASTC on iOS/Android, optional ETC2/EAC for legacy Android, Basis Universal KTX2 for the web,
transcoded at load to whatever the browser supports). Mips are generated offline with correct filters.
Per-texture import settings (type, max size, mips, quality, per-platform overrides) work like Unity's
Texture Importer. Cooked results live in a content-addressed cache, and the editor cooks in the background.
Today we decode PNG/JPG at runtime, box-filter the mips in gamma space and upload RGBA8 everywhere
(42.7 bits per texel with mips). The target is ≤ 5.4 bits per texel on desktop and ≤ 4.8 on mobile
at default quality, smaller downloads than Unity at equal or better PSNR, and faster loads.

## 2. Scope

**In scope**
- Texture import settings stored per file, with per-platform overrides, editor inspector, MCP and CLI.
- Cook pipeline for 2D textures and cubemaps: decode, resize, mip chain (linear-light filtering,
  alpha-coverage preservation, normal renormalization), encode, KTX2 container, zstd.
- Encoders: BC1/BC3/BC4/BC5/BC6H/BC7, ASTC 4x4–12x12 (square), ETC2 RGB/RGBA and EAC R11/RG11,
  Basis ETC1S / UASTC LDR / UASTC HDR. Uncompressed R8/RG8/RGBA8/R16/RGB9E5/RGBA16F.
- Runtime: a KTX2 loader, format capability queries, web transcode, CPU-decode fallbacks,
  RGB565/RGBA4444 fallback on GL-family backends, global mip limit, memory budget, progressive mip tail.
- A content-addressed cooked cache, background cooking on S06 jobs, reimport on S05 file events.
- Export integration: only the target family's files ship, sources are dropped, duplicate cooks are deduplicated.
- Unifying normal-map decoding (RG layout + Z reconstruction) across the built-in shaders.
- `sokol_gfx.h` patch: ASTC 5x5/6x6/8x8/10x10/12x12 and RGB565/RGBA4 (GL family only).

**Out of scope**
- Feedback-driven (screen-space / UV-density) mip streaming. Only budget-driven mip dropping is in v1.
- 3D and array texture assets (S12's noise volumes stay uncompressed), runtime-generated textures
  (glyph atlases S16, render targets, `createDynamicTexture`), texture atlasing / sprite packing.
- Switching colour textures to hardware sRGB sampling (see D7), moving sampler state (wrap/filter) from the
  referencing property into the importer (it stays where `set_texture_settings` puts it today), and ASTC HDR.
- Do-not-copy list: none of the excluded items apply. We do **not** take their bindless registration,
  their `Image/*` decode/resample modules, or vcpkg ports.

## 3. Current state

- **Decode**: `engine/core/texture/TextureData.cpp:186-210` uses `stb_image` (8/16-bit) plus LunaSVG for `.svg`.
  There is no KTX/DDS/Basis reader. `TextureData` (`TextureData.h:15-116`) holds one level of raw pixels
  (`ColorFormat` RED/RGBA/RED16). `originalWidth/Height` already survive resizes, and `Texture::getWidth()`
  (`Texture.cpp:489-501`) returns them, so UI layout uses the source size.
- **Load**: `TextureDataPool::loadFromFile` (`engine/core/pool/TextureDataPool.cpp:93-175`) decodes on
  `ThreadPoolManager` and applies `fixAlphaBorder` plus the project-wide `TextureStrategy` FIT/RESIZE/NONE
  (`Engine.h:185`, pool `:251-263`) at every load.
- **Upload**: `SokolTexture::createTexture` (`engine/renders/sokol/SokolTexture.cpp:259-330`) maps only
  RGBA→RGBA8, RED→R8, RED16→R16. Mips come from `generateMipmaps` (`:140-257`), a 2x2 box filter on
  sRGB-encoded bytes (wrong in gamma space, no alpha weighting, no coverage handling, no normal
  renormalization), and it fails for any other format. Render targets use `ColorFormat` (`Render.h:320`).
- **Colour space**: colour textures upload UNORM and the shaders decode with `sRGBToLinear()` after filtering
  (`engine/shaderlib/mesh.frag:205`, `ui.frag:51`, `gbuffer.frag:47`). Normal maps are decoded as `.rgb*2-1`
  (`shaderlib/includes/pbr.glsl:117`, `includes/lighting2d.glsl:36`). `water.frag:165` already reads `.xy`.
  Shader manifests tag normal slots (`@texture u_normalTexture normal`, parsed at `ShaderManifest.cpp:659-662`).
- **sokol** (`engine/libs/sokol/sokol_gfx.h`, vendored with local patches): the enum (`:2177-2200`) has
  BC1–BC7 (no BC1 sRGB), ETC2/EAC, and **only ASTC 4x4**. There is **no RGB565/RGBA4444**, and `RGB9E5`
  (`:2158`) is sample-only. Capability detection by backend:
  - GL core (`:10030-10080`): s3tc/rgtc/bptc/astc extensions, ETC2 via ES3_compatibility. macOS GL 4.1
    has no BPTC, so no BC6H/BC7.
  - GLES3/WebGL2 (`:10156-10230`): ETC2 is always on natively, but on Emscripten only with
    `WEBGL_compressed_texture_etc`.
  - Metal (`:14781-14805`): BC only on macOS, ETC2/ASTC only on iOS.
  - D3D11 (`:12905-12920`): BC only.
  - Vulkan: mapping at `:20327-20350`, queried per format with `vkGetPhysicalDeviceFormatProperties` (`:20632`).
  - Pitch math (`_sg_block_bytesize` `:8702`, `_sg_row_pitch` `:8738`) assumes square blocks.
    Square ASTC sizes fit it; 6x5, 8x6 and the like do not.
- **Export**: `Exporter::copyAssets` (`editor/Exporter.cpp:1736-1805`) copies every asset file verbatim.
  The web build preloads the whole assets tree (`engine/CMakeLists.txt:193`). Web pthreads are off by
  default (`:208`), so web transcode runs on the main thread. Export targets: `ExportWindow::BuildPlatform`
  (Windows/MacOS/Linux/Android/IOS/Web) and `ShaderBackend` (`engine/core/shader/ShaderBuildTypes.h:18`).
  A source export can target several platforms at once (`ExportWindow.cpp:225-230`).
- **CPU pixel readers** (must not be handed block-compressed data): terrain height and density maps
  (`MeshSystem.cpp:2663,2970,3409`), post LUT strips (`util/PostColorLut.cpp:270`), IBL generation from LDR
  cubemap faces (`texture/IBLEnvironment.h:24`), and Lua `Texture:getData()` (`CoreClassesLua.cpp:1091`).
- **Asset metadata**: `editor/util/AssetDatabase.h` keeps GUIDs for `.prefab` files only. Image files have no
  sidecar (S04 adds them). The project cache dir `.jasperengine/` is git-ignored (`Project.cpp:5237`).
  Hashes: `sha256Hex` and `fnv1a64` in `engine/core/htmlui/compat/Hash.h`.
- **MCP**: `set_texture_settings` (`editor/mcp/tools/AssetTools.cpp:3251`) edits sampler state on a component
  property. The inspector has no file-asset (importer) view. `ImageViewerWindow` previews files.

## 4. Reference (OpenEngine, MIT; read-only at `external/OpenEngine`)

| File | Take | Improve |
|---|---|---|
| `Engine/Source/Assets/TextureCook.cpp:170-230` (key at `:177-185`, versions `:188-215`) | Cache key = GUID + source hash + config hash. The config hash folds in settings, colour space, swizzle, output format, `kTextureCookVersion` and **encoder library versions plus the encoder ISA** for encoders whose bytes differ per ISA. | Key on content, not GUID, so duplicates share one artifact. Use SHA-256 (`Hash.h`) instead of FNV-1a 64. Add the target family and per-platform resolved settings. |
| `Engine/Include/Assets/TextureCook.h:28-130,186-247` | Usage classification by material slot (Color/Normal/Mask/Packed) with widening rules. One shared mip-chain recipe for cook and raw upload. Alpha-coverage semantics (scale non-base alpha, ties favour scale 1, zero stays zero). Banded encode with cancel polling and byte-identical results at any thread count. | Classification becomes our `TextureType` with Unity names, inferred once and stored. We add ASTC/ETC2/Basis, which they lack. |
| `Tools/TextureCompiler/TextureCompiler.cpp` (394 lines) | UASTC + zstd KTX2 options, per-mip normal renormalization (`:218-272`), sRGB/linear in the DFD, the `--decode` verify mode. | It is folded into our CLI `cook-textures --decode`. No separate tool. |
| `Engine/Source/Engine/Rendering/TextureService.cpp:191-195,1474-1510` | Transcode-target and upload-format switch. | Theirs is BC7-or-RGBA32 only. Ours is a per-kind priority list over all families (D5). |
| `Engine/Source/Assets/Textures/BC7TransparentEndpointRepair.cpp` (109) + `Engine/Tests/BC7TransparentEndpointRepairTests.cpp` | **Copy** (MIT, keep header, `NOTICE.md` entry): clears the BC7 mode 6/7 p-bit on transparent endpoints so alpha-0 texels stay 0. | None. |
| `Engine/Tests/TextureCook*Tests.cpp` | Test ideas: determinism, banding equality, key sensitivity. | — |

Licences: their code is MIT. libktx is Apache-2.0, but we do **not** use it (D2).

## 5. Requirements

### Functional
- **R1 Import settings.** Every texture source (`.png .jpg .jpeg .tga .bmp .psd .gif .hdr .exr .svg .ktx2`)
  has importer settings with these fields and defaults:
  - `type`: `default|normal|sprite|ui|lightmap|single_channel|hdr`
  - `srgb` (type default)
  - `alphaSource`: `input|from_gray|none`
  - `alphaIsTransparency` (bakes today's `fixAlphaBorder`)
  - `readable` (Unity Read/Write)
  - `maxSize` (32…16384, default 2048), `resizeFilter`
  - `mipmaps`, `mipFilter`: `box|triangle|mitchell|catmull_rom`, default mitchell
  - `mipLimit`, `preserveCoverage` + `alphaCutoff` (0.5)
  - `flipGreen` (normal), `channel`: `r|g|b|a|luma` (single_channel)
  - `quality`: `none|low|normal|high`, `effort`: `fast|normal|best`
  - `streamable`
  - `platforms.{desktop,mobile,android_etc2,web}.{override,maxSize,format,quality,effort,rdo}`

  Settings that are absent resolve to defaults. Unknown keys are preserved (S03).
- **R2 Type inference.** When a texture first gets settings, its type is inferred from its references:
  - an S04 dependency on a normal slot (glTF `normalTexture`, manifest `@texture … normal`) → `normal`
  - SpriteComponent/Tilemap/particle → `sprite`; UI image / HTML UI `<img>` → `ui`
  - occlusion slot → `single_channel`; `.hdr/.exr` → `hdr`; otherwise `default`

  The result is written once and never silently re-inferred. A later reference that conflicts (for
  example a colour slot binding a `normal` texture) is a warning in the inspector and in `check`.
- **R3 CPU-readable slots.** Textures referenced by terrain `heightMap`/`densityMap`, post-process LUTs (S13)
  or marked `readable` cook to an uncompressed KTX2 (R16 for 16-bit sources, else R8/RGBA8) with zstd
  and stay CPU-addressable at runtime. Skybox cubemaps that feed IBL also cook a 256² RGBA8 cube sidecar
  for `IBLEnvironment::generate`. `Texture:getData()` on a block-compressed texture returns empty data
  and logs once: `"<path>: pixels are GPU-compressed; enable Read/Write in its import settings"`.
- **R4 Mip chain.** One `MipChain` implementation, built on `stb_image_resize2` (vendored), serves both the
  cook and the raw-load path. It replaces `SokolTexture::generateMipmaps` for every format:
  - sRGB textures filter in linear light with alpha-weighted RGB; linear data filters per channel; HDR filters in float.
  - `normal`: decode, filter, renormalize XYZ per level, then store XY.
  - `preserveCoverage`: every level's alpha-test coverage at `alphaCutoff` is within 2% (absolute) of level 0's.
  - `sprite`/`ui`: mips are off by default and alpha bleeding is applied before every level.
- **R5 Format table.** The resolved format comes from the type, quality, has-alpha (any alpha < 255 after
  `alphaSource`) and the platform family. Explicit `format` overrides it.

  | type | desktop (BC) L / N / H | mobile (ASTC) L / N / H | android_etc2 | web (KTX2 Basis) L / N / H |
  |---|---|---|---|---|
  | default, opaque | BC1 / BC1 / BC7 | 8x8 / 6x6 / 4x4 | ETC2_RGB8 | ETC1S q64 / ETC1S q128 / UASTC λ1 |
  | default, alpha | BC7 λ2 / BC7 λ1 / BC7 | 8x8 / 6x6 / 4x4 | ETC2_RGBA8 | ETC1S q64 / ETC1S q128 / UASTC λ1 |
  | normal (RG) | BC5 / BC5 / BC5 | 8x8 / 6x6 / 5x5 | EAC_RG11 | UASTC λ2 / λ1 / λ0 |
  | sprite, ui | BC7 λ2 / BC7 λ0.5 / BC7 | 8x8 / 6x6 / 4x4 | ETC2_RGB8/RGBA8 | ETC1S q128 / UASTC λ1 / UASTC λ0 |
  | single_channel | BC4 / BC4 / BC4 | 10x10 / 8x8 / 6x6 (L) | EAC_R11 | ETC1S q128 / ETC1S q255 / UASTC λ1 |
  | hdr, lightmap(float src) | BC6H (all) | RGB9E5 / RGB9E5 / RGBA16F | RGB9E5 | UASTC-HDR 4x4 (all) |
  | lightmap (LDR src) | as default, linear | as default, linear | as default | as default, linear |

  - `quality: none` gives the uncompressed equivalent (R8/RG8/RGBA8/RGBA16F). λ is the RDO lambda.
  - UASTC and all raw block payloads get zstd (level 19 at `best`, 15 at `normal`, 9 at `fast`).
    ETC1S uses BasisLZ.
  - Opaque BC7 at High drops alpha encoding (mode-6 opaque).
- **R6 Block-size rule.** If a block format needs mip-0 dimensions that are a multiple of 4 (BC and ETC2
  everywhere, because of D3D11) and the source does not have them:
  - `default/normal/single_channel/hdr/lightmap` are rescaled to the nearest multiple of 4.
  - `sprite/ui` fall back to the uncompressed format, with an inspector warning.

  ASTC accepts any size.
- **R7 Cooked cache.** Each artifact is stored at `.jasperengine/cache/textures/v1/<k[0:2]>/<k>.ktx2`
  (written atomically, S03), with a `<k>.json` record (format, dims, mips, bytes, PSNR, cook ms, encoder
  versions). `k` = SHA-256 over:
  - the source content SHA-256;
  - the canonical JSON of the *resolved* per-family settings;
  - the target family and the resolved format;
  - `kTextureCookVersion`, the encoder library versions, and the encoder ISA for encoders that are not
    proven ISA-invariant.

  A change to any of these produces a new key. The source hash cache (path, size, mtime → hash) avoids
  rehashing. Size is capped by LRU eviction (default 4 GB, Project Settings). An optional user-global cache
  dir is shared across projects (safe because keys are content-addressed).
- **R8 Background cook (editor).**
  - Cooking runs on S06's background lane, with priorities. The selected texture and the active scene's
    textures cook first. Jobs can be cancelled when settings or the source change (cancel is polled per
    band, as in OpenEngine).
  - Results are byte-identical at any thread count. The editor renders with the **desktop** cook once it
    exists (raw + `MipChain` until then) and swaps in place (`TexturePool::invalidate`).
  - An S05 source change or a settings edit requeues the cook.
  - Concurrent cooks are limited by a working-memory budget (default 2 GB), so a few 8K textures cannot
    exhaust RAM.
- **R9 Export cook.**
  - Export cooks every texture source under the assets tree for each family the export targets:
    Windows/Linux/macOS → `desktop`; iOS → `mobile`; Android → `mobile`, plus `android_etc2` when
    Project Settings › Texture › *Android ETC2 fallback* is on; Web → `web`. Cache hits make it fast.
  - Output goes to `assets/je-cooked/<family>/<k>.ktx2` (identical cooks dedupe), with
    `assets/je-cooked/<family>/index.json` mapping each asset-relative source path to its file, logical
    size, format and flags.
  - `copyAssets` skips cooked sources unless the texture has `shipSource: true`. `je-cooked` is reserved
    like `resources.pak` (`Exporter.cpp:140`).
  - Multi-platform source exports include each needed family. The generated CMake/Gradle/Xcode packaging
    and the web `--preload-file` include only the family their platform loads.
  - Export fails, naming the file, on a cook error. No silent RGBA8 substitution.
- **R10 Runtime load.** `TextureDataPool` resolves a path through the active family's `index.json`. It reads
  the KTX2, inflates zstd, and on web transcodes Basis per D5. It skips mip levels above the effective
  mip limit **without reading their bytes** (KTX2 stores levels smallest-first). It uploads with the
  matching `sg_pixel_format`. Paths not in the index (user files, `user://`, editor) take the raw path.
  Source `.ktx2` files load directly in the editor and in raw mode.
- **R11 Fallbacks.** If the device cannot sample the cooked format:
  - **desktop**: CPU-decode BC→RGBA8 / RG8 / R8 / RGBA16F (bcdec). This covers macOS GL without BPTC.
  - **web**: transcode to the next entry in the D5 list.
  - **mobile**: the Android manifest declares `<supports-gl-texture android:name="GL_KHR_texture_compression_astc_ldr"/>`
    unless ETC2 fallback is on, in which case the device loads `android_etc2`. iOS always has ASTC.
  - Last resort on GL-family backends: RGB565 (opaque) / RGBA4444 (alpha) when Project Settings ›
    Texture › *Low-memory fallback* is on (default on for web, off elsewhere). Otherwise RGBA8.
    Single-channel and normal textures fall back to R8/RG8.

  The first fallback per format logs once, at info level, with the reason.
- **R12 Quality and budget.**
  - The `textureQuality` setting (0 full … 3 eighth; Unity's Global Mipmap Limit) drops top mips of
    `streamable` textures at load. `sprite/ui` textures are never streamable.
  - `textureMemoryBudgetMB` per platform (default: desktop 0 = unlimited, mobile 512, web 384). When resident
    texture bytes exceed it, the loader drops one extra mip from the largest streamable texture and repeats
    until the total fits (a texture keeps at least 64 px). It re-evaluates on texture load/unload, never per frame.
  - Progressive tail: a texture first uploads its ≤ 64 px mip tail (one read of the file end), then swaps
    to the full chain when the rest is loaded.
- **R13 Normal maps.** All built-in shaders decode normal maps as `xy = t.rg*2-1, z = sqrt(saturate(1-dot(xy,xy)))`
  through a new include function `decodeNormalMap(vec4)`. This covers `pbr.glsl:117`, `lighting2d.glsl:36`,
  `library/glass.frag` and `library/simple_lit.frag`. The function is documented for custom shaders.
  Every family stores normal maps as RG (BC5, EAC_RG11, ASTC `rg01`, UASTC RG). `flipGreen` inverts G at cook.

### Platform & backends
- **R14 sokol patch.**
  - Add `SG_PIXELFORMAT_ASTC_{5x5,6x6,8x8,10x10,12x12}_{RGBA,SRGBA}`, mapped on GL/GLES3/WebGL2 (the
    `astc_ldr` extension), Metal iOS, Vulkan (per-format query) and WGPU. `_sg_block_dim`/`_sg_block_bytesize`
    get the block sizes.
  - Add `SG_PIXELFORMAT_RGB565`/`RGBA4` on GL/GLES3/WebGL2 only (core formats). The other backends report
    them unsupported.
  - Submit the patch upstream. Until it merges, record it in `engine/libs/sokol/PATCHES.md`.
- **R15 Capability query.** `SystemRender::isTextureFormatSupported(GpuTextureFormat)` returns true iff
  `sg_query_pixelformat(f).sample && .filter`. The result is cached at device creation and shown in S15's
  VRAM/diagnostics panel. Every cooked format maps to exactly one `sg_pixel_format`.
- **R16 No new GPU requirements.** No compute or extra features are needed. Every backend × family
  combination renders correctly, through native support or a fallback.

### Performance (reference: 8-core x86 desktop, Pixel 6a, mid iPhone, desktop Chrome)
- **R17 VRAM.** At Normal quality, cooked textures use these bits per mip-0 texel, including mips
  (today: 42.7):

  | desktop opaque | desktop alpha | mobile default | mobile mask | web (BC7/ASTC4x4) |
  |---|---|---|---|---|
  | 5.33 (BC1) | 10.67 (BC7) | 4.74 (6x6) | 2.67 (8x8) | 10.67, or 5.33 when ETC1S lands on ETC2_RGB8/BC1 |

- **R18 Disk/download.** Corpus averages in bits per mip-0 texel, including mips:

  | desktop BC1 | desktop BC7 RDO λ1 + zstd | mobile 6x6 + zstd | web ETC1S q128 | web UASTC λ1 + zstd | normal UASTC λ1 |
  |---|---|---|---|---|---|
  | ≤ 4.8 | ≤ 7.5 | ≤ 4.6 | ≤ 2.0 | ≤ 6.0 | ≤ 6.0 |

- **R19 Load time.** Loading a 2048² BC7 KTX2 with mips (read + zstd + upload) takes ≤ 12 ms on the loader
  thread and ≤ 0.5 ms of main-thread time (≥ 3× faster than today's PNG decode + mip generation, measured
  by `benchmark`). On the web, transcoding a 2048² UASTC to BC7/ASTC takes ≤ 40 ms (desktop Chrome) and
  ≤ 120 ms (Pixel 6a). After the first frame, transcoding is capped at one level per frame on the main thread.
- **R20 Cook time.** A cold 2048² default cook takes ≤ 3 s (desktop BC1), ≤ 6 s (ASTC 6x6 `normal` effort)
  and ≤ 12 s (UASTC λ1), all threads. A warm (cache-hit) export of 200 textures takes ≤ 10 s for the texture
  step. The editor UI thread never blocks more than 2 ms per frame on cook bookkeeping.
- **R21 Binary size.** Native runtimes add ≤ 120 KB (zstd decoder + bcdec + KTX2 reader, Release, stripped).
  The web wasm adds ≤ 250 KB uncompressed (Basis transcoder built with only the D5 targets). Encoders are
  editor-only.

### Editor UX (human)
- **R22 Texture importer inspector.** Selecting a texture file in the Project window shows *Texture Import
  Settings* in the Inspector (Unity layout).
  - Preview: checkerboard for alpha; R/G/B/A channel toggles; mip slider; **Source / Cooked** A/B split
    slider for the chosen platform. The cooked image is decoded on the CPU when the GPU lacks the format.
  - Info line, e.g. `2048×2048 · BC7 · 12 mips · VRAM 5.3 MB · Disk 3.9 MB (source 7.1 MB) · PSNR 43.1 dB`.
  - Fields from R1, grouped (General / Advanced / Mip Maps). A platform tab bar *Default | Desktop | Mobile |
    Web (| Android ETC2)* has an *Override for …* checkbox.
  - A per-platform table: Format / VRAM / Disk / PSNR / status (cooked, queued, cooking, stale, failed + message).
  - *Apply* / *Revert*. Edits are one undo step (S24), and multi-selection edits all of them (S08 multi-edit).
    Warnings cover R2 conflicts, R6 and `maxSize` clamping.
- **R23 Project window and status.** Textures show a small badge while queued, cooking or failed. The status
  bar shows `Cooking textures 12/40` with cancel. A context menu offers *Reimport* and *Reimport All Textures*.
- **R24 Project Settings › Texture.**
  - Per-family defaults (quality, maxSize), Android ETC2 fallback, low-memory fallback,
    `textureQuality`/budget per platform.
  - Cache size cap, global cache dir, *Clear Texture Cache*.
- **R25 Export window.** It shows the estimated texture download and VRAM for the selected platform,
  per-family totals, and *Cook Textures Now*.
- **R26 Game view.** *Simulate platform textures: Desktop / Mobile / Web* renders with that family's cooked
  data (decoded to RGBA8 when the GPU cannot sample it), so artefacts are visible before export.

### Agent UX (MCP, Lua, CLI)
- **R27 MCP tools** (added to `AssetTools.cpp`; descriptions kept short for `tools/mcp/tool_budget.py`):
  - `get_texture_import_settings {path, platform?}` → stored and resolved settings, inferred type and its
    reason, warnings.
  - `set_texture_import_settings {paths[], settings{…}, platform?}` → partial update. It is undoable,
    validates enums and ranges, and queues a recook.
  - `reimport_textures {paths[]|all, platforms[]?, wait?=true, timeout_s?}` → cook results.
  - `get_texture_cook_report {paths[]?, platforms[]?, include_preview?}` → per texture: format, dims, mips,
    VRAM, disk, source bytes, PSNR, cache hit, status. Plus totals per family. `include_preview` returns a
    decoded PNG like `view_asset`.

  The guide (`docs/mcp/guide/`) gains a texture section, and `docs/mcp/TOOLS.md` is regenerated
  (`tools/mcp/gen_tool_docs.py`).
- **R28 Lua API.**
  - `Texture`: `isCompressed()`, `getGpuFormat()` (enum string), `getStoredWidth()/getStoredHeight()`
    (after maxSize/mip drop).
  - `Engine`: `setTextureQuality(int)`, `getTextureQuality()`, `setTextureMemoryBudget(mb)`,
    `getTextureMemoryUsage()`, `isTextureFormatSupported(fmt)`.

  Stubs are regenerated (`generate_lua_stubs.py`), and `docs/mcp/guide/lua-api.md` is updated.
- **R29 CLI.**
  - `jasperengine-editor cook-textures --project P [--platform desktop|mobile|android_etc2|web|all]
    [--paths …] [--report out.json] [--jobs N] [--check]` cooks into the cache and prints the R27 report.
    `--check` exits 1 if any cook fails or any warning-level R2/R6 issue exists.
  - `cook-textures --decode in.ktx2 out.png [--level N] [--platform-target bc7|astc|…]` verifies a file.
  - `export` cooks implicitly. `check` reports texture import problems.

### Data & compatibility
- **R30 Storage.** Importer settings live in the S04 per-asset sidecar under `textureImporter:` (`version: 1`).
  Old projects get inferred settings on first open (R2), which S04 writes with the GUID. Missing sidecars never
  fail a load.
- **R31 Unchanged references.** Scene, material and Lua references still store source paths. No scene format
  change is needed, so no S03 version bump. Raw PNG loading stays fully supported at runtime.
  `TextureStrategy` FIT/RESIZE applies at cook time for cooked textures (same visible result).
- **R32 KTX2 metadata.** Cooked KTX2 files carry key/values `JE.logical` (source w,h), `JE.type`, `JE.flags`
  (alpha, normalRG, coverage) and the sRGB transfer in the DFD. `Texture::getWidth()` keeps returning the
  logical size, so UI/sprite layout does not change when maxSize, mip limit or budget shrink the GPU image.

## 6. Design constraints & decisions

- **D1 Libraries.** Each is vendored under `engine/libs/` with a `NOTICE.md` entry.

  | Library | Licence | Role | Where |
  |---|---|---|---|
  | basis_universal | Apache-2.0 | ETC1S/UASTC/UASTC-HDR encoders + RDO; KTX2 transcoder; UASTC→ETC2/EAC; UASTC-HDR→BC6H | encoder: editor; transcoder: editor + web runtime |
  | astc-encoder | Apache-2.0 | ASTC encode (all square sizes, built with `ASTCENC_INVARIANCE=ON`), CPU decode for previews | editor |
  | bc7enc_rdo | MIT / Unlicense | BC7 + BC1/BC3/BC4/BC5 (rgbcx) encode with RDO, scalar build, no ISPC | editor |
  | zstd | BSD-3 | supercompression | encoder: editor; decoder-only: runtime |
  | bcdec | MIT / Unlicense | BC1–BC7 CPU decode for desktop fallback and previews | runtime + editor |

  Why these:
  - bc7enc_rdo has the best BC7 quality per encode time among MIT encoders, plus RDO, which buys
    20–35% smaller desktop downloads.
  - astcenc is the reference ASTC encoder; its invariance mode gives byte-identical output across ISAs.
  - basisu is the only production web-universal codec.
  - BC6H and ETC2/EAC come from basisu transcodes (UASTC-HDR→BC6H, UASTC→ETC2/EAC at max quality),
    so we avoid adding DirectXTex or etc2comp. The PSNR floors in AC4 guard their quality.
- **D2 No libktx.** basisu already reads and writes Basis KTX2, and its transcoder handles supercompressed
  levels. Raw-payload KTX2 (BC/ASTC/ETC2/uncompressed) uses our own reader/writer in
  `engine/core/texture/Ktx2.{h,cpp}` (≤ 600 lines: header, level index, DFD subset, key/value, zstd).
  This avoids a second basisu copy, a second zstd copy, and about 1 MB of binary.
- **D3 Families, not backends.** The cook target is a family (`desktop|mobile|android_etc2|web`). One desktop
  cook serves GL/D3D11/Vulkan/Metal-macOS (Intel and Apple Silicon both sample BC). macOS ships BC, not ASTC.
- **D4 Content addressing.** Cache and export filenames are the R7 key. The S04 GUID is not part of it.
  Renaming a texture never recooks it, and duplicates share bytes.
- **D5 Web transcode priority** (first supported wins):
  - ETC1S colour: opaque `ETC2_RGB8 > BC1 > ASTC4x4 > BC7`; alpha `ETC2_RGBA8 > BC3 > ASTC4x4 > BC7`.
  - UASTC colour: `ASTC4x4 > BC7 > ETC2 > BC3/BC1`.
  - normal: `BC5 > EAC_RG11 > ASTC4x4 > RG8`; single: `BC4 > EAC_R11 > ASTC4x4 > R8`.
  - HDR: `BC6H > RGB9E5 > RGBA16F`.
  - Then R11's last resort.
- **D6 One mip recipe.** The cook and the raw path call the same `MipChain::build(levels, MipChainOptions)`
  (`engine/core/texture/MipChain.{h,cpp}`). `SokolTexture::generateMipmaps` is deleted, and SokolTexture only
  uploads the levels it is given.
- **D7 Colour space.** Colour textures keep uploading UNORM, and shaders keep calling `sRGBToLinear()`.
  The sRGB flag is written into every KTX2 DFD, so a later switch to `*_SRGBA` formats (owned by S11)
  needs no recook. Encoders use perceptual/sRGB error metrics for sRGB textures.
- **D8 Normal layout RG everywhere** (R13). ASTC's `rrrg` luminance-alpha mode is better per bit, but it
  needs a per-texture decode switch. Deferred; reconsider if AC4's 6x6 normal-map floor fails.
- **D9 Runtime structures.**
  ```cpp
  // engine/core/texture/TextureFormat.h
  enum class GpuTextureFormat : uint8_t { R8, RG8, RGBA8, R16, RGB565, RGBA4, RGB9E5, RGBA16F,
      BC1, BC3, BC4, BC5, BC6H, BC7, ETC2_RGB8, ETC2_RGBA8, EAC_R11, EAC_RG11,
      ASTC_4x4, ASTC_5x5, ASTC_6x6, ASTC_8x8, ASTC_10x10, ASTC_12x12 };
  struct GpuFormatInfo { uint8_t blockDim, bytesPerBlock; bool compressed, alpha, hdr; };
  const GpuFormatInfo& gpuFormatInfo(GpuTextureFormat);
  sg_pixel_format toSokol(GpuTextureFormat, bool srgb);       // SokolTexture.cpp only
  // TextureData gains: GpuTextureFormat gpuFormat; bool srgb; std::vector<TextureLevel> levels
  // (level 0 first; each level holds all faces contiguously). getData()/getColorComponent() are valid
  // only for uncompressed formats.
  // TextureRender: bool createTextureLevels(const std::string& label, TextureType, GpuTextureFormat,
  //     int width, int height, int numFaces, std::span<const TextureLevel>, TextureFilter min,
  //     TextureFilter mag, TextureWrap u, TextureWrap v);
  ```
  `ColorFormat` (`Render.h:320`) stays the render-target enum.
- **D10 Editor modules** (`editor/texture/`):
  - `TextureImportSettings` (parse/serialize/resolve per family)
  - `TextureTypeInference`
  - `TextureFormatSelector` (the R5 table as data, unit-tested)
  - `TextureCooker` (decode → MipChain → encode → KTX2, banded and cancellable)
  - `TextureCookCache`
  - `TextureCookQueue` (S06 lane, memory budget)
  - `TextureCookReport` (shared by inspector, MCP, CLI and the export log)

  Encoders sit behind `ITextureEncoder` with `version()` and `isaInvariant()`.
- **D11 Determinism.** Same inputs give the same bytes at any `--jobs`. Encoders flagged non-invariant add
  their ISA to the key. bc7enc_rdo runs scalar, astcenc runs invariant, and basisu's SSE path stays off
  unless a test proves it invariant.
- **D12 HDR on mobile and web without BC6H** uses RGB9E5 (4 B/texel, sampled on every backend). ASTC HDR is
  out because sokol does not have it and Adreno support is patchy.
- **D13 Read/Write.** CPU-readable textures (R3) are never block-compressed on any family. A size warning
  shows when such a texture exceeds 1024².

## 7. Acceptance criteria

Test files are gtest under `engine/tests/texture/` and `editor/tests/texture/` (S02) unless noted.
The fixture corpus is `tools/fixtures/textures/` (CC0 or generated):
- photo albedo 2048²
- smooth gradient 1024²
- stylized albedo with alpha 1024²
- cutout foliage with alpha 1024²
- tangent normal map 2048²
- grayscale AO 1024²
- UI atlas 1024×512 with text
- pixel-art sprite 256×128
- HDR sky `.hdr` 2048×1024
- 16-bit heightmap 513²
- NPOT 1000×600
- cubemap

- **AC1** `TextureCookKey.*`: the key is stable across runs, machines and `--jobs`. Changing any R7 input
  (one source byte, each settings field, family, format, cook version, encoder version) changes the key.
  Renaming the file does not.
- **AC2** `MipChain.*`:
  - A 50% grey sRGB checker filters to linear 0.5 (sRGB 188 ± 1), not 128.
  - Normal levels have |n| = 1 ± 2e-3.
  - With `preserveCoverage` on the foliage fixture, coverage at the cutoff is within 2% of level 0 on every
    level; without it, the error is > 10% on some level, which proves the test is sensitive.
  - Raw-path and cook-path levels are byte-identical for uncompressed output.
- **AC3** `TextureFormatSelector.*` is table-driven. Every R5 cell, every alpha/opaque case, every override,
  and each of R3/R6 resolves exactly as specified.
- **AC4** `TextureEncodeQuality.*`: PSNR (RGB, mip 0, against the max-size-resized source; alpha checked
  separately with the same floor) on the corpus is at or above these floors (dB):

  | BC7 H | BC7 N (λ1) | BC1 | BC4 | ASTC 4x4 | 5x5 | 6x6 | 8x8 | 10x10 | ETC2 RGB | ETC1S q128 | UASTC λ0 → BC7/ASTC | UASTC λ1 |
  |---|---|---|---|---|---|---|---|---|---|---|---|---|
  | 42 | 40 | 34 | 42 | 42 | 39 | 36 | 32 | 29 | 34 | 31 | 41 | 39 |

  - Normal maps (BC5, EAC_RG11, ASTC 6x6, UASTC λ1): mean angular error ≤ 1.5° and p99 ≤ 6°.
  - BC6H and RGB9E5: relative RMSE ≤ 3% on [0, 16].
  - The foliage fixture after BC7 has no alpha-0 source texel decoding above 0 (endpoint repair).
  - The first implementation PR calibrates each floor to measured − 1 dB (never above these values).
    Lowering a floor later needs a spec change.
- **AC5** `TextureEncodeSize.*`: the corpus averages meet R18 and the VRAM figures meet R17.
- **AC6** `TextureCookDeterminism.*`: cooking the corpus with `--jobs 1` and `--jobs 8` (and twice) gives
  identical bytes for every family.
- **AC7** `Ktx2.*`: write/read round trip for every `GpuTextureFormat`, cubemaps, and key/values. Reading
  only the levels below the mip limit touches only their byte range (file-read spy). Malformed files
  (truncated, bad level index, size overflow) fail cleanly. Fuzz with 10k mutated files under ASan
  without a crash.
- **AC8** Backend format test `texture_formats`:
  - For every format the device reports, upload a cooked fixture, draw it with nearest sampling at each mip,
    read back, and compare with the CPU decode: ≤ 1/255 per channel (exact for uncompressed).
  - Unsupported formats take the R11 fallback and still match the CPU decode.
  - CI runs: Linux GL 3.3, Linux GLES3, Linux Vulkan, Windows D3D11, macOS Metal, and WebGL2 in headless
    Chrome through the S02 web gate.
  - Manual release check on one Android (Vulkan + GLES3) and one iOS device, recorded in the release notes.
- **AC9** `TextureFallback.*`: with capabilities faked to "no BPTC", BC7 loads as RGBA8 and logs once. A web
  run with only s3tc picks BC1/BC3. With nothing available and low-memory fallback on, it picks
  RGB565/RGBA4.
- **AC10** MCP `tools/mcp/tests/test_texture_import.py`:
  - get/set round trip with undo restoring the previous settings.
  - Invalid enum → error with the allowed values.
  - Type inference: a glTF normal slot gives `normal`.
  - `reimport_textures {wait:true}` returns cooked status for 3 families.
  - `get_texture_cook_report` totals equal the sum of the rows. `include_preview` returns a PNG.
  - A texture edited on disk is stale and then recooked (S05).
  - Tool-budget growth ≤ 2,400 characters.
- **AC11** CLI `tools/mcp/tests/test_cli_textures.py`: `cook-textures --platform all --report` on the
  fixture project exits 0 and the report validates against its schema. A second run reports 100% cache
  hits in ≤ 10 s. `--check` exits 1 on an NPOT-4 `ui` texture. `--decode` writes a PNG within the AC4 floor.
- **AC12** Export (`tools/smoke-test.sh` + `test_build.py`):
  - A web export has no `.png/.jpg` from cooked sources, contains `je-cooked/web/index.json`, and has a
    `.data` file ≥ 60% smaller than today's on the fixture project.
  - A desktop export loads and renders the fixture scene, and the golden matches within the existing
    tolerance after the R13 golden refresh.
  - A source export for Windows + Android ships `desktop` and `mobile` files, and each platform's
    packaging includes only its own family.
- **AC13** Runtime budgets: `benchmark` on the fixture scene meets R19. A forced `textureQuality=1` halves
  streamable textures' stored size and leaves `getWidth()` and UI layout unchanged (UI golden identical).
  A 128 MB budget drops mips until resident bytes ≤ 128 MB, with no sprite/ui texture affected.
- **AC14** Editor manual check (recorded with `tools/editor-shot.sh`):
  - Select the photo fixture, switch the Mobile tab to Low: the preview A/B shows 8x8 artefacts and the
    table updates within the R20 time.
  - Undo restores the settings and the table.
  - The Game view in *Simulate: Mobile* matches the preview.
- **AC15** Size and licence checks: R21 binary deltas are measured in CI (stripped Release and wasm),
  `NOTICE.md` has all five libraries plus the OpenEngine BC7 repair file, and
  `engine/libs/sokol/PATCHES.md` lists the R14 patch.

## 8. Dependencies & conflicts

- **Depends on:**
  - S01 (C++20, `std::span`)
  - S02 (gtest, CI backends, web browser gate)
  - S03 (atomic cache writes, unknown-key preservation)
  - S04 (per-asset sidecar for `textureImporter`, typed dependency graph for R2/R3)
  - S06 (background lane, priorities, cancel, memory-bounded queue)
  - S05 (reimport on change)
  - S24 (undo naming for importer edits)
  - S08 multi-edit is optional (single-select works without it).
- **Blocks / feeds:**
  - S15: the VRAM panel shows format, stored and logical size, and family per texture; the capability table.
  - S30: the prebuilt-player export must call the same export cook step; its API is
    `TextureCookQueue::cookForExport(families, progress)`.
  - S13: LUTs are CPU-readable through R3.
  - S20: flipbooks use the `sprite` type.
  - S11: shares slot→type classification with any new material slots, and owns any switch to sRGB sampling.
- **Merge hotspots:**
  - render code: `engine/renders/sokol/SokolTexture.cpp`, `engine/core/render/TextureRender.h`,
    `engine/core/texture/TextureData.*`, `engine/core/pool/TextureDataPool.cpp`, `engine/libs/sokol/sokol_gfx.h`
    (S10/S15 may patch it too)
  - shaders: `engine/shaderlib/includes/pbr.glsl` and `lighting2d.glsl` (S10/S11 edit these heavily;
    land R13 as one small isolated commit)
  - export: `editor/Exporter.cpp` (`copyAssets`), `editor/window/dialog/ExportWindow.cpp` (S30)
  - CLI and MCP: `editor/cli/CommandLine.cpp` (S31), `editor/mcp/tools/AssetTools.cpp`
  - editor windows: `editor/window/InspectorWindow.cpp` (S08), `ProjectWindow.cpp` (S25),
    `dialog/ProjectSettingsWindow.cpp`
  - build and notices: `engine/CMakeLists.txt` (S02), `NOTICE.md`

## 9. Risks & defaults

1. **Emscripten compressed-format exposure.** sokol only enables ETC2/ASTC on the web when the extension
   string is present. *Default:* rely on Emscripten's automatic extension enabling. AC8's headless Chrome run
   plus a manual Safari iOS / Firefox check gate it. If an extension is missing, D5 falls through, so
   rendering is never wrong, only larger.
2. **Normal-map decode change (R13)** slightly changes shading for maps with non-unit normals (JPEG noise).
   *Default:* accept. Refresh the affected goldens once in the R13 commit, with before/after attached to the PR.
3. **Encoder determinism and cook time.** UASTC+RDO is slow on big projects. *Defaults:* `normal` effort, the
   content cache, an optional shared global cache, and the D11 ISA keying. If the R20 UASTC time is missed,
   the web Normal default for sprite/ui falls back to ETC1S q255 (decided now).
4. **ETC1S quality on stylized art** (web default). *Default:* keep ETC1S q128 for `default` (2 bpp target);
   the inspector A/B makes artefacts visible and one click sets High/UASTC.
5. **sokol patch upkeep.** *Default:* a minimal patch tracked in `PATCHES.md` and sent upstream. Non-square
   ASTC is not added.
6. **Android ETC2 fallback doubles texture payload.** *Default:* off. Devices without ASTC are filtered by
   the manifest, and the export window warns about the size when it is enabled.
7. **Memory spikes cooking 8K/HDR sources.** *Default:* the 2 GB working-set budget in R8. HDR intermediates
   are float only for `hdr/lightmap`.
8. **Web main-thread transcode hitches** (no pthreads). *Default:* uncapped during scene load, one level per
   frame afterwards. When `EMSCRIPTEN_THREAD_SUPPORT=ON`, transcodes run on the pool instead.
9. **Lua scripts that read pixels of cooked textures.** *Default:* the R3 one-time error that names the
   Read/Write flag. `shipSource` exists for scripts that parse the raw file.

## Deviations

What the implementation does differently from the text above, and why (IMPLEMENTING.md: the best
alternative, recorded here).

1. **Settings key (R1, R30).** S04's sidecar reserves the top-level `import:` section for importers,
   so the settings live at `import.textureImporter` (`version: 1`) rather than a top-level
   `textureImporter:`. Writes go through `SetAssetMetaCmd`, which now rewrites only its own
   section and keeps S04's lines (and S25's labels) byte for byte.
2. **R1 fields added.** `shape: 2d|cube` (a single-file cross is a cube map; six files stay six 2D
   textures), `shipSource` (R9 names it without listing it), `inferredFrom` (R2's reason, written
   with the inferred type and shown in the inspector). `mipLimit` is the most levels kept (0 = all).
   `streamable` defaults to on, except for sprite/ui (R12) and textures without mips.
3. **Progressive tail (R12).** KTX2 stores levels smallest first, so the ≤ 64 px tail sits right after
   the metadata: the first read is of the file's first bytes, not its end. Same effect, one read.
4. **Sources read from their file (R9).** HTML UI images, terrain surface layers and a glTF's
   external images are read from the source file by their consumers (RmlUi, the terrain detail
   array, tinygltf). A texture whose every reference is one of those is not cooked and ships as
   it is; a texture with one such reference and others is cooked and its source ships too
   (`readsSource`, like an implicit `shipSource`). `.svg` (rasterised at load at the requested
   scale) and `.ktx2` sources load as they are. `.pic`/`.pnm` (stb formats the asset table knows)
   cook like the R1 list.
5. **R3 CPU readers.** Spot light masks join terrain height/density maps and LUTs: their pixels are
   read on the CPU. Terrain blend maps are inferred as linear data (splat weights).
6. **HDR storage (D7).** HDR textures are stored display-encoded (`pow(1/2.2)` in float) because
   every shader decodes with `sRGBToLinear()` and the raw path's stb `.hdr` → LDR conversion does the
   same; the cook's relative RMSE is measured in linear light on [0, 16] as AC4 asks.
7. **ASTC profile (D7).** ASTC is encoded with the LDR (linear) profile on sRGB data, matching the
   UNORM upload and the shader-side decode; the sRGB flag still travels in the DFD.
8. **BC1** is opaque-only (no 3-colour punch-through mode); textures with alpha use BC7 per R5.
   Opaque sprite/ui BC7 uses the opaque mode-6 encoding at every quality (their alpha is 255).
9. **R6 on the web.** Basis files transcode to BC/ETC2 on most devices, so web cooks follow the
   multiple-of-4 rule too. A rescale is a warning in the report and the inspector; only the
   sprite/ui uncompressed fallback and an explicit format that cannot hold the data are
   warning-level problems for `cook-textures --check` / `check --textures`, so a 65² height map
   does not fail a project.
10. **Key ISA (D11).** bc7enc_rdo (scalar float) and basisu are not proven ISA-invariant, so their keys
    carry the host ISA; astcenc (invariance build) and the raw writer do not. Key numbers are printed
    without the locale.
11. **sokol patch (R14).** Upstream submission is not possible from this environment; the patch is in
    `engine/libs/sokol/PATCHES.md`, marked to be offered upstream.
12. **Basis build.** The v2.50 transcoder does not compile with XUASTC off (its BC7 decoder needs it),
    so the transcoder keeps it; the other unused targets are compiled out. `.exr` sources are read
    through basisu's bundled tinyexr (editor only).
13. **Web normal maps** are UASTC with the RG channel layout and transcoded from channels 0 and 1
    (basisu's BC5/EAC defaults read R and A).
14. **Game view simulation (R26).** The family the editor renders cooked textures with is one
    setting for every view (the texture pool is shared), set from the Game view's toolbar.
15. **Reports.** MCP and the CLI report project-relative paths and add each cook's cache `key` (the
    file under `.jasperengine/cache/textures/v1`) and a `source` status for files that are not
    cooked; `check` gains a `--textures` category, on by default.
16. **Batch cooks.** The export and `cook-textures` share `TextureBatchCook` (plain threads up to
    `--jobs`, the same working-memory budget); the editor's background cooks, which MCP's
    `reimport_textures` waits on, stay on S06's Background lane (`TextureCookQueue`). Encoders always
    fork their bands on the job system.
