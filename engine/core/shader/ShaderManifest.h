// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#ifndef SHADERMANIFEST_H
#define SHADERMANIFEST_H

#include "Export.h"
#include "render/Render.h"
#include "math/Vector4.h"
#include "texture/Material.h"

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace jasperengine{

    class ShaderData;

    // How a shader evaluates scene lights (manifest @lighting). The first mode a shader
    // lists is its default; a shader without @lighting is NONE.
    enum class ShaderLightingMode{
        NONE,
        PIXEL,
        VERTEX,
        MIXED
    };

    // Render queue of a manifest @state (queue=...). AUTO follows the blend mode.
    enum class ShaderQueue{
        AUTO,
        GEOMETRY,
        ALPHATEST,
        TRANSPARENT
    };

    enum class ShaderPropertyKind{
        FLOAT,
        RANGE,
        INT,      // uploaded as float
        TOGGLE,   // 0/1, uploaded as float
        COLOR,    // edited as sRGB, stored and uploaded linear
        HDRCOLOR, // like COLOR, components may exceed 1
        VECTOR2,
        VECTOR3,
        VECTOR4
    };

    // the engine texture an unset @texture binds
    enum class ShaderTextureDefault{
        WHITE,
        BLACK,
        GRAY,
        NORMAL // flat tangent-space normal (0.5, 0.5, 1)
    };

    // vertex inputs of @require: forced on when the mesh has them, and the varyings
    // includes/surface_vs.glsl writes (position: the world position, v_position)
    enum ShaderRequireFlags : uint32_t{
        ShaderRequire_Normals  = 1 << 0,
        ShaderRequire_Tangents = 1 << 1,
        ShaderRequire_UV1      = 1 << 2,
        ShaderRequire_UV2      = 1 << 3,
        ShaderRequire_Color    = 1 << 4,
        ShaderRequire_Position = 1 << 5
    };

    struct JASPERENGINE_API ShaderManifestProperty{
        std::string member;
        ShaderPropertyKind kind = ShaderPropertyKind::FLOAT;
        Vector4 defaultValue = Vector4(0.0f, 0.0f, 0.0f, 0.0f); // as stored: colours linear
        float min = 0.0f; // range only
        float max = 1.0f;
        std::string label;
        std::string feature; // shown only while this feature is on
        int line = 0;
    };

    struct JASPERENGINE_API ShaderManifestTexture{
        std::string uniform;
        ShaderTextureDefault defaultTexture = ShaderTextureDefault::WHITE;
        bool cube = false;
        std::string label;
        std::string feature;
        int line = 0;
    };

    struct JASPERENGINE_API ShaderManifestFeature{
        std::string define;
        std::string label;
        bool defaultOn = false;
        int line = 0;
    };

    // an exclusive choice: the chosen value defines <group>_<value>; the first is the default
    struct JASPERENGINE_API ShaderManifestKeywords{
        std::string group;
        std::string label;
        std::vector<std::string> values;
        int line = 0;
    };

    // @state defaults; AUTO leaves the value to the engine's legacy behaviour
    struct JASPERENGINE_API ShaderManifestState{
        MaterialBlendMode blend = MaterialBlendMode::AUTO;
        MaterialCullMode cull = MaterialCullMode::AUTO;
        MaterialDepthWrite depthWrite = MaterialDepthWrite::AUTO;
        MaterialDepthTest depthTest = MaterialDepthTest::AUTO;
        bool hasDepthOffset = false;
        float depthOffsetFactor = 0.0f;
        float depthOffsetUnits = 0.0f;
        ShaderQueue queue = ShaderQueue::AUTO;
    };

    struct JASPERENGINE_API ShaderManifestDiagnostic{
        std::string file;
        int line = 0;
        bool error = true; // false: a warning
        std::string message;
    };

    // The properties block of a shader: "// @<directive> <arguments>" comment lines at the
    // top of its .frag (and .vert; both are merged). See the shader library spec, section 4.
    // A manifest with errors still describes what it could parse.
    struct JASPERENGINE_API ShaderManifest{
        bool present = false;    // at least one directive
        bool hasType = false;    // @shader given
        ShaderType type = ShaderType::MESH;
        std::string name;
        std::string category;
        std::string description;
        std::vector<ShaderLightingMode> lighting; // empty: NONE
        uint32_t require = 0;    // ShaderRequireFlags
        ShaderManifestState state;
        std::vector<ShaderManifestProperty> properties;
        std::vector<ShaderManifestTexture> textures;
        std::vector<ShaderManifestFeature> features;
        std::vector<ShaderManifestKeywords> keywords;
        bool depth = false;      // has its own depth program
        std::vector<ShaderManifestDiagnostic> diagnostics;

        // Adds the directives at the top of one source (up to its first line of code)
        void parse(const std::string& source, const std::string& file);
        // Errors a valid manifest of a library shader must not have (missing @shader/@name)
        void checkComplete(const std::string& file, bool library);

        bool hasErrors() const;
        ShaderLightingMode getDefaultLighting() const;
        bool supportsLighting(ShaderLightingMode mode) const;

        const ShaderManifestProperty* findProperty(const std::string& member) const;
        const ShaderManifestTexture* findTexture(const std::string& uniform) const;
        const ShaderManifestFeature* findFeature(const std::string& define) const;
        // the group a "<group>_<value>" define belongs to, or null
        const ShaderManifestKeywords* findKeywordGroup(const std::string& define) const;

        // The defines a shaderFeatures list enables, sorted: a @feature is on when the list
        // has it, off when the list has "!<define>", else its default; one define per
        // @keywords group (the first of the list in that group, else the group's first
        // value). Entries the manifest does not declare are ignored.
        std::vector<std::string> resolveFeatures(const std::vector<std::string>& list) const;
        // the default value of a member (manifest default), false when not declared
        bool getDefault(const std::string& member, Vector4& value) const;

        // Checks the @property members and @texture uniforms against what compiled
        // variants of the shader declare (one or several; a member behind a feature only
        // exists in the variants with it). Adds errors to diagnostics.
        void validateReflection(const std::vector<const ShaderData*>& variants, const std::string& file);

        // the SURFACE_* defines the shader compiles with (one per @require input, see
        // includes/surface_vs.glsl)
        std::vector<std::string> getRequireDefines() const;

        // the source declares manifest lines (a fork the runtime needs the file of)
        static bool hasManifestLines(const std::string& source);
        static std::string getLightingModeName(ShaderLightingMode mode);
        static std::string getShaderTypeToken(ShaderType type);
    };

    // A shader reference string (section 3.1): "" built-in, "library:<name>" an engine
    // library shader, anything else a project fork. A variant's reference may end in
    // "#A,B" (its enabled feature defines, sorted), which compiles them as #defines.
    class JASPERENGINE_API ShaderReference{
    public:
        static const char* const LIBRARY_PREFIX; // "library:"

        static bool isLibrary(const std::string& reference);
        // "library:toon#RIM" -> "toon"
        static std::string getLibraryName(const std::string& reference);
        // without the "#..." feature suffix
        static std::string getBase(const std::string& reference);
        // the "#..." feature suffix as a list
        static std::vector<std::string> getFeatures(const std::string& reference);
        // a define name: upper case letters, digits and '_', not starting with a digit
        static bool isValidDefine(const std::string& define);
        // a library name: lower case snake_case
        static bool isValidLibraryName(const std::string& name);
        // base + "#" + the defines (already sorted), or base when there are none
        static std::string withFeatures(const std::string& base, const std::vector<std::string>& defines);

        // The reference a variant is keyed by: the base with the features its manifest
        // resolves from a shaderFeatures list (resolveFeatures); the base alone when the
        // shader declares no features or has no manifest.
        static std::string getVariantReference(const std::string& reference, const std::vector<std::string>& shaderFeatures);
    };

    // Edits of the shader-library values of a material or a component (Lua, C++), shared so
    // every owner treats them alike
    class JASPERENGINE_API ShaderLibraryValues{
    public:
        // the value set, else the shader's manifest default, else zero
        static Vector4 getParam(const ShaderUniformValues& values, const std::string& shader, const std::string& name);
        // the texture of a uniform: set from a path (an empty path removes it, so the
        // manifest default binds); false when nothing changed
        static bool setTexture(std::vector<std::pair<std::string, Texture>>& textures, const std::string& uniform, const std::string& path);
        static std::string getTexture(const std::vector<std::pair<std::string, Texture>>& textures, const std::string& uniform);
        // Turns a @feature on or off ("!<define>" keeps a default-on feature off), or picks
        // a @keywords value (the others of its group are dropped); false when nothing changed
        static bool setFeature(std::vector<std::string>& features, const std::string& shader, const std::string& define, bool enabled);
        // whether the define is on, the manifest defaults included
        static bool isFeatureEnabled(const std::vector<std::string>& features, const std::string& shader, const std::string& define);
    };

    // Parsed manifests, once per source and cached: library shaders from the manifests the
    // build embeds, forks from their files (the editor reads the project, a game its assets).
    // Thread-safe.
    class JASPERENGINE_API ShaderManifests{
    public:
        using SourceReader = std::function<bool(const std::string& path, std::string& content)>;

        // The manifest of a shader reference (the feature suffix is ignored); null for the
        // built-in shader, an unknown library shader, or a fork whose files cannot be read
        // (logged once). A fork without manifest lines gives a manifest with present false.
        static std::shared_ptr<const ShaderManifest> get(const std::string& reference);

        // How fork files are read: project-relative paths. Default: the game's assets.
        static void setForkReader(SourceReader reader);
        // Forgets the fork manifests (their sources changed); library ones stay
        static void clearForks();

        // the library shaders the build has, by name, sorted
        static std::vector<std::string> getLibraryShaderNames();
        // manifest lines of the library entry files, by library path ("library/toon.frag")
        static const std::unordered_map<std::string, std::string>& getLibraryManifestSources();
        static bool hasLibraryFile(const std::string& path);
    };

}

#endif //SHADERMANIFEST_H
