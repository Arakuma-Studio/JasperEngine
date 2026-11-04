// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "FoliageTools.h"
#include "PlayViewCommon.h"

#include "Project.h"
#include "component/TerrainComponent.h"
#include "foliage/TerrainOverlay.h"
#include "mcp/McpContext.h"
#include "mcp/McpJobs.h"
#include "mcp/McpRefs.h"
#include "mcp/McpValues.h"
#include "object/TerrainEdit.h"
#include "subsystem/FoliageSystem.h"
#include "terrain/FoliageRules.h"
#include "terrain/FoliageScatter.h"
#include "util/ImpostorBaker.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <filesystem>

using namespace jasperengine;
using namespace jasperengine::terrain;
namespace fs = std::filesystem;

namespace jasperengine::editor::mcp {

Scene* TerrainTarget::engineScene() const {
    return scene ? scene->scene : nullptr;
}

bool resolveTerrainTarget(const CallPtr& call, TerrainTarget& target) {
    target.project = call->ctx().project();
    std::string error;
    const Json& args = call->args();
    target.scene = McpRefs::resolveScene(target.project, args.contains("scene") ? args["scene"] : Json(), error);
    if (!target.scene || !target.scene->scene) {
        call->fail(error.empty() ? "The scene is not loaded." : error);
        return false;
    }
    Scene* scene = target.engineScene();
    if (args.contains("entity")) {
        target.entity = McpRefs::resolveEntity(target.scene, args["entity"], error);
        if (target.entity == NULL_ENTITY) {
            call->fail(error);
            return false;
        }
    } else {
        std::vector<Entity> terrains;
        for (Entity entity : target.scene->entities) {
            if (scene->findComponent<TerrainComponent>(entity)) terrains.push_back(entity);
        }
        if (terrains.size() == 1) {
            target.entity = terrains[0];
        } else {
            const std::vector<Entity> selected = target.project->getSelectedEntities(target.scene->id);
            if (selected.size() == 1 && scene->findComponent<TerrainComponent>(selected[0])) {
                target.entity = selected[0];
            } else {
                call->fail(terrains.empty() ? "The scene has no Terrain: create_entity type=terrain." : "The scene has several Terrains: name one in entity.");
                return false;
            }
        }
    }
    if (!scene->findComponent<TerrainComponent>(target.entity)) {
        call->fail("That GameObject has no Terrain.");
        return false;
    }
    return true;
}

void bakeImpostorsFromOps(const CallPtr& call, const TerrainTarget& target, const std::vector<Op>& ops, bool dryRun) {
    const Json& args = call->args();
    Scene* scene = target.engineScene();
    TerrainComponent& terrain = scene->getComponent<TerrainComponent>(target.entity);
    if (ops.size() != 1) {
        call->fail("bake_impostors runs alone in its call: it spans frames.");
        return;
    }
    const Op& op = ops.front();
    OpError opError;
    if (const OpParam* views = op.params.find("views")) {
        const int value = static_cast<int>(views->number);
        if (value != 4 && value != 8 && value != 16) {
            call->fail("ops[0].views: valid: 4, 8, 16.");
            return;
        }
    }
    if (const OpParam* resolution = op.params.find("resolution")) {
        const int value = static_cast<int>(resolution->number);
        if (value != 128 && value != 256 && value != 512) {
            call->fail("ops[0].resolution: valid: 128, 256, 512.");
            return;
        }
    }

    ImpostorBaker::Request request;
    request.sceneId = target.scene->id;
    request.terrain = target.entity;
    if (const OpParam* views = op.params.find("views")) request.views = static_cast<int>(views->number);
    if (const OpParam* resolution = op.params.find("resolution")) request.resolution = static_cast<int>(resolution->number);
    if (const OpParam* prototypes = op.params.find("prototypes")) {
        for (const OpParam& item : prototypes->items) {
            int index = -1;
            if (item.kind == OpParam::Kind::Number) {
                index = static_cast<int>(item.number);
            } else {
                for (size_t i = 0; i < terrain.treePrototypes.size(); i++) {
                    if (fs::path(terrain.treePrototypes[i].meshPath.str()).stem().string() == item.text) index = static_cast<int>(i);
                }
            }
            if (index < 0 || index >= static_cast<int>(terrain.treePrototypes.size())) {
                call->fail("ops[0].prototypes: no such tree prototype on this Terrain.");
                return;
            }
            request.prototypes.push_back(index);
        }
    } else {
        for (int i = 0; i < static_cast<int>(terrain.treePrototypes.size()); i++) request.prototypes.push_back(i);
    }
    if (request.prototypes.empty()) {
        call->fail("The Terrain has no tree prototypes to bake: add one through set_properties treePrototypes.");
        return;
    }

    int columns = 4, rows = 2;
    ImpostorBaker::atlasGrid(request.views, columns, rows);
    const int atlasWidth = columns * request.resolution;
    const int atlasHeight = rows * request.resolution;
    if (dryRun) {
        Json entries = Json::array();
        for (int prototype : request.prototypes) {
            entries.push_back({{"prototype", prototype}, {"views", request.views}, {"atlas", {atlasWidth, atlasHeight}}});
        }
        call->ok("Dry run of bake_impostors: nothing rendered.", {{"dry_run", true}, {"undo_name", "Bake Impostors"}, {"ops", Json::array({{{"op", "bake_impostors"}, {"entries", entries}}})}});
        return;
    }

    std::shared_ptr<McpJob> job = call->ctx().jobs().create("edit_terrain", "Bake impostors");
    Project* project = target.project;
    const uint32_t sceneId = target.scene->id;
    const Entity entity = target.entity;
    std::string error;
    const bool started = ImpostorBaker::start(project, request, [job, project, sceneId, entity](const std::vector<ImpostorBakeResult>& results, bool cancelled) {
        if (cancelled) {
            job->markCancelled("Bake cancelled; the prototypes keep their impostors.");
            return;
        }
        ImpostorBaker::applyResults(project, sceneId, entity, results);
        Json entries = Json::array();
        std::string failure;
        for (const ImpostorBakeResult& result : results) {
            if (!result.error.empty() && failure.empty()) failure = result.error;
            entries.push_back({{"prototype", result.prototype}, {"albedo", result.albedoPath}, {"normal", result.normalPath},
                               {"bounds_center", {result.boundsCenter[0], result.boundsCenter[1], result.boundsCenter[2]}},
                               {"size", {result.size[0], result.size[1]}}, {"views", result.views},
                               {"source_hash", result.sourceHash}, {"ms", result.milliseconds}});
        }
        Json data = {{"dry_run", false}, {"undo_name", "Bake Impostors"}, {"ops", Json::array({{{"op", "bake_impostors"}, {"entries", entries}}})}};
        if (!failure.empty()) job->fail(failure, data);
        else job->succeed("Baked " + std::to_string(results.size()) + " impostor(s).", data);
    }, [job](double share) { job->setProgress(share, "baking"); }, error);
    if (!started) {
        job->fail(error, Json::object());
        call->fail(error);
        return;
    }
    job->setCancelHandler([]() { ImpostorBaker::cancel(); });
    call->detach();
    replyWhenJobDone(call, job, args.value("wait", true), args.value("timeout_seconds", 300.0), [](const McpJob& finished) {
        Json result = finished.result();
        result["job_id"] = finished.id();
        result["status"] = jobStatusName(finished.status());
        if (finished.status() != JobStatus::Succeeded) return ToolResult::error(finished.message(), result);
        return ToolResult::success(finished.message(), result);
    });
}

void visualizeFoliage(const CallPtr& call, const TerrainTarget& target) {
    const Json& args = call->args();
    const std::string overlay = args.value("overlay", std::string("none"));
    TerrainOverlay::Kind kind = TerrainOverlay::Kind::None;
    if (overlay == "density") kind = TerrainOverlay::Kind::Density;
    else if (overlay == "tree_density") kind = TerrainOverlay::Kind::TreeDensity;
    else if (overlay != "none") {
        call->fail("overlay: valid here are none, density, tree_density.");
        return;
    }
    const int layer = args.value("layer", 0);
    const float opacity = args.value("opacity", 0.7f);
    TerrainTarget local = target;
    TerrainOverlay::show(local.scene, local.entity, kind, layer, opacity);

    // T5 R57: the tier of this capture only
    if (args.contains("foliage_quality")) {
        const std::string name = args["foliage_quality"].get<std::string>();
        FoliageQuality quality;
        if (name == "low") quality = FoliageQuality::LOW;
        else if (name == "medium") quality = FoliageQuality::MEDIUM;
        else if (name == "high") quality = FoliageQuality::HIGH;
        else {
            call->fail("foliage_quality: valid: low, medium, high.");
            return;
        }
        Engine::setFoliageQuality(quality);
    }

    const float top = TerrainOverlay::maxValue(target.scene->id);
    Json legend = Json::array();
    if (kind != TerrainOverlay::Kind::None) {
        legend.push_back({{"value", 0.0f}, {"color", "blue"}, {"label", "none"}});
        legend.push_back({{"value", top * 0.5f}, {"color", "green"}, {"label", "per square unit"}});
        legend.push_back({{"value", top}, {"color", "red"}, {"label", "per square unit"}});
    }
    Json data = {{"overlay", overlay}, {"legend", legend}, {"triangles", TerrainOverlay::triangleCount(target.scene->id)}};
    const bool keepTier = args.contains("foliage_quality");
    target.scene->needUpdateRender = true;
    if (!args.value("screenshot", true)) {
        if (keepTier) Engine::resetFoliageQuality();
        call->ok("Overlay " + overlay + ".", data);
        return;
    }
    w3::CaptureRequest request;
    request.view = "scene";
    request.scene = target.scene->id;
    request.maxWidth = args.value("max_width", 1024);
    const std::string text = "Overlay " + overlay + ".";
    call->afterFrames(2, [call, request, data, text, keepTier]() {
        w3::captureView(call, request, [call, data, text, keepTier](w3::CaptureResult result) {
            if (keepTier) Engine::resetFoliageQuality();
            if (!result.ok) {
                call->fail("Screenshot failed: " + result.error, data);
                return;
            }
            Json out = data;
            out["image"] = result.info;
            ToolResult reply = ToolResult::success(text + " Image: the Scene view.", out);
            reply.images.push_back(std::move(result.image));
            call->reply(std::move(reply));
        });
    });
}

Json foliageStatsJson(Project* project, SceneProject* sceneProject, Entity entity, const std::optional<FoliageRegionXZ>& region) {
    Scene* scene = sceneProject->scene;
    TerrainComponent& terrain = scene->getComponent<TerrainComponent>(entity);
    FoliageSystem* foliage = scene->getSystem<FoliageSystem>().get();
    const FoliageStats& stats = foliage->getStats(entity);

    const float half = terrain.terrainSize * 0.5f;
    const FoliageRegionXZ bounds = region.value_or(FoliageRegionXZ{-half, -half, half, half});
    const int firstX = static_cast<int>(std::floor(std::max(bounds.minX, -half) / FOLIAGE_DETAIL_CELL_SIZE));
    const int lastX = static_cast<int>(std::floor(std::min(bounds.maxX, half) / FOLIAGE_DETAIL_CELL_SIZE));
    const int firstZ = static_cast<int>(std::floor(std::max(bounds.minZ, -half) / FOLIAGE_DETAIL_CELL_SIZE));
    const int lastZ = static_cast<int>(std::floor(std::min(bounds.maxZ, half) / FOLIAGE_DETAIL_CELL_SIZE));
    const int64_t cells = static_cast<int64_t>(lastX - firstX + 1) * (lastZ - firstZ + 1);

    TerrainComponentStore store(scene, entity, terrain);
    const MapId heightId{MapKind::Height, 0};
    const MapFormat heightFormat = store.format(heightId);
    std::vector<uint8_t> heightBytes;
    if (heightFormat.exists) {
        heightBytes.resize(static_cast<size_t>(heightFormat.bounds.area()) * bytesPerTexel(heightFormat));
        if (!store.readBytes(heightId, heightFormat.bounds, heightBytes)) heightBytes.clear();
    }
    std::vector<uint8_t> holeBytes;
    const MapFormat holeFormat = store.format(MapId{MapKind::Hole, 0});
    if (holeFormat.exists) {
        holeBytes.resize(static_cast<size_t>(holeFormat.bounds.area()));
        if (!store.readBytes(MapId{MapKind::Hole, 0}, holeFormat.bounds, holeBytes)) holeBytes.clear();
    }

    Json details = Json::array();
    for (int l = 0; l < static_cast<int>(terrain.foliageLayers.size()); l++) {
        const TerrainFoliageLayer& layer = terrain.foliageLayers[l];
        Json entry = {{"layer", l}, {"mesh", McpValues::assetPathToJson(project, layer.meshPath)}};
        entry["drawn"] = foliage->getInstanceCount(entity, l);

        const MapId densityId{MapKind::Density, l};
        const MapFormat densityFormat = store.format(densityId);
        double meanDensity = 0.0;
        std::vector<uint8_t> densityBytes;
        if (densityFormat.exists) {
            densityBytes.resize(static_cast<size_t>(densityFormat.bounds.area()) * bytesPerTexel(densityFormat));
            if (store.readBytes(densityId, densityFormat.bounds, densityBytes)) {
                uint64_t sum = 0;
                const int step = bytesPerTexel(densityFormat);
                for (size_t i = 0; i < densityBytes.size(); i += static_cast<size_t>(step)) sum += densityBytes[i];
                meanDensity = static_cast<double>(sum) / 255.0 / static_cast<double>(densityFormat.bounds.area());
            } else {
                densityBytes.clear();
            }
        }
        entry["mean_density"] = meanDensity * layer.density;

        if (cells <= 4096 && !densityBytes.empty() && !heightBytes.empty()) {
            // exact: the scatter itself, cell by cell
            FoliageLayerParams params;
            params.seed = layer.seed;
            params.density = layer.density;
            params.minScale = layer.minScale;
            params.maxScale = layer.maxScale;
            params.rotationJitter = layer.rotationJitter;
            params.alignToNormal = layer.alignToNormal;
            params.minSlope = layer.minSlope;
            params.maxSlope = layer.maxSlope;
            params.minHeight = layer.minHeight;
            params.maxHeight = layer.maxHeight;
            int64_t count = 0;
            std::vector<FoliageInstance> placed;
            for (int z = firstZ; z <= lastZ; z++) {
                for (int x = firstX; x <= lastX; x++) {
                    FoliageCellInput cell;
                    cell.layer = params;
                    cell.cellX = x;
                    cell.cellZ = z;
                    cell.terrainSize = terrain.terrainSize;
                    cell.terrainMaxHeight = terrain.maxHeight;
                    cell.heightMap.texels = heightBytes.data();
                    cell.heightMap.mapWidth = cell.heightMap.width = heightFormat.bounds.w;
                    cell.heightMap.mapHeight = cell.heightMap.height = heightFormat.bounds.h;
                    cell.heightMap.channels = heightFormat.channels;
                    cell.heightMap.bytesPerChannel = heightFormat.bitsPerChannel >= 16 ? 2 : 1;
                    cell.densityMap.texels = densityBytes.data();
                    cell.densityMap.mapWidth = cell.densityMap.width = densityFormat.bounds.w;
                    cell.densityMap.mapHeight = cell.densityMap.height = densityFormat.bounds.h;
                    cell.densityMap.channels = densityFormat.channels;
                    cell.densityMap.bytesPerChannel = densityFormat.bitsPerChannel >= 16 ? 2 : 1;
                    if (!holeBytes.empty()) cell.holes = HoleMapView{holeBytes.data(), holeFormat.bounds.w, holeFormat.bounds.h};
                    placed.clear();
                    scatterFoliageCell(cell, placed);
                    for (const FoliageInstance& instance : placed) {
                        if (instance.position[0] >= bounds.minX && instance.position[0] <= bounds.maxX && instance.position[2] >= bounds.minZ && instance.position[2] <= bounds.maxZ) count++;
                    }
                }
            }
            entry["instances"] = count;
        } else {
            const double area = static_cast<double>(std::max(bounds.maxX - bounds.minX, 0.0f)) * std::max(bounds.maxZ - bounds.minZ, 0.0f);
            entry["instances"] = static_cast<int64_t>(meanDensity * layer.density * area);
            entry["estimated"] = true;
        }
        details.push_back(std::move(entry));
    }

    Json trees = Json::array();
    std::shared_ptr<const TreeStore> treeStore = static_cast<const FoliageSystem*>(foliage)->getTreeStore(entity);
    for (int p = 0; p < static_cast<int>(terrain.treePrototypes.size()); p++) {
        const TerrainTreePrototype& prototype = terrain.treePrototypes[p];
        int64_t count = 0;
        if (treeStore) {
            for (const TreeRecord& record : treeStore->records()) {
                if (record.prototypeId != prototype.id) continue;
                const float x = foliageTreeLocalX(record, terrain.terrainSize);
                const float z = foliageTreeLocalZ(record, terrain.terrainSize);
                if (x >= bounds.minX && x <= bounds.maxX && z >= bounds.minZ && z <= bounds.maxZ) count++;
            }
        }
        const ImpostorState state = ImpostorBaker::stateOf(project, prototype.meshPath.str(), prototype.impostorViews, prototype.impostorSourceHash);
        trees.push_back({{"prototype", p}, {"mesh", McpValues::assetPathToJson(project, prototype.meshPath)}, {"count", count},
                         {"impostor_state", state == ImpostorState::None ? "none" : state == ImpostorState::Baked ? "baked" : "out_of_date"}});
    }

    char hash[17];
    snprintf(hash, sizeof(hash), "%016llx", static_cast<unsigned long long>(foliage->getPlacementHash(entity)));
    Json totals = {
        {"resident", stats.detailResident + stats.treesResident}, {"visible", stats.detailVisible + stats.treesVisible},
        {"mesh_trees", stats.meshTrees}, {"trees_total", stats.treesTotal},
        {"cells", stats.cells}, {"draws", stats.draws}, {"stream_bytes", stats.streamBytes}, {"buffer_bytes", stats.bufferBytes},
        {"cpu_bytes", stats.cpuBytes}, {"gpu_bytes", stats.gpuBytes}, {"main_thread_ms", stats.mainThreadMs},
        {"pending_cells", stats.pendingCells}, {"budget_exceeded", stats.budgetExceeded}, {"hash", hash}
    };
    return Json{{"details", details}, {"trees", trees}, {"totals", totals}};
}

}
