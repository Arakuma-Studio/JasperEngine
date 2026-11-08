// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// RenderSystem's side of foliage (T5 F1, F4, F5): which list a pass draws, the streams' uploads and
// the draws by vertex-buffer offset. What a view keeps is FoliageSystem's; this file owns the GPU.

#include "profiler/Profile.h"
#include "RenderSystem.h"

#include "Engine.h"
#include "Scene.h"
#include "component/Transform.h"
#include "shader/ShaderManifest.h"
#include "subsystem/FoliageSystem.h"

#include <algorithm>
#include <cmath>
#include <utility>

using namespace jasperengine;

namespace {

    // u_vs_foliage (includes/foliage_wind.glsl)
    struct FoliageUniforms {
        float fadeRange[4];
        float eye[4];
        float wind[4];
        float params[4];
        float heightRange[4];
        float impostor[4];
        float impostorSize[4];
    };
    static_assert(sizeof(FoliageUniforms) == 112);

    // Render-to-texture cameras' lists come after the main view's and its shadow slots'
    constexpr int FOLIAGE_RTT_SLOT = 1 + MAX_SHADOW_ATLAS_SLOTS;

}

FoliageViewList& RenderSystem::foliageList(const void* owner, int slot) {
    if (foliageLastList && foliageLastOwner == owner && foliageLastSlot == slot) return *foliageLastList;
    std::unique_ptr<FoliageViewList>& list = foliageLists[std::pair<const void*, int>(owner, slot)];
    if (!list) list = std::make_unique<FoliageViewList>();
    foliageLastOwner = owner;
    foliageLastSlot = slot;
    foliageLastList = list.get();
    return *list;
}

void RenderSystem::buildFoliageLists(const void* owner, CameraComponent& camera, Transform& cameraTransform, const LodView& lodView, bool main) {
    FoliageSystem* foliage = scene->getSystem<FoliageSystem>().get();
    if (!foliage || !foliage->hasDrawables()) return;

    FoliageViewRequest request;
    request.planes = camera.frustumPlanes;
    request.farClip = camera.farClip;
    request.eye = cameraTransform.worldPosition;
    request.lodEnabled = lodView.enabled;
    request.lodOrigin = lodView.origin;
    request.lodThreshold = lodView.threshold;
    request.lodPerUnit = lodView.perUnit;
    request.lodConstant = lodView.constant;
    request.main = main;
    foliage->buildView(foliageList(owner, 0), request);

    buildFoliageShadowLists(owner, 1, cameraTransform.worldPosition);
}

void RenderSystem::buildFoliageShadowLists(const void* owner, int firstSlot, const Vector3& eye) {
    FoliageSystem* foliage = scene->getSystem<FoliageSystem>().get();
    if (!foliage || !foliage->hasDrawables()) return;

    for (const ShadowSlot& shadow : collectShadowSlots()) {
        const LightCamera& shadowCamera = lightCamera(shadow.lightIndex, *shadow.light, shadow.cameraIndex);
        const LodView shadowLod = shadowLodView(*shadow.light, shadow.lightIndex, shadow.cameraIndex, shadow.slot);

        FoliageViewRequest request;
        request.planes = shadowCamera.frustumPlanes;
        request.farClip = shadowCamera.nearFar.y;
        // bands by the camera's distance, so a cell casts with the LOD it draws
        request.eye = eye;
        request.lodEnabled = shadowLod.enabled;
        request.lodOrigin = shadowLod.origin;
        request.lodThreshold = shadowLod.threshold;
        request.lodPerUnit = shadowLod.perUnit;
        request.lodConstant = shadowLod.constant;
        request.shadow = true;
        foliage->buildView(foliageList(owner, firstSlot + shadow.slot), request);
    }
}

void RenderSystem::buildFoliageCameraList(CameraComponent& camera, Transform& cameraTransform, int terrainView) {
    FoliageSystem* foliage = scene->getSystem<FoliageSystem>().get();
    if (!foliage || !foliage->hasDrawables() || terrainView <= 0) return;

    const LodView lodView = computeLodView(camera, cameraTransform);
    FoliageViewRequest request;
    request.planes = camera.frustumPlanes;
    request.farClip = camera.farClip;
    request.eye = cameraTransform.worldPosition;
    request.lodEnabled = lodView.enabled;
    request.lodOrigin = lodView.origin;
    request.lodThreshold = lodView.threshold;
    request.lodPerUnit = lodView.perUnit;
    request.lodConstant = lodView.constant;
    FoliageViewList& list = foliageList(&primaryView, FOLIAGE_RTT_SLOT + terrainView);
    foliage->buildView(list, request);
    uploadFoliageList(list);
}

