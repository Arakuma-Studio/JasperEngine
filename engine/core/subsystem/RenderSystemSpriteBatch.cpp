// (c) JasperDev and contributors
// SPDX-License-Identifier: MIT

// Sprite Batch (docs/specs/S52-sprite-instancing.md R10-R17, D7, D8): the instances scripts write
// (SpriteBatchState) packed into the Sprite's instanced variant's InstanceRenderData (one matrix
// that maps the Sprite's quad onto the instance's frame, placed by the instance's position,
// rotation and scale; its colour; its frame's texture rect) and drawn with one instanced draw per
// run of one texture page. Packed in update() only when something it depends on changed; uploaded
// by the draw only when the packed revision moved (Retained) or every frame it draws (Clear Each
// Frame); nothing is packed or uploaded for an empty batch. The instances' bounds are the mesh's
// bounds, so culling and picking take the batch whole.

#include "RenderSystem.h"

#include "Scene.h"
#include "component/CameraComponent.h"
#include "component/MeshComponent.h"
#include "component/SpriteBatchComponent.h"
#include "component/SpriteComponent.h"
#include "component/Transform.h"
#include "pixelart/PixelRotation.h"
#include "registry/SortingLayers.h"
#include "render/sprite/SpriteInstanceBatch.h"
#include "sprite/SpriteFrameGeometry.h"
#include "sprite/SpriteSheet.h"
#include "sprite/SpriteSheetPool.h"
#include "sprite/SpriteSheetSources.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <numeric>
#include <cstring>

using namespace jasperengine;

namespace{

    // one frame of the Sprite as an instance draws it: its quad in local units, its texture rect
    // and its page
    struct SpriteBatchFrame{
        float x0 = 0, y0 = 0, x1 = 0, y1 = 0;
        Rect rect = Rect(0.0f, 0.0f, 1.0f, 1.0f);
        uint16_t page = 0;
    };

    // sin and cos of an angle, exact on the 90-degree grid when `exact` (Steps90) or within a
    // rounding of it (D8: an aligned instance carries exact zeros)
    void spriteBatchRotation(float radians, bool steps90, float& c, float& s){
        constexpr float halfPi = 1.57079632679489662f;
        if (steps90){
            const int step = (((int)std::lround(radians / halfPi)) % 4 + 4) % 4;
            static const float cosTable[4] = {1.0f, 0.0f, -1.0f, 0.0f};
            static const float sinTable[4] = {0.0f, 1.0f, 0.0f, -1.0f};
            c = cosTable[step];
            s = sinTable[step];
            return;
        }
        c = std::cos(radians);
        s = std::sin(radians);
        constexpr float epsilon = 1e-6f;
        if (std::fabs(s) < epsilon){
            s = 0.0f;
            c = c < 0.0f ? -1.0f : 1.0f;
        }else if (std::fabs(c) < epsilon){
            c = 0.0f;
            s = s < 0.0f ? -1.0f : 1.0f;
        }
    }

}

SpriteInstanceChunkList* RenderSystem::spriteInstanceChunksOf(Entity entity){
    if (attachedInstanceChunks.empty())
        return nullptr;
    auto attached = attachedInstanceChunks.find(entity);
    return attached != attachedInstanceChunks.end() ? attached->second : nullptr;
}

SpriteInstanceBatch* RenderSystem::spriteInstancesOf(Entity entity){
    if (SpriteInstanceChunkList* chunks = spriteInstanceChunksOf(entity))
        return &chunks->lead;
    if (!attachedInstanceBatches.empty()){
        auto attached = attachedInstanceBatches.find(entity);
        if (attached != attachedInstanceBatches.end())
            return attached->second;
    }
    SpriteBatchComponent* component = scene->findComponent<SpriteBatchComponent>(entity);
    if (!component)
        return nullptr;
    if (!component->state)
        component->state = std::make_shared<SpriteBatchState>();
    if (!component->state->batch)
        component->state->batch = std::make_unique<SpriteInstanceBatch>();
    return component->state->batch.get();
}

