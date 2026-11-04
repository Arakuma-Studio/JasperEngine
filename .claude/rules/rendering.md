---
paths:
  - "engine/renders/**"
  - "engine/shaders/**"
  - "engine/shaderlib/**"
  - "engine/core/render/**"
---

# Rendering

- Five backends through sokol-gfx: GL 4.1 core, GLES3/WebGL2, Metal, D3D11, Vulkan. A change
  must compile and draw the same on all of them; GLSL is cross-compiled per backend, so avoid
  constructs one of the targets lacks.
- No compute shaders, SSBOs, bindless resources, indirect draws or 64-bit atomics. Use
  fragment-shader and CPU (job system) solutions; a desktop-only fast path needs an equivalent
  fallback.
- A disabled effect adds no pass, no draw and no allocation.
- Rendering changes come with golden images: `tools/build.sh htmlui-tests` and the core GPU
  suites of `tools/build.sh unit-tests` (`JASPER_TEST_UPDATE_GOLDEN=1` rewrites, glcore only;
  review every rewritten image). Check Vulkan too with `GRAPHIC_BACKEND=vulkan`.
- Exported games must still build: run `tools/build.sh web` and `tools/build.sh android` after
  shader or render-layer changes, and `tools/smoke-test.sh` for export paths.
