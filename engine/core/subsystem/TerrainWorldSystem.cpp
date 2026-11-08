// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "subsystem/TerrainWorldSystem.h"

#include "Engine.h"
#include "Scene.h"
#include "component/TerrainComponent.h"
#include "component/TerrainWorldComponent.h"
#include "component/Transform.h"
#include "io/AssetIOService.h"
#include "physics/TerrainWorldCollision.h"
#include "profiler/Profile.h"
#include "subsystem/FoliageSystem.h"
#include "subsystem/TerrainWorldRender.h"
#include "terrain/TreeStore.h"
#include "Log.h"

#include "zstd.h"

#include <algorithm>
#include <cmath>
#include <cstring>

using namespace jasperengine;
using namespace jasperengine::terrain;

namespace {

    TerrainWorldSystem::Host& worldHost() {
        static TerrainWorldSystem::Host host;
        return host;
    }

    // every live system, for the pump; main thread only
    std::vector<TerrainWorldSystem*>& liveSystems() {
        static std::vector<TerrainWorldSystem*> systems;
        return systems;
    }

    double& lastPumpAll() {
        static double last = -1.0;
        return last;
    }

    // R15: the camera's velocity, from its last two positions
    struct FocusTrack {
        Vector3 last;
        double at = -1.0;
    };

    std::map<std::pair<const Scene*, Entity>, FocusTrack>& focusTracks() {
        static std::map<std::pair<const Scene*, Entity>, FocusTrack> tracks;
        return tracks;
    }

    // ------------------------------------------------------------------ R27: foliage

    const char* const TILE_FOLIAGE_READ_ONLY =
        "a Terrain World tile's foliage view only reads: edit the world's maps through its Terrain (edit_terrain, the brushes)";

    // T5's maps over one tile, in the tile's own texels: height with its one-sample gutter (N + 3 a side,
    // texel i at (i - 1) h from the tile's corner, so the scatter's bilerp crosses the seam on the
    // neighbour's samples), holes and density as the tile holds them. The world's own store writes.
    class TileFoliageMaps final : public TerrainMapStore {
    public:
        TileFoliageMaps(TerrainWorldRuntime& runtime, TileCoord coord): runtime(runtime), coord(coord) {}

        TerrainFrame frame() const override {
            TerrainFrame frame;
            frame.maxHeight = runtime.maxHeight();
            return frame;
        }

        MapFormat format(MapId map) const override {
            const WorldLattice& lattice = runtime.lattice();
            MapFormat format;
            format.channels = 1;
            format.bitsPerChannel = 8;
            int side = 0;
            switch (map.kind) {
                case MapKind::Height:
                    format.bitsPerChannel = 16;
                    format.exists = map.index == 0;
                    side = lattice.heightSide();
                    break;
                case MapKind::Hole:
                    format.exists = map.index == 0 && runtime.store().maps().hasHoles;
                    side = lattice.holeSide();
                    break;
                case MapKind::Density:
                    // a layer the tile has no map for reads 0, as the world's store does
                    format.exists = map.index >= 0;
                    side = lattice.densitySide();
                    break;
                case MapKind::Blend:
                    // foliage reads no paint
                    format.channels = 4;
                    side = lattice.blendSide();
                    break;
            }
            format.bounds = TexelRect{0, 0, side, side};
            return format;
        }

