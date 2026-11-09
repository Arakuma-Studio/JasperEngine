// (c) JasperDev and contributors
// SPDX-License-Identifier: MIT

#include "subsystem/TilemapSystem.h"

#include "Engine.h"
#include "Log.h"
#include "Scene.h"
#include "component/MeshComponent.h"
#include "component/Rigidbody2DComponent.h"
#include "component/Transform.h"
#include "jobs/JobSystem.h"
#include "subsystem/MeshSystem.h"
#include "subsystem/RenderSystem.h"
#include "subsystem/TilemapColliders.h"

#include <algorithm>
#include <cmath>
#include <cstring>

using namespace jasperengine;

namespace{

    // R33: a layer's dirty chunks build on jobs above this many (their builds are independent)
    constexpr size_t kMaxSerialChunkBuilds = 4;
    // R33: a chunk nobody saw for this long gives its GPU buffers back
    constexpr float kChunkResidencySeconds = 2.0f;

}

TilemapSystem::TilemapSystem(Scene* scene): SubSystem(scene){
    signature.set(scene->getComponentId<TilemapLayerComponent>());
    declareHookType(scene->getComponentId<TilemapLayerComponent>());
    declareHookType(scene->getComponentId<TilemapCollider2DComponent>());
}

void TilemapSystem::load(){
}

void TilemapSystem::draw(){
}

void TilemapSystem::destroy(){
    auto layers = scene->getComponentArray<TilemapLayerComponent>();
    for (size_t i = 0; i < layers->size(); i++)
        releaseLayer(layers->getEntity(i), layers->getComponentFromIndex(i));
}

void TilemapSystem::onComponentRemoved(Entity entity, ComponentId componentId){
    if (componentId == scene->getComponentId<TilemapLayerComponent>()){
        if (TilemapLayerComponent* layer = scene->findComponent<TilemapLayerComponent>(entity))
            releaseLayer(entity, *layer);
    }else if (componentId == scene->getComponentId<TilemapCollider2DComponent>()){
        if (TilemapCollider2DComponent* collider = scene->findComponent<TilemapCollider2DComponent>(entity))
            tilemapcolliders::release(scene, entity, *collider);
    }
}

Entity TilemapSystem::findGrid(Entity entity) const{
    Entity current = entity;
    while (current != NULL_ENTITY){
        if (scene->findComponent<GridComponent>(current))
            return current;
        const Transform* transform = scene->findComponent<Transform>(current);
        if (!transform)
            break;
        current = transform->parent;
    }
    return NULL_ENTITY;
}

TileGridSpec TilemapSystem::layerSpec(Entity entity, const TilemapLayerComponent& layer) const{
    const Entity gridEntity = findGrid(entity);
    if (gridEntity != NULL_ENTITY)
        return gridSpecOf(scene->getComponent<GridComponent>(gridEntity));
    TileGridSpec spec;
    const double ppu = (double)Engine::getPixelsPerUnit();
    if (std::shared_ptr<const TilesetRuntime> runtime = layer.tileset.empty() ? nullptr : TilesetPool::get(layer.tileset.path)){
        spec.cellWidth = runtime->data->tileWidth / ppu;
        spec.cellHeight = runtime->data->tileHeight / ppu;
    }
    return spec;
}

void TilemapSystem::releaseLayer(Entity entity, TilemapLayerComponent& layer){
    if (!layer.state)
        return;
    if (layer.state->attached){
        if (std::shared_ptr<RenderSystem> render = scene->findSystem<RenderSystem>())
            render->detachInstanceChunks(entity);
    }
    layer.state.reset();
}

TilemapChunkParams TilemapSystem::chunkParams(const TilemapLayerComponent& layer) const{
    const TilemapLayerState& state = *layer.state;
    TilemapChunkParams params;
    params.cells = &layer.cells.read();
    params.grid = state.spec;
    params.tileset = state.tileset.get();
    params.pixelsPerUnit = state.ppu;
    params.tileAnchorX = layer.tileAnchor.x;
    params.tileAnchorY = layer.tileAnchor.y;
    params.dualGrid = layer.dualGrid;
    params.snapEdges = state.snapEdges;
    params.seed = layer.seed;
    params.animationMs = (int64_t)state.animationMs;
    params.individual = layer.mode == TilemapRenderMode::Individual;
    return params;
}

