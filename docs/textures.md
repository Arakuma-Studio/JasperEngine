# Textures: import settings, cooks and the runtime

How JasperEngine turns texture files into what each platform's GPU samples (spec
`docs/specs/S14-texture-compression.md`). Agents: `docs/mcp/guide/textures.md`.

## The pipeline

1. **Sources** stay what they are in the project: png, jpg, tga, bmp, psd, gif, hdr, exr (and svg
   and ktx2, which load as they are, never cooked). Scenes, materials and scripts keep naming
   the source path.
2. **Import settings** live in the source's sidecar, `<file>.meta`, under
   `import.textureImporter` (`version: 1`). A texture without them gets its type inferred from
   its references the first time the editor opens the project, and that is written down once.
3. **Cooks**: per platform family, the source is decoded, prepared (alpha source, channel, green
   flip, Max Size, the project's texture strategy, the block-size rule), given a mip chain
   (`engine/core/texture/MipChain.cpp`), encoded and written as a KTX2 file into the cache
   `.jasperengine/cache/textures/v1/<k[0:2]>/<k>.ktx2` with a `<k>.json` record.
4. **Exports** cook the families their platforms load and ship them as
   `assets/je-cooked/<family>/<k>.ktx2` with `index.json`; the sources they replace stay out.
5. **The runtime** looks a texture path up in its family's `index.json`; anything not there (user
   files, svg, ktx2, files only read from their source) loads raw.

## Families and formats

| family | platforms | formats |
|---|---|---|
| `desktop` | Windows, Linux, macOS | BC1, BC4, BC5, BC6H, BC7 |
| `mobile` | iOS, Android | ASTC 4x4 ... 12x12, RGB9E5, RGBA16F |
| `android_etc2` | Android devices without ASTC (Project Settings > Texture > Android ETC2 Fallback) | ETC2 RGB8/RGBA8, EAC R11/RG11, RGB9E5 |
| `web` | HTML5 | Basis Universal ETC1S, UASTC, UASTC HDR, transcoded at load to what the browser samples |

The format comes from the type, the quality, whether the texture has alpha and the family:

| type | desktop L / N / H | mobile L / N / H | android_etc2 | web L / N / H |
|---|---|---|---|---|
| default, opaque | BC1 / BC1 / BC7 | 8x8 / 6x6 / 4x4 | ETC2_RGB8 | ETC1S q64 / ETC1S q128 / UASTC RDO 1 |
| default, alpha | BC7 RDO 2 / BC7 RDO 1 / BC7 | 8x8 / 6x6 / 4x4 | ETC2_RGBA8 | ETC1S q64 / ETC1S q128 / UASTC RDO 1 |
| normal (RG) | BC5 | 8x8 / 6x6 / 5x5 | EAC_RG11 | UASTC RDO 2 / 1 / 0 |
| sprite, ui | BC7 RDO 2 / 0.5 / 0 | 8x8 / 6x6 / 4x4 | ETC2_RGB8 / RGBA8 | ETC1S q128 / UASTC RDO 1 / UASTC |
| single_channel | BC4 | 10x10 / 8x8 / 6x6 | EAC_R11 | ETC1S q128 / ETC1S q255 / UASTC RDO 1 |
| hdr, float lightmaps | BC6H | RGB9E5 / RGB9E5 / RGBA16F | RGB9E5 | UASTC HDR 4x4 |

Quality `none` keeps the uncompressed equivalent (RGBA8, RG8, R8, RGBA16F). UASTC and every raw
payload are Zstandard-supercompressed (level 9 / 15 / 19 for effort fast / normal / best);
ETC1S uses BasisLZ. A platform override can name any format. BC and ETC2 need multiples of 4 at
mip 0: default, normal, single-channel and HDR textures are rescaled to the nearest ones, sprites
and UI stay uncompressed (with a warning).

Normal maps are stored with two channels on every platform. The built-in shaders rebuild Z with
`decodeNormalMap(vec4)` (`engine/shaderlib/includes/normal_map.glsl`); custom shaders sampling a
normal map should call it too.

Textures the CPU reads are never block-compressed: terrain height and density maps, colour
grading LUTs, spot light masks and anything marked Read/Write cook to R16 (16-bit sources), R8
or RGBA8 with Zstandard. When a terrain or a detail layer starts reading a texture, the editor turns
its Read/Write on (and says so in the Console), so it is cooked again CPU-readable even if it was
cooked as a colour texture before. Cube maps that light the scene (IBL) also get a 256 px RGBA8 copy.
Files only read from their source by their references (HTML UI images, terrain surface layers)
are not cooked; when some reference reads the source, the export ships it next to the cooks. A
glTF's external images are cooked by the slots its materials sample them with (normal, occlusion,
metallic-roughness, colour) and the model loads the cooks, so they ship cooked only; images
embedded in a `.glb` or a data URI stay in the model.

