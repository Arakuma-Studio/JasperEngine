// (c) JasperDev and contributors
// SPDX-License-Identifier: MIT

#include "subsystem/TilemapColliders.h"

#include "component/TilemapCollider2DComponent.h"

#ifdef JASPERENGINE_PHYSICS_2D

#include "Scene.h"
#include "component/Rigidbody2DComponent.h"
#include "component/TilemapLayerComponent.h"
#include "jobs/JobSystem.h"
#include "tilemap/TilesetPool.h"
#include "Log.h"

#include <algorithm>
#include <memory>
#include <vector>

using namespace jasperengine;

namespace {

    TileColliderState& colliderStateOf(TilemapCollider2DComponent& collider){
        if (!collider.state){
            collider.state = std::make_shared<TileColliderState>();
        }
        return *collider.state;
    }

    Collider2D colliderTemplate(const TilemapCollider2DComponent& source){
        Collider2D collider;
        collider.friction = source.friction;
        collider.restitution = source.restitution;
        collider.material = source.material;
        collider.categoryBits = source.categoryBits;
        collider.maskBits = source.maskBits;
        collider.groupIndex = source.groupIndex;
        collider.contactEvents = source.contactEvents;
        collider.sensorEvents = source.sensorEvents;
        collider.enableHitEvents = source.enableHitEvents;
        return collider;
    }

    std::vector<Vector2> toPoints(const std::vector<TilePoint>& points){
        std::vector<Vector2> out;
        out.reserve(points.size());
        for (const TilePoint& point : points){
            out.push_back(Vector2((float)point.x, (float)point.y));
        }
        return out;
    }

    std::vector<Collider2D> makeColliders(const TileColliderChunk& built, const TilemapCollider2DComponent& source){
        std::vector<Collider2D> colliders;
        colliders.reserve(built.chainCount() + built.polygonCount());
        auto addChain = [&](const TileColliderChain& chain){
            Collider2D collider = colliderTemplate(source);
            collider.type = Collider2DType::CHAIN;
            collider.loop = chain.loop;
            collider.points = toPoints(chain.points);
            colliders.push_back(std::move(collider));
        };
        auto addPolygon = [&](const TileColliderPolygon& polygon, bool sensor){
            Collider2D collider = colliderTemplate(source);
            collider.type = Collider2DType::POLYGON;
            collider.sensor = sensor;
            collider.points = toPoints(polygon.points);
            colliders.push_back(std::move(collider));
        };
        for (const TileColliderChain& chain : built.loops) addChain(chain);
        for (const TileColliderChain& chain : built.chains) addChain(chain);
        for (const TileColliderChain& chain : built.oneWay) addChain(chain);
        for (const TileColliderPolygon& polygon : built.polygons) addPolygon(polygon, false);
        for (const TileColliderPolygon& polygon : built.sensors) addPolygon(polygon, true);
        return colliders;
    }

    void addWithin(ChunkCoord coord, int reach, std::vector<ChunkCoord>& out){
        for (int y = -reach; y <= reach; y++){
            for (int x = -reach; x <= reach; x++){
                out.push_back(ChunkCoord{coord.x + x, coord.y + y});
            }
        }
    }

}

void tilemapcolliders::markDirty(TilemapCollider2DComponent& collider, std::span<const ChunkCoord> chunks){
    TileColliderState& state = colliderStateOf(collider);
    state.dirty.insert(chunks.begin(), chunks.end());
}

void tilemapcolliders::markAll(TilemapCollider2DComponent& collider){
    colliderStateOf(collider).allDirty = true;
}

void tilemapcolliders::release(Scene* scene, Entity entity, TilemapCollider2DComponent& collider){
    collider.state.reset();
    collider.generatedChains = 0;
    collider.generatedPolygons = 0;
    collider.needUpdate = true;
    Rigidbody2DComponent* body = scene->findComponent<Rigidbody2DComponent>(entity);
    if (body && body->generated){
        body->generated->clear();
        body->needUpdateColliders = true;
    }
}

