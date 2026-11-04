# libwebp decoder (editor only)

[libwebp](https://chromium.googlesource.com/webm/libwebp) 1.5.0 (tag `v1.5.0`, commit
`a4d7a71`), BSD-3-Clause (`COPYING`, with Google's patent grant in `PATENTS`). The editor
decodes `EXT_texture_webp` images of glTF models with it (`editor/util/GltfEditorDecoders.cpp`,
docs/specs/S53-model-import-cook.md R6); the model cook transcodes them, so exported games never
carry it.

What is here: upstream's `libwebpdecoder` sources (`makefile.unix`: `DEC_OBJS`, `DSP_DEC_OBJS`,
`UTILS_DEC_OBJS`) and every header they include, unmodified. No `config.h`: no threads, SIMD
paths as the compiler's own flags allow (SSE2 on x86-64, NEON on arm64), picked at run time.
