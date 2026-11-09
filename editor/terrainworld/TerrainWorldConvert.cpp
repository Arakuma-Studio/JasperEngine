// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "terrainworld/TerrainWorldConvert.h"

#include "EditorHost.h"
#include "Project.h"
#include "Scene.h"
#include "asset/Guid.h"
#include "command/CommandHandle.h"
#include "command/CommandHistory.h"
#include "command/type/AddComponentCmd.h"
#include "command/type/ComponentValuesCmd.h"
#include "command/type/CreateEntityCmd.h"
#include "command/type/EntityStateCmd.h"
#include "command/type/PropertyCmd.h"
#include "component/MeshComponent.h"
#include "component/TerrainComponent.h"
#include "component/TerrainWorldComponent.h"
#include "component/Transform.h"
#include "foliage/TreeDataFile.h"
#include "jobs/JobSystem.h"
#include "object/TerrainEdit.h"
#include "subsystem/FoliageSystem.h"
#include "subsystem/TerrainWorldRuntime.h"
#include "terrain/FoliageRules.h"
#include "terrain/TerrainSurface.h"
#include "terrain/TreeStore.h"
#include "terrain/world/TiledMapStore.h"
#include "terrainworld/TerrainWorldFiles.h"
#include "util/EntityClipboard.h"
#include "util/FileUtils.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <map>

using namespace jasperengine;
using namespace jasperengine::editor;
using namespace jasperengine::terraincore;

namespace fs = std::filesystem;

namespace {

    // One of the source's maps, as T1's store reads it
    struct ConvertMap {
        std::vector<uint8_t> bytes;
        int width = 0, height = 0, channels = 0, bytesPerChannel = 1;
        bool empty() const { return bytes.empty(); }
        const uint8_t* texel(int x, int y) const { return bytes.data() + (static_cast<size_t>(y) * width + x) * channels * bytesPerChannel; }
    };

    // What the job reads: copies, so the scene may change while it runs
    struct ConvertSource {
        float size = 0.0f, maxHeight = 0.0f;
        ConvertMap height;
        ConvertMap holes;
        std::vector<ConvertMap> blend;
        std::vector<ConvertMap> density;    // per foliage layer; empty where a layer has none
        std::shared_ptr<const TreeStore> trees;
        std::vector<float> colours;
        int layerCount = 0;
    };

    struct ConvertOutput {
        std::vector<std::shared_ptr<WorldTile>> tiles;
        std::map<uint32_t, std::vector<uint8_t>> trees;
        bool cancelled = false;
    };

    bool readConvertMap(TerrainComponentStore& store, MapId id, ConvertMap& out) {
        const MapFormat format = store.format(id);
        if (!format.exists || format.bounds.empty()) return false;
        out.width = format.bounds.w;
        out.height = format.bounds.h;
        out.channels = format.channels;
        out.bytesPerChannel = format.bitsPerChannel >= 16 ? 2 : 1;
        out.bytes.resize(static_cast<size_t>(format.bounds.area()) * bytesPerTexel(format));
        if (!store.readBytes(id, format.bounds, out.bytes)) {
            out.bytes.clear();
            return false;
        }
        return true;
    }

    // A filtered sample of a map at a terrain-local position, its texels at (i + 0.5) / W as T1 reads them
    void bilinearTexel(const ConvertMap& map, float size, float localX, float localZ, uint8_t* out) {
        const float x = std::clamp(surfaceTexelCoord(localX, size, map.width), 0.0f, static_cast<float>(map.width - 1));
        const float z = std::clamp(surfaceTexelCoord(localZ, size, map.height), 0.0f, static_cast<float>(map.height - 1));
        const int x0 = static_cast<int>(std::floor(x)), z0 = static_cast<int>(std::floor(z));
        const int x1 = std::min(x0 + 1, map.width - 1), z1 = std::min(z0 + 1, map.height - 1);
        const float fx = x - static_cast<float>(x0), fz = z - static_cast<float>(z0);
        for (int c = 0; c < map.channels; c++) {
            const auto at = [&](int tx, int tz) { return static_cast<float>(map.texel(tx, tz)[c * map.bytesPerChannel + map.bytesPerChannel - 1]); };
            const float low = at(x0, z0) + (at(x1, z0) - at(x0, z0)) * fx;
            const float high = at(x0, z1) + (at(x1, z1) - at(x0, z1)) * fx;
            out[c] = static_cast<uint8_t>(std::clamp(std::lround(low + (high - low) * fz), 0l, 255l));
        }
    }