void TilemapSystem::buildChunk(TilemapLayerComponent& layer, TilemapChunkRuntime& chunk) const{
    TilemapLayerState& state = *layer.state;
    const TileChunk* stored = layer.cells.read().chunk(chunk.coord);
    chunk.batch.setPivotsEnabled(state.list.lead.hasPivots());
    if (state.list.lead.hasFx())
        chunk.batch.enableFx();
    tilechunk::build(chunkParams(layer), chunk.coord, chunk.batch, chunk.build);
    chunk.builtRevision = stored ? stored->revision : 0;
}

void TilemapSystem::syncChunkList(TilemapLayerComponent& layer){
    TilemapLayerState& state = *layer.state;
    const TileGridSpec& spec = state.spec;
    SpriteInstanceChunkList& list = state.list;
    const TileCells& cells = layer.cells.read();
    std::vector<ChunkCoord> order;
    order.reserve(state.chunks.size());
    for (const auto& entry : state.chunks)
        order.push_back(entry.first);
    const int size = cells.getChunkSize();
    std::sort(order.begin(), order.end(), [&](const ChunkCoord& a, const ChunkCoord& b){
        return tilegrid::painterLess(spec, CellCoord{a.x * size, a.y * size}, CellCoord{b.x * size, b.y * size});
    });
    double shiftX = 0.0, shiftY = 0.0;
    if (layer.dualGrid){
        shiftX = -tilegrid::pitchX(spec) * 0.5;
        shiftY = -tilegrid::pitchY(spec) * 0.5;
    }
    AABB union_;
    union_.setNull();
    std::vector<SpriteInstanceChunk> previous = std::move(list.chunks);
    list.chunks.clear();
    list.chunks.reserve(order.size());
    for (const ChunkCoord& coord : order){
        TilemapChunkRuntime& runtime = *state.chunks.at(coord);
        SpriteInstanceChunk entry;
        if (runtime.listIndex < previous.size() && previous[runtime.listIndex].batch == &runtime.batch)
            entry = previous[runtime.listIndex];
        entry.batch = &runtime.batch;
        entry.bounds = tilechunk::chunkBounds(spec, cells.chunkRect(coord), state.overhangX, state.overhangY, shiftX, shiftY);
        union_.merge(entry.bounds);
        runtime.listIndex = list.chunks.size();
        list.chunks.push_back(entry);
    }
    list.bounds = union_;
    list.revision++;
}

void TilemapSystem::syncIndividualRows(TilemapLayerComponent& layer){
    TilemapLayerState& state = *layer.state;
    SpriteInstanceChunkList& list = state.list;
    const std::vector<SpriteInstanceChunk> chunks = list.chunks;
    list.chunks.clear();
    for (size_t c = 0; c < chunks.size(); c++){
        for (auto& entry : state.chunks){
            if (&entry.second->batch != chunks[c].batch)
                continue;
            TilemapChunkRuntime& runtime = *entry.second;
            bool first = true;
            for (const TilemapChunkRow& row : runtime.build.rows){
                SpriteInstanceChunk part = chunks[c];
                part.rangeFirst = row.first;
                part.rangeCount = row.count;
                part.sortItem = true;
                float local[3];
                tilegrid::planeToLocal(state.spec, TilePoint{row.anchorU, row.anchorV}, local);
                part.sortPoint = Vector3(local[0], local[1], local[2]);
                if (first){
                    runtime.listIndex = list.chunks.size();
                    first = false;
                }
                list.chunks.push_back(part);
            }
            if (first){
                // an empty chunk keeps one entry, so the next sync still knows its batch
                SpriteInstanceChunk part = chunks[c];
                part.rangeFirst = 0;
                part.rangeCount = 0;
                part.sortItem = true;
                runtime.listIndex = list.chunks.size();
                list.chunks.push_back(part);
            }
            break;
        }
    }
    list.revision++;
}

