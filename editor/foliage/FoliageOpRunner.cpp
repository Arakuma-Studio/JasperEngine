// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "foliage/FoliageOpRunner.h"
#include "spline/SplineEvaluator.h"

#include "EditorHost.h"
#include "Project.h"
#include "command/CommandHandle.h"
#include "command/type/TerrainMapDeltaCmd.h"
#include "command/type/TreeEditCmd.h"
#include "component/InstancedMeshComponent.h"
#include "component/MeshComponent.h"
#include "component/ModelComponent.h"
#include "component/SplineComponent.h"
#include "component/TerrainComponent.h"
#include "object/GameObject.h"
#include "foliage/TreeDataFile.h"
#include "jobs/JobSystem.h"
#include "object/TerrainEdit.h"
#include "terrain/EditorTerrainStore.h"
#include "terrain/OverlayMapStore.h"
#include "subsystem/FoliageSystem.h"
#include "subsystem/TerrainWorldRuntime.h"
#include "terrain/FoliageScatter.h"
#include "terrainworld/TerrainWorldTrees.h"
#include "util/Fnv.h"

#include <optional>
#include <algorithm>
#include <cinttypes>
#include <cstdio>
#include <filesystem>
#include <map>

using namespace jasperengine;
using namespace jasperengine::terrain;
namespace fs = std::filesystem;

namespace {

    std::vector<std::shared_ptr<editor::FoliageOpRunner>>& runningOps() {
        static std::vector<std::shared_ptr<editor::FoliageOpRunner>> running;
        return running;
    }

    // Which maps a list reads, so a big terrain copies only those
    struct WantedMaps {
        bool blend = false;
        std::vector<int> density;
    };

    void scanParams(const OpParam& param, WantedMaps& wanted) {
        for (const auto& [key, value] : param.fields) {
            if (key == "layer" && value.kind == OpParam::Kind::List) wanted.blend = true;
            if ((key == "detail_layer" || (key == "layer" && value.kind == OpParam::Kind::Number)) && value.kind == OpParam::Kind::Number) {
                wanted.density.push_back(static_cast<int>(value.number));
            }
            if (key == "target" && value.kind == OpParam::Kind::String && (value.text == "details" || value.text == "all")) {
                wanted.density.push_back(-1);
            }
            scanParams(value, wanted);
        }
        for (const OpParam& item : param.items) scanParams(item, wanted);
    }

    // convert_placed_to_trees' other half: the hosts' instances out on apply, back on undo (R28)
    std::function<void(bool)> emptyPlacedHosts(editor::Project* project, uint32_t sceneId, std::vector<std::string> hosts) {
        auto kept = std::make_shared<std::map<std::string, std::vector<InstanceData>>>();
        return [project, sceneId, hosts = std::move(hosts), kept](bool undone) {
            editor::SceneProject* sceneProject = project->getScene(sceneId);
            if (!sceneProject) return;
            for (const std::string& name : hosts) {
                const Entity entity = static_cast<Entity>(std::stoul(name));
                InstancedMeshComponent* instmesh = sceneProject->scene->findComponent<InstancedMeshComponent>(entity);
                if (!instmesh) continue;
                if (undone) {
                    instmesh->instances = (*kept)[name];
                } else {
                    (*kept)[name] = instmesh->instances;
                    instmesh->instances.clear();
                }
                instmesh->needUpdateInstances = true;
            }
        };
    }

    void copyMap(TerrainMapStore& from, RawMapStore& to, MapId map) {
        const MapFormat format = from.format(map);
        if (!format.exists || format.bounds.empty()) return;
        std::vector<uint8_t> bytes(static_cast<size_t>(format.bounds.area()) * bytesPerTexel(format));
        if (!from.readBytes(map, format.bounds, bytes)) return;
        to.addMap(map, format.bounds.w, format.bounds.h, format.channels, format.bitsPerChannel, std::move(bytes));
    }

    // ------------------------------------------------------------------ T4 R27: a Terrain World, tile by tile

