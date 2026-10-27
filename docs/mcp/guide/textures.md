# Textures

Texture import settings and cooks for agents: what the settings do, how to check a cook. The
user reference (formats per platform, the cache, the runtime) is `docs/textures.md`.

Every texture source (png, jpg, tga, bmp, psd, gif, hdr, exr) is cooked per platform family to a
GPU format: **desktop** (Windows, Linux, macOS: BC1/BC4/BC5/BC6H/BC7), **mobile** (Android, iOS:
ASTC), **android_etc2** (Android without ASTC, when Project Settings > Texture > Android ETC2
Fallback is on) and **web** (Basis Universal ETC1S / UASTC, transcoded at load). The editor cooks
the desktop family in the background and renders with it once it lands (the source until then).
Exports cook their families and ship `assets/je-cooked/<family>/`, not the sources. `.svg` and
`.ktx2` files load as they are.

The tools are in the `assets_pipeline` group: `tool_groups {enable: ["assets_pipeline"]}`.

## Settings

`get_texture_import_settings {path}`: `stored` (null until written), `inferred` (the type the
references give, its `reason`, `cpu_readable`), `resolved` per platform, `warnings`.

When a texture first gets settings its type is inferred from what references it, and written to
its `.meta` (`import.textureImporter`): a normal slot (material or glTF `normalTexture`, a shader's
`@texture ... normal`) gives `normal`; sprites, tilemaps, particles `sprite`; UI images `ui`;
occlusion slots `single_channel`; `.hdr` / `.exr` `hdr`; otherwise `default`. A reference that
does not fit the stored type is a warning (`warnings`, the Inspector, `check`).

`set_texture_import_settings {paths, settings, platform?}` (one undo step; the cooks requeue):

| Setting | Values |
|---|---|
| `type` | default, normal, sprite, ui, lightmap, single_channel, hdr |
| `shape` | 2d, cube (a 4:3 / 3:4 cross in one file) |
| `srgb` | colour (true) or data (false); the type's default otherwise |
| `alpha_source`, `alpha_is_transparency` | none, input, from_gray; fill transparent texels' colour |
| `readable` | keep the pixels CPU-readable (uncompressed): Lua `getData`, custom readers |
| `max_size`, `resize_filter` | 32..16384 (a power of two, default 2048); box, triangle, mitchell, catmull_rom |
| `mipmaps`, `mip_filter`, `mip_limit` | mips on/off (sprites and UI default off), their filter, the most levels kept |
| `preserve_coverage`, `alpha_cutoff` | keep alpha-tested foliage as thick in every mip; 0..1 |
| `flip_green` (normal), `channel` (single_channel) | DirectX-style normal maps; r, g, b, a, luma |
| `quality`, `effort` | none (uncompressed), low, normal, high; fast, normal, best |
| `streamable`, `ship_source` | the quality setting and the budget may drop its top mips; exports keep the source too |

With `platform` (desktop, mobile, android_etc2, web), the settings are that platform's override:
`override` (true unless given), `max_size`, `format` (a format name, `ETC1S`, `UASTC`,
`UASTC_HDR`), `quality`, `effort`, `rdo` (BC7 / UASTC lambda, ETC1S level 1..255).

Formats at Normal quality: opaque colour BC1 / ASTC 6x6 / ETC2_RGB8 / ETC1S q128; colour with
alpha BC7 (RDO 1) / ASTC 6x6 / ETC2_RGBA8 / ETC1S q128; normal maps BC5 / ASTC 6x6 / EAC_RG11 /
UASTC RDO 1 (two channels everywhere: custom shaders call `decodeNormalMap(texture(...))`);
single channel BC4 / ASTC 8x8 / EAC_R11 / ETC1S q255; HDR BC6H / RGB9E5 / RGB9E5 / UASTC HDR. BC
and ETC2 need sizes that are multiples of 4: other textures are rescaled, sprites and UI stay
uncompressed (a warning). Terrain height and density maps, LUTs and spot light masks are
always cooked uncompressed (they are read on the CPU).

## Cooks

- `reimport_textures {paths? (all), platforms? (desktop), wait?, timeout_s?}` cooks again and
  returns the report. The cache answers unchanged inputs at once.
- `get_texture_cook_report {paths?, platforms?, include_preview?}`: per texture and platform
  `status` (cooked, queued, cooking, stale, failed with `error`, source = read from its file),
  `format`, `width`/`height` (stored) and `logical_width`/`logical_height` (the source's), `mips`,
  `vram_bytes`, `disk_bytes`, `source_bytes`, `psnr_db` (normals: `normal_mean_error_deg`; HDR:
  `hdr_relative_rmse`), `cache_hit`, `warnings`; `totals` per platform. `include_preview` adds the
  cooked images of the first four as the platform's GPUs show them.
- A source edited on disk goes stale and cooks again by itself.

CLI: `jasperengine-editor cook-textures --project P --platform all --report out.json` cooks into
the cache and prints the report; `--check` exits 1 on a failed cook or a warning-level problem;
`--decode in.ktx2 out.png [--level N] [--platform-target BC7]` decodes a cooked file.
`check --textures` lists the problems without cooking.

## Runtime

Lua: `Engine.setTextureQuality(0..3)` drops top mips of streamable textures (Unity's Global
Mipmap Limit), `Engine.setTextureMemoryBudget(mb)` caps resident textures,
`Engine.isTextureFormatSupported("ASTC_6x6")`; a `Texture`'s `width`/`height` stay the source's
size (layout never moves), `getStoredWidth()`, `isCompressed()`, `getGpuFormat()`. `getData()`
on a compressed texture is empty: set `readable`. Project Settings > Texture holds the per
platform defaults the export applies. The Game view's texture button renders with another
family's cooks (decoded on the CPU when this GPU cannot sample them).
