// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "NavigationSystem.h"
#include "NavigationAreas.h"

#include "Engine.h"
#include "Log.h"
#include "Scene.h"
#include "component/MeshComponent.h"
#include "component/NavGridComponent.h"
#include "component/NavMeshAgentComponent.h"
#include "component/NavMeshLinkComponent.h"
#include "component/NavMeshModifierComponent.h"
#include "component/NavMeshObstacleComponent.h"
#include "component/NavMeshSurfaceComponent.h"
#include "component/TerrainComponent.h"
#include "navigation/NavHash.h"
#include "component/TilemapComponent.h"
#include "component/Transform.h"
#include "io/Data.h"
#include "jobs/JobSystem.h"
#include "navigation/NavBakeCollector.h"
#include "navigation/NavBakeScheduler.h"
#include "navigation/NavCrowd.h"
#include "object/navigation/NavMeshAgent.h"
#include "object/navigation/NavMeshSurface.h"
#include "profiler/Profile.h"
#include "subsystem/MeshSystem.h"
#include "subsystem/PhysicsSystem.h"

#ifdef JASPERENGINE_PHYSICS_2D
#include "component/CharacterController2DComponent.h"
#include "component/Rigidbody2DComponent.h"
#include "object/physics/CharacterController2D.h"
#include "object/physics/Rigidbody2D.h"
#endif
#ifdef JASPERENGINE_PHYSICS_3D
#include "component/CharacterControllerComponent.h"
#include "component/RigidbodyComponent.h"
#include "object/physics/CharacterController.h"
#include "object/physics/Rigidbody.h"
#endif

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <map>
#include <set>
#include <unordered_map>

using namespace jasperengine;
using namespace jasperengine::nav;

namespace {

    constexpr float PI = 3.14159265358979323846f;
    // carving work per fixed tick (spec R28: 1 ms of tile-cache work per tick)
    constexpr double CARVE_BUDGET_MS = 1.0;
    // arrival never closer than this (spec R12)
    constexpr float MIN_STOPPING_DISTANCE = 0.01f;
    // auto braking: the final approach closes 1/8 of what is left per tick, and arrives within 5 mm
    constexpr float BRAKE_FINAL_TICKS = 8.0f;
    constexpr float ARRIVAL_TOLERANCE = 0.005f;
    // a grid agent waiting this long for an occupied cell plans around it
    constexpr float GRID_WAIT_SECONDS = 0.5f;

    enum class EventType : uint8_t {
        DestinationReached,
        PathFailed,
        LinkStart,
        LinkEnd,
        BakeCompleted
    };

    struct Event {
        Entity entity;
        EventType type;
    };

    void warnOnce(std::set<std::string>& warned, const std::string& key, const std::string& message) {
        if (warned.insert(key).second)
            Log::warn("%s", message.c_str());
    }

    // the rotation about `up` that turns `forward` towards `direction`, at most maxDegrees
    Quaternion turnTowards(const Quaternion& current, const Vector3& up, const Vector3& forwardAxis, const Vector3& direction,
                           float maxDegrees) {
        const Vector3 forward = current * forwardAxis;
        const Vector3 f = (forward - up * forward.dotProduct(up));
        const Vector3 d = (direction - up * direction.dotProduct(up));
        if (f.length() < 1e-6f || d.length() < 1e-6f)
            return current;
        const Vector3 fn = f.normalized(), dn = d.normalized();
        float angle = std::atan2(fn.crossProduct(dn).dotProduct(up), std::clamp(fn.dotProduct(dn), -1.0f, 1.0f));
        const float maxRadians = maxDegrees * PI / 180.0f;
        angle = std::clamp(angle, -maxRadians, maxRadians);
        const float half = angle * 0.5f;
        const Quaternion turn(std::cos(half), up.x * std::sin(half), up.y * std::sin(half), up.z * std::sin(half));
        return turn * current;
    }

}

// ---------------------------------------------------------------- runtime state

struct NavigationSystem::Runtime {
    struct Surface {
        Entity entity = NULL_ENTITY;
        std::unique_ptr<NavMesh> mesh;
        std::string loadedPath;
        bool loaded = false;
        std::string loadError;
        NavFrame frame;
        // the running bake: its tiles, the tiles it removes, what it collected
        std::unique_ptr<NavBake> bake;
        NavBakeMode bakeMode = NavBakeMode::DIRTY;
        std::vector<NavTileCoord> bakeRemoved;
        bool bakeReplacesAll = false;
        NavCollectStats bakeStats;
        std::chrono::steady_clock::time_point bakeStarted;
        std::unique_ptr<NavBakeResult> lastResult;
        int dirtyTiles = -1;
        // S54 R17: each tile's last build in this session, ms by (y, x); cleared on a load
        std::map<std::pair<int, int>, double> tileMs;
        // the crowd of its agents and non-carving obstacles, made for one Detour mesh
        std::unique_ptr<NavCrowd> crowd;
        void* crowdMesh = nullptr;
        int crowdCapacity = 0;
        bool crowdGrow = false;
        // ---- S55 queries: the crowd ran out of query filters once (one warning per crowd)
        bool warnedFilterFallback = false;
    };

    struct Agent {
        Entity entity = NULL_ENTITY;
        Entity surface = NULL_ENTITY;
        bool onGrid = false;
        int crowdIndex = -1;
        NavCrowdAgentParams params;
        bool placed = false;
        float planeZ = 0.0f;
        // the poses of its last two fixed ticks (Transform driver interpolation) and what was
        // written last (a Transform moved since by someone else is followed)
        Vector3 previousPosition, currentPosition;
        Quaternion previousRotation, currentRotation;
        Vector3 writtenPosition;
        Quaternion writtenRotation;
        bool hasPose = false;
        // destination and its events
        bool requested = false;
        Vector3 destination;
        bool arrived = false;
        bool failed = false;
        bool wasPending = false;
        bool warnedOffMesh = false;
        // its own traversal of an off-mesh link (nav space)
        bool onLink = false;
        bool linkByDetour = false;
        bool linkWaiting = false;
        Vector3 linkFrom, linkStart, linkEnd;
        float linkTravelled = 0.0f;
        Entity link = NULL_ENTITY;
        // grid movement
        std::vector<GridCell> gridCells;
        size_t gridNext = 0;
        bool gridPending = false;
        float gridWait = 0.0f;
        Vector3 gridVelocity;
        uint64_t gridRevision = 0;
        GridCell gridGoal;
        // ---- S55 queries: its own area costs (0: the project's), runtime only
        float areaCosts[AREA_COUNT] = {};
    };

    struct Obstacle {
        Entity entity = NULL_ENTITY;
        // the crowd slot it has in each surface's crowd
        std::map<Entity, int> crowdSlots;
        // its tile-cache obstacle in each carving surface
        std::map<Entity, uint32_t> carves;
        Vector3 carvedPosition;
        Quaternion carvedRotation;
        Vector3 lastPosition;
        // S55 R30: from its transform, per fixed tick
        Vector3 velocity;
        float stationaryTime = 0.0f;
        bool carved = false;
        bool hasLast = false;
    };

    struct Grid {
        GridReservations reservations;
        GridSolverPool pool;
        // the agent standing in each cell (0: none)
        std::vector<uint32_t> occupancy;
        uint64_t mapRevision = 0;
        std::string loadedPath;
        bool loaded = false;
        // the source's content hash at the last automatic rebuild (0: none yet)
        uint64_t sourceHash = 0;
    };

    struct GridRequest {
        Entity agent;
        Entity grid;
        nav::GridRequest request;
    };

    std::map<Entity, Surface> surfaces;
    std::map<Entity, Agent> agents;
    std::map<Entity, Obstacle> obstacles;
    std::map<Entity, Grid> grids;
    std::vector<GridRequest> gridRequests;
    std::vector<Event> events;
    std::set<std::string> warned;
    uint64_t settingsVersion = 0;
    std::vector<NavLinkEntry> linkEntries;
    std::vector<int> detourLinks;
    bool inFixedUpdate = false;

    // the fixed tick's steps (spec D2)
    void syncSurfaces(NavigationSystem& system, Scene* scene);
    bool ensureCrowd(Scene* scene, Surface& surface);
    Entity pickSurface(NavigationSystem& system, Scene* scene, Entity entity, const NavMeshAgentComponent& component, const Vector3& position);
    void syncAgents(NavigationSystem& system, Scene* scene);
    void syncObstacles(NavigationSystem& system, Scene* scene, float dt);
    void solveGrids(NavigationSystem& system, Scene* scene, uint32_t tick);
    void updateCarving();
    void stepCrowds(Scene* scene, float dt);
    void writeBack(NavigationSystem& system, Scene* scene, float dt, uint32_t tick);
    void moveGridAgent(NavigationSystem& system, Scene* scene, Agent& agent, NavMeshAgentComponent& component, float dt, uint32_t tick);
    void dispatch(Scene* scene);
    std::vector<Entity> sortedAgents(Scene* scene) const;
    // S55 step 1b: the agents whose path an area change touches ask again (R15)
    void replanAreaChanges(NavigationSystem& system, Scene* scene);
    std::vector<int> replanned;
    // ---- S55 queries: an agent whose area mask and costs found no free crowd filter
    void warnFilterFallback(Scene* scene, Surface& surface, Entity entity, int crowdIndex);
};

NavigationSystem::NavigationSystem(Scene* scene): SubSystem(scene) {
    signature.set(scene->getComponentId<Transform>());
    declareHookType(scene->getComponentId<NavMeshSurfaceComponent>());
    declareHookType(scene->getComponentId<NavMeshAgentComponent>());
    declareHookType(scene->getComponentId<NavMeshObstacleComponent>());
    declareHookType(scene->getComponentId<NavGridComponent>());
}

NavigationSystem::~NavigationSystem() = default;

NavigationSystem::Runtime& NavigationSystem::rt() {
    if (!runtime)
        runtime = std::make_unique<Runtime>();
    return *runtime;
}

bool NavigationSystem::hasNavigationComponents() const {
    return scene->getComponentArray<NavMeshSurfaceComponent>()->size() > 0 ||
           scene->getComponentArray<NavMeshAgentComponent>()->size() > 0 ||
           scene->getComponentArray<NavMeshObstacleComponent>()->size() > 0 ||
           scene->getComponentArray<NavGridComponent>()->size() > 0;
}

void NavigationSystem::load() {
}

void NavigationSystem::destroy() {
    // S55: script volumes end with the system (Stop)
    areas.reset();
    if (!runtime)
        return;
    for (auto& entry : runtime->surfaces) {
        if (entry.second.bake)
            entry.second.bake->cancel();
    }
    runtime.reset();
}

void NavigationSystem::draw() {
}

void NavigationSystem::setKeepBakeData(bool keep) {
    if (keep == keepBakeData)
        return;
    keepBakeData = keep;
    // navmeshes read without (or with) their tile records read their assets again
    if (!runtime)
        return;
    for (auto& entry : runtime->surfaces) {
        if (!entry.second.bake && !entry.second.loadedPath.empty())
            entry.second.loaded = false;
    }
}

void NavigationSystem::setBakeInBackground(bool background) {
    bakeInBackground = background;
}

void NavigationSystem::onComponentAdded(Entity entity, ComponentId componentId) {
    (void)entity;
    (void)componentId;
}

void NavigationSystem::onComponentRemoved(Entity entity, ComponentId componentId) {
    if (!runtime)
        return;
    Runtime& r = *runtime;
    if (componentId == scene->getComponentId<NavMeshSurfaceComponent>()) {
        auto found = r.surfaces.find(entity);
        if (found != r.surfaces.end()) {
            if (found->second.bake)
                found->second.bake->cancel();
            // agents on it pick a surface again
            for (auto& agent : r.agents) {
                if (agent.second.surface == entity) {
                    agent.second.surface = NULL_ENTITY;
                    agent.second.crowdIndex = -1;
                    agent.second.placed = false;
                }
            }
            for (auto& obstacle : r.obstacles) {
                obstacle.second.crowdSlots.erase(entity);
                obstacle.second.carves.erase(entity);
            }
            r.surfaces.erase(found);
        }
    } else if (componentId == scene->getComponentId<NavMeshAgentComponent>()) {
        auto found = r.agents.find(entity);
        if (found != r.agents.end()) {
            Runtime::Agent& agent = found->second;
            auto surface = r.surfaces.find(agent.surface);
            if (surface != r.surfaces.end() && surface->second.crowd && agent.crowdIndex >= 0)
                surface->second.crowd->removeAgent(agent.crowdIndex);
            auto grid = r.grids.find(agent.surface);
            if (grid != r.grids.end())
                grid->second.reservations.release((uint32_t)entity);
            r.agents.erase(found);
        }
    } else if (componentId == scene->getComponentId<NavMeshObstacleComponent>()) {
        auto found = r.obstacles.find(entity);
        if (found != r.obstacles.end()) {
            for (auto& slot : found->second.crowdSlots) {
                auto surface = r.surfaces.find(slot.first);
                if (surface != r.surfaces.end() && surface->second.crowd)
                    surface->second.crowd->removeAgent(slot.second);
            }
            for (auto& carve : found->second.carves) {
                auto surface = r.surfaces.find(carve.first);
                if (surface != r.surfaces.end() && surface->second.mesh)
                    surface->second.mesh->removeObstacle(carve.second);
            }
            r.obstacles.erase(found);
        }
    } else if (componentId == scene->getComponentId<NavGridComponent>()) {
        r.grids.erase(entity);
        for (auto& agent : r.agents) {
            if (agent.second.surface == entity) {
                agent.second.surface = NULL_ENTITY;
                agent.second.onGrid = false;
                agent.second.placed = false;
            }
        }
    }
}

// ---------------------------------------------------------------- surfaces and navmesh data

namespace {

    NavMeshSurfaceComponent* surfaceComponent(Scene* scene, Entity entity) {
        return scene->findComponent<NavMeshSurfaceComponent>(entity);
    }

    bool readAsset(const std::string& path, std::vector<uint8_t>& bytes, std::string& error) {
        Data data;
        if (data.open(path.c_str()) != FileErrors::FILEDATA_OK) {
            error = "cannot read '" + path + "'";
            return false;
        }
        const unsigned char* begin = data.getMemPtr();
        bytes.assign(begin, begin + data.length());
        return true;
    }

}

const NavMesh* NavigationSystem::getNavMesh(Entity surface) {
    NavMeshSurfaceComponent* component = surfaceComponent(scene, surface);
    if (!component)
        return nullptr;
    Runtime::Surface& state = rt().surfaces[surface];
    state.entity = surface;
    if (!state.loaded || state.loadedPath != component->navMeshData.path) {
        std::string error;
        reloadNavMesh(surface, error);
    }
    return state.mesh && state.mesh->isValid() ? state.mesh.get() : nullptr;
}

bool NavigationSystem::reloadNavMesh(Entity surface, std::string& error) {
    NavMeshSurfaceComponent* component = surfaceComponent(scene, surface);
    if (!component) {
        error = "the GameObject has no NavMeshSurface";
        return false;
    }
    Runtime::Surface& state = rt().surfaces[surface];
    state.entity = surface;
    state.loaded = true;
    state.loadedPath = component->navMeshData.path;
    state.loadError.clear();
    state.dirtyTiles = -1;
    state.tileMs.clear();
    if (component->navMeshData.path.empty()) {
        state.mesh.reset();
        return true;
    }
    std::vector<uint8_t> bytes;
    NavMeshData data;
    if (!readAsset(component->navMeshData.path, bytes, error) || !NavMeshFile::read(bytes.data(), bytes.size(), data, error)) {
        state.loadError = error;
        state.mesh.reset();
        Log::error("NavMeshSurface '%s': %s; it reports Not baked", scene->getEntityName(surface).c_str(), error.c_str());
        return false;
    }
    auto mesh = std::make_unique<NavMesh>();
    if (!mesh->create(std::move(data), keepBakeData, error)) {
        state.loadError = error;
        state.mesh.reset();
        Log::error("NavMeshSurface '%s': %s; it reports Not baked", scene->getEntityName(surface).c_str(), error.c_str());
        return false;
    }
    state.mesh = std::move(mesh);
    onAreaMeshReplaced(surface);
    return true;
}

void NavigationSystem::adoptNavMeshAsset(Entity surface) {
    NavMeshSurfaceComponent* component = surfaceComponent(scene, surface);
    if (!component || !runtime)
        return;
    auto found = runtime->surfaces.find(surface);
    if (found == runtime->surfaces.end() || !found->second.mesh)
        return;
    found->second.loaded = true;
    found->second.loadedPath = component->navMeshData.path;
    found->second.loadError.clear();
}

bool NavigationSystem::setNavMeshData(Entity surface, NavMeshData data, std::string& error) {
    if (!surfaceComponent(scene, surface)) {
        error = "the GameObject has no NavMeshSurface";
        return false;
    }
    Runtime::Surface& state = rt().surfaces[surface];
    state.entity = surface;
    auto mesh = std::make_unique<NavMesh>();
    if (!mesh->create(std::move(data), keepBakeData, error))
        return false;
    state.mesh = std::move(mesh);
    state.loaded = true;
    state.loadedPath = surfaceComponent(scene, surface)->navMeshData.path;
    state.loadError.clear();
    state.dirtyTiles = -1;
    state.tileMs.clear();
    onAreaMeshReplaced(surface);
    return true;
}

std::vector<uint8_t> NavigationSystem::getNavMeshFile(Entity surface) {
    if (!getNavMesh(surface))
        return {};
    const NavMesh& mesh = *rt().surfaces[surface].mesh;
    if (!mesh.getData())
        return {};
    return NavMeshFile::write(*mesh.getData());
}

NavSurfaceStatus NavigationSystem::getSurfaceStatus(Entity surface) {
    NavSurfaceStatus status;
    const NavMesh* mesh = getNavMesh(surface);
    if (!surfaceComponent(scene, surface))
        return status;
    Runtime::Surface& state = rt().surfaces[surface];
    status.loadError = state.loadError;
    if (mesh) {
        status.tiles = mesh->getTileCount();
        status.polys = mesh->getPolyCount();
        status.bytes = mesh->getMemoryBytes();
        status.carving = mesh->isCarving();
        status.state = state.dirtyTiles > 0 ? NavBakeState::DIRTY : NavBakeState::BAKED;
    }
    status.dirtyTiles = state.dirtyTiles;
    if (state.bake) {
        status.state = NavBakeState::BAKING;
        status.progress = state.bake->getProgress();
    }
    if (state.crowd) {
        status.crowdAgents = state.crowd->getAgentCount();
        status.crowdCapacity = state.crowd->getCapacity();
        status.crowdFilters = state.crowd->getFilterCount();
        status.crowdFilterOverflow = state.crowd->getFilterOverflowCount();
    }
    return status;
}

int NavigationSystem::countDirtyTiles(Entity surface, std::string* error) {
    const InputReport report = inspectInputs(surface);
    if (error && report.dirtyTiles < 0)
        *error = report.error;
    return report.dirtyTiles;
}

