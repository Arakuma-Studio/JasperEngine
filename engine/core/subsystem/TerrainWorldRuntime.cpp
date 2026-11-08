// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "subsystem/TerrainWorldRuntime.h"

#include "Engine.h"
#include "io/AssetIOService.h"
#include "io/FileData.h"
#include "physics/TerrainWorldCollision.h"
#include "subsystem/TerrainWorldRender.h"
#include "Log.h"

#include "zstd.h"

#include <algorithm>
#include <chrono>
#include <cmath>

using namespace jasperengine;
using namespace jasperengine::terrain;

namespace {

    IOPriority ioPriorityOf(TilePriority priority) {
        switch (priority) {
            case TilePriority::Critical: return IOPriority::Critical;
            case TilePriority::High: return IOPriority::High;
            case TilePriority::Normal: return IOPriority::Normal;
        }
        return IOPriority::Normal;
    }

    // A decoded tile that is not the one asked for, or of another lattice, is an error (R14)
    bool tileMatches(const WorldTile& data, TileCoord tile, const WorldLattice& lattice, std::string& error) {
        if (data.coord != tile) {
            error = "the file holds tile (" + std::to_string(data.coord.x) + ", " + std::to_string(data.coord.z) + ")";
            return false;
        }
        if (data.tileQuads != lattice.tileQuads || data.spacing != lattice.spacing) {
            error = "the tile's lattice is not the world's";
            return false;
        }
        return true;
    }

    // R17: reads through AssetIOService, decodes in its Background continuation (Tag::Asset), polled
    class AssetTileIO final : public TileIO {
    public:
        AssetTileIO(const TerrainWorldSource& source, const WorldLattice& lattice): source(source), lattice(lattice) {}

        ~AssetTileIO() override {
            for (auto& entry : loads) cancelLoad(entry.second);
        }

        uint64_t begin(TileCoord tile, TilePriority priority) override {
            Load load;
            load.path = source.tilePath(tile);
            load.error = std::make_shared<std::string>();
            const std::shared_ptr<std::string> error = load.error;
            const WorldLattice expected = lattice;
            const auto decode = [error, tile, expected](std::vector<uint8_t>&& bytes) -> std::shared_ptr<WorldTile> {
                auto data = std::make_shared<WorldTile>();
                const TileError result = decodeTile(bytes, terrainWorldDecodeCodec(), *data);
                if (result != TileError::None) {
                    *error = std::string("the tile is ") + tileErrorName(result);
                    return nullptr;
                }
                std::string why;
                if (!tileMatches(*data, tile, expected, why)) {
                    *error = why;
                    return nullptr;
                }
                return data;
            };
            AssetIOService::ReadRequest request;
            request.path = load.path;
            request.priority = ioPriorityOf(priority);
            request.cancel = load.cancel;
            request.name = "terrain-world-tile";
            request.onFailure = [error](const std::string& message) {
                if (!AssetIOService::isCancellation(message)) *error = message;
            };
            load.task = AssetIOService::instance().submitRead(request, decode);
            const uint64_t handle = ++next;
            loads.emplace(handle, std::move(load));
            return handle;
        }

        TileLoadStatus poll(uint64_t handle, std::shared_ptr<WorldTile>& data, std::string& error) override {
            const auto it = loads.find(handle);
            if (it == loads.end()) {
                error = "unknown load";
                return TileLoadStatus::Failed;
            }
            Load& load = it->second;
            if (!load.task.isReady()) return TileLoadStatus::Pending;
            if (load.task.status() == jobs::TaskStatus::Completed) data = load.task.get();
            error = load.error->empty() ? std::string("the file could not be read: ") + load.path : *load.error;
            loads.erase(it);
            return data ? TileLoadStatus::Done : TileLoadStatus::Failed;
        }

        void cancel(uint64_t handle) override {
            const auto it = loads.find(handle);
            if (it == loads.end()) return;
            cancelLoad(it->second);
            loads.erase(it);
        }

    private:
        struct Load {
            std::string path;
            jobs::CancelToken cancel;
            std::shared_ptr<std::string> error;
            jobs::Task<std::shared_ptr<WorldTile>> task;
        };

        void cancelLoad(Load& load) {
            load.cancel.cancel();
            load.task.cancel();
            AssetIOService::instance().cancel(load.path, load.cancel);
        }

        const TerrainWorldSource& source;
        WorldLattice lattice;
        std::map<uint64_t, Load> loads;
        uint64_t next = 0;
    };

    // R52: the overview from the tiles' own OVRH and OVRC, over the box of the manifest's tiles
    std::shared_ptr<WorldOverview> assembleOverview(const TerrainWorldSource& source, const WorldManifest& manifest) {
        auto overview = std::make_shared<WorldOverview>();
        if (manifest.tiles.empty()) return overview;
        TileCoord low = manifest.tiles.front().coord, high = low;
        for (const WorldTileInfo& info : manifest.tiles) {
            low = TileCoord(std::min(low.x, info.coord.x), std::min(low.z, info.coord.z));
            high = TileCoord(std::max(high.x, info.coord.x), std::max(high.z, info.coord.z));
        }
        initOverview(*overview, manifest.lattice, low, high, manifest.maps.baseHeight);
        const uint32_t only[] = {TILE_SECTION_OVERVIEW_HEIGHT, TILE_SECTION_OVERVIEW_COLOUR, TILE_SECTION_EDGE};
        const TileCodec codec = terrainWorldDecodeCodec();
        for (const WorldTileInfo& info : manifest.tiles) {
            std::vector<uint8_t> bytes;
            std::string error;
            if (!AssetIOService::readFile(source.tilePath(info.coord), 0, 0, bytes, error)) continue;
            WorldTile data;
            if (decodeTile(bytes, codec, data, only) != TileError::None) continue;
            data.coord = info.coord;
            setOverviewPatch(*overview, data);
        }
        return overview;
    }