## Import settings

| field | values | notes |
|---|---|---|
| `type` | default, normal, sprite, ui, lightmap, single_channel, hdr | inferred: normal slots, sprites/tilemaps/particles, UI images, occlusion slots, .hdr/.exr |
| `shape` | 2d, cube | cube: one file holding a 4:3 or 3:4 cross |
| `srgb` | bool | colour vs data; normal, single_channel, lightmap and hdr default to data |
| `alphaSource` | input, from_gray, none | |
| `alphaIsTransparency` | bool | fills transparent texels' colour from their neighbours (sprites, UI) |
| `readable` | bool | Read/Write: uncompressed, CPU-readable |
| `maxSize`, `resizeFilter` | 32..16384 (powers of two, 2048); box, triangle, mitchell, catmull_rom | |
| `mipmaps`, `mipFilter`, `mipLimit` | bool; filter; the most levels kept (0 = all) | sprites and UI default to no mips |
| `preserveCoverage`, `alphaCutoff` | bool, 0..1 | every mip keeps level 0's alpha-tested coverage within 2% |
| `flipGreen` | bool | normal maps authored Y-down (DirectX) |
| `channel` | r, g, b, a, luma | the channel a single-channel texture keeps |
| `quality`, `effort` | none, low, normal, high; fast, normal, best | |
| `streamable` | bool | the texture quality setting and the memory budget may drop its top mips |
| `shipSource` | bool | exports keep the source file next to the cooks |
| `platforms.<family>` | `override`, `maxSize`, `format`, `quality`, `effort`, `rdo` | applied when `override` is on |

Unknown keys are kept. The Inspector edits them (select texture files in the Project window):
the preview's Source / Cooked split decodes the chosen platform's cook on the CPU, the table
shows each platform's format, VRAM, disk size, PSNR and status, and Apply is one undo step for
every selected texture.

## Cache and cooks in the editor

The key of a cook is a SHA-256 over the source's content hash, the resolved settings of the
family, the family, the format choice, the cook version, the encoder's version and, for encoders
not proven ISA-invariant (bc7enc_rdo and Basis Universal), the host ISA. Renaming a file never
recooks it; identical files share a cook. The source table (path, size, mtime -> hash) spares
rehashing. The cache is capped (Project Settings > Texture > Cache Size, 4 GB, least recently used
first) and may be a directory every project shares (Global Cache Directory).

The editor cooks the desktop family on the background job lane: the selected texture first,
then those the open scenes load, then the rest. Concurrent cooks share a 2 GB working-memory
budget. Results are byte-identical at any thread count: encoders work in fixed bands of 16 block
rows. A texture renders from its source (with its import processing) until its cook lands, then
swaps in place. A source change on disk or a settings edit cancels the texture's cooks and
queues them again. The status bar shows `Cooking textures 12/40` with Cancel; the Project
window badges queued, cooking and failed textures.

## The runtime

- **Loads** read the KTX2's metadata and its mip tail (the levels of at most 64 px, at the start
  of the file) first and upload those, then read the rest: one ranged read each, levels above the
  mip limit never read. Basis files transcode on a worker (the job system's, `web-threads` builds
  included); single-threaded web builds transcode on the main thread, everything while the scenes
  load, then in 4 ms slices of each frame (UASTC in bands of 32 block rows, ETC1S a level at a
  time).
- **Fallbacks**: a desktop GPU without a cooked BC format gets it decoded on the CPU (bcdec);
  the web transcodes to the first format its browser samples: ETC1S colour to ETC2 > BC1/BC3,
  UASTC colour to ASTC 4x4 > BC7 > ETC2 > BC3/BC1, normal maps to BC5 > EAC RG11 > ASTC 4x4
  (UASTC) > RG8, single channel to BC4 > EAC R11 > ASTC 4x4 (UASTC) > R8, HDR to BC6H > RGB9E5 >
  RGBA16F, then uncompressed (every WebGL2 device with ASTC or BPTC has ETC2 or S3TC too, so ETC1S
  has no ASTC or BC7 entry and the web transcoder carries no tables for them); Android without ASTC loads `android_etc2` when the export carries it. With the
  low-memory fallback on (web by default) the last resort is RGB565 / RGBA4444. The first
  fallback of each format logs once.