        bool readBytes(MapId map, TexelRect rect, std::span<uint8_t> out) const override {
            const MapFormat mapFormat = format(map);
            const int bytes = bytesPerTexel(mapFormat);
            if (!mapFormat.exists || rect.empty() || !mapFormat.bounds.contains(rect) || out.size() != static_cast<size_t>(rect.area()) * bytes) return false;
            const std::shared_ptr<const WorldTile> data = runtime.resident(coord);
            if (!data) return false;
            const TileMap* source = nullptr;
            uint8_t fill = 0;
            if (map.kind == MapKind::Height) {
                source = &data->height;
            } else if (map.kind == MapKind::Hole) {
                source = data->holes.empty() ? nullptr : &data->holes;
                fill = 255;
            } else if (map.index < static_cast<int>(data->density.size()) && !data->density[map.index].empty()) {
                source = &data->density[map.index];
            }
            if (!source) {
                std::memset(out.data(), fill, out.size());
                return true;
            }
            if (source->width != mapFormat.bounds.w || source->height != mapFormat.bounds.h || source->bytesPerTexel() != bytes) return false;
            for (int y = 0; y < rect.h; y++) {
                std::memcpy(out.data() + static_cast<size_t>(y) * rect.w * bytes, source->texel(rect.x, rect.y + y), static_cast<size_t>(rect.w) * bytes);
            }
            return true;
        }

        bool read(MapId map, TexelRect rect, std::span<float> out) const override {
            const MapFormat mapFormat = format(map);
            if (mapFormat.channels != 1 || out.size() != static_cast<size_t>(rect.area())) return false;
            std::vector<uint8_t> bytes(static_cast<size_t>(rect.area()) * bytesPerTexel(mapFormat));
            if (!readBytes(map, rect, bytes)) return false;
            for (size_t i = 0; i < out.size(); i++) {
                out[i] = mapFormat.bitsPerChannel >= 16 ? static_cast<float>(bytes[2 * i] | (bytes[2 * i + 1] << 8)) / 65535.0f : bytes[i] / 255.0f;
            }
            return true;
        }

        WriteResult write(MapId, TexelRect, std::span<const float>) override { return WriteResult{false, TILE_FOLIAGE_READ_ONLY, 0}; }
        WriteResult writeBytes(MapId, TexelRect, std::span<const uint8_t>) override { return WriteResult{false, TILE_FOLIAGE_READ_ONLY, 0}; }
        bool canWrite(MapId, TexelRect, std::string* why) const override {
            if (why) *why = TILE_FOLIAGE_READ_ONLY;
            return false;
        }
        WriteResult createMap(MapId, MapFormat, float) override { return WriteResult{false, TILE_FOLIAGE_READ_ONLY, 0}; }
        bool canCreateMap(MapId, std::string* why) const override {
            if (why) *why = TILE_FOLIAGE_READ_ONLY;
            return false;
        }
        void takeDirty(std::vector<std::pair<MapId, TexelRect>>&) override {}

        // the region's frame: centred on the tile
        Vector2 worldOfTexel(MapId map, int x, int y) const override {
            const WorldLattice& lattice = runtime.lattice();
            const float half = lattice.tileSize() * 0.5f;
            if (map.kind == MapKind::Height) {
                return Vector2(static_cast<float>(x - 1) * lattice.spacing - half, static_cast<float>(y - 1) * lattice.spacing - half);
            }
            const float texel = lattice.tileSize() / static_cast<float>(format(map).bounds.w - (map.kind == MapKind::Blend ? 1 : 0));
            const float centre = map.kind == MapKind::Blend ? 0.0f : 0.5f;
            return Vector2((static_cast<float>(x) + centre) * texel - half, (static_cast<float>(y) + centre) * texel - half);
        }

        Vector2 texelOfWorld(MapId map, float localX, float localZ) const override {
            const WorldLattice& lattice = runtime.lattice();
            const float half = lattice.tileSize() * 0.5f;
            if (map.kind == MapKind::Height) {
                return Vector2((localX + half) / lattice.spacing + 1.0f, (localZ + half) / lattice.spacing + 1.0f);
            }
            const float texel = lattice.tileSize() / static_cast<float>(format(map).bounds.w - (map.kind == MapKind::Blend ? 1 : 0));
            const float centre = map.kind == MapKind::Blend ? 0.0f : 0.5f;
            return Vector2((localX + half) / texel - centre, (localZ + half) / texel - centre);
        }

        TexelRect preferredBlock(MapId map) const override { return format(map).bounds; }
        std::unique_ptr<TerrainLargeEdit> beginLarge(MapId, TexelRect) override { return nullptr; }
        std::unique_ptr<TerrainStoreUndoHook> takeUndoHook() override { return nullptr; }