    float boxDistance(float x, float z, float minX, float minZ, float size) {
        const float dx = x < minX ? minX - x : (x > minX + size ? x - minX - size : 0.0f);
        const float dz = z < minZ ? minZ - z : (z > minZ + size ? z - minZ - size : 0.0f);
        return std::sqrt(dx * dx + dz * dz);
    }

    // T1's sampleSurface over one tile's samples, gutter included: N + 3 texels over (N + 3) h centred
    // on the tile's centre puts texel 1 exactly on the tile's corner (R24)
    SurfaceSample sampleTile(const WorldTile& data, const WorldLattice& lattice, float maxHeight, float localX, float localZ) {
        const float size = lattice.tileSize();
        const float originX = static_cast<float>(data.coord.x) * size, originZ = static_cast<float>(data.coord.z) * size;
        SurfaceView view;
        view.pixels = data.height.bytes.data();
        view.width = data.height.width;
        view.height = data.height.height;
        view.channels = 1;
        view.bytesPerChannel = 2;
        view.size = static_cast<float>(data.height.width) * lattice.spacing;
        view.maxHeight = maxHeight;
        SurfaceSample sample = sampleSurface(view, localX - (originX + size * 0.5f), localZ - (originZ + size * 0.5f));
        sample.inside = true;
        sample.present = true;
        sample.exact = true;
        if (!data.holes.empty()) {
            const HoleMapView holes{data.holes.bytes.data(), data.holes.width, data.holes.height};
            sample.hole = isHole(holes, (localX - originX) / size, (localZ - originZ) / size);
        }
        return sample;
    }

}

// ------------------------------------------------------------------ sources

std::string jasperengine::terrainWorldTileName(TileCoord tile) {
    return "tiles/" + std::to_string(tile.x) + "_" + std::to_string(tile.z) + ".jtile";
}

TileCodec jasperengine::terrainWorldDecodeCodec() {
    TileCodec codec;
    codec.decompress = [](std::span<const uint8_t> in, std::span<uint8_t> out) {
        const size_t written = ZSTD_decompress(out.data(), out.size(), in.data(), in.size());
        return !ZSTD_isError(written) && written == out.size();
    };
    return codec;
}

WorldPlatformBudget jasperengine::terrainWorldPlatformBudget() {
#if defined(__EMSCRIPTEN__)
#if defined(__EMSCRIPTEN_PTHREADS__)
    return worldPlatformBudget(WorldPlatform::Web);
#else
    return worldPlatformBudget(WorldPlatform::WebSingleThread);
#endif
#elif defined(__ANDROID__) || (defined(__APPLE__) && defined(JASPERENGINE_IOS))
    return worldPlatformBudget(WorldPlatform::Mobile);
#else
    return worldPlatformBudget(WorldPlatform::Desktop);
#endif
}

const char* jasperengine::terrainWorldPointStateName(TerrainWorldPointState state) {
    switch (state) {
        case TerrainWorldPointState::Absent: return "absent";
        case TerrainWorldPointState::Overview: return "overview";
        case TerrainWorldPointState::Full: return "full";
    }
    return "absent";
}

bool CookedWorldSource::manifest(WorldManifest& out, std::string& error) {
    std::vector<uint8_t> bytes;
    if (!AssetIOService::readFile(folder + "/world.bin", 0, 0, bytes, error)) return false;
    return decodeWorldBin(bytes, out, &error);
}

std::string CookedWorldSource::tilePath(TileCoord tile) const {
    return folder + "/" + terrainWorldTileName(tile);
}

std::string CookedWorldSource::overviewPath() const {
    return folder + "/overview.jtile";
}

// ------------------------------------------------------------------ runtime

TerrainWorldRuntime::TerrainWorldRuntime(Scene* scene, Entity entity, std::shared_ptr<TerrainWorldSource> source, bool playing)
    : scene(scene), entity(entity), worldSource(std::move(source)), isPlaying(playing) {}

TerrainWorldRuntime::~TerrainWorldRuntime() {
    // render and collision release what they hold before the streamer goes
    collision.reset();
    render.reset();
    if (overviewLoading) overviewTask.cancel();
}