    // A tile as T5's ops see a Terrain: a square of the tile's size centred on it, its maps at T1's texel
    // centres. Heights are the lattice's surface at each centre, half a sample in from the samples.
    std::unique_ptr<RawMapStore> worldTileSnapshot(const WorldTile& data, const WorldLattice& lattice, float maxHeight, const WantedMaps& wanted, int layerCount) {
        auto store = std::make_unique<RawMapStore>(lattice.tileSize(), maxHeight);
        const int quads = lattice.tileQuads;
        std::vector<uint8_t> heights(static_cast<size_t>(quads) * quads * 2);
        for (int j = 0; j < quads; j++) {
            for (int i = 0; i < quads; i++) {
                const uint32_t sum = static_cast<uint32_t>(data.sample(i + 1, j + 1)) + data.sample(i + 2, j + 1) + data.sample(i + 1, j + 2) + data.sample(i + 2, j + 2);
                const uint16_t value = static_cast<uint16_t>((sum + 2) / 4);
                const size_t at = (static_cast<size_t>(j) * quads + i) * 2;
                heights[at] = static_cast<uint8_t>(value & 0xFF);
                heights[at + 1] = static_cast<uint8_t>(value >> 8);
            }
        }
        store->addMap(MapId{MapKind::Height, 0}, quads, quads, 1, 16, std::move(heights));
        if (!data.holes.empty()) store->addMap(MapId{MapKind::Hole, 0}, data.holes.width, data.holes.height, 1, 8, data.holes.bytes);
        if (wanted.blend) {
            // the blend lattice's corners averaged to its quads' centres
            const int quadsBlend = lattice.blendQuadCount();
            for (int b = 0; b < static_cast<int>(data.blend.size()); b++) {
                const TileMap& corners = data.blend[b];
                if (corners.empty()) continue;
                std::vector<uint8_t> centres(static_cast<size_t>(quadsBlend) * quadsBlend * 4);
                for (int j = 0; j < quadsBlend; j++) {
                    for (int i = 0; i < quadsBlend; i++) {
                        for (int c = 0; c < 4; c++) {
                            const uint32_t sum = static_cast<uint32_t>(corners.texel(i, j)[c]) + corners.texel(i + 1, j)[c] + corners.texel(i, j + 1)[c] + corners.texel(i + 1, j + 1)[c];
                            centres[(static_cast<size_t>(j) * quadsBlend + i) * 4 + c] = static_cast<uint8_t>((sum + 2) / 4);
                        }
                    }
                }
                store->addMap(MapId{MapKind::Blend, b}, quadsBlend, quadsBlend, 4, 8, std::move(centres));
            }
        }
        const int side = lattice.densitySide();
        const auto addDensity = [&](int index) {
            if (index < 0 || index >= layerCount || store->format(MapId{MapKind::Density, index}).exists) return;
            const bool has = index < static_cast<int>(data.density.size()) && !data.density[index].empty();
            store->addMap(MapId{MapKind::Density, index}, side, side, 1, 8, has ? data.density[index].bytes : std::vector<uint8_t>());
        };
        for (int index : wanted.density) {
            if (index < 0) {
                for (int i = 0; i < layerCount; i++) addDensity(i);
            } else {
                addDensity(index);
            }
        }
        return store;
    }

    void translateWorldShape(Shape& shape, const Vector2& by) {
        shape.center = shape.center - by;
        shape.min = shape.min - by;
        shape.max = shape.max - by;
        for (Vector2& point : shape.points) point = point - by;
    }

    OpParam worldNumberParam(double value) {
        OpParam param;
        param.kind = OpParam::Kind::Number;
        param.number = value;
        return param;
    }

    void setWorldParam(OpParam& params, const std::string& key, OpParam value) {
        for (auto& [name, existing] : params.fields) {
            if (name == key) {
                existing = std::move(value);
                return;
            }
        }
        params.fields.emplace_back(key, std::move(value));
    }

    // One tile's entry of an op into the World's: counts summed, rectangles and ids in the World's terms
    void mergeWorldEntry(OpEntry& into, const OpEntry& from, TileCoord tile, const Vector2& centre, std::string& hashes) {
        into.changedTexels += from.changedTexels;
        into.outside += from.outside;
        into.filteredOut += from.filteredOut;
        for (const auto& [key, value] : from.fields) {
            OpParam* existing = nullptr;
            for (auto& [name, field] : into.fields) {
                if (name == key) existing = &field;
            }
            if (key == "hash") {
                hashes += value.text;
                continue;
            }
            OpParam moved = value;
            if (key == "changed_rects") {
                for (OpParam& rect : moved.items) {
                    for (auto& [corner, point] : rect.fields) {
                        (void)corner;
                        if (point.items.size() == 2) {
                            point.items[0].number += centre.x;
                            point.items[1].number += centre.y;
                        }
                    }
                }
            } else if (key == "ids") {
                for (OpParam& id : moved.items) id.number = static_cast<double>(editor::terrainWorldTreeId(tile, static_cast<uint32_t>(id.number)));
            }
            if (!existing) {
                into.fields.emplace_back(key, std::move(moved));
            } else if (value.kind == OpParam::Kind::Number) {
                existing->number += value.number;
            } else if (value.kind == OpParam::Kind::List) {
                existing->items.insert(existing->items.end(), moved.items.begin(), moved.items.end());
            } else if (value.kind == OpParam::Kind::Object) {
                for (const auto& [name, count] : moved.fields) {
                    for (auto& [have, total] : existing->fields) {
                        if (have == name && count.kind == OpParam::Kind::Number) total.number += count.number;
                    }
                }
            }
        }
    }

}

