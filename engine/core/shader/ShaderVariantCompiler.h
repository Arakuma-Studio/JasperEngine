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

    // The sources of a custom shader: its two entry points and every project file their
    // #include graph reaches. An include resolves against the entry point's directory, then
    // the project root, then the engine shader library. A library shader ("library:<name>")
    // has its entry points in the engine library and no project file.
    struct JASPERENGINE_API CustomShaderSources {
        bool valid = false;
        std::string error;
        bool library = false;                       // an engine library shader
        std::string vertFile;                       // project-relative entry points (library paths for a library shader)
        std::string fragFile;
        std::map<std::string, std::string> files;   // project files, by include key / path
        std::vector<std::string> defines;           // the reference's "#A,B" feature suffix, each #defined to 1
        // a library shader or a fork with a manifest: on the library vertex path
        // (includes/surface_vs.glsl), so a pixel-lit mesh variant also gets the world position
        // and normal varyings (SURFACE_POSITION, SURFACE_NORMALS) its lights need
        bool manifest = false;
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
        // what starts the log line of a GLES3 variant built without PCSS (compile)
        static constexpr const char* PCSS_LEFT_OUT = "pcss-left-out";

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
        // explicit entry points joined by '|') as its two entry points. A "#..." feature
        // suffix is ignored. Not for library references (see resolveCustomShaderSources).
        static void getCustomShaderPaths(const std::string& customShader, std::string& vertFile, std::string& fragFile);
        // The engine library entry points of "library:<name>" as a shaderType (DEPTH: its
        // depth program): <name>.vert/.frag, the shared surface.vert for a mesh shader
        // without a .vert, <name>_depth.vert/.frag or the shared surface_depth ones.
        // False (and an error) for an unknown shader.
        static bool getLibraryShaderPaths(const std::string& reference, ShaderType shaderType, std::string& vertFile, std::string& fragFile, std::string* error = nullptr);
        // directory includes resolve against first: the vertex entry point's
        static std::string getCustomShaderIncludeRoot(const std::string& customShader);
        // the paths quoted by every '#include "..."' of a GLSL source, in order
        static std::vector<std::string> getIncludeKeys(const std::string& source);
        // a relative path that cannot leave the directory it is resolved against
        static bool isSafeRelativePath(const std::string& path);

        // Reads a fork's entry points and the project files its includes reach through
        // readProjectFile (project-relative paths), or finds a library shader's in the
        // engine library, as the shaderType the variant is (a library shader's DEPTH
        // variant is its depth program). The "#A,B" suffix becomes defines. Never throws.
        static CustomShaderSources resolveCustomShaderSources(const std::string& customShader, const FileReader& readProjectFile,
                                                              ShaderType shaderType = ShaderType::MESH);

        // Compiles one variant for a backend. customSources is required for a key with a
        // custom shader id and ignored otherwise. On failure returns false and fills
        // error with what failed and log with the compiler output. progress, when set,
        // is told how far the build is (0..1); cancelled, when set, stops it between steps.
        // For GLES3 a variant the raw-depth shadow view (PCSS) would take past 16 fragment
        // textures is built without it (SHADOW_RAW_VIEW_OFF); log then has a PCSS_LEFT_OUT line.
        static bool compile(ShaderKey key, ShaderBackend backend, const CustomShaderSources* customSources,
                            ShaderData& out, std::string* error = nullptr, std::string* log = nullptr,
                            const ProgressFn& progress = {}, const std::function<bool()>& cancelled = {});

        // The preprocessed GLSL of one variant's two stages, as compile() would parse them for
        // the backend (includes expanded, the variant's #defines applied). A stage the variant
        // does not have is left empty. False (and an error) when it cannot be preprocessed.
        static bool preprocess(ShaderKey key, ShaderBackend backend, const CustomShaderSources* customSources,
                               std::string& vertexSource, std::string& fragmentSource, std::string* error = nullptr);
        // A preprocessed source without what only positions it: #line directives, blank lines
        // and indentation. Two sources the compiler sees as the same program normalize alike
        // when they differ only by comments, guarded-out blocks or layout.
        static std::string normalizePreprocessed(const std::string& source);

        // The engine's per-stage resource budget a compiled variant must fit on every backend
        // (shadercompiler.h: 8 uniform blocks, 16 textures, 16 samplers, 16 texture-sampler
        // pairs; GLES3 has 16 fragment texture units). Empty when it fits, else what is over.
        static std::string checkResourceBudget(const ShaderData& data);
        // A globals block (u_vs_globals / u_fs_globals, ShaderGlobals) over 1024 bytes: the
        // "globals-block-too-large: ..." error compile() fails with; "" when they fit
        static std::string checkGlobalsBlocks(const ShaderData& data);
    };

}

#endif // SHADERVARIANTCOMPILER_H