    // The texel an area map's point falls in, floor(u W) (T3 R19, T5's nearest read)
    uint8_t nearestTexel(const ConvertMap& map, float size, float localX, float localZ) {
        const int x = std::clamp(static_cast<int>(std::floor((localX + size * 0.5f) / size * static_cast<float>(map.width))), 0, map.width - 1);
        const int z = std::clamp(static_cast<int>(std::floor((localZ + size * 0.5f) / size * static_cast<float>(map.height))), 0, map.height - 1);
        return map.texel(x, z)[map.bytesPerChannel - 1];
    }

    WorldLattice latticeOf(const TerrainWorldSettings& settings) {
        WorldLattice lattice;
        lattice.tileQuads = settings.tileQuads;
        lattice.spacing = settings.spacing;
        return lattice;
    }

    WorldMaps mapsOf(const ConvertSource& source) {
        WorldMaps maps;
        maps.maxHeight = source.maxHeight;
        maps.blendMaps = static_cast<int>(source.blend.size());
        maps.hasHoles = !source.holes.empty();
        maps.densityMaps = static_cast<int>(source.density.size());
        return maps;
    }

    // The job: every tile of the plan from the copies. Heights are T1's sampleSurface of the source's map,
    // the query sampleTerrainSurface answers, at each lattice sample, gutters included.
    void buildConvertedTiles(const ConvertSource& source, const TerrainWorldSettings& settings, const std::atomic<bool>& cancel, ConvertOutput& out) {
        const WorldLattice lattice = latticeOf(settings);
        const int n = lattice.tileQuads;
        const float h = lattice.spacing;
        const float tileSize = lattice.tileSize();
        SurfaceView view;
        view.pixels = source.height.bytes.data();
        view.width = source.height.width;
        view.height = source.height.height;
        view.channels = source.height.channels;
        view.bytesPerChannel = source.height.bytesPerChannel;
        view.size = source.size;
        view.maxHeight = source.maxHeight;

        MemoryTileAccess access;
        std::vector<TileCoord> coords;
        for (int z = settings.originZ; z < settings.originZ + settings.gridLength; z++) {
            for (int x = settings.originX; x < settings.originX + settings.gridWidth; x++) {
                if (cancel.load()) {
                    out.cancelled = true;
                    return;
                }
                const TileCoord coord(x, z);
                auto tile = std::make_shared<WorldTile>();
                initTile(*tile, lattice, coord, 0);
                for (int j = 0; j < n + 3; j++) {
                    for (int i = 0; i < n + 3; i++) {
                        const float localX = static_cast<float>(x * n + i - 1) * h;
                        const float localZ = static_cast<float>(z * n + j - 1) * h;
                        const float height = sampleSurface(view, localX, localZ).height;
                        const float normalized = source.maxHeight > 0.0f ? std::clamp(height / source.maxHeight, 0.0f, 1.0f) : 0.0f;
                        tile->setSample(i, j, static_cast<uint16_t>(std::lround(normalized * 65535.0f)));
                    }
                }
                const float originX = static_cast<float>(x) * tileSize, originZ = static_cast<float>(z) * tileSize;
                // blend: shared samples on the lattice's corners
                tile->blend.resize(source.blend.size());
                for (size_t m = 0; m < source.blend.size(); m++) {
                    const int quads = lattice.blendQuadCount();
                    TileMap& map = tile->blend[m];
                    map.allocate(lattice.blendSide(), 4, 8, 0);
                    uint8_t texel[4] = {0, 0, 0, 0};
                    for (int j = 0; j < map.height; j++) {
                        for (int i = 0; i < map.width; i++) {
                            bilinearTexel(source.blend[m], source.size, originX + static_cast<float>(i) * tileSize / static_cast<float>(quads),
                                          originZ + static_cast<float>(j) * tileSize / static_cast<float>(quads), texel);
                            std::copy(texel, texel + std::min(source.blend[m].channels, 4), map.texel(i, j));
                        }
                    }
                }
                // holes and density: areas, read at each texel's centre
                const auto area = [&](const ConvertMap& from, TileMap& map, int side) {
                    map.allocate(side, 1, 8, 0);
                    for (int j = 0; j < side; j++) {
                        for (int i = 0; i < side; i++) {
                            *map.texel(i, j) = nearestTexel(from, source.size, originX + (static_cast<float>(i) + 0.5f) * tileSize / static_cast<float>(side),
                                                            originZ + (static_cast<float>(j) + 0.5f) * tileSize / static_cast<float>(side));
                        }
                    }
                };
                if (!source.holes.empty()) area(source.holes, tile->holes, lattice.holeSide());
                tile->density.resize(source.density.size());
                for (size_t l = 0; l < source.density.size(); l++) {
                    if (!source.density[l].empty()) area(source.density[l], tile->density[l], lattice.densitySide());
                }
                access.add(coord, tile);
                coords.push_back(coord);
            }
        }

        // R3: a gutter where no tile is repeats the border, as the world keeps it
        TiledMapStore store(lattice, mapsOf(source), access);
        for (const TileCoord& coord : coords) store.syncGutters(coord);
        for (const TileCoord& coord : coords) {
            std::shared_ptr<WorldTile> tile = access.writable(coord);
            buildDerived(*tile, lattice, source.colours, source.layerCount);
            out.tiles.push_back(std::move(tile));
        }

        // R27: each tree to the tile under it, in that tile's square
        if (source.trees) {
            std::map<uint32_t, std::vector<TreeRecord>> byTile;
            for (const TreeRecord& record : source.trees->records()) {
                const float localX = foliageTreeLocalX(record, source.size), localZ = foliageTreeLocalZ(record, source.size);
                const TileCoord coord(tileOfLocal(localX, tileSize), tileOfLocal(localZ, tileSize));
                if (coord.x < settings.originX || coord.x >= settings.originX + settings.gridWidth || coord.z < settings.originZ ||
                    coord.z >= settings.originZ + settings.gridLength) {
                    continue;
                }
                TreeRecord moved = record;
                moved.u = (localX - static_cast<float>(coord.x) * tileSize) / tileSize;
                moved.v = (localZ - static_cast<float>(coord.z) * tileSize) / tileSize;
                byTile[coord.key()].push_back(moved);
            }
            for (auto& [key, records] : byTile) {
                TreeStore store;
                store.apply({}, records, {}, nullptr);
                TreeDataFile::encode(store, out.trees[key]);
            }
        }
    }