void RenderSystem::addSpriteInstanceAttributes(ObjectRender& render, ShaderData& shaderData, SpriteInstanceBatch& batch){
    ExternalBuffer& instances = batch.getInstanceBuffer();
    for (auto const& attr : instances.getAttributes()){
        const int index = shaderData.getAttrIndex(attr.first);
        if (index >= 0)
            render.addAttribute(index, instances.getRender(), attr.second.getElements(), attr.second.getDataType(), instances.getStride(),
                                attr.second.getOffset(), attr.second.getNormalized(), attr.second.getPerInstance());
    }
    for (ExternalBuffer* stream : {batch.gpuHasFx() ? &batch.getFxBuffer() : nullptr, batch.gpuHasPivots() ? &batch.getPivotBuffer() : nullptr}){
        if (!stream)
            continue;
        for (auto const& attr : stream->getAttributes()){
            const int index = shaderData.getAttrIndex(attr.first);
            if (index >= 0)
                render.addAttribute(index, stream->getRender(), attr.second.getElements(), attr.second.getDataType(), stream->getStride(),
                                    attr.second.getOffset(), attr.second.getNormalized(), attr.second.getPerInstance());
        }
    }
}

bool RenderSystem::spriteBatchWantsFx(const MeshComponent& mesh){
    if (mesh.numSubmeshes == 0 || !(mesh.batchAttributes & MeshBatchAttribute_SpriteFx))
        return false;
    const Submesh& submesh = mesh.submeshes[0];
    const std::string& shader = getSubmeshShader(mesh, submesh, scene->getDefaultCustomShader(ShaderType::MESH));
    const std::shared_ptr<const ShaderManifest> manifest = shader.empty() ? nullptr : ShaderManifests::get(shader);
    return manifest && manifest->present && manifest->usesInstanceSlots(ShaderReference::getEnabledFeatures(shader, submesh.material.shaderFeatures));
}

void RenderSystem::attachInstanceBatch(Entity entity, SpriteInstanceBatch* batch){
    if (!batch)
        return detachInstanceBatch(entity);
    attachedInstanceBatches[entity] = batch;
    attachedBoundsRevision.erase(entity);
    if (MeshComponent* mesh = scene->findComponent<MeshComponent>(entity)){
        mesh->spriteInstanced = true;
        mesh->needReload = true;
    }
}

void RenderSystem::detachInstanceBatch(Entity entity){
    auto attached = attachedInstanceBatches.find(entity);
    if (attached == attachedInstanceBatches.end())
        return;
    attached->second->destroyGpu();
    attachedInstanceBatches.erase(attached);
    attachedBoundsRevision.erase(entity);
    if (MeshComponent* mesh = scene->findComponent<MeshComponent>(entity)){
        mesh->spriteInstanced = false;
        mesh->needReload = true;
    }
}

void RenderSystem::attachInstanceChunks(Entity entity, SpriteInstanceChunkList* list){
    if (!list)
        return detachInstanceChunks(entity);
    attachedInstanceChunks[entity] = list;
    attachedBoundsRevision.erase(entity);
    if (MeshComponent* mesh = scene->findComponent<MeshComponent>(entity)){
        mesh->spriteInstanced = true;
        mesh->needReload = true;
    }
}

void RenderSystem::detachInstanceChunks(Entity entity){
    auto attached = attachedInstanceChunks.find(entity);
    if (attached == attachedInstanceChunks.end())
        return;
    attached->second->lead.destroyGpu();
    for (SpriteInstanceChunk& chunk : attached->second->chunks)
        chunk.batch->destroyGpu();
    attachedInstanceChunks.erase(attached);
    attachedBoundsRevision.erase(entity);
    if (MeshComponent* mesh = scene->findComponent<MeshComponent>(entity)){
        mesh->spriteInstanced = false;
        mesh->needReload = true;
    }
}

