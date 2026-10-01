// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#ifndef SHADERVARIANTCOMPILER_H
#define SHADERVARIANTCOMPILER_H

#include "Export.h"
#include "ShaderBuildTypes.h"
#include "ShaderData.h"

#include <cstdint>
#include <functional>
#include <map>
#include <string>
#include <unordered_map>
#include <vector>

namespace jasperengine {

    // The sources of a forked (custom) shader: its two entry points and every project
    // file their #include graph reaches. An include resolves against the entry point's
    // directory, then the project root, then the engine shader library.
    struct JASPERENGINE_API CustomShaderSources {
        bool valid = false;
        std::string error;
        std::string vertFile;                       // project-relative entry points
        std::string fragFile;
        std::map<std::string, std::string> files;   // project files, by include key / path
        // Unambiguous description of the resolved graph (origin and content of every
        // file, in resolution order): equal signatures compile to the same shader.
        std::string signature;
    };

    // Compiles shader variants (ShaderType + property bits) from the engine shader
    // library for any graphics backend: GLSL to SPIR-V with glslang, then SPIR-V to the
    // backend's language with SPIRV-Cross. The editor builds and exports with it, and a
    // game compiles at runtime with it the variants it does not ship precompiled.
    //
    // Compiling is thread-safe. A build without JASPERENGINE_RUNTIME_SHADERS (or one
    // whose embedded library is empty) has no compiler: isAvailable() is false and
    // compile() fails.
    class JASPERENGINE_API ShaderVariantCompiler {
    public:
        using FileReader = std::function<bool(const std::string& path, std::string& content)>;
        using ProgressFn = std::function<void(float progress)>;

        static bool isAvailable();

        // engine/shaderlib embedded in the build, by library-relative path ("mesh.frag",
        // "includes/pbr.glsl")
        static const std::unordered_map<std::string, std::string>& getLibrarySources();

        // Changes with the embedded library and with everything else a compiled variant
        // depends on (compiler pipeline, engine limits baked into the shaders): what a
        // cache of compiled variants is keyed by.
        static uint64_t getLibraryHash();

        // 64-bit FNV-1a, the hash the runtime shader cache uses
        static uint64_t hash(const std::string& data, uint64_t seed = 14695981039346656037ULL);
        static std::string toHex(uint64_t value);

        // A customShader value (a project-relative base path, "shaders/myMesh", or two
        // explicit entry points joined by '|') as its two entry points
        static void getCustomShaderPaths(const std::string& customShader, std::string& vertFile, std::string& fragFile);
        // directory includes resolve against first: the vertex entry point's
        static std::string getCustomShaderIncludeRoot(const std::string& customShader);
        // the paths quoted by every '#include "..."' of a GLSL source, in order
        static std::vector<std::string> getIncludeKeys(const std::string& source);
        // a relative path that cannot leave the directory it is resolved against
        static bool isSafeRelativePath(const std::string& path);

        // Reads a fork's entry points and the project files its includes reach through
        // readProjectFile (project-relative paths). Never throws.
        static CustomShaderSources resolveCustomShaderSources(const std::string& customShader, const FileReader& readProjectFile);

        // Compiles one variant for a backend. customSources is required for a key with a
        // custom shader id and ignored otherwise. On failure returns false and fills
        // error with what failed and log with the compiler output. progress, when set,
        // is told how far the build is (0..1); cancelled, when set, stops it between steps.
        static bool compile(ShaderKey key, ShaderBackend backend, const CustomShaderSources* customSources,
                            ShaderData& out, std::string* error = nullptr, std::string* log = nullptr,
                            const ProgressFn& progress = {}, const std::function<bool()>& cancelled = {});
    };

}

#endif // SHADERVARIANTCOMPILER_H