    private:
        TerrainWorldRuntime& runtime;
        TileCoord coord;
    };

    // The world's own query (R24), offset to the tile: a tree on a border leans by both tiles' samples
    class TileFoliageQuery final : public TerrainSurfaceQuery {
    public:
        TileFoliageQuery(TerrainWorldRuntime& runtime, float centreX, float centreZ): runtime(runtime), centreX(centreX), centreZ(centreZ) {}

        SurfaceSample sample(float localX, float localZ) const override { return runtime.query(localX + centreX, localZ + centreZ, false); }
        float heightAt(float localX, float localZ) const override { return sample(localX, localZ).height; }
        bool raycast(const Vector3& origin, const Vector3& direction, float maxT, bool skipHoles, Vector3& point, float& t) const override {
            const Vector3 offset(centreX, 0.0f, centreZ);
            if (!runtime.raycast(origin + offset, direction, maxT, skipHoles, false, point, t)) return false;
            point = point - offset;
            return true;
        }

    private:
        TerrainWorldRuntime& runtime;
        float centreX, centreZ;
    };

    bool worldTreeDecompress(const void* input, size_t inputBytes, void* output, size_t outputBytes) {
        const size_t written = ZSTD_decompress(output, outputBytes, input, inputBytes);
        return !ZSTD_isError(written) && written == outputBytes;
    }

    // One resident tile as T5's region. Its frame is centred on the tile, as a lone Terrain's is on its
    // own centre, so T5's cells (-S/16 .. S/16) cover the tile and no cell crosses a seam (S is a power
    // of two of at least 16 m, R2). Made and dropped by TerrainWorldSystem, never outliving its runtime
    // while FoliageSystem still reads it.
    class TileFoliageRegion final : public FoliageRegion {
    public:
        TileFoliageRegion(Scene* scene, Entity entity, TerrainWorldRuntime& runtime, TileCoord coord, bool hasTrees)
            : scene(scene), entity(entity), runtime(runtime), coord(coord),
              centreX((static_cast<float>(coord.x) + 0.5f) * runtime.lattice().tileSize()),
              centreZ((static_cast<float>(coord.z) + 0.5f) * runtime.lattice().tileSize()),
              tileMaps(runtime, coord), query(runtime, centreX, centreZ) {
            if (hasTrees) startTreeLoad(runtime.source().treePath(coord));
        }

        ~TileFoliageRegion() override {
            if (treesLoading) treeTask.cancel();
        }

        uint32_t regionKey() const override { return coord.key(); }
        TerrainMapStore& maps() override { return tileMaps; }
        const TerrainSurfaceQuery& surface() const override { return query; }

        std::shared_ptr<TreeStore> trees() override {
            pollTrees();
            return store;
        }

        // whole cells from the world's origin: a cell straddling no seam is seeded alike by either tile
        void cellOffset(int& x, int& z) const override {
            const int cells = static_cast<int>(runtime.lattice().tileSize() / FOLIAGE_DETAIL_CELL_SIZE);
            x = coord.x * cells + cells / 2;
            z = coord.z * cells + cells / 2;
        }

        Matrix4 localToWorld() const override {
            const Transform* transform = scene->findComponent<Transform>(entity);
            const Matrix4 model = transform ? transform->modelMatrix : Matrix4();
            return model * Matrix4::translateMatrix(Vector3(centreX, 0.0f, centreZ));
        }

        bool frame(Frame& out) const override {
            const WorldLattice& lattice = runtime.lattice();
            out.size = lattice.tileSize();
            out.heightMapSize = static_cast<float>(lattice.heightSide()) * lattice.spacing;
            out.maxHeight = runtime.maxHeight();
            out.treesLoading = treesLoading;
            return true;
        }