void RenderSystem::updateAttachedInstanceBatches(){
    for (auto it = attachedInstanceChunks.begin(); it != attachedInstanceChunks.end();){
        const Entity entity = it->first;
        SpriteInstanceChunkList* list = it->second;
        if (!scene->isEntityCreated(entity)){
            list->lead.destroyGpu();
            attachedBoundsRevision.erase(entity);
            it = attachedInstanceChunks.erase(it);
            continue;
        }
        ++it;
        MeshComponent* mesh = scene->findComponent<MeshComponent>(entity);
        if (!mesh)
            continue;
        if (!mesh->spriteInstanced){
            mesh->spriteInstanced = true;
            mesh->needReload = true;
        }
        list->lead.setPivotsEnabled(pixelSnapVariants);
        if (mesh->loaded && list->lead.isGpuCreated() && list->lead.hasPivots() != list->lead.gpuHasPivots())
            mesh->needReload = true;
        uint64_t& seen = attachedBoundsRevision[entity];
        if (seen != list->revision){
            seen = list->revision;
            const AABB bounds = list->chunks.empty() ? AABB::ZERO : list->bounds;
            if (mesh->aabb != bounds){
                mesh->aabb = bounds;
                mesh->needUpdateAABB = true;
            }
        }
    }
    for (auto it = attachedInstanceBatches.begin(); it != attachedInstanceBatches.end();){
        const Entity entity = it->first;
        SpriteInstanceBatch* batch = it->second;
        // a destroyed GameObject lets its batch go (the producer still owns it)
        if (!scene->isEntityCreated(entity)){
            batch->destroyGpu();
            attachedBoundsRevision.erase(entity);
            it = attachedInstanceBatches.erase(it);
            continue;
        }
        ++it;
        MeshComponent* mesh = scene->findComponent<MeshComponent>(entity);
        if (!mesh)
            continue;
        if (!mesh->spriteInstanced){
            mesh->spriteInstanced = true;
            mesh->needReload = true;
        }
        batch->setPivotsEnabled(pixelSnapVariants);
        if (mesh->loaded && batch->isGpuCreated() && (batch->getCapacity() > batch->getGpuCapacity() || batch->hasFx() != batch->gpuHasFx() ||
                                                      batch->hasPivots() != batch->gpuHasPivots()))
            mesh->needReload = true;
        // the batch is culled and picked whole by its bounds, in its GameObject's space
        uint64_t& seen = attachedBoundsRevision[entity];
        if (seen != batch->getRevision()){
            seen = batch->getRevision();
            const AABB bounds = batch->getCount() > 0 ? batch->localBounds() : AABB::ZERO;
            if (mesh->aabb != bounds){
                mesh->aabb = bounds;
                mesh->needUpdateAABB = true;
            }
        }
    }
}