bool TerrainWorldRuntime::start(std::string& error) {
    if (!worldSource) {
        error = "the Terrain World has no data";
        return false;
    }
    if (!worldSource->manifest(worldManifest, error)) return false;
    std::string why;
    if (!validateLattice(worldManifest.lattice, &why)) {
        error = why;
        return false;
    }
    const WorldPlatformBudget budget = terrainWorldPlatformBudget();
    if (worldManifest.lattice.tileQuads > budget.maxTileQuads) {
        error = "tile_quads " + std::to_string(worldManifest.lattice.tileQuads) + " is past this platform's limit of " + std::to_string(budget.maxTileQuads);
        return false;
    }
    for (const WorldTileInfo& info : worldManifest.tiles) worldOccupancy.setTile(info.coord, true, info.minHeight, info.maxHeight);

    io = std::make_unique<AssetTileIO>(*worldSource, worldManifest.lattice);
    streamer = std::make_unique<TileStreamer>(*io, worldOccupancy);
    streamer->setLattice(worldManifest.lattice);
    StreamBudgets& limits = streamer->budgets();
    limits.poolSize = budget.poolSize;
    limits.gpuCapBytes = budget.gpuCapBytes;
    limits.cpuCapBytes = budget.cpuCapBytes;
    limits.uploadBytesPerFrame = budget.uploadBytes;
    limits.maxInFlight = budget.inFlight;
    limits.lodDistance = budget.lodDistance;
    limits.gpu = !Engine::isHeadless();

    TiledStoreOptions options;
    options.maxBoxTiles = budget.maxBoxTiles;
    mapStore = std::make_unique<TiledMapStore>(worldManifest.lattice, worldManifest.maps, *this, options);

    statsCache.poolSize = limits.poolSize;
    startOverviewLoad();
    return true;
}

void TerrainWorldRuntime::applySettings(int poolSize, float lodDistance, float uploadBudgetMiB, int maxNearTiles) {
    if (!streamer) return;
    const WorldPlatformBudget budget = terrainWorldPlatformBudget();
    StreamBudgets& limits = streamer->budgets();
    limits.poolSize = poolSize > 0 ? poolSize : budget.poolSize;
    limits.lodDistance = lodDistance > 0.0f ? lodDistance : budget.lodDistance;
    limits.uploadBytesPerFrame = uploadBudgetMiB > 0.0f ? static_cast<uint64_t>(uploadBudgetMiB * 1048576.0f) : budget.uploadBytes;
    (void)maxNearTiles;
    statsCache.poolSize = limits.poolSize;
}

void TerrainWorldRuntime::setFrozen(bool frozen) {
    if (streamer) streamer->setFrozen(frozen);
}

void TerrainWorldRuntime::startOverviewLoad() {
    if (worldManifest.tiles.empty()) {
        // nothing to draw: ready at once, with an empty overview over no box
        overview = WorldOverview();
        overviewRev++;
        overviewDone = true;
        return;
    }
    const std::string path = worldSource->overviewPath();
    const WorldManifest manifest = worldManifest;
    std::shared_ptr<TerrainWorldSource> source = worldSource;
    // D10: Tag::Asset jobs, so the scene's load wait includes the overview
    if (!path.empty()) {
        const auto decode = [source, manifest](std::vector<uint8_t>&& bytes) -> std::shared_ptr<WorldOverview> {
            auto loaded = std::make_shared<WorldOverview>();
            if (decodeOverview(bytes, terrainWorldDecodeCodec(), *loaded) == TileError::None) return loaded;
            return assembleOverview(*source, manifest);
        };
        AssetIOService::ReadRequest request;
        request.path = path;
        request.priority = IOPriority::Critical;
        request.name = "terrain-world-overview";
        overviewTask = AssetIOService::instance().submitRead(request, decode);
    } else if (jobs::hasSystem()) {
        jobs::SubmitOptions options{jobs::Lane::Background, jobs::GateClass::Default, "terrain-world-overview"};
        options.tags = jobs::Tag::Asset;
        overviewTask = jobs::submit([source, manifest]() { return assembleOverview(*source, manifest); }, options);
    } else {
        overview = *assembleOverview(*source, manifest);
        overviewRev++;
        overviewUnsaved = true;
        overviewDone = true;
        return;
    }
    overviewLoading = true;
}

void TerrainWorldRuntime::pollOverview() {
    if (!overviewLoading || !overviewTask.isReady()) return;
    overviewLoading = false;
    std::shared_ptr<WorldOverview> loaded;
    if (overviewTask.status() == jobs::TaskStatus::Completed) loaded = overviewTask.get();
    overviewTask = jobs::Task<std::shared_ptr<WorldOverview>>();
    if (!loaded || loaded->empty()) {
        // a missing or unreadable cache: from the tiles, now
        loaded = assembleOverview(*worldSource, worldManifest);
        overviewUnsaved = true;
    }
    // what the file did not know: the tiles edited or made since
    TileCoord low, high;
    if (worldOccupancy.bounds(low, high) && (!loaded->covers(low) || !loaded->covers(high))) {
        WorldOverview grown;
        initOverview(grown, worldManifest.lattice, TileCoord(std::min(low.x, loaded->low.x), std::min(low.z, loaded->low.z)),
                     TileCoord(std::max(high.x, static_cast<int16_t>(loaded->low.x + loaded->tilesX - 1)),
                               std::max(high.z, static_cast<int16_t>(loaded->low.z + loaded->tilesZ - 1))),
                     worldManifest.maps.baseHeight);
        for (int j = 0; j < loaded->height(); j++) {
            for (int i = 0; i < loaded->width(); i++) {
                const int x = (loaded->low.x - grown.low.x) * grown.stride + i, z = (loaded->low.z - grown.low.z) * grown.stride + j;
                grown.heights[static_cast<size_t>(z) * grown.width() + x] = loaded->heights[static_cast<size_t>(j) * loaded->width() + i];
                std::copy_n(loaded->colour.data() + (static_cast<size_t>(j) * loaded->width() + i) * 4, 4,
                            grown.colour.data() + (static_cast<size_t>(z) * grown.width() + x) * 4);
            }
        }
        grown.skirtDepth = loaded->skirtDepth;
        *loaded = std::move(grown);
        overviewUnsaved = true;
    }
    overview = std::move(*loaded);
    for (const auto& [key, data] : pinned) {
        if (setOverviewPatch(overview, *data)) overviewUnsaved = true;
    }
    overviewRev++;
    overviewDone = true;
}