NavigationSystem::InputReport NavigationSystem::inspectInputs(Entity surface, const NavCollectOptions& options, bool keepInput) {
    InputReport report;
    NavMeshSurfaceComponent* component = surfaceComponent(scene, surface);
    if (!component) {
        report.error = "the GameObject has no NavMeshSurface";
        report.errorCode = NavBakeError::NoSurface;
        return report;
    }
    std::shared_ptr<NavBakeInput> input = NavBakeCollector::collect(scene, surface, report.error, nullptr, options, &report.errorCode);
    Runtime::Surface& state = rt().surfaces[surface];
    const NavMesh* mesh = getNavMesh(surface);
    if (!input) {
        state.dirtyTiles = -1;
        return report;
    }
    report.tilesWide = input->tilesWide();
    report.tilesHigh = input->tilesHigh();
    report.tileSize = input->config.tileSize;
    report.nonEmptyTiles = (int)input->nonEmptyTiles.size();
    report.cellSize = input->config.cellSize;
    report.agentRadius = input->config.agentRadius;
    // a tile is dirty when its input hash differs from the baked one, or it is baked and now empty
    std::map<std::pair<int, int>, uint64_t> baked;
    bool configChanged = true;
    if (mesh) {
        configChanged = mesh->getHeader().config.hash() != input->config.hash();
        if (const NavMeshData* data = mesh->getData()) {
            for (const NavTileBlob& blob : data->tiles)
                baked[{blob.y, blob.x}] = blob.hash;
        } else {
            // a game's mesh keeps no records: read the hashes back from the asset
            std::vector<uint8_t> bytes;
            NavMeshData file;
            std::string ignored;
            if (readAsset(component->navMeshData.path, bytes, ignored) && NavMeshFile::read(bytes.data(), bytes.size(), file, ignored)) {
                for (const NavTileBlob& blob : file.tiles)
                    baked[{blob.y, blob.x}] = blob.hash;
            }
        }
    }
    std::set<std::pair<int, int>> seen;
    for (uint32_t index : input->nonEmptyTiles) {
        const NavTileCoord coord = input->tileCoord(index);
        seen.insert({coord.y, coord.x});
        auto found = baked.find({coord.y, coord.x});
        if (!mesh || configChanged || found == baked.end() || found->second != input->tileHashes[index])
            report.dirty.push_back(coord);
    }
    if (mesh) {
        for (const auto& entry : baked) {
            if (!seen.count(entry.first))
                report.dirty.push_back(NavTileCoord{entry.first.second, entry.first.first});
        }
    }
    std::sort(report.dirty.begin(), report.dirty.end());
    report.dirty.erase(std::unique(report.dirty.begin(), report.dirty.end()), report.dirty.end());
    report.dirtyTiles = (int)report.dirty.size();
    state.dirtyTiles = report.dirtyTiles;
    if (keepInput)
        report.input = std::move(input);
    return report;
}

namespace {

    // a bake that could not start: its code, and (when the surface exists and no bake runs) the
    // surface's last result, so the report and the findings see it (S54 R16)
    bool refuseBake(std::unique_ptr<NavBakeResult>* last, NavBakeError why, const std::string& error, NavBakeError* code,
                    NavCollectStats* stats = nullptr) {
        if (code)
            *code = why;
        if (last) {
            *last = std::make_unique<NavBakeResult>();
            (*last)->error = error;
            (*last)->errorCode = why;
            (*last)->hint = bakeErrorHint(why);
            if (stats) {
                (*last)->sources = std::move(*stats);
                (*last)->warnings = (*last)->sources.warnings;
            }
        }
        return false;
    }

}

bool NavigationSystem::bake(Entity surface, NavBakeMode mode, std::string& error, NavBakeError* code) {
    if (code)
        *code = NavBakeError::None;
    NavMeshSurfaceComponent* component = surfaceComponent(scene, surface);
    if (!component) {
        error = "the GameObject has no NavMeshSurface";
        return refuseBake(nullptr, NavBakeError::NoSurface, error, code);
    }
    Runtime::Surface& state = rt().surfaces[surface];
    state.entity = surface;
    if (!isNavMeshAvailable()) {
        error = "navmesh support is not in this build (JASPERENGINE_NAVIGATION is off)";
        Log::error("Navigation: %s", error.c_str());
        return refuseBake(&state.lastResult, NavBakeError::Unavailable, error, code);
    }
    getNavMesh(surface);
    if (state.bake) {
        error = "a bake of this surface is running";
        return refuseBake(nullptr, NavBakeError::Busy, error, code);
    }
    if (mode == NavBakeMode::CLEAR) {
        state.mesh.reset();
        state.loaded = true;
        state.loadedPath = component->navMeshData.path;
        state.dirtyTiles = -1;
        state.tileMs.clear();
        state.lastResult = std::make_unique<NavBakeResult>();
        state.lastResult->ok = true;
        return true;
    }

    JE_PROFILE_SCOPE("Nav.Bake");
    NavCollectStats stats;
    NavBakeError collectError = NavBakeError::None;
    std::shared_ptr<NavBakeInput> input = NavBakeCollector::collect(scene, surface, error, &stats, {}, &collectError);
    if (!input)
        return refuseBake(&state.lastResult, collectError, error, code, &stats);

    // Dirty: the tiles whose hashes changed (a different configuration rebuilds all); Full: all
    std::vector<uint32_t> tiles;
    std::vector<NavTileCoord> removed;
    bool replaceAll = mode == NavBakeMode::FULL || !state.mesh || !state.mesh->getData() ||
                      state.mesh->getHeader().config.hash() != input->config.hash();
    if (!replaceAll && state.mesh->getHeader().linkAreaCosts != input->linkAreaCosts)
        replaceAll = true;
    if (replaceAll) {
        tiles = input->nonEmptyTiles;
    } else {
        std::map<std::pair<int, int>, uint64_t> baked;
        for (const NavTileBlob& blob : state.mesh->getData()->tiles)
            baked[{blob.y, blob.x}] = blob.hash;
        std::set<std::pair<int, int>> seen;
        for (uint32_t index : input->nonEmptyTiles) {
            const NavTileCoord coord = input->tileCoord(index);
            seen.insert({coord.y, coord.x});
            auto found = baked.find({coord.y, coord.x});
            if (found == baked.end() || found->second != input->tileHashes[index])
                tiles.push_back(index);
        }
        for (const auto& entry : baked) {
            if (!seen.count(entry.first))
                removed.push_back(NavTileCoord{entry.first.second, entry.first.first});
        }
    }
    NavBakeOptions options;
    options.lane = bakeInBackground ? jobs::Lane::Background : jobs::Lane::Normal;
    state.bake = std::make_unique<NavBake>(input, tiles, options);
    state.bakeMode = mode;
    state.bakeRemoved = std::move(removed);
    state.bakeReplacesAll = replaceAll;
    state.bakeStats = std::move(stats);
    state.bakeStarted = std::chrono::steady_clock::now();
    return true;
}

bool NavigationSystem::updateTiles(Entity surface, const AABB& worldBounds, std::string& error, NavBakeError* code) {
    if (code)
        *code = NavBakeError::None;
    NavMeshSurfaceComponent* component = surfaceComponent(scene, surface);
    if (!component) {
        error = "the GameObject has no NavMeshSurface";
        return refuseBake(nullptr, NavBakeError::NoSurface, error, code);
    }
    Runtime::Surface& state = rt().surfaces[surface];
    state.entity = surface;
    getNavMesh(surface);
    if (state.bake) {
        error = "a bake of this surface is running";
        return refuseBake(nullptr, NavBakeError::Busy, error, code);
    }
    // without the tile records (a game's mesh) the asset is read again with them
    if (state.mesh && !state.mesh->getData()) {
        const bool keep = keepBakeData;
        keepBakeData = true;
        reloadNavMesh(surface, error);
        keepBakeData = keep;
    }
    NavCollectStats stats;
    NavBakeError collectError = NavBakeError::None;
    std::shared_ptr<NavBakeInput> input = NavBakeCollector::collect(scene, surface, error, &stats, {}, &collectError);
    if (!input)
        return refuseBake(&state.lastResult, collectError, error, code, &stats);
    if (!state.mesh || !state.mesh->getData() || state.mesh->getHeader().config.hash() != input->config.hash())
        return bake(surface, NavBakeMode::FULL, error, code);

    // the tiles whose bounds (with their border) meet the box, in nav space
    const NavFrame frame = NavBakeCollector::frameOf(scene, surface);
    Vector3 lo(std::numeric_limits<float>::max()), hi(-std::numeric_limits<float>::max());
    for (int c = 0; c < 8; c++) {
        const Vector3 corner((c & 1) ? worldBounds.getMaximum().x : worldBounds.getMinimum().x,
                             (c & 2) ? worldBounds.getMaximum().y : worldBounds.getMinimum().y,
                             (c & 4) ? worldBounds.getMaximum().z : worldBounds.getMinimum().z);
        const Vector3 nav = frame.toNav(corner);
        lo.makeFloor(nav);
        hi.makeCeil(nav);
    }
    const float border = input->config.borderWorldSize();
    std::vector<uint32_t> tiles;
    std::vector<NavTileCoord> removed;
    for (int index = 0; index < input->tileCount(); index++) {
        const NavTileCoord coord = input->tileCoord((uint32_t)index);
        float tmin[3], tmax[3];
        input->tileBounds(coord.x, coord.y, tmin, tmax);
        if (tmax[0] + border < lo.x || tmin[0] - border > hi.x || tmax[2] + border < lo.z || tmin[2] - border > hi.z)
            continue;
        if (input->tileHashes[(size_t)index] != 0)
            tiles.push_back((uint32_t)index);
        else
            removed.push_back(coord);
    }
    NavBakeOptions options;
    options.lane = bakeInBackground ? jobs::Lane::Background : jobs::Lane::Normal;
    state.bake = std::make_unique<NavBake>(input, tiles, options);
    state.bakeMode = NavBakeMode::DIRTY;
    state.bakeRemoved = std::move(removed);
    state.bakeReplacesAll = false;
    state.bakeStats = std::move(stats);
    state.bakeStarted = std::chrono::steady_clock::now();
    return true;
}

NavBakeResult NavigationSystem::bakeNow(Entity surface, NavBakeMode mode) {
    NavBakeResult result;
    std::string error;
    NavBakeError code = NavBakeError::None;
    if (!bake(surface, mode, error, &code)) {
        result.error = error;
        result.errorCode = code;
        result.hint = bakeErrorHint(code);
        if (code != NavBakeError::Busy && runtime && runtime->surfaces[surface].lastResult)
            result = *runtime->surfaces[surface].lastResult;
        return result;
    }
    Runtime::Surface& state = rt().surfaces[surface];
    if (state.bake) {
        while (state.bake->isTimeSliced() && !state.bake->isDone())
            state.bake->pump(1000.0);
        state.bake->wait();
    }
    processBakes(0.0);
    // the report as it landed: its island fields fill on the first getLastBakeResult (R8)
    if (state.lastResult)
        result = *state.lastResult;
    return result;
}

bool NavigationSystem::cancelBake(Entity surface) {
    if (!runtime)
        return false;
    auto found = runtime->surfaces.find(surface);
    if (found == runtime->surfaces.end() || !found->second.bake)
        return false;
    found->second.bake->cancel();
    return true;
}

bool NavigationSystem::isBaking(Entity surface) const {
    if (!runtime)
        return false;
    auto found = runtime->surfaces.find(surface);
    return found != runtime->surfaces.end() && found->second.bake != nullptr;
}

bool NavigationSystem::isAnyBaking() const {
    if (!runtime)
        return false;
    for (const auto& entry : runtime->surfaces) {
        if (entry.second.bake)
            return true;
    }
    return false;
}

const NavBakeResult* NavigationSystem::getLastBakeResult(Entity surface) {
    if (!runtime)
        return nullptr;
    auto found = runtime->surfaces.find(surface);
    if (found == runtime->surfaces.end() || !found->second.lastResult)
        return nullptr;
    NavBakeResult& result = *found->second.lastResult;
    // the island fields on first read, never inside the landing (R8, R16)
    if (result.ok && result.islandCount < 0 && found->second.mesh && found->second.mesh->isValid()) {
        const NavCoverage coverage = getCoverage(surface);
        result.walkableArea = coverage.walkableArea;
        result.islandCount = coverage.islandCount;
        result.islands = getIslands(surface);
        std::stable_sort(result.islands.begin(), result.islands.end(), [](const NavIsland& a, const NavIsland& b) { return a.area > b.area; });
        if (result.islands.size() > 16)
            result.islands.resize(16);
    }
    return &result;
}

void NavigationSystem::processBakes(double budgetMs) {
    if (!runtime)
        return;
    Runtime& r = *runtime;
    for (auto& entry : r.surfaces) {
        Runtime::Surface& state = entry.second;
        if (!state.bake)
            continue;
        if (state.bake->isTimeSliced() && !state.bake->isDone() && budgetMs > 0.0) {
            const auto sliceStart = std::chrono::steady_clock::now();
            state.bake->pump(budgetMs);
            maxBakeSliceMs = std::max(maxBakeSliceMs, std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - sliceStart).count());
            for (const auto& stage : state.bake->getStageMaxMs())
                bakeStageMaxMs[stage.first] = std::max(bakeStageMaxMs[stage.first], stage.second);
            if (state.bake->getSlowestStepMs() > slowestBakeStepMs) {
                slowestBakeStepMs = state.bake->getSlowestStepMs();
                slowestBakeStep = state.bake->getSlowestStep();
            }
        }
        if (!state.bake->isDone())
            continue;

        std::unique_ptr<NavBake> bake = std::move(state.bake);
        auto result = std::make_unique<NavBakeResult>();
        result->warnings = state.bakeStats.warnings;
        result->sources = std::move(state.bakeStats);
        state.bakeStats = NavCollectStats();
        result->tilesTotal = bake->getTilesTotal();
        result->milliseconds = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - state.bakeStarted).count();
        auto failWith = [&](NavBakeError code) {
            result->errorCode = code;
            result->hint = bakeErrorHint(code);
        };
        if (bake->isCancelled()) {
            // the previous navmesh stays, tiles and hashes alike (spec R4)
            result->cancelled = true;
            result->error = "cancelled";
            failWith(NavBakeError::Cancelled);
            state.lastResult = std::move(result);
            continue;
        }

        // the tiles land in tile-index order (spec R40a)
        std::vector<NavTileBlob> blobs;
        std::vector<NavTileCoord> coords;
        bool failed = false;
        for (NavTileBuildResult& tile : bake->getResults()) {
            if (!tile.ok) {
                result->error = tile.error;
                failed = true;
                break;
            }
            coords.push_back(tile.coord);
            result->tilesBuilt++;
            result->polys += tile.polyCount;
            NavTileReport report;
            report.x = tile.coord.x;
            report.y = tile.coord.y;
            report.polys = tile.polyCount;
            report.milliseconds = tile.milliseconds;
            for (NavTileBlob& blob : tile.blobs) {
                report.bytes += blob.data.size();
                blobs.push_back(std::move(blob));
            }
            result->tiles.push_back(report);
        }
        if (failed) {
            failWith(NavBakeError::TileFailed);
            result->tiles.clear();
            Log::error("NavMeshSurface '%s' bake failed: %s", scene->getEntityName(entry.first).c_str(), result->error.c_str());
            state.lastResult = std::move(result);
            continue;
        }
        for (const NavTileCoord& coord : state.bakeRemoved)
            coords.push_back(coord);

        const std::shared_ptr<const NavBakeInput>& input = bake->getInput();
        std::string error;
        bool ok = false;
        const bool newMesh = state.bakeReplacesAll || !state.mesh || !state.mesh->getData();
        if (newMesh) {
            NavMeshData data;
            data.header.agent = getSettings().agentTypeOrDefault(surfaceComponent(scene, entry.first)
                                                                      ? surfaceComponent(scene, entry.first)->agentType : 0);
            data.header.areaTableHash = hashAreaTable(getSettings());
            data.header.config = input->config;
            data.header.maxTiles = std::max(1, input->tileCount());
            data.header.linkAreaCosts = input->linkAreaCosts;
            data.tiles = std::move(blobs);
            auto mesh = std::make_unique<NavMesh>();
            // a baked mesh keeps its records: the editor saves them, a game's updateTiles follows
            ok = mesh->create(std::move(data), true, error);
            if (ok)
                state.mesh = std::move(mesh);
        } else {
            ok = state.mesh->replaceTiles(coords, std::move(blobs), error);
        }
        if (!ok) {
            result->error = error;
            failWith(NavBakeError::NavMeshFailed);
            result->tiles.clear();
            Log::error("NavMeshSurface '%s' bake failed: %s", scene->getEntityName(entry.first).c_str(), error.c_str());
            state.lastResult = std::move(result);
            continue;
        }
        // the tiles' build times persist per surface (R17): a full bake replaces them all, an
        // incremental one its own and drops the removed tiles
        if (newMesh)
            state.tileMs.clear();
        for (const NavTileCoord& coord : state.bakeRemoved)
            state.tileMs.erase({coord.y, coord.x});
        for (const NavTileReport& tile : result->tiles)
            state.tileMs[{tile.y, tile.x}] = tile.milliseconds;
        if (state.mesh)
            result->polys = state.mesh->getPolyCount();
        result->ok = true;
        state.dirtyTiles = 0;
        state.loaded = true;
        if (NavMeshSurfaceComponent* component = surfaceComponent(scene, entry.first))
            state.loadedPath = component->navMeshData.path;
        // S55 R14: a full bake's new navmesh gets the overrides before anything queries it
        if (newMesh)
            onAreaMeshReplaced(entry.first);
        state.lastResult = std::move(result);
        r.events.push_back(Event{entry.first, EventType::BakeCompleted});
    }
    // outside the fixed tick (the editor, a script's wait) the events fire now
    if (!r.inFixedUpdate)
        r.dispatch(scene);
}

// ---------------------------------------------------------------- queries

namespace {

    struct SurfaceQuery {
        Entity entity = NULL_ENTITY;
        const NavMesh* mesh = nullptr;
        NavFrame frame;
        Vector3 halfExtents;
    };

    bool activeSurface(Scene* scene, Entity entity, const NavMeshSurfaceComponent& component) {
        return component.enabled && scene->isEntityActiveInHierarchy(entity);
    }

}

namespace {

    // what a query needs of a surface: its mesh, frame and the search box of its agent type;
    // why not (S54 R3 steps 1-2: not an enabled, active surface, or not baked)
    bool makeQuery(NavigationSystem& system, Scene* scene, Entity entity, SurfaceQuery& out, PathFailure* why = nullptr) {
        const NavMeshSurfaceComponent* component = scene->findComponent<NavMeshSurfaceComponent>(entity);
        if (!component || !activeSurface(scene, entity, *component)) {
            if (why)
                *why = PathFailure::NoSurface;
            return false;
        }
        const NavMesh* mesh = system.getNavMesh(entity);
        if (!mesh) {
            if (why)
                *why = PathFailure::NotBaked;
            return false;
        }
        out.entity = entity;
        out.mesh = mesh;
        out.frame = NavBakeCollector::frameOf(scene, entity);
        const NavBuildConfig& config = mesh->getHeader().config;
        const float radius = std::max(config.agentRadius, config.cellSize) * 2.0f;
        const float height = config.plane == NavPlane::XY ? config.cellSize * 2.0f : std::max(config.agentHeight, config.cellHeight * 4.0f);
        out.halfExtents = Vector3(radius, height, radius);
        return true;
    }

}

namespace {

    std::vector<Entity> surfacesOf(Scene* scene, int agentType) {
        std::vector<Entity> entities;
        auto surfaces = scene->getComponentArray<NavMeshSurfaceComponent>();
        for (size_t i = 0; i < surfaces->size(); i++) {
            const NavMeshSurfaceComponent& component = surfaces->getComponentFromIndex(i);
            if (component.agentType == agentType)
                entities.push_back(surfaces->getEntity(i));
        }
        std::sort(entities.begin(), entities.end());
        return entities;
    }

    NavHit hitToWorld(const NavHit& hit, const NavFrame& frame, float planeZ) {
        NavHit out = hit;
        out.position = frame.toWorld(hit.position, planeZ);
        out.normal = frame.directionToWorld(hit.normal);
        return out;
    }

}