struct editor::FoliageOpRunner::Work {
    std::unique_ptr<RawMapStore> snapshot;
    std::vector<Op> ops;
    OpOptions options;
    std::shared_ptr<const TreeStore> base;
    std::unique_ptr<FoliageOpHost> host;
    TerrainOpResult result;
    std::atomic<bool>* stop = nullptr;
    jobs::Task<void> task;
    std::atomic<bool> done{false};

    // T4 R27: a World's list runs tile by tile, each op in the tile's frame; the edit is main-thread only
    struct TileRun {
        TileCoord coord;
        Vector2 centre;
        std::unique_ptr<RawMapStore> snapshot;
        std::vector<Op> ops;
        std::vector<int> index;     // each op's place in the list
        std::shared_ptr<const TreeStore> base;
        std::unique_ptr<FoliageOpHost> host;
        TerrainOpResult result;
        uint64_t revision = 0;
    };
    bool world = false;
    std::string planError;
    std::vector<TileRun> tiles;
    std::unique_ptr<TerrainWorldTreeEdit> edit;

    void runWorld() {
        result = TerrainOpResult();
        result.dryRun = options.dryRun;
        if (!planError.empty()) {
            result.error = planError;
            return;
        }
        result.ok = true;
        result.ops.resize(ops.size());
        for (size_t i = 0; i < ops.size(); i++) result.ops[i].op = ops[i].registeredName;
        std::vector<std::string> hashes(ops.size());
        for (TileRun& tile : tiles) {
            if (host->shouldStop && host->shouldStop()) break;
            FoliageOpScope scope(*tile.host);
            tile.result = applyOps(*tile.snapshot, tile.ops, options);
            if (!tile.result.ok) {
                result.ok = false;
                result.error = "tile (" + std::to_string(tile.coord.x) + ", " + std::to_string(tile.coord.z) + "): " + tile.result.error;
                return;
            }
            for (size_t k = 0; k < tile.result.ops.size() && k < tile.index.size(); k++) {
                mergeWorldEntry(result.ops[tile.index[k]], tile.result.ops[k], tile.coord, tile.centre, hashes[tile.index[k]]);
            }
            for (MapChangeReport change : tile.result.changed) {
                change.localMin = change.localMin + tile.centre;
                change.localMax = change.localMax + tile.centre;
                result.changed.push_back(change);
            }
            result.warnings.insert(result.warnings.end(), tile.result.warnings.begin(), tile.result.warnings.end());
        }
        for (size_t i = 0; i < ops.size(); i++) {
            if (hashes[i].empty()) continue;
            char text[17];
            std::snprintf(text, sizeof(text), "%016" PRIx64, fnv1a64(hashes[i]));
            OpParam hash;
            hash.kind = OpParam::Kind::String;
            hash.text = text;
            result.ops[i].fields.emplace_back("hash", std::move(hash));
        }
    }

    void run() {
        if (world) {
            runWorld();
        } else {
            FoliageOpScope scope(*host);
            result = applyOps(*snapshot, ops, options);
        }
        done.store(true);
        // The editor's main-thread queue runs every loop and wakes an idle one; Engine's postToMain
        // drains only in frames that draw a scene, so an idle editor would never land the result
        getEditorHost().enqueueMainThreadTask([]() { FoliageOpRunner::updateAll(); });
    }
};