void RenderSystem::updateSpriteBatches(){
    updateAttachedInstanceBatches();
    if (!scene->isComponentRegistered<SpriteBatchComponent>())
        return;
    auto batches = scene->getComponentArray<SpriteBatchComponent>();
    if (batches->size() == 0)
        return;
    const uint64_t commit = SokolSystem::getCommitCount();
    for (size_t i = 0; i < batches->size(); i++){
        SpriteBatchComponent& component = batches->getComponentFromIndex(i);
        const Entity entity = batches->getEntity(i);
        MeshComponent* mesh = scene->findComponent<MeshComponent>(entity);
        SpriteComponent* sprite = scene->findComponent<SpriteComponent>(entity);
        Transform* transform = scene->findComponent<Transform>(entity);
        if (!mesh || !sprite || !transform || !mesh->spriteInstanced || !component.state){
            component.count = component.state ? component.state->count : 0;
            continue;
        }
        SpriteBatchState& state = *component.state;
        // Clear Each Frame: a frame in which no script wrote draws nothing
        if (component.mode == SpriteBatchMode::CLEAR_EACH_FRAME && state.drawnFrame != 0 && !state.writtenSinceDraw && commit > state.drawnFrame)
            state.clear();
        component.count = state.count;
        SpriteInstanceBatch& batch = *spriteInstancesOf(entity);
        batch.setStorage(component.mode == SpriteBatchMode::CLEAR_EACH_FRAME ? SpriteInstanceBatch::Storage::PerFrame
                                                                             : SpriteInstanceBatch::Storage::Retained);
        // the GPU buffer holds the capacity, or past it half as much again as it had (a larger one is
        // made by a reload: a batch filled one instance a frame reloads now and then, not each frame)
        uint32_t room = std::max(component.capacity, batch.getCapacity());
        if (state.count > room)
            room = std::max(state.count, room + room / 2);
        batch.reserve(room);
        if (spriteBatchWantsFx(*mesh))
            batch.enableFx();
        // a snapping scene's instances snap at their own pivots (the 'Spv' instanced variant)
        batch.setPivotsEnabled(pixelSnapVariants);
        if (mesh->loaded && batch.isGpuCreated() && (batch.getCapacity() > batch.getGpuCapacity() || batch.hasFx() != batch.gpuHasFx() ||
                                                     batch.hasPivots() != batch.gpuHasPivots()))
            mesh->needReload = true;

        const auto packStart = std::chrono::steady_clock::now();
        packSpriteBatch(entity, component, state, batch, *sprite, *mesh, *transform);
        spriteBatchStats.packMs += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - packStart).count();

        // the batch is culled and picked whole by its instances' bounds
        const AABB& bounds = state.count > 0 ? batch.localBounds() : AABB::ZERO;
        if (mesh->aabb != bounds){
            mesh->aabb = bounds;
            mesh->needUpdateAABB = true;
        }
        state.drawnFrame = commit;
        state.writtenSinceDraw = false;
    }
}