namespace {

    // a path that never reached a navmesh: from the requested start to the requested end
    NavPath unanswered(PathFailure failure, const Vector3& from, const Vector3& to) {
        NavPath path;
        path.failure = failure;
        path.start = path.closestPoint = from;
        path.end = to;
        path.gap = from.distance(to);
        return path;
    }

    // R5: the lower the better among non-Complete answers
    int failureRank(PathFailure failure) {
        switch (failure) {
            case PathFailure::None: return 0;
            case PathFailure::OutOfNodes: return 1;
            case PathFailure::BlockedByArea: return 2;
            case PathFailure::Disconnected: return 3;
            case PathFailure::EndFiltered: return 4;
            case PathFailure::EndOffMesh: return 5;
            case PathFailure::StartFiltered: return 6;
            case PathFailure::StartOffMesh: return 7;
            case PathFailure::NotBaked: return 8;
            case PathFailure::NoSurface: return 9;
        }
        return 9;
    }

    // whether `a` answers better than `b` (equal ranks: the smaller gap; else the earlier surface)
    bool betterAnswer(const NavPath& a, const NavPath& b) {
        const int ra = failureRank(a.failure), rb = failureRank(b.failure);
        if (ra != rb)
            return ra < rb;
        return a.gap < b.gap;
    }

}

NavPath NavigationSystem::calculatePathOn(Entity surface, const Vector3& from, const Vector3& to, uint16_t areaMask) {
    QueryFilter filter;
    filter.areaMask = areaMask;
    return calculatePathOn(surface, from, to, filter);
}

NavPath NavigationSystem::calculatePathOn(Entity surface, const Vector3& from, const Vector3& to, const QueryFilter& filter,
                                          const PathOptions& options) {
    SurfaceQuery q;
    PathFailure why = PathFailure::NoSurface;
    if (!makeQuery(*this, scene, surface, q, &why))
        return unanswered(why, from, to);
    JE_PROFILE_SCOPE("Nav.Query");
    NavPath path = q.mesh->findPath(q.frame.toNav(from), q.frame.toNav(to), NavFilter::fromQuery(filter), q.halfExtents, options);
    const float planeZ = q.frame.planeZOf(from);
    path.length = 0.0f;
    for (size_t i = 0; i < path.corners.size(); i++) {
        path.corners[i] = q.frame.toWorld(path.corners[i], planeZ);
        if (i > 0)
            path.length += path.corners[i - 1].distance(path.corners[i]);
    }
    // the S54 fields in the world (snap distances and the gap are lengths: the frame keeps them)
    path.surface = surface;
    path.start = path.startSnapDistance >= 0.0f ? q.frame.toWorld(path.start, planeZ) : from;
    path.end = path.endSnapDistance >= 0.0f ? q.frame.toWorld(path.end, q.frame.planeZOf(to)) : to;
    path.closestPoint = path.corners.empty() ? path.start : path.corners.back();
    if (path.status != PathStatus::Complete)
        path.gap = path.closestPoint.distance(path.end);
    return path;
}

bool NavigationSystem::samplePositionOn(Entity surface, const Vector3& point, float maxDistance, NavHit& out, uint16_t areaMask) {
    SurfaceQuery q;
    out = NavHit();
    if (!makeQuery(*this, scene, surface, q))
        return false;
    const NavHit hit = q.mesh->samplePosition(q.frame.toNav(point), maxDistance, NavFilter::fromSettings(areaMask));
    if (!hit.hit)
        return false;
    out = hitToWorld(hit, q.frame, q.frame.planeZOf(point));
    return true;
}

bool NavigationSystem::raycastOn(Entity surface, const Vector3& from, const Vector3& to, NavHit& out, uint16_t areaMask) {
    QueryFilter filter;
    filter.areaMask = areaMask;
    return raycastOn(surface, from, to, out, filter);
}

bool NavigationSystem::raycastOn(Entity surface, const Vector3& from, const Vector3& to, NavHit& out, const QueryFilter& filter) {
    SurfaceQuery q;
    out = NavHit();
    if (!makeQuery(*this, scene, surface, q))
        return false;
    const NavHit hit = q.mesh->raycast(q.frame.toNav(from), q.frame.toNav(to), NavFilter::fromQuery(filter), q.halfExtents);
    out = hitToWorld(hit, q.frame, q.frame.planeZOf(from));
    return hit.hit;
}

bool NavigationSystem::findClosestEdgeOn(Entity surface, const Vector3& point, NavHit& out, uint16_t areaMask) {
    QueryFilter filter;
    filter.areaMask = areaMask;
    return findClosestEdgeOn(surface, point, out, filter);
}

bool NavigationSystem::findClosestEdgeOn(Entity surface, const Vector3& point, NavHit& out, const QueryFilter& filter, float maxDistance) {
    SurfaceQuery q;
    out = NavHit();
    if (!makeQuery(*this, scene, surface, q))
        return false;
    const NavHit hit = q.mesh->findClosestEdge(q.frame.toNav(point), NavFilter::fromQuery(filter), q.halfExtents, maxDistance);
    // on the mesh without a wall within maxDistance: the distance says how far it looked
    if (hit.hit || hit.distance > 0.0f)
        out = hitToWorld(hit, q.frame, q.frame.planeZOf(point));
    return hit.hit;
}

NavPath NavigationSystem::calculatePath(const Vector3& from, const Vector3& to, const QueryFilter& filter, const PathOptions& options) {
    // the first Complete answer (entity order), else the best ranked one (R5)
    NavPath best = unanswered(PathFailure::NoSurface, from, to);
    for (Entity surface : surfacesOf(scene, filter.agentType)) {
        NavPath path = calculatePathOn(surface, from, to, filter, options);
        if (path.status == PathStatus::Complete)
            return path;
        if (betterAnswer(path, best))
            best = std::move(path);
    }
    return best;
}

bool NavigationSystem::samplePosition(const Vector3& point, float maxDistance, NavHit& out, const QueryFilter& filter) {
    out = NavHit();
    bool found = false;
    for (Entity surface : surfacesOf(scene, filter.agentType)) {
        NavHit hit;
        if (samplePositionOn(surface, point, maxDistance, hit, filter.areaMask) && (!found || hit.distance < out.distance)) {
            out = hit;
            found = true;
        }
    }
    return found;
}

bool NavigationSystem::raycast(const Vector3& from, const Vector3& to, NavHit& out, const QueryFilter& filter) {
    out = NavHit();
    for (Entity surface : surfacesOf(scene, filter.agentType)) {
        NavHit start;
        // the surface the ray starts on answers
        if (!samplePositionOn(surface, from, 1.0f, start, filter.areaMask))
            continue;
        return raycastOn(surface, from, to, out, filter);
    }
    out.hit = true;
    out.position = from;
    return true;
}

bool NavigationSystem::findClosestEdge(const Vector3& point, NavHit& out, const QueryFilter& filter, float maxDistance) {
    out = NavHit();
    bool found = false;
    for (Entity surface : surfacesOf(scene, filter.agentType)) {
        NavHit hit;
        const bool wall = findClosestEdgeOn(surface, point, hit, filter, maxDistance);
        if (wall && (!found || hit.distance < out.distance)) {
            out = hit;
            found = true;
        } else if (!wall && !found && hit.distance > 0.0f) {
            out = hit;
        }
    }
    return found;
}

bool NavigationSystem::isReachable(const Vector3& from, const Vector3& to, const QueryFilter& filter, PathFailure* why) {
    PathFailure best = PathFailure::NoSurface;
    const NavFilter navFilter = NavFilter::fromSettings(filter.areaMask);
    for (Entity surface : surfacesOf(scene, filter.agentType)) {
        SurfaceQuery q;
        PathFailure failure = PathFailure::NoSurface;
        if (makeQuery(*this, scene, surface, q, &failure)) {
            JE_PROFILE_SCOPE("Nav.Query");
            if (q.mesh->isReachable(q.frame.toNav(from), q.frame.toNav(to), navFilter, q.halfExtents, &failure)) {
                if (why)
                    *why = PathFailure::None;
                return true;
            }
        }
        if (failureRank(failure) < failureRank(best))
            best = failure;
    }
    if (why)
        *why = best;
    return false;
}

// ---- S55 queries

namespace {

    // Random points need the surface's mesh and frame (makeQuery), the stream to draw from, and the
    // island to stay on (S54's labels; -1: any)
    bool randomPointWith(NavigationSystem& system, Scene* scene, Entity surface, NavHit& out, const QueryFilter& filter,
                         const RandomPointQuery& query, NavRandom& stream) {
        SurfaceQuery q;
        out = NavHit();
        if (!makeQuery(system, scene, surface, q))
            return false;
        const NavFilter navFilter = NavFilter::fromQuery(filter);
        int island = -1;
        if (query.sameIsland) {
            // an explicit island use: locate labels a stale navmesh first (S54 R8)
            const NavLocation under = q.mesh->locate(q.frame.toNav(query.islandOf), navFilter, q.halfExtents, 0.0f);
            if (!under.onMesh || under.island < 0)
                return false;
            island = under.island;
        }
        NavHit hit;
        float planeZ = 0.0f;
        bool found = false;
        if (query.around && query.radius > 0.0f) {
            planeZ = q.frame.planeZOf(query.center);
            found = q.mesh->findRandomPointAround(q.frame.toNav(query.center), query.radius, navFilter, q.halfExtents, stream, island, hit);
        } else {
            found = q.mesh->findRandomPoint(navFilter, stream, island, hit);
        }
        if (!found)
            return false;
        out = hitToWorld(hit, q.frame, planeZ);
        return true;
    }

}

bool NavigationSystem::randomPointOn(Entity surface, NavHit& out, const QueryFilter& filter, const RandomPointQuery& query) {
    if (query.seeded) {
        NavRandom stream(query.seed);
        return randomPointWith(*this, scene, surface, out, filter, query, stream);
    }
    return randomPointWith(*this, scene, surface, out, filter, query, randomStream);
}

bool NavigationSystem::randomPoint(NavHit& out, const QueryFilter& filter, const RandomPointQuery& query) {
    out = NavHit();
    NavRandom seeded(query.seed);
    NavRandom& stream = query.seeded ? seeded : randomStream;
    for (Entity surface : surfacesOf(scene, filter.agentType)) {
        if (randomPointWith(*this, scene, surface, out, filter, query, stream))
            return true;
    }
    return false;
}

void NavigationSystem::setRandomSeed(uint64_t seed) {
    randomStream = NavRandom(seed);
    randomSeed = seed;
}

uint64_t NavigationSystem::getRandomSeed() const {
    return randomSeed;
}

bool NavigationSystem::moveAlongSurfaceOn(Entity surface, const Vector3& from, const Vector3& to, NavHit& out, const QueryFilter& filter) {
    SurfaceQuery q;
    out = NavHit();
    if (!makeQuery(*this, scene, surface, q))
        return false;
    NavHit hit;
    if (!q.mesh->moveAlongSurface(q.frame.toNav(from), q.frame.toNav(to), NavFilter::fromQuery(filter), q.halfExtents, hit))
        return false;
    out = hitToWorld(hit, q.frame, q.frame.planeZOf(from));
    return true;
}

bool NavigationSystem::moveAlongSurface(const Vector3& from, const Vector3& to, NavHit& out, const QueryFilter& filter) {
    out = NavHit();
    for (Entity surface : surfacesOf(scene, filter.agentType)) {
        if (moveAlongSurfaceOn(surface, from, to, out, filter))
            return true;
    }
    return false;
}

bool NavigationSystem::getDebugGeometry(Entity surface, const std::function<bool(const AABB& worldBounds)>& visible, size_t maxTriangles,
                                        bool detailMesh, NavDebugGeometry& out, uint32_t extras) {
    out = NavDebugGeometry();
    SurfaceQuery q;
    if (!makeQuery(*this, scene, surface, q))
        return false;
    const NavFrame frame = q.frame;
    auto worldBox = [&](const float* bmin, const float* bmax) {
        AABB box;
        for (int c = 0; c < 8; c++) {
            const Vector3 corner((c & 1) ? bmax[0] : bmin[0], (c & 2) ? bmax[1] : bmin[1], (c & 4) ? bmax[2] : bmin[2]);
            box.merge(frame.toWorld(corner));
        }
        return box;
    };
    q.mesh->getDebugGeometry([&](const float* bmin, const float* bmax) { return !visible || visible(worldBox(bmin, bmax)); },
                             maxTriangles, detailMesh, out, extras);
    auto toWorld = [&](std::vector<float>& points) {
        for (size_t i = 0; i + 2 < points.size(); i += 3) {
            const Vector3 w = frame.toWorld(Vector3(points[i], points[i + 1], points[i + 2]));
            points[i] = w.x;
            points[i + 1] = w.y;
            points[i + 2] = w.z;
        }
    };
    toWorld(out.triangles);
    toWorld(out.boundaryEdges);
    toWorld(out.polygonEdges);
    // a tile's box: its 8 corners in the world (a rotated surface's tiles are not axis-aligned)
    std::vector<float> corners;
    corners.reserve(out.tileBounds.size() / 6 * 24);
    for (size_t i = 0; i + 5 < out.tileBounds.size(); i += 6) {
        const float* bmin = &out.tileBounds[i];
        const float* bmax = &out.tileBounds[i + 3];
        for (int c = 0; c < 8; c++) {
            const Vector3 w = frame.toWorld(Vector3((c & 1) ? bmax[0] : bmin[0], (c & 2) ? bmax[1] : bmin[1], (c & 4) ? bmax[2] : bmin[2]));
            corners.push_back(w.x);
            corners.push_back(w.y);
            corners.push_back(w.z);
        }
    }
    out.tileBounds = std::move(corners);
    for (NavDebugGeometry::Link& link : out.links) {
        const Vector3 s = frame.toWorld(Vector3(link.start[0], link.start[1], link.start[2]));
        const Vector3 e = frame.toWorld(Vector3(link.end[0], link.end[1], link.end[2]));
        link.start[0] = s.x; link.start[1] = s.y; link.start[2] = s.z;
        link.end[0] = e.x; link.end[1] = e.y; link.end[2] = e.z;
    }
    return true;
}

// ---------------------------------------------------------------- S54 diagnostics

namespace {

    // nav units^2 per m^2 (a 2D surface is in scene points)
    float areaScale(Scene* scene, Entity surface) {
        const NavMeshSurfaceComponent* component = scene->findComponent<NavMeshSurfaceComponent>(surface);
        const float scale = component ? NavBakeCollector::unitScaleOf(scene, *component) : 1.0f;
        return scale * scale;
    }

    // a nav-space box as a world AABB (its 8 corners: a rotated surface's box is not axis-aligned)
    void worldBounds(const NavFrame& frame, const Vector3& bmin, const Vector3& bmax, Vector3& outMin, Vector3& outMax) {
        AABB box;
        for (int c = 0; c < 8; c++)
            box.merge(frame.toWorld(Vector3((c & 1) ? bmax.x : bmin.x, (c & 2) ? bmax.y : bmin.y, (c & 4) ? bmax.z : bmin.z)));
        outMin = box.getMinimum();
        outMax = box.getMaximum();
    }

    std::string navName(Scene* scene, Entity entity) {
        return "'" + scene->getEntityName(entity) + "'";
    }

}

Vector3 NavigationSystem::getSearchExtents(Entity surface) {
    SurfaceQuery q;
    return makeQuery(*this, scene, surface, q) ? q.halfExtents : Vector3();
}

NavLocation NavigationSystem::locateOn(Entity surface, const Vector3& point, uint16_t areaMask, float maxDistance) {
    NavLocation location;
    SurfaceQuery q;
    if (!makeQuery(*this, scene, surface, q))
        return location;
    location = q.mesh->locate(q.frame.toNav(point), NavFilter::fromSettings(areaMask), q.halfExtents, maxDistance);
    if (location.found)
        location.position = q.frame.toWorld(location.position, q.frame.planeZOf(point));
    location.surface = surface;
    return location;
}

NavLocation NavigationSystem::locate(const Vector3& point, const QueryFilter& filter, float maxDistance) {
    NavLocation best;
    for (Entity surface : surfacesOf(scene, filter.agentType)) {
        const NavLocation location = locateOn(surface, point, filter.areaMask, maxDistance);
        if (!location.found)
            continue;
        // the on-mesh one with the smallest snap, else the closest found
        const bool better = !best.found || (location.onMesh && !best.onMesh) ||
                            (location.onMesh == best.onMesh && location.snapDistance < best.snapDistance);
        if (better)
            best = location;
    }
    return best;
}

std::vector<NavIsland> NavigationSystem::getIslands(Entity surface) {
    std::vector<NavIsland> islands;
    SurfaceQuery q;
    if (!makeQuery(*this, scene, surface, q))
        return islands;
    const float scale = areaScale(scene, surface);
    islands = q.mesh->getIslands();
    for (NavIsland& island : islands) {
        island.area /= scale;
        worldBounds(q.frame, island.boundsMin, island.boundsMax, island.boundsMin, island.boundsMax);
    }
    return islands;
}

NavCoverage NavigationSystem::getCoverage(Entity surface) {
    NavCoverage coverage;
    SurfaceQuery q;
    if (!makeQuery(*this, scene, surface, q))
        return coverage;
    const float scale = areaScale(scene, surface);
    const std::vector<NavIsland>& islands = q.mesh->getIslands();
    coverage.walkableArea = q.mesh->getWalkableArea() / scale;
    coverage.islandCount = (int)islands.size();
    for (const NavIsland& island : islands)
        coverage.largestIslandArea = std::max(coverage.largestIslandArea, island.area / scale);
    std::vector<NavTileInfo> tiles;
    q.mesh->getTileInfos(tiles);
    std::set<std::pair<int, int>> nonEmpty;
    for (const NavTileInfo& tile : tiles) {
        if (tile.polys > 0)
            nonEmpty.insert({tile.y, tile.x});
    }
    const float tileSize = q.mesh->getHeader().config.tileWorldSize();
    coverage.tilesArea = (float)nonEmpty.size() * tileSize * tileSize / scale;
    coverage.fraction = coverage.tilesArea > 0.0f ? coverage.walkableArea / coverage.tilesArea : 0.0f;
    return coverage;
}

std::vector<NavigationSystem::LinkStatus> NavigationSystem::getLinkStatus() {
    std::vector<LinkStatus> out;
    std::vector<Entity> links;
    auto array = scene->getComponentArray<NavMeshLinkComponent>();
    for (size_t i = 0; i < array->size(); i++) {
        const Entity entity = array->getEntity(i);
        if (array->getComponentFromIndex(i).enabled && scene->isEntityActiveInHierarchy(entity))
            links.push_back(entity);
    }
    if (links.empty())
        return out;
    std::sort(links.begin(), links.end());
    // every baked surface's connections, once
    std::vector<std::pair<Entity, std::vector<NavLinkState>>> states;
    auto surfaces = scene->getComponentArray<NavMeshSurfaceComponent>();
    std::vector<Entity> surfaceEntities;
    for (size_t i = 0; i < surfaces->size(); i++)
        surfaceEntities.push_back(surfaces->getEntity(i));
    std::sort(surfaceEntities.begin(), surfaceEntities.end());
    for (Entity surface : surfaceEntities) {
        SurfaceQuery q;
        if (!makeQuery(*this, scene, surface, q))
            continue;
        states.emplace_back(surface, std::vector<NavLinkState>());
        q.mesh->getLinkStates(states.back().second);
    }
    for (Entity link : links) {
        bool baked = false;
        for (const auto& entry : states) {
            LinkStatus status;
            status.link = link;
            status.surface = entry.first;
            status.connected = true;
            for (const NavLinkState& state : entry.second) {
                if (state.userId != (uint32_t)link)
                    continue;
                if (status.connections == 0) {
                    status.islandStart = state.startIsland;
                    status.islandEnd = state.endIsland;
                }
                status.connections++;
                status.connected = status.connected && state.startConnected && state.endConnected;
            }
            if (status.connections == 0)
                continue;
            baked = true;
            out.push_back(status);
        }
        if (!baked) {
            LinkStatus status;
            status.link = link;
            out.push_back(status);
        }
    }
    return out;
}