void editor::FoliageOpRunner::fillHost(Project* project, uint32_t sceneId, Entity entity, FoliageOpHost& host) {
    SceneProject* sceneProject = project->getScene(sceneId);
    if (!sceneProject) return;
    Scene* scene = sceneProject->scene;
    const TerrainComponent& terrain = scene->getComponent<TerrainComponent>(entity);

    for (const TerrainTreePrototype& prototype : terrain.treePrototypes) {
        FoliageOpHost::Prototype entry;
        entry.id = static_cast<uint16_t>(prototype.id);
        entry.name = fs::path(prototype.meshPath.str()).stem().string();
        host.prototypes.push_back(entry);
    }
    for (const TerrainFoliageLayer& layer : terrain.foliageLayers) {
        FoliageOpHost::DetailLayer entry;
        entry.name = fs::path(layer.meshPath.str()).stem().string();
        entry.density = layer.density;
        entry.hasMap = !layer.densityMap.empty();
        host.layers.push_back(entry);
    }

    // the scene's splines by name, terrain-local, as polylines of their stations
    const Matrix4 inverse = GameObject(scene, entity).getModelMatrix().inverse();
    auto splines = scene->getComponentArray<SplineComponent>();
    for (int i = 0; i < splines->size(); i++) {
        const Entity splineEntity = splines->getEntity(i);
        const SplineComponent& spline = splines->getComponentFromIndex(i);
        const Matrix4 splineModel = GameObject(scene, splineEntity).getModelMatrix();
        std::vector<Vector2> line;
        const float length = spline::length(spline.spline);
        const int samples = std::clamp(static_cast<int>(length / 2.0f) + 2, 2, 2048);
        for (int s = 0; s < samples; s++) {
            const float t = static_cast<float>(s) / static_cast<float>(samples - 1);
            const Vector3 world = splineModel * spline::evaluatePosition(spline.spline, t);
            const Vector3 local = inverse * world;
            line.emplace_back(local.x, local.z);
        }
        host.splines[scene->getEntityName(splineEntity)].push_back(std::move(line));
    }

    // the Terrain's instanced children (Paint Trees' hosts), convert_placed_to_trees' sources
    auto instancedMeshes = scene->getComponentArray<InstancedMeshComponent>();
    for (int i = 0; i < instancedMeshes->size(); i++) {
        const Entity child = instancedMeshes->getEntity(i);
        const InstancedMeshComponent* instmesh = &instancedMeshes->getComponentFromIndex(i);
        const GameObject childObject(scene, child);
        const std::optional<GameObject> parent = childObject.getParent();
        if (!parent || parent->getEntity() != entity) continue;
        const ModelComponent* model = scene->findComponent<ModelComponent>(child);
        if (!instmesh || !model || instmesh->foliage) continue;
        const Matrix4 childLocal = childObject.getLocalMatrix();
        FoliageOpHost::PlacedHost placed;
        placed.name = scene->getEntityName(child);
        const std::string stem = fs::path(model->filename.str()).stem().string();
        for (const TerrainTreePrototype& prototype : terrain.treePrototypes) {
            if (fs::path(prototype.meshPath.str()).stem().string() == stem) placed.prototypeId = static_cast<uint16_t>(prototype.id);
        }
        for (const InstanceData& instance : instmesh->instances) {
            FoliageOpHost::PlacedInstance entry;
            const Vector3 local = childLocal * instance.position;
            entry.position[0] = local.x;
            entry.position[1] = local.y;
            entry.position[2] = local.z;
            entry.rotation[0] = instance.rotation.x;
            entry.rotation[1] = instance.rotation.y;
            entry.rotation[2] = instance.rotation.z;
            entry.rotation[3] = instance.rotation.w;
            entry.scale[0] = instance.scale.x;
            entry.scale[1] = instance.scale.y;
            entry.scale[2] = instance.scale.z;
            for (int c = 0; c < 4; c++) entry.color[c] = static_cast<uint8_t>(std::clamp(instance.color[c] * 255.0f + 0.5f, 0.0f, 255.0f));
            placed.instances.push_back(entry);
        }
        host.placedHosts[std::to_string(child)] = std::move(placed);
    }
}

Vector3 editor::FoliageOpRunner::toTerrainLocal(Scene* scene, Entity terrain, const Vector3& world) {
    const Matrix4 inverse = GameObject(scene, terrain).getModelMatrix().inverse();
    return inverse.isValid() ? inverse * world : world;
}

std::shared_ptr<editor::FoliageOpRunner> editor::FoliageOpRunner::start(Project* project, Request request, Finished finished, Progress progress) {
    auto runner = std::make_shared<FoliageOpRunner>();
    runner->project = project;
    runner->request = std::move(request);
    runner->onFinished = std::move(finished);
    runner->onProgress = std::move(progress);

    Scene* scene = project->getScene(runner->request.sceneId)->scene;
    const TerrainComponent& terrain = scene->getComponent<TerrainComponent>(runner->request.terrain);

    auto work = std::make_shared<Work>();
    work->ops = runner->request.ops;
    work->options.dryRun = runner->request.dryRun;
    // a job of its own: nothing to gain from helpers inside it, and the kernels stay deterministic
    work->options.parallel = false;

    if (terrain.world) {
        runner->planWorld(*work);
    } else {
        runner->planTerrain(*work);
    }
    FoliageOpRunner* raw = runner.get();
    work->host->shouldStop = [raw]() { return raw->stop.load(); };
    runner->work = work;

    if (jobs::hasSystem() && !jobs::system().isSingleThreaded()) {
        work->task = jobs::submit([work]() { work->run(); }, {jobs::Lane::Background, jobs::GateClass::Default, "foliage-ops"});
    } else {
        work->run();
    }
    runningOps().push_back(runner);
    return runner;
}