    bool snapshotSource(Scene* scene, Entity source, ConvertSource& out, std::string& error) {
        TerrainComponent& terrain = scene->getComponent<TerrainComponent>(source);
        out.size = terrain.terrainSize;
        out.maxHeight = terrain.maxHeight;
        TerrainComponentStore store(scene, source, terrain);
        if (!readConvertMap(store, MapId{MapKind::Height, 0}, out.height)) {
            error = "the Terrain's Height Map is not loaded: open its scene and let it load, then convert again";
            return false;
        }
        readConvertMap(store, MapId{MapKind::Hole, 0}, out.holes);
        for (int m = 0; m < static_cast<int>(terrain.blendMaps.size()) && m < 3; m++) {
            ConvertMap map;
            if (!readConvertMap(store, MapId{MapKind::Blend, m}, map)) break;
            out.blend.push_back(std::move(map));
        }
        bool anyDensity = false;
        out.density.resize(terrain.foliageLayers.size());
        for (int l = 0; l < static_cast<int>(terrain.foliageLayers.size()); l++) anyDensity = readConvertMap(store, MapId{MapKind::Density, l}, out.density[l]) || anyDensity;
        if (!anyDensity) out.density.clear();
        if (FoliageSystem* foliage = scene->findSystem<FoliageSystem>().get()) {
            if (std::shared_ptr<const TreeStore> trees = static_cast<const FoliageSystem*>(foliage)->getTreeStore(source)) {
                out.trees = std::make_shared<TreeStore>(*trees);
            } else if (!terrain.treeData.empty()) {
                error = "the Terrain's trees are still loading: convert again when they are drawn";
                return false;
            }
        }
        out.colours = terrainWorldLayerColours(terrain, scene->findComponent<MeshComponent>(source));
        out.layerCount = static_cast<int>(terrain.surfaceLayers.size());
        return true;
    }