    private:
        void startTreeLoad(const std::string& path) {
            auto error = std::make_shared<std::string>();
            treeError = error;
            AssetIOService::ReadRequest request;
            request.path = path;
            request.priority = IOPriority::Normal;
            request.name = "terrain-world-trees";
            treeTask = AssetIOService::instance().submitRead(request, [error](std::vector<uint8_t>&& bytes) -> std::shared_ptr<TreeStore> {
                auto loaded = std::make_shared<TreeStore>();
                if (!loaded->decode(bytes, error.get(), worldTreeDecompress)) return nullptr;
                return loaded;
            });
            treesLoading = true;
        }

        void pollTrees() {
            if (!treesLoading || !treeTask.isReady()) return;
            treesLoading = false;
            if (treeTask.status() == jobs::TaskStatus::Completed) store = treeTask.get();
            treeTask = jobs::Task<std::shared_ptr<TreeStore>>();
            if (!store) {
                Log::error("Terrain World: the trees of tile (%i, %i) were refused: %s. The tile draws no trees.", coord.x, coord.z,
                           treeError->empty() ? "the file could not be read" : treeError->c_str());
            }
        }

        Scene* scene;
        Entity entity;
        TerrainWorldRuntime& runtime;
        TileCoord coord;
        float centreX, centreZ;
        TileFoliageMaps tileMaps;
        TileFoliageQuery query;
        std::shared_ptr<TreeStore> store;
        jobs::Task<std::shared_ptr<TreeStore>> treeTask;
        std::shared_ptr<std::string> treeError;
        bool treesLoading = false;
    };

    // R27: a tile's foliage starts when the tile draws: Resident, or in memory where nothing uploads (R15)
    bool foliageTileReady(const TerrainWorldRuntime& runtime, TileCoord tile) {
        const TileState state = runtime.tileState(tile);
        const bool drawn = state == TileState::Resident || (!runtime.tiles().budgets().gpu && state == TileState::CpuReady);
        return drawn && runtime.resident(tile) != nullptr;
    }

    bool worldTileHasTrees(const TerrainWorldRuntime& runtime, TileCoord tile) {
        for (const WorldTileInfo& info : runtime.manifest().tiles) {
            if (info.coord == tile) return info.trees;
        }
        return false;
    }

}

void TerrainWorldSystem::setHost(Host host) {
    worldHost() = std::move(host);
}

TerrainWorldSystem::TerrainWorldSystem(Scene* scene): SubSystem(scene) {
    declareHookType(scene->getComponentId<TerrainWorldComponent>());
    declareHookType(scene->getComponentId<TerrainComponent>());
    liveSystems().push_back(this);
}

TerrainWorldSystem::~TerrainWorldSystem() {
    std::vector<TerrainWorldSystem*>& systems = liveSystems();
    systems.erase(std::remove(systems.begin(), systems.end(), this), systems.end());
    for (auto it = focusTracks().begin(); it != focusTracks().end();) {
        it = it->first.first == scene ? focusTracks().erase(it) : std::next(it);
    }
}

void TerrainWorldSystem::load() {}

void TerrainWorldSystem::destroy() {
    std::vector<Entity> entities;
    for (const auto& [entity, runtime] : runtimes) entities.push_back(entity);
    for (Entity entity : entities) release(entity);
    errors.clear();
    areaCallbacks.clear();
}

void TerrainWorldSystem::draw() {}

void TerrainWorldSystem::onComponentRemoved(Entity entity, ComponentId componentId) {
    (void)componentId;
    release(entity);
}