void editor::FoliageOpRunner::planTerrain(Work& work) {
    FoliageOpRunner& runner = *this;
    WantedMaps wanted;
    for (const Op& op : work.ops) scanParams(op.params, wanted);
    SceneProject* sceneProject = project->getScene(runner.request.sceneId);
    Scene* scene = sceneProject->scene;
    TerrainComponent& terrain = scene->getComponent<TerrainComponent>(runner.request.terrain);
    FoliageSystem* foliage = scene->getSystem<FoliageSystem>().get();
    work.snapshot = std::make_unique<RawMapStore>(terrain.terrainSize, terrain.maxHeight);
    TerrainComponentStore live(scene, runner.request.terrain, terrain);
    copyMap(live, *work.snapshot, MapId{MapKind::Height, 0});
    copyMap(live, *work.snapshot, MapId{MapKind::Hole, 0});
    if (wanted.blend) {
        for (int i = 0; i < static_cast<int>(terrain.blendMaps.size()); i++) copyMap(live, *work.snapshot, MapId{MapKind::Blend, i});
    }
    for (int index : wanted.density) {
        if (index < 0) {
            for (int i = 0; i < static_cast<int>(terrain.foliageLayers.size()); i++) copyMap(live, *work.snapshot, MapId{MapKind::Density, i});
        } else if (index < static_cast<int>(terrain.foliageLayers.size())) {
            copyMap(live, *work.snapshot, MapId{MapKind::Density, index});
        }
    }

    std::shared_ptr<TreeStore> store = foliage->getOrCreateTreeStore(runner.request.terrain);
    runner.storeRevision = store ? store->revision() : 0;
    if (store) work.base = std::make_shared<TreeStore>(*store);
    work.host = std::make_unique<FoliageOpHost>(work.base);
    fillHost(project, runner.request.sceneId, runner.request.terrain, *work.host);
}