void TilemapSystem::updateAnimations(Entity entity, TilemapLayerComponent& layer, double dt){
    TilemapLayerState& state = *layer.state;
    (void)entity;
    if (!paused)
        state.animationMs += dt * 1000.0 * (double)layer.animationSpeed;
    if (!state.tileset || state.tileset->animations.empty())
        return;
    const int64_t ms = (int64_t)state.animationMs;
    const TilemapChunkParams params = chunkParams(layer);
    for (auto& entry : state.chunks){
        TilemapChunkRuntime& chunk = *entry.second;
        if (chunk.build.animated.empty())
            continue;
        // one comparison while no frame changes (R60)
        if (ms < chunk.build.nextFrameMs)
            continue;
        bool changed = false;
        if (!tilechunk::updateAnimation(params, chunk.batch, chunk.build, ms, changed)){
            if (layer.mode == TilemapRenderMode::Individual)
                layer.needUpdate = true;
            else
                state.list.chunks[chunk.listIndex].needsBuild = true;
        }
    }
}

void TilemapSystem::updateLayer(Entity entity, TilemapLayerComponent& layer, double dt){
    if (!layer.enabled){
        releaseLayer(entity, layer);
        return;
    }
    if (!layer.state)
        layer.state = std::make_shared<TilemapLayerState>();
    TilemapLayerState& state = *layer.state;
    state.rebuilt = 0;

    // ---- what the chunks depend on
    bool allDirty = layer.needUpdate;
    layer.needUpdate = false;
    if (state.tilesetPath != layer.tileset.path || state.tilesetRevision != TilesetPool::revision()){
        state.tilesetPath = layer.tileset.path;
        state.tilesetRevision = TilesetPool::revision();
        std::shared_ptr<const TilesetRuntime> next = layer.tileset.empty() ? nullptr : TilesetPool::get(layer.tileset.path);
        if (next != state.tileset){
            state.tileset = next;
            allDirty = true;
        }
    }
    const Entity gridEntity = findGrid(entity);
    const uint64_t gridRevision = gridEntity != NULL_ENTITY ? scene->getComponent<GridComponent>(gridEntity).revision : 0;
    if (gridEntity != state.grid || gridRevision != state.gridRevision || state.ppu != (double)Engine::getPixelsPerUnit()){
        state.grid = gridEntity;
        state.gridRevision = gridRevision;
        state.spec = layerSpec(entity, layer);
        state.ppu = (double)Engine::getPixelsPerUnit();
        allDirty = true;
    }
    if (state.spec.cellWidth <= 0.0 || state.spec.cellHeight <= 0.0)
        return;
    std::shared_ptr<RenderSystem> render = scene->findSystem<RenderSystem>();
    const bool snap = render && render->hasPixelSnapVariants();
    if (snap != state.snapEdges){
        state.snapEdges = snap;
        allDirty = true;
    }

    // ---- the cells
    if (layer.cells.isSet() && layer.chunkSize != layer.cells.read().getChunkSize() && TileCells::isValidChunkSize(layer.chunkSize))
        layer.cells.edit().setChunkSize(layer.chunkSize);
    const TileCells& cells = layer.cells.read();
    layer.cellCount = (uint32_t)cells.cellCount();
    layer.chunkCount = (uint32_t)cells.chunkCount();

    // ---- the mesh the instances draw (R2: a layer without a Mesh draws nothing)
    MeshComponent* mesh = scene->findComponent<MeshComponent>(entity);
    if (!mesh)
        return;
    bool meshChanged = false;
    if (!mesh->tileLayer){
        mesh->tileLayer = true;
        meshChanged = true;
    }
    if (mesh->buffer.getCount() == 0 || mesh->numSubmeshes == 0 || mesh->buffer.getAttribute(AttributeType::POSITION) == nullptr){
        MeshSystem::prepareTileLayerMesh(*mesh);
        meshChanged = true;
    }
    if (mesh->numSubmeshes > 0 && state.tileset && !state.tileset->pages.empty()){
        Texture& texture = mesh->submeshes[0].material.baseColorTexture;
        if (texture.getPath() != state.tileset->pages[0]){
            texture.setAlphaBorderAuto(true);
            texture.setPath(state.tileset->pages[0]);
            mesh->submeshes[0].needUpdateTexture = true;
        }
    }
    // the Scene view's Focus On dims the other layers through the alpha (S50 R65): runtime only
    Vector4 tint = layer.tint;
    tint.w *= layer.focusDim;
    if (mesh->numSubmeshes > 0 && std::memcmp(state.appliedTint, &tint, sizeof(state.appliedTint)) != 0){
        std::memcpy(state.appliedTint, &tint, sizeof(state.appliedTint));
        mesh->submeshes[0].material.baseColorFactor = tint;
    }
    uint8_t attributes = MeshSystem::spriteBatchAttributes(*mesh, false, scene->getDefaultCustomShader(ShaderType::MESH));
    attributes &= (uint8_t)MeshBatchAttribute_SpriteFx;
    if (attributes != mesh->batchAttributes){
        mesh->batchAttributes = attributes;
        meshChanged = true;
    }
    if (meshChanged && mesh->loaded)
        mesh->needReload = true;

    if (!state.attached && render){
        render->attachInstanceChunks(entity, &state.list);
        state.attached = true;
    }
    if (!state.list.build){
        const Entity owner = entity;
        Scene* ownerScene = scene;
        state.list.build = [this, owner, ownerScene](SpriteInstanceChunk& chunk){
            TilemapLayerComponent* target = ownerScene->findComponent<TilemapLayerComponent>(owner);
            if (!target || !target->state)
                return;
            for (auto& entry : target->state->chunks){
                if (&entry.second->batch == chunk.batch){
                    buildChunk(*target, *entry.second);
                    target->state->rebuilt++;
                    break;
                }
            }
            chunk.needsBuild = false;
        };
    }

    // ---- which chunks changed (compared by revision: a copy of the store shares them)
    bool listChanged = false;
    std::vector<ChunkCoord> changed;
    // another store with other revisions (an undone copy, a clone that wrote): compare it whole
    if (&cells != state.store && cells.getRevision() != state.storeRevision)
        allDirty = true;
    if (state.tileset && (allDirty || &cells != state.store || cells.getRevision() != state.storeRevision)){
        for (auto it = state.chunks.begin(); it != state.chunks.end();){
            if (!cells.chunk(it->first)){
                changed.push_back(it->first);
                it = state.chunks.erase(it);
                listChanged = true;
            }else{
                ++it;
            }
        }
        for (const auto& entry : cells.chunks()){
            auto found = state.chunks.find(entry.first);
            if (found == state.chunks.end()){
                auto runtime = std::make_unique<TilemapChunkRuntime>();
                runtime->coord = entry.first;
                found = state.chunks.emplace(entry.first, std::move(runtime)).first;
                changed.push_back(entry.first);
                listChanged = true;
            }else if (allDirty || found->second->builtRevision != entry.second.revision){
                changed.push_back(entry.first);
            }
        }
        state.store = &cells;
        state.storeRevision = cells.getRevision();
    }
    if (!state.tileset && !state.chunks.empty()){
        // R89: a missing tileset draws nothing and keeps the cells
        state.chunks.clear();
        listChanged = true;
    }
    if (state.tileset && allDirty){
        double overhangX = 0.0, overhangY = 0.0;
        for (const TileRenderInfo& tile : state.tileset->tiles){
            if (tile.valid){
                overhangX = std::max(overhangX, std::max<double>(tile.width, tile.height) + std::abs(tile.offsetX));
                overhangY = std::max(overhangY, std::max<double>(tile.width, tile.height) + std::abs(tile.offsetY));
            }
        }
        state.overhangX = overhangX / state.ppu;
        state.overhangY = overhangY / state.ppu;
        listChanged = true;
    }
    const bool individual = layer.mode == TilemapRenderMode::Individual;
    // (Individual mode sorted the list into rows: back to one entry per chunk to compare)
    if (listChanged || (individual && !changed.empty()))
        syncChunkList(layer);
    if (individual){
        for (const ChunkCoord& coord : changed){
            auto found = state.chunks.find(coord);
            if (found != state.chunks.end()){
                buildChunk(layer, *found->second);
                state.rebuilt++;
            }
        }
        for (SpriteInstanceChunk& entry : state.list.chunks)
            entry.needsBuild = false;
    }
    for (const ChunkCoord& coord : changed){
        auto found = state.chunks.find(coord);
        if (found != state.chunks.end() && !individual)
            state.list.chunks[found->second->listIndex].needsBuild = true;
    }

    // ---- the pages every chunk binds, and the chunks in view rebuilt now
    if (state.tileset && mesh->numSubmeshes > 0){
        for (auto& entry : state.chunks){
            entry.second->batch.setPages(state.tileset->pages, mesh->submeshes[0].material.baseColorTexture);
            entry.second->batch.setPivotsEnabled(state.list.lead.hasPivots());
        }
    }
    const uint64_t commit = SokolSystem::getCommitCount();
    std::vector<TilemapChunkRuntime*> builds;
    for (auto& entry : state.chunks){
        SpriteInstanceChunk& listed = state.list.chunks[entry.second->listIndex];
        // R33: built the first time it is visible, then again when dirty and still seen
        if (listed.needsBuild && listed.lastDrawn != 0 && commit - listed.lastDrawn <= 1){
            builds.push_back(entry.second.get());
            listed.needsBuild = false;
        }
        if (!individual && layer.gpuResidency == TilemapGpuResidency::Visible && entry.second->batch.isGpuCreated()){
            if (listed.lastDrawn + 1 >= commit){
                entry.second->unseenSeconds = 0.0f;
            }else{
                entry.second->unseenSeconds += (float)dt;
                if (entry.second->unseenSeconds > kChunkResidencySeconds){
                    entry.second->batch.destroyGpu();
                }
            }
        }
    }
    if (!builds.empty()){
        state.rebuilt += (uint32_t)builds.size();
        if (builds.size() > kMaxSerialChunkBuilds){
            jobs::parallelFor(builds.size(), [&](size_t i){ buildChunk(layer, *builds[i]); });
        }else{
            for (TilemapChunkRuntime* chunk : builds)
                buildChunk(layer, *chunk);
        }
    }
    if (individual && (listChanged || !changed.empty()))
        syncIndividualRows(layer);
    if (TilemapCollider2DComponent* collider = scene->findComponent<TilemapCollider2DComponent>(entity)){
        if (allDirty)
            tilemapcolliders::markAll(*collider);
        else if (!changed.empty())
            tilemapcolliders::markDirty(*collider, std::span<const ChunkCoord>(changed));
    }
    updateAnimations(entity, layer, dt);
}