std::vector<NavTileReport> NavigationSystem::getTileReports(Entity surface) {
    std::vector<NavTileReport> reports;
    SurfaceQuery q;
    if (!makeQuery(*this, scene, surface, q))
        return reports;
    const Runtime::Surface& state = rt().surfaces[surface];
    std::vector<NavTileInfo> tiles;
    q.mesh->getTileInfos(tiles);
    reports.reserve(tiles.size());
    for (const NavTileInfo& tile : tiles) {
        NavTileReport report;
        report.x = tile.x;
        report.y = tile.y;
        report.layer = tile.layer;
        report.polys = tile.polys;
        report.bytes = tile.bytes;
        auto found = state.tileMs.find({tile.y, tile.x});
        report.milliseconds = found != state.tileMs.end() ? found->second : -1.0;
        reports.push_back(report);
    }
    return reports;
}

std::vector<NavFinding> NavigationSystem::getFindings(const NavFindingOptions& options) {
    std::vector<NavFinding> findings;
    const NavigationSettings& settings = getSettings();
    auto add = [&](const char* code, FindingSeverity severity, Entity entity, std::string message,
                   std::vector<std::pair<std::string, double>> details = {}) {
        NavFinding finding;
        finding.code = code;
        finding.severity = severity;
        finding.entity = entity;
        finding.message = std::move(message);
        finding.details = std::move(details);
        findings.push_back(std::move(finding));
    };

    auto surfaceArray = scene->getComponentArray<NavMeshSurfaceComponent>();
    auto agentArray = scene->getComponentArray<NavMeshAgentComponent>();
    std::vector<Entity> surfaceEntities, agentEntities;
    for (size_t i = 0; i < surfaceArray->size(); i++)
        surfaceEntities.push_back(surfaceArray->getEntity(i));
    for (size_t i = 0; i < agentArray->size(); i++)
        agentEntities.push_back(agentArray->getEntity(i));
    std::sort(surfaceEntities.begin(), surfaceEntities.end());
    std::sort(agentEntities.begin(), agentEntities.end());
    std::set<int> usedTypes;

    // ---- surfaces: baked, readable, current, last bake
    for (Entity entity : surfaceEntities) {
        const NavMeshSurfaceComponent& surface = scene->getComponent<NavMeshSurfaceComponent>(entity);
        usedTypes.insert(surface.agentType);
        const std::string name = navName(scene, entity);
        if (!settings.findAgentType(surface.agentType))
            add("agent_type_missing", FindingSeverity::Error, entity,
                "NavMesh Surface " + name + " uses agent type " + std::to_string(surface.agentType) +
                    ", which the project does not define: it bakes for '" + settings.agentTypeOrDefault(surface.agentType).name +
                    "' instead (pick an Agent Type, or add it in Window > AI > Navigation > Agents)",
                {{"agent_type", (double)surface.agentType}});
        if (!surface.enabled)
            continue;
        const NavMesh* mesh = getNavMesh(entity);
        const Runtime::Surface& state = rt().surfaces[entity];
        if (!state.loadError.empty()) {
            add("navmesh_unreadable", FindingSeverity::Error, entity,
                "NavMesh Surface " + name + ": its navmesh '" + surface.navMeshData.path + "' cannot be read (" + state.loadError +
                    "); bake it again");
        } else if (!mesh) {
            // S19's text (test_checks.py)
            add("not_baked", FindingSeverity::Warning, entity,
                "NavMesh Surface " + name + " is not baked: its agents find no navmesh in the build (Bake, or bake_navigation)");
        } else if (options.stale) {
            const int dirty = options.dirtyTilesOf ? options.dirtyTilesOf(entity) : countDirtyTiles(entity);
            if (dirty > 0)
                add("stale_bake", FindingSeverity::Warning, entity,
                    "NavMesh Surface " + name + " is out of date: " + std::to_string(dirty) +
                        " tile(s) changed since its bake (Bake, or bake_navigation)",
                    {{"dirty_tiles", (double)dirty}});
        }
        const NavBakeResult* last = state.lastResult.get();
        if (last && !last->ok && !last->cancelled)
            add("bake_failed", FindingSeverity::Error, entity,
                "NavMesh Surface " + name + ": its last bake failed (" + std::string(bakeErrorName(last->errorCode)) + ": " + last->error +
                    "). " + last->hint,
                {{"error_code", (double)last->errorCode}});
    }

    // ---- links: every end on a polygon
    const std::vector<LinkStatus> links = getLinkStatus();
    for (const LinkStatus& link : links) {
        if (link.surface == NULL_ENTITY || link.connected)
            continue;
        add("link_unconnected", FindingSeverity::Warning, link.link,
            "NavMesh Link " + navName(scene, link.link) + " has an end on no polygon of NavMesh Surface " + navName(scene, link.surface) +
                ": agents cannot use it (move its ends onto the navmesh, or widen its search with a smaller Voxel Size, then bake)",
            {{"island_start", (double)link.islandStart}, {"island_end", (double)link.islandEnd}, {"connections", (double)link.connections}});
    }

    // ---- agents: on a surface of their type, or on a grid
    std::vector<Entity> grids;
    auto gridArray = scene->getComponentArray<NavGridComponent>();
    for (size_t i = 0; i < gridArray->size(); i++) {
        const Entity grid = gridArray->getEntity(i);
        if (gridArray->getComponentFromIndex(i).enabled && scene->isEntityActiveInHierarchy(grid))
            grids.push_back(grid);
    }
    std::sort(grids.begin(), grids.end());
    // (surface, island) pairs agents stand on, and each surface's crowd masks
    std::set<std::pair<Entity, int>> occupied;
    std::map<Entity, std::set<uint16_t>> crowdMasks;
    for (Entity entity : agentEntities) {
        const NavMeshAgentComponent& agent = scene->getComponent<NavMeshAgentComponent>(entity);
        usedTypes.insert(agent.agentType);
        const std::string name = navName(scene, entity);
        if (!settings.findAgentType(agent.agentType))
            add("agent_type_missing", FindingSeverity::Error, entity,
                "NavMesh Agent " + name + " uses agent type " + std::to_string(agent.agentType) +
                    ", which the project does not define: it walks the first type's navmesh (pick an Agent Type, or add it in "
                    "Window > AI > Navigation > Agents)",
                {{"agent_type", (double)agent.agentType}});
        const Transform* transform = scene->findComponent<Transform>(entity);
        if (!agent.enabled || !transform || !scene->isEntityActiveInHierarchy(entity))
            continue;
        const uint16_t mask = (uint16_t)(agent.areaMask & ALL_AREAS);
        const Vector3 position = transform->worldPosition;
        // its surfaces: the one it names, else those of its type
        std::vector<Entity> candidates;
        bool onGrid = false;
        if (agent.surface != NULL_ENTITY) {
            if (scene->findComponent<NavGridComponent>(agent.surface))
                onGrid = true;
            else if (scene->findComponent<NavMeshSurfaceComponent>(agent.surface))
                candidates.push_back(agent.surface);
        } else {
            for (Entity surface : surfacesOf(scene, agent.agentType)) {
                const NavMeshSurfaceComponent& component = scene->getComponent<NavMeshSurfaceComponent>(surface);
                if (activeSurface(scene, surface, component))
                    candidates.push_back(surface);
            }
            if (candidates.empty()) {
                for (Entity grid : grids)
                    onGrid = onGrid || gridContains(grid, position);
            }
        }
        if (onGrid)
            continue;
        if (candidates.empty()) {
            add("agent_no_surface", FindingSeverity::Warning, entity,
                "NavMesh Agent " + name + " has no NavMesh Surface of agent type '" + settings.agentTypeOrDefault(agent.agentType).name +
                    "' and no NavGrid holds it: it does not move (add a NavMesh Surface of its type and bake, or a NavGrid under it)");
            continue;
        }
        // where its feet are on each candidate (the closest answers)
        NavLocation best;
        for (Entity surface : candidates) {
            const NavFrame frame = NavBakeCollector::frameOf(scene, surface);
            const Vector3 feet = position - frame.up() * (agent.baseOffset * NavBakeCollector::unitScaleOf(scene, scene->getComponent<NavMeshSurfaceComponent>(surface)));
            const NavLocation location = locateOn(surface, feet, mask);
            if (location.found && (!best.found || (location.onMesh && !best.onMesh) ||
                                   (location.onMesh == best.onMesh && location.snapDistance < best.snapDistance)))
                best = location;
        }
        // in play its crowd says it (isOnNavMesh); in edit its feet do
        bool onMesh = best.onMesh;
        Entity crowdSurface = best.onMesh ? best.surface : NULL_ENTITY;
        if (runtime) {
            auto found = runtime->agents.find(entity);
            if (found != runtime->agents.end() && found->second.placed && !found->second.onGrid) {
                onMesh = agent.isOnNavMesh;
                crowdSurface = found->second.surface;
            }
        }
        if (best.found && best.island >= 0)
            occupied.insert({best.surface, best.island});
        if (crowdSurface != NULL_ENTITY)
            crowdMasks[crowdSurface].insert(mask);
        if (!onMesh)
            add("agent_off_mesh", FindingSeverity::Warning, entity,
                "NavMesh Agent " + name + " is not on the navmesh: it does not move (move it onto the surface, warp it, or bake the "
                    "surface under it)",
                {{"snap_distance", best.found ? (double)best.snapDistance : -1.0}});
    }
    for (const auto& entry : crowdMasks) {
        if (entry.second.size() > 16)
            add("crowd_filter_overflow", FindingSeverity::Warning, entry.first,
                "The agents on NavMesh Surface " + navName(scene, entry.first) + " use " + std::to_string(entry.second.size()) +
                    " different Area Masks; a crowd holds 16, the others walk every area (use fewer distinct masks)",
                {{"masks", (double)entry.second.size()}});
    }

    // ---- small islands: no agent on them, no link reaching them, not the largest
    for (Entity entity : surfaceEntities) {
        const NavMeshSurfaceComponent& surface = scene->getComponent<NavMeshSurfaceComponent>(entity);
        if (!surface.enabled || options.maxSmallIslands == 0)
            continue;
        SurfaceQuery q;
        if (!makeQuery(*this, scene, entity, q))
            continue;
        const std::vector<NavIsland>& islands = q.mesh->getIslands();
        if (islands.size() < 2)
            continue;
        const float scale = areaScale(scene, entity);
        const float walkable = q.mesh->getWalkableArea();
        int largest = 0;
        for (const NavIsland& island : islands) {
            if (island.area > islands[(size_t)largest].area)
                largest = island.id;
        }
        std::set<int> linked;
        for (const LinkStatus& link : links) {
            if (link.surface == entity) {
                linked.insert(link.islandStart);
                linked.insert(link.islandEnd);
            }
        }
        std::vector<const NavIsland*> small;
        for (const NavIsland& island : islands) {
            if (island.id != largest && island.area < 0.05f * walkable && !linked.count(island.id) && !occupied.count({entity, island.id}))
                small.push_back(&island);
        }
        std::stable_sort(small.begin(), small.end(), [](const NavIsland* a, const NavIsland* b) { return a->area > b->area; });
        if (small.size() > options.maxSmallIslands)
            small.resize(options.maxSmallIslands);
        for (const NavIsland* island : small) {
            char area[32];
            std::snprintf(area, sizeof(area), "%.1f", island->area / scale);
            add("small_island", FindingSeverity::Info, entity,
                "NavMesh Surface " + navName(scene, entity) + " has a small island (#" + std::to_string(island->id) + ", " + area +
                    " m\xC2\xB2) no agent stands on and no link reaches: agents cannot get there (raise Minimum Region Area to drop "
                    "it, or connect it with a NavMesh Link)",
                {{"island", (double)island->id}, {"area_m2", (double)(island->area / scale)}});
        }
    }

    // ---- obstacles that ask to carve a surface that cannot
    auto obstacleArray = scene->getComponentArray<NavMeshObstacleComponent>();
    std::vector<Entity> obstacleEntities;
    for (size_t i = 0; i < obstacleArray->size(); i++)
        obstacleEntities.push_back(obstacleArray->getEntity(i));
    std::sort(obstacleEntities.begin(), obstacleEntities.end());
    for (Entity entity : obstacleEntities) {
        const NavMeshObstacleComponent& obstacle = scene->getComponent<NavMeshObstacleComponent>(entity);
        if (!obstacle.enabled || !obstacle.carve || !scene->isEntityActiveInHierarchy(entity))
            continue;
        for (Entity surface : surfaceEntities) {
            const NavMesh* mesh = scene->getComponent<NavMeshSurfaceComponent>(surface).enabled ? getNavMesh(surface) : nullptr;
            if (!mesh || mesh->isCarving())
                continue;
            add("obstacle_carve_ignored", FindingSeverity::Warning, entity,
                "NavMesh Obstacle " + navName(scene, entity) + " carves, but NavMesh Surface " + navName(scene, surface) +
                    " has no Runtime Carving: it is avoided instead (turn on Runtime Carving and bake, or turn off Carve)");
            break;
        }
    }

    // ---- agent types nothing uses (a scene with navigation only)
    if (!surfaceEntities.empty() || !agentEntities.empty()) {
        for (const AgentTypeSettings& type : settings.agentTypes) {
            if (!usedTypes.count(type.id))
                add("agent_type_unused", FindingSeverity::Info, NULL_ENTITY,
                    "Agent type '" + type.name + "' is used by no NavMesh Surface or NavMesh Agent (remove it in Window > AI > Navigation > "
                        "Agents if it is not needed)",
                    {{"agent_type", (double)type.id}});
        }
    }

    std::stable_sort(findings.begin(), findings.end(), [](const NavFinding& a, const NavFinding& b) {
        if (a.severity != b.severity) return a.severity > b.severity;
        if (a.entity != b.entity) return a.entity < b.entity;
        return a.code < b.code;
    });
    return findings;
}

// ---------------------------------------------------------------- grids

namespace {

    // a grid's frame: its GameObject's position and rotation; cells along local X and Z (XZ) or X and Y
    NavFrame gridFrame(Scene* scene, Entity grid) {
        NavFrame frame;
        if (const Transform* transform = scene->findComponent<Transform>(grid)) {
            frame.origin = transform->worldPosition;
            frame.rotation = transform->worldRotation;
        }
        if (const NavGridComponent* component = scene->findComponent<NavGridComponent>(grid))
            frame.plane = component->plane;
        return frame;
    }

    // grid-local 2D <-> world
    Vector2 gridLocal(const NavFrame& frame, const Vector3& world) {
        const Vector3 nav = frame.toNav(world);
        return Vector2(nav.x, nav.z);
    }

    Vector3 gridWorld(const NavFrame& frame, const Vector2& local, float height, float planeZ) {
        return frame.toWorld(Vector3(local.x, frame.plane == NavPlane::XY ? 0.0f : height, local.y), planeZ);
    }

}

NavGridMap* NavigationSystem::getGridMap(Entity grid) {
    NavGridComponent* component = scene->findComponent<NavGridComponent>(grid);
    if (!component)
        return nullptr;
    Runtime::Grid& state = rt().grids[grid];
    if (!state.loaded || state.loadedPath != component->gridData.path || !component->map) {
        // the asset's cells; a grid without one (or a map the editor made) keeps what it has
        bool read = false;
        if (!component->gridData.path.empty() && (!state.loaded || state.loadedPath != component->gridData.path)) {
            auto map = std::make_shared<NavGridMap>();
            std::vector<uint8_t> bytes;
            std::string error;
            if (readAsset(component->gridData.path, bytes, error) && map->read(bytes.data(), bytes.size(), error)) {
                component->map = map;
                component->mapDirty = false;
                read = true;
            } else {
                Log::error("NavGrid '%s': %s", scene->getEntityName(grid).c_str(), error.c_str());
            }
        }
        if (!read && !component->map) {
            component->map = std::make_shared<NavGridMap>();
            component->map->reset(std::max(1, component->width), std::max(1, component->height), AREA_WALKABLE);
        }
        state.loaded = true;
        state.loadedPath = component->gridData.path;
    }
    NavGridMap& map = *component->map;
    // the component's shape is the truth: a resize keeps what fits
    if (map.getWidth() != component->width || map.getHeight() != component->height) {
        NavGridMap resized;
        resized.reset(std::max(1, component->width), std::max(1, component->height), AREA_WALKABLE);
        resized.setLayout(map.getLayout(), map.getNeighbourMode() == 4 ? 4 : 8);
        resized.setHasHeights(map.hasHeights());
        for (int y = 0; y < std::min(map.getHeight(), resized.getHeight()); y++) {
            for (int x = 0; x < std::min(map.getWidth(), resized.getWidth()); x++) {
                const GridCell cell{x, y};
                resized.setArea(cell, map.getArea(cell));
                resized.setOverride(cell, map.getOverride(cell));
                if (map.hasHeights())
                    resized.setHeight(cell, map.getHeight(cell));
            }
        }
        map = std::move(resized);
        component->mapDirty = true;
    }
    if (map.getLayout() != component->layout || (component->layout == GridLayout::Square && map.getNeighbourMode() != component->neighbours))
        map.setLayout(component->layout, component->neighbours);
    if (map.getCellSize() != component->cellSize)
        map.setCellSize(component->cellSize);
    if (map.getMaxSlope() != component->maxSlope || map.getMaxStep() != component->maxStep)
        map.setSlopeLimits(component->maxSlope, component->maxStep);
    if (state.reservations.getCellCount() != map.getCellCount()) {
        state.reservations.reset(map.getCellCount());
        state.occupancy.assign((size_t)map.getCellCount(), 0);
    }
    return &map;
}

bool NavigationSystem::gridContains(Entity grid, const Vector3& world) {
    NavGridMap* map = getGridMap(grid);
    if (!map)
        return false;
    return map->contains(map->cellAt(gridLocal(gridFrame(scene, grid), world)));
}

GridCell NavigationSystem::gridWorldToCell(Entity grid, const Vector3& world) {
    NavGridMap* map = getGridMap(grid);
    if (!map)
        return GridCell{-1, -1};
    return map->cellAt(gridLocal(gridFrame(scene, grid), world));
}

Vector3 NavigationSystem::gridCellToWorld(Entity grid, GridCell cell) {
    NavGridMap* map = getGridMap(grid);
    if (!map)
        return Vector3();
    const NavFrame frame = gridFrame(scene, grid);
    return gridWorld(frame, map->cellCenter(cell), map->contains(cell) ? map->getHeight(cell) : 0.0f, 0.0f);
}

Vector3 NavigationSystem::gridLocalToWorld(Entity grid, const Vector2& local, float height) {
    if (!scene->findComponent<NavGridComponent>(grid))
        return Vector3();
    return gridWorld(gridFrame(scene, grid), local, height, 0.0f);
}

