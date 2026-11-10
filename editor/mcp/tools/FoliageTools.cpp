// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "FoliageTools.h"
#include "PlayViewCommon.h"

#include "Project.h"
#include "component/TerrainComponent.h"
#include "foliage/FoliageOpRunner.h"
#include "window/TerrainEditWindow.h"
#include "App.h"
#include "Backend.h"
#include "object/TerrainOpsJson.h"
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
using namespace jasperengine::terraincore;
namespace fs = std::filesystem;

namespace jasperengine::editor::mcp {

Scene* TerrainTarget::engineScene() const {
    return scene ? scene->scene : nullptr;
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

bool isFoliageOp(const Op& op) {
    if (op.type != OpType::Registered) return false;
    static const char* const NAMES[] = {"scatter_by_rules", "clear_foliage", "place_trees", "remove_trees", "convert_placed_to_trees", "bake_impostors"};
    for (const char* name : NAMES) {
        if (op.registeredName == name) return true;
    }
    return false;
}

namespace {

    Json foliageOutcomeJson(const FoliageOpRunner::Outcome& outcome, const OpSpace& space, const std::vector<Op>& ops, const std::string& undoName, bool dryRun) {
        Json data = opResultJson(outcome.result, space, ops);
        data["dry_run"] = dryRun;
        data["undo_name"] = undoName;
        data["trees"] = outcome.treesAfter;

        // R67: hints an agent must see go in the result
        Json next = Json::array();
        const FoliageTierLimits low = foliageTierLimits(FoliageTier::Low);
        if (outcome.treesAfter > static_cast<int64_t>(low.maxVisibleTrees)) {
            next.push_back("The Terrain has " + std::to_string(outcome.treesAfter) + " trees, past the Mobile budget of " + std::to_string(low.maxVisibleTrees) +
                           " visible: bake_impostors, and check sample_terrain foliage on a Low tier.");
        }
        if (!next.empty()) data["next"] = next;
        return data;
    }

}

void editFoliageFromOps(const CallPtr& call, const TerrainTarget& target, std::vector<Op> ops, const OpSpace& space, bool dryRun) {
    const Json& args = call->args();
    Scene* scene = target.engineScene();
    for (size_t i = 0; i < ops.size(); i++) {
        if (!isFoliageOp(ops[i])) {
            call->fail("ops[" + std::to_string(i) + "]: foliage ops (scatter_by_rules, clear_foliage, place_trees, remove_trees, convert_placed_to_trees, "
                       "bake_impostors) run in a call of their own: send the other ops in a second edit_terrain call.");
            return;
        }
    }
    if (ops.size() == 1 && registeredOpSpansFrames(ops[0].registeredName)) {
        bakeImpostorsFromOps(call, target, ops, dryRun);
        return;
    }
    for (size_t i = 0; i < ops.size(); i++) {
        if (registeredOpSpansFrames(ops[i].registeredName)) {
            call->fail("ops[" + std::to_string(i) + "]: bake_impostors runs alone in its call: it spans frames.");
            return;
        }
    }

    // A layer just added has no density map: scatter_by_rules on it starts from an empty one. In
    // memory only, so a dry run writes nothing either.
    TerrainEditWindow* window = Backend::getApp().getTerrainEditWindow();
    TerrainComponent& component = scene->getComponent<TerrainComponent>(target.entity);
    for (const Op& op : ops) {
        const OpParam* aim = op.params.find("target");
        const OpParam* layerParam = aim && aim->kind == OpParam::Kind::Object ? aim->find("detail_layer") : nullptr;
        if (!layerParam || layerParam->kind != OpParam::Kind::Number) continue;
        const int layer = static_cast<int>(layerParam->number);
        if (window && layer >= 0 && layer < static_cast<int>(component.foliageLayers.size()) && component.foliageLayers[layer].densityMap.empty()) {
            window->ensureDensityMap(target.project, target.scene, target.entity, layer);
        }
    }

    const std::vector<Op> listed = ops;
    FoliageOpRunner::Request request;
    request.sceneId = target.scene->id;
    request.terrain = target.entity;
    request.ops = std::move(ops);
    request.dryRun = dryRun;
    const std::string terrainName = scene->getEntityName(target.entity);
    const std::string& first = request.ops.front().registeredName;
    request.undoName = std::string(first == "scatter_by_rules" ? "Scatter Foliage" : first == "clear_foliage" ? "Clear Foliage" : first == "convert_placed_to_trees" ? "Convert Placed Instances" : first == "remove_trees" ? "Erase Trees" : "Paint Trees") + " (" + terrainName + ")";
    const std::string undoName = request.undoName;

    std::shared_ptr<McpJob> job = call->ctx().jobs().create("edit_terrain", undoName);
    std::shared_ptr<FoliageOpRunner> runner = FoliageOpRunner::start(target.project, std::move(request), [job, listed, space, undoName, dryRun](const FoliageOpRunner::Outcome& outcome) {
        Json data = foliageOutcomeJson(outcome, space, listed, undoName, dryRun);
        if (!outcome.error.empty()) {
            job->fail(outcome.error, data);
        } else if (outcome.cancelled) {
            job->markCancelled("Cancelled; nothing changed.");
        } else {
            job->succeed((dryRun ? std::string("Dry run of ") : std::string()) + undoName + ": " + std::to_string(outcome.treesAfter) + " trees.", data);
        }
    });
    std::weak_ptr<FoliageOpRunner> weak = runner;
    job->setCancelHandler([weak]() { if (auto alive = weak.lock()) alive->cancel(); });
    call->detach();
    replyWhenJobDone(call, job, args.value("wait", true), args.value("timeout_seconds", 120.0), [](const McpJob& finished) {
        Json result = finished.result();
        result["job_id"] = finished.id();
        result["status"] = jobStatusName(finished.status());
        if (finished.status() != JobStatus::Succeeded) return ToolResult::error(finished.message(), result);
        return ToolResult::success(finished.message(), result);
    });
}

Json foliageStatsJson(Project* project, SceneProject* sceneProject, Entity entity, const std::optional<FoliageRegionXZ>& region) {
    Scene* scene = sceneProject->scene;
    TerrainComponent& terrain = scene->getComponent<TerrainComponent>(entity);
    FoliageSystem* foliage = scene->getSystem<FoliageSystem>().get();
    const FoliageStats& stats = foliage->getStats(entity);
    const auto totalsOf = [&](const FoliageStats& counts) {
        char hash[17];
        snprintf(hash, sizeof(hash), "%016llx", static_cast<unsigned long long>(foliage->getPlacementHash(entity)));
        return Json{
            {"resident", counts.detailResident + counts.treesResident}, {"visible", counts.detailVisible + counts.treesVisible},
            {"mesh_trees", counts.meshTrees}, {"trees_total", counts.treesTotal},
            {"cells", counts.cells}, {"draws", counts.draws}, {"stream_bytes", counts.streamBytes}, {"buffer_bytes", counts.bufferBytes},
            {"cpu_bytes", counts.cpuBytes}, {"gpu_bytes", counts.gpuBytes}, {"main_thread_ms", counts.mainThreadMs},
            {"pending_cells", counts.pendingCells}, {"budget_exceeded", counts.budgetExceeded}, {"hash", hash}
        };
    };

    if (terrain.world) {
        // T4 R27: a World's foliage is what its resident tiles hold now, one region a tile
        Json details = Json::array();
        for (int l = 0; l < static_cast<int>(terrain.foliageLayers.size()); l++) {
            const uint64_t drawn = foliage->getInstanceCount(entity, l);
            details.push_back({{"layer", l}, {"mesh", McpValues::assetPathToJson(project, terrain.foliageLayers[l].meshPath)}, {"drawn", drawn}, {"instances", drawn}});
        }
        Json trees = Json::array();
        for (int p = 0; p < static_cast<int>(terrain.treePrototypes.size()); p++) {
            int64_t count = 0;
            foliage->forEachTreeStore(entity, [&](const TreeStore& store) {
                for (const TreeRecord& record : store.records()) count += record.prototypeId == terrain.treePrototypes[p].id ? 1 : 0;
            });
            trees.push_back({{"prototype", p}, {"mesh", McpValues::assetPathToJson(project, terrain.treePrototypes[p].meshPath)}, {"count", count}});
        }
        Json totals = totalsOf(stats);
        totals["regions"] = foliage->getRegionCount(entity);
        return Json{{"details", details}, {"trees", trees}, {"totals", totals}};
    }

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
                    cell.heightMap.bytesPerChannel = terraincore::bytesPerChannelOf(heightFormat.bitsPerChannel);
                    cell.densityMap.texels = densityBytes.data();
                    cell.densityMap.mapWidth = cell.densityMap.width = densityFormat.bounds.w;
                    cell.densityMap.mapHeight = cell.densityMap.height = densityFormat.bounds.h;
                    cell.densityMap.channels = densityFormat.channels;
                    cell.densityMap.bytesPerChannel = terraincore::bytesPerChannelOf(densityFormat.bitsPerChannel);
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

    return Json{{"details", details}, {"trees", trees}, {"totals", totalsOf(stats)}};
}

}