void RenderSystem::packSpriteBatch(Entity entity, SpriteBatchComponent& component, SpriteBatchState& state, SpriteInstanceBatch& batch,
                                   SpriteComponent& sprite, MeshComponent& mesh, Transform& transform){
    // the frames the instances may show: a sheet's, else the legacy sprite's rects of its texture
    std::vector<SpriteBatchFrame> frames;
    std::vector<std::string> pages;
    const AABB base = mesh.verticesAABB;
    if (base.isNull() || base == AABB::ZERO)
        return;
    const float bx0 = base.getMinimum().x, by0 = base.getMinimum().y;
    const float bx1 = base.getMaximum().x, by1 = base.getMaximum().y;
    if (bx1 <= bx0 || by1 <= by0)
        return;
    uint64_t framesKey = 1469598103934665603ull;
    auto mix = [&framesKey](uint64_t value){
        framesKey = (framesKey ^ value) * 1099511628211ull;
    };
    auto mixFloat = [&mix](float value){
        uint32_t bits;
        std::memcpy(&bits, &value, sizeof(bits));
        mix(bits);
    };
    mixFloat(bx0); mixFloat(by0); mixFloat(bx1); mixFloat(by1);

    CameraComponent* camera = scene->findComponent<CameraComponent>(scene->getCamera());
    const bool uiCamera = camera && camera->type == CameraType::CAMERA_UI;
    if (!sprite.sheet.empty() && sprite.sheetData && sprite.loadedSheet == sprite.sheet.path){
        const SpriteSheetData& sheet = *sprite.sheetData;
        mix(sprite.sheetGeneration);
        SpriteFrameGeometryInput input;
        input.width = sprite.width;
        input.height = sprite.height;
        input.pivot = sprite.pivotPreset;
        input.pixelsPerUnit = sprite.pixelsPerUnit > 0.0f ? sprite.pixelsPerUnit : sheet.pixelsPerUnit;
        input.pixelRatio = sheet.pixelRatio;
        input.uiCamera = uiCamera;
        input.flipY = sprite.flipY;
        mix((uint64_t)sprite.width << 32 | sprite.height);
        mix((uint64_t)sprite.pivotPreset | ((uint64_t)sprite.flipY << 8));
        for (const SpriteFrame& frame : sheet.frames){
            if (frame.page >= sheet.pages.size()){
                frames.push_back(SpriteBatchFrame());
                continue;
            }
            input.pageWidth = sheet.pages[frame.page].width;
            input.pageHeight = sheet.pages[frame.page].height;
            const SpriteFrameQuad quad = spriteFrameQuad(frame, input);
            SpriteBatchFrame entry;
            entry.x0 = quad.x0;
            entry.y0 = quad.y0;
            entry.x1 = quad.x1;
            entry.y1 = quad.y1;
            entry.rect = quad.textureRect;
            entry.page = frame.page;
            frames.push_back(entry);
        }
        for (const SpritePage& page : sheet.pages)
            pages.push_back(page.texture);
    }else if (sprite.inlineAtlas && !sprite.inlineAtlas->page.empty() && sprite.inlineAtlas->sourceWidth > 0){
        // a legacy sprite drawn from an atlas page (S45 R8): each of its source rects mapped onto
        // the page, as the Sprite maps the one it shows
        const SpriteInlineAtlas& atlas = *sprite.inlineAtlas.get();
        const float width = (float)atlas.sourceWidth, height = (float)atlas.sourceHeight;
        std::shared_ptr<const SpriteSheetData> sheet = SpriteSheetPool::find(atlas.source);
        mix(atlas.revision);
        mix(sprite.numFramesRect);
        std::vector<Rect> rects;
        if (sprite.numFramesRect == 0)
            rects.push_back(atlas.sourceRect);
        for (unsigned int f = 0; f < sprite.numFramesRect; f++)
            rects.push_back(sprite.framesRect[f].rect);
        SpriteFrameGeometryInput input;
        input.width = sprite.width;
        input.height = sprite.height;
        input.pixelsPerUnit = resolvePixelsPerUnit(sprite.pixelsPerUnit);
        input.pivot = sprite.pivotPreset;
        input.uiCamera = uiCamera;
        input.flipY = sprite.flipY;
        for (const Rect& rect : rects){
            const Rect pixels = rect.isNormalized() ? Rect(rect.getX() * width, rect.getY() * height, rect.getWidth() * width,
                                                           rect.getHeight() * height) : rect;
            SpriteFrame mapped;
            SpriteBatchFrame entry;
            entry.x0 = bx0; entry.y0 = by0; entry.x1 = bx1; entry.y1 = by1;
            if (sheet && SpriteSheetSources::mapInlineRect(atlas.source, pixels, mapped) && mapped.page < sheet->pages.size()){
                input.pageWidth = sheet->pages[mapped.page].width;
                input.pageHeight = sheet->pages[mapped.page].height;
                const SpriteFrameQuad quad = spriteFrameQuad(mapped, input);
                entry.x0 = quad.x0; entry.y0 = quad.y0; entry.x1 = quad.x1; entry.y1 = quad.y1;
                entry.rect = quad.textureRect;
                entry.page = mapped.page;
            }
            frames.push_back(entry);
        }
        if (sheet){
            for (const SpritePage& page : sheet->pages)
                pages.push_back(page.texture);
        }
    }else{
        // a legacy sprite: every frame is its quad, its rects in its texture (pixels or normalized)
        Texture& texture = mesh.submeshes[0].material.baseColorTexture;
        const float width = (float)texture.getWidth();
        const float height = (float)texture.getHeight();
        mix(((uint64_t)texture.getWidth() << 32) | texture.getHeight());
        mix(sprite.numFramesRect);
        auto normalized = [&](Rect rect){
            if (!rect.isNormalized() && width > 0.0f && height > 0.0f)
                rect = Rect(rect.getX() / width, rect.getY() / height, rect.getWidth() / width, rect.getHeight() / height);
            return rect;
        };
        if (sprite.numFramesRect == 0){
            SpriteBatchFrame entry;
            entry.x0 = bx0; entry.y0 = by0; entry.x1 = bx1; entry.y1 = by1;
            entry.rect = mesh.submeshes[0].textureRect;
            frames.push_back(entry);
        }
        for (unsigned int f = 0; f < sprite.numFramesRect; f++){
            SpriteBatchFrame entry;
            entry.x0 = bx0; entry.y0 = by0; entry.x1 = bx1; entry.y1 = by1;
            entry.rect = normalized(sprite.framesRect[f].rect);
            frames.push_back(entry);
        }
        for (const SpriteBatchFrame& entry : frames){
            mixFloat(entry.rect.getX()); mixFloat(entry.rect.getY()); mixFloat(entry.rect.getWidth()); mixFloat(entry.rect.getHeight());
        }
    }
    if (frames.empty())
        return;

    // the rotation policy of a snapped scene (S41 R10): Steps90 rounds each instance's angle
    PixelRotation rotation = PixelRotation::ON_GRID;
    if (pixelSnapVariants && mainPixelPerfect >= 0){
        rotation = pixelPerfectDrives[(size_t)mainPixelPerfect].rotation;
        if (sprite.pixelRotation != PixelRotation::CAMERA)
            rotation = sprite.pixelRotation;
    }
    const bool steps90 = pixelSnapVariants && rotation == PixelRotation::STEPS_90;
    const bool axisSort = component.sortInstances;
    const Vector3 axis = SortingLayers::getTransparencySortAxis();
    const bool world = component.space == SpriteBatchSpace::WORLD;
    const uint32_t layout = (axisSort ? 1u : 0u) | (world ? 2u : 0u) | (steps90 ? 4u : 0u) | (batch.hasPivots() ? 8u : 0u) |
                            (axisSort ? SortingLayers::getRevision() << 4 : 0u);
    const bool modelChanged = world && std::memcmp(state.packedModel, &transform.modelMatrix, sizeof(state.packedModel)) != 0;
    if (state.packedRevision == state.revision && state.packedFrames == framesKey && state.packedLayout == layout && !modelChanged &&
            batch.getCount() == state.count)
        return;
    state.packedRevision = state.revision;
    state.packedFrames = framesKey;
    state.packedLayout = layout;
    std::memcpy(state.packedModel, &transform.modelMatrix, sizeof(state.packedModel));

    const uint32_t count = state.count;
    // the draw order: index order, or along the Transparency Sort Axis (a larger distance first)
    std::vector<uint32_t>& order = spriteBatchOrder;
    order.resize(count);
    std::iota(order.begin(), order.end(), 0u);
    if (axisSort && count > 1){
        std::vector<float>& distance = spriteBatchDistance;
        distance.resize(count);
        for (uint32_t i = 0; i < count; i++){
            const Vector3 p = world ? state.positions[i] : transform.modelMatrix * state.positions[i];
            distance[i] = axis.dotProduct(p);
        }
        std::stable_sort(order.begin(), order.end(), [&distance](uint32_t a, uint32_t b){ return distance[a] > distance[b]; });
    }

    const Matrix4 toLocal = world ? transform.modelMatrix.inverse() : Matrix4();
    batch.setCount(count);
    std::span<InstanceRenderData> instances = batch.instances();
    std::span<Vector4> fx = batch.fx();
    std::span<Vector2> pivots = batch.pivots();
    std::vector<SpritePageRun>& runs = spriteBatchRuns;
    runs.clear();
    const int frameCount = (int)frames.size();
    for (uint32_t n = 0; n < count; n++){
        const uint32_t i = order[n];
        const SpriteBatchFrame& frame = frames[(size_t)std::clamp(state.frames[i], 0, frameCount - 1)];
        // the Sprite's quad onto the frame's, then scaled, turned and placed
        const float fsx = (frame.x1 - frame.x0) / (bx1 - bx0);
        const float fsy = (frame.y1 - frame.y0) / (by1 - by0);
        const float ftx = frame.x0 - bx0 * fsx;
        const float fty = frame.y0 - by0 * fsy;
        float c, s;
        spriteBatchRotation(state.rotations[i], steps90, c, s);
        const Vector2 scale = state.scales[i];
        const float l00 = c * scale.x * fsx, l01 = -s * scale.y * fsy;
        const float l10 = s * scale.x * fsx, l11 = c * scale.y * fsy;
        const float tx = state.positions[i].x + c * scale.x * ftx - s * scale.y * fty;
        const float ty = state.positions[i].y + s * scale.x * ftx + c * scale.y * fty;
        Matrix4 m;
        m[0][0] = l00; m[0][1] = l10; m[0][2] = 0.0f; m[0][3] = 0.0f;
        m[1][0] = l01; m[1][1] = l11; m[1][2] = 0.0f; m[1][3] = 0.0f;
        m[2][0] = 0.0f; m[2][1] = 0.0f; m[2][2] = 1.0f; m[2][3] = 0.0f;
        m[3][0] = tx; m[3][1] = ty; m[3][2] = state.positions[i].z; m[3][3] = 1.0f;
        InstanceRenderData& out = instances[n];
        out.instanceMatrix = world ? toLocal * m : m;
        out.color = state.colors[i];
        out.textureRect = frame.rect;
        if (!fx.empty())
            fx[n] = state.hasFx ? state.fx[i] : Vector4(0.0f, 0.0f, 0.0f, 0.0f);
        // the pivot a rotated instance snaps at: its position, as its Sprite alone snaps at its
        // origin (the matrix's translation carries the frame's offset too)
        if (!pivots.empty()){
            const Vector3 pivot = world ? toLocal * state.positions[i] : state.positions[i];
            pivots[n] = Vector2(pivot.x, pivot.y);
        }
        if (!runs.empty() && runs.back().page == frame.page)
            runs.back().count++;
        else
            runs.push_back({n, 1, frame.page});
    }
    batch.setPageRuns(runs);
    batch.setPages(pages, mesh.submeshes[0].material.baseColorTexture);
    batch.markChanged(bx0, by0, bx1, by1);
}