GridPath NavigationSystem::findGridPath(Entity grid, GridCell from, GridCell to, const GridQuery& query) {
    NavGridMap* map = getGridMap(grid);
    if (!map)
        return GridPath();
    JE_PROFILE_SCOPE("Nav.GridSolve");
    const NavFilter filter = NavFilter::fromSettings(query.areaMask);
    Runtime::Grid& state = rt().grids[grid];
    std::unique_ptr<GridSolver> solver = state.pool.lease();
    GridPath path = solver->solve(*map, from, to, query, filter.areaCosts);
    state.pool.giveBack(std::move(solver));
    return path;
}

std::vector<Vector3> NavigationSystem::gridPathToWorld(Entity grid, const GridPath& path) {
    std::vector<Vector3> points;
    NavGridMap* map = getGridMap(grid);
    if (!map)
        return points;
    const NavFrame frame = gridFrame(scene, grid);
    for (const Vector2& point : path.points) {
        const GridCell cell = map->cellAt(point);
        points.push_back(gridWorld(frame, point, map->contains(cell) ? map->getHeight(cell) : 0.0f, 0.0f));
    }
    return points;
}

bool NavigationSystem::setGridCellArea(Entity grid, GridCell cell, uint8_t area) {
    NavGridMap* map = getGridMap(grid);
    if (!map || !map->setArea(cell, area))
        return false;
    scene->getComponent<NavGridComponent>(grid).mapDirty = true;
    return true;
}

int NavigationSystem::fillGridRect(Entity grid, GridCell a, GridCell b, uint8_t area) {
    NavGridMap* map = getGridMap(grid);
    if (!map)
        return 0;
    const int changed = map->fillRect(a, b, area);
    if (changed > 0)
        scene->getComponent<NavGridComponent>(grid).mapDirty = true;
    return changed;
}

int NavigationSystem::stampGridObstacle(Entity grid, const AABB& worldBounds, uint8_t area) {
    NavGridMap* map = getGridMap(grid);
    if (!map || worldBounds.isNull() || area >= AREA_COUNT)
        return 0;
    const NavFrame frame = gridFrame(scene, grid);
    // the box's footprint in grid space, then every cell whose centre is inside it
    Vector2 lo(std::numeric_limits<float>::max(), std::numeric_limits<float>::max());
    Vector2 hi(-std::numeric_limits<float>::max(), -std::numeric_limits<float>::max());
    for (int c = 0; c < 8; c++) {
        const Vector3 corner((c & 1) ? worldBounds.getMaximum().x : worldBounds.getMinimum().x,
                             (c & 2) ? worldBounds.getMaximum().y : worldBounds.getMinimum().y,
                             (c & 4) ? worldBounds.getMaximum().z : worldBounds.getMinimum().z);
        const Vector2 local = gridLocal(frame, corner);
        lo = Vector2(std::min(lo.x, local.x), std::min(lo.y, local.y));
        hi = Vector2(std::max(hi.x, local.x), std::max(hi.y, local.y));
    }
    int changed = 0;
    for (int y = 0; y < map->getHeight(); y++) {
        for (int x = 0; x < map->getWidth(); x++) {
            const GridCell cell{x, y};
            const Vector2 center = map->cellCenter(cell);
            if (center.x >= lo.x && center.x <= hi.x && center.y >= lo.y && center.y <= hi.y && map->getArea(cell) != area) {
                map->setArea(cell, area);
                changed++;
            }
        }
    }
    if (changed > 0)
        scene->getComponent<NavGridComponent>(grid).mapDirty = true;
    return changed;
}

namespace {

    // what a grid's rebuild reads from its source, hashed (spec R36: Auto Rebuild)
    uint64_t gridSourceHash(Scene* scene, const NavGridComponent& component, Entity grid) {
        const Entity source = component.sourceEntity == NULL_ENTITY ? grid : component.sourceEntity;
        NavHasher hasher;
        hasher.i32((int32_t)component.source);
        hasher.i32(component.width);
        hasher.i32(component.height);
        hasher.f32(component.cellSize);
        hasher.i32((int32_t)component.layout);
        hasher.i32(component.emptyCellArea);
        hasher.boolean(component.collidersBlock);
        for (const NavGridTileArea& row : component.tileAreas) {
            hasher.string(row.rect);
            hasher.i32(row.area);
        }
        if (const Transform* transform = scene->findComponent<Transform>(source)) {
            hasher.floats(&transform->worldPosition.x, 3);
            hasher.f32(transform->worldRotation.w);
            hasher.f32(transform->worldRotation.x);
            hasher.f32(transform->worldRotation.y);
            hasher.f32(transform->worldRotation.z);
        }
        if (const Transform* transform = scene->findComponent<Transform>(grid)) {
            hasher.floats(&transform->worldPosition.x, 3);
            hasher.f32(transform->worldRotation.w);
        }
        if (component.source == NavGridSource::TILEMAP) {
            if (const TilemapComponent* tilemap = scene->findComponent<TilemapComponent>(source)) {
                hasher.u32(tilemap->numTiles);
                for (unsigned int t = 0; t < tilemap->numTiles; t++) {
                    const TileData& tile = tilemap->tiles[t];
                    hasher.string(tile.name);
                    hasher.i32(tile.rectId);
                    hasher.f32(tile.position.x);
                    hasher.f32(tile.position.y);
                    hasher.f32(tile.width);
                    hasher.f32(tile.height);
                }
                for (unsigned int r = 0; r < tilemap->numTilesRect; r++)
                    hasher.string(tilemap->tilesRect[r].name);
            }
        } else if (component.source == NavGridSource::TERRAIN) {
            if (const TerrainComponent* terrain = scene->findComponent<TerrainComponent>(source)) {
                hasher.f32(terrain->terrainSize);
                hasher.f32(terrain->maxHeight);
                hasher.string(terrain->heightMap.getPath());
                hasher.string(terrain->heightMap.getId());
            }
        }
        return hasher.value();
    }

}

void NavigationSystem::updateGridSources() {
    auto grids = scene->getComponentArray<NavGridComponent>();
    for (size_t i = 0; i < grids->size(); i++) {
        NavGridComponent& component = grids->getComponentFromIndex(i);
        const Entity entity = grids->getEntity(i);
        if (!component.enabled || !component.autoRebuild || component.source == NavGridSource::NONE)
            continue;
        const uint64_t hash = gridSourceHash(scene, component, entity);
        Runtime::Grid& state = rt().grids[entity];
        if (state.sourceHash == hash)
            continue;
        // the first look only records it when the cells came from an asset (they are the rebuild)
        const bool first = state.sourceHash == 0;
        state.sourceHash = hash;
        if (first && !component.gridData.path.empty())
            continue;
        std::string error;
        if (!rebuildGrid(entity, error))
            Log::warn("NavGrid '%s': %s", scene->getEntityName(entity).c_str(), error.c_str());
    }
}

bool NavigationSystem::rebuildGrid(Entity grid, std::string& error) {
    NavGridComponent* component = scene->findComponent<NavGridComponent>(grid);
    NavGridMap* map = getGridMap(grid);
    if (!component || !map) {
        error = "the GameObject has no NavGrid";
        return false;
    }
    const Entity source = component->sourceEntity == NULL_ENTITY ? grid : component->sourceEntity;
    const NavFrame frame = gridFrame(scene, grid);
    // painted cells (the override bit) keep their area (spec R36)
    auto assign = [&](GridCell cell, uint8_t area) {
        if (!map->getOverride(cell))
            map->setArea(cell, area);
    };
    if (component->source == NavGridSource::TILEMAP) {
        const TilemapComponent* tilemap = scene->findComponent<TilemapComponent>(source);
        const Transform* tilemapTransform = scene->findComponent<Transform>(source);
        if (!tilemap || !tilemapTransform) {
            error = "the grid's source has no Tilemap";
            return false;
        }
        std::map<std::string, uint8_t> areas;
        for (const NavGridTileArea& row : component->tileAreas)
            areas[row.rect] = (uint8_t)std::clamp(row.area, 0, AREA_COUNT - 1);
        for (int y = 0; y < map->getHeight(); y++) {
            for (int x = 0; x < map->getWidth(); x++) {
                const GridCell cell{x, y};
                const Vector3 world = gridWorld(frame, map->cellCenter(cell), 0.0f, 0.0f);
                const Vector3 local = tilemapTransform->modelMatrix.inverse() * world;
                // the topmost (last drawn) tile holding the cell's centre gives its area
                int found = -1;
                for (unsigned int t = 0; t < tilemap->numTiles; t++) {
                    const TileData& tile = tilemap->tiles[t];
                    if (local.x >= tile.position.x && local.x < tile.position.x + tile.width && local.y >= tile.position.y &&
                        local.y < tile.position.y + tile.height)
                        found = (int)t;
                }
                uint8_t area = (uint8_t)std::clamp(component->emptyCellArea, 0, AREA_COUNT - 1);
                if (found >= 0) {
                    const TileData& tile = tilemap->tiles[(unsigned int)found];
                    std::string rect = tile.name;
                    if (tile.rectId >= 0 && (unsigned int)tile.rectId < tilemap->numTilesRect)
                        rect = tilemap->tilesRect[(unsigned int)tile.rectId].name;
                    auto mapped = areas.find(rect);
                    area = mapped != areas.end() ? mapped->second : AREA_WALKABLE;
                }
                assign(cell, area);
            }
        }
#ifdef JASPERENGINE_PHYSICS_2D
        if (component->collidersBlock) {
            // static Collider2D footprints block the cells whose centre they hold
            auto bodies = scene->getComponentArray<Rigidbody2DComponent>();
            for (size_t i = 0; i < bodies->size(); i++) {
                const Rigidbody2DComponent& body = bodies->getComponentFromIndex(i);
                const Entity entity = bodies->getEntity(i);
                const Transform* transform = scene->findComponent<Transform>(entity);
                if (!body.enabled || body.type != BodyType::STATIC || !transform || !scene->isEntityActiveInHierarchy(entity))
                    continue;
                for (size_t c = 0; c < body.colliders.size(); c++) {
                    const Collider2D& collider = body.colliders[c];
                    if (collider.sensor)
                        continue;
                    AABB bounds;
                    auto add = [&](const Vector2& p) {
                        bounds.merge(transform->modelMatrix * Vector3(p.x, p.y, 0.0f));
                    };
                    const float r = collider.radius;
                    if (collider.type == Collider2DType::CIRCLE) {
                        add(collider.pointA - Vector2(r, r));
                        add(collider.pointA + Vector2(r, r));
                    } else if (collider.type == Collider2DType::POLYGON && collider.points.size() >= 3) {
                        for (size_t v = 0; v < collider.points.size(); v++) add(collider.points[v]);
                    } else if (collider.type == Collider2DType::CHAIN) {
                        for (size_t v = 0; v < collider.points.size(); v++) add(collider.points[v]);
                    } else {
                        add(collider.pointA - Vector2(r, r));
                        add(collider.pointB + Vector2(r, r));
                        add(collider.pointA + Vector2(r, r));
                        add(collider.pointB - Vector2(r, r));
                    }
                    if (bounds.isNull())
                        continue;
                    bounds = AABB(Vector3(bounds.getMinimum().x, bounds.getMinimum().y, -1e6f), Vector3(bounds.getMaximum().x, bounds.getMaximum().y, 1e6f));
                    for (int y = 0; y < map->getHeight(); y++) {
                        for (int x = 0; x < map->getWidth(); x++) {
                            const GridCell cell{x, y};
                            const Vector3 world = gridWorld(frame, map->cellCenter(cell), 0.0f, 0.0f);
                            if (world.x >= bounds.getMinimum().x && world.x <= bounds.getMaximum().x && world.y >= bounds.getMinimum().y &&
                                world.y <= bounds.getMaximum().y)
                                assign(cell, AREA_NOT_WALKABLE);
                        }
                    }
                }
            }
        }
#endif
    } else if (component->source == NavGridSource::TERRAIN) {
        TerrainComponent* terrain = scene->findComponent<TerrainComponent>(source);
        const Transform* terrainTransform = scene->findComponent<Transform>(source);
        if (!terrain || !terrainTransform) {
            error = "the grid's source has no Terrain";
            return false;
        }
        map->setHasHeights(true);
        const Matrix4 toLocal = terrainTransform->modelMatrix.inverse();
        const float half = terrain->terrainSize * 0.5f;
        for (int y = 0; y < map->getHeight(); y++) {
            for (int x = 0; x < map->getWidth(); x++) {
                const GridCell cell{x, y};
                const Vector3 world = gridWorld(frame, map->cellCenter(cell), 0.0f, 0.0f);
                const Vector3 local = toLocal * world;
                if (std::fabs(local.x) > half || std::fabs(local.z) > half) {
                    assign(cell, AREA_NOT_WALKABLE);
                    continue;
                }
                float height = 0.0f;
                Vector3 normal;
                // heights at the cell centre (spec risk table: hex cells too)
                MeshSystem::sampleTerrainSurface(*terrain, local.x, local.z, height, normal);
                const Vector3 surfaceWorld = terrainTransform->modelMatrix * Vector3(local.x, height, local.z);
                map->setHeight(cell, frame.toNav(surfaceWorld).y);
                if (!map->getOverride(cell) && map->getArea(cell) == AREA_NOT_WALKABLE)
                    map->setArea(cell, AREA_WALKABLE);
            }
        }
    } else {
        error = "the grid has no source (Source: None)";
        return false;
    }
    component->mapDirty = true;
    return true;
}

// ---------------------------------------------------------------- agents

namespace {

    float unitScaleOfSurface(Scene* scene, Entity surface) {
        const NavMeshSurfaceComponent* component = scene->findComponent<NavMeshSurfaceComponent>(surface);
        return component ? NavBakeCollector::unitScaleOf(scene, *component) : 1.0f;
    }

    // the agent's crowd parameters (its lengths and speeds in metres, scaled to the surface's units)
    // with its own area costs
    NavCrowdAgentParams agentParams(const NavMeshAgentComponent& agent, float scale, const float (&areaCosts)[AREA_COUNT]) {
        NavCrowdAgentParams params;
        params.radius = std::max(agent.radius, 1e-3f) * scale;
        params.height = std::max(agent.height, 1e-3f) * scale;
        params.maxSpeed = std::max(agent.speed, 0.0f) * scale;
        params.maxAcceleration = std::max(agent.acceleration, 0.0f) * scale;
        params.quality = agent.obstacleAvoidance;
        params.priority = (uint8_t)std::clamp(agent.avoidancePriority, 0, 99);
        params.areaMask = (uint16_t)(agent.areaMask & ALL_AREAS);
        std::memcpy(params.areaCosts, areaCosts, sizeof(params.areaCosts));
        return params;
    }

    bool sameParams(const NavCrowdAgentParams& a, const NavCrowdAgentParams& b) {
        return a.radius == b.radius && a.height == b.height && a.maxSpeed == b.maxSpeed && a.maxAcceleration == b.maxAcceleration &&
               a.quality == b.quality && a.priority == b.priority && a.areaMask == b.areaMask && a.obstacle == b.obstacle &&
               std::memcmp(a.areaCosts, b.areaCosts, sizeof(a.areaCosts)) == 0;
    }

}

Entity NavigationSystem::getAgentSurface(Entity agent) {
    if (!runtime)
        return NULL_ENTITY;
    auto found = runtime->agents.find(agent);
    return found != runtime->agents.end() ? found->second.surface : NULL_ENTITY;
}

// ---- S55 queries: an agent's own area costs

void NavigationSystem::setAgentAreaCost(Entity agent, int area, float cost) {
    if (area < 0 || area >= AREA_COUNT || !scene->findComponent<NavMeshAgentComponent>(agent))
        return;
    Runtime::Agent& state = rt().agents[agent];
    state.entity = agent;
    // the next fixed tick hands the crowd the agent's new filter (syncAgents)
    state.areaCosts[area] = clampAreaCost(cost);
}

float NavigationSystem::getAgentAreaCost(Entity agent, int area) const {
    if (area < 0 || area >= AREA_COUNT)
        return 0.0f;
    if (runtime) {
        auto found = runtime->agents.find(agent);
        if (found != runtime->agents.end() && found->second.areaCosts[area] > 0.0f)
            return found->second.areaCosts[area];
    }
    return getSettings().areas[(size_t)area].cost;
}

void NavigationSystem::resetAgentAreaCost(Entity agent, int area) {
    if (area < 0 || area >= AREA_COUNT || !runtime)
        return;
    auto found = runtime->agents.find(agent);
    if (found != runtime->agents.end())
        found->second.areaCosts[area] = 0.0f;
}

QueryFilter NavigationSystem::getAgentQueryFilter(Entity agent) const {
    QueryFilter filter;
    const NavMeshAgentComponent* component = scene->findComponent<NavMeshAgentComponent>(agent);
    if (!component)
        return filter;
    filter.agentType = component->agentType;
    filter.areaMask = (uint16_t)(component->areaMask & ALL_AREAS);
    if (runtime) {
        auto found = runtime->agents.find(agent);
        if (found != runtime->agents.end()) {
            for (int area = 0; area < AREA_COUNT; area++) {
                if (found->second.areaCosts[area] > 0.0f)
                    filter.setAreaCost(area, found->second.areaCosts[area]);
            }
        }
    }
    return filter;
}

bool NavigationSystem::setDestination(Entity entity, const Vector3& target) {
    NavMeshAgentComponent* agent = scene->findComponent<NavMeshAgentComponent>(entity);
    if (!agent)
        return false;
    Runtime::Agent& state = rt().agents[entity];
    state.entity = entity;
    // the destination snaps to the mesh within twice the agent's height (spec R12)
    Vector3 snapped = target;
    if (!state.onGrid) {
        NavHit hit;
        const float reach = 2.0f * std::max(agent->height, 1e-3f) * (state.surface != NULL_ENTITY ? unitScaleOfSurface(scene, state.surface) : 1.0f);
        const uint16_t mask = (uint16_t)(agent->areaMask & ALL_AREAS);
        const bool found = state.surface != NULL_ENTITY ? samplePositionOn(state.surface, target, reach, hit, mask)
                                                         : samplePosition(target, reach, hit, QueryFilter{agent->agentType, mask});
        if (found)
            snapped = hit.position;
    }
    agent->destination = snapped;
    agent->hasPath = true;
    agent->pathPending = true;
    state.destination = snapped;
    state.requested = true;
    state.arrived = false;
    state.failed = false;
    state.wasPending = true;
    state.gridPending = false;
    state.gridCells.clear();
    if (state.placed && !state.onGrid && state.crowdIndex >= 0) {
        auto surface = runtime->surfaces.find(state.surface);
        if (surface != runtime->surfaces.end() && surface->second.crowd) {
            const NavFrame frame = NavBakeCollector::frameOf(scene, state.surface);
            if (!surface->second.crowd->requestMoveTarget(state.crowdIndex, frame.toNav(snapped))) {
                agent->pathPending = false;
                agent->pathStatus = PathStatus::Invalid;
                state.failed = true;
                runtime->events.push_back(Event{entity, EventType::PathFailed});
                return false;
            }
        }
    }
    return true;
}

void NavigationSystem::resetPath(Entity entity) {
    NavMeshAgentComponent* agent = scene->findComponent<NavMeshAgentComponent>(entity);
    if (!agent || !runtime)
        return;
    auto found = runtime->agents.find(entity);
    if (found == runtime->agents.end())
        return;
    Runtime::Agent& state = found->second;
    state.requested = false;
    state.gridCells.clear();
    state.gridPending = false;
    agent->hasPath = false;
    agent->pathPending = false;
    agent->remainingDistance = 0.0f;
    if (state.placed && state.crowdIndex >= 0) {
        auto surface = runtime->surfaces.find(state.surface);
        if (surface != runtime->surfaces.end() && surface->second.crowd)
            surface->second.crowd->resetMoveTarget(state.crowdIndex);
    }
    auto grid = runtime->grids.find(state.surface);
    if (grid != runtime->grids.end())
        grid->second.reservations.release((uint32_t)entity);
}