// T4 R27: each op goes to the tiles it reaches, moved into each tile's frame; a tile's trees are its
// store as the edit reads it (a cold one from its file). Points over no tile are skipped: trees need ground.
void editor::FoliageOpRunner::planWorld(Work& work) {
    FoliageOpRunner& runner = *this;
    WantedMaps wanted;
    for (const Op& op : work.ops) scanParams(op.params, wanted);
    const uint32_t sceneId = runner.request.sceneId;
    const Entity entity = runner.request.terrain;
    Scene* scene = project->getScene(sceneId)->scene;
    const TerrainComponent& terrain = scene->getComponent<TerrainComponent>(entity);
    work.world = true;
    // the stop flag and the totals hang on a host of its own
    work.host = std::make_unique<FoliageOpHost>(std::make_shared<TreeStore>());
    work.edit = std::make_unique<TerrainWorldTreeEdit>(project, sceneId, entity);
    TerrainWorldTreeEdit& edit = *work.edit;
    if (!edit.runtime()) {
        work.planError = edit.error();
        return;
    }
    const WorldLattice& lattice = edit.runtime()->lattice();
    FoliageOpHost shared;
    fillHost(project, sceneId, entity, shared);

    std::map<uint32_t, size_t> runOf;
    const auto runFor = [&](TileCoord tile) -> Work::TileRun* {
        const auto found = runOf.find(tile.key());
        if (found != runOf.end()) return &work.tiles[found->second];
        std::string why;
        const TreeStore* trees = edit.trees(tile, &why);
        if (!trees) {
            if (work.planError.empty()) work.planError = why + "; nothing was changed.";
            return nullptr;
        }
        std::shared_ptr<const WorldTile> data = edit.runtime()->tile(tile);
        if (!data) {
            if (work.planError.empty()) work.planError = "tile (" + std::to_string(tile.x) + ", " + std::to_string(tile.z) + ") could not be read; nothing was changed.";
            return nullptr;
        }
        Work::TileRun run;
        run.coord = tile;
        run.centre = edit.centre(tile);
        run.snapshot = worldTileSnapshot(*data, lattice, edit.maxHeight(), wanted, static_cast<int>(terrain.foliageLayers.size()));
        run.base = std::make_shared<TreeStore>(*trees);
        run.revision = trees->revision();
        run.host = std::make_unique<FoliageOpHost>(run.base);
        run.host->prototypes = shared.prototypes;
        run.host->layers = shared.layers;
        for (const auto& [name, lines] : shared.splines) {
            std::vector<std::vector<Vector2>>& moved = run.host->splines[name];
            for (const std::vector<Vector2>& line : lines) {
                std::vector<Vector2> points;
                for (const Vector2& point : line) points.push_back(point - run.centre);
                moved.push_back(std::move(points));
            }
        }
        FoliageOpRunner* raw = &runner;
        run.host->shouldStop = [raw]() { return raw->stop.load(); };
        runOf[tile.key()] = work.tiles.size();
        work.tiles.push_back(std::move(run));
        return &work.tiles.back();
    };
    const auto shapeTiles = [&](const Op& op) {
        LocalBounds bounds;
        if (!ShapeEvaluator(op.shape).bounds(bounds)) return edit.presentTiles();
        return edit.tilesIn(bounds.min.x, bounds.min.y, bounds.max.x, bounds.max.y);
    };

    for (size_t i = 0; i < work.ops.size() && work.planError.empty(); i++) {
        const Op& op = work.ops[i];
        const std::string where = "ops[" + std::to_string(i) + "]";
        // what each tile gets of this op, before the frame moves
        std::map<uint32_t, Op> parts;
        if (op.registeredName == "place_trees") {
            std::map<uint32_t, OpParam> points;
            for (const OpParam& point : op.params.find("points") ? op.params.find("points")->items : std::vector<OpParam>()) {
                if (point.kind != OpParam::Kind::List || point.items.size() < 2) continue;
                const TileCoord tile = edit.tileAt(static_cast<float>(point.items[0].number), static_cast<float>(point.items[1].number));
                if (!edit.present(tile)) continue;
                OpParam moved = point;
                const Vector2 centre = edit.centre(tile);
                moved.items[0].number -= centre.x;
                moved.items[1].number -= centre.y;
                OpParam& list = points[tile.key()];
                list.kind = OpParam::Kind::List;
                list.items.push_back(std::move(moved));
            }
            if (points.empty()) {
                work.planError = where + ".points: every point is over no tile of the Terrain World (trees need ground); nothing was changed.";
                break;
            }
            for (auto& [key, list] : points) {
                Op part = op;
                setWorldParam(part.params, "points", std::move(list));
                parts[key] = std::move(part);
            }
        } else if (op.registeredName == "remove_trees" && op.params.find("ids")) {
            std::map<uint32_t, OpParam> ids;
            const std::vector<OpParam>& items = op.params.find("ids")->items;
            for (size_t k = 0; k < items.size(); k++) {
                TileCoord tile;
                uint32_t record = 0;
                if (items[k].number < 0.0 || !splitTerrainWorldTreeId(static_cast<uint64_t>(items[k].number), tile, record) || !edit.present(tile)) {
                    work.planError = where + ".ids[" + std::to_string(k) + "]: not a tree of this Terrain World; place_trees returns its ids.";
                    break;
                }
                OpParam& list = ids[tile.key()];
                list.kind = OpParam::Kind::List;
                list.items.push_back(worldNumberParam(record));
            }
            for (auto& [key, list] : ids) {
                Op part = op;
                setWorldParam(part.params, "ids", std::move(list));
                parts[key] = std::move(part);
            }
        } else if (op.registeredName == "convert_placed_to_trees") {
            const OpParam* hostParam = op.params.find("host");
            const std::string key = hostParam ? (hostParam->kind == OpParam::Kind::Number ? std::to_string(static_cast<int64_t>(hostParam->number)) : hostParam->text) : std::string();
            const auto placed = shared.placedHosts.find(key);
            if (placed == shared.placedHosts.end()) {
                // the op's own check names the hosts there are
                for (const TileCoord& tile : edit.presentTiles()) {
                    parts[tile.key()] = op;
                    break;
                }
            } else {
                for (const FoliageOpHost::PlacedInstance& instance : placed->second.instances) {
                    const TileCoord tile = edit.tileAt(instance.position[0], instance.position[2]);
                    if (!edit.present(tile)) {
                        work.planError = where + ": an instance of " + placed->second.name + " is over no tile of the Terrain World (trees need ground); nothing was changed.";
                        break;
                    }
                    parts[tile.key()] = op;
                }
            }
        } else {
            const std::vector<TileCoord> reached = shapeTiles(op);
            for (const TileCoord& tile : reached) {
                Op part = op;
                // Mass Place's count is the World's: with no shape, the density that attempts it over every tile
                const OpParam* count = op.params.find("count");
                if (count && op.shape.type == ShapeType::All && !reached.empty()) {
                    const double area = static_cast<double>(reached.size()) * edit.tileSize() * edit.tileSize();
                    part.params.fields.erase(std::remove_if(part.params.fields.begin(), part.params.fields.end(),
                                                            [](const auto& field) { return field.first == "count"; }), part.params.fields.end());
                    setWorldParam(part.params, "density", worldNumberParam(count->number / area));
                }
                // every tile its own pattern, the same on every run
                if (op.registeredName == "scatter_by_rules") {
                    const OpParam* seed = op.params.find("seed");
                    const uint32_t mixed = (seed ? static_cast<uint32_t>(seed->number) : op.seed) ^ (tile.key() * 0x9E3779B9u);
                    if (seed) setWorldParam(part.params, "seed", worldNumberParam(mixed));
                    part.seed = mixed;
                }
                parts[tile.key()] = std::move(part);
            }
            if (reached.empty()) {
                work.planError = where + ": the shape reaches no tile of the Terrain World; nothing was changed.";
                break;
            }
        }
        for (auto& [key, part] : parts) {
            Work::TileRun* run = runFor(TileCoord::fromKey(key));
            if (!run) break;
            translateWorldShape(part.shape, run->centre);
            run->ops.push_back(std::move(part));
            run->index.push_back(static_cast<int>(i));
        }
    }
    // the instanced hosts' instances each tile converts: its own, in its frame
    for (Work::TileRun& run : work.tiles) {
        for (const auto& [key, placed] : shared.placedHosts) {
            FoliageOpHost::PlacedHost own = placed;
            own.instances.clear();
            for (const FoliageOpHost::PlacedInstance& instance : placed.instances) {
                if (edit.tileAt(instance.position[0], instance.position[2]) != run.coord) continue;
                FoliageOpHost::PlacedInstance moved = instance;
                moved.position[0] -= run.centre.x;
                moved.position[2] -= run.centre.y;
                own.instances.push_back(moved);
            }
            run.host->placedHosts[key] = std::move(own);
        }
    }
}