void TerrainWorldRuntime::refreshOverview(double now) {
    if (!overviewPatchPending || overviewLoading) return;
    if (now < overviewPatchAt) return;
    overviewPatchPending = false;
    TileCoord low, high;
    if (!worldOccupancy.bounds(low, high)) {
        overview = WorldOverview();
        overviewRev++;
        return;
    }
    if (overview.empty() || !overview.covers(low) || !overview.covers(high)) {
        // R52: a tile past the box: the overview again over the new box
        WorldOverview grown;
        initOverview(grown, worldManifest.lattice, low, high, worldManifest.maps.baseHeight);
        if (!overview.empty()) {
            for (int j = 0; j < overview.height(); j++) {
                for (int i = 0; i < overview.width(); i++) {
                    const int x = (overview.low.x - low.x) * grown.stride + i, z = (overview.low.z - low.z) * grown.stride + j;
                    if (x < 0 || z < 0 || x >= grown.width() || z >= grown.height()) continue;
                    grown.heights[static_cast<size_t>(z) * grown.width() + x] = overview.heights[static_cast<size_t>(j) * overview.width() + i];
                    std::copy_n(overview.colour.data() + (static_cast<size_t>(j) * overview.width() + i) * 4, 4,
                                grown.colour.data() + (static_cast<size_t>(z) * grown.width() + x) * 4);
                }
            }
        }
        overview = std::move(grown);
    }
    for (const auto& [key, data] : pinned) setOverviewPatch(overview, *data);
    overviewRev++;
    overviewUnsaved = true;
}

void TerrainWorldRuntime::rebuildOverview(bool wait) {
    if (overviewLoading) overviewTask.cancel();
    overviewLoading = false;
    WorldManifest manifest = worldManifest;
    manifest.tiles.clear();
    for (const TileCoord& tile : worldOccupancy.presentTiles()) {
        const TileMinMax heights = worldOccupancy.minMax(0, tile.x, tile.z);
        manifest.tiles.push_back(WorldTileInfo{tile, heights.min, heights.max});
    }
    if (wait || !jobs::hasSystem()) {
        overview = *assembleOverview(*worldSource, manifest);
        for (const auto& [key, data] : pinned) setOverviewPatch(overview, *data);
        overviewRev++;
        overviewUnsaved = true;
        return;
    }
    std::shared_ptr<TerrainWorldSource> source = worldSource;
    jobs::SubmitOptions options{jobs::Lane::Background, jobs::GateClass::Default, "terrain-world-overview"};
    options.tags = jobs::Tag::Asset;
    overviewTask = jobs::submit([source, manifest]() { return assembleOverview(*source, manifest); }, options);
    overviewLoading = true;
}

void TerrainWorldRuntime::setLayerColours(std::vector<float> colours, int count) {
    layerColours = std::move(colours);
    layerCount = count;
}

void TerrainWorldRuntime::rebuildDerived(TileCoord tile, WorldTile& data) {
    buildMinMax(data, worldManifest.lattice);
    buildOverviewHeight(data, worldManifest.lattice);
    buildSkirtDepth(data, worldManifest.lattice);
    if (!layerColours.empty()) buildOverviewColour(data, worldManifest.lattice, layerColours, layerCount);
    const TileMinMax whole = tileNodeMinMax(data, worldManifest.lattice, worldManifest.lattice.tileLevel(), 0, 0);
    worldOccupancy.setTile(tile, true, whole.min, whole.max);
    worldManifest.skirtDepth = std::max(worldManifest.skirtDepth, data.skirtDepth);
}

void TerrainWorldRuntime::pump(double now) {
    std::lock_guard<std::recursive_mutex> guard(lock);
    if (!streamer) return;
    const auto started = std::chrono::steady_clock::now();
    lastNow = now;

    pollOverview();

    // R15, R18: every focus, then every pending area as a demand
    std::vector<StreamFocus> focuses = autoFocuses;
    for (const ScriptFocus& focus : scriptFocuses) focuses.push_back(focus.focus);
    streamer->setFocuses(focuses);
    const float size = worldManifest.lattice.tileSize();
    for (const TerrainWorldAreaWait& wait : areaWaits) {
        for (const TileCoord& tile : missingTiles(wait.x, wait.z, wait.radius, wait.level)) {
            const bool inside = tileOfLocal(wait.x, size) == tile.x && tileOfLocal(wait.z, size) == tile.z;
            streamer->demand(tile, inside ? TilePriority::Critical : TilePriority::High);
        }
    }
    streamer->update(now);

    for (const TileStreamEvent& event : streamer->takeEvents()) {
        switch (event.kind) {
            case TileStreamEvent::Kind::CpuReady: {
                revisions[event.tile.key()]++;
                // R51: borders against resident neighbours, one warning a tile
                const int errors = checkSeams(event.tile, false);
                if (errors > 0) {
                    statsCache.seamErrors += errors;
                    if (seamWarned.insert(event.tile.key()).second) {
                        Log::warn("Terrain World: tile (%i, %i) has %i border samples that differ from its neighbours'. Repair Seams resolves them.",
                                  event.tile.x, event.tile.z, errors);
                    }
                }
                break;
            }
            case TileStreamEvent::Kind::PageEvicted:
                break;
            case TileStreamEvent::Kind::Error:
                // R14: once a tile; it falls back to the overview (R10)
                Log::error("Terrain World: tile (%i, %i) could not load: %s", event.tile.x, event.tile.z, event.message.c_str());
                statsCache.errors.push_back("tile (" + std::to_string(event.tile.x) + ", " + std::to_string(event.tile.z) + "): " + event.message);
                if (statsCache.errors.size() > 32) statsCache.errors.erase(statsCache.errors.begin());
                break;
            case TileStreamEvent::Kind::CpuEvicted:
            case TileStreamEvent::Kind::Cancelled:
                break;
        }
    }

    // the derived sections of what was edited, once a pump
    settleEdits();
    refreshOverview(now);

    const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started).count();
    statsCache.mainThreadMs = ms;
    statsCache.worstMainThreadMs = std::max(statsCache.worstMainThreadMs, ms);
}

