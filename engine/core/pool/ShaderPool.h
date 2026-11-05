// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#ifndef SHADERPOOL_H
#define SHADERPOOL_H

#include "Engine.h"
#include "render/ShaderRender.h"
#include "shader/ShaderBuildTypes.h"
#include <map>
#include <memory>
#include <functional>
#include <utility>
#include <vector>

namespace jasperengine{

    typedef std::map<ShaderKey, std::shared_ptr<ShaderRender>> shaders_t;

    using ShaderBuilderFn = std::function<ShaderBuildResult(ShaderKey)>;
    // how many variants the builder is still building (off the main thread)
    using ShaderBuildsPendingFn = std::function<size_t()>;

    // the exclusive variants of ShaderType::HTMLUI_POST, in property-bit order
    // YUV: <video> frames (phase 6), appended
    enum class HtmlUIPostVariant { PASSTHROUGH, COLOR_MATRIX, BLEND_MASK, DROP_SHADOW, BLUR, UNPREMULTIPLY, YUV };
    static const int HTMLUI_POST_VARIANT_COUNT = 7;

    // what a particle system draw needs of its variant (ShaderType::PARTICLES)
    struct ParticlesShaderFlags {
        bool texture = false;        // HAS_TEXTURE: the sprite sheet
        bool mesh = false;           // PARTICLE_MESH: mesh particles (the mesh's vertices, a rotation per instance)
        bool trail = false;          // PARTICLE_TRAIL: trail and ribbon strips
        bool stretch = false;        // STRETCH: stretched billboards
        bool frameBlend = false;     // FRAME_BLEND: the next sprite frame blended in
        bool lit = false;            // LIT: per-vertex lighting
        bool sixWay = false;         // SIXWAY: six-way smoke lighting
        bool customData = false;     // CUSTOM_DATA: the Custom Data stream
        bool receiveShadows = false; // RECEIVE_SHADOWS: the main light's shadow
        bool fog = false;            // HAS_FOG: the scene fog
    };

    class JASPERENGINE_API ShaderPool{
    private:
        static shaders_t& getMap();

        static ShaderBuilderFn shaderBuilderFn;
        static ShaderBuildsPendingFn shaderBuildsPendingFn;

        static std::string getShaderName(const std::string& shaderStr);

        static void addMissingShader(const std::string& shaderStr);

        // Loads the variant from what the build ships: the embedded headers, then the
        // shader directory
        static bool loadPrecompiledShader(ShaderKey shaderKey, const std::string& shaderStr, ShaderData& shaderData);

    public:
        // ShaderKey (shader/ShaderBuildTypes.h): the ShaderType, a 64-bit properties
        // bitfield and the customShaderId (0 = built-in)
        static ShaderKey getShaderKey(ShaderType shaderType, uint64_t properties, uint16_t customId = 0);
        // A key in the packed uint64 layout files and tools stored before the properties
        // were widened to 64 bits (customId << 48 | ShaderType << 32 | 32 property bits)
        static ShaderKey unpackLegacyKey(uint64_t packed);
        // The key as text, for tools: the legacy packed hex ("0x10000001f", no leading
        // zeros) while the properties fit in 32 bits, so a key reads as it always did;
        // else "0x" and 24 hex digits, customId (4), type (4) and properties (16).
        // parseShaderKeyText reads both (and a decimal packed key).
        static std::string getShaderKeyText(ShaderKey key);
        static bool parseShaderKeyText(const std::string& text, ShaderKey& key);

        // A build without the runtime compiler (or with it off) that lacks a variant draws
        // with the nearest one it ships (a warning names both); none: reported missing
        static std::shared_ptr<ShaderRender> get(ShaderType shaderType, uint64_t properties, uint16_t customId = 0);
        // The variants a missing one falls back to, nearest first: its optional features (SSAO,
        // fog, 2D shadows, shadows, mirror, IBL, normal map, tangents, UV sets; depth: alpha
        // mask, texture; G-buffer: metallic-roughness, base colour texture) given up, the
        // fewest first. Never a bit that changes a uniform block the draw uploads by its key.
        static std::vector<uint64_t> getFallbackProperties(ShaderType shaderType, uint64_t properties);
        static void remove(ShaderType shaderType, uint64_t properties, uint16_t customId = 0);