bool NavigationSystem::warp(Entity entity, const Vector3& position) {
    NavMeshAgentComponent* agent = scene->findComponent<NavMeshAgentComponent>(entity);
    Transform* transform = scene->findComponent<Transform>(entity);
    if (!agent || !transform)
        return false;
    Runtime::Agent& state = rt().agents[entity];
    state.entity = entity;
    state.onLink = false;
    state.linkWaiting = false;
    agent->isOnOffMeshLink = false;
    PhysicsSystem* physics = scene->getSystem<PhysicsSystem>().get();
    if (state.onGrid || state.surface == NULL_ENTITY || !state.placed) {
        physics->writeTransformWorldPose(entity, position, transform->worldRotation);
        state.writtenPosition = transform->worldPosition;
        state.previousPosition = state.currentPosition = transform->worldPosition;
        if (state.onGrid && state.requested) {
            state.gridCells.clear();
            state.gridPending = false;
        }
        return true;
    }
    auto surface = runtime->surfaces.find(state.surface);
    if (surface == runtime->surfaces.end() || !surface->second.crowd)
        return false;
    const NavFrame frame = NavBakeCollector::frameOf(scene, state.surface);
    const float scale = unitScaleOfSurface(scene, state.surface);
    const Vector3 up = frame.up();
    const bool ok = surface->second.crowd->teleport(state.crowdIndex, frame.toNav(position - up * (agent->baseOffset * scale)));
    agent->isOnNavMesh = ok;
    const Vector3 placed = frame.toWorld(surface->second.crowd->getPosition(state.crowdIndex), state.planeZ) + up * (agent->baseOffset * scale);
    physics->writeTransformWorldPose(entity, ok ? placed : position, transform->worldRotation);
    state.previousPosition = state.currentPosition = state.writtenPosition = transform->worldPosition;
    return ok;
}

void NavigationSystem::move(Entity entity, const Vector3& offset) {
    NavMeshAgentComponent* agent = scene->findComponent<NavMeshAgentComponent>(entity);
    Transform* transform = scene->findComponent<Transform>(entity);
    if (!agent || !transform || !runtime)
        return;
    auto found = runtime->agents.find(entity);
    if (found == runtime->agents.end() || !found->second.placed || found->second.onGrid)
        return;
    Runtime::Agent& state = found->second;
    auto surface = runtime->surfaces.find(state.surface);
    if (surface == runtime->surfaces.end() || !surface->second.crowd)
        return;
    const NavFrame frame = NavBakeCollector::frameOf(scene, state.surface);
    NavCrowd& crowd = *surface->second.crowd;
    crowd.movePosition(state.crowdIndex, crowd.getPosition(state.crowdIndex) + frame.directionToNav(offset));
    if (agent->updatePosition && agent->driver == NavAgentDriver::TRANSFORM) {
        const float scale = unitScaleOfSurface(scene, state.surface);
        const Vector3 world = frame.toWorld(crowd.getPosition(state.crowdIndex), state.planeZ) + frame.up() * (agent->baseOffset * scale);
        scene->getSystem<PhysicsSystem>()->writeTransformWorldPose(entity, world, transform->worldRotation);
        state.previousPosition = state.currentPosition = state.writtenPosition = transform->worldPosition;
    }
}

void NavigationSystem::completeOffMeshLink(Entity entity) {
    NavMeshAgentComponent* agent = scene->findComponent<NavMeshAgentComponent>(entity);
    if (!agent || !runtime)
        return;
    auto found = runtime->agents.find(entity);
    if (found == runtime->agents.end() || !found->second.onLink)
        return;
    Runtime::Agent& state = found->second;
    auto surface = runtime->surfaces.find(state.surface);
    if (surface != runtime->surfaces.end() && surface->second.crowd)
        surface->second.crowd->completeOffMeshLink(state.crowdIndex, state.linkEnd);
    state.onLink = false;
    state.linkWaiting = false;
    agent->isOnOffMeshLink = false;
    runtime->events.push_back(Event{entity, EventType::LinkEnd});
}

bool NavigationSystem::getCurrentOffMeshLink(Entity entity, Vector3& start, Vector3& end, Entity& link) {
    if (!runtime)
        return false;
    auto found = runtime->agents.find(entity);
    if (found == runtime->agents.end() || !found->second.onLink)
        return false;
    const Runtime::Agent& state = found->second;
    const NavFrame frame = NavBakeCollector::frameOf(scene, state.surface);
    start = frame.toWorld(state.linkStart, state.planeZ);
    end = frame.toWorld(state.linkEnd, state.planeZ);
    link = state.link;
    return true;
}

bool NavigationSystem::getAgentPath(Entity entity, std::vector<Vector3>& corners) {
    corners.clear();
    if (!runtime)
        return false;
    auto found = runtime->agents.find(entity);
    if (found == runtime->agents.end() || !found->second.placed)
        return false;
    const Runtime::Agent& state = found->second;
    if (state.onGrid) {
        NavGridMap* map = getGridMap(state.surface);
        if (!map)
            return false;
        const NavFrame frame = gridFrame(scene, state.surface);
        for (size_t i = state.gridNext; i < state.gridCells.size(); i++)
            corners.push_back(gridWorld(frame, map->cellCenter(state.gridCells[i]), map->getHeight(state.gridCells[i]), state.planeZ));
        return true;
    }
    auto surface = runtime->surfaces.find(state.surface);
    if (surface == runtime->surfaces.end() || !surface->second.crowd)
        return false;
    const NavFrame frame = NavBakeCollector::frameOf(scene, state.surface);
    std::vector<Vector3> nav;
    surface->second.crowd->getCorners(state.crowdIndex, nav);
    for (const Vector3& corner : nav)
        corners.push_back(frame.toWorld(corner, state.planeZ));
    return true;
}

int NavigationSystem::getCrowdCount() const {
    if (!runtime)
        return 0;
    int count = 0;
    for (const auto& entry : runtime->surfaces) {
        if (entry.second.crowd)
            count++;
    }
    return count;
}

int NavigationSystem::getAgentCount() const {
    return runtime ? (int)runtime->agents.size() : 0;
}

size_t NavigationSystem::getMemoryBytes() const {
    if (!runtime)
        return 0;
    size_t bytes = sizeof(Runtime);
    for (const auto& entry : runtime->surfaces) {
        if (entry.second.mesh)
            bytes += entry.second.mesh->getMemoryBytes();
        if (entry.second.crowd)
            bytes += entry.second.crowd->getMemoryBytes();
    }
    for (const auto& entry : runtime->grids)
        bytes += entry.second.pool.scratchBytes() + entry.second.occupancy.capacity() * sizeof(uint32_t);
    bytes += runtime->agents.size() * sizeof(Runtime::Agent);
    return bytes;
}

// ---------------------------------------------------------------- the fixed tick

std::vector<Entity> NavigationSystem::Runtime::sortedAgents(Scene* scene) const {
    std::vector<Entity> entities;
    auto array = scene->getComponentArray<NavMeshAgentComponent>();
    entities.reserve(array->size());
    for (size_t i = 0; i < array->size(); i++)
        entities.push_back(array->getEntity(i));
    // ascending entity order: the same order every run (spec R40c)
    std::sort(entities.begin(), entities.end());
    return entities;
}

void NavigationSystem::Runtime::syncSurfaces(NavigationSystem& system, Scene* scene) {
    auto array = scene->getComponentArray<NavMeshSurfaceComponent>();
    for (size_t i = 0; i < array->size(); i++) {
        const Entity entity = array->getEntity(i);
        Surface& surface = surfaces[entity];
        surface.entity = entity;
        surface.frame = NavBakeCollector::frameOf(scene, entity);
        system.getNavMesh(entity);
        // a new Detour mesh (a load, a full bake) needs a new crowd: its agents are placed again
        const void* detour = surface.mesh && surface.mesh->isValid() ? surface.mesh->detourNavMesh() : nullptr;
        if (surface.crowd && surface.crowdMesh != detour) {
            surface.crowd.reset();
            surface.crowdMesh = nullptr;
            for (auto& agent : agents) {
                if (agent.second.surface == entity) {
                    agent.second.placed = false;
                    agent.second.crowdIndex = -1;
                    agent.second.onLink = false;
                }
            }
            for (auto& obstacle : obstacles)
                obstacle.second.crowdSlots.erase(entity);
        }
        // a mesh that went away (Clear) leaves its agents unplaced
        if (!detour) {
            for (auto& agent : agents) {
                if (agent.second.surface == entity && !agent.second.onGrid) {
                    agent.second.surface = NULL_ENTITY;
                    agent.second.placed = false;
                    agent.second.crowdIndex = -1;
                }
            }
        }
    }
}

bool NavigationSystem::Runtime::ensureCrowd(Scene* scene, Surface& surface) {
    if (!surface.mesh || !surface.mesh->isValid())
        return false;
    if (surface.crowd && !surface.crowdGrow && surface.crowdMesh == surface.mesh->detourNavMesh())
        return true;
    const NavigationSettings& settings = getSettings();
    int capacity = std::max(settings.crowdMaxAgents, surface.crowdCapacity);
    if (surface.crowdGrow) {
        capacity = std::max(capacity, surface.crowdCapacity * 2);
        Log::warn("Navigation: the crowd of '%s' is full (%d agents); it grows to %d (Project Settings > Navigation > Crowd Max Agents)",
                  scene->getEntityName(surface.entity).c_str(), surface.crowdCapacity, capacity);
    }
    // the largest radius among its agents and obstacles sizes the crowd's grid and placement box
    const float scale = unitScaleOfSurface(scene, surface.entity);
    float maxRadius = surface.mesh->getHeader().config.agentRadius;
    for (const auto& entry : agents) {
        if (entry.second.surface == surface.entity)
            maxRadius = std::max(maxRadius, entry.second.params.radius);
    }
    auto obstacleArray = scene->getComponentArray<NavMeshObstacleComponent>();
    for (size_t i = 0; i < obstacleArray->size(); i++) {
        const NavMeshObstacleComponent& obstacle = obstacleArray->getComponentFromIndex(i);
        maxRadius = std::max(maxRadius, (obstacle.shape == NavObstacleShape::BOX ? 0.5f * std::sqrt(obstacle.size.x * obstacle.size.x + obstacle.size.z * obstacle.size.z)
                                                                                 : obstacle.radius) * scale);
    }
    auto crowd = std::make_unique<NavCrowd>();
    if (!crowd->init(*surface.mesh, capacity, maxRadius))
        return false;
    surface.crowd = std::move(crowd);
    surface.warnedFilterFallback = false;
    surface.crowdMesh = surface.mesh->detourNavMesh();
    surface.crowdCapacity = capacity;
    surface.crowdGrow = false;
    for (auto& entry : agents) {
        if (entry.second.surface == surface.entity && !entry.second.onGrid) {
            entry.second.placed = false;
            entry.second.crowdIndex = -1;
            entry.second.onLink = false;
        }
    }
    for (auto& obstacle : obstacles)
        obstacle.second.crowdSlots.erase(surface.entity);
    return true;
}

Entity NavigationSystem::Runtime::pickSurface(NavigationSystem& system, Scene* scene, Entity entity, const NavMeshAgentComponent& component,
                                              const Vector3& position) {
    if (component.surface != NULL_ENTITY) {
        if (scene->findComponent<NavMeshSurfaceComponent>(component.surface) || scene->findComponent<NavGridComponent>(component.surface))
            return component.surface;
        warnOnce(warned, "surface-ref-" + std::to_string(entity),
                 "NavMeshAgent '" + scene->getEntityName(entity) + "': its Surface has no NavMeshSurface or NavGrid");
        return NULL_ENTITY;
    }
    // the surface of its agent type nearest under it, else the NavGrid holding it
    Entity best = NULL_ENTITY;
    float bestDistance = std::numeric_limits<float>::max();
    for (Entity surface : surfacesOf(scene, component.agentType)) {
        NavHit hit;
        const float reach = 2.0f * std::max(component.height, 1e-3f) * unitScaleOfSurface(scene, surface);
        // a 2D surface holds the agents on (or near) its plane only: its queries drop the depth
        const NavFrame surfaceFrame = NavBakeCollector::frameOf(scene, surface);
        if (surfaceFrame.plane == NavPlane::XY && std::fabs(position.z - surfaceFrame.origin.z) > reach)
            continue;
        if (system.samplePositionOn(surface, position, reach, hit, (uint16_t)(component.areaMask & ALL_AREAS)) && hit.distance < bestDistance) {
            best = surface;
            bestDistance = hit.distance;
        }
    }
    if (best != NULL_ENTITY)
        return best;
    std::vector<Entity> gridEntities;
    auto gridArray = scene->getComponentArray<NavGridComponent>();
    for (size_t i = 0; i < gridArray->size(); i++)
        gridEntities.push_back(gridArray->getEntity(i));
    std::sort(gridEntities.begin(), gridEntities.end());
    for (Entity grid : gridEntities) {
        const NavGridComponent& gridComponent = scene->getComponent<NavGridComponent>(grid);
        if (gridComponent.enabled && scene->isEntityActiveInHierarchy(grid) && system.gridContains(grid, position))
            return grid;
    }
    return NULL_ENTITY;
}

void NavigationSystem::Runtime::warnFilterFallback(Scene* scene, Surface& surface, Entity entity, int crowdIndex) {
    if (surface.warnedFilterFallback || !surface.crowd->isFilterFallback(crowdIndex))
        return;
    surface.warnedFilterFallback = true;
    Log::warn("Navigation: the crowd of '%s' has no free query filter (16, one per area mask and costs) for NavMeshAgent '%s': it "
              "uses one with its area mask and other costs, or every area",
              scene->getEntityName(surface.entity).c_str(), scene->getEntityName(entity).c_str());
}

void NavigationSystem::Runtime::syncAgents(NavigationSystem& system, Scene* scene) {
    for (Entity entity : sortedAgents(scene)) {
        NavMeshAgentComponent& component = scene->getComponent<NavMeshAgentComponent>(entity);
        Transform* transform = scene->findComponent<Transform>(entity);
        Agent& agent = agents[entity];
        agent.entity = entity;
        const bool active = component.enabled && transform && scene->isEntityActiveInHierarchy(entity);
        if (!active) {
            if (agent.placed) {
                auto surface = surfaces.find(agent.surface);
                if (surface != surfaces.end() && surface->second.crowd && agent.crowdIndex >= 0)
                    surface->second.crowd->removeAgent(agent.crowdIndex);
                auto grid = grids.find(agent.surface);
                if (grid != grids.end()) {
                    grid->second.reservations.release((uint32_t)entity);
                    for (uint32_t& holder : grid->second.occupancy) {
                        if (holder == (uint32_t)entity)
                            holder = 0;
                    }
                }
            }
            agent.placed = false;
            agent.crowdIndex = -1;
            component.isOnNavMesh = false;
            continue;
        }

        const bool surfaceGone = agent.surface != NULL_ENTITY && !scene->findComponent<NavMeshSurfaceComponent>(agent.surface) &&
                                 !scene->findComponent<NavGridComponent>(agent.surface);
        if (agent.surface == NULL_ENTITY || surfaceGone || (component.surface != NULL_ENTITY && component.surface != agent.surface)) {
            agent.surface = pickSurface(system, scene, entity, component, transform->worldPosition);
            agent.onGrid = agent.surface != NULL_ENTITY && scene->findComponent<NavGridComponent>(agent.surface) != nullptr;
            agent.placed = false;
            agent.crowdIndex = -1;
        }
        if (agent.surface == NULL_ENTITY) {
            component.isOnNavMesh = false;
            if (!agent.warnedOffMesh) {
                agent.warnedOffMesh = true;
                Log::warn("NavMeshAgent '%s' is not on a navmesh or a NavGrid: it does not move", scene->getEntityName(entity).c_str());
            }
            continue;
        }

        if (agent.onGrid) {
            NavGridMap* map = system.getGridMap(agent.surface);
            if (!map)
                continue;
            Grid& grid = grids[agent.surface];
            const NavFrame frame = gridFrame(scene, agent.surface);
            if (!agent.placed) {
                agent.placed = true;
                agent.planeZ = frame.planeZOf(transform->worldPosition);
                agent.previousPosition = agent.currentPosition = agent.writtenPosition = transform->worldPosition;
                agent.previousRotation = agent.currentRotation = agent.writtenRotation = transform->worldRotation;
                agent.hasPose = true;
                const GridCell cell = map->cellAt(gridLocal(frame, transform->worldPosition));
                component.isOnNavMesh = map->contains(cell) && map->isWalkable(map->index(cell));
                if (component.isOnNavMesh)
                    grid.occupancy[(size_t)map->index(cell)] = (uint32_t)entity;
                else if (!agent.warnedOffMesh) {
                    agent.warnedOffMesh = true;
                    Log::warn("NavMeshAgent '%s' is not on a walkable cell of its NavGrid: it does not move", scene->getEntityName(entity).c_str());
                }
            } else if (component.driver != NavAgentDriver::NONE && component.updatePosition &&
                       transform->worldPosition != agent.writtenPosition) {
                // moved by a body or a script: the agent follows its transform
                agent.currentPosition = agent.writtenPosition = transform->worldPosition;
            }
            // a destination without a path asks the batch for one (spec R19)
            if (agent.requested && !agent.arrived && agent.gridCells.empty() && !agent.gridPending && component.isOnNavMesh) {
                const GridCell start = map->cellAt(gridLocal(frame, agent.currentPosition));
                const GridCell goal = map->cellAt(gridLocal(frame, agent.destination));
                const NavGridComponent& gridComponent = scene->getComponent<NavGridComponent>(agent.surface);
                Runtime::GridRequest request;
                request.agent = entity;
                request.grid = agent.surface;
                request.request.start = start;
                request.request.goal = goal;
                request.request.query.areaMask = (uint16_t)(component.areaMask & ALL_AREAS);
                request.request.query.allowPartial = true;
                request.request.query.movement = gridComponent.movement;
                request.request.query.agent = (uint32_t)entity;
                request.request.query.reservations = &grid.reservations;
                request.request.query.reservationPenalty = std::max(1.0f, gridComponent.reservationPenalty);
                gridRequests.push_back(request);
                agent.gridPending = true;
                agent.gridGoal = goal;
                component.pathPending = true;
            }
            continue;
        }

        auto surfaceIt = surfaces.find(agent.surface);
        if (surfaceIt == surfaces.end())
            continue;
        Surface& surface = surfaceIt->second;
        if (!ensureCrowd(scene, surface)) {
            component.isOnNavMesh = false;
            continue;
        }
        const NavFrame& frame = surface.frame;
        const float scale = unitScaleOfSurface(scene, agent.surface);
        const Vector3 offset = frame.up() * (component.baseOffset * scale);
        const NavCrowdAgentParams params = agentParams(component, scale, agent.areaCosts);
        if (params.radius > surface.mesh->getHeader().config.agentRadius * 1.0001f && params.radius > agent.params.radius && agent.placed) {
            // a larger agent than the crowd was sized for: the crowd is made again next tick
            surface.crowdGrow = true;
        }
        if (!agent.placed) {
            const int index = surface.crowd->addAgent(frame.toNav(transform->worldPosition - offset), params);
            if (index < 0) {
                surface.crowdGrow = true;
                continue;
            }
            agent.placed = true;
            agent.crowdIndex = index;
            agent.params = params;
            warnFilterFallback(scene, surface, entity, index);
            agent.planeZ = frame.planeZOf(transform->worldPosition);
            agent.previousPosition = agent.currentPosition = agent.writtenPosition = transform->worldPosition;
            agent.previousRotation = agent.currentRotation = agent.writtenRotation = transform->worldRotation;
            agent.hasPose = true;
            agent.onLink = false;
            component.isOnNavMesh = surface.crowd->isOnMesh(index);
            if (!component.isOnNavMesh && !agent.warnedOffMesh) {
                agent.warnedOffMesh = true;
                Log::warn("NavMeshAgent '%s' is not on the navmesh: it does not move (move it onto the surface, or warp it)",
                          scene->getEntityName(entity).c_str());
            }
            if (agent.requested && !agent.arrived && component.isOnNavMesh)
                surface.crowd->requestMoveTarget(index, frame.toNav(agent.destination));
            continue;
        }
        if (!sameParams(params, agent.params)) {
            surface.crowd->setParams(agent.crowdIndex, params);
            agent.params = params;
            warnFilterFallback(scene, surface, entity, agent.crowdIndex);
        }
        if (agent.onLink)
            continue;
        // a body or controller moved it, or a script moved the Transform: the crowd follows
        const bool followsTransform = component.driver == NavAgentDriver::RIGIDBODY || component.driver == NavAgentDriver::CHARACTER_CONTROLLER ||
                                      (component.driver == NavAgentDriver::TRANSFORM && component.updatePosition &&
                                       transform->worldPosition != agent.writtenPosition);
        if (followsTransform && component.isOnNavMesh) {
            surface.crowd->movePosition(agent.crowdIndex, frame.toNav(transform->worldPosition - offset));
            agent.currentPosition = transform->worldPosition;
        }
    }
}