void TilemapSystem::updateColliders(){
    auto colliders = scene->getComponentArray<TilemapCollider2DComponent>();
    for (size_t i = 0; i < colliders->size(); i++){
        TilemapCollider2DComponent& collider = colliders->getComponentFromIndex(i);
        const Entity entity = colliders->getEntity(i);
        TilemapLayerComponent* layer = scene->findComponent<TilemapLayerComponent>(entity);
        if (!layer || !layer->enabled || !collider.enabled){
            tilemapcolliders::release(scene, entity, collider);
            continue;
        }
        if (collider.needUpdate){
            collider.needUpdate = false;
            tilemapcolliders::markAll(collider);
        }
        if (!layer->state || !layer->state->tileset)
            continue;
        tilemapcolliders::update(scene, entity, *layer, collider, layer->state->spec, layer->state->tileset.get());
    }
}

void TilemapSystem::update(double dt){
    auto layers = scene->getComponentArray<TilemapLayerComponent>();
    if (layers->size() == 0)
        return;
    for (size_t i = 0; i < layers->size(); i++)
        updateLayer(layers->getEntity(i), layers->getComponentFromIndex(i), dt);
    updateColliders();
}

void TilemapSystem::fixedUpdate(double){
    if (scene->getComponentArray<TilemapCollider2DComponent>()->size() == 0)
        return;
    updateColliders();
}