    // The main thread's end: the folder, then one undo step
    TerrainConvertResult finishConversion(Project* project, uint32_t sceneId, Entity source, const TerrainConvertPlan& plan, const ConvertSource& data,
                                          ConvertOutput& output) {
        TerrainConvertResult result;
        result.assetPath = plan.world.assetPath;
        SceneProject* sceneProject = project ? project->getScene(sceneId) : nullptr;
        Scene* scene = sceneProject ? sceneProject->scene : nullptr;
        if (!scene || !scene->isEntityCreated(source) || !scene->findComponent<TerrainComponent>(source)) {
            result.error = "the Terrain was removed while it converted; nothing was changed";
            return result;
        }
        WorldManifest manifest;
        manifest.lattice = latticeOf(plan.world);
        manifest.maps = mapsOf(data);
        const fs::path folder = project->resolveAssetPath(FileUtils::pathFromUtf8(plan.world.assetPath));
        if (!writeWorldFolder(folder, manifest, output.tiles, output.trees, result.error)) return result;
        FileUtils::reportWritten(folder);

        // copies: the commands below add entities and components, which move the arrays
        const Entity parent = scene->getComponent<Transform>(source).parent;
        const Vector3 position = scene->getComponent<Transform>(source).position;
        const MeshComponent* sourceMesh = scene->findComponent<MeshComponent>(source);
        const bool hasMaterial = sourceMesh && sourceMesh->numSubmeshes > 0;
        const Material material = hasMaterial ? sourceMesh->submeshes[0].material : Material();
        const std::string name = plan.world.name;
        CommandHistory* history = CommandHandle::get(sceneId);
        const size_t start = history->mark();
        auto fail = [&](const std::string& why) {
            while (history->position() > start && history->canUndo() && history->undo()) {}
            result.error = why;
            result.entity = NULL_ENTITY;
            return result;
        };
        CreateEntityCmd* createCmd = new CreateEntityCmd(project, sceneId, name, EntityCreationType::TERRAIN, parent);
        if (!history->addCommandNoMerge(createCmd)) return fail("the Terrain World could not be made");
        const Entity entity = createCmd->getEntity();

        // the layers, material and foliage settings; the maps are the tiles' now
        YAML::Node values = EntityClipboard::encodeComponent(scene, source, ComponentType::TerrainComponent);
        values.remove("heightMap");
        values.remove("holeMap");
        values.remove("blendMaps");
        values.remove("treeData");
        if (values["foliageLayers"] && values["foliageLayers"].IsSequence()) {
            for (size_t l = 0; l < values["foliageLayers"].size(); l++) values["foliageLayers"][l].remove("densityMap");
        }
        auto clip = std::make_shared<ComponentClip>();
        clip->project = EntityClipboard::projectKey(project);
        clip->sceneId = sceneId;
        clip->type = ComponentType::TerrainComponent;
        clip->entity = source;
        clip->value = values;
        if (!history->addCommandNoMerge(new PropertyCmd<Vector3>(project, sceneId, entity, ComponentType::Transform, "position", position)) ||
            !history->addCommandNoMerge(new ComponentValuesCmd(project, sceneId, {entity}, ComponentType::TerrainComponent, clip)) ||
            (hasMaterial &&
             !history->addCommandNoMerge(new PropertyCmd<Material>(project, sceneId, entity, ComponentType::MeshComponent, "submeshes[0].material", material)))) {
            return fail("the Terrain's settings could not be copied");
        }
        const AssetRef ref(plan.world.assetPath, Guid::parse(manifest.guid));
        if (!history->addCommandNoMerge(new AddComponentCmd(project, sceneId, entity, ComponentType::TerrainWorldComponent)) ||
            !history->addCommandNoMerge(new PropertyCmd<AssetRef>(project, sceneId, entity, ComponentType::TerrainWorldComponent, "worldData", ref)) ||
            // R20: a static Rigidbody with no collider: the world pages its own
            !history->addCommandNoMerge(new AddComponentCmd(project, sceneId, entity, ComponentType::RigidbodyComponent))) {
            return fail("the Terrain World's components could not be added");
        }
        // R33: the source stays, inactive, so nothing of it is lost
        if (!history->addCommandNoMerge(EntityStateCmd::active(project, sceneId, {source}, false))) return fail("the Terrain could not be deactivated");
        history->groupSince(start, "Convert to Terrain World (" + name + ")");
        project->setSelectedEntity(sceneId, entity);
        result.entity = entity;
        result.tiles = static_cast<int>(output.tiles.size());
        return result;
    }

}