bool RenderSystem::drawSpriteChunks(ObjectRender& render, Submesh& submesh, SpriteInstanceChunkList& list, ShaderRender* shader, int baseColorSlotType,
                                    const Transform& transform, float cameraFar, const Plane frustumPlanes[6]){
    const uint64_t commit = SokolSystem::getCommitCount();
    bool drawn = false;
    const size_t firstPart = passSpritePart >= 0 ? (size_t)passSpritePart : 0;
    const size_t endPart = passSpritePart >= 0 ? std::min(list.chunks.size(), firstPart + 1) : list.chunks.size();
    for (size_t part = firstPart; part < endPart; part++){
        SpriteInstanceChunk& chunk = list.chunks[part];
        if (!isInsideCamera(cameraFar, frustumPlanes, transform.modelMatrix * chunk.bounds))
            continue;
        chunk.lastDrawn = commit;
        if (chunk.needsBuild && list.build)
            list.build(chunk);
        SpriteInstanceBatch& batch = *chunk.batch;
        if (batch.getCount() == 0)
            continue;
        // the streams and size the draw needs: the mesh loaded with the lead's streams
        if (list.lead.hasFx())
            batch.enableFx();
        batch.setPivotsEnabled(list.lead.hasPivots());
        if (!batch.isGpuCreated() || batch.getCapacity() > batch.getGpuCapacity() || batch.hasFx() != batch.gpuHasFx() ||
            batch.hasPivots() != batch.gpuHasPivots()){
            if (!batch.createGpu())
                continue;
        }
        drawn = drawSpriteInstances(render, submesh, batch, shader, baseColorSlotType, &list.lead, chunk.rangeFirst, chunk.rangeCount) || drawn;
    }
    return drawn;
}