void TerrainWorldSystem::syncFoliage(Entity entity, TerrainWorldRuntime& runtime) {
    const TerrainComponent* terrain = scene->findComponent<TerrainComponent>(entity);
    // R37: a world with no foliage layer and no tree prototype makes no region
    const bool wanted = terrain && (!terrain->foliageLayers.empty() || !terrain->treePrototypes.empty());
    auto found = foliageTiles.find(entity);
    if (!wanted) {
        if (found != foliageTiles.end()) releaseFoliage(entity);
        return;
    }
    FoliageSystem* foliage = scene->findSystem<FoliageSystem>().get();
    if (!foliage) return;

    if (found != foliageTiles.end()) {
        std::map<uint32_t, FoliageTile>& tiles = found->second;
        for (auto it = tiles.begin(); it != tiles.end();) {
            const TileCoord coord = TileCoord::fromKey(it->first);
            if (!foliageTileReady(runtime, coord)) {
                foliage->removeRegion(entity, it->first);
                it = tiles.erase(it);
                continue;
            }
            // an edit wrote the tile, or a neighbour's wrote its gutter: its cells go again
            const uint32_t revision = runtime.tileRevision(coord);
            if (revision != it->second.revision) {
                it->second.revision = revision;
                foliage->regionChanged(entity, it->first);
            }
            ++it;
        }
    }
    for (const TileCoord& coord : runtime.tiles().knownTiles()) {
        if (found != foliageTiles.end() && found->second.count(coord.key())) continue;
        if (!foliageTileReady(runtime, coord)) continue;
        auto region = std::make_shared<TileFoliageRegion>(scene, entity, runtime, coord, worldTileHasTrees(runtime, coord));
        foliage->addRegion(entity, region);
        if (found == foliageTiles.end()) found = foliageTiles.emplace(entity, std::map<uint32_t, FoliageTile>()).first;
        found->second[coord.key()] = FoliageTile{region, runtime.tileRevision(coord)};
    }
    if (found != foliageTiles.end() && found->second.empty()) foliageTiles.erase(found);
}

void TerrainWorldSystem::releaseFoliage(Entity entity) {
    const auto found = foliageTiles.find(entity);
    if (found == foliageTiles.end()) return;
    if (FoliageSystem* foliage = scene->findSystem<FoliageSystem>().get()) {
        for (const auto& [key, tile] : found->second) foliage->removeRegion(entity, key);
    }
    foliageTiles.erase(found);
}

void TerrainWorldSystem::release(Entity entity) {
    const auto it = runtimes.find(entity);
    if (it == runtimes.end()) return;
    // the regions read the runtime: they go first
    releaseFoliage(entity);
    if (worldHost().released) worldHost().released(scene, entity, *it->second);
    if (TerrainComponent* terrain = scene->findComponent<TerrainComponent>(entity)) {
        if (terrain->world == it->second.get()) {
            terrain->world = nullptr;
            terrain->needUpdateTerrain = true;
        }
    }
    for (auto callback = areaCallbacks.begin(); callback != areaCallbacks.end();) {
        callback = callback->entity == entity ? areaCallbacks.erase(callback) : std::next(callback);
    }
    runtimes.erase(it);
}

void TerrainWorldSystem::restart(Entity entity) {
    release(entity);
    errors.erase(entity);
}

std::string TerrainWorldSystem::getError(Entity entity) const {
    const auto it = errors.find(entity);
    return it == errors.end() ? std::string() : it->second;
}

TerrainWorldRuntime* TerrainWorldSystem::getRuntime(Entity entity) {
    const auto it = runtimes.find(entity);
    return it == runtimes.end() ? nullptr : it->second.get();
}

TerrainWorldRuntime* TerrainWorldSystem::ensureRuntime(Entity entity, std::string* error) {
    if (TerrainWorldRuntime* runtime = getRuntime(entity)) return runtime;
    syncRuntimes();
    if (TerrainWorldRuntime* runtime = getRuntime(entity)) return runtime;
    if (error) {
        const std::string why = getError(entity);
        *error = why.empty() ? "the GameObject is not a Terrain World (it needs a Terrain and an enabled Terrain World)" : why;
    }
    return nullptr;
}

