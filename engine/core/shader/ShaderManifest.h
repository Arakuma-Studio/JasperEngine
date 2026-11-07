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
        NORMAL, // flat tangent-space normal (0.5, 0.5, 1)
        RIPPLES, // a seamless ripple normal map (library:water's, Water::makeRipplesTexture)
        NOISE,   // seamless grey value noise (Water::makeNoiseTexture)
        PARTICLE // a white soft disc fading to transparent (particles::makeParticleTexture)
    };

    // vertex inputs of @require: forced on when the mesh has them, and the varyings
    // includes/surface_vs.glsl writes (position: the world position, v_position; rawcolor:
    // the vertex colour attribute alone, v_rawColor)
    enum ShaderRequireFlags : uint32_t{
        ShaderRequire_Normals  = 1 << 0,
        ShaderRequire_Tangents = 1 << 1,
        ShaderRequire_UV1      = 1 << 2,
        ShaderRequire_UV2      = 1 << 3,
        ShaderRequire_Color    = 1 << 4,
        ShaderRequire_Position = 1 << 5,
        ShaderRequire_RawColor = 1 << 6, // the vertex colour without the instance colour (v_rawColor)
        // a_smoothNormal: the normal averaged over the vertices sharing a position, which the
        // renderer generates for the mesh (shader library phase 2, inverted-hull outlines)
        ShaderRequire_SmoothNormals = 1 << 7,
        // scene inputs (shader library phase 3, includes/scene.glsl): not vertex inputs but
        // what the engine produces for the camera pass a requiring draw is in: the opaque
        // scene's depth (SURFACE_SCENE_DEPTH, u_sceneDepthTexture) and a copy of its colour
        // (SURFACE_SCENE_COLOR, u_sceneColorTexture). Mesh and points shaders only; the draw
        // must be in the transparent queue (it reads what the opaque queue drew).
        ShaderRequire_SceneDepth = 1 << 8,
        ShaderRequire_SceneColor = 1 << 9,
        ShaderRequire_SceneInputs = ShaderRequire_SceneDepth | ShaderRequire_SceneColor,
        // the reflection environment Lit reflects (includes/environment.glsl,
        // SURFACE_ENVIRONMENT, u_environmentTexture): the sky's prefiltered map or the
        // reflection probe around the object. Mesh shaders only; bound per draw, no pass.
        ShaderRequire_Environment = 1 << 10,
        // the material's own normal map (Material::normalTexture) on the 2D light path
        // (SURFACE_NORMAL_MAP): under 2D lights, with a normal texture, on a mesh with normals,
        // tangents and uv1, the variant samples it (HAS_NORMAL_MAP) and the shader lights
        // through includes/lighting2d.glsl's getNormal2D (it must include that file, which
        // reads the tangent frame surface_vs.glsl writes then). Mesh shaders only
        ShaderRequire_NormalMap = 1 << 11,
        // the scene mirrored about the horizontal plane of the draw's water level
        // (includes/planar_reflection.glsl, SURFACE_PLANAR_REFLECTION, u_planarReflectionTexture):
        // a reflection pass of the camera per plane height (scene setting Planar Reflections),
        // bound per draw; without one this frame the shader is told so and falls back. Mesh
        // shaders only, fragment stage
        ShaderRequire_PlanarReflection = 1 << 12
    };

    // What the engine treats a shader as (manifest @role), whatever its name: forks of
    // library:water copy "@role water" with the manifest, and keep the buoyancy, the Water
    // queries and the underwater switch (object/environment/Water.h)
    enum class ShaderRole{
        NONE,
        WATER,       // a water surface: library:water's parameters, buoyancy, layering
        UNDERWATER   // the view from inside a water: library:underwater's switch and values
    };

    struct JASPERENGINE_API ShaderManifestProperty{
        std::string member;
        ShaderPropertyKind kind = ShaderPropertyKind::FLOAT;
        Vector4 defaultValue = Vector4(0.0f, 0.0f, 0.0f, 0.0f); // as stored: colours linear
        float min = 0.0f; // range only
        float max = 1.0f;
        std::string label;
        std::string feature; // shown only while this feature is on ("!<DEFINE>": while it is off)
        int line = 0;
    };

    struct JASPERENGINE_API ShaderManifestTexture{
        std::string uniform;
        ShaderTextureDefault defaultTexture = ShaderTextureDefault::WHITE;
        bool cube = false;
        std::string label;
        std::string feature; // as ShaderManifestProperty::feature
        // default[<DEFINE>]=<default>: what an unset slot binds instead while that @feature (or
        // @keywords value) is on, so a material opts into a built-in texture without a file
        // (library:water's BUILTIN_TEXTURES); empty: none
        std::string featureDefaultDefine;
        ShaderTextureDefault featureDefaultTexture = ShaderTextureDefault::WHITE;
        int line = 0;

        // the default an unset slot binds with these defines on (ShaderReference::getEnabledFeatures)
        ShaderTextureDefault getDefault(const std::vector<std::string>& enabledFeatures) const;
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

    // Whether the object casts shadows (manifest @state shadows=on|off). AUTO and ON leave it
    // to the mesh (MeshComponent::castShadows); OFF keeps the submesh out of the shadow maps.
    enum class ShaderShadowCasting{
        AUTO,
        ON,
        OFF
    };

    // the stencil keys a @state sets (ShaderManifestState::stencilSet), so a @state[...] rule
    // replaces only those over the base @state
    enum ShaderStencilFields : uint8_t{
        ShaderStencil_Compare   = 1 << 0, // stencil=
        ShaderStencil_Ref       = 1 << 1, // ref=
        ShaderStencil_ReadMask  = 1 << 2, // readmask=
        ShaderStencil_WriteMask = 1 << 3, // writemask=
        ShaderStencil_Pass      = 1 << 4, // pass=
        ShaderStencil_Fail      = 1 << 5, // fail=
        ShaderStencil_DepthFail = 1 << 6  // zfail=
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
        ShaderShadowCasting shadows = ShaderShadowCasting::AUTO;
        // stencil rule (phase 2): compare AUTO when the manifest has none (stencil off),
        // DISABLED for stencil=off; the other fields as given, else MaterialStencil's defaults
        MaterialStencil stencil;
        uint8_t stencilSet = 0; // ShaderStencilFields this state sets
        int colorWrite = -1;    // colorwrite=: MaterialColorWriteBits, -1 not set (all channels)
        // priority=<int>: an offset added to the material's (or component's) sortPriority, the
        // draw order within its queue, lower first (Unity's "Queue=Transparent-1"); 0 not set
        bool hasPriority = false;
        int priority = 0;

        // the values other sets (those it does not leave AUTO) replace these
        void apply(const ShaderManifestState& other);
        // whether it names a stencil test (stencil= other than off)
        bool hasStencil() const;
    };

    // "@state[<DEFINE>] key=value...": state that applies while a @feature or @keywords
    // define is on ("[!<DEFINE>]": while it is off), over the base @state
    struct JASPERENGINE_API ShaderManifestStateRule{
        std::string define;
        bool negate = false;
        ShaderManifestState state;
        int line = 0;
    };

    // "@require <inputs> feature=<DEFINE>": inputs required only while the define is on
    // ("feature=!<DEFINE>": while it is off)
    struct JASPERENGINE_API ShaderManifestRequireRule{
        uint32_t require = 0; // ShaderRequireFlags
        std::string define;
        bool negate = false;
        int line = 0;
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
        uint32_t require = 0;    // ShaderRequireFlags, whatever the features
        std::vector<ShaderManifestRequireRule> requireRules;
        ShaderManifestState state;
        std::vector<ShaderManifestStateRule> stateRules;
        std::vector<ShaderManifestProperty> properties;
        std::vector<ShaderManifestTexture> textures;
        std::vector<ShaderManifestFeature> features;
        std::vector<ShaderManifestKeywords> keywords;
        bool depth = false;      // has its own depth program
        ShaderRole role = ShaderRole::NONE; // @role
        // A fork's .frag defines LIT_STOCHASTIC_PARAMS (definesLitStochasticParams): a fork of
        // the built-in Lit that samples the material's Stochastic Tiling (includes/pbr.glsl).
        // Set by ShaderManifests::get for forks, from the entry point an export ships, so the
        // editor collecting variants and the game loading meshes decide alike
        // (RenderSystem::supportsStochasticTiling).
        bool litStochasticParams = false;
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
        // whether a row with feature= (a property's or texture's: "<DEFINE>", "!<DEFINE>" or
        // empty) applies to a variant with these enabled defines (resolveFeatures)
        static bool isRowShown(const std::string& feature, const std::vector<std::string>& enabled);
        // the default value of a member (manifest default), false when not declared
        bool getDefault(const std::string& member, Vector4& value) const;

        // Checks the @property members and @texture uniforms against what compiled
        // variants of the shader declare (one or several; a member behind a feature only
        // exists in the variants with it). Adds errors to diagnostics.
        void validateReflection(const std::vector<const ShaderData*>& variants, const std::string& file);

        // The state of a variant: @state, then the @state[...] rules whose condition holds for
        // the enabled defines (resolveFeatures), in order
        ShaderManifestState resolveState(const std::vector<std::string>& enabled) const;
        // The inputs a variant requires: @require, plus the feature= ones whose condition
        // holds for the enabled defines
        uint32_t resolveRequire(const std::vector<std::string>& enabled) const;

        // The scene inputs (ShaderRequire_SceneDepth / _SceneColor) some variant of the shader
        // may require: @require plus every feature= rule, whatever the features (the shader
        // picker's cost mark, list_shaders)
        uint32_t getPossibleSceneInputs() const;
        // Every input some variant of the shader may require: @require plus every feature= rule
        uint32_t getPossibleRequire() const;

        // the SURFACE_* defines a variant compiles with (one per required input, see
        // includes/surface_vs.glsl), for its enabled defines
        std::vector<std::string> getRequireDefines(const std::vector<std::string>& enabled) const;
        static std::vector<std::string> getRequireDefines(uint32_t require);

        // the @require input names of a mask: position, normals, tangents, uv1, uv2, color,
        // rawcolor, smoothnormals, scenedepth, scenecolor, environment, normalmap (in that order)
        static std::vector<std::string> getRequireNames(uint32_t require);

        // the source declares manifest lines (a fork the runtime needs the file of)
        static bool hasManifestLines(const std::string& source);
        // a line "#define LIT_STOCHASTIC_PARAMS ..." outside comments: the source names the
        // material's Stochastic Tiling values for includes/pbr.glsl
        static bool definesLitStochasticParams(const std::string& source);
        static std::string getLightingModeName(ShaderLightingMode mode);
        static std::string getShaderTypeToken(ShaderType type);

        // @state stencil and colour write values (section 2.1 of the phase 2 spec), for the
        // editor, MCP and checks: names as the manifest writes them
        // stencil=: off, never, less, equal, lequal, greater, notequal, gequal, always
        // (AUTO: "auto")
        static const char* getStencilCompareName(MaterialStencilCompare compare);
        static bool parseStencilCompare(const std::string& name, MaterialStencilCompare& compare);
        // pass= / fail= / zfail=: keep, zero, replace, incr, decr, invert, incrwrap, decrwrap
        static const char* getStencilOpName(MaterialStencilOp op);
        static bool parseStencilOp(const std::string& name, MaterialStencilOp& op);
        // colorwrite=: "none", or the channels in rgba order ("rgba", "rgb", "a", "rg"...);
        // -1: "auto"
        static std::string getColorWriteName(int colorWrite);
        // any subset of the letters r, g, b, a in any order, or none; -1 on error
        static int parseColorWrite(const std::string& name);
    };

    // A shader reference string (section 3.1): "" built-in, "library:<name>" an engine
    // library shader, anything else a project fork. A variant's reference may end in
    // "#A,B" (its enabled feature defines, sorted), which compiles them as #defines.
    // "builtin:lit#A,B" is a variant of the built-in Lit with defines its property bits have
    // no room for (the extended lobes, hex tiling, the High Lit Quality: MaterialPbr); it is
    // only made by the renderer, never set on a material.
    class JASPERENGINE_API ShaderReference{
    public:
        static const char* const LIBRARY_PREFIX; // "library:"
        static const char* const BUILTIN_PREFIX; // "builtin:"
        static const char* const BUILTIN_LIT;    // "builtin:lit"

        static bool isLibrary(const std::string& reference);
        // "builtin:..." (the built-in shader with defines)
        static bool isBuiltin(const std::string& reference);
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
        // the defines of getVariantReference, resolved (empty without a manifest)
        static std::vector<std::string> getEnabledFeatures(const std::string& reference, const std::vector<std::string>& shaderFeatures);
        // The reference a colour variant of a shader with a manifest is keyed by: its
        // features plus the define of the blend mode its pipeline resolves to (blendMode:
        // the material's or component's, AUTO resolving to the manifest's @state, then to
        // the legacy alpha blend), BLEND_NONE, BLEND_ALPHA, BLEND_PREMULTIPLIED,
        // BLEND_ADDITIVE or BLEND_MULTIPLY. getVariantReference for shaders without one.
        static std::string getColorVariantReference(const std::string& reference, const std::vector<std::string>& shaderFeatures, MaterialBlendMode blendMode);
        // BLEND_* of a resolved blend mode (AUTO: the legacy alpha blend)
        static const char* getBlendDefine(MaterialBlendMode blendMode);
        static bool isBlendDefine(const std::string& define);
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
        // a cube map for a @texture ... cube uniform: one image holding the six faces (a cross
        // or a strip, as a Skybox takes it; an empty path removes it), or six face images
        // (front, back, left, right, up, down)
        static bool setCubeTexture(std::vector<std::pair<std::string, Texture>>& textures, const std::string& uniform, const std::string& path);
        static bool setCubeTextureFaces(std::vector<std::pair<std::string, Texture>>& textures, const std::string& uniform,
                                        const std::string& front, const std::string& back, const std::string& left,
                                        const std::string& right, const std::string& up, const std::string& down);
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
        // Reads a fork file (source or include) with that reader: the runtime compiler's too
        static bool readForkFile(const std::string& path, std::string& content);
        // Forgets the fork manifests (their sources changed); library ones stay
        static void clearForks();
        // Only the forks `which` names (a hot reload of some shader files, S05)
        static void clearForks(const std::function<bool(const std::string& fork)>& which);

        // the library shaders the build has, by name, sorted
        static std::vector<std::string> getLibraryShaderNames();
        // manifest lines of the library entry files, by library path ("library/toon.frag")
        static const std::unordered_map<std::string, std::string>& getLibraryManifestSources();
        static bool hasLibraryFile(const std::string& path);
    };

}

#endif //SHADERMANIFEST_H