        // True when the last get() attempt failed, either building the source or creating
        // the backend resource, or (a game without the editor's builder) the build does not
        // have the precompiled shader. The pool stops re-invoking the (expensive, error-spamming)
        // builder until the failure is cleared by remove()/destroyCustomShaders().
        static bool isShaderBuildFailed(ShaderType shaderType, uint64_t properties, uint16_t customId = 0);

        // ---- Warm-up (Unity: ShaderVariantCollection.WarmUp, Shader.WarmupAllShaders) ----
        // A variant is ready once its GPU program exists. Asking for one that is not
        // starts what it takes: loading it from the build, or compiling it at runtime
        // (in the background, see RuntimeShaderCompiler). Call again (once a frame, say)
        // until it reports ready; nothing that draws with it waits for it.

        // Requests the variant; true once it is ready
        static bool warmUp(ShaderType shaderType, uint64_t properties, uint16_t customId = 0);
        static bool warmUp(ShaderKey shaderKey);
        // Ready, without requesting it
        static bool isShaderReady(ShaderType shaderType, uint64_t properties, uint16_t customId = 0);
        // NotStarted (never requested), Loading, Finished (ready) or Failed (not in the
        // build and not compilable, or it failed to compile or to create)
        static ResourceLoadState getShaderState(ShaderKey shaderKey);
        // Requests every built-in variant the build ships precompiled and every one a
        // previous run compiled into the runtime shader cache; how many are not ready yet
        static size_t warmUpAllShaders();
        // built-in variants the build ships precompiled for this backend
        static std::vector<ShaderKey> getPrecompiledShaderKeys();
        // The key of the variant whose GPU program has this backend handle id (the warm-up's
        // recording, spec S32); false when none has it
        static bool findKeyOfHandle(uint32_t handleId, ShaderKey& key);
        // A file the build embeds next to its shaders in the shader headers (the warm-up list,
        // "warmup"); empty when the build embeds none of that name
        static std::string getEmbeddedData(const std::string& name);

        // variants being compiled at runtime, or whose GPU program is being created (spec S32)
        static size_t getPendingShaderCount();
        // pipelines being created (spec S32: off the render thread on Vulkan and Metal)
        static size_t getPendingPipelineCount();
        // no shader, pipeline or GL warm draw is being created
        static bool isGpuCreationIdle();
        // nothing a draw waits for is being made: no variant compiling at runtime or building in
        // the shader builder, and the GPU creation idle. What a screenshot waits for (spec S32: a
        // captured frame never shows a stand-in)
        static bool isCreationIdle();

        // ---- Runtime compilation of variants the build lacks (RuntimeShaderCompiler) ----
        // The build carries the compiler (export setting "Runtime shader compilation")
        static bool isRuntimeCompilationSupported();
        // On by default where supported; off, a missing variant is reported and skipped
        static void setRuntimeCompilationEnabled(bool enabled);
        static bool isRuntimeCompilationEnabled();
        // Compile on a worker thread (default; not on the web) or on the main thread,
        // one variant a frame: a hitch instead of the object showing up late
        static void setAsyncCompilation(bool async);
        static bool isAsyncCompilation();