bool editor::FoliageOpRunner::poll() {
    if (finished) return false;
    if (!work->done.load()) return true;
    commit();
    finished = true;
    return false;
}

void editor::FoliageOpRunner::commit() {
    Outcome outcome;
    outcome.result = std::move(work->result);
    outcome.cancelled = stop.load();
    if (work->world) {
        for (const Work::TileRun& tile : work->tiles) {
            outcome.treesBefore += static_cast<int64_t>(tile.base->size());
            outcome.treesAfter += static_cast<int64_t>(tile.host->current().size());
        }
        commitWorld(outcome);
        if (onFinished) onFinished(outcome);
        return;
    }
    outcome.treesBefore = work->base ? static_cast<int64_t>(work->base->size()) : 0;
    outcome.treesAfter = static_cast<int64_t>(work->host->current().size());

    SceneProject* sceneProject = project->getScene(request.sceneId);
    if (!sceneProject || !sceneProject->scene->findComponent<TerrainComponent>(request.terrain)) {
        outcome.error = "The Terrain went away while its operation ran.";
    } else if (!outcome.result.ok) {
        outcome.error = outcome.result.error;
    } else if (!request.dryRun) {
        Scene* scene = sceneProject->scene;
        TerrainComponent& terrain = scene->getComponent<TerrainComponent>(request.terrain);
        FoliageSystem* foliage = scene->getSystem<FoliageSystem>().get();

        // T1 R69: computed on a snapshot, so a base changed meanwhile fails with a clear error
        std::shared_ptr<TreeStore> store = foliage->getOrCreateTreeStore(request.terrain);
        if (work->host->touched() && store && store->revision() != storeRevision) {
            outcome.error = "The Terrain's trees changed while this ran; nothing was applied. Run it again.";
            outcome.result.ok = false;
        } else {
            CommandHistory* history = CommandHandle::get(request.sceneId);
            const size_t start = history->mark();

            // the density maps first, as T1's delta step: the snapshot's bytes over what changed
            {
                EditorTerrainStore writer(project, request.sceneId, scene, request.terrain, terrain);
                writer.setKeepsMaps(true);
                OverlayMapStore overlay(writer);
                for (const MapChangeReport& change : outcome.result.changed) {
                    if (change.map.kind != MapKind::Density) continue;
                    const MapFormat format = work->snapshot->format(change.map);
                    if (!format.exists) continue;
                    std::vector<uint8_t> after(static_cast<size_t>(change.rect.area()) * bytesPerTexel(format));
                    if (work->snapshot->readBytes(change.map, change.rect, after)) overlay.writeBytes(change.map, change.rect, after);
                }
                auto delta = std::make_unique<TerrainMapDeltaCmd>(project, request.sceneId, request.terrain, request.terrain, overlay, false, request.undoName);
                if (!delta->empty()) {
                    Command* command = delta.release();
                    command->setNoMerge();
                    history->addCommand(command);
                }
                overlay.discard();
            }

            if (work->host->touched() && store) {
                TreeDelta delta = work->host->delta();
                auto* command = new TreeEditCmd(project, request.sceneId, request.terrain, request.undoName, std::move(delta));
                // a convert empties what it took, in the same step
                if (!work->host->convertedHosts.empty()) {
                    command->alongside = emptyPlacedHosts(project, request.sceneId, work->host->convertedHosts);
                }
                history->addCommand(command);
            }
            history->groupSince(start, request.undoName);
        }
    }

    if (onFinished) onFinished(outcome);
}

