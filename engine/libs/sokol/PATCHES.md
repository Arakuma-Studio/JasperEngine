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

## Asynchronous creation (spec S32)

Shaders and pipelines whose driver work runs off the render thread. The backend code is not in
`sokol_gfx.h`: `sokol_async.h` / `sokol_async_impl.h` (included by `sokol.cpp` after
`sokol_gfx.h`) split creation where sokol's own `_sg_init_shader` / `_sg_init_pipeline` already
split it (defaults and validation, `_sg_create_*`, the slot state). Every changed line in
`sokol_gfx.h` carries `JasperEngine: async creation` (`SokolPatchMarkers.AsyncCreationHooks`
counts them):

- GL `_sg_gl_create_shader`: a program already in `shd->gl.prog` (compiled and linked by
  sokol_async on the render context, with `KHR_parallel_shader_compile` or a deferred status read)
  is adopted. Its compile and link are skipped, and the link status and the uniform resolution
  run as before. Upstream asserts the field is 0.
- Vulkan: `static VkPipelineCache _sgj_vk_pipeline_cache` (`VK_NULL_HANDLE` unless sokol_async made
  one) is the cache of `vkCreateGraphicsPipelines` and `vkCreateComputePipelines`. Upstream passes
  `VK_NULL_HANDLE`.
- Metal: three forward-declared hooks (defined in `sokol_async_impl.h`):
  - `_sg_mtl_create_shader_func` takes a library compiled by `newLibraryWithSource:...
    completionHandler:` when there is one;
  - `_sg_mtl_create_pipeline` hands its render pipeline descriptor to sokol_async and returns
    `SG_RESOURCESTATE_ALLOC` in the capture phase, and takes the state the completion handler made
    when it is called again to finish.

  With sokol_async idle the hooks return nil/false and nothing changes.

Status: JasperEngine-specific (upstream has no asynchronous creation API); every sokol update
re-applies the marked lines.

## A lock on the resource pools

A thread with a command queue of its own (`SokolCmdQueue`, the editor's thumbnail strand, a game's
`Engine.startAsyncThread`) allocates the handles of the GPU objects it asks for with `sg_alloc_*`,
when the main thread may be running `sg_make_*` or `sg_destroy_*`: the frames' semaphore keeps it
off the frames, not off what the main thread frees outside one. Upstream's pools are not thread
safe. Every changed line in `sokol_gfx.h` carries `JasperEngine: pool lock`
(`SokolPatchMarkers.PoolLock` counts them):

- `static std::mutex _sgj_pool_mutex`, and `_SGJ_POOL_LOCK()` taking it (`sokol.cpp` includes
  `<mutex>`).
- `_sg_alloc_{buffer,image,sampler,shader,pipeline,view}` and `_sg_dealloc_*` hold it for their whole
  body: a free list's top and queue, a slot's id and state, and the `allocated` and `deallocated`
  counters. A dealloc frees the index before it resets the slot, so a thread allocating has to wait
  for both.
- `_sg_update_stats` and `sg_query_stats` hold it: they read those counters and the free lists' tops.
- `sg_destroy_*` no longer assert the slot's state after the dealloc: from there the slot may be the
  other thread's.

A draw takes no lock: a resource's lookup reads only the slot of a handle the caller owns.

Status: local. Upstream's single-thread contract is intended; nothing to offer.

## Flattened uniform arrays sized as the driver reports them

On GL, GLES3 and WebGL2 the engine declares a uniform block as one array of vec4 (or ivec4) and
uploads it with one `glUniform*v(location, count)` (`SokolShader.cpp`, `glsl_uniforms[0]` with
`array_count = size / 16`). A driver may size that array to the elements the linked program reads
(a variant that leaves the block's tail unused: the built-in Lit's `u_fs_texCoordSets` without a
normal map) or report it as no array; a declared count above that is clamped by some drivers and
rejected with `GL_INVALID_OPERATION` by others, the whole block with it (Doriax `ebd4b64e7`).
Every changed line carries `JasperEngine: flattened uniform arrays` or the comment above it:

- `_sg_gl_create_shader` (uniform resolution): for a uniform of more than one element found in the
  program, `glGetUniformIndices` and `glGetActiveUniformsiv(GL_UNIFORM_SIZE)`; a smaller active size
  becomes the count uploaded. The offsets keep the declared layout. Both functions are in the
  Windows GL loader's list (`_SG_XMACRO`), with `GL_UNIFORM_SIZE` / `GL_INVALID_INDEX` fallbacks.
- `sgj_query_gl_uniform_count(shader, ub_slot, u_index)`: the count a GL backend uploads, -1 on the
  other backends (the engine's test `SokolGlUniformArray` reads it).

Status: to be offered upstream (the clamp; the query is for the tests).

## Invariant positions on Metal

The engine's vertex shaders declare `invariant gl_Position` so a depth pre-pass, the shadow passes
and the colour pass put a vertex at the same depth, and SPIRV-Cross writes it as
`[[position, invariant]]`. Metal honours that only in a library compiled with
`MTLCompileOptions.preserveInvariance`; with `options:nil` an instanced batch (its matrices read from
a vertex buffer) and the same draw alone (from a uniform block) reached different depths in 11,436
pixels of `InstancingGoldenGpu.ShadowAndDepthPassesBatch`, none with it.

- `_sg_mtl_compile_options()` (once, never released) sets `preserveInvariance`; `_sg_mtl_compile_library`
  and sokol_async's `newLibraryWithSource:options:completionHandler:` pass it instead of `nil`. Fast math
  stays on. Marked `JasperEngine: invariant position`.

Status: to be offered upstream (any sokol user with `invariant` positions has the same problem).

## GL 1.1 calls for the Jasper add-ons on Windows

`sokol_gpu_timer_impl.h` and `sokol_async_impl.h` call `glFlush` and `glGetString(GL_RENDERER /
GL_VERSION)`. On Windows with `SOKOL_GLCORE`, sokol_gfx declares GL through its own loader
(`_SOKOL_USE_WIN32_GL_LOADER`), which knows only the functions and enums sokol_gfx itself uses, so
the add-ons did not compile with MSVC.

- The loader's X-macro list gains `glFlush` and `glGetString`; both are GL 1.1, so
  `_sg_gl_getprocaddr` finds them through its `GetProcAddress(opengl32.dll)` fallback.
- The loader's enum block gains `GL_VERSION` and `GL_RENDERER`.

Status: local; upstream has no use for them. Every sokol update re-applies it.