        // "type[:Prop,Prop...]" or "type:<mask>", as in Build Settings and the editor's
        // shaders command ("mesh:Ult,Vc4", "depth:Ski", "mesh:0x41"). A custom shader is
        // given apart (customShader: its project-relative base path, "" for built-in).
        // UI effects (phase 5) name their effect: "uieffect/<name>[:Flt]" (a built-in effect of
        // the engine library, else a project effect the effect registry knows)
        static bool parseShaderSpec(const std::string& spec, ShaderKey& shaderKey, std::string* error = nullptr, const std::string& customShader = "");
        // parseShaderSpec, plus "uieffect/all": every effect of the engine library, a paint
        // effect in both forms, a filter (or internal) effect in the filter form
        static bool expandShaderSpec(const std::string& spec, std::vector<ShaderKey>& shaderKeys, std::string* error = nullptr);
        // a built-in getShaderStr() back into its key (UI effects: library effects, and project
        // effects through the custom shader name resolver)
        static bool parseShaderStr(const std::string& shaderStr, ShaderKey& shaderKey);
        // a UI effect's "<basename>[_<properties>]" (the part of its shader string after
        // "uieffect_") into its custom shader and properties (Flt, Stp, Sct); false when the
        // basename names no effect
        static bool splitUiEffectShaderStr(const std::string& basenameAndProperties, std::string& customShader, uint64_t& properties);

        static std::string getShaderStr(ShaderType shaderType, uint64_t properties, uint16_t customId = 0);
        static std::string getShaderTypeName(ShaderType shaderType, bool lowerCase = false);
        static bool parseShaderTypeToken(const std::string& typeToken, ShaderType& shaderType);
        static int getShaderPropertyCount(ShaderType shaderType);
        // the variant bit of the post-processing colour (USE_POST_COLOR, includes/post_color.glsl:
        // phase 4's in-shader tier) of a shader type, 0 for a type that never applies it (UI,
        // depth, effects)
        static uint64_t getPostColorProperty(ShaderType shaderType);
        // 'Pxs': the pixel-snapped vertex stage of MESH, DEPTH, UI and PARTICLES (S41); 0 for the others
        static uint64_t getPixelSnapProperty(ShaderType shaderType);
        // the mesh variant bit of the High and Ultra shadow filters (USE_SHADOWS_HQ: the Vogel
        // disk, PCSS and the raw-depth view of the atlas; docs/rendering/shadows.md), set on a
        // variant that receives shadows when the shadow tier is High or above (MESH bit 33)
        static uint64_t getShadowsHQProperty();
        // the variant bit of a Vertex Animation (includes/vat.glsl; HAS_VAT, with `bones`
        // HAS_VAT_BONES) of MESH, DEPTH and GBUFFER, 0 for another type; getVatProperties: both
        static uint64_t getVatProperty(ShaderType shaderType, bool bones);
        // MESH's LOD cross-fade bit ('Lfd', spec S54): the dithered variant of fading draws
        static uint64_t getLodFadeProperty();
        static uint64_t getVatProperties(ShaderType shaderType);
        // the variant bit of automatic GPU instancing (HAS_AUTO_INSTANCING: the object block per
        // instance, spec S55) of MESH and DEPTH, 0 for another type
        static uint64_t getAutoInstancingProperty(ShaderType shaderType);
        // S57's MESH bits: 'Til' (Light Mode Tiled), 'Cap' (capsule shadows), 'Cts' (contact
        // shadows) and 'Oit' (an alpha-blended submesh's OIT accumulation render)
        enum class LightingFeature { TILED_LIGHTS, CAPSULE_SHADOWS, CONTACT_SHADOWS, OIT };
        static uint64_t getLightingProperty(LightingFeature feature);
        static std::string getShaderPropertyName(ShaderType shaderType, int bit, bool shortName = true);
        static ShaderType getShaderTypeFromKey(ShaderKey key);
        static uint64_t getPropertiesFromKey(ShaderKey key);
        static uint16_t getCustomIdFromKey(ShaderKey key);

        // Key used to validate a serialized .sdat/header blob. The customShaderId is a
        // session-local registry value (assigned in registration order) and is NOT
        // stable between the editor that exported the shader and the standalone runtime
        // that loads it. The unique filename already identifies the forked source, so
        // the on-disk validation key drops customId and keeps only type+properties.
        static ShaderKey getStorageKey(ShaderKey key);

        // Bitmask of the property positions that currently map to a real variant for
        // this shader type (derived from getShaderPropertyName, so retired/reserved
        // bits like MESH bit 5 are excluded). Used to normalize keys. A new property is
        // a case in getShaderPropertyName and a getShaderPropertyCount that covers it
        // (up to SHADER_PROPERTY_BITS); bits 32..63 work like the others.
        static uint64_t getValidPropertyMask(ShaderType shaderType);

