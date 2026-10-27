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

        // The built-in Lit's own variants (docs/specs/S11-materials-lighting.md, section 6.3):
        // each extended lobe alone, per pixel and mixed, with and without its map; every lobe
        // with three maps; transmission with volume; hex tiling unlit and lit; a hex tiled
        // painted Terrain; the High Lit Quality on the base variants. Each in the worst-case
        // scene for textures (pixel lights with shadows, IBL, SSAO, the post colour), so the
        // budget check covers the 16 fragment textures of GLES3 and WebGL2.
        static constexpr const char* BUILTIN_LIT_NAME = "builtin_lit";
        static std::vector<Variant> buildBuiltinLitVariants();
        static Result runBuiltinLit(const std::vector<ShaderBackend>& backends);

    private:
        // compiles the variants for the backends into result (problems against file), the
        // colour variants' reflection of the first backend into reflections
        static void compileVariants(const std::vector<Variant>& variants, const std::vector<ShaderBackend>& backends,
                                    const std::string& file, Result& result, std::vector<ShaderData>* reflections);
    };

}
