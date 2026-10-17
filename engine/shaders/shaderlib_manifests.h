// Stand-in for the library shader manifests the build embeds. CMake builds generate the
// real one from engine/shaderlib/library (libs/shadercompiler/embed_shaderlib.cmake with
// MANIFESTS=ON), which takes precedence over this file; the editor's Apple export writes
// it here. Without it a build knows no library shader's properties.

#pragma once

#include <cstddef>

namespace jasperengine { namespace shaderlib_manifests {

struct File { const char* name; const char* source; };

static const File files[1] = { { nullptr, nullptr } };
static const size_t fileCount = 0;

} }