void TerrainWorldRuntime::settleEdits() {
    std::lock_guard<std::recursive_mutex> guard(lock);
    if (derivedDirty.empty()) return;
    for (uint32_t key : derivedDirty) {
        const TileCoord tile = TileCoord::fromKey(key);
        const auto it = pinned.find(key);
        if (it == pinned.end()) continue;
        rebuildDerived(tile, *it->second);
        streamer->replace(tile, it->second);
        revisions[key]++;
    }
    derivedDirty.clear();
    // R52: the overview follows at most this often
    overviewPatchPending = true;
    overviewPatchAt = lastNow + (isPlaying ? OVERVIEW_REFRESH_PLAY : OVERVIEW_REFRESH_EDITOR);
}

void TerrainWorldRuntime::setAutoFocuses(const std::vector<StreamFocus>& focuses) {
    autoFocuses = focuses;
}

int TerrainWorldRuntime::addFocus(float x, float z, float radiusScale, float velocityX, float velocityZ) {
    if (focusCount() >= WORLD_MAX_FOCUSES) return 0;
    ScriptFocus focus;
    focus.id = nextFocusId++;
    focus.focus.id = focus.id;
    focus.focus.x = x;
    focus.focus.z = z;
    focus.focus.radiusScale = radiusScale > 0.0f ? radiusScale : 1.0f;
    focus.focus.velocityX = velocityX;
    focus.focus.velocityZ = velocityZ;
    scriptFocuses.push_back(focus);
    return focus.id;
}

bool TerrainWorldRuntime::setFocus(int id, float x, float z, float velocityX, float velocityZ) {
    for (ScriptFocus& focus : scriptFocuses) {
        if (focus.id != id) continue;
        focus.focus.x = x;
        focus.focus.z = z;
        focus.focus.velocityX = velocityX;
        focus.focus.velocityZ = velocityZ;
        return true;
    }
    return false;
}

bool TerrainWorldRuntime::removeFocus(int id) {
    const size_t before = scriptFocuses.size();
    scriptFocuses.erase(std::remove_if(scriptFocuses.begin(), scriptFocuses.end(), [id](const ScriptFocus& focus) { return focus.id == id; }),
                        scriptFocuses.end());
    return scriptFocuses.size() != before;
}

std::vector<TileCoord> TerrainWorldRuntime::missingTiles(float x, float z, float radius, TerrainWorldReadyLevel level) const {
    std::vector<TileCoord> missing;
    const float size = worldManifest.lattice.tileSize();
    const int reach = static_cast<int>(std::ceil(radius / size)) + 1;
    const int cx = tileOfLocal(x, size), cz = tileOfLocal(z, size);
    for (int tz = cz - reach; tz <= cz + reach; tz++) {
        for (int tx = cx - reach; tx <= cx + reach; tx++) {
            const TileCoord tile(tx, tz);
            if (!tile.addressable() || !worldOccupancy.present(tile)) continue;
            if (boxDistance(x, z, static_cast<float>(tx) * size, static_cast<float>(tz) * size, size) > radius) continue;
            const TileState state = tileState(tile);
            bool ready = false;
            if (level == TerrainWorldReadyLevel::Render) {
                ready = state == TileState::Resident || (!streamer->budgets().gpu && state == TileState::CpuReady);
            } else {
                const bool cpu = state == TileState::CpuReady || state == TileState::Uploading || state == TileState::Resident;
                ready = cpu && (!collision || collision->fineActive(tile));
            }
            if (!ready) missing.push_back(tile);
        }
    }
    return missing;
}

bool TerrainWorldRuntime::isAreaReady(float x, float z, float radius, TerrainWorldReadyLevel level) const {
    return streamer && isReady() && missingTiles(x, z, radius, level).empty();
}

int TerrainWorldRuntime::waitForArea(float x, float z, float radius, TerrainWorldReadyLevel level) {
    TerrainWorldAreaWait wait;
    wait.id = nextWaitId++;
    wait.x = x;
    wait.z = z;
    wait.radius = radius;
    wait.level = level;
    areaWaits.push_back(wait);
    return wait.id;
}

void TerrainWorldRuntime::dropAreaWait(int id) {
    areaWaits.erase(std::remove_if(areaWaits.begin(), areaWaits.end(), [id](const TerrainWorldAreaWait& wait) { return wait.id == id; }),
                    areaWaits.end());
}

