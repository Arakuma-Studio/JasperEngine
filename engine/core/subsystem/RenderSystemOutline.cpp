// (c) JasperDev and contributors
// SPDX-License-Identifier: MIT

// The Outline component (docs/specs/S94-rendering-anim-gaps.md F22, F23, F-PB2), QuickOutline's
// technique: after a camera pass's opaque queue (a 2D pass: after its hierarchy), every outlined
// submesh first writes stencil bit 0x01 over its silhouette with colour off (the mask), then every
// one draws library:outline_stencil's hull, pushed out along the smoothed normals, where the bit
// is not set (the fill). All masks go before all fills, so no outline covers another outlined
// object. The mode only picks the depth tests and the fill's width:
//
//   mode                    mask depth   fill depth   fill width
//   Outline All             always       always       width
//   Outline Visible         always       less-equal   width
//   Outline Hidden          always       greater      width
//   Outline And Silhouette  less-equal   always       width
//   Silhouette Only         less-equal   greater      0
//
// The bit is the one the Outline (stencil) material preset marks, so both kinds respect each
// other. One render per outlined submesh, on the dynamic pipeline path: the two draws and every
// mode share it; its load and draw live in RenderSystem.cpp beside the extra pass they follow.
// No enabled Outline: nothing gathered, no draw, no allocation.

#include "RenderSystem.h"

#include "Scene.h"
#include "component/CameraComponent.h"
#include "component/InstancedMeshComponent.h"
#include "component/MeshComponent.h"
#include "component/OutlineComponent.h"
#include "component/TerrainComponent.h"
#include "component/TilemapComponent.h"
#include "component/Transform.h"
#include "pool/ShaderPool.h"
#include "render/SystemRender.h"
#include "shader/ShaderManifest.h"

using namespace jasperengine;

namespace {

    // the stencil bit the masks write and the fills test (MaterialPresets' outline mark)
    constexpr uint8_t OUTLINE_STENCIL_BIT = 0x01;
    // an outlined mesh's renders are let go after this many frames without an enabled Outline
    constexpr uint64_t OUTLINE_RENDER_KEEP_FRAMES = 2;

    uint64_t outlineRenderKey(Entity entity, unsigned int submesh){
        return ((uint64_t)entity << 32) | (uint64_t)submesh;
    }

    struct OutlineDepthTests{
        CompareFunc mask;
        CompareFunc fill;
        bool fillWidth;
    };

    OutlineDepthTests outlineDepthTests(OutlineMode mode){
        switch (mode){
            case OutlineMode::OUTLINE_VISIBLE:        return {CompareFunc::ALWAYS, CompareFunc::LESS_EQUAL, true};
            case OutlineMode::OUTLINE_HIDDEN:         return {CompareFunc::ALWAYS, CompareFunc::GREATER, true};
            case OutlineMode::OUTLINE_AND_SILHOUETTE: return {CompareFunc::LESS_EQUAL, CompareFunc::ALWAYS, true};
            case OutlineMode::SILHOUETTE_ONLY:        return {CompareFunc::LESS_EQUAL, CompareFunc::GREATER, false};
            default:                                  return {CompareFunc::ALWAYS, CompareFunc::ALWAYS, true};
        }
    }

}

const OutlineComponent* RenderSystem::findOutline(EntityRegistry* registry, Entity entity){
    if (!registry || !registry->isComponentRegistered<OutlineComponent>())
        return nullptr;
    Entity current = entity;
    while (current != NULL_ENTITY){
        const OutlineComponent* outline = registry->findComponent<OutlineComponent>(current);
        if (outline && outline->enabled && (current == entity || outline->includeChildren))
            return outline;
        const Transform* transform = registry->findComponent<Transform>(current);
        current = transform ? transform->parent : NULL_ENTITY;
    }
    return nullptr;
}

MaterialPass RenderSystem::getOutlinePass(OutlineWidthUnits units){
    MaterialPass pass;
    pass.shader = "library:outline_stencil";
    pass.shaderFeatures = {units == OutlineWidthUnits::WORLD ? "WIDTH_MODE_WORLD" : "WIDTH_MODE_SCREEN"};
    pass.blendMode = MaterialBlendMode::ALPHA;
    return pass;
}

std::string RenderSystem::getOutlineReference(OutlineWidthUnits units){
    const MaterialPass pass = getOutlinePass(units);
    return ShaderReference::getColorVariantReference(pass.shader, pass.shaderFeatures, pass.blendMode);
}