void RenderSystem::uploadFoliageList(FoliageViewList& list) {
    if (!list.needUpload) return;
    list.needUpload = false;

    const auto upload = [&](ExternalBuffer& buffer, size_t& capacity, const void* data, size_t size) {
        if (size == 0) return;
        BufferRender* render = buffer.getRender();
        // grows with headroom and never shrinks
        if (render->isCreated() && capacity < size) render->destroyBuffer();
        if (!render->isCreated()) {
            capacity = size + size / 2;
            JE_GPU_ALLOC_SCOPE(Uniforms, "Foliage instances");
            render->createBuffer(static_cast<unsigned int>(capacity), nullptr, BufferType::VERTEX_BUFFER, BufferUsage::STREAM);
        }
        // past the frame's stream staging buffer: tried again next frame
        if (!render->updateBuffer(static_cast<unsigned int>(size), const_cast<void*>(data))) {
            list.needUpload = true;
        } else {
            foliageUploadBytes += size;
        }
    };

    upload(list.buffer, list.capacity, list.stream.data(), list.stream.size() * sizeof(terrain::FoliageInstance));
    upload(list.expandedBuffer, list.expandedCapacity, list.expanded.data(), list.expanded.size() * sizeof(InstanceRenderData));
}

void RenderSystem::uploadFoliageViews() {
    FoliageSystem* foliage = scene->getSystem<FoliageSystem>().get();
    if (!foliage || !foliage->hasDrawables()) return;
    foliageUploadBytes = 0;
    foliage->uploadTreeBuffers();
    for (auto& entry : foliageLists) uploadFoliageList(*entry.second);
    foliage->setLastUploadBytes(foliageUploadBytes);
}

FoliageViewList* RenderSystem::currentFoliageList(int instanceView) {
    const void* owner = &rv();
    int slot = instanceView;
    if (instanceView >= 1 && ps().ownShadowInstances) {
        owner = &ps();
    } else if (instanceView < 0) {
        // a render-to-texture camera's own, past MAX_TERRAIN_VIEWS the main camera's
        owner = &primaryView;
        slot = foliagePassView > 0 ? FOLIAGE_RTT_SLOT + foliagePassView : 0;
    }
#ifdef JASPERENGINE_EDITOR
    else if (rv().secondary && rv().sharesPrimaryCamera) {
        owner = &primaryView;
    }
#endif
    const auto it = foliageLists.find(std::pair<const void*, int>(owner, slot));
    return it != foliageLists.end() ? it->second.get() : nullptr;
}

unsigned int RenderSystem::foliageDrawCount(const InstancedMeshComponent& instmesh, int instanceView) {
    FoliageViewList* list = currentFoliageList(instanceView);
    if (!list || instmesh.foliageSlot >= list->ranges.size()) return 0;
    const std::pair<uint32_t, uint32_t>& range = list->ranges[instmesh.foliageSlot];
    unsigned int total = 0;
    for (uint32_t i = 0; i < range.second; i++) total += list->items[range.first + i].count;
    return total;
}

