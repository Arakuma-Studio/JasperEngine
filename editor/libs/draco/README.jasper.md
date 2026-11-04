# Draco decoder (editor only)

[Draco](https://github.com/google/draco) 1.5.7 (tag `1.5.7`, commit `8786740`), Apache-2.0
(`LICENSE`). The editor decodes `KHR_draco_mesh_compression` glTF files with it
(`editor/util/GltfEditorDecoders.cpp`, docs/specs/S53-model-import-cook.md R6); the model cook
transcodes them, so exported games never carry it.

What is here: the source groups upstream's Emscripten decoder target builds
(`CMakeLists.txt`, `draco_decoder_src`: attributes, the attribute, mesh and point cloud decoders,
prediction schemes, bit coders, entropy, core, mesh, metadata, point cloud), and every header they
include, unmodified. `src/draco/draco_features.h` replaces the header upstream's CMake generates.
Upstream's build files, tests, tools, encoders and the glTF/transcoder code are not vendored.