TileState TerrainWorldRuntime::tileState(TileCoord tile) const {
    if (!streamer) return TileState::Absent;
    return streamer->state(tile);
}

std::shared_ptr<const WorldTile> TerrainWorldRuntime::resident(TileCoord tile) const {
    std::lock_guard<std::recursive_mutex> guard(lock);
    const auto it = pinned.find(tile.key());
    if (it != pinned.end()) return it->second;
    return streamer ? streamer->data(tile) : nullptr;
}

std::shared_ptr<WorldTile> TerrainWorldRuntime::decodeFile(TileCoord tile, std::string& error) const {
    std::vector<uint8_t> bytes;
    if (!AssetIOService::readFile(worldSource->tilePath(tile), 0, 0, bytes, error)) return nullptr;
    auto data = std::make_shared<WorldTile>();
    const TileError result = decodeTile(bytes, terrainWorldDecodeCodec(), *data);
    if (result != TileError::None) {
        error = std::string("the tile is ") + tileErrorName(result);
        return nullptr;
    }
    if (!tileMatches(*data, tile, worldManifest.lattice, error)) return nullptr;
    return data;
}

std::shared_ptr<const WorldTile> TerrainWorldRuntime::loadNow(TileCoord tile) {
    std::lock_guard<std::recursive_mutex> guard(lock);
    if (std::shared_ptr<const WorldTile> data = resident(tile)) return data;
    if (!worldOccupancy.present(tile) || !streamer) return nullptr;
    std::string error;
    std::shared_ptr<WorldTile> data = decodeFile(tile, error);
    if (!data) return nullptr;
    streamer->replace(tile, data);
    return data;
}

SurfaceSample TerrainWorldRuntime::query(float localX, float localZ, bool mayWait) {
    std::lock_guard<std::recursive_mutex> guard(lock);
    const float size = worldManifest.lattice.tileSize();
    const TileCoord tile(tileOfLocal(localX, size), tileOfLocal(localZ, size));
    SurfaceSample sample;
    if (!tile.addressable() || !worldOccupancy.present(tile)) {
        sample.present = false;
        sample.inside = false;
        return sample;
    }
    std::shared_ptr<const WorldTile> data = resident(tile);
    if (!data && mayWait) data = loadNow(tile);
    if (data) return sampleTile(*data, worldManifest.lattice, worldManifest.maps.maxHeight, localX, localZ);
    if (!overview.empty()) return sampleOverview(overview, worldManifest.lattice, worldManifest.maps.maxHeight, localX, localZ);
    sample.present = false;
    sample.exact = false;
    return sample;
}

TerrainWorldPointState TerrainWorldRuntime::pointState(float localX, float localZ) const {
    std::lock_guard<std::recursive_mutex> guard(lock);
    const float size = worldManifest.lattice.tileSize();
    const TileCoord tile(tileOfLocal(localX, size), tileOfLocal(localZ, size));
    if (!tile.addressable() || !worldOccupancy.present(tile)) return TerrainWorldPointState::Absent;
    return resident(tile) ? TerrainWorldPointState::Full : TerrainWorldPointState::Overview;
}