bool RenderSystem::drawSpriteInstances(ObjectRender& render, Submesh& submesh, SpriteInstanceBatch& batch, ShaderRender* shader, int baseColorSlotType,
                                       SpriteInstanceBatch* bound, uint32_t rangeFirst, uint32_t rangeCount){
    if (batch.getCount() == 0 || !batch.isGpuCreated() || batch.getCount() > batch.getGpuCapacity())
        return false;
    const uint64_t commit = SokolSystem::getCommitCount();
    if (batch.needsUpload(commit)){
        const auto uploadStart = std::chrono::steady_clock::now();
        spriteBatchStats.uploadBytes += batch.upload(commit);
        spriteBatchStats.uploadMs += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - uploadStart).count();
    }
    ExternalBuffer& instances = batch.getInstanceBuffer();
    ExternalBuffer& fx = batch.getFxBuffer();
    ExternalBuffer& pivots = batch.getPivotBuffer();
    // what the render's vertex bindings were made from (a chunk list's lead; else this batch)
    SpriteInstanceBatch& owner = bound ? *bound : batch;
    BufferRender* boundInstances = owner.getInstanceBuffer().getRender();
    BufferRender* boundFx = owner.getFxBuffer().getRender();
    BufferRender* boundPivots = owner.getPivotBuffer().getRender();
    // a run on another page than the material's binds that page's texture for its draw
    std::pair<int, int> slot = {-1, -1};
    if (batch.getPageCount() > 0 && baseColorSlotType >= 0 && shader)
        slot = shader->shaderData.getTextureIndex((TextureShaderType)baseColorSlotType);
    const std::string materialPage = submesh.material.baseColorTexture.getPath();
    TextureRender* materialTexture = submesh.material.baseColorTexture.getRender(&emptyWhite);
    bool lent = false;
    const uint64_t rangeEnd = rangeCount == UINT32_MAX ? UINT64_MAX : (uint64_t)rangeFirst + rangeCount;
    for (const SpritePageRun& pageRun : batch.getPageRuns()){
        // the whole run, or its part inside this piece's range
        const uint64_t runBegin = std::max<uint64_t>(pageRun.first, rangeFirst);
        const uint64_t runEnd = std::min<uint64_t>((uint64_t)pageRun.first + pageRun.count, rangeEnd);
        if (runBegin >= runEnd)
            continue;
        const SpritePageRun run{(uint32_t)runBegin, (uint32_t)(runEnd - runBegin), pageRun.page};
        if (slot.first >= 0){
            Texture* page = batch.getPageTexture(run.page);
            if (page && page->getPath() != materialPage){
                TextureRender* pageRender = page->getRender(&emptyWhite);
                if (!pageRender || !pageRender->isCreated())
                    continue; // still loading: drawn once it is there
                render.addTexture(slot, ShaderStageType::FRAGMENT, pageRender);
                lent = true;
            }else if (lent && materialTexture){
                render.addTexture(slot, ShaderStageType::FRAGMENT, materialTexture);
                lent = false;
            }
        }
        render.replaceVertexBuffer(boundInstances, instances.getRender(), (size_t)run.first * instances.getStride());
        if (batch.gpuHasFx())
            render.replaceVertexBuffer(boundFx, fx.getRender(), (size_t)run.first * fx.getStride());
        if (batch.gpuHasPivots())
            render.replaceVertexBuffer(boundPivots, pivots.getRender(), (size_t)run.first * pivots.getStride());
        render.draw(0, submesh.vertexCount, run.count);
        spriteBatchStats.draws++;
        spriteBatchStats.instances += run.count;
    }
    render.replaceVertexBuffer(boundInstances, boundInstances, 0);
    if (batch.gpuHasFx())
        render.replaceVertexBuffer(boundFx, boundFx, 0);
    if (batch.gpuHasPivots())
        render.replaceVertexBuffer(boundPivots, boundPivots, 0);
    if (lent && materialTexture)
        render.addTexture(slot, ShaderStageType::FRAGMENT, materialTexture);
    return true;
}