// T4 R27: the tiles' density into the World's maps as T1's delta step, then their trees as one step of
// tile files, grouped
void editor::FoliageOpRunner::commitWorld(Outcome& outcome) {
    SceneProject* sceneProject = project->getScene(request.sceneId);
    if (!sceneProject || !sceneProject->scene->findComponent<TerrainComponent>(request.terrain)) {
        outcome.error = "The Terrain went away while its operation ran.";
        return;
    }
    if (!outcome.result.ok) {
        outcome.error = outcome.result.error;
        return;
    }
    if (request.dryRun || outcome.cancelled) return;
    TerrainWorldTreeEdit& edit = *work->edit;
    if (!edit.runtime()) {
        outcome.error = edit.error();
        outcome.result.ok = false;
        return;
    }
    for (const Work::TileRun& tile : work->tiles) {
        const TreeStore* now = tile.host->touched() ? edit.trees(tile.coord) : nullptr;
        if (tile.host->touched() && (!now || now->revision() != tile.revision)) {
            outcome.error = "The Terrain World's trees changed while this ran; nothing was applied. Run it again.";
            outcome.result.ok = false;
            return;
        }
    }

    Scene* scene = sceneProject->scene;
    TerrainComponent& terrain = scene->getComponent<TerrainComponent>(request.terrain);
    CommandHistory* history = CommandHandle::get(request.sceneId);
    const size_t start = history->mark();
    {
        const WorldLattice& lattice = edit.runtime()->lattice();
        EditorTerrainStore writer(project, request.sceneId, scene, request.terrain, terrain);
        writer.setKeepsMaps(true);
        OverlayMapStore overlay(writer);
        for (const Work::TileRun& tile : work->tiles) {
            for (const MapChangeReport& change : tile.result.changed) {
                if (change.map.kind != MapKind::Density) continue;
                const MapFormat format = tile.snapshot->format(change.map);
                if (!format.exists) continue;
                std::vector<uint8_t> after(static_cast<size_t>(change.rect.area()) * bytesPerTexel(format));
                if (!tile.snapshot->readBytes(change.map, change.rect, after)) continue;
                const TexelRect global{lattice.densityOrigin() + tile.coord.x * lattice.densitySide() + change.rect.x,
                                       lattice.densityOrigin() + tile.coord.z * lattice.densitySide() + change.rect.y, change.rect.w, change.rect.h};
                overlay.writeBytes(change.map, global, after);
            }
        }
        auto delta = std::make_unique<TerrainMapDeltaCmd>(project, request.sceneId, request.terrain, request.terrain, overlay, false, request.undoName);
        if (!delta->empty()) {
            Command* command = delta.release();
            command->setNoMerge();
            history->addCommand(command);
        }
        overlay.discard();
    }

    std::vector<std::string> converted;
    for (const Work::TileRun& tile : work->tiles) {
        if (!tile.host->touched()) continue;
        edit.applyDelta(tile.coord, tile.host->delta());
        for (const std::string& host : tile.host->convertedHosts) {
            if (std::find(converted.begin(), converted.end(), host) == converted.end()) converted.push_back(host);
        }
    }
    std::string error;
    if (!edit.commit(request.undoName, &error, converted.empty() ? nullptr : emptyPlacedHosts(project, request.sceneId, converted))) {
        while (history->position() > start && history->canUndo() && history->undo()) {}
        outcome.error = error;
        outcome.result.ok = false;
        return;
    }
    history->groupSince(start, request.undoName);
}

void editor::FoliageOpRunner::updateAll() {
    std::vector<std::shared_ptr<FoliageOpRunner>>& running = runningOps();
    if (running.empty()) return;
    for (size_t i = 0; i < running.size();) {
        if (running[i]->poll()) {
            i++;
        } else {
            running.erase(running.begin() + static_cast<std::ptrdiff_t>(i));
        }
    }
}
