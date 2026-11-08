// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#ifndef MESH_COMPONENT_H
#define MESH_COMPONENT_H

#include "Engine.h"
#include "util/GrowArray.h"
#include "util/StableArray.h"
#include "math/Vector3.h"
#include "math/Quaternion.h"
#include "math/AABB.h"
#include "buffer/Buffer.h"
#include "render/ObjectRender.h"
#include "render/TextureRender.h"
#include "texture/TextureData.h"
#include "texture/Material.h"
#include "shader/ShaderUniforms.h"
#include "shader/ShaderManifest.h"
#include "buffer/InterleavedBuffer.h"
#include "buffer/IndexBuffer.h"
#include "buffer/ExternalBuffer.h"
#include "pool/MeshLodPool.h"
#include "render/LodFade.h"
#include "math/Rect.h"
#include "lattice/LatticeTypes.h"
#include <deque>
#include <map>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace jasperengine{

    namespace vat{ class VertexAnimation; }

    // Submesh fields a model load rewrites from its source file, so an edit to one only survives
    // the load while it is flagged in Submesh::overrideFields (64 bits, saved as a number).
    enum SubmeshOverrideFlags : uint64_t {
        SubmeshOverride_BaseColorFactor          = 1 << 0,
        SubmeshOverride_MetallicFactor           = 1 << 1,
        SubmeshOverride_RoughnessFactor          = 1 << 2,
        SubmeshOverride_AlphaCutoff              = 1 << 3,
        SubmeshOverride_EmissiveFactor           = 1 << 4,
        SubmeshOverride_AlphaMode                = 1 << 5,
        SubmeshOverride_MaterialName             = 1 << 6,
        SubmeshOverride_BaseColorTexture         = 1 << 7,
        SubmeshOverride_EmissiveTexture          = 1 << 8,
        SubmeshOverride_MetallicRoughnessTexture = 1 << 9,
        SubmeshOverride_OcclusionTexture         = 1 << 10,
        SubmeshOverride_NormalTexture            = 1 << 11,
        SubmeshOverride_FaceCulling              = 1 << 12,
        SubmeshOverride_TextureShadow            = 1 << 13,
        SubmeshOverride_PrimitiveType            = 1 << 14,
        SubmeshOverride_FoliageTransmission      = 1 << 15,
        // shader library fields of the material: a model file never sets them, so a load
        // only keeps them through these bits
        SubmeshOverride_Shader                   = 1 << 16,
        SubmeshOverride_ShaderParams             = 1 << 17,
        SubmeshOverride_ShaderTextures           = 1 << 18,
        SubmeshOverride_ShaderFeatures           = 1 << 19,
        SubmeshOverride_BlendMode                = 1 << 20,
        SubmeshOverride_CullMode                 = 1 << 21,
        SubmeshOverride_DepthWrite               = 1 << 22,
        SubmeshOverride_DepthTest                = 1 << 23,
        SubmeshOverride_DepthOffset              = 1 << 24,
        SubmeshOverride_SortPriority             = 1 << 25,
        SubmeshOverride_Lighting                 = 1 << 26,
        // shader library phase 2: material.stencil, material.colorWrite and the whole extraPass
        SubmeshOverride_Stencil                  = 1 << 27,
        SubmeshOverride_ColorWrite               = 1 << 28,
        SubmeshOverride_ExtraPass                = 1 << 29,
        // Stochastic Tiling: material.stochasticTiling and its four values under one bit
        // (upstream's, in released files)
        SubmeshOverride_StochasticTiling         = 1ull << 30,
        // the extended lobes and hex tiling (Material::pbr, all of it) and the emissive strength,
        // which a glTF model sets (KHR_materials_*; S11's, after Stochastic Tiling's bit)
        SubmeshOverride_PbrExtensions            = 1ull << 31,
        SubmeshOverride_EmissiveStrength         = 1ull << 32,
        // material.shaderGlobalBindings (shader globals; upstream's 1 << 31 is S11's PbrExtensions
        // here, so it comes after S11's bits)
        SubmeshOverride_ShaderGlobalBindings     = 1ull << 33,
        // material.shaderPalettes (S43 Palette Swap)
        SubmeshOverride_ShaderPalettes           = 1ull << 34,

        SubmeshOverride_ShaderLibrary = SubmeshOverride_Shader | SubmeshOverride_ShaderParams |
                                        SubmeshOverride_ShaderTextures | SubmeshOverride_ShaderFeatures |
                                        SubmeshOverride_ShaderGlobalBindings | SubmeshOverride_ShaderPalettes |
                                        SubmeshOverride_BlendMode | SubmeshOverride_CullMode |
                                        SubmeshOverride_DepthWrite | SubmeshOverride_DepthTest |
                                        SubmeshOverride_DepthOffset | SubmeshOverride_SortPriority |
                                        SubmeshOverride_Lighting | SubmeshOverride_Stencil |
                                        SubmeshOverride_ColorWrite | SubmeshOverride_ExtraPass,

        SubmeshOverride_Material = SubmeshOverride_BaseColorFactor | SubmeshOverride_MetallicFactor |
                                   SubmeshOverride_RoughnessFactor | SubmeshOverride_AlphaCutoff |
                                   SubmeshOverride_EmissiveFactor | SubmeshOverride_AlphaMode |
                                   SubmeshOverride_MaterialName | SubmeshOverride_BaseColorTexture |
                                   SubmeshOverride_EmissiveTexture | SubmeshOverride_MetallicRoughnessTexture |
                                   SubmeshOverride_OcclusionTexture | SubmeshOverride_NormalTexture |
                                   SubmeshOverride_FoliageTransmission | SubmeshOverride_ShaderLibrary |
                                   SubmeshOverride_StochasticTiling | SubmeshOverride_PbrExtensions |
                                   SubmeshOverride_EmissiveStrength
    };

    // The lights reaching a mesh (an instanced batch counts as one mesh), picked by
    // RenderSystem under the scene's maxLightsPerObject: the ones it is lit by, and the rest
    // folded into an L1 spherical-harmonics ambient. Picked again only when the mesh or a
    // light reaching it changes.
    struct MeshLights{
        uint16_t lights[MAX_LIGHTS_PER_OBJECT] = {}; // scene light indices, ascending
        unsigned int count = 0;
        // per channel, rgb_c(n) = dot(sh[c].xyz, n) + sh[c].w: the scene ambient plus the
        // reaching lights over the cap, evaluated at the bounds' center
        Vector4 sh[3];
        // what they were picked for
        AABB bounds;
        uint32_t layerBit = 0;
        uint64_t reachMask = 0; // bit (index % 64) of each scene light that reached the mesh
        uint64_t epoch = 0;     // RenderSystem's light list epoch, 0 = not picked yet
        // a directional light went into the SH (more directional lights than the cap)
        bool foldedDirectional = false;
        // picked for a directional-only ('Dlo') variant: its point and spot lights went into the SH
        bool directionalOnly = false;
    };

    // The extra draw of a submesh whose material has an extra pass (Material::extraPass,
    // shader library phase 2): its own shader, pipelines, uniform blocks and textures, built
    // by RenderSystem::loadMesh and drawn by drawMesh before (Before passes, ahead of all the
    // mesh's submeshes) or after (After passes, after all of them) the mesh's main draws. A
    // submesh without an extra pass has none (Submesh::extraPass is null).
    // The OIT accumulation render of an alpha-blended submesh (spec S57 R4): its colour variant
    // with USE_OIT ('Oit'), drawn on the submesh's own vertex layout and bindings
    // (ObjectRender::loadAlike) into the camera's accumulation targets
    struct JASPERENGINE_API SubmeshOit{
        ObjectRender render;
        std::shared_ptr<ShaderRender> shader;
        uint64_t shaderProperties = 0;
        uint16_t customShaderId = 0;
        int slotFSOit = -1;
        bool loaded = false;
        // its variant failed to build or does not match the colour variant's blocks: Sorted
        bool failed = false;
    };

    // the records form's tail in u_fs_objectLights (includes/lights.glsl LIGHT_DATA_TAIL): the
    // byte offsets of its members, -1 where the variant has none
    struct LightDataTail{
        int16_t bases = -1;       // lightDataBases
        int16_t occlusion = -1;   // contactViewport, contactParams, capsuleParams
        int16_t lights2D = -1;    // lights2DAmbient, lights2DAtlasInfo
    };

    struct JASPERENGINE_API SubmeshExtraPass{
        ObjectRender render;
        std::shared_ptr<ShaderRender> shader;
        uint64_t shaderProperties = 0; // ShaderType::MESH
        uint16_t customShaderId = 0;   // the pass's shader reference, registered
        MaterialPassOrder order = MaterialPassOrder::AFTER;

        // the manifest of its shader (null for a fork without one) and its resolved @state
        std::shared_ptr<const ShaderManifest> manifest;
        ShaderManifestState manifestState;
        // what its variant reads of the scene (ShaderRequire_SceneDepth / _SceneColor)
        uint32_t sceneInputs = 0;
        ShaderLibraryBindings libraryBindings;
        // filled from MaterialPass::shaderParams, else the manifest defaults
        CustomUniformBlock customVSParams;
        CustomUniformBlock customFSParams;

        int slotVSParams = -1;
        // reflected: a pixel-snapped variant's block ends with pixelSnapDraw (S41), a fork's may not
        unsigned int sizeVSParams = 0;
        int slotFSParams = -1;
        unsigned int sizeFSParams = 0; // reflected, read by the Stochastic Tiling ('Sto') variants
        int slotFSTexCoordSets = -1;
        int slotVSFade = -1;
        int slotFSFog = -1;
        int slotFSLighting = -1;
        int slotVSObjectLights = -1;
        int slotFSObjectLights = -1;
        unsigned int sizeVSObjectLights = 0;
        unsigned int sizeFSObjectLights = 0;
        std::pair<int, int> slotLightRecords = {-1, -1};
        LightDataTail lightDataTail;
        int slotFSLitFrame = -1;
        unsigned int sizeFSLitFrame = 0;
        int slotFSLighting2D = -1;
        int slotVSSprite = -1;
        int slotVSSkinning = -1;
        int slotVSMorphTarget = -1;
        int slotVSLattice = -1;
        std::pair<int, int> slotVSLatticeTexture = {-1, -1};
        int slotVSVat = -1;
        std::pair<int, int> slotVSVatTexture = {-1, -1};

        bool loaded = false;
        // its shader failed to build: the submesh draws without it
        bool failed = false;
        bool needUpdateTexture = false;
    };

    // a_smoothNormal of a mesh (manifest @require smoothnormals): per vertex of a position
    // attribute, the normal averaged over the vertices sharing its position. Made at load,
    // only for a mesh whose shaders ask for it, and kept while the geometry is the same.
    struct JASPERENGINE_API MeshSmoothNormals{
        struct Entry{
            uint64_t key = 0;             // hash of the positions, normals and indices it is made of
            std::vector<int8_t> data;     // 4 signed normalized bytes per vertex (xyz, 0)
            BufferRender buffer;
            bool created = false;         // buffer asked for (an async load creates it later)
            bool used = false;            // by a submesh of the load that is running
        };
        // a deque: adding an entry moves no other, whose data an async buffer creation may
        // still be about to read
        std::deque<Entry> entries;
    };

    // The indices of a submesh's triangles over the first vertex of each position
    // (meshopt_generateShadowIndexBuffer): a depth program reading positions only draws the same
    // triangles fetching and shading fewer vertices where the mesh splits its vertices for
    // normals or UVs. Made when they reference more than 10% fewer vertices; count 0 otherwise.
    // It owns its GPU buffer: the last submesh holding it destroys it (a BufferRender handle
    // frees nothing on its own).
    struct JASPERENGINE_API ShadowIndexBuffer{
        std::vector<unsigned char> bytes;
        ExternalBuffer buffer;
        unsigned int count = 0;

        ShadowIndexBuffer() = default;
        ShadowIndexBuffer(const ShadowIndexBuffer&) = delete;
        ShadowIndexBuffer& operator=(const ShadowIndexBuffer&) = delete;
        // off the main thread the destroy is posted there (MainThreadQueue)
        ~ShadowIndexBuffer();
    };

    // Automatic GPU instancing (spec S55): the HAS_AUTO_INSTANCING variants of a submesh's render
    // and depthRender (RenderSystem::loadSubmeshBatchVariants), which draw batches of its draws
    // with those of every submesh like it, linked to render and depthRender (else they draw
    // nothing). Held only while GPU Instancing is on and the submesh can batch (Submesh::batch).
    // It owns them: its last holder (a copy of the component included) frees the pipelines and
    // lets go of the shaders' pool entries.
    struct JASPERENGINE_API SubmeshBatchVariants{
        ObjectRender render;
        ObjectRender depthRender;
        std::shared_ptr<ShaderRender> shader;
        std::shared_ptr<ShaderRender> depthShader;
        uint64_t shaderProperties = 0;
        uint64_t depthShaderProperties = 0;
        uint16_t shaderId = 0;
        uint16_t depthShaderId = 0;

        SubmeshBatchVariants() = default;
        SubmeshBatchVariants(const SubmeshBatchVariants&) = delete;
        SubmeshBatchVariants& operator=(const SubmeshBatchVariants&) = delete;
        // releases them; off the main thread the release is posted there (MainThreadQueue)
        ~SubmeshBatchVariants();
        // frees them now, leaving it empty (main thread)
        void release();
    };

    struct JASPERENGINE_API Submesh{
        Material material;
        std::map<AttributeType, Attribute> attributes;

        ObjectRender render;
        ObjectRender depthRender;
        ObjectRender gbufferRender;

        std::shared_ptr<ShaderRender> shader;
        std::shared_ptr<ShaderRender> depthShader;
        std::shared_ptr<ShaderRender> gbufferShader;

        uint64_t shaderProperties = 0;
        uint64_t depthShaderProperties = 0;
        uint64_t gbufferShaderProperties = 0;

        // resolved ids of MeshComponent::customShader/customDepthShader (0 = built-in);
        // cached here so the matching ShaderPool::remove uses the same key
        uint16_t customShaderId = 0;
        uint16_t customDepthShaderId = 0;
        // the built-in Lit's defines its colour variant was loaded with (the "builtin:lit#..."
        // reference, SubmeshShaderVariant::litDefines, as MaterialPbr::getDefineBits) and the
        // extension maps that variant binds, in slot order (u_pbrExtTexture0..2; COUNT: none)
        uint32_t litDefineBits = 0;
        PbrMap pbrMaps[MaterialPbr::MAX_EXTENSION_MAPS] = {PbrMap::COUNT, PbrMap::COUNT, PbrMap::COUNT};

        int slotVSParams = -1;
        // reflected: a pixel-snapped variant's block ends with pixelSnapDraw (S41), a fork's may not
        unsigned int sizeVSParams = 0;
        int slotFSParams = -1;
        // reflected, read by the Stochastic Tiling ('Sto') variants: the material prefix with
        // its stochastic vec4 (16 floats), or less from a fork made before it
        unsigned int sizeFSParams = 0;
        int slotFSTexCoordSets = -1;
        int slotVSFade = -1;
        int slotFSLighting = -1;        // legacy u_fs_lighting of an older fork
        int slotVSObjectLights = -1;    // includes/lights.glsl
        int slotFSObjectLights = -1;
        unsigned int sizeVSObjectLights = 0; // reflected: the vertex stage's is a prefix of the fragment stage's
        unsigned int sizeFSObjectLights = 0;
        int slotFSLitFrame = -1;        // includes/lit_frame.glsl (built-in Lit)
        unsigned int sizeFSLitFrame = 0; // reflected: the prefix the variant declares
        int slotFSReflectionProbe = -1;
        int slotFSLighting2D = -1;
        int slotFSFog = -1;
        int slotFSMirror = -1;
        int slotVSSprite = -1;
        int slotVSShadows = -1;
        int slotFSShadows = -1;
        int slotFSPointShadows = -1;
        int slotFSShadowData = -1;        // u_fs_shadow_data (includes/shadow_data.glsl)
        int slotFSPointShadowData = -1;   // u_fs_point_shadow_data
        int slotVSSkinning = -1;
        int slotVSMorphTarget = -1;
        // u_vs_lattice and u_latticeTexture (includes/lattice.glsl), loaded with a lattice variant
        int slotVSLattice = -1;
        std::pair<int, int> slotVSLatticeTexture = {-1, -1};
        // u_vs_vat and u_vatTexture (includes/vat.glsl), loaded with a Vertex Animation variant
        int slotVSVat = -1;
        std::pair<int, int> slotVSVatTexture = {-1, -1};
        int slotVSTerrain = -1;
        int slotFSTerrainLayers = -1;
        // u_fs_pbrParams of a built-in Lit lobe variant ends with the lobes' values (and the UV
        // sets with a second set): its size, their offsets (bytes; size 0: no lobes)
        uint16_t pbrParamsSize = 0;
        uint16_t pbrExtOffset = 0;
        int16_t pbrSetsOffset = -1;
        // and the Hair shading model's three vec4 after them (spec S57 R5; -1: no hair)
        int16_t pbrHairOffset = -1;
        // u_fs_lightOcclusion (includes/light_occlusion.glsl: capsule and contact shadows),
        // u_tiledLights and u_contactShadowTexture of the S57 variants ('Til', 'Cap', 'Cts')
        int slotFSLightOcclusion = -1;
        unsigned int sizeFSLightOcclusion = 0;
        std::pair<int, int> slotTiledLights = {-1, -1};
        std::pair<int, int> slotContactShadows = {-1, -1};
        // u_lightRecords of a shader with the lights' records form (includes/lights.glsl), and
        // the records form's tail in its lights' block
        std::pair<int, int> slotLightRecords = {-1, -1};
        LightDataTail lightDataTail;

        int slotVSDepthParams = -1;
        unsigned int sizeVSDepthParams = 0; // reflected: an older fork declares the block without mvpMatrix
        int slotFSDepthMaterial = -1;
        unsigned int sizeFSDepthMaterial = 0; // reflected: 2 vec4 with Stochastic Tiling, else 1
        int slotVSDepthFade = -1;
        int slotVSDepthSkinning = -1;
        int slotVSDepthMorphTarget = -1;
        int slotVSDepthLattice = -1;
        std::pair<int, int> slotVSDepthLatticeTexture = {-1, -1};
        int slotVSDepthVat = -1;
        std::pair<int, int> slotVSDepthVatTexture = {-1, -1};
        int slotVSDepthTerrain = -1;
        int slotVSDepthSprite = -1;

        int slotVSGBufferParams = -1;
        int slotVSGBufferFade = -1;
        int slotFSGBufferMaterial = -1;
        unsigned int sizeFSGBufferMaterial = 0; // reflected: 3 vec4 with Stochastic Tiling, else 2
        int slotVSGBufferSkinning = -1;
        int slotVSGBufferMorphTarget = -1;
        int slotVSGBufferLattice = -1;
        std::pair<int, int> slotVSGBufferLatticeTexture = {-1, -1};
        int slotVSGBufferVat = -1;
        std::pair<int, int> slotVSGBufferVatTexture = {-1, -1};
        int slotVSGBufferTerrain = -1;
        int slotFSGBufferTerrainLayers = -1;

        // custom uniform blocks of the forked shaders, filled from MeshComponent::shaderUniforms
        CustomUniformBlock customVSParams;
        CustomUniformBlock customFSParams;
        CustomUniformBlock customVSDepthParams;
        CustomUniformBlock customFSDepthParams;

        // shader library, resolved at load: the manifest of the shader it draws with (null
        // for the built-in shader or a fork without one), and what its color and depth
        // shaders read of the frame block and custom textures
        std::shared_ptr<const ShaderManifest> manifest;
        // its @state with the @state[...] rules of the material's features (manifest only)
        ShaderManifestState manifestState;
        ShaderLibraryBindings libraryBindings;
        ShaderLibraryBindings depthLibraryBindings;
        // its colour pass writes neither colour nor depth (a stencil mask), resolved at load: it
        // stays out of the SSAO depth pre-pass and the G-buffer, where it would be an invisible
        // surface darkening and reflecting what is behind it
        bool skipsScreenEffects = false;
        // set at load: a colour pass testing the stencil (other than always), drawn only where
        // a mask marked it, so it stays out of the SSAO and scene depth pre-passes too
        bool stencilTested = false;

        Rect textureRect = Rect(0.0, 0.0, 1.0, 1.0);

        PrimitiveType primitiveType = PrimitiveType::TRIANGLES;
        unsigned int vertexCount = 0;

        uint64_t overrideFields = 0; // SubmeshOverrideFlags edited after import, kept across loads

        // material.lighting the shader variant was picked for
        MaterialLighting loadedLighting = MaterialLighting::AUTO;

        bool faceCulling = true;
        bool textureShadow = false;

        bool hasTexCoord1 = false;
        bool hasTexCoord2 = false;
        bool hasNormal = false;
        bool hasIBL = false;
        // what its colour pass adds as environment specular (RenderSystem's SSR composite swaps
        // it; render/SSRProbeSlots.h SSRSource)
        uint8_t ssrSource = 0;
        bool hasNormalMap = false;
        bool hasTangent = false;
        bool hasVertexColor3 = false;
        bool hasVertexColor4 = false;
        bool hasTextureRect = false;
        bool hasSkinning = false;
        bool hasMorphTarget = false;
        bool hasMorphNormal = false;
        bool hasMorphTangent = false;

        bool alphaBlend = false;
        bool needUpdateTexture = false;
        bool needUpdateDepthTexture = false;
        bool needUpdateGBufferTexture = false;
        // runtime (RenderSystem): a render target image made again, seen first by a Before pass:
        // the colour draw binds the same textures again (not an edit: no variant check)
        bool needRebindTexture = false;
        // runtime (RenderSystem): TexturePool::getReadyEpoch when the material was bound with a
        // stand-in for an image it names (0: every image bound is its own). When it moves on, the
        // material binds again.
        uint32_t textureStandInEpoch = 0;

        bool generated = false;

        float normAdjustJoint = 1.0f;
        float normAdjustWeight = 1.0f;
        bool hasSkinningNormalization = false;

        // detail levels over the same vertices, keyed by the geometry they were built from
        std::shared_ptr<MeshLodData> lodData;
        ExternalBuffer lodIndices;
        uint64_t lodKey = 0;
        // the levels a cooked model baked (glTF JE_mesh_lods, MeshLodPool::readGltfLevels) and the
        // builder version they were made with: used instead of a runtime build when it is this one
        std::shared_ptr<MeshLodData> bakedLods;
        int bakedLodRecipe = 0;
        // the LOD cross-fade variant of its colour shader (MESH 'Lfd'), asked for while the mesh
        // may fade (RenderSystem::prepareSubmeshLodFade); refused (it failed, or its bindings are
        // not its base's): the submesh never fades until it loads again
        std::shared_ptr<ShaderRender> lodFadeShader;
        uint64_t lodFadeShaderProperties = 0;
        uint16_t lodFadeShaderId = 0;
        bool lodFadeChecked = false;
        bool lodFadeRefused = false;
        // set at load: it draws with the built-in Mesh or Lit shader and depth program (only
        // those get detail levels: another vertex stage may move the vertices)
        bool lodBuiltinShader = true;

        // the material's extra pass as loaded (null: none, nothing allocated)
        std::shared_ptr<SubmeshExtraPass> extraPass;
        // its weighted blended OIT render (an alpha-blended submesh while a camera of the scene
        // draws Order Independent; null: none, nothing allocated)
        std::shared_ptr<SubmeshOit> oit;
        // MeshComponent::smoothNormals entry its position attribute uses (-1: none)
        int smoothNormals = -1;

        // its GPU Instancing variants (null: GPU Instancing off, the submesh cannot batch, or not
        // loaded yet; 1.7 KB a submesh not paid then). A copy of the component shares them until
        // its load makes its own (RenderSystem::requestSubmeshBatchShaders)
        std::shared_ptr<SubmeshBatchVariants> batch;
        // the bounds of the vertices it draws in the mesh's space (null: not known, the mesh's
        // are used): a pass culls a visible mesh's submeshes one by one (spec S55 R4)
        AABB localBounds;
        // position-only indices for the shadow passes (spec S55 R5), shared by the submeshes
        // over the same buffers: null until a shadow pass first draws it, then kept (empty when
        // they would not pay off)
        std::shared_ptr<ShadowIndexBuffer> shadowIndices;
    };

    // The Vertex Animation variant a mesh draws with (vertex animation spec 3.5)
    enum class VatVariant : uint8_t {
        NONE,
        VERTICES,   // 'Vat' (HAS_VAT)
        BONES       // 'Vab' (HAS_VAT, HAS_VAT_BONES)
    };

    // u_vs_vat (includes/vat.glsl): clock and bone index/weight scales, layout, the entity's
    // tracks, the clip table
    constexpr int VAT_BLOCK_HEADER = 4;
    constexpr int VAT_BLOCK_CLIPS = 64;     // = vat::VAT_MAX_CLIPS

    // A Vertex Animator's draw state on its mesh: RenderSystem fills it every frame from the
    // entity's VertexAnimatorComponent (gpuActive, asset, gpuState0/1, gpuNow, gpuMode) and binds
    // it to the 'Vat' / 'Vab' variants. A change of `variant` reloads the mesh with the new
    // variant once its shaders are built (needVariantReload), the loaded one drawing until then.
    struct JASPERENGINE_API VatMeshState{
        VatVariant variant = VatVariant::NONE;          // what the animator asks for now
        VatVariant variantActive = VatVariant::NONE;    // as the loaded shaders were built
        std::shared_ptr<const vat::VertexAnimation> asset; // holds the payload texture
        TextureRender* texture = nullptr;               // the payload, null without a variant
        // u_vs_vat, VAT_BLOCK_HEADER + VAT_BLOCK_CLIPS vectors (allocated with a variant); the
        // bone index and weight scales (x.y, x.z) are each submesh's, written at its draw
        std::vector<Vector4> block;
        // the animatedBounds the bounds were last made with (a change marks them dirty)
        AABB boundsApplied;
    };

    struct MeshComponent{
        // component checkbox: its system skips a disabled component
        bool enabled = true;

        bool loaded = false;
        bool loadCalled = false;
        // its load could not make a GPU object because that pool was full (SokolSystem logs
        // which, once): not drawn and counted as settled (RenderSystem::isAllLoaded), not as
        // loading; loaded again when the pools have room (SystemRender::hasResourcePoolRoom)
        bool loadFailed = false;
        // a submesh's attribute names a buffer the mesh does not have: not drawn and settled as
        // above, and not tried again until the mesh is reloaded
        bool loadMissingBuffer = false;

        InterleavedBuffer buffer;
        IndexBuffer indices;
        // a model's vertex buffers (numExternalBuffers of them in use)
        StableArray<ExternalBuffer> eBuffers;
        unsigned int numExternalBuffers = 0;

        unsigned int vertexCount = 0;

        // numSubmeshes of them in use; growing the array never moves one (the render code keeps
        // pointers into them for a frame)
        StableArray<Submesh> submeshes;
        unsigned int numSubmeshes = 0;

        // Optional user-forked shader. Project-relative base path (no extension),
        // e.g. "shaders/myMesh" -> shaders/myMesh.vert + shaders/myMesh.frag.
        // Empty = use the built-in Mesh shader. Drives the main render pass only;
        // the gbuffer pass keeps the built-in shader.
        ShaderRef customShader;
        // Same for the depth shader (shadow maps, and the SSAO/post-process depth when SSR
        // is off; with SSR on those read the built-in gbuffer), so a fork that displaces
        // vertices or discards keeps its shadows in step.
        ShaderRef customDepthShader;
        // values of the fork's custom uniform blocks, by member name, shared by all submeshes
        ShaderUniformValues shaderUniforms;
        bool needUpdateShaderUniforms = false;
        // ShaderGlobals::getVersion() when the custom blocks of the submeshes whose materials
        // bind globals (Material::shaderGlobalBindings) were last written
        uint64_t globalsVersionSeen = 0;

        // the skin's palette, one matrix per joint (MeshSystem sizes it at the model load); empty on a
        // mesh without a skin, which then has no palette resource either
        GrowArray<Matrix4> bonesMatrix;
        BufferRender bonesBuffer;
        TextureRender bonesTexture;
        // runtime: rows of the palette resource as it was made (RenderSystem), 0 = none
        uint32_t bonesUploadRows = 0;
        bool needUpdateBones = false;
        float normAdjustJoint = 1;
        float normAdjustWeight = 1;

        // planar reflection: logical view-projection of the mirror's reflection
        // camera, set each frame by RenderSystem::updateMirrors, uploaded to the
        // USE_MIRROR shader for projective sampling of the reflection texture
        Matrix4 mirrorViewProjection;

        float morphWeights[MAX_MORPHTARGETS] = {};
        // the weights are this component's own: read from a saved scene or prefab (the editor's
        // Stream, an exported game's factory code) or kept from an earlier load of the same model
        // (edits, deliberate zeros). A model load gives the file's default weights only when this
        // is false (a freshly loaded model) or when it loads another model file.
        bool morphWeightsSet = false;
        // how many targets those weights cover: the mesh's target count at its last load (saved
        // with them). A reload of a model file that has more targets now gives the new ones the
        // file's defaults. Weights saved without it (older files) or set before any load cover
        // the targets up to their last non-zero weight (morphWeightsPresentCount): the zeros after
        // it get the file's defaults, as every load gave them before the count existed.
        int morphTargetCount = 0;
        // the blend shape (morph target) names of this mesh in its model file (glTF
        // extras.targetNames), index = weight; empty when the file names none. Filled by the
        // model load (per mesh: two meshes of a model can name their targets differently), not saved.
        std::vector<std::string> morphTargetNames;

        // lattice deformation (spec 2026-10-23-lattice-deformer-design.md §5): filled by
        // LatticeRender::prepare, uploaded and bound by RenderSystem
        LatticeMeshState lattice;
        // a Vertex Animator's playback (vertex animation spec 3.5), filled by RenderSystem
        VatMeshState vat;
        // loaded from a Vertex Animation mesh (a .vat.glb, glTF extension JASPER_vertex_animation):
        // its JOINTS_0 / WEIGHTS_0 name the baked bones, there is no skin to draw them with
        bool vatMesh = false;

        AABB aabb = AABB::ZERO;
        AABB verticesAABB = AABB::ZERO; // is not influenced by instances
        AABB worldAABB; // initially NULL

        std::vector<AABB> bonesAABB; // bind-pose bounds per bone index, empty when not skinned
        AABB skinnedAABB; // local bounds of the posed mesh, null when not skinned
        // Vertex Animator: local bounds over every frame of the clips in use (null = none); used
        // instead of verticesAABB for culling, the world AABB and instance spheres (vertex animation spec 3.2)
        AABB animatedBounds;

        // the lights it is drawn with, see MeshLights
        MeshLights lights;

        // generated a_smoothNormal buffers, null unless a shader of a submesh asks for them
        std::shared_ptr<MeshSmoothNormals> smoothNormals;

        bool receiveLights = true;
        bool receiveIBL = false; // image-based lighting from a sky environment or a reflection probe
        bool castShadows = true;
        bool receiveShadows = true;
        bool shadowsBillboard = true;
        // runtime: a Capsule Shadow Caster set to Capsules Only holds it (on its GameObject or
        // an ancestor): its shadow is the capsules', it draws into no shadow map (spec S57 R2)
        bool capsuleShadowsOnly = false;
        // what the shadow passes and the shadow cache take it as
        bool castsShadowMaps() const { return castShadows && !capsuleShadowsOnly; }
        // its shadows are cached in the static layer: redrawn only when it (or another static
        // caster of the slot) changes, while dynamic casters move over it (docs/rendering/shadows.md)
        bool staticShadowCaster = false;
        // runtime: bumped on every change of what it casts that its bounds alone may miss (a
        // transform, a mesh reload, a buffer or depth texture update): the shadow cache's input
        uint32_t shadowCasterRevision = 0;
        bool renderInReflectionProbes = true; // when false the mesh is skipped by probe captures

        // authored (saved): transparent drawn when autoTransparency is off
        bool transparent = false;
        bool autoTransparency = true;
        // set at load with autoTransparency: a submesh draws in the transparent queue. Runtime
        // only, never saved, so a scene saves the same whether or not it has loaded yet
        bool derivedTransparent = false;
        // drawn in the transparent queue (the derived value with autoTransparency, else the
        // authored one)
        bool isTransparent() const { return autoTransparency ? derivedTransparent : transparent; }
        // what its arrays hold outside the component (ComponentArray::arrayHeapBytes)
        size_t arrayHeapBytes() const {
            size_t bytes = submeshes.storageBytes() + eBuffers.storageBytes() + bonesMatrix.storageBytes();
            for (size_t i = 0; i < submeshes.stored(); i++)
                bytes += submeshes[i].batch ? sizeof(SubmeshBatchVariants) : 0;
            return bytes;
        }
        // set at load: transparent-queue order (the lowest sortPriority of its blended
        // submeshes), opaque-queue order (the lowest of the others; before the distance)
        // and whether the camera depth pre-pass leaves it out (a forked color shader without
        // a depth program, a submesh that does not write depth or that tests stencil)
        int transparentSortPriority = 0;
        int opaqueSortPriority = 0;
        bool skipsDepthPrepass = false;
        // set at load: what its shaders (main draws and extra passes) read of the scene,
        // ShaderRequire_SceneDepth / _SceneColor (manifest @require scenedepth / scenecolor)
        uint32_t sceneInputs = 0;
        // set at load: a mesh of opaque and blended submeshes (autoTransparency) whose blended
        // ones read the scene: its opaque submeshes draw in the opaque queue (so they are in the
        // scene depth and colour it reads), the blended ones in the transparent queue
        bool splitQueues = false;
        // what it was registered to read before it loaded (RenderSystem::predictSceneInputUsers),
        // done once per load
        bool sceneInputsPredicted = false;

        // opt-out of the scene's detail levels (Scene::setMeshLodEnabled); bias above 1 keeps detail longer
        bool lodEnabled = true;
        float lodBias = 1.0f;
        // Allow GPU Instancing (spec S55): its draws may batch with those of the same mesh,
        // material and pipeline when the project's GPU Instancing is on (Unity's per-material
        // switch, here per renderer); off draws it alone
        bool allowGpuInstancing = true;
        float lodError[MAX_MESH_LODS] = {}; // largest error of any submesh at each level
        unsigned int numLods = 1;
        // how a level change shows (spec S54 R9, Unity's LOD Group): Fade Mode; Animate
        // Cross-fading fades over the scene's Cross-fade Duration, else over the last Fade
        // Transition Width of each level's band (an Instanced Mesh always fades so); Cull Below
        // Screen Height: not drawn under that share of the view's height (0: never, Unity's Culled)
        LodFadeMode lodFadeMode = LodFadeMode::CROSS_FADE;
        bool lodAnimateCrossFading = true;
        float lodFadeTransitionWidth = 0.1f;
        float lodCullBelow = 0.0f;
        // runtime: a level forced for every view (Mesh.forceLod, capture_model_lods; -1:
        // automatic), and the fade of the main view (others: RenderSystem::RenderView::lodFades)
        int lodForced = -1;
        MeshLodFade lodFade;
        // runtime, set at load: its submeshes draw with shaders that have a fade variant (the
        // built-in Mesh and Lit, no fork); and in the colour pass: every fade variant it draws
        // with is made (a fade starts only then)
        bool lodFadeCapable = false;
        bool lodFadeReady = false;
        // runtime: bumped when its levels change (they arrive, go or are rebuilt): the next level
        // change of every view snaps instead of fading from levels that are no longer the same
        uint32_t lodRevision = 0;

        CullingMode cullingMode = CullingMode::BACK;
        WindingOrder windingOrder = WindingOrder::CCW;
        // The winding order already compensates a mirroring transform: a glTF import whose
        // scene root or node with a negative determinant is placed on the entity transforms
        // turns windingOrder to CW for it (MeshSystem::loadGLTF). Any other draw whose model
        // matrix mirrors (negative determinant) reverses the face winding itself, so front faces
        // stay front: RenderSystem::isMirroredDraw
        bool mirroredWinding = false;

        bool needUpdateAABB = false;
        bool needUpdateBuffer = false;
        bool needReload = false;

        // runtime: content hash of the triangles picking reads (picking/PickGeometry.h), the
        // key of the shared picking BVH; 0 until a pick computes it, and again whenever the
        // geometry may have changed (calculateMeshAABB, a dynamic buffer upload)
        uint64_t pickGeometryHash = 0;
        // the scene's shader environment changed (light cap, fog, lights, scene depth programs:
        // RenderSystem::needReloadMeshVariants): the mesh keeps drawing its loaded variant and
        // reloads once the new variant's shaders are built (needReload then)
        bool needVariantReload = false;
    };

    // the targets weights without a count cover: up to the last non-zero one
    // (MeshComponent::morphTargetCount)
    inline int morphWeightsPresentCount(const float* weights){
        int count = 0;
        for (int i = 0; i < MAX_MORPHTARGETS; i++){
            if (weights[i] != 0.0f) count = i + 1;
        }
        return count;
    }

}

#endif //MESH_COMPONENT_H
