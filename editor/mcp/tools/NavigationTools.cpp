// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// Navigation (docs/specs/S19-navigation.md R37): bake NavMesh Surfaces as a job, read the
// navigation state (settings, surfaces, grids, links, agents and crowds), query paths, samples,
// raycasts, closest edges and reachability on navmeshes or NavGrids (edit and play), change the
// project's navigation settings, paint NavGrid cells (one undo step) and show the Scene view's
// AI Navigation overlay (with a screenshot). The guide is docs/mcp/guide/navigation.md.
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
#include "component/NavMeshSurfaceComponent.h"
#include "component/Transform.h"
#include "navigation/NavGridBrush.h"
#include "navigation/NavGridCellsCmd.h"
#include "navigation/NavigationEditor.h"
#include "navigation/NavigationSettingsCmd.h"
#include "subsystem/NavigationSystem.h"

#include <algorithm>
#include <cmath>
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

Json bakeResultJson(SceneProject* scene, Entity surface, const NavBakeResult& result) {
    Json item = {{"entity", McpRefs::entityJson(scene, surface)}, {"ok", result.ok}, {"tiles_built", result.tilesBuilt},
                 {"tiles_total", result.tilesTotal}, {"polys", result.polys}, {"ms", rounded(result.milliseconds)},
                 {"warnings", result.warnings}};
    if (result.cancelled) item["cancelled"] = true;
    if (!result.error.empty() && !result.cancelled) item["error"] = result.error;
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
        for (Entity surface : state->surfaces) {
            const NavBakeResult& result = state->results[surface];
            list.push_back(bakeResultJson(sp, surface, result));
            ok += result.ok ? 1 : 0;
            built += result.tilesBuilt;
            polys += result.polys;
            cancelled = cancelled || result.cancelled;
            text += "\n" + (sp && sp->scene ? McpRefs::entityPath(sp, surface) : std::to_string(surface)) + ": " +
                    (result.ok ? std::to_string(result.tilesBuilt) + "/" + std::to_string(result.tilesTotal) + " tiles, " +
                                     std::to_string(result.polys) + " polys, " + std::to_string((int)result.milliseconds) + " ms"
                               : (result.cancelled ? std::string("cancelled") : "failed: " + result.error));
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
            NavBakeResult failed;
            failed.error = why;
            state->results[surface] = failed;
            state->waiting--;
        }
    }
    finishIfDone();
    state->job->setProgress(0.0, "baking");
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

void getNavigationInfoTool(const CallPtr& call) {
    Project* project = call->ctx().project();
    std::string error;
    SceneProject* scene = sceneOf(call, true, error);
    if (!scene) {
        call->fail(error);
        return;
    }
    NavigationSystem* system = NavigationEditor::systemOf(*scene);
    Scene* s = scene->scene;
    const bool playing = scene->playState != ScenePlayState::STOPPED;
    std::string text = "Navigation of " + scene->name + (playing ? " (playing)" : "") + ":";

    Json surfaces = Json::array();
    for (Entity entity : entitiesWith<NavMeshSurfaceComponent>(s)) {
        const NavMeshSurfaceComponent& surface = s->getComponent<NavMeshSurfaceComponent>(entity);
        const NavSurfaceStatus status = system->getSurfaceStatus(entity);
        const int dirty = playing ? status.dirtyTiles : NavigationEditor::get().dirtyTiles(project, scene->id, entity);
        Json item = {{"entity", McpRefs::entityJson(scene, entity)}, {"state", stateName(status.state)}, {"tiles", status.tiles},
                     {"dirty_tiles", dirty}, {"polys", status.polys}, {"bytes", status.bytes}, {"carving", status.carving},
                     {"agent_type", surface.agentType}, {"enabled", surface.enabled}, {"asset", surface.navMeshData.path}};
        if (status.state == NavBakeState::BAKING) item["progress"] = rounded(status.progress);
        if (!status.loadError.empty()) item["load_error"] = status.loadError;
        if (playing) item["crowd"] = {{"agents", status.crowdAgents}, {"capacity", status.crowdCapacity}};
        surfaces.push_back(item);
        text += "\n  surface " + McpRefs::entityPath(scene, entity) + ": " + stateName(status.state) + ", " + std::to_string(status.tiles) +
                " tiles, " + std::to_string(status.polys) + " polys" + (dirty > 0 ? ", " + std::to_string(dirty) + " dirty tiles" : "");
    }
    Json grids = Json::array();
    for (Entity entity : entitiesWith<NavGridComponent>(s)) {
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
    Json links = Json::array();
    for (Entity entity : entitiesWith<NavMeshLinkComponent>(s)) {
        const NavMeshLinkComponent& link = s->getComponent<NavMeshLinkComponent>(entity);
        const Transform* transform = s->findComponent<Transform>(entity);
        const Matrix4 model = transform ? transform->modelMatrix : Matrix4();
        links.push_back({{"entity", McpRefs::entityJson(scene, entity)}, {"start", vec3Json(model * link.startPoint)},
                         {"end", vec3Json(model * link.endPoint)}, {"bidirectional", link.bidirectional}, {"area", link.area},
                         {"width", link.width}, {"enabled", link.enabled}});
    }
    Json agents = Json::array();
    for (Entity entity : entitiesWith<NavMeshAgentComponent>(s)) {
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
        }
        agents.push_back(item);
    }
    Json data = {{"scene_id", scene->id}, {"playing", playing}, {"navmesh_available", nav::isNavMeshAvailable()},
                 {"settings", settingsJson(project->getNavigationSettings())}, {"surfaces", surfaces}, {"grids", grids},
                 {"links", links}, {"agents", agents},
                 {"stats", {{"crowds", system->getCrowdCount()}, {"crowd_agents", system->getAgentCount()}, {"bytes", system->getMemoryBytes()}}}};
    text += "\n  " + std::to_string(links.size()) + " link(s), " + std::to_string(agents.size()) + " agent(s)";
    call->ok(text, data);
}