void TerrainWorldSystem::syncRuntimes() {
    auto worlds = scene->getComponentArray<TerrainWorldComponent>();
    // R37: a scene with no world stops here
    if (worlds->size() == 0 && runtimes.empty()) return;

    const Host& host = worldHost();
    // a scene the editor plays in place restarts its worlds: Play caps edits and waits differently (R29, R19)
    const bool playingNow = host.playing ? host.playing(scene) : true;
    std::vector<Entity> stale;
    for (const auto& [entity, runtime] : runtimes) {
        const TerrainWorldComponent* world = scene->findComponent<TerrainWorldComponent>(entity);
        if (!world || !world->enabled || !scene->findComponent<TerrainComponent>(entity) || runtime->playing() != playingNow) {
            stale.push_back(entity);
        }
    }
    for (Entity entity : stale) release(entity);

    for (int i = 0; i < worlds->size(); i++) {
        const TerrainWorldComponent& world = worlds->getComponentFromIndex(i);
        const Entity entity = worlds->getEntity(i);
        TerrainComponent* terrain = scene->findComponent<TerrainComponent>(entity);
        if (!world.enabled || !terrain || runtimes.count(entity) || errors.count(entity)) continue;

        std::shared_ptr<TerrainWorldSource> source;
        if (host.makeSource) {
            source = host.makeSource(scene, entity, world);
        } else if (!world.worldData.empty()) {
            source = std::make_shared<CookedWorldSource>(world.worldData.path);
        }
        auto runtime = std::make_unique<TerrainWorldRuntime>(scene, entity, source, playingNow);
        std::string error;
        if (!runtime->start(error)) {
            Log::error("Terrain World '%s' cannot start: %s", scene->getEntityName(entity).c_str(), error.c_str());
            errors[entity] = error;
            continue;
        }
        runtime->collision = std::make_unique<TerrainWorldCollision>(*runtime);
        terrain->world = runtime.get();
        // its grids are skirted and it has no node tree (MeshSystem::createTerrain)
        terrain->needUpdateTerrain = true;
        TerrainWorldRuntime* made = runtime.get();
        runtimes[entity] = std::move(runtime);
        if (host.started) host.started(scene, entity, *made);
    }
}

void TerrainWorldSystem::updateFocuses(TerrainWorldRuntime& runtime, const TerrainWorldComponent& component) {
    Vector3 origin;
    if (const Transform* transform = scene->findComponent<Transform>(runtime.getEntity())) origin = transform->worldPosition;
    const double now = Engine::getRealtimeSinceStartup();
    std::vector<StreamFocus> focuses;
    auto follow = [&](Entity entity, int id) {
        const Transform* transform = scene->findComponent<Transform>(entity);
        if (!transform) return;
        StreamFocus focus;
        focus.id = id;
        focus.x = transform->worldPosition.x - origin.x;
        focus.z = transform->worldPosition.z - origin.z;
        FocusTrack& track = focusTracks()[{scene, entity}];
        if (track.at >= 0.0 && now > track.at) {
            const float dt = static_cast<float>(now - track.at);
            focus.velocityX = (transform->worldPosition.x - track.last.x) / dt;
            focus.velocityZ = (transform->worldPosition.z - track.last.z) / dt;
            // a teleport is no velocity
            if (std::fabs(focus.velocityX) > 500.0f || std::fabs(focus.velocityZ) > 500.0f) focus.velocityX = focus.velocityZ = 0.0f;
        }
        track.last = transform->worldPosition;
        track.at = now;
        focuses.push_back(focus);
    };
    if (component.useCameraFocus && scene->getCamera() != NULL_ENTITY) follow(scene->getCamera(), -1);
    if (component.focus != NULL_ENTITY && scene->isEntityCreated(component.focus)) follow(component.focus, -2);
    runtime.setAutoFocuses(focuses);
}

void TerrainWorldSystem::forEachRuntime(const std::function<void(TerrainWorldRuntime& runtime)>& fn) {
    for (TerrainWorldSystem* system : liveSystems()) {
        for (auto& [entity, runtime] : system->runtimes) fn(*runtime);
    }
}

void TerrainWorldSystem::pumpAll() {
    // editor views draw a frame several times: the pump runs once (D9)
    const double now = Engine::getRealtimeSinceStartup();
    if (lastPumpAll() >= 0.0 && now - lastPumpAll() < 0.001) return;
    lastPumpAll() = now;
    for (TerrainWorldSystem* system : liveSystems()) system->pump(now);
}