        // Strips property bits that no longer map to a variant (retired features,
        // bits beyond the current count). Keeps type + customId. Persisted keys from
        // older editor versions collapse onto their current equivalent, so stale
        // variants stop being exported and looked up.
        static ShaderKey normalizeKey(ShaderKey key);

        // Custom (user-forked) shader registry. Interns a project-relative base path
        // (e.g. "shaders/myMesh", no extension) and returns a stable session id used
        // in the ShaderKey; an empty path returns 0 (built-in). The derived (sanitized)
        // basename is what makes editor build, disk cache, export and shipping-runtime
        // .sdat filenames line up. Thread-safe (ShaderBuilder runs on a thread pool).
        static uint16_t registerCustomShader(const std::string& baseName);
        static std::string getCustomShaderName(uint16_t customId);     // project-relative base, "" if none
        static std::string getCustomShaderBasename(uint16_t customId); // filesystem-safe token, "" if none
        // A custom name starting with '@' is a library id: its sources are in the engine
        // library, not in the project ("@effects/<name>", the UI effects of phase 5; basename
        // fx_<name>, '-' as '_'). Its variants are built-ins for caching, export and reload.
        static bool isLibraryCustomShader(uint16_t customId);
        // Project custom shaders whose basename cannot be mapped back by itself (a UI effect's
        // .frag): the effect registry resolves them (parseShaderStr, getPrecompiledShaderKeys)
        static void setCustomShaderNameResolver(std::function<std::string(const std::string& basename)> resolver);
        // the custom name of a UI effect named in a spec ("uieffect/<name>"), set by the effect
        // registry; "" when unknown (the library's own effects resolve without it)
        static void setUiEffectResolver(std::function<std::string(const std::string& effectName)> resolver);
        // the custom name a basename stands for ("" when unknown)
        static std::string resolveCustomShaderBasename(const std::string& basename);
        // "@effects/<name>" or a project effect's .frag for a UI effect name ("" when unknown)
        static std::string resolveUiEffect(const std::string& effectName);
        // the effect name of a UI effect's custom name (a project .frag: its stem)
        static std::string getUiEffectName(const std::string& customShader);
        // Bumped when the custom shader registry is reset (clear, clearUnused): every id
        // registered before is stale and must be registered again
        static uint32_t getCustomShaderGeneration();

        // Frees the GPU handle of every cached forked-shader variant (library ids excepted)
        // so the next get() rebuilds (used by the editor after a shader source is edited). The pool keeps
        // the ShaderRender objects, so meshes holding them stay valid and re-bind on
        // their needReload pass.
        static void destroyCustomShaders();
        // Only the forks `which` names by their custom shader name (a hot reload, S05)
        static void destroyCustomShaders(const std::function<bool(const std::string& name)>& which);

        static const std::vector<ShaderBackend>& getShaderBackends();
        // backend this runtime was built for
        static ShaderBackend getShaderBackend();
        static std::string getShaderBackendName(ShaderBackend backend);      // "Metal (iOS)"
        static std::string getShaderBackendCliToken(ShaderBackend backend);  // "metal-ios"
        static bool parseShaderBackend(const std::string& value, ShaderBackend& out);

        static std::string getShaderLangStr();
        static std::string getShaderLangStr(ShaderBackend backend);
        static std::string getShaderLangStr(ShaderLang lang, int version, bool es = false, Platform platform = Platform::Linux);
        static bool getShaderCliSpec(const std::string& shaderStr, std::string& cliSpec);
        static std::string getSuggestedCliBackend();
        static std::string getMissingShadersCliArgs();
        static std::string getMissingShadersDisplayList();
        // (a copy: a model loading off the main thread may add to them)
        static std::vector<std::string> getMissingShaders();

        // the builder of the variants (the editor's), and how many it is still building
        static void setShaderBuilder(ShaderBuilderFn fn, ShaderBuildsPendingFn pending = nullptr);