std::string jasperengine::editor::freeTerrainWorldAssetPath(Project* project, const std::string& name) {
    for (int serial = 0;; serial++) {
        const std::string candidate = (serial == 0 ? name : name + " " + std::to_string(serial)) + ".terrainworld";
        std::error_code ec;
        if (!fs::exists(project->resolveAssetPath(FileUtils::pathFromUtf8(candidate)), ec)) return candidate;
    }
}

bool jasperengine::editor::planTerrainConversion(Scene* scene, Entity source, const TerrainConvertOptions& options, TerrainConvertPlan& plan,
                                                 std::string& error) {
    TerrainComponent* terrain = scene ? scene->findComponent<TerrainComponent>(source) : nullptr;
    const Transform* transform = scene ? scene->findComponent<Transform>(source) : nullptr;
    if (!terrain || !transform) {
        error = "from_terrain must be a GameObject with a Terrain";
        return false;
    }
    if (terrain->world || scene->findComponent<TerrainWorldComponent>(source)) {
        error = "the GameObject is a Terrain World already";
        return false;
    }
    // R4: a world translates only
    if (transform->rotation != Quaternion() || transform->scale != Vector3(1.0f, 1.0f, 1.0f) || transform->worldRotation != Quaternion() ||
        transform->worldScale != Vector3(1.0f, 1.0f, 1.0f)) {
        error = "a Terrain World only moves, it does not rotate or scale: set the Terrain's Rotation to 0 and Scale to 1 (its parents' too), then convert";
        return false;
    }
    if (!(terrain->terrainSize > 0.0f) || !(terrain->maxHeight > 0.0f)) {
        error = "the Terrain needs a Size and a Max Height above 0";
        return false;
    }
    TerrainComponentStore store(scene, source, *terrain);
    const MapFormat height = store.format(MapId{MapKind::Height, 0});
    if (!height.exists || height.bounds.empty()) {
        error = "the Terrain's Height Map is not loaded: open its scene and let it load, then convert again";
        return false;
    }

    TerrainWorldSettings& world = plan.world;
    const std::string sourceName = scene->getEntityName(source);
    world.name = options.name.empty() ? (sourceName.empty() ? std::string("Terrain") : sourceName) + " World" : options.name;
    world.maxHeight = terrain->maxHeight;
    world.baseHeight = 0.0f;
    // the heightmap's sample density: the largest power of two at or under a texel
    const float texel = terrain->terrainSize / static_cast<float>(std::max(height.bounds.w, height.bounds.h));
    world.spacing = options.spacing > 0.0f ? options.spacing : std::ldexp(1.0f, std::clamp(static_cast<int>(std::floor(std::log2(texel))), -2, 3));
    // the largest tile the platform takes, down to the half of the source a tile of a 2 x 2 grid covers
    const int maxQuads = terrainWorldPlatformBudget().maxTileQuads;
    const float need = terrain->terrainSize * 0.5f / world.spacing;
    int quads = 64;
    while (quads < maxQuads && static_cast<float>(quads) < need) quads *= 2;
    world.tileQuads = options.tileQuads > 0 ? options.tileQuads : quads;
    const float tileSize = static_cast<float>(world.tileQuads) * world.spacing;
    // the tiles under the Terrain's square, centred on its origin as the Terrain is
    const float half = terrain->terrainSize * 0.5f;
    world.originX = world.originZ = static_cast<int>(std::floor(-half / tileSize));
    const int high = static_cast<int>(std::ceil(half / tileSize)) - 1;
    world.gridWidth = world.gridLength = high - world.originX + 1;
    if (!validateTerrainWorldSettings(world, error)) return false;

    plan.samples = static_cast<int64_t>(world.gridWidth) * world.gridLength * (world.tileQuads + 3) * (world.tileQuads + 3);
    plan.trees = !terrain->treeData.empty();
    return true;
}