void tilemapcolliders::update(Scene* scene, Entity entity, const TilemapLayerComponent& layer,
                              TilemapCollider2DComponent& collider, const TileGridSpec& grid,
                              const TilesetRuntime* tileset){
    Rigidbody2DComponent* body = scene->findComponent<Rigidbody2DComponent>(entity);
    if (!body){
        return;
    }
    TileColliderState& state = colliderStateOf(collider);
    bool forceAll = state.allDirty;
    if (collider.needUpdate){
        collider.needUpdate = false;
        // a field (surface, geometry, source) changed: every chunk's colliders are replaced
        forceAll = true;
        state.allDirty = true;
    }
    if (!state.allDirty && state.dirty.empty()){
        return;
    }
    if (grid.plane != TileGridPlane::XY){
        Log::warn("Tilemap Collider 2D on '%s': its Grid lies on the XZ plane, where 2D physics has no colliders. Use a Grid on the XY plane.",
                  scene->getEntityName(entity).c_str());
        state.allDirty = false;
        state.dirty.clear();
        return;
    }

    const TileCells& cells = layer.cells.read();
    TileColliderSettings settings;
    settings.cells = &cells;
    settings.tileset = tileset && tileset->data ? tileset->data.get() : nullptr;
    settings.runtime = tileset;
    settings.grid = grid;
    settings.source = collider.source;
    settings.geometry = collider.geometry;
    settings.merge = collider.merge;
    settings.offset = TilePoint{(double)collider.offset.x, (double)collider.offset.y};
    settings.tileAnchor = TilePoint{(double)layer.tileAnchor.x, (double)layer.tileAnchor.y};
    settings.dualGrid = layer.dualGrid;
    if (collider.source == TileColliderSource::Terrain && settings.tileset){
        for (const std::string& name : collider.solidTerrains){
            const int terrain = settings.tileset->findTerrain(name);
            if (terrain != NO_TERRAIN){
                settings.solidTerrains.push_back(terrain);
            }
        }
    }
    const TileColliderBuilder builder(std::move(settings));
    const int reach = builder.chunkReach();

    std::vector<ChunkCoord> candidates;
    if (state.allDirty){
        for (const ChunkCoord& coord : cells.sortedChunks()){
            addWithin(coord, reach, candidates);
        }
        // the ones that had colliders may have none now
        for (const auto& [coord, chunk] : state.built){
            candidates.push_back(coord);
        }
    }else{
        candidates.reserve(state.dirty.size() * (size_t)(2 * reach + 1) * (size_t)(2 * reach + 1));
        for (const ChunkCoord& coord : state.dirty){
            addWithin(coord, reach, candidates);
        }
    }
    std::sort(candidates.begin(), candidates.end());
    candidates.erase(std::unique(candidates.begin(), candidates.end()), candidates.end());

    std::vector<TileColliderChunk> results(candidates.size());
    jobs::parallelFor(results.size(), [&](size_t i){
        results[i] = builder.build(candidates[i]);
    });

    bool changed = false;
    for (size_t i = 0; i < candidates.size(); i++){
        const ChunkCoord coord = candidates[i];
        TileColliderChunk& result = results[i];
        auto found = state.built.find(coord);
        const bool hadColliders = found != state.built.end();
        // an unchanged chunk keeps its shapes (and the contacts on them), unless the surface
        // every collider copies changed
        if (!forceAll && (hadColliders ? found->second == result : result.empty())){
            continue;
        }
        if (!body->generated){
            body->generated = std::make_shared<GeneratedColliders2D>();
        }
        body->generated->replaceChunk(coord, result.empty() ? std::vector<Collider2D>() : makeColliders(result, collider));
        if (result.empty()){
            if (hadColliders){
                state.built.erase(found);
            }
        }else if (hadColliders){
            found->second = std::move(result);
        }else{
            state.built.emplace(coord, std::move(result));
        }
        state.chunksReplaced++;
        changed = true;
    }
    state.chunksBuilt += candidates.size();
    state.allDirty = false;
    state.dirty.clear();

    if (changed){
        body->needUpdateColliders = true;
        uint32_t chains = 0, polygons = 0;
        for (const auto& [coord, built] : state.built){
            chains += (uint32_t)built.chainCount();
            polygons += (uint32_t)built.polygonCount();
        }
        collider.generatedChains = chains;
        collider.generatedPolygons = polygons;
    }
}

#else

using namespace jasperengine;

void tilemapcolliders::markDirty(TilemapCollider2DComponent&, std::span<const ChunkCoord>){}
void tilemapcolliders::markAll(TilemapCollider2DComponent&){}
void tilemapcolliders::update(Scene*, Entity, const TilemapLayerComponent&, TilemapCollider2DComponent&, const TileGridSpec&, const TilesetRuntime*){}
void tilemapcolliders::release(Scene*, Entity, TilemapCollider2DComponent&){}

#endif