void NavigationSystem::Runtime::syncObstacles(NavigationSystem& system, Scene* scene, float dt) {
    std::vector<Entity> entities;
    auto array = scene->getComponentArray<NavMeshObstacleComponent>();
    for (size_t i = 0; i < array->size(); i++)
        entities.push_back(array->getEntity(i));
    std::sort(entities.begin(), entities.end());
    for (Entity entity : entities) {
        NavMeshObstacleComponent& component = scene->getComponent<NavMeshObstacleComponent>(entity);
        Transform* transform = scene->findComponent<Transform>(entity);
        Obstacle& obstacle = obstacles[entity];
        obstacle.entity = entity;
        const bool active = component.enabled && transform && scene->isEntityActiveInHierarchy(entity);
        const Vector3 scale = transform ? Vector3(std::fabs(transform->worldScale.x), std::fabs(transform->worldScale.y), std::fabs(transform->worldScale.z))
                                        : Vector3(1.0f);
        const Vector3 position = transform ? transform->modelMatrix * component.center : Vector3();
        const Vector3 velocity = obstacle.hasLast && dt > 0.0f ? (position - obstacle.lastPosition) / dt : Vector3();
        // stationary time and whether it moved past the threshold since its carve
        const bool moved = !obstacle.carved || position.distance(obstacle.carvedPosition) > component.moveThreshold ||
                           (transform && std::fabs(obstacle.carvedRotation.dot(transform->worldRotation)) < std::cos(0.5f * component.moveThreshold));
        if (obstacle.hasLast && position.distance(obstacle.lastPosition) <= 1e-5f)
            obstacle.stationaryTime += dt;
        else
            obstacle.stationaryTime = 0.0f;
        obstacle.lastPosition = position;
        obstacle.velocity = velocity;
        obstacle.hasLast = true;

        for (auto& entry : surfaces) {
            Surface& surface = entry.second;
            const bool usable = active && surface.mesh && surface.mesh->isValid();
            const float unitScale = unitScaleOfSurface(scene, entry.first);
            const NavFrame& frame = surface.frame;
            const Vector3 navPosition = frame.toNav(position);
            bool carve = usable && component.carve && surface.mesh->isCarving();
            if (usable && component.carve && !surface.mesh->isCarving()) {
                warnOnce(warned, "carve-" + std::to_string(entity) + "-" + std::to_string(entry.first),
                         "NavMeshObstacle '" + scene->getEntityName(entity) + "' carves, but NavMeshSurface '" +
                             scene->getEntityName(entry.first) + "' has no Runtime Carving: it is avoided instead");
            }
            // carving: in the tile cache once stationary (or always), re-cut past the threshold
            auto carved = obstacle.carves.find(entry.first);
            const bool stationary = obstacle.stationaryTime >= component.timeToStationary;
            const bool wantCarve = carve && (!component.carveOnlyStationary || stationary);
            if (carved != obstacle.carves.end() && (!wantCarve || moved)) {
                surface.mesh->removeObstacle(carved->second);
                obstacle.carves.erase(carved);
                carved = obstacle.carves.end();
            }
            if (wantCarve && carved == obstacle.carves.end()) {
                uint32_t ref = 0;
                if (component.shape == NavObstacleShape::BOX) {
                    const Vector3 half = component.size * scale * 0.5f;
                    const Vector3 forward = frame.directionToNav(transform->worldRotation * Vector3(0.0f, 0.0f, 1.0f));
                    const float yaw = std::atan2(forward.x, forward.z);
                    ref = surface.mesh->addBoxObstacle(navPosition, frame.plane == NavPlane::XY ? Vector3(half.x, 1e6f, half.y) : half, yaw);
                } else {
                    const float radius = component.radius * std::max(scale.x, scale.z);
                    const float height = component.height * scale.y;
                    ref = surface.mesh->addCylinderObstacle(navPosition - Vector3(0.0f, height * 0.5f, 0.0f), radius,
                                                            frame.plane == NavPlane::XY ? 1e6f : height);
                }
                if (ref) {
                    obstacle.carves[entry.first] = ref;
                    obstacle.carvedPosition = position;
                    obstacle.carvedRotation = transform->worldRotation;
                }
            }
            obstacle.carved = !obstacle.carves.empty();

            // avoided: a crowd agent where it is not carving now (spec R9)
            const bool avoid = usable && surface.crowd && obstacle.carves.find(entry.first) == obstacle.carves.end();
            auto slot = obstacle.crowdSlots.find(entry.first);
            if (!avoid) {
                if (slot != obstacle.crowdSlots.end()) {
                    if (surface.crowd)
                        surface.crowd->removeAgent(slot->second);
                    obstacle.crowdSlots.erase(slot);
                }
                continue;
            }
            NavCrowdAgentParams params;
            params.obstacle = true;
            params.radius = (component.shape == NavObstacleShape::BOX ? 0.5f * std::sqrt(component.size.x * scale.x * component.size.x * scale.x +
                                                                                         (frame.plane == NavPlane::XY ? component.size.y * scale.y * component.size.y * scale.y
                                                                                                                       : component.size.z * scale.z * component.size.z * scale.z))
                                                                      : component.radius * std::max(scale.x, scale.z));
            params.height = component.shape == NavObstacleShape::BOX ? component.size.y * scale.y : component.height * scale.y;
            (void)unitScale;
            if (slot == obstacle.crowdSlots.end()) {
                const int index = surface.crowd->addAgent(navPosition, params);
                if (index < 0) {
                    surface.crowdGrow = true;
                    continue;
                }
                slot = obstacle.crowdSlots.emplace(entry.first, index).first;
            } else if (!sameParams(params, surface.crowd->getParams(slot->second))) {
                surface.crowd->setParams(slot->second, params);
            }
            surface.crowd->setObstacleState(slot->second, navPosition, frame.directionToNav(velocity));
        }
    }
    (void)system;
}

void NavigationSystem::Runtime::solveGrids(NavigationSystem& system, Scene* scene, uint32_t tick) {
    // edited cells send the agents whose path crosses them back to the batch (spec R20)
    for (auto& entry : grids) {
        NavGridComponent* component = scene->findComponent<NavGridComponent>(entry.first);
        if (!component || !component->map)
            continue;
        NavGridMap& map = *component->map;
        if (map.getRevision() == entry.second.mapRevision)
            continue;
        entry.second.mapRevision = map.getRevision();
        const std::vector<int> edited = map.takeEditedCells();
        if (edited.empty())
            continue;
        for (auto& agentEntry : agents) {
            Agent& agent = agentEntry.second;
            if (agent.surface != entry.first || !agent.onGrid || agent.gridCells.empty())
                continue;
            for (size_t i = agent.gridNext; i < agent.gridCells.size(); i++) {
                if (std::binary_search(edited.begin(), edited.end(), map.index(agent.gridCells[i]))) {
                    agent.gridCells.clear();
                    agent.gridNext = 0;
                    break;
                }
            }
        }
    }
    if (gridRequests.empty())
        return;

    // at most maxGridSolvesPerTick a tick, the rest the next one, in request order
    const size_t limit = (size_t)std::max(1, getSettings().maxGridSolvesPerTick);
    std::vector<GridRequest> now(gridRequests.begin(), gridRequests.begin() + (std::ptrdiff_t)std::min(limit, gridRequests.size()));
    gridRequests.erase(gridRequests.begin(), gridRequests.begin() + (std::ptrdiff_t)now.size());
    std::map<Entity, std::vector<size_t>> byGrid;
    for (size_t i = 0; i < now.size(); i++)
        byGrid[now[i].grid].push_back(i);
    std::vector<GridPath> results(now.size());
    for (auto& entry : byGrid) {
        NavGridMap* map = system.getGridMap(entry.first);
        if (!map)
            continue;
        std::vector<nav::GridRequest> requests;
        for (size_t i : entry.second) {
            nav::GridRequest request = now[i].request;
            request.query.tick = tick;
            requests.push_back(request);
        }
        const NavFilter filter = NavFilter::fromSettings(ALL_AREAS);
        std::vector<GridPath> paths;
        JE_PROFILE_SCOPE("Nav.GridSolve");
        solveGridBatch(*map, requests, filter.areaCosts, grids[entry.first].pool, paths);
        for (size_t k = 0; k < entry.second.size(); k++)
            results[entry.second[k]] = std::move(paths[k]);
    }
    for (size_t i = 0; i < now.size(); i++) {
        auto found = agents.find(now[i].agent);
        NavMeshAgentComponent* component = scene->findComponent<NavMeshAgentComponent>(now[i].agent);
        if (found == agents.end() || !component || !found->second.gridPending)
            continue;
        Agent& agent = found->second;
        agent.gridPending = false;
        component->pathPending = false;
        GridPath& path = results[i];
        component->pathStatus = path.status;
        if (path.status == PathStatus::Invalid || path.cells.empty()) {
            agent.gridCells.clear();
            if (!agent.failed) {
                agent.failed = true;
                events.push_back(Event{now[i].agent, EventType::PathFailed});
            }
            continue;
        }
        if (path.status == PathStatus::Partial && !agent.failed) {
            agent.failed = true;
            events.push_back(Event{now[i].agent, EventType::PathFailed});
        }
        agent.gridCells = std::move(path.cells);
        agent.gridNext = agent.gridCells.size() > 1 ? 1 : 0;
        agent.gridWait = 0.0f;
    }
}

void NavigationSystem::Runtime::updateCarving() {
    for (auto& entry : surfaces) {
        if (!entry.second.mesh || !entry.second.mesh->isCarving())
            continue;
        bool upToDate = true;
        int rebuilt = 0;
        entry.second.mesh->updateCarving(CARVE_BUDGET_MS, upToDate, rebuilt);
    }
}

void NavigationSystem::Runtime::stepCrowds(Scene* scene, float dt) {
    for (auto& entry : surfaces) {
        Surface& surface = entry.second;
        if (!surface.crowd || surface.crowd->getAgentCount() == 0)
            continue;
        NavCrowd& crowd = *surface.crowd;
        const float scale = unitScaleOfSurface(scene, entry.first);
        std::map<int, Entity> byIndex;
        // the speed each agent may have this step: auto braking (spec R12), isStopped
        for (auto& agentEntry : agents) {
            Agent& agent = agentEntry.second;
            if (agent.surface != entry.first || !agent.placed || agent.onGrid || agent.crowdIndex < 0)
                continue;
            byIndex[agent.crowdIndex] = agentEntry.first;
            NavMeshAgentComponent* component = scene->findComponent<NavMeshAgentComponent>(agentEntry.first);
            if (!component)
                continue;
            NavCrowdAgentParams params = agent.params;
            if (component->isStopped) {
                params.maxSpeed = 0.0f;
            } else if (component->autoBraking && agent.requested && !agent.arrived && crowd.getTargetState(agent.crowdIndex) == NavCrowdTarget::Valid) {
                bool reachedEnd = false;
                float remaining = crowd.getSteeringDistance(agent.crowdIndex, reachedEnd);
                const float accel = std::max(params.maxAcceleration, 1e-4f);
                // braking at half the acceleration (the velocity follows within the acceleration)
                const float brakeDistance = params.maxSpeed * params.maxSpeed / accel;
                if (!reachedEnd && remaining <= brakeDistance)
                    remaining = crowd.getRemainingDistance(agent.crowdIndex, reachedEnd);
                if (reachedEnd) {
                    const float stop = std::max(component->stoppingDistance, MIN_STOPPING_DISTANCE) * scale;
                    const float left = std::max(0.0f, remaining - stop);
                    // decelerate to stop at the stopping distance: v = sqrt(a d) (a constant a / 2), then
                    // the last centimetres closed by an eighth per tick, so the speed at the stop is
                    // under 0.05 m/s and it never passes it
                    const float allowed = std::min(std::sqrt(accel * left), dt > 0.0f ? left / (BRAKE_FINAL_TICKS * dt) : left);
                    // dtCrowd scales the speed down within two radii of the end itself (distance / 2r):
                    // asked for allowed / scale, it moves at `allowed` (else the two slow-downs
                    // compound and it creeps towards the stop without reaching it)
                    const float slowDown = params.radius * 2.0f;
                    const float crowdScale = slowDown > 0.0f ? std::min(1.0f, remaining / slowDown) : 1.0f;
                    params.maxSpeed = std::min(params.maxSpeed, allowed / std::max(crowdScale, 1e-3f));
                }
            }
            if (params.maxSpeed != crowd.getParams(agent.crowdIndex).maxSpeed)
                crowd.setParams(agent.crowdIndex, params);
        }

        // links reached take the agents off the crowd's steering (their own traversal)
        linkEntries.clear();
        float lookahead = 0.0f;
        for (const auto& indexed : byIndex)
            lookahead = std::max(lookahead, crowd.getParams(indexed.first).maxSpeed * dt);
        crowd.takeOffMeshLinks(lookahead, linkEntries);
        for (const NavLinkEntry& link : linkEntries) {
            auto owner = byIndex.find(link.agent);
            if (owner == byIndex.end())
                continue;
            Agent& agent = agents[owner->second];
            NavMeshAgentComponent* component = scene->findComponent<NavMeshAgentComponent>(owner->second);
            agent.onLink = true;
            agent.linkByDetour = false;
            agent.linkFrom = crowd.getPosition(link.agent);
            agent.linkStart = link.start;
            agent.linkEnd = link.end;
            agent.linkTravelled = 0.0f;
            agent.link = (Entity)link.userId;
            agent.linkWaiting = component && !component->autoTraverseOffMeshLink;
            if (component)
                component->isOnOffMeshLink = true;
            events.push_back(Event{owner->second, EventType::LinkStart});
        }

        {
            JE_PROFILE_SCOPE("Nav.Crowd");
            crowd.update(dt);
        }

        detourLinks.clear();
        crowd.takeDetourTraversals(detourLinks);
        for (int index : detourLinks) {
            auto owner = byIndex.find(index);
            if (owner == byIndex.end())
                continue;
            Agent& agent = agents[owner->second];
            agent.onLink = true;
            agent.linkByDetour = true;
            if (NavMeshAgentComponent* component = scene->findComponent<NavMeshAgentComponent>(owner->second))
                component->isOnOffMeshLink = true;
            events.push_back(Event{owner->second, EventType::LinkStart});
        }
    }
}

void NavigationSystem::Runtime::moveGridAgent(NavigationSystem& system, Scene* scene, Agent& agent, NavMeshAgentComponent& component, float dt,
                                              uint32_t tick) {
    NavGridMap* map = system.getGridMap(agent.surface);
    const NavGridComponent* gridComponent = scene->findComponent<NavGridComponent>(agent.surface);
    if (!map || !gridComponent || !component.isOnNavMesh)
        return;
    Grid& grid = grids[agent.surface];
    const NavFrame frame = gridFrame(scene, agent.surface);
    const uint32_t self = (uint32_t)agent.entity;
    const float speed = std::max(component.speed, 0.0f);
    const float accel = std::max(component.acceleration, 1e-4f);
    Vector3 position = agent.currentPosition;
    auto worldOf = [&](GridCell cell) { return gridWorld(frame, map->cellCenter(cell), map->getHeight(cell), agent.planeZ); };

    float remaining = 0.0f;
    Vector3 desired;
    if (agent.requested && !agent.arrived && !agent.gridCells.empty() && agent.gridNext < agent.gridCells.size()) {
        const GridCell target = agent.gridCells[agent.gridNext];
        const int targetIndex = map->index(target);
        const uint32_t holder = grid.occupancy[(size_t)targetIndex];
        // the remaining path, cell centre to cell centre
        Vector3 previous = position;
        for (size_t i = agent.gridNext; i < agent.gridCells.size(); i++) {
            const Vector3 next = worldOf(agent.gridCells[i]);
            remaining += previous.distance(next);
            previous = next;
        }
        if (holder != 0 && holder != self) {
            // another agent holds the cell: wait, then plan around it
            agent.gridWait += dt;
            if (agent.gridWait >= GRID_WAIT_SECONDS && component.autoRepath) {
                agent.gridCells.clear();
                agent.gridNext = 0;
                agent.gridWait = 0.0f;
            }
        } else {
            agent.gridWait = 0.0f;
            grid.occupancy[(size_t)targetIndex] = self;
            const Vector3 goal = worldOf(target);
            Vector3 toGoal = goal - position;
            const float distance = toGoal.length();
            float maxSpeed = component.isStopped ? 0.0f : speed;
            if (component.autoBraking && agent.gridNext + 1 == agent.gridCells.size()) {
                const float stop = std::max(component.stoppingDistance, MIN_STOPPING_DISTANCE);
                maxSpeed = std::min(maxSpeed, std::sqrt(2.0f * accel * std::max(0.0f, remaining - stop)));
            }
            if (distance > 1e-6f)
                desired = toGoal * (std::min(maxSpeed, distance / std::max(dt, 1e-6f)) / distance);
        }
    }
    // the velocity follows the desired one within the acceleration
    Vector3 change = desired - agent.gridVelocity;
    const float maxChange = accel * dt;
    if (change.length() > maxChange)
        change = change * (maxChange / change.length());
    agent.gridVelocity += change;
    position += agent.gridVelocity * dt;

    if (!agent.gridCells.empty() && agent.gridNext < agent.gridCells.size()) {
        const GridCell target = agent.gridCells[agent.gridNext];
        if (position.distance(worldOf(target)) <= std::max(1e-3f, 0.05f * map->getCellSize())) {
            // reached the cell: the one behind is free again
            const GridCell behind = agent.gridCells[agent.gridNext - (agent.gridNext > 0 ? 1 : 0)];
            if (behind != target && grid.occupancy[(size_t)map->index(behind)] == self)
                grid.occupancy[(size_t)map->index(behind)] = 0;
            agent.gridNext++;
        }
        // the cells ahead within the lookahead are reserved until then (spec R18)
        const float step = Engine::getUpdateTime() > 0.0f ? Engine::getUpdateTime() : dt;
        const uint32_t lookaheadTicks = (uint32_t)std::max(1.0f, std::ceil(gridComponent->reservationLookahead / std::max(step, 1e-4f)));
        const size_t ahead = (size_t)std::ceil(speed * gridComponent->reservationLookahead / std::max(map->getCellSize(), 1e-4f)) + 1;
        std::vector<int> cells;
        for (size_t i = agent.gridNext > 0 ? agent.gridNext - 1 : 0; i < agent.gridCells.size() && cells.size() <= ahead; i++)
            cells.push_back(map->index(agent.gridCells[i]));
        grid.reservations.reserve(self, cells, tick + lookaheadTicks);
    }

    component.remainingDistance = remaining;
    component.velocity = agent.gridVelocity;
    component.desiredVelocity = desired;
    component.nextPosition = position;
    const float stop = std::max(component.stoppingDistance, MIN_STOPPING_DISTANCE);
    if (agent.requested && !agent.arrived && !agent.gridCells.empty() &&
        (agent.gridNext >= agent.gridCells.size() || (agent.gridNext + 1 == agent.gridCells.size() && remaining <= stop)) &&
        component.pathStatus == PathStatus::Complete) {
        agent.arrived = true;
        agent.gridVelocity = Vector3();
        component.velocity = Vector3();
        grid.reservations.release(self);
        events.push_back(Event{agent.entity, EventType::DestinationReached});
    }
    agent.previousPosition = agent.currentPosition;
    agent.currentPosition = position;
}