TerrainConvertResult jasperengine::editor::convertTerrainNow(Project* project, uint32_t sceneId, Entity source, const TerrainConvertOptions& options) {
    TerrainConvertResult result;
    SceneProject* sceneProject = project ? project->getScene(sceneId) : nullptr;
    Scene* scene = sceneProject ? sceneProject->scene : nullptr;
    TerrainConvertPlan plan;
    if (!planTerrainConversion(scene, source, options, plan, result.error)) return result;
    plan.world.assetPath = options.assetPath.empty() ? freeTerrainWorldAssetPath(project, plan.world.name) : options.assetPath;
    ConvertSource data;
    if (!snapshotSource(scene, source, data, result.error)) return result;
    ConvertOutput output;
    const std::atomic<bool> never{false};
    buildConvertedTiles(data, plan.world, never, output);
    return finishConversion(project, sceneId, source, plan, data, output);
}

std::function<void()> jasperengine::editor::convertTerrainLater(Project* project, uint32_t sceneId, Entity source, const TerrainConvertOptions& options,
                                                                std::function<void(const TerrainConvertResult&)> done) {
    TerrainConvertResult failed;
    SceneProject* sceneProject = project ? project->getScene(sceneId) : nullptr;
    Scene* scene = sceneProject ? sceneProject->scene : nullptr;
    auto plan = std::make_shared<TerrainConvertPlan>();
    auto data = std::make_shared<ConvertSource>();
    if (!planTerrainConversion(scene, source, options, *plan, failed.error) || !snapshotSource(scene, source, *data, failed.error)) {
        done(failed);
        return []() {};
    }
    plan->world.assetPath = options.assetPath.empty() ? freeTerrainWorldAssetPath(project, plan->world.name) : options.assetPath;
    auto cancel = std::make_shared<std::atomic<bool>>(false);
    auto output = std::make_shared<ConvertOutput>();
    jobs::SubmitOptions submit;
    submit.lane = jobs::Lane::Background;
    submit.name = "terrain-world-convert";
    jobs::submit([project, sceneId, source, plan, data, cancel, output, done]() {
        buildConvertedTiles(*data, plan->world, *cancel, *output);
        getEditorHost().enqueueMainThreadTask([project, sceneId, source, plan, data, cancel, output, done]() {
            TerrainConvertResult result;
            if (cancel->load() || output->cancelled) {
                result.cancelled = true;
                result.error = "the conversion was cancelled; nothing was changed";
            } else {
                result = finishConversion(project, sceneId, source, *plan, *data, *output);
            }
            done(result);
        });
    }, submit);
    return [cancel]() { cancel->store(true); };
}