- **Quality and budget**: `Engine.setTextureQuality(0..3)` drops the top mips of streamable
  textures; `Engine.setTextureMemoryBudget(mb)` (desktop 0 = unlimited, mobile 512, web 384 by
  default) drops one more mip from the largest streamable texture until the resident bytes fit
  (a texture keeps at least 64 px), re-planned on load and unload. Sprites and UI never shrink.
- **Sizes**: `Texture.width` / `height` stay the source's (layout never moves);
  `getStoredWidth()` / `getStoredHeight()` are the GPU image's.
- **CPU data**: `Texture:getData()` of a compressed texture is empty and logs once.
- **Late images**: a material whose image has no GPU texture yet and nothing loading it (no pixels
  pooled, no generator) binds a stand-in (white, a flat normal) and binds again once an image can
  be made (pixels pooled or replaced, a generator registered, a GPU texture made:
  `TexturePool::getReadyEpoch`), in every slot (the five core maps, the extension lobe maps, a
  shader's own textures). An image whose pixels are replaced (a hot reload, a cook swapped in, the
  rest of a progressive load) is bound again by every mesh, UI image, Points, sky and terrain holding
  it (`TexturePool::getRebindEpoch`); the old one draws until the new one is made. A model's embedded images are made again from the model once their pixels
  were freed after the upload; a preview's (a thumbnail's 256 px load) are keyed apart
  (`|max256`) and never share or replace the scene's. A sky's image lights the scene too: replaced
  pixels make its environment maps again (the maps' ids carry the image's generation). So does a
  static reflection probe's: its image has no GPU texture to go stale, so a probe looks at the
  generation when `TextureDataPool::getReplaceEpoch` moves (no lookup in a steady frame), and its old
  maps draw until the new ones are made. A cook swapped in that the CPU cannot read keeps both's maps.
- **Terrain layers** are read from their files into the terrain's array, not through the pools, so
  the file pick-up asks for the array again (`reload::reloadTextures`,
  `RenderSystem::invalidateTerrainDetailArray`), once the file was read: a broken file leaves what
  the terrain draws.
- **A lobe's map** samples the mesh's UVs as the core maps do: a variant that binds one takes the UV
  sets whatever else the material has (`RenderSystem::getSubmeshShaderVariant`), and a mesh with no
  UVs draws the lobe's factors alone (`includes/pbr_ext_slots.glsl` has no slot then).
- **A core map alone** (metallic-roughness, normal, occlusion, emissive, no base colour) gets the UVs
  under whatever lights the surface: a scene light, or a sky or a probe alone
  (`RenderSystem::checkPBRTextures`).