bool RenderSystem::outlinesNeedStencilTarget() const{
    if (SystemRender::hasSwapchainStencil() || !scene->isComponentRegistered<OutlineComponent>())
        return false;
    auto outlines = scene->getComponentArray<OutlineComponent>();
    for (size_t i = 0; i < outlines->size(); i++){
        if (outlines->getComponentFromIndex(i).enabled && scene->isEntityActiveInHierarchy(outlines->getEntity(i)))
            return true;
    }
    return false;
}

void RenderSystem::releaseOutlineRender(OutlineRender& outline){
    SubmeshExtraPass& extra = outline.pass;
    if (extra.loaded)
        extra.render.destroy();
    extra.loaded = false;
    extra.libraryBindings.clear();
    extra.customVSParams.clear();
    extra.customFSParams.clear();
    if (extra.shader){
        extra.shader.reset();
        ShaderPool::remove(ShaderType::MESH, extra.shaderProperties, extra.customShaderId);
    }
}

void RenderSystem::releaseOutlineRenders(Entity entity){
    if (outlineRenders.empty())
        return;
    for (auto it = outlineRenders.begin(); it != outlineRenders.end();){
        if ((Entity)(it->first >> 32) == entity){
            releaseOutlineRender(*it->second);
            it = outlineRenders.erase(it);
        }else{
            ++it;
        }
    }
}

void RenderSystem::releaseAllOutlineRenders(){
    for (auto& entry : outlineRenders)
        releaseOutlineRender(*entry.second);
    outlineRenders.clear();
}

