// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// Navigation (docs/specs/S19-navigation.md R37): bake NavMesh Surfaces as a job, read the
// navigation state (settings, surfaces, grids, links, agents and crowds), query paths, samples,
// raycasts, closest edges and reachability on navmeshes or NavGrids (edit and play), change the
// project's navigation settings, paint NavGrid cells (one undo step) and show the Scene view's
// AI Navigation overlay (with a screenshot). S54 (docs/specs/S54-navigation-diagnostics.md
// R30-R34) adds why: failure reasons, locate with the walkability probe, the bake report,
// coverage, islands, link states and the findings. The guide is docs/mcp/guide/navigation.md.
#include "McpTools.h"

#include "mcp/McpCall.h"
#include "mcp/McpContext.h"
#include "mcp/McpJobs.h"
#include "mcp/McpRefs.h"
#include "mcp/McpRegistry.h"
#include "mcp/McpSchema.h"
#include "mcp/McpValues.h"
#include "mcp/tools/PlayViewCommon.h"

#include "Project.h"
#include "command/CommandHandle.h"
#include "command/CommandHistory.h"
#include "component/NavGridComponent.h"
#include "component/NavMeshAgentComponent.h"
#include "component/NavMeshLinkComponent.h"
#include "component/NavMeshModifierComponent.h"
#include "component/NavMeshObstacleComponent.h"
#include "component/NavMeshSurfaceComponent.h"
#include "component/Transform.h"
#include "navigation/NavBakeCollector.h"
#include "navigation/NavDiagnostics.h"
#include "navigation/NavGridBrush.h"
#include "navigation/NavGridCellsCmd.h"
#include "navigation/NavWalkabilityProbe.h"
#include "navigation/NavigationEditor.h"
#include "navigation/NavigationOverlay.h"
#include "navigation/NavigationSettingsCmd.h"
#include "sceneview/SceneViewController.h"
#include "subsystem/NavigationSystem.h"
#include "window/SceneWindow.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <map>
#include <memory>
#include <set>
#include <string>

