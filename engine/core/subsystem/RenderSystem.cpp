// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "RenderSystem.h"
#include "InstanceSort.h"
#include "object/Camera.h"
#include "math/Quaternion.h"
#include "Scene.h"
#include "Engine.h"
#include "System.h"
#include "render/Render.h"
#include "render/ObjectRender.h"
#include "render/SystemRender.h"
#include "pool/ShaderPool.h"
#include "pool/TexturePool.h"
#include "math/Vector3.h"
#include "texture/IBLEnvironment.h"
#include "util/Angle.h"
#include "util/Color.h"
#include "buffer/ExternalBuffer.h"
#include "math/AABB.h"
#include "pool/TextureDataPool.h"
#include "subsystem/MeshSystem.h"
#include "subsystem/UIDocumentSystem.h"
#include <memory>
#include <algorithm>
#include <array>
#include <cmath>
#include <map>
#include <set>
#include <mutex>
#include <unordered_map>
#include <random>
#include <cstdint>
#include <cstring>

using namespace jasperengine;

namespace {
    constexpr int SPOT_MASK_TILE_SIZE = 256;

    static_assert(sizeof(Matrix4) == sizeof(float) * 16);

    bool bindSkinningResource(ObjectRender& render, ShaderData& shader, MeshComponent& mesh){
        const size_t boneCount = mesh.bonesMatrix.size();
        const unsigned int size = static_cast<unsigned int>(boneCount * sizeof(Matrix4));
        const int storageSlot = shader.getStorageBufferIndex(StorageBufferType::VS_SKINNING);
        if (storageSlot != -1){
            if (!mesh.bonesBuffer.isCreated() &&
                    !mesh.bonesBuffer.createBuffer(size, nullptr, BufferType::STORAGE_BUFFER, BufferUsage::STREAM))
                return false;
            render.addStorageBuffer(storageSlot, ShaderStageType::VERTEX, &mesh.bonesBuffer);
        }else{
            const auto textureSlot = shader.getTextureIndex(TextureShaderType::BONES);
            if (textureSlot.first == -1 || (!mesh.bonesTexture.isCreated() &&
                    !mesh.bonesTexture.createDynamicTexture("mesh-bones", 4,
                        static_cast<int>(boneCount))))
                return false;
            render.addTexture(textureSlot, ShaderStageType::VERTEX, &mesh.bonesTexture);
        }
        mesh.needUpdateBones = true;
        return true;
    }

    void applySkinningUniform(ObjectRender& render, int slot, const MeshComponent& mesh,
                              const Submesh& submesh){
        Vector4 normAdjust(
            submesh.hasSkinningNormalization ? submesh.normAdjustJoint : mesh.normAdjustJoint,
            submesh.hasSkinningNormalization ? submesh.normAdjustWeight : mesh.normAdjustWeight,
            0.0f, 0.0f);
        render.applyUniformBlock(slot, sizeof(normAdjust), &normAdjust);
    }

    // fade distance past which instances can be dropped: only the built-in triangle
    // shaders shrink them to nothing, and the SSR G-buffer has no fade yet
    float instanceFadeCullEnd(const InstancedMeshComponent& instmesh, const MeshComponent& mesh, const Scene& scene){
        if (scene.isSSREnabled() || !scene.getDefaultMeshShader().empty() ||
            !instmesh.distanceFade || !mesh.customShader.empty() || !mesh.customDepthShader.empty() ||
            !(instmesh.fadeEnd > instmesh.fadeStart) || !std::isfinite(instmesh.fadeStart) ||
            !std::isfinite(instmesh.fadeEnd) || instmesh.fadeEnd < 0.0f){
            return -1.0f;
        }
        for (unsigned int s = 0; s < mesh.numSubmeshes; s++){
            const auto primitive = mesh.submeshes[s].primitiveType;
            if (primitive != PrimitiveType::TRIANGLES && primitive != PrimitiveType::TRIANGLE_STRIP){
                return -1.0f;
            }
        }
        return instmesh.fadeEnd;
    }

    // shadow-map texels of error a caster may lose, well inside the PCF blur
    const float SHADOW_LOD_TEXELS = 2.0f;

    // an instanced mesh with culling off keeps its whole batch, bounds or not
    bool cullsBatch(const MeshComponent& mesh, const InstancedMeshComponent* instmesh){
        return mesh.worldAABB != AABB::ZERO && (!instmesh || instmesh->cullInstances);
    }

    // getCenter() asserts on null/infinite boxes. New foliage (and any empty
    // instanced mesh) leaves worldAABB null; AABB::ZERO is the unloaded default
    // and is not a useful sample point.
    Vector3 meshProbeSamplePosition(const MeshComponent& mesh, const Transform& transform){
        if (mesh.worldAABB.isFinite() && mesh.worldAABB != AABB::ZERO){
            return mesh.worldAABB.getCenter();
        }
        return transform.worldPosition;
    }

    void applyInstanceFadeUniform(ObjectRender& render, int slot, const InstancedMeshComponent& instmesh, const Vector3& fadeEyeLocal){
        float fade[8] = {instmesh.fadeStart, instmesh.fadeEnd, 0.0f, 0.0f,
            fadeEyeLocal.x, fadeEyeLocal.y, fadeEyeLocal.z, 0.0f};
        render.applyUniformBlock(slot, sizeof(fade), &fade);
    }

    // custom block of a forked shader, with the engine-written members refreshed
    void applyCustomUniforms(ObjectRender& render, CustomUniformBlock& block, float frameTime, const Vector2& resolution){
        if (block.slot == -1)
            return;

        block.writeTime(frameTime);
        block.writeResolution(resolution.x, resolution.y);
        render.applyUniformBlock(block.slot, (unsigned int)block.data.size(), block.data.data());
    }

    // the custom blocks of a submesh's shaders: its material's parameters, else the mesh's
    // shader uniforms (forks set on the component), else the manifest defaults
    void writeSubmeshCustomUniforms(const MeshComponent& mesh, Submesh& submesh){
        const ShaderUniformValues& values = submesh.material.shaderParams;
        const ShaderManifest* manifest = submesh.manifest.get();
        submesh.customVSParams.writeValues(values, &mesh.shaderUniforms, manifest);
        submesh.customFSParams.writeValues(values, &mesh.shaderUniforms, manifest);
        submesh.customVSDepthParams.writeValues(values, &mesh.shaderUniforms, manifest);
        submesh.customFSDepthParams.writeValues(values, &mesh.shaderUniforms, manifest);
        // the extra pass: its own parameters, else its manifest's defaults (not while a component
        // copy still shares the original's: its load makes its own and writes that one)
        if (submesh.extraPass && submesh.extraPass.use_count() == 1){
            const ShaderUniformValues& passValues = submesh.material.extraPass->shaderParams;
            submesh.extraPass->customVSParams.writeValues(passValues, nullptr, submesh.extraPass->manifest.get());
            submesh.extraPass->customFSParams.writeValues(passValues, nullptr, submesh.extraPass->manifest.get());
        }
    }

    // the mesh blocks of both forks, from whichever pass draws first (depth runs before color)
    void writeCustomUniforms(MeshComponent& mesh){
        if (!mesh.needUpdateShaderUniforms)
            return;

        for (int i = 0; i < mesh.numSubmeshes; i++){
            writeSubmeshCustomUniforms(mesh, mesh.submeshes[i]);
        }
        mesh.needUpdateShaderUniforms = false;
    }

    // both stages of a single-block component with manifest defaults, rewritten first when
    // its values changed
    void applyComponentUniforms(ObjectRender& render, CustomUniformBlock& vsBlock, CustomUniformBlock& fsBlock,
                                const ShaderUniformValues& values, const ShaderManifest* manifest, bool& needUpdate,
                                float frameTime, const Vector2& resolution){
        if (needUpdate){
            vsBlock.writeValues(values, nullptr, manifest);
            fsBlock.writeValues(values, nullptr, manifest);
            needUpdate = false;
        }
        applyCustomUniforms(render, vsBlock, frameTime, resolution);
        applyCustomUniforms(render, fsBlock, frameTime, resolution);
    }

    // how a shader without a manifest takes the post-processing colour under its resolved blend
    // (RenderSystem::PostColorBlend); a library shader's BLEND_* define does it in the shader
    uint8_t postColorBlendOf(const RenderState& state, bool libraryShader){
        if (libraryShader)
            return POST_BLEND_OWN;
        switch (state.blend){
            case BlendMode::ADDITIVE:      return POST_BLEND_ADDITIVE;
            case BlendMode::PREMULTIPLIED: return POST_BLEND_PREMULTIPLIED;
            case BlendMode::MULTIPLY:      return POST_BLEND_MULTIPLY;
            default:                       return POST_BLEND_OWN;
        }
    }

    BlendMode toBlendMode(MaterialBlendMode mode){
        switch (mode){
            case MaterialBlendMode::NONE:          return BlendMode::NONE;
            case MaterialBlendMode::PREMULTIPLIED: return BlendMode::PREMULTIPLIED;
            case MaterialBlendMode::ADDITIVE:      return BlendMode::ADDITIVE;
            case MaterialBlendMode::MULTIPLY:      return BlendMode::MULTIPLY;
            default:                               return BlendMode::ALPHA;
        }
    }

    DepthCompare toDepthCompare(MaterialDepthTest test){
        switch (test){
            case MaterialDepthTest::LESS:          return DepthCompare::LESS;
            case MaterialDepthTest::EQUAL:         return DepthCompare::EQUAL;
            case MaterialDepthTest::GREATER_EQUAL: return DepthCompare::GREATER_EQUAL;
            case MaterialDepthTest::GREATER:       return DepthCompare::GREATER;
            case MaterialDepthTest::NOT_EQUAL:     return DepthCompare::NOT_EQUAL;
            case MaterialDepthTest::ALWAYS:        return DepthCompare::ALWAYS;
            default:                               return DepthCompare::LESS_EQUAL;
        }
    }

    StencilCompare toStencilCompare(MaterialStencilCompare compare){
        switch (compare){
            case MaterialStencilCompare::NEVER:         return StencilCompare::NEVER;
            case MaterialStencilCompare::LESS:          return StencilCompare::LESS;
            case MaterialStencilCompare::EQUAL:         return StencilCompare::EQUAL;
            case MaterialStencilCompare::LESS_EQUAL:    return StencilCompare::LESS_EQUAL;
            case MaterialStencilCompare::GREATER:       return StencilCompare::GREATER;
            case MaterialStencilCompare::NOT_EQUAL:     return StencilCompare::NOT_EQUAL;
            case MaterialStencilCompare::GREATER_EQUAL: return StencilCompare::GREATER_EQUAL;
            default:                                    return StencilCompare::ALWAYS;
        }
    }

    StencilOp toStencilOp(MaterialStencilOp op){
        switch (op){
            case MaterialStencilOp::ZERO:       return StencilOp::ZERO;
            case MaterialStencilOp::REPLACE:    return StencilOp::REPLACE;
            case MaterialStencilOp::INCR_CLAMP: return StencilOp::INCR_CLAMP;
            case MaterialStencilOp::DECR_CLAMP: return StencilOp::DECR_CLAMP;
            case MaterialStencilOp::INVERT:     return StencilOp::INVERT;
            case MaterialStencilOp::INCR_WRAP:  return StencilOp::INCR_WRAP;
            case MaterialStencilOp::DECR_WRAP:  return StencilOp::DECR_WRAP;
            default:                            return StencilOp::KEEP;
        }
    }

    uint8_t clampStencilByte(int value){
        return (uint8_t)std::clamp(value, 0, 255);
    }

    // Stencil and colour write (phase 2): an explicit MaterialStencil (compare not AUTO) is
    // taken whole, else the manifest's stencil rule, else off; colorWrite -1 takes the
    // manifest's colorwrite, else every channel.
    void resolveStencilState(const MaterialStencil& stencil, int colorWrite, const ShaderManifestState& manifestState, RenderState& state){
        const MaterialStencil& resolved = (stencil.compare != MaterialStencilCompare::AUTO) ? stencil : manifestState.stencil;
        state.stencilEnabled = resolved.compare != MaterialStencilCompare::AUTO && resolved.compare != MaterialStencilCompare::DISABLED;
        if (state.stencilEnabled){
            state.stencilCompare = toStencilCompare(resolved.compare);
            state.stencilRef = clampStencilByte(resolved.ref);
            state.stencilReadMask = clampStencilByte(resolved.readMask);
            state.stencilWriteMask = clampStencilByte(resolved.writeMask);
            state.stencilPass = toStencilOp(resolved.pass);
            state.stencilFail = toStencilOp(resolved.fail);
            state.stencilDepthFail = toStencilOp(resolved.depthFail);
        }

        const int mask = (colorWrite >= 0) ? colorWrite : manifestState.colorWrite;
        state.colorWriteMask = (mask >= 0) ? (uint8_t)(mask & COLOR_WRITE_ALL) : (uint8_t)COLOR_WRITE_ALL;
    }

    // explicit value, else the manifest's @state value, else AUTO (the legacy behaviour)
    template<typename T>
    T resolveState(T explicitValue, T manifestValue){
        return (explicitValue != T::AUTO) ? explicitValue : manifestValue;
    }

    // Render state resolution (section 5.2) shared by meshes and components: the explicit
    // values, then the manifest's @state, then the legacy state given. Stencil and colour
    // write (phase 2) resolve the same way (resolveStencilState).
    RenderState resolveRenderState(MaterialBlendMode blendMode, MaterialCullMode cullMode, MaterialDepthWrite depthWrite,
                                   MaterialDepthTest depthTest, float offsetFactor, float offsetUnits,
                                   const MaterialStencil& stencil, int colorWrite,
                                   const ShaderManifestState* resolvedState, bool legacyBlended, bool legacyFaceCulling,
                                   CullingMode legacyCullingMode, WindingOrder windingOrder,
                                   bool& blended, bool& transparentQueue){
        const ShaderManifestState noState;
        const ShaderManifestState& manifestState = resolvedState ? *resolvedState : noState;

        RenderState state;
        state.windingOrder = windingOrder;

        // legacy: every pipeline blends with alpha, opaque objects included
        const MaterialBlendMode blend = resolveState(blendMode, manifestState.blend);
        if (blend == MaterialBlendMode::AUTO){
            blended = legacyBlended;
            state.blend = BlendMode::ALPHA;
        }else{
            blended = blend != MaterialBlendMode::NONE;
            state.blend = toBlendMode(blend);
        }

        const MaterialCullMode cull = resolveState(cullMode, manifestState.cull);
        if (cull == MaterialCullMode::AUTO){
            state.faceCulling = legacyFaceCulling;
            state.cullingMode = legacyCullingMode;
        }else{
            state.faceCulling = cull != MaterialCullMode::OFF;
            state.cullingMode = (cull == MaterialCullMode::FRONT) ? CullingMode::FRONT : CullingMode::BACK;
        }
        state.prepassFaceCulling = state.faceCulling;

        const MaterialDepthWrite write = resolveState(depthWrite, manifestState.depthWrite);
        state.depthWrite = (write == MaterialDepthWrite::AUTO) ? !blended : (write == MaterialDepthWrite::ENABLED);

        state.depthCompare = toDepthCompare(resolveState(depthTest, manifestState.depthTest));

        if (offsetFactor != 0.0f || offsetUnits != 0.0f){
            state.depthBiasSlope = offsetFactor;
            state.depthBias = offsetUnits;
        }else if (manifestState.hasDepthOffset){
            state.depthBiasSlope = manifestState.depthOffsetFactor;
            state.depthBias = manifestState.depthOffsetUnits;
        }

        if (manifestState.queue == ShaderQueue::TRANSPARENT){
            transparentQueue = true;
        }else if (manifestState.queue == ShaderQueue::GEOMETRY || manifestState.queue == ShaderQueue::ALPHATEST){
            transparentQueue = false;
        }else{
            transparentQueue = blended;
        }

        resolveStencilState(stencil, colorWrite, manifestState, state);

        return state;
    }

    // the manifest a shader string has, when it has one (built-in and forks without
    // manifest lines: null, the legacy path)
    std::shared_ptr<const ShaderManifest> findManifest(const std::string& shader){
        if (shader.empty())
            return nullptr;
        std::shared_ptr<const ShaderManifest> manifest = ShaderManifests::get(shader);
        return (manifest && manifest->present) ? manifest : nullptr;
    }

    bool usesAlphaBlend(const Material& material){
        return material.alphaMode == MaterialAlphaMode::BLEND ||
            (material.alphaMode == MaterialAlphaMode::AUTO &&
             (material.baseColorTexture.isTransparent() || material.baseColorFactor.w != 1.0f));
    }

    // the destination selects swapchain or RTT, a reflection pass reverses winding
    // and a canvas camera drops the depth buffer entirely
    PipelineType colorPipeline(const CameraComponent& camera, bool offscreenTarget){
        if (!offscreenTarget)
            return camera.depthTest ? PIP_DEFAULT : PIP_DEFAULT_NODEPTH;
        if (camera.invertCulling)
            return PIP_RTT_INVERT;
        return camera.depthTest ? PIP_RTT : PIP_RTT_NODEPTH;
    }

    // a canvas camera has no depth buffer to fix blending order, so it composites
    // strictly in submission order instead
    bool sortsByDistance(const CameraComponent& camera){
        return camera.depthTest && camera.transparentSort;
    }

    float readSpotMaskTexel(TextureData& source, int x, int y, bool useAlpha){
        x = std::clamp(x, 0, source.getWidth() - 1);
        y = std::clamp(y, 0, source.getHeight() - 1);

        const int channels = source.getChannels();
        const int bytesPerChannel = TextureData::getBytesPerChannel(source.getColorFormat());
        const size_t pixelOffset =
            (static_cast<size_t>(y) * static_cast<size_t>(source.getWidth()) + static_cast<size_t>(x)) *
            static_cast<size_t>(channels) * static_cast<size_t>(bytesPerChannel);
        const unsigned char* pixels = static_cast<const unsigned char*>(source.getData());

        if (source.getColorFormat() == ColorFormat::RED16){
            uint16_t value = 0;
            std::memcpy(&value, pixels + pixelOffset, sizeof(value));
            return static_cast<float>(value) / 65535.0f;
        }

        if (useAlpha && channels >= 4){
            return static_cast<float>(pixels[pixelOffset + 3]) / 255.0f;
        }

        if (channels >= 3){
            Vector3 color = Color::sRGBToLinear(
                static_cast<float>(pixels[pixelOffset]) / 255.0f,
                static_cast<float>(pixels[pixelOffset + 1]) / 255.0f,
                static_cast<float>(pixels[pixelOffset + 2]) / 255.0f);
            return 0.2126f * color.x + 0.7152f * color.y + 0.0722f * color.z;
        }

        return static_cast<float>(pixels[pixelOffset]) / 255.0f;
    }

    void rasterizeSpotMaskTile(TextureData& source, int tileIndex, int atlasWidth, std::vector<unsigned char>& atlas){
        const bool useAlpha = source.getColorFormat() == ColorFormat::RGBA && source.hasAlpha();
        const float sourceWidth = static_cast<float>(source.getWidth());
        const float sourceHeight = static_cast<float>(source.getHeight());

        for (int y = 0; y < SPOT_MASK_TILE_SIZE; y++){
            const float sourceY = ((static_cast<float>(y) + 0.5f) / static_cast<float>(SPOT_MASK_TILE_SIZE)) * sourceHeight - 0.5f;
            const int y0 = static_cast<int>(std::floor(sourceY));
            const int y1 = y0 + 1;
            const float fy = sourceY - static_cast<float>(y0);

            for (int x = 0; x < SPOT_MASK_TILE_SIZE; x++){
                const float sourceX = ((static_cast<float>(x) + 0.5f) / static_cast<float>(SPOT_MASK_TILE_SIZE)) * sourceWidth - 0.5f;
                const int x0 = static_cast<int>(std::floor(sourceX));
                const int x1 = x0 + 1;
                const float fx = sourceX - static_cast<float>(x0);

                const float top = readSpotMaskTexel(source, x0, y0, useAlpha) * (1.0f - fx) +
                                  readSpotMaskTexel(source, x1, y0, useAlpha) * fx;
                const float bottom = readSpotMaskTexel(source, x0, y1, useAlpha) * (1.0f - fx) +
                                     readSpotMaskTexel(source, x1, y1, useAlpha) * fx;
                const float value = top * (1.0f - fy) + bottom * fy;

                const int atlasX = tileIndex * SPOT_MASK_TILE_SIZE + x;
                atlas[static_cast<size_t>(y) * static_cast<size_t>(atlasWidth) + static_cast<size_t>(atlasX)] =
                    static_cast<unsigned char>(std::round(std::clamp(value, 0.0f, 1.0f) * 255.0f));
            }
        }
    }
}

uint32_t RenderSystem::pixelsWhite[64];
uint32_t RenderSystem::pixelsBlack[64];
uint32_t RenderSystem::pixelsNormal[64];

TextureRender RenderSystem::emptyWhite;
TextureRender RenderSystem::emptyArrayWhite;
TextureRender RenderSystem::emptyBlack;
TextureRender RenderSystem::emptyCubeBlack;
TextureRender RenderSystem::emptyCubeWhite;
TextureRender RenderSystem::emptyNormal;
TextureRender RenderSystem::emptyShadowDepth;
uint32_t RenderSystem::pixelsGray[64];
uint32_t RenderSystem::pixelsFlatNormal[64];
TextureRender RenderSystem::emptyGray;
TextureRender RenderSystem::emptyFlatNormal;
TextureRender RenderSystem::emptyCubeGray;

bool RenderSystem::emptyTexturesCreated = false;

TextureRender* RenderSystem::frameBlurredScene = nullptr;
blurred_scene_t RenderSystem::frameBlurredSceneBlock;

RenderSystem::RenderSystem(Scene* scene): SubSystem(scene), atmosphere(this){
    instanceViewsDirty = true;
    signature.set(scene->getComponentId<Transform>());

    this->scene = scene;

    ssaoLoaded = false;
    ssaoSlotParams = -1;
    ssaoBlurSlotParams = -1;
    frameTime = 0.0f;
    passResolution = Vector2(0.0f, 0.0f);
    passFrame = {};
    passSceneBlock = {};

    ssrLoaded = false;
    ssrSlotParams = -1;
    ssrBlurSlotParams = -1;
    compositeSlotParams = -1;
    swapchainRedirect = false;
    sceneColorRedirectAllowed = false;
    sceneColorRedirect = false;
    sceneColorRedirectHold = 0;
    sceneColorRedirectIdle = 0;

    blitLoaded = false;
    blitSlotParams = -1;

    postProcessLoaded = false;
    postProcessNeedReload = false;
    postProcessNeedUpdateUniforms = false;
    postProcessNeedsDepth = false;
    postProcessNeedsGBuffer = false;

    shadowAtlasSlotResolution = 0;
    shadowAtlasCols = 0;
    shadowAtlasRows = 0;
    shadowAtlasUsedSlots = 0;
    needUpdateShadowAtlas = true;
    shadowPointAtlasSlotResolution = 0;
    needUpdateShadowPointAtlas = true;
    needUpdateShadowBindings = false;
    hasShadowAtlas = false;
    hasShadowPointAtlas = false;
    spotMaskAtlasCreated = false;
    initShadowAtlasRects();
    initShadowPointAtlasRects();

    shadow2DAtlasWidth = 0;
    hasShadow2DAtlas = false;
    shadowCaster2DLoaded = false;
    shadow2DSlotParams = -1;
    shadowCaster2DBufferCapacity = 0;
    shadowCaster2DVertexCount = 0;
    shadowCaster2DNeedUpdateBuffer = false;
    hasReflectionProbes = false;
    capturingReflectionProbe = false;

    lightListEpoch = 1;
    lightListCap = 0;
    lightCapacityTier = 8;
}

RenderSystem::~RenderSystem(){
}

void RenderSystem::load(){
    hasLights = false;
    hasShadows = false;
    hasFog = false;
    heightFogVariants = false;
    atmosphereDepthPrograms = false;
    hasIBL = false;
    hasReflectionProbes = false;
    hasMultipleCameras = false;
    lastMultiCameraDraw = false;
    capturingReflectionProbe = false;
    activeSky = NULL_ENTITY;
    lastBatchSort = true;
    lastMeshLodEnabled = false;
    loadedPipelines = 0;
    hasLights2D = false;
    hasShadows2D = false;
    numLights2D = 0;

    createEmptyTextures();

    update(0); // first update
}

void RenderSystem::destroy(){
    // View teardown keeps the scene alive; retain CPU assets for its next load.
    const bool reloadView = !Engine::isViewLoaded();

    // Before the transform traversal below, which the camera entities are part of.
    while (!mirrorCameras.empty()){
        destroyMirrorCamera(mirrorCameras.begin()->first);
    }

    // cannot destroy static textures because it affects other scenes

    emptyTexturesCreated = false;

#ifdef JASPERENGINE_EDITOR
    for (auto& entry : secondaryViews){
        destroyRenderView(*entry.second);
    }
    secondaryViews.clear();
    activeView = &primaryView;
#endif

    destroySSAO();
    destroySSR();
    destroyBlit();
    destroyPostProcess();
    destroyUber();
    destroyBloomPrograms();
    atmosphere.destroy();
    primaryView.atmosphere.destroy();
    if (postLinearSampler.isCreated())
        postLinearSampler.destroyTexture();
    rv().postSceneFramebuffer.destroy();
    rv().postSceneWidth = 0;
    rv().postSceneHeight = 0;
    rv().bloomTargets.destroy();
    rv().blurTargets.destroy();
    rv().msaaFramebuffer.destroy();
    rv().msaaWidth = 0;
    rv().msaaHeight = 0;
    rv().msaaSamples = 1;
    rv().msaaResolveFramebuffer.destroy();
    if (frameBlurredScene == &rv().blurTargets.up[0].getRender().getColorTexture() ||
            frameBlurredScene == &rv().blurTargets.down[0].getRender().getColorTexture())
        frameBlurredScene = nullptr;
    // the post-processing LUTs, rebaked by the next applyPostProcessing
    postTailLut.destroyTexture();
    postTailLutSize = 0;
    postGradeLut.destroyTexture();
    postGradeLutSize = 0;
    postProcessingDirty = true;
    // scene inputs: the targets, the copy, and who reads them (loaded again with the scene)
    releaseSceneInputTargets(primaryView, true);
    primaryView.sceneColorRedirect.destroy();
    if (sceneColorCopyLoaded){
        sceneColorCopyRender.destroy();
        sceneColorCopyLoaded = false;
    }
    if (sceneColorCopyShader){
        sceneColorCopyShader.reset();
        ShaderPool::remove(ShaderType::BLIT, 0);
    }
    sceneInputMeshes.clear();
    sceneInputPoints.clear();
    sceneInputUsage = 0;
    sceneDepthPrograms = false;
    sceneDepthIdleUpdates = 0;
    sceneColorIdleUpdates = 0;
    sceneColorReaders = false;
    gbufferDepthMismatch.clear();
    passSceneDepth = nullptr;
    passSceneColor = nullptr;
    for (InstanceView& view : rv().instanceViews){
        view.buffer.getRender()->destroyBuffer();
        view.data.clear();
        view.capacity = 0;
    }
    instanceViewsDirty = true;
    passShadows = nullptr;
    cameraShadows.clear();
    primaryView.shadows.release();
    shadowAtlasFramebuffer.destroyFramebuffer();
    shadowAtlasSlotResolution = 0;
    shadowAtlasCols = 0;
    shadowAtlasRows = 0;
    shadowAtlasUsedSlots = 0;
    hasShadowAtlas = false;
    shadowPointAtlasFramebuffer.destroyFramebuffer();
    shadowPointAtlasSlotResolution = 0;
    hasShadowPointAtlas = false;
    shadow2DAtlasFramebuffer.destroyFramebuffer();
    shadow2DAtlasWidth = 0;
    hasShadow2DAtlas = false;
    destroyShadowCaster2DPass();
    if (spotMaskAtlasCreated){
        spotMaskAtlas.destroyTexture();
        spotMaskAtlasCreated = false;
    }
    spotMaskAtlasPixels.clear();
    spotMaskAtlasEntries.fill({});

    auto skys = scene->getComponentArray<SkyboxComponent>();
    if (skys->size() > 0){
        SkyboxComponent& sky = skys->getComponentFromIndex(0);
        Entity entity = skys->getEntity(0);
        if (sky.loaded){
            if (reloadView){
                sky.needReload = true;
                releaseSkyEnvironment(sky);
                sky.needUpdateEnvironment = true;
            }
            destroySky(entity, sky);
        }
    }

    auto probes = scene->getComponentArray<ReflectionProbeComponent>();
    for (int i = 0; i < probes->size(); i++){
        ReflectionProbeComponent& probe = probes->getComponentFromIndex(i);
        destroyReflectionProbe(probes->getEntity(i), probe);
    }
    // defensive sweep: a runtime can outlive its component (e.g. the probe lost
    // its Transform and updateReflectionProbes never revisited it)
    for (auto& entry : reflectionProbeRuntimes){
        entry.second->captureFramebuffer.destroyFramebuffer();
        releaseReflectionProbeMaps(*entry.second);
    }
    reflectionProbeRuntimes.clear();

    auto transforms = scene->getComponentArray<Transform>();
    for (int i = 0; i < transforms->size(); i++){
        Entity entity = transforms->getEntity(i);
        Signature signature = scene->getSignature(entity);

        if (signature.test(scene->getComponentId<MeshComponent>())){
            MeshComponent& mesh = scene->getComponent<MeshComponent>(entity);
            if (mesh.loaded){
                if (reloadView) mesh.needReload = true;
                destroyMesh(entity, mesh);
            }
        }else if (signature.test(scene->getComponentId<UIComponent>())){
            UIComponent& ui = scene->getComponent<UIComponent>(entity);
            if (ui.loaded){
                if (reloadView) ui.needReload = true;
                destroyUI(entity, ui);
            }
        }else if (signature.test(scene->getComponentId<PointsComponent>())){
            PointsComponent& points = scene->getComponent<PointsComponent>(entity);
            if (points.loaded){
                if (reloadView) points.needReload = true;
                destroyPoints(entity, points);
            }
        }else if (signature.test(scene->getComponentId<LineRendererComponent>())){
            LineRendererComponent& lines = scene->getComponent<LineRendererComponent>(entity);
            if (lines.loaded){
                if (reloadView) lines.needReload = true;
                destroyLines(entity, lines);
            }
        }else if (signature.test(scene->getComponentId<LightComponent>())){
            LightComponent& light = scene->getComponent<LightComponent>(entity);
            destroyLight(light);
        }else if (signature.test(scene->getComponentId<CameraComponent>())){
            CameraComponent& camera = scene->getComponent<CameraComponent>(entity);
            destroyCamera(camera, false);
        }
    }

    // Cached GPU arrays cannot survive renderer shutdown, even when CPU assets are retained.
    while (!terrainDetailArrays.empty()){
        destroyTerrainDetailArray(terrainDetailArrays.begin()->first);
    }
}

void RenderSystem::updateFramebuffer(CameraComponent& camera){
    if (camera.framebuffer->isCreated()){
        camera.framebuffer->destroy();
        camera.framebuffer->create();
    }
}

void RenderSystem::createEmptyTextures(){
    if (!emptyTexturesCreated){
        void* data_array[6];
        size_t size_array[6];

        for (int i = 0; i < 64; i++) {
            pixelsNormal[i] = 0xFF808080;
        }

        for (int i = 0; i < 6; i++){
            data_array[i]  = (void*)pixelsNormal;
            size_array[i] = 8 * 8 * 4;
        }

        emptyNormal.createTexture(
                "empty|normal", 8, 8, ColorFormat::RGBA, TextureType::TEXTURE_2D, 1, data_array, size_array, 
                TextureFilter::NEAREST, TextureFilter::NEAREST, TextureWrap::REPEAT, TextureWrap::REPEAT);

        for (int i = 0; i < 64; i++) {
            pixelsWhite[i] = 0xFFFFFFFF;
        }

        for (int i = 0; i < 6; i++){
            data_array[i]  = (void*)pixelsWhite;
            size_array[i] = 8 * 8 * 4;
        }

        emptyWhite.createTexture(
                "empty|white", 8, 8, ColorFormat::RGBA, TextureType::TEXTURE_2D, 1, data_array, size_array,
                TextureFilter::NEAREST, TextureFilter::NEAREST, TextureWrap::REPEAT, TextureWrap::REPEAT);

        emptyArrayWhite.createTexture(
                "empty|array|white", 8, 8, ColorFormat::RGBA, TextureType::TEXTURE_ARRAY, 1, data_array, size_array,
                TextureFilter::NEAREST, TextureFilter::NEAREST, TextureWrap::REPEAT, TextureWrap::REPEAT);

        // white cube fallback for the skybox cube sampler: a color-only sky (no
        // texture) multiplies skyParams.color by this, so white keeps the color intact
        emptyCubeWhite.createTexture(
                "empty|cube|white", 8, 8, ColorFormat::RGBA, TextureType::TEXTURE_CUBE, 6, data_array, size_array,
                TextureFilter::NEAREST, TextureFilter::NEAREST, TextureWrap::REPEAT, TextureWrap::REPEAT);

        for (int i = 0; i < 64; i++) {
            pixelsBlack[i] = 0xFF000000;
        }

        for (int i = 0; i < 6; i++){
            data_array[i]  = (void*)pixelsBlack;
            size_array[i] = 8 * 8 * 4;
        }

        emptyBlack.createTexture(
                "empty|black", 8, 8, ColorFormat::RGBA, TextureType::TEXTURE_2D, 1, data_array, size_array, 
                TextureFilter::NEAREST, TextureFilter::NEAREST, TextureWrap::REPEAT, TextureWrap::REPEAT);
        emptyCubeBlack.createTexture(
                "empty|cube|black", 8, 8, ColorFormat::RGBA, TextureType::TEXTURE_CUBE, 6, data_array, size_array, 
                TextureFilter::NEAREST, TextureFilter::NEAREST, TextureWrap::REPEAT, TextureWrap::REPEAT);

        // shader library @texture defaults: mid gray, and a flat tangent-space normal (0.5, 0.5, 1)
        for (int i = 0; i < 64; i++) {
            pixelsGray[i] = 0xFF808080;
            pixelsFlatNormal[i] = 0xFFFF8080;
        }

        for (int i = 0; i < 6; i++){
            data_array[i]  = (void*)pixelsGray;
            size_array[i] = 8 * 8 * 4;
        }

        emptyGray.createTexture(
                "empty|gray", 8, 8, ColorFormat::RGBA, TextureType::TEXTURE_2D, 1, data_array, size_array,
                TextureFilter::NEAREST, TextureFilter::NEAREST, TextureWrap::REPEAT, TextureWrap::REPEAT);
        emptyCubeGray.createTexture(
                "empty|cube|gray", 8, 8, ColorFormat::RGBA, TextureType::TEXTURE_CUBE, 6, data_array, size_array,
                TextureFilter::NEAREST, TextureFilter::NEAREST, TextureWrap::REPEAT, TextureWrap::REPEAT);

        data_array[0] = (void*)pixelsFlatNormal;
        emptyFlatNormal.createTexture(
                "empty|flatnormal", 8, 8, ColorFormat::RGBA, TextureType::TEXTURE_2D, 1, data_array, size_array,
                TextureFilter::NEAREST, TextureFilter::NEAREST, TextureWrap::REPEAT, TextureWrap::REPEAT);

        emptyTexturesCreated = true;
    }

    // Point-only scenes still declare the projective sampler, so keep a
    // type-compatible binding available even if it is never sampled.
    if (!emptyShadowDepth.isCreated()){
        emptyShadowDepth.createFramebufferTexture(
            TextureType::TEXTURE_2D, true, true, 1, 1,
            TextureFilter::NEAREST, TextureFilter::NEAREST,
            TextureWrap::CLAMP_TO_BORDER, TextureWrap::CLAMP_TO_BORDER);
    }
}

int RenderSystem::checkLightsAndShadow(){
    hasLights = false;
    hasShadows = false;

    auto lights = scene->getComponentArray<LightComponent>();

    // any number: each mesh picks the ones it is lit by (updateObjectLights)
    int numLights = lights->size();

    if (scene->getLightState() == LightState::AUTO){
        if (numLights > 0)
            hasLights = true;
    }else if (scene->getLightState() == LightState::ON){
        hasLights = true;
    }
    
    for (int i = 0; i < numLights; i++){
        LightComponent& light = lights->getComponentFromIndex(i);
        if (light.shadows){
            hasShadows = true;
        }
    }

    // 2D lights are gated only by their own component count, NOT by
    // scene->getLightState(): the editor forces LightState::OFF in 2D scenes
    // to keep 3D lights out of them, but 2D lights must still work there.
    hasLights2D = false;
    hasShadows2D = false;

    auto lights2d = scene->getComponentArray<Light2DComponent>();
    numLights2D = lights2d->size();
    if (numLights2D > MAX_LIGHTS_2D)
        numLights2D = MAX_LIGHTS_2D;

    if (numLights2D > 0)
        hasLights2D = true;

    return numLights;
}

bool RenderSystem::loadLights(int numLights){
    int freeProjectiveSlot = 0;
    int freePointSlot = 0;
    unsigned int maxProjectiveSlotResolution = 0;
    unsigned int maxPointSlotResolution = 0;

    auto lights = scene->getComponentArray<LightComponent>();
    
    for (int i = 0; i < numLights; i++){
        LightComponent& light = lights->getComponentFromIndex(i);

        light.shadowMapIndex = -1;
        
        if (light.shadows){
            if (light.type == LightType::DIRECTIONAL &&
                (light.numShadowCascades < 1 || light.numShadowCascades > MAX_SHADOWCASCADES)){
                unsigned int requestedCascades = light.numShadowCascades;
                light.numShadowCascades = std::clamp(
                    light.numShadowCascades, 1u, (unsigned int)MAX_SHADOWCASCADES);
                Log::warn("Shadow cascades clamped from %u to %u", requestedCascades, light.numShadowCascades);
            }

            if (light.needUpdateShadowMap){
                needUpdateShadowAtlas = true;
                needUpdateShadowPointAtlas = true;
                needUpdateShadowBindings = true;
                light.needUpdateShadowMap = false;
            }
        }
    }

    for (int i = 0; i < numLights; i++){
        LightComponent& light = lights->getComponentFromIndex(i);
        if (!light.shadows) continue;

        if (light.type == LightType::SPOT){
            if (freeProjectiveSlot < MAX_SHADOW_ATLAS_SLOTS){
                light.shadowMapIndex = freeProjectiveSlot;
                freeProjectiveSlot++;
                maxProjectiveSlotResolution = std::max(maxProjectiveSlotResolution, light.mapResolution);
            }
        }else if (light.type == LightType::DIRECTIONAL){
            if ((freeProjectiveSlot + light.numShadowCascades) <= MAX_SHADOW_ATLAS_SLOTS){
                light.shadowMapIndex = freeProjectiveSlot;
                freeProjectiveSlot += light.numShadowCascades;
                maxProjectiveSlotResolution = std::max(maxProjectiveSlotResolution, light.mapResolution);
            }
        }
    }

    for (int i = 0; i < numLights; i++){
        LightComponent& light = lights->getComponentFromIndex(i);
        if (!light.shadows || light.type != LightType::POINT) continue;

        if ((freePointSlot + SHADOW_CUBE_FACES) <= MAX_POINT_SHADOW_ATLAS_SLOTS){
            light.shadowMapIndex = freePointSlot;
            freePointSlot += SHADOW_CUBE_FACES;
            maxPointSlotResolution = std::max(maxPointSlotResolution, light.mapResolution);
        }
    }

    if (freeProjectiveSlot > 0){
        if (!ensureShadowAtlas(maxProjectiveSlotResolution, freeProjectiveSlot)){
            Log::warn("Failed to create projective shadow atlas");
            for (int i = 0; i < numLights; i++){
                LightComponent& light = lights->getComponentFromIndex(i);
                if (light.shadows && light.type != LightType::POINT){
                    light.shadowMapIndex = -1;
                }
            }
        }
    }else{
        if (hasShadowAtlas || shadowAtlasFramebuffer.isCreated()){
            needUpdateShadowBindings = true;
        }
        shadowAtlasFramebuffer.destroyFramebuffer();
        shadowAtlasSlotResolution = 0;
        shadowAtlasCols = 0;
        shadowAtlasRows = 0;
        shadowAtlasUsedSlots = 0;
        initShadowAtlasRects();
        hasShadowAtlas = false;
    }

    if (freePointSlot > 0){
        if (!ensureShadowPointAtlas(maxPointSlotResolution)){
            Log::warn("Failed to create point shadow atlas");
            for (int i = 0; i < numLights; i++){
                LightComponent& light = lights->getComponentFromIndex(i);
                if (light.shadows && light.type == LightType::POINT){
                    light.shadowMapIndex = -1;
                }
            }
        }
    }else{
        if (hasShadowPointAtlas || shadowPointAtlasFramebuffer.isCreated()){
            needUpdateShadowBindings = true;
        }
        shadowPointAtlasFramebuffer.destroyFramebuffer();
        shadowPointAtlasSlotResolution = 0;
        hasShadowPointAtlas = false;
    }

    return true;
}

void RenderSystem::updateSpotMaskAtlas(int numLights){
    auto lights = scene->getComponentArray<LightComponent>();
    std::array<SpotMaskAtlasEntry, SPOT_MASK_ATLAS_TILES> newEntries;
    std::array<TextureData*, SPOT_MASK_ATLAS_TILES> sources{};
    std::array<std::shared_ptr<std::array<TextureData, 6>>, SPOT_MASK_ATLAS_TILES> sourceOwners;
    // scene light index each tile goes to, -1 for none
    std::array<int, SPOT_MASK_ATLAS_TILES> tileLights;
    tileLights.fill(-1);

    // A light keeps its tile while that tile holds its mask (its pixels may be released once
    // rasterized), so tiles never move between lights. Free tiles go to the lit spot lights
    // with a mask in scene order; an unlit one keeps its tile only while no lit one needs it.
    // Tries tile t for light i: its current source, the tile's committed copy, or a load.
    auto tryTile = [&](int t, LightComponent& light){
        const std::string sourceId = light.spotMask.getId();

        auto acceptSource = [&](const std::shared_ptr<std::array<TextureData, 6>>& owner){
            if (!owner){
                return false;
            }

            TextureData& source = owner->at(0);
            if (!source.getData() || source.getWidth() <= 0 || source.getHeight() <= 0 || source.getChannels() <= 0){
                return false;
            }

            sourceOwners[t] = owner;
            sources[t] = &sourceOwners[t]->at(0);
            newEntries[t].sourceId = sourceId;
            newEntries[t].sourceOwner = owner.get();
            newEntries[t].sourcePixels = source.getData();
            newEntries[t].width = source.getWidth();
            newEntries[t].height = source.getHeight();
            newEntries[t].aspect = std::clamp(
                static_cast<float>(source.getWidth()) / static_cast<float>(source.getHeight()),
                0.1f, 10.0f);
            return true;
        };

        std::shared_ptr<std::array<TextureData, 6>> pooledSource = TextureDataPool::get(sourceId);
        bool hasSource = acceptSource(pooledSource);

        if (!hasSource){
            const SpotMaskAtlasEntry& committedEntry = spotMaskAtlasEntries[t];
            const bool canReuseCommittedTile =
                spotMaskAtlasCreated &&
                pooledSource &&
                pooledSource.get() == committedEntry.sourceOwner &&
                sourceId == committedEntry.sourceId &&
                !committedEntry.empty();

            if (canReuseCommittedTile){
                newEntries[t] = committedEntry;
            }else{
                TextureLoadResult result = light.spotMask.load();
                if (result.state == ResourceLoadState::Finished){
                    acceptSource(result.data);
                }
            }
        }

        return !newEntries[t].empty();
    };

    // 0: lit lights keep their tile, 1: lit lights without one take a free one, 2: unlit
    // lights keep theirs if still free
    for (int pass = 0; pass < 3; pass++){
        for (int i = 0; i < numLights; i++){
            LightComponent& light = lights->getComponentFromIndex(i);
            if (light.type != LightType::SPOT || light.spotMask.empty() || light.spotMask.isFramebuffer()){
                continue;
            }
            const bool lit = light.intensity > 0.0f && isLightActive(lights->getEntity(i), light);
            if (lit == (pass == 2) || std::find(tileLights.begin(), tileLights.end(), i) != tileLights.end()){
                continue;
            }

            int tile = -1;
            if (pass == 1){
                for (int t = 0; t < SPOT_MASK_ATLAS_TILES && tile < 0; t++){
                    if (tileLights[t] < 0)
                        tile = t;
                }
            }else{
                const int previous = light.spotMaskTile;
                if (previous >= 0 && previous < SPOT_MASK_ATLAS_TILES && tileLights[previous] < 0 &&
                        spotMaskAtlasEntries[previous].sourceId == light.spotMask.getId()){
                    tile = previous;
                }
            }

            if (tile >= 0 && tryTile(tile, light)){
                tileLights[tile] = i;
            }else if (tile >= 0){
                newEntries[tile] = SpotMaskAtlasEntry();
                sources[tile] = nullptr;
                sourceOwners[tile].reset();
            }
        }
    }

    auto applyCommittedReadiness = [&](){
        for (int i = 0; i < static_cast<int>(lights->size()); i++){
            LightComponent& light = lights->getComponentFromIndex(i);
            int lightTile = -1;
            for (int t = 0; t < SPOT_MASK_ATLAS_TILES; t++){
                if (tileLights[t] == i){
                    lightTile = t;
                }
            }
            const bool ready =
                lightTile >= 0 &&
                spotMaskAtlasCreated &&
                newEntries[lightTile] == spotMaskAtlasEntries[lightTile];
            const float aspect = ready ? spotMaskAtlasEntries[lightTile].aspect : 1.0f;

            if (light.spotMaskReady != ready || std::abs(light.spotMaskAspect - aspect) > 0.0001f){
                light.needUpdateShadowCamera = true;
            }
            light.spotMaskReady = ready;
            light.spotMaskAspect = aspect;
            light.spotMaskTile = ready ? lightTile : -1;
        }
    };

    if (spotMaskAtlasCreated && newEntries == spotMaskAtlasEntries){
        applyCommittedReadiness();
        return;
    }
    if (!Engine::isViewLoaded()){
        applyCommittedReadiness();
        return;
    }

    const bool hasSpotMasks = std::any_of(
        newEntries.begin(),
        newEntries.end(),
        [](const SpotMaskAtlasEntry& entry){ return !entry.empty(); });
    if (!hasSpotMasks){
        if (spotMaskAtlasCreated){
            spotMaskAtlas.destroyTexture();
            spotMaskAtlasCreated = false;
        }
        spotMaskAtlasEntries.fill({});
        spotMaskAtlasPixels.clear();
        applyCommittedReadiness();
        return;
    }

    const int atlasWidth = SPOT_MASK_TILE_SIZE * SPOT_MASK_ATLAS_TILES;
    const size_t atlasSize =
        static_cast<size_t>(atlasWidth) * static_cast<size_t>(SPOT_MASK_TILE_SIZE);
    std::vector<unsigned char> newPixels = spotMaskAtlasPixels;
    const bool rebuildAllTiles = newPixels.size() != atlasSize;
    if (rebuildAllTiles){
        newPixels.assign(atlasSize, 255);
    }

    for (int t = 0; t < SPOT_MASK_ATLAS_TILES; t++){
        if (sources[t] &&
            (rebuildAllTiles || !spotMaskAtlasCreated || !(newEntries[t] == spotMaskAtlasEntries[t]))){
            rasterizeSpotMaskTile(*sources[t], t, atlasWidth, newPixels);
        }else if (newEntries[t].empty() &&
                  (rebuildAllTiles || !spotMaskAtlasEntries[t].empty())){
            const int tileStart = t * SPOT_MASK_TILE_SIZE;
            for (int y = 0; y < SPOT_MASK_TILE_SIZE; y++){
                auto row = newPixels.begin() +
                    static_cast<size_t>(y) * static_cast<size_t>(atlasWidth) +
                    static_cast<size_t>(tileStart);
                std::fill(row, row + SPOT_MASK_TILE_SIZE, 255);
            }
        }
    }

    void* data[6] = {};
    size_t size[6] = {};
    data[0] = newPixels.data();
    size[0] = newPixels.size();

    TextureRender newAtlas;
    const bool newAtlasCreated = newAtlas.createTexture(
        "spot-mask-atlas",
        atlasWidth,
        SPOT_MASK_TILE_SIZE,
        ColorFormat::RED,
        TextureType::TEXTURE_2D,
        1,
        data,
        size,
        TextureFilter::LINEAR,
        TextureFilter::LINEAR,
        TextureWrap::CLAMP_TO_EDGE,
        TextureWrap::CLAMP_TO_EDGE);

    if (!newAtlasCreated){
        newAtlas.destroyTexture();
        applyCommittedReadiness();
        return;
    }

    if (spotMaskAtlasCreated){
        spotMaskAtlas.destroyTexture();
    }
    spotMaskAtlas = newAtlas;
    spotMaskAtlasCreated = true;
    spotMaskAtlasPixels = std::move(newPixels);
    spotMaskAtlasEntries = newEntries;
    applyCommittedReadiness();

    // The atlas owns its resampled copy, so honor the source Texture's normal
    // release-after-upload policy instead of retaining full-resolution pixels.
    for (int t = 0; t < SPOT_MASK_ATLAS_TILES; t++){
        if (tileLights[t] < 0 || !sourceOwners[t]){
            continue;
        }
        LightComponent& light = lights->getComponentFromIndex(tileLights[t]);
        if (light.spotMask.isReleaseDataAfterLoad()){
            sourceOwners[t]->at(0).releaseImageData();
            spotMaskAtlasEntries[t].sourcePixels = nullptr;
        }
    }
}

void RenderSystem::loadSpotMaskTexture(ShaderData& shaderData, ObjectRender& render){
    render.addTexture(
        shaderData.getTextureIndex(TextureShaderType::SPOTMASKATLAS),
        ShaderStageType::FRAGMENT,
        spotMaskAtlasCreated ? &spotMaskAtlas : &emptyWhite);
}

// inverse viewport size, used by USE_SSAO to convert gl_FragCoord to a screen UV.
// .z = SSAO debug flag (output raw AO); .w = rendering-flipped flag (the AO buffer
// is in logical orientation, so flip Y on sample when the color pass is flipped).
Vector4 RenderSystem::lightingViewportInfo(const CameraComponent& camera) const{
    float vpW = targetViewRect().getWidth();
    float vpH = targetViewRect().getHeight();
    if (isFixedResolutionActive() && !camera.renderToTexture){
        // the main color pass renders into the fixed-resolution target
        vpW = (float)scene->getFixedResolutionWidth();
        vpH = (float)scene->getFixedResolutionHeight();
    }
    return Vector4(
        (vpW > 0.0f) ? 1.0f / vpW : 0.0f,
        (vpH > 0.0f) ? 1.0f / vpH : 0.0f,
        scene->isSSAODebug() ? 1.0f : 0.0f,
        isRenderingFlipped(camera) ? 1.0f : 0.0f);
}

void RenderSystem::processLights(CameraShadows& view, int numLights, const CameraComponent& camera, const Transform& cameraTransform){
    auto lights = scene->getComponentArray<LightComponent>();

    // the shadow cameras of the lights holding atlas slots
    for (int i = 0; i < numLights; i++){
        LightComponent& light = lights->getComponentFromIndex(i);

        // shadowMapIndex < 0 means the atlas had no free slot for this light this frame:
        // the per-object lists pass it on (cone_shadow.z), which the shader treats as "no
        // shadow". The light keeps its shadow intent and reclaims a slot automatically once
        // one frees up.
        if (!light.shadows || light.shadowMapIndex < 0)
            continue;

        size_t numMaps = 1;
        if (light.type == LightType::DIRECTIONAL){
            numMaps = lightCascades(view, i, light);
        }else if (light.type == LightType::POINT){
            numMaps = SHADOW_CUBE_FACES;
        }

        for (int c = 0; c < (int)numMaps; c++){
            const LightCamera& shadowCamera = lightCamera(view, i, light, c);
            if (light.type == LightType::POINT){
                view.fs_point_shadows.bias_texSize_nearFar[light.shadowMapIndex+c] = Vector4(
                    light.shadowBias,
                    (float)shadowPointAtlasSlotResolution,
                    shadowCamera.nearFar.x,
                    shadowCamera.nearFar.y);
            }else{
                view.vs_shadows.lightViewProjectionMatrix[light.shadowMapIndex+c] = shadowCamera.lightViewProjectionMatrix;

                float projScale = std::max(std::abs(shadowCamera.lightProjectionMatrix[0][0]),
                                           std::abs(shadowCamera.lightProjectionMatrix[1][1]));
                float texelSizeWorld = (projScale > 0.0f) ? 2.0f / (projScale * (float)shadowAtlasSlotResolution) : 0.0f;
                view.vs_shadows.shadowParams[light.shadowMapIndex+c] = Vector4(light.shadowNormalBias * texelSizeWorld, 0.0, 0.0, 0.0);

                view.fs_shadows.bias_texSize_nearFar[light.shadowMapIndex+c] = Vector4(
                    light.shadowBias,
                    (float)shadowAtlasSlotResolution,
                    shadowCamera.nearFar.x,
                    shadowCamera.nearFar.y);
            }
        }
    }

    // the camera part of the legacy u_fs_lighting (lights and eyePos.w are per draw); the
    // frame block of the camera's passes is filled by updatePassFrame
    view.fs_lighting.eyePos = Vector4(cameraTransform.worldPosition.x, cameraTransform.worldPosition.y, cameraTransform.worldPosition.z, 0.0);
    view.fs_lighting.cameraDir = Vector4(camera.worldDirection.x, camera.worldDirection.y, camera.worldDirection.z, shadowPCFRadius());
    view.fs_lighting.globalIllum = Vector4(scene->getGlobalIlluminationColorLinear(), scene->getGlobalIlluminationIntensity());
    view.fs_lighting.envColor = environmentInfo();
    view.fs_lighting.viewportInfo = lightingViewportInfo(camera);
}

float RenderSystem::shadowPCFRadius() const{
    // PCF tap radius of the 3D shadow filter (kernel = (2*radius+1)^2 taps; uniform-driven,
    // no shader variant)
    switch (scene->getShadowQuality()){
        case ShadowQuality::NONE:   return 0.0f;
        case ShadowQuality::LOW:    return 1.0f;
        case ShadowQuality::MEDIUM: return 2.0f;
        case ShadowQuality::HIGH:   return 3.0f;
    }
    return 1.0f;
}

Vector4 RenderSystem::environmentInfo() const{
    if (hasIBL){
        auto skys = scene->getComponentArray<SkyboxComponent>();
        if (skys->size() > 0){
            SkyboxComponent& sky = skys->getComponentFromIndex(0);
            Vector3 linColor = Color::sRGBToLinear(Vector3(sky.color.x, sky.color.y, sky.color.z));
            return Vector4(linColor.x, linColor.y, linColor.z, Angle::defaultToRad(sky.rotation));
        }
    }
    return Vector4(1.0, 1.0, 1.0, 0.0);
}

void RenderSystem::fillFrameLighting(frame_t& frame, lit_frame_t& litFrame, const CameraComponent& camera) const{
    const float pcfRadius = shadowPCFRadius();
    frame.lightingInfo = Vector4(pcfRadius, 0.0f, 0.0f, 0.0f);

    // the variant decides whether fog applies: a fogless scene leaves these unread
    Vector4 fogColorType = fs_fog.color_type;
    Vector4 fogDensityStartEnd = fs_fog.density_start_end;
#ifdef JASPERENGINE_EDITOR
    if (rv().disableFog){
        // exponential fog of density 0: a factor of 1 everywhere, no fog
        fogColorType = Vector4(0.0f, 0.0f, 0.0f, 1.0f);
        fogDensityStartEnd = Vector4(0.0f, 0.0f, 0.0f, 0.0f);
    }
#endif
    frame.fogColor_type = fogColorType;
    frame.fogDensity_start_end = fogDensityStartEnd;

    // Lit's part (includes/lit_frame.glsl): the camera from the frame block's contract
    // members (filled first, fillFrameBlock), the rest packed into their spare components
    const Vector4 viewport = lightingViewportInfo(camera);
    const Vector4 environment = environmentInfo();
    // linear fog (type 0) reads start and end, the exponential ones the density
    const float fogParam = (fogColorType.w == 0.0f) ? fogDensityStartEnd.z : fogDensityStartEnd.x;
    litFrame.cameraPosition_fogEnd = Vector4(frame.cameraPosition.x, frame.cameraPosition.y, frame.cameraPosition.z, fogDensityStartEnd.w);
    litFrame.ambient_fogType = Vector4(frame.ambient.x, frame.ambient.y, frame.ambient.z, fogColorType.w);
    litFrame.fogColor_param = Vector4(fogColorType.x, fogColorType.y, fogColorType.z, fogParam);
    litFrame.cameraForward_pcf = Vector4(frame.cameraDirection.x, frame.cameraDirection.y, frame.cameraDirection.z, pcfRadius);
    litFrame.viewport_ssao_envRotation = Vector4(viewport.x, viewport.y, viewport.z + 2.0f * viewport.w, environment.w);
    litFrame.environmentColor = Vector4(environment.x, environment.y, environment.z, 0.0f);
}

//-----Unified lighting: per-object light lists-----

// spot_shadow.w of the lights block packs (shadow map index + 1) in 6 bits, the cascades
// in 3 and the spot mask tile in 3 (7 = none): 12 bits, exact in a float
static_assert(SPOT_MASK_ATLAS_TILES <= 7, "spot mask tiles must fit 3 bits, 7 meaning none");
static_assert(MAX_SHADOW_ATLAS_SLOTS < 63 && MAX_POINT_SHADOW_ATLAS_SLOTS < 63, "shadow map indices must fit 6 bits");
static_assert(MAX_SHADOWCASCADES <= 7, "shadow cascades must fit 3 bits");
static_assert(MAX_LIGHTS_PER_OBJECT <= 16, "the lights block tiers are 4, 8 and 16 lights");
static_assert(MAX_LIGHTS <= 8, "the legacy eyePos.w holds the spot mask tiles of 8 lights");

namespace {
    // a unit vector octahedral-encoded into [-1, 1]^2 (lights.glsl lightsOctahedralDecode)
    Vector2 octahedralEncode(const Vector3& vector){
        const float sum = std::abs(vector.x) + std::abs(vector.y) + std::abs(vector.z);
        if (sum <= 1e-8f)
            return Vector2(0.0f, 1.0f); // +Y
        float x = vector.x / sum;
        float y = vector.y / sum;
        if (vector.z < 0.0f){
            const float wrappedX = (1.0f - std::abs(y)) * (x >= 0.0f ? 1.0f : -1.0f);
            const float wrappedY = (1.0f - std::abs(x)) * (y >= 0.0f ? 1.0f : -1.0f);
            x = wrappedX;
            y = wrappedY;
        }
        return Vector2(x, y);
    }

    float lightLuminance(const Vector4& colorIntensity){
        return (0.2126f * colorIntensity.x + 0.7152f * colorIntensity.y + 0.0722f * colorIntensity.z) * std::max(colorIntensity.w, 0.0f);
    }

    // punctual.glsl getSpotAttenuation, towardsLight normalised
    float lightSpotAttenuation(const Vector3& towardsLight, const Vector4& direction, float outerConeCos, float innerConeCos){
        const float actualCos = Vector3(direction.x, direction.y, direction.z).normalized().dotProduct(-towardsLight);
        if (actualCos <= outerConeCos){
            return 0.0f;
        }
        if (actualCos >= innerConeCos){
            return 1.0f;
        }
        const float t = std::clamp((actualCos - outerConeCos) / (innerConeCos - outerConeCos), 0.0f, 1.0f);
        return t * t * (3.0f - 2.0f * t);
    }

    // Where a mesh is for its lights: the center and radius of its world bounds (an
    // instanced batch's, all instances), or its position when it has none yet
    struct LightBounds{
        AABB box;
        Vector3 center;
        float radius = 0.0f;
        bool hasBox = false;

        LightBounds(const AABB& worldAABB, const Vector3& position){
            hasBox = worldAABB.isFinite() && !worldAABB.isNull() && worldAABB != AABB::ZERO;
            if (hasBox){
                box = worldAABB;
                center = worldAABB.getCenter();
                radius = worldAABB.getHalfSize().length();
            }else{
                center = position;
            }
        }

        float distanceTo(const Vector3& point) const{
            return hasBox ? std::sqrt(box.squaredDistance(point)) : (point - center).length();
        }
    };

    // What a light gives a mesh, estimated at its bounds' center: the direction towards the
    // light and its attenuation there. The range window is measured to the bounds (a light
    // touching them counts), the falloff from the center, never closer than half the mesh's
    // size; withCone adds a spot's cone at the center.
    float estimateLightAttenuation(const Vector4& positionType, const Vector4& directionRange, const Vector4& coneShadow,
            const LightBounds& bounds, bool withCone, Vector3& towardsLight){
        if (positionType.w == 0.0f){
            towardsLight = -Vector3(directionRange.x, directionRange.y, directionRange.z).normalized();
            return 1.0f;
        }
        const Vector3 position(positionType.x, positionType.y, positionType.z);
        const Vector3 toLight = position - bounds.center;
        const float distance = toLight.length();
        towardsLight = (distance > 1e-5f) ? toLight / distance : Vector3(0.0f, 1.0f, 0.0f);

        const float falloffDistance = std::max(distance, std::max(0.5f * bounds.radius, 0.05f));
        float attenuation = 1.0f / (falloffDistance * falloffDistance);
        const float range = directionRange.w;
        if (range > 0.0f){
            const float ratio = bounds.distanceTo(position) / range;
            attenuation *= std::clamp(1.0f - ratio * ratio * ratio * ratio, 0.0f, 1.0f);
        }
        if (withCone && positionType.w == 2.0f){
            attenuation *= lightSpotAttenuation(towardsLight, directionRange, coneShadow.y, coneShadow.x);
        }
        return attenuation;
    }

    // Whether a spot light's outer cone can touch the bounds' sphere: false when the sphere
    // lies wholly outside the cone (its distance to the cone's surface is over its radius),
    // or, for a cone narrower than a hemisphere, wholly behind the apex
    bool spotReachesBounds(const Vector4& positionType, const Vector4& directionRange, float outerConeCos, const LightBounds& bounds){
        Vector3 axis(directionRange.x, directionRange.y, directionRange.z);
        const float axisLength = axis.length();
        if (axisLength <= 1e-6f)
            return true;
        axis = axis / axisLength;
        const Vector3 toCenter = bounds.center - Vector3(positionType.x, positionType.y, positionType.z);
        const float along = toCenter.dotProduct(axis);
        const float across = std::sqrt(std::max(toCenter.dotProduct(toCenter) - along * along, 0.0f));
        const float cosAngle = std::clamp(outerConeCos, -1.0f, 1.0f);
        const float sinAngle = std::sqrt(std::max(1.0f - cosAngle * cosAngle, 0.0f));
        if (cosAngle > 0.0f && along < -bounds.radius)
            return false;
        return across * cosAngle - along * sinAngle <= bounds.radius;
    }

    // a light over the cap into an L1 SH ambient: its Lambert diffuse, kept non-negative as
    // radiance * (1 + n.l) / 4 (the energy of max(n.l, 0) over the sphere), divided by PI
    // like every light lights.glsl hands a shader (light to multiply by albedo, as Lit's
    // BRDF_lambertian), so the SH is the scene ambient plus the folded lights as is
    void foldLight(const Vector4& colorIntensity, const Vector3& towardsLight, float attenuation, Vector4 sh[3]){
        const float scale = (0.25f / 3.14159265f) * colorIntensity.w * attenuation;
        for (int c = 0; c < 3; c++){
            const float radiance = scale * colorIntensity[c];
            sh[c] += Vector4(radiance * towardsLight.x, radiance * towardsLight.y, radiance * towardsLight.z, radiance);
        }
    }
}

bool RenderSystem::SceneLight::operator==(const SceneLight& other) const{
    return position_type == other.position_type &&
        direction_range == other.direction_range &&
        color_intensity == other.color_intensity &&
        cone_shadow == other.cone_shadow &&
        spot_mask == other.spot_mask &&
        cullingMask == other.cullingMask &&
        spotMaskTile == other.spotMaskTile &&
        shading == other.shading &&
        active == other.active;
}

int RenderSystem::getLightCapacity(int maxLightsPerObject){
    if (maxLightsPerObject <= 4)
        return 4;
    if (maxLightsPerObject <= 8)
        return 8;
    return 16;
}

// Snapshot of every light as the object lists copy it, after the lights' transforms, shadow
// slots and spot mask tiles are settled for this update. Lights whose data changed since the
// previous update go to changedLights; a change every mesh depends on bumps lightListEpoch.
void RenderSystem::updateSceneLights(){
    auto lights = scene->getComponentArray<LightComponent>();
    const size_t count = lights->size();

    // the arrays keep their capacity: nothing is allocated once the light count is reached
    std::swap(sceneLights, previousSceneLights);
    sceneLights.resize(count);
    changedLights.clear();

    bool restructured = previousSceneLights.size() != count || sceneLightEntities.size() != count;
    sceneLightEntities.resize(count);

    for (size_t i = 0; i < count; i++){
        const Entity entity = lights->getEntity(i);
        if (sceneLightEntities[i] != entity){
            sceneLightEntities[i] = entity;
            restructured = true;
        }

        const LightComponent& light = lights->getComponentFromIndex(i);
        const Transform* transform = scene->findComponent<Transform>(entity);
        const Vector3 position = transform ? transform->worldPosition : Vector3(0, 0, 0);

        float type = 0.0f;
        if (light.type == LightType::POINT)
            type = 1.0f;
        if (light.type == LightType::SPOT)
            type = 2.0f;

        const bool spotMask = light.type == LightType::SPOT && light.spotMaskReady && light.spotMaskTile >= 0;

        SceneLight& sceneLight = sceneLights[i];
        sceneLight.position_type = Vector4(position.x, position.y, position.z, type);
        sceneLight.direction_range = Vector4(light.worldDirection.x, light.worldDirection.y, light.worldDirection.z, light.range);
        sceneLight.color_intensity = Vector4(light.color.x, light.color.y, light.color.z, light.intensity);
        sceneLight.cone_shadow = Vector4(light.innerConeCos, light.outerConeCos, (float)light.shadowMapIndex, (float)light.numShadowCascades);
        sceneLight.spot_mask = Vector4(light.worldUp.x, light.worldUp.y, light.worldUp.z, spotMask ? light.spotMaskAspect : 0.0f);
        // the per-object lights block's form (includes/lights.glsl); spot_shadow.w is per draw
        const bool directional = light.type == LightType::DIRECTIONAL;
        const bool spot = light.type == LightType::SPOT;
        sceneLight.packed[0] = Vector4(position.x, position.y, position.z, directional ? 0.0f : 1.0f + std::max(light.range, 0.0f));
        sceneLight.packed[1] = Vector4(light.worldDirection.x, light.worldDirection.y, light.worldDirection.z, spot ? light.innerConeCos : -2.0f);
        sceneLight.packed[2] = Vector4(light.color.x * light.intensity, light.color.y * light.intensity, light.color.z * light.intensity,
                                       spot ? light.outerConeCos : -2.0f);
        const Vector2 up = octahedralEncode(light.worldUp);
        sceneLight.packed[3] = Vector4(up.x, up.y, spotMask ? light.spotMaskAspect : 0.0f, 0.0f);
        sceneLight.cullingMask = light.cullingMask;
        sceneLight.spotMaskTile = spotMask ? light.spotMaskTile : -1;
        sceneLight.shading = light.shading;
        sceneLight.active = light.intensity > 0.0f && isLightActive(entity, light);

        if (!restructured && !(sceneLight == previousSceneLights[i])){
            changedLights.push_back((int)i);
        }
    }

    // what every mesh's pick depends on
    const Vector3 ambient = scene->getGlobalIlluminationColorLinear() * scene->getGlobalIlluminationIntensity();
    const int cap = std::clamp(scene->getMaxLightsPerObject(), 1, MAX_LIGHTS_PER_OBJECT);
    if (restructured || ambient != lightListAmbient || cap != lightListCap){
        lightListEpoch++;
        lightListAmbient = ambient;
        lightListCap = cap;
        changedLights.clear();
    }
}

void RenderSystem::updateObjectLights(){
    // a directional light that only changed direction (a turning sun)
    auto onlyTurned = [this](int index){
        const SceneLight& light = sceneLights[index];
        if (light.position_type.w != 0.0f || index >= (int)previousSceneLights.size())
            return false;
        SceneLight before = previousSceneLights[index];
        before.direction_range = light.direction_range;
        before.position_type = light.position_type;
        return before == light;
    };
    // a directional light that only turned or changed colour or intensity (a day-night
    // cycle), still giving light
    auto onlyRetinted = [this](int index){
        const SceneLight& light = sceneLights[index];
        if (light.position_type.w != 0.0f || index >= (int)previousSceneLights.size())
            return false;
        SceneLight before = previousSceneLights[index];
        before.direction_range = light.direction_range;
        before.position_type = light.position_type;
        before.color_intensity = light.color_intensity;
        return before == light;
    };
    auto isPicked = [](const MeshLights& picked, int index){
        for (unsigned int k = 0; k < picked.count; k++){
            if (picked.lights[k] == index)
                return true;
        }
        return false;
    };

    auto meshes = scene->getComponentArray<MeshComponent>();
    for (int i = 0; i < meshes->size(); i++){
        MeshComponent& mesh = meshes->getComponentFromIndex(i);
        const Entity entity = meshes->getEntity(i);
        MeshLights& picked = mesh.lights;

        // a mesh drawn unlit or not at all picks nothing, and picks again from scratch
        // when it shows: it did not follow the light changes in between
        Transform* transform = scene->findComponent<Transform>(entity);
        if (!hasLights || !mesh.loaded || !mesh.receiveLights || !transform || !isRenderActive(entity)){
            picked.epoch = 0;
            continue;
        }

        const uint32_t layerBit = 1u << scene->getEntityLayer(entity);
        bool repick = picked.epoch != lightListEpoch || picked.layerBit != layerBit || picked.bounds != mesh.worldAABB;

        if (!repick && !changedLights.empty()){
            // a light that reached it changed, or a changed light reaches it now
            const LightBounds bounds(mesh.worldAABB, transform->worldPosition);
            for (int index : changedLights){
                if (onlyTurned(index) && isPicked(picked, index)){
                    // its rank and reach do not depend on its direction, which the draw reads
                    continue;
                }
                if (!picked.foldedDirectional && onlyRetinted(index) && isPicked(picked, index)){
                    // every directional light reaching the mesh is in its list (directional
                    // lights rank first), so its colour cannot change the pick or the SH;
                    // the draw reads it live
                    continue;
                }
                if (picked.reachMask & (1ull << (index & 63))){
                    repick = true;
                    break;
                }
                const SceneLight& light = sceneLights[index];
                if (!light.active || !(light.cullingMask & layerBit))
                    continue;
                const float range = light.direction_range.w;
                if (light.position_type.w == 0.0f || range <= 0.0f ||
                        bounds.distanceTo(Vector3(light.position_type.x, light.position_type.y, light.position_type.z)) < range){
                    repick = true;
                    break;
                }
            }
        }

        if (repick){
            selectMeshLights(entity, mesh, *transform);
        }
    }
}

// The lights reaching a mesh: directional ones always; point and spot ones whose range
// sphere touches its bounds; none whose culling mask leaves out its layer. Ranked by what
// they give at the bounds' center (directional first, then luminance * attenuation; a light
// already picked keeps a margin, so near-ties do not flip every frame), the top
// maxLightsPerObject are its list and the others go into its SH ambient, evaluated at the
// center, so a light never just vanishes.
void RenderSystem::selectMeshLights(Entity entity, MeshComponent& mesh, const Transform& transform){
    MeshLights& picked = mesh.lights;
    const LightBounds bounds(mesh.worldAABB, transform.worldPosition);
    const uint32_t layerBit = 1u << scene->getEntityLayer(entity);
    const int cap = std::min(lightListCap, MAX_LIGHTS_PER_OBJECT);

    // the previous pick earns a margin while the light list holds the same lights
    uint16_t previous[MAX_LIGHTS_PER_OBJECT];
    unsigned int previousCount = 0;
    if (picked.epoch == lightListEpoch){
        previousCount = picked.count;
        std::copy(picked.lights, picked.lights + previousCount, previous);
    }

    for (int c = 0; c < 3; c++){
        picked.sh[c] = Vector4(0.0f, 0.0f, 0.0f, lightListAmbient[c]);
    }

    picked.foldedDirectional = false;

    // a light over the cap goes into the SH, with its spot cone at the center
    auto fold = [&](int index){
        const SceneLight& light = sceneLights[index];
        if (light.position_type.w == 0.0f)
            picked.foldedDirectional = true;
        Vector3 towardsLight;
        const float attenuation = estimateLightAttenuation(light.position_type, light.direction_range, light.cone_shadow, bounds, true, towardsLight);
        foldLight(light.color_intensity, towardsLight, attenuation, picked.sh);
    };

    struct Candidate{
        int index;
        float score;
        bool directional;
    };
    auto better = [](const Candidate& a, const Candidate& b){
        if (a.directional != b.directional)
            return a.directional;
        if (a.score != b.score)
            return a.score > b.score;
        return a.index < b.index;
    };
    Candidate top[MAX_LIGHTS_PER_OBJECT];
    int topCount = 0;
    uint64_t reachMask = 0;

    for (int i = 0; i < (int)sceneLights.size(); i++){
        const SceneLight& light = sceneLights[i];
        if (!light.active || !(light.cullingMask & layerBit))
            continue;

        Candidate candidate{i, 0.0f, light.position_type.w == 0.0f};
        if (!candidate.directional){
            const float range = light.direction_range.w;
            if (range > 0.0f && bounds.distanceTo(Vector3(light.position_type.x, light.position_type.y, light.position_type.z)) >= range)
                continue;
        }
        reachMask |= 1ull << (i & 63);

        // a spot whose cone misses the bounds gives the mesh nothing (it stays in reachMask,
        // so turning it towards the mesh picks again)
        if (light.position_type.w == 2.0f && !spotReachesBounds(light.position_type, light.direction_range, light.cone_shadow.y, bounds))
            continue;

        // ranked without the spot cone: a spot may light part of the mesh away from its center
        Vector3 towardsLight;
        candidate.score = lightLuminance(light.color_intensity) *
            estimateLightAttenuation(light.position_type, light.direction_range, light.cone_shadow, bounds, false, towardsLight);
        for (unsigned int p = 0; p < previousCount; p++){
            if (previous[p] == i){
                candidate.score *= 1.25f;
                break;
            }
        }

        // keep top sorted best first; what does not fit goes into the SH
        if (topCount == cap){
            if (!better(candidate, top[cap - 1])){
                fold(i);
                continue;
            }
            fold(top[cap - 1].index);
            topCount--;
        }
        int slot = topCount;
        while (slot > 0 && better(candidate, top[slot - 1])){
            top[slot] = top[slot - 1];
            slot--;
        }
        top[slot] = candidate;
        topCount++;
    }

    // scene order, as the lights add up in the shader (and as they did before the lists)
    std::sort(top, top + topCount, [](const Candidate& a, const Candidate& b){ return a.index < b.index; });
    for (int k = 0; k < topCount; k++){
        picked.lights[k] = (uint16_t)top[k].index;
    }
    picked.count = (unsigned int)topCount;
    picked.bounds = mesh.worldAABB;
    picked.layerBit = layerBit;
    picked.reachMask = reachMask;
    picked.epoch = lightListEpoch;
}

// The object's lights for a submesh variant, as scene light indices into order: pixel
// lights first (USE_PUNCTUAL), then vertex lights (USE_VERTEX_LIGHTS), each in scene order;
// MIXED sends the lights shaded VERTEX per vertex and the others per pixel. At most the
// variant's capacity.
void RenderSystem::orderObjectLights(const MeshComponent& mesh, uint32_t properties, int order[MAX_LIGHTS_PER_OBJECT], int& pixelCount, int& vertexCount) const{
    const int capacity = (properties & (1u << 29)) ? 4 : ((properties & (1u << 30)) ? 16 : 8);
    const bool pixel = (properties & (1u << 3)) != 0;
    const bool vertex = (properties & (1u << 28)) != 0;
    const MeshLights& picked = mesh.lights;

    pixelCount = 0;
    vertexCount = 0;
    if (picked.epoch == 0)
        return;
    for (int pass = 0; pass < 2; pass++){
        for (unsigned int k = 0; k < picked.count; k++){
            const int index = picked.lights[k];
            if (index >= (int)sceneLights.size())
                continue;
            const bool perPixel = pixel && (!vertex || sceneLights[index].shading != LightShading::VERTEX);
            if (pass == 0 && perPixel && pixelCount < capacity){
                order[pixelCount++] = index;
            }else if (pass == 1 && !perPixel && vertex && pixelCount + vertexCount < capacity){
                order[pixelCount + vertexCount++] = index;
            }
        }
    }
}

// the shadow cascades of a light in the pass being drawn: directional cascades are fitted
// to its camera
int RenderSystem::passLightCascades(const SceneLight& light) const{
    const CameraShadows& camera = ps();
    const int slot = (int)light.cone_shadow.z;
    if (light.position_type.w == 0.0f && camera.ownLightCameras && slot >= 0 && slot < MAX_SHADOW_ATLAS_SLOTS)
        return (int)camera.lightCameras[slot].numCascades;
    return (int)light.cone_shadow.w;
}

// The per-object lights block of a submesh variant (layout in includes/lights.glsl), at the
// variant's capacity C: counts, then position_type[C], direction_inner[C], color_outer[C]
// (all the vertex stage reads), the SH ambient, spot_shadow[C].
void RenderSystem::buildObjectLights(const MeshComponent& mesh, uint32_t properties, object_lights_t& block) const{
    const int capacity = (properties & (1u << 29)) ? 4 : ((properties & (1u << 30)) ? 16 : 8);
    const MeshLights& picked = mesh.lights;

    int order[MAX_LIGHTS_PER_OBJECT];
    int pixelCount = 0;
    int vertexCount = 0;
    orderObjectLights(mesh, properties, order, pixelCount, vertexCount);

    Vector4* positionType = block.data + 1;
    Vector4* directionInner = positionType + capacity;
    Vector4* colorOuter = directionInner + capacity;
    Vector4* sh = colorOuter + capacity;
    Vector4* spotShadow = sh + 3;

    block.data[0] = Vector4((float)pixelCount, (float)vertexCount, 0.0f, 0.0f);
    for (int c = 0; c < 3; c++){
        sh[c] = (picked.epoch != 0) ? picked.sh[c] : Vector4(0.0f, 0.0f, 0.0f, lightListAmbient[c]);
    }

    for (int n = 0; n < pixelCount + vertexCount; n++){
        const SceneLight& light = sceneLights[order[n]];
        positionType[n] = light.packed[0];
        directionInner[n] = light.packed[1];
        colorOuter[n] = light.packed[2];
        spotShadow[n] = light.packed[3];
        // vertex lights take the cone, never the mask or a shadow
        const bool perPixel = n < pixelCount;
        const int tile = (perPixel && light.spotMaskTile >= 0) ? light.spotMaskTile : 7;
        const int shadowSlot = perPixel ? (int)light.cone_shadow.z : -1;
        const int cascades = perPixel ? passLightCascades(light) : 0;
        if (!perPixel)
            spotShadow[n].z = 0.0f;
        spotShadow[n].w = (float)((std::max(shadowSlot + 1, 0) & 63) | ((cascades & 7) << 6) | ((tile & 7) << 9));
    }
}

// An older fork's u_fs_lighting: the camera's part, then the object's first MAX_LIGHTS
// lights, all per pixel. What the block cannot hold (lights over the cap, past MAX_LIGHTS)
// is added to its flat ambient as the SH would, which changes nothing when none is left out.
void RenderSystem::buildLegacyLighting(const MeshComponent& mesh, uint32_t properties, fs_lighting_t& block) const{
    block = ps().fs_lighting;

    // every picked light per pixel, at the largest capacity
    const uint32_t legacyProperties = (properties | (1u << 3) | (1u << 30)) & ~((1u << 28) | (1u << 29));
    int order[MAX_LIGHTS_PER_OBJECT];
    int pixelCount = 0;
    int vertexCount = 0;
    orderObjectLights(mesh, legacyProperties, order, pixelCount, vertexCount);
    const int count = std::min(pixelCount, MAX_LIGHTS);

    // more than the block holds: the directional lights first (they rank first), each group
    // in scene order; MAX_LIGHTS or fewer keep scene order, as the lights added up before
    if (pixelCount > MAX_LIGHTS){
        int sorted[MAX_LIGHTS_PER_OBJECT];
        int n = 0;
        for (int pass = 0; pass < 2; pass++){
            for (int k = 0; k < pixelCount; k++){
                if ((sceneLights[order[k]].position_type.w == 0.0f) == (pass == 0))
                    sorted[n++] = order[k];
            }
        }
        std::copy(sorted, sorted + pixelCount, order);
    }

    uint32_t tiles = 0;
    for (int n = 0; n < MAX_LIGHTS; n++){
        if (n < count){
            const SceneLight& light = sceneLights[order[n]];
            block.position_type[n] = light.position_type;
            block.direction_range[n] = light.direction_range;
            block.color_intensity[n] = light.color_intensity;
            block.inCon_ouCon_shadows_cascades[n] = light.cone_shadow;
            block.inCon_ouCon_shadows_cascades[n].w = (float)passLightCascades(light);
            block.spotUp_maskAspect[n] = light.spot_mask;
            const uint32_t tile = (light.spotMaskTile >= 0) ? (uint32_t)light.spotMaskTile : 7u;
            if (tile == 7u)
                block.spotUp_maskAspect[n].w = 0.0f;
            tiles |= tile << (3 * n);
        }else{
            // adds nothing
            block.position_type[n] = Vector4(0.0f, 0.0f, 0.0f, 0.0f);
            block.direction_range[n] = Vector4(0.0f, 0.0f, 0.0f, 0.0f);
            block.color_intensity[n] = Vector4(0.0f, 0.0f, 0.0f, 0.0f);
            block.inCon_ouCon_shadows_cascades[n] = Vector4(0.0f, 0.0f, -1.0f, 0.0f);
            block.spotUp_maskAspect[n] = Vector4(0.0f, 0.0f, 0.0f, 0.0f);
            tiles |= 7u << (3 * n);
        }
    }
    block.eyePos.w = (float)tiles;

    const MeshLights& picked = mesh.lights;
    Vector4 sh[3];
    bool leftOut = false;
    for (int c = 0; c < 3; c++){
        sh[c] = (picked.epoch != 0) ? picked.sh[c] : Vector4(0.0f, 0.0f, 0.0f, lightListAmbient[c]);
        leftOut = leftOut || sh[c].w != lightListAmbient[c];
    }
    if (pixelCount > count){
        const LightBounds bounds(mesh.lights.bounds, Vector3(0.0f, 0.0f, 0.0f));
        for (int n = count; n < pixelCount; n++){
            const SceneLight& light = sceneLights[order[n]];
            Vector3 towardsLight;
            const float attenuation = estimateLightAttenuation(light.position_type, light.direction_range, light.cone_shadow, bounds, true, towardsLight);
            foldLight(light.color_intensity, towardsLight, attenuation, sh);
        }
        leftOut = true;
    }
    if (leftOut){
        // the SH at no particular normal (its constant term); its light part is already
        // divided by PI (foldLight), as Lit-like forks divide their direct light
        Vector3 flat;
        for (int c = 0; c < 3; c++){
            flat[c] = lightListAmbient[c] + std::max(sh[c].w - lightListAmbient[c], 0.0f);
        }
        block.globalIllum = Vector4(flat.x, flat.y, flat.z, 1.0f);
    }
}

bool RenderSystem::loadLights2D(){
    bool prevHasShadows2D = hasShadows2D;
    hasShadows2D = false;

    auto lights2d = scene->getComponentArray<Light2DComponent>();
    auto occluders = scene->getComponentArray<ShadowCaster2DComponent>();

    bool anyOccluder = false;
    for (int i = 0; i < occluders->size(); i++){
        if (occluders->getComponentFromIndex(i).enabled){
            anyOccluder = true;
            break;
        }
    }

    int freeRow = 0;
    unsigned int maxResolution = 0;
    for (int i = 0; i < numLights2D; i++){
        Light2DComponent& light = lights2d->getComponentFromIndex(i);

        light.shadowMapIndex = -1;

        if (light.shadows && anyOccluder){
            light.shadowMapIndex = freeRow++;
            maxResolution = std::max(maxResolution, light.mapResolution);
        }
    }

    hasShadows2D = (freeRow > 0);

    if (hasShadows2D){
        if (!ensureShadow2DAtlas(maxResolution)){
            Log::warn("Failed to create 2D shadow atlas");
            for (int i = 0; i < numLights2D; i++){
                lights2d->getComponentFromIndex(i).shadowMapIndex = -1;
            }
            hasShadows2D = false;
        }
    }

    if (!hasShadows2D){
        if (hasShadow2DAtlas){
            needUpdateShadowBindings = true;
        }
        hasShadow2DAtlas = false;
    }

    // USE_SHADOWS_2D is baked into mesh shader variants, so a flip (first shadow
    // light + occluder appearing, or the last one going away) rebuilds meshes
    if (hasShadows2D != prevHasShadows2D){
        needReloadMeshVariants();
    }

    return true;
}

bool RenderSystem::ensureShadow2DAtlas(unsigned int width){
    int maxTextureSize = sg_query_limits().max_image_size_2d;
    if (maxTextureSize < 1){
        maxTextureSize = 4096;
    }
    if (width < 1){
        width = 1;
    }
    if ((int)width > maxTextureSize){
        Log::warn("2D shadow atlas width clamped from %u to %d (GPU max texture size)", width, maxTextureSize);
        width = (unsigned int)maxTextureSize;
    }

    if (shadow2DAtlasFramebuffer.isCreated() && shadow2DAtlasWidth == width){
        if (!hasShadow2DAtlas){
            needUpdateShadowBindings = true;
        }
        hasShadow2DAtlas = true;
        return true;
    }

    shadow2DAtlasFramebuffer.destroyFramebuffer();
    if (!shadow2DAtlasFramebuffer.createFramebuffer(
            TextureType::TEXTURE_2D, (int)width, MAX_LIGHTS_2D,
            TextureFilter::NEAREST, TextureFilter::NEAREST,
            TextureWrap::CLAMP_TO_EDGE, TextureWrap::CLAMP_TO_EDGE, true)){
        hasShadow2DAtlas = false;
        return false;
    }

    shadow2DAtlasWidth = width;
    hasShadow2DAtlas = true;
    needUpdateShadowBindings = true;
    return true;
}

void RenderSystem::processLights2D(){
    auto lights2d = scene->getComponentArray<Light2DComponent>();

    for (int i = 0; i < numLights2D; i++){
        Light2DComponent& light = lights2d->getComponentFromIndex(i);
        Entity entity = lights2d->getEntity(i);
        Transform* transform = scene->findComponent<Transform>(entity);

        Vector3 worldPosition;
        if (transform){
            worldPosition = Vector3(transform->worldPosition);
        }

        fs_lighting2d.position_range[i] = Vector4(worldPosition.x, worldPosition.y, light.height, std::max(light.range, 0.001f));
        const float intensity = isLight2DActive(entity, light) ? light.intensity : 0.0f;
        fs_lighting2d.color_intensity[i] = Vector4(light.color.x, light.color.y, light.color.z, intensity);
        fs_lighting2d.falloff_shadow[i] = Vector4(light.falloff, (float)light.shadowMapIndex, light.shadowSoftness, light.shadowBias);
    }

    Vector3 ambient = scene->getAmbientLight2DColorLinear() * scene->getAmbientLight2DIntensity();
    fs_lighting2d.ambient = Vector4(ambient.x, ambient.y, ambient.z, (float)numLights2D);

    // PCF tap radius from the scene's 2D shadow quality (taps = 2*radius + 1)
    float pcfRadius = 2.0f;
    switch (scene->getShadow2DQuality()){
        case ShadowQuality::NONE:   pcfRadius = 0.0f; break;
        case ShadowQuality::LOW:    pcfRadius = 2.0f; break;
        case ShadowQuality::MEDIUM: pcfRadius = 4.0f; break;
        case ShadowQuality::HIGH:   pcfRadius = 6.0f; break;
    }

    // rows without shadows keep shadowMapIndex -1 and never sample the atlas
    float atlasW = (hasShadow2DAtlas && shadow2DAtlasWidth > 0) ? (float)shadow2DAtlasWidth : 1.0f;
    fs_lighting2d.atlasInfo = Vector4(1.0f / atlasW, 1.0f / (float)MAX_LIGHTS_2D, atlasW, pcfRadius);
}

bool RenderSystem::loadAndProcessFog(){

    FogComponent* fog = scene->findComponentFromIndex<FogComponent>(0);
    const bool hadFog = hasFog;
    hasFog = false;
    if (fog){
        hasFog = true;

        // the height fog reads its own block (u_fs_heightFog, the 'Hfg' variants): this one
        // stays a linear fog's
        int fogTypeI = 0;
        if (fog->type == FogType::EXPONENTIAL){
            fogTypeI = 1;
        }else if (fog->type == FogType::EXPONENTIALSQUARED){
            fogTypeI = 2;
        }

        fs_fog.color_type = Vector4(fog->color.x, fog->color.y, fog->color.z, fogTypeI);
        fs_fog.density_start_end = Vector4(fog->density, 0.0, fog->linearStart, fog->linearEnd);
    }

    // fog is a shader variant: a Fog added or removed while playing switches every mesh
    if (hasFog != hadFog){
        needReloadMeshVariants();
    }

    return hasFog;
}

// pooled maps stay alive in TexturePool for reuse; only unpooled ones are destroyed
void RenderSystem::releaseSkyEnvironment(SkyboxComponent& sky){
    if (sky.irradianceMap && sky.irradianceMap.use_count() == 1){
        sky.irradianceMap->destroyTexture();
    }
    if (sky.prefilteredMap && sky.prefilteredMap.use_count() == 1){
        sky.prefilteredMap->destroyTexture();
    }
    sky.irradianceMap.reset();
    sky.prefilteredMap.reset();
    sky.envMapsLoaded = false;
}

// generates (or regenerates) the IBL environment maps from the sky texture,
// must run while the texture pixels are still on CPU (before the sky GPU upload)
void RenderSystem::updateSkyEnvironment(SkyboxComponent& sky){
    if (!sky.needUpdateEnvironment)
        return;

    if (sky.texture.empty() || sky.texture.getNumFaces() != 6){
        releaseSkyEnvironment(sky);
        sky.needUpdateEnvironment = false;
        return;
    }

    // generation is expensive (cosine + GGX convolution), reuse cached maps
    const std::string texId = sky.texture.getId();
    const std::string irradianceId = texId + "|ibl|irradiance";
    const std::string prefilteredId = texId + "|ibl|prefiltered";

    if (!texId.empty()){
        std::shared_ptr<TextureRender> irradiance = TexturePool::get(irradianceId);
        std::shared_ptr<TextureRender> prefiltered = TexturePool::get(prefilteredId);
        if (irradiance && prefiltered){
            // A hidden sky never clears needUpdateTexture, so this is requested every frame
            if (sky.irradianceMap != irradiance || sky.prefilteredMap != prefiltered){
                releaseSkyEnvironment(sky);
                sky.irradianceMap = irradiance;
                sky.prefilteredMap = prefiltered;
                sky.envMapsLoaded = true;
                needReloadMeshes();
            }
            sky.needUpdateEnvironment = false;
            return;
        }
    }

    TextureLoadResult texResult = sky.texture.load();

    if (texResult.state == ResourceLoadState::Loading){
        return; // try again next frame
    }

    if (texResult.state == ResourceLoadState::Finished && texResult.data){
        releaseSkyEnvironment(sky);

        std::shared_ptr<TextureRender> irradiance = std::make_shared<TextureRender>();
        std::shared_ptr<TextureRender> prefiltered = std::make_shared<TextureRender>();

        IBLEnvironment::HorizonRing horizonRing;
        if (IBLEnvironment::generate(texId, *(texResult.data), *irradiance, *prefiltered, &horizonRing)){
            AtmosphereRenderer::storeHorizonRing(texId, horizonRing);
            sky.irradianceMap = irradiance;
            sky.prefilteredMap = prefiltered;
            sky.envMapsLoaded = true;
            TexturePool::add(irradianceId, irradiance);
            TexturePool::add(prefilteredId, prefiltered);
            needReloadMeshes();
        }
    }

    sky.needUpdateEnvironment = false;
}

void RenderSystem::releaseReflectionProbeMaps(ReflectionProbeRuntime& runtime){
    if (runtime.irradianceMap && runtime.irradianceMap.use_count() == 1)
        runtime.irradianceMap->destroyTexture();
    if (runtime.prefilteredMap && runtime.prefilteredMap.use_count() == 1)
        runtime.prefilteredMap->destroyTexture();
    runtime.irradianceMap.reset();
    runtime.prefilteredMap.reset();
}

void RenderSystem::destroyReflectionProbe(Entity entity, ReflectionProbeComponent& probe){
    auto it = reflectionProbeRuntimes.find(entity);
    if (it != reflectionProbeRuntimes.end()){
        it->second->captureFramebuffer.destroyFramebuffer();
        releaseReflectionProbeMaps(*it->second);
        reflectionProbeRuntimes.erase(it);
    }
    probe.texture.destroy();
    if (activeReflectionProbe == entity)
        activeReflectionProbe = NULL_ENTITY;
}

void RenderSystem::updateReflectionProbes(double dt){
    auto probes = scene->getComponentArray<ReflectionProbeComponent>();
    bool newHasReflectionProbes = probes->size() > 0;
    if (newHasReflectionProbes != hasReflectionProbes){
        hasReflectionProbes = newHasReflectionProbes;
        needReloadMeshVariants();
    }

    activeReflectionProbe = NULL_ENTITY;

    // Finish an already-started cube before scheduling another probe. This keeps
    // the one-face-per-frame budget without interleaving two partial cubemaps.
    for (int i = 0; i < probes->size(); i++){
        Entity entity = probes->getEntity(i);
        auto runtimeIt = reflectionProbeRuntimes.find(entity);
        if (runtimeIt != reflectionProbeRuntimes.end() && runtimeIt->second->captureInProgress &&
            probes->getComponentFromIndex(i).needUpdate && scene->findComponent<Transform>(entity)){
            activeReflectionProbe = entity;
            break;
        }
    }

    for (int i = 0; i < probes->size(); i++){
        Entity entity = probes->getEntity(i);
        ReflectionProbeComponent& probe = probes->getComponentFromIndex(i);
        Transform* transform = scene->findComponent<Transform>(entity);
        if (!transform)
            continue;

        auto& runtimePtr = reflectionProbeRuntimes[entity];
        if (!runtimePtr)
            runtimePtr = std::make_unique<ReflectionProbeRuntime>();
        ReflectionProbeRuntime& runtime = *runtimePtr;

        if (!runtime.modeInitialized){
            runtime.lastMode = probe.mode;
            runtime.modeInitialized = true;
        }else if (runtime.lastMode != probe.mode){
            if (activeReflectionProbe == entity)
                activeReflectionProbe = NULL_ENTITY;
            runtime.lastMode = probe.mode;
            runtime.captureInProgress = false;
            runtime.nextFace = 0;
            runtime.ready = false;
            runtime.elapsed = 0.0f;
            runtime.retryDelay = 0.0f;
            probe.needUpdate = true;
        }

        // Capture-affecting editor changes can arrive while a six-face job is
        // already running. Restart that job so no face uses the old settings.
        if (runtime.observedCaptureRevision != probe.captureRevision){
            if (activeReflectionProbe == entity)
                activeReflectionProbe = NULL_ENTITY;
            runtime.observedCaptureRevision = probe.captureRevision;
            runtime.captureInProgress = false;
            runtime.nextFace = 0;
        }

        auto scheduleCapture = [&](){
            if (activeReflectionProbe != NULL_ENTITY && activeReflectionProbe != entity)
                return;
            if (!runtime.captureInProgress){
                runtime.captureInProgress = true;
                // A finished cube keeps being sampled while its faces are
                // rewritten. Only the first capture leaves the probe unselectable.
                if (!runtime.captureFramebuffer.isCreated())
                    runtime.ready = false;
                runtime.nextFace = 0;
                // Freeze the entity origin for all six faces. boxOffset controls
                // only the influence volume and is intentionally not a capture offset.
                runtime.capturePosition = transform->worldPosition;
            }
            activeReflectionProbe = entity;
        };

        if (probe.mode == ReflectionProbeMode::STATIC){
            bool hasAuthoredCubemap = !probe.texture.empty() && probe.texture.getNumFaces() == 6;
            if (hasAuthoredCubemap && runtime.ready)
                runtime.capturedPosition = transform->worldPosition;

            if (!probe.needUpdate)
                continue;

            if (!Engine::isViewLoaded())
                continue;

            // An authored cubemap is the preferred static path because it can be
            // GGX-prefiltered once and cached. With no cubemap, capture the scene
            // once on load (or when Refresh is pressed) and then keep that result.
            if (!hasAuthoredCubemap){
                runtime.retryDelay = 0.0f;
                releaseReflectionProbeMaps(runtime);
                scheduleCapture();
                continue;
            }

            if (activeReflectionProbe == entity)
                activeReflectionProbe = NULL_ENTITY;
            runtime.captureInProgress = false;
            runtime.captureFramebuffer.destroyFramebuffer();
            runtime.resolution = 0;
            runtime.nextFace = 0;
            runtime.ready = false;
            releaseReflectionProbeMaps(runtime);

            const std::string textureId = probe.texture.getId();
            const std::string irradianceId = textureId + "|probe|irradiance";
            const std::string prefilteredId = textureId + "|probe|prefiltered";
            if (!textureId.empty()){
                runtime.irradianceMap = TexturePool::get(irradianceId);
                runtime.prefilteredMap = TexturePool::get(prefilteredId);
                if (runtime.irradianceMap && runtime.prefilteredMap){
                    runtime.ready = true;
                    runtime.capturedPosition = transform->worldPosition;
                    runtime.retryDelay = 0.0f;
                    probe.needUpdate = false;
                    continue;
                }
            }

            runtime.retryDelay = std::max(0.0f, runtime.retryDelay - (float)dt);
            if (runtime.retryDelay > 0.0f)
                continue;

            TextureLoadResult loadResult = probe.texture.load();
            if (loadResult.state == ResourceLoadState::Loading)
                continue;

            if (loadResult.state == ResourceLoadState::Finished && loadResult.data){
                auto irradiance = std::make_shared<TextureRender>();
                auto prefiltered = std::make_shared<TextureRender>();
                if (IBLEnvironment::generate(textureId, *loadResult.data, *irradiance, *prefiltered)){
                    runtime.irradianceMap = irradiance;
                    runtime.prefilteredMap = prefiltered;
                    runtime.ready = true;
                    runtime.capturedPosition = transform->worldPosition;
                    runtime.retryDelay = 0.0f;
                    probe.needUpdate = false;
                    if (!textureId.empty()){
                        TexturePool::add(irradianceId, irradiance);
                        TexturePool::add(prefilteredId, prefiltered);
                    }
                    continue;
                }
            }

            // Texture::load disables its own load latch after a failure. Re-arm
            // it and back off so a transient authored-bake failure is recoverable
            // without logging/retrying on every frame.
            if (loadResult.state == ResourceLoadState::Failed)
                probe.texture.retryLoad();
            runtime.retryDelay = 1.0f;
            continue;
        }

        // A component switched from an authored static probe to dynamic must not
        // keep selecting the stale prefiltered map while its live capture updates.
        releaseReflectionProbeMaps(runtime);
        runtime.retryDelay = 0.0f;
        runtime.elapsed += (float)dt;
        Vector3 capturePosition = transform->worldPosition;
        if (probe.updateMode == ReflectionProbeUpdateMode::ON_MOVE && runtime.ready){
            if ((capturePosition - runtime.capturedPosition).length() > 0.0001f)
                probe.needUpdate = true;
        }else if (probe.updateMode == ReflectionProbeUpdateMode::INTERVAL && runtime.ready){
            if (runtime.elapsed >= std::max(probe.updateInterval, 0.01f))
                probe.needUpdate = true;
        }

        if (probe.needUpdate)
            scheduleCapture();
    }
}

bool RenderSystem::selectReflectionProbe(const Vector3& worldPosition, fs_reflection_probe_t& params, TextureRender*& texture){
    params.position_weight = Vector4(0.0, 0.0, 0.0, 0.0);
    params.boxMin_intensity = Vector4(-1.0, -1.0, -1.0, 1.0);
    params.boxMax_lod = Vector4(1.0, 1.0, 1.0, 0.0);
    texture = &emptyCubeBlack;

    if (capturingReflectionProbe)
        return false;

    auto probes = scene->getComponentArray<ReflectionProbeComponent>();
    ReflectionProbeComponent* selected = nullptr;
    ReflectionProbeRuntime* selectedRuntime = nullptr;
    Vector3 selectedMin;
    Vector3 selectedMax;
    float selectedDistance = 0.0f;

    for (int i = 0; i < probes->size(); i++){
        Entity entity = probes->getEntity(i);
        ReflectionProbeComponent& probe = probes->getComponentFromIndex(i);
        Transform* transform = scene->findComponent<Transform>(entity);
        auto runtimeIt = reflectionProbeRuntimes.find(entity);
        if (!transform || runtimeIt == reflectionProbeRuntimes.end() || !runtimeIt->second->ready)
            continue;

        Vector3 center = transform->modelMatrix * probe.boxOffset;
        Vector3 half(
            std::abs(probe.boxSize.x * transform->worldScale.x) * 0.5f,
            std::abs(probe.boxSize.y * transform->worldScale.y) * 0.5f,
            std::abs(probe.boxSize.z * transform->worldScale.z) * 0.5f);
        Vector3 boxMin = center - half;
        Vector3 boxMax = center + half;
        if (worldPosition.x < boxMin.x || worldPosition.y < boxMin.y || worldPosition.z < boxMin.z ||
            worldPosition.x > boxMax.x || worldPosition.y > boxMax.y || worldPosition.z > boxMax.z)
            continue;

        float distance = (worldPosition - center).length();
        if (!selected || probe.priority > selected->priority ||
            (probe.priority == selected->priority && distance < selectedDistance)){
            selected = &probe;
            selectedRuntime = runtimeIt->second.get();
            selectedMin = boxMin;
            selectedMax = boxMax;
            selectedDistance = distance;
        }
    }

    if (!selected || !selectedRuntime)
        return false;

    bool hasPrefilteredMap = selectedRuntime->prefilteredMap && selectedRuntime->prefilteredMap->isCreated();
    if (hasPrefilteredMap){
        texture = selectedRuntime->prefilteredMap.get();
    }else{
        if (!selectedRuntime->captureFramebuffer.isCreated())
            return false;
        texture = &selectedRuntime->captureFramebuffer.getColorTexture();
    }

    float edgeDistance = std::min(
        std::min(worldPosition.x - selectedMin.x, selectedMax.x - worldPosition.x),
        std::min(std::min(worldPosition.y - selectedMin.y, selectedMax.y - worldPosition.y),
                 std::min(worldPosition.z - selectedMin.z, selectedMax.z - worldPosition.z)));
    Vector3 selectedHalf = (selectedMax - selectedMin) * 0.5f;
    float maxBlendDistance = std::max(0.0f, std::min(selectedHalf.x, std::min(selectedHalf.y, selectedHalf.z)));
    float blendDistance = std::min(std::max(selected->blendDistance, 0.0f), maxBlendDistance);
    float weight = blendDistance > 0.0001f
        ? std::max(0.0f, std::min(1.0f, edgeDistance / blendDistance))
        : 1.0f;

    params.position_weight = Vector4(selectedRuntime->capturedPosition.x, selectedRuntime->capturedPosition.y,
                                     selectedRuntime->capturedPosition.z, weight);
    params.boxMin_intensity = Vector4(selectedMin.x, selectedMin.y, selectedMin.z, std::max(selected->intensity, 0.0f));
    params.boxMax_lod = Vector4(selectedMax.x, selectedMax.y, selectedMax.z,
        hasPrefilteredMap ? (float)(IBLEnvironment::SPECULAR_MIPS - 1) : 0.0f);
    return true;
}

void RenderSystem::renderReflectionProbeCapture(){
    if (activeReflectionProbe == NULL_ENTITY || !scene->isEntityCreated(activeReflectionProbe))
        return;

    ReflectionProbeComponent* probe = scene->findComponent<ReflectionProbeComponent>(activeReflectionProbe);
    Transform* probeTransform = scene->findComponent<Transform>(activeReflectionProbe);
    auto runtimeIt = reflectionProbeRuntimes.find(activeReflectionProbe);
    if (!probe || !probeTransform || runtimeIt == reflectionProbeRuntimes.end())
        return;

    ReflectionProbeRuntime& runtime = *runtimeIt->second;
    if (!runtime.captureInProgress || !probe->needUpdate)
        return;
    unsigned int resolution = std::max(16u, std::min(probe->resolution, 1024u));
    if (!runtime.captureFramebuffer.isCreated() || runtime.resolution != resolution){
        runtime.captureFramebuffer.destroyFramebuffer();
        if (!runtime.captureFramebuffer.createFramebuffer(TextureType::TEXTURE_CUBE, (int)resolution, (int)resolution,
                TextureFilter::LINEAR, TextureFilter::LINEAR, TextureWrap::CLAMP_TO_EDGE, TextureWrap::CLAMP_TO_EDGE, false)){
            return;
        }
        runtime.resolution = resolution;
        runtime.nextFace = 0;
        runtime.ready = false;
    }

    static const Vector3 directions[6] = {
        Vector3(1, 0, 0), Vector3(-1, 0, 0), Vector3(0, 1, 0),
        Vector3(0, -1, 0), Vector3(0, 0, 1), Vector3(0, 0, -1)
    };
    static const Vector3 ups[6] = {
        Vector3(0, -1, 0), Vector3(0, -1, 0), Vector3(0, 0, 1),
        Vector3(0, 0, -1), Vector3(0, -1, 0), Vector3(0, -1, 0)
    };

    int face = runtime.nextFace;
    Vector3 capturePosition = runtime.capturePosition;

    Transform captureTransform;
    captureTransform.position = capturePosition;
    captureTransform.worldPosition = capturePosition;
    captureTransform.worldScale = Vector3(1.0f, 1.0f, 1.0f);
    captureTransform.parent = NULL_ENTITY;

    CameraComponent captureCamera;
    captureCamera.type = CameraType::CAMERA_PERSPECTIVE;
    captureCamera.renderToTexture = true;
    // cube faces flip on the opposite backends from 2D RTT (see
    // isRenderingFlipped), so the matching winding is PIP_RTT_INVERT
    captureCamera.invertCulling = true;
    captureCamera.autoResize = false;
    captureCamera.useTarget = true;
    captureCamera.target = capturePosition + directions[face];
    captureCamera.up = ups[face];
    captureCamera.yfov = 1.57079632679f;
    captureCamera.aspect = 1.0f;
    captureCamera.nearClip = std::max(probe->nearClip, 0.001f);
    captureCamera.farClip = std::max(probe->farClip, captureCamera.nearClip + 0.001f);
    captureCamera.cullingMask = probe->cullingMask;
    updateCamera(captureCamera, captureTransform);

    // the face's own lighting and shadow maps: directional cascades fitted to the face,
    // spot and point maps shared when its culling mask selects the same casters
    CameraShadows& captureShadows = getCameraShadows(activeReflectionProbe);
    prepareCameraShadows(captureShadows, captureCamera, captureTransform, mainTerrainView(), true);
    buildCameraShadowInstances(captureShadows);
    // the face's frame block, for its shadow passes and its draws below
    updatePassFrame(captureCamera, captureTransform, Vector2((float)resolution, (float)resolution));
    renderCameraShadows(captureShadows);
    CameraShadows* previousShadows = passShadows;
    passShadows = &captureShadows;

    capturingReflectionProbe = true;
    // the face's draws flip Y like a cube face (isRenderingFlipped reads the flag), so the
    // frame block's matrices are made again for them: the shadow passes above drew with
    // the camera's unflipped ones
    updatePassFrame(captureCamera, captureTransform, Vector2((float)resolution, (float)resolution));
    passResolution = Vector2((float)resolution, (float)resolution);
    runtime.capturePass.setClearColor(probe->includeSky ? scene->getBackgroundColor() : Vector4(0.0f, 0.0f, 0.0f, 1.0f));
    runtime.capturePass.setLabel("reflection probe");
    runtime.capturePass.startRenderPass(&runtime.captureFramebuffer, (size_t)face);

    auto skys = scene->getComponentArray<SkyboxComponent>();
    if (probe->includeSky && skys->size() > 0){
        SkyboxComponent& sky = skys->getComponentFromIndex(0);
        if (sky.visible){
            updateSkyViewProjection(sky, captureCamera);
            drawSky(sky, PIP_RTT_INVERT, sky.skyViewProjectionMatrix);
        }
    }

    // the face's height fog and atmosphere: analytic fog, the sky fill and the Sky Map clouds
    // (no volumetric passes off the view's camera)
    const Rect captureViewport(0.0f, 0.0f, (float)resolution, (float)resolution);
    AtmosphereRenderer::PassPlan capturePlan = atmosphere.planPass(captureCamera, false, probe->includeSky);
    if (capturePlan.any())
        atmosphere.renderPassTargets(capturePlan, captureCamera, captureTransform, nullptr, false, resolution, resolution);
    if (heightFogVariants || capturePlan.any())
        atmosphere.fillHeightFog(passHeightFog, captureCamera, captureTransform, captureViewport, resolution, isRenderingFlipped(captureCamera), 0.0f);

    // the opaque meshes in the order a camera pass draws them: sortPriority (stencil masks
    // before their readers, occluders before through-walls outlines), then nearest first
    std::vector<OpaqueRenderData>& opaqueRenders = opaqueQueue;
    opaqueRenders.clear();
    auto transforms = scene->getComponentArray<Transform>();
    for (int i = 0; i < transforms->size(); i++){
        Entity entity = transforms->getEntity(i);
        if (entity == activeReflectionProbe)
            continue;
        Transform& transform = transforms->getComponentFromIndex(i);
        MeshComponent* mesh = scene->findComponent<MeshComponent>(entity);
        if (!mesh || !transform.visible || mesh->transparent || !mesh->renderInReflectionProbes || !isRenderActive(entity) ||
                !isLayerVisible(entity, probe->cullingMask))
            continue;

        updateMVP(i, transform, captureCamera, captureTransform);
        InstancedMeshComponent* instanced = scene->findComponent<InstancedMeshComponent>(entity);
        TerrainComponent* terrain = scene->findComponent<TerrainComponent>(entity);
        TilemapComponent* tilemap = scene->findComponent<TilemapComponent>(entity);
        const float distance = mesh->worldAABB.isNull() ? 0.0f : mesh->worldAABB.squaredDistance(capturePosition);
        opaqueRenders.push_back({entity, mesh, instanced, terrain, tilemap, &transform, distance, mesh->opaqueSortPriority, (uint32_t)opaqueRenders.size()});
    }
    std::sort(opaqueRenders.begin(), opaqueRenders.end(), OpaqueRenderComparison());
    for (const OpaqueRenderData& opaque : opaqueRenders){
        drawMesh(opaque.entity, *opaque.mesh, *opaque.transform, captureCamera, captureTransform, PIP_RTT_INVERT, opaque.instmesh, opaque.terrain, opaque.tilemap, mainTerrainView());
    }
    if (capturePlan.any())
        atmosphere.drawComposites(capturePlan, PIP_RTT_INVERT, 1, captureViewport, resolution, isRenderingFlipped(captureCamera), captureCamera, captureTransform);

    runtime.capturePass.endRenderPass();
    capturingReflectionProbe = false;
    passShadows = previousShadows;

    delete captureCamera.framebuffer;
    captureCamera.framebuffer = nullptr;

    runtime.nextFace++;
    if (runtime.nextFace >= 6){
        runtime.nextFace = 0;
        runtime.ready = true;
        runtime.captureInProgress = false;
        runtime.elapsed = 0.0f;
        runtime.capturedPosition = capturePosition;
        probe->needUpdate = false;
        activeReflectionProbe = NULL_ENTITY;
    }
}

void RenderSystem::initShadowAtlasRects(){
    for (int s = 0; s < MAX_SHADOW_ATLAS_SLOTS; s++){
        if (s >= shadowAtlasUsedSlots || shadowAtlasCols < 1 || shadowAtlasRows < 1){
            primaryView.shadows.fs_shadows.atlasRect[s] = Vector4(0.0f, 0.0f, 0.0f, 0.0f);
            continue;
        }

        int col = s % shadowAtlasCols;
        int row = s / shadowAtlasCols;
        primaryView.shadows.fs_shadows.atlasRect[s] = Vector4(
            (float)col / (float)shadowAtlasCols,
            (float)row / (float)shadowAtlasRows,
            1.0f / (float)shadowAtlasCols,
            1.0f / (float)shadowAtlasRows);
    }
}

void RenderSystem::initShadowPointAtlasRects(){
    for (int s = 0; s < MAX_POINT_SHADOW_ATLAS_SLOTS; s++){
        int col = s % SHADOW_POINT_ATLAS_COLS;
        int row = s / SHADOW_POINT_ATLAS_COLS;
        primaryView.shadows.fs_point_shadows.atlasRect[s] = Vector4(
            (float)col / (float)SHADOW_POINT_ATLAS_COLS,
            (float)row / (float)SHADOW_POINT_ATLAS_ROWS,
            1.0f / (float)SHADOW_POINT_ATLAS_COLS,
            1.0f / (float)SHADOW_POINT_ATLAS_ROWS);
    }
}

// Clamps a per-slot shadow resolution so the whole atlas (slotResolution * grid) stays
// within the GPU's max 2D texture size (sg_query_limits().max_image_size_2d). The atlas
// is square-tiled, so the limiting dimension is the larger grid axis. Returns the usable
// per-slot resolution (possibly reduced, logged once when clamped), or 0 when even a
// 1px slot cannot fit. This keeps shadow allocation succeeding at lower quality instead
// of failing outright on GPUs with small texture caps (e.g. 4096/8192).
unsigned int RenderSystem::clampShadowAtlasSlotResolution(unsigned int requestedResolution, int atlasCols, int atlasRows) const{
    if (requestedResolution < 1 || atlasCols < 1 || atlasRows < 1){
        return 0;
    }

    int maxTextureSize = sg_query_limits().max_image_size_2d;
    if (maxTextureSize < 1){
        maxTextureSize = 4096;
    }

    int gridMax = std::max(atlasCols, atlasRows);
    unsigned int maxSlotResolution = (unsigned int)(maxTextureSize / gridMax);
    if (maxSlotResolution < 1){
        Log::warn("Shadow atlas grid %dx%d does not fit in GPU max texture size %d", atlasCols, atlasRows, maxTextureSize);
        return 0;
    }

    if (requestedResolution > maxSlotResolution){
        Log::warn("Shadow atlas slot resolution clamped from %u to %u (GPU max texture %d, %dx%d grid)",
                  requestedResolution, maxSlotResolution, maxTextureSize, atlasCols, atlasRows);
        return maxSlotResolution;
    }

    return requestedResolution;
}

bool RenderSystem::ensureShadowAtlas(unsigned int slotResolution, int usedSlots){
    usedSlots = std::clamp(usedSlots, 1, MAX_SHADOW_ATLAS_SLOTS);

    // Smallest grid within the 3x3 capacity; equal-area layouts prefer more
    // columns so three cascades pack into a 3x1 row.
    int atlasCols = 1;
    int atlasRows = usedSlots;
    int bestArea = MAX_SHADOW_ATLAS_SLOTS + 1;
    for (int cols = 1; cols <= SHADOW_ATLAS_COLS; cols++){
        int rows = (usedSlots + cols - 1) / cols;
        if (rows > SHADOW_ATLAS_ROWS){
            continue;
        }

        int area = cols * rows;
        if (area < bestArea || (area == bestArea && cols > atlasCols)){
            atlasCols = cols;
            atlasRows = rows;
            bestArea = area;
        }
    }

    slotResolution = clampShadowAtlasSlotResolution(slotResolution, atlasCols, atlasRows);
    if (slotResolution < 1){
        return false;
    }

    int atlasWidth = (int)slotResolution * atlasCols;
    int atlasHeight = (int)slotResolution * atlasRows;

    if (shadowAtlasFramebuffer.isCreated() &&
        shadowAtlasSlotResolution == slotResolution &&
        shadowAtlasCols == atlasCols &&
        shadowAtlasRows == atlasRows &&
        shadowAtlasUsedSlots == usedSlots &&
        !needUpdateShadowAtlas){
        if (!hasShadowAtlas){
            needUpdateShadowBindings = true;
        }
        hasShadowAtlas = true;
        return true;
    }

    shadowAtlasFramebuffer.destroyFramebuffer();
    // LINEAR on a comparison sampler is hardware PCF: each tap is bilinearly filtered
    // over 2x2 comparison results, so every quality level gets softer edges for free.
    if (!shadowAtlasFramebuffer.createDepthOnlyFramebuffer(
            atlasWidth, atlasHeight,
            TextureFilter::LINEAR, TextureFilter::LINEAR,
            TextureWrap::CLAMP_TO_BORDER, TextureWrap::CLAMP_TO_BORDER, true)){
        shadowAtlasSlotResolution = 0;
        shadowAtlasCols = 0;
        shadowAtlasRows = 0;
        shadowAtlasUsedSlots = 0;
        initShadowAtlasRects();
        hasShadowAtlas = false;
        needUpdateShadowBindings = true;
        return false;
    }

    shadowAtlasSlotResolution = slotResolution;
    shadowAtlasCols = atlasCols;
    shadowAtlasRows = atlasRows;
    shadowAtlasUsedSlots = usedSlots;
    initShadowAtlasRects();
    needUpdateShadowAtlas = false;
    hasShadowAtlas = true;
    needUpdateShadowBindings = true;
    return true;
}

bool RenderSystem::ensureShadowPointAtlas(unsigned int slotResolution){
    slotResolution = clampShadowAtlasSlotResolution(slotResolution, SHADOW_POINT_ATLAS_COLS, SHADOW_POINT_ATLAS_ROWS);
    if (slotResolution < 1){
        return false;
    }

    int atlasWidth = (int)slotResolution * SHADOW_POINT_ATLAS_COLS;
    int atlasHeight = (int)slotResolution * SHADOW_POINT_ATLAS_ROWS;

    if (shadowPointAtlasFramebuffer.isCreated() && shadowPointAtlasSlotResolution == slotResolution && !needUpdateShadowPointAtlas){
        if (!hasShadowPointAtlas){
            needUpdateShadowBindings = true;
        }
        hasShadowPointAtlas = true;
        return true;
    }

    shadowPointAtlasFramebuffer.destroyFramebuffer();
    if (!shadowPointAtlasFramebuffer.createFramebuffer(
            TextureType::TEXTURE_2D, atlasWidth, atlasHeight,
            TextureFilter::NEAREST, TextureFilter::NEAREST,
            TextureWrap::CLAMP_TO_BORDER, TextureWrap::CLAMP_TO_BORDER, true)){
        hasShadowPointAtlas = false;
        return false;
    }

    shadowPointAtlasSlotResolution = slotResolution;
    needUpdateShadowPointAtlas = false;
    hasShadowPointAtlas = true;
    needUpdateShadowBindings = true;
    return true;
}

Rect RenderSystem::getShadowAtlasSlotRect(int slotIndex) const{
    if (shadowAtlasCols < 1 || slotIndex < 0 || slotIndex >= shadowAtlasUsedSlots){
        return Rect();
    }

    int col = slotIndex % shadowAtlasCols;
    int row = slotIndex / shadowAtlasCols;
    float x = (float)(col * (int)shadowAtlasSlotResolution);
    float y = (float)(row * (int)shadowAtlasSlotResolution);
    return Rect(x, y, (float)shadowAtlasSlotResolution, (float)shadowAtlasSlotResolution);
}

Rect RenderSystem::getShadowPointAtlasSlotRect(int slotIndex) const{
    int col = slotIndex % SHADOW_POINT_ATLAS_COLS;
    int row = slotIndex / SHADOW_POINT_ATLAS_COLS;
    float x = (float)(col * (int)shadowPointAtlasSlotResolution);
    float y = (float)(row * (int)shadowPointAtlasSlotResolution);
    return Rect(x, y, (float)shadowPointAtlasSlotResolution, (float)shadowPointAtlasSlotResolution);
}

bool RenderSystem::checkPBRFrabebufferUpdate(Material& material){
    return (
        material.baseColorTexture.isFramebufferOutdated() ||
        material.metallicRoughnessTexture.isFramebufferOutdated() ||
        material.normalTexture.isFramebufferOutdated() ||
        material.occlusionTexture.isFramebufferOutdated() ||
        material.emissiveTexture.isFramebufferOutdated() );
}

bool RenderSystem::checkPBRTextures(Material& material, bool receiveLights){
    SubmeshShaderInputs inputs;
    inputs.baseColorTexture = !material.baseColorTexture.empty();
    inputs.metallicRoughnessTexture = !material.metallicRoughnessTexture.empty();
    inputs.normalTexture = !material.normalTexture.empty();
    inputs.occlusionTexture = !material.occlusionTexture.empty();
    inputs.emissiveTexture = !material.emissiveTexture.empty();
    inputs.receiveLights = receiveLights;

    return checkPBRTextures(getMeshShaderEnvironment(), inputs);
}

bool RenderSystem::checkPBRTextures(const MeshShaderEnvironment& environment, const SubmeshShaderInputs& inputs){
    bool hasOtherPBRTextures = false;
    if (environment.lights && inputs.receiveLights) {
        hasOtherPBRTextures = inputs.metallicRoughnessTexture ||
                              inputs.normalTexture ||
                              inputs.occlusionTexture ||
                              inputs.emissiveTexture;
    } else if (environment.lights2D && inputs.receiveLights) {
        // the 2D light path samples only the normal map from the PBR set
        hasOtherPBRTextures = inputs.normalTexture;
    }

    return inputs.baseColorTexture || hasOtherPBRTextures;
}

MeshShaderEnvironment RenderSystem::getMeshShaderEnvironment() const{
    MeshShaderEnvironment environment;
    environment.lights = hasLights;
    environment.lightCapacity = getLightCapacity(scene->getMaxLightsPerObject());
    environment.shadows = hasShadows;
    environment.ibl = hasIBL;
    environment.reflectionProbes = hasReflectionProbes;
    environment.fog = hasFog;
    environment.heightFog = heightFogVariants;
    environment.atmosphereDepth = atmosphereDepthPrograms;
    environment.lights2D = hasLights2D;
    environment.shadows2D = hasShadows2D;
    environment.ssao = scene->isSSAOEnabled();
    environment.ssr = scene->isSSREnabled();
    environment.depthPrepass = scene->isDepthPrepassEnabled();
    environment.sceneDepth = sceneDepthPrograms;
    environment.postColor = postColorVariants;
    return environment;
}

const std::string& RenderSystem::getSubmeshShader(const MeshComponent& mesh, const Submesh& submesh, const std::string& sceneMeshShader){
    if (!submesh.material.shader.empty())
        return submesh.material.shader;
    if (!mesh.customShader.empty())
        return mesh.customShader;
    return sceneMeshShader;
}

std::string RenderSystem::getSubmeshShaderReference(const MeshComponent& mesh, const Submesh& submesh, const std::string& sceneMeshShader){
    return ShaderReference::getColorVariantReference(getSubmeshShader(mesh, submesh, sceneMeshShader), submesh.material.shaderFeatures,
                                                     submesh.material.blendMode);
}

std::string RenderSystem::getSubmeshDepthShaderReference(const MeshComponent& mesh, const Submesh& submesh, const std::string& sceneMeshShader){
    // the library shader's own depth program, else the depth fork, else built-in
    const std::string& shader = getSubmeshShader(mesh, submesh, sceneMeshShader);
    if (ShaderReference::isLibrary(shader)){
        std::shared_ptr<const ShaderManifest> manifest = findManifest(shader);
        if (manifest && manifest->depth)
            return ShaderReference::getVariantReference(shader, submesh.material.shaderFeatures);
    }
    if (mesh.customDepthShader.empty())
        return std::string();
    return ShaderReference::getVariantReference(mesh.customDepthShader, submesh.material.shaderFeatures);
}

std::string RenderSystem::getComponentShaderReference(const std::string& customShader, const std::vector<std::string>& shaderFeatures,
                                                      MaterialBlendMode blendMode){
    return ShaderReference::getColorVariantReference(customShader, shaderFeatures, blendMode);
}

bool RenderSystem::hasExtraPass(const Submesh& submesh, bool terrain){
    // a terrain's patches are per-view instance buffers and painted layers: no extra pass
    return !terrain && !submesh.material.extraPass->shader.empty();
}

std::string RenderSystem::getSubmeshExtraPassReference(const Submesh& submesh){
    const MaterialPass& pass = submesh.material.extraPass;
    return ShaderReference::getColorVariantReference(pass.shader, pass.shaderFeatures, pass.blendMode);
}

SubmeshShaderInputs RenderSystem::getExtraPassShaderInputs(const SubmeshShaderInputs& submeshInputs, const MaterialPass& pass){
    SubmeshShaderInputs inputs = submeshInputs;
    inputs.manifest = findManifest(pass.shader);
    inputs.features = inputs.manifest ? ShaderReference::getEnabledFeatures(pass.shader, pass.shaderFeatures) : std::vector<std::string>();
    // the pass has no lighting field: its shader's default mode
    inputs.lighting = MaterialLighting::AUTO;
    // it is in no shadow, depth or G-buffer pass
    inputs.depthProgram = false;
    inputs.customDepthShader = false;
    inputs.castShadows = false;
    // and an overlay of the submesh: no shadows received, no reflections
    inputs.receiveShadows = false;
    inputs.receiveIBL = false;
    inputs.mirror = false;
    return inputs;
}

RenderState RenderSystem::resolveExtraPassRenderState(const MeshComponent& mesh, const Submesh& submesh, const ShaderManifestState* manifestState){
    // the same resolver as the submesh's colour state, with the pass's values (when that
    // state gains a field, the pass's field of the same name goes here too)
    const MaterialPass& pass = submesh.material.extraPass;
    bool blended = false;
    bool transparentQueue = false;
    return resolveRenderState(pass.blendMode, pass.cullMode, pass.depthWrite, pass.depthTest,
                              pass.depthOffsetFactor, pass.depthOffsetUnits, pass.stencil, pass.colorWrite, manifestState,
                              usesAlphaBlend(submesh.material), submesh.faceCulling, mesh.cullingMode, mesh.windingOrder,
                              blended, transparentQueue);
}

RenderState RenderSystem::resolveSubmeshRenderState(const MeshComponent& mesh, const Submesh& submesh, const ShaderManifest* manifest,
                                                    bool& blended, bool& transparentQueue){
    if (!manifest)
        return resolveSubmeshRenderState(mesh, submesh, (const ShaderManifestState*)nullptr, blended, transparentQueue);
    const ShaderManifestState state = manifest->resolveState(manifest->resolveFeatures(submesh.material.shaderFeatures));
    return resolveSubmeshRenderState(mesh, submesh, &state, blended, transparentQueue);
}

RenderState RenderSystem::resolveSubmeshRenderState(const MeshComponent& mesh, const Submesh& submesh, const ShaderManifestState* manifestState,
                                                    bool& blended, bool& transparentQueue){
    const Material& material = submesh.material;
    return resolveRenderState(material.blendMode, material.cullMode, material.depthWrite, material.depthTest,
                              material.depthOffsetFactor, material.depthOffsetUnits, material.stencil, material.colorWrite, manifestState,
                              usesAlphaBlend(material), submesh.faceCulling, mesh.cullingMode, mesh.windingOrder,
                              blended, transparentQueue);
}

uint32_t RenderSystem::getSubmeshSceneInputs(const MeshComponent& mesh, const Submesh& submesh, const std::string& sceneMeshShader,
                                             bool& opaqueQueue){
    opaqueQueue = false;
    uint32_t inputs = 0;
    const std::string& shader = getSubmeshShader(mesh, submesh, sceneMeshShader);
    std::shared_ptr<const ShaderManifest> manifest = findManifest(shader);
    if (manifest)
        inputs |= manifest->resolveRequire(ShaderReference::getEnabledFeatures(shader, submesh.material.shaderFeatures));
    const MaterialPass& pass = submesh.material.extraPass;
    if (std::shared_ptr<const ShaderManifest> passManifest = findManifest(pass.shader))
        inputs |= passManifest->resolveRequire(ShaderReference::getEnabledFeatures(pass.shader, pass.shaderFeatures));
    inputs &= ShaderRequire_SceneInputs;
    if (inputs){
        // the extra pass draws in the submesh's queue position, which is the mesh's: with
        // autoTransparency a blended submesh makes the whole mesh transparent (but the opaque
        // submeshes of a split mesh, MeshComponent::splitQueues), without it the mesh's own
        // setting decides (setTransparent(false) keeps a blended reader in the opaque queue)
        bool blended = false;
        bool transparentQueue = false;
        resolveSubmeshRenderState(mesh, submesh, manifest.get(), blended, transparentQueue);
        if (mesh.autoTransparency)
            opaqueQueue = !transparentQueue && (!mesh.transparent || mesh.splitQueues);
        else
            opaqueQueue = !mesh.transparent;
    }
    return inputs;
}

RenderState RenderSystem::resolveComponentRenderState(MaterialBlendMode blendMode, MaterialDepthWrite depthWrite, const ShaderManifest* manifest,
                                                      const std::vector<std::string>& shaderFeatures, bool legacyCulling){
    return resolveComponentRenderState(blendMode, depthWrite, MaterialStencil(), -1, manifest, shaderFeatures, legacyCulling);
}

RenderState RenderSystem::resolveComponentRenderState(MaterialBlendMode blendMode, MaterialDepthWrite depthWrite,
                                                      const MaterialStencil& stencil, int colorWrite, const ShaderManifest* manifest,
                                                      const std::vector<std::string>& shaderFeatures, bool legacyCulling){
    bool blended = false;
    bool transparentQueue = false;
    ShaderManifestState manifestState;
    if (manifest)
        manifestState = manifest->resolveState(manifest->resolveFeatures(shaderFeatures));
    RenderState state = resolveRenderState(blendMode, MaterialCullMode::AUTO, depthWrite, MaterialDepthTest::AUTO, 0.0f, 0.0f,
                                           stencil, colorWrite,
                                           manifest ? &manifestState : nullptr, true, legacyCulling, CullingMode::BACK, WindingOrder::CCW,
                                           blended, transparentQueue);
    // AUTO keeps the component's legacy depth write (on), blended or not
    const MaterialDepthWrite write = resolveState(depthWrite, manifestState.depthWrite);
    if (write == MaterialDepthWrite::AUTO)
        state.depthWrite = true;
    return state;
}

void RenderSystem::getShaderVariants(Entity entity, std::vector<ShaderKey>& keys){
    auto addKey = [&keys](ShaderKey key){
        if (std::find(keys.begin(), keys.end(), key) == keys.end())
            keys.push_back(key);
    };
    // a fork that fails to build falls back to the built-in variant
    auto add = [&addKey](ShaderType type, uint32_t properties, uint16_t customId){
        addKey(ShaderPool::getShaderKey(type, properties, customId));
        if (customId != 0)
            addKey(ShaderPool::getShaderKey(type, properties, 0));
    };
    // the component's fork, the scene default, or built-in (as the load functions pick),
    // with the features the component enables
    auto shaderId = [this](const std::string& customShader, ShaderType type, const std::vector<std::string>& features, MaterialBlendMode blendMode){
        return ShaderPool::registerCustomShader(getComponentShaderReference(
            customShader.empty() ? scene->getDefaultCustomShader(type) : customShader, features, blendMode));
    };

    if (MeshComponent* mesh = scene->findComponent<MeshComponent>(entity)){
        InstancedMeshComponent* instmesh = scene->findComponent<InstancedMeshComponent>(entity);
        TerrainComponent* terrain = scene->findComponent<TerrainComponent>(entity);
        const bool mirror = scene->findComponent<MirrorComponent>(entity) != nullptr;

        // the buffers loadMesh draws from
        std::vector<const Buffer*> buffers;
        if (mesh->buffer.getSize() > 0)
            buffers.push_back(&mesh->buffer);
        if (mesh->indices.getSize() > 0)
            buffers.push_back(&mesh->indices);
        for (int i = 0; i < mesh->numExternalBuffers; i++)
            buffers.push_back(&mesh->eBuffers[i]);

        if (!buffers.empty()){
            const MeshShaderEnvironment environment = getMeshShaderEnvironment();
            const std::string& sceneMeshShader = scene->getDefaultCustomShader(ShaderType::MESH);
            for (unsigned int s = 0; s < mesh->numSubmeshes; s++){
                // a copy: scanning sets the submesh's attribute flags
                Submesh submesh = mesh->submeshes[s];
                // a terrain's submeshes draw with submesh 0's material (loadMesh)
                if (terrain)
                    submesh.material = mesh->submeshes[0].material;
                // MeshSystem::update turns the texture rect path on before the mesh loads
                if (MeshSystem::usesTextureRect(submesh))
                    submesh.hasTextureRect = true;
                scanSubmeshAttributes(submesh, buffers);

                const SubmeshShaderInputs inputs = getSubmeshShaderInputs(*mesh, submesh, terrain, instmesh, mirror, sceneMeshShader);
                const SubmeshShaderVariant variant = getSubmeshShaderVariant(environment, inputs);
                // the shader of each submesh (its material's, the mesh's, the scene's), see loadMesh
                const uint16_t meshShaderId = ShaderPool::registerCustomShader(getSubmeshShaderReference(*mesh, submesh, sceneMeshShader));
                const uint16_t depthShaderId = ShaderPool::registerCustomShader(getSubmeshDepthShaderReference(*mesh, submesh, sceneMeshShader));
                add(ShaderType::MESH, variant.properties, meshShaderId);
                if (variant.depth)
                    add(ShaderType::DEPTH, variant.depthProperties, depthShaderId);
                if (variant.gbuffer)
                    add(ShaderType::GBUFFER, variant.gbufferProperties, 0);
                // its extra pass (no built-in fallback: a pass that fails is not drawn)
                if (hasExtraPass(submesh, terrain != nullptr)){
                    const SubmeshShaderVariant pass = getSubmeshShaderVariant(environment, getExtraPassShaderInputs(inputs, submesh.material.extraPass));
                    addKey(ShaderPool::getShaderKey(ShaderType::MESH, pass.properties,
                                                    ShaderPool::registerCustomShader(getSubmeshExtraPassReference(submesh))));
                }
            }
        }
    }

    // as loadUI, loadPoints, loadLines and loadSky pick them
    if (UIComponent* ui = scene->findComponent<UIComponent>(entity)){
        const bool isText = scene->findComponent<TextComponent>(entity) != nullptr;
        bool hasTexture = false;
        bool hasFontAtlasTexture = false;
        bool vertexColorVec4 = true;
        if (!ui->texture.empty()){
            if (isText){
                vertexColorVec4 = false;
                hasFontAtlasTexture = true;
            }else{
                hasTexture = true;
            }
        }
        add(ShaderType::UI, ShaderPool::getUIProperties(hasTexture, hasFontAtlasTexture, false, vertexColorVec4),
            shaderId(ui->customShader, ShaderType::UI, ui->shaderFeatures, ui->blendMode));
    }
    if (PointsComponent* points = scene->findComponent<PointsComponent>(entity)){
        const bool hasTexture = !points->texture.empty();
        add(ShaderType::POINTS, ShaderPool::getPointsProperties(hasTexture, false, true, hasTexture && points->hasTextureRect) | postColorProperty(ShaderType::POINTS),
            shaderId(points->customShader, ShaderType::POINTS, points->shaderFeatures, points->blendMode));
    }
    if (LineRendererComponent* lines = scene->findComponent<LineRendererComponent>(entity)){
        add(ShaderType::LINES, ShaderPool::getLinesProperties(false, true) | postColorProperty(ShaderType::LINES),
            shaderId(lines->customShader, ShaderType::LINES, lines->shaderFeatures, lines->blendMode));
    }
    if (SkyboxComponent* sky = scene->findComponent<SkyboxComponent>(entity)){
        add(ShaderType::SKYBOX, postColorProperty(ShaderType::SKYBOX), shaderId(sky->customShader, ShaderType::SKYBOX, sky->shaderFeatures, MaterialBlendMode::AUTO));
    }
    // an HTML UI document: geometry, gradients and layer/filter passes (RmlRenderer)
    if (scene->findComponent<UIDocumentComponent>(entity)){
        for (const auto& variant : ShaderPool::getHtmlUIVariants())
            add(variant.first, variant.second, 0);
    }
}

void RenderSystem::logRenderObjects(){
    size_t meshes = 0, submeshes = 0, extraPasses = 0, extraPassObjects = 0, smoothNormalBuffers = 0;
    auto meshArray = scene->getComponentArray<MeshComponent>();
    for (int i = 0; i < meshArray->size(); i++){
        const MeshComponent& mesh = meshArray->getComponentFromIndex(i);
        if (!mesh.loaded)
            continue;
        meshes++;
        submeshes += mesh.numSubmeshes;
        for (unsigned int s = 0; s < mesh.numSubmeshes; s++){
            if (mesh.submeshes[s].extraPass){
                extraPassObjects++;
                if (mesh.submeshes[s].extraPass->loaded)
                    extraPasses++;
            }
        }
        if (mesh.smoothNormals)
            smoothNormalBuffers += mesh.smoothNormals->entries.size();
    }
    Log::print("Render objects: %zu meshes, %zu submeshes, %zu extra passes drawn (%zu allocated), %zu smoothed-normal buffers, %d object pipelines",
               meshes, submeshes, extraPasses, extraPassObjects, smoothNormalBuffers, ObjectRender::getLivePipelineCount());
    // shader library phase 3: what reads the scene and what the last frame made for it
    size_t sceneInputTargets = primaryView.sceneInputTargets.size() + (primaryView.sceneColorRedirect.isCreated() ? 1 : 0);
    Log::print("Scene inputs: %zu objects read the scene (depth %s, colour %s); last frame: %u camera passes with a reading draw, "
               "%u depth pre-passes, %u shared depths, %u colour copies; %zu targets alive, %u made; scene camera offscreen for the copy: %s",
               sceneInputMeshes.size() + sceneInputPoints.size(), (sceneInputUsage & ShaderRequire_SceneDepth) ? "yes" : "no",
               (sceneInputUsage & ShaderRequire_SceneColor) ? "yes" : "no", lastSceneInputStats.passes, lastSceneInputStats.depthPrePasses,
               lastSceneInputStats.sharedDepth, lastSceneInputStats.colorCopies, sceneInputTargets, sceneInputStats.targetsCreated,
               sceneColorRedirect ? "yes" : "no");
    // shader library phase 4: what the post-processing runs (nothing printed while it is off)
    if (postTiers.tailColor || postTiers.uber){
        std::string features;
        for (int bit = 0; bit < UBER_FEATURE_COUNT; bit++){
            if (postTiers.uberFeatures & (1u << bit))
                features += (features.empty() ? "" : ",") + ShaderPool::getShaderPropertyName(ShaderType::BLIT, bit, true);
        }
        Log::print("Post-processing: in-shader colour %s (%s, LUT %d), uber pass %s (features %s, render scale %.2f, MSAA %d)",
                   postTiers.tailColor ? "on" : "off", postTiers.tailGrade ? "curve, grade, vignette, dither" : "exposure and curve",
                   postTailLutSize, postTiers.uber ? "on" : "off", features.empty() ? "none" : features.c_str(),
                   postTiers.renderScale, postTiers.msaaSamples);
    }
}

uint32_t RenderSystem::getSceneInputUsage() const{
    return ((lastSceneInputStats.depthPrePasses + lastSceneInputStats.sharedDepth) ? 1u : 0u) | (lastSceneInputStats.colorCopies ? 2u : 0u);
}

void RenderSystem::scanSubmeshAttributes(Submesh& submesh, const std::vector<const Buffer*>& buffers){
    // Reset before rescanning: model reloads can replace and reorder submeshes, so
    // retaining a flag from the previous slot may select a shader attribute that the
    // rebuilt submesh does not provide.
    submesh.hasTexCoord1 = false;
    submesh.hasTexCoord2 = false;
    submesh.hasNormal = false;
    submesh.hasNormalMap = false;
    submesh.hasTangent = false;
    submesh.hasVertexColor3 = false;
    submesh.hasVertexColor4 = false;
    submesh.hasSkinning = false;
    submesh.hasMorphTarget = false;
    submesh.hasMorphNormal = false;
    submesh.hasMorphTangent = false;

    auto scanAttribute = [&submesh](AttributeType type, const Attribute& attribute){
        if (type == AttributeType::TEXCOORD1){
            submesh.hasTexCoord1 = true;
        }
        if (type == AttributeType::TEXCOORD2){
            submesh.hasTexCoord2 = true;
        }
        if (type == AttributeType::NORMAL){
            submesh.hasNormal = true;
        }
        if (type == AttributeType::TANGENT){
            submesh.hasTangent = true;
        }
        if (type == AttributeType::COLOR){
            if (attribute.getElements() == 3){
                submesh.hasVertexColor3 = true;
            }else{
                submesh.hasVertexColor4 = true;
            }
        }
        if (type == AttributeType::BONEIDS || type == AttributeType::BONEWEIGHTS){
            submesh.hasSkinning = true;
        }
        if (type == AttributeType::MORPHTARGET0){
            submesh.hasMorphTarget = true;
        }
        if (type == AttributeType::MORPHNORMAL0){
            submesh.hasMorphNormal = true;
        }
        if (type == AttributeType::MORPHTANGENT0){
            submesh.hasMorphTangent = true;
        }
    };

    for (const Buffer* buffer : buffers){
        if (buffer->isRenderAttributes()) {
            for (auto const &attr : buffer->getAttributes()) {
                scanAttribute(attr.first, attr.second);
            }
        }
    }
    for (auto const& attr : submesh.attributes){
        scanAttribute(attr.first, attr.second);
    }

    if (!submesh.material.normalTexture.empty()){
        submesh.hasNormalMap = true;
    }
}

SubmeshShaderInputs RenderSystem::getSubmeshShaderInputs(const MeshComponent& mesh, const Submesh& submesh, const TerrainComponent* terrain, const InstancedMeshComponent* instmesh, bool mirror,
                                                         const std::string& sceneMeshShader){
    SubmeshShaderInputs inputs;

    inputs.texCoord1 = submesh.hasTexCoord1;
    inputs.texCoord2 = submesh.hasTexCoord2;
    inputs.normal = submesh.hasNormal;
    inputs.tangent = submesh.hasTangent;
    inputs.vertexColor3 = submesh.hasVertexColor3;
    inputs.vertexColor4 = submesh.hasVertexColor4;
    inputs.textureRect = submesh.hasTextureRect;
    inputs.skinning = submesh.hasSkinning;
    inputs.morphTarget = submesh.hasMorphTarget;
    inputs.morphNormal = submesh.hasMorphNormal;
    inputs.morphTangent = submesh.hasMorphTangent;

    const Material& material = submesh.material;
    inputs.baseColorTexture = !material.baseColorTexture.empty();
    inputs.metallicRoughnessTexture = !material.metallicRoughnessTexture.empty();
    inputs.normalTexture = !material.normalTexture.empty();
    inputs.occlusionTexture = !material.occlusionTexture.empty();
    inputs.emissiveTexture = !material.emissiveTexture.empty();
    inputs.alphaMode = material.alphaMode;
    inputs.lighting = material.lighting;
    inputs.textureShadow = submesh.textureShadow;

    inputs.receiveLights = mesh.receiveLights;
    inputs.receiveIBL = mesh.receiveIBL;
    inputs.receiveShadows = mesh.receiveShadows;
    inputs.castShadows = mesh.castShadows;
    inputs.customDepthShader = !mesh.customDepthShader.empty();

    // shader library: the manifest of the shader the submesh draws with decides its inputs
    const std::string& shader = getSubmeshShader(mesh, submesh, sceneMeshShader);
    inputs.manifest = findManifest(shader);
    inputs.depthProgram = ShaderReference::isLibrary(shader) && inputs.manifest && inputs.manifest->depth;
    if (inputs.manifest){
        inputs.features = ShaderReference::getEnabledFeatures(shader, material.shaderFeatures);
        // @state shadows=off (with its keyword rules) keeps the submesh out of the shadow maps
        if (inputs.manifest->resolveState(inputs.features).shadows == ShaderShadowCasting::OFF)
            inputs.castShadows = false;
    }
    if (inputs.depthProgram){
        // the library depth program takes the depth fork's place
        inputs.customDepthShader = false;
    }

    inputs.terrain = terrain != nullptr;
    inputs.terrainBlendMaps = terrain && !terrain->blendMaps.empty();
    // Painted layers that bring their own surface need the wider terrain path; a
    // terrain of plain color layers keeps the cheaper one it always had.
    inputs.terrainSurface = terrain && hasTerrainSurfaceLayers(*terrain);
    inputs.instanced = instmesh != nullptr;
    // Opted in per field, not per band, so emptying the band (editor preview) neither
    // rebuilds the shader nor drops the variant from an export.
    inputs.instanceFade = instmesh && instmesh->distanceFade;
    inputs.mirror = mirror;

    return inputs;
}

namespace {
    MaterialLighting toMaterialLighting(ShaderLightingMode mode){
        switch (mode){
            case ShaderLightingMode::PIXEL:  return MaterialLighting::PIXEL;
            case ShaderLightingMode::VERTEX: return MaterialLighting::VERTEX;
            case ShaderLightingMode::MIXED:  return MaterialLighting::MIXED;
            default:                         return MaterialLighting::NONE;
        }
    }
}

void RenderSystem::warnUnsupportedLighting(const Material& material, const std::string& shader, const ShaderManifest* manifest,
                                           const std::string& entityName, Entity entity, unsigned int submesh){
    static const char* const names[] = {"auto", "pixel", "vertex", "mixed", "none"};
    const MaterialLighting resolved = resolveLighting(material.lighting, manifest);
    // once per material and shader pair (a material is named by its file, else its GameObject,
    // else, created at runtime without a name, by its entity and submesh); meshes may load on
    // the scene loading thread
    static std::mutex reportedMutex;
    static std::set<std::string> reported;
    std::string materialName = material.name;
    if (materialName.empty()){
        materialName = entityName.empty() ? "entity " + std::to_string(entity) : entityName;
        materialName += " submesh " + std::to_string(submesh);
    }
    const std::string shaderName = shader.empty() ? std::string("Lit (built-in)") : shader;
    {
        std::lock_guard<std::mutex> lock(reportedMutex);
        if (!reported.insert(materialName + "\n" + shaderName + "\n" + names[(int)material.lighting]).second)
            return;
    }
    Log::warn("Material '%s': shader '%s' has no '%s' lighting mode; drawing with its default, '%s'",
              materialName.c_str(), shaderName.c_str(), names[(int)material.lighting], names[(int)resolved]);
}

bool RenderSystem::supportsLighting(MaterialLighting mode, const ShaderManifest* manifest){
    if (mode == MaterialLighting::AUTO)
        return true;
    if (!manifest){
        // the built-in Lit, and forks of it without a manifest
        return mode != MaterialLighting::NONE;
    }
    if (manifest->lighting.empty())
        return mode == MaterialLighting::NONE;
    for (ShaderLightingMode listed : manifest->lighting){
        if (toMaterialLighting(listed) == mode)
            return true;
    }
    return false;
}

MaterialLighting RenderSystem::resolveLighting(MaterialLighting requested, const ShaderManifest* manifest){
    if (requested != MaterialLighting::AUTO && supportsLighting(requested, manifest))
        return requested;
    return manifest ? toMaterialLighting(manifest->getDefaultLighting()) : MaterialLighting::PIXEL;
}

SubmeshShaderVariant RenderSystem::getSubmeshShaderVariant(const MeshShaderEnvironment& environment, const SubmeshShaderInputs& inputs){
    SubmeshShaderVariant variant;

    const bool hasPBRTextures = checkPBRTextures(environment, inputs);

    bool p_unlit = false;
    bool p_punctual = false;
    bool p_vertexLights = false;
    bool p_ibl = false;
    bool p_hasTexture1 = false;
    bool p_hasTexture2 = false;
    bool p_hasNormalMap = false;
    bool p_hasNormal = false;
    bool p_hasTangent = false;
    bool p_receiveShadows = false;

    if (inputs.texCoord1 && hasPBRTextures){
        p_hasTexture1 = true;
    }
    // second UV set: only when both UV attributes and PBR textures are present
    // (never for terrain, which generates its UVs in the shader)
    if (inputs.texCoord1 && inputs.texCoord2 && hasPBRTextures){
        p_hasTexture2 = true;
    }
    if (inputs.terrain && (inputs.terrainBlendMaps || hasPBRTextures)){
        p_hasTexture1 = true;
    }
    // painted layer surfaces are Lit's (a shader with a manifest draws a terrain with its
    // own surface, its uv1 the terrain's tiled UV)
    const bool p_terrainSurface = inputs.terrain && inputs.terrainSurface && !inputs.manifest;
    const bool useIBL = (environment.ibl || environment.reflectionProbes) && inputs.receiveIBL;
    // the stages evaluating the object's lights: pixel (USE_PUNCTUAL), vertex
    // (USE_VERTEX_LIGHTS) or both, which the lights' own shading then splits (MIXED)
    const ShaderManifest* manifest = inputs.manifest.get();
    const MaterialLighting lighting = resolveLighting(inputs.lighting, manifest);
    if (manifest){
        // a shader with a manifest (a library shader, a fork declaring one): its lighting
        // mode alone decides. A lit mode evaluates the object's lights (includes/lights.glsl)
        // and, without scene lights, draws unlit as Lit does; IBL, SSAO and the material's
        // PBR maps are Lit's, never a manifest shader's (normals: see @require below).
        if (lighting != MaterialLighting::NONE && environment.lights && inputs.receiveLights){
            p_punctual = lighting != MaterialLighting::VERTEX;
            p_vertexLights = lighting == MaterialLighting::VERTEX || lighting == MaterialLighting::MIXED;
            p_receiveShadows = environment.shadows && inputs.receiveShadows && p_punctual;
        }else{
            p_unlit = true;
        }
    }else if ((environment.lights || useIBL) && inputs.receiveLights){
        p_punctual = environment.lights && lighting != MaterialLighting::VERTEX;
        p_vertexLights = environment.lights && (lighting == MaterialLighting::VERTEX || lighting == MaterialLighting::MIXED);
        p_ibl = useIBL;

        p_hasNormal = true;
        if (inputs.tangent){
            p_hasTangent = true;
        }
        if (inputs.normalTexture){
            p_hasNormalMap = true;
        }
        // only pixel lights take shadows
        if (environment.shadows && inputs.receiveShadows && p_punctual){
            p_receiveShadows = true;
        }
    }else{
        p_unlit = true;
    }
    // light counts are uniforms: only the block capacity is part of the variant
    const int lightCapacity = (p_punctual || p_vertexLights) ? environment.lightCapacity : 8;

    // dedicated 2D light path: composes with MATERIAL_UNLIT (sprites keep the
    // cheap unlit shader; accumulated 2D light multiplies the base color) and
    // with the lit path in mixed scenes (contributions add)
    const bool useLight2D = environment.lights2D && inputs.receiveLights;
    const bool p_light2d = useLight2D;
    const bool p_shadows2d = useLight2D && environment.shadows2D && inputs.receiveShadows;
    if (useLight2D){
        p_hasNormal = true;
        if (inputs.tangent){
            p_hasTangent = true;
        }
        if (inputs.normalTexture){
            p_hasNormalMap = true;
        }
    }

    // Shader library (section 7.3): a manifest's @require forces the inputs it names when
    // the mesh has them, whatever the lights and textures, and only those (the varyings
    // surface_vs.glsl writes follow @require). A lit mode adds the normals the lights need
    // (2D lights do not: its varyings follow @require); a mesh without them gets the
    // model's +Y in surface_vs.glsl.
    bool p_vertexColorVec3 = inputs.vertexColor3;
    bool p_vertexColorVec4 = inputs.vertexColor4;
    const uint32_t manifestRequire = manifest ? manifest->resolveRequire(inputs.features) : 0;
    if (manifest){
        const uint32_t require = manifestRequire;
        if (!inputs.terrain){
            p_hasTexture1 = (require & ShaderRequire_UV1) && inputs.texCoord1;
            p_hasTexture2 = (require & ShaderRequire_UV2) && inputs.texCoord2;
        }else{
            // a terrain's UV set is the one its vertex stage makes (includes/terrain_vs.glsl)
            p_hasTexture1 = (require & ShaderRequire_UV1) != 0;
            p_hasTexture2 = false;
        }
        const bool requiresColor = (require & (ShaderRequire_Color | ShaderRequire_RawColor)) != 0;
        p_vertexColorVec3 = requiresColor && inputs.vertexColor3;
        p_vertexColorVec4 = requiresColor && inputs.vertexColor4;

        const bool requiresNormals = (require & (ShaderRequire_Normals | ShaderRequire_Tangents)) != 0;
        p_hasNormal = (requiresNormals || p_punctual || p_vertexLights) && inputs.normal;
        p_hasTangent = p_hasNormal && (require & ShaderRequire_Tangents) && inputs.tangent;
        p_hasNormalMap = false;
    }

    // screen-space AO modulates ambient/indirect light on lit submeshes (Lit's).
    const bool p_ssao = environment.ssao && !p_unlit && !manifest && (p_punctual || p_vertexLights || p_ibl);
    const bool p_alphaMask = inputs.alphaMode == MaterialAlphaMode::MASK;
    const bool p_alphaOpaque = inputs.alphaMode == MaterialAlphaMode::ALPHA_OPAQUE;
    // AUTO + textureShadow preserves the legacy opt-in for editor-created
    // materials; explicit glTF modes derive the depth behavior from alphaMode. A shader
    // with a manifest never alpha clips the depth pass by the material: its colour pass
    // does not discard by it (a shader that clips has its own @depth program and hook)
    Material alphaMaterial;
    alphaMaterial.alphaMode = inputs.alphaMode;
    const bool p_depthAlphaMask = !manifest && usesAlphaMask(alphaMaterial, inputs.textureShadow);
    const bool p_depthTexture = p_depthAlphaMask && inputs.texCoord1 && inputs.baseColorTexture;
    const bool p_instanceFade = inputs.instanced && inputs.instanceFade;

    variant.properties = ShaderPool::getMeshProperties(
                    p_unlit, p_hasTexture1, p_hasTexture2, p_punctual,
                    p_receiveShadows, p_hasNormal, p_hasNormalMap,
                    p_hasTangent, p_vertexColorVec3, p_vertexColorVec4, inputs.textureRect,
                    environment.fog, inputs.skinning, inputs.morphTarget, inputs.morphNormal, inputs.morphTangent,
                    inputs.terrain, inputs.instanced, p_ibl, inputs.mirror, p_ssao, p_light2d, p_shadows2d,
                    p_alphaMask, p_alphaOpaque, p_instanceFade, p_terrainSurface,
                    p_vertexLights, lightCapacity);
    // the post-processing colour (phase 4): the scene's, for every colour variant
    if (environment.postColor)
        variant.properties |= ShaderPool::getPostColorProperty(ShaderType::MESH);
    // the height fog: a variant of the fog's
    if (environment.fog && environment.heightFog)
        variant.properties |= ShaderPool::getHeightFogProperty();

    // the depth shader feeds shadow maps (casters) and the SSAO depth pre-pass
    // (SSR uses its own G-buffer shader). A depth fork is built even without a
    // pass, so its uniforms and build errors show up in the editor.
    variant.depthPass = (environment.shadows && inputs.castShadows) || environment.ssao || environment.depthPrepass || environment.sceneDepth
                        || environment.atmosphereDepth;
    variant.depth = variant.depthPass || inputs.customDepthShader;
    if (variant.depth){
        // a library depth program gets the inputs its manifest requires, as the color pass
        bool d_normals = false, d_vertexColorVec3 = false, d_vertexColorVec4 = false, d_uv1 = false;
        if (inputs.depthProgram && inputs.manifest){
            const uint32_t require = manifestRequire;
            d_normals = (require & (ShaderRequire_Normals | ShaderRequire_Tangents)) && (inputs.normal || inputs.terrain);
            const bool d_color = (require & (ShaderRequire_Color | ShaderRequire_RawColor)) != 0;
            d_vertexColorVec3 = d_color && inputs.vertexColor3 && !inputs.terrain;
            d_vertexColorVec4 = d_color && inputs.vertexColor4 && !inputs.terrain;
            d_uv1 = (require & ShaderRequire_UV1) && (inputs.texCoord1 || inputs.terrain);
        }
        // the sprite rect the alpha-clip UV goes through (an atlas frame's shadow)
        const bool d_textureRect = inputs.textureRect && !inputs.terrain && (p_depthTexture || d_uv1);
        variant.depthProperties = ShaderPool::getDepthMeshProperties(
            p_depthTexture, inputs.skinning, inputs.morphTarget,
            inputs.morphNormal, inputs.morphTangent, inputs.terrain, inputs.instanced,
            p_depthAlphaMask, p_instanceFade, d_normals, d_vertexColorVec3, d_vertexColorVec4, d_uv1, d_textureRect);
    }

    // the G-buffer shader feeds the SSR geometry pass (MRT: packed depth + view-space
    // normal/roughness/metallic). Built only when SSR is enabled (it needs normals).
    variant.gbuffer = environment.ssr;
    if (variant.gbuffer){
        variant.gbufferProperties = ShaderPool::getGBufferMeshProperties(
            inputs.baseColorTexture, inputs.normal, inputs.skinning,
            inputs.morphTarget, inputs.morphNormal, inputs.morphTangent,
            inputs.terrain, inputs.instanced, inputs.metallicRoughnessTexture, p_terrainSurface && inputs.normal);
    }

    return variant;
}

TextureRender* RenderSystem::getDefaultShaderTexture(int fallback, bool cube){
    switch (static_cast<ShaderTextureDefault>(fallback)){
        case ShaderTextureDefault::BLACK:  return cube ? &emptyCubeBlack : &emptyBlack;
        case ShaderTextureDefault::GRAY:   return cube ? &emptyCubeGray : &emptyGray;
        case ShaderTextureDefault::NORMAL: return cube ? &emptyCubeGray : &emptyFlatNormal;
        default:                           return cube ? &emptyCubeWhite : &emptyWhite;
    }
}

bool RenderSystem::loadShaderLibraryBindings(ShaderLibraryBindings& bindings, ShaderData& shaderData, ObjectRender& render,
                                             std::vector<std::pair<std::string, Texture>>& textures, const ShaderManifest* manifest){
    bindings.clear();

    // the frame block, by name; both stages declare the same layout but for the post colour's
    // members the fragment one may append (USE_POST_COLOR)
    bindings.slotVSFrame = shaderData.getUniformBlockIndexByName("u_vs_frame");
    bindings.slotFSFrame = shaderData.getUniformBlockIndexByName("u_fs_frame");
    if (bindings.slotVSFrame != -1){
        unsigned int sizeBytes = 0;
        if (!shaderData.getUniformBlockMembers("u_vs_frame", sizeBytes))
            sizeBytes = 0;
        bindings.frameSize = std::min((unsigned int)sizeof(frame_t), sizeBytes);
    }
    if (bindings.slotFSFrame != -1){
        unsigned int sizeBytes = 0;
        if (!shaderData.getUniformBlockMembers("u_fs_frame", sizeBytes))
            sizeBytes = 0;
        bindings.frameSizeFS = std::min((unsigned int)sizeof(frame_t), sizeBytes);
    }

    // the post-processing colour (includes/post_color.glsl): its block when no other holds it,
    // and its LUT, bound per draw (applyPostColor)
    bindings.slotFSPostColor = shaderData.getUniformBlockIndexByName("u_fs_postColor");
    bindings.slotPostColorLut = shaderData.getTextureIndexByName("u_postColorLut");
    if (bindings.slotPostColorLut.first != -1)
        render.addTexture(bindings.slotPostColorLut, ShaderStageType::FRAGMENT, &emptyWhite);

    // the scene inputs (includes/scene.glsl), bound to the neutral fallbacks until a draw
    // binds its pass's (applySceneInputs)
    bindings.slotFSSceneInputs = shaderData.getUniformBlockIndexByName("u_fs_sceneInputs");
    if (bindings.slotFSSceneInputs != -1){
        unsigned int sizeBytes = 0;
        if (!shaderData.getUniformBlockMembers("u_fs_sceneInputs", sizeBytes))
            sizeBytes = 0;
        bindings.sceneInputsSize = std::min((unsigned int)sizeof(scene_inputs_t), sizeBytes);
    }
    bindings.slotSceneDepth = shaderData.getTextureIndexByName("u_sceneDepthTexture");
    if (bindings.slotSceneDepth.first != -1)
        render.addTexture(bindings.slotSceneDepth, ShaderStageType::FRAGMENT, &emptyWhite);
    bindings.slotSceneColor = shaderData.getTextureIndexByName("u_sceneColorTexture");
    if (bindings.slotSceneColor.first != -1)
        render.addTexture(bindings.slotSceneColor, ShaderStageType::FRAGMENT, &emptyBlack);
    // the reflection environment (includes/environment.glsl), black until a draw binds its own
    bindings.slotFSEnvironment = shaderData.getUniformBlockIndexByName("u_fs_environment");
    bindings.slotEnvironment = shaderData.getTextureIndexByName("u_environmentTexture");
    if (bindings.slotEnvironment.first != -1)
        render.addTexture(bindings.slotEnvironment, ShaderStageType::FRAGMENT, &emptyCubeBlack);
    // the frame's blurred scene (includes/blurred_scene.glsl), black until a draw binds it
    bindings.slotFSBlurredScene = shaderData.getUniformBlockIndexByName("u_fs_blurredScene");
    bindings.slotBlurredScene = shaderData.getTextureIndexByName("u_blurredSceneTexture");
    if (bindings.slotBlurredScene.first != -1)
        render.addTexture(bindings.slotBlurredScene, ShaderStageType::FRAGMENT, &emptyBlack);
    // the height fog's block of the pass (a fogged variant of a height fog scene)
    bindings.slotFSHeightFog = shaderData.getUniformBlockIndexByName("u_fs_heightFog");

    // the textures the manifest declares, else (a shader without one) the ones set by name
    auto addTexture = [&](const std::string& uniform, int fallback){
        const std::pair<int, int> slot = shaderData.getTextureIndexByName(uniform);
        if (slot.first == -1)
            return;
        ShaderTextureSlot textureSlot;
        textureSlot.slot = slot;
        textureSlot.fallback = fallback;
        for (const ShaderStage& stage : shaderData.stages){
            for (const ShaderTexture& texture : stage.textures){
                if (texture.name == uniform)
                    textureSlot.cube = texture.type == TextureType::TEXTURE_CUBE;
            }
        }
        for (size_t t = 0; t < textures.size(); t++){
            if (textures[t].first == uniform){
                textureSlot.source = (int)t;
                break;
            }
        }
        bindings.textures.push_back(textureSlot);
    };
    if (manifest){
        for (const ShaderManifestTexture& texture : manifest->textures)
            addTexture(texture.uniform, static_cast<int>(texture.defaultTexture));
    }else{
        for (const auto& texture : textures)
            addTexture(texture.first, static_cast<int>(ShaderTextureDefault::WHITE));
    }

    return bindShaderTextures(bindings, render, textures);
}

bool RenderSystem::bindShaderTextures(const ShaderLibraryBindings& bindings, ObjectRender& render, std::vector<std::pair<std::string, Texture>>& textures){
    for (const ShaderTextureSlot& textureSlot : bindings.textures){
        TextureRender* fallback = getDefaultShaderTexture(textureSlot.fallback, textureSlot.cube);
        TextureRender* textureRender = fallback;
        if (textureSlot.source >= 0 && textureSlot.source < (int)textures.size()){
            Texture& texture = textures[textureSlot.source].second;
            if (!texture.empty()){
                textureRender = texture.getRender(fallback);
                if (!textureRender){
                    textureRender = fallback;
                }else if (!textureRender->isCreated()){
                    return false; // still loading
                }
            }
        }
        render.addTexture(textureSlot.slot, ShaderStageType::FRAGMENT, textureRender);
    }
    return true;
}

bool RenderSystem::shaderTexturesOutdated(const ShaderLibraryBindings& bindings, const std::vector<std::pair<std::string, Texture>>& textures){
    for (const ShaderTextureSlot& textureSlot : bindings.textures){
        if (textureSlot.source >= 0 && textureSlot.source < (int)textures.size() &&
                textures[textureSlot.source].second.isFramebufferOutdated())
            return true;
    }
    return false;
}

void RenderSystem::fillFrameBlock(frame_t& frame, const CameraComponent& camera, const Transform& cameraTransform, const Vector2& resolution){
    // the matrices the pass renders with: offscreen GL targets flip Y (see updateMVP)
    Matrix4 projection = camera.projectionMatrix;
    Matrix4 viewProjection = camera.viewProjectionMatrix;
    if (isRenderingFlipped(camera)){
        static const Matrix4 flipYMatrix = Matrix4::scaleMatrix(Vector3(1, -1, 1));
        projection = flipYMatrix * projection;
        viewProjection = flipYMatrix * viewProjection;
    }
    frame.viewMatrix = camera.viewMatrix;
    frame.projectionMatrix = projection;
    frame.viewProjectionMatrix = viewProjection;

    const bool orthographic = camera.type != CameraType::CAMERA_PERSPECTIVE;
    frame.cameraPosition = Vector4(cameraTransform.worldPosition, orthographic ? 1.0f : 0.0f);
    // worldDirection is the view's backward axis
    frame.cameraDirection = Vector4(-camera.worldDirection.x, -camera.worldDirection.y, -camera.worldDirection.z, 0.0f);

    frame.time = Vector4(frameTime, std::sin(frameTime), std::cos(frameTime), Engine::getDeltatime());
    frame.resolution = Vector4(resolution.x, resolution.y,
                               resolution.x > 0.0f ? 1.0f / resolution.x : 0.0f,
                               resolution.y > 0.0f ? 1.0f / resolution.y : 0.0f);

    // the brightest active directional light reaching the camera's layers
    frame.mainLightDirection = Vector4(0.0f, 0.0f, 0.0f, 0.0f);
    frame.mainLightColor = Vector4(0.0f, 0.0f, 0.0f, 0.0f);
    float brightest = 0.0f;
    auto lights = scene->getComponentArray<LightComponent>();
    for (size_t i = 0; i < lights->size(); i++){
        const LightComponent& light = lights->getComponentFromIndex(i);
        if (light.type != LightType::DIRECTIONAL || light.intensity <= 0.0f || !isLightActive(lights->getEntity(i), light))
            continue;
        const float luminance = 0.2126f * light.color.x + 0.7152f * light.color.y + 0.0722f * light.color.z;
        const float brightness = light.intensity * luminance;
        if (brightness <= brightest)
            continue;
        brightest = brightness;
        Vector3 towardsLight = Vector3(-light.worldDirection.x, -light.worldDirection.y, -light.worldDirection.z);
        if (towardsLight.length() > 0.0f)
            towardsLight.normalize();
        frame.mainLightDirection = Vector4(towardsLight, light.intensity);
        frame.mainLightColor = Vector4(light.color, 1.0f);
    }

    frame.ambient = Vector4(scene->getGlobalIlluminationColorLinear() * scene->getGlobalIlluminationIntensity(), 1.0f);
}

Vector2 RenderSystem::passFrameResolution(const CameraComponent& camera) const{
    if (camera.renderToTexture && camera.framebuffer)
        return Vector2((float)camera.framebuffer->getWidth(), (float)camera.framebuffer->getHeight());
    if (isFixedResolutionActive())
        return Vector2((float)scene->getFixedResolutionWidth(), (float)scene->getFixedResolutionHeight());
    return Vector2(targetViewRect().getWidth(), targetViewRect().getHeight());
}

void RenderSystem::updatePassFrame(const CameraComponent& camera, const Transform& cameraTransform){
    updatePassFrame(camera, cameraTransform, passFrameResolution(camera));
}

void RenderSystem::updatePassFrame(const CameraComponent& camera, const Transform& cameraTransform, const Vector2& resolution){
    fillFrameBlock(passFrame, camera, cameraTransform, resolution);
    // the engine-internal members appended after ambient, and Lit's camera block
    fillFrameLighting(passFrame, passLitFrameBlocks[POST_BLEND_OWN].lit, camera);
    // no post-processing colour unless the pass is the post-processed camera's colour pass
    // (renderCameraPass sets it then)
    fillPassPostColor(false, Rect(), 0.0f);
}

PostProcessingTiers RenderSystem::getPostProcessingTiers(const PostProcessingSettings& settings){
    PostProcessingTiers tiers;
    tiers.renderScale = std::min(std::max(settings.renderScale, 0.5f), 1.0f);
    tiers.msaaSamples = settings.msaaSamples >= 4 ? 4 : (settings.msaaSamples >= 2 ? 2 : 1);

    uint32_t features = 0;
    if (settings.fxaaEnabled) features |= UBER_FXAA;
    if (settings.chromaticAberrationEnabled && settings.chromaticAberrationIntensity > 0.0f) features |= UBER_CHROMATIC;
    if (settings.pixelateEnabled && settings.pixelateSize > 1.0f) features |= UBER_PIXELATE;
    if (settings.crtEnabled) features |= UBER_CRT;
    if (settings.edgeDetectEnabled && settings.edgeDetectIntensity > 0.0f) features |= UBER_EDGE;
    if (settings.filmGrainEnabled && settings.filmGrainIntensity > 0.0f) features |= UBER_GRAIN;
    if (settings.sharpenEnabled && settings.sharpenIntensity > 0.0f) features |= UBER_SHARPEN;
    if (settings.bloomEnabled && settings.bloomIntensity > 0.0f) features |= UBER_BLOOM;
    if (settings.blurEnabled) features |= UBER_BLUR;

    const bool curve = PostColorLut::hasCurve(settings);
    const bool grade = PostColorLut::hasGrade(settings);
    const bool vignette = settings.vignetteEnabled && settings.vignetteIntensity > 0.0f;
    const bool dither = settings.ditherEnabled;

    // (msaaSamples does not run the uber pass: the multisampled scene target resolves into
    // whatever offscreen target the camera draws to, see the phase 4 spec's notes 5.2)
    tiers.uber = features != 0 || tiers.renderScale < 1.0f;
    if (tiers.uber){
        // the grade, vignette and dither move to the uber pass (after the bloom composite and
        // the chromatic aberration, so they are graded once); exposure and the curve stay in
        // the shaders, which have the unclamped linear colour
        if (grade) features |= UBER_LUT;
        if (vignette) features |= UBER_VIGNETTE;
        if (dither) features |= UBER_DITHER;
        tiers.tailColor = curve;
        tiers.tailGrade = false;
    }else{
        tiers.tailColor = curve || grade || vignette || dither;
        tiers.tailGrade = true;
    }
    // the render scale alone: the plain upscale variant (a BLIT variant 0 is the blit)
    if (tiers.uber && features == 0)
        features = UBER_UPSCALE;
    tiers.uberFeatures = features;
    return tiers;
}

void RenderSystem::bakePostColorLut(TextureRender& texture, int& textureSize, PostColorLut::Stage stage){
    const PostProcessingSettings& settings = scene->getPostProcessing();
    const int n = settings.lutSize > 16 ? 32 : 16;
    if (texture.isCreated() && textureSize != n){
        texture.destroyTexture();
        textureSize = 0;
    }
    if (!texture.isCreated()){
        if (!texture.createDynamicColorTexture(stage == PostColorLut::Stage::GRADE ? "post grade LUT" : "post colour LUT", n * n, n))
            return;
        textureSize = n;
    }
    // the shaper domain's k (the display domain has none)
    float shaper = 1.0f;
    if (stage != PostColorLut::Stage::GRADE){
        shaper = getPostColorShaper(settings, n);
        postTailShaper = shaper;
    }
    PostColorLut::bake(settings, postLutStrip.size > 1 ? &postLutStrip : nullptr, stage, n, postLutPixels, shaper);
    texture.updateTexture(postLutPixels.data(), postLutPixels.size());
}

void RenderSystem::applyPostProcessing(){
    if (!postProcessingDirty || !Engine::isViewLoaded())
        return;
    postProcessingDirty = false;

    const PostProcessingSettings& settings = scene->getPostProcessing();
    const PostProcessingTiers tiers = getPostProcessingTiers(settings);

    // a new uber variant applies once it is built (or failed): until then the frame keeps the
    // tiers it draws with (its uber variant, its in-shader colour), instead of frames without
    // the uber pass and its grade, vignette and dither, and the scene camera leaving its
    // offscreen target for them (a reload of every object, twice)
    if (tiers.uber && tiers.uberFeatures != uberFeatures){
        std::shared_ptr<ShaderRender> shader = ShaderPool::get(ShaderType::BLIT, tiers.uberFeatures);
        if ((!shader || !shader->isCreated()) && !ShaderPool::isShaderBuildFailed(ShaderType::BLIT, tiers.uberFeatures)){
            postProcessingDirty = true;
            return;
        }
    }

    // no uber pass: its variant goes (its grade LUT below with it); no bloom or blur: the
    // chain's programs (their targets go in renderCameraPass)
    if (!tiers.uber && uberLoaded)
        destroyUber();
    if (!(tiers.uberFeatures & (UBER_BLOOM | UBER_BLUR)))
        destroyBloomPrograms();

    // the user's LUT strip, read once per path (a missing or malformed one is logged and left out)
    const std::string stripPath = settings.colorGradingEnabled ? settings.lutTexture : std::string();
    if (stripPath != postLutStripPath){
        postLutStripPath = stripPath;
        if (!PostColorLut::loadStrip(stripPath, postLutStrip))
            postLutStrip = PostColorLut::Strip();
    }

    // the in-shader LUT: exposure, curve and (without the uber pass) the grade
    if (tiers.tailColor){
        bakePostColorLut(postTailLut, postTailLutSize, tiers.tailGrade ? PostColorLut::Stage::ALL : PostColorLut::Stage::CURVE);
    }else if (postTailLut.isCreated()){
        postTailLut.destroyTexture();
        postTailLutSize = 0;
    }
    // the uber pass's grade (kept while a loaded variant samples it: loadUber frees it)
    if (tiers.uberFeatures & UBER_LUT){
        bakePostColorLut(postGradeLut, postGradeLutSize, PostColorLut::Stage::GRADE);
    }else if (postGradeLut.isCreated() && !(uberLoaded && (uberFeatures & UBER_LUT))){
        postGradeLut.destroyTexture();
        postGradeLutSize = 0;
    }

    // one global variant change per toggle of the in-shader colour; the values above are data.
    // Every object keeps drawing its variant until its new one is built (meshes each, points,
    // lines and sky together: update, postColorVariantsReady)
    if (tiers.tailColor != postColorVariants){
        postColorVariants = tiers.tailColor;
        needReloadMeshVariants();
        postColorReloadPending = true;
    }
    postTiers = tiers;
    // the new uber variant (built: see above) replaces the loaded one now, with these tiers
    if (tiers.uber)
        loadUber(tiers.uberFeatures);
}

bool RenderSystem::postColorVariantsReady(){
    if (!Engine::isViewLoaded())
        return false;
    // a variant is ready when built or failed (its load falls back to the built-in one)
    auto ready = [](ShaderType type, uint32_t properties, uint16_t customId){
        return ShaderPool::get(type, properties, customId)->isCreated() || ShaderPool::isShaderBuildFailed(type, properties, customId);
    };
    bool allReady = true;
    auto pointsArray = scene->getComponentArray<PointsComponent>();
    for (size_t i = 0; i < pointsArray->size(); i++){
        const PointsComponent& points = pointsArray->getComponentFromIndex(i);
        if (!points.loaded || points.needReload)
            continue;
        const uint32_t bit = ShaderPool::getPostColorProperty(ShaderType::POINTS);
        const uint32_t properties = (points.shaderProperties & ~bit) | postColorProperty(ShaderType::POINTS);
        allReady = ready(ShaderType::POINTS, properties, points.customShaderId) && allReady;
    }
    auto linesArray = scene->getComponentArray<LineRendererComponent>();
    for (size_t i = 0; i < linesArray->size(); i++){
        const LineRendererComponent& lines = linesArray->getComponentFromIndex(i);
        if (!lines.loaded || lines.needReload)
            continue;
        const uint32_t bit = ShaderPool::getPostColorProperty(ShaderType::LINES);
        const uint32_t properties = (lines.shaderProperties & ~bit) | postColorProperty(ShaderType::LINES);
        allReady = ready(ShaderType::LINES, properties, lines.customShaderId) && allReady;
    }
    auto skyArray = scene->getComponentArray<SkyboxComponent>();
    for (size_t i = 0; i < skyArray->size(); i++){
        const SkyboxComponent& sky = skyArray->getComponentFromIndex(i);
        if (!sky.loaded || sky.needReload)
            continue;
        allReady = ready(ShaderType::SKYBOX, postColorProperty(ShaderType::SKYBOX), sky.customShaderId) && allReady;
    }
    return allReady;
}

uint32_t RenderSystem::postColorProperty(ShaderType type) const{
    return postColorVariants ? ShaderPool::getPostColorProperty(type) : 0;
}

void RenderSystem::setPassPostColor(const post_color_t& postColor){
    // the frame block (library shaders: their BLEND_* define picks the encoding) and the pass
    // block of each blend a built-in shader may draw with (post_color.glsl's
    // postColorEncodeBlended reads shaper.y): its own colour, an added one, a premultiplied
    // one, and the plain encoding of a multiply blend's factor (neutral: no LUT, vignette or
    // dither)
    passFrame.postColor = postColor;
    for (int blend = 0; blend < POST_BLEND_COUNT; blend++){
        post_color_t& block = passPostColors[blend];
        block = postColor;
        if (blend == POST_BLEND_MULTIPLY){
            block.vignetteScaleOffset = Vector4(0.0f, 0.0f, 0.0f, 0.0f);
            block.vignetteColor = Vector4(0.0f, 0.0f, 0.0f, 0.0f);
            block.params = Vector4(0.0f, -1.0f, 0.0f, 0.0f);
        }else{
            block.shaper.y = (float)blend;
        }
        passLitFrameBlocks[blend].postColor = block;
        if (blend != POST_BLEND_OWN)
            passLitFrameBlocks[blend].lit = passLitFrameBlocks[POST_BLEND_OWN].lit;
    }
}

float RenderSystem::getPostColorShaper(const PostProcessingSettings& settings, int lutSide){
    // the shaper s = sqrt(x / (x + k)) puts linear 1 near s = 0.71 (k = 1). Tone mapping NONE
    // clips at x = 2^-exposure: a kink that linear filtering between two texels rounds off
    // (white 245/255 at side 16), so there k moves the clip point onto the texel nearest 0.71.
    // The curves are smooth there and keep k = 1.
    if (settings.tonemap != TonemapMode::NONE || lutSide < 2)
        return 1.0f;
    const float last = (float)(lutSide - 1);
    const float c = std::round(0.70710678f * last) / last;
    const float clip = std::exp2(-settings.exposure);
    return clip * (1.0f / (c * c) - 1.0f);
}

void RenderSystem::fillPassPostColor(bool active, const Rect& viewport, float targetHeight){
    post_color_t postColor;
    postColor.vignetteScaleOffset = Vector4(0.0f, 0.0f, 0.0f, 0.0f);
    postColor.vignetteColor = Vector4(0.0f, 0.0f, 0.0f, 0.0f);
    postColor.params = Vector4(0.0f, -1.0f, 0.0f, 0.0f);
    postColor.shaper = Vector4(1.0f, 0.0f, 0.0f, 0.0f);
    if (!active || !postTiers.tailColor || !postTailLut.isCreated()){
        setPassPostColor(postColor);
        return;
    }
    const PostProcessingSettings& settings = scene->getPostProcessing();
    postColor.params.w = (float)postTailLutSize;
    postColor.shaper.x = postTailShaper;
    // vignette and dither belong to the tail without the uber pass (which does them otherwise)
    if (postTiers.tailGrade){
        if (settings.vignetteEnabled && settings.vignetteIntensity > 0.0f){
            fillVignette(settings, viewport, targetHeight, postColor.vignetteScaleOffset, postColor.vignetteColor,
                         postColor.params.x, postColor.params.y);
        }
        if (settings.ditherEnabled)
            postColor.params.z = 1.0f / 255.0f;
    }
    setPassPostColor(postColor);
}

void RenderSystem::fillVignette(const PostProcessingSettings& settings, const Rect& viewport, float targetHeight,
                                Vector4& scaleOffset, Vector4& color, float& falloffScale, float& falloffBias) const{
    const float w = viewport.getWidth();
    const float h = viewport.getHeight();
    if (w <= 0.0f || h <= 0.0f){
        scaleOffset = Vector4(0.0f, 0.0f, 0.0f, 0.0f);
        color = Vector4(0.0f, 0.0f, 0.0f, 0.0f);
        falloffScale = 0.0f;
        falloffBias = -1.0f;
        return;
    }
    // the viewport's centre in gl_FragCoord: viewports are applied from the bottom
    // (SokolCamera::applyViewport), gl_FragCoord counts from the bottom on GL only
    const float centreX = viewport.getX() + 0.5f * w;
    const float centreY = Engine::isOpenGL() ? viewport.getY() + 0.5f * h : targetHeight - (viewport.getY() + 0.5f * h);
    // 1 at the edges; roundness scales x towards the height's units (a circle)
    const float scaleX = (1.0f + (w / h - 1.0f) * settings.vignetteRoundness) / (0.5f * w);
    const float scaleY = 1.0f / (0.5f * h);
    scaleOffset = Vector4(scaleX, scaleY, centreX * scaleX, centreY * scaleY);
    color = Vector4(settings.vignetteColor.x, settings.vignetteColor.y, settings.vignetteColor.z, settings.vignetteIntensity);
    // falloff over dot(p, p): from start to start + width, start and width by smoothness
    const float start = 0.6f * (1.0f - settings.vignetteSmoothness);
    const float width = 0.2f + 1.6f * settings.vignetteSmoothness;
    falloffScale = 1.0f / width;
    falloffBias = -start / width;
}

bool RenderSystem::loadUber(uint32_t features){
    if (uberLoaded && uberFeatures == features)
        return true;
    if (features == 0)
        return false;

    // the variant may still be building (async in the editor, at runtime without precompiled
    // ones): the loaded one keeps drawing until it is ready (applyPostProcessing waits for it
    // too), so the scene camera never drops off its offscreen target for those frames
    std::shared_ptr<ShaderRender> shader = ShaderPool::get(ShaderType::BLIT, features);
    if (!shader || !shader->isCreated()){
        if (ShaderPool::isShaderBuildFailed(ShaderType::BLIT, features)){
            destroyUber();
            return false;
        }
        return uberLoaded;
    }
    const bool lut = uberLoaded && (uberFeatures & UBER_LUT);
    if (uberLoaded)
        destroyUber();
    // the grade LUT an old variant kept (applyPostProcessing) goes with it
    if (lut && !(features & UBER_LUT) && !(postTiers.uberFeatures & UBER_LUT) && postGradeLut.isCreated()){
        postGradeLut.destroyTexture();
        postGradeLutSize = 0;
    }
    uberShader = shader;
    uberFeatures = features;

    // the swapchain (PIP_DEFAULT) or a target (PIP_RTT: the editor's views, a render-to-texture camera)
    ShaderData& shaderData = uberShader.get()->shaderData;
    uberRender.beginLoad(PrimitiveType::TRIANGLES);
    uberRender.setShader(uberShader.get());
    uberSlotVSParams = shaderData.getUniformBlockIndexByName("u_vs_uberParams");
    uberSlotFSParams = shaderData.getUniformBlockIndexByName("u_fs_uberParams");
    uberSlotSource = shaderData.getTextureIndexByName("u_sceneColorTexture");
    uberSlotLut = shaderData.getTextureIndexByName("u_colorLut");
    uberSlotBloom = shaderData.getTextureIndexByName("u_bloomTexture");
    uberSlotBlur = shaderData.getTextureIndexByName("u_blurTexture");
    if (!uberRender.endLoad(PIP_RTT | PIP_DEFAULT, false, true, CullingMode::BACK, WindingOrder::CCW)){
        uberShader.reset();
        uberFeatures = 0;
        return false;
    }
    uberLoaded = true;
    return true;
}

void RenderSystem::destroyUber(){
    if (uberLoaded){
        uberRender.destroy();
    }
    if (uberShader){
        uberShader.reset();
        ShaderPool::remove(ShaderType::BLIT, uberFeatures);
    }
    uberLoaded = false;
    uberFeatures = 0;
    uberSlotVSParams = -1;
    uberSlotFSParams = -1;
    uberSlotSource = {-1, -1};
    uberSlotLut = {-1, -1};
    uberSlotBloom = {-1, -1};
    uberSlotBlur = {-1, -1};
}

bool RenderSystem::ensurePostSceneFramebuffer(unsigned int width, unsigned int height){
    if (width == 0 || height == 0)
        return false;
    if (rv().postSceneFramebuffer.isCreated() && rv().postSceneWidth == width && rv().postSceneHeight == height)
        return true;

    rv().postSceneFramebuffer.destroy();
    // the scene at the render scale, bilinear for the upscale and the uber pass's taps
    rv().postSceneFramebuffer.setWidth(width);
    rv().postSceneFramebuffer.setHeight(height);
    rv().postSceneFramebuffer.setMinFilter(TextureFilter::LINEAR);
    rv().postSceneFramebuffer.setMagFilter(TextureFilter::LINEAR);
    rv().postSceneFramebuffer.setWrapU(TextureWrap::CLAMP_TO_EDGE);
    rv().postSceneFramebuffer.setWrapV(TextureWrap::CLAMP_TO_EDGE);
    rv().postSceneFramebuffer.create();
    rv().postSceneWidth = width;
    rv().postSceneHeight = height;
    return rv().postSceneFramebuffer.isCreated();
}

TextureRender* RenderSystem::linearSampler(){
    if (!postLinearSampler.isCreated()){
        void* data[1] = {pixelsBlack};
        size_t size[1] = {sizeof(pixelsBlack)};
        postLinearSampler.createTexture("linear sampler", 8, 8, ColorFormat::RGBA, TextureType::TEXTURE_2D, 1, data, size,
                                        TextureFilter::LINEAR, TextureFilter::LINEAR, TextureWrap::CLAMP_TO_EDGE, TextureWrap::CLAMP_TO_EDGE);
    }
    return &postLinearSampler;
}

void RenderSystem::renderUber(TextureRender* source, unsigned int sourceWidth, unsigned int sourceHeight,
                              FramebufferRender* destination, const Rect& viewport, bool nearestSource){
    // the background around the viewport, as the blit (CLEAR: a tiler loads nothing)
    uberPassRender.setClearColor(scene->getBackgroundColor());
    uberPassRender.setLabel("uber");

    PipelineType pip = PIP_RTT;
    bool flipGL = false;
    float targetHeight = 0.0f;
    if (destination){
        uberPassRender.startRenderPass(destination);
        targetHeight = viewport.getY() + viewport.getHeight();
    }else if (Framebuffer* target = targetFramebuffer()){
        if (!target->isCreated()){
            target->create();
        }
        // source and destination are both flipped on GL
        uberPassRender.startRenderPass(&target->getRender());
        targetHeight = (float)target->getHeight();
    }else{
        uberPassRender.startRenderPass();
        pip = PIP_DEFAULT;
        // the source was rendered flipped (PIP_RTT) but the GL swapchain is not
        flipGL = Engine::isOpenGL();
        targetHeight = (float)System::instance().getScreenHeight();
    }
    uberPassRender.applyViewport(viewport);

    if (uberRender.beginDraw(pip)){
        const PostProcessingSettings& settings = scene->getPostProcessing();
        // bilinear taps always (FXAA, sharpen, edge and the bloom's assume it): a NEAREST source
        // through a linear sampler, its UV snapped to texel centres in the shader
        if (nearestSource)
            uberRender.addTexture(uberSlotSource, ShaderStageType::FRAGMENT, source, linearSampler());
        else
            uberRender.addTexture(uberSlotSource, ShaderStageType::FRAGMENT, source);
        if (uberSlotLut.first != -1)
            uberRender.addTexture(uberSlotLut, ShaderStageType::FRAGMENT, postGradeLut.isCreated() ? &postGradeLut : &emptyWhite);
        if (uberSlotBloom.first != -1)
            uberRender.addTexture(uberSlotBloom, ShaderStageType::FRAGMENT, uberBloomTexture ? uberBloomTexture : &emptyBlack);
        if (uberSlotBlur.first != -1)
            uberRender.addTexture(uberSlotBlur, ShaderStageType::FRAGMENT, uberBlurTexture ? uberBlurTexture : source);

        const Vector4 uvScaleOffset = flipGL ? Vector4(1.0f, 0.0f, -1.0f, 1.0f) : Vector4(1.0f, 0.0f, 1.0f, 0.0f);
        const float sw = (float)std::max(sourceWidth, 1u);
        const float sh = (float)std::max(sourceHeight, 1u);
        const float vw = std::max(viewport.getWidth(), 1.0f);
        const float vh = std::max(viewport.getHeight(), 1.0f);
        vs_uber.uvScaleOffset = uvScaleOffset;
        vs_uber.sourceTexel = Vector4(1.0f / sw, 1.0f / sh, sw, sh);
        fs_uber.uvScaleOffset = uvScaleOffset;
        fs_uber.sourceTexel = vs_uber.sourceTexel;
        fs_uber.outputTexel = Vector4(1.0f / vw, 1.0f / vh, vw, vh);
        fs_uber.effects = Vector4(settings.chromaticAberrationIntensity, settings.pixelateSize, settings.filmGrainIntensity, settings.filmGrainResponse);
        fs_uber.crt = Vector4(settings.crtScanlines, settings.crtMask, settings.crtCurvature, settings.crtVignette);
        fs_uber.edgeSharpenTime = Vector4(settings.edgeDetectThreshold, settings.edgeDetectIntensity, settings.sharpenIntensity,
                                          (float)std::fmod(frameTime, 1000.0));
        fs_uber.edgeColorDither = Vector4(settings.edgeDetectColor.x, settings.edgeDetectColor.y, settings.edgeDetectColor.z,
                                          settings.ditherEnabled ? 1.0f / 255.0f : 0.0f);
        float falloffScale = 0.0f;
        float falloffBias = -1.0f;
        fillVignette(settings, viewport, targetHeight, fs_uber.vignetteScaleOffset, fs_uber.vignetteColor, falloffScale, falloffBias);
        // a scanline per source row once a row is two output pixels or more, else every three
        const float rowPixels = vh / sh;
        fs_uber.vignetteLutScan = Vector4(falloffScale, falloffBias, (float)postGradeLutSize, rowPixels >= 2.0f ? rowPixels : 3.0f);
        // the bloom tint is a colour picked in sRGB; the composite adds in linear light
        const Vector3 tint = Color::sRGBToLinear(settings.bloomTint);
        // (the blur mixes in nothing until its chain made a texture: the source is bound instead)
        fs_uber.bloomBlur = Vector4(tint.x * settings.bloomIntensity, tint.y * settings.bloomIntensity, tint.z * settings.bloomIntensity,
                                    uberBlurTexture ? settings.blurIntensity : 0.0f);
        fs_uber.sampling = Vector4(nearestSource ? 1.0f : 0.0f, 0.0f, 0.0f, 0.0f);

        uberRender.applyUniformBlock(uberSlotVSParams, sizeof(vs_uber_t), &vs_uber);
        uberRender.applyUniformBlock(uberSlotFSParams, sizeof(fs_uber_t), &fs_uber);
        uberRender.draw(0, 3, 1);
    }
    uberPassRender.endRenderPass();

    // where a later pass into the same destination (a layer scene's) reads the blurred scene: the
    // UV the uber pass sampled its source at for that pixel. Viewports count from the bottom;
    // gl_FragCoord does on GL only (see fillVignette)
    const float vx = viewport.getX();
    const float vy = viewport.getY();
    const float vw = std::max(viewport.getWidth(), 1.0f);
    const float vh = std::max(viewport.getHeight(), 1.0f);
    Vector4& screenToUV = frameBlurredSceneBlock.screenToUV;
    if (!Engine::isOpenGL()){
        screenToUV = Vector4(1.0f / vw, 1.0f / vh, -vx / vw, -(targetHeight - vy - vh) / vh);
    }else if (flipGL){
        screenToUV = Vector4(1.0f / vw, -1.0f / vh, -vx / vw, 1.0f + vy / vh);
    }else{
        screenToUV = Vector4(1.0f / vw, 1.0f / vh, -vx / vw, -vy / vh);
    }
    frameBlurredSceneBlock.params = Vector4(1.0f, 0.0f, 0.0f, 0.0f);
}

void RenderSystem::PostChainTargets::destroy(){
    for (Framebuffer& framebuffer : down)
        framebuffer.destroy();
    for (Framebuffer& framebuffer : up)
        framebuffer.destroy();
    sourceWidth = 0;
    sourceHeight = 0;
    levels = -1;
}

bool RenderSystem::loadBloomProgram(BloomProgramKind kind){
    BloomProgram& program = bloomPrograms[kind];
    if (program.loaded)
        return true;

    // ShaderType::BLOOM bits: 'Pre' prefilter, 'Thr' threshold, 'Ups' upsample, 'Mix' level mix
    static const uint32_t properties[BLOOM_PROGRAMS] = {
        (1u << 0) | (1u << 1), (1u << 0), 0u, (1u << 2) | (1u << 3), (1u << 2)
    };
    // the variant may still be building (async in the editor, at runtime without precompiled ones)
    std::shared_ptr<ShaderRender> shader = ShaderPool::get(ShaderType::BLOOM, properties[kind]);
    if (!shader || !shader->isCreated())
        return false;
    program.shader = shader;

    ShaderData& shaderData = shader.get()->shaderData;
    program.render.beginLoad(PrimitiveType::TRIANGLES);
    program.render.setShader(shader.get());
    program.slotVSParams = shaderData.getUniformBlockIndexByName("u_vs_bloomParams");
    program.slotFSParams = shaderData.getUniformBlockIndexByName("u_fs_bloomParams");
    program.slotSource = shaderData.getTextureIndexByName("u_sourceTexture");
    program.slotLevel = shaderData.getTextureIndexByName("u_levelTexture");
    // SRGB8A8 targets without depth, written whole
    if (!program.render.endLoad(PIP_POST, false, false, CullingMode::BACK, WindingOrder::CCW)){
        program.render.destroy();
        program.shader.reset();
        return false;
    }
    program.loaded = true;
    return true;
}

void RenderSystem::destroyBloomPrograms(){
    static const uint32_t properties[BLOOM_PROGRAMS] = {
        (1u << 0) | (1u << 1), (1u << 0), 0u, (1u << 2) | (1u << 3), (1u << 2)
    };
    for (int kind = 0; kind < BLOOM_PROGRAMS; kind++){
        BloomProgram& program = bloomPrograms[kind];
        if (program.loaded)
            program.render.destroy();
        if (program.shader){
            program.shader.reset();
            ShaderPool::remove(ShaderType::BLOOM, properties[kind]);
        }
        program = BloomProgram();
    }
}

bool RenderSystem::ensurePostChainTargets(PostChainTargets& targets, unsigned int sourceWidth, unsigned int sourceHeight, int levels){
    if (sourceWidth == 0 || sourceHeight == 0)
        return false;
    if (targets.isCreated() && targets.sourceWidth == sourceWidth && targets.sourceHeight == sourceHeight && targets.levels == levels)
        return true;

    // made again in place: a new size or level count replaces the old targets (no pile-up)
    targets.destroy();
    for (int level = 0; level <= levels; level++){
        // 1/4, 1/8, 1/16, 1/32 of the source (at least a texel)
        const unsigned int divisor = 4u << level;
        const unsigned int width = std::max(1u, sourceWidth / divisor);
        const unsigned int height = std::max(1u, sourceHeight / divisor);
        Framebuffer* sizes[2] = {&targets.down[level], level < levels ? &targets.up[level] : nullptr};
        for (Framebuffer* framebuffer : sizes){
            if (!framebuffer)
                continue;
            ColorFormat format = ColorFormat::SRGB8A8;
            framebuffer->setColorAttachments(1, &format);
            framebuffer->setDepthAttachment(FramebufferDepth::NONE);
            framebuffer->setWidth(width);
            framebuffer->setHeight(height);
            framebuffer->setMinFilter(TextureFilter::LINEAR);
            framebuffer->setMagFilter(TextureFilter::LINEAR);
            framebuffer->setWrapU(TextureWrap::CLAMP_TO_EDGE);
            framebuffer->setWrapV(TextureWrap::CLAMP_TO_EDGE);
            framebuffer->create();
            if (!framebuffer->isCreated()){
                targets.destroy();
                return false;
            }
        }
    }
    targets.sourceWidth = sourceWidth;
    targets.sourceHeight = sourceHeight;
    targets.levels = levels;
    return true;
}

bool RenderSystem::ensureMsaaFramebuffer(unsigned int width, unsigned int height, int samples){
    if (width == 0 || height == 0 || samples < 2)
        return false;
    if (rv().msaaFramebuffer.isCreated() && rv().msaaWidth == width && rv().msaaHeight == height && rv().msaaSamples == samples)
        return true;
    // made again in place for a new size (no pile-up)
    rv().msaaFramebuffer.destroy();
    rv().msaaWidth = 0;
    rv().msaaHeight = 0;
    rv().msaaSamples = 1;
    if (!rv().msaaFramebuffer.getRender().createMultisampleFramebuffer((int)width, (int)height, samples))
        return false;
    rv().msaaWidth = width;
    rv().msaaHeight = height;
    rv().msaaSamples = samples;
    return true;
}

bool RenderSystem::ensureMsaaResolveFramebuffer(unsigned int width, unsigned int height){
    if (width == 0 || height == 0)
        return false;
    Framebuffer& resolve = rv().msaaResolveFramebuffer;
    if (resolve.isCreated() && resolve.getWidth() == width && resolve.getHeight() == height)
        return true;
    resolve.destroy();
    resolve.setWidth(width);
    resolve.setHeight(height);
    resolve.setMinFilter(TextureFilter::LINEAR);
    resolve.setMagFilter(TextureFilter::LINEAR);
    resolve.setWrapU(TextureWrap::CLAMP_TO_EDGE);
    resolve.setWrapV(TextureWrap::CLAMP_TO_EDGE);
    // only resolved into (the multisampled target has the depth)
    resolve.setDepthAttachment(FramebufferDepth::NONE);
    resolve.create();
    return resolve.isCreated();
}

void RenderSystem::renderBloomPass(BloomProgramKind kind, const char* label, TextureRender* source, unsigned int sourceWidth,
                                   unsigned int sourceHeight, TextureRender* level, Framebuffer& target, const Vector4& offset,
                                   TextureRender* samplerOf){
    BloomProgram& program = bloomPrograms[kind];
    // CLEAR: the pass writes every texel, so a tiler loads nothing (DONTCARE does nothing on GL)
    bloomPassRender.setClearColor(Vector4(0.0f, 0.0f, 0.0f, 1.0f));
    bloomPassRender.setLabel(label);
    bloomPassRender.startRenderPass(&target.getRender());
    if (program.render.beginDraw(PIP_POST)){
        if (samplerOf)
            program.render.addTexture(program.slotSource, ShaderStageType::FRAGMENT, source, samplerOf);
        else
            program.render.addTexture(program.slotSource, ShaderStageType::FRAGMENT, source);
        if (program.slotLevel.first != -1)
            program.render.addTexture(program.slotLevel, ShaderStageType::FRAGMENT, level ? level : &emptyBlack);
        vs_bloom.offset = offset;
        program.render.applyUniformBlock(program.slotVSParams, sizeof(vs_bloom_t), &vs_bloom);
        if (program.slotFSParams != -1)
            program.render.applyUniformBlock(program.slotFSParams, sizeof(fs_bloom_t), &fs_bloom);
        program.render.draw(0, 3, 1);
    }
    bloomPassRender.endRenderPass();
    (void)sourceWidth;
    (void)sourceHeight;
}

TextureRender* RenderSystem::renderBloomChain(TextureRender* source, unsigned int sourceWidth, unsigned int sourceHeight, bool nearestSource){
    if (!loadBloomProgram(BLOOM_PREFILTER_THRESHOLD) || !loadBloomProgram(BLOOM_DOWN) || !loadBloomProgram(BLOOM_UP_MIX))
        return nullptr;
    PostChainTargets& targets = rv().bloomTargets;
    const int levels = PostChainTargets::LEVELS - 1;
    if (!ensurePostChainTargets(targets, sourceWidth, sourceHeight, levels))
        return nullptr;

    const PostProcessingSettings& settings = scene->getPostProcessing();
    // the threshold is a display brightness: in the linear (gamma-2) domain the chain works in,
    // its square; the soft knee a fraction of it (Unity's curve)
    const float threshold = settings.bloomThreshold * settings.bloomThreshold;
    const float knee = threshold * settings.bloomKnee + 1e-5f;
    fs_bloom.curve = Vector4(threshold - knee, 2.0f * knee, 0.25f / knee, threshold);
    fs_bloom.params = Vector4(settings.bloomScatter, 0.0f, 0.0f, 0.0f);

    // the scene to 1/4: taps one source texel off the centre (the 2x2 quadrants of the 4x4 box)
    renderBloomPass(BLOOM_PREFILTER_THRESHOLD, "bloom prefilter", source, sourceWidth, sourceHeight, nullptr, targets.down[0],
                    Vector4(1.0f / (float)sourceWidth, 1.0f / (float)sourceHeight, 0.0f, 0.0f), nearestSource ? linearSampler() : nullptr);
    // down: taps one texel of the larger image (half of the smaller) off the centre
    for (int level = 0; level < levels; level++){
        Framebuffer& from = targets.down[level];
        renderBloomPass(BLOOM_DOWN, "bloom down", &from.getRender().getColorTexture(), from.getWidth(), from.getHeight(), nullptr,
                        targets.down[level + 1], Vector4(1.0f / (float)from.getWidth(), 1.0f / (float)from.getHeight(), 0.0f, 0.0f));
    }
    // up: from the smallest, each mixed with its level of the way down by the scatter
    Framebuffer* from = &targets.down[levels];
    for (int level = levels - 1; level >= 0; level--){
        renderBloomPass(BLOOM_UP_MIX, "bloom up", &from->getRender().getColorTexture(), from->getWidth(), from->getHeight(),
                        &targets.down[level].getRender().getColorTexture(), targets.up[level],
                        Vector4(0.5f / (float)from->getWidth(), 0.5f / (float)from->getHeight(), 0.0f, 0.0f));
        from = &targets.up[level];
    }
    return &targets.up[0].getRender().getColorTexture();
}

int RenderSystem::getBlurLevels(float radius){
    radius = std::min(std::max(radius, 0.0f), 4.0f);
    return std::max(0, std::min(PostChainTargets::LEVELS - 1, (int)std::ceil(radius - 1e-3f)));
}

TextureRender* RenderSystem::renderBlurChain(TextureRender* source, unsigned int sourceWidth, unsigned int sourceHeight, bool nearestSource){
    if (!loadBloomProgram(BLOOM_PREFILTER_PLAIN) || !loadBloomProgram(BLOOM_DOWN) || !loadBloomProgram(BLOOM_UP))
        return nullptr;
    // the radius: as many dual-filter levels (each doubles the reach), the taps spread by the
    // fraction left (0: the quarter-resolution box alone)
    const float radius = std::min(std::max(scene->getPostProcessing().blurRadius, 0.0f), 4.0f);
    const int levels = getBlurLevels(radius);
    const float spread = levels > 0 ? radius / (float)levels : 1.0f;
    PostChainTargets& targets = rv().blurTargets;
    if (!ensurePostChainTargets(targets, sourceWidth, sourceHeight, levels))
        return nullptr;

    renderBloomPass(BLOOM_PREFILTER_PLAIN, "blur prefilter", source, sourceWidth, sourceHeight, nullptr, targets.down[0],
                    Vector4(1.0f / (float)sourceWidth, 1.0f / (float)sourceHeight, 0.0f, 0.0f), nearestSource ? linearSampler() : nullptr);
    for (int level = 0; level < levels; level++){
        Framebuffer& from = targets.down[level];
        renderBloomPass(BLOOM_DOWN, "blur down", &from.getRender().getColorTexture(), from.getWidth(), from.getHeight(), nullptr,
                        targets.down[level + 1], Vector4(spread / (float)from.getWidth(), spread / (float)from.getHeight(), 0.0f, 0.0f));
    }
    Framebuffer* from = &targets.down[levels];
    for (int level = levels - 1; level >= 0; level--){
        renderBloomPass(BLOOM_UP, "blur up", &from->getRender().getColorTexture(), from->getWidth(), from->getHeight(), nullptr,
                        targets.up[level], Vector4(0.5f * spread / (float)from->getWidth(), 0.5f * spread / (float)from->getHeight(), 0.0f, 0.0f));
        from = &targets.up[level];
    }
    return &from->getRender().getColorTexture();
}

void RenderSystem::applyBlurredScene(ObjectRender& render, const ShaderLibraryBindings& bindings){
    if (bindings.slotFSBlurredScene != -1){
        static const blurred_scene_t none = {Vector4(0.0f, 0.0f, 0.0f, 0.0f), Vector4(0.0f, 0.0f, 0.0f, 0.0f)};
        render.applyUniformBlock(bindings.slotFSBlurredScene, sizeof(blurred_scene_t),
                                 frameBlurredScene ? (void*)&frameBlurredSceneBlock : (void*)&none);
    }
    if (bindings.slotBlurredScene.first != -1)
        render.addTexture(bindings.slotBlurredScene, ShaderStageType::FRAGMENT, frameBlurredScene ? frameBlurredScene : &emptyBlack);
}

void RenderSystem::applyFrameBlock(ObjectRender& render, const ShaderLibraryBindings& bindings){
    if (bindings.slotVSFrame != -1)
        render.applyUniformBlock(bindings.slotVSFrame, bindings.frameSize, &passFrame);
    if (bindings.slotFSFrame != -1)
        render.applyUniformBlock(bindings.slotFSFrame, bindings.frameSizeFS, &passFrame);
    applyPostColor(render, bindings);
    applyBlurredScene(render, bindings);
    if (bindings.slotFSHeightFog != -1)
        render.applyUniformBlock(bindings.slotFSHeightFog, sizeof(height_fog_t), &passHeightFog);
}

void RenderSystem::applyPostColor(ObjectRender& render, const ShaderLibraryBindings& bindings){
    if (bindings.slotFSPostColor != -1)
        render.applyUniformBlock(bindings.slotFSPostColor, sizeof(post_color_t), &passPostColors[bindings.postColorBlend]);
    if (bindings.slotPostColorLut.first != -1)
        render.addTexture(bindings.slotPostColorLut, ShaderStageType::FRAGMENT, postTailLut.isCreated() ? &postTailLut : &emptyWhite);
}

void RenderSystem::applySceneInputs(ObjectRender& render, const ShaderLibraryBindings& bindings){
    if (bindings.slotFSSceneInputs != -1)
        render.applyUniformBlock(bindings.slotFSSceneInputs, bindings.sceneInputsSize, &passSceneBlock);
    // the decoded white is past the far plane (scene.glsl clamps it to it): no occluder
    if (bindings.slotSceneDepth.first != -1)
        render.addTexture(bindings.slotSceneDepth, ShaderStageType::FRAGMENT, passSceneDepth ? passSceneDepth : &emptyWhite);
    if (bindings.slotSceneColor.first != -1)
        render.addTexture(bindings.slotSceneColor, ShaderStageType::FRAGMENT, passSceneColor ? passSceneColor : &emptyBlack);
}

void RenderSystem::applyEnvironment(ObjectRender& render, const ShaderLibraryBindings& bindings, const MeshComponent& mesh, const Transform& transform){
    if (bindings.slotFSEnvironment == -1 && bindings.slotEnvironment.first == -1)
        return;
    fs_environment_t block;
    block.colorRotation = Vector4(1.0f, 1.0f, 1.0f, 0.0f);
    block.params = Vector4(0.0f, 0.0f, 0.0f, 0.0f);
    TextureRender* texture = &emptyCubeBlack;

    // the probe whose box holds the mesh (as Lit picks it, without its blend and box projection)
    fs_reflection_probe_t probe;
    TextureRender* probeTexture = nullptr;
    if (hasReflectionProbes && selectReflectionProbe(meshProbeSamplePosition(mesh, transform), probe, probeTexture) && probeTexture){
        texture = probeTexture;
        const float intensity = probe.boxMin_intensity.w;
        block.colorRotation = Vector4(intensity, intensity, intensity, 0.0f);
        block.params = Vector4(probe.boxMax_lod.w, 1.0f, 0.0f, 0.0f);
    }else if (hasIBL){
        // the scene sky's prefiltered map, turned and tinted as it lights Lit (environmentInfo)
        auto skys = scene->getComponentArray<SkyboxComponent>();
        if (skys->size() > 0){
            SkyboxComponent& sky = skys->getComponentFromIndex(0);
            if (sky.envMapsLoaded && sky.prefilteredMap && sky.prefilteredMap->isCreated()){
                texture = sky.prefilteredMap.get();
                block.colorRotation = environmentInfo();
                block.params = Vector4((float)(IBLEnvironment::SPECULAR_MIPS - 1), 1.0f, 0.0f, 0.0f);
            }
        }
    }
    if (bindings.slotFSEnvironment != -1)
        render.applyUniformBlock(bindings.slotFSEnvironment, sizeof(fs_environment_t), &block);
    if (bindings.slotEnvironment.first != -1)
        render.addTexture(bindings.slotEnvironment, ShaderStageType::FRAGMENT, texture);
}

bool RenderSystem::loadPBRTextures(Material& material, ShaderData& shaderData, ObjectRender& render, bool receiveLights){
    TextureRender* textureRender = NULL;
    std::pair<int, int> slotTex(-1, -1);

    textureRender = material.baseColorTexture.getRender(&emptyWhite);
    slotTex = shaderData.getTextureIndex(TextureShaderType::BASECOLOR);
    if (textureRender){
        if (!textureRender->isCreated()){
            return false;
        }
        render.addTexture(slotTex, ShaderStageType::FRAGMENT, textureRender);
    }else{
        render.addTexture(slotTex, ShaderStageType::FRAGMENT, &emptyWhite);
    }

    if (hasLights && receiveLights){
        textureRender = material.metallicRoughnessTexture.getRender(&emptyWhite);
        slotTex = shaderData.getTextureIndex(TextureShaderType::METALLICROUGHNESS);
        if (textureRender){
            if (!textureRender->isCreated()){
                return false;
            }
            render.addTexture(slotTex, ShaderStageType::FRAGMENT, textureRender);
        }else{
            render.addTexture(slotTex, ShaderStageType::FRAGMENT, &emptyWhite);
        }

        textureRender = material.occlusionTexture.getRender(&emptyWhite);
        slotTex = shaderData.getTextureIndex(TextureShaderType::OCCULSION);
        if (textureRender){
            if (!textureRender->isCreated()){
                return false;
            }
            render.addTexture(slotTex, ShaderStageType::FRAGMENT, textureRender);
        }else{
            render.addTexture(slotTex, ShaderStageType::FRAGMENT, &emptyWhite);
        }

        // A missing emissive texture must default to white (glTF: emissive =
        // emissiveFactor * emissiveTexture, absent texture = 1,1,1,1). The shader
        // unconditionally multiplies emissiveFactor by this sample for UV meshes, so a
        // black fallback would silently zero out emissiveFactor when no texture is set.
        textureRender = material.emissiveTexture.getRender(&emptyWhite);
        slotTex = shaderData.getTextureIndex(TextureShaderType::EMISSIVE);
        if (textureRender){
            if (!textureRender->isCreated()){
                return false;
            }
            render.addTexture(slotTex, ShaderStageType::FRAGMENT, textureRender);
        }else{
            render.addTexture(slotTex, ShaderStageType::FRAGMENT, &emptyWhite);
        }
    }

    // normal map is sampled by both the 3D lit path and the 2D light path (which
    // otherwise keeps the unlit shader); addTexture no-ops when the compiled
    // variant has no u_normalTexture slot
    if ((hasLights || hasLights2D) && receiveLights){
        textureRender = material.normalTexture.getRender(&emptyNormal);
        slotTex = shaderData.getTextureIndex(TextureShaderType::NORMAL);
        if (textureRender){
            if (!textureRender->isCreated()){
                return false;
            }
            render.addTexture(slotTex, ShaderStageType::FRAGMENT, textureRender);
        }else{
            render.addTexture(slotTex, ShaderStageType::FRAGMENT, &emptyNormal);
        }
    }

    if (hasLights && receiveLights){
        loadSpotMaskTexture(shaderData, render);
    }

    return true;
}

void RenderSystem::updateShadowBindings(){
    if (!needUpdateShadowBindings){
        return;
    }

    auto meshes = scene->getComponentArray<MeshComponent>();
    for (int i = 0; i < meshes->size(); i++){
        MeshComponent& mesh = meshes->getComponentFromIndex(i);
        if (!mesh.loaded || mesh.needReload){
            continue;
        }

        for (int s = 0; s < mesh.numSubmeshes; s++){
            bool has3DShadows = mesh.submeshes[s].shaderProperties & (1u << 4);  // 'Shw'
            bool has2DShadows = mesh.submeshes[s].shaderProperties & (1u << 23); // 'S2d'
            if (!has3DShadows && !has2DShadows){
                continue;
            }
            if (!mesh.submeshes[s].shader){
                continue;
            }

            ShaderData& shaderData = mesh.submeshes[s].shader.get()->shaderData;
            if (has3DShadows){
                loadShadowTextures(shaderData, mesh.submeshes[s].render, mesh.receiveLights, true);
            }
            if (has2DShadows){
                loadShadow2DTexture(shaderData, mesh.submeshes[s].render, true);
            }
        }
    }

    needUpdateShadowBindings = false;
}

void RenderSystem::loadShadowTextures(ShaderData& shaderData, ObjectRender& render, bool receiveLights, bool receiveShadows){
    std::pair<int, int> slotTex(-1, -1);

    if (hasLights && receiveLights && hasShadows && receiveShadows){
        // Bind a depth/comparison fallback when the projective atlas is absent.
        slotTex = shaderData.getTextureIndex(TextureShaderType::SHADOWATLAS);
        render.addTexture(slotTex, ShaderStageType::FRAGMENT,
                          hasShadowAtlas ? &shadowAtlasFramebuffer.getDepthTexture() : &emptyShadowDepth);

        slotTex = shaderData.getTextureIndex(TextureShaderType::SHADOWPOINTATLAS);
        render.addTexture(slotTex, ShaderStageType::FRAGMENT,
                          hasShadowPointAtlas ? &shadowPointAtlasFramebuffer.getColorTexture() : &emptyWhite);
    }
}

bool RenderSystem::bindViewShadowMaps(ShaderData& shaderData, ObjectRender& render, bool receiveLights, bool restore){
    const CameraShadows& shadows = ps();
    const bool otherProjective = shadows.projectiveAtlas && shadows.projectiveAtlas != &shadowAtlasFramebuffer;
    const bool otherPoint = shadows.pointAtlas && shadows.pointAtlas != &shadowPointAtlasFramebuffer;
    if ((!otherProjective && !otherPoint) || !hasLights || !receiveLights)
        return false;

    if (otherProjective){
        TextureRender* atlas = restore
            ? (hasShadowAtlas ? &shadowAtlasFramebuffer.getDepthTexture() : &emptyShadowDepth)
            : &shadows.projectiveAtlas->getDepthTexture();
        render.addTexture(shaderData.getTextureIndex(TextureShaderType::SHADOWATLAS), ShaderStageType::FRAGMENT, atlas);
    }
    if (otherPoint){
        TextureRender* atlas = restore
            ? (hasShadowPointAtlas ? &shadowPointAtlasFramebuffer.getColorTexture() : &emptyWhite)
            : &shadows.pointAtlas->getColorTexture();
        render.addTexture(shaderData.getTextureIndex(TextureShaderType::SHADOWPOINTATLAS), ShaderStageType::FRAGMENT, atlas);
    }
    return true;
}

void RenderSystem::loadShadow2DTexture(ShaderData& shaderData, ObjectRender& render, bool receiveShadows2D){
    if (receiveShadows2D){
        // same white fallback as the 3D atlases: decoded depth ~1.0 = unoccluded
        std::pair<int, int> slotTex = shaderData.getTextureIndex(TextureShaderType::SHADOW2DATLAS);
        render.addTexture(slotTex, ShaderStageType::FRAGMENT,
                          hasShadow2DAtlas ? &shadow2DAtlasFramebuffer.getColorTexture() : &emptyWhite);
    }
}

// Merges every enabled ShadowCaster2D into shadowCaster2DSegments as a world-space line
// list: each vertex carries its own endpoint (POSITION) plus the segment's other
// endpoint (TEXCOORD1), which the shadow2d vertex shader needs to unwrap segments
// crossing the polar seam. Returns the vertex count.
unsigned int RenderSystem::buildShadowCaster2DSegments(){
    shadowCaster2DSegments.clear();

    auto occluders = scene->getComponentArray<ShadowCaster2DComponent>();

    std::vector<Vector2> worldPoints;
    for (int i = 0; i < occluders->size(); i++){
        ShadowCaster2DComponent& occluder = occluders->getComponentFromIndex(i);
        if (!occluder.enabled){
            continue;
        }
        Entity entity = occluders->getEntity(i);
        Transform* transform = scene->findComponent<Transform>(entity);
        if (!transform || !transform->visible || !scene->isEntityActiveInHierarchy(entity)){
            continue;
        }

        worldPoints.clear();
        bool closed = true;

        if (occluder.shape == ShadowCaster2DShape::AUTO_QUAD){
            MeshComponent* mesh = scene->findComponent<MeshComponent>(entity);
            if (!mesh || mesh->aabb == AABB::ZERO){
                continue;
            }
            Vector3 mn = mesh->aabb.getMinimum();
            Vector3 mx = mesh->aabb.getMaximum();
            worldPoints.push_back(Vector2(mn.x, mn.y));
            worldPoints.push_back(Vector2(mx.x, mn.y));
            worldPoints.push_back(Vector2(mx.x, mx.y));
            worldPoints.push_back(Vector2(mn.x, mx.y));
        }else{
            worldPoints.assign(occluder.points.begin(), occluder.points.end());
            closed = occluder.closed;
        }

        size_t numPoints = worldPoints.size();
        if (numPoints < 2){
            continue;
        }

        for (size_t p = 0; p < numPoints; p++){
            Vector3 world = transform->modelMatrix * Vector3(worldPoints[p].x, worldPoints[p].y, 0.0f);
            worldPoints[p] = Vector2(world.x, world.y);
        }

        size_t numSegments = closed ? numPoints : (numPoints - 1);
        for (size_t s = 0; s < numSegments; s++){
            const Vector2& a = worldPoints[s];
            const Vector2& b = worldPoints[(s + 1) % numPoints];

            shadowCaster2DSegments.push_back(a.x);
            shadowCaster2DSegments.push_back(a.y);
            shadowCaster2DSegments.push_back(b.x);
            shadowCaster2DSegments.push_back(b.y);

            shadowCaster2DSegments.push_back(b.x);
            shadowCaster2DSegments.push_back(b.y);
            shadowCaster2DSegments.push_back(a.x);
            shadowCaster2DSegments.push_back(a.y);
        }
    }

    return (unsigned int)(shadowCaster2DSegments.size() / 4);
}

bool RenderSystem::loadShadowCaster2DPass(unsigned int vertexCapacity){
    if (!Engine::isViewLoaded())
        return false;

    destroyShadowCaster2DPass(); // also called when growing the buffer capacity

    ObjectRender& render = shadowCaster2DRender;

    render.beginLoad(PrimitiveType::LINES);

    shadow2DShader = ShaderPool::get(ShaderType::SHADOW2D, 0);
    if (!shadow2DShader->isCreated())
        return false;
    render.setShader(shadow2DShader.get());
    ShaderData& shaderData = shadow2DShader.get()->shaderData;

    shadow2DSlotParams = shaderData.getUniformBlockIndex(UniformBlockType::SHADOW2D_VS_PARAMS);

    shadowCaster2DBuffer.clear();
    shadowCaster2DBuffer.addAttribute(AttributeType::POSITION, 2, 0);
    shadowCaster2DBuffer.addAttribute(AttributeType::TEXCOORD1, 2, 2 * sizeof(float));
    shadowCaster2DBuffer.setStride(4 * sizeof(float));
    shadowCaster2DBuffer.setRenderAttributes(true);
    shadowCaster2DBuffer.setUsage(BufferUsage::STREAM);

    size_t bufferSize = vertexCapacity * shadowCaster2DBuffer.getStride();
    if (bufferSize == 0)
        return false;

    shadowCaster2DBuffer.getRender()->createBuffer(bufferSize, shadowCaster2DBuffer.getData(), shadowCaster2DBuffer.getType(), shadowCaster2DBuffer.getUsage());
    for (auto const &attr : shadowCaster2DBuffer.getAttributes()) {
        render.addAttribute(shaderData.getAttrIndex(attr.first), shadowCaster2DBuffer.getRender(), attr.second.getElements(), attr.second.getDataType(), shadowCaster2DBuffer.getStride(), attr.second.getOffset(), attr.second.getNormalized(), attr.second.getPerInstance());
    }

    if (!render.endLoad(PIP_DEPTH, false, true, CullingMode::BACK, WindingOrder::CCW)){
        return false;
    }

    shadowCaster2DBufferCapacity = vertexCapacity;
    shadowCaster2DLoaded = true;
    return true;
}

void RenderSystem::destroyShadowCaster2DPass(){
    if (!shadowCaster2DLoaded)
        return;

    if (shadow2DShader){
        shadow2DShader.reset();
        ShaderPool::remove(ShaderType::SHADOW2D, 0);
    }

    shadowCaster2DRender.destroy();

    shadowCaster2DBuffer.clearAll();
    shadowCaster2DBuffer.getRender()->destroyBuffer();

    shadow2DSlotParams = -1;
    shadowCaster2DBufferCapacity = 0;
    shadowCaster2DLoaded = false;
}

bool RenderSystem::loadDepthTexture(Material& material, ShaderData& shaderData, ObjectRender& render){
    TextureRender* textureDepthRender = material.baseColorTexture.getRender(&emptyWhite);
    std::pair<int, int> slotTex = shaderData.getTextureIndex(TextureShaderType::DEPTHTEXTURE);
    if (textureDepthRender){
        if (!textureDepthRender->isCreated()){
            return false;
        }
        render.addTexture(slotTex, ShaderStageType::FRAGMENT, textureDepthRender);
    }else{
        render.addTexture(slotTex, ShaderStageType::FRAGMENT, &emptyWhite);
    }

    return true;
}

bool RenderSystem::loadGBufferTextures(Material& material, ShaderData& shaderData, ObjectRender& render){
    // base color (albedo + alpha cutout); bound only when the gbuffer variant declares it
    std::pair<int, int> slotBase = shaderData.getTextureIndex(TextureShaderType::BASECOLOR);
    if (slotBase.first != -1){
        TextureRender* t = material.baseColorTexture.getRender(&emptyWhite);
        if (!t || !t->isCreated()){
            return false;
        }
        render.addTexture(slotBase, ShaderStageType::FRAGMENT, t);
    }

    // metallic-roughness (per-pixel roughness/metallic)
    std::pair<int, int> slotMR = shaderData.getTextureIndex(TextureShaderType::METALLICROUGHNESS);
    if (slotMR.first != -1){
        TextureRender* t = material.metallicRoughnessTexture.getRender(&emptyWhite);
        if (!t || !t->isCreated()){
            return false;
        }
        render.addTexture(slotMR, ShaderStageType::FRAGMENT, t);
    }

    return true;
}

namespace {
    // The loader parses the scale back out of the suffix, so the cache key carries it
    std::string terrainSourceKey(const Texture& texture){
        const std::string path = texture.getPath(0);
        if (path.empty()){
            return std::string();
        }
        return TextureData::hasSvgExtension(path.c_str()) ? TextureData::buildSvgScalePath(path, texture.getSvgScale()) : path;
    }

    bool terrainLayerHasSurfaceMaps(const TerrainSurfaceLayer& layer){
        return !layer.occlusionTexture.empty() || !layer.roughnessTexture.empty() ||
               !layer.metallicTexture.empty() || !layer.heightTexture.empty();
    }

    // A map is read from its conventional channel when it has color, and from its only
    // channel when it is grayscale, so packed and separate sources both work
    unsigned char terrainSourceChannel(TextureData* source, size_t texel, int channel, unsigned char fallback){
        if (!source || !source->getData()){
            return fallback;
        }
        const int channels = source->getChannels();
        if (channel == 3 && channels < 4){
            return fallback;
        }
        const unsigned char* pixels = static_cast<const unsigned char*>(source->getData());
        return pixels[texel * channels + ((channels < 3) ? 0 : channel)];
    }
}

// Read here rather than through Texture, which would upload a 2D image per layer on top
// of the array. Rebuilt only when the set of source files changes.
RenderSystem::TerrainDetailArray* RenderSystem::getTerrainDetailArray(Entity entity, TerrainComponent& terrain){
    // The shader reaches all three layers of any bound blend map, so colors come in threes
    const size_t layerCount = std::min(terrain.surfaceLayers.size(), (size_t)MAX_TERRAIN_LAYERS);
    const size_t colorSlices = ((layerCount + 2) / 3) * 3;

    // One slice key per array slice: the color slices first, then the normal and packed
    // surface slices of the layers that have them
    std::vector<std::string> sliceKeys(colorSlices);
    std::array<int, MAX_TERRAIN_LAYERS> normalSlice;
    std::array<int, MAX_TERRAIN_LAYERS> surfaceSlice;
    normalSlice.fill(-1);
    surfaceSlice.fill(-1);

    bool anySource = false;
    for (size_t i = 0; i < layerCount; i++){
        const TerrainSurfaceLayer& layer = terrain.surfaceLayers[i];
        sliceKeys[i] = terrainSourceKey(layer.colorTexture);
        anySource = anySource || !sliceKeys[i].empty();

        if (!layer.pbr){
            continue;
        }
        if (!layer.normalTexture.empty()){
            normalSlice[i] = (int)sliceKeys.size();
            sliceKeys.push_back("normal|" + terrainSourceKey(layer.normalTexture));
            anySource = true;
        }
        if (terrainLayerHasSurfaceMaps(layer)){
            surfaceSlice[i] = (int)sliceKeys.size();
            sliceKeys.push_back("surface|" + terrainSourceKey(layer.occlusionTexture) + "|" + terrainSourceKey(layer.roughnessTexture) +
                                "|" + terrainSourceKey(layer.metallicTexture) + "|" + terrainSourceKey(layer.heightTexture));
            anySource = true;
        }
    }

    if (!anySource){
        destroyTerrainDetailArray(entity);
        return NULL;
    }

    // One sampler covers every slice, so the first assigned layer sets it for all of them
    const Texture* sampled = &terrain.surfaceLayers[0].colorTexture;
    for (size_t i = 0; i < layerCount; i++){
        if (!terrain.surfaceLayers[i].colorTexture.empty()){
            sampled = &terrain.surfaceLayers[i].colorTexture;
            break;
        }
    }

    // Tiled surface maps alias badly at distance without mipmaps. A color-only terrain
    // keeps whatever filter its layer textures ask for, so its look is unchanged.
    TextureFilter minFilter = sampled->getMinFilter();
    if (minFilter == TextureFilter::LINEAR && hasTerrainSurfaceLayers(terrain)){
        minFilter = TextureFilter::LINEAR_MIPMAP_LINEAR;
    }

    auto it = terrainDetailArrays.find(entity);
    if (it != terrainDetailArrays.end()){
        const TerrainDetailArray& cached = it->second;
        if (cached.sliceKeys == sliceKeys && cached.minFilter == minFilter && cached.magFilter == sampled->getMagFilter() &&
            cached.wrapU == sampled->getWrapU() && cached.wrapV == sampled->getWrapV()){
            // A set that could not be built is remembered, not retried every frame
            return cached.failed ? NULL : &it->second;
        }
        destroyTerrainDetailArray(entity);
    }

    auto remember = [&](bool failed) -> TerrainDetailArray& {
        TerrainDetailArray& entry = terrainDetailArrays[entity];
        entry.sliceKeys = sliceKeys;
        entry.colorSlices = (int)colorSlices;
        entry.normalSlice = normalSlice;
        entry.surfaceSlice = surfaceSlice;
        entry.minFilter = minFilter;
        entry.magFilter = sampled->getMagFilter();
        entry.wrapU = sampled->getWrapU();
        entry.wrapV = sampled->getWrapV();
        entry.failed = failed;
        return entry;
    };

    // A map shared by several layers is read once, and the array takes the largest of them
    std::map<std::string, TextureData> sources;
    int width = 0;
    int height = 0;
    auto source = [&](const Texture& texture) -> TextureData* {
        const std::string key = terrainSourceKey(texture);
        if (key.empty()){
            return NULL;
        }
        auto found = sources.find(key);
        if (found == sources.end()){
            found = sources.emplace(key, TextureData()).first;
            if (!found->second.loadTextureFromFile(key.c_str())){
                return NULL;
            }
            // stb hands over the pixels, and nothing else will free them
            found->second.setDataOwned(true);
            width = std::max(width, found->second.getWidth());
            height = std::max(height, found->second.getHeight());
        }
        return found->second.getData() ? &found->second : NULL;
    };

    struct SliceSources{
        enum Kind{ SLICE_COLOR, SLICE_NORMAL, SLICE_SURFACE };
        Kind kind = SLICE_COLOR;
        TextureData* color = NULL;
        TextureData* normal = NULL;
        TextureData* occlusion = NULL;
        TextureData* roughness = NULL;
        TextureData* metallic = NULL;
        TextureData* height = NULL;
    };
    std::vector<SliceSources> sliceSources(sliceKeys.size());
    for (size_t i = 0; i < layerCount; i++){
        const TerrainSurfaceLayer& layer = terrain.surfaceLayers[i];
        sliceSources[i].color = source(layer.colorTexture);
        if (normalSlice[i] >= 0){
            sliceSources[normalSlice[i]].kind = SliceSources::SLICE_NORMAL;
            sliceSources[normalSlice[i]].normal = source(layer.normalTexture);
        }
        if (surfaceSlice[i] >= 0){
            SliceSources& packed = sliceSources[surfaceSlice[i]];
            packed.kind = SliceSources::SLICE_SURFACE;
            packed.occlusion = source(layer.occlusionTexture);
            packed.roughness = source(layer.roughnessTexture);
            packed.metallic = source(layer.metallicTexture);
            packed.height = source(layer.heightTexture);
        }
    }

    if (width == 0){
        remember(true);
        return NULL;
    }
    if (width > MAX_TERRAIN_DETAIL_SIZE || height > MAX_TERRAIN_DETAIL_SIZE){
        Log::warn("Terrain detail maps are capped at %d px, larger sources are downscaled", MAX_TERRAIN_DETAIL_SIZE);
        width = std::min(width, MAX_TERRAIN_DETAIL_SIZE);
        height = std::min(height, MAX_TERRAIN_DETAIL_SIZE);
    }

    for (auto& entry : sources){
        // A map that failed to load is kept so it is not retried, but it has nothing to resize
        if (!entry.second.getData()){
            continue;
        }
        if (entry.second.getWidth() != width || entry.second.getHeight() != height){
            entry.second.resize(width, height);
        }
    }

    // Slices share one format, and the packed ones need four channels anyway
    const size_t texels = (size_t)width * height;
    std::vector<std::vector<unsigned char>> slices(sliceKeys.size());
    std::vector<void*> data(sliceKeys.size());
    std::vector<size_t> size(sliceKeys.size());
    for (size_t s = 0; s < sliceKeys.size(); s++){
        const SliceSources& src = sliceSources[s];
        // An unassigned map falls back to the neutral value of its channel: white for
        // color and surface, and a flat normal that leaves the geometry alone
        slices[s].resize(texels * 4, 0xFF);
        for (size_t t = 0; t < texels; t++){
            unsigned char* texel = &slices[s][t * 4];
            if (src.kind == SliceSources::SLICE_NORMAL){
                texel[0] = terrainSourceChannel(src.normal, t, 0, 0x80);
                texel[1] = terrainSourceChannel(src.normal, t, 1, 0x80);
                texel[2] = terrainSourceChannel(src.normal, t, 2, 0xFF);
            }else if (src.kind == SliceSources::SLICE_SURFACE){
                texel[0] = terrainSourceChannel(src.occlusion, t, 0, 0xFF);
                texel[1] = terrainSourceChannel(src.roughness, t, 1, 0xFF);
                texel[2] = terrainSourceChannel(src.metallic, t, 2, 0xFF);
                texel[3] = terrainSourceChannel(src.height, t, 0, 0xFF);
            }else{
                texel[0] = terrainSourceChannel(src.color, t, 0, 0xFF);
                texel[1] = terrainSourceChannel(src.color, t, 1, 0xFF);
                texel[2] = terrainSourceChannel(src.color, t, 2, 0xFF);
                texel[3] = terrainSourceChannel(src.color, t, 3, 0xFF);
            }
        }
        data[s] = slices[s].data();
        size[s] = slices[s].size();
    }

    TerrainDetailArray& detail = remember(false);
    if (!detail.render.createTexture("terrain|detail|" + std::to_string(entity), width, height,
            ColorFormat::RGBA, TextureType::TEXTURE_ARRAY, (int)sliceKeys.size(), data.data(), size.data(),
            minFilter, sampled->getMagFilter(), sampled->getWrapU(), sampled->getWrapV())){
        detail.failed = true;
        return NULL;
    }

    return &detail;
}

void RenderSystem::destroyTerrainDetailArray(Entity entity){
    auto it = terrainDetailArrays.find(entity);
    if (it != terrainDetailArrays.end()){
        it->second.render.destroyTexture();
        terrainDetailArrays.erase(it);
    }
}

// Factors change without rebuilding the array, so the block is filled at draw time
void RenderSystem::applyTerrainLayersUniform(ObjectRender& render, int slot, Entity entity, TerrainComponent& terrain){
    if (slot == -1){
        return;
    }

    auto it = terrainDetailArrays.find(entity);
    const TerrainDetailArray* detail = (it != terrainDetailArrays.end() && !it->second.failed) ? &it->second : NULL;

    static const TerrainSurfaceLayer emptyLayer;

    fs_terrain_layers_t layers;
    for (int i = 0; i < MAX_TERRAIN_LAYERS; i++){
        const bool present = i < (int)terrain.surfaceLayers.size();
        const TerrainSurfaceLayer& layer = present ? terrain.surfaceLayers[i] : emptyLayer;
        const bool pbr = present && layer.pbr;

        layers.colorFactor[i] = Vector4(layer.colorFactor.x, layer.colorFactor.y, layer.colorFactor.z, pbr ? 1.0f : 0.0f);
        layers.uvTransform[i] = Vector4(layer.uvScale.x, layer.uvScale.y, layer.uvOffset.x, layer.uvOffset.y);
        layers.surface[i] = Vector4(layer.roughnessFactor, layer.metallicFactor, layer.normalStrength, layer.occlusionStrength);
        layers.slices[i] = Vector4((float)i,
                                   detail ? (float)detail->normalSlice[i] : -1.0f,
                                   detail ? (float)detail->surfaceSlice[i] : -1.0f, 0.0f);
    }

    render.applyUniformBlock(slot, sizeof(fs_terrain_layers_t), &layers);
}

bool RenderSystem::loadTerrainTextures(Entity entity, TerrainComponent& terrain, ObjectRender& render, ShaderData& shaderData){
    TextureRender* textureRender = NULL;
    std::pair<int, int> slotTex(-1, -1);

    // The heightmap and the blend map span the whole 0..1 range, so their borders must read
    // the border texels, not the REPEAT blend with the opposite edge (detail maps are tiled).
    terrain.heightMap.setWrapU(TextureWrap::CLAMP_TO_EDGE);
    terrain.heightMap.setWrapV(TextureWrap::CLAMP_TO_EDGE);
    for (Texture& blendMap : terrain.blendMaps){
        blendMap.setWrapU(TextureWrap::CLAMP_TO_EDGE);
        blendMap.setWrapV(TextureWrap::CLAMP_TO_EDGE);
    }

    textureRender = terrain.heightMap.getRender(&emptyWhite);
    slotTex = shaderData.getTextureIndex(TextureShaderType::HEIGHTMAP);
    if (textureRender){
        if (!textureRender->isCreated()){
            return false;
        }
        render.addTexture(slotTex, ShaderStageType::VERTEX, textureRender);
    }else{
        render.addTexture(slotTex, ShaderStageType::VERTEX, &emptyWhite);
    }

    // The array comes first: how deep it came out is what decides which blend maps may
    // weight anything, and the white fallback is a single slice.
    TerrainDetailArray* detail = getTerrainDetailArray(entity, terrain);
    const bool arrayCreated = detail && detail->render.isCreated();
    slotTex = shaderData.getTextureIndex(TextureShaderType::TERRAINDETAIL);
    render.addTexture(slotTex, ShaderStageType::FRAGMENT, arrayCreated ? &detail->render : &emptyArrayWhite);

    // The rest bind black, which weights none of their layers
    const int coveredMaps = arrayCreated ? detail->colorSlices / 3 : 0;
    for (int m = 0; m < MAX_TERRAIN_BLENDMAPS; m++){
        const bool covered = m < coveredMaps && m < static_cast<int>(terrain.blendMaps.size());
        textureRender = covered ? terrain.blendMaps[m].getRender(&emptyBlack) : NULL;
        slotTex = shaderData.getTextureIndex(static_cast<TextureShaderType>(static_cast<int>(TextureShaderType::BLENDMAP) + m));
        if (textureRender){
            if (!textureRender->isCreated()){
                return false;
            }
            render.addTexture(slotTex, ShaderStageType::FRAGMENT, textureRender);
        }else{
            render.addTexture(slotTex, ShaderStageType::FRAGMENT, &emptyBlack);
        }
    }

    return true;
}

bool RenderSystem::loadTerrainHeightTexture(TerrainComponent& terrain, ObjectRender& render, ShaderData& shaderData){
    std::pair<int, int> slotTex = shaderData.getTextureIndex(TextureShaderType::HEIGHTMAP);
    if (slotTex.first == -1){
        return true;
    }

    TextureRender* textureRender = terrain.heightMap.getRender(&emptyWhite);
    if (!textureRender || !textureRender->isCreated()){
        return false;
    }

    render.addTexture(slotTex, ShaderStageType::VERTEX, textureRender);
    return true;
}

bool RenderSystem::updateTerrainRenderTextures(Entity entity, TerrainComponent& terrain, MeshComponent& mesh){
    if (!terrain.needUpdateTexture){
        return true;
    }

    bool texLoaded = true;
    for (int s = 0; s < 2; s++){
        if (s >= mesh.numSubmeshes){
            break;
        }
        if (!mesh.submeshes[s].shader){
            texLoaded = false;
            continue;
        }
        ShaderData& shaderData = mesh.submeshes[s].shader.get()->shaderData;
        if (!loadTerrainTextures(entity, terrain, mesh.submeshes[s].render, shaderData)){
            texLoaded = false;
        }
        if (mesh.submeshes[s].depthShader){
            ShaderData& depthShaderData = mesh.submeshes[s].depthShader.get()->shaderData;
            if (!loadTerrainHeightTexture(terrain, mesh.submeshes[s].depthRender, depthShaderData)){
                texLoaded = false;
            }
        }
        if (mesh.submeshes[s].gbufferShader){
            ShaderData& gbufferShaderData = mesh.submeshes[s].gbufferShader.get()->shaderData;
            const bool painted = mesh.submeshes[s].slotFSGBufferTerrainLayers != -1;
            if (painted ? !loadTerrainTextures(entity, terrain, mesh.submeshes[s].gbufferRender, gbufferShaderData)
                        : !loadTerrainHeightTexture(terrain, mesh.submeshes[s].gbufferRender, gbufferShaderData)){
                texLoaded = false;
            }
        }
    }

    if (texLoaded){
        terrain.needUpdateTexture = false;
    }

    return texLoaded;
}

void RenderSystem::updateAllTerrainRenderTextures(){
    auto meshes = scene->getComponentArray<MeshComponent>();
    for (int i = 0; i < meshes->size(); i++){
        MeshComponent& mesh = meshes->getComponentFromIndex(i);
        if (!mesh.loaded || mesh.needReload){
            continue;
        }

        Entity entity = meshes->getEntity(i);
        TerrainComponent* terrain = scene->findComponent<TerrainComponent>(entity);
        if (terrain){
            updateTerrainRenderTextures(entity, *terrain, mesh);
        }
    }
}

void RenderSystem::setDisableFaceCulling(bool disableFaceCulling){
    // the camera passes pick no-culling pipeline variants at draw time (editor-only debug
    // view), so nothing reloads
    this->disableFaceCulling = disableFaceCulling;
}

void RenderSystem::setDisableFog(bool disableFog){
    primaryView.disableFog = disableFog;
}

void RenderSystem::setDisableClouds(bool disableClouds){
    primaryView.disableClouds = disableClouds;
}

void RenderSystem::setPreviewAtmosphereQuality(int quality){
    primaryView.atmosphereQuality = quality;
}

const AtmosphereInfo& RenderSystem::getAtmosphereInfo(uint32_t viewId){
#ifdef JASPERENGINE_EDITOR
    if (viewId != 0){
        // the newer of the view's own frames and the frames the view was drawn as the frame's
        const AtmosphereInfo* newest = nullptr;
        auto view = secondaryViews.find(viewId);
        if (view != secondaryViews.end())
            newest = &view->second->atmosphere.info;
        auto kept = keptAtmosphereInfo.find(viewId);
        if (kept != keptAtmosphereInfo.end() && (!newest || kept->second.frame > newest->frame))
            newest = &kept->second;
        if (newest)
            return *newest;
    }
#endif
    return primaryView.atmosphere.info;
}

#ifdef JASPERENGINE_EDITOR
void RenderSystem::keepAtmosphereInfo(uint32_t viewId){
    keptAtmosphereInfo[viewId] = primaryView.atmosphere.info;
}
#endif

void RenderSystem::setDisablePostProcessing(bool disablePostProcessing){
    primaryView.postProcessing = !disablePostProcessing;
}

void RenderSystem::setDisableFixedResolution(bool disableFixedResolution){
    // No reload needed: the editor always renders through Engine::getFramebuffer(),
    // so the PIP_RTT pipeline is already baked whether fixed resolution is on or off.
    this->disableFixedResolution = disableFixedResolution;
}

bool RenderSystem::loadMesh(Entity entity, MeshComponent& mesh, uint16_t pipelines, InstancedMeshComponent* instmesh, TerrainComponent* terrain){

    if (!Engine::isViewLoaded()) 
        return false;

    if (terrain && !terrain->heightMapLoaded)
        return false;

    if (terrain && instmesh){
        Log::warn("Terrain cannot be an InstancedMesh");
    }

    if (mesh.autoTransparency){
        // Recompute from every current submesh below. This also clears a stale
        // transparent flag when a model is reloaded with OPAQUE/MASK materials.
        mesh.transparent = false;
    }

    std::map<std::string, Buffer*> buffers;
    std::map<std::string, BufferRender*> bufferNameToRender;
    bool allBuffersEmpty = true;

    if (mesh.buffer.getSize() > 0){
        buffers["vertices"] = &mesh.buffer;
        allBuffersEmpty = false;
        if (mesh.buffer.getUsage() != BufferUsage::IMMUTABLE){
            mesh.needUpdateBuffer = true;
        }
    }
    if (mesh.indices.getSize() > 0){
        buffers["indices"] = &mesh.indices;
        allBuffersEmpty = false;
        if (mesh.indices.getUsage() != BufferUsage::IMMUTABLE){
            mesh.needUpdateBuffer = true;
        }
    }
    for (int i = 0; i < mesh.numExternalBuffers; i++){
        buffers[mesh.eBuffers[i].getName()] = &mesh.eBuffers[i];
        allBuffersEmpty = false;
        if (mesh.eBuffers[i].getUsage() != BufferUsage::IMMUTABLE){
            mesh.needUpdateBuffer = true;
        }
    }

    if (mesh.vertexCount == 0){
        mesh.vertexCount = mesh.buffer.getCount();
    }

    if (allBuffersEmpty) {
        // Multi-node glTF models use their root entity only as a hierarchy
        // container; renderable geometry lives in child MeshComponents. Treat
        // that empty root as successfully loaded so one-time preview scenes can
        // complete instead of waiting forever in RenderSystem::isAllLoaded().
        ModelComponent* model = scene->findComponent<ModelComponent>(entity);
        if (model && !model->meshNodesMapping.empty() && mesh.numSubmeshes == 0) {
            mesh.needReload = false;
            mesh.needUpdateAABB = true;
            mesh.loadCalled = true;
            SystemRender::addQueueCommand(&changeLoaded, new check_load_t{scene, entity});
            return true;
        }
        return false;
    }

    std::map<std::string, unsigned int> bufferStride;

    std::vector<BufferRender*> buffersCreatedThisLoad;
    for (auto const& buf : buffers){
        BufferRender* bufferRender = buf.second->getRender();

        // A GPU buffer keeps for life the usage it was created with, so a generator
        // that turns its buffer dynamic after the mesh was first loaded from
        // serialized data (a tilemap rebuilding its indices) needs the buffer
        // recreated — updating an immutable one is rejected by the backend.
        if (bufferRender->isCreated() && bufferRender->getCreatedUsage() != buf.second->getUsage()) {
            bufferRender->destroyBuffer();
        }

        if (!bufferRender->isCreated()) {
            if (!bufferRender->createBuffer(buf.second->getSize(), buf.second->getData(), buf.second->getType(), buf.second->getUsage())) {
                Log::error("Cannot create GPU buffer '%s' (%zu bytes) for mesh entity %u; resource pool may be exhausted",
                           buf.first.c_str(), buf.second->getSize(), entity);
                for (BufferRender* created : buffersCreatedThisLoad) {
                    created->destroyBuffer();
                }
                return false;
            }
            buffersCreatedThisLoad.push_back(bufferRender);
        }
        bufferNameToRender[buf.first] = bufferRender;
        bufferStride[buf.first] = buf.second->getStride();
    }

    if (instmesh){
        // Now buffer size is zero than it needed to be calculated
        size_t bufferSize = instmesh->maxInstances * instmesh->buffer.getStride();
        instmesh->buffer.getRender()->createBuffer(bufferSize, instmesh->buffer.getData(), instmesh->buffer.getType(), instmesh->buffer.getUsage());

        instmesh->needUpdateBuffer = true;
    }

    if (terrain){
        for (int v = 0; v < MAX_TERRAIN_VIEWS; v++){
            for (int s = 0; s < 2; s++){
                size_t bufferSize = terrain->nodes.size() * terrain->views[v].nodesbuffer[s].getStride();
                terrain->views[v].nodesbuffer[s].getRender()->createBuffer(bufferSize, terrain->views[v].nodesbuffer[s].getData(), terrain->views[v].nodesbuffer[s].getType(), terrain->views[v].nodesbuffer[s].getUsage());
            }
        }

        // using same material in both terrain submeshes
        mesh.submeshes[1].material = mesh.submeshes[0].material;
    }

    // sprites, tilemaps and polygons draw images with transparent borders
    Signature signature = scene->getSignature(entity);
    if (signature.test(scene->getComponentId<SpriteComponent>()) || signature.test(scene->getComponentId<TilemapComponent>()) || signature.test(scene->getComponentId<MeshPolygonComponent>())){
        for (int i = 0; i < mesh.numSubmeshes; i++){
            mesh.submeshes[i].material.baseColorTexture.setAlphaBorderAuto(true);
        }
    }

    const MeshShaderEnvironment shaderEnvironment = getMeshShaderEnvironment();
    const bool hasMirror = (scene->findComponent<MirrorComponent>(entity) != NULL);
    const std::string& sceneMeshShader = scene->getDefaultCustomShader(ShaderType::MESH);

    // recomputed from every submesh below
    mesh.transparentSortPriority = 0;
    mesh.opaqueSortPriority = 0;
    mesh.skipsDepthPrepass = false;
    bool hasTransparentSubmesh = false;
    bool hasOpaqueSubmesh = false;

    std::vector<const Buffer*> attributeBuffers;
    for (auto const& buf : buffers){
        attributeBuffers.push_back(buf.second);
    }

    // the generated a_smoothNormal buffers this load binds (a copy of a component shares
    // them until it loads: it makes its own, as it does its renders)
    if (mesh.smoothNormals){
        if (mesh.smoothNormals.use_count() > 1){
            auto own = std::make_shared<MeshSmoothNormals>();
            for (const MeshSmoothNormals::Entry& entry : mesh.smoothNormals->entries){
                own->entries.emplace_back();
                own->entries.back().key = entry.key;
                own->entries.back().data = entry.data;
            }
            mesh.smoothNormals = own;
        }
        for (MeshSmoothNormals::Entry& entry : mesh.smoothNormals->entries)
            entry.used = false;
    }

    // Every submesh's shaders first, before any pipeline is made. A load that waits for one
    // returns false and is tried again, so a later submesh's shader still building must stop
    // the load before an earlier submesh makes pipelines. All of them are asked for, so
    // they build together.
    struct SubmeshLoad {
        SubmeshShaderInputs shaderInputs;
        SubmeshShaderVariant variant;
        std::string meshShaderSrc;
        std::string depthShaderSrc;
    };
    std::vector<SubmeshLoad> submeshLoads(mesh.numSubmeshes);
    bool shadersReady = true;

    for (int i = 0; i < mesh.numSubmeshes; i++){

        // The vertex attribute flags come from the current buffers and attributes
        scanSubmeshAttributes(mesh.submeshes[i], attributeBuffers);

        submeshLoads[i].shaderInputs = getSubmeshShaderInputs(mesh, mesh.submeshes[i], terrain, instmesh, hasMirror, sceneMeshShader);
        submeshLoads[i].variant = getSubmeshShaderVariant(shaderEnvironment, submeshLoads[i].shaderInputs);
        const SubmeshShaderInputs& shaderInputs = submeshLoads[i].shaderInputs;
        const SubmeshShaderVariant& variant = submeshLoads[i].variant;
        mesh.submeshes[i].manifest = shaderInputs.manifest;
        // its @state with the rules of the features the material enables
        mesh.submeshes[i].manifestState = shaderInputs.manifest ? shaderInputs.manifest->resolveState(shaderInputs.features) : ShaderManifestState();
        if (!supportsLighting(shaderInputs.lighting, shaderInputs.manifest.get())){
            warnUnsupportedLighting(mesh.submeshes[i].material, getSubmeshShader(mesh, mesh.submeshes[i], sceneMeshShader),
                                    shaderInputs.manifest.get(), scene->getEntityName(entity), entity, (unsigned int)i);
        }

        // remember whether this submesh's lit shader applies IBL specular, so the SSR
        // G-buffer can flag those pixels for the energy-conserving composite
        // (see ShaderPool::getMeshProperties for the bits)
        mesh.submeshes[i].hasIBL = (variant.properties & (1u << 19)) != 0;

        mesh.submeshes[i].shaderProperties = variant.properties;
        mesh.submeshes[i].loadedLighting = mesh.submeshes[i].material.lighting;
        // a custom main shader (a library shader or a fork) overrides the built-in Mesh
        // shader; the variant (#define) system, depth/gbuffer passes and bind-slots are
        // unchanged. Priority: material shader > component customShader > scene default
        // shader > built-in (empty); the material's features key the variant too.
        submeshLoads[i].meshShaderSrc = getSubmeshShaderReference(mesh, mesh.submeshes[i], sceneMeshShader);
        const std::string& meshShaderSrc = submeshLoads[i].meshShaderSrc;
        mesh.submeshes[i].customShaderId = ShaderPool::registerCustomShader(meshShaderSrc);
        mesh.submeshes[i].shader = ShaderPool::get(ShaderType::MESH, mesh.submeshes[i].shaderProperties, mesh.submeshes[i].customShaderId);
        // the depth shader feeds shadow maps (casters) and the SSAO depth pre-pass
        // (SSR uses its own G-buffer shader, built below). A depth fork is built even
        // without a pass, so its uniforms and build errors show up in the editor.
        // the library shader's depth program, the depth fork, or built-in
        submeshLoads[i].depthShaderSrc = getSubmeshDepthShaderReference(mesh, mesh.submeshes[i], sceneMeshShader);
        const std::string& depthShaderSrc = submeshLoads[i].depthShaderSrc;
        if (variant.depth){
            mesh.submeshes[i].depthShaderProperties = variant.depthProperties;
            mesh.submeshes[i].customDepthShaderId = ShaderPool::registerCustomShader(depthShaderSrc);
            mesh.submeshes[i].depthShader = ShaderPool::get(ShaderType::DEPTH, mesh.submeshes[i].depthShaderProperties, mesh.submeshes[i].customDepthShaderId);
            if (!mesh.submeshes[i].depthShader->isCreated()){
                // same fallback as the main shader below
                if (mesh.submeshes[i].customDepthShaderId != 0 &&
                    ShaderPool::isShaderBuildFailed(ShaderType::DEPTH, mesh.submeshes[i].depthShaderProperties, mesh.submeshes[i].customDepthShaderId)){
                    Log::error("Custom depth shader '%s' failed to compile; rendering '%s' with the built-in shader",
                               depthShaderSrc.c_str(), scene->getEntityName(entity).c_str());
                    mesh.submeshes[i].customDepthShaderId = 0;
                    mesh.submeshes[i].depthShader = ShaderPool::get(ShaderType::DEPTH, mesh.submeshes[i].depthShaderProperties, 0);
                }
            }
            if (mesh.submeshes[i].depthShader->isCreated()){
                // custom blocks of a forked depth shader, sharing the mesh values
                ShaderData& depthShaderData = mesh.submeshes[i].depthShader.get()->shaderData;
                mesh.submeshes[i].customVSDepthParams.resolve(depthShaderData, "u_vs_customParams");
                mesh.submeshes[i].customFSDepthParams.resolve(depthShaderData, "u_fs_customParams");
            }else{
                shadersReady = false;
            }
        }else if (mesh.submeshes[i].depthShader){
            // a reload keeps the handle; release it with the key it was taken with
            mesh.submeshes[i].depthShader.reset();
            ShaderPool::remove(ShaderType::DEPTH, mesh.submeshes[i].depthShaderProperties, mesh.submeshes[i].customDepthShaderId);
            mesh.submeshes[i].customDepthShaderId = 0;
        }
        // the G-buffer shader feeds the SSR geometry pass (MRT: packed depth + view-space
        // normal/roughness/metallic). Built only when SSR is enabled (it needs normals).
        if (variant.gbuffer){
            mesh.submeshes[i].gbufferShaderProperties = variant.gbufferProperties;
            mesh.submeshes[i].gbufferShader = ShaderPool::get(ShaderType::GBUFFER, mesh.submeshes[i].gbufferShaderProperties);
            if (!mesh.submeshes[i].gbufferShader->isCreated())
                shadersReady = false;
        }
        if (!mesh.submeshes[i].shader->isCreated()){
            // A custom shader that failed to compile falls back to the built-in shader so
            // the object stays visible and the mesh finishes loading (otherwise needReload
            // stays set and we retry forever). The fork is retried after its source changes
            // (invalidateCustomShaders clears the failure and re-flags needReload).
            if (mesh.submeshes[i].customShaderId != 0 &&
                ShaderPool::isShaderBuildFailed(ShaderType::MESH, mesh.submeshes[i].shaderProperties, mesh.submeshes[i].customShaderId)){
                Log::error("Custom shader '%s' failed to compile; rendering '%s' with the built-in shader",
                           meshShaderSrc.c_str(), scene->getEntityName(entity).c_str());
                mesh.submeshes[i].customShaderId = 0;
                mesh.submeshes[i].shader = ShaderPool::get(ShaderType::MESH, mesh.submeshes[i].shaderProperties, 0);
            }
            if (!mesh.submeshes[i].shader->isCreated())
                shadersReady = false;
        }
        // the extra pass's shader too (loadSubmeshExtraPass takes it below)
        if (hasExtraPass(mesh.submeshes[i], terrain != nullptr)){
            const SubmeshShaderVariant passVariant = getSubmeshShaderVariant(shaderEnvironment,
                getExtraPassShaderInputs(shaderInputs, mesh.submeshes[i].material.extraPass));
            const uint16_t passShaderId = ShaderPool::registerCustomShader(getSubmeshExtraPassReference(mesh.submeshes[i]));
            if (!ShaderPool::get(ShaderType::MESH, passVariant.properties, passShaderId)->isCreated() &&
                    !ShaderPool::isShaderBuildFailed(ShaderType::MESH, passVariant.properties, passShaderId))
                shadersReady = false;
        }
    }
    if (!shadersReady){
        return false;
    }

    // Then the pipelines. A load failing from here on (a texture still loading, a resource
    // pool full) destroys the pipelines this try made before returning: the next try makes
    // them again, and a mesh destroyed before it loads has nothing to release. Only what
    // this try began (a copy of the component shares its source's pipelines until it loads).
    struct LoadAttempt {
        std::vector<ObjectRender*> renders;
        std::vector<SubmeshExtraPass*> extraPasses;
        bool succeeded = false;
        ~LoadAttempt(){
            if (succeeded)
                return;
            for (ObjectRender* render : renders)
                render->destroy();
            for (SubmeshExtraPass* extra : extraPasses){
                extra->render.destroy();
                extra->loaded = false;
            }
        }
    } attempt;

    // looked up again (bindSmoothNormals): the first submesh to ask sets it on every submesh
    // over the same vertices
    for (int i = 0; i < mesh.numSubmeshes; i++){
        mesh.submeshes[i].smoothNormals = -1;
    }

    for (int i = 0; i < mesh.numSubmeshes; i++){

        ObjectRender& render = mesh.submeshes[i].render;

        render.beginLoad(mesh.submeshes[i].primitiveType);
        attempt.renders.push_back(&render);

        const SubmeshShaderInputs& shaderInputs = submeshLoads[i].shaderInputs;
        const SubmeshShaderVariant& variant = submeshLoads[i].variant;
        const std::string& meshShaderSrc = submeshLoads[i].meshShaderSrc;
        const std::string& depthShaderSrc = submeshLoads[i].depthShaderSrc;
        const bool needDepthPass = variant.depthPass;

        // what the variant enables, from the property bits (see ShaderPool::getMeshProperties)
        const uint32_t meshProperties = variant.properties;
        const bool p_lit = !(meshProperties & (1u << 0));
        const bool p_hasTexture2 = (meshProperties & (1u << 2)) != 0;
        const bool p_receiveShadows = (meshProperties & (1u << 4)) != 0;
        const bool p_ibl = (meshProperties & (1u << 19)) != 0;
        const bool p_mirror = (meshProperties & (1u << 20)) != 0;
        const bool p_light2d = (meshProperties & (1u << 22)) != 0;
        const bool p_shadows2d = (meshProperties & (1u << 23)) != 0;
        const bool p_instanceFade = (meshProperties & (1u << 26)) != 0;
        const bool p_terrainSurface = (meshProperties & (1u << 27)) != 0;
        // see ShaderPool::getDepthMeshProperties
        const bool p_depthTexture = (variant.depthProperties & (1u << 0)) != 0;
        const bool p_depthAlphaMask = (variant.depthProperties & (1u << 7)) != 0;
        render.setShader(mesh.submeshes[i].shader.get());
        ShaderData& shaderData = mesh.submeshes[i].shader.get()->shaderData;

        mesh.submeshes[i].slotVSParams = shaderData.getUniformBlockIndex(UniformBlockType::PBR_VS_PARAMS);
        mesh.submeshes[i].slotFSParams = shaderData.getUniformBlockIndex(UniformBlockType::PBR_FS_PARAMS);
        if (p_hasTexture2){
            mesh.submeshes[i].slotFSTexCoordSets = shaderData.getUniformBlockIndex(UniformBlockType::PBR_FS_TEXCOORDSETS);
        }
        if (p_instanceFade){
            mesh.submeshes[i].slotVSFade = shaderData.getUniformBlockIndex(UniformBlockType::PBR_VS_FADE);
        }
        if (hasFog){
            mesh.submeshes[i].slotFSFog = shaderData.getUniformBlockIndex(UniformBlockType::FS_FOG);
        }
        if (p_mirror){
            mesh.submeshes[i].slotFSMirror = shaderData.getUniformBlockIndex(UniformBlockType::FS_MIRROR);
        }
        if (p_lit){
            // the object's lights (includes/lights.glsl), or the legacy block of an older fork
            // the vertex stage declares a prefix of the fragment stage's block (includes/lights.glsl)
            mesh.submeshes[i].slotVSObjectLights = shaderData.getUniformBlockIndexByName("u_vs_objectLights");
            mesh.submeshes[i].slotFSObjectLights = shaderData.getUniformBlockIndexByName("u_fs_objectLights");
            shaderData.getUniformBlockMembers("u_vs_objectLights", mesh.submeshes[i].sizeVSObjectLights);
            shaderData.getUniformBlockMembers("u_fs_objectLights", mesh.submeshes[i].sizeFSObjectLights);
            mesh.submeshes[i].sizeVSObjectLights = std::min(mesh.submeshes[i].sizeVSObjectLights, (unsigned int)sizeof(object_lights_t));
            mesh.submeshes[i].sizeFSObjectLights = std::min(mesh.submeshes[i].sizeFSObjectLights, (unsigned int)sizeof(object_lights_t));
            // the built-in Lit's camera block (includes/lit_frame.glsl): the prefix it declares
            mesh.submeshes[i].slotFSLitFrame = shaderData.getUniformBlockIndexByName("u_fs_litFrame");
            shaderData.getUniformBlockMembers("u_fs_litFrame", mesh.submeshes[i].sizeFSLitFrame);
            mesh.submeshes[i].sizeFSLitFrame = std::min(mesh.submeshes[i].sizeFSLitFrame, (unsigned int)sizeof(lit_frame_block_t));
            mesh.submeshes[i].slotFSLighting = shaderData.getUniformBlockIndex(UniformBlockType::FS_LIGHTING);
            if (p_ibl){
                mesh.submeshes[i].slotFSReflectionProbe = shaderData.getUniformBlockIndex(UniformBlockType::FS_REFLECTION_PROBE);
            }
            if (p_receiveShadows){
                mesh.submeshes[i].slotVSShadows = shaderData.getUniformBlockIndex(UniformBlockType::VS_SHADOWS);
                mesh.submeshes[i].slotFSShadows = shaderData.getUniformBlockIndex(UniformBlockType::FS_SHADOWS);
                mesh.submeshes[i].slotFSPointShadows = shaderData.getUniformBlockIndex(UniformBlockType::FS_POINT_SHADOWS);
            }
        }
        if (p_light2d){
            mesh.submeshes[i].slotFSLighting2D = shaderData.getUniformBlockIndex(UniformBlockType::FS_LIGHTING2D);
        }
        if (mesh.submeshes[i].hasTextureRect){
            mesh.submeshes[i].slotVSSprite = shaderData.getUniformBlockIndex(UniformBlockType::SPRITE_VS_PARAMS);
        }
        if (mesh.submeshes[i].hasSkinning){
            mesh.submeshes[i].slotVSSkinning = shaderData.getUniformBlockIndex(UniformBlockType::VS_SKINNING);
            if (!bindSkinningResource(render, shaderData, mesh))
                return false;
        }
        if (mesh.submeshes[i].hasMorphTarget){
            mesh.submeshes[i].slotVSMorphTarget = shaderData.getUniformBlockIndex(UniformBlockType::VS_MORPHTARGET);
        }
        // custom blocks of a forked shader (none in the built-in), with the depth shader's:
        // the material's parameters, the mesh's shader uniforms, the manifest defaults
        mesh.submeshes[i].customVSParams.resolve(shaderData, "u_vs_customParams");
        mesh.submeshes[i].customFSParams.resolve(shaderData, "u_fs_customParams");
        writeSubmeshCustomUniforms(mesh, mesh.submeshes[i]);

        if (!loadPBRTextures(mesh.submeshes[i].material, shaderData, mesh.submeshes[i].render, mesh.receiveLights)){
            return false;
        }
        // shader library: the frame block and the material's custom textures
        if (!loadShaderLibraryBindings(mesh.submeshes[i].libraryBindings, shaderData, mesh.submeshes[i].render,
                                       mesh.submeshes[i].material.shaderTextures, mesh.submeshes[i].manifest.get())){
            return false;
        }
        loadShadowTextures(shaderData, mesh.submeshes[i].render, mesh.receiveLights, p_receiveShadows);
        loadShadow2DTexture(shaderData, mesh.submeshes[i].render, p_shadows2d);

        if (p_ibl){
            auto skyArray = scene->getComponentArray<SkyboxComponent>();
            TextureRender* irradiance = &emptyCubeBlack;
            TextureRender* prefiltered = &emptyCubeBlack;
            if (skyArray->size() > 0){
                SkyboxComponent& sky = skyArray->getComponentFromIndex(0);
                if (sky.irradianceMap && sky.prefilteredMap){
                    irradiance = sky.irradianceMap.get();
                    prefiltered = sky.prefilteredMap.get();
                }
            }
            render.addTexture(shaderData.getTextureIndex(TextureShaderType::IRRADIANCEMAP), ShaderStageType::FRAGMENT, irradiance);
            render.addTexture(shaderData.getTextureIndex(TextureShaderType::PREFILTEREDMAP), ShaderStageType::FRAGMENT, prefiltered);
            render.addTexture(shaderData.getTextureIndex(TextureShaderType::REFLECTIONPROBE), ShaderStageType::FRAGMENT, &emptyCubeBlack);
        }

        if (terrain){
            mesh.submeshes[i].slotVSTerrain = shaderData.getUniformBlockIndex(UniformBlockType::TERRAIN_VS_PARAMS);
            if (p_terrainSurface){
                mesh.submeshes[i].slotFSTerrainLayers = shaderData.getUniformBlockIndex(UniformBlockType::FS_TERRAIN_LAYERS);
            }

            if (!loadTerrainTextures(entity, *terrain, mesh.submeshes[i].render, shaderData)){
                return false;
            }

            terrain->needUpdateTexture = false;

            // bind view 0 as the load-time default; per-view buffers (same layout) are
            // swapped in at draw via ObjectRender::replaceVertexBuffer
            for (auto const &attr : terrain->views[0].nodesbuffer[i].getAttributes()) {
                render.addAttribute(shaderData.getAttrIndex(attr.first), terrain->views[0].nodesbuffer[i].getRender(), attr.second.getElements(), attr.second.getDataType(), terrain->views[0].nodesbuffer[i].getStride(), attr.second.getOffset(), attr.second.getNormalized(), attr.second.getPerInstance());
            }
        }

        mesh.submeshes[i].needUpdateTexture = false;
        mesh.submeshes[i].needUpdateDepthTexture = false;
        mesh.submeshes[i].needUpdateGBufferTexture = false;

        // render state: the material's, the manifest's @state, else the legacy state
        // (alpha blended materials go to the transparent queue and write no depth)
        bool blended = false;
        bool transparentQueue = false;
        const ShaderManifestState* manifestState = mesh.submeshes[i].manifest ? &mesh.submeshes[i].manifestState : nullptr;
        const RenderState colorState = resolveSubmeshRenderState(mesh, mesh.submeshes[i], manifestState, blended, transparentQueue);
        mesh.submeshes[i].skipsScreenEffects = colorState.colorWriteMask == 0 && !colorState.depthWrite;
        mesh.submeshes[i].alphaBlend = transparentQueue;
        if (mesh.autoTransparency && !mesh.transparent && transparentQueue){
            mesh.transparent = true;
        }
        // the material's sort priority plus the shader's @state priority= offset
        const int priority = mesh.submeshes[i].material.sortPriority + (manifestState ? manifestState->priority : 0);
        if (transparentQueue){
            mesh.transparentSortPriority = hasTransparentSubmesh ? std::min(mesh.transparentSortPriority, priority) : priority;
            hasTransparentSubmesh = true;
        }else{
            mesh.opaqueSortPriority = hasOpaqueSubmesh ? std::min(mesh.opaqueSortPriority, priority) : priority;
            hasOpaqueSubmesh = true;
        }
        // the camera depth pre-pass leaves out a forked color shader without a depth
        // program (its vertices may not match), and what writes no depth
        const bool forkedColor = mesh.submeshes[i].customShaderId != 0 && !ShaderReference::isLibrary(meshShaderSrc);
        const bool explicitDepthWrite = resolveState(mesh.submeshes[i].material.depthWrite,
            manifestState ? manifestState->depthWrite : MaterialDepthWrite::AUTO) != MaterialDepthWrite::AUTO;
        if ((forkedColor && depthShaderSrc.empty()) || (explicitDepthWrite && !colorState.depthWrite)){
            mesh.skipsDepthPrepass = true;
        }
        // and a colour pass that fails its own pre-pass depth (a strict or inverted test:
        // less, greater, not equal), which would draw nothing
        if (colorState.depthCompare != DepthCompare::LESS_EQUAL && colorState.depthCompare != DepthCompare::EQUAL &&
                colorState.depthCompare != DepthCompare::GREATER_EQUAL && colorState.depthCompare != DepthCompare::ALWAYS){
            mesh.skipsDepthPrepass = true;
        }
        // and a stencil test: the pre-pass would write depth where the stencil hides the
        // colour (a writer that always passes keeps its depth; it writes stencil when its
        // colour pass draws, at the depth the pre-pass left)
        mesh.submeshes[i].stencilTested = colorState.stencilEnabled && colorState.stencilCompare != StencilCompare::ALWAYS;
        if (mesh.submeshes[i].stencilTested){
            mesh.skipsDepthPrepass = true;
        }
    
        unsigned int indexCount = 0;
        // an indexed submesh draws indexCount elements even when that count is zero;
        // only a submesh without any index data falls back to the whole vertex buffer
        bool indexed = false;

        for (auto const& buf : buffers){
            if (buf.second->isRenderAttributes()) {
                if (buf.second->getType() == BufferType::INDEX_BUFFER){
                    indexed = true;
                    indexCount = buf.second->getCount();
                    Attribute indexattr = buf.second->getAttributes()[AttributeType::INDEX];
                    render.setIndex(buf.second->getRender(), indexattr.getDataType(), indexattr.getOffset());
                }else{
                    for (auto const &attr : buf.second->getAttributes()) {
                        render.addAttribute(shaderData.getAttrIndex(attr.first), buf.second->getRender(), attr.second.getElements(), attr.second.getDataType(), buf.second->getStride(), attr.second.getOffset(), attr.second.getNormalized(), attr.second.getPerInstance());
                    }
                }
            }
        }

        for (auto const& attr : mesh.submeshes[i].attributes){
            if (bufferNameToRender.count(attr.second.getBufferName())){
                if (attr.first == AttributeType::INDEX){
                    indexed = true;
                    indexCount = attr.second.getCount();
                    render.setIndex(bufferNameToRender[attr.second.getBufferName()], attr.second.getDataType(), attr.second.getOffset());
                }else{
                    render.addAttribute(shaderData.getAttrIndex(attr.first), bufferNameToRender[attr.second.getBufferName()], attr.second.getElements(), attr.second.getDataType(), bufferStride[attr.second.getBufferName()], attr.second.getOffset(), attr.second.getNormalized(), attr.second.getPerInstance());
                }
            }else{
                Log::error("Cannot load submesh attribute from buffer name: %s", attr.second.getBufferName().c_str());
            }
        }

        if (instmesh){
            for (auto const &attr : instmesh->buffer.getAttributes()) {
                render.addAttribute(shaderData.getAttrIndex(attr.first), instmesh->buffer.getRender(), attr.second.getElements(), attr.second.getDataType(), instmesh->buffer.getStride(), attr.second.getOffset(), attr.second.getNormalized(), attr.second.getPerInstance());
            }
        }

        if (indexed){
            // An empty indexed submesh (a tilemap rect with no tile placed on it)
            // must draw nothing: falling back to the mesh vertex count would read
            // indices belonging to the other submeshes, and past the index buffer.
            mesh.submeshes[i].vertexCount = indexCount;
        }else{
            mesh.submeshes[i].vertexCount = mesh.vertexCount;
        }

        // a_smoothNormal, when the shader asks for it (@require smoothnormals)
        if (!bindSmoothNormals(render, shaderData, mesh, mesh.submeshes[i], buffers)){
            return false;
        }

        mesh.submeshes[i].libraryBindings.postColorBlend = postColorBlendOf(colorState, mesh.submeshes[i].manifest != nullptr);
        if (!render.endLoad(pipelines, colorState, msaaPipelineSamples)){
            return false;
        }

        //----------Start depth shader---------------
        if (needDepthPass){
            ObjectRender& depthRender = mesh.submeshes[i].depthRender;

            depthRender.beginLoad(mesh.submeshes[i].primitiveType);
            attempt.renders.push_back(&depthRender);

            depthRender.setShader(mesh.submeshes[i].depthShader.get());
            ShaderData& depthShaderData = mesh.submeshes[i].depthShader.get()->shaderData;

            mesh.submeshes[i].slotVSDepthParams = depthShaderData.getUniformBlockIndex(UniformBlockType::DEPTH_VS_PARAMS);
            depthShaderData.getUniformBlockMembers("u_vs_depthParams", mesh.submeshes[i].sizeVSDepthParams);
            if (p_depthAlphaMask){
                mesh.submeshes[i].slotFSDepthMaterial = depthShaderData.getUniformBlockIndex(UniformBlockType::DEPTH_FS_MATERIAL);
            }
            if (p_instanceFade){
                mesh.submeshes[i].slotVSDepthFade = depthShaderData.getUniformBlockIndex(UniformBlockType::PBR_VS_FADE);
            }
            // the sprite rect an alpha-clipping depth program's UV goes through ('Txr')
            if (mesh.submeshes[i].hasTextureRect){
                mesh.submeshes[i].slotVSDepthSprite = depthShaderData.getUniformBlockIndex(UniformBlockType::SPRITE_VS_PARAMS);
            }

            if (mesh.submeshes[i].hasSkinning){
                mesh.submeshes[i].slotVSDepthSkinning = depthShaderData.getUniformBlockIndex(UniformBlockType::DEPTH_VS_SKINNING);
                if (!bindSkinningResource(depthRender, depthShaderData, mesh))
                    return false;
            }
            if (mesh.submeshes[i].hasMorphTarget){
                mesh.submeshes[i].slotVSDepthMorphTarget = depthShaderData.getUniformBlockIndex(UniformBlockType::DEPTH_VS_MORPHTARGET);
            }

            if (p_depthTexture){
                if (!loadDepthTexture(mesh.submeshes[i].material, depthShaderData, depthRender)){
                    return false;
                }
            }
            if (!loadShaderLibraryBindings(mesh.submeshes[i].depthLibraryBindings, depthShaderData, depthRender,
                                           mesh.submeshes[i].material.shaderTextures, mesh.submeshes[i].manifest.get())){
                return false;
            }
            // a custom depth program (library @depth or a fork) gets every attribute it
            // declares; the built-in one only position, UV, skin and morph targets
            const bool allDepthAttributes = mesh.submeshes[i].customDepthShaderId != 0;

            if (terrain){
                mesh.submeshes[i].slotVSDepthTerrain = depthShaderData.getUniformBlockIndex(UniformBlockType::DEPTH_TERRAIN_VS_PARAMS);

                if (!loadTerrainHeightTexture(*terrain, depthRender, depthShaderData)){
                    return false;
                }

                // shadow depth pass always renders the main camera's selection (view 0)
                for (auto const &attr : terrain->views[0].nodesbuffer[i].getAttributes()) {
                    depthRender.addAttribute(depthShaderData.getAttrIndex(attr.first), terrain->views[0].nodesbuffer[i].getRender(), attr.second.getElements(), attr.second.getDataType(), terrain->views[0].nodesbuffer[i].getStride(), attr.second.getOffset(), attr.second.getNormalized(), attr.second.getPerInstance());
                }
            }

            for (auto const& buf : buffers){
                if (buf.second->isRenderAttributes()) {

                    if (buf.second->getType() == BufferType::INDEX_BUFFER){
                        indexCount = buf.second->getCount();
                        Attribute indexattr = buf.second->getAttributes()[AttributeType::INDEX];
                        depthRender.setIndex(buf.second->getRender(), indexattr.getDataType(), indexattr.getOffset());
                    }else{
                        for (auto const &attr : buf.second->getAttributes()){
                            if (allDepthAttributes){
                                depthRender.addAttribute(depthShaderData.getAttrIndex(attr.first), buf.second->getRender(), attr.second.getElements(), attr.second.getDataType(), buf.second->getStride(), attr.second.getOffset(), attr.second.getNormalized(), attr.second.getPerInstance());
                                continue;
                            }
                            if (attr.first == AttributeType::POSITION || attr.first == AttributeType::TEXCOORD1){
                                depthRender.addAttribute(depthShaderData.getAttrIndex(attr.first), buf.second->getRender(), attr.second.getElements(), attr.second.getDataType(), buf.second->getStride(), attr.second.getOffset(), attr.second.getNormalized(), attr.second.getPerInstance());
                            }
                            if (mesh.submeshes[i].hasSkinning){
                                if (attr.first == AttributeType::BONEIDS || attr.first == AttributeType::BONEWEIGHTS){
                                    depthRender.addAttribute(depthShaderData.getAttrIndex(attr.first), buf.second->getRender(), attr.second.getElements(), attr.second.getDataType(), buf.second->getStride(), attr.second.getOffset(), attr.second.getNormalized(), attr.second.getPerInstance());
                                }
                            }
                            if (mesh.submeshes[i].hasMorphTarget){
                                if (attr.first == AttributeType::MORPHTARGET0 || attr.first == AttributeType::MORPHTARGET1 ||
                                    attr.first == AttributeType::MORPHTARGET2 || attr.first == AttributeType::MORPHTARGET3 ||
                                    attr.first == AttributeType::MORPHTARGET4 || attr.first == AttributeType::MORPHTARGET5 ||
                                    attr.first == AttributeType::MORPHTARGET6 || attr.first == AttributeType::MORPHTARGET7 ||
                                    attr.first == AttributeType::MORPHNORMAL0 || attr.first == AttributeType::MORPHNORMAL1 ||
                                    attr.first == AttributeType::MORPHNORMAL2 || attr.first == AttributeType::MORPHNORMAL3 ||
                                    attr.first == AttributeType::MORPHTANGENT0 || attr.first == AttributeType::MORPHTANGENT1){
                                    depthRender.addAttribute(depthShaderData.getAttrIndex(attr.first), buf.second->getRender(), attr.second.getElements(), attr.second.getDataType(), buf.second->getStride(), attr.second.getOffset(), attr.second.getNormalized(), attr.second.getPerInstance());
                                }
                            }
                        }
                    }
                }
            }

            for (auto const& attr : mesh.submeshes[i].attributes){
                if (bufferNameToRender.count(attr.second.getBufferName())){
                    if (attr.first == AttributeType::INDEX){
                        depthRender.setIndex(bufferNameToRender[attr.second.getBufferName()], attr.second.getDataType(), attr.second.getOffset());
                    }else if (allDepthAttributes){
                        depthRender.addAttribute(depthShaderData.getAttrIndex(attr.first), bufferNameToRender[attr.second.getBufferName()], attr.second.getElements(), attr.second.getDataType(), bufferStride[attr.second.getBufferName()], attr.second.getOffset(), attr.second.getNormalized(), attr.second.getPerInstance());
                        continue;
                    }else if (attr.first == AttributeType::POSITION || attr.first == AttributeType::TEXCOORD1){
                        depthRender.addAttribute(depthShaderData.getAttrIndex(attr.first), bufferNameToRender[attr.second.getBufferName()], attr.second.getElements(), attr.second.getDataType(), bufferStride[attr.second.getBufferName()], attr.second.getOffset(), attr.second.getNormalized(), attr.second.getPerInstance());
                    }
                    if (mesh.submeshes[i].hasSkinning){
                        if (attr.first == AttributeType::BONEIDS || attr.first == AttributeType::BONEWEIGHTS){
                            depthRender.addAttribute(depthShaderData.getAttrIndex(attr.first), bufferNameToRender[attr.second.getBufferName()], attr.second.getElements(), attr.second.getDataType(), bufferStride[attr.second.getBufferName()], attr.second.getOffset(), attr.second.getNormalized(), attr.second.getPerInstance());
                        }
                    }
                    if (mesh.submeshes[i].hasMorphTarget){
                        if (attr.first == AttributeType::MORPHTARGET0 || attr.first == AttributeType::MORPHTARGET1 ||
                            attr.first == AttributeType::MORPHTARGET2 || attr.first == AttributeType::MORPHTARGET3 ||
                            attr.first == AttributeType::MORPHTARGET4 || attr.first == AttributeType::MORPHTARGET5 ||
                            attr.first == AttributeType::MORPHTARGET6 || attr.first == AttributeType::MORPHTARGET7 ||
                            attr.first == AttributeType::MORPHNORMAL0 || attr.first == AttributeType::MORPHNORMAL1 ||
                            attr.first == AttributeType::MORPHNORMAL2 || attr.first == AttributeType::MORPHNORMAL3 ||
                            attr.first == AttributeType::MORPHTANGENT0 || attr.first == AttributeType::MORPHTANGENT1){
                            depthRender.addAttribute(depthShaderData.getAttrIndex(attr.first), bufferNameToRender[attr.second.getBufferName()], attr.second.getElements(), attr.second.getDataType(), bufferStride[attr.second.getBufferName()], attr.second.getOffset(), attr.second.getNormalized(), attr.second.getPerInstance());
                        }
                    }
                }else{
                    Log::error("Cannot load (depth) submesh attribute from buffer name: %s", attr.second.getBufferName().c_str());
                }
            }

            if (instmesh){
                for (auto const &attr : instmesh->buffer.getAttributes()) {
                    depthRender.addAttribute(depthShaderData.getAttrIndex(attr.first), instmesh->buffer.getRender(), attr.second.getElements(), attr.second.getDataType(), instmesh->buffer.getStride(), attr.second.getOffset(), attr.second.getNormalized(), attr.second.getPerInstance());
                }
            }

            if (!bindSmoothNormals(depthRender, depthShaderData, mesh, mesh.submeshes[i], buffers)){
                return false;
            }

            // culls as the color pass resolved (a texture shadow casts from both faces)
            RenderState depthState(colorState.faceCulling, true, colorState.cullingMode, mesh.windingOrder, colorState.faceCulling);
            if (mesh.submeshes[i].textureShadow){
                depthState.faceCulling = false;
            }
            // the prepass writes the depth the color pass will test, offset like it
            depthState.depthBias = colorState.depthBias;
            depthState.depthBiasSlope = colorState.depthBiasSlope;

            uint16_t depthPipelines = PIP_DEPTH | PIP_SHADOW_DEPTH;
            if (shaderEnvironment.depthPrepass){
                if (pipelines & PIP_DEFAULT) depthPipelines |= PIP_ZPREPASS;
                if (pipelines & PIP_RTT) depthPipelines |= PIP_ZPREPASS_RTT;
                if (pipelines & (PIP_MSAA | PIP_MSAA_NODEPTH)) depthPipelines |= PIP_ZPREPASS_MSAA;
            }
            if (!depthRender.endLoad(depthPipelines, depthState, msaaPipelineSamples)){
                return false;
            }
        }
        //----------End depth shader---------------

        //----------Start G-buffer shader---------------
        if (scene->isSSREnabled()){
            ObjectRender& gbufferRender = mesh.submeshes[i].gbufferRender;

            gbufferRender.beginLoad(mesh.submeshes[i].primitiveType);
            attempt.renders.push_back(&gbufferRender);

            gbufferRender.setShader(mesh.submeshes[i].gbufferShader.get());
            ShaderData& gbufferShaderData = mesh.submeshes[i].gbufferShader.get()->shaderData;

            mesh.submeshes[i].slotVSGBufferParams = gbufferShaderData.getUniformBlockIndex(UniformBlockType::GBUFFER_VS_PARAMS);
            mesh.submeshes[i].slotFSGBufferMaterial = gbufferShaderData.getUniformBlockIndex(UniformBlockType::GBUFFER_FS_MATERIAL);

            if (mesh.submeshes[i].hasSkinning){
                mesh.submeshes[i].slotVSGBufferSkinning = gbufferShaderData.getUniformBlockIndex(UniformBlockType::DEPTH_VS_SKINNING);
                if (!bindSkinningResource(gbufferRender, gbufferShaderData, mesh))
                    return false;
            }
            if (mesh.submeshes[i].hasMorphTarget){
                mesh.submeshes[i].slotVSGBufferMorphTarget = gbufferShaderData.getUniformBlockIndex(UniformBlockType::DEPTH_VS_MORPHTARGET);
            }

            if (!loadGBufferTextures(mesh.submeshes[i].material, gbufferShaderData, gbufferRender)){
                return false;
            }

            if (terrain){
                mesh.submeshes[i].slotVSGBufferTerrain = gbufferShaderData.getUniformBlockIndex(UniformBlockType::DEPTH_TERRAIN_VS_PARAMS);

                // The painted G-buffer variant samples the layers too, so it binds the
                // same blend maps and detail array the color pass does
                if (p_terrainSurface && mesh.submeshes[i].hasNormal){
                    mesh.submeshes[i].slotFSGBufferTerrainLayers = gbufferShaderData.getUniformBlockIndex(UniformBlockType::FS_TERRAIN_LAYERS);
                    if (!loadTerrainTextures(entity, *terrain, gbufferRender, gbufferShaderData)){
                        return false;
                    }
                }else if (!loadTerrainHeightTexture(*terrain, gbufferRender, gbufferShaderData)){
                    return false;
                }

                for (auto const &attr : terrain->views[0].nodesbuffer[i].getAttributes()) {
                    gbufferRender.addAttribute(gbufferShaderData.getAttrIndex(attr.first), terrain->views[0].nodesbuffer[i].getRender(), attr.second.getElements(), attr.second.getDataType(), terrain->views[0].nodesbuffer[i].getStride(), attr.second.getOffset(), attr.second.getNormalized(), attr.second.getPerInstance());
                }
            }

            for (auto const& buf : buffers){
                if (buf.second->isRenderAttributes()) {
                    if (buf.second->getType() == BufferType::INDEX_BUFFER){
                        Attribute indexattr = buf.second->getAttributes()[AttributeType::INDEX];
                        gbufferRender.setIndex(buf.second->getRender(), indexattr.getDataType(), indexattr.getOffset());
                    }else{
                        for (auto const &attr : buf.second->getAttributes()){
                            if (attr.first == AttributeType::POSITION || attr.first == AttributeType::TEXCOORD1 || attr.first == AttributeType::NORMAL){
                                gbufferRender.addAttribute(gbufferShaderData.getAttrIndex(attr.first), buf.second->getRender(), attr.second.getElements(), attr.second.getDataType(), buf.second->getStride(), attr.second.getOffset(), attr.second.getNormalized(), attr.second.getPerInstance());
                            }
                            if (mesh.submeshes[i].hasSkinning){
                                if (attr.first == AttributeType::BONEIDS || attr.first == AttributeType::BONEWEIGHTS){
                                    gbufferRender.addAttribute(gbufferShaderData.getAttrIndex(attr.first), buf.second->getRender(), attr.second.getElements(), attr.second.getDataType(), buf.second->getStride(), attr.second.getOffset(), attr.second.getNormalized(), attr.second.getPerInstance());
                                }
                            }
                            if (mesh.submeshes[i].hasMorphTarget){
                                if (attr.first == AttributeType::MORPHTARGET0 || attr.first == AttributeType::MORPHTARGET1 ||
                                    attr.first == AttributeType::MORPHTARGET2 || attr.first == AttributeType::MORPHTARGET3 ||
                                    attr.first == AttributeType::MORPHTARGET4 || attr.first == AttributeType::MORPHTARGET5 ||
                                    attr.first == AttributeType::MORPHTARGET6 || attr.first == AttributeType::MORPHTARGET7 ||
                                    attr.first == AttributeType::MORPHNORMAL0 || attr.first == AttributeType::MORPHNORMAL1 ||
                                    attr.first == AttributeType::MORPHNORMAL2 || attr.first == AttributeType::MORPHNORMAL3 ||
                                    attr.first == AttributeType::MORPHTANGENT0 || attr.first == AttributeType::MORPHTANGENT1){
                                    gbufferRender.addAttribute(gbufferShaderData.getAttrIndex(attr.first), buf.second->getRender(), attr.second.getElements(), attr.second.getDataType(), buf.second->getStride(), attr.second.getOffset(), attr.second.getNormalized(), attr.second.getPerInstance());
                                }
                            }
                        }
                    }
                }
            }

            for (auto const& attr : mesh.submeshes[i].attributes){
                if (bufferNameToRender.count(attr.second.getBufferName())){
                    if (attr.first == AttributeType::INDEX){
                        gbufferRender.setIndex(bufferNameToRender[attr.second.getBufferName()], attr.second.getDataType(), attr.second.getOffset());
                    }else if (attr.first == AttributeType::POSITION || attr.first == AttributeType::TEXCOORD1 || attr.first == AttributeType::NORMAL){
                        gbufferRender.addAttribute(gbufferShaderData.getAttrIndex(attr.first), bufferNameToRender[attr.second.getBufferName()], attr.second.getElements(), attr.second.getDataType(), bufferStride[attr.second.getBufferName()], attr.second.getOffset(), attr.second.getNormalized(), attr.second.getPerInstance());
                    }
                    if (mesh.submeshes[i].hasSkinning){
                        if (attr.first == AttributeType::BONEIDS || attr.first == AttributeType::BONEWEIGHTS){
                            gbufferRender.addAttribute(gbufferShaderData.getAttrIndex(attr.first), bufferNameToRender[attr.second.getBufferName()], attr.second.getElements(), attr.second.getDataType(), bufferStride[attr.second.getBufferName()], attr.second.getOffset(), attr.second.getNormalized(), attr.second.getPerInstance());
                        }
                    }
                    if (mesh.submeshes[i].hasMorphTarget){
                        if (attr.first == AttributeType::MORPHTARGET0 || attr.first == AttributeType::MORPHTARGET1 ||
                            attr.first == AttributeType::MORPHTARGET2 || attr.first == AttributeType::MORPHTARGET3 ||
                            attr.first == AttributeType::MORPHTARGET4 || attr.first == AttributeType::MORPHTARGET5 ||
                            attr.first == AttributeType::MORPHTARGET6 || attr.first == AttributeType::MORPHTARGET7 ||
                            attr.first == AttributeType::MORPHNORMAL0 || attr.first == AttributeType::MORPHNORMAL1 ||
                            attr.first == AttributeType::MORPHNORMAL2 || attr.first == AttributeType::MORPHNORMAL3 ||
                            attr.first == AttributeType::MORPHTANGENT0 || attr.first == AttributeType::MORPHTANGENT1){
                            gbufferRender.addAttribute(gbufferShaderData.getAttrIndex(attr.first), bufferNameToRender[attr.second.getBufferName()], attr.second.getElements(), attr.second.getDataType(), bufferStride[attr.second.getBufferName()], attr.second.getOffset(), attr.second.getNormalized(), attr.second.getPerInstance());
                        }
                    }
                }
            }

            if (instmesh){
                for (auto const &attr : instmesh->buffer.getAttributes()) {
                    gbufferRender.addAttribute(gbufferShaderData.getAttrIndex(attr.first), instmesh->buffer.getRender(), attr.second.getElements(), attr.second.getDataType(), instmesh->buffer.getStride(), attr.second.getOffset(), attr.second.getNormalized(), attr.second.getPerInstance());
                }
            }

            CullingMode gbufferCullingMode = colorState.cullingMode;
            bool gbufferFaceCulling = (mesh.submeshes[i].textureShadow)? false : colorState.faceCulling;

            if (!gbufferRender.endLoad(PIP_GBUFFER, gbufferFaceCulling, true, gbufferCullingMode, mesh.windingOrder)){
                return false;
            }
        }
        //----------End G-buffer shader---------------

        // the material's extra pass (shader library phase 2), or nothing
        const bool extraPassLoaded = loadSubmeshExtraPass(entity, mesh, i, shaderInputs, shaderEnvironment, pipelines, buffers, bufferNameToRender, bufferStride, instmesh);
        // after the call the pass object is this component's own (none without a pass)
        if (mesh.submeshes[i].extraPass){
            attempt.extraPasses.push_back(mesh.submeshes[i].extraPass.get());
        }
        if (!extraPassLoaded){
            return false;
        }
    }
    attempt.succeeded = true;

    // what its shaders read of the scene (phase 3): the camera passes drawing it make them
    uint32_t sceneInputs = 0;
    uint32_t blendedSceneInputs = 0;
    bool customDepthOpaque = false;
    for (int i = 0; i < mesh.numSubmeshes; i++){
        const SubmeshShaderInputs& inputs = submeshLoads[i].shaderInputs;
        uint32_t submeshInputs = 0;
        // (a failed shader drawn by the built-in one reads nothing)
        if (inputs.manifest && mesh.submeshes[i].customShaderId != 0)
            submeshInputs |= inputs.manifest->resolveRequire(inputs.features);
        if (mesh.submeshes[i].extraPass && mesh.submeshes[i].extraPass->loaded)
            submeshInputs |= mesh.submeshes[i].extraPass->sceneInputs;
        sceneInputs |= submeshInputs;
        if (mesh.submeshes[i].alphaBlend)
            blendedSceneInputs |= submeshInputs;
        // an opaque surface whose depth program moves or clips it (a library @depth program
        // or a depth fork reading a parameter block, the frame block or a texture: displacing
        // by time or parameters, clipping by a texture), which the SSR G-buffer draws with the
        // built-in program. A @depth program without them (unlit without ALPHA_CLIP) draws
        // what the built-in one does.
        const Submesh& submesh = mesh.submeshes[i];
        if (!submesh.alphaBlend && submesh.customDepthShaderId != 0 && submesh.depthShader &&
                (submesh.customVSDepthParams.slot != -1 || submesh.customFSDepthParams.slot != -1 ||
                 submesh.depthLibraryBindings.slotVSFrame != -1 || submesh.depthLibraryBindings.slotFSFrame != -1 ||
                 !submesh.depthLibraryBindings.textures.empty()))
            customDepthOpaque = true;
    }
    mesh.sceneInputs = sceneInputs & ShaderRequire_SceneInputs;
    // a mixed mesh whose blended submeshes read the scene keeps its opaque ones in the opaque
    // queue: the basin under a water surface is in the depth and colour the water reads
    mesh.splitQueues = mesh.autoTransparency && mesh.transparent && hasOpaqueSubmesh && hasTransparentSubmesh &&
        (blendedSceneInputs & ShaderRequire_SceneInputs) != 0;
    setSceneInputUser(entity, false, mesh.sceneInputs);
    mesh.sceneInputsPredicted = false;
    // what the G-buffer's depth would leave out or draw elsewhere, so a pass reading the
    // scene depth does not take it from there
    setGBufferDepthMismatch(entity, (customDepthOpaque && !mesh.transparent) || mesh.splitQueues);

    trimSmoothNormals(mesh);

    mesh.needReload = false;
    // loaded with the current environment
    mesh.needVariantReload = false;
    mesh.needUpdateShaderUniforms = false;
    mesh.needUpdateAABB = true;
    mesh.loadCalled = true;

    // A reload recreates the terrain's per-view CDLOD node buffers empty; force a
    // re-selection next frame (the main-camera cut at view 0 is otherwise only
    // re-run on camera/transform movement), so the terrain doesn't vanish until
    // the camera moves after a settings change such as toggling SSAO.
    if (terrain){
        if (Transform* terrainTransform = scene->findComponent<Transform>(entity)){
            terrainTransform->needUpdate = true;
        }
    }

    updateMeshLods(entity, mesh, true);
    if (instmesh){
        instanceViewsDirty = true;
    }

    SystemRender::addQueueCommand(&changeLoaded, new check_load_t{scene, entity});

    return true;
}

// Levels come from MeshLodPool asynchronously; a load rehashes the geometry so a mesh
// that changed never draws stale levels. A mesh that should not have levels drops them
void RenderSystem::updateMeshLods(Entity entity, MeshComponent& mesh, bool rehash){
    if (!scene->isMeshLodEnabled() || !mesh.lodEnabled || scene->findComponent<TerrainComponent>(entity) || scene->findComponent<TilemapComponent>(entity)){
        releaseMeshLods(mesh);
        return;
    }

    std::map<std::string, Buffer*> buffers;
    if (mesh.buffer.getSize() > 0)
        buffers["vertices"] = &mesh.buffer;
    if (mesh.indices.getSize() > 0)
        buffers["indices"] = &mesh.indices;
    for (int i = 0; i < mesh.numExternalBuffers; i++)
        buffers[mesh.eBuffers[i].getName()] = &mesh.eBuffers[i];

    for (unsigned int i = 0; i < mesh.numSubmeshes; i++){
        Submesh& submesh = mesh.submeshes[i];

        if (!rehash && submesh.lodKey != 0){
            pollMeshLod(mesh, submesh);
            continue;
        }

        LodAttributes attributes;
        if (!findLodAttributes(submesh, buffers, attributes)){
            if (releaseSubmeshLod(submesh))
                updateMeshLodErrors(mesh);
            continue;
        }

        // every simplifier input takes part in the key, and the index type the levels are packed in
        const uint32_t layout[3] = {(uint32_t)attributes.index.attribute->getDataType(), attributes.index.attribute->getCount(), attributes.position.attribute->getCount()};
        uint64_t key = MeshLodPool::hash(0, layout, sizeof(layout));
        key = attributeHash(key, attributes.index);
        key = attributeHash(key, attributes.position);
        key = attributeHash(key, attributes.normal);
        key = attributeHash(key, attributes.uv);
        if (key == submesh.lodKey){
            pollMeshLod(mesh, submesh);
            continue;
        }

        submesh.lodKey = key;
        submesh.lodData.reset();
        submesh.lodIndices.getRender()->destroyBuffer();

        if (!MeshLodPool::has(key)){
            MeshLodSource source;
            if (readLodSource(attributes, source)){
                MeshLodPool::build(key, std::move(source));
            }
        }
        pollMeshLod(mesh, submesh);
    }
}

RenderSystem::LodAttribute RenderSystem::findSubmeshAttribute(Submesh& submesh, std::map<std::string, Buffer*>& buffers, AttributeType type){
    auto it = submesh.attributes.find(type);
    if (it != submesh.attributes.end()){
        auto buf = buffers.find(it->second.getBufferName());
        if (buf == buffers.end())
            return {};
        return {buf->second, &it->second};
    }
    for (auto const& buf : buffers){
        if (buf.second->isRenderAttributes() && buf.second->getAttribute(type)){
            return {buf.second, buf.second->getAttribute(type)};
        }
    }
    return {};
}

// the position and index attributes a level can be built over, with the normal and uv
// attributes that guide the collapses when present
bool RenderSystem::findLodAttributes(Submesh& submesh, std::map<std::string, Buffer*>& buffers, LodAttributes& attributes){
    if (submesh.primitiveType != PrimitiveType::TRIANGLES)
        return false;

    attributes.position = findSubmeshAttribute(submesh, buffers, AttributeType::POSITION);
    attributes.index = findSubmeshAttribute(submesh, buffers, AttributeType::INDEX);
    if (!attributes.position.attribute || !attributes.index.attribute)
        return false;
    if (attributes.position.attribute->getDataType() != AttributeDataType::FLOAT || attributes.position.attribute->getElements() != 3)
        return false;
    AttributeDataType indexType = attributes.index.attribute->getDataType();
    if (indexType != AttributeDataType::UNSIGNED_SHORT && indexType != AttributeDataType::UNSIGNED_INT)
        return false;
    if (attributes.index.attribute->getCount() % 3 != 0)
        return false;

    unsigned int vertexCount = attributes.position.attribute->getCount();
    attributes.normal = findSubmeshAttribute(submesh, buffers, AttributeType::NORMAL);
    attributes.uv = findSubmeshAttribute(submesh, buffers, AttributeType::TEXCOORD1);
    for (LodAttribute* optional : {&attributes.normal, &attributes.uv}){
        if (optional->attribute && (optional->attribute->getDataType() != AttributeDataType::FLOAT || optional->attribute->getCount() != vertexCount))
            *optional = {};
    }
    return true;
}

uint64_t RenderSystem::attributeHash(uint64_t seed, const LodAttribute& attribute){
    if (!attribute.attribute)
        return MeshLodPool::hash(seed, "", 1);
    size_t stride = std::max(attribute.buffer->getStride(), 1u);
    size_t begin = std::min((size_t)attribute.attribute->getOffset(), attribute.buffer->getSize());
    size_t end = std::min(begin + (size_t)attribute.attribute->getCount() * stride, attribute.buffer->getSize());
    return MeshLodPool::hash(seed, attribute.buffer->getData() + begin, end - begin);
}

bool RenderSystem::readLodSource(const LodAttributes& attributes, MeshLodSource& source){
    unsigned int vertexCount = attributes.position.attribute->getCount();
    unsigned int indexCount = attributes.index.attribute->getCount();
    bool normal = attributes.normal.attribute != nullptr;
    bool uv = attributes.uv.attribute != nullptr;

    source.indexType = attributes.index.attribute->getDataType();
    source.indices.reserve(indexCount);
    for (unsigned int k = 0; k < indexCount; k++){
        uint32_t value = (source.indexType == AttributeDataType::UNSIGNED_INT)
            ? attributes.index.buffer->getUInt32(attributes.index.attribute, k)
            : attributes.index.buffer->getUInt16(attributes.index.attribute, k);
        if (value >= vertexCount)
            return false;
        source.indices.push_back(value);
    }

    source.positions.reserve(vertexCount * 3);
    source.attributeCount = (normal ? 3 : 0) + (uv ? 2 : 0);
    source.attributes.reserve(vertexCount * source.attributeCount);
    for (unsigned int v = 0; v < vertexCount; v++){
        Vector3 p = attributes.position.buffer->getVector3(attributes.position.attribute, v);
        source.positions.insert(source.positions.end(), {p.x, p.y, p.z});
        if (normal){
            Vector3 n = attributes.normal.buffer->getVector3(attributes.normal.attribute, v);
            source.attributes.insert(source.attributes.end(), {n.x, n.y, n.z});
        }
        if (uv){
            Vector2 t = attributes.uv.buffer->getVector2(attributes.uv.attribute, v);
            source.attributes.insert(source.attributes.end(), {t.x, t.y});
        }
    }

    return true;
}

void RenderSystem::pollMeshLod(MeshComponent& mesh, Submesh& submesh){
    if (!submesh.lodData){
        submesh.lodData = MeshLodPool::get(submesh.lodKey);
        if (!submesh.lodData)
            return;
        submesh.lodIndices.setData(submesh.lodData->indices.data(), submesh.lodData->indices.size());
        submesh.lodIndices.setType(BufferType::INDEX_BUFFER);
        submesh.lodIndices.setUsage(BufferUsage::IMMUTABLE);
        updateMeshLodErrors(mesh);
    }

    if (submesh.lodData->numLevels > 1 && !submesh.lodIndices.getRender()->isCreated()){
        submesh.lodIndices.getRender()->createBuffer(submesh.lodIndices.getSize(), submesh.lodIndices.getData(), BufferType::INDEX_BUFFER, BufferUsage::IMMUTABLE);
    }
}

// true when the submesh had levels; builds already started still finish, and the pool
// keeps its copy until pools are cleared
bool RenderSystem::releaseSubmeshLod(Submesh& submesh){
    if (submesh.lodKey == 0 && !submesh.lodData && !submesh.lodIndices.getRender()->isCreated())
        return false;
    submesh.lodIndices.getRender()->destroyBuffer();
    submesh.lodIndices.clearAll();
    submesh.lodData.reset();
    submesh.lodKey = 0;
    return true;
}

void RenderSystem::releaseMeshLods(MeshComponent& mesh){
    bool hadLods = false;
    for (unsigned int i = 0; i < mesh.numSubmeshes; i++)
        hadLods |= releaseSubmeshLod(mesh.submeshes[i]);
    if (hadLods)
        updateMeshLodErrors(mesh);
}

// ---- shader library phase 2: extra pass and smoothed normals

namespace {
    // A vertex attribute as up to 3 floats, whatever its component type: a quantized mesh's
    // integers as they are (normalized ones to -1..1 or 0..1), missing components 0. False
    // past the end of the buffer.
    bool readAttributeVector3(const Buffer* buffer, const Attribute* attribute, unsigned int index, Vector3& out){
        const unsigned int elements = std::min(attribute->getElements(), 3u);
        size_t component = 4;
        switch (attribute->getDataType()){
            case AttributeDataType::BYTE: case AttributeDataType::UNSIGNED_BYTE: component = 1; break;
            case AttributeDataType::SHORT: case AttributeDataType::UNSIGNED_SHORT: component = 2; break;
            default: component = 4; break;
        }
        const size_t pos = (size_t)index * buffer->getStride() + attribute->getOffset();
        if (pos + component * elements > buffer->getSize())
            return false;
        const unsigned char* data = buffer->getData() + pos;
        const bool normalized = attribute->getNormalized();
        float value[3] = {0.0f, 0.0f, 0.0f};
        for (unsigned int e = 0; e < elements; e++){
            const unsigned char* at = data + e * component;
            switch (attribute->getDataType()){
                case AttributeDataType::BYTE:{ int8_t v; memcpy(&v, at, 1); value[e] = normalized ? std::max(v / 127.0f, -1.0f) : (float)v; break; }
                case AttributeDataType::UNSIGNED_BYTE:{ uint8_t v; memcpy(&v, at, 1); value[e] = normalized ? v / 255.0f : (float)v; break; }
                case AttributeDataType::SHORT:{ int16_t v; memcpy(&v, at, 2); value[e] = normalized ? std::max(v / 32767.0f, -1.0f) : (float)v; break; }
                case AttributeDataType::UNSIGNED_SHORT:{ uint16_t v; memcpy(&v, at, 2); value[e] = normalized ? v / 65535.0f : (float)v; break; }
                case AttributeDataType::INT:{ int32_t v; memcpy(&v, at, 4); value[e] = (float)v; break; }
                case AttributeDataType::UNSIGNED_INT:{ uint32_t v; memcpy(&v, at, 4); value[e] = (float)v; break; }
                case AttributeDataType::FLOAT:{ memcpy(&value[e], at, 4); break; }
            }
        }
        out = Vector3(value[0], value[1], value[2]);
        return true;
    }

    // the triangles of one submesh drawing over the shared positions
    struct SmoothNormalTriangles{
        const Buffer* indexBuffer = nullptr;
        const Attribute* index = nullptr;
        PrimitiveType primitive = PrimitiveType::TRIANGLES;
    };

    // what a_smoothNormal is made from: positions (any component type, 2 or 3 of them), the
    // normals when there are 3 per vertex, and the triangles of every submesh drawing over
    // those positions, so submeshes sharing one vertex buffer share one entry (welded once)
    struct SmoothNormalSource{
        const Buffer* positionBuffer = nullptr;
        const Attribute* position = nullptr;
        const Buffer* normalBuffer = nullptr;
        const Attribute* normal = nullptr;
        std::vector<SmoothNormalTriangles> triangles;
    };

    // Per vertex, the normal averaged over every vertex sharing its position (welded within a
    // small tolerance of the mesh's size). Each vertex weighs its normal (its triangles'
    // normal when the mesh has none) by the angles of its triangle corners, so a cube corner
    // gets the even diagonal however its faces are split. Only vertices some triangle uses
    // take part (others would add a normal no face has); with no triangle at all (points,
    // lines) every vertex counts with its own normal. Packed as 4 signed normalized bytes.
    void computeSmoothNormals(const SmoothNormalSource& source, std::vector<int8_t>& out){
        const unsigned int count = source.position->getCount();

        std::vector<Vector3> positions(count, Vector3::ZERO);
        AABB bounds;
        bounds.setNull();
        for (unsigned int v = 0; v < count; v++){
            readAttributeVector3(source.positionBuffer, source.position, v, positions[v]);
            bounds.merge(positions[v]);
        }
        std::vector<Vector3> normals;
        if (source.normal){
            normals.resize(count, Vector3::ZERO);
            for (unsigned int v = 0; v < count; v++){
                if (readAttributeVector3(source.normalBuffer, source.normal, v, normals[v]) && normals[v].squaredLength() > 0.0f)
                    normals[v].normalize();
            }
        }

        // each vertex's normal weighed by its corner angles
        std::vector<Vector3> weighted(count, Vector3::ZERO);
        std::vector<uint8_t> referenced(count, 0);
        bool anyTriangle = false;
        auto addTriangle = [&](uint32_t a, uint32_t b, uint32_t c){
            if (a >= count || b >= count || c >= count || a == b || b == c || a == c)
                return;
            referenced[a] = referenced[b] = referenced[c] = 1;
            anyTriangle = true;
            const Vector3 ab = positions[b] - positions[a];
            const Vector3 ac = positions[c] - positions[a];
            const Vector3 bc = positions[c] - positions[b];
            Vector3 face = ab.crossProduct(ac);
            if (face.squaredLength() <= 0.0f)
                return;
            face.normalize();
            auto angle = [](Vector3 u, Vector3 w){
                const float lu = u.length(), lw = w.length();
                if (lu <= 0.0f || lw <= 0.0f)
                    return 0.0f;
                return std::acos(std::clamp(u.dotProduct(w) / (lu * lw), -1.0f, 1.0f));
            };
            const uint32_t corners[3] = {a, b, c};
            const float angles[3] = {angle(ab, ac), angle(bc, -ab), angle(-ac, -bc)};
            for (int k = 0; k < 3; k++){
                const Vector3& normal = normals.empty() ? face : normals[corners[k]];
                weighted[corners[k]] += normal * angles[k];
            }
        };

        for (const SmoothNormalTriangles& triangles : source.triangles){
            if (triangles.primitive != PrimitiveType::TRIANGLES && triangles.primitive != PrimitiveType::TRIANGLE_STRIP)
                continue;
            auto vertexAt = [&](unsigned int k) -> uint32_t {
                if (!triangles.index)
                    return k;
                Buffer* indexBuffer = const_cast<Buffer*>(triangles.indexBuffer);
                Attribute* indexAttribute = const_cast<Attribute*>(triangles.index);
                return (triangles.index->getDataType() == AttributeDataType::UNSIGNED_INT)
                    ? indexBuffer->getUInt32(indexAttribute, k) : indexBuffer->getUInt16(indexAttribute, k);
            };
            const unsigned int elements = triangles.index ? triangles.index->getCount() : count;
            if (triangles.primitive == PrimitiveType::TRIANGLES){
                for (unsigned int k = 0; k + 2 < elements; k += 3)
                    addTriangle(vertexAt(k), vertexAt(k + 1), vertexAt(k + 2));
            }else{
                for (unsigned int k = 0; k + 2 < elements; k++){
                    if (k % 2 == 0)
                        addTriangle(vertexAt(k), vertexAt(k + 1), vertexAt(k + 2));
                    else
                        addTriangle(vertexAt(k + 1), vertexAt(k), vertexAt(k + 2));
                }
            }
        }
        // no triangle at all (points, lines): every vertex counts with its own normal
        if (!anyTriangle){
            for (unsigned int v = 0; v < count; v++){
                referenced[v] = 1;
                if (!normals.empty())
                    weighted[v] = normals[v];
            }
        }

        // weld by position: cells of the tolerance's size, a vertex joining the first group
        // whose first vertex is within the tolerance (in its cell or a neighbour)
        const Vector3 extent = bounds.isNull() ? Vector3::ZERO : (bounds.getMaximum() - bounds.getMinimum());
        const float tolerance = std::max(std::max(extent.x, std::max(extent.y, extent.z)) * 1e-5f, 1e-7f);
        const float inverse = 1.0f / tolerance;
        auto cellOf = [inverse](float value){ return (int64_t)std::floor(value * inverse); };
        auto cellKey = [](int64_t x, int64_t y, int64_t z){
            return ((uint64_t)(x & 0x1FFFFF) << 42) | ((uint64_t)(y & 0x1FFFFF) << 21) | (uint64_t)(z & 0x1FFFFF);
        };
        std::unordered_map<uint64_t, int> cellFirstGroup;
        std::vector<int> groupNext;      // the next group in the same cell
        std::vector<uint32_t> groupVertex; // the vertex that started it
        std::vector<Vector3> groupSum;
        std::vector<int> vertexGroup(count, -1);
        cellFirstGroup.reserve(count);
        const float tolerance2 = tolerance * tolerance;
        for (unsigned int v = 0; v < count; v++){
            if (!referenced[v])
                continue;
            const Vector3& p = positions[v];
            int found = -1;
            const int64_t x0 = cellOf(p.x - tolerance), x1 = cellOf(p.x + tolerance);
            const int64_t y0 = cellOf(p.y - tolerance), y1 = cellOf(p.y + tolerance);
            const int64_t z0 = cellOf(p.z - tolerance), z1 = cellOf(p.z + tolerance);
            for (int64_t x = x0; x <= x1 && found == -1; x++){
                for (int64_t y = y0; y <= y1 && found == -1; y++){
                    for (int64_t z = z0; z <= z1 && found == -1; z++){
                        auto it = cellFirstGroup.find(cellKey(x, y, z));
                        for (int g = (it == cellFirstGroup.end()) ? -1 : it->second; g != -1; g = groupNext[g]){
                            if ((positions[groupVertex[g]] - p).squaredLength() <= tolerance2){
                                found = g;
                                break;
                            }
                        }
                    }
                }
            }
            if (found == -1){
                found = (int)groupVertex.size();
                groupVertex.push_back(v);
                groupSum.push_back(Vector3::ZERO);
                const uint64_t key = cellKey(cellOf(p.x), cellOf(p.y), cellOf(p.z));
                auto it = cellFirstGroup.find(key);
                groupNext.push_back(it == cellFirstGroup.end() ? -1 : it->second);
                cellFirstGroup[key] = found;
            }
            vertexGroup[v] = found;
            groupSum[found] += weighted[v];
        }

        out.resize((size_t)count * 4);
        for (unsigned int v = 0; v < count; v++){
            Vector3 n = (vertexGroup[v] >= 0) ? groupSum[vertexGroup[v]] : Vector3::ZERO;
            // opposite faces sharing a position cancel out: keep the vertex's own
            if (n.squaredLength() < 1e-12f)
                n = weighted[v];
            if (n.squaredLength() < 1e-12f)
                n = normals.empty() ? Vector3(0.0f, 1.0f, 0.0f) : normals[v];
            if (n.squaredLength() < 1e-12f)
                n = Vector3(0.0f, 1.0f, 0.0f);
            n.normalize();
            out[(size_t)v * 4 + 0] = (int8_t)std::lround(std::clamp(n.x, -1.0f, 1.0f) * 127.0f);
            out[(size_t)v * 4 + 1] = (int8_t)std::lround(std::clamp(n.y, -1.0f, 1.0f) * 127.0f);
            out[(size_t)v * 4 + 2] = (int8_t)std::lround(std::clamp(n.z, -1.0f, 1.0f) * 127.0f);
            out[(size_t)v * 4 + 3] = 0;
        }
    }

    // a_smoothNormal when the positions cannot be read (fewer than 2 components, or none):
    // the vertex normal (+Y without one), so the mesh still draws, without closed corners
    void fallbackSmoothNormals(const Buffer* normalBuffer, const Attribute* normal, unsigned int count, std::vector<int8_t>& out){
        out.assign((size_t)count * 4, 0);
        for (unsigned int v = 0; v < count; v++){
            Vector3 n(0.0f, 1.0f, 0.0f);
            if (normal && (!readAttributeVector3(normalBuffer, normal, v, n) || n.squaredLength() < 1e-12f))
                n = Vector3(0.0f, 1.0f, 0.0f);
            n.normalize();
            out[(size_t)v * 4 + 0] = (int8_t)std::lround(std::clamp(n.x, -1.0f, 1.0f) * 127.0f);
            out[(size_t)v * 4 + 1] = (int8_t)std::lround(std::clamp(n.y, -1.0f, 1.0f) * 127.0f);
            out[(size_t)v * 4 + 2] = (int8_t)std::lround(std::clamp(n.z, -1.0f, 1.0f) * 127.0f);
        }
    }

}

bool RenderSystem::bindSmoothNormals(ObjectRender& render, ShaderData& shaderData, MeshComponent& mesh, Submesh& submesh, BufferMap& buffers){
    const int slot = shaderData.getAttrIndex(AttributeType::SMOOTHNORMAL);
    if (slot == -1)
        return true;

    if (submesh.smoothNormals == -1){
        const LodAttribute position = findSubmeshAttribute(submesh, buffers, AttributeType::POSITION);
        auto normalOf = [&](Submesh& owner, unsigned int count) -> LodAttribute {
            const LodAttribute normal = findSubmeshAttribute(owner, buffers, AttributeType::NORMAL);
            if (normal.attribute && normal.attribute->getElements() >= 3 && normal.attribute->getCount() == count &&
                    normal.attribute->getDataType() != AttributeDataType::UNSIGNED_INT && normal.attribute->getDataType() != AttributeDataType::INT)
                return normal;
            return LodAttribute();
        };
        const bool readable = position.attribute && position.attribute->getElements() >= 2 &&
                              position.attribute->getDataType() != AttributeDataType::UNSIGNED_INT &&
                              position.attribute->getDataType() != AttributeDataType::INT;
        const unsigned int count = position.attribute ? position.attribute->getCount() : mesh.vertexCount;
        const LodAttribute normal = normalOf(submesh, count);
        auto sameAttribute = [](const LodAttribute& a, const LodAttribute& b){
            if (!a.attribute || !b.attribute)
                return !a.attribute && !b.attribute;
            return a.buffer == b.buffer && a.attribute->getOffset() == b.attribute->getOffset() &&
                   a.attribute->getCount() == b.attribute->getCount() && a.attribute->getDataType() == b.attribute->getDataType() &&
                   a.attribute->getElements() == b.attribute->getElements();
        };

        // every submesh drawing over the same positions and normals: one entry for all of them
        std::vector<unsigned int> sharing;
        SmoothNormalSource source;
        source.positionBuffer = position.buffer;
        source.position = position.attribute;
        source.normalBuffer = normal.buffer;
        source.normal = normal.attribute;
        for (unsigned int s = 0; s < mesh.numSubmeshes; s++){
            Submesh& other = mesh.submeshes[s];
            if (&other != &submesh){
                if (other.smoothNormals != -1 || !sameAttribute(findSubmeshAttribute(other, buffers, AttributeType::POSITION), position) ||
                        !sameAttribute(normalOf(other, count), normal))
                    continue;
            }
            sharing.push_back(s);
            SmoothNormalTriangles triangles;
            triangles.primitive = other.primitiveType;
            const LodAttribute index = findSubmeshAttribute(other, buffers, AttributeType::INDEX);
            if (index.attribute && (index.attribute->getDataType() == AttributeDataType::UNSIGNED_SHORT ||
                                    index.attribute->getDataType() == AttributeDataType::UNSIGNED_INT)){
                triangles.indexBuffer = index.buffer;
                triangles.index = index.attribute;
            }
            source.triangles.push_back(triangles);
        }

        // cached by what it is made of, so the same geometry is welded once
        std::vector<uint32_t> layout = {readable ? 1u : 0u, count};
        for (const SmoothNormalTriangles& triangles : source.triangles){
            layout.push_back((uint32_t)triangles.primitive);
            layout.push_back(triangles.index ? triangles.index->getCount() : 0u);
        }
        uint64_t key = MeshLodPool::hash(0, layout.data(), layout.size() * sizeof(uint32_t));
        key = attributeHash(key, position);
        key = attributeHash(key, normal);
        for (const SmoothNormalTriangles& triangles : source.triangles){
            key = attributeHash(key, triangles.index ? LodAttribute{const_cast<Buffer*>(triangles.indexBuffer), const_cast<Attribute*>(triangles.index)} : LodAttribute());
        }

        if (!mesh.smoothNormals)
            mesh.smoothNormals = std::make_shared<MeshSmoothNormals>();
        std::deque<MeshSmoothNormals::Entry>& entries = mesh.smoothNormals->entries;
        int found = -1;
        for (size_t e = 0; e < entries.size(); e++){
            if (entries[e].key == key && entries[e].data.size() == (size_t)count * 4){
                found = (int)e;
                break;
            }
        }
        if (found == -1){
            entries.emplace_back();
            entries.back().key = key;
            if (readable){
                computeSmoothNormals(source, entries.back().data);
            }else{
                Log::warn("A shader of this mesh reads a_smoothNormal (@require smoothnormals), but its positions cannot be read "
                          "(%s): it gets the vertex normal instead, and an outline opens at hard edges",
                          position.attribute ? "fewer than 2 components" : "none");
                fallbackSmoothNormals(normal.buffer, normal.attribute, count, entries.back().data);
            }
            found = (int)entries.size() - 1;
        }
        for (unsigned int s : sharing)
            mesh.submeshes[s].smoothNormals = found;
    }

    MeshSmoothNormals::Entry& entry = mesh.smoothNormals->entries[submesh.smoothNormals];
    entry.used = true;
    // once per entry: on the async thread the buffer is created later, isCreated() is false
    // until then, and the other renders of the submesh bind the same buffer
    if (!entry.created){
        if (entry.data.empty() || !entry.buffer.createBuffer((unsigned int)entry.data.size(), entry.data.data(), BufferType::VERTEX_BUFFER, BufferUsage::IMMUTABLE))
            return false;
        entry.created = true;
    }
    render.addAttribute(slot, &entry.buffer, 4, AttributeDataType::BYTE, 4, 0, true, false);
    return true;
}

void RenderSystem::releaseSmoothNormals(MeshComponent& mesh, bool keepData){
    if (!mesh.smoothNormals)
        return;
    // a copy of the component shares them until its own load: its buffers are not ours alone
    if (mesh.smoothNormals.use_count() == 1){
        for (MeshSmoothNormals::Entry& entry : mesh.smoothNormals->entries){
            entry.buffer.destroyBuffer();
            entry.created = false;
        }
    }
    if (!keepData)
        mesh.smoothNormals.reset();
}

void RenderSystem::trimSmoothNormals(MeshComponent& mesh){
    if (!mesh.smoothNormals)
        return;
    std::deque<MeshSmoothNormals::Entry>& entries = mesh.smoothNormals->entries;
    std::vector<int> remap(entries.size(), -1);
    size_t kept = 0;
    for (size_t e = 0; e < entries.size(); e++){
        if (!entries[e].used){
            entries[e].buffer.destroyBuffer();
            continue;
        }
        remap[e] = (int)kept;
        if (kept != e){
            // moved, not copied: the data keeps its address (an async buffer creation reads it)
            entries[kept].key = entries[e].key;
            entries[kept].data = std::move(entries[e].data);
            entries[kept].buffer = entries[e].buffer;
            entries[kept].created = entries[e].created;
            entries[kept].used = entries[e].used;
        }
        kept++;
    }
    entries.resize(kept);
    for (unsigned int s = 0; s < mesh.numSubmeshes; s++){
        int& index = mesh.submeshes[s].smoothNormals;
        index = (index >= 0 && index < (int)remap.size()) ? remap[index] : -1;
    }
    // nothing asks for them any more
    if (entries.empty())
        mesh.smoothNormals.reset();
}

unsigned int RenderSystem::bindSubmeshAttributes(ObjectRender& render, ShaderData& shaderData, MeshComponent& mesh, Submesh& submesh,
                                                 BufferMap& buffers, BufferRenderMap& bufferNameToRender, BufferStrideMap& bufferStride,
                                                 InstancedMeshComponent* instmesh){
    // as loadMesh binds the colour render's
    unsigned int indexCount = 0;
    bool indexed = false;
    for (auto const& buf : buffers){
        if (!buf.second->isRenderAttributes())
            continue;
        if (buf.second->getType() == BufferType::INDEX_BUFFER){
            indexed = true;
            indexCount = buf.second->getCount();
            Attribute indexattr = buf.second->getAttributes()[AttributeType::INDEX];
            render.setIndex(buf.second->getRender(), indexattr.getDataType(), indexattr.getOffset());
        }else{
            for (auto const &attr : buf.second->getAttributes()) {
                render.addAttribute(shaderData.getAttrIndex(attr.first), buf.second->getRender(), attr.second.getElements(), attr.second.getDataType(), buf.second->getStride(), attr.second.getOffset(), attr.second.getNormalized(), attr.second.getPerInstance());
            }
        }
    }
    for (auto const& attr : submesh.attributes){
        if (!bufferNameToRender.count(attr.second.getBufferName()))
            continue;
        if (attr.first == AttributeType::INDEX){
            indexed = true;
            indexCount = attr.second.getCount();
            render.setIndex(bufferNameToRender[attr.second.getBufferName()], attr.second.getDataType(), attr.second.getOffset());
        }else{
            render.addAttribute(shaderData.getAttrIndex(attr.first), bufferNameToRender[attr.second.getBufferName()], attr.second.getElements(), attr.second.getDataType(), bufferStride[attr.second.getBufferName()], attr.second.getOffset(), attr.second.getNormalized(), attr.second.getPerInstance());
        }
    }
    if (instmesh){
        for (auto const &attr : instmesh->buffer.getAttributes()) {
            render.addAttribute(shaderData.getAttrIndex(attr.first), instmesh->buffer.getRender(), attr.second.getElements(), attr.second.getDataType(), instmesh->buffer.getStride(), attr.second.getOffset(), attr.second.getNormalized(), attr.second.getPerInstance());
        }
    }
    return indexed ? indexCount : mesh.vertexCount;
}

void RenderSystem::releaseSubmeshExtraPass(Submesh& submesh, bool keepShader){
    if (!submesh.extraPass)
        return;
    // a copy of the component shares the object until its own load
    if (submesh.extraPass.use_count() > 1){
        submesh.extraPass.reset();
        return;
    }
    SubmeshExtraPass& extra = *submesh.extraPass;
    if (extra.loaded)
        extra.render.destroy();
    extra.loaded = false;
    extra.libraryBindings.clear();
    extra.customVSParams.clear();
    extra.customFSParams.clear();
    if (keepShader)
        return;
    if (extra.shader){
        extra.shader.reset();
        ShaderPool::remove(ShaderType::MESH, extra.shaderProperties, extra.customShaderId);
    }
    submesh.extraPass.reset();
}

bool RenderSystem::loadSubmeshExtraPass(Entity entity, MeshComponent& mesh, unsigned int submeshIndex, const SubmeshShaderInputs& submeshInputs,
                                        const MeshShaderEnvironment& environment, uint16_t pipelines, BufferMap& buffers,
                                        BufferRenderMap& bufferNameToRender, BufferStrideMap& bufferStride, InstancedMeshComponent* instmesh){
    Submesh& submesh = mesh.submeshes[submeshIndex];
    if (!hasExtraPass(submesh, submeshInputs.terrain)){
        // nothing is kept for a submesh without one
        releaseSubmeshExtraPass(submesh, false);
        return true;
    }
    // (set: its shader is not empty) the textures bind through non-const references
    MaterialPass& pass = submesh.material.extraPass.edit();

    // the submesh's mesh variant bits with the pass's shader (getExtraPassShaderInputs)
    const SubmeshShaderInputs inputs = getExtraPassShaderInputs(submeshInputs, pass);
    const SubmeshShaderVariant variant = getSubmeshShaderVariant(environment, inputs);
    const std::string reference = getSubmeshExtraPassReference(submesh);
    const uint16_t customId = ShaderPool::registerCustomShader(reference);

    // a copy of the component shares the object until it loads: it makes its own
    if (!submesh.extraPass || submesh.extraPass.use_count() > 1)
        submesh.extraPass = std::make_shared<SubmeshExtraPass>();
    SubmeshExtraPass& extra = *submesh.extraPass;

    // a load tried again (another submesh was not ready): the pipelines of the last try go
    if (extra.loaded)
        extra.render.destroy();
    extra.loaded = false;
    // the shader of another variant (the pass or the scene changed) goes back to the pool
    if (extra.shader && (extra.shaderProperties != variant.properties || extra.customShaderId != customId)){
        extra.shader.reset();
        ShaderPool::remove(ShaderType::MESH, extra.shaderProperties, extra.customShaderId);
    }
    extra.shaderProperties = variant.properties;
    extra.customShaderId = customId;
    extra.order = pass.order;
    extra.failed = false;
    extra.shader = ShaderPool::get(ShaderType::MESH, extra.shaderProperties, extra.customShaderId);
    if (!extra.shader->isCreated()){
        if (ShaderPool::isShaderBuildFailed(ShaderType::MESH, extra.shaderProperties, extra.customShaderId)){
            // no built-in fallback (it would draw the submesh twice): the submesh draws alone
            Log::error("Extra pass shader '%s' failed to compile; drawing '%s' without its extra pass",
                       reference.c_str(), scene->getEntityName(entity).c_str());
            extra.failed = true;
            return true;
        }
        return false;
    }

    extra.manifest = inputs.manifest;
    extra.manifestState = inputs.manifest ? inputs.manifest->resolveState(inputs.features) : ShaderManifestState();
    extra.sceneInputs = inputs.manifest ? (inputs.manifest->resolveRequire(inputs.features) & ShaderRequire_SceneInputs) : 0;

    ObjectRender& render = extra.render;
    render.beginLoad(submesh.primitiveType);
    render.setShader(extra.shader.get());
    ShaderData& shaderData = extra.shader->shaderData;

    // the blocks of its variant, as the submesh's colour render resolves them
    const uint32_t properties = extra.shaderProperties;
    const bool p_lit = !(properties & (1u << 0));
    extra.slotVSParams = shaderData.getUniformBlockIndex(UniformBlockType::PBR_VS_PARAMS);
    extra.slotFSParams = shaderData.getUniformBlockIndex(UniformBlockType::PBR_FS_PARAMS);
    extra.slotFSTexCoordSets = (properties & (1u << 2)) ? shaderData.getUniformBlockIndex(UniformBlockType::PBR_FS_TEXCOORDSETS) : -1;
    extra.slotVSFade = (properties & (1u << 26)) ? shaderData.getUniformBlockIndex(UniformBlockType::PBR_VS_FADE) : -1;
    extra.slotFSFog = hasFog ? shaderData.getUniformBlockIndex(UniformBlockType::FS_FOG) : -1;
    extra.slotVSObjectLights = -1;
    extra.slotFSObjectLights = -1;
    extra.sizeVSObjectLights = 0;
    extra.sizeFSObjectLights = 0;
    extra.slotFSLitFrame = -1;
    extra.sizeFSLitFrame = 0;
    extra.slotFSLighting = -1;
    if (p_lit){
        extra.slotVSObjectLights = shaderData.getUniformBlockIndexByName("u_vs_objectLights");
        extra.slotFSObjectLights = shaderData.getUniformBlockIndexByName("u_fs_objectLights");
        shaderData.getUniformBlockMembers("u_vs_objectLights", extra.sizeVSObjectLights);
        shaderData.getUniformBlockMembers("u_fs_objectLights", extra.sizeFSObjectLights);
        extra.sizeVSObjectLights = std::min(extra.sizeVSObjectLights, (unsigned int)sizeof(object_lights_t));
        extra.sizeFSObjectLights = std::min(extra.sizeFSObjectLights, (unsigned int)sizeof(object_lights_t));
        extra.slotFSLitFrame = shaderData.getUniformBlockIndexByName("u_fs_litFrame");
        shaderData.getUniformBlockMembers("u_fs_litFrame", extra.sizeFSLitFrame);
        extra.sizeFSLitFrame = std::min(extra.sizeFSLitFrame, (unsigned int)sizeof(lit_frame_block_t));
        extra.slotFSLighting = shaderData.getUniformBlockIndex(UniformBlockType::FS_LIGHTING);
    }
    extra.slotFSLighting2D = (properties & (1u << 22)) ? shaderData.getUniformBlockIndex(UniformBlockType::FS_LIGHTING2D) : -1;
    extra.slotVSSprite = submesh.hasTextureRect ? shaderData.getUniformBlockIndex(UniformBlockType::SPRITE_VS_PARAMS) : -1;
    extra.slotVSSkinning = -1;
    if (submesh.hasSkinning){
        extra.slotVSSkinning = shaderData.getUniformBlockIndex(UniformBlockType::VS_SKINNING);
        if (!bindSkinningResource(render, shaderData, mesh))
            return false;
    }
    extra.slotVSMorphTarget = submesh.hasMorphTarget ? shaderData.getUniformBlockIndex(UniformBlockType::VS_MORPHTARGET) : -1;

    // its own parameters, else its manifest's defaults
    extra.customVSParams.resolve(shaderData, "u_vs_customParams");
    extra.customFSParams.resolve(shaderData, "u_fs_customParams");
    extra.customVSParams.writeValues(pass.shaderParams, nullptr, extra.manifest.get());
    extra.customFSParams.writeValues(pass.shaderParams, nullptr, extra.manifest.get());

    // the material's textures (a fork of Lit samples them) and its own custom textures
    if (!loadPBRTextures(submesh.material, shaderData, render, mesh.receiveLights))
        return false;
    if (!loadShaderLibraryBindings(extra.libraryBindings, shaderData, render, pass.shaderTextures, extra.manifest.get()))
        return false;
    extra.needUpdateTexture = false;

    bindSubmeshAttributes(render, shaderData, mesh, submesh, buffers, bufferNameToRender, bufferStride, instmesh);
    if (!bindSmoothNormals(render, shaderData, mesh, submesh, buffers))
        return false;

    const RenderState extraState = resolveExtraPassRenderState(mesh, submesh, extra.manifest ? &extra.manifestState : nullptr);
    extra.libraryBindings.postColorBlend = postColorBlendOf(extraState, extra.manifest != nullptr);
    if (!render.endLoad(pipelines, extraState, msaaPipelineSamples))
        return false;

    extra.loaded = true;
    return true;
}

void RenderSystem::drawSubmeshExtraPass(Entity entity, MeshComponent& mesh, unsigned int submeshIndex, Transform& transform, PipelineType pipType,
                                        InstancedMeshComponent* instmesh, int instanceView, const MeshDraw& draw, unsigned int instanceCount,
                                        const Matrix4* objectBlock, TilemapComponent* tilemap, uint32_t& builtLightBits){
    Submesh& submesh = mesh.submeshes[submeshIndex];
    SubmeshExtraPass& extra = *submesh.extraPass;
    MaterialPass& pass = submesh.material.extraPass.edit(); // set: it loaded
    ObjectRender& render = extra.render;
    ShaderData& shaderData = extra.shader->shaderData;
    const uint32_t properties = extra.shaderProperties;

    // textures changed (drawMesh also sets it when a material texture is a render target that was
    // redrawn), or one of its own is a redrawn render target
    if (extra.needUpdateTexture ||
            (!extra.libraryBindings.textures.empty() && shaderTexturesOutdated(extra.libraryBindings, pass.shaderTextures))){
        if (loadPBRTextures(submesh.material, shaderData, render, mesh.receiveLights) &&
                bindShaderTextures(extra.libraryBindings, render, pass.shaderTextures)){
            extra.needUpdateTexture = false;
        }
    }

    // the editor's "no face culling" view keeps the pass's culling: it is part of the technique
    // (an inverted hull drawn unculled covers its object)
    if (!render.beginDraw(pipType, false)){
        mesh.needReload = true;
        return;
    }

    applyFrameBlock(render, extra.libraryBindings);
    applySceneInputs(render, extra.libraryBindings);
    applyEnvironment(render, extra.libraryBindings, mesh, transform);

    if (extra.slotFSFog != -1){
#ifdef JASPERENGINE_EDITOR
        static fs_fog_t noFog = {Vector4(0.0f, 0.0f, 0.0f, 1.0f), Vector4(0.0f, 0.0f, 0.0f, 0.0f)};
        render.applyUniformBlock(extra.slotFSFog, sizeof(float) * 8, (rv().disableFog) ? &noFog : &fs_fog);
#else
        render.applyUniformBlock(extra.slotFSFog, sizeof(float) * 8, &fs_fog);
#endif
    }

    const bool lit = !(properties & (1u << 0));
    if (lit){
        loadSpotMaskTexture(shaderData, render);
        if (extra.slotVSObjectLights != -1 || extra.slotFSObjectLights != -1){
            const uint32_t lightBits = properties & ((1u << 3) | (1u << 28) | (1u << 29) | (1u << 30));
            if (lightBits != builtLightBits){
                buildObjectLights(mesh, properties, objectLightsBlock);
                builtLightBits = lightBits;
            }
            render.applyUniformBlock(extra.slotVSObjectLights, extra.sizeVSObjectLights, &objectLightsBlock);
            render.applyUniformBlock(extra.slotFSObjectLights, extra.sizeFSObjectLights, &objectLightsBlock);
        }
        if (extra.slotFSLitFrame != -1){
            lit_frame_block_t& litFrame = passLitFrameBlocks[extra.libraryBindings.postColorBlend];
            render.applyUniformBlock(extra.slotFSLitFrame, extra.sizeFSLitFrame,
                (properties & ShaderPool::getPostColorProperty(ShaderType::MESH)) ? (void*)&litFrame : (void*)&litFrame.lit);
        }
        if (extra.slotFSLighting != -1){
            buildLegacyLighting(mesh, properties, legacyLightingBlock);
            render.applyUniformBlock(extra.slotFSLighting, sizeof(fs_lighting_t), &legacyLightingBlock);
        }
        // USE_SSAO (a fork of Lit without a manifest)
        if (properties & (1u << 21)){
            render.addTexture(shaderData.getTextureIndex(TextureShaderType::SSAOTEXTURE), ShaderStageType::FRAGMENT,
                              rv().currentSSAOTexture ? rv().currentSSAOTexture : &emptyWhite);
        }
    }

    if (extra.slotFSLighting2D != -1){
        render.applyUniformBlock(extra.slotFSLighting2D, sizeof(fs_lighting2d_t), &fs_lighting2d);
    }
    if (extra.slotVSSprite != -1){
        render.applyUniformBlock(extra.slotVSSprite, sizeof(float) * 4, &submesh.textureRect);
    }
    if (submesh.hasSkinning){
        applySkinningUniform(render, extra.slotVSSkinning, mesh, submesh);
    }
    if (extra.slotVSMorphTarget != -1){
        if (!submesh.hasMorphNormal && !submesh.hasMorphTangent){
            render.applyUniformBlock(extra.slotVSMorphTarget, sizeof(float) * MAX_MORPHTARGETS, &mesh.morphWeights);
        }else{
            render.applyUniformBlock(extra.slotVSMorphTarget, sizeof(float) * MAX_MORPHTARGETS / 2, &mesh.morphWeights);
        }
    }

    // the material's prefix, the size its variant declares (a fork of Lit reads it)
    if (lit){
        render.applyUniformBlock(extra.slotFSParams, sizeof(float) * 12, &submesh.material);
    }else if (properties & (1u << 24)){
        render.applyUniformBlock(extra.slotFSParams, sizeof(float) * 8, &submesh.material);
    }else{
        render.applyUniformBlock(extra.slotFSParams, sizeof(float) * 4, &submesh.material);
    }

    if (extra.slotVSFade != -1 && instmesh){
        applyInstanceFadeUniform(render, extra.slotVSFade, *instmesh, instanceFadeEye(entity, *instmesh, instanceView));
    }
    if (extra.slotFSTexCoordSets != -1){
        const Material& mat = submesh.material;
        int32_t texCoordSets[8] = {
            mat.baseColorTexCoord, mat.metallicRoughnessTexCoord, mat.occlusionTexCoord, mat.emissiveTexCoord,
            mat.normalTexCoord, 0, 0, 0
        };
        render.applyUniformBlock(extra.slotFSTexCoordSets, sizeof(texCoordSets), &texCoordSets);
    }

    render.applyUniformBlock(extra.slotVSParams, sizeof(float) * 48, (void*)objectBlock);

    applyCustomUniforms(render, extra.customVSParams, frameTime, passResolution);
    applyCustomUniforms(render, extra.customFSParams, frameTime, passResolution);

    // the same instances, detail level and tilemap chunks as the submesh's draw
    if (tilemap && !tilemapDrawRanges.empty()){
        if (instmesh){
            bindInstances(render, *instmesh, instanceView, draw.instanceFirst);
        }
        for (const TilemapDrawRange& range : tilemapDrawRanges){
            render.draw(range.offset, range.count, instanceCount);
        }
    }else{
        drawSubmeshGeometry(render, entity, mesh, submeshIndex, instmesh, instanceView, draw.lod, instanceCount);
    }
}

bool RenderSystem::hasPendingMeshLods() const{
    auto meshes = scene->getComponentArray<MeshComponent>();
    for (int i = 0; i < meshes->size(); i++){
        MeshComponent& mesh = meshes->getComponentFromIndex(i);
        for (unsigned int s = 0; s < mesh.numSubmeshes; s++){
            if (isMeshLodPending(mesh.submeshes[s]))
                return true;
        }
    }
    return false;
}

// a submesh still waiting for its levels, or for their GPU buffer
bool RenderSystem::isMeshLodPending(Submesh& submesh){
    if (submesh.lodKey == 0)
        return false;
    if (!submesh.lodData)
        return true;
    return submesh.lodData->numLevels > 1 && !submesh.lodIndices.getRender()->isCreated();
}

// a level is only as good as its worst submesh
void RenderSystem::updateMeshLodErrors(MeshComponent& mesh){
    mesh.numLods = 1;
    for (unsigned int l = 0; l < MAX_MESH_LODS; l++)
        mesh.lodError[l] = 0;
    for (unsigned int i = 0; i < mesh.numSubmeshes; i++){
        const MeshLodData* data = mesh.submeshes[i].lodData.get();
        if (!data)
            continue;
        mesh.numLods = std::max(mesh.numLods, data->numLevels);
        for (unsigned int l = 1; l < data->numLevels; l++)
            mesh.lodError[l] = std::max(mesh.lodError[l], data->levels[l].error);
    }
    instanceViewsDirty = true;
}

// world units one pixel of the view being drawn spans through camera
RenderSystem::LodView RenderSystem::computeLodView(const CameraComponent& camera, const Transform& cameraTransform) const{
    float viewHeight = targetViewRect().getHeight();
    if (isFixedResolutionActive())
        viewHeight = (float)scene->getFixedResolutionHeight();
    viewHeight = std::max(viewHeight, 1.0f);

    LodView view;
    view.enabled = scene->isMeshLodEnabled();
    view.threshold = scene->getMeshLodThreshold();
    view.origin = cameraTransform.worldPosition;
    if (camera.type == CameraType::CAMERA_PERSPECTIVE){
        view.perUnit = 2.0f * std::tan(camera.yfov * 0.5f) / viewHeight;
    }else{
        view.constant = std::abs(camera.topClip - camera.bottomClip) / viewHeight;
    }
    return view;
}

// a change rebuilds the instance views
void RenderSystem::updateMainLodView(CameraComponent& camera, Transform& cameraTransform){
    LodView view = computeLodView(camera, cameraTransform);

    if (view.enabled != rv().lodView.enabled || view.threshold != rv().lodView.threshold ||
        view.origin != rv().lodView.origin || view.perUnit != rv().lodView.perUnit || view.constant != rv().lodView.constant){
        instanceViewsDirty = true;
    }
    rv().lodView = view;
}

unsigned int RenderSystem::selectMeshLod(const MeshComponent& mesh, const LodView* view, float distance, float worldScale) const{
    if (!view || !view->enabled || !mesh.lodEnabled || mesh.numLods < 2)
        return 0;

    float allowed = view->threshold * (view->perUnit * std::max(distance, 0.0f) + view->constant) / std::max(mesh.lodBias, 0.001f);
    unsigned int level = 0;
    for (unsigned int l = 1; l < mesh.numLods; l++){
        if (mesh.lodError[l] * worldScale > allowed)
            break;
        level = l;
    }
    return level;
}

RenderSystem::LodView RenderSystem::shadowLodView(const LightComponent& light, int lightIndex, int cameraIndex, int slot) const{
    LodView view;
    view.enabled = scene->isMeshLodEnabled();
    view.threshold = SHADOW_LOD_TEXELS;

    const LightCamera& shadowCamera = lightCamera(lightIndex, light, cameraIndex);
    const Matrix4& projection = shadowCamera.lightProjectionMatrix;
    float texel = 2.0f / (std::abs(projection[1][1]) * std::max(getShadowAtlasSlotRect(slot).getHeight(), 1.0f));
    if (light.type == LightType::DIRECTIONAL){
        view.constant = texel;
    }else{
        view.origin = shadowCamera.lightViewMatrix.inverse() * Vector3(0, 0, 0);
        view.perUnit = texel;
    }
    return view;
}

// projective atlas slots the shadow pass draws this frame
const std::vector<RenderSystem::ShadowSlot>& RenderSystem::collectShadowSlots(){
    std::vector<ShadowSlot>& slots = shadowSlots;
    slots.clear();
    if (!hasShadows || !hasShadowAtlas)
        return slots;

    auto lights = scene->getComponentArray<LightComponent>();
    int shadowLightCount = (int)lights->size();
    for (int l = 0; l < shadowLightCount; l++){
        LightComponent& light = lights->getComponentFromIndex(l);
        if (light.type == LightType::POINT || light.intensity <= 0 || !light.shadows || light.shadowMapIndex < 0 ||
                !isLightActive(lights->getEntity(l), light))
            continue;
        int numShadowCameras = (light.type == LightType::DIRECTIONAL) ? (int)lightCascades(l, light) : 1;
        for (int c = 0; c < numShadowCameras; c++){
            int slot = light.shadowMapIndex + c;
            if (slot >= 0 && slot < shadowAtlasUsedSlots)
                slots.push_back({slot, &light, l, c});
        }
    }
    return slots;
}

// culled slice of every instanced mesh for the main camera and each shadow slot;
// runs when a list or a camera changed, so a still frame uploads nothing
void RenderSystem::buildInstanceViews(CameraComponent& mainCamera, Transform& mainCameraTransform){
    if (!instanceViewsDirty)
        return;
    instanceViewsDirty = false;

    collectShadowSlots();
    for (InstanceView& view : rv().instanceViews){
        view.data.clear();
    }

    // hidden entities are built too: the passes skip them, but their slices must stay valid
    auto instmeshes = scene->getComponentArray<InstancedMeshComponent>();
    for (int i = 0; i < instmeshes->size(); i++){
        InstancedMeshComponent& instmesh = instmeshes->getComponentFromIndex(i);
        MeshComponent* mesh = scene->findComponent<MeshComponent>(instmeshes->getEntity(i));
        Transform* transform = scene->findComponent<Transform>(instmeshes->getEntity(i));
        if (!mesh || !transform || !mesh->loaded){
            for (InstanceViewRange& range : instmesh.views)
                range = InstanceViewRange();
            continue;
        }

        // the main camera keeps the order where it is the layering: blended batches and 2D
        InstanceOrder order = (mesh->transparent || !sortsByDistance(mainCamera)) ? InstanceOrder::KEEP : InstanceOrder::NEAREST;
        const Vector3& eye = mainCameraTransform.worldPosition;
        const InstanceRenderData* instances = instmesh.renderInstances.data();
        const InstanceBounds* bounds = instmesh.renderBounds.data();
        const unsigned int count = (instmesh.renderBounds.size() < instmesh.numVisible) ? 0 : instmesh.numVisible;
        buildInstanceView(rv().instanceViews[0], instmesh.views[0], instances, bounds, count, instmesh, *mesh, *transform,
            mainCamera.farClip, mainCamera.frustumPlanes, rv().lodView, eye, eye, Vector3::ZERO, order);

        for (const ShadowSlot& shadow : shadowSlots){
            if (!mesh->castShadows){
                instmesh.views[1 + shadow.slot] = InstanceViewRange();
                continue;
            }
            const LightCamera& shadowCamera = lightCamera(shadow.lightIndex, *shadow.light, shadow.cameraIndex);
            LodView lodView = shadowLodView(*shadow.light, shadow.lightIndex, shadow.cameraIndex, shadow.slot);
            Vector3 direction = (shadow.light->type == LightType::DIRECTIONAL) ? shadow.light->worldDirection : Vector3::ZERO;
            buildInstanceView(rv().instanceViews[1 + shadow.slot], instmesh.views[1 + shadow.slot], instances, bounds, count, instmesh, *mesh, *transform,
                shadowCamera.nearFar.y, shadowCamera.frustumPlanes, lodView, eye, lodView.origin, direction, InstanceOrder::NEAREST);
        }
    }

    for (InstanceView& view : rv().instanceViews){
        view.needUpload = !view.data.empty();
    }
    instanceRevision++;
}

// instances inside the frustum and the cull distance from the eye, appended to view with
// their range. NEAREST: nearest first (along direction when given), grouped by level;
// KEEP: list order at full detail; BACK_TO_FRONT: ascending along direction, full detail
void RenderSystem::buildInstanceView(InstanceView& view, InstanceViewRange& range, const InstanceRenderData* instances, const InstanceBounds* instanceBounds, unsigned int count, const InstancedMeshComponent& instmesh, const MeshComponent& mesh, const Transform& transform, const float cameraFar, const Plane frustumPlanes[6], const LodView& lodView, const Vector3& eye, const Vector3& origin, const Vector3& direction, InstanceOrder order, bool limitDistance){
    struct Candidate{
        unsigned int index;
        unsigned int lod;
        float key;
    };
    static std::vector<Candidate> candidates;
    candidates.clear();

    range = InstanceViewRange();
    range.offset = (unsigned int)view.data.size();

    // KEEP already arrives in draw order. Above the exact integer range of a
    // float, retain the existing sort's tie ordering for its rounded index keys.
    const bool keepOrder = order == InstanceOrder::KEEP && count <= (1u << 24);
    if (keepOrder && !instmesh.cullInstances){
        if (count > 0)
            view.data.insert(view.data.end(), instances, instances + count);
        range.lodCount[0] = count;
        return;
    }

    const Vector3& worldScale = transform.worldScale;
    const float entityScale = std::max(std::abs(worldScale.x), std::max(std::abs(worldScale.y), std::abs(worldScale.z)));
    limitDistance = limitDistance && std::isfinite(instmesh.cullDistance) && instmesh.cullDistance > 0.0f;

    for (unsigned int i = 0; i < count; i++){
        const InstanceBounds& bounds = instanceBounds[i];
        const Vector3 center = transform.modelMatrix * bounds.center;
        const float radius = bounds.radius * entityScale;
        if (instmesh.cullInstances){
            if (!isInsideCamera(cameraFar, frustumPlanes, center, radius))
                continue;
            if (limitDistance && (center - eye).length() - radius > instmesh.cullDistance)
                continue;
        }

        if (keepOrder){
            view.data.push_back(instances[i]);
            range.lodCount[0]++;
            continue;
        }

        Candidate candidate;
        candidate.index = i;
        candidate.lod = 0;
        candidate.key = (float)i;
        if (order == InstanceOrder::NEAREST){
            float distance = (center - lodView.origin).length() - radius;
            candidate.lod = selectMeshLod(mesh, &lodView, distance, bounds.scale * entityScale);
            candidate.key = (direction == Vector3::ZERO) ? (center - origin).squaredLength() : center.dotProduct(direction);
        }else if (order == InstanceOrder::BACK_TO_FRONT){
            candidate.key = center.dotProduct(direction);
        }
        candidates.push_back(candidate);
    }

    if (keepOrder)
        return;

    std::sort(candidates.begin(), candidates.end(), [](const Candidate& a, const Candidate& b){
        return a.lod != b.lod ? a.lod < b.lod : a.key < b.key;
    });

    for (const Candidate& candidate : candidates){
        view.data.push_back(instances[candidate.index]);
        range.lodCount[candidate.lod]++;
    }
}

// instances and detail level a pass draws a mesh with; false when there is nothing to draw
bool RenderSystem::selectMeshDraw(Entity entity, MeshComponent& mesh, Transform& transform, InstancedMeshComponent* instmesh, bool ownLod, int instanceView, const LodView* lodView, MeshDraw& draw){
    if (instmesh){
        const InstanceViewRange* range = (instanceView >= 0) ? &instanceRange(entity, *instmesh, instanceView) : nullptr;
        draw.instanceCount = range ? range->count() : instmesh->numVisible;
        draw.instanceFirst = range ? range->offset : 0;
        // unpainted foliage chunks make this common
        if (draw.instanceCount == 0)
            return false;
    }

    if (!ownLod && lodView){
        float worldScale = std::max(std::abs(transform.worldScale.x), std::max(std::abs(transform.worldScale.y), std::abs(transform.worldScale.z)));
        float distance = mesh.worldAABB.isNull() ? 0.0f : mesh.worldAABB.distance(lodView->origin);
        draw.lod = selectMeshLod(mesh, lodView, distance, worldScale);
    }

    return true;
}

// the slice of a view buffer, or the whole component buffer, starting at instance first
void RenderSystem::bindInstances(ObjectRender& render, InstancedMeshComponent& instmesh, int instanceView, unsigned int first){
    BufferRender* buffer = (instanceView >= 0) ? instanceBuffer(instanceView) : instmesh.buffer.getRender();
    render.replaceVertexBuffer(instmesh.buffer.getRender(), buffer, (size_t)first * instmesh.buffer.getStride());
}

// a culled view draws each level bucket of its slice; anything else draws every instance at one level
void RenderSystem::drawSubmeshGeometry(ObjectRender& render, Entity entity, MeshComponent& mesh, unsigned int submeshIndex, InstancedMeshComponent* instmesh, int instanceView, unsigned int lod, unsigned int instanceCount){
    Submesh& submesh = mesh.submeshes[submeshIndex];
    const MeshLodData* lodData = submesh.lodData.get();
    bool lodReady = lodData && lodData->numLevels > 1 && submesh.lodIndices.getRender()->isCreated();

    auto drawLevel = [&](unsigned int level, unsigned int instances, unsigned int first){
        if (instances == 0)
            return;
        if (instmesh)
            bindInstances(render, *instmesh, instanceView, first);
        level = lodReady ? std::min(level, lodData->numLevels - 1) : 0;
        if (level > 0){
            render.setIndexBuffer(submesh.lodIndices.getRender());
            render.draw(lodData->levels[level].indexBase, lodData->levels[level].indexCount, instances);
            render.resetIndexBuffer();
        }else{
            render.draw(0, submesh.vertexCount, instances);
        }
    };

    if (instmesh && instanceView >= 0){
        const InstanceViewRange& range = instanceRange(entity, *instmesh, instanceView);
        unsigned int first = range.offset;
        for (unsigned int l = 0; l < MAX_MESH_LODS; l++){
            drawLevel(l, range.lodCount[l], first);
            first += range.lodCount[l];
        }
    }else{
        drawLevel(lod, instanceCount, 0);
    }
}

// Uploads the dirty CPU-side buffers of a mesh, before the first pass of the frame
// that draws it. Every pass needs this, not only the color one that comes last: a
// rebuilt tilemap reorders its indices, so a shadow or G-buffer pass drawing chunk
// ranges would slice the previous upload with the new offsets. Sokol allows a single
// update per buffer per frame, which the flag already guarantees.
void RenderSystem::updateMeshBuffers(MeshComponent& mesh){
    if (mesh.needUpdateBones){
        const unsigned int size = static_cast<unsigned int>(
            mesh.bonesMatrix.size() * sizeof(Matrix4));
        if (mesh.bonesBuffer.isCreated())
            mesh.bonesBuffer.updateBuffer(size, mesh.bonesMatrix.data());
        if (mesh.bonesTexture.isCreated())
            mesh.bonesTexture.updateTexture(mesh.bonesMatrix.data(), size);
        mesh.needUpdateBones = false;
    }

    if (!mesh.needUpdateBuffer)
        return;

    if (mesh.buffer.getUsage() != BufferUsage::IMMUTABLE)
        mesh.buffer.getRender()->updateBuffer(mesh.buffer.getSize(), mesh.buffer.getData());
    if (mesh.indices.getUsage() != BufferUsage::IMMUTABLE)
        mesh.indices.getRender()->updateBuffer(mesh.indices.getSize(), mesh.indices.getData());
    for (int i = 0; i < mesh.numExternalBuffers; i++){
        if (mesh.eBuffers[i].getUsage() != BufferUsage::IMMUTABLE)
            mesh.eBuffers[i].getRender()->updateBuffer(mesh.eBuffers[i].getSize(), mesh.eBuffers[i].getData());
    }

    mesh.needUpdateBuffer = false;
}

// Once per frame, before any pass: the depth passes take their instance count from here.
void RenderSystem::updateInstanceBuffers(){
    auto instmeshes = scene->getComponentArray<InstancedMeshComponent>();

    for (int i = 0; i < instmeshes->size(); i++){
        InstancedMeshComponent& instmesh = instmeshes->getComponentFromIndex(i);

        if (!instmesh.needUpdateBuffer)
            continue;

        // setData here because component can change order and lose reference
        if (instmesh.numVisible > 0 && !instmesh.renderInstances.empty()){
            instmesh.buffer.setData((unsigned char*)(&instmesh.renderInstances[0]), sizeof(InstanceRenderData)*instmesh.numVisible);
            instmesh.buffer.getRender()->updateBuffer(instmesh.buffer.getSize(), instmesh.buffer.getData());
        }

        instmesh.needUpdateBuffer = false;
    }

    uploadInstanceViews(rv());
}

// the culled lists a view rebuilt, each into its own buffer
void RenderSystem::uploadInstanceViews(RenderView& renderView){
    uploadInstanceViews(renderView.instanceViews, sizeof(renderView.instanceViews) / sizeof(renderView.instanceViews[0]));
}

void RenderSystem::uploadInstanceViews(InstanceView* views, size_t count){
    for (size_t v = 0; v < count; v++){
        InstanceView& view = views[v];
        if (!view.needUpload)
            continue;
        view.needUpload = false;

        const size_t size = view.data.size() * sizeof(InstanceRenderData);
        BufferRender* render = view.buffer.getRender();
        // grows with headroom and never shrinks
        if (render->isCreated() && view.capacity < size)
            render->destroyBuffer();
        if (!render->isCreated()){
            view.capacity = size + size / 2;
            render->createBuffer(view.capacity, nullptr, BufferType::VERTEX_BUFFER, BufferUsage::STREAM);
        }
        view.buffer.setData((unsigned char*)view.data.data(), size);
        render->updateBuffer(size, view.buffer.getData());
    }
}

// Uploads a terrain view's CDLOD node selection, before the first pass of the frame
// that draws it — the same rule as updateMeshBuffers. The depth and G-buffer passes
// draw the main camera's selection and take their instance count from the CPU-side
// buffer, so without this they would draw this frame's node count out of the previous
// frame's node data.
void RenderSystem::updateTerrainNodesBuffer(TerrainView& view){
    if (!view.needUpdateNodesBuffer)
        return;

    for (int s = 0; s < 2; s++){
        view.nodesbuffer[s].getRender()->updateBuffer(view.nodesbuffer[s].getSize(), view.nodesbuffer[s].getData());
    }

    view.needUpdateNodesBuffer = false;
}

bool RenderSystem::drawMesh(Entity entity, MeshComponent& mesh, Transform& transform, CameraComponent& camera, Transform& camTransform, PipelineType pipType, InstancedMeshComponent* instmesh, TerrainComponent* terrain, TilemapComponent* tilemap, int terrainView, int instanceView, const LodView* lodView){
    if (mesh.loaded && !mesh.needReload){

        if (terrain && terrain->needUpdateTexture){
            return false;
        }

        if (!isBatchVisible(entity, mesh, transform, instmesh, instanceView, camera.farClip, camera.frustumPlanes)) {
            return false;
        }

        // Buffers already uploaded this frame by updateInstanceBuffers().
        MeshDraw draw;
        if (!selectMeshDraw(entity, mesh, transform, instmesh, terrain || tilemap, instanceView, lodView, draw)){
            return false;
        }
        unsigned int instanceCount = draw.instanceCount;

        updateMeshBuffers(mesh);
        writeCustomUniforms(mesh);

        TerrainView* terrainNodes = nullptr;
        if (terrain){
            terrainNodes = &terrainViewData(entity, *terrain, terrainView);
            updateTerrainNodesBuffer(*terrainNodes);
        }

        // model, normal and mvp matrices of this pass (one uniform block)
        Matrix4 objectScratch[3];
        const Matrix4* objectBlock = objectMatrices(transform, objectScratch);
        // Transform::normalMatrix is kept only while the scene has lights (and is saved with
        // the scene), but library shaders read it whatever the lighting (outline widths,
        // fresnel, matcap, rim): without lights theirs is made here
        if (!hasLights){
            bool libraryShader = false;
            for (int s = 0; s < mesh.numSubmeshes && !libraryShader; s++){
                libraryShader = mesh.submeshes[s].manifest || (mesh.submeshes[s].extraPass && mesh.submeshes[s].extraPass->loaded);
            }
            if (libraryShader){
                if (objectBlock != objectScratch){
                    objectScratch[0] = objectBlock[0];
                    objectScratch[2] = objectBlock[2];
                }
                objectScratch[1] = objectScratch[0].inverse().transpose();
                objectBlock = objectScratch;
            }
        }

        // the lighting and shadow maps of the camera this pass draws for
        CameraShadows& passLighting = ps();

        // the object's lights (MeshComponent::lights) as the last submesh built them: its
        // lighting and capacity bits
        uint32_t builtLightBits = UINT32_MAX;

        // The extra passes (Material::extraPass) of the whole mesh go around all of its main
        // draws: every Before pass ahead of the first, every After pass behind the last, in
        // submesh order. Interleaved per submesh, a submesh's pass would see the other
        // submeshes of its own object half drawn: a silhouette (Before, depth test greater)
        // shows through the submeshes drawn before it, and a stencil outline (After, outside
        // the stencil its object marks) covers the submeshes that have not marked it yet.
        // a split mesh drawn in one queue: the submeshes of that queue (passSubmeshQueue)
        const SubmeshQueue submeshQueue = passSubmeshQueue;
        auto inQueue = [&](int i){
            return submeshQueue == SubmeshQueue::ALL ||
                (submeshQueue == SubmeshQueue::TRANSPARENT) == mesh.submeshes[i].alphaBlend;
        };
        bool hasBeforePasses = false;
        bool hasAfterPasses = false;
        for (int i = 0; i < mesh.numSubmeshes; i++){
            const SubmeshExtraPass* extra = mesh.submeshes[i].extraPass.get();
            if (extra && extra->loaded && inQueue(i)){
                if (extra->order == MaterialPassOrder::BEFORE)
                    hasBeforePasses = true;
                else
                    hasAfterPasses = true;
            }
        }
        auto drawExtraPasses = [&](MaterialPassOrder order){
            for (int i = 0; i < mesh.numSubmeshes; i++){
                SubmeshExtraPass* extra = mesh.submeshes[i].extraPass.get();
                if (!extra || !extra->loaded || extra->order != order || mesh.submeshes[i].vertexCount == 0 || !inQueue(i))
                    continue;
                if (tilemap && !selectTilemapChunks(*tilemap, i, camera.farClip, camera.frustumPlanes))
                    continue;
                if (order == MaterialPassOrder::BEFORE){
                    // checked once for both draws: rebinding a redrawn render target texture takes
                    // its new version, so the pass rebinding first would hide the change from the
                    // main draw; the main draw is told through its own flag
                    const bool outdated = checkPBRFrabebufferUpdate(mesh.submeshes[i].material);
                    if (mesh.submeshes[i].needUpdateTexture || outdated)
                        extra->needUpdateTexture = true;
                    if (outdated)
                        mesh.submeshes[i].needUpdateTexture = true;
                }
                drawSubmeshExtraPass(entity, mesh, i, transform, pipType, instmesh, instanceView, draw, instanceCount, objectBlock, tilemap, builtLightBits);
            }
        };
        if (hasBeforePasses){
            drawExtraPasses(MaterialPassOrder::BEFORE);
        }

        for (int i = 0; i < mesh.numSubmeshes; i++){
            // an indexed submesh without indices has nothing to draw (a tilemap rect
            // with no tile placed on it). Its texture state is cleared here because
            // gaining indices always reloads the mesh, which reloads the textures.
            if (mesh.submeshes[i].vertexCount == 0){
                mesh.submeshes[i].needUpdateTexture = false;
                continue;
            }
            if (!inQueue(i))
                continue;

            // per-chunk culling: skip the submesh entirely when no chunk is visible
            if (tilemap && !selectTilemapChunks(*tilemap, i, camera.farClip, camera.frustumPlanes))
                continue;

            ObjectRender& render = mesh.submeshes[i].render;

            if (terrain){
                instanceCount = terrainNodes->nodesbuffer[i].getCount();
                // swap in this view's node instance buffer (no-op for view 0)
                render.replaceVertexBuffer(terrain->views[0].nodesbuffer[i].getRender(), terrainNodes->nodesbuffer[i].getRender());
            }

            // checked once for this draw and its After pass, which then rebinds (see above)
            const bool pbrFramebufferOutdated = checkPBRFrabebufferUpdate(mesh.submeshes[i].material);
            SubmeshExtraPass* extraPass = mesh.submeshes[i].extraPass.get();
            if (extraPass && extraPass->loaded && extraPass->order == MaterialPassOrder::AFTER &&
                    (mesh.submeshes[i].needUpdateTexture || pbrFramebufferOutdated)){
                extraPass->needUpdateTexture = true;
            }

            const ShaderLibraryBindings& libraryBindings = mesh.submeshes[i].libraryBindings;
            bool needUpdateFramebuffer = pbrFramebufferOutdated ||
                (!libraryBindings.textures.empty() && shaderTexturesOutdated(libraryBindings, mesh.submeshes[i].material.shaderTextures));

            if (mesh.submeshes[i].needUpdateTexture || needUpdateFramebuffer){
                ShaderData& shaderData = mesh.submeshes[i].shader.get()->shaderData;
                if (loadPBRTextures(mesh.submeshes[i].material, shaderData, mesh.submeshes[i].render, mesh.receiveLights) &&
                        bindShaderTextures(libraryBindings, render, mesh.submeshes[i].material.shaderTextures)){
                    mesh.submeshes[i].needUpdateTexture = false;
                }
            }

            if (!render.beginDraw(pipType, passNoFaceCulling())){
                mesh.needReload = true;
                return false;
            }

            applyFrameBlock(render, libraryBindings);
            applySceneInputs(render, libraryBindings);
            applyEnvironment(render, libraryBindings, mesh, transform);

            if (mesh.submeshes[i].hasIBL){
                TextureRender* probeTexture = &emptyCubeBlack;
                // Probe assignment is per renderable. Its bounds center is a
                // better representative point than the entity origin for meshes
                // whose geometry is offset in local space.
                Vector3 probeSamplePosition = meshProbeSamplePosition(mesh, transform);
                selectReflectionProbe(probeSamplePosition, fs_reflection_probe, probeTexture);
                ShaderData& probeShaderData = mesh.submeshes[i].shader.get()->shaderData;
                render.addTexture(probeShaderData.getTextureIndex(TextureShaderType::REFLECTIONPROBE), ShaderStageType::FRAGMENT, probeTexture);
                if (mesh.submeshes[i].slotFSReflectionProbe != -1){
                    render.applyUniformBlock(mesh.submeshes[i].slotFSReflectionProbe, sizeof(fs_reflection_probe_t), &fs_reflection_probe);
                }
            }

            // the variant's (a mesh waiting for a variant reload keeps its block when the fog goes)
            if (mesh.submeshes[i].slotFSFog != -1){
#ifdef JASPERENGINE_EDITOR
                // exponential fog of density 0: a factor of 1 everywhere, no fog
                static fs_fog_t noFog = {Vector4(0.0f, 0.0f, 0.0f, 1.0f), Vector4(0.0f, 0.0f, 0.0f, 0.0f)};
                render.applyUniformBlock(mesh.submeshes[i].slotFSFog, sizeof(float) * 8, (rv().disableFog) ? &noFog : &fs_fog);
#else
                render.applyUniformBlock(mesh.submeshes[i].slotFSFog, sizeof(float) * 8, &fs_fog);
#endif
            }

            // a secondary view's own reflection, bound for this draw only
            bool viewMirrorBound = false;
            if (mesh.submeshes[i].slotFSMirror != -1){
                const Matrix4* mirrorViewProjection = &mesh.mirrorViewProjection;
#ifdef JASPERENGINE_EDITOR
                if (MirrorView* mirror = passMirror(entity)){
                    mirrorViewProjection = &mirror->viewProjection;
                    ShaderData& mirrorShaderData = mesh.submeshes[i].shader.get()->shaderData;
                    render.addTexture(mirrorShaderData.getTextureIndex(TextureShaderType::BASECOLOR), ShaderStageType::FRAGMENT, &mirror->framebuffer.getRender().getColorTexture());
                    viewMirrorBound = true;
                }
#endif
                render.applyUniformBlock(mesh.submeshes[i].slotFSMirror, sizeof(float) * 16, (void*)mirrorViewProjection);
            }

            // a submesh is lit (uses the lighting block + full PBR params) unless it
            // was compiled as MATERIAL_UNLIT (property bit 0)
            bool submeshLit = !(mesh.submeshes[i].shaderProperties & (1u << 0));
            bool viewShadowMapsBound = false;

            if (submeshLit){
                ShaderData& lightingShaderData = mesh.submeshes[i].shader.get()->shaderData;
                loadSpotMaskTexture(lightingShaderData, render);
                // the object's lights, split for this submesh's lighting mode
                if (mesh.submeshes[i].slotVSObjectLights != -1 || mesh.submeshes[i].slotFSObjectLights != -1){
                    const uint32_t lightBits = mesh.submeshes[i].shaderProperties & ((1u << 3) | (1u << 28) | (1u << 29) | (1u << 30));
                    if (lightBits != builtLightBits){
                        buildObjectLights(mesh, mesh.submeshes[i].shaderProperties, objectLightsBlock);
                        builtLightBits = lightBits;
                    }
                    render.applyUniformBlock(mesh.submeshes[i].slotVSObjectLights, mesh.submeshes[i].sizeVSObjectLights, &objectLightsBlock);
                    render.applyUniformBlock(mesh.submeshes[i].slotFSObjectLights, mesh.submeshes[i].sizeFSObjectLights, &objectLightsBlock);
                }
                if (mesh.submeshes[i].slotFSLitFrame != -1){
                    // a variant with the post colour has its parameters first (lit_frame_block_t)
                    lit_frame_block_t& litFrame = passLitFrameBlocks[mesh.submeshes[i].libraryBindings.postColorBlend];
                    render.applyUniformBlock(mesh.submeshes[i].slotFSLitFrame, mesh.submeshes[i].sizeFSLitFrame,
                        (mesh.submeshes[i].shaderProperties & ShaderPool::getPostColorProperty(ShaderType::MESH)) ? (void*)&litFrame : (void*)&litFrame.lit);
                }
                if (mesh.submeshes[i].slotFSLighting != -1){
                    buildLegacyLighting(mesh, mesh.submeshes[i].shaderProperties, legacyLightingBlock);
                    render.applyUniformBlock(mesh.submeshes[i].slotFSLighting, sizeof(fs_lighting_t), &legacyLightingBlock);
                }
                // the variant's ('Shw'; a mesh waiting for a variant reload keeps its blocks)
                if (mesh.submeshes[i].shaderProperties & (1u << 4)){
                    render.applyUniformBlock(mesh.submeshes[i].slotVSShadows, sizeof(float) * (20 * MAX_SHADOW_ATLAS_SLOTS), &passLighting.vs_shadows);
                    render.applyUniformBlock(mesh.submeshes[i].slotFSShadows, sizeof(fs_shadows_t), &passLighting.fs_shadows);
                    render.applyUniformBlock(mesh.submeshes[i].slotFSPointShadows, sizeof(fs_point_shadows_t), &passLighting.fs_point_shadows);
                    viewShadowMapsBound = bindViewShadowMaps(lightingShaderData, render, mesh.receiveLights, false);
                }
                // USE_SSAO (property bit 21): bind the screen-space AO texture for
                // this view (blurred AO for the main camera, empty white otherwise)
                if (mesh.submeshes[i].shaderProperties & (1u << 21)){
                    ShaderData& ssaoShaderData = mesh.submeshes[i].shader.get()->shaderData;
                    render.addTexture(ssaoShaderData.getTextureIndex(TextureShaderType::SSAOTEXTURE), ShaderStageType::FRAGMENT,
                                      rv().currentSSAOTexture ? rv().currentSSAOTexture : &emptyWhite);
                }
            }

            // USE_LIGHT2D (property bit 22): dedicated 2D light path; composes with
            // both unlit (sprites) and lit submeshes, so it is applied independently
            if (mesh.submeshes[i].shaderProperties & (1u << 22)){
                render.applyUniformBlock(mesh.submeshes[i].slotFSLighting2D, sizeof(fs_lighting2d_t), &fs_lighting2d);
            }

            if (mesh.submeshes[i].hasTextureRect){
                render.applyUniformBlock(mesh.submeshes[i].slotVSSprite, sizeof(float) * 4, &mesh.submeshes[i].textureRect);
            }

            if (mesh.submeshes[i].hasSkinning){
                applySkinningUniform(render, mesh.submeshes[i].slotVSSkinning, mesh, mesh.submeshes[i]);
            }

            if (mesh.submeshes[i].hasMorphTarget){
                if (!mesh.submeshes[i].hasMorphNormal && !mesh.submeshes[i].hasMorphTangent){
                    render.applyUniformBlock(mesh.submeshes[i].slotVSMorphTarget, sizeof(float) * MAX_MORPHTARGETS, &mesh.morphWeights);
                }else{
                    render.applyUniformBlock(mesh.submeshes[i].slotVSMorphTarget, sizeof(float) * MAX_MORPHTARGETS / 2, &mesh.morphWeights);
                }
            }

            if (submeshLit){
                render.applyUniformBlock(mesh.submeshes[i].slotFSParams, sizeof(float) * 12, &mesh.submeshes[i].material);
            }else if (mesh.submeshes[i].shaderProperties & (1u << 24)){
                // Unlit alpha-mask variants include the metallic/roughness padding
                // and alphaCutoff from the shared CPU material prefix.
                render.applyUniformBlock(mesh.submeshes[i].slotFSParams, sizeof(float) * 8, &mesh.submeshes[i].material);
            }else{
                render.applyUniformBlock(mesh.submeshes[i].slotFSParams, sizeof(float) * 4, &mesh.submeshes[i].material);
            }

            if (mesh.submeshes[i].slotVSFade != -1 && instmesh){
                applyInstanceFadeUniform(render, mesh.submeshes[i].slotVSFade, *instmesh, instanceFadeEye(entity, *instmesh, instanceView));
            }

            if (mesh.submeshes[i].slotFSTexCoordSets != -1){
                // per-texture UV set selector (0 = a_texcoord1, 1 = a_texcoord2):
                // set0 = baseColor, metallicRoughness, occlusion, emissive; set1.x = normal
                const Material& mat = mesh.submeshes[i].material;
                int32_t texCoordSets[8] = {
                    mat.baseColorTexCoord, mat.metallicRoughnessTexCoord, mat.occlusionTexCoord, mat.emissiveTexCoord,
                    mat.normalTexCoord, 0, 0, 0
                };
                render.applyUniformBlock(mesh.submeshes[i].slotFSTexCoordSets, sizeof(texCoordSets), &texCoordSets);
            }

            if (terrain){
                // morph from this view's eye position (paired with its node selection)
                terrain->eyePos = terrainNodes->nodesEyePos;
                render.applyUniformBlock(mesh.submeshes[i].slotVSTerrain, sizeof(float) * 8, &(terrain->eyePos));
                applyTerrainLayersUniform(render, mesh.submeshes[i].slotFSTerrainLayers, entity, *terrain);
            }

            //model, normal and mvp matrix
            render.applyUniformBlock(mesh.submeshes[i].slotVSParams, sizeof(float) * 48, (void*)objectBlock);

            applyCustomUniforms(render, mesh.submeshes[i].customVSParams, frameTime, passResolution);
            applyCustomUniforms(render, mesh.submeshes[i].customFSParams, frameTime, passResolution);

            if (tilemap && !tilemapDrawRanges.empty()){
                if (instmesh){
                    bindInstances(render, *instmesh, instanceView, draw.instanceFirst);
                }
                for (const TilemapDrawRange& range : tilemapDrawRanges){
                    render.draw(range.offset, range.count, instanceCount);
                }
            }else{
                drawSubmeshGeometry(render, entity, mesh, i, instmesh, instanceView, draw.lod, instanceCount);
            }

            // the frame keeps its own bindings for the next draw()
            if (viewShadowMapsBound){
                bindViewShadowMaps(mesh.submeshes[i].shader.get()->shaderData, render, mesh.receiveLights, true);
            }
            if (viewMirrorBound){
                ShaderData& mirrorShaderData = mesh.submeshes[i].shader.get()->shaderData;
                TextureRender* baseColor = mesh.submeshes[i].material.baseColorTexture.getRender(&emptyWhite);
                render.addTexture(mirrorShaderData.getTextureIndex(TextureShaderType::BASECOLOR), ShaderStageType::FRAGMENT, baseColor ? baseColor : &emptyWhite);
            }
        }

        if (hasAfterPasses){
            drawExtraPasses(MaterialPassOrder::AFTER);
        }
    }

    return true;
}

bool RenderSystem::drawMeshDepth(Entity entity, MeshComponent& mesh, Transform& transform, const float cameraFar, const Plane frustumPlanes[6], vs_depth_t vsDepthParams, InstancedMeshComponent* instmesh, TerrainComponent* terrain, TilemapComponent* tilemap, bool forSSAO, PipelineType pipelineType, int instanceView, const LodView* lodView){
    // shadow passes only draw casters; the SSAO depth pre-pass draws every opaque mesh
    if (mesh.loaded && !mesh.needReload && (mesh.castShadows || forSSAO)){

        if (terrain && terrain->needUpdateTexture){
            return false;
        }

        if (!isBatchVisible(entity, mesh, transform, instmesh, instanceView, cameraFar, frustumPlanes)) {
            return false;
        }

        MeshDraw draw;
        if (!selectMeshDraw(entity, mesh, transform, instmesh, terrain || tilemap, instanceView, lodView, draw)){
            return false;
        }
        unsigned int instanceCount = draw.instanceCount;

        updateMeshBuffers(mesh);
        writeCustomUniforms(mesh);

        // the depth pass renders the view camera's selection (view 0 in draw())
        TerrainView* terrainNodes = nullptr;
        if (terrain){
            terrainNodes = &terrainViewData(entity, *terrain, mainTerrainView());
            updateTerrainNodesBuffer(*terrainNodes);
        }

        for (int i = 0; i < mesh.numSubmeshes; i++){
            if (mesh.submeshes[i].vertexCount == 0){
                mesh.submeshes[i].needUpdateDepthTexture = false;
                continue;
            }

            // a shader whose @state says shadows=off casts none (its depth program may
            // not even be loaded when no other depth pass needs it)
            if (!forSSAO && mesh.submeshes[i].manifest && mesh.submeshes[i].manifestState.shadows == ShaderShadowCasting::OFF)
                continue;
            // and what draws nothing visible (a stencil mask) is no surface for the pre-passes,
            // nor what draws only where a mask marked the stencil (its depth would be everywhere)
            if (forSSAO && (mesh.submeshes[i].skipsScreenEffects || mesh.submeshes[i].stencilTested))
                continue;
            // a split mesh's opaque submeshes only (renderOpaqueDepth, the camera pre-pass)
            if (passSubmeshQueue == SubmeshQueue::OPAQUE && mesh.submeshes[i].alphaBlend)
                continue;

            if (tilemap && !selectTilemapChunks(*tilemap, i, cameraFar, frustumPlanes))
                continue;

            ObjectRender& depthRender = mesh.submeshes[i].depthRender;

            if (mesh.submeshes[i].needUpdateDepthTexture &&
                (mesh.submeshes[i].depthShaderProperties & (1u << 0))){
                ShaderData& depthShaderData = mesh.submeshes[i].depthShader.get()->shaderData;
                if (loadDepthTexture(mesh.submeshes[i].material, depthShaderData, mesh.submeshes[i].depthRender)){
                    mesh.submeshes[i].needUpdateDepthTexture = false;
                }
            }else if (mesh.submeshes[i].needUpdateDepthTexture){
                mesh.submeshes[i].needUpdateDepthTexture = false;
            }

            const ShaderLibraryBindings& depthBindings = mesh.submeshes[i].depthLibraryBindings;
            if (!depthBindings.textures.empty() && (mesh.submeshes[i].needUpdateTexture ||
                    shaderTexturesOutdated(depthBindings, mesh.submeshes[i].material.shaderTextures))){
                bindShaderTextures(depthBindings, depthRender, mesh.submeshes[i].material.shaderTextures);
            }

            if (!depthRender.beginDraw(pipelineType, passNoFaceCulling())){
                // a submesh without this depth program is left out of a pre-pass (its scene
                // gives every mesh one when a pass needs them all: updateSceneInputPrograms);
                // reloading would not give it one and would leave the mesh out of its colour
                // pass this frame; a mesh waiting for its new variant gets it then
                if (forSSAO || mesh.needVariantReload)
                    continue;
                mesh.needReload = true;
                return false;
            }

            applyFrameBlock(depthRender, depthBindings);

            if (terrain){
                instanceCount = terrainNodes->nodesbuffer[i].getCount();
                depthRender.replaceVertexBuffer(terrain->views[0].nodesbuffer[i].getRender(), terrainNodes->nodesbuffer[i].getRender());
            }

            //model, mvp matrix
            depthRender.applyUniformBlock(mesh.submeshes[i].slotVSDepthParams, std::min((unsigned int)sizeof(vs_depth_t), mesh.submeshes[i].sizeVSDepthParams), &vsDepthParams);

            applyCustomUniforms(depthRender, mesh.submeshes[i].customVSDepthParams, frameTime, passResolution);
            applyCustomUniforms(depthRender, mesh.submeshes[i].customFSDepthParams, frameTime, passResolution);

            if (mesh.submeshes[i].slotVSDepthFade != -1 && instmesh){
                applyInstanceFadeUniform(depthRender, mesh.submeshes[i].slotVSDepthFade, *instmesh, instanceFadeEye(entity, *instmesh, instanceView));
            }
            if (mesh.submeshes[i].slotVSDepthSprite != -1){
                depthRender.applyUniformBlock(mesh.submeshes[i].slotVSDepthSprite, sizeof(float) * 4, &mesh.submeshes[i].textureRect);
            }

            if (mesh.submeshes[i].slotFSDepthMaterial != -1){
                const Material& material = mesh.submeshes[i].material;
                Vector4 alphaParams(material.baseColorFactor.w, material.alphaCutoff, 0.0f, 0.0f);
                depthRender.applyUniformBlock(mesh.submeshes[i].slotFSDepthMaterial, sizeof(Vector4), &alphaParams);
            }

            if (mesh.submeshes[i].hasSkinning){
                applySkinningUniform(depthRender, mesh.submeshes[i].slotVSDepthSkinning, mesh, mesh.submeshes[i]);
            }
            if (mesh.submeshes[i].hasMorphTarget){
                if (!mesh.submeshes[i].hasMorphNormal && !mesh.submeshes[i].hasMorphTangent){
                    depthRender.applyUniformBlock(mesh.submeshes[i].slotVSDepthMorphTarget, sizeof(float) * MAX_MORPHTARGETS, &mesh.morphWeights);
                }else{
                    depthRender.applyUniformBlock(mesh.submeshes[i].slotVSDepthMorphTarget, sizeof(float) * MAX_MORPHTARGETS / 2, &mesh.morphWeights);
                }
            }

            if (terrain){
                // shadow depth renders the view camera's selection
                terrain->eyePos = terrainNodes->nodesEyePos;
                depthRender.applyUniformBlock(mesh.submeshes[i].slotVSDepthTerrain, sizeof(float) * 8, &(terrain->eyePos));
            }

            if (tilemap && !tilemapDrawRanges.empty()){
                if (instmesh){
                    bindInstances(depthRender, *instmesh, instanceView, draw.instanceFirst);
                }
                for (const TilemapDrawRange& range : tilemapDrawRanges){
                    depthRender.draw(range.offset, range.count, instanceCount);
                }
            }else{
                drawSubmeshGeometry(depthRender, entity, mesh, i, instmesh, instanceView, draw.lod, instanceCount);
            }
        }
    }

    return true;
}

void RenderSystem::loadSSAO(){
    if (ssaoLoaded)
        return;

    // the SSAO fullscreen shaders may still be building (async in the editor)
    ssaoShader = ShaderPool::get(ShaderType::SSAO, 0);
    ssaoBlurShader = ShaderPool::get(ShaderType::SSAO_BLUR, 0);
    if (!ssaoShader || !ssaoShader->isCreated() || !ssaoBlurShader || !ssaoBlurShader->isCreated())
        return;

    // hemisphere sample kernel, weighted toward the origin (more near-field samples).
    // It is constant, so write it straight into the uniform block once here.
    std::mt19937 rng(1337);
    std::uniform_real_distribution<float> dist01(0.0f, 1.0f);
    std::uniform_real_distribution<float> distm11(-1.0f, 1.0f);

    for (int i = 0; i < SSAO_KERNEL_SIZE; i++){
        Vector3 sample(distm11(rng), distm11(rng), dist01(rng)); // +Z hemisphere
        sample.normalize();
        sample *= dist01(rng);
        float scale = (float)i / (float)SSAO_KERNEL_SIZE;
        scale = 0.1f + (scale * scale) * (1.0f - 0.1f); // lerp(0.1, 1.0, scale^2)
        sample *= scale;
        fs_ssao.kernel[i] = Vector4(sample.x, sample.y, sample.z, 0.0f);
    }

    // 4x4 rotation-noise texture: random vectors in the tangent XY plane (z=0).
    // Pixel layout matches the empty textures: 0xAABBGGRR in memory.
    const int noiseDim = 4;
    uint32_t noisePixels[noiseDim * noiseDim];
    for (int i = 0; i < noiseDim * noiseDim; i++){
        uint8_t r = (uint8_t)((distm11(rng) * 0.5f + 0.5f) * 255.0f);
        uint8_t g = (uint8_t)((distm11(rng) * 0.5f + 0.5f) * 255.0f);
        noisePixels[i] = (0xFFu << 24) | (0x80u << 16) | ((uint32_t)g << 8) | (uint32_t)r;
    }

    void* ndata[6]; size_t nsize[6];
    ndata[0] = (void*)noisePixels; nsize[0] = noiseDim * noiseDim * 4;
    ssaoNoiseTexture.createTexture(
            "ssao|noise", noiseDim, noiseDim, ColorFormat::RGBA, TextureType::TEXTURE_2D, 1, ndata, nsize,
            TextureFilter::NEAREST, TextureFilter::NEAREST, TextureWrap::REPEAT, TextureWrap::REPEAT);

    // fullscreen ssao pass (vertexless: no attributes added)
    ssaoRender.beginLoad(PrimitiveType::TRIANGLES);
    ssaoRender.setShader(ssaoShader.get());
    ssaoSlotParams = ssaoShader.get()->shaderData.getUniformBlockIndex(UniformBlockType::SSAO_FS_PARAMS);
    if (!ssaoRender.endLoad(PIP_EFFECT, false, true, CullingMode::BACK, WindingOrder::CCW))
        return;

    // fullscreen blur pass
    ssaoBlurRender.beginLoad(PrimitiveType::TRIANGLES);
    ssaoBlurRender.setShader(ssaoBlurShader.get());
    ssaoBlurSlotParams = ssaoBlurShader.get()->shaderData.getUniformBlockIndex(UniformBlockType::SSAO_BLUR_FS_PARAMS);
    if (!ssaoBlurRender.endLoad(PIP_EFFECT, false, true, CullingMode::BACK, WindingOrder::CCW))
        return;

    ssaoLoaded = true;
}

void RenderSystem::destroySSAO(){
    if (ssaoLoaded){
        ssaoRender.destroy();
        ssaoBlurRender.destroy();
        ssaoNoiseTexture.destroyTexture();
    }
    if (ssaoShader){
        ssaoShader.reset();
        ShaderPool::remove(ShaderType::SSAO, 0);
    }
    if (ssaoBlurShader){
        ssaoBlurShader.reset();
        ShaderPool::remove(ShaderType::SSAO_BLUR, 0);
    }

    rv().ssaoDepthFramebuffer.destroy();
    rv().ssaoFramebuffer.destroy();
    rv().ssaoBlurFramebuffer.destroy();

    rv().ssaoWidth = 0;
    rv().ssaoHeight = 0;
    ssaoSlotParams = -1;
    ssaoBlurSlotParams = -1;
    ssaoLoaded = false;
    rv().currentSSAOTexture = NULL;
}

bool RenderSystem::ensureSSAOFramebuffers(unsigned int width, unsigned int height){
    if (width == 0 || height == 0)
        return false;

    if (rv().ssaoDepthFramebuffer.isCreated() && rv().ssaoWidth == width && rv().ssaoHeight == height)
        return true;

    rv().ssaoDepthFramebuffer.destroy();
    rv().ssaoFramebuffer.destroy();
    rv().ssaoBlurFramebuffer.destroy();

    // depth pre-pass target: packed depth in color, sampled point-exact
    rv().ssaoDepthFramebuffer.setWidth(width);
    rv().ssaoDepthFramebuffer.setHeight(height);
    rv().ssaoDepthFramebuffer.setMinFilter(TextureFilter::NEAREST);
    rv().ssaoDepthFramebuffer.setMagFilter(TextureFilter::NEAREST);
    rv().ssaoDepthFramebuffer.setWrapU(TextureWrap::CLAMP_TO_EDGE);
    rv().ssaoDepthFramebuffer.setWrapV(TextureWrap::CLAMP_TO_EDGE);
    // drawn only by PIP_DEPTH pipelines, which ignore stencil
    rv().ssaoDepthFramebuffer.setDepthStencil(false);
    rv().ssaoDepthFramebuffer.create();

    rv().ssaoFramebuffer.setWidth(width);
    rv().ssaoFramebuffer.setHeight(height);
    rv().ssaoFramebuffer.setMinFilter(TextureFilter::NEAREST);
    rv().ssaoFramebuffer.setMagFilter(TextureFilter::NEAREST);
    rv().ssaoFramebuffer.setWrapU(TextureWrap::CLAMP_TO_EDGE);
    rv().ssaoFramebuffer.setWrapV(TextureWrap::CLAMP_TO_EDGE);
    // fullscreen passes only (PIP_EFFECT): no stencil (see FramebufferDepth::DEPTH)
    rv().ssaoFramebuffer.setDepthAttachment(FramebufferDepth::DEPTH);
    rv().ssaoFramebuffer.create();

    // blurred AO is sampled bilinearly by the lit meshes
    rv().ssaoBlurFramebuffer.setWidth(width);
    rv().ssaoBlurFramebuffer.setHeight(height);
    rv().ssaoBlurFramebuffer.setMinFilter(TextureFilter::LINEAR);
    rv().ssaoBlurFramebuffer.setMagFilter(TextureFilter::LINEAR);
    rv().ssaoBlurFramebuffer.setWrapU(TextureWrap::CLAMP_TO_EDGE);
    rv().ssaoBlurFramebuffer.setWrapV(TextureWrap::CLAMP_TO_EDGE);
    rv().ssaoBlurFramebuffer.setDepthAttachment(FramebufferDepth::DEPTH);
    rv().ssaoBlurFramebuffer.create();

    rv().ssaoWidth = width;
    rv().ssaoHeight = height;

    return rv().ssaoDepthFramebuffer.isCreated() && rv().ssaoFramebuffer.isCreated() && rv().ssaoBlurFramebuffer.isCreated();
}

TextureRender* RenderSystem::renderSSAO(CameraComponent& camera, bool fixedResolution, TextureRender* sharedDepth, Transform* perCameraTransform){
    loadSSAO();
    if (!ssaoLoaded)
        return nullptr;

    unsigned int w = (unsigned int)targetViewRect().getWidth();
    unsigned int h = (unsigned int)targetViewRect().getHeight();
    if (fixedResolution){
        // the main color pass renders into the fixed-resolution target, so the
        // AO buffer must match it (meshes convert gl_FragCoord with these sizes)
        w = scene->getFixedResolutionWidth();
        h = scene->getFixedResolutionHeight();
    }
    if (!ensureSSAOFramebuffers(w, h))
        return nullptr;

    Matrix4 renderProj = camera.projectionMatrix;

    // --- 1. depth + normal source: reuse the SSR G-buffer (packed depth in color[0],
    // view-space normal in color[1]) when available, otherwise run the SSAO depth
    // pre-pass and reconstruct the normal from the depth gradient in the shader ---
    bool useGBufferNormal = (sharedDepth != nullptr);
    TextureRender* depthTexture = sharedDepth;
    TextureRender* normalTexture; // G-buffer normal when shared, else an unused placeholder
    if (!depthTexture){
        renderDepthPrePass(camera, perCameraTransform);
        depthTexture = &rv().ssaoDepthFramebuffer.getRender().getColorTexture();
        normalTexture = depthTexture; // placeholder binding; not sampled (params.w = 0)
    }else{
        normalTexture = &rv().gbufferFramebuffer.getRender().getColorAttachmentTexture(1);
    }

    // --- 2. ssao pass (kernel was filled once in loadSSAO) ---
    fs_ssao.projection = renderProj;
    fs_ssao.invProjection = renderProj.inverse();
    fs_ssao.params = Vector4(scene->getSSAORadius(), scene->getSSAOBias(), scene->getSSAOIntensity(), useGBufferNormal ? 1.0f : 0.0f);
    // xy = noise tiling (4x4), zw = inverse depth-texture size (neighbour taps for normals)
    fs_ssao.noiseScale = Vector4((float)w / 4.0f, (float)h / 4.0f, 1.0f / (float)w, 1.0f / (float)h);

    ssaoPassRender.setClearColor(Vector4(1.0, 1.0, 1.0, 1.0));
    ssaoPassRender.setLabel("ssao");
    ssaoPassRender.startRenderPass(&rv().ssaoFramebuffer.getRender());
    if (ssaoRender.beginDraw(PIP_EFFECT)){
        ShaderData& sd = ssaoShader.get()->shaderData;
        ssaoRender.addTexture(sd.getTextureIndex(TextureShaderType::DEPTHTEXTURE), ShaderStageType::FRAGMENT, depthTexture);
        ssaoRender.addTexture(sd.getTextureIndex(TextureShaderType::GBUFFERTEXTURE), ShaderStageType::FRAGMENT, normalTexture);
        ssaoRender.addTexture(sd.getTextureIndex(TextureShaderType::NOISETEXTURE), ShaderStageType::FRAGMENT, &ssaoNoiseTexture);
        ssaoRender.applyUniformBlock(ssaoSlotParams, sizeof(fs_ssao_t), &fs_ssao);
        ssaoRender.draw(0, 3, 1);
    }
    ssaoPassRender.endRenderPass();

    // --- 3. blur pass ---
    fs_ssao_blur.texelSize = Vector4(1.0f / (float)w, 1.0f / (float)h, 0.0, 0.0);

    ssaoPassRender.setClearColor(Vector4(1.0, 1.0, 1.0, 1.0));
    ssaoPassRender.setLabel("ssao blur");
    ssaoPassRender.startRenderPass(&rv().ssaoBlurFramebuffer.getRender());
    if (ssaoBlurRender.beginDraw(PIP_EFFECT)){
        ShaderData& bd = ssaoBlurShader.get()->shaderData;
        ssaoBlurRender.addTexture(bd.getTextureIndex(TextureShaderType::SSAOTEXTURE), ShaderStageType::FRAGMENT, &rv().ssaoFramebuffer.getRender().getColorTexture());
        ssaoBlurRender.applyUniformBlock(ssaoBlurSlotParams, sizeof(fs_ssao_blur_t), &fs_ssao_blur);
        ssaoBlurRender.draw(0, 3, 1);
    }
    ssaoPassRender.endRenderPass();

    rv().currentSSAOTexture = &rv().ssaoBlurFramebuffer.getRender().getColorTexture();
    return depthTexture;
}

void RenderSystem::renderDepthPrePass(CameraComponent& camera, Transform* perCameraTransform){
    renderOpaqueDepth(camera, rv().ssaoDepthFramebuffer, rv().ssaoWidth, rv().ssaoHeight, 0, &rv().lodView, perCameraTransform);
}

void RenderSystem::renderOpaqueDepth(CameraComponent& camera, Framebuffer& target, unsigned int width, unsigned int height,
                                     int instanceView, const LodView* lodView, Transform* perCameraTransform){
    // Logical (un-flipped) matrices: flipY reverses winding but PIP_DEPTH does not
    // compensate (only PIP_RTT reverses winding on GL). The depth buffer therefore
    // stays in logical orientation; SSAO/SSR consumers flip Y on GL when sampling.
    Matrix4 renderVP = camera.viewProjectionMatrix;
    // A mirror camera's reflection turns its faces' winding over (its colour pass culls with
    // PIP_RTT_INVERT): mirrored in x the depth pass culls as the colour pass does, and the
    // scene inputs read it mirrored back (fillSceneInputs)
    if (camera.invertCulling){
        static const Matrix4 flipXMatrix = Matrix4::scaleMatrix(Vector3(-1, 1, 1));
        renderVP = flipXMatrix * renderVP;
    }

    ssaoPassRender.setClearColor(Vector4(1.0, 1.0, 1.0, 1.0)); // background -> depth ~1.0
    ssaoPassRender.setLabel("depth pre-pass");
    ssaoPassRender.startRenderPass(&target.getRender());
    passResolution = Vector2((float)width, (float)height);

    auto transforms = scene->getComponentArray<Transform>();
    for (int i = 0; i < transforms->size(); i++){
        Transform& transform = transforms->getComponentFromIndex(i);
        Entity entity = transforms->getEntity(i);
        Signature signature = scene->getSignature(entity);

        if (!signature.test(scene->getComponentId<MeshComponent>()))
            continue;

        MeshComponent& mesh = scene->getComponent<MeshComponent>(entity);
        // (a split mesh: its opaque submeshes, as the opaque queue draws them)
        if (!transform.visible || (mesh.transparent && !mesh.splitQueues) || !isRenderActive(entity, signature) || !isLayerVisible(entity, camera.cullingMask))
            continue;
        // what the colour pass leaves out (a render-to-texture camera does not draw the
        // objects showing its own image)
        if (samplesCameraTarget(camera, mesh))
            continue;

        // a camera drawn after another makes its own billboard matrices (as renderCameraPass)
        if (perCameraTransform){
            updateMVP(i, transform, camera, *perCameraTransform);
        }

        InstancedMeshComponent* instmesh = scene->findComponent<InstancedMeshComponent>(entity);
        TerrainComponent* terrain = scene->findComponent<TerrainComponent>(entity);
        TilemapComponent* tilemap = scene->findComponent<TilemapComponent>(entity);

        const Matrix4& model = objectModel(transform);
        vs_depth_t params = {model, renderVP, renderVP * model};
        passSubmeshQueue = mesh.transparent ? SubmeshQueue::OPAQUE : SubmeshQueue::ALL;
        drawMeshDepth(entity, mesh, transform, camera.farClip, camera.frustumPlanes, params, instmesh, terrain, tilemap, true, PIP_DEPTH, instanceView, lodView);
    }
    passSubmeshQueue = SubmeshQueue::ALL;
    ssaoPassRender.endRenderPass();
}

void RenderSystem::setSceneInputUser(Entity entity, bool points, uint32_t inputs){
    std::unordered_map<Entity, uint32_t>& users = points ? sceneInputPoints : sceneInputMeshes;
    inputs &= ShaderRequire_SceneInputs;
    if (inputs){
        auto found = users.find(entity);
        if (found != users.end() && found->second == inputs)
            return;
        users[entity] = inputs;
    }else if (users.empty() || users.erase(entity) == 0){
        return;
    }
    uint32_t usage = 0;
    for (const auto& user : sceneInputMeshes)
        usage |= user.second;
    for (const auto& user : sceneInputPoints)
        usage |= user.second;
    sceneInputUsage = usage;
}

void RenderSystem::predictSceneInputUsers(){
    // Registered at load only, the first reader of the scene depth would load after the other
    // meshes, which then lack a depth program for its pre-pass: one frame without them, and
    // the next frame every mesh reloads with one. So the meshes and points about to load are
    // registered first (once per load, nothing while everything is loaded); their load sets
    // the final value (a shader that fails reads nothing).
    auto meshes = scene->getComponentArray<MeshComponent>();
    const std::string* sceneMeshShader = nullptr;
    for (int i = 0; i < meshes->size(); i++){
        MeshComponent& mesh = meshes->getComponentFromIndex(i);
        if ((mesh.loadCalled && !mesh.needReload) || mesh.sceneInputsPredicted)
            continue;
        mesh.sceneInputsPredicted = true;
        if (!sceneMeshShader)
            sceneMeshShader = &scene->getDefaultCustomShader(ShaderType::MESH);
        uint32_t inputs = 0;
        for (unsigned int s = 0; s < mesh.numSubmeshes; s++){
            bool opaqueQueue = false;
            inputs |= getSubmeshSceneInputs(mesh, mesh.submeshes[s], *sceneMeshShader, opaqueQueue);
        }
        if (inputs){
            const Entity entity = meshes->getEntity(i);
            auto found = sceneInputMeshes.find(entity);
            setSceneInputUser(entity, false, inputs | (found != sceneInputMeshes.end() ? found->second : 0));
        }
    }
    auto pointsArray = scene->getComponentArray<PointsComponent>();
    for (int i = 0; i < pointsArray->size(); i++){
        PointsComponent& points = pointsArray->getComponentFromIndex(i);
        if ((points.loadCalled && !points.needReload) || points.sceneInputsPredicted)
            continue;
        points.sceneInputsPredicted = true;
        const std::string& shader = points.customShader.empty() ? scene->getDefaultCustomShader(ShaderType::POINTS) : points.customShader;
        std::shared_ptr<const ShaderManifest> manifest = findManifest(shader);
        if (!manifest)
            continue;
        const uint32_t inputs = manifest->resolveRequire(ShaderReference::getEnabledFeatures(shader, points.shaderFeatures)) & ShaderRequire_SceneInputs;
        if (inputs){
            const Entity entity = pointsArray->getEntity(i);
            auto found = sceneInputPoints.find(entity);
            setSceneInputUser(entity, true, inputs | (found != sceneInputPoints.end() ? found->second : 0));
        }
    }
}

void RenderSystem::updateSceneInputPrograms(){
    // kept this many updates (about 10 s at 60 fps) after the last reader went: a game making
    // and destroying readers (an effect, a pickup) reloads every mesh once, not at each
    static const uint32_t HOLD_UPDATES = 600;

    // a scene reading the scene depth (phase 3), or a post pass sampling the depth, gives every
    // mesh a depth program for the depth pre-pass (a mesh without one is left out of it)
    const bool depthReaders = (sceneInputUsage & ShaderRequire_SceneDepth) != 0 ||
        (postProcessLoaded && postProcessNeedsDepth && !scene->getPostProcessPasses().empty());
    bool depthPrograms = sceneDepthPrograms;
    if (depthReaders){
        depthPrograms = true;
        sceneDepthIdleUpdates = 0;
    }else if (sceneDepthPrograms && ++sceneDepthIdleUpdates > HOLD_UPDATES){
        depthPrograms = false;
    }
    if (depthPrograms != sceneDepthPrograms){
        sceneDepthPrograms = depthPrograms;
        sceneDepthIdleUpdates = 0;
        needReloadMeshVariants();
    }

    // the same for the scene camera's offscreen pipelines of an exported build (a colour reader:
    // updateSwapchainRedirect), which reload every object when they change
    if (sceneInputUsage & ShaderRequire_SceneColor){
        sceneColorReaders = true;
        sceneColorIdleUpdates = 0;
    }else if (sceneColorReaders && ++sceneColorIdleUpdates > HOLD_UPDATES){
        sceneColorReaders = false;
        sceneColorIdleUpdates = 0;
    }
}

void RenderSystem::setGBufferDepthMismatch(Entity entity, bool mismatch){
    if (mismatch)
        gbufferDepthMismatch.insert(entity);
    else if (!gbufferDepthMismatch.empty())
        gbufferDepthMismatch.erase(entity);
}

bool RenderSystem::hasVisibleGBufferDepthMismatch(CameraComponent& camera, int instanceView){
    for (Entity entity : gbufferDepthMismatch){
        if (!scene->isEntityCreated(entity))
            continue;
        MeshComponent* mesh = scene->findComponent<MeshComponent>(entity);
        Transform* transform = scene->findComponent<Transform>(entity);
        if (!mesh || !transform || !mesh->loaded || mesh->needReload || !transform->visible)
            continue;
        if (!isRenderActive(entity, scene->getSignature(entity)) || !isLayerVisible(entity, camera.cullingMask) || samplesCameraTarget(camera, *mesh))
            continue;
        if (isBatchVisible(entity, *mesh, *transform, scene->findComponent<InstancedMeshComponent>(entity), instanceView, camera.farClip, camera.frustumPlanes))
            return true;
    }
    return false;
}

RenderSystem::SceneInputDetails RenderSystem::getSceneInputDetails(){
    SceneInputDetails usage;
    usage.reads = sceneInputUsage;
    usage.readers = (uint32_t)(sceneInputMeshes.size() + sceneInputPoints.size());
    usage.passes = lastSceneInputStats.passes;
    usage.depthPrePasses = lastSceneInputStats.depthPrePasses;
    usage.sharedDepth = lastSceneInputStats.sharedDepth;
    usage.colorCopies = lastSceneInputStats.colorCopies;
    usage.targets = (uint32_t)primaryView.sceneInputTargets.size() + (primaryView.sceneColorRedirect.isCreated() ? 1u : 0u);
#ifdef JASPERENGINE_EDITOR
    for (const auto& entry : secondaryViews)
        usage.targets += (uint32_t)entry.second->sceneInputTargets.size();
#endif
    usage.depthPrograms = sceneDepthPrograms;
    usage.colorRedirect = sceneColorRedirect;
    return usage;
}

uint32_t RenderSystem::findPassSceneInputs(CameraComponent& camera, int instanceView){
    // the same visibility the draws below test (drawMesh, drawPoints)
    uint32_t inputs = 0;
    for (const auto& user : sceneInputMeshes){
        if ((inputs | user.second) == inputs || !scene->isEntityCreated(user.first))
            continue;
        const Entity entity = user.first;
        MeshComponent* mesh = scene->findComponent<MeshComponent>(entity);
        Transform* transform = scene->findComponent<Transform>(entity);
        if (!mesh || !transform || !mesh->loaded || mesh->needReload || !transform->visible)
            continue;
        if (!isRenderActive(entity, scene->getSignature(entity)) || !isLayerVisible(entity, camera.cullingMask) || samplesCameraTarget(camera, *mesh))
            continue;
        if (!isBatchVisible(entity, *mesh, *transform, scene->findComponent<InstancedMeshComponent>(entity), instanceView, camera.farClip, camera.frustumPlanes))
            continue;
        inputs |= user.second;
    }
    for (const auto& user : sceneInputPoints){
        if ((inputs | user.second) == inputs || !scene->isEntityCreated(user.first))
            continue;
        const Entity entity = user.first;
        PointsComponent* points = scene->findComponent<PointsComponent>(entity);
        Transform* transform = scene->findComponent<Transform>(entity);
        if (!points || !transform || !points->loaded || points->numVisible == 0 || !transform->visible)
            continue;
        if (!isRenderActive(entity, scene->getSignature(entity)) || !isLayerVisible(entity, camera.cullingMask) || samplesCameraTarget(camera, points->texture))
            continue;
        inputs |= user.second;
    }
    return inputs;
}

bool RenderSystem::drawsSceneColorReader(Entity entity, Signature signature, Transform& transform, CameraComponent& camera, int instanceView){
    // the visibility its draw below tests (renderCameraPass, drawMesh, drawPoints)
    if (signature.test(scene->getComponentId<MeshComponent>())){
        MeshComponent& mesh = scene->getComponent<MeshComponent>(entity);
        if (!(mesh.sceneInputs & ShaderRequire_SceneColor) || !mesh.loaded || mesh.needReload || !transform.visible)
            return false;
        if (!isRenderActive(entity, signature) || !isLayerVisible(entity, camera.cullingMask) || samplesCameraTarget(camera, mesh))
            return false;
        return isBatchVisible(entity, mesh, transform, scene->findComponent<InstancedMeshComponent>(entity), instanceView, camera.farClip, camera.frustumPlanes);
    }
    if (signature.test(scene->getComponentId<PointsComponent>())){
        PointsComponent& points = scene->getComponent<PointsComponent>(entity);
        if (!(points.sceneInputs & ShaderRequire_SceneColor) || !points.loaded || points.numVisible == 0 || !transform.visible)
            return false;
        return isRenderActive(entity, signature) && isLayerVisible(entity, camera.cullingMask) && !samplesCameraTarget(camera, points.texture);
    }
    return false;
}

RenderSystem::SceneInputTarget* RenderSystem::ensureSceneInputTarget(bool color, unsigned int width, unsigned int height){
    if (width == 0 || height == 0)
        return nullptr;
    RenderView& view = rv();
    // the view's draw this is (counted from 1: a target made in it is never unused in it)
    const uint64_t viewDraw = view.sceneInputDraws + 1;
    // one target per size and kind: cameras of different sizes keep theirs from frame to
    // frame instead of resizing a shared one
    for (std::unique_ptr<SceneInputTarget>& target : view.sceneInputTargets){
        if (target->color == color && target->width == width && target->height == height && target->framebuffer.isCreated()){
            target->lastUsed = drawCount;
            target->lastViewDraw = viewDraw;
            return target.get();
        }
    }
    // A size no target has: one of this kind used neither in this draw of the view nor in the
    // one before is of a size it no longer draws at (a window or camera target resized), so it
    // goes now rather than some seconds later: a window dragged for a while would otherwise
    // pile up a pair of targets per frame. Sizes in use every frame (several cameras) stay.
    view.sceneInputTargets.erase(std::remove_if(view.sceneInputTargets.begin(), view.sceneInputTargets.end(),
        [&](const std::unique_ptr<SceneInputTarget>& target){
            return target->color == color && target->lastViewDraw + 1 < viewDraw;
        }), view.sceneInputTargets.end());
    std::unique_ptr<SceneInputTarget> target = std::make_unique<SceneInputTarget>();
    target->color = color;
    target->width = width;
    target->height = height;
    target->lastUsed = drawCount;
    target->lastViewDraw = viewDraw;
    Framebuffer& framebuffer = target->framebuffer;
    framebuffer.setWidth(width);
    framebuffer.setHeight(height);
    // packed depth is read texel-exact; the colour copy is filtered (refraction offsets)
    const TextureFilter filter = color ? TextureFilter::LINEAR : TextureFilter::NEAREST;
    framebuffer.setMinFilter(filter);
    framebuffer.setMagFilter(filter);
    framebuffer.setWrapU(TextureWrap::CLAMP_TO_EDGE);
    framebuffer.setWrapV(TextureWrap::CLAMP_TO_EDGE);
    // drawn by PIP_DEPTH (the depth pre-pass) or PIP_EFFECT (the copy): depth only, no stencil
    framebuffer.setDepthAttachment(FramebufferDepth::DEPTH);
    framebuffer.create();
    if (!framebuffer.isCreated())
        return nullptr;
    sceneInputStats.targetsCreated++;
    view.sceneInputTargets.push_back(std::move(target));
    return view.sceneInputTargets.back().get();
}

void RenderSystem::releaseSceneInputTargets(RenderView& view, bool all){
    // kept while a camera keeps drawing a requiring object, freed some seconds after the
    // last such pass (an object out of view for a moment keeps them)
    static const uint64_t KEEP_DRAWS = 180;
    view.sceneInputTargets.erase(std::remove_if(view.sceneInputTargets.begin(), view.sceneInputTargets.end(),
        [&](const std::unique_ptr<SceneInputTarget>& target){
            return all || drawCount > target->lastUsed + KEEP_DRAWS;
        }), view.sceneInputTargets.end());
}

bool RenderSystem::ensureSceneColorRedirect(unsigned int width, unsigned int height){
    if (width == 0 || height == 0)
        return false;
    Framebuffer& framebuffer = rv().sceneColorRedirect;
    if (framebuffer.isCreated() && framebuffer.getWidth() == width && framebuffer.getHeight() == height)
        return true;
    framebuffer.destroy();
    framebuffer.setWidth(width);
    framebuffer.setHeight(height);
    // blitted 1:1 to the swapchain, and the source of the (half-resolution) colour copy
    framebuffer.setMinFilter(TextureFilter::LINEAR);
    framebuffer.setMagFilter(TextureFilter::LINEAR);
    framebuffer.setWrapU(TextureWrap::CLAMP_TO_EDGE);
    framebuffer.setWrapV(TextureWrap::CLAMP_TO_EDGE);
    framebuffer.create();
    if (framebuffer.isCreated())
        sceneInputStats.targetsCreated++;
    return framebuffer.isCreated();
}

void RenderSystem::loadSceneColorCopy(){
    if (sceneColorCopyLoaded)
        return;
    // the blit shader, into a copy target (depth-only attachment, never tested); asked again
    // until it is built (each get() moves an asynchronous build on)
    sceneColorCopyShader = ShaderPool::get(ShaderType::BLIT, 0);
    if (!sceneColorCopyShader || !sceneColorCopyShader->isCreated())
        return;
    sceneColorCopyRender.beginLoad(PrimitiveType::TRIANGLES);
    sceneColorCopyRender.setShader(sceneColorCopyShader.get());
    sceneColorCopySlotParams = sceneColorCopyShader.get()->shaderData.getUniformBlockIndex(UniformBlockType::BLIT_FS_PARAMS);
    if (!sceneColorCopyRender.endLoad(PIP_EFFECT, false, true, CullingMode::BACK, WindingOrder::CCW))
        return;
    sceneColorCopyLoaded = true;
}

TextureRender* RenderSystem::copySceneColor(TextureRender* source, const Rect& viewport, unsigned int targetWidth, unsigned int targetHeight){
    loadSceneColorCopy();
    if (!sceneColorCopyLoaded || !source || targetWidth == 0 || targetHeight == 0)
        return nullptr;

    const unsigned int width = (unsigned int)viewport.getWidth();
    const unsigned int height = (unsigned int)viewport.getHeight();
    const bool full = scene->getSceneColorScale() == SceneColorScale::FULL;
    SceneInputTarget* target = ensureSceneInputTarget(true, full ? width : std::max(1u, width / 2), full ? height : std::max(1u, height / 2));
    if (!target)
        return nullptr;

    // the viewport's part of the source, rows as stored: GL counts them from the bottom (the
    // viewport's own origin), the other backends from the top. The copy keeps that order.
    const float rowStart = Engine::isOpenGL() ? viewport.getY() : (float)targetHeight - viewport.getY() - viewport.getHeight();
    fs_blit_t copyParams;
    copyParams.params = Vector4(viewport.getWidth() / (float)targetWidth, viewport.getX() / (float)targetWidth,
                                viewport.getHeight() / (float)targetHeight, rowStart / (float)targetHeight);

    sceneInputPassRender.setClearColor(Vector4(0.0f, 0.0f, 0.0f, 1.0f));
    sceneInputPassRender.setLabel("scene colour copy");
    sceneInputPassRender.startRenderPass(&target->framebuffer.getRender());
    if (sceneColorCopyRender.beginDraw(PIP_EFFECT)){
        ShaderData& shaderData = sceneColorCopyShader.get()->shaderData;
        sceneColorCopyRender.addTexture(shaderData.getTextureIndex(TextureShaderType::SCENECOLORTEXTURE), ShaderStageType::FRAGMENT, source);
        sceneColorCopyRender.applyUniformBlock(sceneColorCopySlotParams, sizeof(fs_blit_t), &copyParams);
        sceneColorCopyRender.draw(0, 3, 1);
    }
    sceneInputPassRender.endRenderPass();
    sceneInputStats.colorCopies++;
    return &target->framebuffer.getRender().getColorTexture();
}

void RenderSystem::fillSceneInputs(const CameraComponent& camera, const Rect& viewport, unsigned int targetHeight, bool flipped,
                                   unsigned int copyWidth, unsigned int copyHeight){
    // the clip space the depth pre-pass draws (the camera's, without the GL target flip);
    // the projection out of the view-projection, so a mirror's oblique near plane is in it
    const Matrix4 inverseView = camera.viewMatrix.inverse();
    const Matrix4 inverseProjection = (camera.viewProjectionMatrix * inverseView).inverse();
    passSceneBlock.inverseViewProjection = inverseView * inverseProjection;
    // distance along the view axis = -view.z / view.w, view = inverse projection * ndc
    const Vector4 rowZ = inverseProjection.row(2);
    passSceneBlock.inverseProjectionZ = Vector4(-rowZ.x, -rowZ.y, -rowZ.z, -rowZ.w);
    passSceneBlock.inverseProjectionW = inverseProjection.row(3);

    // gl_FragCoord.y counts rows from the first one stored: the bottom on GL, the top
    // elsewhere. Whether the camera's top (ndc +1) is at that first row depends on the
    // flip of the pass (offscreen GL passes and non-GL probe faces draw upside down).
    const bool gl = Engine::isOpenGL();
    const float width = std::max(viewport.getWidth(), 1.0f);
    const float height = std::max(viewport.getHeight(), 1.0f);
    const float rowStart = gl ? viewport.getY() : (float)targetHeight - viewport.getY() - viewport.getHeight();
    const bool topFirst = gl ? flipped : !flipped;
    passSceneBlock.screenParams = Vector4(1.0f / width, topFirst ? -1.0f / height : 1.0f / height,
                                          -viewport.getX() / width, topFirst ? 1.0f + rowStart / height : -rowStart / height);
    // the copy keeps the rows of its source
    passSceneBlock.colorParams = Vector4(topFirst ? -1.0f : 1.0f, topFirst ? 1.0f : 0.0f,
                                         copyWidth ? 1.0f / (float)copyWidth : 0.0f, copyHeight ? 1.0f / (float)copyHeight : 0.0f);
    // the depth pre-pass draws unflipped: its first row is ndc -1 on GL, +1 elsewhere
    // (depth_util.glsl); a mirror camera's draws it mirrored in x (renderOpaqueDepth)
    const bool mirrored = camera.invertCulling;
    passSceneBlock.depthParams = Vector4(mirrored ? -1.0f : 1.0f, gl ? 1.0f : -1.0f, mirrored ? 1.0f : 0.0f, gl ? 0.0f : 1.0f);
}

bool RenderSystem::ensureGBufferFramebuffer(unsigned int width, unsigned int height){
    if (width == 0 || height == 0)
        return false;

    if (rv().gbufferFramebuffer.isCreated() && rv().gbufferFramebuffer.getWidth() == width && rv().gbufferFramebuffer.getHeight() == height)
        return true;

    rv().gbufferFramebuffer.destroy();

    // MRT: color[0] = packed depth (point-sampled like the depth pre-pass),
    //      color[1] = view-space normal (oct .rg) + roughness (.b) + metallic (.a),
    //      color[2] = linear base color (.rgb) + hasIBL flag (.a)
    ColorFormat formats[3] = { ColorFormat::RGBA, ColorFormat::RGBA, ColorFormat::RGBA };
    rv().gbufferFramebuffer.setColorAttachments(3, formats);
    rv().gbufferFramebuffer.setWidth(width);
    rv().gbufferFramebuffer.setHeight(height);
    rv().gbufferFramebuffer.setMinFilter(TextureFilter::NEAREST);
    rv().gbufferFramebuffer.setMagFilter(TextureFilter::NEAREST);
    rv().gbufferFramebuffer.setWrapU(TextureWrap::CLAMP_TO_EDGE);
    rv().gbufferFramebuffer.setWrapV(TextureWrap::CLAMP_TO_EDGE);
    // drawn only by PIP_GBUFFER pipelines, which ignore stencil
    rv().gbufferFramebuffer.setDepthStencil(false);
    rv().gbufferFramebuffer.create();

    return rv().gbufferFramebuffer.isCreated();
}

bool RenderSystem::drawMeshGBuffer(Entity entity, MeshComponent& mesh, Transform& transform, const float cameraFar, const Plane frustumPlanes[6], vs_gbuffer_t vsGBufferParams, bool hasLocalProbe, InstancedMeshComponent* instmesh, TerrainComponent* terrain, TilemapComponent* tilemap, int instanceView, const LodView* lodView){
    if (!mesh.loaded || mesh.needReload)
        return true;

    if (terrain && terrain->needUpdateTexture){
        return false;
    }

    if (!isBatchVisible(entity, mesh, transform, instmesh, instanceView, cameraFar, frustumPlanes)) {
        return false;
    }

    MeshDraw draw;
    if (!selectMeshDraw(entity, mesh, transform, instmesh, terrain || tilemap, instanceView, lodView, draw)){
        return false;
    }
    unsigned int instanceCount = draw.instanceCount;

    updateMeshBuffers(mesh);

    // the G-buffer pass renders the view camera's selection (view 0 in draw())
    TerrainView* terrainNodes = nullptr;
    if (terrain){
        terrainNodes = &terrainViewData(entity, *terrain, mainTerrainView());
        updateTerrainNodesBuffer(*terrainNodes);
    }

    for (int i = 0; i < mesh.numSubmeshes; i++){
        // the G-buffer shader/render only exist when SSR was enabled at load time; a
        // mesh loaded before SSR was toggled on is skipped until it reloads
        if (!mesh.submeshes[i].gbufferShader)
            continue;

        // a stencil mask is no surface to reflect (see Submesh::skipsScreenEffects)
        if (mesh.submeshes[i].skipsScreenEffects)
            continue;

        if (mesh.submeshes[i].vertexCount == 0){
            mesh.submeshes[i].needUpdateGBufferTexture = false;
            continue;
        }

        if (tilemap && !selectTilemapChunks(*tilemap, i, cameraFar, frustumPlanes))
            continue;

        ObjectRender& gbufferRender = mesh.submeshes[i].gbufferRender;

        if (mesh.submeshes[i].needUpdateGBufferTexture){
            ShaderData& gbufferShaderData = mesh.submeshes[i].gbufferShader.get()->shaderData;
            if (loadGBufferTextures(mesh.submeshes[i].material, gbufferShaderData, mesh.submeshes[i].gbufferRender)){
                mesh.submeshes[i].needUpdateGBufferTexture = false;
            }
        }

        if (!gbufferRender.beginDraw(PIP_GBUFFER, passNoFaceCulling())){
            mesh.needReload = true;
            return false;
        }

        gbufferRender.applyUniformBlock(mesh.submeshes[i].slotVSGBufferParams, sizeof(vs_gbuffer_t), &vsGBufferParams);

        fs_gbuffer_material_t mat;
        // IBL source encoding: 0 = none, 0.5 = sky, 1 = local probe. The SSR
        // composite can exactly replace sky IBL; local probes need a deferred
        // cubemap array to be reconstructed per pixel, so they use additive SSR.
        float iblSource = mesh.submeshes[i].hasIBL ? (hasLocalProbe ? 1.0f : 0.5f) : 0.0f;
        const Material& material = mesh.submeshes[i].material;
        const float alphaCutoff = usesAlphaMask(material, mesh.submeshes[i].textureShadow)
            ? material.alphaCutoff
            : -1.0f;
        mat.params = Vector4(material.roughnessFactor, material.metallicFactor, iblSource, alphaCutoff);
        mat.baseColorFactor = mesh.submeshes[i].material.baseColorFactor;
        gbufferRender.applyUniformBlock(mesh.submeshes[i].slotFSGBufferMaterial, sizeof(fs_gbuffer_material_t), &mat);

        if (mesh.submeshes[i].hasSkinning){
            applySkinningUniform(gbufferRender, mesh.submeshes[i].slotVSGBufferSkinning, mesh, mesh.submeshes[i]);
        }
        if (mesh.submeshes[i].hasMorphTarget){
            if (!mesh.submeshes[i].hasMorphNormal && !mesh.submeshes[i].hasMorphTangent){
                gbufferRender.applyUniformBlock(mesh.submeshes[i].slotVSGBufferMorphTarget, sizeof(float) * MAX_MORPHTARGETS, &mesh.morphWeights);
            }else{
                gbufferRender.applyUniformBlock(mesh.submeshes[i].slotVSGBufferMorphTarget, sizeof(float) * MAX_MORPHTARGETS / 2, &mesh.morphWeights);
            }
        }

        if (terrain){
            terrain->eyePos = terrainNodes->nodesEyePos;
            gbufferRender.applyUniformBlock(mesh.submeshes[i].slotVSGBufferTerrain, sizeof(float) * 8, &(terrain->eyePos));
            applyTerrainLayersUniform(gbufferRender, mesh.submeshes[i].slotFSGBufferTerrainLayers, entity, *terrain);
            instanceCount = terrainNodes->nodesbuffer[i].getCount();
            gbufferRender.replaceVertexBuffer(terrain->views[0].nodesbuffer[i].getRender(), terrainNodes->nodesbuffer[i].getRender());
        }

        if (tilemap && !tilemapDrawRanges.empty()){
            if (instmesh){
                bindInstances(gbufferRender, *instmesh, instanceView, draw.instanceFirst);
            }
            for (const TilemapDrawRange& range : tilemapDrawRanges){
                gbufferRender.draw(range.offset, range.count, instanceCount);
            }
        }else{
            drawSubmeshGeometry(gbufferRender, entity, mesh, i, instmesh, instanceView, draw.lod, instanceCount);
        }
    }

    return true;
}

void RenderSystem::renderGBufferPass(CameraComponent& camera){
    // Same logical orientation as renderDepthPrePass: PIP_GBUFFER (like PIP_DEPTH)
    // does not reverse winding on GL, so color[0] depth matches the depth pre-pass
    // and SSR's existing Y-flip logic applies unchanged. color[1] carries the
    // view-space normal (octahedral) + roughness + metallic at the same texel layout.
    Matrix4 renderVP = camera.viewProjectionMatrix;
    Matrix4 viewMatrix = camera.viewMatrix;

    gbufferPassRender.setClearColor(Vector4(1.0, 1.0, 1.0, 1.0)); // color[0] background -> depth ~1.0
    gbufferPassRender.setLabel("gbuffer");
    gbufferPassRender.startRenderPass(&rv().gbufferFramebuffer.getRender());

    auto transforms = scene->getComponentArray<Transform>();
    for (int i = 0; i < transforms->size(); i++){
        Transform& transform = transforms->getComponentFromIndex(i);
        Entity entity = transforms->getEntity(i);
        Signature signature = scene->getSignature(entity);

        if (!signature.test(scene->getComponentId<MeshComponent>()))
            continue;

        MeshComponent& mesh = scene->getComponent<MeshComponent>(entity);
        if (!transform.visible || mesh.transparent || !isRenderActive(entity, signature) || !isLayerVisible(entity, camera.cullingMask))
            continue;

        InstancedMeshComponent* instmesh = scene->findComponent<InstancedMeshComponent>(entity);
        TerrainComponent* terrain = scene->findComponent<TerrainComponent>(entity);
        TilemapComponent* tilemap = scene->findComponent<TilemapComponent>(entity);

        fs_reflection_probe_t probeParams;
        TextureRender* probeTexture = nullptr;
        Vector3 probeSamplePosition = meshProbeSamplePosition(mesh, transform);
        bool hasLocalProbe = selectReflectionProbe(probeSamplePosition, probeParams, probeTexture)
            && probeParams.position_weight.w > 0.001f;

        // view-space normal matrix = transpose(inverse(view * model))
        const Matrix4& model = objectModel(transform);
        Matrix4 viewModel = viewMatrix * model;
        Matrix4 normalMatrix = viewModel.inverse().transpose();

        vs_gbuffer_t params = {model, renderVP, normalMatrix};
        drawMeshGBuffer(entity, mesh, transform, camera.farClip, camera.frustumPlanes, params, hasLocalProbe, instmesh, terrain, tilemap, 0, &rv().lodView);
    }
    gbufferPassRender.endRenderPass();
}

void RenderSystem::loadSSR(){
    if (ssrLoaded)
        return;

    // fullscreen SSR shaders may still be building (async in the editor)
    ssrShader = ShaderPool::get(ShaderType::SSR, 0);
    ssrBlurShader = ShaderPool::get(ShaderType::SSR_BLUR, 0);
    compositeShader = ShaderPool::get(ShaderType::COMPOSITE, 0);
    if (!ssrShader || !ssrShader->isCreated() || !ssrBlurShader || !ssrBlurShader->isCreated() || !compositeShader || !compositeShader->isCreated())
        return;

    // fullscreen ssr pass (vertexless: no attributes added)
    ssrRender.beginLoad(PrimitiveType::TRIANGLES);
    ssrRender.setShader(ssrShader.get());
    ssrSlotParams = ssrShader.get()->shaderData.getUniformBlockIndex(UniformBlockType::SSR_FS_PARAMS);
    if (!ssrRender.endLoad(PIP_EFFECT, false, true, CullingMode::BACK, WindingOrder::CCW))
        return;

    // fullscreen glossy blur pass
    ssrBlurRender.beginLoad(PrimitiveType::TRIANGLES);
    ssrBlurRender.setShader(ssrBlurShader.get());
    ssrBlurSlotParams = ssrBlurShader.get()->shaderData.getUniformBlockIndex(UniformBlockType::SSR_BLUR_FS_PARAMS);
    if (!ssrBlurRender.endLoad(PIP_EFFECT, false, true, CullingMode::BACK, WindingOrder::CCW))
        return;

    // composite targets either an offscreen framebuffer (PIP_RTT) or the swapchain
    // (PIP_DEFAULT), so build both pipeline variants.
    compositeRender.beginLoad(PrimitiveType::TRIANGLES);
    compositeRender.setShader(compositeShader.get());
    compositeSlotParams = compositeShader.get()->shaderData.getUniformBlockIndex(UniformBlockType::COMPOSITE_FS_PARAMS);
    if (!compositeRender.endLoad(PIP_RTT | PIP_DEFAULT, false, true, CullingMode::BACK, WindingOrder::CCW))
        return;

    ssrLoaded = true;
}

void RenderSystem::destroySSR(){
    if (ssrLoaded){
        ssrRender.destroy();
        ssrBlurRender.destroy();
        compositeRender.destroy();
    }
    if (ssrShader){
        ssrShader.reset();
        ShaderPool::remove(ShaderType::SSR, 0);
    }
    if (ssrBlurShader){
        ssrBlurShader.reset();
        ShaderPool::remove(ShaderType::SSR_BLUR, 0);
    }
    if (compositeShader){
        compositeShader.reset();
        ShaderPool::remove(ShaderType::COMPOSITE, 0);
    }

    rv().gbufferFramebuffer.destroy();
    rv().sceneColorFramebuffer.destroy();
    rv().ssrFramebuffer.destroy();
    rv().ssrBlurFramebuffer.destroy();

    rv().ssrWidth = 0;
    rv().ssrHeight = 0;
    ssrSlotParams = -1;
    ssrBlurSlotParams = -1;
    compositeSlotParams = -1;
    ssrLoaded = false;
}

bool RenderSystem::ensureSSRFramebuffers(unsigned int width, unsigned int height){
    if (width == 0 || height == 0)
        return false;

    if (rv().sceneColorFramebuffer.isCreated() && rv().ssrWidth == width && rv().ssrHeight == height)
        return true;

    rv().sceneColorFramebuffer.destroy();
    rv().ssrFramebuffer.destroy();
    rv().ssrBlurFramebuffer.destroy();

    // offscreen opaque scene color (sampled bilinearly by ssr.frag); has a depth
    // attachment so the redirected main color pass can depth-test normally.
    rv().sceneColorFramebuffer.setWidth(width);
    rv().sceneColorFramebuffer.setHeight(height);
    rv().sceneColorFramebuffer.setMinFilter(TextureFilter::LINEAR);
    rv().sceneColorFramebuffer.setMagFilter(TextureFilter::LINEAR);
    rv().sceneColorFramebuffer.setWrapU(TextureWrap::CLAMP_TO_EDGE);
    rv().sceneColorFramebuffer.setWrapV(TextureWrap::CLAMP_TO_EDGE);
    rv().sceneColorFramebuffer.create();

    // reflection color + mask
    rv().ssrFramebuffer.setWidth(width);
    rv().ssrFramebuffer.setHeight(height);
    rv().ssrFramebuffer.setMinFilter(TextureFilter::LINEAR);
    rv().ssrFramebuffer.setMagFilter(TextureFilter::LINEAR);
    rv().ssrFramebuffer.setWrapU(TextureWrap::CLAMP_TO_EDGE);
    rv().ssrFramebuffer.setWrapV(TextureWrap::CLAMP_TO_EDGE);
    // fullscreen passes only (PIP_EFFECT): no stencil (see FramebufferDepth::DEPTH)
    rv().ssrFramebuffer.setDepthAttachment(FramebufferDepth::DEPTH);
    rv().ssrFramebuffer.create();

    // glossy-blurred reflection (sampled by the composite when blur > 0)
    rv().ssrBlurFramebuffer.setWidth(width);
    rv().ssrBlurFramebuffer.setHeight(height);
    rv().ssrBlurFramebuffer.setMinFilter(TextureFilter::LINEAR);
    rv().ssrBlurFramebuffer.setMagFilter(TextureFilter::LINEAR);
    rv().ssrBlurFramebuffer.setWrapU(TextureWrap::CLAMP_TO_EDGE);
    rv().ssrBlurFramebuffer.setWrapV(TextureWrap::CLAMP_TO_EDGE);
    rv().ssrBlurFramebuffer.setDepthAttachment(FramebufferDepth::DEPTH);
    rv().ssrBlurFramebuffer.create();

    rv().ssrWidth = width;
    rv().ssrHeight = height;

    return rv().sceneColorFramebuffer.isCreated() && rv().ssrFramebuffer.isCreated() && rv().ssrBlurFramebuffer.isCreated();
}

void RenderSystem::renderSSR(CameraComponent& camera, FramebufferRender* destination){
    unsigned int w = rv().ssrWidth;
    unsigned int h = rv().ssrHeight;
    if (w == 0 || h == 0)
        return;

    // The depth buffer is in logical orientation; the offscreen scene color was
    // rendered with the flip an offscreen target uses, so on GL the two differ by a
    // Y flip (isRenderingFlipped, always true on GL here).
    float flipGL = isRenderingFlipped(camera) ? 1.0f : 0.0f;
    Matrix4 renderProj = camera.projectionMatrix;

    // --- 1. ssr pass: march the depth buffer, sample offscreen scene color ---
    fs_ssr.projection = renderProj;
    fs_ssr.invProjection = renderProj.inverse();
    fs_ssr.params = Vector4(scene->getSSRMaxDistance(), scene->getSSRThickness(), scene->getSSRIntensity(), (float)scene->getSSRMaxSteps());
    // misc.w = glossy-blur amount, used only to scale the march jitter (0 => sharp, no jitter)
    fs_ssr.misc = Vector4(1.0f / (float)w, 1.0f / (float)h, flipGL, scene->getSSRBlur());

    ssrPassRender.setClearColor(Vector4(0.0, 0.0, 0.0, 0.0));
    ssrPassRender.setLabel("ssr");
    ssrPassRender.startRenderPass(&rv().ssrFramebuffer.getRender());
    if (ssrRender.beginDraw(PIP_EFFECT)){
        ShaderData& sd = ssrShader.get()->shaderData;
        ssrRender.addTexture(sd.getTextureIndex(TextureShaderType::DEPTHTEXTURE), ShaderStageType::FRAGMENT, &rv().gbufferFramebuffer.getRender().getColorAttachmentTexture(0));
        ssrRender.addTexture(sd.getTextureIndex(TextureShaderType::GBUFFERTEXTURE), ShaderStageType::FRAGMENT, &rv().gbufferFramebuffer.getRender().getColorAttachmentTexture(1));
        ssrRender.addTexture(sd.getTextureIndex(TextureShaderType::SCENECOLORTEXTURE), ShaderStageType::FRAGMENT, &rv().sceneColorFramebuffer.getRender().getColorTexture());
        ssrRender.applyUniformBlock(ssrSlotParams, sizeof(fs_ssr_t), &fs_ssr);
        ssrRender.draw(0, 3, 1);
    }
    ssrPassRender.endRenderPass();

    // --- 2. optional glossy blur pass (premultiplied) ---
    // ssrBlur in [0..1] maps to a pixel spread radius; 0 keeps sharp mirror
    // reflections and skips the pass entirely. The blur ramps smoothly with the
    // radius (the shader scales its tap offsets by it), so there is no hard step.
    const float ssrBlurMaxRadius = 24.0f;
    float blurRadius = scene->getSSRBlur() * ssrBlurMaxRadius;
    TextureRender* reflectionTexture = &rv().ssrFramebuffer.getRender().getColorTexture();
    if (scene->getSSRBlur() > 0.001f && scene->getSSRDebugMode() == 0){
        // blurRadius is the MAX radius (full roughness); the blur shader scales it
        // per-pixel by the G-buffer roughness. w = flip (gbuffer is in depth space).
        fs_ssr_blur.params = Vector4(1.0f / (float)w, 1.0f / (float)h, blurRadius, flipGL);

        ssrPassRender.setClearColor(Vector4(0.0, 0.0, 0.0, 0.0));
        ssrPassRender.setLabel("ssr blur");
        ssrPassRender.startRenderPass(&rv().ssrBlurFramebuffer.getRender());
        if (ssrBlurRender.beginDraw(PIP_EFFECT)){
            ShaderData& bd = ssrBlurShader.get()->shaderData;
            ssrBlurRender.addTexture(bd.getTextureIndex(TextureShaderType::SSRTEXTURE), ShaderStageType::FRAGMENT, &rv().ssrFramebuffer.getRender().getColorTexture());
            ssrBlurRender.addTexture(bd.getTextureIndex(TextureShaderType::GBUFFERTEXTURE), ShaderStageType::FRAGMENT, &rv().gbufferFramebuffer.getRender().getColorAttachmentTexture(1));
            ssrBlurRender.applyUniformBlock(ssrBlurSlotParams, sizeof(fs_ssr_blur_t), &fs_ssr_blur);
            ssrBlurRender.draw(0, 3, 1);
        }
        ssrPassRender.endRenderPass();

        reflectionTexture = &rv().ssrBlurFramebuffer.getRender().getColorTexture();
    }

    // --- 3. composite pass: energy-conserving SSR-over-IBL -> real destination ---
    // The composite recomputes the IBL specular term per pixel (for surfaces flagged
    // hasIBL in the G-buffer) and replaces it with the SSR reflection, so the env
    // reflection already in the scene is not double-counted.
    fs_composite.invProjection = renderProj.inverse();
    fs_composite.invView = camera.viewMatrix.inverse();

    // the GL swapchain keeps row 0 at the bottom while the offscreen sources do not,
    // so a swapchain destination samples them flipped (renderBlit does the same)
    float flipDest = (!destination && Engine::isOpenGL()) ? 1.0f : 0.0f;
    fs_composite.params = Vector4(scene->getSSRIntensity(), flipGL, (float)scene->getSSRDebugMode(), flipDest);

    // environment color/rotation, matching mesh.frag's lighting.envColor; the prefiltered
    // GGX map comes from the sky (fall back to a black cube when there is no IBL sky)
    fs_composite.envColor = Vector4(0.0, 0.0, 0.0, 0.0);
    TextureRender* prefilteredTexture = &emptyCubeBlack;
    {
        auto skys = scene->getComponentArray<SkyboxComponent>();
        if (skys->size() > 0){
            SkyboxComponent& sky = skys->getComponentFromIndex(0);
            if (sky.prefilteredMap){
                prefilteredTexture = sky.prefilteredMap.get();
                Vector3 linColor = Color::sRGBToLinear(Vector3(sky.color.x, sky.color.y, sky.color.z));
                fs_composite.envColor = Vector4(linColor.x, linColor.y, linColor.z, Angle::defaultToRad(sky.rotation));
            }
        }
    }

    ssrPassRender.setLabel("ssr composite");
    ssrPassRender.setClearColor(scene->getBackgroundColor());
    PipelineType compositePip;
    if (destination){
        ssrPassRender.startRenderPass(destination);
        compositePip = PIP_RTT;
    }else{
        ssrPassRender.startRenderPass();
        ssrPassRender.applyViewport(targetViewRect());
        compositePip = PIP_DEFAULT;
    }
    if (compositeRender.beginDraw(compositePip)){
        ShaderData& cd = compositeShader.get()->shaderData;
        compositeRender.addTexture(cd.getTextureIndex(TextureShaderType::SCENECOLORTEXTURE), ShaderStageType::FRAGMENT, &rv().sceneColorFramebuffer.getRender().getColorTexture());
        compositeRender.addTexture(cd.getTextureIndex(TextureShaderType::SSRTEXTURE), ShaderStageType::FRAGMENT, reflectionTexture);
        compositeRender.addTexture(cd.getTextureIndex(TextureShaderType::DEPTHTEXTURE), ShaderStageType::FRAGMENT, &rv().gbufferFramebuffer.getRender().getColorAttachmentTexture(0));
        compositeRender.addTexture(cd.getTextureIndex(TextureShaderType::GBUFFERTEXTURE), ShaderStageType::FRAGMENT, &rv().gbufferFramebuffer.getRender().getColorAttachmentTexture(1));
        compositeRender.addTexture(cd.getTextureIndex(TextureShaderType::GBUFFERALBEDOTEXTURE), ShaderStageType::FRAGMENT, &rv().gbufferFramebuffer.getRender().getColorAttachmentTexture(2));
        compositeRender.addTexture(cd.getTextureIndex(TextureShaderType::PREFILTEREDMAP), ShaderStageType::FRAGMENT, prefilteredTexture);
        compositeRender.applyUniformBlock(compositeSlotParams, sizeof(fs_composite_t), &fs_composite);
        compositeRender.draw(0, 3, 1);
    }
    ssrPassRender.endRenderPass();
}

void RenderSystem::loadBlit(){
    if (blitLoaded)
        return;

    // the fullscreen blit shader may still be building (async in the editor)
    blitShader = ShaderPool::get(ShaderType::BLIT, 0);
    if (!blitShader || !blitShader->isCreated())
        return;

    // the blit targets either the Engine framebuffer (PIP_RTT, editor) or the
    // swapchain (PIP_DEFAULT, exported builds), so build both pipeline variants.
    blitRender.beginLoad(PrimitiveType::TRIANGLES);
    blitRender.setShader(blitShader.get());
    blitSlotParams = blitShader.get()->shaderData.getUniformBlockIndex(UniformBlockType::BLIT_FS_PARAMS);
    if (!blitRender.endLoad(PIP_RTT | PIP_DEFAULT, false, true, CullingMode::BACK, WindingOrder::CCW))
        return;

    blitLoaded = true;
}

void RenderSystem::destroyBlit(){
    if (blitLoaded){
        blitRender.destroy();
    }
    if (blitShader){
        blitShader.reset();
        ShaderPool::remove(ShaderType::BLIT, 0);
    }

    rv().fixedResFramebuffer.destroy();

    rv().fixedResWidth = 0;
    rv().fixedResHeight = 0;
    blitSlotParams = -1;
    blitLoaded = false;
}

bool RenderSystem::ensureFixedResFramebuffer(unsigned int width, unsigned int height, TextureFilter filter){
    if (width == 0 || height == 0)
        return false;

    if (rv().fixedResFramebuffer.isCreated() && rv().fixedResWidth == width && rv().fixedResHeight == height
            && rv().fixedResFramebuffer.getMinFilter() == filter)
        return true;

    rv().fixedResFramebuffer.destroy();

    // offscreen scene color at the fixed game resolution; the min/mag filter is
    // what the upscale blit samples with (nearest = crisp pixels, linear = smooth)
    rv().fixedResFramebuffer.setWidth(width);
    rv().fixedResFramebuffer.setHeight(height);
    rv().fixedResFramebuffer.setMinFilter(filter);
    rv().fixedResFramebuffer.setMagFilter(filter);
    rv().fixedResFramebuffer.setWrapU(TextureWrap::CLAMP_TO_EDGE);
    rv().fixedResFramebuffer.setWrapV(TextureWrap::CLAMP_TO_EDGE);
    rv().fixedResFramebuffer.create();

    rv().fixedResWidth = width;
    rv().fixedResHeight = height;

    return rv().fixedResFramebuffer.isCreated();
}

void RenderSystem::renderBlit(TextureRender* source, Rect viewport){
    // bars outside the viewport keep the background color (same as the direct
    // path, which clears the whole destination before applying the viewport)
    fixedResPassRender.setClearColor(scene->getBackgroundColor());
    fixedResPassRender.setLabel("blit");

    PipelineType blitPip;
    bool flipGL = false;
    if (Framebuffer* target = targetFramebuffer()){
        if (!target->isCreated()){
            target->create();
        }
        // source and destination are both flipped on GL, so no un-flip needed
        fixedResPassRender.startRenderPass(&target->getRender());
        blitPip = PIP_RTT;
    }else{
        fixedResPassRender.startRenderPass();
        blitPip = PIP_DEFAULT;
        // the source was rendered flipped (PIP_RTT) but the GL swapchain is not
        flipGL = Engine::isOpenGL();
    }
    fixedResPassRender.applyViewport(viewport);

    if (blitRender.beginDraw(blitPip)){
        ShaderData& bd = blitShader.get()->shaderData;
        blitRender.addTexture(bd.getTextureIndex(TextureShaderType::SCENECOLORTEXTURE), ShaderStageType::FRAGMENT, source);
        fs_blit.params = flipGL ? Vector4(1.0f, 0.0f, -1.0f, 1.0f) : Vector4(1.0f, 0.0f, 1.0f, 0.0f);
        blitRender.applyUniformBlock(blitSlotParams, sizeof(fs_blit_t), &fs_blit);
        blitRender.draw(0, 3, 1);
    }
    fixedResPassRender.endRenderPass();
}

void RenderSystem::renderFixedResolutionBlit(){
    renderBlit(&rv().fixedResFramebuffer.getRender().getColorTexture(), targetViewRect());
}

void RenderSystem::needReloadPostProcess(){
    postProcessNeedReload = true;
}

void RenderSystem::needUpdatePostProcessUniforms(){
    postProcessNeedUpdateUniforms = true;
}

void RenderSystem::needUpdatePostProcessing(){
    postProcessingDirty = true;
}

void RenderSystem::invalidatePostProcessTextures(){
    // applyPostProcessing reads the strip again when its path differs from this one
    postLutStripPath.clear();
    postLutStrip = PostColorLut::Strip();
    postProcessingDirty = true;
    postProcessNeedReload = true;
}

void RenderSystem::loadPostProcess(){
    if (postProcessLoaded && !postProcessNeedReload)
        return;

    // a pass whose shader is still building aborts the attempt, so the partial
    // chain is always dropped before rebuilding
    for (int i = 0; i < (int)postProcessPasses.size(); i++){
        postProcessPasses[i].render.destroy();
        postProcessPasses[i].shader.reset();
    }
    postProcessPasses.clear();
    postProcessNeedReload = false;
    postProcessLoaded = false;
    postProcessNeedsDepth = false;
    postProcessNeedsGBuffer = false;
    postProcessKeepsOriginal = false;

    const std::vector<PostProcessPass>& passes = scene->getPostProcessPasses();

    for (int i = 0; i < (int)passes.size(); i++){
        if (!passes[i].enabled)
            continue;

        PostProcessRuntime pass;
        pass.passIndex = i;

        // an empty path is the built-in passthrough, also the fallback of a failed fork; a
        // "library:<name>" reference is a library post-process shader (phase 4)
        pass.customId = ShaderPool::registerCustomShader(passes[i].shader);
        pass.shader = ShaderPool::get(ShaderType::POSTPROCESS, 0, pass.customId);
        if (pass.customId != 0 && ShaderPool::isShaderBuildFailed(ShaderType::POSTPROCESS, 0, pass.customId)){
            // the pool already logged the failure; the panel marks the pass
            pass.customId = 0;
            pass.shader = ShaderPool::get(ShaderType::POSTPROCESS, 0);
        }
        // shaders may still be building (async in the editor)
        if (!pass.shader || !pass.shader->isCreated())
            return;

        pass.render.beginLoad(PrimitiveType::TRIANGLES);
        pass.render.setShader(pass.shader.get());
        // the chain never targets the swapchain directly, renderPostProcess blits for that
        // PIP_RTT into buffer 0 and the destination (colour targets with depth-stencil),
        // PIP_EFFECT into the others (depth only)
        if (!pass.render.endLoad(PIP_RTT | PIP_EFFECT, false, true, CullingMode::BACK, WindingOrder::CCW))
            return;

        ShaderData& sd = pass.shader.get()->shaderData;
        pass.slotSceneColor = sd.getTextureIndexByName("u_sceneColorTexture");
        pass.slotDepth = sd.getTextureIndexByName("u_depthTexture");
        pass.slotGBuffer = sd.getTextureIndexByName("u_gbufferTexture");
        pass.slotSSAO = sd.getTextureIndexByName("u_ssaoTexture");
        pass.slotOriginal = sd.getTextureIndexByName("u_originalSceneTexture");

        if (pass.slotDepth.first != -1)
            postProcessNeedsDepth = true;
        if (pass.slotGBuffer.first != -1)
            postProcessNeedsGBuffer = true;
        // the first pass reads the original as its input; a later one needs it kept
        if (pass.slotOriginal.first != -1 && !postProcessPasses.empty())
            postProcessKeepsOriginal = true;

        // its textures (manifest @texture, or the ones set by name) and the shader-library data
        // it declares, bound to the fallbacks first; a texture still loading retries the load
        if (pass.customId != 0)
            pass.manifest = ShaderManifests::get(passes[i].shader);
        pass.textures = passes[i].textures;
        const ShaderManifest* manifest = (pass.manifest && pass.manifest->present) ? pass.manifest.get() : nullptr;
        if (!loadShaderLibraryBindings(pass.bindings, sd, pass.render, pass.textures, manifest))
            return;

        // stored by name, so a renamed or removed member just drops; unset members take the
        // manifest's @property defaults
        if (pass.params.resolve(sd, "u_fs_postParams")){
            pass.params.writeValues(passes[i].uniforms, nullptr, manifest);
        }

        postProcessPasses.push_back(std::move(pass));
    }

    postProcessLoaded = true;
    postProcessNeedUpdateUniforms = false;
}

// rewrites the pass values from the scene chain, keeping the compiled shaders
void RenderSystem::updatePostProcessUniforms(){
    if (!postProcessNeedUpdateUniforms || !postProcessLoaded)
        return;

    const std::vector<PostProcessPass>& passes = scene->getPostProcessPasses();
    for (int i = 0; i < (int)postProcessPasses.size(); i++){
        PostProcessRuntime& pass = postProcessPasses[i];
        if (pass.passIndex < (int)passes.size()){
            const ShaderManifest* manifest = (pass.manifest && pass.manifest->present) ? pass.manifest.get() : nullptr;
            pass.params.writeValues(passes[pass.passIndex].uniforms, nullptr, manifest);
        }
    }

    postProcessNeedUpdateUniforms = false;
}

void RenderSystem::destroyPostProcess(){
    for (int i = 0; i < (int)postProcessPasses.size(); i++){
        postProcessPasses[i].render.destroy();
        postProcessPasses[i].shader.reset();
        ShaderPool::remove(ShaderType::POSTPROCESS, 0, postProcessPasses[i].customId);
    }
    postProcessPasses.clear();

    for (Framebuffer& framebuffer : rv().postProcessFramebuffer)
        framebuffer.destroy();
    rv().postScaledTargets.clear();

    rv().postProcessWidth = 0;
    rv().postProcessHeight = 0;
    postProcessNeedsDepth = false;
    postProcessNeedsGBuffer = false;
    postProcessKeepsOriginal = false;
    postProcessNeedReload = false;
    postProcessNeedUpdateUniforms = false;
    postProcessLoaded = false;
}

bool RenderSystem::ensurePostProcessFramebuffers(unsigned int width, unsigned int height){
    if (width == 0 || height == 0)
        return false;

    // the third full-size target only while a later pass reads the original scene
    const int count = postProcessKeepsOriginal ? 3 : 2;
    if (rv().postProcessFramebuffer[0].isCreated() && rv().postProcessWidth == width && rv().postProcessHeight == height){
        if (count == 2 && rv().postProcessFramebuffer[2].isCreated())
            rv().postProcessFramebuffer[2].destroy();
        if (count == 2 || rv().postProcessFramebuffer[2].isCreated())
            return true;
    }

    // the chain ping-pongs between them; its last pass writes the real destination
    for (int i = 0; i < 3; i++){
        rv().postProcessFramebuffer[i].destroy();
        if (i >= count)
            continue;
        rv().postProcessFramebuffer[i].setWidth(width);
        rv().postProcessFramebuffer[i].setHeight(height);
        rv().postProcessFramebuffer[i].setMinFilter(TextureFilter::LINEAR);
        rv().postProcessFramebuffer[i].setMagFilter(TextureFilter::LINEAR);
        rv().postProcessFramebuffer[i].setWrapU(TextureWrap::CLAMP_TO_EDGE);
        rv().postProcessFramebuffer[i].setWrapV(TextureWrap::CLAMP_TO_EDGE);
        // buffer 0 takes the colour pass (meshes, stencil); the others only the chain's passes
        rv().postProcessFramebuffer[i].setDepthAttachment(i == 0 ? FramebufferDepth::DEPTH_STENCIL : FramebufferDepth::DEPTH);
        rv().postProcessFramebuffer[i].create();
    }

    rv().postProcessWidth = width;
    rv().postProcessHeight = height;

    for (int i = 0; i < count; i++){
        if (!rv().postProcessFramebuffer[i].isCreated())
            return false;
    }
    return true;
}

Framebuffer* RenderSystem::postProcessOutput(unsigned int width, unsigned int height, TextureRender* input){
    auto isInput = [input](Framebuffer& framebuffer){
        return &framebuffer.getRender().getColorTexture() == input;
    };
    if (width == rv().postProcessWidth && height == rv().postProcessHeight){
        // 1 first (the first pass writes it, as before phase 4), then 2 (made only while the
        // original scene is kept), then 0 (which holds the original scene while it is kept)
        static const int order[3] = {1, 2, 0};
        for (int index : order){
            if (index == 0 && postProcessKeepsOriginal)
                continue;
            Framebuffer& framebuffer = rv().postProcessFramebuffer[index];
            if (framebuffer.isCreated() && !isInput(framebuffer))
                return &framebuffer;
        }
        return nullptr;
    }

    // below full size: two targets of that size, made when first used
    RenderView::PostScaledTargets* targets = nullptr;
    for (auto& entry : rv().postScaledTargets){
        if (entry->width == width && entry->height == height){
            targets = entry.get();
            break;
        }
    }
    if (!targets){
        rv().postScaledTargets.push_back(std::make_unique<RenderView::PostScaledTargets>());
        targets = rv().postScaledTargets.back().get();
        targets->width = width;
        targets->height = height;
        for (Framebuffer& framebuffer : targets->framebuffer){
            framebuffer.setWidth(width);
            framebuffer.setHeight(height);
            framebuffer.setMinFilter(TextureFilter::LINEAR);
            framebuffer.setMagFilter(TextureFilter::LINEAR);
            framebuffer.setWrapU(TextureWrap::CLAMP_TO_EDGE);
            framebuffer.setWrapV(TextureWrap::CLAMP_TO_EDGE);
            framebuffer.setDepthAttachment(FramebufferDepth::DEPTH);
            framebuffer.create();
        }
    }
    targets->used = true;
    for (Framebuffer& framebuffer : targets->framebuffer){
        if (framebuffer.isCreated() && !isInput(framebuffer))
            return &framebuffer;
    }
    return nullptr;
}

void RenderSystem::renderPostProcessCopy(TextureRender* source, FramebufferRender* destination){
    postProcessPassRender.setClearColor(Vector4(0.0f, 0.0f, 0.0f, 1.0f));
    postProcessPassRender.setLabel("user post-process copy");
    postProcessPassRender.startRenderPass(destination);
    if (blitRender.beginDraw(PIP_RTT)){
        ShaderData& bd = blitShader.get()->shaderData;
        blitRender.addTexture(bd.getTextureIndex(TextureShaderType::SCENECOLORTEXTURE), ShaderStageType::FRAGMENT, source);
        fs_blit.params = Vector4(1.0f, 0.0f, 1.0f, 0.0f);
        blitRender.applyUniformBlock(blitSlotParams, sizeof(fs_blit_t), &fs_blit);
        blitRender.draw(0, 3, 1);
    }
    postProcessPassRender.endRenderPass();
}

TextureRender* RenderSystem::renderPostProcess(FramebufferRender* destination, bool keepResult){
    // depth is the SSR G-buffer or the chain's own pre-pass; the G-buffer
    // normal/roughness only exists while SSR is on
    TextureRender* depthTexture = &emptyBlack;
    TextureRender* gbufferTexture = &emptyBlack;
    if (postProcessNeedsGBuffer && rv().gbufferFramebuffer.isCreated()){
        gbufferTexture = &rv().gbufferFramebuffer.getRender().getColorAttachmentTexture(1);
    }
    if (postProcessNeedsDepth){
        if (rv().gbufferFramebuffer.isCreated()){
            depthTexture = &rv().gbufferFramebuffer.getRender().getColorAttachmentTexture(0);
        }else if (rv().ssaoDepthFramebuffer.isCreated()){
            depthTexture = &rv().ssaoDepthFramebuffer.getRender().getColorTexture();
        }
    }
    TextureRender* ssaoTexture = rv().currentSSAOTexture ? rv().currentSSAOTexture : &emptyWhite;

    // the color pass (or the SSR composite) was redirected into buffer 0
    TextureRender* original = &rv().postProcessFramebuffer[0].getRender().getColorTexture();
    TextureRender* input = original;

    updatePostProcessUniforms();

    const std::vector<PostProcessPass>& scenePasses = scene->getPostProcessPasses();
    for (auto& entry : rv().postScaledTargets)
        entry->used = false;

    bool wroteDestination = false;
    for (int i = 0; i < (int)postProcessPasses.size(); i++){
        PostProcessRuntime& pass = postProcessPasses[i];
        const float scale = pass.passIndex < (int)scenePasses.size() ? scenePasses[pass.passIndex].getAppliedScale() : 1.0f;
        const unsigned int width = scale < 1.0f ? std::max(1u, (unsigned int)std::lround(rv().postProcessWidth * scale)) : rv().postProcessWidth;
        const unsigned int height = scale < 1.0f ? std::max(1u, (unsigned int)std::lround(rv().postProcessHeight * scale)) : rv().postProcessHeight;
        const bool fullSize = width == rv().postProcessWidth && height == rv().postProcessHeight;
        // the last pass writes the destination itself unless it is smaller (then a copy does)
        const bool writeDestination = destination && fullSize && (i + 1 == (int)postProcessPasses.size());

        Framebuffer* output = writeDestination ? nullptr : postProcessOutput(width, height, input);
        if (!writeDestination && !output)
            break;

        // CLEAR: the pass writes every pixel, so a tiler loads nothing (DONTCARE does nothing on GL)
        postProcessPassRender.setClearColor(Vector4(0.0f, 0.0f, 0.0f, 1.0f));
        postProcessPassRender.setLabel("user post-process");
        postProcessPassRender.startRenderPass(writeDestination ? destination : &output->getRender());

        // buffer 0 and the destination have a depth-stencil attachment, the others depth only
        const bool rtt = writeDestination || output == &rv().postProcessFramebuffer[0];
        if (pass.render.beginDraw(rtt ? PIP_RTT : PIP_EFFECT)){
            pass.render.addTexture(pass.slotSceneColor, ShaderStageType::FRAGMENT, input);
            pass.render.addTexture(pass.slotDepth, ShaderStageType::FRAGMENT, depthTexture);
            pass.render.addTexture(pass.slotGBuffer, ShaderStageType::FRAGMENT, gbufferTexture);
            pass.render.addTexture(pass.slotSSAO, ShaderStageType::FRAGMENT, ssaoTexture);
            pass.render.addTexture(pass.slotOriginal, ShaderStageType::FRAGMENT, original);
            // its own textures (a camera target may have been made again) and engine data
            bindShaderTextures(pass.bindings, pass.render, pass.textures);
            applyFrameBlock(pass.render, pass.bindings);
            applyCustomUniforms(pass.render, pass.params, frameTime, Vector2((float)width, (float)height));
            pass.render.draw(0, 3, 1);
        }
        postProcessPassRender.endRenderPass();

        if (writeDestination){
            wroteDestination = true;
        }else{
            input = &output->getRender().getColorTexture();
        }
    }

    // sizes no pass used this frame go (a scale changed)
    auto& scaled = rv().postScaledTargets;
    scaled.erase(std::remove_if(scaled.begin(), scaled.end(), [](const std::unique_ptr<RenderView::PostScaledTargets>& entry){
        return !entry->used;
    }), scaled.end());

    if (destination){
        // a last pass below full size, or a chain that stopped: copied to the destination
        if (!wroteDestination){
            loadBlit();
            if (blitLoaded)
                renderPostProcessCopy(input, destination);
        }
    }else if (keepResult){
        // the uber pass reads the last output as it is (no copy, and no scene target of its own)
        return input;
    }else{
        // the swapchain is written by the blit pass, which handles the GL orientation flip
        renderBlit(input, targetViewRect());
    }
    return nullptr;
}

void RenderSystem::presentFramebufferToSwapchain(Framebuffer* source){
    if (!source || !source->isCreated())
        return;

    loadBlit();
    if (!blitLoaded)
        return;

    // the scene passes already letterboxed into the composite, so copy it 1:1
    renderBlit(&source->getRender().getColorTexture(), Rect(0, 0,
            (float)System::instance().getScreenWidth(), (float)System::instance().getScreenHeight()));
}

void RenderSystem::destroyMesh(Entity entity, MeshComponent& mesh, bool clearAssets){
    InstancedMeshComponent* instmesh = scene->findComponent<InstancedMeshComponent>(entity);
    TerrainComponent* terrain = scene->findComponent<TerrainComponent>(entity);
    const bool preserveAssets = mesh.needReload && !clearAssets;

    if (!mesh.loaded)
        return;

    for (int i = 0; i < mesh.numSubmeshes; i++){

        Submesh& submesh = mesh.submeshes[i];
        if (!preserveAssets){
            //Destroy shader
            if (submesh.shader){
                submesh.shader.reset();
                ShaderPool::remove(ShaderType::MESH, submesh.shaderProperties, submesh.customShaderId);
            }
            // depth shader may have been loaded for shadows and/or SSAO
            if (submesh.depthShader){
                submesh.depthShader.reset();
                ShaderPool::remove(ShaderType::DEPTH, submesh.depthShaderProperties, submesh.customDepthShaderId);
            }
            // G-buffer shader may have been loaded for SSR
            if (submesh.gbufferShader){
                submesh.gbufferShader.reset();
                ShaderPool::remove(ShaderType::GBUFFER, submesh.gbufferShaderProperties);
            }

            //Destroy texture
            submesh.material.baseColorTexture.destroy();
            submesh.material.metallicRoughnessTexture.destroy();
            submesh.material.normalTexture.destroy();
            submesh.material.occlusionTexture.destroy();
            submesh.material.emissiveTexture.destroy();
            for (auto& shaderTexture : submesh.material.shaderTextures){
                shaderTexture.second.destroy();
            }
            if (MaterialPass* pass = submesh.material.extraPass.editIfSet()){
                for (auto& shaderTexture : pass->shaderTextures){
                    shaderTexture.second.destroy();
                }
            }
        }

        // the extra pass: its render always, its shader with the other assets
        releaseSubmeshExtraPass(submesh, preserveAssets);
        submesh.smoothNormals = -1;

        if (terrain){
            //Destroy terrain texture
            if (!preserveAssets){
                terrain->heightMap.destroy();
                for (Texture& blendMap : terrain->blendMaps){
                    blendMap.destroy();
                }
                for (TerrainSurfaceLayer& layer : terrain->surfaceLayers){
                    forEachTerrainLayerTexture(layer, [](Texture& texture){ texture.destroy(); });
                }
                destroyTerrainDetailArray(entity);
            }

            //Destroy terrain buffer
            for (int v = 0; v < MAX_TERRAIN_VIEWS; v++){
                for (int s = 0; s < 2; s++){
                    terrain->views[v].nodesbuffer[s].getRender()->destroyBuffer();
                }
            }
        }

        if (instmesh){
            //Destroy instmesh buffer
            if (!preserveAssets){
                instmesh->buffer.clearAll();
            }
            instmesh->buffer.getRender()->destroyBuffer();
        }

        submesh.lodIndices.getRender()->destroyBuffer();
        if (!preserveAssets){
            submesh.lodIndices.clearAll();
            submesh.lodData.reset();
            submesh.lodKey = 0;
        }

        //Destroy render
        submesh.render.destroy();
        submesh.depthRender.destroy();
        submesh.gbufferRender.destroy();

        //Shaders uniforms
        submesh.slotVSParams = -1;
        submesh.slotFSParams = -1;
        submesh.slotFSTexCoordSets = -1;
        submesh.slotVSFade = -1;
        // set only when the variant has them (loadMesh): a reload without must not keep them
        submesh.slotFSFog = -1;
        submesh.slotFSMirror = -1;
        submesh.slotFSLighting = -1;
        submesh.slotVSObjectLights = -1;
        submesh.slotFSObjectLights = -1;
        submesh.slotFSLitFrame = -1;
        submesh.slotFSReflectionProbe = -1;
        submesh.slotVSSprite = -1;
        submesh.slotVSShadows = -1;
        submesh.slotFSShadows = -1;
        submesh.slotFSPointShadows = -1;
        submesh.slotVSSkinning = -1;
        submesh.slotVSMorphTarget = -1;
        submesh.slotVSTerrain = -1;
        submesh.slotFSTerrainLayers = -1;

        submesh.slotVSDepthParams = -1;
        submesh.slotVSDepthFade = -1;
        submesh.slotFSDepthMaterial = -1;
        submesh.slotVSDepthSkinning = -1;
        submesh.slotVSDepthMorphTarget = -1;
        submesh.slotVSDepthTerrain = -1;
        submesh.slotVSDepthSprite = -1;

        submesh.slotVSGBufferParams = -1;
        submesh.slotFSGBufferMaterial = -1;
        submesh.slotVSGBufferSkinning = -1;
        submesh.slotVSGBufferMorphTarget = -1;
        submesh.slotVSGBufferTerrain = -1;
        submesh.slotFSGBufferTerrainLayers = -1;

        submesh.customVSParams.clear();
        submesh.customFSParams.clear();
        submesh.customVSDepthParams.clear();
        submesh.customFSDepthParams.clear();
        submesh.libraryBindings.clear();
        submesh.depthLibraryBindings.clear();
    }

    //Destroy buffer
    if (!preserveAssets){
        mesh.buffer.clearAll();
        mesh.indices.clearAll();
    }
    mesh.buffer.getRender()->destroyBuffer();
    mesh.indices.getRender()->destroyBuffer();
    // generated from the buffers: kept with them, made again from new ones
    releaseSmoothNormals(mesh, preserveAssets);
    mesh.bonesBuffer.destroyBuffer();
    mesh.bonesTexture.destroyTexture();
    for (int i = 0; i < mesh.numExternalBuffers; i++){
        if (!preserveAssets){
            mesh.eBuffers[i].clearAll();
        }
        mesh.eBuffers[i].getRender()->destroyBuffer();
    }

    if (mesh.needReload){
        // Reload destroys GPU objects immediately; reset load state synchronously so
        // update() can call loadMesh in the same frame.
        // Queuing changeDestroy would leave mesh.loaded true until later, so draw()
        // could run with stale bindings and trip Sokol validation.
        mesh.loaded = false;
        mesh.loadCalled = false;
    }else{
        SystemRender::addQueueCommand(&changeDestroy, new check_load_t{scene, entity});
    }
}

void RenderSystem::prepareMeshForDataReload(Entity entity, MeshComponent& mesh){
    if (!mesh.loaded)
        return;

    // A structural model reload replaces submesh and external-buffer counts. Tear down using
    // the old counts before MeshSystem mutates them, while retaining the synchronous reload state.
    mesh.needReload = true;
    destroyMesh(entity, mesh, true);
}

bool RenderSystem::loadUI(Entity entity, UIComponent& ui, uint16_t pipelines, bool isText){

    if (!Engine::isViewLoaded()) 
        return false;

    ui.texture.setAlphaBorderAuto(true);

    ObjectRender& render = ui.render;

    render.beginLoad(ui.primitiveType);

    bool hasUITexture = !ui.texture.empty();

    bool p_hasTexture = false;
    bool p_vertexColorVec4 = true;
    bool p_hasFontAtlasTexture = false;
    if (hasUITexture){
        if (isText){
            p_vertexColorVec4 = false;
            p_hasFontAtlasTexture = true;
        }else{
            p_hasTexture = true;
        }
    }

    ui.shaderProperties = ShaderPool::getUIProperties(p_hasTexture, p_hasFontAtlasTexture, false, p_vertexColorVec4);
    const std::string& uiShaderBase = ui.customShader.empty() ? scene->getDefaultCustomShader(ShaderType::UI) : ui.customShader;
    const std::string uiShaderSrc = getComponentShaderReference(uiShaderBase, ui.shaderFeatures, ui.blendMode);
    ui.customShaderId = ShaderPool::registerCustomShader(uiShaderSrc);
    ui.shader = ShaderPool::get(ShaderType::UI, ui.shaderProperties, ui.customShaderId);
    if (!ui.shader->isCreated()){
        // A custom shader that failed to compile falls back to the built-in shader (so the
        // object stays visible and loading finishes); retried after its source changes.
        if (ui.customShaderId != 0 && ShaderPool::isShaderBuildFailed(ShaderType::UI, ui.shaderProperties, ui.customShaderId)){
            Log::error("Custom shader '%s' failed to compile; using the built-in shader", uiShaderSrc.c_str());
            ui.customShaderId = 0;
            ui.shader = ShaderPool::get(ShaderType::UI, ui.shaderProperties, 0);
        }
        if (!ui.shader->isCreated())
            return false;
    }
    render.setShader(ui.shader.get());
    ShaderData& shaderData = ui.shader.get()->shaderData;

    ui.slotVSParams = shaderData.getUniformBlockIndex(UniformBlockType::UI_VS_PARAMS);
    ui.slotFSParams = shaderData.getUniformBlockIndex(UniformBlockType::UI_FS_PARAMS);
    // the manifest of the shader it draws with (none for the built-in fallback)
    ui.shaderManifest = (ui.customShaderId != 0) ? findManifest(uiShaderBase) : nullptr;
    ui.customVSParams.resolve(shaderData, "u_vs_customParams");
    ui.customFSParams.resolve(shaderData, "u_fs_customParams");
    ui.customVSParams.writeValues(ui.shaderUniforms, nullptr, ui.shaderManifest.get());
    ui.customFSParams.writeValues(ui.shaderUniforms, nullptr, ui.shaderManifest.get());
    ui.needUpdateShaderUniforms = false;

    size_t bufferSize;
    size_t minBufferSize;

    bufferSize = ui.buffer.getSize();
    minBufferSize = ui.minBufferCount * ui.buffer.getStride();
    if (minBufferSize > bufferSize)
        bufferSize = minBufferSize;

    if (bufferSize == 0)
        return false;

    ui.buffer.getRender()->createBuffer(bufferSize, ui.buffer.getData(), ui.buffer.getType(), ui.buffer.getUsage());
    if (ui.buffer.isRenderAttributes()) {
        for (auto const &attr : ui.buffer.getAttributes()) {
            render.addAttribute(shaderData.getAttrIndex(attr.first), ui.buffer.getRender(), attr.second.getElements(), attr.second.getDataType(), ui.buffer.getStride(), attr.second.getOffset(), attr.second.getNormalized(), attr.second.getPerInstance());
        }
    }
    if (ui.buffer.getUsage() != BufferUsage::IMMUTABLE){
        ui.needUpdateBuffer = true;
    }

    bufferSize = ui.indices.getSize();
    minBufferSize = ui.minIndicesCount * ui.indices.getStride();
    if (minBufferSize > bufferSize)
        bufferSize = minBufferSize;

    if (ui.indices.getCount() > 0){
        ui.indices.getRender()->createBuffer(bufferSize, ui.indices.getData(), ui.indices.getType(), ui.indices.getUsage());
        ui.vertexCount = ui.indices.getCount();
        Attribute indexattr = ui.indices.getAttributes()[AttributeType::INDEX];
        render.setIndex(ui.indices.getRender(), indexattr.getDataType(), indexattr.getOffset());
        if (ui.indices.getUsage() != BufferUsage::IMMUTABLE){
            ui.needUpdateBuffer = true;
        }
    }else{
        ui.vertexCount = ui.buffer.getCount();
    }

    if (TextureRender* textureRender = ui.texture.getRender(&emptyWhite)){
        if (!textureRender->isCreated()){
            return false;
        }
        render.addTexture(shaderData.getTextureIndex(TextureShaderType::UI), ShaderStageType::FRAGMENT, textureRender);
    }
    if (!loadShaderLibraryBindings(ui.libraryBindings, shaderData, render, ui.shaderTextures, ui.shaderManifest.get())){
        return false;
    }
    
    ui.needUpdateTexture = false;

    if (!render.endLoad(pipelines, resolveComponentRenderState(ui.blendMode, ui.depthWrite, ui.stencil, ui.colorWrite, ui.shaderManifest.get(), ui.shaderFeatures, false),
                        msaaPipelineSamples)){
        return false;
    }

    ui.needReload = false;
    ui.needUpdateAABB = true;
    ui.loadCalled = true;
    SystemRender::addQueueCommand(&changeLoaded, new check_load_t{scene, entity});

    return true;
}

bool RenderSystem::drawUI(UIComponent& ui, Transform& transform, PipelineType pipType){
    if (ui.loaded && ui.buffer.getSize() > 0){

        if (ui.needUpdateTexture || ui.texture.isFramebufferOutdated()){
            ShaderData& shaderData = ui.shader.get()->shaderData;
            ui.texture.setAlphaBorderAuto(true);
            if (TextureRender* textureRender = ui.texture.getRender(&emptyWhite))
                if (textureRender->isCreated()){
                    ui.render.addTexture(shaderData.getTextureIndex(TextureShaderType::UI), ShaderStageType::FRAGMENT, textureRender);
                    ui.needUpdateTexture = false;
                }
        }
        if (!ui.libraryBindings.textures.empty() && (ui.needUpdateTexture || shaderTexturesOutdated(ui.libraryBindings, ui.shaderTextures))){
            bindShaderTextures(ui.libraryBindings, ui.render, ui.shaderTextures);
        }

        if (ui.needUpdateBuffer){
            ui.buffer.getRender()->updateBuffer(ui.buffer.getSize(), ui.buffer.getData());

            if (ui.indices.getCount() > 0){
                ui.indices.getRender()->updateBuffer(ui.indices.getSize(), ui.indices.getData());
                ui.vertexCount = ui.indices.getCount();
            }else{
                ui.vertexCount = ui.buffer.getCount();
            }

            ui.needUpdateBuffer = false;
        }

        ObjectRender& render = ui.render;

        if (!render.beginDraw(pipType)){
            ui.needReload = true;
            return false;
        }
        render.applyUniformBlock(ui.slotVSParams, sizeof(float) * 16, (void*)&objectMVP(transform));
        //Color
        render.applyUniformBlock(ui.slotFSParams, sizeof(float) * 4, &ui.color);
        applyComponentUniforms(render, ui.customVSParams, ui.customFSParams, ui.shaderUniforms, ui.shaderManifest.get(), ui.needUpdateShaderUniforms, frameTime, passResolution);
        applyFrameBlock(render, ui.libraryBindings);
        render.draw(0, ui.vertexCount, 1);

    }

    return true;
}

void RenderSystem::destroyUI(Entity entity, UIComponent& ui){
    TextComponent* text = scene->findComponent<TextComponent>(entity);

    if (!ui.loaded)
        return;

    if (!ui.needReload){
        //Destroy shader
        if (ui.shader){
            ui.shader.reset();
            ShaderPool::remove(ShaderType::UI, ui.shaderProperties, ui.customShaderId);
        }

        //Destroy texture
        ui.texture.destroy();
        for (auto& shaderTexture : ui.shaderTextures){
            shaderTexture.second.destroy();
        }
    }

    //Destroy render
    ui.render.destroy();

    //Destroy buffer
    if (!ui.needReload){
        ui.buffer.clearAll();
        ui.indices.clearAll();
    }
    ui.buffer.getRender()->destroyBuffer();
    ui.indices.getRender()->destroyBuffer();

    if (text){
        text->needReloadAtlas = true;
    }

    //Shaders uniforms
    ui.slotVSParams = -1;
    ui.slotFSParams = -1;
    ui.customVSParams.clear();
    ui.customFSParams.clear();
    ui.libraryBindings.clear();

    SystemRender::addQueueCommand(&changeDestroy, new check_load_t{scene, entity});
}

bool RenderSystem::loadPoints(Entity entity, PointsComponent& points, uint16_t pipelines){

    if (!Engine::isViewLoaded()) 
        return false;

    points.texture.setAlphaBorderAuto(true);

    ObjectRender& render = points.render;

    render.beginLoad(PrimitiveType::POINTS);

    bool hasPointsTexture = !points.texture.empty();

    bool p_hasTexture = false;
    bool p_hasTextureRect = false;
    if (hasPointsTexture){
        p_hasTexture = true;
        if (points.hasTextureRect){
            p_hasTextureRect = true;
        }
    }

    points.shaderProperties = ShaderPool::getPointsProperties(p_hasTexture, false, true, p_hasTextureRect) | postColorProperty(ShaderType::POINTS);
    const std::string& pointsShaderBase = points.customShader.empty() ? scene->getDefaultCustomShader(ShaderType::POINTS) : points.customShader;
    const std::string pointsShaderSrc = getComponentShaderReference(pointsShaderBase, points.shaderFeatures, points.blendMode);
    points.customShaderId = ShaderPool::registerCustomShader(pointsShaderSrc);
    points.shader = ShaderPool::get(ShaderType::POINTS, points.shaderProperties, points.customShaderId);
    if (!points.shader->isCreated()){
        // A custom shader that failed to compile falls back to the built-in shader (so the
        // object stays visible and loading finishes); retried after its source changes.
        if (points.customShaderId != 0 && ShaderPool::isShaderBuildFailed(ShaderType::POINTS, points.shaderProperties, points.customShaderId)){
            Log::error("Custom shader '%s' failed to compile; using the built-in shader", pointsShaderSrc.c_str());
            points.customShaderId = 0;
            points.shader = ShaderPool::get(ShaderType::POINTS, points.shaderProperties, 0);
        }
        if (!points.shader->isCreated())
            return false;
    }
    render.setShader(points.shader.get());
    ShaderData& shaderData = points.shader.get()->shaderData;

    points.slotVSParams = shaderData.getUniformBlockIndex(UniformBlockType::POINTS_VS_PARAMS);
    points.shaderManifest = (points.customShaderId != 0) ? findManifest(pointsShaderBase) : nullptr;
    // what it reads of the scene (phase 3) and its @state priority=, with the features it enables
    points.sceneInputs = 0;
    points.sortPriority = 0;
    if (points.shaderManifest){
        const std::vector<std::string> features = ShaderReference::getEnabledFeatures(pointsShaderBase, points.shaderFeatures);
        points.sceneInputs = points.shaderManifest->resolveRequire(features) & ShaderRequire_SceneInputs;
        points.sortPriority = points.shaderManifest->resolveState(features).priority;
    }
    setSceneInputUser(entity, true, points.sceneInputs);
    points.sceneInputsPredicted = false;
    points.customVSParams.resolve(shaderData, "u_vs_customParams");
    points.customFSParams.resolve(shaderData, "u_fs_customParams");
    points.customVSParams.writeValues(points.shaderUniforms, nullptr, points.shaderManifest.get());
    points.customFSParams.writeValues(points.shaderUniforms, nullptr, points.shaderManifest.get());
    points.needUpdateShaderUniforms = false;

    points.buffer.clear();
    points.buffer.addAttribute(AttributeType::POSITION, 3, 0);
    points.buffer.addAttribute(AttributeType::COLOR, 4, 3 * sizeof(float));
    points.buffer.addAttribute(AttributeType::POINTSIZE, 1, 7 * sizeof(float));
    points.buffer.addAttribute(AttributeType::POINTROTATION, 1, 8 * sizeof(float));
    points.buffer.addAttribute(AttributeType::TEXTURERECT, 4, 9 * sizeof(float));
    points.buffer.setStride(13 * sizeof(float));
    points.buffer.setRenderAttributes(true);
    points.buffer.setUsage(BufferUsage::STREAM);

    // Now buffer size is zero than it needed to be calculated
    size_t bufferSize = points.maxPoints * points.buffer.getStride();

    points.buffer.getRender()->createBuffer(bufferSize, points.buffer.getData(), points.buffer.getType(), points.buffer.getUsage());
    if (points.buffer.isRenderAttributes()) {
        for (auto const &attr : points.buffer.getAttributes()) {
            render.addAttribute(shaderData.getAttrIndex(attr.first), points.buffer.getRender(), attr.second.getElements(), attr.second.getDataType(), points.buffer.getStride(), attr.second.getOffset(), attr.second.getNormalized(), attr.second.getPerInstance());
        }
    }

    points.needUpdateBuffer = true;

    if (TextureRender* textureRender = points.texture.getRender(&emptyWhite)){
        if (!textureRender->isCreated()){
            return false;
        }
        render.addTexture(shaderData.getTextureIndex(TextureShaderType::POINTS), ShaderStageType::FRAGMENT, textureRender);
    }
    // once the texture is loaded (or found in the pool): its alpha is known only then
    if (points.autoTransparency && !points.transparent){
        if (points.texture.isTransparent()){ // Particle color is not tested here
            points.transparent = true;
        }
    }
    if (!loadShaderLibraryBindings(points.libraryBindings, shaderData, render, points.shaderTextures, points.shaderManifest.get())){
        return false;
    }

    points.needUpdateTexture = false;

    const RenderState pointsState = resolveComponentRenderState(points.blendMode, points.depthWrite, points.stencil, points.colorWrite,
                                                                points.shaderManifest.get(), points.shaderFeatures, false);
    points.libraryBindings.postColorBlend = postColorBlendOf(pointsState, points.shaderManifest != nullptr);
    if (!render.endLoad(pipelines, pointsState, msaaPipelineSamples)){
        return false;
    }

    points.needReload = false;
    points.loadCalled = true;
    SystemRender::addQueueCommand(&changeLoaded, new check_load_t{scene, entity});

    return true;
}

bool RenderSystem::loadLines(Entity entity, LineRendererComponent& lines, uint16_t pipelines){

    if (!Engine::isViewLoaded()) 
        return false;

    if (lines.lines.size() == 0)
        return false;

    ObjectRender& render = lines.render;

    render.beginLoad(PrimitiveType::LINES);

    lines.shaderProperties = ShaderPool::getLinesProperties(false, true) | postColorProperty(ShaderType::LINES);
    const std::string& linesShaderBase = lines.customShader.empty() ? scene->getDefaultCustomShader(ShaderType::LINES) : lines.customShader;
    const std::string linesShaderSrc = getComponentShaderReference(linesShaderBase, lines.shaderFeatures, lines.blendMode);
    lines.customShaderId = ShaderPool::registerCustomShader(linesShaderSrc);
    lines.shader = ShaderPool::get(ShaderType::LINES, lines.shaderProperties, lines.customShaderId);
    if (!lines.shader->isCreated()){
        // A custom shader that failed to compile falls back to the built-in shader (so the
        // object stays visible and loading finishes); retried after its source changes.
        if (lines.customShaderId != 0 && ShaderPool::isShaderBuildFailed(ShaderType::LINES, lines.shaderProperties, lines.customShaderId)){
            Log::error("Custom shader '%s' failed to compile; using the built-in shader", linesShaderSrc.c_str());
            lines.customShaderId = 0;
            lines.shader = ShaderPool::get(ShaderType::LINES, lines.shaderProperties, 0);
        }
        if (!lines.shader->isCreated())
            return false;
    }
    render.setShader(lines.shader.get());
    ShaderData& shaderData = lines.shader.get()->shaderData;

    lines.slotVSParams = shaderData.getUniformBlockIndex(UniformBlockType::LINES_VS_PARAMS);
    lines.shaderManifest = (lines.customShaderId != 0) ? findManifest(linesShaderBase) : nullptr;
    lines.customVSParams.resolve(shaderData, "u_vs_customParams");
    lines.customFSParams.resolve(shaderData, "u_fs_customParams");
    lines.customVSParams.writeValues(lines.shaderUniforms, nullptr, lines.shaderManifest.get());
    lines.customFSParams.writeValues(lines.shaderUniforms, nullptr, lines.shaderManifest.get());
    lines.needUpdateShaderUniforms = false;

    lines.buffer.clear();
    lines.buffer.addAttribute(AttributeType::POSITION, 3, 0);
    lines.buffer.addAttribute(AttributeType::COLOR, 4, 3 * sizeof(float));
    lines.buffer.setStride(7 * sizeof(float));
    lines.buffer.setRenderAttributes(true);
    lines.buffer.setUsage(BufferUsage::DYNAMIC);

    // two points per line
    size_t bufferSize = lines.maxLines * (lines.buffer.getStride() * 2);

    if (bufferSize == 0)
        return false;

    lines.buffer.getRender()->createBuffer(bufferSize, lines.buffer.getData(), lines.buffer.getType(), lines.buffer.getUsage());
    if (lines.buffer.isRenderAttributes()) {
        for (auto const &attr : lines.buffer.getAttributes()) {
            render.addAttribute(shaderData.getAttrIndex(attr.first), lines.buffer.getRender(), attr.second.getElements(), attr.second.getDataType(), lines.buffer.getStride(), attr.second.getOffset(), attr.second.getNormalized(), attr.second.getPerInstance());
        }
    }
    // buffer is dynamic
    lines.needUpdateBuffer = true;

    if (!loadShaderLibraryBindings(lines.libraryBindings, shaderData, render, lines.shaderTextures, lines.shaderManifest.get())){
        return false;
    }

    const RenderState linesState = resolveComponentRenderState(lines.blendMode, lines.depthWrite, lines.stencil, lines.colorWrite,
                                                               lines.shaderManifest.get(), lines.shaderFeatures, false);
    lines.libraryBindings.postColorBlend = postColorBlendOf(linesState, lines.shaderManifest != nullptr);
    if (!render.endLoad(pipelines, linesState, msaaPipelineSamples)){
        return false;
    }

    lines.needReload = false;
    lines.loadCalled = true;
    SystemRender::addQueueCommand(&changeLoaded, new check_load_t{scene, entity});

    return true;
}

float RenderSystem::getPointsViewportHeight(const CameraComponent& camera) const{
    if (camera.renderToTexture && camera.framebuffer){
        return (float)camera.framebuffer->getHeight();
    }
    if (isFixedResolutionActive()){
        // the main color pass renders into the fixed-resolution target
        return (float)scene->getFixedResolutionHeight();
    }
    if (Framebuffer* target = targetFramebuffer()){
        if (target->isCreated()){
            return (float)target->getHeight();
        }
    }
    float viewHeight = targetViewRect().getHeight();
    if (viewHeight > 0.0f){
        return viewHeight;
    }
    return (float)Engine::getCanvasHeight();
}

float RenderSystem::computePointsScale(const CameraComponent& camera, float viewportHeight) const{
    if (viewportHeight <= 0.0f){
        return 1.0f;
    }

    if (camera.type == CameraType::CAMERA_PERSPECTIVE){
        float tanHalfFov = tanf(camera.yfov * 0.5f);
        if (tanHalfFov <= 0.0f){
            return 1.0f;
        }
        return viewportHeight / (2.0f * tanHalfFov);
    }

    float orthoHeight = 0.0f;
    if (camera.type == CameraType::CAMERA_UI){
        orthoHeight = fabsf(camera.topClip - camera.bottomClip);
    }else{
        orthoHeight = camera.topClip - camera.bottomClip;
    }

    if (orthoHeight <= 0.0f){
        return 1.0f;
    }

    return viewportHeight / orthoHeight;
}

bool RenderSystem::drawPoints(Entity entity, PointsComponent& points, Transform& transform, CameraComponent& camera, Transform& camTransform, PipelineType pipType){
    if (points.loaded && points.numVisible > 0){

        if (points.needUpdateTexture || points.texture.isFramebufferOutdated()){
            ShaderData& shaderData = points.shader.get()->shaderData;
            points.texture.setAlphaBorderAuto(true);
            if (TextureRender* textureRender = points.texture.getRender(&emptyWhite)){
                if (textureRender->isCreated()){
                    points.render.addTexture(shaderData.getTextureIndex(TextureShaderType::POINTS), ShaderStageType::FRAGMENT, textureRender);
                    points.needUpdateTexture = false;
                }
            }
        }
        if (!points.libraryBindings.textures.empty() && (points.needUpdateTexture || shaderTexturesOutdated(points.libraryBindings, points.shaderTextures))){
            bindShaderTextures(points.libraryBindings, points.render, points.shaderTextures);
        }

        if (points.needUpdateBuffer){
            // setData here because component can change order and lose reference
            points.buffer.setData((unsigned char*)(&points.renderPoints[0]), sizeof(PointRenderData)*points.numVisible);
            points.buffer.getRender()->updateBuffer(points.buffer.getSize(), points.buffer.getData());
            points.needUpdateBuffer = false;
        }

        ObjectRender& render = points.render;

        if (!render.beginDraw(pipType)){
            points.needReload = true;
            return false;
        }
        vs_points_params_t vsParams = {};
        vsParams.mvpMatrix = objectMVP(transform);
        vsParams.pointScale = computePointsScale(camera, getPointsViewportHeight(camera));
        render.applyUniformBlock(points.slotVSParams, sizeof(vs_points_params_t), &vsParams);
        applyComponentUniforms(render, points.customVSParams, points.customFSParams, points.shaderUniforms, points.shaderManifest.get(), points.needUpdateShaderUniforms, frameTime, passResolution);
        applyFrameBlock(render, points.libraryBindings);
        applySceneInputs(render, points.libraryBindings);
#ifdef JASPERENGINE_EDITOR
        const bool viewOrder = bindViewPointsOrder(entity, points, transform, camera, camTransform);
#endif
        render.draw(0, points.numVisible, 1);
#ifdef JASPERENGINE_EDITOR
        if (viewOrder){
            render.replaceVertexBuffer(points.buffer.getRender(), points.buffer.getRender());
        }
#endif
    }

    return true;
}

void RenderSystem::destroyPoints(Entity entity, PointsComponent& points){
    if (!points.loaded)
        return;

    if (!points.needReload){
        //Destroy shader
        if (points.shader){
            points.shader.reset();
            ShaderPool::remove(ShaderType::POINTS, points.shaderProperties, points.customShaderId);
        }

        //Destroy texture
        points.texture.destroy();
        for (auto& shaderTexture : points.shaderTextures){
            shaderTexture.second.destroy();
        }
    }

    //Destroy render
    points.render.destroy();

    //Destroy buffer
    if (!points.needReload){
        points.buffer.clearAll();
    }
    points.buffer.getRender()->destroyBuffer();

    //Shaders uniforms
    points.slotVSParams = -1;
    points.customVSParams.clear();
    points.customFSParams.clear();
    points.libraryBindings.clear();

    SystemRender::addQueueCommand(&changeDestroy, new check_load_t{scene, entity});
}

bool RenderSystem::drawLines(LineRendererComponent& lines, Transform& transform, Transform& camTransform, PipelineType pipType){
    if (lines.loaded && lines.lines.size() > 0){

        if (lines.needUpdateBuffer){
            // setData here because component can change order and lose reference
            lines.buffer.setData((unsigned char*)(&lines.lines[0]), sizeof(LineData)*lines.lines.size());
            lines.buffer.getRender()->updateBuffer(lines.buffer.getSize(), lines.buffer.getData());
            lines.needUpdateBuffer = false;
        }

        ObjectRender& render = lines.render;

        if (!lines.libraryBindings.textures.empty() && shaderTexturesOutdated(lines.libraryBindings, lines.shaderTextures)){
            bindShaderTextures(lines.libraryBindings, render, lines.shaderTextures);
        }

        if (!render.beginDraw(pipType)){
            lines.needReload = true;
            return false;
        }
        render.applyUniformBlock(lines.slotVSParams, sizeof(float) * 16, (void*)&objectMVP(transform));
        applyComponentUniforms(render, lines.customVSParams, lines.customFSParams, lines.shaderUniforms, lines.shaderManifest.get(), lines.needUpdateShaderUniforms, frameTime, passResolution);
        applyFrameBlock(render, lines.libraryBindings);
        render.draw(0, lines.lines.size() * 2, 1);
    }

    return true;
}

void RenderSystem::destroyLines(Entity entity, LineRendererComponent& lines){
    if (!lines.loaded)
        return;

    if (!lines.needReload){
        //Destroy shader
        if (lines.shader){
            lines.shader.reset();
            ShaderPool::remove(ShaderType::LINES, lines.shaderProperties, lines.customShaderId);
        }
        for (auto& shaderTexture : lines.shaderTextures){
            shaderTexture.second.destroy();
        }
    }

    //Destroy render
    lines.render.destroy();

    //Destroy buffer
    if (!lines.needReload){
        lines.buffer.clearAll();
    }
    lines.buffer.getRender()->destroyBuffer();

    //Shaders uniforms
    lines.slotVSParams = -1;
    lines.customVSParams.clear();
    lines.customFSParams.clear();
    lines.libraryBindings.clear();

    SystemRender::addQueueCommand(&changeDestroy, new check_load_t{scene, entity});
}

bool RenderSystem::loadSky(Entity entity, SkyboxComponent& sky, uint16_t pipelines){

    if (!Engine::isViewLoaded()) 
        return false;	

    sky.buffer.clear();
    sky.buffer.addAttribute(AttributeType::POSITION, 3);

    Attribute* attVertex = sky.buffer.getAttribute(AttributeType::POSITION);

    sky.buffer.addVector3(attVertex, Vector3(-1.0f,  1.0f, -1.0f));
    sky.buffer.addVector3(attVertex, Vector3(-1.0f, -1.0f, -1.0f));
    sky.buffer.addVector3(attVertex, Vector3(1.0f, -1.0f, -1.0f));
    sky.buffer.addVector3(attVertex, Vector3(1.0f, -1.0f, -1.0f));
    sky.buffer.addVector3(attVertex, Vector3(1.0f,  1.0f, -1.0f));
    sky.buffer.addVector3(attVertex, Vector3(-1.0f,  1.0f, -1.0f));

    sky.buffer.addVector3(attVertex, Vector3(-1.0f, -1.0f,  1.0f));
    sky.buffer.addVector3(attVertex, Vector3(-1.0f, -1.0f, -1.0f));
    sky.buffer.addVector3(attVertex, Vector3(-1.0f,  1.0f, -1.0f));
    sky.buffer.addVector3(attVertex, Vector3(-1.0f,  1.0f, -1.0f));
    sky.buffer.addVector3(attVertex, Vector3(-1.0f,  1.0f,  1.0f));
    sky.buffer.addVector3(attVertex, Vector3(-1.0f, -1.0f,  1.0f));

    sky.buffer.addVector3(attVertex, Vector3(1.0f, -1.0f, -1.0f));
    sky.buffer.addVector3(attVertex, Vector3(1.0f, -1.0f,  1.0f));
    sky.buffer.addVector3(attVertex, Vector3(1.0f,  1.0f,  1.0f));
    sky.buffer.addVector3(attVertex, Vector3(1.0f,  1.0f,  1.0f));
    sky.buffer.addVector3(attVertex, Vector3(1.0f,  1.0f, -1.0f));
    sky.buffer.addVector3(attVertex, Vector3(1.0f, -1.0f, -1.0f));

    sky.buffer.addVector3(attVertex, Vector3(-1.0f, -1.0f,  1.0f));
    sky.buffer.addVector3(attVertex, Vector3(-1.0f,  1.0f,  1.0f));
    sky.buffer.addVector3(attVertex, Vector3(1.0f,  1.0f,  1.0f));
    sky.buffer.addVector3(attVertex, Vector3(1.0f,  1.0f,  1.0f));
    sky.buffer.addVector3(attVertex, Vector3(1.0f, -1.0f,  1.0f));
    sky.buffer.addVector3(attVertex, Vector3(-1.0f, -1.0f,  1.0f));

    sky.buffer.addVector3(attVertex, Vector3(-1.0f,  1.0f, -1.0f));
    sky.buffer.addVector3(attVertex, Vector3(1.0f,  1.0f, -1.0f));
    sky.buffer.addVector3(attVertex, Vector3(1.0f,  1.0f,  1.0f));
    sky.buffer.addVector3(attVertex, Vector3(1.0f,  1.0f,  1.0f));
    sky.buffer.addVector3(attVertex, Vector3(-1.0f,  1.0f,  1.0f));
    sky.buffer.addVector3(attVertex, Vector3(-1.0f,  1.0f, -1.0f));

    sky.buffer.addVector3(attVertex, Vector3(-1.0f, -1.0f, -1.0f));
    sky.buffer.addVector3(attVertex, Vector3(-1.0f, -1.0f,  1.0f));
    sky.buffer.addVector3(attVertex, Vector3(1.0f, -1.0f, -1.0f));
    sky.buffer.addVector3(attVertex, Vector3(1.0f, -1.0f, -1.0f));
    sky.buffer.addVector3(attVertex, Vector3(-1.0f, -1.0f,  1.0f));
    sky.buffer.addVector3(attVertex, Vector3(1.0f, -1.0f,  1.0f));
    ObjectRender* render = &sky.render;

    render->beginLoad(PrimitiveType::TRIANGLES);

    const std::string& skyShaderBase = sky.customShader.empty() ? scene->getDefaultCustomShader(ShaderType::SKYBOX) : sky.customShader;
    const std::string skyShaderSrc = getComponentShaderReference(skyShaderBase, sky.shaderFeatures, MaterialBlendMode::AUTO);
    sky.customShaderId = ShaderPool::registerCustomShader(skyShaderSrc);
    sky.shaderProperties = postColorProperty(ShaderType::SKYBOX);
    sky.shader = ShaderPool::get(ShaderType::SKYBOX, sky.shaderProperties, sky.customShaderId);
    if (!sky.shader->isCreated()){
        // A custom shader that failed to compile falls back to the built-in shader (so the
        // object stays visible and loading finishes); retried after its source changes.
        if (sky.customShaderId != 0 && ShaderPool::isShaderBuildFailed(ShaderType::SKYBOX, sky.shaderProperties, sky.customShaderId)){
            Log::error("Custom shader '%s' failed to compile; using the built-in shader", skyShaderSrc.c_str());
            sky.customShaderId = 0;
            sky.shader = ShaderPool::get(ShaderType::SKYBOX, sky.shaderProperties, 0);
        }
        if (!sky.shader->isCreated())
            return false;
    }
    render->setShader(sky.shader.get());
    ShaderData& shaderData = sky.shader.get()->shaderData;

    sky.slotVSParams = shaderData.getUniformBlockIndex(UniformBlockType::SKY_VS_PARAMS);
    sky.slotFSParams = shaderData.getUniformBlockIndex(UniformBlockType::SKY_FS_PARAMS);
    sky.shaderManifest = (sky.customShaderId != 0) ? findManifest(skyShaderBase) : nullptr;
    sky.customVSParams.resolve(shaderData, "u_vs_customParams");
    sky.customFSParams.resolve(shaderData, "u_fs_customParams");
    sky.customVSParams.writeValues(sky.shaderUniforms, nullptr, sky.shaderManifest.get());
    sky.customFSParams.writeValues(sky.shaderUniforms, nullptr, sky.shaderManifest.get());
    sky.needUpdateShaderUniforms = false;

    if (TextureRender* textureRender = sky.texture.getRender(&emptyCubeWhite)){
        if (!textureRender->isCreated()){
            return false;
        }
        render->addTexture(shaderData.getTextureIndex(TextureShaderType::SKYCUBE), ShaderStageType::FRAGMENT, textureRender);
    } else {
        return false;
    }
    if (!loadShaderLibraryBindings(sky.libraryBindings, shaderData, *render, sky.shaderTextures, sky.shaderManifest.get())){
        return false;
    }

    sky.needUpdateTexture = false;

    sky.buffer.getRender()->createBuffer(sky.buffer.getSize(), sky.buffer.getData(), sky.buffer.getType(), sky.buffer.getUsage());

    if (sky.buffer.isRenderAttributes()) {
        for (auto const &attr : sky.buffer.getAttributes()) {
            render->addAttribute(shaderData.getAttrIndex(attr.first), sky.buffer.getRender(), attr.second.getElements(), attr.second.getDataType(), sky.buffer.getStride(), attr.second.getOffset(), attr.second.getNormalized(),  attr.second.getPerInstance());
        }
    }

    if (!render->endLoad(pipelines, resolveComponentRenderState(MaterialBlendMode::AUTO, MaterialDepthWrite::AUTO, sky.shaderManifest.get(), sky.shaderFeatures, true),
                         msaaPipelineSamples)){
        return false;
    }

    sky.needReload = false;
    sky.loadCalled = true;
    SystemRender::addQueueCommand(&changeLoaded, new check_load_t{scene, entity});

    // one sky per scene (see SkyboxComponent): say which, once per load
    const size_t skies = scene->getComponentArray<SkyboxComponent>()->size();
    if (skies > 1){
        Log::warn("Scene has %zu skyboxes: only the first added (entity %u '%s') is drawn and lights the scene",
                  skies, (unsigned int)entity, scene->getEntityName(entity).c_str());
    }

    return true;
}

bool RenderSystem::drawSky(SkyboxComponent& sky, PipelineType pipType, const Matrix4& viewProjection){
    if (sky.loaded){

        if (sky.needUpdateTexture || sky.texture.isFramebufferOutdated()){
            ShaderData& shaderData = sky.shader.get()->shaderData;
            if (TextureRender* textureRender = sky.texture.getRender(&emptyCubeWhite)){
                if (textureRender->isCreated()){
                    sky.render.addTexture(shaderData.getTextureIndex(TextureShaderType::SKYCUBE), ShaderStageType::FRAGMENT, textureRender);
                    sky.needUpdateTexture = false;
                }
            }
        }
        if (!sky.libraryBindings.textures.empty() && (sky.needUpdateTexture || shaderTexturesOutdated(sky.libraryBindings, sky.shaderTextures))){
            bindShaderTextures(sky.libraryBindings, sky.render, sky.shaderTextures);
        }

        ObjectRender& render = sky.render;

        if (!render.beginDraw(pipType)){
            sky.needReload = true;
            return false;
        }
        render.applyUniformBlock(sky.slotVSParams, sizeof(float) * 16, (void*)&viewProjection);
        render.applyUniformBlock(sky.slotFSParams, sizeof(float) * 4, &sky.color);
        applyComponentUniforms(render, sky.customVSParams, sky.customFSParams, sky.shaderUniforms, sky.shaderManifest.get(), sky.needUpdateShaderUniforms, frameTime, passResolution);
        applyFrameBlock(render, sky.libraryBindings);
        render.draw(0, 36, 1);
    }

    return true;
}

void RenderSystem::destroySky(Entity entity, SkyboxComponent& sky){
    if (!sky.loaded)
        return;

    if (!sky.needReload){
        //Destroy shader
        if (sky.shader){
            sky.shader.reset();
            ShaderPool::remove(ShaderType::SKYBOX, sky.shaderProperties, sky.customShaderId);
        }

        //Destroy texture
        sky.texture.destroy();
        for (auto& shaderTexture : sky.shaderTextures){
            shaderTexture.second.destroy();
        }

        //Release IBL environment maps (pooled maps are kept for reuse)
        releaseSkyEnvironment(sky);
        sky.needUpdateEnvironment = true;
    }

    //Destroy render
    sky.render.destroy();

    //Destroy buffer
    if (!sky.needReload){
        sky.buffer.clearAll();
    }
    sky.buffer.getRender()->destroyBuffer();

    //Shaders uniforms
    sky.slotVSParams = -1;
    sky.slotFSParams = -1;
    sky.customVSParams.clear();
    sky.customFSParams.clear();
    sky.libraryBindings.clear();

    SystemRender::addQueueCommand(&changeDestroy, new check_load_t{scene, entity});
}

void RenderSystem::destroyLight(LightComponent& light){
    // shadow maps live in the RenderSystem-owned atlases, so a light owns no GPU
    // resources of its own to release here
    light.shadowMapIndex = -1;
}

void RenderSystem::destroyCamera(CameraComponent& camera, bool entityDestroyed){
    if (camera.framebuffer){
        if (entityDestroyed){
            delete camera.framebuffer; // destroy framebuffer in destructor
        }else{
            camera.framebuffer->destroy();
        }
    }
}

// Rect::fitOnRect moves the origin without shrinking the opposite edge, which widens a
// scissor that starts outside the bounds
static Rect intersectScissor(const Rect& rect, const Rect& bounds){
    float boundsRight = bounds.getX() + bounds.getWidth();
    float boundsTop = bounds.getY() + bounds.getHeight();

    float left = std::min(std::max(rect.getX(), bounds.getX()), boundsRight);
    float bottom = std::min(std::max(rect.getY(), bounds.getY()), boundsTop);
    float right = std::max(std::min(rect.getX() + rect.getWidth(), boundsRight), left);
    float top = std::max(std::min(rect.getY() + rect.getHeight(), boundsTop), bottom);

    return Rect(left, bottom, right - left, top - bottom);
}

Rect RenderSystem::getScissorRect(RectTransformComponent& layout, ImageComponent& img, Transform& transform, const Rect& passViewport){
    // the scissor is in the pass target pixels, so it follows the viewport the color pass
    // applied: offscreen targets start at (0,0), a swapchain keeps the letterbox offset
    float viewX = passViewport.getX();
    float viewY = passViewport.getY();
    float viewWidth = passViewport.getWidth();
    float viewHeight = passViewport.getHeight();

    if (viewWidth <= 0.0f || viewHeight <= 0.0f)
        return Rect(0, 0, 0, 0);

    float localLeft = (float) img.patchMarginLeft;
    float localTop = (float) img.patchMarginTop;
    float localRight = (float) layout.width - (float) img.patchMarginRight;
    float localBottom = (float) layout.height - (float) img.patchMarginBottom;

    if (localRight < localLeft)
        localRight = localLeft;
    if (localBottom < localTop)
        localBottom = localTop;

    const Matrix4& mvp = objectMVP(transform);

    Vector4 corners[4] = {
        mvp * Vector4(localLeft, localTop, 0.0f, 1.0f),
        mvp * Vector4(localRight, localTop, 0.0f, 1.0f),
        mvp * Vector4(localRight, localBottom, 0.0f, 1.0f),
        mvp * Vector4(localLeft, localBottom, 0.0f, 1.0f)
    };

    // a corner behind the eye has no screen position and a negative w mirrors the rect,
    // so the quad is clipped against w > 0 first
    const float EPSILON = 1e-5f;
    Vector4 visible[8];
    int visibleCount = 0;

    for (int i = 0; i < 4; i++){
        const Vector4& current = corners[i];
        const Vector4& next = corners[(i + 1) % 4];
        bool currentInFront = current.w > EPSILON;
        bool nextInFront = next.w > EPSILON;

        if (currentInFront)
            visible[visibleCount++] = current;

        if (currentInFront != nextInFront)
            visible[visibleCount++] = current + (next - current) * ((EPSILON - current.w) / (next.w - current.w));
    }

    if (visibleCount == 0)
        return Rect(0, 0, 0, 0);

    float minX = 0.0f;
    float minY = 0.0f;
    float maxX = 0.0f;
    float maxY = 0.0f;

    for (int i = 0; i < visibleCount; i++){
        float screenX = viewX + (((visible[i].x / visible[i].w) + 1.0f) * 0.5f * viewWidth);
        float screenY = viewY + (((visible[i].y / visible[i].w) + 1.0f) * 0.5f * viewHeight);

        if (i == 0){
            minX = maxX = screenX;
            minY = maxY = screenY;
        }else{
            minX = std::min(minX, screenX);
            minY = std::min(minY, screenY);
            maxX = std::max(maxX, screenX);
            maxY = std::max(maxY, screenY);
        }
    }

    return intersectScissor(Rect(minX, minY, maxX - minX, maxY - minY), passViewport);
}

void RenderSystem::updateTransform(Transform& transform, Entity entity){
    // UI grows around its pivot, so its origin, the top-left corner, moves
    Vector3 origin = transform.position;
    RectTransformComponent* layout = scene->findComponent<RectTransformComponent>(entity);
    if (layout && layout->pivot != Vector2(0, 0)){
        origin += getUIPivotShift(*layout, transform.rotation, transform.scale);
    }

    Matrix4 translateMatrix = Matrix4::translateMatrix(origin);
    Matrix4 rotationMatrix = transform.rotation.getRotationMatrix();
    Matrix4 scaleMatrix = Matrix4::scaleMatrix(transform.scale);

    transform.localMatrix = translateMatrix * rotationMatrix * scaleMatrix;

    Transform* transformParent = NULL;
    if (transform.parent != NULL_ENTITY){
        transformParent = scene->findComponent<Transform>(transform.parent);
    }

    if (transformParent){
        transform.modelMatrix = transformParent->modelMatrix * transform.localMatrix;

        transform.worldPosition = transformParent->modelMatrix * origin;
        transform.worldScale = transformParent->worldScale * transform.scale;
        transform.worldRotation = transformParent->worldRotation * transform.rotation;
    }else{
        transform.modelMatrix = transform.localMatrix;

        transform.worldPosition = origin;
        transform.worldScale = transform.scale;
        transform.worldRotation = transform.rotation;
    }

    if (hasLights){
        transform.normalMatrix = transform.modelMatrix.inverse().transpose();
    }
}

void RenderSystem::updateCamera(CameraComponent& camera, Transform& transform){
    //Update ProjectionMatrix
    if (camera.type == CameraType::CAMERA_UI){
        camera.projectionMatrix = Matrix4::orthoMatrix(camera.leftClip, camera.rightClip, camera.topClip, camera.bottomClip, camera.nearClip, camera.farClip);
    }else if (camera.type == CameraType::CAMERA_ORTHO) {
        camera.projectionMatrix = Matrix4::orthoMatrix(camera.leftClip, camera.rightClip, camera.bottomClip, camera.topClip, camera.nearClip, camera.farClip);
    }else if (camera.type == CameraType::CAMERA_PERSPECTIVE){
        camera.projectionMatrix = Matrix4::perspectiveMatrix(camera.yfov, camera.aspect, camera.nearClip, camera.farClip);
    }

    if (camera.useTarget){
        camera.direction = (transform.position - camera.target).normalize();
        camera.right = (camera.up.crossProduct(camera.direction)).normalize();

        if (transform.parent != NULL_ENTITY){
            camera.worldTarget = transform.modelMatrix * (camera.target - transform.position);
            camera.worldUp = ((transform.modelMatrix * camera.up) - (transform.modelMatrix * Vector3(0,0,0))).normalize();
        }else{
            camera.worldTarget = camera.target;
            camera.worldUp = camera.up;
        }

        //Update ViewMatrix
        camera.viewMatrix = Matrix4::lookAtMatrix(transform.worldPosition, camera.worldTarget, camera.worldUp);
    }else{
        camera.direction = transform.rotation * Vector3(0, 0, 1);
        camera.right = transform.rotation * Vector3(1, 0, 0);

        camera.worldTarget = transform.worldPosition - (transform.worldRotation * Vector3(0, 0, 1));

        //Update ViewMatrix
        camera.viewMatrix = transform.worldRotation.inverse().getRotationMatrix();
        camera.viewMatrix.translateInPlace(-transform.worldPosition.x, -transform.worldPosition.y, -transform.worldPosition.z);
    }

    // planar reflection drives the view matrix directly (mainView * reflect);
    // projection above still comes from the copied perspective params
    if (camera.hasCustomViewMatrix){
        camera.viewMatrix = camera.customViewMatrix;
    }

    camera.worldRight = Vector3(camera.viewMatrix[0][0], camera.viewMatrix[1][0], camera.viewMatrix[2][0]);
    camera.worldUp = Vector3(camera.viewMatrix[0][1], camera.viewMatrix[1][1], camera.viewMatrix[2][1]);
    camera.worldDirection = Vector3(camera.viewMatrix[0][2], camera.viewMatrix[1][2], camera.viewMatrix[2][2]);

    //Update ViewProjectionMatrix
    camera.viewProjectionMatrix = camera.projectionMatrix * camera.viewMatrix;

    updateCameraFrustumPlanes(camera.viewProjectionMatrix, camera.frustumPlanes);
}

// Creates the internal render-to-texture camera a mirror drives: a hidden system
// entity (not serialized, not shown in the editor) whose framebuffer feeds the
// mirror mesh base texture. Returns the camera entity.
Entity RenderSystem::createMirrorCamera(Entity mirrorEntity){
    if (!scene->findComponent<MirrorComponent>(mirrorEntity))
        return NULL_ENTITY;

    const Entity existing = getMirrorCamera(mirrorEntity);
    if (existing != NULL_ENTITY && scene->isEntityCreated(existing))
        return existing;

    Entity camEntity = scene->createSystemEntity();
    scene->addComponent<Transform>(camEntity, {});
    scene->addComponent<CameraComponent>(camEntity, {});

    CameraComponent& cam = scene->getComponent<CameraComponent>(camEntity);
    cam.type = CameraType::CAMERA_PERSPECTIVE;
    cam.renderToTexture = true;
    cam.autoResize = false; // driven entirely by updateMirrors
    int w = (Engine::getCanvasWidth() > 0) ? Engine::getCanvasWidth() : 1024;
    int h = (Engine::getCanvasHeight() > 0) ? Engine::getCanvasHeight() : 1024;
    cam.framebuffer->setWidth(w);
    cam.framebuffer->setHeight(h);
    // keep authored fields in sync so RenderSystem::update doesn't resize it back
    cam.framebufferWidth = w;
    cam.framebufferHeight = h;

    mirrorCameras[mirrorEntity] = camEntity;
    return camEntity;
}

Entity RenderSystem::getMirrorCamera(Entity mirrorEntity) const{
    auto it = mirrorCameras.find(mirrorEntity);
    return it != mirrorCameras.end() ? it->second : NULL_ENTITY;
}

void RenderSystem::destroyMirrorCamera(Entity entity){
    // Ownership leaves the map before entity destruction dispatches removal callbacks.
    auto camera = mirrorCameras.extract(entity);
    if (camera.empty())
        return;

    const Entity cameraEntity = camera.mapped();
    if (cameraEntity == NULL_ENTITY || !scene->isEntityCreated(cameraEntity))
        return;

    // Detach the borrowed framebuffer before destroying the camera that owns it.
    if (CameraComponent* cameraComponent = scene->findComponent<CameraComponent>(cameraEntity)){
        if (MeshComponent* mesh = scene->findComponent<MeshComponent>(entity)){
            if (mesh->numSubmeshes > 0){
                Texture& baseTexture = mesh->submeshes[0].material.baseColorTexture;
                if (baseTexture.getFramebuffer() == cameraComponent->framebuffer){
                    baseTexture = Texture();
                }
            }
        }
    }
    scene->destroyEntity(cameraEntity);
}

// Drives each mirror's reflection camera: its view = mainView * reflect(plane),
// so the reflected scene is rendered into that camera's framebuffer. Runs after
// the main camera is updated and before the draw pass. The handedness flip from
// the reflection is compensated by invertCulling on the reflection camera.
void RenderSystem::updateMirrors(Entity mainCameraEntity){
    auto mirrors = scene->getComponentArray<MirrorComponent>();
    if (mirrors->size() == 0)
        return;

    CameraComponent* mainCameraPtr = scene->findComponent<CameraComponent>(mainCameraEntity);
    if (!mainCameraPtr)
        return;

    // copy main camera state before any camera creation: creating a reflection
    // camera below can reallocate the camera/transform arrays and dangle references
    const Matrix4 mainView = mainCameraPtr->viewMatrix;
    // main camera eye in world space (reflected per-mirror below); derived from the
    // view matrix so it doesn't depend on the (possibly dangling) camera component
    const Vector3 mainEye = mainView.inverse() * Vector3(0.0f, 0.0f, 0.0f);
    const CameraType mainType = mainCameraPtr->type;
    const float mYfov = mainCameraPtr->yfov, mAspect = mainCameraPtr->aspect;
    const float mNear = mainCameraPtr->nearClip, mFar = mainCameraPtr->farClip;
    const float mLeft = mainCameraPtr->leftClip, mRight = mainCameraPtr->rightClip;
    const float mBottom = mainCameraPtr->bottomClip, mTop = mainCameraPtr->topClip;
    // a mirror reflects the layers its viewer sees
    const uint32_t mCullingMask = mainCameraPtr->cullingMask;
    // mainCameraPtr must not be used past here (may dangle after camera creation)

    // Creating a reflection camera appends to the camera/transform arrays but never
    // the mirror array, so indexed iteration over mirrors stays valid in one pass;
    // transforms/cameras are (re)fetched after creation since their arrays may move.
    for (size_t i = 0; i < mirrors->size(); i++){
        Entity entity = mirrors->getEntity(i);

        Entity camEntity = getMirrorCamera(entity);
        if (camEntity == NULL_ENTITY || !scene->isEntityCreated(camEntity)){
            camEntity = createMirrorCamera(entity);
            // the loop counting render-to-texture cameras has run, but draw() still
            // renders this one in the same frame
            hasMultipleCameras = true;
        }

        CameraComponent* refCam = scene->findComponent<CameraComponent>(camEntity);
        Transform* refCamTransform = scene->findComponent<Transform>(camEntity);
        Transform* mirrorTransform = scene->findComponent<Transform>(entity);
        if (!refCam || !refCamTransform || !mirrorTransform)
            continue;

        // mirror plane in world space (entity position, world-rotated normal)
        Vector3 worldNormal = (mirrorTransform->worldRotation * mirrors->getComponentFromIndex(i).normal).normalize();
        Plane plane(worldNormal, mirrorTransform->worldPosition);

        // reflection view keeps the handedness flip (true mirror); winding is
        // restored by invertCulling so reflected geometry shows front faces
        Matrix4 reflectMat = Matrix4::reflectMatrix(plane);
        refCam->customViewMatrix = mainView * reflectMat;
        refCam->hasCustomViewMatrix = true;

        // Give the reflection camera the reflected eye position. The custom view
        // matrix overrides the view, but distance-based effects still read the
        // transform's worldPosition (terrain CDLOD detail + morph origin, transparent
        // sorting, per-camera lighting eye) — without this they'd key off the origin
        // and the reflected terrain would morph at the wrong LOD (visible cracks).
        refCamTransform->worldPosition = reflectMat * mainEye;
        refCam->invertCulling = true;
        refCam->renderToTexture = true;

        // match the main camera projection so the reflection lines up 1:1
        refCam->type = mainType;
        refCam->yfov = mYfov;
        refCam->aspect = mAspect;
        refCam->nearClip = mNear;
        refCam->farClip = mFar;
        refCam->leftClip = mLeft;
        refCam->rightClip = mRight;
        refCam->bottomClip = mBottom;
        refCam->topClip = mTop;
        refCam->cullingMask = mCullingMask;

        // recompute the reflection camera matrices with the override applied
        updateCamera(*refCam, *refCamTransform);

        // oblique near-plane clipping: move the reflection camera's near plane onto
        // the mirror plane so geometry behind the mirror doesn't leak into the
        // reflection. The clip plane (mirror plane, normal toward the front/reflecting
        // side) is taken to the reflection camera's view space via inverse-transpose.
        // Applied ONLY to viewProjectionMatrix (used by meshes), NOT projectionMatrix:
        // the skybox renders from projectionMatrix and the oblique depth would clip it.
        // Only the depth row changes, so projective sampling (clip.xy/w) is unaffected.
        Vector4 worldClipPlane(worldNormal.x, worldNormal.y, worldNormal.z, plane.d);
        Vector4 viewClipPlane = refCam->viewMatrix.inverse().transpose() * worldClipPlane;
        Matrix4 obliqueProjection = refCam->projectionMatrix.obliqueNearClip(viewClipPlane);
        refCam->viewProjectionMatrix = obliqueProjection * refCam->viewMatrix;
        updateCameraFrustumPlanes(refCam->viewProjectionMatrix, refCam->frustumPlanes);

        MeshComponent* mesh = scene->findComponent<MeshComponent>(entity);
        if (mesh && mesh->numSubmeshes > 0){
            // (re)bind the reflection framebuffer to the mesh base texture every frame.
            // Doing this per-frame (not just at camera creation) re-heals the mirror
            // after a play-stop snapshot restore, which clears the framebuffer binding
            // (framebuffer textures aren't serialized). One-shot: no rebind once matched.
            Texture& baseTex = mesh->submeshes[0].material.baseColorTexture;
            if (baseTex.getFramebuffer() != refCam->framebuffer){
                baseTex.setFramebuffer(refCam->framebuffer);
                mesh->needReload = true;
            }

            // hand the reflection VP to the mirror mesh for projective sampling
            // (USE_MIRROR shader); logical matrix, the shader handles the texture origin
            mesh->mirrorViewProjection = refCam->viewProjectionMatrix;
        }
    }
}

void RenderSystem::updateSkyViewProjection(SkyboxComponent& sky, CameraComponent& camera){
    sky.skyViewProjectionMatrix = skyViewProjection(sky, camera);
    sky.needUpdateSky = false;
}

Matrix4 RenderSystem::skyViewProjection(const SkyboxComponent& sky, const CameraComponent& camera) const{
    Matrix4 skyViewMatrix = camera.viewMatrix;

    skyViewMatrix.set(3,0,0);
    skyViewMatrix.set(3,1,0);
    skyViewMatrix.set(3,2,0);
    skyViewMatrix.set(3,3,1);
    skyViewMatrix.set(2,3,0);
    skyViewMatrix.set(1,3,0);
    skyViewMatrix.set(0,3,0);

    Matrix4 rotationMatrix = Quaternion(0, sky.rotation, 0).getRotationMatrix();

    Matrix4 viewProjection = camera.projectionMatrix * skyViewMatrix * rotationMatrix;

    if (isRenderingFlipped(camera)){
        static const Matrix4 flipYMatrix = Matrix4::scaleMatrix(Vector3(1, -1, 1));
        viewProjection = flipYMatrix * viewProjection;
    }

    return viewProjection;
}

void RenderSystem::updatePoints(PointsComponent& points, Transform& transform, CameraComponent& camera, Transform& camTransform){
    float worldScale = std::max(transform.worldScale.x, std::max(transform.worldScale.y, transform.worldScale.z));

    points.numVisible = 0;
    size_t pointsSize = (points.points.size() < points.maxPoints)? points.points.size() : points.maxPoints;
    if (points.renderPoints.size() < pointsSize){
        points.renderPoints.resize(pointsSize);
    }

    for (int i = 0; i < pointsSize; i++){
        if (points.points[i].visible){
            PointRenderData& renderPoint = points.renderPoints[points.numVisible];
            renderPoint.position = points.points[i].position;
            renderPoint.color = points.points[i].color;
            renderPoint.size = points.points[i].size * worldScale;
            renderPoint.rotation = points.points[i].rotation;
            renderPoint.textureRect = points.points[i].textureRect;
            points.numVisible++;
        }
    }
    points.renderPoints.resize(points.numVisible);

    if (points.loaded)
        points.needUpdateBuffer = true;
}

void RenderSystem::sortPoints(PointsComponent& points, Transform& transform, CameraComponent& camera, Transform& camTransform){
    Vector3 camDir = (camTransform.worldPosition - camera.worldTarget).normalize();

    auto comparePoints = [&transform, &camDir](const PointRenderData& a, const PointRenderData& b) -> bool {
        return (transform.modelMatrix * a.position).dotProduct(camDir) < (transform.modelMatrix * b.position).dotProduct(camDir);
    };
    std::sort(points.renderPoints.begin(), points.renderPoints.end(), comparePoints);

    if (points.loaded)
        points.needUpdateBuffer = true;
}

void RenderSystem::updateTerrain(TerrainComponent& terrain, const Transform& transform, CameraComponent& camera, const Transform& cameraTransform, TerrainView& view){
    if (terrain.heightMapLoaded){
        for (int i = 0; i < terrain.numNodes; i++){
            terrain.nodes[i].visible = false;
        }

        for (int s = 0; s < 2; s++){
            view.nodesbuffer[s].clear();
        }

        for (int i = 0; i < (terrain.rootGridSize*terrain.rootGridSize); i++){
            terrainNodeLODSelect(terrain, transform, camera, cameraTransform, terrain.nodes[terrain.grid[i]], terrain.levels-1, view);
        }

        view.needUpdateNodesBuffer = true;

        view.nodesEyePos = Vector3(cameraTransform.worldPosition.x, cameraTransform.worldPosition.y, cameraTransform.worldPosition.z);
    }
}

AABB RenderSystem::getTerrainNodeAABB(const Transform& transform, TerrainNode& terrainNode){
    float halfSize = terrainNode.size / 2.0f;
    Vector3 localMin(terrainNode.position.x - halfSize, terrainNode.minHeight, terrainNode.position.y - halfSize);
    Vector3 localMax(terrainNode.position.x + halfSize, terrainNode.maxHeight, terrainNode.position.y + halfSize);

    AABB box(localMin, localMax);
    box.transform(transform.modelMatrix);
    return box;
};

bool RenderSystem::isTerrainNodeInSphere(Vector3 position, float radius, const AABB& box) {
    float r2 = radius*radius;

    Vector3 c1 = box.getMinimum();
    Vector3 c2 = box.getMaximum();
    Vector3 distV;

    if (position.x < c1.x) distV.x = (position.x - c1.x);
    else if (position.x > c2.x) distV.x = (position.x - c2.x);
    if (position.y < c1.y) distV.y = (position.y - c1.y);
    else if (position.y > c2.y) distV.y = (position.y - c2.y);
    if (position.z < c1.z) distV.z = (position.z - c1.z);
    else if (position.z > c2.z) distV.z = (position.z - c2.z);

    float dist2 = distV.dotProduct(distV);

    return dist2 <= r2;
}

// appends one selected CDLOD node's per-instance data (position, size, range, resolution)
static void appendTerrainNode(InterleavedBuffer& buffer, const TerrainNode& node){
    buffer.addVector2(AttributeType::TERRAINNODEPOSITION, node.position);
    buffer.addFloat(AttributeType::TERRAINNODESIZE, node.size);
    buffer.addFloat(AttributeType::TERRAINNODERANGE, node.currentRange);
    buffer.addFloat(AttributeType::TERRAINNODERESOLUTION, node.resolution);
}

bool RenderSystem::terrainNodeLODSelect(TerrainComponent& terrain, const Transform& transform, CameraComponent& camera, const Transform& cameraTransform, TerrainNode& terrainNode, int lodLevel, TerrainView& view){
    terrainNode.currentRange = terrain.ranges[lodLevel];

    AABB box = getTerrainNodeAABB(transform, terrainNode);

    if (!isTerrainNodeInSphere(cameraTransform.worldPosition, terrain.ranges[lodLevel], box)) {
        // no node or child nodes were selected
        return false;
    }

    if (!isInsideCamera(camera, box)) {
        // return true to parent node does not select itself over area
        return true;
    }

    if( lodLevel == 0 ) {
        //Full resolution
        terrainNode.resolution = terrain.resolution;
        terrainNode.visible = true;
        appendTerrainNode(view.nodesbuffer[0], terrainNode);

        return true;
    } else {

        if( !isTerrainNodeInSphere(cameraTransform.worldPosition, terrain.ranges[lodLevel-1], box) ) {
            //Full resolution
            terrainNode.resolution = terrain.resolution;
            terrainNode.visible = true;
            appendTerrainNode(view.nodesbuffer[0], terrainNode);
        } else {
            for (int i = 0; i < 4; i++) {
                TerrainNode& child = terrain.nodes[terrainNode.childs[i]];
                if (!terrainNodeLODSelect(terrain, transform, camera, cameraTransform, child, lodLevel-1, view)){
                    //Half resolution
                    child.resolution = terrain.resolution / 2;
                    child.currentRange = terrainNode.currentRange;
                    child.visible = true;
                    appendTerrainNode(view.nodesbuffer[1], child);
                }
            }
        }

        return true;
    }

}

// Fills tilemapDrawRanges with the index runs of a tilemap submesh that survive
// frustum culling, merging chunks that are adjacent in the index buffer so a visible
// region costs as few draw calls as it can. Returns false when nothing of the submesh
// is visible; an empty range list on success means "draw the submesh whole", the
// fallback for a submesh the chunk data does not cover.
bool RenderSystem::selectTilemapChunks(TilemapComponent& tilemap, unsigned int submeshIndex, const float cameraFar, const Plane frustumPlanes[6]){
    tilemapDrawRanges.clear();

    if ((submeshIndex + 1) >= tilemap.chunkStart.size())
        return true;

    const unsigned int begin = tilemap.chunkStart[submeshIndex];
    const unsigned int end = tilemap.chunkStart[submeshIndex + 1];

    for (unsigned int c = begin; c < end; c++){
        const TilemapChunk& chunk = tilemap.chunks[c];

        if (!isInsideCamera(cameraFar, frustumPlanes, chunk.worldAABB))
            continue;

        if (!tilemapDrawRanges.empty() && (tilemapDrawRanges.back().offset + tilemapDrawRanges.back().count) == chunk.offset){
            tilemapDrawRanges.back().count += chunk.count;
        }else{
            tilemapDrawRanges.push_back({chunk.offset, chunk.count});
        }
    }

    return !tilemapDrawRanges.empty();
}

void RenderSystem::updateCameraSize(Entity entity){
    updateCameraSizeForCanvas(entity, (float)Engine::getCanvasWidth(), (float)Engine::getCanvasHeight());
}

void RenderSystem::updateCameraSizeForCanvas(Entity entity, float canvasWidth, float canvasHeight){
    Signature signature = scene->getSignature(entity);
    if (signature.test(scene->getComponentId<CameraComponent>())){
        CameraComponent& camera = scene->getComponent<CameraComponent>(entity);

        Rect rect;
        if (camera.renderToTexture) {
            rect = Rect(0, 0, camera.framebuffer->getWidth(), camera.framebuffer->getHeight());
        }else{
            rect = Rect(0, 0, canvasWidth, canvasHeight);
        }

        if (camera.autoResize){
            float newLeft = rect.getX();
            float newBottom = rect.getY();
            float newRight = rect.getWidth();
            float newTop = rect.getHeight();
            if (newRight <= 0.0f || newTop <= 0.0f) return;
            float newAspect = newRight / newTop;

            if ((camera.leftClip != newLeft) || (camera.bottomClip != newBottom) || (camera.rightClip != newRight) || (camera.topClip != newTop) || (camera.aspect != newAspect)){
                camera.leftClip = newLeft;
                camera.bottomClip = newBottom;
                camera.rightClip = newRight;
                camera.topClip = newTop;
                camera.aspect = newAspect;

                camera.needUpdate = true;
            }
        }
    }
}

bool RenderSystem::isInsideCamera(const float cameraFar, const Plane frustumPlanes[6], const AABB& box){
    if (box.isNull() || box.isInfinite())
        return false;

    Vector3 centre = box.getCenter();
    Vector3 halfSize = box.getHalfSize();

    for (int plane = 0; plane < 6; ++plane){
        if (plane == FRUSTUM_PLANE_FAR && cameraFar == 0)
            continue;

        Plane::Side side = frustumPlanes[plane].getSide(centre, halfSize);
        if (side == Plane::Side::NEGATIVE_SIDE){
            return false;
        }
    }

    return true;
}

bool RenderSystem::isInsideCamera(CameraComponent& camera, const AABB& box){
    return isInsideCamera(camera.farClip, camera.frustumPlanes, box);
}

bool RenderSystem::isInsideCamera(CameraComponent& camera, const Vector3& point){
    for (int plane = 0; plane < 6; ++plane){
        if (plane == FRUSTUM_PLANE_FAR && camera.farClip == 0)
            continue;

        if (camera.frustumPlanes[plane].getSide(point) == Plane::Side::NEGATIVE_SIDE){
            return false;
        }
    }

    return true;
}

bool RenderSystem::isInsideCamera(CameraComponent& camera, const Vector3& center, const float& radius){
    return isInsideCamera(camera.farClip, camera.frustumPlanes, center, radius);
}

bool RenderSystem::isInsideCamera(const float cameraFar, const Plane frustumPlanes[6], const Vector3& center, const float& radius){
    for (int plane = 0; plane < 6; ++plane){
        if (plane == FRUSTUM_PLANE_FAR && cameraFar == 0)
            continue;

        if (frustumPlanes[plane].getDistance(center) < -radius){
            return false;
        }
    }

    return true;
}

void RenderSystem::needReloadPoints() {
    auto pointsArray = scene->getComponentArray<PointsComponent>();
    for (int i = 0; i < pointsArray->size(); i++) {
        PointsComponent& points = pointsArray->getComponentFromIndex(i);
        points.needReload = true;
    }
}

void RenderSystem::needReloadLines() {
    auto linesArray = scene->getComponentArray<LineRendererComponent>();
    for (int i = 0; i < linesArray->size(); i++) {
        LineRendererComponent& lines = linesArray->getComponentFromIndex(i);
        lines.needReload = true;
    }
}

void RenderSystem::needReloadMeshes() {
    auto meshes = scene->getComponentArray<MeshComponent>();
    for (int i = 0; i < meshes->size(); i++) {
        MeshComponent& mesh = meshes->getComponentFromIndex(i);
        mesh.needReload = true;
    }
}

void RenderSystem::needReloadMeshVariants() {
    auto meshes = scene->getComponentArray<MeshComponent>();
    for (int i = 0; i < meshes->size(); i++) {
        MeshComponent& mesh = meshes->getComponentFromIndex(i);
        // one not loaded (or already reloading) loads with the new environment anyway
        if (mesh.loaded && !mesh.needReload)
            mesh.needVariantReload = true;
        else
            mesh.needReload = true;
    }
}

// Whether every shader a reload of the mesh would take now is built (or failed: the load
// falls back), asking for them as loadMesh does (ShaderPool::get starts their builds). The
// variants as loadMesh resolves them, from the submeshes' attributes of their last load
// (a variant-only reload keeps the buffers).
bool RenderSystem::meshVariantShadersReady(Entity entity, const MeshComponent& mesh, const InstancedMeshComponent* instmesh, const TerrainComponent* terrain){
    if (!Engine::isViewLoaded())
        return false;
    auto ready = [](ShaderType type, uint32_t properties, uint16_t customId){
        return ShaderPool::get(type, properties, customId)->isCreated() || ShaderPool::isShaderBuildFailed(type, properties, customId);
    };
    const MeshShaderEnvironment shaderEnvironment = getMeshShaderEnvironment();
    const bool hasMirror = (scene->findComponent<MirrorComponent>(entity) != NULL);
    const std::string& sceneMeshShader = scene->getDefaultCustomShader(ShaderType::MESH);
    bool allReady = true;
    for (unsigned int i = 0; i < mesh.numSubmeshes; i++){
        const Submesh& submesh = mesh.submeshes[i];
        const SubmeshShaderInputs inputs = getSubmeshShaderInputs(mesh, submesh, terrain, instmesh, hasMirror, sceneMeshShader);
        const SubmeshShaderVariant variant = getSubmeshShaderVariant(shaderEnvironment, inputs);
        allReady = ready(ShaderType::MESH, variant.properties,
                         ShaderPool::registerCustomShader(getSubmeshShaderReference(mesh, submesh, sceneMeshShader))) && allReady;
        if (variant.depth){
            allReady = ready(ShaderType::DEPTH, variant.depthProperties,
                             ShaderPool::registerCustomShader(getSubmeshDepthShaderReference(mesh, submesh, sceneMeshShader))) && allReady;
        }
        if (variant.gbuffer){
            allReady = ready(ShaderType::GBUFFER, variant.gbufferProperties, 0) && allReady;
        }
        if (hasExtraPass(submesh, terrain != nullptr)){
            const SubmeshShaderVariant passVariant = getSubmeshShaderVariant(shaderEnvironment, getExtraPassShaderInputs(inputs, submesh.material.extraPass));
            allReady = ready(ShaderType::MESH, passVariant.properties, ShaderPool::registerCustomShader(getSubmeshExtraPassReference(submesh))) && allReady;
        }
    }
    return allReady;
}

void RenderSystem::needReloadUIs() {
    auto uis = scene->getComponentArray<UIComponent>();
    for (int i = 0; i < uis->size(); i++) {
        UIComponent& ui = uis->getComponentFromIndex(i);
        ui.needReload = true;
    }
}

void RenderSystem::needReloadSky() {
    auto skyArray = scene->getComponentArray<SkyboxComponent>();
    for (int i = 0; i < skyArray->size(); i++) {
        SkyboxComponent& sky = skyArray->getComponentFromIndex(i);
        sky.needReload = true;
    }
}

bool RenderSystem::isAllLoaded() const{
    size_t loaded;
    size_t total;
    getLoadCount(loaded, total);

    return loaded == total;
}

// A load that cannot finish: a shader it asked for is not in the build or failed to build
// (ShaderPool stops trying it), so the object is not drawn. Counted as settled, so a loading
// screen or a one-frame scene does not wait for it.
static bool isShaderUnavailable(const std::shared_ptr<ShaderRender>& shader, ShaderType type, uint32_t properties, uint16_t customId){
    return shader && !shader->isCreated() && ShaderPool::isShaderBuildFailed(type, properties, customId);
}

static bool isMeshShaderUnavailable(const MeshComponent& mesh){
    for (unsigned int s = 0; s < mesh.numSubmeshes; s++){
        const Submesh& submesh = mesh.submeshes[s];
        if (isShaderUnavailable(submesh.shader, ShaderType::MESH, submesh.shaderProperties, submesh.customShaderId) ||
            isShaderUnavailable(submesh.depthShader, ShaderType::DEPTH, submesh.depthShaderProperties, submesh.customDepthShaderId) ||
            isShaderUnavailable(submesh.gbufferShader, ShaderType::GBUFFER, submesh.gbufferShaderProperties, 0)){
            return true;
        }
    }
    return false;
}

void RenderSystem::getLoadCount(size_t& loaded, size_t& total) const{
    loaded = 0;
    total = 0;

    auto count = [&](bool isLoaded){
        total++;
        if (isLoaded)
            loaded++;
    };

    // Check MeshComponents
    auto meshes = scene->getComponentArray<MeshComponent>();
    for (int i = 0; i < meshes->size(); i++) {
        const MeshComponent& mesh = meshes->getComponentFromIndex(i);
        // a mesh drawing its previous variant while the new one builds is still loading
        count((mesh.loaded && !mesh.needVariantReload) || (!mesh.loadCalled && isMeshShaderUnavailable(mesh)));
    }

    // Check UIComponents
    auto uis = scene->getComponentArray<UIComponent>();
    for (int i = 0; i < uis->size(); i++) {
        const UIComponent& ui = uis->getComponentFromIndex(i);
        // An empty UI never loads: loadUI() retries each frame and nothing sets needReload.
        const size_t uiBufferSize = std::max(ui.buffer.getSize(),
                                             (size_t)ui.minBufferCount * ui.buffer.getStride());
        if (uiBufferSize > 0)
            count(ui.loaded || (!ui.loadCalled && isShaderUnavailable(ui.shader, ShaderType::UI, ui.shaderProperties, ui.customShaderId)));
    }

    // Check PointsComponents
    auto pointsArray = scene->getComponentArray<PointsComponent>();
    for (int i = 0; i < pointsArray->size(); i++) {
        const PointsComponent& points = pointsArray->getComponentFromIndex(i);
        count(points.loaded || (!points.loadCalled && isShaderUnavailable(points.shader, ShaderType::POINTS, points.shaderProperties, points.customShaderId)));
    }

    // Check LineRendererComponents
    auto linesArray = scene->getComponentArray<LineRendererComponent>();
    for (int i = 0; i < linesArray->size(); i++) {
        const LineRendererComponent& lines = linesArray->getComponentFromIndex(i);
        // An empty Lines never loads: addLine() only sets needReload when maxLines grows.
        if (!lines.lines.empty())
            count(lines.loaded || (!lines.loadCalled && isShaderUnavailable(lines.shader, ShaderType::LINES, lines.shaderProperties, lines.customShaderId)));
    }

    // Check the SkyboxComponent: only the first is drawn (and loaded), so another one would
    // keep the scene from ever counting as loaded
    auto skyArray = scene->getComponentArray<SkyboxComponent>();
    if (skyArray->size() > 0) {
        const SkyboxComponent& sky = skyArray->getComponentFromIndex(0);
        count(sky.loaded || (!sky.loadCalled && isShaderUnavailable(sky.shader, ShaderType::SKYBOX, sky.shaderProperties, sky.customShaderId)));
    }

    // the atmosphere's passes (their shaders, the clouds' noise bake)
    count(!atmosphere.isLoading());
}

void RenderSystem::updateCameraFrustumPlanes(const Matrix4 viewProjectionMatrix, Plane* frustumPlanes){

    frustumPlanes[FRUSTUM_PLANE_LEFT].normal.x = viewProjectionMatrix[0][3] + viewProjectionMatrix[0][0];
    frustumPlanes[FRUSTUM_PLANE_LEFT].normal.y = viewProjectionMatrix[1][3] + viewProjectionMatrix[1][0];
    frustumPlanes[FRUSTUM_PLANE_LEFT].normal.z = viewProjectionMatrix[2][3] + viewProjectionMatrix[2][0];
    frustumPlanes[FRUSTUM_PLANE_LEFT].d = viewProjectionMatrix[3][3] + viewProjectionMatrix[3][0];

    frustumPlanes[FRUSTUM_PLANE_RIGHT].normal.x = viewProjectionMatrix[0][3] - viewProjectionMatrix[0][0];
    frustumPlanes[FRUSTUM_PLANE_RIGHT].normal.y = viewProjectionMatrix[1][3] - viewProjectionMatrix[1][0];
    frustumPlanes[FRUSTUM_PLANE_RIGHT].normal.z = viewProjectionMatrix[2][3] - viewProjectionMatrix[2][0];
    frustumPlanes[FRUSTUM_PLANE_RIGHT].d = viewProjectionMatrix[3][3] - viewProjectionMatrix[3][0];

    frustumPlanes[FRUSTUM_PLANE_TOP].normal.x = viewProjectionMatrix[0][3] - viewProjectionMatrix[0][1];
    frustumPlanes[FRUSTUM_PLANE_TOP].normal.y = viewProjectionMatrix[1][3] - viewProjectionMatrix[1][1];
    frustumPlanes[FRUSTUM_PLANE_TOP].normal.z = viewProjectionMatrix[2][3] - viewProjectionMatrix[2][1];
    frustumPlanes[FRUSTUM_PLANE_TOP].d = viewProjectionMatrix[3][3] - viewProjectionMatrix[3][1];

    frustumPlanes[FRUSTUM_PLANE_BOTTOM].normal.x = viewProjectionMatrix[0][3] + viewProjectionMatrix[0][1];
    frustumPlanes[FRUSTUM_PLANE_BOTTOM].normal.y = viewProjectionMatrix[1][3] + viewProjectionMatrix[1][1];
    frustumPlanes[FRUSTUM_PLANE_BOTTOM].normal.z = viewProjectionMatrix[2][3] + viewProjectionMatrix[2][1];
    frustumPlanes[FRUSTUM_PLANE_BOTTOM].d = viewProjectionMatrix[3][3] + viewProjectionMatrix[3][1];

    frustumPlanes[FRUSTUM_PLANE_NEAR].normal.x = viewProjectionMatrix[0][3] + viewProjectionMatrix[0][2];
    frustumPlanes[FRUSTUM_PLANE_NEAR].normal.y = viewProjectionMatrix[1][3] + viewProjectionMatrix[1][2];
    frustumPlanes[FRUSTUM_PLANE_NEAR].normal.z = viewProjectionMatrix[2][3] + viewProjectionMatrix[2][2];
    frustumPlanes[FRUSTUM_PLANE_NEAR].d = viewProjectionMatrix[3][3] + viewProjectionMatrix[3][2];

    frustumPlanes[FRUSTUM_PLANE_FAR].normal.x = viewProjectionMatrix[0][3] - viewProjectionMatrix[0][2];
    frustumPlanes[FRUSTUM_PLANE_FAR].normal.y = viewProjectionMatrix[1][3] - viewProjectionMatrix[1][2];
    frustumPlanes[FRUSTUM_PLANE_FAR].normal.z = viewProjectionMatrix[2][3] - viewProjectionMatrix[2][2];
    frustumPlanes[FRUSTUM_PLANE_FAR].d = viewProjectionMatrix[3][3] - viewProjectionMatrix[3][2];

    for (int i=0; i<6; i++){
        float length = frustumPlanes[i].normal.normalizeL();
        frustumPlanes[i].d /= length;
    }
}

void RenderSystem::updateInstancedMesh(InstancedMeshComponent& instmesh, MeshComponent& mesh, Transform& transform, CameraComponent& camera, Transform& camTransform){
    instmesh.renderInstances.clear();
    instmesh.renderInstances.reserve(instmesh.instances.size());
    instmesh.renderBounds.clear();
    instmesh.renderBounds.reserve(instmesh.instances.size());

    const bool hasBounds = !mesh.verticesAABB.isNull() && !mesh.verticesAABB.isInfinite();
    const Vector3 localCenter = hasBounds ? mesh.verticesAABB.getCenter() : Vector3::ZERO;
    const float localRadius = hasBounds ? mesh.verticesAABB.getHalfSize().length() : 0.0f;

    const float cullEnd = instanceFadeCullEnd(instmesh, mesh, *scene);
    instmesh.lastFadeCullEnd = cullEnd;
    instmesh.lastFadeCullEye = instmesh.fadeEyeLocal;
    // small margin for CPU/GPU float differences at the edge
    const float cullRadius = cullEnd + std::max(0.0001f, cullEnd * 0.00001f);
    const float cullRadiusSquared = cullRadius * cullRadius;

    Quaternion bRotation;
    if (instmesh.instancedBillboard){
        Matrix4 m1 = camera.viewMatrix.inverse();
        bRotation.fromRotationMatrix(m1);
        bRotation = transform.worldRotation.inverse() * bRotation;
    }

    // Not AABB::ZERO: that is a finite box at the origin, so merging into it stretches every
    // instanced mesh back to its own origin and defeats culling.
    mesh.aabb.setNull();
    instmesh.numVisible = 0;
    size_t instancesSize = (instmesh.instances.size() < instmesh.maxInstances)? instmesh.instances.size() : instmesh.maxInstances;
    for (int i = 0; i < instancesSize; i++){
        if (instmesh.instances[i].visible){
            // model-local XZ distance from the instance origin, as in mesh.vert
            const float dx = instmesh.instances[i].position.x - instmesh.fadeEyeLocal.x;
            const float dz = instmesh.instances[i].position.z - instmesh.fadeEyeLocal.z;
            if (cullEnd >= 0.0f && dx * dx + dz * dz > cullRadiusSquared){
                continue;
            }
            Matrix4 translateMatrix = Matrix4::translateMatrix(instmesh.instances[i].position);
            Matrix4 rotationMatrix;
            Matrix4 scaleMatrix = Matrix4::scaleMatrix(instmesh.instances[i].scale);

            if (instmesh.instancedBillboard){
                rotationMatrix = bRotation.getRotationMatrix();
            }else{
                rotationMatrix = instmesh.instances[i].rotation.getRotationMatrix();
            }

            Matrix4 instanceMatrix = translateMatrix * rotationMatrix * scaleMatrix;

            instmesh.renderInstances.push_back({});
            instmesh.renderInstances[instmesh.numVisible].instanceMatrix = instanceMatrix;
            instmesh.renderInstances[instmesh.numVisible].color = instmesh.instances[i].color;
            instmesh.renderInstances[instmesh.numVisible].textureRect = instmesh.instances[i].textureRect;
            instmesh.numVisible++;

            const Vector3& scale = instmesh.instances[i].scale;
            InstanceBounds bounds;
            bounds.scale = std::max(std::abs(scale.x), std::max(std::abs(scale.y), std::abs(scale.z)));
            bounds.center = instanceMatrix * localCenter;
            bounds.radius = localRadius * bounds.scale;
            instmesh.renderBounds.push_back(bounds);

            mesh.aabb.merge(instanceMatrix * mesh.verticesAABB);
        }
    }

    mesh.needUpdateAABB = true;
    instanceViewsDirty = true;

    if (mesh.loaded)
        instmesh.needUpdateBuffer = true;
}

void RenderSystem::sortInstancedMesh(InstancedMeshComponent& instmesh, MeshComponent& mesh, Transform& transform, CameraComponent& camera, Transform& camTransform){
    Vector3 camDir = (camTransform.worldPosition - camera.worldTarget).normalize();

    // the bounds follow the sort so the instance views keep matching
    const auto order = detail::instanceDepthOrder(instmesh.renderInstances, transform.modelMatrix, camDir);
    std::vector<InstanceRenderData> sortedInstances(order.size());
    std::vector<InstanceBounds> sortedBounds(std::min(order.size(), instmesh.renderBounds.size()));
    for (unsigned int i = 0; i < order.size(); i++){
        sortedInstances[i] = instmesh.renderInstances[order[i]];
        if (order[i] < sortedBounds.size() && i < sortedBounds.size())
            sortedBounds[i] = instmesh.renderBounds[order[i]];
    }
    instmesh.renderInstances.swap(sortedInstances);
    instmesh.renderBounds.swap(sortedBounds);
    instanceViewsDirty = true;

    if (mesh.loaded)
        instmesh.needUpdateBuffer = true;
}

void RenderSystem::configureLightShadowNearFar(LightComponent& light, const CameraComponent& camera){
    if (light.automaticShadowCamera){
        light.shadowCameraNearFar.x = camera.nearClip;
        if (light.range == 0.0){
            light.shadowCameraNearFar.y = camera.farClip;
        }else{
            light.shadowCameraNearFar.y = light.range;
        }
    }
}

float RenderSystem::lerp(float a, float b, float fraction) {
    return (a * (1.0f - fraction)) + (b * fraction);
}

Matrix4 RenderSystem::getDirLightProjection(const Matrix4& viewMatrix, const Matrix4& sceneCameraInv, float shadowMaxDistance, const Vector3& lightDirection, const Vector3& cameraPosition){
    // Extend the frustum backwards to include more geometry
    float extension = shadowMaxDistance * 0.5f;

    Matrix4 t = viewMatrix * sceneCameraInv;
    // Add additional points behind the camera
    Vector3 backwardOffset = -lightDirection * extension;
    // runs per camera and frame: a fixed array, no allocation
    Vector4 frustumCorners[9] = {
        t * Vector4(-1.f, 1.f, -1.f, 1.f),
        t * Vector4(1.f, 1.f, -1.f, 1.f),
        t * Vector4(1.f, -1.f, -1.f, 1.f),
        t * Vector4(-1.f, -1.f, -1.f, 1.f),
        t * Vector4(-1.f, 1.f, 1.f, 1.f),
        t * Vector4(1.f, 1.f, 1.f, 1.f),
        t * Vector4(1.f, -1.f, 1.f, 1.f),
        t * Vector4(-1.f, -1.f, 1.f, 1.f),
        viewMatrix * Vector4(cameraPosition + backwardOffset, 1.0f)
    };

    float minX = std::numeric_limits<float>::max();
    float maxX = std::numeric_limits<float>::lowest();
    float minY = std::numeric_limits<float>::max();
    float maxY = std::numeric_limits<float>::lowest();
    float minZ = std::numeric_limits<float>::max();
    float maxZ = std::numeric_limits<float>::lowest();

    for (auto& p : frustumCorners){
        if (p.w != 0.0f) {
            p = p / p.w;
        }

        minX = std::min(minX, p.x);
        maxX = std::max(maxX, p.x);
        minY = std::min(minY, p.y);
        maxY = std::max(maxY, p.y);
        minZ = std::min(minZ, p.z);
        maxZ = std::max(maxZ, p.z);
    }

    float paddingX = (maxX - minX) * 0.05f;
    float paddingY = (maxY - minY) * 0.05f;
    float paddingZ = (maxZ - minZ) * 0.1f;

    minX -= paddingX;
    maxX += paddingX;
    minY -= paddingY;
    maxY += paddingY;
    minZ -= paddingZ;
    maxZ += paddingZ;

    return Matrix4::orthoMatrix(minX, maxX, minY, maxY, -maxZ, -minZ);
}

void RenderSystem::updateLightFromScene(LightComponent& light, Transform& transform, CameraComponent& camera, Transform& cameraTransform){
    light.worldDirection = transform.worldRotation * light.direction;
    light.worldUp = (transform.worldRotation * light.getLocalSpotProjectionUp()).normalized();

    if (hasShadows && (light.intensity > 0)){
        configureLightShadowNearFar(light, camera);

        unsigned int numCascades = light.numShadowCascades;
        computeLightCameras(light, transform, camera, cameraTransform, light.shadowCameraNearFar, light.cameras, numCascades);
        if (numCascades != light.numShadowCascades){
            light.numShadowCascades = numCascades;
            Log::warn("Can not have multiple cascades shadows when using ortho scene camera. Reducing num shadow cascades to 1");
        }
    }
}

// Shadow cameras of light seen through camera: the directional cascades fitted to its
// frustum, or the spot / point cameras. numCascades comes in as the light's and goes
// out as what the camera allows (one cascade with an orthographic camera).
void RenderSystem::computeLightCameras(const LightComponent& light, const Transform& transform, const CameraComponent& camera, const Transform& cameraTransform, const Vector2& shadowCameraNearFar, LightCamera* cameras, unsigned int& numCascades){
    {
        Vector3 up = Vector3(0, 1, 0);
        if (light.worldDirection.crossProduct(up) == Vector3::ZERO){
            up = Vector3(0, 0, 1);
        }

        if (light.type == LightType::DIRECTIONAL){

            float shadowSplitLogFactor = 0.7f;
            float shadowMaxDistance = shadowCameraNearFar.y;

            Matrix4 projectionMatrix[MAX_SHADOWCASCADES];
            Matrix4 viewMatrix;

            viewMatrix = Matrix4::lookAtMatrix(transform.worldPosition, light.worldDirection + transform.worldPosition, up);

            // cascades fitted to the frustum of the camera given (each camera's own)
            if (camera.type == CameraType::CAMERA_PERSPECTIVE) {

                float zNear = shadowCameraNearFar.x;
                float zFar = shadowCameraNearFar.y;
                float planeFarOffset = (zFar - zNear) * 0.05f;

                // Split perspective frustum to create cascades
                Matrix4 projection = camera.projectionMatrix;
                Matrix4 invProjection = projection.inverse();

                // near and far corners of the camera frustum in view space
                const Vector4 nearCorner = invProjection * Vector4(-1.f, 1.f, -1.f, 1.f);
                const Vector4 farCorner = invProjection * Vector4(-1.f, 1.f, 1.f, 1.f);

                zFar = std::min(zFar, -(farCorner / farCorner.w).z);
                zNear = -(nearCorner / nearCorner.w).z;
                float fov = atanf(1.f / projection[1][1]) * 2.f;
                float ratio = projection[1][1] / projection[0][0];

                // runs per camera and frame: fixed arrays, no allocation
                numCascades = std::clamp(numCascades, 1u, (unsigned int)MAX_SHADOWCASCADES);
                float splitFar[MAX_SHADOWCASCADES];
                float splitNear[MAX_SHADOWCASCADES];
                splitNear[0] = zNear;
                splitFar[numCascades - 1] = zFar;

                float j = 1.f;
                for (auto i = 0u; i < numCascades - 1; ++i, j += 1.f) {
                    splitFar[i] = lerp(
                        zNear + (j / (float)numCascades) * (zFar - zNear),
                        zNear * powf(zFar / zNear, j / (float)numCascades),
                        shadowSplitLogFactor
                    );
                    splitNear[i + 1] = splitFar[i];
                }

                // Get frustum box and create light ortho
                for (int ca = 0; ca < (int)numCascades; ca++) {
                    Matrix4 cascadeProjection = Matrix4::perspectiveMatrix(fov, ratio, splitNear[ca], splitFar[ca] + planeFarOffset);
                    Matrix4 sceneCameraInv = (cascadeProjection * camera.viewMatrix).inverse();

                    projectionMatrix[ca] = getDirLightProjection(viewMatrix, sceneCameraInv, shadowMaxDistance, light.worldDirection, cameraTransform.worldPosition);

                    cameras[ca].lightViewMatrix = viewMatrix;
                    cameras[ca].lightProjectionMatrix = projectionMatrix[ca];
                    cameras[ca].lightViewProjectionMatrix = projectionMatrix[ca] * viewMatrix;
                    cameras[ca].nearFar = Vector2(splitNear[ca], splitFar[ca]);
                    updateCameraFrustumPlanes(cameras[ca].lightViewProjectionMatrix, cameras[ca].frustumPlanes);
                }

            } else {
                // Orthographic camera handling remains the same
                if (numCascades > 1){
                    numCascades = 1;
                }

                Matrix4 sceneCameraInv = camera.viewProjectionMatrix.inverse();

                projectionMatrix[0] = getDirLightProjection(viewMatrix, sceneCameraInv, shadowMaxDistance, light.worldDirection, cameraTransform.worldPosition);

                cameras[0].lightViewMatrix = viewMatrix;
                cameras[0].lightProjectionMatrix = projectionMatrix[0];
                cameras[0].lightViewProjectionMatrix = projectionMatrix[0] * viewMatrix;
                cameras[0].nearFar = Vector2(-shadowMaxDistance, shadowMaxDistance);
                updateCameraFrustumPlanes(cameras[0].lightViewProjectionMatrix, cameras[0].frustumPlanes);

            }

        }else if (light.type == LightType::SPOT){
            Matrix4 projectionMatrix;
            Matrix4 viewMatrix;

            Vector3 projectionUp = light.spotMaskReady ? light.worldUp : up;
            float projectionAspect = light.spotMaskReady ? light.spotMaskAspect : 1.0f;
            viewMatrix = Matrix4::lookAtMatrix(transform.worldPosition, light.worldDirection + transform.worldPosition, projectionUp);

            projectionMatrix = Matrix4::perspectiveMatrix(
                acos(light.outerConeCos) * 2,
                projectionAspect,
                shadowCameraNearFar.x,
                shadowCameraNearFar.y);

            cameras[0].lightViewMatrix = viewMatrix;
            cameras[0].lightProjectionMatrix = projectionMatrix;
            cameras[0].lightViewProjectionMatrix = projectionMatrix * viewMatrix;
            cameras[0].nearFar = shadowCameraNearFar;
            updateCameraFrustumPlanes(cameras[0].lightViewProjectionMatrix, cameras[0].frustumPlanes);

        }else if (light.type == LightType::POINT){
            Matrix4 projectionMatrix;
            Matrix4 viewMatrix[6];

            viewMatrix[0] = Matrix4::lookAtMatrix(transform.worldPosition, Vector3( 1.f, 0.f, 0.f) + transform.worldPosition, Vector3(0.f, -1.f, 0.f));
            viewMatrix[1] = Matrix4::lookAtMatrix(transform.worldPosition, Vector3(-1.f, 0.f, 0.f) + transform.worldPosition, Vector3(0.f, -1.f, 0.f));
            viewMatrix[2] = Matrix4::lookAtMatrix(transform.worldPosition, Vector3( 0.f, 1.f, 0.f) + transform.worldPosition, Vector3(0.f,  0.f, 1.f));
            viewMatrix[3] = Matrix4::lookAtMatrix(transform.worldPosition, Vector3( 0.f,-1.f, 0.f) + transform.worldPosition, Vector3(0.f,  0.f,-1.f));
            viewMatrix[4] = Matrix4::lookAtMatrix(transform.worldPosition, Vector3( 0.f, 0.f, 1.f) + transform.worldPosition, Vector3(0.f, -1.f, 0.f));
            viewMatrix[5] = Matrix4::lookAtMatrix(transform.worldPosition, Vector3( 0.f, 0.f,-1.f) + transform.worldPosition, Vector3(0.f, -1.f, 0.f));

            projectionMatrix = Matrix4::perspectiveMatrix(Angle::degToRad(90), 1, shadowCameraNearFar.x, shadowCameraNearFar.y);

            Vector2 calculedNearFar;
            float nfsub = shadowCameraNearFar.y - shadowCameraNearFar.x;
            calculedNearFar.x = (shadowCameraNearFar.y + shadowCameraNearFar.x) / nfsub * 0.5f + 0.5f;
            calculedNearFar.y =-(shadowCameraNearFar.y * shadowCameraNearFar.x) / nfsub;

            for (int f = 0; f < 6; f++){
                cameras[f].lightViewMatrix = viewMatrix[f];
                cameras[f].lightProjectionMatrix = projectionMatrix;
                cameras[f].lightViewProjectionMatrix = projectionMatrix * viewMatrix[f];
                cameras[f].nearFar = calculedNearFar;
                updateCameraFrustumPlanes(cameras[f].lightViewProjectionMatrix, cameras[f].frustumPlanes);
            }
        }
    }
}

void RenderSystem::changeLoaded(void* data){
    if (!Engine::isViewLoaded())
        return;

    check_load_t* loadObj = (check_load_t*)data;

    Scene* scene = loadObj->scene;
    Entity entity = loadObj->entity;

    Signature signature = scene->getSignature(entity);

    if (signature.test(scene->getComponentId<MeshComponent>())){
        MeshComponent& mesh = scene->getComponent<MeshComponent>(entity);

        mesh.loaded = true;

    }else if (signature.test(scene->getComponentId<UIComponent>())){
        UIComponent& ui = scene->getComponent<UIComponent>(entity);

        ui.loaded = true;

    }else if (signature.test(scene->getComponentId<PointsComponent>())){
        PointsComponent& points = scene->getComponent<PointsComponent>(entity);

        points.loaded = true;

    }else if (signature.test(scene->getComponentId<LineRendererComponent>())){
        LineRendererComponent& lines = scene->getComponent<LineRendererComponent>(entity);

        lines.loaded = true;

    }else if (signature.test(scene->getComponentId<SkyboxComponent>())){
        SkyboxComponent& sky = scene->getComponent<SkyboxComponent>(entity);

        sky.loaded = true;
    }

    delete (check_load_t*)data;
}

void RenderSystem::changeDestroy(void* data){
    check_load_t* loadObj = (check_load_t*)data;

    Scene* scene = loadObj->scene;
    Entity entity = loadObj->entity;

    Signature signature = scene->getSignature(entity);

    if (signature.test(scene->getComponentId<MeshComponent>())){
        MeshComponent& mesh = scene->getComponent<MeshComponent>(entity);

        mesh.loaded = false;
        mesh.loadCalled = false;

    }else if (signature.test(scene->getComponentId<UIComponent>())){
        UIComponent& ui = scene->getComponent<UIComponent>(entity);

        ui.loaded = false;
        ui.loadCalled = false;

    }else if (signature.test(scene->getComponentId<PointsComponent>())){
        PointsComponent& points = scene->getComponent<PointsComponent>(entity);

        points.loaded = false;
        points.loadCalled = false;

    }else if (signature.test(scene->getComponentId<LineRendererComponent>())){
        LineRendererComponent& lines = scene->getComponent<LineRendererComponent>(entity);

        lines.loaded = false;
        lines.loadCalled = false;

    }else if (signature.test(scene->getComponentId<SkyboxComponent>())){
        SkyboxComponent& sky = scene->getComponent<SkyboxComponent>(entity);

        sky.loaded = false;
        sky.loadCalled = false;
    }

    delete (check_load_t*)data;
}

bool RenderSystem::usesAlphaMask(const Material& material, bool textureShadow){
    return material.alphaMode == MaterialAlphaMode::MASK ||
        (material.alphaMode == MaterialAlphaMode::AUTO && textureShadow);
}

// a texture cannot be sampled in the pass that renders into it; objects showing a
// camera's framebuffer must be skipped when drawing that same camera
bool RenderSystem::samplesCameraTarget(const CameraComponent& camera, const MeshComponent& mesh){
    if (!camera.renderToTexture)
        return false;

    for (unsigned int i = 0; i < mesh.numSubmeshes; i++){
        const Material& material = mesh.submeshes[i].material;
        if (material.baseColorTexture.getFramebuffer() == camera.framebuffer ||
            material.metallicRoughnessTexture.getFramebuffer() == camera.framebuffer ||
            material.normalTexture.getFramebuffer() == camera.framebuffer ||
            material.occlusionTexture.getFramebuffer() == camera.framebuffer ||
            material.emissiveTexture.getFramebuffer() == camera.framebuffer){
            return true;
        }
    }

    return false;
}

bool RenderSystem::samplesCameraTarget(const CameraComponent& camera, const Texture& texture){
    return camera.renderToTexture && texture.getFramebuffer() == camera.framebuffer;
}

template<typename T>
static bool isComponentDisabled(const Scene* scene, Entity entity, const Signature& signature){
    return signature.test(scene->getComponentId<T>()) && !scene->getComponent<T>(entity).enabled;
}

bool RenderSystem::isRenderActive(Entity entity, Signature signature) const{
    if (!scene->isEntityActiveInHierarchy(entity))
        return false;

#ifdef JASPERENGINE_EDITOR
    if (!passDrawsOverlays && isEditorOverlay(entity))
        return false;
    if (entity == passExcludedEntity)
        return false;
#endif

    return !isComponentDisabled<MeshComponent>(scene, entity, signature) &&
           !isComponentDisabled<SpriteComponent>(scene, entity, signature) &&
           !isComponentDisabled<ModelComponent>(scene, entity, signature) &&
           !isComponentDisabled<TilemapComponent>(scene, entity, signature) &&
           !isComponentDisabled<TerrainComponent>(scene, entity, signature) &&
           !isComponentDisabled<InstancedMeshComponent>(scene, entity, signature) &&
           !isComponentDisabled<ParticleSystemComponent>(scene, entity, signature) &&
           !isComponentDisabled<PointsComponent>(scene, entity, signature) &&
           !isComponentDisabled<LineRendererComponent>(scene, entity, signature) &&
           !isComponentDisabled<UIComponent>(scene, entity, signature);
}

bool RenderSystem::isRenderActive(Entity entity) const{
    return isRenderActive(entity, scene->getSignature(entity));
}

bool RenderSystem::isLayerVisible(Entity entity, uint32_t cullingMask) const{
    return cullingMask == LAYER_MASK_EVERYTHING || (cullingMask & (1u << scene->getEntityLayer(entity))) != 0;
}

bool RenderSystem::isLightActive(Entity entity, const LightComponent& light) const{
    return light.enabled && scene->isEntityActiveInHierarchy(entity);
}

bool RenderSystem::isLight2DActive(Entity entity, const Light2DComponent& light) const{
    return light.enabled && scene->isEntityActiveInHierarchy(entity);
}

Framebuffer* RenderSystem::targetFramebuffer() const{
#ifdef JASPERENGINE_EDITOR
    if (rv().secondary)
        return rv().target;
#endif
    return Engine::getFramebuffer();
}

Rect RenderSystem::targetViewRect() const{
#ifdef JASPERENGINE_EDITOR
    if (rv().secondary)
        return rv().viewRect;
#endif
    return Engine::getViewRect();
}

bool RenderSystem::isBaseLayer() const{
#ifdef JASPERENGINE_EDITOR
    if (rv().secondary)
        return rv().baseLayer;
#endif
    return Engine::getMainScene() == scene;
}

const LightCamera& RenderSystem::lightCamera(int lightIndex, const LightComponent& light, int cameraIndex) const{
    return lightCamera(ps(), lightIndex, light, cameraIndex);
}

unsigned int RenderSystem::lightCascades(int lightIndex, const LightComponent& light) const{
    return lightCascades(ps(), lightIndex, light);
}

// only directional cascades follow the camera: spot and point lights keep their own cameras.
// A camera's own cascades are kept by the light's base shadow slot.
const LightCamera& RenderSystem::lightCamera(const CameraShadows& shadows, int lightIndex, const LightComponent& light, int cameraIndex) const{
    if (shadows.ownLightCameras && light.type == LightType::DIRECTIONAL && light.shadowMapIndex >= 0 && light.shadowMapIndex < MAX_SHADOW_ATLAS_SLOTS)
        return shadows.lightCameras[light.shadowMapIndex].cameras[cameraIndex];
    return light.cameras[cameraIndex];
}

unsigned int RenderSystem::lightCascades(const CameraShadows& shadows, int lightIndex, const LightComponent& light) const{
    if (shadows.ownLightCameras && light.type == LightType::DIRECTIONAL && light.shadowMapIndex >= 0 && light.shadowMapIndex < MAX_SHADOW_ATLAS_SLOTS)
        return shadows.lightCameras[light.shadowMapIndex].numCascades;
    return light.numShadowCascades;
}

const InstanceViewRange& RenderSystem::instanceRange(Entity entity, const InstancedMeshComponent& instmesh, int instanceView) const{
    // a shadow slot of a camera culling its own casters
    if (instanceView >= 1 && ps().ownShadowInstances){
        auto it = ps().instanceRanges.find(entity);
        if (it != ps().instanceRanges.end())
            return it->second.slots[instanceView - 1];
        static const InstanceViewRange emptyRange;
        return emptyRange;
    }
#ifdef JASPERENGINE_EDITOR
    if (rv().secondary && !rv().sharesPrimaryCamera){
        auto it = rv().instanceRanges.find(entity);
        if (it != rv().instanceRanges.end())
            return it->second[instanceView];
        static const InstanceViewRange emptyRange;
        return emptyRange;
    }
#endif
    return instmesh.views[instanceView];
}

BufferRender* RenderSystem::instanceBuffer(int instanceView){
    if (instanceView >= 1 && ps().ownShadowInstances)
        return ps().instanceViews[instanceView - 1].buffer.getRender();
#ifdef JASPERENGINE_EDITOR
    if (rv().secondary && rv().sharesPrimaryCamera)
        return primaryView.instanceViews[instanceView].buffer.getRender();
#endif
    return rv().instanceViews[instanceView].buffer.getRender();
}

Vector3 RenderSystem::instanceFadeEye(Entity entity, const InstancedMeshComponent& instmesh, int instanceView) const{
#ifdef JASPERENGINE_EDITOR
    // a camera culling its own casters culls them from the frame's lists
    const bool ownShadowSlot = instanceView >= 1 && ps().ownShadowInstances;
    if (rv().secondary && !rv().sharesPrimaryCamera && instanceView >= 0 && !ownShadowSlot){
        auto it = rv().instanceSources.find(entity);
        if (it != rv().instanceSources.end())
            return it->second.fadeEyeLocal;
    }
#endif
    return instmesh.fadeEyeLocal;
}

bool RenderSystem::isBatchVisible(Entity entity, const MeshComponent& mesh, const Transform& transform, const InstancedMeshComponent* instmesh, int instanceView, const float cameraFar, const Plane frustumPlanes[6]){
    if (!cullsBatch(mesh, instmesh))
        return true;
#ifdef JASPERENGINE_EDITOR
    if (passObjects){
        // a billboard turned to this pass's camera left its frame bounds behind
        if (isObjectOverridden(transform))
            return true;
        // instances rebuilt for this view have their own bounds (not the shadow slots of a
        // camera culling its own casters, which come from the frame's lists)
        if (instmesh && instanceView >= 0 && rv().secondary && !rv().sharesPrimaryCamera &&
                !(instanceView >= 1 && ps().ownShadowInstances)){
            auto it = rv().instanceSources.find(entity);
            if (it != rv().instanceSources.end() && it->second.own)
                return isInsideCamera(cameraFar, frustumPlanes, it->second.worldAABB);
        }
    }
#endif
    return isInsideCamera(cameraFar, frustumPlanes, mesh.worldAABB);
}

TerrainView& RenderSystem::terrainViewData(Entity entity, TerrainComponent& terrain, int terrainView){
#ifdef JASPERENGINE_EDITOR
    if (terrainView == TERRAIN_PASS_VIEW && passObjects){
        auto it = passObjects->terrainViews.find(entity);
        if (it != passObjects->terrainViews.end())
            return it->second;
        return terrain.views[0];
    }
#endif
    return terrain.views[terrainView];
}

int RenderSystem::mainTerrainView() const{
    // the shadow (and color) passes of a camera with its own lighting draw its own cut
    if (passShadows)
        return passShadows->terrainView;
#ifdef JASPERENGINE_EDITOR
    if (passObjects && !rv().sharesPrimaryCamera)
        return TERRAIN_PASS_VIEW;
#endif
    return 0;
}

const Matrix4* RenderSystem::objectMatrices(Transform& transform, Matrix4 (&scratch)[3]){
#ifdef JASPERENGINE_EDITOR
    size_t index;
    if (passObjects && passObjectIndex(transform, index)){
        int overrideIndex = passObjects->override[index];
        scratch[0] = (overrideIndex >= 0) ? passObjects->overrideModel[overrideIndex] : transform.modelMatrix;
        scratch[1] = (overrideIndex >= 0) ? passObjects->overrideNormal[overrideIndex] : transform.normalMatrix;
        scratch[2] = passObjects->mvp[index];
        return scratch;
    }
#endif
    (void)scratch;
    return &transform.modelMatrix;
}

const Matrix4& RenderSystem::objectMVP(Transform& transform) const{
#ifdef JASPERENGINE_EDITOR
    size_t index;
    if (passObjects && passObjectIndex(transform, index))
        return passObjects->mvp[index];
#endif
    return transform.modelViewProjectionMatrix;
}

const Matrix4& RenderSystem::objectModel(Transform& transform) const{
#ifdef JASPERENGINE_EDITOR
    size_t index;
    if (passObjects && passObjectIndex(transform, index)){
        int overrideIndex = passObjects->override[index];
        if (overrideIndex >= 0)
            return passObjects->overrideModel[overrideIndex];
    }
#endif
    return transform.modelMatrix;
}

float RenderSystem::objectDistance(Transform& transform) const{
#ifdef JASPERENGINE_EDITOR
    size_t index;
    if (passObjects && passObjectIndex(transform, index))
        return passObjects->distance[index];
#endif
    return transform.distanceToCamera;
}

RenderSystem::PassScissor RenderSystem::passScissor(Transform& transform, RectTransformComponent& layout){
#ifdef JASPERENGINE_EDITOR
    size_t index;
    if (passObjects && passObjectIndex(transform, index)){
        PassScissorState& state = passObjects->scissors[index];
        return {&state.rect, &state.active};
    }
#endif
    (void)transform;
    return {&layout.scissor, &layout.scissorActive};
}

const Matrix4& RenderSystem::passSkyViewProjection(const SkyboxComponent& sky) const{
#ifdef JASPERENGINE_EDITOR
    if (passObjects)
        return passObjects->skyViewProjection;
#endif
    return sky.skyViewProjectionMatrix;
}

// the frame's atlases are the scene camera's; any other camera draws into its own
FramebufferRender& RenderSystem::shadowAtlasTarget(){
    CameraShadows& shadows = ps();
    return (&shadows == &primaryView.shadows) ? shadowAtlasFramebuffer : shadows.ownProjective;
}

FramebufferRender& RenderSystem::shadowPointAtlasTarget(){
    CameraShadows& shadows = ps();
    return (&shadows == &primaryView.shadows) ? shadowPointAtlasFramebuffer : shadows.ownPoint;
}

bool RenderSystem::isRenderingFlipped(const CameraComponent& camera) const{
    // Cubemap sampling follows one fixed convention on every API, and with the
    // classic capture table (renderReflectionProbeCapture) it expects GL-style
    // bottom-up face storage: flip only on the backends that natively rasterize
    // top-left. This is the inverse of the 2D RTT rule below, and its winding
    // pairs with PIP_RTT_INVERT (one reversal off PIP_RTT on every backend).
    if (capturingReflectionProbe)
        return !Engine::isOpenGL();
    // OpenGL offscreen targets are bottom-up; rendering them with a Y-flipped
    // projection makes framebuffer textures top-left origin on every backend.
    // Must match the PIP_RTT pipeline selection (its winding is reversed on GL).
    return (camera.renderToTexture || targetFramebuffer() || isFixedResolutionActive() || swapchainRedirect) && Engine::isOpenGL();
}

bool RenderSystem::isFixedResolutionActive() const{
    if (!scene->isFixedResolutionEnabled() || scene->getFixedResolutionWidth() == 0 || scene->getFixedResolutionHeight() == 0)
        return false;
#ifdef JASPERENGINE_EDITOR
    // a secondary view asks for it (a preview of the game), the editor's own views never do
    if (rv().secondary)
        return rv().fixedResolution && rv().baseLayer;
#endif
    // main scene only: layer scenes keep rendering at native resolution on top
    return !disableFixedResolution && Engine::getMainScene() == scene;
}

void RenderSystem::updateSwapchainRedirect(){
    // SSR and the post-process chain need the scene in an offscreen color buffer. Without
    // a framebuffer destination (exported builds) the color pass is redirected into it and
    // the last pass targets the swapchain. Main scene only: a layer scene composites over
    // the scene below, and its own pass would clear it.
    bool redirect = false;

    if (!Engine::getFramebuffer() && !isFixedResolutionActive()
            && Engine::getMainScene() == scene && Engine::isViewLoaded()){
        unsigned int w = (unsigned int)Engine::getViewRect().getWidth();
        unsigned int h = (unsigned int)Engine::getViewRect().getHeight();

        // the meshes render with PIP_RTT (flipped on GL) while it is on, so the
        // redirect only starts once everything it needs exists
        if (scene->isSSREnabled()){
            loadSSR();
            redirect = ssrLoaded && ensureSSRFramebuffers(w, h) && ensureGBufferFramebuffer(w, h);
        }
        if (!redirect && !scene->getPostProcessPasses().empty()){
            loadPostProcess();
            loadBlit();
            redirect = postProcessLoaded && blitLoaded && !postProcessPasses.empty()
                    && ensurePostProcessFramebuffers(w, h);
        }
        // the uber pass (phase 4) reads the scene from an offscreen target too
        if (!redirect && postTiers.uber){
            redirect = loadUber(postTiers.uberFeatures);
        }
        // so does MSAA: the multisampled scene resolves offscreen, then is blitted
        msaaRedirect = false;
        if (!redirect && msaaPipelineSamples > 1){
            loadBlit();
            redirect = msaaRedirect = blitLoaded;
        }
        // the scene colour (phase 3) is copied out of an offscreen colour target: in a scene
        // with an object reading it (or one gone a short while ago), the camera may draw into
        // one blitted to the screen, decided each frame by what it sees (updateSceneColorRedirect)
        sceneColorRedirectAllowed = !redirect && sceneColorReaders;
    }else{
        sceneColorRedirectAllowed = false;
        msaaRedirect = false;
    }
    if (!msaaRedirect && rv().msaaResolveFramebuffer.isCreated())
        rv().msaaResolveFramebuffer.destroy();
    if (!sceneColorRedirectAllowed){
        sceneColorRedirect = false;
        sceneColorRedirectHold = 0;
        if (rv().sceneColorRedirect.isCreated())
            rv().sceneColorRedirect.destroy();
    }
    redirect = redirect || sceneColorRedirect;

    if (redirect != swapchainRedirect){
        swapchainRedirect = redirect;
        // the flip is baked in the MVP matrices
        scene->getComponent<CameraComponent>(scene->getCamera()).needUpdate = true;
    }
}

void RenderSystem::updateSceneColorRedirect(CameraComponent& mainCamera){
    if (!sceneColorRedirectAllowed)
        return;

    // a reader of the scene colour the camera may see this frame: in its view, not loaded yet
    // (registered before it loads: predictSceneInputUsers), or points (no bounds)
    bool seen = false;
    for (const auto& user : sceneInputMeshes){
        if (!(user.second & ShaderRequire_SceneColor) || !scene->isEntityCreated(user.first))
            continue;
        const Entity entity = user.first;
        MeshComponent* mesh = scene->findComponent<MeshComponent>(entity);
        Transform* transform = scene->findComponent<Transform>(entity);
        if (!mesh || !transform || !transform->visible || !isRenderActive(entity) || !isLayerVisible(entity, mainCamera.cullingMask))
            continue;
        // loading this update or the last (the loaded flag comes with the render queue): its
        // bounds are not known yet, and its first draw reads a copy too
        if (!mesh->loaded){
            seen = true;
            break;
        }
        InstancedMeshComponent* instmesh = scene->findComponent<InstancedMeshComponent>(entity);
        if (transform->needUpdate || mesh->needUpdateAABB){
            // moved (a billboard turns every frame): its bounds are made again later in the
            // update, from the model matrix updateTransform made already
            if (!cullsBatch(*mesh, instmesh)){
                seen = true;
            }else{
                const AABB& local = mesh->skinnedAABB.isNull() ? mesh->aabb : mesh->skinnedAABB;
                seen = isInsideCamera(mainCamera.farClip, mainCamera.frustumPlanes, transform->modelMatrix * local);
            }
        }else{
            seen = isBatchVisible(entity, *mesh, *transform, instmesh, 0, mainCamera.farClip, mainCamera.frustumPlanes);
        }
        if (seen)
            break;
    }
    for (const auto& user : sceneInputPoints){
        if (seen)
            break;
        if (!(user.second & ShaderRequire_SceneColor) || !scene->isEntityCreated(user.first))
            continue;
        PointsComponent* points = scene->findComponent<PointsComponent>(user.first);
        Transform* transform = scene->findComponent<Transform>(user.first);
        seen = points && transform && transform->visible && isRenderActive(user.first) && isLayerVisible(user.first, mainCamera.cullingMask);
    }

    // kept some frames after the last one seen: an object at the edge of the view does not
    // switch it every frame (each switch remakes the matrices with the other Y flip)
    static const int HOLD_FRAMES = 30;
    if (seen){
        sceneColorRedirectHold = HOLD_FRAMES;
    }else if (sceneColorRedirectHold > 0){
        sceneColorRedirectHold--;
    }
    bool redirect = sceneColorRedirectHold > 0;
    if (redirect){
        loadBlit();
        redirect = blitLoaded && ensureSceneColorRedirect((unsigned int)Engine::getViewRect().getWidth(), (unsigned int)Engine::getViewRect().getHeight());
        sceneColorRedirectIdle = 0;
    }else if (rv().sceneColorRedirect.isCreated() && ++sceneColorRedirectIdle > 180){
        // unused for a while: its memory goes (made again when a reader comes into view)
        rv().sceneColorRedirect.destroy();
    }
    if (redirect != sceneColorRedirect){
        sceneColorRedirect = redirect;
        swapchainRedirect = redirect;
        // the flip is baked in the MVP matrices (made again below, the camera needing an update)
        mainCamera.needUpdate = true;
        mvpFlipped = isRenderingFlipped(mainCamera);
    }
}

void RenderSystem::updateMVP(size_t index, Transform& transform, CameraComponent& camera, Transform& cameraTransform){
    if (transform.billboard && !transform.fakeBillboard){

        Vector3 camPos = cameraTransform.worldPosition;

        if (transform.cylindricalBillboard)
            camPos.y = transform.worldPosition.y;

        Vector3 toCamera = camPos - transform.worldPosition;
        const float EPSILON = 1e-6f;

        if (toCamera.length() > EPSILON && toCamera.crossProduct(camera.worldUp).length() > EPSILON){ // check if not parallel
            Matrix4 m1 = Matrix4::lookAtMatrix(camPos, transform.worldPosition, camera.worldUp).inverse();

            Quaternion oldRotation = transform.rotation;

            if (m1.isValid()){
                transform.rotation.fromRotationMatrix(m1);
                transform.rotation = transform.rotation * transform.billboardRotation;
                if (transform.parent != NULL_ENTITY){
                    Transform* transformParent = scene->findComponent<Transform>(transform.parent);
                    if (transformParent){
                        transform.rotation = transformParent->worldRotation.inverse() * transform.rotation;
                    }
                }
            }

            if (transform.rotation != oldRotation){
                transform.needUpdate = true;

                std::vector<Entity> parentList;
                auto transforms = scene->getComponentArray<Transform>();
                for (int i = index; i < transforms->size(); i++){
                    Transform& childTransform = transforms->getComponentFromIndex(i);

                    // Finding childs
                    if (i > index){
                        if (std::find(parentList.begin(), parentList.end(), childTransform.parent) != parentList.end()){
                            childTransform.needUpdate = true;
                        }else{
                            break;
                        }
                    }

                    if (childTransform.needUpdate){
                        Entity entity = transforms->getEntity(i);
                        parentList.push_back(entity);
                        updateTransform(childTransform, entity);
                    }
                }
            }
        }

    }

    if (transform.billboard && transform.fakeBillboard){
        
        Matrix4 modelViewMatrix = camera.viewMatrix * transform.modelMatrix;

        modelViewMatrix.set(0, 0, transform.worldScale.x);
        modelViewMatrix.set(0, 1, 0.0);
        modelViewMatrix.set(0, 2, 0.0);

        if (!transform.cylindricalBillboard) {
            modelViewMatrix.set(1, 0, 0.0);
            modelViewMatrix.set(1, 1, transform.worldScale.y);
            modelViewMatrix.set(1, 2, 0.0);
        }

        modelViewMatrix.set(2, 0, 0.0);
        modelViewMatrix.set(2, 1, 0.0);
        modelViewMatrix.set(2, 2, transform.worldScale.z);

        transform.modelViewProjectionMatrix = camera.projectionMatrix * modelViewMatrix;

    }else{

        transform.modelViewProjectionMatrix = camera.viewProjectionMatrix * transform.modelMatrix;

    }

    if (isRenderingFlipped(camera)){
        // flip only the render matrix; camera.viewProjectionMatrix stays logical
        // (top-left screen origin) for frustum planes, rays and UI hit tests
        static const Matrix4 flipYMatrix = Matrix4::scaleMatrix(Vector3(1, -1, 1));
        transform.modelViewProjectionMatrix = flipYMatrix * transform.modelViewProjectionMatrix;
    }

    transform.distanceToCamera = (cameraTransform.worldPosition - transform.worldPosition).length();
}

uint16_t RenderSystem::getScenePipelines() const{
    uint16_t pipelines = 0;

    Entity mainCameraEntity = scene->getCamera();
    auto cameras = scene->getComponentArray<CameraComponent>();

    for (int i = 0; i < cameras->size(); i++){
        CameraComponent& camera = cameras->getComponentFromIndex(i);
        Entity cameraEntity = cameras->getEntity(i);

        // no Transform means no pass
        if (!scene->findComponent<Transform>(cameraEntity))
            continue;

        if (cameraEntity == mainCameraEntity && !camera.renderToTexture){
            pipelines |= camera.depthTest ? PIP_DEFAULT : PIP_DEFAULT_NODEPTH;
        }

        // (a scene-colour reader: the scene camera switches between the swapchain and an
        // offscreen target without reloading)
        if (camera.renderToTexture || Engine::getFramebuffer() || isFixedResolutionActive() || swapchainRedirect ||
                (sceneColorRedirectAllowed && cameraEntity == mainCameraEntity)){
            pipelines |= camera.depthTest ? PIP_RTT : PIP_RTT_NODEPTH;
        }

        // the post-processed camera's multisampled scene target (phase 4 MSAA; not a mirror's
        // inverted winding)
        if (cameraEntity == mainCameraEntity && msaaPipelineSamples > 1 && !camera.invertCulling){
            pipelines |= camera.depthTest ? PIP_MSAA : PIP_MSAA_NODEPTH;
        }
    }

    // mirrors and probe capture render with reversed winding (see isRenderingFlipped)
    if (scene->getComponentArray<MirrorComponent>()->size() > 0 ||
            scene->getComponentArray<ReflectionProbeComponent>()->size() > 0){
        pipelines |= PIP_RTT_INVERT;
    }

#ifdef JASPERENGINE_EDITOR
    pipelines |= viewPipelines;
#endif

    return pipelines;
}

void RenderSystem::update(double dt){
#ifdef JASPERENGINE_EDITOR
    frameDrawn = false;
#endif
    // shadow maps drawn before this update hold another scene
    shadowEpoch++;

    if (paused) {
        return;
    }

    int numLights = checkLightsAndShadow();

    // changed post-processing settings: LUT bakes and the variant bit the objects load with below
    applyPostProcessing();

    // the points, lines and sky of a colour bit switch reload once their new variants are built
    if (postColorReloadPending && postColorVariantsReady()){
        postColorReloadPending = false;
        needReloadPoints();
        needReloadLines();
        needReloadSky();
    }

    // MSAA (phase 4): the sample count the objects' PIP_MSAA pipelines take, as the device has
    // it; another count than they were made with reloads them (getScenePipelines adds the type)
    {
        const int msaa = (postTiers.msaaSamples > 1 && Engine::isViewLoaded()) ? FramebufferRender::getMaxMsaaSamples(postTiers.msaaSamples) : 1;
        if (msaa != msaaPipelineSamples){
            const bool countChanged = msaa > 1 && msaaPipelineSamples > 1;
            msaaPipelineSamples = msaa;
            if (countChanged){
                needReloadMeshes();
                needReloadUIs();
                needReloadPoints();
                needReloadLines();
                needReloadSky();
            }
        }
    }

    auto transforms = scene->getComponentArray<Transform>();
    auto cameras = scene->getComponentArray<CameraComponent>();

    for (int i = 0; i < transforms->size(); i++){
        Transform& transform = transforms->getComponentFromIndex(i);

        if (transform.parent != NULL_ENTITY){
            Transform* transformParent = scene->findComponent<Transform>(transform.parent);
            if (transformParent){
                if (transformParent->needUpdate){
                    transform.needUpdate = true;
                }

                if (transformParent->needUpdateChildVisibility){
                    transform.visible = transformParent->visible;
                    transform.needUpdateChildVisibility = true;
                }
            }
        }

        if (transform.needUpdate){
            updateTransform(transform, transforms->getEntity(i));
        }
    }

    auto models = scene->getComponentArray<ModelComponent>();
    for (int i = 0; i < models->size(); i++) {
        ModelComponent& model = models->getComponentFromIndex(i);
        for (const ModelSkinBinding& binding : model.skinBindings) {
            MeshComponent* mesh = scene->findComponent<MeshComponent>(binding.mesh);
            Transform* meshTransform = scene->findComponent<Transform>(binding.mesh);
            if (!mesh || !meshTransform)
                continue;

            bool dirty = meshTransform->needUpdate;
            for (Entity joint : binding.joints) {
                if (dirty)
                    break;
                Transform* jointTransform = scene->findComponent<Transform>(joint);
                dirty = jointTransform && jointTransform->needUpdate;
            }
            if (!dirty)
                continue;

            const Matrix4 inverseMeshTransform = meshTransform->modelMatrix.inverse();
            // Scale 0 makes this NaN; posing the skinned AABB with it trips setExtents.
            if (!inverseMeshTransform.isValid())
                continue;

            const size_t jointCount = std::min(binding.joints.size(), binding.inverseBindMatrices.size());
            for (size_t jointIndex = 0; jointIndex < jointCount &&
                    mesh->bonesMatrix.validIndex(static_cast<int>(jointIndex)); jointIndex++) {
                Transform* jointTransform = scene->findComponent<Transform>(binding.joints[jointIndex]);
                if (jointTransform)
                    mesh->bonesMatrix[jointIndex] = inverseMeshTransform * jointTransform->modelMatrix *
                        binding.inverseBindMatrices[jointIndex];
            }
            mesh->needUpdateBones = true;
            mesh->needUpdateAABB = true;
        }
    }

    updateReflectionProbes(dt);

    // the readers loading this frame count from now: what every mesh loads with below (a depth
    // program for the scene depth) and the scene camera's pipelines (offscreen for a colour
    // reader) are decided from them
    predictSceneInputUsers();
    updateSceneInputPrograms();

    // the pipeline mask below depends on the redirect
    updateSwapchainRedirect();

    Entity mainCameraEntity = scene->getCamera();

    // The Y flip of an offscreen target is baked in the MVP matrices (isRenderingFlipped),
    // which are only made again when the camera or the object moves. A scene loaded with
    // no target set (the editor adds a play's child scenes before the Game view sets its
    // framebuffer) would keep a static UI unflipped, so upside down, in the framebuffer.
    if (CameraComponent* mainCameraComponent = scene->findComponent<CameraComponent>(mainCameraEntity)){
        const bool flipped = isRenderingFlipped(*mainCameraComponent);
        if (flipped != mvpFlipped){
            mvpFlipped = flipped;
            mainCameraComponent->needUpdate = true;
        }
    }
    uint16_t pipelines = getScenePipelines();

    hasMultipleCameras = false;
    for (int i = 0; i < cameras->size(); i++){
        CameraComponent& camera = cameras->getComponentFromIndex(i);
        Entity cameraEntity = cameras->getEntity(i);
        Transform* cameraTransformPtr = scene->findComponent<Transform>(cameraEntity);
        if (!cameraTransformPtr)
            continue;
        Transform& cameraTransform = *cameraTransformPtr;

        if (camera.renderToTexture){
            // sync authored framebuffer settings (editor/serialized) into the live
            // framebuffer; recreate it when they change so edits take effect. A
            // non-positive size is ignored so an in-use framebuffer is never left
            // destroyed (meshes sampling an uncreated framebuffer would be invalid).
            bool validSize = camera.framebufferWidth > 0 && camera.framebufferHeight > 0;

            bool settingsChanged = validSize &&
                (camera.framebuffer->getWidth() != camera.framebufferWidth ||
                 camera.framebuffer->getHeight() != camera.framebufferHeight ||
                 camera.framebuffer->getMinFilter() != camera.framebufferFilter ||
                 camera.framebuffer->getMagFilter() != camera.framebufferFilter);

            if (settingsChanged){
                camera.framebuffer->setWidth(camera.framebufferWidth);
                camera.framebuffer->setHeight(camera.framebufferHeight);
                camera.framebuffer->setMinFilter(camera.framebufferFilter);
                camera.framebuffer->setMagFilter(camera.framebufferFilter);

                if (camera.framebuffer->isCreated()){
                    camera.framebuffer->destroy();
                }
            }

            bool created = false;
            if (!camera.framebuffer->isCreated() && validSize){
                camera.framebuffer->create();
                created = true;
            }

            // autoResize cameras derive clip/aspect from the framebuffer size;
            // reconcile on first creation and after any size change, otherwise the
            // camera keeps a stale (e.g. canvas) aspect and the RTT image is distorted
            if (settingsChanged || created){
                updateCameraSize(cameraEntity);
            }
        }

        if (camera.renderToTexture && cameraEntity != mainCameraEntity){
            hasMultipleCameras = true;
        }

        if (cameraTransform.needUpdate){
            camera.needUpdate = true;
        }
        
        if (camera.needUpdate){
            updateCamera(camera, cameraTransform);
        }
    }

    // drive mirror reflection cameras from the (now updated) main camera, so the
    // reflection passes render with current matrices in the draw step. This may
    // create reflection camera entities (reallocating the camera/transform arrays),
    // so fetch mainCamera/mainCameraTransform references AFTER it returns.
    updateMirrors(mainCameraEntity);

    // the rest of the frame is built from the main camera view
    CameraComponent* mainCameraPtr = scene->findComponent<CameraComponent>(mainCameraEntity);
    Transform* mainCameraTransformPtr = scene->findComponent<Transform>(mainCameraEntity);
    if (!mainCameraPtr || !mainCameraTransformPtr)
        return;

    CameraComponent& mainCamera = *mainCameraPtr;
    Transform& mainCameraTransform = *mainCameraTransformPtr;
    fadeEyePosition = mainCameraTransform.worldPosition;

    // with its view up to date, whether it draws offscreen for a scene-colour reader
    updateSceneColorRedirect(mainCamera);

    updateMainLodView(mainCamera, mainCameraTransform);
    if (mainCamera.needUpdate){
        instanceViewsDirty = true;
    }

    // while extra cameras render, draw() rewrites the shared MVP and sky matrices
    // per camera; removing the last of them (a mirror, a reflection probe) would
    // otherwise leave the scene on that camera's view until something turns dirty
    bool multiCameraDraw = hasMultipleCameras || hasReflectionProbes;
    if (lastMultiCameraDraw && !multiCameraDraw){
        mainCamera.needUpdate = true;
    }
    lastMultiCameraDraw = multiCameraDraw;

    // the batch order is baked into the shared render arrays, so a mode change has to
    // rebuild them from the authored instances and points
    bool batchSort = sortsByDistance(mainCamera);
    bool batchSortChanged = batchSort != lastBatchSort;
    lastBatchSort = batchSort;

    // levels exist only while the scene switch is on, so a change builds or frees them
    bool meshLodToggled = scene->isMeshLodEnabled() != lastMeshLodEnabled;
    lastMeshLodEnabled = scene->isMeshLodEnabled();

    // the destination is baked into the pipelines at load, so a scene entering or
    // leaving a stack (or fixed resolution switching) has to reload with the new set
    // the per-object lights blocks' capacity is part of every lit mesh variant
    const int lightCapacity = getLightCapacity(scene->getMaxLightsPerObject());
    if (lightCapacity != lightCapacityTier){
        lightCapacityTier = lightCapacity;
        needReloadMeshVariants();
    }

    if (pipelines != loadedPipelines){
        loadedPipelines = pipelines;

        needReloadMeshes();
        needReloadUIs();
        needReloadPoints();
        needReloadLines();
        needReloadSky();
    }

    loadLights(numLights);
    updateSpotMaskAtlas(numLights);
    loadLights2D();
    loadAndProcessFog();
    // the atmosphere (height fog, volumetric fog and clouds): a height fog is a variant of the
    // fog variants, and its volumetric passes need depth programs on every opaque mesh
    atmosphere.update(dt);
    {
        const bool heightFog = hasFog && atmosphere.hasHeightFog();
        const bool depthPrograms = atmosphere.needsDepthPrograms();
        if (heightFog != heightFogVariants || depthPrograms != atmosphereDepthPrograms){
            heightFogVariants = heightFog;
            atmosphereDepthPrograms = depthPrograms;
            needReloadMeshVariants();
        }
    }

    // the scene's sky is its first SkyboxComponent (SkyboxComponent.h); the others are not loaded
    auto skys = scene->getComponentArray<SkyboxComponent>();
    bool newHasIBL = false;
    const Entity firstSky = skys->size() > 0 ? skys->getEntity(0) : NULL_ENTITY;
    if (firstSky != activeSky){
        // Another sky leads now (the first was removed, or the editor reordered them: its
        // hierarchy order is the array's, ProjectUtils): the one it replaces unloads as a
        // sky that is not first (its maps go), and the meshes rebind the image-based lighting
        if (activeSky != NULL_ENTITY && scene->isEntityCreated(activeSky)){
            if (SkyboxComponent* previous = scene->findComponent<SkyboxComponent>(activeSky)){
                if (previous->loaded){
                    previous->needReload = true;
                    releaseSkyEnvironment(*previous);
                    previous->needUpdateEnvironment = true;
                    destroySky(activeSky, *previous);
                }
            }
        }
        if (activeSky != NULL_ENTITY && hasIBL){
            needReloadMeshes();
        }
        activeSky = firstSky;
    }
    if (skys->size() > 0){
        SkyboxComponent& sky = skys->getComponentFromIndex(0);
        Entity entity = skys->getEntity(0);
        if (mainCamera.needUpdate || sky.needUpdateSky){
            if (!hasMultipleCameras){
                updateSkyViewProjection(sky, mainCamera);
            }
        }

        if (sky.needUpdateTexture){
            if (sky.texture.empty()){
                sky.needReload = true;
            }
            // texture changed: IBL environment maps must be regenerated (or destroyed)
            sky.needUpdateEnvironment = true;
        }

        // generate IBL maps before loadSky uploads (and releases) the texture pixels
        if (Engine::isViewLoaded()){
            updateSkyEnvironment(sky);
        }

        if (sky.loaded && sky.needReload){
            destroySky(entity, sky);
        }
        if (!sky.loadCalled){
            loadSky(entity, sky, pipelines);
        }

        newHasIBL = sky.envMapsLoaded;
    }

    if (hasIBL != newHasIBL){
        hasIBL = newHasIBL;
        needReloadMeshes();
    }

    for (int i = 0; i < transforms->size(); i++){
        Transform& transform = transforms->getComponentFromIndex(i);

        Entity entity = transforms->getEntity(i);
        Signature signature = scene->getSignature(entity);

        if (signature.test(scene->getComponentId<MeshComponent>())){
            MeshComponent& mesh = scene->getComponent<MeshComponent>(entity);

            TerrainComponent* terrain = scene->findComponent<TerrainComponent>(entity);

            InstancedMeshComponent* instmesh = scene->findComponent<InstancedMeshComponent>(entity);
            if (instmesh){
                if (instmesh->distanceFade){
                    // The range is a model-space distance, so the eye moves into that space
                    // instead: scaling the range into world space breaks on a scaled mesh.
                    const Matrix4 inverseModel = transform.modelMatrix.inverse();
                    instmesh->fadeEyeLocal = inverseModel.isValid() ?
                        (inverseModel * fadeEyePosition) : fadeEyePosition;
                }

                bool sortTransparentInstances = mesh.transparent && batchSort;

                const float cullEnd = instanceFadeCullEnd(*instmesh, mesh, *scene);
                const bool fadeSelectionChanged = cullEnd != instmesh->lastFadeCullEnd ||
                    (cullEnd >= 0.0f && (instmesh->fadeEyeLocal.x != instmesh->lastFadeCullEye.x ||
                                        instmesh->fadeEyeLocal.z != instmesh->lastFadeCullEye.z));
                bool instancesNeedUpdate = instmesh->needUpdateInstances || mesh.needUpdateAABB ||
                    batchSortChanged || fadeSelectionChanged;

                if (instancesNeedUpdate && !instmesh->instancedBillboard){
                    updateInstancedMesh(*instmesh, mesh, transform, mainCamera, mainCameraTransform);
                }

                if (instancesNeedUpdate || ((mainCamera.needUpdate || transform.needUpdate) && sortTransparentInstances)){
                    // The instance buffer is shared by every camera and sokol allows one
                    // sg_update_buffer per frame (VALIDATE_UPDATEBUF_ONCE), so the main camera
                    // fixes the batch order and every other camera reuses it.
                    if (instmesh->instancedBillboard){
                        updateInstancedMesh(*instmesh, mesh, transform, mainCamera, mainCameraTransform);
                    }
                    if (sortTransparentInstances){
                        sortInstancedMesh(*instmesh, mesh, transform, mainCamera, mainCameraTransform);
                    }
                }

                instmesh->needUpdateInstances = false;

                InstanceViewSettings viewSettings = {instmesh->cullInstances, mesh.castShadows, mesh.lodEnabled, mesh.lodBias, instmesh->cullDistance};
                if (transform.needUpdate || viewSettings != instmesh->viewSettings){
                    instmesh->viewSettings = viewSettings;
                    instanceViewsDirty = true;
                }
            }

            if (meshLodToggled && mesh.loadCalled && !mesh.needReload){
                updateMeshLods(entity, mesh, true);
            }else if (mesh.loaded && !mesh.needReload){
                for (unsigned int s = 0; s < mesh.numSubmeshes; s++){
                    if (isMeshLodPending(mesh.submeshes[s])){
                        updateMeshLods(entity, mesh, false);
                        break;
                    }
                }
            }

            if (terrain && mesh.numSubmeshes > 1){
                for (unsigned int s = 1; s < mesh.numSubmeshes; s++){
                    const bool materialOutOfSync = mesh.submeshes[s].material != mesh.submeshes[0].material;
                    if (materialOutOfSync){
                        mesh.submeshes[s].material = mesh.submeshes[0].material;
                    }
                    if (mesh.submeshes[0].needUpdateTexture || materialOutOfSync){
                        mesh.submeshes[s].needUpdateTexture = true;
                    }
                }
            }

            bool expectedTransparent = false;
            for (int s = 0; s < mesh.numSubmeshes; s++){
                const MaterialAlphaMode alphaMode = mesh.submeshes[s].material.alphaMode;
                // the queue the render state resolves to (legacy: alpha blended materials)
                bool blended = false;
                bool alphaBlend = false;
                // the manifest's state as loaded (keyword rules resolved at load, no allocation)
                resolveSubmeshRenderState(mesh, mesh.submeshes[s], mesh.submeshes[s].manifest ? &mesh.submeshes[s].manifestState : nullptr,
                                          blended, alphaBlend);
                expectedTransparent = expectedTransparent || alphaBlend;

                if (mesh.submeshes[s].alphaBlend != alphaBlend){
                    mesh.needReload = true;
                }

                // Explicit alpha modes own the derived shadow-cutout state. AUTO
                // remains the compatibility path for setCastShadowsWithTexture().
                if (alphaMode != MaterialAlphaMode::AUTO){
                    const bool expectedTextureShadow = alphaMode == MaterialAlphaMode::MASK;
                    if (mesh.submeshes[s].textureShadow != expectedTextureShadow){
                        mesh.submeshes[s].textureShadow = expectedTextureShadow;
                        mesh.needReload = true;
                    }
                }

                // Alpha mode can be changed directly by scripts/generated code, so
                // shader variant reconciliation must not depend on a texture update.
                const bool shaderAlphaMask = (mesh.submeshes[s].shaderProperties & (1u << 24)) != 0;
                const bool shaderAlphaOpaque = (mesh.submeshes[s].shaderProperties & (1u << 25)) != 0;
                if (shaderAlphaMask != (alphaMode == MaterialAlphaMode::MASK) ||
                    shaderAlphaOpaque != (alphaMode == MaterialAlphaMode::ALPHA_OPAQUE)) {
                    mesh.needReload = true;
                }

                // so can the lighting mode, which picks the stages evaluating the lights
                if (mesh.loaded && mesh.submeshes[s].loadedLighting != mesh.submeshes[s].material.lighting){
                    mesh.needReload = true;
                }

                // A layer gains or loses its own surface from the editor, a script or an AI
                // action, and only a reload can swap the terrain variant that reads it
                if (terrain && !mesh.submeshes[s].manifest){
                    const bool shaderTerrainSurface = (mesh.submeshes[s].shaderProperties & (1u << 27)) != 0;
                    if (shaderTerrainSurface != hasTerrainSurfaceLayers(*terrain)){
                        mesh.needReload = true;
                    }
                }

                if (mesh.submeshes[s].needUpdateTexture){
                    if (terrain){
                        transform.needUpdate = true;
                    }
                    if (mesh.submeshes[s].textureShadow){
                        mesh.submeshes[s].needUpdateDepthTexture = true;
                    }
                    if (mesh.submeshes[s].gbufferShader){
                        mesh.submeshes[s].needUpdateGBufferTexture = true;
                    }
                    // a shader with a manifest takes its UV sets from @require, not the textures
                    if (mesh.submeshes[s].manifest){
                        // its custom textures are bound again by drawMesh
                    }else if (checkPBRTextures(mesh.submeshes[s].material, mesh.receiveLights)){
                        if (!(mesh.submeshes[s].shaderProperties & (1 << 1)) && !(mesh.submeshes[s].shaderProperties & (1 << 2))){ // not 'Uv1' and not 'Uv2'
                            mesh.needReload = true;
                        }
                    }else{
                        if ((mesh.submeshes[s].shaderProperties & (1 << 1)) || (mesh.submeshes[s].shaderProperties & (1 << 2))){ // 'Uv1' or 'Uv2'
                            mesh.needReload = true;
                        }
                    }
                    if (mesh.submeshes[s].manifest){
                        // no normal map variant: its lighting comes from the manifest
                    }else if (!mesh.submeshes[s].material.normalTexture.empty()){
                        if (!(mesh.submeshes[s].shaderProperties & (1 << 7))){ // not 'Nmp'
                            mesh.needReload = true;
                        }
                    }else{
                        if (mesh.submeshes[s].shaderProperties & (1 << 7)){ // 'Nmp'
                            mesh.needReload = true;
                        }
                    }

                }
            }

            if (mesh.autoTransparency && mesh.transparent != expectedTransparent){
                mesh.needReload = true;
            }

            // a new variant for the environment: the loaded one draws until its shaders are built
            if (mesh.needVariantReload){
                if (!mesh.loaded || mesh.needReload){
                    mesh.needReload = true;
                    mesh.needVariantReload = false;
                }else if (meshVariantShadersReady(entity, mesh, instmesh, terrain)){
                    mesh.needReload = true;
                    mesh.needVariantReload = false;
                }
            }

            if (mesh.loaded && mesh.needReload){
                destroyMesh(entity, mesh);
            }
            if (!mesh.loadCalled){
                loadMesh(entity, mesh, pipelines, instmesh, terrain);
            }
            if (mesh.needUpdateAABB || transform.needUpdate){
                // Bones cancel out this mesh's model matrix, so the shader draws at
                // modelMatrix * bonesMatrix[b] * vertex and modelMatrix * aabb mis-sizes the box
                // once posed. Each bone bounds only its own share: the whole aabb per bone works
                // too, but unions body-sized boxes over the skeleton. Local space so the editor
                // draws the same box.
                mesh.skinnedAABB.setNull();
                if (!instmesh){
                    const size_t boneBounds = std::min(mesh.bonesAABB.size(), mesh.bonesMatrix.size());
                    for (size_t b = 0; b < boneBounds; b++){
                        if (!mesh.bonesAABB[b].isNull()){
                            mesh.skinnedAABB.merge(mesh.bonesMatrix[b] * mesh.bonesAABB[b]);
                        }
                    }

                    if (mesh.skinnedAABB.isNull()){ // no per-bone bounds to pose
                        const Matrix4 identityMatrix;
                        for (size_t b = 0; b < mesh.bonesMatrix.size(); b++){
                            const Matrix4& bone = mesh.bonesMatrix[b];
                            if (bone != identityMatrix){
                                mesh.skinnedAABB.merge(bone * mesh.aabb);
                            }
                        }
                    }
                }
                mesh.worldAABB = mesh.skinnedAABB.isNull() ? (transform.modelMatrix * mesh.aabb)
                                                           : (transform.modelMatrix * mesh.skinnedAABB);

                mesh.needUpdateAABB = false;

                // Tilemap chunk bounds are refreshed with the mesh AABB: rebuilding a
                // tilemap recalculates that AABB, and MeshSystem updates before this
                // system, so new chunks are always current before draw().
                if (signature.test(scene->getComponentId<TilemapComponent>())){
                    TilemapComponent& tilemap = scene->getComponent<TilemapComponent>(entity);
                    for (TilemapChunk& chunk : tilemap.chunks){
                        chunk.worldAABB = transform.modelMatrix * chunk.aabb;
                    }
                }
            }
        }else if (signature.test(scene->getComponentId<UIComponent>())){
            UIComponent& ui = scene->getComponent<UIComponent>(entity);

            if (ui.needUpdateTexture){
                if (!ui.texture.empty()){
                    if (!(ui.shaderProperties & (1 << 0))){ // not 'Tex'
                        ui.needReload = true;
                    }
                }else{
                    if (ui.shaderProperties & (1 << 0)){ // 'Tex'
                        ui.needReload = true;
                    }
                }
            }

            if (ui.loaded && ui.needReload){
                destroyUI(entity, ui);
            }
            if (!ui.loadCalled){
                bool isText = false;
                if (signature.test(scene->getComponentId<TextComponent>())){
                    isText = true;
                }
                loadUI(entity, ui, pipelines, isText);
            }
            if (ui.needUpdateAABB || transform.needUpdate){
                ui.worldAABB = transform.modelMatrix * ui.aabb;
                ui.needUpdateAABB = false;
            }
        }else if (signature.test(scene->getComponentId<PointsComponent>())){
            PointsComponent& points = scene->getComponent<PointsComponent>(entity);

            if (points.needUpdateTexture){
                if (!points.texture.empty()){
                    if (!(points.shaderProperties & (1 << 0))){ // not 'Tex'
                        points.needReload = true;
                    }
                }else{
                    if (points.shaderProperties & (1 << 0)){ // 'Tex'
                        points.needReload = true;
                    }
                }
            }

            if (points.loaded && points.needReload){
                destroyPoints(entity, points);
            }
            if (!points.loadCalled){
                loadPoints(entity, points, pipelines);
            }

            bool sortTransparentPoints = points.transparent && batchSort;

            if (batchSortChanged){
                points.needUpdate = true;
            }

            if (points.needUpdate){
                updatePoints(points, transform, mainCamera, mainCameraTransform);
            }

            // Sort/upload transparent points once here for the main camera: the buffer is
            // shared by every camera and sokol only allows one sg_update_buffer per frame
            // (VALIDATE_UPDATEBUF_ONCE), so every other camera reuses this order.
            if (sortTransparentPoints && (points.needUpdate || mainCamera.needUpdate || transform.needUpdate)){
                sortPoints(points, transform, mainCamera, mainCameraTransform);
            }

            if (points.loaded){
                points.needUpdate = false;
            }
        }else if (signature.test(scene->getComponentId<LineRendererComponent>())){
            LineRendererComponent& lines = scene->getComponent<LineRendererComponent>(entity);
            if (lines.loaded && lines.needReload){
                destroyLines(entity, lines);
            }
            if (!lines.loadCalled){
                loadLines(entity, lines, pipelines);
            }

        }else if (signature.test(scene->getComponentId<LightComponent>())){
            LightComponent& light = scene->getComponent<LightComponent>(entity);

            if (mainCamera.needUpdate || transform.needUpdate || light.needUpdateShadowCamera){
                // need to be updated ONLY for main camera
                updateLightFromScene(light, transform, mainCamera, mainCameraTransform);

                light.needUpdateShadowCamera = false;
                instanceViewsDirty = true;
            }
        }

        if (mainCamera.needUpdate || transform.needUpdate){

            // need to be updated for every camera
            if (!hasMultipleCameras){
                updateMVP(i, transform, mainCamera, mainCameraTransform);
            }

            // Select the main camera's CDLOD cut (view 0) once per frame; both the
            // shadow depth pass and the main color pass use it. Render-to-texture
            // cameras select their own views (1..N-1) during draw().
            if (signature.test(scene->getComponentId<TerrainComponent>())){
                TerrainComponent& terrain = scene->getComponent<TerrainComponent>(entity);

                updateTerrain(terrain, transform, mainCamera, mainCameraTransform, terrain.views[0]);
            }
            
            if (signature.test(scene->getComponentId<AudioSourceComponent>())){
                AudioSourceComponent& audio = scene->getComponent<AudioSourceComponent>(entity);

                audio.needUpdate = true;
            }

        }

        if (transform.needUpdate){

            if (signature.test(scene->getComponentId<ModelComponent>())){
                ModelComponent& model = scene->getComponent<ModelComponent>(entity);

                model.inverseDerivedTransform = transform.modelMatrix.inverse();
            }

            if (signature.test(scene->getComponentId<BoneComponent>())){
                BoneComponent& bone = scene->getComponent<BoneComponent>(entity);

                if (bone.model != NULL_ENTITY && bone.index >= 0){
                    ModelComponent* model = scene->findComponent<ModelComponent>(bone.model);

                    if (model && model->skinBindings.empty()) {
                        Matrix4 skinning = model->inverseDerivedTransform * transform.modelMatrix * bone.offsetMatrix;

                        auto updateDrivenMesh = [&](MeshComponent* drivenMesh) {
                            if (!drivenMesh || !drivenMesh->bonesMatrix.validIndex(bone.index))
                                return;
                            drivenMesh->bonesMatrix[bone.index] = skinning;
                            drivenMesh->needUpdateBones = true;
                            drivenMesh->needUpdateAABB = true;
                        };

                        // Single-mesh / flattened models keep their skinned geometry on the model
                        // entity's own mesh. Multi-node skinned models split each mesh node into a
                        // child entity (each with its own MeshComponent), so the same bone must drive
                        // every child mesh that shares this skeleton.
                        // A moved bone changes the skinned bounds, so flag the driven mesh(es)
                        // for a world-AABB rebuild. Bones are processed after their mesh in this
                        // loop, so this takes effect next frame with the fresh bonesMatrix.
                        updateDrivenMesh(scene->findComponent<MeshComponent>(bone.model));

                        for (auto const& meshNode : model->meshNodesMapping){
                            updateDrivenMesh(scene->findComponent<MeshComponent>(meshNode.second));
                        }
                    }
                }
            }

        }

        transform.needUpdateChildVisibility = false;
        transform.needUpdate = false;
    }

    for (int i = 0; i < cameras->size(); i++){
        CameraComponent& camera = cameras->getComponentFromIndex(i);
        if (camera.needUpdate){
            camera.needUpdate = false;
        }
    }

    // every light as the objects' lists copy it, then the lights each mesh is lit by
    updateSceneLights();
    updateObjectLights();

    processLights(primaryView.shadows, numLights, mainCamera, mainCameraTransform);
    if (hasLights2D){
        processLights2D();
    }

    // build the merged occluder segment list once per frame (world transforms are
    // final here); draw() uploads it at most once, even when the same scene is
    // drawn multiple times per frame (e.g. open as a tab AND layered as a child scene)
    if (hasShadows2D && hasShadow2DAtlas){
        shadowCaster2DVertexCount = buildShadowCaster2DSegments();
        shadowCaster2DNeedUpdateBuffer = (shadowCaster2DVertexCount > 0);
    }else{
        shadowCaster2DVertexCount = 0;
        shadowCaster2DNeedUpdateBuffer = false;
    }
}

//-----Per-camera lighting and shadow maps-----

bool RenderSystem::ShadowAtlasKey::sameDepths(const ShadowAtlasKey& other) const{
    if (slots != other.slots || cameraCasters != other.cameraCasters)
        return false;
    for (int s = 0; s < SHADOW_KEY_SLOTS; s++){
        if (casterMask[s] != other.casterMask[s] || viewProjection[s] != other.viewProjection[s])
            return false;
    }
    return true;
}

RenderSystem::CameraShadows::~CameraShadows(){
    release();
}

void RenderSystem::CameraShadows::releaseProjective(){
    ownProjective.destroyFramebuffer();
    ownProjectiveWidth = 0;
    ownProjectiveHeight = 0;
    projectiveKey.epoch = 0;
    if (projectiveAtlas == &ownProjective)
        projectiveAtlas = nullptr;
}

void RenderSystem::CameraShadows::releasePoint(){
    ownPoint.destroyFramebuffer();
    ownPointWidth = 0;
    ownPointHeight = 0;
    pointKey.epoch = 0;
    if (pointAtlas == &ownPoint)
        pointAtlas = nullptr;
}

void RenderSystem::CameraShadows::releaseInstances(){
    for (InstanceView& view : instanceViews){
        view.buffer.getRender()->destroyBuffer();
        view.data.clear();
        view.data.shrink_to_fit();
        view.capacity = 0;
        view.needUpload = false;
    }
    instanceRanges.clear();
    instanceBuild = 0;
}

void RenderSystem::CameraShadows::release(){
    releaseProjective();
    releasePoint();
    releaseInstances();
    projectiveAtlas = nullptr;
    pointAtlas = nullptr;
    drawProjective = false;
    drawPoint = false;
    keysEpoch = 0;
}

RenderSystem::CameraShadows& RenderSystem::getCameraShadows(Entity entity){
    std::unique_ptr<CameraShadows>& shadows = cameraShadows[entity];
    if (!shadows){
        shadows = std::make_unique<CameraShadows>();
    }
    return *shadows;
}

// A camera that stopped rendering keeps its maps a moment, so one that comes back
// (re-enabled, a probe capturing again) reallocates nothing.
void RenderSystem::releaseUnusedCameraShadows(){
    static constexpr uint64_t KEEP_UNUSED_DRAWS = 120;
    for (auto it = cameraShadows.begin(); it != cameraShadows.end();){
        if (drawCount - it->second->lastUsed > KEEP_UNUSED_DRAWS){
            it = cameraShadows.erase(it);
        }else{
            ++it;
        }
    }
}

// The layers holding meshes the shadow passes draw this update, and among them those whose
// casters a camera draws its own way: a terrain through its CDLOD cut, a billboard turned
// to it, instances culled or faded from its eye.
void RenderSystem::ensureCasterLayers(){
    if (casterLayersEpoch == shadowEpoch)
        return;
    casterLayersEpoch = shadowEpoch;
    shadowCasterLayers = 0;
    cameraCasterLayers = 0;

    // the spot and point lights the shadow passes draw: their shadow cameras are the
    // lights' own, so what reaches their maps is the same for every camera. Only lights
    // holding a shadow slot, so at most one per slot.
    auto lights = scene->getComponentArray<LightComponent>();
    const int numLights = (int)lights->size();
    lightCasterLayers.assign(numLights, 0u);
    int localLights[MAX_SHADOW_ATLAS_SLOTS + MAX_POINT_SHADOW_LIGHTS];
    int numLocalLights = 0;
    for (int l = 0; l < numLights; l++){
        const LightComponent& light = lights->getComponentFromIndex(l);
        if (light.type != LightType::DIRECTIONAL && light.shadows && light.intensity > 0 && light.shadowMapIndex >= 0 &&
                isLightActive(lights->getEntity(l), light) &&
                numLocalLights < MAX_SHADOW_ATLAS_SLOTS + MAX_POINT_SHADOW_LIGHTS){
            localLights[numLocalLights++] = l;
        }
    }

#ifdef JASPERENGINE_EDITOR
    // what a shadow pass draws: never the editor's gizmos and icons, always a mirror
    const bool drawsOverlays = passDrawsOverlays;
    const Entity excludedEntity = passExcludedEntity;
    passDrawsOverlays = false;
    passExcludedEntity = NULL_ENTITY;
#endif

    auto meshes = scene->getComponentArray<MeshComponent>();
    for (int i = 0; i < meshes->size(); i++){
        MeshComponent& mesh = meshes->getComponentFromIndex(i);
        if (!mesh.loaded || mesh.needReload || !mesh.castShadows)
            continue;
        Entity entity = meshes->getEntity(i);
        Transform* transform = scene->findComponent<Transform>(entity);
        if (!transform || !transform->visible || !isRenderActive(entity))
            continue;

        const uint32_t layerBit = 1u << scene->getEntityLayer(entity);
        shadowCasterLayers |= layerBit;

        // a terrain's cut stays inside its bounds; a turned billboard or a camera's own
        // instance list may not stay inside the frame's
        const bool terrain = scene->findComponent<TerrainComponent>(entity) != nullptr;
        bool unbounded = transform->billboard && !transform->fakeBillboard && !mesh.shadowsBillboard;
        InstancedMeshComponent* instmesh = scene->findComponent<InstancedMeshComponent>(entity);
        if (instmesh){
            unbounded = unbounded || instmesh->instancedBillboard ||
                instanceFadeCullEnd(*instmesh, mesh, *scene) >= 0.0f ||
                (instmesh->cullInstances && std::isfinite(instmesh->cullDistance) && instmesh->cullDistance > 0.0f);
        }
        if (terrain || unbounded){
            cameraCasterLayers |= layerBit;
        }

        // the batch test of the depth passes (isBatchVisible), against each shadow camera
        const bool everywhere = unbounded || !cullsBatch(mesh, instmesh);
        for (int i = 0; i < numLocalLights; i++){
            const int l = localLights[i];
            if (lightCasterLayers[l] & layerBit)
                continue;
            const LightComponent& light = lights->getComponentFromIndex(l);
            bool reaches = everywhere;
            const int faces = (light.type == LightType::POINT) ? SHADOW_CUBE_FACES : 1;
            for (int c = 0; c < faces && !reaches; c++){
                reaches = isInsideCamera(light.cameras[c].nearFar.y, light.cameras[c].frustumPlanes, mesh.worldAABB);
            }
            if (reaches){
                lightCasterLayers[l] |= layerBit;
            }
        }
    }

    for (int l = 0; l < numLights; l++){
        if (lights->getComponentFromIndex(l).type == LightType::DIRECTIONAL){
            lightCasterLayers[l] = shadowCasterLayers;
        }
    }

#ifdef JASPERENGINE_EDITOR
    passDrawsOverlays = drawsOverlays;
    passExcludedEntity = excludedEntity;
#endif
}

// What target's atlases hold once drawn: each slot's shadow camera and the layers casting
// into it. A slot nothing casts into is plain clear depth, whatever its camera.
void RenderSystem::computeShadowKeys(const CameraShadows& target, ShadowAtlasKey& projective, ShadowAtlasKey& point){
    for (ShadowAtlasKey* key : {&projective, &point}){
        key->slots = 0;
        key->cameraCasters = nullptr;
        for (int s = 0; s < SHADOW_KEY_SLOTS; s++){
            key->viewProjection[s] = Matrix4();
            key->casterMask[s] = 0;
        }
    }
    if (!hasShadows)
        return;

    ensureCasterLayers();

    auto lights = scene->getComponentArray<LightComponent>();
    const int numLights = std::min((int)lights->size(), (int)lightCasterLayers.size());
    for (int l = 0; l < numLights; l++){
        LightComponent& light = lights->getComponentFromIndex(l);
        // the lights renderShadowMaps draws
        if (light.intensity <= 0 || !light.shadows || light.shadowMapIndex < 0 ||
                !isLightActive(lights->getEntity(l), light))
            continue;

        const uint32_t mask = target.cameraMask & light.cullingMask & lightCasterLayers[l];
        const void* cameraCasters = (mask & cameraCasterLayers) ? (const void*)&target : nullptr;

        if (light.type == LightType::POINT){
            if (!hasShadowPointAtlas)
                continue;
            for (int c = 0; c < SHADOW_CUBE_FACES; c++){
                const int slot = light.shadowMapIndex + c;
                if (slot < 0 || slot >= MAX_POINT_SHADOW_ATLAS_SLOTS)
                    continue;
                point.slots++;
                point.casterMask[slot] = mask;
                if (mask)
                    point.viewProjection[slot] = lightCamera(target, l, light, c).lightViewProjectionMatrix;
                if (cameraCasters)
                    point.cameraCasters = cameraCasters;
            }
        }else{
            if (!hasShadowAtlas)
                continue;
            const int numCameras = (light.type == LightType::DIRECTIONAL) ? (int)lightCascades(target, l, light) : 1;
            for (int c = 0; c < numCameras; c++){
                const int slot = light.shadowMapIndex + ((light.type == LightType::DIRECTIONAL) ? c : 0);
                if (slot < 0 || slot >= shadowAtlasUsedSlots)
                    continue;
                projective.slots++;
                projective.casterMask[slot] = mask;
                if (mask)
                    projective.viewProjection[slot] = lightCamera(target, l, light, c).lightViewProjectionMatrix;
                if (cameraCasters)
                    projective.cameraCasters = cameraCasters;
            }
        }
    }
}

// An atlas drawn after this update holding what target wants: the frame's, target's own
// from an earlier draw, or another camera's. Null when none does.
FramebufferRender* RenderSystem::findShadowSource(CameraShadows& target, const ShadowAtlasKey& wanted, bool projective){
    auto holds = [&](const CameraShadows& source){
        const ShadowAtlasKey& key = projective ? source.projectiveKey : source.pointKey;
        return key.epoch == shadowEpoch && key.sameDepths(wanted);
    };

    // the frame's maps, described the first time a camera asks after the update
    CameraShadows& frame = primaryView.shadows;
    if ((frame.projectiveKey.epoch == shadowEpoch || frame.pointKey.epoch == shadowEpoch) && frame.keysEpoch != shadowEpoch){
        const uint64_t projectiveEpoch = frame.projectiveKey.epoch;
        const uint64_t pointEpoch = frame.pointKey.epoch;
        computeShadowKeys(frame, frame.projectiveKey, frame.pointKey);
        frame.projectiveKey.epoch = projectiveEpoch;
        frame.pointKey.epoch = pointEpoch;
        frame.keysEpoch = shadowEpoch;
    }
    if (holds(frame))
        return projective ? &shadowAtlasFramebuffer : &shadowPointAtlasFramebuffer;

    if (holds(target))
        return projective ? &target.ownProjective : &target.ownPoint;

    auto other = [&](CameraShadows& source) -> FramebufferRender*{
        if (&source == &target || !holds(source))
            return nullptr;
        return projective ? &source.ownProjective : &source.ownPoint;
    };
    for (auto& entry : cameraShadows){
        if (FramebufferRender* atlas = other(*entry.second))
            return atlas;
    }
#ifdef JASPERENGINE_EDITOR
    for (auto& entry : secondaryViews){
        if (FramebufferRender* atlas = other(entry.second->shadows))
            return atlas;
        for (auto& mirror : entry.second->mirrors){
            if (FramebufferRender* atlas = other(mirror.second->shadows))
                return atlas;
        }
    }
#endif
    return nullptr;
}

int RenderSystem::ownPointAtlasRows() const{
    auto lights = scene->getComponentArray<LightComponent>();
    const int numLights = (int)lights->size();
    int usedSlots = 0;
    for (int l = 0; l < numLights; l++){
        const LightComponent& light = lights->getComponentFromIndex(l);
        if (light.type == LightType::POINT && light.shadows && light.shadowMapIndex >= 0){
            usedSlots = std::max(usedSlots, std::min(light.shadowMapIndex + SHADOW_CUBE_FACES, MAX_POINT_SHADOW_ATLAS_SLOTS));
        }
    }
    return std::max(1, (usedSlots + SHADOW_POINT_ATLAS_COLS - 1) / SHADOW_POINT_ATLAS_COLS);
}

// A camera's own projective atlas has the frame's layout, so the slots and their uniforms
// line up; its own point atlas has the frame's slots in only the rows its lights use
bool RenderSystem::ensureOwnShadowAtlas(CameraShadows& target, bool projective){
    if (projective){
        const int width = (int)shadowAtlasSlotResolution * shadowAtlasCols;
        const int height = (int)shadowAtlasSlotResolution * shadowAtlasRows;
        if (target.ownProjective.isCreated() && target.ownProjectiveWidth == width && target.ownProjectiveHeight == height)
            return true;
        target.releaseProjective();
        // LINEAR on a comparison sampler is hardware PCF, as on the frame's atlas
        if (width <= 0 || height <= 0 || !target.ownProjective.createDepthOnlyFramebuffer(
                width, height,
                TextureFilter::LINEAR, TextureFilter::LINEAR,
                TextureWrap::CLAMP_TO_BORDER, TextureWrap::CLAMP_TO_BORDER, true)){
            return false;
        }
        target.ownProjectiveWidth = width;
        target.ownProjectiveHeight = height;
        return true;
    }

    const int width = (int)shadowPointAtlasSlotResolution * SHADOW_POINT_ATLAS_COLS;
    const int height = (int)shadowPointAtlasSlotResolution * ownPointAtlasRows();
    if (target.ownPoint.isCreated() && target.ownPointWidth == width && target.ownPointHeight == height)
        return true;
    target.releasePoint();
    if (width <= 0 || height <= 0 || !target.ownPoint.createFramebuffer(
            TextureType::TEXTURE_2D, width, height,
            TextureFilter::NEAREST, TextureFilter::NEAREST,
            TextureWrap::CLAMP_TO_BORDER, TextureWrap::CLAMP_TO_BORDER, true)){
        return false;
    }
    target.ownPointWidth = width;
    target.ownPointHeight = height;
    return true;
}

void RenderSystem::prepareCameraShadows(CameraShadows& target, const CameraComponent& camera, const Transform& cameraTransform, int terrainView, bool ownInstances){
    target.lastUsed = drawCount;
    target.cameraMask = camera.cullingMask;
    target.terrainView = terrainView;
    target.ownLightCameras = true;
    target.ownShadowInstances = ownInstances;

    auto lights = scene->getComponentArray<LightComponent>();
    const int numLights = (int)lights->size();

    // directional cascades fitted to this camera's frustum, by the light's base shadow slot
    for (int i = 0; i < numLights; i++){
        LightComponent& light = lights->getComponentFromIndex(i);
        if (light.type != LightType::DIRECTIONAL || light.shadowMapIndex < 0 || light.shadowMapIndex >= MAX_SHADOW_ATLAS_SLOTS)
            continue;
        LightCameraSet& cameras = target.lightCameras[light.shadowMapIndex];
        cameras.numCascades = light.numShadowCascades;

        if (!hasShadows || !light.shadows || light.intensity <= 0)
            continue;

        Transform* transform = scene->findComponent<Transform>(lights->getEntity(i));
        if (!transform){
            for (int c = 0; c < MAX_SHADOWCASCADES; c++){
                cameras.cameras[c] = light.cameras[c];
            }
            continue;
        }

        Vector2 nearFar = light.shadowCameraNearFar;
        if (light.automaticShadowCamera){
            nearFar.x = camera.nearClip;
            nearFar.y = (light.range == 0.0f) ? camera.farClip : light.range;
        }
        computeLightCameras(light, *transform, camera, cameraTransform, nearFar, cameras.cameras, cameras.numCascades);
    }

    // the atlas layout and every camera-independent value are the frame's
    const CameraShadows& frame = primaryView.shadows;
    target.fs_lighting = frame.fs_lighting;
    target.vs_shadows = frame.vs_shadows;
    target.fs_shadows = frame.fs_shadows;
    target.fs_point_shadows = frame.fs_point_shadows;
    processLights(target, numLights, camera, cameraTransform);

    target.projectiveAtlas = nullptr;
    target.pointAtlas = nullptr;
    target.drawProjective = false;
    target.drawPoint = false;

    if (!hasShadows){
        target.releaseProjective();
        target.releasePoint();
        target.releaseInstances();
        return;
    }

    computeShadowKeys(target, wantedProjectiveKey, wantedPointKey);

    if (wantedProjectiveKey.slots == 0){
        target.releaseProjective();
        target.releaseInstances();
    }else if (FramebufferRender* source = findShadowSource(target, wantedProjectiveKey, true)){
        target.projectiveAtlas = source;
        if (source != &target.ownProjective){
            target.releaseProjective();
            target.releaseInstances();
        }
    }else if (ensureOwnShadowAtlas(target, true)){
        target.projectiveKey = wantedProjectiveKey;
        target.projectiveKey.epoch = 0; // until drawn
        target.projectiveAtlas = &target.ownProjective;
        target.drawProjective = true;
    }

    if (wantedPointKey.slots == 0){
        target.releasePoint();
    }else if (FramebufferRender* source = findShadowSource(target, wantedPointKey, false)){
        target.pointAtlas = source;
        if (source != &target.ownPoint){
            target.releasePoint();
        }
    }else if (ensureOwnShadowAtlas(target, false)){
        target.pointKey = wantedPointKey;
        target.pointKey.epoch = 0; // until drawn
        target.pointAtlas = &target.ownPoint;
        target.drawPoint = true;
    }

    // a camera's own point atlas (this one's or another's) keeps only the rows in use
    if (target.pointAtlas && target.pointAtlas != &shadowPointAtlasFramebuffer){
        const int rows = ownPointAtlasRows();
        for (int s = 0; s < MAX_POINT_SHADOW_ATLAS_SLOTS; s++){
            const int col = s % SHADOW_POINT_ATLAS_COLS;
            const int row = s / SHADOW_POINT_ATLAS_COLS;
            target.fs_point_shadows.atlasRect[s] = (row < rows) ? Vector4(
                (float)col / (float)SHADOW_POINT_ATLAS_COLS,
                (float)row / (float)rows,
                1.0f / (float)SHADOW_POINT_ATLAS_COLS,
                1.0f / (float)rows) : Vector4(0.0f, 0.0f, 0.0f, 0.0f);
        }
    }
}

void RenderSystem::buildCameraShadowInstances(CameraShadows& target){
    if (!target.drawProjective || !target.ownShadowInstances)
        return;

    auto instmeshes = scene->getComponentArray<InstancedMeshComponent>();
    if (instmeshes->size() == 0){
        if (target.instanceBuild != 0)
            target.releaseInstances();
        return;
    }

    // the slots, their cameras and detail levels are target's
    CameraShadows* previousShadows = passShadows;
    passShadows = &target;

    const std::vector<ShadowSlot>& slots = collectShadowSlots();

    uint64_t shadowHash = 1469598103934665603ULL;
    for (const ShadowSlot& shadow : slots){
        const LightCamera& shadowCamera = lightCamera(shadow.lightIndex, *shadow.light, shadow.cameraIndex);
        const unsigned char* bytes = reinterpret_cast<const unsigned char*>(&shadowCamera.lightViewProjectionMatrix);
        for (size_t b = 0; b < sizeof(Matrix4); b++){
            shadowHash = (shadowHash ^ bytes[b]) * 1099511628211ULL;
        }
        shadowHash = (shadowHash ^ (uint64_t)(shadow.slot + 1)) * 1099511628211ULL;
    }

    if (target.instanceBuild != 0 && target.instanceRevision == instanceRevision && target.instanceShadowHash == shadowHash){
        passShadows = previousShadows;
        return;
    }
    target.instanceBuild++;
    target.instanceRevision = instanceRevision;
    target.instanceShadowHash = shadowHash;

    for (InstanceView& view : target.instanceViews){
        view.data.clear();
    }

    for (int i = 0; i < instmeshes->size(); i++){
        InstancedMeshComponent& instmesh = instmeshes->getComponentFromIndex(i);
        Entity entity = instmeshes->getEntity(i);
        MeshComponent* mesh = scene->findComponent<MeshComponent>(entity);
        Transform* transform = scene->findComponent<Transform>(entity);

        CameraShadows::InstanceRanges& ranges = target.instanceRanges[entity];
        ranges.build = target.instanceBuild;
        ranges.slots.fill(InstanceViewRange());
        if (!mesh || !transform || !mesh->loaded || !mesh->castShadows)
            continue;

        // the frame's list, the one this camera's color pass draws
        const InstanceRenderData* instances = instmesh.renderInstances.data();
        const InstanceBounds* bounds = instmesh.renderBounds.data();
        const unsigned int count = (instmesh.renderBounds.size() < instmesh.numVisible) ? 0 : instmesh.numVisible;

        for (const ShadowSlot& shadow : slots){
            const LightCamera& shadowCamera = lightCamera(shadow.lightIndex, *shadow.light, shadow.cameraIndex);
            LodView shadowLod = shadowLodView(*shadow.light, shadow.lightIndex, shadow.cameraIndex, shadow.slot);
            Vector3 lightDirection = (shadow.light->type == LightType::DIRECTIONAL) ? shadow.light->worldDirection : Vector3::ZERO;
            buildInstanceView(target.instanceViews[shadow.slot], ranges.slots[shadow.slot], instances, bounds, count, instmesh, *mesh, *transform,
                shadowCamera.nearFar.y, shadowCamera.frustumPlanes, shadowLod, shadowLod.origin, shadowLod.origin, lightDirection, InstanceOrder::NEAREST, false);
        }
    }

    // batches that went away
    for (auto it = target.instanceRanges.begin(); it != target.instanceRanges.end();){
        if (it->second.build != target.instanceBuild){
            it = target.instanceRanges.erase(it);
        }else{
            ++it;
        }
    }

    for (InstanceView& view : target.instanceViews){
        view.needUpload = !view.data.empty();
    }
    uploadInstanceViews(target.instanceViews, MAX_SHADOW_ATLAS_SLOTS);

    passShadows = previousShadows;
}

void RenderSystem::renderCameraShadows(CameraShadows& target){
    if (!target.drawProjective && !target.drawPoint)
        return;

    CameraShadows* previousShadows = passShadows;
    passShadows = &target;
    renderShadowMaps(target.drawProjective, target.drawPoint);
    passShadows = previousShadows;

    if (target.drawProjective)
        target.projectiveKey.epoch = shadowEpoch;
    if (target.drawPoint)
        target.pointKey.epoch = shadowEpoch;
    target.drawProjective = false;
    target.drawPoint = false;
}

// The CDLOD cut a render-to-texture camera draws, selected before its shadow passes so
// they draw the same terrain its color pass does
void RenderSystem::selectCameraTerrain(CameraComponent& camera, Transform& cameraTransform, int terrainView){
#ifdef JASPERENGINE_EDITOR
    // a camera image never shows the editor's gizmos and icons
    passDrawsOverlays = false;
#endif
    auto terrains = scene->getComponentArray<TerrainComponent>();
    for (int i = 0; i < terrains->size(); i++){
        TerrainComponent& terrain = terrains->getComponentFromIndex(i);
        Entity entity = terrains->getEntity(i);
        Transform* transform = scene->findComponent<Transform>(entity);
        MeshComponent* mesh = scene->findComponent<MeshComponent>(entity);
        if (!transform || !mesh || scene->findComponent<CameraComponent>(entity))
            continue;
        if (!transform->visible || samplesCameraTarget(camera, *mesh) || !isRenderActive(entity) || !isLayerVisible(entity, camera.cullingMask))
            continue;
        updateTerrain(terrain, *transform, camera, cameraTransform, terrain.views[terrainView]);
    }
}

// Work the frame does once, whatever views draw it: texture and buffer uploads, the
// culled instance lists of the scene camera, the shadow maps of its lights, the 2D
// shadow rows and the reflection probe face of the frame.
void RenderSystem::drawFramePasses(){
    frameTime = (float)Engine::getSystemTime();

#ifdef JASPERENGINE_EDITOR
    passDrawsOverlays = false;
#endif

    updateShadowBindings();
    updateAllTerrainRenderTextures();
    // as in Unity, a layer the scene camera culls casts no shadow into its maps
    CameraShadows& frameShadows = primaryView.shadows;
    frameShadows.cameraMask = LAYER_MASK_EVERYTHING;
    if (CameraComponent* mainCamera = scene->findComponent<CameraComponent>(scene->getCamera())){
        frameShadows.cameraMask = mainCamera->cullingMask;
        if (Transform* mainCameraTransform = scene->findComponent<Transform>(scene->getCamera())){
            buildInstanceViews(*mainCamera, *mainCameraTransform);
        }
    }
    updateInstanceBuffers();

    // free the fixed-resolution target when the setting is turned off (the blit
    // pipeline itself is kept; it is cheap and may be re-enabled)
    if (!isFixedResolutionActive() && rv().fixedResFramebuffer.isCreated()){
        rv().fixedResFramebuffer.destroy();
        rv().fixedResWidth = 0;
        rv().fixedResHeight = 0;
    }

    // the frame block the shadow passes' depth programs read: the scene camera's
    if (CameraComponent* mainCamera = scene->findComponent<CameraComponent>(scene->getCamera())){
        if (Transform* mainCameraTransform = scene->findComponent<Transform>(scene->getCamera())){
            updatePassFrame(*mainCamera, *mainCameraTransform);
        }
    }

    renderShadowMaps(true, true);
    // the frame's maps hold this update's casters; what they hold is worked out only
    // when another camera asks (computeShadowKeys)
    frameShadows.projectiveKey.epoch = hasShadows ? shadowEpoch : 0;
    frameShadows.pointKey.epoch = hasShadows ? shadowEpoch : 0;
    render2DShadows();

    // Runtime-captured probes share a strict budget: one cubemap face total per
    // frame. Static probes without an authored cubemap capture once; dynamic
    // probes follow their configured update policy.
    renderReflectionProbeCapture();

    // HTML UI documents draw into their own offscreen target before any camera pass, so
    // the composite below never needs a second swapchain pass (Vulkan allows one a frame)
    bool renderDocuments = true;
#ifdef JASPERENGINE_EDITOR
    // the Scene view shows documents of UI and 2D scenes (canvas in world space); a 3D
    // scene's free camera has no canvas, its documents show in the Game view only
    if (drawEditorOverlays && !rv().secondary){
        CameraComponent* sceneCamera = scene->findComponent<CameraComponent>(scene->getCamera());
        renderDocuments = sceneCamera && sceneCamera->type != CameraType::CAMERA_PERSPECTIVE;
    }
#endif
    if (renderDocuments)
        scene->getSystem<UIDocumentSystem>()->renderOffscreen(targetViewRect());
}

// Depth passes of the shadow-casting lights into the projective and point atlases of
// the camera being drawn (ps(): the frame's for the scene camera, its own otherwise),
// through its shadow cameras.
void RenderSystem::renderShadowMaps(bool projective, bool point){
    if (!hasShadows)
        return;

#ifdef JASPERENGINE_EDITOR
    // the scene's editor gizmos and icons never cast shadows
    passDrawsOverlays = false;
#endif

    auto lights = scene->getComponentArray<LightComponent>();
    auto meshes = scene->getComponentArray<MeshComponent>();
    int shadowLightCount = (int)lights->size();

    // as in Unity, a layer the rendering camera or the light culls casts no shadow
    const uint32_t shadowCameraMask = ps().cameraMask;

    // a projective slot has its own culled instance view; point faces draw everything
    auto drawShadowCasters = [&](LightComponent& light, int lightIndex, int cameraIndex, PipelineType pipelineType, int instanceView){
        const LightCamera& shadowCamera = lightCamera(lightIndex, light, cameraIndex);
        const LodView shadowLod = (instanceView >= 0) ? shadowLodView(light, lightIndex, cameraIndex, instanceView - 1) : LodView();
        const LodView* lodView = (instanceView >= 0) ? &shadowLod : nullptr;
        const uint32_t casterMask = shadowCameraMask & light.cullingMask;
        for (int i = 0; i < meshes->size(); i++){
            MeshComponent& mesh = meshes->getComponentFromIndex(i);
            Entity entity = meshes->getEntity(i);
            Transform* transform = scene->findComponent<Transform>(entity);

            if (!transform || !transform->visible || !isRenderActive(entity) || !isLayerVisible(entity, casterMask)){
                continue;
            }

            InstancedMeshComponent* instmesh = scene->findComponent<InstancedMeshComponent>(entity);
            TerrainComponent* terrain = scene->findComponent<TerrainComponent>(entity);
            TilemapComponent* tilemap = scene->findComponent<TilemapComponent>(entity);

            const Matrix4& model = objectModel(*transform);
            vs_depth_t vsDepthParams;
            if (transform->billboard && mesh.shadowsBillboard){
                Matrix4 modelViewMatrix = shadowCamera.lightViewMatrix * model;

                modelViewMatrix.set(0, 0, transform->worldScale.x);
                modelViewMatrix.set(0, 1, 0.0);
                modelViewMatrix.set(0, 2, 0.0);

                if (!transform->cylindricalBillboard) {
                    modelViewMatrix.set(1, 0, 0.0);
                    modelViewMatrix.set(1, 1, transform->worldScale.y);
                    modelViewMatrix.set(1, 2, 0.0);
                }

                modelViewMatrix.set(2, 0, 0.0);
                modelViewMatrix.set(2, 1, 0.0);
                modelViewMatrix.set(2, 2, transform->worldScale.z);

                const Matrix4& projection = shadowCamera.lightProjectionMatrix;
                vsDepthParams = {modelViewMatrix, projection, projection * modelViewMatrix};
            }else{
                const Matrix4& lightVP = shadowCamera.lightViewProjectionMatrix;
                vsDepthParams = {model, lightVP, lightVP * model};
            }

            drawMeshDepth(
                entity,
                mesh,
                *transform,
                shadowCamera.nearFar.y,
                shadowCamera.frustumPlanes,
                vsDepthParams,
                instmesh,
                terrain,
                tilemap,
                false,
                pipelineType,
                instanceView,
                lodView);
        }
    };

    // All projective slots share one pass: clear once, then viewport/scissor per slot.
    FramebufferRender& projectiveAtlas = shadowAtlasTarget();
    bool projectivePassStarted = false;
    for (int l = 0; projective && l < shadowLightCount; l++){
        LightComponent& light = lights->getComponentFromIndex(l);
        if (light.type == LightType::POINT ||
            light.intensity <= 0 || !light.shadows ||
            light.shadowMapIndex < 0 || !hasShadowAtlas ||
            !isLightActive(lights->getEntity(l), light)){
            continue;
        }

        int numShadowCameras =
            (light.type == LightType::DIRECTIONAL) ? (int)lightCascades(l, light) : 1;
        for (int c = 0; c < numShadowCameras; c++){
            int slotIndex = light.shadowMapIndex +
                ((light.type == LightType::DIRECTIONAL) ? c : 0);
            if (slotIndex < 0 || slotIndex >= shadowAtlasUsedSlots){
                continue;
            }

            if (!projectivePassStarted){
                shadowAtlasPassRender.setClearDepth(1.0f);
                shadowAtlasPassRender.setLabel("shadow");
                shadowAtlasPassRender.startRenderPass(&projectiveAtlas);
                projectivePassStarted = true;
            }

            Rect slotRect = getShadowAtlasSlotRect(slotIndex);
            shadowAtlasPassRender.applyViewport(slotRect);
            shadowAtlasPassRender.applyScissor(slotRect);
            passResolution = Vector2(slotRect.getWidth(), slotRect.getHeight());
            drawShadowCasters(light, l, c, PIP_SHADOW_DEPTH, 1 + slotIndex);
        }
    }
    if (projectivePassStarted){
        shadowAtlasPassRender.endRenderPass();
    }

    // Point shadows keep the packed-color depth per-face pass (PIP_DEPTH).
    FramebufferRender& pointAtlas = shadowPointAtlasTarget();
    bool pointAtlasSlotWritten = false;
    for (int l = 0; point && l < shadowLightCount; l++){
        LightComponent& light = lights->getComponentFromIndex(l);
        if (light.type != LightType::POINT ||
            light.intensity <= 0 || !light.shadows ||
            light.shadowMapIndex < 0 || !hasShadowPointAtlas ||
            !isLightActive(lights->getEntity(l), light)){
            continue;
        }

        for (int c = 0; c < SHADOW_CUBE_FACES; c++){
            int slotIndex = light.shadowMapIndex + c;
            if (slotIndex < 0 || slotIndex >= MAX_POINT_SHADOW_ATLAS_SLOTS){
                continue;
            }

            if (!pointAtlasSlotWritten){
                shadowPointAtlasPassRender.setClearColor(Vector4(1.0, 1.0, 1.0, 1.0));
            }else{
                shadowPointAtlasPassRender.setLoadActionLoad();
            }

            Rect slotRect = getShadowPointAtlasSlotRect(slotIndex);
            shadowPointAtlasPassRender.setLabel("point shadow");
            shadowPointAtlasPassRender.startRenderPass(&pointAtlas);
            shadowPointAtlasPassRender.applyViewport(slotRect);
            shadowPointAtlasPassRender.applyScissor(slotRect);
            pointAtlasSlotWritten = true;

            passResolution = Vector2(slotRect.getWidth(), slotRect.getHeight());
            drawShadowCasters(light, l, c, PIP_DEPTH, -1);
            shadowPointAtlasPassRender.endRenderPass();
        }
    }
}

// 2D light shadow pass (1D polar rows): world space, so every view shares it
void RenderSystem::render2DShadows(){
    if (!hasShadows2D || !hasShadow2DAtlas)
        return;

    if (shadowCaster2DVertexCount > 0 && (!shadowCaster2DLoaded || shadowCaster2DVertexCount > shadowCaster2DBufferCapacity)){
        unsigned int capacity = std::max(shadowCaster2DVertexCount * 2u, 256u);
        loadShadowCaster2DPass(capacity);
    }

    bool drawSegments = shadowCaster2DLoaded && shadowCaster2DVertexCount > 0;

    if (drawSegments && shadowCaster2DNeedUpdateBuffer){
        // occluders are world-space and can move every frame: re-upload the merged
        // segment buffer (built in update(), at most once per frame — sokol allows
        // a single sg_update_buffer per buffer and frame)
        shadowCaster2DBuffer.setData((unsigned char*)shadowCaster2DSegments.data(), shadowCaster2DSegments.size() * sizeof(float));
        shadowCaster2DBuffer.getRender()->updateBuffer(shadowCaster2DBuffer.getSize(), shadowCaster2DBuffer.getData());
        shadowCaster2DNeedUpdateBuffer = false;
    }

    // rows are rendered (or at least cleared) every frame even with zero
    // segments: the rows are in light-relative polar space, so stale contents
    // would make old shadows follow the light around
    auto lights2d = scene->getComponentArray<Light2DComponent>();
    bool atlasRowWritten = false;

    for (int l = 0; l < numLights2D; l++){
        Light2DComponent& light = lights2d->getComponentFromIndex(l);
        if (!light.shadows || light.shadowMapIndex < 0 || light.intensity <= 0 ||
                !isLight2DActive(lights2d->getEntity(l), light)){
            continue;
        }
        Entity entity = lights2d->getEntity(l);
        Transform* lightTransform = scene->findComponent<Transform>(entity);
        if (!lightTransform){
            continue;
        }

        // first row of the frame clears the whole atlas to white (= far/unoccluded)
        if (!atlasRowWritten){
            shadow2DAtlasPassRender.setClearColor(Vector4(1.0, 1.0, 1.0, 1.0));
        }else{
            shadow2DAtlasPassRender.setLoadActionLoad();
        }
        Rect rowRect(0.0f, (float)light.shadowMapIndex, (float)shadow2DAtlasWidth, 1.0f);
        shadow2DAtlasPassRender.setLabel("2d shadow");
        shadow2DAtlasPassRender.startRenderPass(&shadow2DAtlasFramebuffer);
        shadow2DAtlasPassRender.applyViewport(rowRect);
        shadow2DAtlasPassRender.applyScissor(rowRect);
        atlasRowWritten = true;

        if (drawSegments && shadowCaster2DRender.beginDraw(PIP_DEPTH)){
            vs_shadow2d_t shadow2dParams;
            shadow2dParams.lightPos_range = Vector4(
                lightTransform->worldPosition.x, lightTransform->worldPosition.y,
                0.0f, std::max(light.range, 0.001f));

            // three passes at NDC x offsets -2/0/+2 so segments crossing the
            // theta = +-pi seam rasterize on both ends of the row
            for (int k = -1; k <= 1; k++){
                shadow2dParams.offset = Vector4(2.0f * (float)k, 0.0f, 0.0f, 0.0f);
                shadowCaster2DRender.applyUniformBlock(shadow2DSlotParams, sizeof(vs_shadow2d_t), &shadow2dParams);
                shadowCaster2DRender.draw(0, shadowCaster2DVertexCount, 1);
            }
        }

        shadow2DAtlasPassRender.endRenderPass();
    }
}

void RenderSystem::draw(){
    drawFramePasses();
    drawCameras(true);

#ifdef JASPERENGINE_EDITOR
    frameDrawn = true;
#endif

    //---------Missing some shaders----------
    // Like Unity, a shader variant the build lacks does not end the game: each one is
    // reported once, and what draws with it is left out (its load fails, see
    // ShaderPool::get) while everything else keeps rendering.
    static size_t reportedMissingShaders = 0;
    const std::vector<std::string>& missingShaders = ShaderPool::getMissingShaders();
    for (; reportedMissingShaders < missingShaders.size(); reportedMissingShaders++){
        const std::string& shaderStr = missingShaders[reportedMissingShaders];

        std::string shaderSpec;
        if (!ShaderPool::getShaderCliSpec(shaderStr, shaderSpec)) {
            shaderSpec = "<shader-spec>";
        }
        const std::string shaderOutputDir = System::instance().getShaderPath();
        const std::string shaderBackend = ShaderPool::getSuggestedCliBackend();
        const std::string shaderCommand = "jasperengine-editor shaders --out \"" + shaderOutputDir + "\" --backend " + shaderBackend + " --shader \"" + shaderSpec + "\"";

        // why it was not compiled here instead
        const char* runtimeCompilation = ShaderPool::isRuntimeCompilationSupported()
            ? "Runtime shader compilation is turned off (ShaderPool::setRuntimeCompilationEnabled)"
            : "This build has no runtime shader compiler (Project Settings > Graphics > Runtime Shader Compilation)";

        Log::error(
            "Missing shader: the build has no precompiled shader '%s' (%s) in %s; "
            "objects drawn with it are not shown. %s. Export the project again so it includes it, "
            "add it under Shaders in the Build Settings window, or generate it with "
            "JasperEngine Editor: %s",
            shaderStr.c_str(), ShaderPool::getShaderLangStr().c_str(), shaderOutputDir.c_str(),
            runtimeCompilation, shaderCommand.c_str());
    }
}

// The frame's cameras: the scene camera and each enabled render-to-texture camera, in
// component order. includeMain false leaves out the scene camera (a secondary view
// draws its own camera instead).
void RenderSystem::drawCameras(bool includeMain){
    auto cameras = scene->getComponentArray<CameraComponent>();

    // the blurred scene a view's passes may read is its base layer's of this frame
    if (includeMain && isBaseLayer())
        frameBlurredScene = nullptr;

    // assigns each rendering camera its own terrain CDLOD view slot this frame:
    // main camera = 0 (selected in update(), also used by the shadow pass), each
    // render-to-texture camera = 1..N-1; cameras past the cap reuse the main view
    int terrainViewCounter = 0;

    // Like Unity, cameras rendering into a texture draw before the camera that shows the
    // frame: whatever displays their image (a mesh, a UI image) then shows this frame's,
    // not the previous one, and the first frame of a scene does not sample a texture
    // nothing has rendered into yet (fully transparent, so those objects vanished).
    // The render-to-texture cameras keep their component order among themselves.
    for (int round = 0; round < 2; round++)
    for (int i = 0; i < cameras->size(); i++){
        Entity cameraEntity = cameras->getEntity(i);
        CameraComponent& camera = cameras->getComponentFromIndex(i);
        Transform* cameraTransformPtr = scene->findComponent<Transform>(cameraEntity);
        if (!cameraTransformPtr)
            continue;

        bool isMainCamera = (cameraEntity == scene->getCamera());

        if (isMainCamera != (round == 1)){
            continue; // render-to-texture cameras in the first round, the main one in the second
        }

        if (!isMainCamera && !camera.renderToTexture){
            continue; // camera is not used
        }

        // a disabled render-to-texture camera keeps its last image; the main camera always draws
        if (!isMainCamera && (!camera.enabled || !scene->isEntityActiveInHierarchy(cameraEntity))){
            continue;
        }

        int terrainView = 0;
        if (!isMainCamera){
            terrainViewCounter++;
            terrainView = (terrainViewCounter < MAX_TERRAIN_VIEWS) ? terrainViewCounter : 0;
        }

        if (isMainCamera && !includeMain){
            continue;
        }

        // Any other camera sees the scene through its own frustum and culling mask: its
        // terrain cut, then lighting and shadow maps fitted to it (shared where they
        // would hold the same depths as maps already drawn), before its color pass
        CameraShadows* cameraLighting = nullptr;
        if (!isMainCamera){
            if (terrainView != 0){
                selectCameraTerrain(camera, *cameraTransformPtr, terrainView);
            }
            cameraLighting = &getCameraShadows(cameraEntity);
            prepareCameraShadows(*cameraLighting, camera, *cameraTransformPtr, terrainView, true);
            buildCameraShadowInstances(*cameraLighting);
            updatePassFrame(camera, *cameraTransformPtr);
            renderCameraShadows(*cameraLighting);
        }

        CameraPass pass;
        pass.camera = &camera;
        pass.cameraTransform = cameraTransformPtr;
        pass.isMain = isMainCamera;
        pass.renderToTexture = camera.renderToTexture;
        pass.terrainView = terrainView;
        passShadows = cameraLighting;
        renderCameraPass(pass);
        passShadows = nullptr;
    }

    // what the view's atmosphere ran (get_atmosphere_info), its unused targets freed; a
    // secondary view ends its own after its camera (drawView)
    if (includeMain)
        atmosphere.endFrame();

    drawCount++;
    rv().sceneInputDraws++;
    releaseUnusedCameraShadows();

    // scene inputs (phase 3): what this frame's passes did, and the targets no pass used
    // for a while (nothing to do in a scene that never read the scene)
    lastSceneInputStats = sceneInputStats;
    sceneInputStats.passes = 0;
    sceneInputStats.depthPrePasses = 0;
    sceneInputStats.sharedDepth = 0;
    sceneInputStats.colorCopies = 0;
    if (!primaryView.sceneInputTargets.empty())
        releaseSceneInputTargets(primaryView, false);
#ifdef JASPERENGINE_EDITOR
    for (auto& entry : secondaryViews){
        if (!entry.second->sceneInputTargets.empty())
            releaseSceneInputTargets(*entry.second, false);
    }
#endif
}

// One camera's color pass, with the effects its destination asks for: SSR, SSAO, the
// post-process chain and the fixed resolution run for the view camera only.
void RenderSystem::renderCameraPass(const CameraPass& pass){
    std::vector<TransparentRenderData>& transparentRenders = transparentQueue;
    transparentRenders.clear();
    // submission order of transparentRenders, the last tie-break of their sort
    uint32_t transparentOrder = 0;

    auto transforms = scene->getComponentArray<Transform>();

    CameraComponent& camera = *pass.camera;
    Transform& cameraTransform = *pass.cameraTransform;
    const bool isMainCamera = pass.isMain;
    const int terrainView = pass.terrainView;
    // layers this camera draws (Unity Camera.cullingMask)
    const uint32_t cullingMask = camera.cullingMask;

#ifdef JASPERENGINE_EDITOR
    // a secondary pass reads its own per-object matrices; draw() rewrites the Transform
    // ones per camera when the frame has several
    const bool primaryPass = (passObjects == nullptr);
    // editor gizmos and icons show in the editor's own view only, never in a camera image
    passDrawsOverlays = isMainCamera && !pass.renderToTexture && (rv().secondary ? rv().editorOverlays : drawEditorOverlays);
    // so does the Scene view's face culling toggle: its camera's depth, G-buffer and color
    // passes draw every face, a camera image or reflection keeps the game's culling
    passDisablesFaceCulling = isMainCamera && !pass.renderToTexture && (rv().secondary ? rv().disableFaceCulling : disableFaceCulling);
#else
    const bool primaryPass = true;
#endif
    const bool perCameraMVP = primaryPass && (hasMultipleCameras || hasReflectionProbes);

    // the frame block of this camera (its depth, G-buffer and colour passes below read it)
    updatePassFrame(camera, cameraTransform);

    // Scene inputs (shader library phase 3): what the visible draws of this pass read of the
    // scene (nothing to look at in a scene without such a shader)
    passSceneDepth = nullptr;
    passSceneColor = nullptr;
    const uint32_t sceneInputs = sceneInputUsage ? findPassSceneInputs(camera, isMainCamera ? 0 : -1) : 0;

    // Screen-space reflections (main camera only). The G-buffer geometry pass runs
    // FIRST so SSAO can share its depth (a single geometry pre-pass feeds both
    // effects). Further below the opaque color pass is redirected into an offscreen
    // buffer, then renderSSR() marches the G-buffer and composites reflections to the
    // real destination: a framebuffer (editor / render-to-texture / fixed resolution)
    // or the swapchain in exported builds (see updateSwapchainRedirect).
    // Fixed game resolution (main scene main camera only): the color pass is
    // redirected into fixedResFramebuffer at the scene's fixed size, and
    // renderFixedResolutionBlit() upscales it to the view rect afterwards.
    bool useFixedRes = false;
    if (isMainCamera && !pass.renderToTexture && isFixedResolutionActive()){
        loadBlit();
        if (blitLoaded && ensureFixedResFramebuffer(scene->getFixedResolutionWidth(),
                scene->getFixedResolutionHeight(), scene->getFixedResolutionFilter())){
            useFixedRes = true;
        }
    }

    Framebuffer* target = targetFramebuffer();
    const Rect viewRect = targetViewRect();

    // The uber pass (shader library phase 4): the post-processed camera (as the post-process
    // chain: the main camera of the base layer, or a main camera drawing into a texture; the
    // view lets it) draws its scene offscreen, at the render scale, and the uber pass draws that
    // to the destination with its effects. With fixed resolution it is the blit: it reads the
    // fixed-resolution target (no extra pass).
    bool useUber = false;
    unsigned int uberSceneWidth = 0;
    unsigned int uberSceneHeight = 0;
    if (postTiers.uber && isMainCamera && (isBaseLayer() || pass.renderToTexture) && rv().postProcessing
            && (target || pass.renderToTexture || useFixedRes || swapchainRedirect) && loadUber(postTiers.uberFeatures)){
        if (useFixedRes){
            uberSceneWidth = rv().fixedResWidth;
            uberSceneHeight = rv().fixedResHeight;
            useUber = true;
        }else{
            const float baseWidth = pass.renderToTexture ? (float)camera.framebuffer->getWidth() : viewRect.getWidth();
            const float baseHeight = pass.renderToTexture ? (float)camera.framebuffer->getHeight() : viewRect.getHeight();
            uberSceneWidth = (unsigned int)std::max(1L, std::lround(baseWidth * postTiers.renderScale));
            uberSceneHeight = (unsigned int)std::max(1L, std::lround(baseHeight * postTiers.renderScale));
            // its source target is made below, unless the post-process chain's output is it
            useUber = true;
        }
    }
    // the bloom and blur chains' targets go with their effect (not for a frame the uber pass
    // waits for its variant)
    if (isMainCamera && (isBaseLayer() || pass.renderToTexture)){
        if (!(postTiers.uberFeatures & UBER_BLOOM) && rv().bloomTargets.isCreated())
            rv().bloomTargets.destroy();
        if (!(postTiers.uberFeatures & UBER_BLUR) && rv().blurTargets.isCreated())
            rv().blurTargets.destroy();
        // so do the uber pass's scene target (not made with fixed resolution: the uber pass
        // reads that target), the multisampled target and the linear sampler a NEAREST fixed
        // resolution is read through. By the settings, not this pass: the editor's Scene view
        // draws the same view without post-processing between two Game view frames
        if ((!postTiers.uber || isFixedResolutionActive()) && rv().postSceneFramebuffer.isCreated()){
            rv().postSceneFramebuffer.destroy();
            rv().postSceneWidth = 0;
            rv().postSceneHeight = 0;
        }
        if (msaaPipelineSamples <= 1 && rv().msaaFramebuffer.isCreated()){
            rv().msaaFramebuffer.destroy();
            rv().msaaWidth = 0;
            rv().msaaHeight = 0;
            rv().msaaSamples = 1;
        }
        if (postLinearSampler.isCreated() && !(postTiers.uber && isFixedResolutionActive()
                && scene->getFixedResolutionFilter() == TextureFilter::NEAREST))
            postLinearSampler.destroyTexture();
    }
    bool useSSR = false;
    FramebufferRender* ssrDestination = nullptr;
    if (isMainCamera && scene->isSSREnabled() && (target || pass.renderToTexture || useFixedRes || useUber || swapchainRedirect)){
        loadSSR();
        unsigned int sw = (unsigned int)viewRect.getWidth();
        unsigned int sh = (unsigned int)viewRect.getHeight();
        if (useUber){
            // SSR composites into the uber pass's source, at the render scale
            sw = uberSceneWidth;
            sh = uberSceneHeight;
        }else if (useFixedRes){
            // effects run at the fixed resolution: SSR composites into the
            // fixed-res target, which the blit then upscales
            sw = rv().fixedResWidth;
            sh = rv().fixedResHeight;
        }
        if (ssrLoaded && ensureSSRFramebuffers(sw, sh) && ensureGBufferFramebuffer(sw, sh)){
            renderGBufferPass(camera);
            useSSR = true;
        }
    }

    // the packed opaque depth of this pass another effect draws (the G-buffer, the SSAO or
    // post-process depth pre-pass), which the scene inputs then reuse. Not the G-buffer's when
    // it draws a visible opaque mesh differently (built-in programs: no library displacement or
    // clipping; no split mesh's opaque submeshes): then the pass makes its own.
    TextureRender* gbufferDepth = useSSR ? &rv().gbufferFramebuffer.getRender().getColorAttachmentTexture(0) : nullptr;
    const bool gbufferSceneDepth = gbufferDepth && (sceneInputs & ShaderRequire_SceneDepth) &&
        (gbufferDepthMismatch.empty() || !hasVisibleGBufferDepthMismatch(camera, isMainCamera ? 0 : -1));
    TextureRender* sharedSceneDepth = gbufferSceneDepth ? gbufferDepth : nullptr;

    // User post-process chain: like SSR it needs the scene color offscreen, so it
    // follows the same destination-capture rule. Main scene only: its last pass
    // writes every pixel, so a layer scene would erase the scene below it.
    bool usePostProcess = false;
    FramebufferRender* postDestination = nullptr;
    if (isMainCamera && !scene->getPostProcessPasses().empty()
            && (isBaseLayer() || pass.renderToTexture)
            && (target || pass.renderToTexture || useFixedRes || useUber || swapchainRedirect)){
        loadPostProcess();

        // only a swapchain destination ends the chain with a blit
        bool needsBlit = !(target || pass.renderToTexture || useFixedRes || useUber);
        if (needsBlit){
            loadBlit();
        }

        unsigned int pw = useSSR ? rv().ssrWidth : (unsigned int)viewRect.getWidth();
        unsigned int ph = useSSR ? rv().ssrHeight : (unsigned int)viewRect.getHeight();
        if (useUber){
            // the chain runs before the uber pass, at the render scale, into its source
            pw = uberSceneWidth;
            ph = uberSceneHeight;
        }else if (useFixedRes){
            pw = rv().fixedResWidth;
            ph = rv().fixedResHeight;
        }

        if (postProcessLoaded && !postProcessPasses.empty() && (!needsBlit || blitLoaded)
                && ensurePostProcessFramebuffers(pw, ph)){
            usePostProcess = true;

            // a pass sampling depth needs the pre-pass when neither SSR nor SSAO ran
            if (postProcessNeedsDepth && !useSSR && !scene->isSSAOEnabled()
                    && ensureSSAOFramebuffers(pw, ph)){
                renderDepthPrePass(camera, perCameraMVP ? &cameraTransform : nullptr);
                sharedSceneDepth = &rv().ssaoDepthFramebuffer.getRender().getColorTexture();
            }
        }
    }

    // what the uber pass reads: the scene colour pass or SSR write it (the fixed-resolution
    // target, else the scene target at the render scale); after the user post-process chain, the
    // chain's last output (no target of its own: renderPostProcess keeps its result)
    FramebufferRender* uberSource = nullptr;
    const bool uberReadsChain = useUber && usePostProcess && !useFixedRes;
    if (useUber && !uberReadsChain){
        if (useFixedRes){
            uberSource = &rv().fixedResFramebuffer.getRender();
        }else if (ensurePostSceneFramebuffer(uberSceneWidth, uberSceneHeight)){
            uberSource = &rv().postSceneFramebuffer.getRender();
        }else{
            useUber = false;
        }
    }
    if (useUber && !uberSource && !uberReadsChain)
        useUber = false;
    // the scene target the uber pass does not read goes (the chain's output is its source now)
    if (uberReadsChain && rv().postSceneFramebuffer.isCreated()){
        rv().postSceneFramebuffer.destroy();
        rv().postSceneWidth = 0;
        rv().postSceneHeight = 0;
    }

    // Screen-space AO is produced once for the main camera before its color pass.
    // When SSR is active its G-buffer already holds the camera depth (color[0]), so
    // SSAO reuses it instead of running a second geometry pre-pass. Other cameras
    // sample empty white (AO = 1) so their USE_SSAO meshes are unaffected.
    rv().currentSSAOTexture = &emptyWhite;
    if (isMainCamera && scene->isSSAOEnabled()){
        TextureRender* ssaoDepth = renderSSAO(camera, useFixedRes, gbufferDepth, perCameraMVP ? &cameraTransform : nullptr);
        if (ssaoDepth && (ssaoDepth != gbufferDepth || gbufferSceneDepth))
            sharedSceneDepth = ssaoDepth;
    }

    // Exported builds whose scene reads the scene colour draw the scene camera offscreen
    // (updateSwapchainRedirect), then blit it to the swapchain, when nothing else does
    const bool useSceneColorRedirect = isMainCamera && !pass.renderToTexture && !target && !useFixedRes && !useSSR && !usePostProcess
            && !useUber && sceneColorRedirect && rv().sceneColorRedirect.isCreated();

    // Exported builds with MSAA and no other offscreen effect: the multisampled scene resolves
    // into a target blitted to the swapchain (MSAA needs an offscreen colour target)
    const bool useMsaaRedirect = isMainCamera && !pass.renderToTexture && !target && !useFixedRes && !useSSR && !usePostProcess
            && !useUber && !useSceneColorRedirect && msaaRedirect && isBaseLayer() && rv().postProcessing
            && ensureMsaaResolveFramebuffer((unsigned int)viewRect.getWidth(), (unsigned int)viewRect.getHeight());

    // whether this camera's color pass targets an offscreen framebuffer
    // (selects PIP_RTT pipelines and flipped rendering on GL)
    bool offscreenTarget = pass.renderToTexture || target || useFixedRes || useUber || useSSR || usePostProcess || useSceneColorRedirect
            || useMsaaRedirect;

    // MSAA (phase 4): the post-processed camera's colour pass draws into a multisampled target
    // that resolves, at the end of the pass, into the offscreen target it would draw into (so
    // what reads that target, SSR, the chain, the bloom and the uber pass, reads it resolved)
    int passMsaa = 1;
    if (msaaPipelineSamples > 1 && isMainCamera && (isBaseLayer() || pass.renderToTexture) && rv().postProcessing
            && offscreenTarget && !camera.invertCulling)
        passMsaa = msaaPipelineSamples;

    // The colour target of the pass (null: the swapchain), its size and viewport, as the
    // branches below pick them: the scene inputs and the atmosphere's targets are made to
    // match it
    Rect sceneInputViewport;
    unsigned int sceneInputTargetWidth = 0;
    unsigned int sceneInputTargetHeight = 0;
    {
        if (useSSR){
            sceneInputTargetWidth = rv().ssrWidth;
            sceneInputTargetHeight = rv().ssrHeight;
        }else if (usePostProcess){
            sceneInputTargetWidth = rv().postProcessWidth;
            sceneInputTargetHeight = rv().postProcessHeight;
        }else if (useUber){
            sceneInputTargetWidth = uberSceneWidth;
            sceneInputTargetHeight = uberSceneHeight;
        }else if (useFixedRes){
            sceneInputTargetWidth = rv().fixedResWidth;
            sceneInputTargetHeight = rv().fixedResHeight;
        }else if (pass.renderToTexture){
            sceneInputTargetWidth = (unsigned int)camera.framebuffer->getWidth();
            sceneInputTargetHeight = (unsigned int)camera.framebuffer->getHeight();
        }else if (useSceneColorRedirect){
            sceneInputTargetWidth = rv().sceneColorRedirect.getWidth();
            sceneInputTargetHeight = rv().sceneColorRedirect.getHeight();
        }else if (useMsaaRedirect){
            sceneInputTargetWidth = rv().msaaResolveFramebuffer.getWidth();
            sceneInputTargetHeight = rv().msaaResolveFramebuffer.getHeight();
        }
        if (sceneInputTargetWidth){
            sceneInputViewport = Rect(0, 0, (float)sceneInputTargetWidth, (float)sceneInputTargetHeight);
        }else{
            sceneInputViewport = viewRect;
            sceneInputTargetWidth = target ? (unsigned int)target->getWidth() : (unsigned int)System::instance().getScreenWidth();
            sceneInputTargetHeight = target ? (unsigned int)target->getHeight() : (unsigned int)System::instance().getScreenHeight();
        }
        if (sceneInputs)
            sceneInputStats.passes++;
    }

    // The atmosphere (docs/specs/S12-atmosphere.md): what this pass draws of it, and before
    // its colour pass the reduced-resolution passes (volumetric fog, Screen clouds, the Sky
    // Map's refresh). The volumetric passes read the packed depth of the pass: an effect's
    // above, else a depth pre-pass of their own the scene inputs then share.
    AtmosphereRenderer::PassPlan atmospherePlan = atmosphere.planPass(camera, isMainCamera, sortsByDistance(camera));
    float atmosphereMarched = 0.0f;
    if (atmospherePlan.any()){
        bool forcedDepth = false;
        if (atmospherePlan.needsDepth() && !sharedSceneDepth && atmosphereDepthPrograms){
            const unsigned int depthWidth = (unsigned int)sceneInputViewport.getWidth();
            const unsigned int depthHeight = (unsigned int)sceneInputViewport.getHeight();
            if (SceneInputTarget* depthTarget = ensureSceneInputTarget(false, depthWidth, depthHeight)){
                renderOpaqueDepth(camera, depthTarget->framebuffer, depthWidth, depthHeight, isMainCamera ? 0 : -1,
                                  isMainCamera ? &rv().lodView : nullptr, perCameraMVP ? &cameraTransform : nullptr);
                sharedSceneDepth = &depthTarget->framebuffer.getRender().getColorTexture();
                forcedDepth = true;
            }
        }
        atmosphereMarched = atmosphere.renderPassTargets(atmospherePlan, camera, cameraTransform, sharedSceneDepth, forcedDepth,
                                                         (unsigned int)sceneInputViewport.getWidth(), (unsigned int)sceneInputViewport.getHeight());
    }
    // the scene colour is copied out of an offscreen colour target (the swapchain cannot be read)
    const bool copySceneColorPending = (sceneInputs & ShaderRequire_SceneColor) && offscreenTarget;

    // the scene depth: the opaque meshes' depth pre-pass of this camera, drawn here unless an
    // effect above drew it already
    if (sceneInputs & ShaderRequire_SceneDepth){
        if (sharedSceneDepth){
            passSceneDepth = sharedSceneDepth;
            sceneInputStats.sharedDepth++;
        }else if (sceneDepthPrograms){
            // (without depth programs yet, a reader that came after this update's switch, the
            // meshes could not draw it: the far-depth fallback this once)
            const unsigned int depthWidth = (unsigned int)sceneInputViewport.getWidth();
            const unsigned int depthHeight = (unsigned int)sceneInputViewport.getHeight();
            if (SceneInputTarget* depthTarget = ensureSceneInputTarget(false, depthWidth, depthHeight)){
                renderOpaqueDepth(camera, depthTarget->framebuffer, depthWidth, depthHeight, isMainCamera ? 0 : -1,
                                  isMainCamera ? &rv().lodView : nullptr, perCameraMVP ? &cameraTransform : nullptr);
                passSceneDepth = &depthTarget->framebuffer.getRender().getColorTexture();
                sceneInputStats.depthPrePasses++;
            }
        }
    }
    PipelineType colorPip = colorPipeline(camera, offscreenTarget);
    if (passMsaa > 1)
        colorPip = camera.depthTest ? PIP_MSAA : PIP_MSAA_NODEPTH;
    bool distanceSort = sortsByDistance(camera);

    if (isBaseLayer() || pass.renderToTexture){
        camera.render.setClearColor(scene->getBackgroundColor());
    } else {
        // When drawing a scene as an Engine layer, don't clear the color buffer.
        // Otherwise a scene that was previously the main scene would keep clearing
        // and hide the main scene when reused as a layer.
        camera.render.setLoadActionLoad();
    }

    // the space the UI scissor lives in
    Rect colorPassViewport;
    // the offscreen target the colour pass draws into, and whether it set a viewport (both
    // for a pass split by the scene-colour copy)
    FramebufferRender* colorTarget = nullptr;
    bool colorPassViewportApplied = false;
    camera.render.setLabel(pass.renderToTexture ? "camera rtt" : (isBaseLayer() ? "camera" : "camera layer"));
    auto startColorPass = [&](FramebufferRender* framebuffer){
        colorTarget = framebuffer;
        if (passMsaa > 1){
            // the multisampled target the size of the one it resolves into
            TextureRender& resolve = framebuffer->getColorTexture();
            if (ensureMsaaFramebuffer((unsigned int)resolve.getWidth(), (unsigned int)resolve.getHeight(), passMsaa)){
                camera.render.setMultisample(&rv().msaaFramebuffer.getRender());
            }else{
                passMsaa = 1;
                colorPip = colorPipeline(camera, offscreenTarget);
            }
        }
        if (copySceneColorPending){
            camera.render.startSplitRenderPass(framebuffer);
        }else{
            camera.render.startRenderPass(framebuffer);
        }
    };

    if (useSSR || usePostProcess){
        // capture the real destination for the composite / post-process chain, then
        // redirect the scene into the offscreen color buffer
        FramebufferRender* destination = nullptr;
        if (useUber){
            // the uber pass draws the real destination
            destination = uberSource;
        }else if (pass.renderToTexture){
            if (!camera.framebuffer->isCreated()){
                camera.framebuffer->create();
            }
            destination = &camera.framebuffer->getRender();
        }else if (useFixedRes){
            destination = &rv().fixedResFramebuffer.getRender();
        }else if (target){
            if (!target->isCreated()){
                target->create();
            }
            destination = &target->getRender();
        }

        // the chain samples its input, so whatever runs before it writes the first
        // ping-pong buffer and the chain writes the real destination
        if (usePostProcess){
            postDestination = destination;
            ssrDestination = &rv().postProcessFramebuffer[0].getRender();
        }else{
            ssrDestination = destination;
        }

        if (useSSR){
            colorPassViewport = Rect(0, 0, (float)rv().ssrWidth, (float)rv().ssrHeight);
            startColorPass(&rv().sceneColorFramebuffer.getRender());
            camera.render.applyViewport(colorPassViewport);
        }else{
            colorPassViewport = Rect(0, 0, (float)rv().postProcessWidth, (float)rv().postProcessHeight);
            startColorPass(&rv().postProcessFramebuffer[0].getRender());
            camera.render.applyViewport(colorPassViewport);
        }
        colorPassViewportApplied = true;
    }else if (useFixedRes){
        // full offscreen target; letterbox/upscale happens in the blit pass
        colorPassViewport = Rect(0, 0, (float)rv().fixedResWidth, (float)rv().fixedResHeight);
        startColorPass(&rv().fixedResFramebuffer.getRender());
    }else if (useUber){
        // the scene at the render scale; the uber pass draws it at the view rect
        colorPassViewport = Rect(0, 0, (float)uberSceneWidth, (float)uberSceneHeight);
        startColorPass(&rv().postSceneFramebuffer.getRender());
        camera.render.applyViewport(colorPassViewport);
        colorPassViewportApplied = true;
    }else if (useSceneColorRedirect){
        // the whole redirect target; renderBlit draws it at the view rect afterwards
        colorPassViewport = Rect(0, 0, (float)rv().sceneColorRedirect.getWidth(), (float)rv().sceneColorRedirect.getHeight());
        startColorPass(&rv().sceneColorRedirect.getRender());
    }else if (useMsaaRedirect){
        // the whole resolve target; renderBlit draws it at the view rect afterwards
        colorPassViewport = Rect(0, 0, (float)rv().msaaResolveFramebuffer.getWidth(), (float)rv().msaaResolveFramebuffer.getHeight());
        startColorPass(&rv().msaaResolveFramebuffer.getRender());
    }else if (!pass.renderToTexture){
        if (target){
            if (!target->isCreated()){
                target->create();
            }
            startColorPass(&target->getRender());
        }else{
            camera.render.startRenderPass();
        }
        colorPassViewport = viewRect;
        camera.render.applyViewport(colorPassViewport);
        colorPassViewportApplied = true;
    }else{
        if (!camera.framebuffer->isCreated()){
            camera.framebuffer->create();
        }
        colorPassViewport = Rect(0, 0, (float)camera.framebuffer->getWidth(), (float)camera.framebuffer->getHeight());
        startColorPass(&camera.framebuffer->getRender());
    }

    passResolution = Vector2(colorPassViewport.getWidth(), colorPassViewport.getHeight());
    passFrame.resolution = Vector4(passResolution.x, passResolution.y,
                                   passResolution.x > 0.0f ? 1.0f / passResolution.x : 0.0f,
                                   passResolution.y > 0.0f ? 1.0f / passResolution.y : 0.0f);

    // the height fog of this colour pass (u_fs_heightFog of the 'Hfg' variants), with the
    // distance its volumetric fog marched (the composite replaces that part of the analytic fog)
    const bool passFlipped = isRenderingFlipped(camera);
    if (heightFogVariants || atmospherePlan.any())
        atmosphere.fillHeightFog(passHeightFog, camera, cameraTransform, colorPassViewport, sceneInputTargetHeight, passFlipped, atmosphereMarched);

    // the post-processing colour of this colour pass: the scene's for the post-processed camera
    // (the main camera of the base layer, as the post-process chain; not mirrors, probes or other
    // render-to-texture cameras), neutral for the others (updatePassFrame)
    if (postTiers.tailColor && isMainCamera && (isBaseLayer() || pass.renderToTexture) && rv().postProcessing){
        // (an engine target is covered whole; the view's target or the swapchain may letterbox)
        const float targetHeight = (target && (!colorTarget || colorTarget == &target->getRender())) ? (float)target->getHeight()
                                 : (colorTarget ? colorPassViewport.getY() + colorPassViewport.getHeight() : (float)System::instance().getScreenHeight());
        fillPassPostColor(true, colorPassViewport, targetHeight);
    }

    // the scene inputs' block of this pass, and the copy of the opaque queue and the sky the
    // draws requiring the scene colour sample (a 3D pass copies after its opaque queue, a 2D
    // pass, drawn in hierarchy order, right before the first such draw)
    bool sceneColorPending = copySceneColorPending && colorTarget;
    if (sceneInputs){
        const bool fullCopy = scene->getSceneColorScale() == SceneColorScale::FULL;
        const unsigned int copyWidth = fullCopy ? (unsigned int)colorPassViewport.getWidth() : std::max(1u, (unsigned int)colorPassViewport.getWidth() / 2);
        const unsigned int copyHeight = fullCopy ? (unsigned int)colorPassViewport.getHeight() : std::max(1u, (unsigned int)colorPassViewport.getHeight() / 2);
        fillSceneInputs(camera, colorPassViewport, sceneInputTargetHeight, passFlipped, copyWidth, copyHeight);
    }
    auto takeSceneColor = [&](){
        sceneColorPending = false;
        camera.render.endRenderPass();
        passSceneColor = copySceneColor(&colorTarget->getColorTexture(), colorPassViewport, sceneInputTargetWidth, sceneInputTargetHeight);
        camera.render.resumeRenderPass(colorTarget);
        if (colorPassViewportApplied){
            camera.render.applyViewport(colorPassViewport);
        }
    };

    //---------Draw opaque meshes and UI----------
    bool hasActiveScissor = false;

    // only the main camera has a culled instance view and detail levels
    const int instanceView = isMainCamera ? 0 : -1;
    const LodView* lodView = isMainCamera ? &rv().lodView : nullptr;

    const bool depthPrepass = isMainCamera && camera.depthTest && scene->isDepthPrepassEnabled();

    // a 3D pass draws opaque meshes nearest first so early depth rejects what nearer
    // surfaces cover; a 2D pass keeps the transform order, which is its layering, and
    // only lists them for the depth prepass
    std::vector<OpaqueRenderData>& opaqueRenders = opaqueQueue;
    opaqueRenders.clear();
    if (distanceSort || depthPrepass){
        for (int i = 0; i < transforms->size(); i++){
            Transform& transform = transforms->getComponentFromIndex(i);
            Entity entity = transforms->getEntity(i);
            Signature signature = scene->getSignature(entity);

            if (!signature.test(scene->getComponentId<MeshComponent>()) || signature.test(scene->getComponentId<CameraComponent>())){
                continue;
            }

            if (perCameraMVP){
                updateMVP(i, transform, camera, cameraTransform);
            }

            MeshComponent& mesh = scene->getComponent<MeshComponent>(entity);
            if (!transform.visible || samplesCameraTarget(camera, mesh) || !isRenderActive(entity, signature) || !isLayerVisible(entity, cullingMask)){
                continue;
            }

            InstancedMeshComponent* instmesh = scene->findComponent<InstancedMeshComponent>(entity);
            // a render-to-texture camera's own cut was selected before its shadow passes
            TerrainComponent* terrain = scene->findComponent<TerrainComponent>(entity);
            TilemapComponent* tilemap = scene->findComponent<TilemapComponent>(entity);

            // a split mesh (MeshComponent::splitQueues) is in both: its opaque submeshes here
            if (!mesh.transparent || mesh.splitQueues){
                const float distance = mesh.worldAABB.isNull() ? 0.0f : mesh.worldAABB.squaredDistance(cameraTransform.worldPosition);
                opaqueRenders.push_back({entity, &mesh, instmesh, terrain, tilemap, &transform, distance, mesh.opaqueSortPriority, (uint32_t)opaqueRenders.size()});
            }
            if (mesh.transparent && distanceSort){
                transparentRenders.push_back({TransparentRenderType::MESH, entity, &mesh, nullptr, instmesh, terrain, tilemap, &transform, objectDistance(transform), mesh.transparentSortPriority, transparentOrder++});
            }
        }

        if (distanceSort){
            // sortPriority first (lower draws first, so stencil writers go before their
            // readers), then nearest first; ties keep the transform order (no allocation,
            // unlike std::stable_sort)
            std::sort(opaqueRenders.begin(), opaqueRenders.end(), OpaqueRenderComparison());
        }
    }

    // the same draws, depth only, with the color pass's own MVP; a color fork
    // without a depth fork is left out since its vertices may not match
    if (depthPrepass){
        const PipelineType prepassPip = passMsaa > 1 ? PIP_ZPREPASS_MSAA : (offscreenTarget ? PIP_ZPREPASS_RTT : PIP_ZPREPASS);
        Matrix4 renderVP = camera.viewProjectionMatrix;
        if (isRenderingFlipped(camera)){
            renderVP = Matrix4::scaleMatrix(Vector3(1, -1, 1)) * renderVP;
        }
        for (const OpaqueRenderData& opaque : opaqueRenders){
            // a forked color shader without a depth program, or no depth write (see loadMesh)
            if (opaque.mesh->skipsDepthPrepass)
                continue;
            vs_depth_t params = {objectModel(*opaque.transform), renderVP, objectMVP(*opaque.transform)};
            passSubmeshQueue = opaque.mesh->transparent ? SubmeshQueue::OPAQUE : SubmeshQueue::ALL;
            drawMeshDepth(opaque.entity, *opaque.mesh, *opaque.transform, camera.farClip, camera.frustumPlanes, params, opaque.instmesh, opaque.terrain, opaque.tilemap, true, prepassPip, instanceView, lodView);
        }
        passSubmeshQueue = SubmeshQueue::ALL;
    }

    //---------Draw sky----------
    auto skys = scene->getComponentArray<SkyboxComponent>();
    if (skys->size() > 0){
        SkyboxComponent& sky = skys->getComponentFromIndex(0);
        if (sky.visible){
            if (perCameraMVP){
                updateSkyViewProjection(sky, camera);
            }

            drawSky(sky, colorPip, passSkyViewProjection(sky));
        }
    }

    if (distanceSort){
        for (const OpaqueRenderData& opaque : opaqueRenders){
            // (a transparent mesh here is split: its opaque submeshes)
            passSubmeshQueue = opaque.mesh->transparent ? SubmeshQueue::OPAQUE : SubmeshQueue::ALL;
            drawMesh(opaque.entity, *opaque.mesh, *opaque.transform, camera, cameraTransform, colorPip, opaque.instmesh, opaque.terrain, opaque.tilemap, terrainView, instanceView, lodView);
        }
        passSubmeshQueue = SubmeshQueue::ALL;
        // the atmosphere over the opaque scene: the sky composite (Sky Map clouds and the sky
        // fill on the sky's pixels), then the Screen clouds and the volumetric fog; what is
        // drawn after (transparents) takes the analytic fog alone
        if (atmospherePlan.any())
            atmosphere.drawComposites(atmospherePlan, colorPip, passMsaa, colorPassViewport, sceneInputTargetHeight, passFlipped, camera, cameraTransform);
        // the opaque queue and the sky are drawn
        if (sceneColorPending){
            takeSceneColor();
        }
    }

    for (int i = 0; i < transforms->size(); i++){
        Transform& transform = transforms->getComponentFromIndex(i);
        Entity entity = transforms->getEntity(i);
        Signature signature = scene->getSignature(entity);

        if (signature.test(scene->getComponentId<CameraComponent>())){
            continue;
        }

        // a 2D pass: what was drawn before the first draw requiring the scene colour (one this
        // pass draws: an object hidden, culled or out of view takes no copy)
        if (sceneColorPending && drawsSceneColorReader(entity, signature, transform, camera, instanceView)){
            takeSceneColor();
        }

        if (distanceSort && signature.test(scene->getComponentId<MeshComponent>())){
            continue; // drawn above
        }

        if (perCameraMVP){
            updateMVP(i, transform, camera, cameraTransform);
        }

        // inactive GameObjects, disabled renderers and layers the camera culls draw
        // nothing; a UI element still sets its scissor below so its children are
        // clipped the same way
        const bool renderActive = isRenderActive(entity, signature) && isLayerVisible(entity, cullingMask);

        // apply scissor on UI
        if (signature.test(scene->getComponentId<RectTransformComponent>())){
            RectTransformComponent& layout = scene->getComponent<RectTransformComponent>(entity);
            PassScissor scissor = passScissor(transform, layout);

            Rect parentScissor;
            bool parentScissorActive = false;

            if (transform.parent != NULL_ENTITY){
                Signature parentSignature = scene->getSignature(transform.parent);
                if (parentSignature.test(scene->getComponentId<RectTransformComponent>())){
                    RectTransformComponent& parentLayout = scene->getComponent<RectTransformComponent>(transform.parent);
                    Transform& parentTransform = scene->getComponent<Transform>(transform.parent);
                    PassScissor parent = passScissor(parentTransform, parentLayout);

                    parentScissorActive = *parent.active;
                    parentScissor = *parent.rect;
                }
            }

            // recomputed every pass, so children never get a stale rect
            *scissor.active = false;

            bool inheritsScissor = parentScissorActive && !layout.ignoreScissor;
            bool clippedAway = false;

            if (inheritsScissor){
                *scissor.rect = parentScissor;
                *scissor.active = true;

                if (parentScissor.getWidth() <= 0 || parentScissor.getHeight() <= 0){
                    // sokol widens a scissor to 1x1 on Metal, Vulkan and WebGPU, so an
                    // empty rect would leak a pixel: skip the draw instead
                    clippedAway = true;
                }else{
                    camera.render.applyScissor(parentScissor);

                    hasActiveScissor = true;
                }
            }

            if (signature.test(scene->getComponentId<ImageComponent>())){
                ImageComponent& img = scene->getComponent<ImageComponent>(entity);

                *scissor.rect = getScissorRect(layout, img, transform, colorPassViewport);
                // applied or not: a clipped away element still passes the empty rect down
                if (inheritsScissor){
                    *scissor.rect = intersectScissor(*scissor.rect, parentScissor);
                }
                *scissor.active = true;
            }

            if (clippedAway)
                continue;
        }

        if (signature.test(scene->getComponentId<MeshComponent>())){
            MeshComponent& mesh = scene->getComponent<MeshComponent>(entity);

            if (transform.visible && renderActive && !samplesCameraTarget(camera, mesh)){

                // Per-camera transparent-instance sorting is intentionally NOT done
                // here: the instance buffer is shared across all cameras and sokol
                // only allows one sg_update_buffer per buffer per frame
                // (VALIDATE_UPDATEBUF_ONCE). Re-sorting per camera re-flags
                // needUpdateBuffer and uploads the buffer again, which panics the
                // validation layer when a render-to-texture camera is present. Every
                // camera reuses the main camera's ordering computed in update().
                InstancedMeshComponent* instmesh = scene->findComponent<InstancedMeshComponent>(entity);

                // Per-view CDLOD: the main camera's selection (view 0) is done in
                // update(); each render-to-texture camera (mirror/RTT) selected its
                // own node cut into its own buffer before its shadow passes (see
                // drawCameras), so the reflection shows complete, correctly-tessellated
                // terrain.
                TerrainComponent* terrain = scene->findComponent<TerrainComponent>(entity);

                // chunk bounds are view-independent, so every camera reuses the
                // ones refreshed in update()
                TilemapComponent* tilemap = scene->findComponent<TilemapComponent>(entity);

                drawMesh(entity, mesh, transform, camera, cameraTransform, colorPip, instmesh, terrain, tilemap, terrainView, instanceView, lodView);
            }

        }else if (signature.test(scene->getComponentId<UIComponent>())){
            UIComponent& ui = scene->getComponent<UIComponent>(entity);

            if (transform.visible && renderActive && !samplesCameraTarget(camera, ui.texture))
                drawUI(ui, transform, colorPip);

        }else if (signature.test(scene->getComponentId<PointsComponent>())){
            PointsComponent& points = scene->getComponent<PointsComponent>(entity);

            // Per-camera sorting is intentionally NOT done here: the points buffer is
            // shared across all cameras and sokol only allows one sg_update_buffer per
            // buffer per frame (VALIDATE_UPDATEBUF_ONCE). Every camera reuses the main
            // camera's ordering computed in update().
            if (transform.visible && renderActive && !samplesCameraTarget(camera, points.texture)){
                if (!points.transparent || !distanceSort){
                    drawPoints(entity, points, transform, camera, cameraTransform, colorPip);
                }else{
                    transparentRenders.push_back({TransparentRenderType::POINTS, entity, nullptr, &points, nullptr, nullptr, nullptr, &transform, objectDistance(transform), points.sortPriority, transparentOrder++});
                }
            }

        }else if (signature.test(scene->getComponentId<LineRendererComponent>())){
            LineRendererComponent& lines = scene->getComponent<LineRendererComponent>(entity);

            if (transform.visible && renderActive)
                drawLines(lines, transform, cameraTransform, colorPip);

        }

        if (hasActiveScissor){
            // screen pixels would overflow an offscreen SSR / post-process attachment
            camera.render.applyScissor(colorPassViewport);
            hasActiveScissor = false;
        }
    }

    //---------Draw transparent renderers----------
    std::sort(transparentRenders.begin(), transparentRenders.end(), TransparentRenderComparison());
    for (const TransparentRenderData& renderData : transparentRenders){

        if (renderData.type == TransparentRenderType::MESH){
            // a split mesh: its blended submeshes (the opaque ones were drawn in the opaque queue)
            passSubmeshQueue = renderData.mesh->splitQueues ? SubmeshQueue::TRANSPARENT : SubmeshQueue::ALL;
            drawMesh(renderData.entity, *renderData.mesh, *renderData.transform, camera, cameraTransform, colorPip, renderData.instmesh, renderData.terrain, renderData.tilemap, terrainView, instanceView, lodView);
            passSubmeshQueue = SubmeshQueue::ALL;
        }else if (renderData.type == TransparentRenderType::POINTS){
            drawPoints(renderData.entity, *renderData.points, *renderData.transform, camera, cameraTransform, colorPip);
        }
    }

    //---------Draw HTML UI documents (in front of everything, including component UI)----------
    // UI and 2D cameras draw the canvas in world space, the editor's Scene view included; a
    // 3D camera fills its viewport, in the game camera only
    bool drawDocuments = isMainCamera && !pass.renderToTexture;
#ifdef JASPERENGINE_EDITOR
    drawDocuments = drawDocuments && (!passDrawsOverlays || camera.type != CameraType::CAMERA_PERSPECTIVE);
#endif
    if (drawDocuments){
        camera.render.applyScissor(colorPassViewport);
        scene->getSystem<UIDocumentSystem>()->drawComposite(camera, cameraTransform, colorPip, isRenderingFlipped(camera), passMsaa);
    }

    camera.render.endRenderPass();
    camera.render.setMultisample(nullptr);
    passSceneDepth = nullptr;
    passSceneColor = nullptr;

#ifdef JASPERENGINE_EDITOR
    passDisablesFaceCulling = false;
#endif

    // SSR: march the offscreen scene color and composite reflections into the
    // real destination (the swapchain or the captured framebuffer).
    if (useSSR){
        renderSSR(camera, ssrDestination);
    }

    // buffer 0 now holds the scene color (or the SSR composite)
    TextureRender* chainResult = nullptr;
    if (usePostProcess){
        chainResult = renderPostProcess(postDestination, uberReadsChain);
    }

    // The uber pass: the scene (and what SSR and the chain made of it) to the real destination,
    // upscaled; with fixed resolution it is that blit. Layer scenes draw after this (load
    // action), so overlaid UI scenes stay at native resolution and untouched.
    if (useUber && (uberSource || chainResult)){
        TextureRender* sourceTexture = chainResult ? chainResult : &uberSource->getColorTexture();
        const unsigned int sourceWidth = chainResult ? (unsigned int)chainResult->getWidth() : (useFixedRes ? rv().fixedResWidth : rv().postSceneWidth);
        const unsigned int sourceHeight = chainResult ? (unsigned int)chainResult->getHeight() : (useFixedRes ? rv().fixedResHeight : rv().postSceneHeight);
        // T2: the bloom and blur chains of what the uber pass reads (quarter resolution and below)
        // the fixed-resolution target keeps the user's filter (the plain blit's); NEAREST is
        // read bilinear at texel centres (renderUber)
        const bool nearestSource = !chainResult && useFixedRes && rv().fixedResFramebuffer.getMinFilter() == TextureFilter::NEAREST;
        // T2: the bloom and blur chains of what the uber pass reads (quarter resolution and
        // below), when the loaded variant composites them
        uberBloomTexture = (uberFeatures & UBER_BLOOM) ? renderBloomChain(sourceTexture, sourceWidth, sourceHeight, nearestSource) : nullptr;
        uberBlurTexture = (uberFeatures & UBER_BLUR) ? renderBlurChain(sourceTexture, sourceWidth, sourceHeight, nearestSource) : nullptr;
        if (pass.renderToTexture){
            renderUber(sourceTexture, sourceWidth, sourceHeight, &camera.framebuffer->getRender(),
                       Rect(0, 0, (float)camera.framebuffer->getWidth(), (float)camera.framebuffer->getHeight()), nearestSource);
        }else{
            renderUber(sourceTexture, sourceWidth, sourceHeight, nullptr, targetViewRect(), nearestSource);
            // the passes after it in this view (layer scenes) may read the blurred scene
            if (uberBlurTexture)
                frameBlurredScene = uberBlurTexture;
        }
        uberBloomTexture = nullptr;
        uberBlurTexture = nullptr;
    // Fixed game resolution: upscale the offscreen scene color to the view
    // rect of the real destination. Layer scenes draw after this (load
    // action), so overlaid UI scenes stay at native resolution.
    }else if (useFixedRes){
        renderFixedResolutionBlit();
    }

    // the scene drawn offscreen for its scene-colour copy, onto the swapchain
    if (useSceneColorRedirect){
        renderBlit(&rv().sceneColorRedirect.getRender().getColorTexture(), targetViewRect());
    }
    // the resolved multisampled scene, onto the swapchain
    if (useMsaaRedirect){
        renderBlit(&rv().msaaResolveFramebuffer.getRender().getColorTexture(), targetViewRect());
    }
}

void RenderSystem::onComponentAdded(Entity entity, ComponentId componentId) {
    if (componentId == scene->getComponentId<LightComponent>()) {
        needReloadMeshVariants();
    } else if (componentId == scene->getComponentId<Light2DComponent>()) {
        // shader variant change (USE_LIGHT2D)
        needReloadMeshVariants();
    } else if (componentId == scene->getComponentId<MirrorComponent>()) {
        // a mirror enables the inverted-culling pipeline on meshes; reload so it bakes
        needReloadMeshes();
    } else if (componentId == scene->getComponentId<ReflectionProbeComponent>()) {
        hasReflectionProbes = true;
        needReloadMeshVariants();
    }
}

void RenderSystem::onComponentRemoved(Entity entity, ComponentId componentId) {
#ifdef JASPERENGINE_EDITOR
    forgetEntityInViews(entity, componentId);
#endif

    if (componentId == scene->getComponentId<LightComponent>()) {
        LightComponent& light = scene->getComponent<LightComponent>(entity);
        destroyLight(light);
        needReloadMeshVariants();
    } else if (componentId == scene->getComponentId<Light2DComponent>()) {
        needReloadMeshVariants();
    } else if (componentId == scene->getComponentId<MeshComponent>()) {
        MeshComponent& mesh = scene->getComponent<MeshComponent>(entity);
        destroyMesh(entity, mesh);
        // (kept over reloads, which load it again)
        setSceneInputUser(entity, false, 0);
        setGBufferDepthMismatch(entity, false);
    } else if (componentId == scene->getComponentId<UIComponent>()) {
        UIComponent& ui = scene->getComponent<UIComponent>(entity);
        destroyUI(entity, ui);
    } else if (componentId == scene->getComponentId<PointsComponent>()) {
        PointsComponent& points = scene->getComponent<PointsComponent>(entity);
        destroyPoints(entity, points);
        setSceneInputUser(entity, true, 0);
    } else if (componentId == scene->getComponentId<LineRendererComponent>()) {
        LineRendererComponent& lines = scene->getComponent<LineRendererComponent>(entity);
        destroyLines(entity, lines);
    } else if (componentId == scene->getComponentId<SkyboxComponent>()) {
        SkyboxComponent& sky = scene->getComponent<SkyboxComponent>(entity);
        destroySky(entity, sky);
    } else if (componentId == scene->getComponentId<CameraComponent>()) {
        CameraComponent& camera = scene->getComponent<CameraComponent>(entity);
        destroyCamera(camera, true);
        // its shadow maps go with it
        cameraShadows.erase(entity);
    } else if (componentId == scene->getComponentId<MirrorComponent>()) {
        destroyMirrorCamera(entity);

        // reload meshes so the surface drops the projective-mirror shader variant and
        // other meshes drop the now-unused inverted-culling pipeline
        needReloadMeshes();
    } else if (componentId == scene->getComponentId<ReflectionProbeComponent>()) {
        ReflectionProbeComponent& probe = scene->getComponent<ReflectionProbeComponent>(entity);
        destroyReflectionProbe(entity, probe);
        cameraShadows.erase(entity);
        hasReflectionProbes = scene->getComponentArray<ReflectionProbeComponent>()->size() > 1;
        needReloadMeshVariants();
    }
}

#ifdef JASPERENGINE_EDITOR

//-----Editor overlays and secondary views-----

RenderSystem::MirrorView::MirrorView(){
    // the reflection renders into the view's own framebuffer
    delete camera.framebuffer;
    camera.framebuffer = &framebuffer;
    camera.type = CameraType::CAMERA_PERSPECTIVE;
    camera.renderToTexture = true;
    camera.autoResize = false;
    camera.invertCulling = true;
}

RenderSystem::MirrorView::~MirrorView(){
    camera.framebuffer = nullptr;
    framebuffer.destroy();
    for (auto& entry : objects.terrainViews){
        for (int s = 0; s < 2; s++){
            entry.second.nodesbuffer[s].getRender()->destroyBuffer();
        }
    }
}

void RenderSystem::setEditorOverlay(Entity entity, bool overlay){
    if (entity == NULL_ENTITY)
        return;
    if (entity >= editorOverlayEntities.size()){
        if (!overlay)
            return;
        editorOverlayEntities.resize((size_t)entity + 1, 0);
    }
    editorOverlayEntities[entity] = overlay ? 1 : 0;
}

bool RenderSystem::isEditorOverlay(Entity entity) const{
    return entity < editorOverlayEntities.size() && editorOverlayEntities[entity];
}

void RenderSystem::setDrawEditorOverlays(bool draw){
    drawEditorOverlays = draw;
}

bool RenderSystem::passObjectIndex(const Transform& transform, size_t& index) const{
    if (!passObjects || !passObjects->base || &transform < passObjects->base)
        return false;
    index = (size_t)(&transform - passObjects->base);
    return index < passObjects->count;
}

bool RenderSystem::isObjectOverridden(const Transform& transform) const{
    size_t index;
    return passObjectIndex(transform, index) && passObjects->override[index] >= 0;
}

RenderSystem::MirrorView* RenderSystem::passMirror(Entity entity){
    // only the view camera's own pass shows the view's reflections
    RenderView& view = rv();
    if (!view.secondary || view.sharesPrimaryCamera || passObjects != &view.objects)
        return nullptr;
    auto it = view.mirrors.find(entity);
    if (it == view.mirrors.end() || !it->second->ready)
        return nullptr;
    return it->second.get();
}

// Sorts a transparent points batch back to front for the view camera into the view's
// own buffer and binds it for this draw; the component buffer keeps the frame's order.
bool RenderSystem::bindViewPointsOrder(Entity entity, PointsComponent& points, Transform& transform, CameraComponent& camera, Transform& cameraTransform){
    RenderView& view = rv();
    if (!view.secondary || view.sharesPrimaryCamera || passObjects != &view.objects)
        return false;
    if (!points.transparent || !sortsByDistance(camera) || points.numVisible == 0 || points.renderPoints.size() < points.numVisible)
        return false;

    PointsView& pointsView = view.points[entity];
    pointsView.points.assign(points.renderPoints.begin(), points.renderPoints.begin() + points.numVisible);

    const Matrix4& model = objectModel(transform);
    const Vector3 cameraDirection = (cameraTransform.worldPosition - camera.worldTarget).normalize();
    std::sort(pointsView.points.begin(), pointsView.points.end(), [&](const PointRenderData& a, const PointRenderData& b){
        return (model * a.position).dotProduct(cameraDirection) < (model * b.position).dotProduct(cameraDirection);
    });

    const size_t size = pointsView.points.size() * sizeof(PointRenderData);
    BufferRender* render = pointsView.buffer.getRender();
    // grows with headroom and never shrinks
    if (render->isCreated() && pointsView.capacity < size)
        render->destroyBuffer();
    if (!render->isCreated()){
        pointsView.capacity = size + size / 2;
        render->createBuffer(pointsView.capacity, nullptr, BufferType::VERTEX_BUFFER, BufferUsage::STREAM);
    }
    pointsView.buffer.setData((unsigned char*)pointsView.points.data(), size);
    render->updateBuffer(size, pointsView.buffer.getData());

    points.render.replaceVertexBuffer(points.buffer.getRender(), render);
    return true;
}

RenderSystem::RenderView& RenderSystem::getSecondaryView(uint32_t viewId){
    std::unique_ptr<RenderView>& view = secondaryViews[viewId];
    if (!view){
        view = std::make_unique<RenderView>();
        view->secondary = true;
    }
    return *view;
}

void RenderSystem::destroyRenderView(RenderView& view){
    view.atmosphere.destroy();
    view.ssaoDepthFramebuffer.destroy();
    view.ssaoFramebuffer.destroy();
    view.ssaoBlurFramebuffer.destroy();
    view.ssaoWidth = 0;
    view.ssaoHeight = 0;
    view.currentSSAOTexture = nullptr;

    view.gbufferFramebuffer.destroy();
    view.sceneColorFramebuffer.destroy();
    view.ssrFramebuffer.destroy();
    view.ssrBlurFramebuffer.destroy();
    view.ssrWidth = 0;
    view.ssrHeight = 0;

    view.fixedResFramebuffer.destroy();
    view.fixedResWidth = 0;
    view.fixedResHeight = 0;

    for (Framebuffer& framebuffer : view.postProcessFramebuffer)
        framebuffer.destroy();
    view.postScaledTargets.clear();
    view.postProcessWidth = 0;
    view.postProcessHeight = 0;

    view.postSceneFramebuffer.destroy();
    view.postSceneWidth = 0;
    view.postSceneHeight = 0;
    if (frameBlurredScene && view.blurTargets.isCreated())
        frameBlurredScene = nullptr;
    view.bloomTargets.destroy();
    view.blurTargets.destroy();
    view.msaaFramebuffer.destroy();
    view.msaaWidth = 0;
    view.msaaHeight = 0;
    view.msaaSamples = 1;
    view.msaaResolveFramebuffer.destroy();

    releaseSceneInputTargets(view, true);
    view.sceneColorRedirect.destroy();

    for (InstanceView& instanceView : view.instanceViews){
        instanceView.buffer.getRender()->destroyBuffer();
        instanceView.data.clear();
        instanceView.capacity = 0;
        instanceView.needUpload = false;
    }
    view.instanceRanges.clear();
    view.instanceSources.clear();
    view.instancesBuilt = false;

    view.shadows.release();

    for (auto& entry : view.points){
        entry.second.buffer.getRender()->destroyBuffer();
    }
    view.points.clear();

    auto destroyTerrainViews = [](PassObjects& objects){
        for (auto& entry : objects.terrainViews){
            for (int s = 0; s < 2; s++){
                entry.second.nodesbuffer[s].getRender()->destroyBuffer();
            }
        }
        objects.terrainViews.clear();
        objects.terrainCapacity.clear();
    };
    destroyTerrainViews(view.objects);
    view.mirrors.clear();
}

void RenderSystem::releaseView(uint32_t viewId){
    auto it = secondaryViews.find(viewId);
    if (it == secondaryViews.end())
        return;
    if (activeView == it->second.get())
        activeView = &primaryView;
    destroyRenderView(*it->second);
    secondaryViews.erase(it);
}

// A removed component takes its per-view data along; a removed Transform ends the
// entity's overlay mark, since destroyed entities reach every system that way.
void RenderSystem::forgetEntityInViews(Entity entity, ComponentId componentId){
    if (componentId == scene->getComponentId<Transform>()){
        setEditorOverlay(entity, false);
    }

    const bool instances = componentId == scene->getComponentId<InstancedMeshComponent>() || componentId == scene->getComponentId<MeshComponent>();
    const bool terrain = componentId == scene->getComponentId<TerrainComponent>() || componentId == scene->getComponentId<MeshComponent>();
    const bool points = componentId == scene->getComponentId<PointsComponent>();
    const bool mirror = componentId == scene->getComponentId<MirrorComponent>() || componentId == scene->getComponentId<MeshComponent>();
    if (!instances && !terrain && !points && !mirror)
        return;

    auto forgetTerrain = [entity](PassObjects& objects){
        auto it = objects.terrainViews.find(entity);
        if (it != objects.terrainViews.end()){
            for (int s = 0; s < 2; s++){
                it->second.nodesbuffer[s].getRender()->destroyBuffer();
            }
            objects.terrainViews.erase(it);
        }
        objects.terrainCapacity.erase(entity);
    };

    for (auto& entry : secondaryViews){
        RenderView& view = *entry.second;
        if (instances){
            view.instanceRanges.erase(entity);
            view.instanceSources.erase(entity);
            view.instancesBuilt = false;
        }
        if (terrain){
            forgetTerrain(view.objects);
            for (auto& mirrorEntry : view.mirrors){
                forgetTerrain(mirrorEntry.second->objects);
            }
        }
        if (points){
            auto it = view.points.find(entity);
            if (it != view.points.end()){
                it->second.buffer.getRender()->destroyBuffer();
                view.points.erase(it);
            }
        }
        if (mirror){
            view.mirrors.erase(entity);
        }
    }
}

// Model-view-projection, distance and billboard facing of every object for a camera
// pass of a secondary view. A real billboard turns to this camera: it and its children
// get their own model matrices, the frame's Transforms stay as they are.
void RenderSystem::buildPassObjects(PassObjects& objects, CameraComponent& camera, Transform& cameraTransform){
    auto transforms = scene->getComponentArray<Transform>();
    const size_t count = transforms->size();

    objects.base = (count > 0) ? &transforms->getComponentFromIndex(0) : nullptr;
    objects.count = count;
    objects.mvp.resize(count);
    objects.distance.resize(count);
    objects.override.assign(count, -1);
    objects.overrideModel.clear();
    objects.overrideNormal.clear();
    objects.scissors.assign(count, PassScissorState());

    // world rotation of the overridden entries, which their children compose with
    static std::vector<Quaternion> overrideRotation;
    overrideRotation.clear();

    const bool flipped = isRenderingFlipped(camera);
    static const Matrix4 flipYMatrix = Matrix4::scaleMatrix(Vector3(1, -1, 1));
    const float EPSILON = 1e-6f;

    for (size_t i = 0; i < count; i++){
        Transform& transform = transforms->getComponentFromIndex(i);
        Entity entity = transforms->getEntity(i);

        // parents come first in the array, so an overridden parent is known here
        int parentOverride = -1;
        Transform* parent = nullptr;
        if (transform.parent != NULL_ENTITY){
            parent = scene->findComponent<Transform>(transform.parent);
            if (parent && !objects.overrideModel.empty()){
                size_t parentIndex;
                if (passObjectIndex(*parent, parentIndex))
                    parentOverride = objects.override[parentIndex];
            }
        }

        const Matrix4* model = &transform.modelMatrix;

        if (transform.billboard && !transform.fakeBillboard){
            const Matrix4 parentModel = (parentOverride >= 0) ? objects.overrideModel[parentOverride] : (parent ? parent->modelMatrix : Matrix4());
            const Quaternion parentRotation = (parentOverride >= 0) ? overrideRotation[parentOverride] : (parent ? parent->worldRotation : Quaternion());
            RectTransformComponent* layout = scene->findComponent<RectTransformComponent>(entity);

            // where it stands before turning, as updateTransform placed it
            Vector3 worldPosition = transform.worldPosition;
            if (parentOverride >= 0){
                Vector3 origin = transform.position;
                if (layout && layout->pivot != Vector2(0, 0)){
                    origin += getUIPivotShift(*layout, transform.rotation, transform.scale);
                }
                worldPosition = parentModel * origin;
            }

            Vector3 camPos = cameraTransform.worldPosition;
            if (transform.cylindricalBillboard)
                camPos.y = worldPosition.y;

            Vector3 toCamera = camPos - worldPosition;
            if (toCamera.length() > EPSILON && toCamera.crossProduct(camera.worldUp).length() > EPSILON){
                Matrix4 facing = Matrix4::lookAtMatrix(camPos, worldPosition, camera.worldUp).inverse();
                if (facing.isValid()){
                    Quaternion worldRotation;
                    worldRotation.fromRotationMatrix(facing);
                    worldRotation = worldRotation * transform.billboardRotation;
                    Quaternion localRotation = parent ? (parentRotation.inverse() * worldRotation) : worldRotation;

                    Vector3 origin = transform.position;
                    if (layout && layout->pivot != Vector2(0, 0)){
                        origin += getUIPivotShift(*layout, localRotation, transform.scale);
                    }
                    Matrix4 localMatrix = Matrix4::translateMatrix(origin) * localRotation.getRotationMatrix() * Matrix4::scaleMatrix(transform.scale);

                    objects.override[i] = (int)objects.overrideModel.size();
                    objects.overrideModel.push_back(parent ? parentModel * localMatrix : localMatrix);
                    objects.overrideNormal.push_back(objects.overrideModel.back().inverse().transpose());
                    overrideRotation.push_back(parent ? parentRotation * localRotation : localRotation);
                    model = &objects.overrideModel.back();
                }
            }
        }

        if (objects.override[i] < 0 && parentOverride >= 0){
            // follows its turned parent
            objects.override[i] = (int)objects.overrideModel.size();
            objects.overrideModel.push_back(objects.overrideModel[parentOverride] * transform.localMatrix);
            objects.overrideNormal.push_back(objects.overrideModel.back().inverse().transpose());
            overrideRotation.push_back(overrideRotation[parentOverride] * transform.rotation);
            model = &objects.overrideModel.back();
        }

        const Vector3 worldPosition = (objects.override[i] >= 0) ? (*model * Vector3(0, 0, 0)) : transform.worldPosition;

        Matrix4 mvp;
        if (transform.billboard && transform.fakeBillboard){
            Matrix4 modelViewMatrix = camera.viewMatrix * (*model);

            modelViewMatrix.set(0, 0, transform.worldScale.x);
            modelViewMatrix.set(0, 1, 0.0);
            modelViewMatrix.set(0, 2, 0.0);

            if (!transform.cylindricalBillboard) {
                modelViewMatrix.set(1, 0, 0.0);
                modelViewMatrix.set(1, 1, transform.worldScale.y);
                modelViewMatrix.set(1, 2, 0.0);
            }

            modelViewMatrix.set(2, 0, 0.0);
            modelViewMatrix.set(2, 1, 0.0);
            modelViewMatrix.set(2, 2, transform.worldScale.z);

            mvp = camera.projectionMatrix * modelViewMatrix;
        }else{
            mvp = camera.viewProjectionMatrix * (*model);
        }
        if (flipped){
            mvp = flipYMatrix * mvp;
        }

        objects.mvp[i] = mvp;
        objects.distance[i] = (cameraTransform.worldPosition - worldPosition).length();
    }

    auto skys = scene->getComponentArray<SkyboxComponent>();
    if (skys->size() > 0){
        objects.skyViewProjection = skyViewProjection(skys->getComponentFromIndex(0), camera);
    }
}

// The camera's own CDLOD cut of every terrain, into buffers the pass keeps
void RenderSystem::selectPassTerrain(PassObjects& objects, CameraComponent& camera, Transform& cameraTransform){
    auto terrains = scene->getComponentArray<TerrainComponent>();
    for (int i = 0; i < terrains->size(); i++){
        TerrainComponent& terrain = terrains->getComponentFromIndex(i);
        Entity entity = terrains->getEntity(i);
        Transform* transform = scene->findComponent<Transform>(entity);
        MeshComponent* mesh = scene->findComponent<MeshComponent>(entity);
        if (!transform || !mesh || !mesh->loaded || !terrain.heightMapLoaded)
            continue;

        TerrainView& view = objects.terrainViews[entity];
        size_t& capacity = objects.terrainCapacity[entity];

        for (int s = 0; s < 2; s++){
            InterleavedBuffer& buffer = view.nodesbuffer[s];
            // the layout of the frame's node buffers (MeshSystem::createTerrain)
            if (buffer.getAttributes().empty()){
                buffer.addAttribute(AttributeType::TERRAINNODEPOSITION, 2, true);
                buffer.addAttribute(AttributeType::TERRAINNODESIZE, 1, true);
                buffer.addAttribute(AttributeType::TERRAINNODERANGE, 1, true);
                buffer.addAttribute(AttributeType::TERRAINNODERESOLUTION, 1, true);
                buffer.setRenderAttributes(true);
                buffer.setInstanceBuffer(true);
                buffer.setUsage(BufferUsage::STREAM);
            }
        }

        // every node can be selected once at most, like the frame's own buffers
        const size_t needed = std::max<size_t>(terrain.nodes.size(), 1) * view.nodesbuffer[0].getStride();
        if (capacity < needed || !view.nodesbuffer[0].getRender()->isCreated()){
            for (int s = 0; s < 2; s++){
                InterleavedBuffer& buffer = view.nodesbuffer[s];
                buffer.getRender()->destroyBuffer();
                buffer.getRender()->createBuffer(needed, nullptr, buffer.getType(), buffer.getUsage());
            }
            capacity = needed;
        }

        updateTerrain(terrain, *transform, camera, cameraTransform, view);
    }
}

// The instances of a batch as seen from camera, when they depend on it: billboards face
// it and the distance fade keeps those in range of it (updateInstancedMesh's rules).
void RenderSystem::buildInstanceList(const InstancedMeshComponent& instmesh, const MeshComponent& mesh, const Transform& transform, const CameraComponent& camera, const Vector3& fadeEyeLocal, ViewInstanceSource& source){
    source.instances.clear();
    source.bounds.clear();

    const bool hasBounds = !mesh.verticesAABB.isNull() && !mesh.verticesAABB.isInfinite();
    const Vector3 localCenter = hasBounds ? mesh.verticesAABB.getCenter() : Vector3::ZERO;
    const float localRadius = hasBounds ? mesh.verticesAABB.getHalfSize().length() : 0.0f;

    const float cullEnd = instanceFadeCullEnd(instmesh, mesh, *scene);
    const float cullRadius = cullEnd + std::max(0.0001f, cullEnd * 0.00001f);
    const float cullRadiusSquared = cullRadius * cullRadius;

    Quaternion billboardRotation;
    if (instmesh.instancedBillboard){
        billboardRotation.fromRotationMatrix(camera.viewMatrix.inverse());
        billboardRotation = transform.worldRotation.inverse() * billboardRotation;
    }

    AABB localAABB;
    localAABB.setNull();

    const size_t instancesSize = std::min(instmesh.instances.size(), (size_t)instmesh.maxInstances);
    source.instances.reserve(instancesSize);
    source.bounds.reserve(instancesSize);
    for (size_t i = 0; i < instancesSize; i++){
        const InstanceData& instance = instmesh.instances[i];
        if (!instance.visible)
            continue;

        // model-local XZ distance from the instance origin, as in mesh.vert
        const float dx = instance.position.x - fadeEyeLocal.x;
        const float dz = instance.position.z - fadeEyeLocal.z;
        if (cullEnd >= 0.0f && dx * dx + dz * dz > cullRadiusSquared)
            continue;

        Matrix4 rotationMatrix = instmesh.instancedBillboard ? billboardRotation.getRotationMatrix() : instance.rotation.getRotationMatrix();
        Matrix4 instanceMatrix = Matrix4::translateMatrix(instance.position) * rotationMatrix * Matrix4::scaleMatrix(instance.scale);

        InstanceRenderData renderData{};
        renderData.instanceMatrix = instanceMatrix;
        renderData.color = instance.color;
        renderData.textureRect = instance.textureRect;
        source.instances.push_back(renderData);

        InstanceBounds bounds;
        bounds.scale = std::max(std::abs(instance.scale.x), std::max(std::abs(instance.scale.y), std::abs(instance.scale.z)));
        bounds.center = instanceMatrix * localCenter;
        bounds.radius = localRadius * bounds.scale;
        source.bounds.push_back(bounds);

        localAABB.merge(instanceMatrix * mesh.verticesAABB);
    }

    if (localAABB.isNull()){
        source.worldAABB.setNull();
    }else{
        source.worldAABB = transform.modelMatrix * localAABB;
    }
}

// The view camera's culled slice of every instanced batch and of its shadow slots, in
// the view's own buffers. Rebuilt when the frame's lists, the view camera or its shadow
// cameras changed, so a still view uploads nothing.
void RenderSystem::buildViewInstances(RenderView& view, CameraComponent& camera, Transform& cameraTransform){
    auto instmeshes = scene->getComponentArray<InstancedMeshComponent>();
    if (instmeshes->size() == 0){
        view.instanceRanges.clear();
        view.instanceSources.clear();
        view.instancesBuilt = false;
        return;
    }

    collectShadowSlots();

    // what the lists depend on besides the batches themselves
    uint64_t shadowHash = 1469598103934665603ULL;
    for (const ShadowSlot& shadow : shadowSlots){
        const LightCamera& shadowCamera = lightCamera(shadow.lightIndex, *shadow.light, shadow.cameraIndex);
        const unsigned char* bytes = reinterpret_cast<const unsigned char*>(&shadowCamera.lightViewProjectionMatrix);
        for (size_t b = 0; b < sizeof(Matrix4); b++){
            shadowHash = (shadowHash ^ bytes[b]) * 1099511628211ULL;
        }
        shadowHash = (shadowHash ^ (uint64_t)(shadow.slot + 1)) * 1099511628211ULL;
    }
    const LodView& lodView = view.lodView;
    const bool lodChanged = lodView.enabled != view.instanceLodView.enabled || lodView.threshold != view.instanceLodView.threshold ||
        lodView.origin != view.instanceLodView.origin || lodView.perUnit != view.instanceLodView.perUnit || lodView.constant != view.instanceLodView.constant;

    if (view.instancesBuilt && view.instanceRevision == instanceRevision && view.instanceShadowHash == shadowHash && !lodChanged &&
            view.instanceViewProjection == camera.viewProjectionMatrix && view.instanceEye == cameraTransform.worldPosition){
        return;
    }
    view.instancesBuilt = true;
    view.instanceRevision = instanceRevision;
    view.instanceShadowHash = shadowHash;
    view.instanceLodView = lodView;
    view.instanceViewProjection = camera.viewProjectionMatrix;
    view.instanceEye = cameraTransform.worldPosition;

    for (InstanceView& instanceView : view.instanceViews){
        instanceView.data.clear();
    }

    const Vector3& eye = cameraTransform.worldPosition;
    const Vector3 towardCamera = (cameraTransform.worldPosition - camera.worldTarget).normalize();

    for (int i = 0; i < instmeshes->size(); i++){
        InstancedMeshComponent& instmesh = instmeshes->getComponentFromIndex(i);
        Entity entity = instmeshes->getEntity(i);
        MeshComponent* mesh = scene->findComponent<MeshComponent>(entity);
        Transform* transform = scene->findComponent<Transform>(entity);

        std::array<InstanceViewRange, 1 + MAX_SHADOW_ATLAS_SLOTS>& ranges = view.instanceRanges[entity];
        ranges.fill(InstanceViewRange());
        if (!mesh || !transform || !mesh->loaded){
            view.instanceSources.erase(entity);
            continue;
        }

        ViewInstanceSource& source = view.instanceSources[entity];
        const Matrix4 inverseModel = transform->modelMatrix.inverse();
        source.fadeEyeLocal = inverseModel.isValid() ? (inverseModel * eye) : eye;

        const InstanceRenderData* instances;
        const InstanceBounds* bounds;
        unsigned int count;
        source.own = instmesh.instancedBillboard || instanceFadeCullEnd(instmesh, *mesh, *scene) >= 0.0f;
        if (source.own){
            buildInstanceList(instmesh, *mesh, *transform, camera, source.fadeEyeLocal, source);
            instances = source.instances.data();
            bounds = source.bounds.data();
            count = (unsigned int)source.instances.size();
        }else{
            source.instances.clear();
            source.bounds.clear();
            instances = instmesh.renderInstances.data();
            bounds = instmesh.renderBounds.data();
            count = (instmesh.renderBounds.size() < instmesh.numVisible) ? 0 : instmesh.numVisible;
        }

        // blended batches go back to front from this camera, 2D keeps its layering
        InstanceOrder order = InstanceOrder::NEAREST;
        Vector3 direction = Vector3::ZERO;
        if (!sortsByDistance(camera)){
            order = InstanceOrder::KEEP;
        }else if (mesh->transparent){
            order = InstanceOrder::BACK_TO_FRONT;
            direction = towardCamera;
        }
        buildInstanceView(view.instanceViews[0], ranges[0], instances, bounds, count, instmesh, *mesh, *transform,
            camera.farClip, camera.frustumPlanes, view.lodView, eye, eye, direction, order);

        for (const ShadowSlot& shadow : shadowSlots){
            if (!mesh->castShadows)
                continue;
            const LightCamera& shadowCamera = lightCamera(shadow.lightIndex, *shadow.light, shadow.cameraIndex);
            LodView shadowLod = shadowLodView(*shadow.light, shadow.lightIndex, shadow.cameraIndex, shadow.slot);
            Vector3 lightDirection = (shadow.light->type == LightType::DIRECTIONAL) ? shadow.light->worldDirection : Vector3::ZERO;
            buildInstanceView(view.instanceViews[1 + shadow.slot], ranges[1 + shadow.slot], instances, bounds, count, instmesh, *mesh, *transform,
                shadowCamera.nearFar.y, shadowCamera.frustumPlanes, shadowLod, eye, shadowLod.origin, lightDirection, InstanceOrder::NEAREST);
        }
    }

    for (InstanceView& instanceView : view.instanceViews){
        instanceView.needUpload = !instanceView.data.empty();
    }
    uploadInstanceViews(view);
}

// Each mirror's reflection as the view camera sees it (updateMirrors' reflection for
// the view camera), rendered into the view's own framebuffer for the mirror surface.
void RenderSystem::renderViewMirrors(RenderView& view, CameraComponent& camera){
    auto mirrors = scene->getComponentArray<MirrorComponent>();
    if (mirrors->size() == 0){
        view.mirrors.clear();
        return;
    }

    const Matrix4 mainView = camera.viewMatrix;
    const Vector3 mainEye = mainView.inverse() * Vector3(0.0f, 0.0f, 0.0f);

    PassObjects* viewObjects = passObjects;

    for (int i = 0; i < mirrors->size(); i++){
        Entity entity = mirrors->getEntity(i);
        MirrorComponent& mirror = mirrors->getComponentFromIndex(i);
        Transform* mirrorTransform = scene->findComponent<Transform>(entity);
        MeshComponent* mesh = scene->findComponent<MeshComponent>(entity);
        if (!mirrorTransform || !mesh || mesh->numSubmeshes == 0 || !mesh->loaded){
            view.mirrors.erase(entity);
            continue;
        }
        if (!mirrorTransform->visible || !isRenderActive(entity))
            continue;

        std::unique_ptr<MirrorView>& mirrorViewPtr = view.mirrors[entity];
        if (!mirrorViewPtr)
            mirrorViewPtr = std::make_unique<MirrorView>();
        MirrorView& mirrorView = *mirrorViewPtr;

        // the reflection is sampled across the view, so it follows the view's size
        const unsigned int width = (unsigned int)std::max(1.0f, view.viewRect.getWidth());
        const unsigned int height = (unsigned int)std::max(1.0f, view.viewRect.getHeight());
        if (!mirrorView.framebuffer.isCreated() || mirrorView.framebuffer.getWidth() != width || mirrorView.framebuffer.getHeight() != height){
            mirrorView.framebuffer.destroy();
            mirrorView.framebuffer.setWidth(width);
            mirrorView.framebuffer.setHeight(height);
            mirrorView.framebuffer.create();
            mirrorView.ready = false;
        }
        if (!mirrorView.framebuffer.isCreated())
            continue;

        CameraComponent& refCam = mirrorView.camera;
        Transform& refCamTransform = mirrorView.transform;

        Vector3 worldNormal = (mirrorTransform->worldRotation * mirror.normal).normalize();
        Plane plane(worldNormal, mirrorTransform->worldPosition);

        Matrix4 reflectMat = Matrix4::reflectMatrix(plane);
        refCam.customViewMatrix = mainView * reflectMat;
        refCam.hasCustomViewMatrix = true;
        refCamTransform.position = reflectMat * mainEye;
        refCamTransform.worldPosition = refCamTransform.position;
        refCam.invertCulling = true;
        refCam.renderToTexture = true;

        refCam.type = camera.type;
        refCam.yfov = camera.yfov;
        refCam.aspect = camera.aspect;
        refCam.nearClip = camera.nearClip;
        refCam.farClip = camera.farClip;
        refCam.leftClip = camera.leftClip;
        refCam.rightClip = camera.rightClip;
        refCam.bottomClip = camera.bottomClip;
        refCam.topClip = camera.topClip;
        // a mirror reflects the layers its viewer sees
        refCam.cullingMask = camera.cullingMask;

        updateCamera(refCam, refCamTransform);

        // oblique near plane on the mirror (see updateMirrors)
        Vector4 worldClipPlane(worldNormal.x, worldNormal.y, worldNormal.z, plane.d);
        Vector4 viewClipPlane = refCam.viewMatrix.inverse().transpose() * worldClipPlane;
        Matrix4 obliqueProjection = refCam.projectionMatrix.obliqueNearClip(viewClipPlane);
        refCam.viewProjectionMatrix = obliqueProjection * refCam.viewMatrix;
        updateCameraFrustumPlanes(refCam.viewProjectionMatrix, refCam.frustumPlanes);
        mirrorView.viewProjection = refCam.viewProjectionMatrix;

        passObjects = &mirrorView.objects;
        buildPassObjects(mirrorView.objects, refCam, refCamTransform);
        selectPassTerrain(mirrorView.objects, refCam, refCamTransform);

        // the reflection camera's lighting and shadow maps (the mirror itself still casts)
        prepareCameraShadows(mirrorView.shadows, refCam, refCamTransform, TERRAIN_PASS_VIEW, true);
        buildCameraShadowInstances(mirrorView.shadows);
        updatePassFrame(refCam, refCamTransform);
        renderCameraShadows(mirrorView.shadows);

        passExcludedEntity = entity;

        CameraPass pass;
        pass.camera = &refCam;
        pass.cameraTransform = &refCamTransform;
        pass.isMain = false;
        pass.renderToTexture = true;
        pass.terrainView = TERRAIN_PASS_VIEW;
        CameraShadows* viewShadows = passShadows;
        passShadows = &mirrorView.shadows;
        renderCameraPass(pass);
        passShadows = viewShadows;

        passExcludedEntity = NULL_ENTITY;
        passObjects = viewObjects;
        mirrorView.ready = true;
    }

    // mirrors that went away
    for (auto it = view.mirrors.begin(); it != view.mirrors.end();){
        if (!scene->findComponent<MirrorComponent>(it->first)){
            it = view.mirrors.erase(it);
        }else{
            ++it;
        }
    }
}

void RenderSystem::drawView(uint32_t viewId, const ViewDesc& desc){
    if (!Engine::isViewLoaded() || !desc.target)
        return;
    if (desc.viewRect.getWidth() <= 0.0f || desc.viewRect.getHeight() <= 0.0f)
        return;

    CameraComponent* sceneCamera = scene->findComponent<CameraComponent>(desc.camera);
    Transform* cameraTransformPtr = scene->findComponent<Transform>(desc.camera);
    if (!sceneCamera || !cameraTransformPtr)
        return;
    Transform& cameraTransform = *cameraTransformPtr;

    // pipelines are baked at load: a camera needing a new one reloads the objects on
    // the next update, and until then they skip its pass
    viewPipelines |= colorPipeline(*sceneCamera, true);

    // documents of a view without editor overlays (the editor's Game view) render at the
    // view's size; the editor commits a sokol frame per view, so a Scene view frame that
    // skipped them does not keep them from this one
    if (!desc.editorOverlays)
        scene->getSystem<UIDocumentSystem>()->renderOffscreen(desc.viewRect);

    // a scene drawn only through views does the frame's shared work here: uploads,
    // 2D shadows, probe faces and its render-to-texture cameras
    if (!frameDrawn){
        drawFramePasses();
        drawCameras(false);
        frameDrawn = true;
    }

    const bool frameFlipped = isRenderingFlipped(*sceneCamera);

    RenderView& view = getSecondaryView(viewId);
    view.target = desc.target;
    view.viewRect = desc.viewRect;
    view.baseLayer = desc.baseLayer;
    view.editorOverlays = desc.editorOverlays;
    view.fixedResolution = desc.fixedResolution;
    view.disableFog = desc.disableFog;
    view.disableFaceCulling = desc.disableFaceCulling;
    view.postProcessing = desc.postProcessing;
    view.disableClouds = desc.disableClouds;
    view.atmosphereQuality = desc.atmosphereQuality;

    activeView = &view;

    // the view's fixed-resolution target goes with the setting, like the frame's
    if (!isFixedResolutionActive() && view.fixedResFramebuffer.isCreated()){
        view.fixedResFramebuffer.destroy();
        view.fixedResWidth = 0;
        view.fixedResHeight = 0;
    }

    // the view's own copy of the camera, sized for the view's canvas
    view.camera = *sceneCamera;
    CameraComponent& camera = view.camera;
    if (desc.canvasWidth > 0.0f && desc.canvasHeight > 0.0f && camera.autoResize && !camera.renderToTexture){
        camera.leftClip = 0.0f;
        camera.bottomClip = 0.0f;
        camera.rightClip = desc.canvasWidth;
        camera.topClip = desc.canvasHeight;
        camera.aspect = desc.canvasWidth / desc.canvasHeight;
    }
    updateCamera(camera, cameraTransform);

    // through the frame's own camera, everything that depends only on the camera is the
    // frame's: culled instances, terrain cut, shadow cascades and maps, mirrors
    view.sharesPrimaryCamera = desc.camera == scene->getCamera() &&
        camera.viewProjectionMatrix == sceneCamera->viewProjectionMatrix &&
        frameFlipped == isRenderingFlipped(camera);

    view.lodView = computeLodView(camera, cameraTransform);

    passObjects = &view.objects;
    buildPassObjects(view.objects, camera, cameraTransform);

    CameraShadows& viewShadows = view.shadows;
    if (view.sharesPrimaryCamera){
        // the frame's lighting and maps, with this view's viewport
        const CameraShadows& frameShadows = primaryView.shadows;
        viewShadows.fs_lighting = frameShadows.fs_lighting;
        viewShadows.vs_shadows = frameShadows.vs_shadows;
        viewShadows.fs_shadows = frameShadows.fs_shadows;
        viewShadows.fs_point_shadows = frameShadows.fs_point_shadows;
        viewShadows.fs_lighting.viewportInfo = lightingViewportInfo(camera);
        viewShadows.ownLightCameras = false;
        viewShadows.ownShadowInstances = false;
        viewShadows.cameraMask = frameShadows.cameraMask;
        viewShadows.projectiveAtlas = nullptr;
        viewShadows.pointAtlas = nullptr;
        viewShadows.drawProjective = false;
        viewShadows.drawPoint = false;
        viewShadows.releaseProjective();
        viewShadows.releasePoint();
        if (!view.instanceRanges.empty() || !view.instanceSources.empty()){
            view.instanceRanges.clear();
            view.instanceSources.clear();
            view.instancesBuilt = false;
        }
        view.mirrors.clear();
    }else{
        // the view's shadow passes draw its terrain cut and cull its instance lists
        prepareCameraShadows(viewShadows, camera, cameraTransform, TERRAIN_PASS_VIEW, false);
        buildViewInstances(view, camera, cameraTransform);
        selectPassTerrain(view.objects, camera, cameraTransform);
        updatePassFrame(camera, cameraTransform);
        renderCameraShadows(viewShadows);
        renderViewMirrors(view, camera);
    }

    if (!view.target->isCreated()){
        view.target->create();
    }

    CameraPass pass;
    pass.camera = &camera;
    pass.cameraTransform = &cameraTransform;
    pass.isMain = true;
    pass.renderToTexture = false;
    pass.terrainView = view.sharesPrimaryCamera ? 0 : TERRAIN_PASS_VIEW;
    renderCameraPass(pass);
    atmosphere.endFrame();

    passObjects = nullptr;
    passExcludedEntity = NULL_ENTITY;
    passDrawsOverlays = true;
    activeView = &primaryView;
}

#endif
