# Local changes to sokol_gfx.h

`sokol_gfx.h` is vendored from upstream `master` with local changes. Every changed line carries
a `// JasperEngine:` comment; the git history has the full diffs. This file lists the changes
that add API, so an update of sokol knows what to carry over and what upstream should get.

## Compressed and packed texture formats (spec S14 R14)

Cooked textures (`docs/textures.md`) need ASTC block sizes beyond 4x4 on mobile and the web, and
16-bit formats for the low-memory fallback on GL-family backends.

- `sg_pixel_format` gains `SG_PIXELFORMAT_ASTC_{5x5,6x6,8x8,10x10,12x12}_{RGBA,SRGBA}`, after
  `SG_PIXELFORMAT_ASTC_4x4_SRGBA`, and `SG_PIXELFORMAT_RGB565` / `SG_PIXELFORMAT_RGBA4`, before
  `SG_PIXELFORMAT_DEPTH` (the compressed formats stay at the end of the enum).
- `_sg_is_compressed_pixel_format` and `_sg_block_bytesize` know the new ASTC formats (16-byte
  blocks); `_sg_block_dim` returns their block size (5, 6, 8, 10, 12), so `_sg_row_pitch`,
  `_sg_num_rows` and the upload paths that use them (GL, D3D11, Metal, Vulkan staging) handle
  them. `_sg_pixelformat_bytesize` returns 2 for RGB565 and RGBA4. The WebGPU upload rounds a
  compressed mip to the format's block size instead of 4. Only square ASTC sizes are added: the
  pitch helpers take one block dimension.
- GL / GLES3 / WebGL2: the `GL_COMPRESSED_{RGBA,SRGB8_ALPHA8}_ASTC_NxN_KHR` enums (with
  `#ifndef` fallbacks), enabled with ASTC 4x4 by `KHR_texture_compression_astc_ldr` /
  `WEBGL_compressed_texture_astc` (`_sg_gl_init_pixelformats_astc`). RGB565 uploads as
  `GL_RGB` / `GL_UNSIGNED_SHORT_5_6_5` into `GL_RGB565` (`GL_RGB5` on GL 3.3 core, which has no
  `GL_RGB565` before 4.1), RGBA4 as `GL_RGBA` / `GL_UNSIGNED_SHORT_4_4_4_4` into `GL_RGBA4`;
  both are core formats, sampled and filtered. GL 3.3 core does not report RGB565 as supported:
  a driver may keep `GL_RGB5` at 5 bits of green, which no longer matches the data (the engine's
  texture format test samples it), so desktop GL takes RGBA8 where GLES and WebGL2 take RGB565.
- Metal: the `MTLPixelFormatASTC_NxN_{LDR,sRGB}` mappings, enabled on iOS with ASTC 4x4.
- Vulkan: the `VK_FORMAT_ASTC_NxN_{UNORM,SRGB}_BLOCK` mappings; support is queried per format with
  `vkGetPhysicalDeviceFormatProperties` like every other format.
- WebGPU: the `WGPUTextureFormat_ASTCNxN*` mappings, enabled by `texture-compression-astc`.
- RGB565 and RGBA4 have no mapping on D3D11, Metal, Vulkan and WebGPU, which report them
  unsupported (`sg_query_pixelformat`), as the engine expects (it picks them only where they sample).

Status: to be offered upstream (floooh/sokol) as one pull request with the diff of the lines marked
`(PATCHES.md)`; until it merges, every sokol update re-applies it.