// ------------------------------------------------------------ query_navigation

bool parsePoint(const Json& args, const char* key, Vector3& out, std::string& error) {
    if (!args.contains(key) || !McpValues::parseVector3(args[key], out)) {
        error = std::string(key) + ": [x, y, z] (world).";
        return false;
    }
    return true;
}

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

Json hitJson(const nav::NavHit& hit) {
    return {{"hit", hit.hit}, {"position", vec3Json(hit.position)}, {"normal", vec3Json(hit.normal)}, {"distance", rounded(hit.distance)},
            {"area", hit.area}, {"area_name", nav::getSettings().areas[(size_t)std::min<int>(hit.area, nav::AREA_COUNT - 1)].name}};
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

void queryGrid(const CallPtr& call, SceneProject* scene, NavigationSystem* system, Entity grid, const std::string& op) {
    const Json& args = call->args();
    std::string error;
    Vector3 from, to;
    if (op != "path" && op != "sample" && op != "reachable") {
        call->fail("op " + op + " is for navmeshes; a grid takes path, sample or reachable.");
        return;
    }
    if (!parsePoint(args, "from", from, error)) {
        call->fail(error);
        return;
    }
    nav::NavGridMap* map = system->getGridMap(grid);
    if (!map) {
        call->fail("The grid has no cells.");
        return;
    }
    const nav::GridCell a = system->gridWorldToCell(grid, from);
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
    if (!parsePoint(args, "to", to, error)) {
        call->fail(error);
        return;
    }
    const nav::GridCell b = system->gridWorldToCell(grid, to);
    data["to_cell"] = cellJson(b);
    if (op == "reachable") {
        const bool reachable = map->contains(a) && map->contains(b) && map->isWalkable(map->index(a)) && map->isWalkable(map->index(b)) &&
                               map->isReachable(a, b);
        data["reachable"] = reachable;
        call->ok(reachable ? "Reachable." : "Not reachable.", data);
        return;
    }
    nav::GridQuery query;
    query.allowPartial = args.value("allow_partial", true);
    if (args.contains("area_mask")) query.areaMask = (uint16_t)(args["area_mask"].get<int>() & nav::ALL_AREAS);
    const NavGridComponent& component = scene->scene->getComponent<NavGridComponent>(grid);
    query.movement = component.movement;
    const nav::GridPath path = system->findGridPath(grid, a, b, query);
    Json cells = Json::array();
    for (const nav::GridCell& c : path.cells) cells.push_back(cellJson(c));
    Json corners = Json::array();
    for (const Vector3& p : system->gridPathToWorld(grid, path)) corners.push_back(vec3Json(p));
    data["status"] = nav::pathStatusName(path.status);
    data["cells"] = cells;
    data["corners"] = corners;
    data["cost"] = rounded(path.cost);
    data["expanded"] = path.expanded;
    call->ok(std::string("Grid path ") + nav::pathStatusName(path.status) + ": " + std::to_string(path.cells.size()) + " cells, cost " +
             std::to_string(rounded(path.cost)), data);
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
    nav::QueryFilter filter;
    if (!agentTypeOf(args, filter.agentType, error)) {
        call->fail(error);
        return;
    }
    if (args.contains("area_mask")) filter.areaMask = (uint16_t)(args["area_mask"].get<int>() & nav::ALL_AREAS);
    if (targetEntity != NULL_ENTITY) filter.agentType = scene->scene->getComponent<NavMeshSurfaceComponent>(targetEntity).agentType;
    Json data = {{"target", "navmesh"}, {"op", op}, {"agent_type", filter.agentType}, {"area_mask", filter.areaMask}};
    if (targetEntity != NULL_ENTITY) data["surface"] = McpRefs::entityJson(scene, targetEntity);
    Vector3 from, to;
    nav::NavHit hit;
    if (op == "random") {
        // S55 R42: anywhere without from; around from within max_distance; else on from's island.
        // Its own stream (the seed, else the default): the game's stream is never drawn from.
        nav::RandomPointQuery query;
        query.seeded = true;
        query.seed = args.contains("seed") ? args["seed"].get<uint64_t>() : NavigationSystem::DEFAULT_RANDOM_SEED;
        if (args.contains("from")) {
            if (!parsePoint(args, "from", from, error)) {
                call->fail(error);
                return;
            }
            query.around = args.contains("max_distance");
            query.center = from;
            query.radius = args.value("max_distance", 0.0f);
            query.sameIsland = !query.around;
            query.islandOf = from;
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
    if (!parsePoint(args, "from", from, error)) {
        call->fail(error);
        return;
    }
    if (op == "sample") {
        const float distance = args.value("max_distance", 2.0f);
        const bool found = targetEntity != NULL_ENTITY ? system->samplePositionOn(targetEntity, from, distance, hit, filter.areaMask)
                                                       : system->samplePosition(from, distance, hit, filter);
        data.update(hitJson(hit));
        data["found"] = found;
        call->ok(found ? "Nearest navmesh point " + vec3Json(hit.position).dump() + ", " + std::to_string(rounded(hit.distance)) + " away"
                       : "No navmesh within " + std::to_string(distance) + ".", data);
        return;
    }
    if (op == "closest_edge") {
        const float maxDistance = args.value("max_distance", -1.0f);
        const bool found = targetEntity != NULL_ENTITY ? system->findClosestEdgeOn(targetEntity, from, hit, filter, maxDistance)
                                                       : system->findClosestEdge(from, hit, filter, maxDistance);
        data.update(hitJson(hit));
        data["found"] = found;
        call->ok(found ? "Closest edge " + vec3Json(hit.position).dump() + ", " + std::to_string(rounded(hit.distance)) + " away"
                 : hit.distance > 0.0f ? "No edge within " + std::to_string(rounded(hit.distance)) + "."
                                       : std::string("No navmesh at from."), data);
        return;
    }
    if (!parsePoint(args, "to", to, error)) {
        call->fail(error);
        return;
    }
    if (op == "raycast") {
        const bool blocked = targetEntity != NULL_ENTITY ? system->raycastOn(targetEntity, from, to, hit, filter)
                                                         : system->raycast(from, to, hit, filter);
        data.update(hitJson(hit));
        data["blocked"] = blocked;
        data["cost"] = rounded(hit.cost);
        call->ok(blocked ? "Blocked at " + vec3Json(hit.position).dump() : "Clear line.", data);
        return;
    }
    if (op == "move_along") {
        const bool found = targetEntity != NULL_ENTITY ? system->moveAlongSurfaceOn(targetEntity, from, to, hit, filter)
                                                       : system->moveAlongSurface(from, to, hit, filter);
        data["found"] = found;
        data["position"] = vec3Json(found ? hit.position : from);
        data["blocked"] = hit.hit;
        data["distance"] = rounded(hit.distance);
        data["area"] = hit.area;
        call->ok(!found ? std::string("No navmesh at from.")
                 : hit.hit ? "Stopped at " + vec3Json(hit.position).dump() + " after " + std::to_string(rounded(hit.distance))
                           : "Reached " + vec3Json(hit.position).dump(), data);
        return;
    }
    if (op == "reachable") {
        const bool reachable = system->isReachable(from, to, filter);
        data["reachable"] = reachable;
        call->ok(reachable ? "Reachable." : "Not reachable.", data);
        return;
    }
    if (op != "path") {
        call->fail("op: path, sample, raycast, closest_edge, reachable, random or move_along.");
        return;
    }
    const nav::NavPath path = targetEntity != NULL_ENTITY ? system->calculatePathOn(targetEntity, from, to, filter)
                                                          : system->calculatePath(from, to, filter);
    Json corners = Json::array();
    for (const Vector3& p : path.corners) corners.push_back(vec3Json(p));
    data["status"] = nav::pathStatusName(path.status);
    data["corners"] = corners;
    data["length"] = rounded(path.length);
    addPathCosts(data, path);
    call->ok(std::string("Path ") + nav::pathStatusName(path.status) + ": " + std::to_string(path.corners.size()) + " corners, " +
             std::to_string(rounded(path.length)) + " long", data);
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

void visualizeNavigationTool(const CallPtr& call) {
    const Json& args = call->args();
    std::string error;
    SceneProject* scene = sceneOf(call, false, error);
    if (!scene) {
        call->fail(error);
        return;
    }
    NavOverlaySettings& overlay = scene->displaySettings.navigation;
    overlay.enabled = true;
    if (args.contains("show")) {
        const Json& show = args["show"];
        overlay.showNavMesh = show.value("navmesh", overlay.showNavMesh);
        overlay.showTileBounds = show.value("tiles", overlay.showTileBounds);
        overlay.showHeightMesh = show.value("heightmesh", overlay.showHeightMesh);
        overlay.showGrid = show.value("grid", overlay.showGrid);
        overlay.showLinks = show.value("links", overlay.showLinks);
        overlay.showObstacles = show.value("obstacles", overlay.showObstacles);
        overlay.showAvoidance = show.value("avoidance", overlay.showAvoidance);
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
    Json data = {{"scene_id", scene->id}, {"overlay", encodeNavOverlaySettings(overlay).size() ? Json::parse("{}") : Json::object()}};
    data["overlay"] = {{"navmesh", overlay.showNavMesh}, {"tiles", overlay.showTileBounds}, {"heightmesh", overlay.showHeightMesh},
                       {"grid", overlay.showGrid}, {"links", overlay.showLinks}, {"obstacles", overlay.showObstacles},
                       {"avoidance", overlay.showAvoidance},
                       {"paths", overlay.showPaths == NavOverlaySettings::Paths::All ? "all" : overlay.showPaths == NavOverlaySettings::Paths::None ? "none" : "selected"}};
    std::string text = "AI Navigation overlay on.";
    NavigationEditor::get().setQueryPath(scene->id, {});
    if (args.contains("path") && args["path"].is_object()) {
        Vector3 from, to;
        if (!parsePoint(args["path"], "from", from, error) || !parsePoint(args["path"], "to", to, error)) {
            call->fail("path." + error);
            return;
        }
        NavigationSystem* system = NavigationEditor::systemOf(*scene);
        const nav::NavPath path = system->calculatePath(from, to);
        NavigationEditor::get().setQueryPath(scene->id, path.corners);
        data["path"] = {{"status", nav::pathStatusName(path.status)}, {"corners", path.corners.size()}, {"length", rounded(path.length)}};
        text += std::string(" Path ") + nav::pathStatusName(path.status) + ", " + std::to_string(rounded(path.length)) + " long (drawn magenta).";
    }
    scene->needUpdateRender = true;
    if (!args.value("screenshot", false)) {
        call->ok(text, data);
        return;
    }
    w3::CaptureRequest request;
    request.view = "scene";
    request.scene = scene->id;
    request.maxWidth = args.value("max_width", 1024);
    call->afterFrames(2, [call, request, data, text]() {
        w3::captureView(call, request, [call, data, text](w3::CaptureResult result) {
            if (!result.ok) {
                call->fail("Screenshot failed: " + result.error, data);
                return;
            }
            Json out = data;
            out["image"] = result.info;
            if (!result.note.empty()) out["note"] = result.note;
            ToolResult reply = ToolResult::success(text + " Image: the Scene view.", out);
            reply.images.push_back(std::move(result.image));
            call->reply(std::move(reply));
        });
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
            "time), full every tile, clear removes the navmesh and its .navmesh. Saved to the surface's NavMesh Data. Result: "
            "tiles built, polys, ms, warnings per surface. wait:false returns a job_id for get_job. Guide: jasper://docs/navigation.";
        tool.inputSchema = object({
            {"scene", sceneRef()},
            {"surfaces", entities("Default: every NavMesh Surface of the scene.")},
            {"mode", withDefault(stringEnum("", {"dirty", "full", "clear"}), "dirty")},
            {"wait", withDefault(boolean(""), true)},
            {"timeout_seconds", withDefault(number("", 0.0, 3600.0), 120)}
        });
        tool.handler = bakeNavigationTool;
        registry.addTool(std::move(tool));
    }
    {
        ToolDef tool;
        tool.name = "get_navigation_info";
        tool.title = "Get Navigation Info";
        tool.description =
            "Navigation state of a scene (the running one while playing): settings (agent types, areas, budgets), surfaces "
            "(state, tiles, dirty tiles, polys, bytes, asset), grids (size, cells per area, overrides), links (world ends), "
            "agents (in play: destination, path_status, remaining_distance, velocity, is_on_navmesh) and crowd stats.";
        tool.inputSchema = object({{"scene", sceneRef()}});
        tool.annotations.readOnly = true;
        tool.annotations.idempotent = true;
        tool.handler = getNavigationInfoTool;
        registry.addTool(std::move(tool));
    }
    {
        ToolDef tool;
        tool.name = "query_navigation";
        tool.title = "Query Navigation";
        tool.description =
            "A navmesh or Nav Grid query in world space, in edit or play: path (corners, length, status, cost), sample, "
            "raycast, closest_edge, reachable, random, move_along (jasper://docs/navigation). target: navmesh (default, by "
            "agent_type), grid (the first Nav Grid) or a surface/grid GameObject. area_mask: a bit per area.";
        tool.inputSchema = object({
            {"op", withDefault(stringEnum("", {"path", "sample", "raycast", "closest_edge", "reachable", "random", "move_along"}), "path")},
            {"from", vec3("Not for random anywhere.")},
            {"to", vec3("path, raycast, reachable, move_along.")},
            {"max_distance", number("sample (2), closest_edge, random.", 0.0, 1e6)},
            {"seed", integer("random.", 0, 4294967295LL)},
            {"agent_type", Json{{"description", "Name or id. Default: the first."}, {"anyOf", Json::array({Json{{"type", "string"}}, Json{{"type", "integer"}}})}}},
            {"area_mask", integer("", 0, 65535)},
            {"allow_partial", withDefault(boolean("Grid paths."), true)},
            {"target", Json{{"description", "navmesh | grid | a GameObject."}, {"anyOf", Json::array({stringEnum("", {"navmesh", "grid"}), entityRef()})}}},
            {"scene", sceneRef()}
        });
        tool.annotations.readOnly = true;
        tool.annotations.idempotent = true;
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
        tool.handler = paintNavGridTool;
        registry.addTool(std::move(tool));
    }
    {
        ToolDef tool;
        tool.name = "visualize_navigation";
        tool.title = "Visualize Navigation";
        tool.description =
            "Turn on the Scene view's AI Navigation overlay and set what it shows (navmesh in area colours, tiles, heightmesh, "
            "grid, agent paths, avoidance, links, obstacles); path {from, to} also draws a query path. screenshot: true returns "
            "the Scene view as a PNG.";
        tool.inputSchema = object({
            {"scene", sceneRef()},
            {"show", object({{"navmesh", boolean("")}, {"tiles", boolean("")}, {"heightmesh", boolean("")}, {"grid", boolean("")},
                             {"paths", Json{{"anyOf", Json::array({Json{{"type", "boolean"}}, stringEnum("", {"none", "selected", "all"})})}}},
                             {"avoidance", boolean("")}, {"links", boolean("")}, {"obstacles", boolean("")}})},
            {"path", object({{"from", vec3("")}, {"to", vec3("")}}, {"from", "to"})},
            {"screenshot", withDefault(boolean(""), false)},
            {"max_width", withDefault(integer("", 0, 4096), 1024)}
        });
        tool.handler = visualizeNavigationTool;
        registry.addTool(std::move(tool));
    }
}

}