void RenderSystem::drawOutlines(CameraComponent& camera, Transform& cameraTransform, PipelineType colorPip, int passMsaa,
                                uint32_t cullingMask, int instanceView, const LodView* lodView){
    (void)cameraTransform;
    if (!scene->isComponentRegistered<OutlineComponent>())
        return;
    auto outlines = scene->getComponentArray<OutlineComponent>();
    if (outlines->size() == 0){
        if (!outlineRenders.empty())
            releaseAllOutlineRenders();
        return;
    }
#ifdef JASPERENGINE_EDITOR
    // the Scene view's Wireframe and Overdraw show the meshes alone
    if (rv().drawMode == ViewDrawMode::WIREFRAME || rv().drawMode == ViewDrawMode::OVERDRAW)
        return;
#endif

    // the renders of meshes no enabled Outline covered for a while go (once a frame)
    const uint64_t frame = SystemRender::getFrameCount();
    if (frame != outlineSweepFrame){
        outlineSweepFrame = frame;
        for (auto it = outlineRenders.begin(); it != outlineRenders.end();){
            if (it->second->coveredFrame + OUTLINE_RENDER_KEEP_FRAMES < frame){
                releaseOutlineRender(*it->second);
                it = outlineRenders.erase(it);
            }else{
                ++it;
            }
        }
    }

    // the Meshes each enabled Outline covers that this pass shows
    outlineDraws.clear();
    auto transforms = scene->getComponentArray<Transform>();
    const ComponentId meshId = scene->getComponentId<MeshComponent>();
    const ComponentId cameraId = scene->getComponentId<CameraComponent>();
    for (size_t o = 0; o < outlines->size(); o++){
        const OutlineComponent& outline = outlines->getComponentFromIndex(o);
        const Entity owner = outlines->getEntity(o);
        if (!outline.enabled || !scene->isEntityActiveInHierarchy(owner) || !scene->findComponent<Transform>(owner))
            continue;
        const size_t first = transforms->getIndex(owner);
        const size_t last = outline.includeChildren ? scene->findBranchLastIndex(owner) : first;
        for (size_t index = first; index <= last && index < transforms->size(); index++){
            const Entity entity = transforms->getEntity(index);
            const Signature signature = scene->getSignature(entity);
            if (!signature.test(meshId) || signature.test(cameraId))
                continue;
            // a nearer Outline below this one draws it
            if (entity != owner && findOutline(scene, entity) != &outline)
                continue;
            MeshComponent& mesh = scene->getComponent<MeshComponent>(entity);
            if (!mesh.loaded || mesh.needReload)
                continue;
            for (unsigned int s = 0; s < mesh.numSubmeshes; s++){
                auto render = outlineRenders.find(outlineRenderKey(entity, s));
                if (render != outlineRenders.end())
                    render->second->coveredFrame = frame;
            }
            Transform& transform = transforms->getComponentFromIndex(index);
            // a terrain's patches, a tilemap's chunks and a Sprite Batch's instances draw from
            // per-view data an outline cannot follow
            if (!transform.visible || mesh.spriteInstanced || signature.test(scene->getComponentId<TerrainComponent>()) ||
                    signature.test(scene->getComponentId<TilemapComponent>()) || samplesCameraTarget(camera, mesh) ||
                    !isRenderActive(entity, signature) || !isLayerVisible(entity, cullingMask))
                continue;
            InstancedMeshComponent* instmesh = scene->findComponent<InstancedMeshComponent>(entity);
            ensureBatchMVP(entity, transform);
            if (!isBatchVisible(entity, mesh, transform, instmesh, instanceView, camera.farClip, camera.frustumPlanes))
                continue;
            OutlineDraw target;
            target.entity = entity;
            target.mesh = &mesh;
            target.transform = &transform;
            target.instmesh = instmesh;
            target.outline = &outline;
            if (!selectMeshDraw(entity, mesh, transform, instmesh, false, instanceView, lodView, target.draw))
                continue;
            outlineDraws.push_back(target);
        }
    }
    if (outlineDraws.empty())
        return;

    PipelineState mask;
    mask.target = targetFormatFor(colorPip, passMsaa);
    mask.blend = BlendMode::NONE;
    mask.writeMask = ColorWriteMask::NONE;
    mask.depthWrite = false;
    mask.cullBack = false;
    mask.stencil.enabled = true;
    mask.stencil.compare = CompareFunc::ALWAYS;
    mask.stencil.passOp = StencilOp::REPLACE;
    mask.stencil.ref = OUTLINE_STENCIL_BIT;
    mask.stencil.readMask = OUTLINE_STENCIL_BIT;
    mask.stencil.writeMask = OUTLINE_STENCIL_BIT;

    PipelineState fill;
    fill.target = mask.target;
    fill.blend = BlendMode::ALPHA;
    // (the swapchain's colour pipelines never write its alpha)
    fill.writeMask = mask.target.swapchain ? ColorWriteMask::RGB : ColorWriteMask::RGBA;
    fill.depthWrite = false;
    fill.cullBack = false;
    fill.stencil.enabled = true;
    fill.stencil.compare = CompareFunc::NOT_EQUAL;
    fill.stencil.ref = OUTLINE_STENCIL_BIT;
    fill.stencil.readMask = OUTLINE_STENCIL_BIT;
    fill.stencil.writeMask = 0;

    // phase 0 the masks, phase 1 the fills
    for (int phase = 0; phase < 2; phase++){
        for (const OutlineDraw& target : outlineDraws){
            const OutlineComponent& outline = *target.outline;
            const OutlineDepthTests tests = outlineDepthTests(outline.mode);
            PipelineState state = phase == 0 ? mask : fill;
            if (camera.depthTest){
                state.depthCompare = phase == 0 ? tests.mask : tests.fill;
            }else{
                // nothing is ever covered without a depth test: no hidden parts to draw
                if (phase == 1 && tests.fill == CompareFunc::GREATER)
                    continue;
                state.depthCompare = CompareFunc::ALWAYS;
            }
            const float width = (phase == 1 && tests.fillWidth) ? outline.width : 0.0f;

            // the pass's object block; library shaders read the normal matrix whatever the lighting
            Matrix4 objectScratch[3];
            const Matrix4* objectBlock = objectMatrices(*target.transform, objectScratch);
            if (!hasLights){
                if (objectBlock != objectScratch){
                    objectScratch[0] = objectBlock[0];
                    objectScratch[2] = objectBlock[2];
                }
                objectScratch[1] = objectScratch[0].inverse().transpose();
                objectBlock = objectScratch;
            }

            MeshComponent& mesh = *target.mesh;
            for (unsigned int s = 0; s < mesh.numSubmeshes; s++){
                Submesh& submesh = mesh.submeshes[s];
                if (submesh.vertexCount == 0 || submesh.primitiveType != PrimitiveType::TRIANGLES)
                    continue;
                std::unique_ptr<OutlineRender>& render = outlineRenders[outlineRenderKey(target.entity, s)];
                if (!render)
                    render = std::make_unique<OutlineRender>();
                render->coveredFrame = frame;
                // other width units are another variant; a buffer the submesh's render was
                // retargeted to (MeshBufferPool) another binding
                if (render->pass.loaded && (render->units != outline.widthUnits ||
                        render->vertexBufferId != submesh.render.getVertexBufferId() ||
                        render->indexBufferId != submesh.render.getIndexBufferId())){
                    releaseOutlineRender(*render);
                }
                render->units = outline.widthUnits;
                if (!render->pass.loaded && !loadOutlineRender(target.entity, mesh, s, target.instmesh, *render))
                    continue;
                drawOutlineRender(*render, target, s, instanceView, objectBlock, state, width, outline.color);
            }
        }
    }
}
