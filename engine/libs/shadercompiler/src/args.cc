// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "shadercompiler.h"

using namespace shadercompiler;

args_t shadercompiler::initialize_args(){
    args_t args;
    args.useBuffers = false;
    args.lang = LANG_GLSL;
    args.version = 0;
    args.es = false;
    args.platform = SHADER_DEFAULT;
    args.list_includes = false;
    args.optimization = true;

    return args;
}