void NavigationSystem::Runtime::writeBack(NavigationSystem& system, Scene* scene, float dt, uint32_t tick) {
    PhysicsSystem* physics = scene->getSystem<PhysicsSystem>().get();
    for (Entity entity : sortedAgents(scene)) {
        auto found = agents.find(entity);
        if (found == agents.end() || !found->second.placed)
            continue;
        Agent& agent = found->second;
        NavMeshAgentComponent& component = scene->getComponent<NavMeshAgentComponent>(entity);
        Transform* transform = scene->findComponent<Transform>(entity);
        if (!transform)
            continue;
        Vector3 worldVelocity;
        Vector3 up(0.0f, 1.0f, 0.0f);
        Vector3 forwardAxis(0.0f, 0.0f, 1.0f);

        if (agent.onGrid) {
            const NavFrame frame = gridFrame(scene, agent.surface);
            up = frame.up();
            forwardAxis = frame.plane == NavPlane::XY ? Vector3(1.0f, 0.0f, 0.0f) : Vector3(0.0f, 0.0f, 1.0f);
            moveGridAgent(system, scene, agent, component, dt, tick);
            worldVelocity = component.velocity;
        } else {
            auto surfaceIt = surfaces.find(agent.surface);
            if (surfaceIt == surfaces.end() || !surfaceIt->second.crowd)
                continue;
            Surface& surface = surfaceIt->second;
            NavCrowd& crowd = *surface.crowd;
            const NavFrame& frame = surface.frame;
            const float scale = unitScaleOfSurface(scene, agent.surface);
            up = frame.up();
            forwardAxis = frame.plane == NavPlane::XY ? Vector3(1.0f, 0.0f, 0.0f) : Vector3(0.0f, 0.0f, 1.0f);
            const int index = agent.crowdIndex;

            // its own link traversal: linear at its speed, start then end (spec R7)
            if (agent.onLink && !agent.linkByDetour && !agent.linkWaiting) {
                const float speed = std::max(component.speed, 1e-3f) * scale;
                const float first = agent.linkFrom.distance(agent.linkStart);
                const float second = agent.linkStart.distance(agent.linkEnd);
                agent.linkTravelled += speed * dt;
                Vector3 position;
                Vector3 velocity;
                if (agent.linkTravelled < first) {
                    position = agent.linkFrom.lerp(agent.linkStart, agent.linkTravelled / std::max(first, 1e-6f));
                    velocity = (agent.linkStart - agent.linkFrom) * (speed / std::max(first, 1e-6f));
                } else if (agent.linkTravelled < first + second) {
                    position = agent.linkStart.lerp(agent.linkEnd, (agent.linkTravelled - first) / std::max(second, 1e-6f));
                    velocity = (agent.linkEnd - agent.linkStart) * (speed / std::max(second, 1e-6f));
                } else {
                    position = agent.linkEnd;
                }
                if (agent.linkTravelled >= first + second) {
                    crowd.completeOffMeshLink(index, agent.linkEnd);
                    agent.onLink = false;
                    component.isOnOffMeshLink = false;
                    events.push_back(Event{entity, EventType::LinkEnd});
                } else {
                    crowd.setLinkPosition(index, position, velocity);
                }
            } else if (agent.onLink && agent.linkByDetour && !crowd.isTraversingLink(index)) {
                agent.onLink = false;
                agent.linkByDetour = false;
                component.isOnOffMeshLink = false;
                events.push_back(Event{entity, EventType::LinkEnd});
            }

            // the path's state and its events (spec R12)
            const NavCrowdTarget target = crowd.getTargetState(index);
            if (agent.requested && !agent.arrived) {
                if (target == NavCrowdTarget::Pending) {
                    if (!agent.wasPending && !component.autoRepath) {
                        // a replan without autoRepath: the path is dropped
                        crowd.resetMoveTarget(index);
                        agent.requested = false;
                        component.hasPath = false;
                        component.pathStatus = PathStatus::Invalid;
                        events.push_back(Event{entity, EventType::PathFailed});
                    }
                    component.pathPending = agent.requested;
                    agent.wasPending = agent.requested;
                } else if (target == NavCrowdTarget::Valid) {
                    component.pathPending = false;
                    if (agent.wasPending) {
                        agent.wasPending = false;
                        // the whole path's status (Detour's corridor holds 256 polygons)
                        const Vector3 from = frame.toWorld(crowd.getPosition(index), agent.planeZ);
                        component.pathStatus = system.calculatePathOn(agent.surface, from, agent.destination, (uint16_t)(component.areaMask & ALL_AREAS)).status;
                        if (component.pathStatus == PathStatus::Invalid)
                            component.pathStatus = crowd.isPathPartial(index) ? PathStatus::Partial : PathStatus::Complete;
                        if (component.pathStatus != PathStatus::Complete && !agent.failed) {
                            agent.failed = true;
                            events.push_back(Event{entity, EventType::PathFailed});
                        }
                    }
                } else if (target == NavCrowdTarget::Failed) {
                    component.pathPending = false;
                    component.pathStatus = PathStatus::Invalid;
                    agent.wasPending = false;
                    if (!agent.failed) {
                        agent.failed = true;
                        events.push_back(Event{entity, EventType::PathFailed});
                    }
                }
            }

            // arrival: within the stopping distance of the destination, once (spec R12)
            if (agent.requested && !agent.arrived && target == NavCrowdTarget::Valid && !agent.onLink) {
                bool reachedEnd = false;
                // along the crowd's corners, the rest as the crow flies (no corridor walk per tick)
                const float remaining = crowd.getSteeringRemaining(index, reachedEnd);
                component.remainingDistance = remaining;
                const float stop = std::max(component.stoppingDistance, MIN_STOPPING_DISTANCE) * scale;
                if (reachedEnd && remaining <= stop + (component.autoBraking ? ARRIVAL_TOLERANCE * scale : 0.0f)) {
                    agent.arrived = true;
                    crowd.resetMoveTarget(index);
                    crowd.setVelocity(index, Vector3());
                    component.remainingDistance = remaining;
                    events.push_back(Event{entity, EventType::DestinationReached});
                }
            }

            component.isOnNavMesh = crowd.isOnMesh(index) || agent.onLink;
            const Vector3 navPosition = crowd.getPosition(index);
            worldVelocity = frame.directionToWorld(crowd.getVelocity(index));
            component.velocity = worldVelocity;
            component.desiredVelocity = frame.directionToWorld(crowd.getDesiredVelocity(index));
            component.nextPosition = frame.toWorld(navPosition, agent.planeZ) + up * (component.baseOffset * scale);
            agent.previousPosition = agent.currentPosition;
            agent.currentPosition = component.nextPosition;
        }

        // the driver moves the GameObject (spec R11)
        if (!component.updatePosition && component.driver == NavAgentDriver::TRANSFORM)
            continue;
        Quaternion rotation = transform->worldRotation;
        if (component.updateRotation && worldVelocity.length() > 1e-3f && component.driver != NavAgentDriver::NONE &&
            component.driver != NavAgentDriver::RIGIDBODY)
            rotation = turnTowards(transform->worldRotation, up, forwardAxis, worldVelocity, component.angularSpeed * dt);
        agent.previousRotation = agent.currentRotation;
        agent.currentRotation = rotation;
        switch (component.driver) {
            case NavAgentDriver::TRANSFORM:
                physics->writeTransformWorldPose(entity, agent.currentPosition, rotation);
                agent.writtenPosition = transform->worldPosition;
                agent.writtenRotation = transform->worldRotation;
                break;
            case NavAgentDriver::RIGIDBODY:
#ifdef JASPERENGINE_PHYSICS_3D
                if (scene->findComponent<RigidbodyComponent>(entity)) {
                    Rigidbody(scene, entity).setLinearVelocity(worldVelocity);
                    break;
                }
#endif
#ifdef JASPERENGINE_PHYSICS_2D
                if (scene->findComponent<Rigidbody2DComponent>(entity)) {
                    Rigidbody2D(scene, entity).setLinearVelocity(Vector2(worldVelocity.x, worldVelocity.y));
                    break;
                }
#endif
                warnOnce(warned, "rigidbody-" + std::to_string(entity),
                         "NavMeshAgent '" + scene->getEntityName(entity) + "' drives a Rigidbody, but has none");
                break;
            case NavAgentDriver::CHARACTER_CONTROLLER:
#ifdef JASPERENGINE_PHYSICS_3D
                if (scene->findComponent<CharacterControllerComponent>(entity)) {
                    CharacterController(scene, entity).simpleMove(worldVelocity);
                    physics->writeTransformWorldPose(entity, transform->worldPosition, rotation);
                    break;
                }
#endif
#ifdef JASPERENGINE_PHYSICS_2D
                if (scene->findComponent<CharacterController2DComponent>(entity)) {
                    CharacterController2D(scene, entity).simpleMove(Vector2(worldVelocity.x, worldVelocity.y));
                    break;
                }
#endif
                warnOnce(warned, "controller-" + std::to_string(entity),
                         "NavMeshAgent '" + scene->getEntityName(entity) + "' drives a CharacterController, but has none");
                break;
            case NavAgentDriver::NONE:
                break;
        }
    }
}

void NavigationSystem::Runtime::dispatch(Scene* scene) {
    if (events.empty())
        return;
    // after the step, in entity order (stable within an entity): a callback may call any
    // navigation API, and the events it causes fire in the next dispatch
    std::vector<Event> batch;
    batch.swap(events);
    std::stable_sort(batch.begin(), batch.end(), [](const Event& a, const Event& b) { return a.entity < b.entity; });
    for (const Event& event : batch) {
        if (event.type == EventType::BakeCompleted) {
            if (NavMeshSurfaceComponent* surface = scene->findComponent<NavMeshSurfaceComponent>(event.entity))
                surface->onBakeCompleted.call(NavMeshSurface(scene, event.entity));
            continue;
        }
        NavMeshAgentComponent* agent = scene->findComponent<NavMeshAgentComponent>(event.entity);
        if (!agent)
            continue;
        switch (event.type) {
            case EventType::DestinationReached: agent->onDestinationReached.call(NavMeshAgent(scene, event.entity)); break;
            case EventType::PathFailed: agent->onPathFailed.call(NavMeshAgent(scene, event.entity)); break;
            case EventType::LinkStart: agent->onOffMeshLinkStart.call(NavMeshAgent(scene, event.entity)); break;
            case EventType::LinkEnd: agent->onOffMeshLinkEnd.call(NavMeshAgent(scene, event.entity)); break;
            case EventType::BakeCompleted: break;
        }
    }
}

// ---------------------------------------------------------------- S55 areas

NavMesh* NavigationSystem::editableNavMesh(Entity surface) {
    if (!surfaceComponent(scene, surface))
        return nullptr;
    getNavMesh(surface);
    auto found = rt().surfaces.find(surface);
    return found != runtime->surfaces.end() ? found->second.mesh.get() : nullptr;
}

bool NavigationSystem::hasAreaCrowd(Entity surface) const {
    if (!runtime)
        return false;
    auto found = runtime->surfaces.find(surface);
    return found != runtime->surfaces.end() && found->second.crowd && found->second.crowd->getAgentCount() > 0;
}

bool NavigationSystem::isLinkOccupied(Entity link) const {
    if (!runtime)
        return false;
    for (const auto& entry : runtime->agents) {
        if (entry.second.placed && entry.second.onLink && entry.second.link == link)
            return true;
    }
    return false;
}

Vector3 NavigationSystem::getObstacleVelocity(Entity obstacle) const {
    if (!runtime)
        return Vector3();
    auto found = runtime->obstacles.find(obstacle);
    return found != runtime->obstacles.end() ? found->second.velocity : Vector3();
}

bool NavigationSystem::updateLink(Entity link) {
    const NavMeshLinkComponent* component = scene->findComponent<NavMeshLinkComponent>(link);
    if (!component)
        return false;
    bool ok = true;
    std::vector<NavLinkState> states;
    for (Entity surface : surfacesOf(scene, component->agentType)) {
        NavMesh* mesh = editableNavMesh(surface);
        if (!mesh || !mesh->isValid())
            continue;
        // the surfaces whose navmesh holds the link
        mesh->getLinkStates(states);
        if (std::none_of(states.begin(), states.end(), [&](const NavLinkState& state) { return state.userId == (uint32_t)link; }))
            continue;
        std::vector<float> costs = mesh->getHeader().linkAreaCosts;
        const size_t baked = costs.size();
        std::vector<NavLinkInput> connections;
        NavBakeCollector::linkConnections(scene, link, NavBakeCollector::frameOf(scene, surface), mesh->getHeader().config.agentRadius,
                                          &costs, connections);
        // a cost the bake did not see: the area's (a rebake gives it its own)
        for (NavLinkInput& connection : connections) {
            if (connection.polyArea >= FIRST_LINK_COST_AREA + (int)baked)
                connection.polyArea = connection.area;
        }
        std::string error;
        if (!mesh->updateLink((uint32_t)link, connections, error)) {
            Log::warn("NavMeshLink '%s' on NavMeshSurface '%s': %s", scene->getEntityName(link).c_str(),
                      scene->getEntityName(surface).c_str(), error.c_str());
            ok = false;
        }
    }
    return ok;
}

void NavigationSystem::Runtime::replanAreaChanges(NavigationSystem& system, Scene* scene) {
    if (!system.areas)
        return;
    for (auto& entry : system.areas->changed) {
        if (entry.second.empty())
            continue;
        auto found = surfaces.find(entry.first);
        if (found == surfaces.end() || !found->second.crowd) {
            entry.second.clear();
            continue;
        }
        Surface& surface = found->second;
        NavCrowd& crowd = *surface.crowd;
        crowd.replanCrossing(entry.second, replanned);
        entry.second.clear();
        for (auto& agentEntry : agents) {
            Agent& agent = agentEntry.second;
            if (agent.surface != entry.first || agent.onGrid || !agent.placed || agent.crowdIndex < 0 || !agent.requested || agent.arrived)
                continue;
            NavMeshAgentComponent* component = scene->findComponent<NavMeshAgentComponent>(agentEntry.first);
            if (!component)
                continue;
            if (std::binary_search(replanned.begin(), replanned.end(), agent.crowdIndex)) {
                if (!component->autoRepath) {
                    // a path crossing a changed polygon is dropped without Auto Repath (R15)
                    crowd.resetMoveTarget(agent.crowdIndex);
                    agent.requested = false;
                    component->hasPath = false;
                    component->pathPending = false;
                    component->pathStatus = PathStatus::Invalid;
                    events.push_back(Event{agentEntry.first, EventType::PathFailed});
                    continue;
                }
                // its new path's status is taken when it lands, and a failure fires again
                agent.failed = false;
                agent.wasPending = true;
                component->pathPending = true;
                continue;
            }
            // a path that stopped short, or a target that failed, may go through now
            const bool stoppedShort = component->pathStatus == PathStatus::Partial || crowd.getTargetState(agent.crowdIndex) == NavCrowdTarget::Failed;
            if (stoppedShort && component->autoRepath && !agent.onLink && crowd.requestMoveTarget(agent.crowdIndex, surface.frame.toNav(agent.destination))) {
                agent.wasPending = true;
                component->pathPending = true;
            }
        }
    }
}

void NavigationSystem::fixedUpdate(double dt) {
    fixedTick++;
    // nothing to do, nothing allocated (spec R25)
    if (!runtime && !hasNavigationComponents())
        return;
    if (!hasNavigationComponents()) {
        processBakes(0.0);
        return;
    }
    if (paused)
        return;
    JE_PROFILE_SCOPE("Navigation");
    workingTicks++;
    Runtime& r = rt();
    r.inFixedUpdate = true;
    const float step = (float)dt;

    // 1. finished bake tiles land
    processBakes(0.0);
    // the project's area costs reach the crowds' filters
    if (r.settingsVersion != getSettingsVersion()) {
        r.settingsVersion = getSettingsVersion();
        for (auto& entry : r.surfaces) {
            if (entry.second.crowd)
                entry.second.crowd->refreshFilters();
        }
    }
    r.syncSurfaces(*this, scene);
    // grids that follow their tilemap or terrain
    updateGridSources();
    // 1b. runtime area volumes and link states (S55 D9); agents whose path they touch ask again
    syncAreaOverrides();
    r.replanAreaChanges(*this, scene);
    // 2. transforms -> agents and obstacles
    r.syncAgents(*this, scene);
    r.syncObstacles(*this, scene, step);
    // 3. grid paths, a batch
    r.solveGrids(*this, scene, fixedTick);
    // 4. tile caches, budgeted
    r.updateCarving();
    // 5. crowds
    r.stepCrowds(scene, step);
    // 6. drivers
    r.writeBack(*this, scene, step, fixedTick);
    r.inFixedUpdate = false;
    // 7. events
    r.dispatch(scene);
}

void NavigationSystem::update(double dt) {
    (void)dt;
    if (!runtime)
        return;
    // time-sliced bakes (web without threads) within the frame budget (spec R23)
    if (isAnyBaking())
        processBakes(getSettings().webBakeBudgetMs);
    if (paused)
        return;
    // Transform-driven agents are drawn between their last two ticks
    const double alpha = Engine::getInterpolationAlpha();
    PhysicsSystem* physics = scene->getSystem<PhysicsSystem>().get();
    for (auto& entry : runtime->agents) {
        Runtime::Agent& agent = entry.second;
        if (!agent.placed || !agent.hasPose)
            continue;
        NavMeshAgentComponent* component = scene->findComponent<NavMeshAgentComponent>(entry.first);
        Transform* transform = scene->findComponent<Transform>(entry.first);
        if (!component || !transform || component->driver != NavAgentDriver::TRANSFORM || !component->updatePosition)
            continue;
        // a Transform someone else moved since keeps that pose
        if (transform->worldPosition != agent.writtenPosition)
            continue;
        const Vector3 position = agent.previousPosition.lerp(agent.currentPosition, (float)alpha);
        const Quaternion rotation = agent.previousRotation == agent.currentRotation ? agent.currentRotation
                                                                                     : Quaternion::slerp((float)alpha, agent.previousRotation, agent.currentRotation);
        physics->writeTransformWorldPose(entry.first, position, rotation);
        agent.writtenPosition = transform->worldPosition;
        agent.writtenRotation = transform->worldRotation;
    }
}
