---
paths:
  - "engine/renders/**"
  - "engine/shaders/**"
  - "engine/shaderlib/**"
  - "engine/core/render/**"
---

# Rendering

- GLSL is cross-compiled per backend: avoid constructs one of the targets lacks.
- In place of the GPU features `AGENTS.md` bans, use fragment-shader and CPU (job system) solutions.
- A disabled effect adds no pass, no draw and no allocation.
- In a loop with a per-fragment exit (`continue`, `discard`) or branch, sample with textureLod/textureGrad
  and take derivatives before the loop: `python3 tools/check_shader_lod.py` checks it.
- Rendering changes come with golden images: `tools/build.sh htmlui-tests` and the core GPU
  suites of `tools/build.sh unit-tests` (`JASPER_TEST_UPDATE_GOLDEN=1` rewrites, glcore only). Check Vulkan too with `GRAPHIC_BACKEND=vulkan` (`unit-tests`)
  and `HTMLUI_BACKEND=vulkan` (`htmlui-tests`).
- Exported games must still build: run `tools/build.sh web` and `tools/build.sh android` after
  shader or render-layer changes, and `tools/smoke-test.sh` for export paths.