void RenderSystem::applyFoliageUniforms(ObjectRender& render, int slot, Entity entity, int instanceView) {
    if (slot == -1) return;
    FoliageSystem* foliage = scene->getSystem<FoliageSystem>().get();
    FoliageSystem::PrototypeDraw draw;
    if (!foliage->getPrototypeDraw(entity, draw)) return;
    const TerrainComponent* terrain = scene->findComponent<TerrainComponent>(draw.terrain);
    const Transform* transform = scene->findComponent<Transform>(draw.terrain);
    if (!terrain || !transform) return;

    FoliageUniforms uniforms = {};
    // R17: shadow passes draw the cell's bands with no dither. The range left empty keeps factor 1.
    if (instanceView < 1) {
        uniforms.fadeRange[0] = draw.inStart;
        uniforms.fadeRange[1] = draw.inEnd;
        uniforms.fadeRange[2] = draw.outStart;
        uniforms.fadeRange[3] = draw.outEnd;
    }

    // the shadow passes run before the camera's: a billboard faces the main camera in its shadow too
    const Vector3 eyeWorld = instanceView >= 1 ? fadeEyePosition : foliagePassEye;
    const Matrix4 inverse = transform->modelMatrix.inverse();
    const Vector3 eye = inverse.isValid() ? inverse * eyeWorld : eyeWorld;
    uniforms.eye[0] = eye.x;
    uniforms.eye[1] = eye.y;
    uniforms.eye[2] = eye.z;
    uniforms.eye[3] = frameTime;

    const Vector3 wind = transform->worldRotation.inverse() * draw.wind;
    uniforms.wind[0] = wind.x;
    uniforms.wind[1] = wind.y;
    uniforms.wind[2] = wind.z;
    uniforms.wind[3] = terrain->windSpeed;

    uniforms.params[0] = terrain->windSize > 0.0f ? 1.0f / terrain->windSize : 0.0f;
    uniforms.params[1] = 0.6f;
    uniforms.params[2] = draw.impostor ? 0.0f : 0.2f;
    uniforms.params[3] = draw.colorMask ? 1.0f : 0.0f;

    // the mesh's own bounds: the height where sway starts and reaches its full
    uniforms.heightRange[0] = draw.heightStart;
    uniforms.heightRange[1] = draw.heightRange;
    uniforms.heightRange[2] = draw.impostorBlend ? 1.0f : 0.0f;
    uniforms.heightRange[3] = draw.impostorViews;

    uniforms.impostor[0] = draw.impostorCenter.x;
    uniforms.impostor[1] = draw.impostorCenter.y;
    uniforms.impostor[2] = draw.impostorCenter.z;
    uniforms.impostor[3] = draw.impostorSize.x;
    uniforms.impostorSize[0] = draw.impostorSize.y;

    render.applyUniformBlock(slot, sizeof(uniforms), &uniforms);
}

void RenderSystem::drawFoliageSubmesh(ObjectRender& render, Entity entity, MeshComponent& mesh, unsigned int submeshIndex, InstancedMeshComponent& instmesh, int instanceView) {
    FoliageViewList* list = currentFoliageList(instanceView);
    if (!list || instmesh.foliageSlot >= list->ranges.size()) return;
    const std::pair<uint32_t, uint32_t> range = list->ranges[instmesh.foliageSlot];
    if (range.second == 0) return;

    Submesh& submesh = mesh.submeshes[submeshIndex];
    const int slot = &render == &submesh.depthRender ? submesh.slotVSDepthFoliage
                   : &render == &submesh.gbufferRender ? submesh.slotVSGBufferFoliage
                   : &render == &submesh.render ? submesh.slotVSFoliage : -1;
    applyFoliageUniforms(render, slot, entity, instanceView);

    const MeshLodData* lodData = submesh.lodData.get();
    const bool lodReady = lodData && lodData->numLevels > 1 && submesh.lodIndices.getRender()->isCreated();
    const unsigned int stride = instmesh.buffer.getStride();

    for (uint32_t i = 0; i < range.second; i++) {
        const FoliageDrawItem& item = list->items[range.first + i];
        render.replaceVertexBuffer(instmesh.buffer.getRender(), item.buffer, static_cast<size_t>(item.first) * stride);
        const unsigned int level = lodReady ? std::min(item.level, lodData->numLevels - 1) : 0;
        if (level > 0) {
            render.setIndexBuffer(submesh.lodIndices.getRender());
            render.draw(lodData->levels[level].indexBase, lodData->levels[level].indexCount, item.count);
            render.resetIndexBuffer();
        } else {
            render.draw(0, submesh.vertexCount, item.count);
        }
    }
}

bool RenderSystem::shaderSupportsCompactInstances(const std::string& shader) {
    // the built-in Lit and every library shader share the vertex paths that read compact instances.
    // A project's own fork reads what it reads, so it draws from the expanded stream (R37).
    return shader.empty() || ShaderReference::isLibrary(shader) || ShaderReference::getBase(shader) == ShaderReference::BUILTIN_LIT;
}

bool RenderSystem::shaderSupportsFoliageDither(const std::string& shader) {
    if (shader.empty() || ShaderReference::getBase(shader) == ShaderReference::BUILTIN_LIT) return true;
    const std::string base = ShaderReference::getBase(shader);
    return base == "library:foliage" || base == "library:impostor";
}