bool TerrainWorldRuntime::raycast(const Vector3& origin, const Vector3& direction, float maxT, bool skipHoles, bool mayWait, Vector3& point, float& t) {
    std::lock_guard<std::recursive_mutex> guard(lock);
    const float size = worldManifest.lattice.tileSize();
    const float maxHeight = worldManifest.maps.maxHeight;
    // R24: a grid walk over the tiles the ray's ground track crosses, nearest first
    float tNow = 0.0f;
    const float dx = direction.x, dz = direction.z;
    int tx = tileOfLocal(origin.x, size), tz = tileOfLocal(origin.z, size);
    const int stepX = dx > 0.0f ? 1 : -1, stepZ = dz > 0.0f ? 1 : -1;
    const float infinite = 1.0e30f;
    auto boundaryT = [&](float o, float d, int cell, int step) {
        if (d == 0.0f) return infinite;
        const float edge = static_cast<float>(step > 0 ? cell + 1 : cell) * size;
        return (edge - o) / d;
    };
    float nextX = boundaryT(origin.x, dx, tx, stepX), nextZ = boundaryT(origin.z, dz, tz, stepZ);
    const float deltaX = dx != 0.0f ? size / std::fabs(dx) : infinite, deltaZ = dz != 0.0f ? size / std::fabs(dz) : infinite;
    for (int guard = 0; guard < 4 * WORLD_TILES_PER_SIDE && tNow <= maxT; guard++) {
        const float tEnd = std::min(std::min(nextX, nextZ), maxT);
        const TileCoord tile(tx, tz);
        if (tile.addressable() && worldOccupancy.present(tile) && tEnd >= tNow) {
            // min/max rejection: the segment passes wholly above or below the tile
            const TileMinMax heights = worldOccupancy.minMax(0, tx, tz);
            const float y0 = origin.y + direction.y * tNow, y1 = origin.y + direction.y * tEnd;
            const float low = static_cast<float>(heights.min) / 65535.0f * maxHeight, high = static_cast<float>(heights.max) / 65535.0f * maxHeight;
            if (!(std::min(y0, y1) > high + 0.01f || std::max(y0, y1) < low - 0.01f)) {
                const float pad = worldManifest.lattice.spacing * 1.0e-3f;
                const float from = std::max(0.0f, tNow - pad), to = tEnd + pad;
                std::shared_ptr<const WorldTile> data = resident(tile);
                if (!data && mayWait) data = loadNow(tile);
                if (data) {
                    SurfaceView view;
                    view.pixels = data->height.bytes.data();
                    view.width = data->height.width;
                    view.height = data->height.height;
                    view.channels = 1;
                    view.bytesPerChannel = 2;
                    view.size = static_cast<float>(data->height.width) * worldManifest.lattice.spacing;
                    view.maxHeight = maxHeight;
                    const float cx = static_cast<float>(tx) * size + size * 0.5f, cz = static_cast<float>(tz) * size + size * 0.5f;
                    const Vector3 start(origin.x + direction.x * from - cx, origin.y + direction.y * from, origin.z + direction.z * from - cz);
                    Vector3 hit;
                    float hitT = 0.0f;
                    if (raycastSurface(view, start, direction, to - from, false, hit, hitT)) {
                        const float worldT = from + hitT;
                        const Vector3 at(hit.x + cx, hit.y, hit.z + cz);
                        bool hole = false;
                        if (!data->holes.empty()) {
                            const HoleMapView holes{data->holes.bytes.data(), data->holes.width, data->holes.height};
                            hole = isHole(holes, (at.x - static_cast<float>(tx) * size) / size, (at.z - static_cast<float>(tz) * size) / size);
                        }
                        if (!(hole && skipHoles)) {
                            point = at;
                            t = worldT;
                            return true;
                        }
                    }
                } else if (!overview.empty()) {
                    // not in memory: the overview, as drawn, marched at half its stride and bisected
                    const float stepLength = size / static_cast<float>(overview.stride) * 0.5f;
                    const float length = std::sqrt(direction.x * direction.x + direction.y * direction.y + direction.z * direction.z);
                    const float dt = length > 0.0f ? stepLength / length : (to - from);
                    float previous = from;
                    auto above = [&](float at) {
                        const Vector3 p = origin + direction * at;
                        return p.y - sampleOverview(overview, worldManifest.lattice, maxHeight, p.x, p.z).height;
                    };
                    float previousValue = above(previous);
                    for (float at = std::min(from + dt, to); ; at = std::min(at + dt, to)) {
                        const float value = above(at);
                        if ((previousValue > 0.0f) != (value > 0.0f)) {
                            float lo = previous, hi = at;
                            for (int i = 0; i < 20; i++) {
                                const float mid = (lo + hi) * 0.5f;
                                if ((above(mid) > 0.0f) == (previousValue > 0.0f)) lo = mid; else hi = mid;
                            }
                            t = (lo + hi) * 0.5f;
                            point = origin + direction * t;
                            point.y = sampleOverview(overview, worldManifest.lattice, maxHeight, point.x, point.z).height;
                            return true;
                        }
                        previous = at;
                        previousValue = value;
                        if (at >= to) break;
                    }
                }
            }
        }
        if (tEnd >= maxT) break;
        tNow = tEnd;
        if (nextX < nextZ) {
            tx += stepX;
            nextX += deltaX;
        } else {
            tz += stepZ;
            nextZ += deltaZ;
        }
        if (!TileCoord(tx, tz).addressable() && (tx < WORLD_TILE_MIN - 1 || tx > WORLD_TILE_MAX + 1 || tz < WORLD_TILE_MIN - 1 || tz > WORLD_TILE_MAX + 1)) break;
    }
    return false;
}

std::vector<TileCoord> TerrainWorldRuntime::takeDirtyFiles() {
    std::vector<TileCoord> out;
    for (uint32_t key : dirtyFiles) out.push_back(TileCoord::fromKey(key));
    dirtyFiles.clear();
    return out;
}

std::vector<TileCoord> TerrainWorldRuntime::takeRemovedFiles() {
    std::vector<TileCoord> out;
    for (uint32_t key : removedFiles) out.push_back(TileCoord::fromKey(key));
    removedFiles.clear();
    return out;
}

void TerrainWorldRuntime::releaseFlushed(TileCoord tile) {
    std::lock_guard<std::recursive_mutex> guard(lock);
    const auto it = pinned.find(tile.key());
    if (it == pinned.end() || dirtyFiles.count(tile.key())) return;
    // the streamer keeps the data as its own; it may evict it now
    streamer->replace(tile, it->second);
    streamer->pin(tile, false);
    pinned.erase(it);
}

void TerrainWorldRuntime::fileChanged(TileCoord tile, bool exists) {
    std::lock_guard<std::recursive_mutex> guard(lock);
    if (!streamer) return;
    if (pinned.count(tile.key()) && dirtyFiles.count(tile.key())) return;
    pinned.erase(tile.key());
    if (!exists) {
        worldOccupancy.setTile(tile, false);
        streamer->forget(tile);
        revisions[tile.key()]++;
        overviewPatchPending = true;
        return;
    }
    std::string error;
    std::shared_ptr<WorldTile> data = decodeFile(tile, error);
    if (!data) {
        streamer->invalidate(tile);
        return;
    }
    const TileMinMax whole = tileNodeMinMax(*data, worldManifest.lattice, worldManifest.lattice.tileLevel(), 0, 0);
    worldOccupancy.setTile(tile, true, whole.min, whole.max);
    streamer->replace(tile, data);
    revisions[tile.key()]++;
    if (setOverviewPatch(overview, *data)) overviewRev++;
    else overviewPatchPending = true;
}

