// Stand-in for the engine shader library the build embeds for the runtime shader
// compiler. CMake builds generate the real one from engine/shaderlib
// (libs/shadercompiler/embed_shaderlib.cmake), which takes precedence over this file;
// the editor's Apple export writes it here. Without it a build compiles no shader at
// runtime and only has its precompiled ones.

#pragma once

#include <cstddef>

namespace jasperengine { namespace shaderlib {

struct File { const char* name; const char* source; };

static const File files[1] = { { nullptr, nullptr } };
static const size_t fileCount = 0;

} }
