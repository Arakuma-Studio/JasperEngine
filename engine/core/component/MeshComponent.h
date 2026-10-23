// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#ifndef MESH_COMPONENT_H
#define MESH_COMPONENT_H

#include "Engine.h"
#include "util/HybridArray.h"
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
#include "math/Rect.h"
#include <deque>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace jasperengine{

    // Submesh fields a model load rewrites from its source file, so an edit to one only survives
    // the load while it is flagged in Submesh::overrideFields.
    enum SubmeshOverrideFlags : uint32_t {
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

        SubmeshOverride_ShaderLibrary = SubmeshOverride_Shader | SubmeshOverride_ShaderParams |
                                        SubmeshOverride_ShaderTextures | SubmeshOverride_ShaderFeatures |
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
                                   SubmeshOverride_FoliageTransmission | SubmeshOverride_ShaderLibrary
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
    };

    // The extra draw of a submesh whose material has an extra pass (Material::extraPass,
    // shader library phase 2): its own shader, pipelines, uniform blocks and textures, built
    // by RenderSystem::loadMesh and drawn by drawMesh before (Before passes, ahead of all the
    // mesh's submeshes) or after (After passes, after all of them) the mesh's main draws. A
    // submesh without an extra pass has none (Submesh::extraPass is null).
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
        int slotFSParams = -1;
        int slotFSTexCoordSets = -1;
        int slotVSFade = -1;
        int slotFSFog = -1;
        int slotFSLighting = -1;
        int slotVSObjectLights = -1;
        int slotFSObjectLights = -1;
        unsigned int sizeVSObjectLights = 0;
        unsigned int sizeFSObjectLights = 0;
        int slotFSLitFrame = -1;
        unsigned int sizeFSLitFrame = 0;
        int slotFSLighting2D = -1;
        int slotVSSprite = -1;
        int slotVSSkinning = -1;
        int slotVSMorphTarget = -1;

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

        int slotVSParams = -1;
        int slotFSParams = -1;
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
        int slotVSSkinning = -1;
        int slotVSMorphTarget = -1;
        int slotVSTerrain = -1;
        int slotFSTerrainLayers = -1;

        int slotVSDepthParams = -1;
        unsigned int sizeVSDepthParams = 0; // reflected: an older fork declares the block without mvpMatrix
        int slotFSDepthMaterial = -1;
        int slotVSDepthFade = -1;
        int slotVSDepthSkinning = -1;
        int slotVSDepthMorphTarget = -1;
        int slotVSDepthTerrain = -1;
        int slotVSDepthSprite = -1;

        int slotVSGBufferParams = -1;
        int slotFSGBufferMaterial = -1;
        int slotVSGBufferSkinning = -1;
        int slotVSGBufferMorphTarget = -1;
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

        uint32_t overrideFields = 0; // SubmeshOverrideFlags edited after import, kept across loads

        // material.lighting the shader variant was picked for
        MaterialLighting loadedLighting = MaterialLighting::AUTO;

        bool faceCulling = true;
        bool textureShadow = false;

        bool hasTexCoord1 = false;
        bool hasTexCoord2 = false;
        bool hasNormal = false;
        bool hasIBL = false;
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

        bool generated = false;

        float normAdjustJoint = 1.0f;
        float normAdjustWeight = 1.0f;
        bool hasSkinningNormalization = false;

        // detail levels over the same vertices, keyed by the geometry they were built from
        std::shared_ptr<MeshLodData> lodData;
        ExternalBuffer lodIndices;
        uint64_t lodKey = 0;

        // the material's extra pass as loaded (null: none, nothing allocated)
        std::shared_ptr<SubmeshExtraPass> extraPass;
        // MeshComponent::smoothNormals entry its position attribute uses (-1: none)
        int smoothNormals = -1;
    };

    struct MeshComponent{
        // component checkbox: its system skips a disabled component
        bool enabled = true;

        bool loaded = false;
        bool loadCalled = false;

        InterleavedBuffer buffer;
        IndexBuffer indices;
        HybridArray<ExternalBuffer, MAX_EXTERNAL_BUFFERS> eBuffers;
        unsigned int numExternalBuffers = 0;

        unsigned int vertexCount = 0;

        HybridArray<Submesh, MAX_SUBMESHES> submeshes;
        unsigned int numSubmeshes = 0;

        // Optional user-forked shader. Project-relative base path (no extension),
        // e.g. "shaders/myMesh" -> shaders/myMesh.vert + shaders/myMesh.frag.
        // Empty = use the built-in Mesh shader. Drives the main render pass only;
        // the gbuffer pass keeps the built-in shader.
        std::string customShader;
        // Same for the depth shader (shadow maps, and the SSAO/post-process depth when SSR
        // is off; with SSR on those read the built-in gbuffer), so a fork that displaces
        // vertices or discards keeps its shadows in step.
        std::string customDepthShader;
        // values of the fork's custom uniform blocks, by member name, shared by all submeshes
        ShaderUniformValues shaderUniforms;
        bool needUpdateShaderUniforms = false;

        HybridArray<Matrix4, MAX_BONES> bonesMatrix;
        BufferRender bonesBuffer;
        TextureRender bonesTexture;
        bool needUpdateBones = false;
        float normAdjustJoint = 1;
        float normAdjustWeight = 1;

        // planar reflection: logical view-projection of the mirror's reflection
        // camera, set each frame by RenderSystem::updateMirrors, uploaded to the
        // USE_MIRROR shader for projective sampling of the reflection texture
        Matrix4 mirrorViewProjection;

        float morphWeights[MAX_MORPHTARGETS] = {};

        AABB aabb = AABB::ZERO;
        AABB verticesAABB = AABB::ZERO; // is not influenced by instances
        AABB worldAABB; // initially NULL

        std::vector<AABB> bonesAABB; // bind-pose bounds per bone index, empty when not skinned
        AABB skinnedAABB; // local bounds of the posed mesh, null when not skinned

        // the lights it is drawn with, see MeshLights
        MeshLights lights;

        // generated a_smoothNormal buffers, null unless a shader of a submesh asks for them
        std::shared_ptr<MeshSmoothNormals> smoothNormals;

        bool receiveLights = true;
        bool receiveIBL = false; // image-based lighting from a sky environment or a reflection probe
        bool castShadows = true;
        bool receiveShadows = true;
        bool shadowsBillboard = true;
        bool renderInReflectionProbes = true; // when false the mesh is skipped by probe captures

        bool transparent = false;
        bool autoTransparency = true;
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
        float lodError[MAX_MESH_LODS] = {}; // largest error of any submesh at each level
        unsigned int numLods = 1;

        CullingMode cullingMode = CullingMode::BACK;
        WindingOrder windingOrder = WindingOrder::CCW;

        bool needUpdateAABB = false;
        bool needUpdateBuffer = false;
        bool needReload = false;
        // the scene's shader environment changed (light cap, fog, lights, scene depth programs:
        // RenderSystem::needReloadMeshVariants): the mesh keeps drawing its loaded variant and
        // reloads once the new variant's shaders are built (needReload then)
        bool needVariantReload = false;
    };
    
}

#endif //MESH_COMPONENT_H
