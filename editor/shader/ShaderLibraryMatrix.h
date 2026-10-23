// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#pragma once

#include "shader/ShaderBuildTypes.h"
#include "shader/ShaderManifest.h"

#include <string>
#include <vector>

namespace jasperengine::editor {

    // The compile matrix of the engine's library shaders (jasperengine-editor shaders
    // --library): for each shader its base variant, each feature alone, all features on,
    // each keyword value, each lighting mode it supports and, for mesh shaders, the mesh
    // variants (skinning, morph targets, instancing, vertex colour + UV2, texture rect, fog,
    // shadows), plus the depth program of a shader with @depth. Every variant compiles for
    // every backend asked, and the manifest is checked against what compiled.
    class ShaderLibraryMatrix {
    public:
        struct Variant {
            std::string label;      // what the variant covers ("base", "feature RIM", "skinning", ...)
            std::string reference;  // the custom shader string, "library:<name>#FEATURES"
            ShaderType type = ShaderType::MESH;
            uint64_t properties = 0;
        };

        struct Problem {
            std::string file;       // engine/shaderlib/... path
            int line = 0;
            bool error = true;
            std::string message;
        };

        struct Result {
            std::string shader;     // library:<name>
            size_t variants = 0;
            size_t compiled = 0;    // variant x backend compiles that succeeded
            size_t failed = 0;
            std::vector<Problem> problems;
        };

        // the variants of one library shader (name without the "library:" prefix)
        static std::vector<Variant> buildVariants(const std::string& name, const ShaderManifest& manifest);

        // compiles every variant for every backend and validates the manifest
        static Result run(const std::string& name, const std::vector<ShaderBackend>& backends);
    };

}