void TerrainWorldSystem::pump(double now) {
    syncRuntimes();
    if (runtimes.empty()) return;
    JE_PROFILE_SCOPE(PROFILE_SCOPE);
    lastPump = now;
    for (auto& [entity, runtime] : runtimes) {
        const TerrainWorldComponent* component = scene->findComponent<TerrainWorldComponent>(entity);
        if (!component) continue;

        // R4: a world translates only
        if (Transform* transform = scene->findComponent<Transform>(entity)) {
            const bool rotated = transform->rotation != Quaternion();
            const bool scaled = transform->scale != Vector3(1.0f, 1.0f, 1.0f);
            if (rotated || scaled) {
                Log::warn("Terrain World '%s' translates only: its rotation and scale are set back to identity.", scene->getEntityName(entity).c_str());
                transform->rotation = Quaternion();
                transform->scale = Vector3(1.0f, 1.0f, 1.0f);
                transform->needUpdate = true;
            }
        }

        const WorldPlatformBudget budget = terrainWorldPlatformBudget();
        runtime->applySettings(component->poolSize, component->lodDistance, component->uploadBudget, budget.nearTiles);
        // R29: brushes, edit_terrain and generation make tiles; a game's Lua edits do not
        runtime->store().options().autoAddTiles = component->autoAddTiles && !runtime->playing();
        updateFocuses(*runtime, *component);
        const uint32_t overviewBefore = runtime->overviewRevision();
        const int residentBefore = runtime->tiles().stats().residentPages;
        runtime->pump(now);
        if (runtime->collision) {
            const float radius = component->collisionRadius > 0.0f ? component->collisionRadius : budget.collisionRadius;
            runtime->collision->pump(radius, component->collisionChunkQuads);
        }
        syncFoliage(entity, *runtime);
        // D9: what draws changed, so the selection runs again this frame
        const bool pagesChanged = runtime->render && runtime->render->takeReselect();
        if (pagesChanged || runtime->overviewRevision() != overviewBefore || runtime->tiles().stats().residentPages != residentBefore) {
            if (Transform* transform = scene->findComponent<Transform>(entity)) transform->needUpdate = true;
        }
    }
}

int TerrainWorldSystem::onAreaReady(Entity entity, float x, float z, float radius, TerrainWorldReadyLevel level, std::function<void()> callback) {
    TerrainWorldRuntime* runtime = ensureRuntime(entity);
    if (!runtime || !callback) return 0;
    AreaCallback entry;
    entry.id = nextCallbackId++;
    entry.entity = entity;
    entry.wait = runtime->waitForArea(x, z, radius, level);
    entry.x = x;
    entry.z = z;
    entry.radius = radius;
    entry.level = level;
    entry.callback = std::move(callback);
    areaCallbacks.push_back(std::move(entry));
    return areaCallbacks.back().id;
}

void TerrainWorldSystem::cancelAreaReady(int id) {
    for (auto it = areaCallbacks.begin(); it != areaCallbacks.end(); ++it) {
        if (it->id != id) continue;
        if (TerrainWorldRuntime* runtime = getRuntime(it->entity)) runtime->dropAreaWait(it->wait);
        areaCallbacks.erase(it);
        return;
    }
}

void TerrainWorldSystem::update(double dt) {
    (void)dt;
    if (areaCallbacks.empty()) return;
    // R18: in the update phase, so a paused game hears of it on resume
    std::vector<AreaCallback> ready;
    for (auto it = areaCallbacks.begin(); it != areaCallbacks.end();) {
        TerrainWorldRuntime* runtime = getRuntime(it->entity);
        if (runtime && runtime->isAreaReady(it->x, it->z, it->radius, it->level)) {
            runtime->dropAreaWait(it->wait);
            ready.push_back(std::move(*it));
            it = areaCallbacks.erase(it);
        } else {
            ++it;
        }
    }
    for (AreaCallback& callback : ready) callback.callback();
}
