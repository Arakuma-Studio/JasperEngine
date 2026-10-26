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
or RGBA8 with Zstandard. Cube maps that light the scene (IBL) also get a 256 px RGBA8 copy.
Files only read from their source by their references (HTML UI images, terrain surface layers,
a glTF's external images) are not cooked; when some reference reads the source, the export ships
it next to the cooks.

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
  mip limit never read. Basis files transcode on a worker (on the main thread, one level a frame,
  in single-threaded web builds).
- **Fallbacks**: a desktop GPU without a cooked BC format gets it decoded on the CPU (bcdec);
  the web transcodes to the first format its browser samples: ETC1S colour to ETC2 > BC1/BC3 >
  ASTC 4x4 > BC7, UASTC colour to ASTC 4x4 > BC7 > ETC2 > BC3/BC1, normal maps to BC5 > EAC RG11 >
  ASTC 4x4 > RG8, single channel to BC4 > EAC R11 > ASTC 4x4 > R8, HDR to BC6H > RGB9E5 > RGBA16F,
  then uncompressed; Android without ASTC loads `android_etc2` when the export carries it. With the
  low-memory fallback on (web by default) the last resort is RGB565 / RGBA4444. The first
  fallback of each format logs once.
- **Quality and budget**: `Engine.setTextureQuality(0..3)` drops the top mips of streamable
  textures; `Engine.setTextureMemoryBudget(mb)` (desktop 0 = unlimited, mobile 512, web 384 by
  default) drops one more mip from the largest streamable texture until the resident bytes fit
  (a texture keeps at least 64 px), re-planned on load and unload. Sprites and UI never shrink.
- **Sizes**: `Texture.width` / `height` stay the source's (layout never moves);
  `getStoredWidth()` / `getStoredHeight()` are the GPU image's.
- **CPU data**: `Texture:getData()` of a compressed texture is empty and logs once.

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

## Libraries

Zstandard 1.5.7 (decoder in every game, encoder in the editor), bcdec, Basis Universal 2.50 (the
web's transcoder; the encoder in the editor), astc-encoder 5.7.0 and bc7enc_rdo (editor only),
under `engine/libs`, recorded in `NOTICE.md`. The sokol patch adding ASTC 5x5..12x12 and RGB565 /
RGBA4 is `engine/libs/sokol/PATCHES.md`.