namespace jasperengine::editor::mcp {

namespace {

using namespace schema;

double rounded(double value) {
    return std::round(value * 1000.0) / 1000.0;
}

Json vec3Json(const Vector3& v) {
    return Json::array({rounded(v.x), rounded(v.y), rounded(v.z)});
}

// a number in the text summaries, as the JSON shows it (12.1, not 12.100000)
std::string numberText(double value) {
    char text[32];
    std::snprintf(text, sizeof(text), "%g", rounded(value));
    return text;
}

const char* stateName(NavBakeState state) {
    switch (state) {
        case NavBakeState::NOT_BAKED: return "not_baked";
        case NavBakeState::BAKED: return "baked";
        case NavBakeState::DIRTY: return "dirty";
        case NavBakeState::BAKING: return "baking";
    }
    return "not_baked";
}

std::vector<Entity> sortedEntities(Scene* scene, const std::function<void(std::vector<Entity>&)>& fill) {
    std::vector<Entity> entities;
    fill(entities);
    std::sort(entities.begin(), entities.end());
    (void)scene;
    return entities;
}

template <typename T>
std::vector<Entity> entitiesWith(Scene* scene) {
    return sortedEntities(scene, [scene](std::vector<Entity>& out) {
        auto array = scene->getComponentArray<T>();
        for (size_t i = 0; i < array->size(); i++) out.push_back(array->getEntity(i));
    });
}

// the scene of a call: the running copy while playing (play: true), else the edited one
SceneProject* sceneOf(const CallPtr& call, bool allowPlay, std::string& error) {
    Project* project = call->ctx().project();
    const Json& args = call->args();
    const Json ref = args.contains("scene") ? args["scene"] : Json();
    if (allowPlay && project->getPlaySessionSceneId() != NULL_PROJECT_SCENE) {
        SceneProject* running = w3::resolveRuntimeScene(project, ref, error);
        if (running) return running;
        error.clear();
    }
    SceneProject* scene = McpRefs::resolveScene(project, ref, error);
    if (scene && !scene->scene) {
        error = McpRefs::notOpenError(project, *scene);
        return nullptr;
    }
    return scene;
}

Json settingsJson(const NavigationProjectSettings& project) {
    const nav::NavigationSettings& settings = project.settings;
    Json agents = Json::array();
    for (const nav::AgentTypeSettings& agent : settings.agentTypes) {
        agents.push_back({{"id", agent.id}, {"name", agent.name}, {"radius", agent.radius}, {"height", agent.height},
                          {"step_height", agent.stepHeight}, {"max_slope", agent.maxSlope}});
    }
    Json areas = Json::array();
    for (int i = 0; i < nav::AREA_COUNT; i++) {
        const nav::AreaSettings& area = settings.areas[(size_t)i];
        areas.push_back({{"index", i}, {"name", area.name}, {"cost", area.cost},
                         {"color", Json::array({rounded(area.color.x), rounded(area.color.y), rounded(area.color.z), rounded(area.color.w)})}});
    }
    return {{"agents", agents}, {"areas", areas}, {"crowd_max_agents", settings.crowdMaxAgents},
            {"web_bake_budget_ms", settings.webBakeBudgetMs}, {"max_grid_solves_per_tick", settings.maxGridSolvesPerTick},
            {"include_in_builds", project.includeInBuilds}};
}

// query_navigation's and visualize_navigation's agent type
Json agentTypeRef() {
    return Json{{"description", "Name or id. Default: the first."}, {"anyOf", Json::array({Json{{"type", "string"}}, Json{{"type", "integer"}}})}};
}

// a point of query_navigation and visualize_navigation: [x, y, z] or a GameObject (the handler
// tells them apart; one loose type list keeps the listing short)
Json navPointSchema(const std::string& more, size_t maxItems = 0) {
    Json point = {{"description", "[x, y, z] or a GameObject." + (more.empty() ? std::string() : " " + more)},
                  {"type", Json::array({"array", "integer", "string"})}};
    if (maxItems > 0) point["maxItems"] = maxItems;
    return point;
}

// at most `limit` islands, largest first (ties by id)
Json islandsJson(std::vector<nav::NavIsland> islands, size_t limit) {
    std::stable_sort(islands.begin(), islands.end(), [](const nav::NavIsland& a, const nav::NavIsland& b) { return a.area > b.area; });
    if (islands.size() > limit) islands.resize(limit);
    Json list = Json::array();
    for (const nav::NavIsland& island : islands) {
        list.push_back({{"id", island.id}, {"area_m2", rounded(island.area)}, {"polys", island.polys},
                        {"bounds", Json::array({vec3Json(island.boundsMin), vec3Json(island.boundsMax)})}});
    }
    return list;
}

// S54 R16, R31: one surface's bake report (bake_navigation's per surface, get_navigation_info's
// last_bake); the island fields once getLastBakeResult filled them
Json bakeReportJson(const NavBakeResult& result) {
    const nav::NavCollectStats& sources = result.sources;
    Json item = {{"ok", result.ok}, {"tiles_built", result.tilesBuilt}, {"tiles_total", result.tilesTotal}, {"polys", result.polys},
                 {"ms", rounded(result.milliseconds)}, {"warnings", result.warnings},
                 {"sources", {{"meshes", sources.meshes}, {"instances", sources.instances}, {"terrains", sources.terrains},
                              {"colliders", sources.colliders}, {"colliders_2d", sources.colliders2D}, {"volumes", sources.volumes},
                              {"links", sources.links}, {"excluded", sources.excluded}, {"triangles", sources.triangles}}}};
    if (result.islandCount >= 0) {
        item["walkable_area_m2"] = rounded(result.walkableArea);
        item["island_count"] = result.islandCount;
        item["islands"] = islandsJson(result.islands, 16);
    }
    if (!result.tiles.empty()) {
        std::vector<nav::NavTileReport> tiles = result.tiles;
        std::stable_sort(tiles.begin(), tiles.end(), [](const nav::NavTileReport& a, const nav::NavTileReport& b) {
            return a.milliseconds > b.milliseconds;
        });
        if (tiles.size() > 5) tiles.resize(5);
        Json slowest = Json::array();
        for (const nav::NavTileReport& tile : tiles) {
            slowest.push_back({{"tile", Json::array({tile.x, tile.y})}, {"ms", rounded(tile.milliseconds)}, {"polys", tile.polys},
                               {"kb", rounded((double)tile.bytes / 1024.0)}});
        }
        item["slowest_tiles"] = slowest;
    }
    if (result.cancelled) item["cancelled"] = true;
    if (!result.error.empty() && !result.cancelled) item["error"] = result.error;
    if (!result.ok && !result.cancelled && result.errorCode != nav::NavBakeError::None) {
        item["error_code"] = nav::bakeErrorName(result.errorCode);
        item["hint"] = result.hint.empty() ? std::string(nav::bakeErrorHint(result.errorCode)) : result.hint;
    }
    return item;
}

Json bakeResultJson(SceneProject* scene, Entity surface, const NavBakeResult& result) {
    Json item = {{"entity", McpRefs::entityJson(scene, surface)}};
    item.update(bakeReportJson(result));
    return item;
}

// ------------------------------------------------------------ bake_navigation

struct BakeCall {
    JobPtr job;
    uint32_t sceneId = 0;
    std::vector<Entity> surfaces;
    std::map<Entity, NavBakeResult> results;
    size_t waiting = 0;
};

void bakeNavigationTool(const CallPtr& call) {
    const Json& args = call->args();
    McpContext& ctx = call->ctx();
    Project* project = ctx.project();
    std::string error;
    SceneProject* scene = sceneOf(call, false, error);
    if (!scene) {
        call->fail(error);
        return;
    }
    if (scene->playState != ScenePlayState::STOPPED) {
        call->fail("The scene is playing: stop it first (a game rebakes with NavMeshSurface:bakeAsync()).");
        return;
    }
    if (!nav::isNavMeshAvailable()) {
        call->fail("This editor was built without Recast/Detour (JASPERENGINE_NAVIGATION=OFF).");
        return;
    }
    const std::string modeName = args.value("mode", std::string("dirty"));
    const NavBakeMode mode = modeName == "full" ? NavBakeMode::FULL : modeName == "clear" ? NavBakeMode::CLEAR : NavBakeMode::DIRTY;
    std::vector<Entity> surfaces;
    if (args.contains("surfaces") && !args["surfaces"].is_null()) {
        if (!McpRefs::resolveEntities(scene, args["surfaces"], surfaces, error)) {
            call->fail(error);
            return;
        }
        for (Entity entity : surfaces) {
            if (!scene->scene->findComponent<NavMeshSurfaceComponent>(entity)) {
                call->fail(McpRefs::entityPath(scene, entity) + " has no NavMesh Surface.");
                return;
            }
        }
    } else {
        surfaces = entitiesWith<NavMeshSurfaceComponent>(scene->scene);
    }
    if (surfaces.empty()) {
        call->fail("The scene has no NavMesh Surface: add_component navmeshsurface to the ground's GameObject first.");
        return;
    }

    auto state = std::make_shared<BakeCall>();
    state->sceneId = scene->id;
    state->surfaces = surfaces;
    state->job = ctx.jobs().create("bake_navigation", "Bake " + std::to_string(surfaces.size()) + " NavMesh Surface(s) (" + modeName + ")");
    // before any start: when every start is refused the job ends in the loop, with the reasons
    state->job->setProgress(0.0, "baking");
    std::weak_ptr<BakeCall> weak = state;
    state->job->setCancelHandler([weak, project]() {
        if (auto s = weak.lock()) {
            for (Entity surface : s->surfaces) NavigationEditor::get().cancel(project, s->sceneId, surface);
        }
    });
    auto finishIfDone = [state, project]() {
        if (state->waiting > 0 || state->job->status() != JobStatus::Running) return;
        SceneProject* sp = project->getScene(state->sceneId);
        Json list = Json::array();
        int ok = 0, built = 0, polys = 0;
        bool cancelled = false;
        std::string text;
        NavigationSystem* system = sp && sp->scene ? NavigationEditor::systemOf(*sp) : nullptr;
        for (Entity surface : state->surfaces) {
            NavBakeResult& result = state->results[surface];
            // the island fields label on their first read (never inside the landing, S54 R8)
            const NavBakeResult* last = system && result.ok && !result.cancelled ? system->getLastBakeResult(surface) : nullptr;
            if (last && last->islandCount >= 0) {
                result.walkableArea = last->walkableArea;
                result.islandCount = last->islandCount;
                result.islands = last->islands;
            }
            list.push_back(bakeResultJson(sp, surface, result));
            ok += result.ok ? 1 : 0;
            built += result.tilesBuilt;
            polys += result.polys;
            cancelled = cancelled || result.cancelled;
            std::string islands;
            if (result.ok && result.islandCount >= 0) {
                char area[48];
                std::snprintf(area, sizeof(area), ", %d island(s), %.1f m\xC2\xB2 walkable", result.islandCount, result.walkableArea);
                islands = area;
            }
            std::string failure = "failed: " + result.error;
            if (result.errorCode != nav::NavBakeError::None) {
                failure += " [" + std::string(nav::bakeErrorName(result.errorCode)) + "] " +
                           (result.hint.empty() ? std::string(nav::bakeErrorHint(result.errorCode)) : result.hint);
            }
            text += "\n" + (sp && sp->scene ? McpRefs::entityPath(sp, surface) : std::to_string(surface)) + ": " +
                    (result.ok ? std::to_string(result.tilesBuilt) + "/" + std::to_string(result.tilesTotal) + " tiles, " +
                                     std::to_string(result.polys) + " polys, " + std::to_string((int)result.milliseconds) + " ms" + islands
                               : (result.cancelled ? std::string("cancelled") : failure));
            for (const std::string& warning : result.warnings) text += "\n  warning: " + warning;
        }
        Json data = {{"surfaces", list}, {"tiles_built", built}, {"polys", polys}};
        if (cancelled) {
            state->job->markCancelled("Bake cancelled; the previous navmesh stays." + text);
        } else if (ok == (int)state->surfaces.size()) {
            state->job->succeed("Baked " + std::to_string(ok) + " surface(s)." + text, data);
        } else {
            state->job->fail(std::to_string(state->surfaces.size() - (size_t)ok) + " surface(s) failed." + text, data);
        }
    };
    for (Entity surface : surfaces) {
        state->waiting++;
        std::string why;
        const bool started = NavigationEditor::get().bake(project, scene->id, surface, mode, why, [state, surface, finishIfDone](const NavBakeResult& result) {
            state->results[surface] = result;
            state->waiting--;
            const size_t done = state->surfaces.size() - state->waiting;
            state->job->setProgress((double)done / (double)state->surfaces.size(), std::to_string(done) + " surface(s) done");
            finishIfDone();
        });
        if (!started) {
            // a bake that cannot start is the surface's last result (its error code and what it
            // collected); an error before the system was asked has only the text
            NavigationSystem* system = NavigationEditor::systemOf(*scene);
            const NavBakeResult* last = system ? system->getLastBakeResult(surface) : nullptr;
            NavBakeResult failed;
            if (last && !last->ok && last->error == why) failed = *last;
            failed.ok = false;
            failed.error = why;
            state->results[surface] = failed;
            state->waiting--;
        }
    }
    finishIfDone();
    call->detach();
    replyWhenJobDone(call, state->job, args.value("wait", true), args.value("timeout_seconds", 120.0), [state](const McpJob& finished) {
        Json result = finished.result();
        result["job_id"] = finished.id();
        result["status"] = jobStatusName(finished.status());
        if (finished.status() != JobStatus::Succeeded) return ToolResult::error(finished.message(), result);
        return ToolResult::success(finished.message(), result);
    });
}

// ------------------------------------------------------------ get_navigation_info

// a finding as get_navigation_info lists it (S54 R32); a detail holding a code reads as its name
Json findingJson(SceneProject* scene, const nav::NavFinding& finding) {
    Json details = Json::object();
    for (const auto& detail : finding.details) {
        if (detail.first == "error_code") {
            details[detail.first] = nav::bakeErrorName((nav::NavBakeError)(int)detail.second);
        } else if (detail.second == std::floor(detail.second) && std::fabs(detail.second) < 1e9) {
            details[detail.first] = (long long)detail.second;
        } else {
            details[detail.first] = rounded(detail.second);
        }
    }
    return {{"code", finding.code}, {"severity", nav::findingSeverityName(finding.severity)},
            {"entity", finding.entity != NULL_ENTITY ? McpRefs::entityJson(scene, finding.entity) : Json()},
            {"message", finding.message}, {"details", details}};
}

const char* areaName(int area) {
    return nav::getSettings().areas[(size_t)std::clamp(area, 0, nav::AREA_COUNT - 1)].name.c_str();
}

void getNavigationInfoTool(const CallPtr& call) {
    Project* project = call->ctx().project();
    const Json& args = call->args();
    std::string error;
    SceneProject* scene = sceneOf(call, true, error);
    if (!scene) {
        call->fail(error);
        return;
    }
    NavigationSystem* system = NavigationEditor::systemOf(*scene);
    Scene* s = scene->scene;
    const bool playing = scene->playState != ScenePlayState::STOPPED;
    // entities: every list and the findings keep only these GameObjects
    std::set<Entity> only;
    const bool filtered = args.contains("entities") && !args["entities"].is_null();
    if (filtered) {
        std::vector<Entity> list;
        if (!McpRefs::resolveEntities(scene, args["entities"], list, error)) {
            call->fail(error);
            return;
        }
        only.insert(list.begin(), list.end());
    }
    auto listed = [&](Entity entity) { return !filtered || only.count(entity) > 0; };
    std::string text = "Navigation of " + scene->name + (playing ? " (playing)" : "") + ":";

    // the dirty tile counts the findings reuse: the editor's cache in edit, the crowd's state in play
    std::map<Entity, int> dirtyOf;
    Json surfaces = Json::array();
    for (Entity entity : entitiesWith<NavMeshSurfaceComponent>(s)) {
        const NavMeshSurfaceComponent& surface = s->getComponent<NavMeshSurfaceComponent>(entity);
        const NavSurfaceStatus status = system->getSurfaceStatus(entity);
        const int dirty = playing ? status.dirtyTiles : NavigationEditor::get().dirtyTiles(project, scene->id, entity);
        dirtyOf[entity] = dirty;
        if (!listed(entity)) continue;
        Json item = {{"entity", McpRefs::entityJson(scene, entity)}, {"state", stateName(status.state)}, {"tiles", status.tiles},
                     {"dirty_tiles", dirty}, {"polys", status.polys}, {"bytes", status.bytes}, {"carving", status.carving},
                     {"agent_type", surface.agentType}, {"enabled", surface.enabled}, {"asset", surface.navMeshData.path}};
        if (status.state == NavBakeState::BAKING) item["progress"] = rounded(status.progress);
        if (!status.loadError.empty()) item["load_error"] = status.loadError;
        if (playing) item["crowd"] = {{"agents", status.crowdAgents}, {"capacity", status.crowdCapacity}};
        std::string coverageText;
        if (system->getNavMesh(entity)) {
            const nav::NavCoverage coverage = system->getCoverage(entity);
            item["coverage"] = {{"walkable_area_m2", rounded(coverage.walkableArea)}, {"island_count", coverage.islandCount},
                                {"largest_island_m2", rounded(coverage.largestIslandArea)}, {"tiles_area_m2", rounded(coverage.tilesArea)},
                                {"fraction", rounded(coverage.fraction)}};
            item["islands"] = islandsJson(system->getIslands(entity), 16);
            char line[64];
            std::snprintf(line, sizeof(line), ", %d island(s), %.1f m\xC2\xB2 walkable", coverage.islandCount, coverage.walkableArea);
            coverageText = line;
        }
        if (const NavBakeResult* last = system->getLastBakeResult(entity)) item["last_bake"] = bakeReportJson(*last);
        surfaces.push_back(item);
        text += "\n  surface " + McpRefs::entityPath(scene, entity) + ": " + stateName(status.state) + ", " + std::to_string(status.tiles) +
                " tiles, " + std::to_string(status.polys) + " polys" + coverageText +
                (dirty > 0 ? ", " + std::to_string(dirty) + " dirty tiles" : "");
    }
    Json grids = Json::array();
    for (Entity entity : entitiesWith<NavGridComponent>(s)) {
        if (!listed(entity)) continue;
        const NavGridComponent& grid = s->getComponent<NavGridComponent>(entity);
        nav::NavGridMap* map = system->getGridMap(entity);
        Json item = {{"entity", McpRefs::entityJson(scene, entity)}, {"layout", grid.layout == nav::GridLayout::Hex ? "hex" : "square"},
                     {"width", grid.width}, {"height", grid.height}, {"cell_size", grid.cellSize}, {"asset", grid.gridData.path}};
        if (map) {
            item["overrides"] = map->countOverrides();
            item["bytes"] = map->memoryBytes();
            Json counts = Json::object();
            std::vector<int> perArea(nav::AREA_COUNT, 0);
            for (int i = 0; i < map->getCellCount(); i++) perArea[(size_t)map->getAreaAt(i)]++;
            for (int a = 0; a < nav::AREA_COUNT; a++) {
                if (perArea[(size_t)a] > 0) counts[nav::getSettings().areas[(size_t)a].name] = perArea[(size_t)a];
            }
            item["cells_per_area"] = counts;
        }
        grids.push_back(item);
        text += "\n  grid " + McpRefs::entityPath(scene, entity) + ": " + std::to_string(grid.width) + " x " + std::to_string(grid.height);
    }
    // a link's connection on the first surface that holds it
    std::map<Entity, NavigationSystem::LinkStatus> linkStates;
    for (const NavigationSystem::LinkStatus& state : system->getLinkStatus()) {
        auto found = linkStates.find(state.link);
        if (found == linkStates.end() || (found->second.surface == NULL_ENTITY && state.surface != NULL_ENTITY)) linkStates[state.link] = state;
    }
    Json links = Json::array();
    for (Entity entity : entitiesWith<NavMeshLinkComponent>(s)) {
        if (!listed(entity)) continue;
        const NavMeshLinkComponent& link = s->getComponent<NavMeshLinkComponent>(entity);
        const Transform* transform = s->findComponent<Transform>(entity);
        const Matrix4 model = transform ? transform->modelMatrix : Matrix4();
        Json item = {{"entity", McpRefs::entityJson(scene, entity)}, {"start", vec3Json(model * link.startPoint)},
                     {"end", vec3Json(model * link.endPoint)}, {"bidirectional", link.bidirectional}, {"area", link.area},
                     {"width", link.width}, {"enabled", link.enabled}, {"activated", link.activated}};
        auto state = linkStates.find(entity);
        const bool held = state != linkStates.end() && state->second.surface != NULL_ENTITY;
        item["surface"] = held ? McpRefs::entityJson(scene, state->second.surface) : Json();
        item["connected"] = held && state->second.connected;
        item["connections"] = held ? state->second.connections : 0;
        item["island_start"] = held ? state->second.islandStart : -1;
        item["island_end"] = held ? state->second.islandEnd : -1;
        if (playing) item["occupied"] = system->isLinkOccupied(entity);
        links.push_back(item);
    }
    // S55 R43: runtime modifier volumes and script volumes, applied or not (with `entities`, only
    // those GameObjects' volumes)
    Json areaOverrides = Json::array();
    int appliedOverrides = 0;
    for (const NavigationSystem::AreaOverrideStatus& status : system->getAreaOverrides()) {
        if (status.handle != 0 ? filtered : !listed(status.modifier)) continue;
        Json source = status.handle != 0 ? Json{{"handle", status.handle}} : Json{{"entity", McpRefs::entityJson(scene, status.modifier)}};
        areaOverrides.push_back({{"source", source}, {"area", status.area}, {"applied", status.applied}, {"polygons", status.polygons}});
        if (status.applied) appliedOverrides++;
    }
    Json agents = Json::array();
    for (Entity entity : entitiesWith<NavMeshAgentComponent>(s)) {
        if (!listed(entity)) continue;
        const NavMeshAgentComponent& agent = s->getComponent<NavMeshAgentComponent>(entity);
        Json item = {{"entity", McpRefs::entityJson(scene, entity)}, {"agent_type", agent.agentType}, {"speed", agent.speed}};
        if (playing) {
            item["destination"] = vec3Json(agent.destination);
            item["path_status"] = nav::pathStatusName(agent.pathStatus);
            item["path_pending"] = agent.pathPending;
            item["remaining_distance"] = rounded(agent.remainingDistance);
            item["velocity"] = vec3Json(agent.velocity);
            item["is_on_navmesh"] = agent.isOnNavMesh;
            item["is_on_off_mesh_link"] = agent.isOnOffMeshLink;
            // ---- S55 agents (R43): why its last path failed, and the watchdog's count
            item["last_failure"] = nav::pathFailureName(agent.lastPathFailure);
            if (agent.stuckDetection) item["stuck_seconds"] = rounded(agent.stuckTime);
        }
        agents.push_back(item);
    }
    Json obstacles = Json::array();
    for (Entity entity : entitiesWith<NavMeshObstacleComponent>(s)) {
        if (!listed(entity)) continue;
        const NavMeshObstacleComponent& obstacle = s->getComponent<NavMeshObstacleComponent>(entity);
        Json item = {{"entity", McpRefs::entityJson(scene, entity)}, {"enabled", obstacle.enabled},
                     {"shape", obstacle.shape == NavObstacleShape::BOX ? "box" : "capsule"}, {"center", vec3Json(obstacle.center)},
                     {"carve", obstacle.carve}};
        if (obstacle.shape == NavObstacleShape::BOX) item["size"] = vec3Json(obstacle.size);
        else item.update({{"radius", rounded(obstacle.radius)}, {"height", rounded(obstacle.height)}});
        obstacles.push_back(item);
    }
    Json modifiers = Json::array();
    for (Entity entity : entitiesWith<NavMeshModifierComponent>(s)) {
        if (!listed(entity)) continue;
        const NavMeshModifierComponent& modifier = s->getComponent<NavMeshModifierComponent>(entity);
        const char* inclusion = modifier.overrideInclusion == NavModifierInclusion::INCLUDE ? "include"
                              : modifier.overrideInclusion == NavModifierInclusion::EXCLUDE ? "exclude" : "auto";
        Json item = {{"entity", McpRefs::entityJson(scene, entity)}, {"enabled", modifier.enabled}, {"inclusion", inclusion},
                     {"apply_to_children", modifier.applyToChildren}};
        if (modifier.overrideArea) item.update({{"area", modifier.area}, {"area_name", areaName(modifier.area)}});
        if (modifier.useVolume) item["volume"] = {{"center", vec3Json(modifier.volumeCenter)}, {"size", vec3Json(modifier.volumeSize)}};
        if (modifier.useVolume) item["apply_at_runtime"] = modifier.applyAtRuntime;
        modifiers.push_back(item);
    }
    // S54 R13: the one findings function, with the dirty counts read above
    nav::NavFindingOptions options;
    options.dirtyTilesOf = [&dirtyOf](Entity surface) {
        auto found = dirtyOf.find(surface);
        return found != dirtyOf.end() ? found->second : -1;
    };
    Json findings = Json::array();
    std::string findingsText;
    for (const nav::NavFinding& finding : system->getFindings(options)) {
        if (filtered && !only.count(finding.entity)) continue;
        if (findings.size() < 10) {
            findingsText += "\n  " + std::string(nav::findingSeverityName(finding.severity)) + " " + finding.code + ": " + finding.message;
        }
        findings.push_back(findingJson(scene, finding));
    }
    Json data = {{"scene_id", scene->id}, {"playing", playing}, {"navmesh_available", nav::isNavMeshAvailable()},
                 {"settings", settingsJson(project->getNavigationSettings())}, {"surfaces", surfaces}, {"grids", grids},
                 {"links", links}, {"area_overrides", areaOverrides}, {"agents", agents}, {"obstacles", obstacles}, {"modifiers", modifiers},
                 {"findings", findings},
                 {"stats", {{"crowds", system->getCrowdCount()}, {"crowd_agents", system->getAgentCount()}, {"bytes", system->getMemoryBytes()}}}};
    text += "\n  " + std::to_string(links.size()) + " link(s), " + std::to_string(agents.size()) + " agent(s), " +
            std::to_string(obstacles.size()) + " obstacle(s), " + std::to_string(modifiers.size()) + " modifier(s)";
    if (!areaOverrides.empty())
        text += ", " + std::to_string(areaOverrides.size()) + " area override(s) (" + std::to_string(appliedOverrides) + " applied)";
    text += "\n" + std::to_string(findings.size()) + " finding(s)" + (findings.empty() ? "." : ":") + findingsText;
    if (findings.size() > 10) text += "\n  ... (" + std::to_string(findings.size() - 10) + " more in findings)";
    call->ok(text, data);
}

// ------------------------------------------------------------ query_navigation

// A point argument (S54 R30): [x, y, z] in world space, or a GameObject: its world position, a
// NavMesh Agent's feet (its base offset below)
struct NavPoint {
    Vector3 position;
    Entity entity = NULL_ENTITY;
    const NavMeshAgentComponent* agent = nullptr;
};

bool parseNavPoint(SceneProject* scene, const Json& value, const std::string& key, NavPoint& out, std::string& error) {
    const bool reference = value.is_number_integer() || value.is_string() ||
                           (value.is_object() && (value.contains("id") || value.contains("name") || value.contains("path")));
    if (!reference) {
        if ((value.is_array() || value.is_object()) && McpValues::parseVector3(value, out.position)) return true;
        error = key + ": [x, y, z] (world) or a GameObject.";
        return false;
    }
    Scene* s = scene->scene;
    out.entity = McpRefs::resolveEntity(scene, value, error);
    if (out.entity == NULL_ENTITY) {
        error = key + ": " + error;
        return false;
    }
    const Transform* transform = s->findComponent<Transform>(out.entity);
    if (!transform) {
        error = key + ": " + McpRefs::entityPath(scene, out.entity) + " has no position (no Transform).";
        return false;
    }
    out.position = transform->worldPosition;
    out.agent = s->findComponent<NavMeshAgentComponent>(out.entity);
    if (out.agent) {
        // the up and units of a surface of its type (a 2D surface's up is +Z, in points)
        Vector3 up(0.0f, 1.0f, 0.0f);
        float scale = 1.0f;
        for (Entity surface : entitiesWith<NavMeshSurfaceComponent>(s)) {
            const NavMeshSurfaceComponent& component = s->getComponent<NavMeshSurfaceComponent>(surface);
            if (component.agentType != out.agent->agentType) continue;
            up = nav::NavBakeCollector::frameOf(s, surface).up();
            scale = nav::NavBakeCollector::unitScaleOf(s, component);
            break;
        }
        out.position = out.position - up * (out.agent->baseOffset * scale);
    }
    return true;
}

bool parseNavPointArg(SceneProject* scene, const Json& args, const char* key, NavPoint& out, std::string& error) {
    if (!args.contains(key) || args[key].is_null()) {
        error = std::string(key) + ": [x, y, z] (world) or a GameObject.";
        return false;
    }
    return parseNavPoint(scene, args[key], key, out, error);
}

// How a result echoes a point: [x, y, z], or the GameObject
Json navPointJson(SceneProject* scene, const NavPoint& point) {
    return point.entity != NULL_ENTITY ? McpRefs::entityJson(scene, point.entity) : vec3Json(point.position);
}

// `to` as a list of up to 64 targets (an array that is not one [x, y, z])
bool isTargetList(const Json& value) {
    if (!value.is_array()) return false;
    if (value.size() == 3 && std::all_of(value.begin(), value.end(), [](const Json& v) { return v.is_number(); })) return false;
    return true;
}

constexpr size_t MAX_TARGETS = 64;

bool agentTypeOf(const Json& args, int& out, std::string& error) {
    out = nav::getSettings().agentTypes.empty() ? 0 : nav::getSettings().agentTypes.front().id;
    if (!args.contains("agent_type") || args["agent_type"].is_null()) return true;
    const Json& value = args["agent_type"];
    if (value.is_number_integer()) {
        out = value.get<int>();
        if (!nav::getSettings().findAgentType(out)) {
            error = "agent_type " + std::to_string(out) + " is not an agent type id (get_navigation_info lists them).";
            return false;
        }
        return true;
    }
    if (value.is_string()) {
        const nav::AgentTypeSettings* type = nav::getSettings().findAgentType(value.get<std::string>());
        if (!type) {
            error = "agent_type \"" + value.get<std::string>() + "\" is not an agent type (get_navigation_info lists them).";
            return false;
        }
        out = type->id;
        return true;
    }
    error = "agent_type: a name or an id.";
    return false;
}

std::string agentTypeLabel(int id) {
    const nav::AgentTypeSettings* type = nav::getSettings().findAgentType(id);
    return (type ? "'" + type->name + "'" : std::string("an unknown type")) + " (" + std::to_string(id) + ")";
}

// The agent type and area mask of a navmesh query (S54 R30): agent_type, else from's NavMesh
// Agent's, else the target surface's, else the first type; area_mask, else from's agent's
bool queryFilterOf(SceneProject* scene, const Json& args, const NavPoint& from, Entity targetSurface, nav::QueryFilter& filter,
                   std::string& error) {
    const bool explicitType = args.contains("agent_type") && !args["agent_type"].is_null();
    if (!agentTypeOf(args, filter.agentType, error)) return false;
    if (!explicitType && from.agent) filter.agentType = from.agent->agentType;
    if (targetSurface != NULL_ENTITY) {
        const int surfaceType = scene->scene->getComponent<NavMeshSurfaceComponent>(targetSurface).agentType;
        if (!explicitType && !from.agent) {
            filter.agentType = surfaceType;
        } else if (surfaceType != filter.agentType) {
            error = "target " + McpRefs::entityPath(scene, targetSurface) + " is a NavMesh Surface of agent type " + agentTypeLabel(surfaceType) +
                    ", but " + (explicitType ? std::string("agent_type is ") : "from's NavMesh Agent is of type ") +
                    agentTypeLabel(filter.agentType) + ": drop target (every surface of that type answers) or " +
                    (explicitType ? "agent_type." : "set agent_type to the surface's.");
            return false;
        }
    }
    if (args.contains("area_mask")) filter.areaMask = (uint16_t)(args["area_mask"].get<int>() & nav::ALL_AREAS);
    else if (from.agent) filter.areaMask = (uint16_t)(from.agent->areaMask & nav::ALL_AREAS);
    return true;
}

// Diagnostics label islands on demand (S54 R8): the snapped ends of the queries then name theirs
void labelIslandsOf(Scene* s, NavigationSystem* system, int agentType, Entity target) {
    if (target != NULL_ENTITY) {
        system->getIslands(target);
        return;
    }
    for (Entity surface : entitiesWith<NavMeshSurfaceComponent>(s)) {
        if (s->getComponent<NavMeshSurfaceComponent>(surface).agentType == agentType) system->getIslands(surface);
    }
}

Json hitJson(const nav::NavHit& hit) {
    return {{"hit", hit.hit}, {"position", vec3Json(hit.position)}, {"normal", vec3Json(hit.normal)}, {"distance", rounded(hit.distance)},
            {"area", hit.area}, {"area_name", areaName(hit.area)}};
}

// What to try next for a path that is not Complete: the tool route of each reason
const char* failureNext(nav::PathFailure failure) {
    switch (failure) {
        case nav::PathFailure::None: return "";
        case nav::PathFailure::NoSurface:
            return "Add a NavMesh Surface of this agent type (add_component navmeshsurface on the ground), then bake_navigation.";
        case nav::PathFailure::NotBaked: return "bake_navigation, then query again.";
        case nav::PathFailure::StartOffMesh: return "query_navigation op locate at from says why it is off the navmesh.";
        case nav::PathFailure::EndOffMesh: return "query_navigation op locate at to says why it is off the navmesh.";
        case nav::PathFailure::StartFiltered:
        case nav::PathFailure::EndFiltered:
            return "Add that end's area to area_mask (op locate names the area), or move the end.";
        case nav::PathFailure::Disconnected:
            return "The ends are on different islands (get_navigation_info lists them, visualize_navigation show.islands draws "
                   "them): join them with walkable geometry or a NavMesh Link, then bake_navigation.";
        case nav::PathFailure::BlockedByArea: return "Only areas area_mask leaves out connect the ends: add them to area_mask.";
        case nav::PathFailure::OutOfNodes: return "The path is too long for one search: query a nearer target, or split the trip.";
    }
    return "";
}

// reason, surface, closest_point, gap and next of an answer that is not Complete (S54 R30)
void addFailureJson(SceneProject* scene, Json& data, nav::PathFailure failure, Entity surface, const Vector3& closest, float gap) {
    data["reason"] = nav::pathFailureName(failure);
    data["surface"] = surface != NULL_ENTITY ? McpRefs::entityJson(scene, surface) : Json();
    data["closest_point"] = vec3Json(closest);
    data["gap"] = rounded(gap);
    data["next"] = failureNext(failure);
}

std::string failureText(nav::PathFailure failure, float gap) {
    if (failure == nav::PathFailure::None) return "";
    return std::string(" [") + nav::pathFailureName(failure) + "] " + nav::pathFailureDescription(failure) +
           " Gap " + numberText(rounded(gap)) + ". Next: " + failureNext(failure);
}

Json pathEndJson(const Vector3& position, float snapDistance, int island) {
    return {{"position", vec3Json(position)}, {"snap_distance", rounded(snapDistance)}, {"island", island}};
}

// A navmesh path; full: its corners and snapped ends (one target), else a summary (a list)
Json navPathJson(SceneProject* scene, const nav::NavPath& path, bool full) {
    Json data = {{"status", nav::pathStatusName(path.status)}, {"length", rounded(path.length)}};
    if (full) {
        Json corners = Json::array();
        for (const Vector3& p : path.corners) corners.push_back(vec3Json(p));
        data["corners"] = corners;
        data["start"] = pathEndJson(path.start, path.startSnapDistance, path.startIsland);
        data["end"] = pathEndJson(path.end, path.endSnapDistance, path.endIsland);
        if (path.surface != NULL_ENTITY) data["surface"] = McpRefs::entityJson(scene, path.surface);
    } else {
        data["corner_count"] = path.corners.size();
    }
    if (path.status != nav::PathStatus::Complete) {
        addFailureJson(scene, data, path.failure, path.surface, path.closestPoint, path.gap);
        if (!full) data.erase("next");
    }
    return data;
}

// ---- S55 queries: a path's cost, per-corner areas and flags, and its length per area name
void addPathCosts(Json& data, const nav::NavPath& path) {
    data["cost"] = rounded(path.cost);
    Json areas = Json::array(), flags = Json::array();
    for (uint8_t area : path.cornerAreas) areas.push_back(area);
    for (uint8_t flag : path.cornerFlags) flags.push_back(flag);
    data["corner_areas"] = areas;
    data["corner_flags"] = flags;
    Json lengths = Json::object();
    for (int area = 0; area < nav::AREA_COUNT; area++) {
        if (path.areaLengths[(size_t)area] > 0.0f)
            lengths[nav::getSettings().areas[(size_t)area].name] = rounded(path.areaLengths[(size_t)area]);
    }
    data["area_lengths"] = lengths;
}

// A grid answer's reason (S54 R30): outside or a Not Walkable cell is *_off_mesh, a cell the
// mask leaves out *_filtered, no way through disconnected
nav::PathFailure gridEndFailure(const nav::NavGridMap& map, const nav::GridCell& cell, uint16_t areaMask, bool start) {
    if (!map.contains(cell) || !map.isWalkable(map.index(cell))) return start ? nav::PathFailure::StartOffMesh : nav::PathFailure::EndOffMesh;
    if (!(areaMask & (1u << map.getArea(cell)))) return start ? nav::PathFailure::StartFiltered : nav::PathFailure::EndFiltered;
    return nav::PathFailure::None;
}

void queryGrid(const CallPtr& call, SceneProject* scene, NavigationSystem* system, Entity grid, const std::string& op) {
    const Json& args = call->args();
    std::string error;
    if (op != "path" && op != "sample" && op != "reachable") {
        call->fail("op " + op + " is for navmeshes; a grid takes path, sample or reachable.");
        return;
    }
    NavPoint from;
    if (!parseNavPointArg(scene, args, "from", from, error)) {
        call->fail(error);
        return;
    }
    nav::NavGridMap* map = system->getGridMap(grid);
    if (!map) {
        call->fail("The grid has no cells.");
        return;
    }
    const nav::GridCell a = system->gridWorldToCell(grid, from.position);
    auto cellJson = [](const nav::GridCell& c) { return Json::array({c.x, c.y}); };
    Json data = {{"target", "grid"}, {"grid", McpRefs::entityJson(scene, grid)}, {"op", op}, {"from_cell", cellJson(a)}};
    if (op == "sample") {
        data["inside"] = map->contains(a);
        if (map->contains(a)) {
            data["area"] = map->getArea(a);
            data["area_name"] = nav::getSettings().areas[map->getArea(a)].name;
            data["position"] = vec3Json(system->gridCellToWorld(grid, a));
        }
        call->ok("Cell (" + std::to_string(a.x) + ", " + std::to_string(a.y) + ")" + (map->contains(a) ? "" : " is outside the grid"), data);
        return;
    }
    nav::GridQuery query;
    query.allowPartial = args.value("allow_partial", true);
    if (args.contains("area_mask")) query.areaMask = (uint16_t)(args["area_mask"].get<int>() & nav::ALL_AREAS);
    query.movement = scene->scene->getComponent<NavGridComponent>(grid).movement;
    // one target: the whole answer; a list: a summary per target
    auto answer = [&](const NavPoint& to, bool full) {
        const nav::GridCell b = system->gridWorldToCell(grid, to.position);
        Json item = {{"to_cell", cellJson(b)}};
        if (!full) item["to"] = navPointJson(scene, to);
        nav::PathFailure failure = nav::PathFailure::None;
        Vector3 closest = system->gridCellToWorld(grid, a);
        if (op == "reachable") {
            failure = gridEndFailure(*map, a, query.areaMask, true);
            if (failure == nav::PathFailure::None) failure = gridEndFailure(*map, b, query.areaMask, false);
            if (failure == nav::PathFailure::None && !map->isReachable(a, b)) failure = nav::PathFailure::Disconnected;
            item["reachable"] = failure == nav::PathFailure::None;
            if (failure != nav::PathFailure::None) item["reason"] = nav::pathFailureName(failure);
            return item;
        }
        const nav::GridPath path = system->findGridPath(grid, a, b, query);
        const std::vector<Vector3> world = system->gridPathToWorld(grid, path);
        item["status"] = nav::pathStatusName(path.status);
        item["cost"] = rounded(path.cost);
        if (full) {
            Json cells = Json::array();
            for (const nav::GridCell& c : path.cells) cells.push_back(cellJson(c));
            Json corners = Json::array();
            for (const Vector3& p : world) corners.push_back(vec3Json(p));
            item["cells"] = cells;
            item["corners"] = corners;
            item["expanded"] = path.expanded;
        } else {
            item["cell_count"] = path.cells.size();
        }
        if (path.status != nav::PathStatus::Complete) {
            if (!world.empty()) closest = world.back();
            const Vector3 end = map->contains(b) ? system->gridCellToWorld(grid, b) : to.position;
            item["reason"] = nav::pathFailureName(path.failure);
            item["closest_point"] = vec3Json(closest);
            item["gap"] = rounded((end - closest).length());
            if (full) item["next"] = failureNext(path.failure);
        }
        return item;
    };
    if (args.contains("to") && isTargetList(args["to"])) {
        const Json& list = args["to"];
        if (list.empty() || list.size() > MAX_TARGETS) {
            call->fail("to: 1-64 targets (got " + std::to_string(list.size()) + "); split the list.");
            return;
        }
        Json results = Json::array();
        int done = 0;
        for (size_t i = 0; i < list.size(); i++) {
            NavPoint to;
            if (!parseNavPoint(scene, list[i], "to[" + std::to_string(i) + "]", to, error)) {
                call->fail(error);
                return;
            }
            Json item = answer(to, false);
            done += (item.value("reachable", false) || item.value("status", std::string()) == "complete") ? 1 : 0;
            results.push_back(item);
        }
        data["results"] = results;
        call->ok("Grid " + op + " to " + std::to_string(list.size()) + " target(s): " + std::to_string(done) +
                 (op == "reachable" ? " reachable." : " complete."), data);
        return;
    }
    NavPoint to;
    if (!parseNavPointArg(scene, args, "to", to, error)) {
        call->fail(error);
        return;
    }
    data.update(answer(to, true));
    if (op == "reachable") {
        call->ok(data["reachable"].get<bool>() ? "Reachable." : "Not reachable [" + data["reason"].get<std::string>() + "].", data);
        return;
    }
    std::string text = "Grid path " + data["status"].get<std::string>() + ": " + std::to_string(data["cells"].size()) + " cells, cost " +
                       numberText(data["cost"].get<double>());
    if (data.contains("reason")) {
        nav::PathFailure failure = nav::PathFailure::None;
        nav::pathFailureFromName(data["reason"].get<std::string>(), failure);
        text += failureText(failure, data["gap"].get<float>());
    }
    call->ok(text, data);
}

// op locate (S54 R10, R30): where a point is on the navmesh and, off it, why (the walkability probe)
void locateTool(const CallPtr& call, SceneProject* scene, NavigationSystem* system, const NavPoint& from, const nav::QueryFilter& filter,
                Entity target, Json data) {
    const Json& args = call->args();
    const float maxDistance = args.value("max_distance", 5.0f);
    labelIslandsOf(scene->scene, system, filter.agentType, target);
    const nav::NavLocation location = target != NULL_ENTITY ? system->locateOn(target, from.position, filter.areaMask, maxDistance)
                                                            : system->locate(from.position, filter, maxDistance);
    data.update({{"on_mesh", location.onMesh}, {"found", location.found}, {"snap_distance", rounded(location.snapDistance)},
                 {"island", location.island}});
    if (location.found) {
        data.update({{"position", vec3Json(location.position)}, {"area", location.area}, {"area_name", areaName(location.area)}});
    }
    data["surface"] = location.surface != NULL_ENTITY ? McpRefs::entityJson(scene, location.surface) : Json();
    std::string text;
    if (location.onMesh) {
        text = "On the navmesh at " + vec3Json(location.position).dump() + " (island " + std::to_string(location.island) + ", " +
               areaName(location.area) + ").";
        call->ok(text, data);
        return;
    }
    text = location.found ? "Not on the navmesh; its nearest point " + vec3Json(location.position).dump() + " is " +
                                numberText(rounded(location.snapDistance)) + " away."
                          : "Not on the navmesh; none within " + numberText(maxDistance) + ".";
    // the probe explains one surface: the one found, else the target, else the first of the type
    Entity surface = location.surface != NULL_ENTITY ? location.surface : target;
    if (surface == NULL_ENTITY) {
        for (Entity candidate : entitiesWith<NavMeshSurfaceComponent>(scene->scene)) {
            if (scene->scene->getComponent<NavMeshSurfaceComponent>(candidate).agentType == filter.agentType) {
                surface = candidate;
                break;
            }
        }
    }
    Json why = Json::array();
    if (surface != NULL_ENTITY) {
        const NavProbeResult probe = NavWalkabilityProbe::explain(scene->scene, *system, surface, from.position, filter);
        for (const NavProbeReason& reason : probe.reasons) {
            Json item = {{"code", reason.code}, {"message", reason.message}};
            if (reason.entity != NULL_ENTITY) item["entity"] = McpRefs::entityJson(scene, reason.entity);
            if (reason.value != 0.0f || reason.limit != 0.0f) item.update({{"value", rounded(reason.value)}, {"limit", rounded(reason.limit)}});
            if (!reason.detail.empty()) item["detail"] = reason.detail;
            why.push_back(item);
            text += "\n  " + reason.code + ": " + reason.message;
        }
    } else {
        text += "\n  no_surface: " + std::string(nav::pathFailureDescription(nav::PathFailure::NoSurface));
    }
    data["why"] = why;
    call->ok(text, data);
}

void queryNavigationTool(const CallPtr& call) {
    const Json& args = call->args();
    std::string error;
    SceneProject* scene = sceneOf(call, true, error);
    if (!scene) {
        call->fail(error);
        return;
    }
    NavigationSystem* system = NavigationEditor::systemOf(*scene);
    const std::string op = args.value("op", std::string("path"));
    // the target: a navmesh (agent type), the first grid, or a given surface / grid
    Entity targetEntity = NULL_ENTITY;
    bool grid = false;
    if (args.contains("target") && !args["target"].is_null()) {
        const Json& target = args["target"];
        if (target.is_string() && target.get<std::string>() == "grid") {
            const std::vector<Entity> grids = entitiesWith<NavGridComponent>(scene->scene);
            if (grids.empty()) {
                call->fail("The scene has no Nav Grid.");
                return;
            }
            targetEntity = grids.front();
            grid = true;
        } else if (!(target.is_string() && target.get<std::string>() == "navmesh")) {
            targetEntity = McpRefs::resolveEntity(scene, target, error);
            if (targetEntity == NULL_ENTITY) {
                call->fail(error);
                return;
            }
            grid = scene->scene->findComponent<NavGridComponent>(targetEntity) != nullptr;
            if (!grid && !scene->scene->findComponent<NavMeshSurfaceComponent>(targetEntity)) {
                call->fail("target " + McpRefs::entityPath(scene, targetEntity) + " has no NavMesh Surface or Nav Grid.");
                return;
            }
        }
    }
    if (grid) {
        queryGrid(call, scene, system, targetEntity, op);
        return;
    }
    if (!nav::isNavMeshAvailable()) {
        call->fail("This editor was built without Recast/Detour (JASPERENGINE_NAVIGATION=OFF); query a Nav Grid instead (target: \"grid\").");
        return;
    }
    if (op != "path" && op != "sample" && op != "raycast" && op != "closest_edge" && op != "reachable" && op != "locate" &&
        op != "random" && op != "move_along") {
        call->fail("op: path, sample, raycast, closest_edge, reachable, locate, random or move_along.");
        return;
    }
    // every op needs from but random (without it: anywhere)
    NavPoint from;
    const bool hasFrom = args.contains("from") && !args["from"].is_null();
    if ((op != "random" || hasFrom) && !parseNavPointArg(scene, args, "from", from, error)) {
        call->fail(error);
        return;
    }
    nav::QueryFilter filter;
    if (!queryFilterOf(scene, args, from, targetEntity, filter, error)) {
        call->fail(error);
        return;
    }
    Json data = {{"target", "navmesh"}, {"op", op}, {"agent_type", filter.agentType}, {"area_mask", filter.areaMask}};
    if (targetEntity != NULL_ENTITY) data["surface"] = McpRefs::entityJson(scene, targetEntity);
    nav::NavHit hit;
    if (op == "locate") {
        locateTool(call, scene, system, from, filter, targetEntity, data);
        return;
    }
    if (op == "random") {
        // S55 R42: anywhere without from; around from within max_distance; else on from's island.
        // Its own stream (the seed, else the default): the game's stream is never drawn from.
        nav::RandomPointQuery query;
        query.seeded = true;
        query.seed = args.contains("seed") ? args["seed"].get<uint64_t>() : NavigationSystem::DEFAULT_RANDOM_SEED;
        if (hasFrom) {
            query.around = args.contains("max_distance");
            query.center = from.position;
            query.radius = args.value("max_distance", 0.0f);
            query.sameIsland = !query.around;
            query.islandOf = from.position;
        }
        const bool found = targetEntity != NULL_ENTITY ? system->randomPointOn(targetEntity, hit, filter, query)
                                                       : system->randomPoint(hit, filter, query);
        data["found"] = found;
        data["seed"] = query.seed;
        if (found) {
            data["position"] = vec3Json(hit.position);
            data["area"] = hit.area;
        }
        call->ok(found ? "Random point " + vec3Json(hit.position).dump() : std::string("No navmesh point found."), data);
        return;
    }
    if (op == "sample") {
        const float distance = args.value("max_distance", 2.0f);
        const bool found = targetEntity != NULL_ENTITY ? system->samplePositionOn(targetEntity, from.position, distance, hit, filter.areaMask)
                                                       : system->samplePosition(from.position, distance, hit, filter);
        data.update(hitJson(hit));
        data["found"] = found;
        call->ok(found ? "Nearest navmesh point " + vec3Json(hit.position).dump() + ", " + numberText(rounded(hit.distance)) + " away"
                       : "No navmesh within " + numberText(distance) + ".", data);
        return;
    }
    if (op == "closest_edge") {
        const float maxDistance = args.value("max_distance", -1.0f);
        const bool found = targetEntity != NULL_ENTITY ? system->findClosestEdgeOn(targetEntity, from.position, hit, filter, maxDistance)
                                                       : system->findClosestEdge(from.position, hit, filter, maxDistance);
        data.update(hitJson(hit));
        data["found"] = found;
        call->ok(found ? "Closest edge " + vec3Json(hit.position).dump() + ", " + numberText(rounded(hit.distance)) + " away"
                 : hit.distance > 0.0f ? "No edge within " + numberText(rounded(hit.distance)) + "."
                                       : std::string("No navmesh at from."), data);
        return;
    }
    if (op == "move_along") {
        NavPoint to;
        if (!parseNavPointArg(scene, args, "to", to, error)) {
            call->fail(error);
            return;
        }
        const bool found = targetEntity != NULL_ENTITY ? system->moveAlongSurfaceOn(targetEntity, from.position, to.position, hit, filter)
                                                       : system->moveAlongSurface(from.position, to.position, hit, filter);
        data["found"] = found;
        data["position"] = vec3Json(found ? hit.position : from.position);
        data["blocked"] = hit.hit;
        data["distance"] = rounded(hit.distance);
        data["area"] = hit.area;
        call->ok(!found ? std::string("No navmesh at from.")
                 : hit.hit ? "Stopped at " + vec3Json(hit.position).dump() + " after " + numberText(rounded(hit.distance))
                           : "Reached " + vec3Json(hit.position).dump(), data);
        return;
    }
    labelIslandsOf(scene->scene, system, filter.agentType, targetEntity);
    // one target's answer: whole (full) or a summary (a list of targets)
    auto answer = [&](const NavPoint& to, bool full) {
        Json item = full ? Json::object() : Json{{"to", navPointJson(scene, to)}};
        if (op == "raycast") {
            const bool blocked = targetEntity != NULL_ENTITY ? system->raycastOn(targetEntity, from.position, to.position, hit, filter)
                                                             : system->raycast(from.position, to.position, hit, filter);
            if (full) item.update(hitJson(hit));
            else item.update({{"position", vec3Json(hit.position)}, {"distance", rounded(hit.distance)}});
            item["blocked"] = blocked;
            item["cost"] = rounded(hit.cost);
            return item;
        }
        const nav::NavPath path = targetEntity != NULL_ENTITY ? system->calculatePathOn(targetEntity, from.position, to.position, filter)
                                                              : system->calculatePath(from.position, to.position, filter);
        if (op == "path") {
            item.update(navPathJson(scene, path, full));
            if (full) addPathCosts(item, path);
            else item["cost"] = rounded(path.cost);
            return item;
        }
        // reachable: the O(1) island answer of isReachable (S54 R6); the path says where it stops
        nav::PathFailure why = nav::PathFailure::None;
        const bool reachable = targetEntity != NULL_ENTITY ? path.status == nav::PathStatus::Complete
                                                           : system->isReachable(from.position, to.position, filter, &why);
        if (targetEntity != NULL_ENTITY) why = path.failure;
        item["reachable"] = reachable;
        if (!reachable) {
            addFailureJson(scene, item, why, path.surface, path.closestPoint, path.gap);
            if (!full) item.erase("next");
        }
        return item;
    };
    if (args.contains("to") && isTargetList(args["to"])) {
        const Json& list = args["to"];
        if (list.empty() || list.size() > MAX_TARGETS) {
            call->fail("to: 1-64 targets (got " + std::to_string(list.size()) + "); split the list.");
            return;
        }
        Json results = Json::array();
        std::map<std::string, int> counts;
        for (size_t i = 0; i < list.size(); i++) {
            NavPoint to;
            if (!parseNavPoint(scene, list[i], "to[" + std::to_string(i) + "]", to, error)) {
                call->fail(error);
                return;
            }
            Json item = answer(to, false);
            const std::string key = op == "raycast" ? (item["blocked"].get<bool>() ? "blocked" : "clear")
                                  : op == "reachable" ? (item["reachable"].get<bool>() ? "reachable" : "not reachable")
                                                      : item["status"].get<std::string>();
            counts[key]++;
            results.push_back(item);
        }
        data["results"] = results;
        std::string summary;
        for (const auto& count : counts) summary += (summary.empty() ? "" : ", ") + std::to_string(count.second) + " " + count.first;
        call->ok("query_navigation " + op + " to " + std::to_string(list.size()) + " target(s): " + summary + ".", data);
        return;
    }
    NavPoint to;
    if (!parseNavPointArg(scene, args, "to", to, error)) {
        call->fail(error);
        return;
    }
    data.update(answer(to, true));
    if (op == "raycast") {
        call->ok(data["blocked"].get<bool>() ? "Blocked at " + vec3Json(hit.position).dump() : "Clear line.", data);
        return;
    }
    nav::PathFailure failure = nav::PathFailure::None;
    if (data.contains("reason")) nav::pathFailureFromName(data["reason"].get<std::string>(), failure);
    const float gap = data.value("gap", 0.0f);
    if (op == "reachable") {
        call->ok((data["reachable"].get<bool>() ? std::string("Reachable.") : "Not reachable." + failureText(failure, gap)), data);
        return;
    }
    call->ok("Path " + data["status"].get<std::string>() + ": " + std::to_string(data["corners"].size()) + " corners, " +
             numberText(data["length"].get<double>()) + " long." + failureText(failure, gap), data);
}

// ------------------------------------------------------------ set_navigation_settings

bool readNumber(const Json& item, const char* key, float minimum, float maximum, float& out, std::string& error) {
    if (!item.contains(key)) return true;
    if (!item[key].is_number()) {
        error = std::string(key) + " must be a number";
        return false;
    }
    const float value = item[key].get<float>();
    if (!(value >= minimum && value <= maximum)) {
        error = std::string(key) + " must be " + std::to_string(minimum) + "-" + std::to_string(maximum);
        return false;
    }
    out = value;
    return true;
}

void setNavigationSettingsTool(const CallPtr& call) {
    const Json& args = call->args();
    Project* project = call->ctx().project();
    NavigationProjectSettings next = project->getNavigationSettings();
    nav::NavigationSettings& settings = next.settings;
    std::string error;
    if (args.contains("agents")) {
        const Json& list = args["agents"];
        std::vector<nav::AgentTypeSettings> agents;
        std::set<int> ids;
        std::set<std::string> names;
        for (size_t i = 0; i < list.size(); i++) {
            const Json& item = list[i];
            nav::AgentTypeSettings agent;
            if (item.contains("id")) agent.id = item["id"].get<int>();
            else {
                // a name that exists keeps its id; a new one takes the smallest free id
                const nav::AgentTypeSettings* known = item.contains("name") ? project->getNavigationSettings().settings.findAgentType(item["name"].get<std::string>()) : nullptr;
                if (known && !ids.count(known->id)) agent.id = known->id;
                else {
                    int id = 0;
                    while (ids.count(id) || std::any_of(list.begin(), list.end(), [&](const Json& j) { return j.contains("id") && j["id"] == id; })) id++;
                    agent.id = id;
                }
            }
            if (ids.count(agent.id)) {
                call->fail("agents[" + std::to_string(i) + "]: id " + std::to_string(agent.id) + " twice.");
                return;
            }
            if (const nav::AgentTypeSettings* old = project->getNavigationSettings().settings.findAgentType(agent.id)) agent = *old;
            if (item.contains("name")) agent.name = item["name"].get<std::string>();
            if (agent.name.empty() || names.count(agent.name)) {
                call->fail("agents[" + std::to_string(i) + "]: a unique, non-empty name.");
                return;
            }
            if (!readNumber(item, "radius", 0.01f, 100.0f, agent.radius, error) || !readNumber(item, "height", 0.01f, 100.0f, agent.height, error) ||
                !readNumber(item, "step_height", 0.0f, 100.0f, agent.stepHeight, error) ||
                !readNumber(item, "max_slope", 0.0f, 89.0f, agent.maxSlope, error)) {
                call->fail("agents[" + std::to_string(i) + "]: " + error + ".");
                return;
            }
            ids.insert(agent.id);
            names.insert(agent.name);
            agents.push_back(agent);
        }
        if (agents.empty()) {
            call->fail("agents: at least one agent type.");
            return;
        }
        settings.agentTypes = agents;
    }
    if (args.contains("areas")) {
        for (size_t i = 0; i < args["areas"].size(); i++) {
            const Json& item = args["areas"][i];
            const int index = item.value("index", -1);
            if (index < 0 || index >= nav::AREA_COUNT) {
                call->fail("areas[" + std::to_string(i) + "]: index 0-15.");
                return;
            }
            nav::AreaSettings& area = settings.areas[(size_t)index];
            if (item.contains("name")) {
                if (index <= nav::AREA_JUMP && item["name"].get<std::string>() != area.name) {
                    call->fail("areas[" + std::to_string(i) + "]: areas 0-2 (Walkable, Not Walkable, Jump) keep their names.");
                    return;
                }
                area.name = item["name"].get<std::string>();
            }
            if (!readNumber(item, "cost", 1.0f, 1e6f, area.cost, error)) {
                call->fail("areas[" + std::to_string(i) + "]: " + error + ".");
                return;
            }
            if (item.contains("color")) {
                Vector4 color;
                Vector3 rgb;
                if (McpValues::parseVector4(item["color"], color)) area.color = color;
                else if (McpValues::parseVector3(item["color"], rgb)) area.color = Vector4(rgb.x, rgb.y, rgb.z, 1.0f);
                else {
                    call->fail("areas[" + std::to_string(i) + "]: color [r, g, b] or [r, g, b, a], 0-1.");
                    return;
                }
            }
        }
    }
    if (args.contains("crowd_max_agents")) settings.crowdMaxAgents = std::clamp(args["crowd_max_agents"].get<int>(), 1, 4096);
    if (args.contains("web_bake_budget_ms")) settings.webBakeBudgetMs = std::clamp(args["web_bake_budget_ms"].get<float>(), 0.5f, 50.0f);
    if (args.contains("max_grid_solves_per_tick")) settings.maxGridSolvesPerTick = std::clamp(args["max_grid_solves_per_tick"].get<int>(), 1, 4096);
    if (args.contains("include_in_builds")) next.includeInBuilds = args["include_in_builds"].get<bool>();
    settings.sanitize();
    if (next == project->getNavigationSettings()) {
        call->ok("No change.", settingsJson(next));
        return;
    }
    project->getProjectCommandHistory()->addCommand(new NavigationSettingsCmd(project, next));
    Json data = settingsJson(project->getNavigationSettings());
    data["undo_name"] = "Change Navigation Settings";
    call->ok("Navigation settings changed (one undo step). Surfaces of a changed agent type need a bake.", data);
}

// ------------------------------------------------------------ paint_nav_grid

void paintNavGridTool(const CallPtr& call) {
    const Json& args = call->args();
    Project* project = call->ctx().project();
    std::string error;
    SceneProject* scene = sceneOf(call, false, error);
    if (!scene) {
        call->fail(error);
        return;
    }
    const Entity grid = McpRefs::resolveEntity(scene, args.contains("grid") ? args["grid"] : Json(), error);
    if (grid == NULL_ENTITY) {
        call->fail(error);
        return;
    }
    NavigationSystem* system = NavigationEditor::systemOf(*scene);
    NavGridComponent* component = scene->scene->findComponent<NavGridComponent>(grid);
    nav::NavGridMap* map = component ? system->getGridMap(grid) : nullptr;
    if (!map) {
        call->fail(McpRefs::entityPath(scene, grid) + " has no Nav Grid.");
        return;
    }
    const bool world = args.value("space", std::string("cell")) == "world";
    NavGridBrush::Stroke stroke;
    const Json& strokes = args["strokes"];
    for (size_t s = 0; s < strokes.size(); s++) {
        const Json& item = strokes[s];
        const std::string mode = item.value("mode", std::string("paint"));
        int area = nav::AREA_NOT_WALKABLE;
        if (item.contains("area") && item["area"].is_string()) area = nav::getSettings().findArea(item["area"].get<std::string>());
        else if (item.contains("area") && item["area"].is_number_integer()) area = item["area"].get<int>();
        if (mode == "erase") area = nav::AREA_WALKABLE;
        if (area < 0 || area >= nav::AREA_COUNT) {
            stroke.revert(*map);
            call->fail("strokes[" + std::to_string(s) + "]: area 0-15 or an area name.");
            return;
        }
        const int size = std::max(1, item.value("radius", 0) * 2 + 1);
        const NavGridBrush::Shape shape = item.value("shape", std::string("circle")) == "square" ? NavGridBrush::Shape::Square
                                                                                                 : NavGridBrush::Shape::Circle;
        std::vector<nav::GridCell> centres;
        const char* key = item.contains("cells") ? "cells" : "points";
        if (!item.contains(key) || !item[key].is_array() || item[key].empty()) {
            stroke.revert(*map);
            call->fail("strokes[" + std::to_string(s) + "]: cells [[x, y]...] or points [[x, y, z]...].");
            return;
        }
        for (const Json& p : item[key]) {
            if (std::string(key) == "cells" && !world) {
                if (!p.is_array() || p.size() != 2) {
                    stroke.revert(*map);
                    call->fail("strokes[" + std::to_string(s) + "].cells: [x, y] pairs.");
                    return;
                }
                centres.push_back(nav::GridCell{p[0].get<int>(), p[1].get<int>()});
            } else {
                Vector3 point;
                if (!McpValues::parseVector3(p, point)) {
                    stroke.revert(*map);
                    call->fail("strokes[" + std::to_string(s) + "].points: [x, y, z] world points.");
                    return;
                }
                centres.push_back(system->gridWorldToCell(grid, point));
            }
        }
        if (mode == "fill") {
            for (const nav::GridCell& centre : centres) stroke.apply(*map, NavGridBrush::flood(*map, centre), (uint8_t)area);
            continue;
        }
        if (mode != "paint" && mode != "erase") {
            stroke.revert(*map);
            call->fail("strokes[" + std::to_string(s) + "].mode: paint, erase or fill.");
            return;
        }
        // consecutive centres are joined (a dragged stroke)
        for (size_t c = 0; c < centres.size(); c++) {
            const std::vector<nav::GridCell> along = c == 0 ? std::vector<nav::GridCell>{centres[0]} : NavGridBrush::line(*map, centres[c - 1], centres[c]);
            for (const nav::GridCell& cell : along) stroke.apply(*map, NavGridBrush::stamp(*map, cell, size, shape), (uint8_t)area);
        }
    }
    const std::vector<NavGridCellsCmd::Cell> cells = stroke.cells();
    Json data = {{"grid", McpRefs::entityJson(scene, grid)}, {"changed_cells", cells.size()}};
    if (cells.empty()) {
        call->ok("No cell changed.", data);
        return;
    }
    component->mapDirty = true;
    CommandHandle::get(scene->id)->addCommand(new NavGridCellsCmd(project, scene->id, grid, cells));
    data["undo_name"] = "Paint Nav Grid";
    call->ok(std::to_string(cells.size()) + " cell(s) changed (one undo step \"Paint Nav Grid\"; the .navgrid is written on save).", data);
}

// ------------------------------------------------------------ visualize_navigation

const char* colorByName(NavOverlaySettings::ColorBy colorBy) {
    switch (colorBy) {
        case NavOverlaySettings::ColorBy::Areas: return "areas";
        case NavOverlaySettings::ColorBy::Islands: return "islands";
        case NavOverlaySettings::ColorBy::Tiles: return "tiles";
    }
    return "areas";
}

// The visible tile labels of the overlay's last update (S54 R33)
Json tileLabelsJson(SceneProject* scene) {
    Json labels = Json::array();
    const NavigationOverlay* overlay = NavigationEditor::get().overlayOf(scene->id);
    if (!overlay) return labels;
    for (const NavTileLabel& label : overlay->getTileLabels()) {
        Json item = {{"surface", McpRefs::entityJson(scene, label.surface)}, {"tile", Json::array({label.x, label.y})}, {"text", label.text}};
        if (label.dirty) item["dirty"] = true;
        labels.push_back(item);
    }
    return labels;
}

// After the overlay drew (tile labels) and the camera moved (frame): the reply, with the image
void visualizeReply(const CallPtr& call, uint32_t sceneId, Json data, std::string text, const std::string& view, int maxWidth,
                    bool labels) {
    if (view.empty() && !labels) {
        call->ok(text, data);
        return;
    }
    call->afterFrames(2, [call, sceneId, data, text, view, maxWidth, labels]() mutable {
        SceneProject* scene = call->ctx().project()->getScene(sceneId);
        if (!scene) {
            call->fail("The scene closed.");
            return;
        }
        if (labels) {
            data["tile_labels"] = tileLabelsJson(scene);
            text += " " + std::to_string(data["tile_labels"].size()) + " tile label(s) visible.";
        }
        if (view.empty()) {
            call->ok(text, data);
            return;
        }
        w3::CaptureRequest request;
        request.view = view;
        if (view == "scene") request.scene = sceneId;
        request.maxWidth = maxWidth;
        w3::captureView(call, request, [call, data, text, view](w3::CaptureResult result) {
            if (!result.ok) {
                call->fail("Screenshot failed: " + result.error, data);
                return;
            }
            Json out = data;
            out["image"] = result.info;
            if (!result.note.empty()) out["note"] = result.note;
            ToolResult reply = ToolResult::success(text + (view == "game" ? " Image: the Game view." : " Image: the Scene view."), out);
            reply.images.push_back(std::move(result.image));
            call->reply(std::move(reply));
        });
    });
}

void visualizeNavigationTool(const CallPtr& call) {
    const Json& args = call->args();
    std::string error;
    SceneProject* scene = sceneOf(call, true, error);
    if (!scene) {
        call->fail(error);
        return;
    }
    // screenshot: true is the Scene view (S54 R33)
    std::string view;
    if (args.contains("screenshot")) {
        const Json& shot = args["screenshot"];
        if (shot.is_boolean()) view = shot.get<bool>() ? "scene" : "";
        else if (shot.is_string() && (shot.get<std::string>() == "scene" || shot.get<std::string>() == "game")) view = shot.get<std::string>();
        else {
            call->fail("screenshot: true (the Scene view), \"scene\" or \"game\".");
            return;
        }
    }
    NavigationSystem* system = NavigationEditor::systemOf(*scene);
    // frame: the baked navmesh (every surface's islands) or a GameObject, checked before any change
    Entity frameEntity = NULL_ENTITY;
    AABB frameBox;
    const bool framing = args.contains("frame") && !args["frame"].is_null();
    if (framing) {
        const Json& frame = args["frame"];
        if (frame.is_string() && frame.get<std::string>() == "navmesh") {
            for (Entity surface : entitiesWith<NavMeshSurfaceComponent>(scene->scene)) {
                for (const nav::NavIsland& island : system->getIslands(surface)) frameBox.merge(AABB(island.boundsMin, island.boundsMax));
            }
            if (frameBox.isNull()) {
                call->fail("frame: no NavMesh Surface of " + scene->name + " is baked; bake_navigation first, or frame a GameObject.");
                return;
            }
        } else {
            frameEntity = McpRefs::resolveEntity(scene, frame, error);
            if (frameEntity == NULL_ENTITY) {
                call->fail("frame: \"navmesh\" or a GameObject. " + error);
                return;
            }
        }
        if (!call->ctx().sceneWindow()) {
            call->fail("frame: this editor has no Scene view.");
            return;
        }
    }
    NavOverlaySettings& overlay = scene->displaySettings.navigation;
    if (args.contains("show")) {
        const Json& show = args["show"];
        // Color By is one choice: islands and tile_colors cannot both be on
        if (show.value("islands", false) && show.value("tile_colors", false)) {
            call->fail("show.islands and show.tile_colors both colour the navmesh: turn on one.");
            return;
        }
        overlay.showNavMesh = show.value("navmesh", overlay.showNavMesh);
        overlay.showTileBounds = show.value("tiles", overlay.showTileBounds);
        overlay.showHeightMesh = show.value("heightmesh", overlay.showHeightMesh);
        overlay.showGrid = show.value("grid", overlay.showGrid);
        overlay.showLinks = show.value("links", overlay.showLinks);
        overlay.showObstacles = show.value("obstacles", overlay.showObstacles);
        overlay.showAreaOverrides = show.value("area_overrides", overlay.showAreaOverrides);
        overlay.showAvoidance = show.value("avoidance", overlay.showAvoidance);
        overlay.showSteepSlopes = show.value("slope", overlay.showSteepSlopes);
        overlay.showTileLabels = show.value("tile_labels", overlay.showTileLabels);
        auto colorBy = [&overlay, &show](const char* key, NavOverlaySettings::ColorBy value) {
            if (!show.contains(key)) return;
            if (show[key].get<bool>()) overlay.colorBy = value;
            else if (overlay.colorBy == value) overlay.colorBy = NavOverlaySettings::ColorBy::Areas;
        };
        colorBy("islands", NavOverlaySettings::ColorBy::Islands);
        colorBy("tile_colors", NavOverlaySettings::ColorBy::Tiles);
        if (show.contains("paths")) {
            const Json& paths = show["paths"];
            if (paths.is_boolean()) overlay.showPaths = paths.get<bool>() ? NavOverlaySettings::Paths::All : NavOverlaySettings::Paths::None;
            else if (paths.is_string()) {
                const std::string name = paths.get<std::string>();
                overlay.showPaths = name == "all" ? NavOverlaySettings::Paths::All : name == "none" ? NavOverlaySettings::Paths::None
                                                                                                     : NavOverlaySettings::Paths::Selected;
            }
        }
    }
    Json data = {{"scene_id", scene->id}};
    std::string text = "AI Navigation overlay on.";
    NavigationEditor::get().clearQueryDraw(scene->id);
    if (args.contains("path") && args["path"].is_object()) {
        const Json& query = args["path"];
        NavPoint from, to;
        nav::QueryFilter filter;
        if (!parseNavPointArg(scene, query, "from", from, error) || !parseNavPointArg(scene, query, "to", to, error) ||
            !queryFilterOf(scene, query, from, NULL_ENTITY, filter, error)) {
            call->fail("path." + error);
            return;
        }
        labelIslandsOf(scene->scene, system, filter.agentType, NULL_ENTITY);
        const nav::NavPath path = system->calculatePath(from.position, to.position, filter);
        NavigationEditor::get().setQueryDraw(scene->id, NavQueryDraw{path, from.position, to.position, true, true});
        Json drawn = {{"status", nav::pathStatusName(path.status)}, {"corners", path.corners.size()}, {"length", rounded(path.length)},
                      {"agent_type", filter.agentType}, {"area_mask", filter.areaMask}};
        if (path.status != nav::PathStatus::Complete) addFailureJson(scene, drawn, path.failure, path.surface, path.closestPoint, path.gap);
        else if (path.surface != NULL_ENTITY) drawn["surface"] = McpRefs::entityJson(scene, path.surface);
        data["path"] = drawn;
        text += " Path " + std::string(nav::pathStatusName(path.status)) + ", " + numberText(path.length) + " long (drawn " +
                (path.status == nav::PathStatus::Complete ? "magenta)." : "orange, its gap red).") + failureText(path.failure, path.gap);
    }
    overlay.enabled = true;
    data["overlay"] = {{"navmesh", overlay.showNavMesh}, {"tiles", overlay.showTileBounds}, {"heightmesh", overlay.showHeightMesh},
                       {"grid", overlay.showGrid}, {"links", overlay.showLinks}, {"obstacles", overlay.showObstacles},
                       {"area_overrides", overlay.showAreaOverrides}, {"avoidance", overlay.showAvoidance},
                       {"color_by", colorByName(overlay.colorBy)}, {"slope", overlay.showSteepSlopes},
                       {"tile_labels", overlay.showTileLabels},
                       {"paths", overlay.showPaths == NavOverlaySettings::Paths::All ? "all" : overlay.showPaths == NavOverlaySettings::Paths::None ? "none" : "selected"}};
    scene->needUpdateRender = true;
    const int maxWidth = args.value("max_width", 1024);
    const bool labels = overlay.showTileLabels;
    if (!framing && !labels) {
        visualizeReply(call, scene->id, data, text, view, maxWidth, false);
        return;
    }
    // framing and tile labels need the Scene view laid out (its camera, its overlay update)
    const uint32_t sceneId = scene->id;
    w3::whenSceneViewShown(call, sceneId, true, 10000, [call, sceneId, data, text, view, maxWidth, labels, framing, frameEntity,
                                                       frameBox](bool ready, const std::string& err) mutable {
        if (!ready) {
            call->fail(err);
            return;
        }
        SceneProject* s = call->ctx().project()->getScene(sceneId);
        SceneWindow* window = call->ctx().sceneWindow();
        if (!s || !window) {
            call->fail("The scene closed.");
            return;
        }
        if (framing) {
            if (frameEntity != NULL_ENTITY) window->frameEntities(sceneId, sceneId, {frameEntity});
            else window->frameBounds(sceneId, frameBox);
            // an agent's framing lands at once (Camera Easing animates the user's)
            window->getViewController().finishTween(*s);
            data["framed"] = frameEntity != NULL_ENTITY ? McpRefs::entityJson(s, frameEntity) : Json("navmesh");
            data["camera"] = w3::sceneViewCameraJson(s);
            text += " Framed " + (frameEntity != NULL_ENTITY ? McpRefs::entityPath(s, frameEntity) : std::string("the navmesh")) + ".";
            s->needUpdateRender = true;
        }
        visualizeReply(call, sceneId, data, text, view, maxWidth, labels);
    });
}

} // namespace

void registerNavigationTools(McpRegistry& registry) {
    {
        ToolDef tool;
        tool.name = "bake_navigation";
        tool.title = "Bake Navigation";
        tool.description =
            "Bake NavMesh Surfaces as a job (edit mode): dirty rebuilds the tiles whose inputs changed (all of them the first "
            "time), full every tile, clear removes the navmesh and its .navmesh. Saved to the surface's NavMesh Data. wait:false "
            "returns a job_id for get_job. Guide: jasper://docs/navigation.";
        tool.inputSchema = object({
            {"scene", sceneRef()},
            {"surfaces", entities("Default: every NavMesh Surface of the scene.")},
            {"mode", withDefault(stringEnum("", {"dirty", "full", "clear"}), "dirty")},
            {"wait", withDefault(boolean(""), true)},
            {"timeout_seconds", withDefault(number("", 0.0, 3600.0), 120)}
        });
        // clear deletes the .navmesh; a game rebakes with NavMeshSurface:bakeAsync()
        tool.annotations.destructive = true;
        tool.activity = ToolActivity::Edit;
        tool.allowedWhilePlaying = false;
        tool.handler = bakeNavigationTool;
        registry.addTool(std::move(tool));
    }
    {
        ToolDef tool;
        tool.name = "get_navigation_info";
        tool.title = "Get Navigation Info";
        tool.description =
            "A scene's navigation state (the running one while playing): settings, surfaces, grids, links, agents, obstacles, "
            "modifiers and crowds, and its findings: each problem with its fix. Start here when agents do not move. entities: "
            "only those GameObjects.";
        tool.inputSchema = object({{"scene", sceneRef()}, {"entities", entities()}});
        tool.annotations.readOnly = true;
        tool.annotations.idempotent = true;
        tool.activity = ToolActivity::Read;
        tool.allowedWhilePlaying = true;
        tool.handler = getNavigationInfoTool;
        registry.addTool(std::move(tool));
    }
    {
        ToolDef tool;
        tool.name = "query_navigation";
        tool.title = "Query Navigation";
        tool.description =
            "A navmesh or Nav Grid query in world space, in edit or play: path, sample (nearest point), raycast, closest_edge, "
            "reachable, locate (is a point on the navmesh; if not, why), random, move_along. A failed path says why and what to "
            "do. from/to: a point or a GameObject (an agent: its feet, type and mask). target: navmesh (by agent_type), grid or a "
            "surface/grid GameObject. area_mask: a bit per area.";
        tool.inputSchema = object({
            {"op", withDefault(stringEnum("", {"path", "sample", "raycast", "closest_edge", "reachable", "locate", "random", "move_along"}), "path")},
            {"from", navPointSchema("")},
            {"to", navPointSchema("Or a list of up to 64.", MAX_TARGETS)},
            {"max_distance", number("Default: sample 2, locate 5; closest_edge, random.", 0.0, 1e6)},
            {"seed", integer("random.", 0, 4294967295LL)},
            {"agent_type", agentTypeRef()},
            {"area_mask", integer("", 0, 65535)},
            {"allow_partial", withDefault(boolean("Grid paths."), true)},
            {"target", Json{{"description", "navmesh | grid | a GameObject."}, {"anyOf", Json::array({stringEnum("", {"navmesh", "grid"}), entityRef()})}}},
            {"scene", sceneRef()}
        });
        tool.annotations.readOnly = true;
        tool.annotations.idempotent = true;
        tool.activity = ToolActivity::Read;
        tool.allowedWhilePlaying = true;
        tool.handler = queryNavigationTool;
        registry.addTool(std::move(tool));
    }
    {
        ToolDef tool;
        tool.name = "set_navigation_settings";
        tool.title = "Set Navigation Settings";
        tool.description =
            "Change the project's navigation settings as one undo step: agents replaces the agent type list ({id?, name, "
            "radius, height, step_height, max_slope}; a known name keeps its id), areas edits rows ({index, name?, cost >= 1, "
            "color?}; 0-2 keep their names), and the crowd, web bake and grid budgets.";
        Json agent = object({{"id", integer("", 0, 1000000)}, {"name", string("")}, {"radius", number("")}, {"height", number("")},
                             {"step_height", number("")}, {"max_slope", number("Degrees.")}}, {"name"});
        Json area = object({{"index", integer("", 0, 15)}, {"name", string("")}, {"cost", number("")}, {"color", color()}}, {"index"});
        tool.inputSchema = object({
            {"agents", Json{{"type", "array"}, {"minItems", 1}, {"maxItems", 64}, {"items", agent}}},
            {"areas", Json{{"type", "array"}, {"maxItems", 16}, {"items", area}}},
            {"crowd_max_agents", integer("", 1, 4096)},
            {"web_bake_budget_ms", number("", 0.5, 50.0)},
            {"max_grid_solves_per_tick", integer("", 1, 4096)},
            {"include_in_builds", boolean("Recast/Detour in exported builds.")}
        });
        // area costs apply at the next tick; the edit stays after Stop, like Project Settings
        tool.activity = ToolActivity::Edit;
        tool.allowedWhilePlaying = true;
        tool.handler = setNavigationSettingsTool;
        registry.addTool(std::move(tool));
    }
    {
        ToolDef tool;
        tool.name = "paint_nav_grid";
        tool.title = "Paint Nav Grid";
        tool.description =
            "Paint a Nav Grid's cells as ONE undo step (\"Paint Nav Grid\"), like the Scene view brush: strokes of mode paint "
            "(area: index or name, default Not Walkable), erase (to Walkable) or fill (flood the clicked area), at cells "
            "[[x, y]] (space cell) or world points (space world); consecutive centres are joined, radius in cells, shape circle "
            "or square. Painted cells survive a Rebuild (overrides). The .navgrid is written on save.";
        Json stroke = object({
            {"mode", withDefault(stringEnum("", {"paint", "erase", "fill"}), "paint")},
            {"area", Json{{"anyOf", Json::array({Json{{"type", "integer"}, {"minimum", 0}, {"maximum", 15}}, Json{{"type", "string"}}})}}},
            {"cells", Json{{"type", "array"}, {"items", Json{{"type", "array"}, {"minItems", 2}, {"maxItems", 2}, {"items", Json{{"type", "integer"}}}}}}},
            {"points", Json{{"type", "array"}, {"items", vec3("")}}},
            {"radius", withDefault(integer("Cells around the centre.", 0, 256), 0)},
            {"shape", withDefault(stringEnum("", {"circle", "square"}), "circle")}
        });
        tool.inputSchema = object({
            {"grid", entityRef()},
            {"strokes", Json{{"type", "array"}, {"minItems", 1}, {"maxItems", 256}, {"items", stroke}}},
            {"space", withDefault(stringEnum("", {"cell", "world"}), "cell")},
            {"scene", sceneRef()}
        }, {"grid", "strokes"});
        // the brush is edit-only; a game edits cells from Lua
        tool.activity = ToolActivity::Edit;
        tool.allowedWhilePlaying = false;
        tool.handler = paintNavGridTool;
        registry.addTool(std::move(tool));
    }
    {
        ToolDef tool;
        tool.name = "visualize_navigation";
        tool.title = "Visualize Navigation";
        tool.description =
            "Turn on the Scene view's AI Navigation overlay and set what it shows (navmesh in area, island or tile colours, "
            "steep slopes, tile labels, agent paths, links, area overrides...) of a scene (the running one while playing); path "
            "draws a query_navigation path with its reason. frame first moves the Scene camera to the navmesh or a GameObject. "
            "screenshot: true or scene returns the Scene view, game the Game view.";
        tool.inputSchema = object({
            {"scene", sceneRef()},
            {"show", object({{"navmesh", boolean("")}, {"tiles", boolean("")}, {"heightmesh", boolean("")}, {"grid", boolean("")},
                             {"paths", Json{{"anyOf", Json::array({Json{{"type", "boolean"}}, stringEnum("", {"none", "selected", "all"})})}}},
                             {"avoidance", boolean("")}, {"links", boolean("")}, {"obstacles", boolean("")}, {"islands", boolean("")},
                             {"tile_colors", boolean("")}, {"slope", boolean("")}, {"tile_labels", boolean("")},
                             {"area_overrides", boolean("")}})},
            {"path", object({{"from", navPointSchema("")}, {"to", navPointSchema("")}, {"agent_type", agentTypeRef()},
                             {"area_mask", integer("", 0, 65535)}}, {"from", "to"})},
            {"frame", Json{{"description", "navmesh or a GameObject."}, {"type", Json::array({"integer", "string"})}}},
            {"screenshot", Json{{"anyOf", Json::array({Json{{"type", "boolean"}}, stringEnum("", {"scene", "game"})})}}},
            {"max_width", withDefault(integer("", 0, 4096), 1024)}
        });
        // display settings of the workspace (and, with frame, the Scene camera): not read-only
        tool.annotations.idempotent = true;
        tool.activity = ToolActivity::Control;
        tool.allowedWhilePlaying = true;
        tool.handler = visualizeNavigationTool;
        registry.addTool(std::move(tool));
    }
}

}