int TerrainWorldRuntime::checkSeams(TileCoord tile, bool repair) {
    if (!mapStore) return 0;
    // only neighbours in memory: a cold neighbour is checked when it arrives
    if (!resident(tile)) return 0;
    return mapStore->seamErrors(tile, repair);
}

TerrainWorldStats TerrainWorldRuntime::stats() const {
    TerrainWorldStats out = statsCache;
    out.tiles = worldOccupancy.tileCount();
    out.ready = isReady();
    if (!streamer) return out;
    const TileStreamStats& streaming = streamer->stats();
    for (int i = 0; i < 8; i++) out.states[i] = streaming.states[i];
    out.gpuBytes = streaming.gpuBytes;
    out.cpuBytes = streaming.cpuBytes;
    for (const auto& [key, data] : pinned) {
        if (!streamer->data(TileCoord::fromKey(key))) out.cpuBytes += data->cpuBytes();
    }
    out.gpuCap = streamer->budgets().gpuCapBytes;
    out.cpuCap = streamer->budgets().cpuCapBytes;
    out.poolSize = streamer->budgets().poolSize;
    out.queue = streaming.queued;
    out.inFlight = streaming.inFlight;
    out.uploadBytesLastFrame = streaming.uploadBytesLastFrame;
    out.uploadBytesTotal = streaming.uploadBytesTotal;
    out.uploadRetries = streaming.uploadRetries;
    out.poolExhausted = streaming.poolExhausted;
    out.frozen = streamer->frozen();
    if (collision) {
        out.collisionFine = collision->fineBodies();
        out.collisionCoarse = collision->coarseBodies();
        out.frozenBodies = collision->frozenBodies();
    }
    return out;
}

// ------------------------------------------------------------------ TileAccess

bool TerrainWorldRuntime::present(TileCoord tile) const {
    return worldOccupancy.present(tile);
}

std::shared_ptr<const WorldTile> TerrainWorldRuntime::tile(TileCoord coord) const {
    if (std::shared_ptr<const WorldTile> data = resident(coord)) return data;
    // the store edits what is there: a cold tile is read now (an edit is never made over a guess)
    return const_cast<TerrainWorldRuntime*>(this)->loadNow(coord);
}

std::shared_ptr<WorldTile> TerrainWorldRuntime::writable(TileCoord coord) {
    std::lock_guard<std::recursive_mutex> guard(lock);
    const auto it = pinned.find(coord.key());
    if (it != pinned.end()) return it->second;
    if (!mayWrite(coord, nullptr)) return nullptr;
    std::shared_ptr<const WorldTile> current = tile(coord);
    if (!current) return nullptr;
    // the writer's own copy: a job or the streamer's cache keeps the one it holds
    auto copy = std::make_shared<WorldTile>(*current);
    pinned[coord.key()] = copy;
    streamer->replace(coord, copy);
    streamer->pin(coord, true);
    return copy;
}

bool TerrainWorldRuntime::mayWrite(TileCoord coord, std::string* why) const {
    if (!isPlaying || pinned.count(coord.key())) return true;
    if (static_cast<int>(pinned.size()) < MAX_RUNTIME_EDITED_TILES) return true;
    if (why) *why = "a game may hold at most " + std::to_string(MAX_RUNTIME_EDITED_TILES) + " edited tiles of a Terrain World at once";
    return false;
}

void TerrainWorldRuntime::add(TileCoord coord, std::shared_ptr<WorldTile> data) {
    std::lock_guard<std::recursive_mutex> guard(lock);
    if (!data) return;
    const TileMinMax whole = tileNodeMinMax(*data, worldManifest.lattice, worldManifest.lattice.tileLevel(), 0, 0);
    worldOccupancy.setTile(coord, true, whole.min, whole.max);
    pinned[coord.key()] = data;
    streamer->replace(coord, data);
    streamer->pin(coord, true);
    removedFiles.erase(coord.key());
    dirtyFiles.insert(coord.key());
    derivedDirty.insert(coord.key());
    revisions[coord.key()]++;
}

void TerrainWorldRuntime::restore(TileCoord coord, std::shared_ptr<WorldTile> data) {
    std::lock_guard<std::recursive_mutex> guard(lock);
    if (!data) return;
    add(coord, std::move(data));
    derivedDirty.erase(coord.key());
    overviewPatchPending = true;
}

void TerrainWorldRuntime::remove(TileCoord coord) {
    std::lock_guard<std::recursive_mutex> guard(lock);
    worldOccupancy.setTile(coord, false);
    pinned.erase(coord.key());
    streamer->forget(coord);
    dirtyFiles.erase(coord.key());
    derivedDirty.erase(coord.key());
    removedFiles.insert(coord.key());
    revisions[coord.key()]++;
    overviewPatchPending = true;
}

void TerrainWorldRuntime::changed(TileCoord coord) {
    std::lock_guard<std::recursive_mutex> guard(lock);
    if (!pinned.count(coord.key())) return;
    dirtyFiles.insert(coord.key());
    derivedDirty.insert(coord.key());
}

uint32_t TerrainWorldRuntime::tileRevision(TileCoord coord) const {
    const auto it = revisions.find(coord.key());
    return it == revisions.end() ? 0 : it->second;
}

bool TerrainWorldRuntime::bounds(TileCoord& low, TileCoord& high) const {
    return worldOccupancy.bounds(low, high);
}
