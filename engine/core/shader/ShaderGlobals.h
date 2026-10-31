// (c) JasperDev and contributors
// SPDX-License-Identifier: MIT

#ifndef SHADERGLOBALS_H
#define SHADERGLOBALS_H

#include "Export.h"
#include "ShaderData.h"
#include "ShaderUniforms.h"
#include "math/Matrix4.h"
#include "math/Vector4.h"
#include "texture/Texture.h"

#include <cstdint>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace jasperengine {

    // Global shader values (Unity's Shader.SetGlobal*, Godot's global uniforms): set once,
    // read by every shader that declares them. A shader reads them through one block per
    // stage, u_vs_globals / u_fs_globals, with only the members it needs, filled by member
    // name; a texture global binds to a texture uniform of its name (sampler <name>_smp).
    // docs/superpowers/specs/2026-10-31-shader-globals-design.md
    enum class ShaderGlobalType : uint8_t {
        Float, Vector2, Vector3, Vector4,
        Color,          // a Vector4 set and shown as sRGB, stored linear (alpha as is)
        Matrix4,
        FloatArray, Vector4Array,
        Texture2D, TextureCube
    };

    // a Project Settings > Graphics > Shader Globals entry
    struct JASPERENGINE_API ShaderGlobalDecl {
        std::string name;
        ShaderGlobalType type = ShaderGlobalType::Vector4;
        unsigned int arraySize = 0;          // arrays: 1..64, else 0
        // linear; one 16-byte slot per element: float in .x, a matrix its 4 columns, an
        // array its arraySize elements (missing ones read zero)
        std::vector<Vector4> defaultValue;
        std::string defaultTexture;          // an asset path; "" = white (2D) / black (cube)

        bool operator == (const ShaderGlobalDecl& other) const {
            return name == other.name && type == other.type && arraySize == other.arraySize &&
                   defaultValue == other.defaultValue && defaultTexture == other.defaultTexture;
        }
        bool operator != (const ShaderGlobalDecl& other) const { return !(*this == other); }
    };

    // what a global holds now
    struct JASPERENGINE_API ShaderGlobalValue {
        ShaderGlobalType type = ShaderGlobalType::Vector4;
        unsigned int arraySize = 0;
        std::vector<Vector4> values;         // as ShaderGlobalDecl::defaultValue
        Texture texture;                     // texture types: the set one, else the default (empty: the fallback)
        bool declared = false;               // in Project Settings, else made by a script's first set
    };

    // A stage's globals block as a shader declares it (reflection offsets, std140), and its
    // bytes packed from ShaderGlobals. Shared by every program whose block has the same layout,
    // so it is packed at most once per change of the values (ShaderGlobals::acquireLayout).
    struct JASPERENGINE_API GlobalsBlockLayout {
        struct Member {
            int globalId = 0;
            std::string name;
            unsigned int offset = 0;
            ShaderUniformType type = ShaderUniformType::FLOAT4;
            unsigned int arrayCount = 1;
        };
        unsigned int size = 0;               // bytes, rounded up to 16
        std::vector<Member> members;
        std::vector<uint8_t> bytes;
        uint64_t packedVersion = ~0ull;

        // repacks when the values changed since the last pack
        void update();
        // every member from its global's current value (zero when there is none)
        void pack();
    };

    // a problem of a shader's globals block against the declared globals (check_shader, Console)
    struct JASPERENGINE_API ShaderGlobalDiagnostic {
        std::string code;      // global-undeclared, global-type-mismatch, globals-block-too-large
        bool error = false;    // else a warning
        std::string message;
    };

    class JASPERENGINE_API ShaderGlobals {
    public:
        static constexpr unsigned int MAX_DECLARED = 256;
        static constexpr unsigned int MAX_ARRAY_SIZE = 64;
        static constexpr unsigned int MAX_BLOCK_BYTES = 1024;   // 64 vec4 slots per stage
        static constexpr const char* VERTEX_BLOCK = "u_vs_globals";
        static constexpr const char* FRAGMENT_BLOCK = "u_fs_globals";

        // ---- names and ids (main thread)
        // a stable id for the process (made for a new name; 0 for an empty one)
        static int propertyToID(const std::string& name);
        // the id of a known name, 0 when the name was never seen
        static int findID(const std::string& name);
        // "" for an unknown id
        static const std::string& nameOf(int id);
        // a GLSL identifier [A-Za-z_][A-Za-z0-9_]{0,63}, not "gl_..." or "u_..." (engine uniforms), no "__"
        static bool isValidName(const std::string& name, std::string* error = nullptr);

        // ---- declarations (Project Settings)
        // Replaces the declarations. A global whose declaration is unchanged keeps its value;
        // a new or changed one takes its default; a global no longer declared is dropped.
        // Invalid entries (name, duplicate, array size, over MAX_DECLARED) are skipped with an error.
        static void declare(const std::vector<ShaderGlobalDecl>& decls);
        static const std::vector<ShaderGlobalDecl>& getDeclarations();
        // a copy, safe from any thread (a shader build checking what it reads)
        static std::vector<ShaderGlobalDecl> getDeclarationsSnapshot();
        // declared globals back to their defaults, the ones scripts made dropped (app start,
        // editor Play start and stop, Shader.resetGlobals)
        static void resetToDefaults();
        // the problems of a declaration list ("" when it is valid), as declare() would report them
        static std::string validate(const std::vector<ShaderGlobalDecl>& decls);
        // a declaration with its type's default (black, identity, zero; arrays of arraySize)
        static ShaderGlobalDecl makeDecl(const std::string& name, ShaderGlobalType type, unsigned int arraySize = 0);

        // ---- values (main thread; false when refused: wrong kind, unknown id)
        // A setter of another kind converts: a float into (x, 0, 0, 0) for a vector, a vector
        // into its x for a float; an array takes element 0 from a scalar or vector setter; a
        // Matrix4 global only takes setMatrix (and setMatrix only a Matrix4 global); a texture
        // global only setTexture. A name never declared nor set takes the kind of its first
        // setter (an array its size; a later longer array is truncated with a warning).
        static bool setFloat(int id, float value);
        static bool setVector(int id, const Vector4& value);
        static bool setColor(int id, const Vector4& srgb);   // rgb sRGB to linear, alpha as is
        static bool setMatrix(int id, const Matrix4& value);
        static bool setFloatArray(int id, const float* values, size_t count);
        static bool setVectorArray(int id, const Vector4* values, size_t count);
        static bool setTexture(int id, const Texture& texture);   // a path, a framebuffer, data
        // back to the declared default texture (else the fallback)
        static bool clearTexture(int id);
        // a value exactly as stored (Project Settings Live column: linear colours)
        static bool setValues(int id, const std::vector<Vector4>& values);

        static bool exists(int id);
        static bool isDeclared(int id);
        static bool get(int id, ShaderGlobalValue& out);
        // the first slot (a float in .x), false when the global does not exist or is a texture
        static bool getVector(int id, Vector4& out);
        // the GPU texture a draw binds for a texture global (loaded on first use): fallback
        // while it loads, when it is unset, missing or of the other kind (2D / cube)
        static TextureRender* getTextureRender(int id, bool cube, TextureRender* fallback);
        // existing globals (declared first, in declaration order, then the ones scripts made)
        static std::vector<std::string> getNames();

        // +1 when a value changes (textures included)
        static uint64_t getVersion();
        // +1 when a global is made, declared or dropped
        static uint64_t getNamesVersion();

        // ---- shaders
        // The globals block a program declares (VERTEX_BLOCK / FRAGMENT_BLOCK), shared by every
        // program with the same layout; null when the program has none or it is empty
        static std::shared_ptr<GlobalsBlockLayout> acquireLayout(ShaderData& shaderData, const std::string& blockName);
        // the GLSL a shader pastes to read these globals: the stage's block (non-texture
        // globals) and, for the fragment stage, the texture and sampler uniforms; only the
        // names given when `only` is not empty. "" when nothing applies to the stage
        static std::string getGlslBlock(const std::vector<ShaderGlobalDecl>& decls, ShaderStageType stage,
                                        const std::vector<std::string>& only = {});
        // the GLSL member declaration of a non-texture type ("vec4 FogTint", "float w[8]")
        static std::string getGlslMember(ShaderGlobalType type, unsigned int arraySize, const std::string& name);
        // 16-byte slots a block member of the type takes (std140)
        static unsigned int getSlotCount(ShaderGlobalType type, unsigned int arraySize);
        // what a compiled shader's globals blocks and texture uniforms read of these declarations:
        // members no global declares (they read 0 until a script sets one), members whose GLSL
        // type is not the global's (they read its leading components, or 0), a block over
        // MAX_BLOCK_BYTES
        static std::vector<ShaderGlobalDiagnostic> checkShader(const ShaderData& shaderData, const std::vector<ShaderGlobalDecl>& decls);

        // ---- bindings (a member of a material's or component's custom block that follows a global)
        // global "" (or a texture global's name: refused) removes it; false when nothing changed
        static bool setBinding(ShaderGlobalBindings& bindings, const std::string& member, const std::string& global);
        static std::string getBinding(const ShaderGlobalBindings& bindings, const std::string& member);
        // the value a bound member takes: the global's first slot; false (zero) when it does
        // not exist or is a texture or a matrix
        static bool getBoundValue(const std::string& global, Vector4& out);
        // blocks written with these bindings at versionSeen (getVersion() then) must be written
        // again: a value changed since, and something is bound
        static bool isOutdated(const ShaderGlobalBindings& bindings, uint64_t versionSeen);

        // ---- types as text (project.yaml, MCP): float, vector2, vector3, vector4, color,
        // matrix4, float_array, vector4_array, texture2d, texturecube
        static const char* getTypeName(ShaderGlobalType type);
        static bool parseTypeName(const std::string& text, ShaderGlobalType& type);
        static bool isTexture(ShaderGlobalType type);
        static bool isArray(ShaderGlobalType type);
        // components a value of the type has (1 float, 2, 3, 4; 16 a matrix; 0 a texture)
        static int getComponentCount(ShaderGlobalType type);

        // drops every value, declaration and texture (ids stay); tests
        static void clear();
        // lets go of the textures the globals hold (engine shutdown, before the texture pools)
        static void releaseTextures();
    };

}

#endif //SHADERGLOBALS_H