        static uint64_t getMeshProperties(bool unlit, bool uv1, bool uv2,
						bool punctual, bool shadows, bool normals, bool normalMap,
						bool tangents, bool vertexColorVec3, bool vertexColorVec4, bool textureRect,
                        bool fog, bool skinning, bool morphTarget, bool morphNormal, bool morphTangent,
                        bool terrain, bool instanced, bool ibl, bool mirror, bool ssao = false,
                        bool light2d = false, bool shadows2d = false,
                        bool alphaMask = false, bool alphaOpaque = false, bool instanceFade = false,
                        bool terrainPBR = false,
                        bool vertexLights = false, int lightCapacity = 8, bool stochasticTiling = false,
                        bool lattice = false, bool vat = false, bool vatBones = false);
        // normals, vertexColorVec3/4 and uv1: the inputs a library depth program requires
        // (vertexColorVec3/4 also a lattice's vertex colour mask); textureRect: the sprite
        // texture rect its UV (texture or uv1) goes through
        static uint64_t getDepthMeshProperties(bool texture, bool skinning, bool morphTarget, bool morphNormal, bool morphTangent, bool terrain, bool instanced, bool alphaMask = false, bool instanceFade = false,
                                               bool normals = false, bool vertexColorVec3 = false, bool vertexColorVec4 = false, bool uv1 = false,
                                               bool textureRect = false, bool stochasticTiling = false, bool lattice = false,
                                               bool vat = false, bool vatBones = false);
        // vertexColorVec3/4: the vertex colour a lattice's mask reads (set only with lattice)
        static uint64_t getGBufferMeshProperties(bool baseColorTexture, bool normals, bool skinning, bool morphTarget, bool morphNormal, bool morphTangent, bool terrain, bool instanced, bool metallicRoughnessTexture, bool terrainPBR = false,
                                                 bool stochasticTiling = false, bool lattice = false,
                                                 bool vertexColorVec3 = false, bool vertexColorVec4 = false,
                                                 bool vat = false, bool vatBones = false);
        // fontAtlasSdf: a Text with outline or shadow (with fontAtlasTexture only)
        static uint64_t getUIProperties(bool texture, bool fontAtlasTexture, bool vertexColorVec3, bool vertexColorVec4, bool fontAtlasSdf = false);
        static uint64_t getHtmlUIProperties(bool texture);
        // HTMLUI_POST: one property bit per (exclusive) variant
        static uint64_t getHtmlUIPostProperties(HtmlUIPostVariant variant);
        // every built-in variant an HTML UI document draws with (export and Build Settings
        // lists): HTMLUI untextured and textured, HTMLUI_GRADIENT, the seven HTMLUI_POST
        static std::vector<std::pair<ShaderType, uint64_t>> getHtmlUIVariants();
        // what a Video Player draws with: the HTMLUI_POST Yuv conversion and Passthrough blit
        static std::vector<std::pair<ShaderType, uint64_t>> getVideoPlayerVariants();
        static uint64_t getPointsProperties(bool texture, bool vertexColorVec3, bool vertexColorVec4, bool textureRect);
        // a particle draw's variant (the post-processing colour bit comes from getPostColorProperty)
        static uint64_t getParticlesProperties(const ParticlesShaderFlags& flags);
        // ribbon: a thick Line Renderer or a Trail Renderer ('Rib', LINE_RIBBON); texture: the
        // built-in shader samples its u_baseTexture ('Tex'); pixelWidth: widths in screen pixels
        // ('Pxw', LINE_PIXEL_WIDTH). The hairline is (false, true) as before
        static uint64_t getLinesProperties(bool vertexColorVec3, bool vertexColorVec4, bool ribbon = false, bool texture = false, bool pixelWidth = false);

		// necessary for engine shutdown
		static void clear();
		// Destroys shaders no one else references and resets per-project state
		// (missing/failed lists, custom shader registry)
		static void clearUnused();
    };
}

#endif /* SHADERPOOL_H */