- **Threads**: a model may load off the main thread next to the frames (the thumbnail strand, a
  game's `Engine.startAsyncThread`). `TextureDataPool` and `TexturePool` keep their maps under a lock
  of their own, held around the containers' operations alone; it costs nothing a frame reaches: a
  steady frame makes no lookup in either, a binding a handful (`TextureDataPoolBench.Lookup`, 2000 ids: 159-163 ns
  a lookup without a lock, 164-174 ns with one uncontended; 100 meshes of 3 images each make 103 lookups
  in a frame replacing one image). The GPU objects stay the frame's: that thread makes them
  only inside `Engine::executeSceneOnce`, under the frames' semaphore.

## Command line

```
jasperengine-editor cook-textures --project P [--platform desktop|mobile|android_etc2|web|all] [--paths ...]
                                  [--report out.json] [--jobs N] [--check]
jasperengine-editor cook-textures --decode in.ktx2 out.png [--level N] [--platform-target BC7]
jasperengine-editor check --project P --textures
```

## Packaging

Each platform's packaging carries its family only: native builds drop the other families when
they copy the assets, the web's `--preload-file` excludes them, Android's Gradle project syncs the
assets without `desktop` and `web`, and the Xcode workspace's targets remove the families their
platform does not load (`tools/textures/xcode_add_texture_libs.py`). An Android manifest
declares `GL_KHR_texture_compression_astc_ldr` unless the ETC2 fallback is on.

## Measurements

Measured on the benchmark server (4 dedicated AMD EPYC-Milan vCPUs, Linux, GL 3.3 on Mesa
llvmpipe for the uploads) with `JASPER_TEST_PERF_SLACK=1 UNIT_TEST_ARGS='--gtest_filter=
TextureLoadBench*:TextureCookBench*:TextureEncodeSize*:TextureEncodeQuality*' tools/build.sh
unit-tests`: the generated corpus of `editor/tests/support/TextureCorpus.cpp`, Normal quality,
bits per mip-0 texel with the mips.

Per texture and family (recorded benchmark run):

| texture | family | format | VRAM bpt | disk bpt | quality |
|---|---|---|---|---|---|
| photo 2048² | desktop | BC1 + Zstd | 5.33 | 3.82 | 45.06 dB |
| photo 2048² | mobile | ASTC 6x6 + Zstd | 4.76 | 4.37 | 47.70 dB |
| photo 2048² | android_etc2 | ETC2 RGB8 + Zstd | 5.33 | 2.84 | 40.36 dB |
| photo 2048² | web | ETC1S q128 | 5.33 | 1.22 | 39.94 dB |
| gradient 1024² | desktop | BC1 + Zstd | 5.33 | 2.30 | 48.70 dB |
| gradient 1024² | mobile | ASTC 6x6 + Zstd | 4.78 | 1.53 | 49.72 dB |
| gradient 1024² | android_etc2 | ETC2 RGB8 + Zstd | 5.33 | 1.28 | 40.23 dB |
| gradient 1024² | web | ETC1S q128 | 5.33 | 0.35 | 37.40 dB |
| ui 1024×512 | desktop | BC7 RDO 0.5 + Zstd | 8.00 | 0.11 | 55.71 dB |
| ui 1024×512 | mobile | ASTC 6x6 + Zstd | 3.59 | 0.12 | 58.63 dB |
| ui 1024×512 | android_etc2 | ETC2 RGBA8 + Zstd | 8.00 | 0.14 | 41.55 dB |
| ui 1024×512 | web | UASTC RDO 1 + Zstd | 8.00 | 0.18 | 60.75 dB |
| foliage 1024² | desktop | BC7 RDO 1 + Zstd | 10.67 | 4.50 | 39.46 dB |
| foliage 1024² | mobile | ASTC 6x6 + Zstd | 4.78 | 3.75 | 43.87 dB |
| foliage 1024² | android_etc2 | ETC2 RGBA8 + Zstd | 10.67 | 4.06 | 39.15 dB |
| foliage 1024² | web | ETC1S q128 | 10.67 | 1.66 | 38.63 dB |
| normal 2048² | desktop | BC5 + Zstd | 10.67 | 9.81 | 0.36° mean |
| normal 2048² | mobile | ASTC 6x6 + Zstd | 4.76 | 4.29 | 0.86° mean |
| normal 2048² | android_etc2 | EAC RG11 + Zstd | 10.67 | 7.32 | 0.82° mean |
| normal 2048² | web | UASTC RDO 1 + Zstd | 10.67 | 5.37 | 1.13° mean |
| mask 1024² | desktop | BC4 + Zstd | 5.33 | 5.10 | 50.36 dB |
| mask 1024² | mobile | ASTC 8x8 + Zstd | 2.67 | 2.34 | 46.21 dB |
| mask 1024² | android_etc2 | EAC R11 + Zstd | 5.33 | 4.71 | 50.28 dB |
| mask 1024² | web | ETC1S q255 | 5.33 | 1.60 | 42.94 dB |
| albedo 1024² | desktop | BC1 + Zstd | 5.33 | 2.24 | 46.40 dB |
| albedo 1024² | mobile | ASTC 6x6 + Zstd | 4.78 | 3.75 | 48.09 dB |
| albedo 1024² | android_etc2 | ETC2 RGB8 + Zstd | 5.33 | 1.82 | 40.88 dB |
| albedo 1024² | web | ETC1S q128 | 5.33 | 0.91 | 41.33 dB |
| sky 2048×1024 (HDR) | desktop | BC6H + Zstd | 10.67 | 4.32 | 0.0030 rel. RMSE |
| sky 2048×1024 (HDR) | mobile, android_etc2 | RGB9E5 + Zstd | 42.67 | 5.50 | 0.0014 rel. RMSE |
| sky 2048×1024 (HDR) | web | UASTC HDR 4x4 + Zstd | 10.67 | 4.46 | 0.0027 rel. RMSE |

Corpus averages against the budgets (`TextureEncodeSize`, `TextureEncodeQuality`, 512² corpus):

| | disk bpt (R18) | VRAM bpt (R17) | PSNR (AC4 floor) |
|---|---|---|---|
| desktop BC1 | 2.99 (≤ 4.8) | 5.33 (5.33) | 46.26 dB (34) |
| desktop BC7 RDO λ1 + zstd | 2.60 (≤ 7.5) | 10.67 (10.67) | 44.39 dB (40) |
| mobile ASTC 6x6 + zstd | 3.53 (≤ 4.6) | 4.83 (4.74, deviation 19) | 48.12 dB (36) |
| web ETC1S q128 | 1.05 (≤ 2.0) | 5.33 | 39.64 dB (31) |
| web UASTC λ1 + zstd | 4.62 (≤ 6.0) | 10.67 | 47.16 dB (39) |
| web normal UASTC λ1 | 5.72 (≤ 6.0) | 10.67 | 1.12° mean, 3.19° p99 |

Other floors: BC7 High 52.67 dB (42), ASTC 4x4 / 5x5 / 8x8 / 10x10 54.35 / 50.73 / 45.36 /
42.56 dB (42 / 39 / 32 / 29), ETC2 RGB 40.46 dB (34), BC4 50.42 dB (42), UASTC λ0 transcoded to
BC7 / ASTC 4x4 52.44 / 54.71 dB (41); normals BC5 0.36°, EAC RG11 0.82°, ASTC 6x6 0.86° mean
(≤ 1.5°); BC6H and RGB9E5 0.90% and 0.14% relative RMSE (≤ 3%).

Times (R19, R20):

| | measured | budget |
|---|---|---|
| load of a 2048² BC7 KTX2 with mips: loader thread (read + zstd) | 3.29 ms | 12 ms |
| the same: main-thread upload | 0.342 ms (0.33–0.43 over runs) | 0.5 ms |
| PNG decode + mip generation + upload of the same image | 50.5 ms (13.9× the KTX2) | ≥ 3× |
| cold 2048² cook, desktop BC1 | 1.10 s | 3 s |
| cold 2048² cook, mobile ASTC 6x6 (normal effort) | 1.30 s | 6 s |
| cold 2048² cook, web UASTC λ1 | 9.57 s | 12 s |
| warm cook (cache hits) of 200 textures | 0.01 s | 10 s |
| web (headless Chrome, one thread): 2048² UASTC λ1 with its mips to BC7 | 35.5 ms | 40 ms |
| the same to ASTC 4x4 | 33.2 ms | 40 ms |
| web: the transcoder's first-use set-up (once, the first Basis file) | 45.6 ms | |
| web: the longest 32-block-row band of a 2048² level to BC7 (a main-thread slice) | 1.8 ms | |

The web rows run in the texture-formats project's page (`?transcode_bench=1`, a Release web build)
in headless Chrome on the same server: the bench's 2048² chain tiles the blocks of
`assets/bench/uastc-photo-512.ktx2`, a 512² UASTC λ1 cook of the corpus photo (UASTC blocks
transcode independently, so the work per block is a 2048² photo's), median of 15 runs. A budget
missed is an error line; `?transcode_bench=report` (CI, the build servers) prints the figures
without a verdict:

```sh
BUILD_TYPE=Release WEB_PROJECT=texture-formats tools/build.sh web
GATE=texture-formats GATE_ARGS='--query=transcode_bench=1 --expect=transcode_web_budgets=pass --seconds 15' tools/build.sh web-gate
```

Sizes (R21, `tools/textures/runtime_size.py`): a linked Release game carries 97.3 KB of Zstandard's
decoder (73.5), bcdec (9.8) and the KTX2 reader (14.0), budget 120 KB, and 31.3 KB more of CPU
decodes, fallback plans and the cooked index. The web build adds 232.6 KB against R21's 250 KB
(823 KB before S14b): Basis Universal's transcoder 145.2 KB (the D5 paths only, at `-O2`;
`engine/libs/basisu/README.jasper.md`), Zstandard 37.3 KB and the texture runtime 50.1 KB (both at
`-Os`); spec S14 deviation 23 has what was taken out. The limit is 240 KB.

## Libraries

Zstandard 1.5.7 (decoder in every game, encoder in the editor), bcdec, Basis Universal 2.50 (the
web's transcoder; the encoder in the editor), astc-encoder 5.7.0 and bc7enc_rdo (editor only),
under `engine/libs`, recorded in `NOTICE.md`. The sokol patch adding ASTC 5x5..12x12 and RGB565 /
RGBA4 is `engine/libs/sokol/PATCHES.md`.
