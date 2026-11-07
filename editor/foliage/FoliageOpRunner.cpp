// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "foliage/FoliageOpRunner.h"
#include "spline/SplineEvaluator.h"

#include "Project.h"
#include "command/CommandHandle.h"
#include "command/type/TreeEditCmd.h"
#include "component/InstancedMeshComponent.h"
#include "component/MeshComponent.h"
#include "component/ModelComponent.h"
#include "component/SplineComponent.h"
#include "component/TerrainComponent.h"
#include "object/GameObject.h"
#include "foliage/FoliageMapPatchCmd.h"
#include "foliage/TreeDataFile.h"
#include "jobs/JobSystem.h"
#include "object/TerrainEdit.h"
#include "subsystem/FoliageSystem.h"
#include "terrain/FoliageScatter.h"

#include <optional>
#include <algorithm>
#include <filesystem>

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

    void copyMap(TerrainMapStore& from, RawMapStore& to, MapId map) {
        const MapFormat format = from.format(map);
        if (!format.exists || format.bounds.empty()) return;
        std::vector<uint8_t> bytes(static_cast<size_t>(format.bounds.area()) * bytesPerTexel(format));
        if (!from.readBytes(map, format.bounds, bytes)) return;
        to.addMap(map, format.bounds.w, format.bounds.h, format.channels, format.bitsPerChannel, std::move(bytes));
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

    void run() {
        FoliageOpScope scope(*host);
        result = applyOps(*snapshot, ops, options);
        done.store(true);
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

    SceneProject* sceneProject = project->getScene(runner->request.sceneId);
    Scene* scene = sceneProject->scene;
    TerrainComponent& terrain = scene->getComponent<TerrainComponent>(runner->request.terrain);
    FoliageSystem* foliage = scene->getSystem<FoliageSystem>().get();

    auto work = std::make_shared<Work>();
    work->snapshot = std::make_unique<RawMapStore>(terrain.terrainSize, terrain.maxHeight);
    work->ops = runner->request.ops;
    work->options.dryRun = runner->request.dryRun;
    // a job of its own: nothing to gain from helpers inside it, and the kernels stay deterministic
    work->options.parallel = false;

    WantedMaps wanted;
    for (const Op& op : work->ops) scanParams(op.params, wanted);
    TerrainComponentStore live(scene, runner->request.terrain, terrain);
    copyMap(live, *work->snapshot, MapId{MapKind::Height, 0});
    copyMap(live, *work->snapshot, MapId{MapKind::Hole, 0});
    if (wanted.blend) {
        for (int i = 0; i < static_cast<int>(terrain.blendMaps.size()); i++) copyMap(live, *work->snapshot, MapId{MapKind::Blend, i});
    }
    for (int index : wanted.density) {
        if (index < 0) {
            for (int i = 0; i < static_cast<int>(terrain.foliageLayers.size()); i++) copyMap(live, *work->snapshot, MapId{MapKind::Density, i});
        } else if (index < static_cast<int>(terrain.foliageLayers.size())) {
            copyMap(live, *work->snapshot, MapId{MapKind::Density, index});
        }
    }

    std::shared_ptr<TreeStore> store = foliage->getOrCreateTreeStore(runner->request.terrain);
    runner->storeRevision = store ? store->revision() : 0;
    if (store) work->base = std::make_shared<TreeStore>(*store);
    work->host = std::make_unique<FoliageOpHost>(work->base);
    fillHost(project, runner->request.sceneId, runner->request.terrain, *work->host);
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

            // the density maps first: their bytes as they were and are, over what changed
            TerrainComponentStore live(scene, request.terrain, terrain);
            for (const MapChangeReport& change : outcome.result.changed) {
                if (change.map.kind != MapKind::Density) continue;
                const MapFormat format = work->snapshot->format(change.map);
                if (!format.exists) continue;
                std::vector<uint8_t> after(static_cast<size_t>(change.rect.area()) * bytesPerTexel(format));
                std::vector<uint8_t> before(after.size());
                if (work->snapshot->readBytes(change.map, change.rect, after) && live.readBytes(change.map, change.rect, before)) {
                    history->addCommand(new FoliageMapPatchCmd(project, request.sceneId, request.terrain, change.map, change.rect, std::move(before), std::move(after), request.undoName));
                }
            }

            if (work->host->touched() && store) {
                TreeDelta delta = work->host->delta();
                auto* command = new TreeEditCmd(project, request.sceneId, request.terrain, request.undoName, std::move(delta));
                // a convert empties what it took, in the same step
                if (!work->host->convertedHosts.empty()) {
                    command->alongside = FoliageMapPatchCmd::emptyHosts(project, request.sceneId, work->host->convertedHosts);
                }
                history->addCommand(command);
            }
            history->groupSince(start, request.undoName);
        }
    }

    if (onFinished) onFinished(outcome);
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
