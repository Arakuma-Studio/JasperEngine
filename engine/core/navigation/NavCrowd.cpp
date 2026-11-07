// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT
//
// The dtCrowd setup and its obstacle avoidance presets follow OpenEngine's ObstacleAvoidance.cpp
// (Engine/Modules/Pathfinding/Source/ObstacleAvoidance.cpp), Copyright (c) 2026 Gamecrafters Guild
// Limited and contributors, MIT License, with priorities, area filters and owner-driven off-mesh
// links added. See NOTICE.md.

#include "NavCrowd.h"

#include "Log.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstring>
#include <iterator>

using namespace jasperengine;
using namespace jasperengine::nav;

#if JASPERENGINE_NAVIGATION

#include "DetourCommon.h"
#include "DetourCrowd.h"
#include "DetourNavMesh.h"
#include "DetourNavMeshQuery.h"

namespace {

    // Detour's sample presets: Low, Medium, Good, High (dtObstacleAvoidanceParams indices 0-3).
    // Every field is set: a zero weight or grid makes the sampled velocity zero.
    void setAvoidancePresets(dtCrowd* crowd) {
        dtObstacleAvoidanceParams params;
        std::memcpy(&params, crowd->getObstacleAvoidanceParams(0), sizeof(dtObstacleAvoidanceParams));
        params.velBias = 0.5f;
        params.weightDesVel = 2.0f;
        params.weightCurVel = 0.75f;
        params.weightSide = 0.75f;
        params.weightToi = 2.5f;
        params.horizTime = 2.5f;
        params.gridSize = 33;

        params.adaptiveDivs = 5;
        params.adaptiveRings = 2;
        params.adaptiveDepth = 1;
        crowd->setObstacleAvoidanceParams(0, &params);

        params.adaptiveDivs = 5;
        params.adaptiveRings = 2;
        params.adaptiveDepth = 2;
        crowd->setObstacleAvoidanceParams(1, &params);

        params.adaptiveDivs = 7;
        params.adaptiveRings = 2;
        params.adaptiveDepth = 3;
        crowd->setObstacleAvoidanceParams(2, &params);

        params.adaptiveDivs = 7;
        params.adaptiveRings = 3;
        params.adaptiveDepth = 3;
        crowd->setObstacleAvoidanceParams(3, &params);
    }

    Vector3 vec(const float* v) { return Vector3(v[0], v[1], v[2]); }

}

struct NavCrowd::Impl {
    const NavMesh* mesh = nullptr;
    dtCrowd* crowd = nullptr;
    int capacity = 0;
    int count = 0;
    std::vector<NavCrowdAgentParams> params;
    // the agents on a link: off the crowd's steering until completeOffMeshLink
    std::vector<uint8_t> onLink;
    // where each obstacle stands (dtCrowd's collision pass would push it like any agent)
    std::vector<std::array<float, 3>> pinned;
    std::vector<const dtCrowdAgent*> obstacleScratch;
    // the agents whose traversal Detour runs (a fallback, see takeDetourTraversals)
    std::vector<uint8_t> detourLink;
    // ---- S55 queries: Detour's 16 query filters, keyed by area mask and costs (0: the
    // project's), reference-counted; filter 0 (every area, the project's costs) is never freed
    struct FilterSlot {
        uint16_t mask = 0;
        float costs[AREA_COUNT] = {};
        int refs = 0;
        bool used = false;
    };
    std::vector<FilterSlot> filters;
    // each agent's filter (-1: none) and whether it is a fallback (no filter of its own was free)
    std::vector<int> agentFilter;
    std::vector<uint8_t> agentFallback;
    int fallbackCount = 0;
    std::vector<dtCrowdAgent*> active;
    // ---- S55 agents: corridors a script set (followed without the crowd's shortcuts until a
    // request or replan) and this update's velocity overrides (spec S55 R20, R22)
    std::vector<uint8_t> scripted;
    int scriptedCount = 0;
    struct VelocityOverride {
        int agent = -1;
        float velocity[3] = {0.0f, 0.0f, 0.0f};
        float dt = 0.0f;
        float start[3] = {0.0f, 0.0f, 0.0f};
    };
    std::vector<VelocityOverride> overrides;

    // the crowd's own path again: its corridor shortcuts come back
    void unscript(int index) {
        if (index < 0 || index >= capacity || !scripted[(size_t)index])
            return;
        scripted[(size_t)index] = 0;
        scriptedCount--;
        if (!params[(size_t)index].obstacle)
            crowd->getEditableAgent(index)->params.updateFlags |= DT_CROWD_OPTIMIZE_VIS | DT_CROWD_OPTIMIZE_TOPO;
    }

    ~Impl() { release(); }

    void release() {
        if (crowd)
            dtFreeCrowd(crowd);
        crowd = nullptr;
        mesh = nullptr;
        capacity = 0;
        count = 0;
        params.clear();
        onLink.clear();
        detourLink.clear();
        filters.clear();
        agentFilter.clear();
        agentFallback.clear();
        fallbackCount = 0;
    }

    const dtNavMeshQuery* query() const { return crowd->getNavMeshQuery(); }
    dtNavMeshQuery* editableQuery() const { return const_cast<dtNavMeshQuery*>(crowd->getNavMeshQuery()); }

    static bool sameKey(const FilterSlot& slot, const NavCrowdAgentParams& p) {
        return slot.mask == p.areaMask && std::memcmp(slot.costs, p.areaCosts, sizeof(slot.costs)) == 0;
    }

    // The filter of an agent's mask and costs: its own (shared with the same key), else a free
    // one, else (a fallback) one with its mask (its areas kept, other costs), else every area
    int acquireFilter(const NavCrowdAgentParams& p, bool& fallback) {
        fallback = false;
        int chosen = -1;
        for (size_t i = 0; i < filters.size() && chosen < 0; i++) {
            if (filters[i].used && sameKey(filters[i], p))
                chosen = (int)i;
        }
        for (size_t i = 1; i < filters.size() && chosen < 0; i++) {
            if (!filters[i].used) {
                FilterSlot& slot = filters[i];
                slot.used = true;
                slot.mask = p.areaMask;
                std::memcpy(slot.costs, p.areaCosts, sizeof(slot.costs));
                slot.refs = 0;
                fillFilter((int)i);
                chosen = (int)i;
            }
        }
        for (size_t i = 0; i < filters.size() && chosen < 0; i++) {
            if (filters[i].used && filters[i].mask == p.areaMask) {
                chosen = (int)i;
                fallback = true;
            }
        }
        if (chosen < 0) {
            chosen = 0;
            fallback = true;
        }
        filters[(size_t)chosen].refs++;
        return chosen;
    }

    void assignFilter(int agent, int slot, bool fallback) {
        agentFilter[(size_t)agent] = slot;
        agentFallback[(size_t)agent] = fallback ? 1 : 0;
        if (fallback)
            fallbackCount++;
    }

    // drops the agent's hold on its filter; true when that freed the filter
    bool releaseFilter(int agent) {
        const int slot = agentFilter[(size_t)agent];
        if (slot < 0)
            return false;
        if (agentFallback[(size_t)agent])
            fallbackCount--;
        agentFilter[(size_t)agent] = -1;
        agentFallback[(size_t)agent] = 0;
        FilterSlot& filter = filters[(size_t)slot];
        filter.refs--;
        if (filter.refs > 0 || slot == 0)
            return false;
        filter = FilterSlot();
        return true;
    }

    // a freed filter goes to the agents on a fallback (ascending index: the same every run)
    void promoteFallbacks() {
        for (int i = 0; i < capacity && fallbackCount > 0; i++) {
            if (!agentFallback[(size_t)i] || !agent(i)->active)
                continue;
            bool fallback = false;
            const int previous = agentFilter[(size_t)i];
            releaseFilter(i);
            const int slot = acquireFilter(params[(size_t)i], fallback);
            assignFilter(i, slot, fallback);
            if (slot != previous) {
                dtCrowdAgentParams detourParams = toDetour(params[(size_t)i], slot);
                crowd->updateAgentParameters(i, &detourParams);
            }
        }
    }

    void fillFilter(int index) {
        dtQueryFilter* filter = crowd->getEditableFilter(index);
        const FilterSlot& slot = filters[(size_t)index];
        const NavFilter source = NavFilter::fromSettings(slot.mask);
        filter->setIncludeFlags(source.areaMask);
        filter->setExcludeFlags(0);
        for (int a = 0; a < AREA_COUNT; a++)
            filter->setAreaCost(a, slot.costs[a] > 0.0f ? std::max(1.0f, slot.costs[a]) : source.areaCosts[a]);
        const std::vector<float>& linkCosts = mesh->getHeader().linkAreaCosts;
        for (size_t i = 0; i < linkCosts.size() && FIRST_LINK_COST_AREA + (int)i < MAX_POLY_AREAS; i++)
            filter->setAreaCost(FIRST_LINK_COST_AREA + (int)i, std::max(0.0f, linkCosts[i]));
    }

    dtCrowdAgentParams toDetour(const NavCrowdAgentParams& p, int filter) {
        dtCrowdAgentParams out;
        std::memset(&out, 0, sizeof(out));
        out.radius = std::max(p.radius, 1e-4f);
        out.height = std::max(p.height, 1e-4f);
        out.maxAcceleration = std::max(0.0f, p.maxAcceleration);
        out.maxSpeed = std::max(0.0f, p.maxSpeed);
        out.collisionQueryRange = out.radius * 12.0f;
        out.pathOptimizationRange = out.radius * 30.0f;
        out.separationWeight = 2.0f;
        out.avoidancePriority = p.obstacle ? 0 : std::min<uint8_t>(p.priority, 99);
        out.queryFilterType = (unsigned char)filter;
        if (p.obstacle) {
            out.updateFlags = 0;
            out.maxSpeed = 0.0f;
            out.maxAcceleration = 0.0f;
        } else {
            out.updateFlags = DT_CROWD_ANTICIPATE_TURNS | DT_CROWD_OPTIMIZE_TOPO;
            // Detour's visibility shortcut raycasts without costs: it would cut an agent's own
            // costly areas (spec S55 R9) straight through, so agents with costs of their own skip it
            if (std::none_of(std::begin(p.areaCosts), std::end(p.areaCosts), [](float cost) { return cost > 0.0f; }))
                out.updateFlags |= DT_CROWD_OPTIMIZE_VIS;
            if (p.quality != AvoidanceQuality::None) {
                out.updateFlags |= DT_CROWD_OBSTACLE_AVOIDANCE | DT_CROWD_SEPARATION;
                out.obstacleAvoidanceType = (unsigned char)((int)p.quality - 1);
            }
        }
        return out;
    }

    const dtCrowdAgent* agent(int index) const { return crowd->getAgent(index); }
    dtCrowdAgent* editable(int index) { return crowd->getEditableAgent(index); }
    const dtQueryFilter* filterOf(const dtCrowdAgent* ag) const { return crowd->getFilter(ag->params.queryFilterType); }
};

NavCrowd::NavCrowd(): impl(std::make_unique<Impl>()) {}
NavCrowd::~NavCrowd() = default;

bool NavCrowd::init(const NavMesh& mesh, int maxAgents, float maxAgentRadius) {
    impl->release();
    dtNavMesh* navMesh = static_cast<dtNavMesh*>(mesh.detourNavMesh());
    if (!navMesh)
        return false;
    impl->crowd = dtAllocCrowd();
    if (!impl->crowd || !impl->crowd->init(std::max(1, maxAgents), std::max(maxAgentRadius, 1e-3f), navMesh)) {
        impl->release();
        return false;
    }
    impl->mesh = &mesh;
    impl->capacity = std::max(1, maxAgents);
    impl->params.assign((size_t)impl->capacity, NavCrowdAgentParams());
    impl->onLink.assign((size_t)impl->capacity, 0);
    impl->pinned.assign((size_t)impl->capacity, std::array<float, 3>{0.0f, 0.0f, 0.0f});
    impl->obstacleScratch.reserve((size_t)impl->capacity);
    impl->detourLink.assign((size_t)impl->capacity, 0);
    impl->filters.assign(DT_CROWD_MAX_QUERY_FILTER_TYPE, Impl::FilterSlot());
    impl->filters[0].used = true;
    impl->filters[0].mask = ALL_AREAS;
    impl->fillFilter(0);
    impl->agentFilter.assign((size_t)impl->capacity, -1);
    impl->agentFallback.assign((size_t)impl->capacity, 0);
    impl->fallbackCount = 0;
    impl->active.resize((size_t)impl->capacity);
    impl->scripted.assign((size_t)impl->capacity, 0);
    impl->scriptedCount = 0;
    impl->overrides.clear();
    impl->overrides.reserve((size_t)impl->capacity);
    setAvoidancePresets(impl->crowd);
    return true;
}

void NavCrowd::release() {
    impl->release();
}

bool NavCrowd::isValid() const {
    return impl->crowd != nullptr;
}

int NavCrowd::getCapacity() const {
    return impl->capacity;
}

int NavCrowd::getAgentCount() const {
    return impl->count;
}

const NavMesh* NavCrowd::getMesh() const {
    return impl->mesh;
}

int NavCrowd::addAgent(const Vector3& position, const NavCrowdAgentParams& params) {
    if (!impl->crowd)
        return -1;
    bool fallback = false;
    const int filter = impl->acquireFilter(params, fallback);
    const dtCrowdAgentParams detourParams = impl->toDetour(params, filter);
    const float p[3] = {position.x, position.y, position.z};
    const int index = impl->crowd->addAgent(p, &detourParams);
    if (index < 0) {
        // the filter's hold goes back (a filter made for it alone is freed)
        Impl::FilterSlot& slot = impl->filters[(size_t)filter];
        if (--slot.refs <= 0 && filter != 0)
            slot = Impl::FilterSlot();
        return -1;
    }
    impl->assignFilter(index, filter, fallback);
    impl->params[(size_t)index] = params;
    impl->onLink[(size_t)index] = 0;
    impl->detourLink[(size_t)index] = 0;
    impl->count++;
    return index;
}

void NavCrowd::removeAgent(int agent) {
    if (!impl->crowd || agent < 0 || agent >= impl->capacity || !impl->agent(agent)->active)
        return;
    impl->unscript(agent);
    impl->crowd->removeAgent(agent);
    impl->onLink[(size_t)agent] = 0;
    impl->detourLink[(size_t)agent] = 0;
    impl->count--;
    if (impl->releaseFilter(agent))
        impl->promoteFallbacks();
}

void NavCrowd::setParams(int agent, const NavCrowdAgentParams& params) {
    if (!impl->crowd || agent < 0 || agent >= impl->capacity)
        return;
    int filter = impl->agentFilter[(size_t)agent];
    bool freed = false;
    if (filter < 0 || impl->agentFallback[(size_t)agent] || !Impl::sameKey(impl->filters[(size_t)filter], params)) {
        freed = impl->releaseFilter(agent);
        bool fallback = false;
        filter = impl->acquireFilter(params, fallback);
        impl->assignFilter(agent, filter, fallback);
    }
    const dtCrowdAgentParams detourParams = impl->toDetour(params, filter);
    impl->crowd->updateAgentParameters(agent, &detourParams);
    impl->params[(size_t)agent] = params;
    if (freed)
        impl->promoteFallbacks();
}

const NavCrowdAgentParams& NavCrowd::getParams(int agent) const {
    return impl->params[(size_t)agent];
}

bool NavCrowd::requestMoveTarget(int agent, const Vector3& target) {
    if (!impl->crowd)
        return false;
    impl->unscript(agent);
    const dtCrowdAgent* ag = impl->agent(agent);
    const float p[3] = {target.x, target.y, target.z};
    dtPolyRef ref = 0;
    float nearest[3];
    if (dtStatusFailed(impl->query()->findNearestPoly(p, impl->crowd->getQueryHalfExtents(), impl->filterOf(ag), &ref, nearest)) || !ref)
        return false;
    return impl->crowd->requestMoveTarget(agent, ref, nearest);
}

void NavCrowd::resetMoveTarget(int agent) {
    if (!impl->crowd)
        return;
    impl->unscript(agent);
    impl->crowd->resetMoveTarget(agent);
}

NavCrowdTarget NavCrowd::getTargetState(int agent) const {
    const dtCrowdAgent* ag = impl->agent(agent);
    switch (ag->targetState) {
        case DT_CROWDAGENT_TARGET_NONE:
        case DT_CROWDAGENT_TARGET_VELOCITY:
            return NavCrowdTarget::None;
        case DT_CROWDAGENT_TARGET_FAILED:
            return NavCrowdTarget::Failed;
        case DT_CROWDAGENT_TARGET_VALID:
            return NavCrowdTarget::Valid;
        default:
            return NavCrowdTarget::Pending;
    }
}

bool NavCrowd::isPathPartial(int agent) const {
    return impl->agent(agent)->partial;
}

NavCrowdPath NavCrowd::getPathState(int agent) const {
    const dtCrowdAgent* ag = impl->agent(agent);
    if (ag->partial)
        return NavCrowdPath::Partial;
    // not partial, so the search reached the target (DT_PARTIAL_RESULT unset): a corridor
    // ending elsewhere holds the path's first polygons only (spec S53 D5)
    return ag->corridor.getPathCount() > 0 && ag->corridor.getLastPoly() == ag->targetRef ? NavCrowdPath::Complete
                                                                                          : NavCrowdPath::Truncated;
}

int NavCrowd::getCorridorSize(int agent) const {
    return impl->agent(agent)->corridor.getPathCount();
}

Vector3 NavCrowd::getTargetPosition(int agent) const {
    return vec(impl->agent(agent)->targetPos);
}

void NavCrowd::update(float dt) {
    if (!impl->crowd || impl->count == 0)
        return;
    // ---- S55 agents: a script's corridor is followed as given, until a request for a new path
    // (a replan since the last update included); velocity overrides start here
    if (impl->scriptedCount > 0) {
        for (int i = 0; i < impl->capacity; i++) {
            if (!impl->scripted[(size_t)i])
                continue;
            if (impl->agent(i)->targetState != DT_CROWDAGENT_TARGET_VALID)
                impl->unscript(i);
            else
                impl->editable(i)->params.updateFlags &= (unsigned char)~(DT_CROWD_OPTIMIZE_VIS | DT_CROWD_OPTIMIZE_TOPO);
        }
    }
    for (Impl::VelocityOverride& entry : impl->overrides)
        dtVcopy(entry.start, impl->agent(entry.agent)->npos);

    impl->crowd->update(dt, nullptr);

    if (impl->scriptedCount > 0) {
        // a replan (an invalid corridor, the far end of a long path) plans the crowd's own way
        for (int i = 0; i < impl->capacity; i++) {
            if (impl->scripted[(size_t)i] && impl->agent(i)->targetState != DT_CROWDAGENT_TARGET_VALID)
                impl->unscript(i);
        }
    }
    for (const Impl::VelocityOverride& entry : impl->overrides) {
        dtCrowdAgent* ag = impl->editable(entry.agent);
        if (!ag->active || ag->state != DT_CROWDAGENT_STATE_WALKING)
            continue;
        float target[3];
        dtVmad(target, entry.start, entry.velocity, entry.dt);
        ag->corridor.movePosition(target, impl->editableQuery(), impl->filterOf(ag));
        dtVcopy(ag->npos, ag->corridor.getPos());
        dtVcopy(ag->vel, entry.velocity);
    }
    impl->overrides.clear();

    // obstacles stand where their transforms put them
    const int count = impl->crowd->getActiveAgents(impl->active.data(), impl->capacity);
    std::vector<const dtCrowdAgent*>& obstacles = impl->obstacleScratch;
    obstacles.clear();
    for (int i = 0; i < count; i++) {
        dtCrowdAgent* ag = impl->active[(size_t)i];
        const int index = (int)(ag - impl->crowd->getEditableAgent(0));
        if (impl->params[(size_t)index].obstacle) {
            dtVcopy(ag->npos, impl->pinned[(size_t)index].data());
            obstacles.push_back(ag);
        }
    }
    if (obstacles.empty())
        return;

    // Avoidance is a velocity preference, so a crowd squeezing past a non-carving obstacle can
    // graze it; agents are put back on the obstacle's circle (along the navmesh), the way
    // dtCrowd's collision pass separates agents, but the obstacle never moves (spec R9, AC10)
    for (int pass = 0; pass < 2; pass++) {
        for (int i = 0; i < count; i++) {
            dtCrowdAgent* ag = impl->active[(size_t)i];
            const int index = (int)(ag - impl->crowd->getEditableAgent(0));
            if (impl->params[(size_t)index].obstacle || ag->state != DT_CROWDAGENT_STATE_WALKING)
                continue;
            float push[3] = {0.0f, 0.0f, 0.0f};
            bool pushed = false;
            for (const dtCrowdAgent* other : obstacles) {
                const float minimum = ag->params.radius + other->params.radius;
                // vertically apart (another floor): no contact
                if (std::fabs(ag->npos[1] - other->npos[1]) > (ag->params.height + other->params.height) * 0.5f)
                    continue;
                float dx = ag->npos[0] - other->npos[0];
                float dz = ag->npos[2] - other->npos[2];
                const float distance = std::sqrt(dx * dx + dz * dz);
                if (distance >= minimum)
                    continue;
                if (distance < 1e-5f) {
                    // dead centre: out along the agent's velocity, else +x
                    const float speed = std::sqrt(ag->vel[0] * ag->vel[0] + ag->vel[2] * ag->vel[2]);
                    dx = speed > 1e-5f ? -ag->vel[0] / speed : 1.0f;
                    dz = speed > 1e-5f ? -ag->vel[2] / speed : 0.0f;
                } else {
                    dx /= distance;
                    dz /= distance;
                }
                // a hair past the circle, so the next tick starts outside
                const float depth = minimum - distance + 1e-3f;
                push[0] += dx * depth;
                push[2] += dz * depth;
                pushed = true;
            }
            if (!pushed)
                continue;
            float target[3] = {ag->npos[0] + push[0], ag->npos[1], ag->npos[2] + push[2]};
            ag->corridor.movePosition(target, impl->editableQuery(), impl->filterOf(ag));
            dtVcopy(ag->npos, ag->corridor.getPos());
        }
    }
}

Vector3 NavCrowd::getPosition(int agent) const {
    return vec(impl->agent(agent)->npos);
}

Vector3 NavCrowd::getVelocity(int agent) const {
    return vec(impl->agent(agent)->vel);
}

Vector3 NavCrowd::getDesiredVelocity(int agent) const {
    return vec(impl->agent(agent)->dvel);
}

bool NavCrowd::isOnMesh(int agent) const {
    const dtCrowdAgent* ag = impl->agent(agent);
    return ag->active && ag->state != DT_CROWDAGENT_STATE_INVALID;
}

uint8_t NavCrowd::getArea(int agent) const {
    const dtCrowdAgent* ag = impl->agent(agent);
    const dtNavMesh* mesh = static_cast<const dtNavMesh*>(impl->mesh->detourNavMesh());
    const dtMeshTile* tile = nullptr;
    const dtPoly* poly = nullptr;
    const dtPolyRef ref = ag->corridor.getFirstPoly();
    if (!ref || dtStatusFailed(mesh->getTileAndPolyByRef(ref, &tile, &poly)))
        return AREA_WALKABLE;
    return poly->getArea() < AREA_COUNT ? poly->getArea() : AREA_WALKABLE;
}

void NavCrowd::movePosition(int agent, const Vector3& position) {
    dtCrowdAgent* ag = impl->editable(agent);
    if (!ag->active || ag->state != DT_CROWDAGENT_STATE_WALKING)
        return;
    const float p[3] = {position.x, position.y, position.z};
    ag->corridor.movePosition(p, impl->editableQuery(), impl->filterOf(ag));
    dtVcopy(ag->npos, ag->corridor.getPos());
}

bool NavCrowd::teleport(int agent, const Vector3& position) {
    dtCrowdAgent* ag = impl->editable(agent);
    if (!ag->active)
        return false;
    const float p[3] = {position.x, position.y, position.z};
    dtPolyRef ref = 0;
    float nearest[3] = {p[0], p[1], p[2]};
    impl->query()->findNearestPoly(p, impl->crowd->getQueryHalfExtents(), impl->filterOf(ag), &ref, nearest);
    impl->unscript(agent);
    impl->onLink[(size_t)agent] = 0;
    impl->detourLink[(size_t)agent] = 0;
    if (!ref) {
        ag->corridor.reset(0, p);
        dtVcopy(ag->npos, p);
        ag->state = DT_CROWDAGENT_STATE_INVALID;
        return false;
    }
    const bool hadTarget = ag->targetState != DT_CROWDAGENT_TARGET_NONE && ag->targetState != DT_CROWDAGENT_TARGET_VELOCITY &&
                           ag->targetState != DT_CROWDAGENT_TARGET_FAILED;
    const dtPolyRef targetRef = ag->targetRef;
    float targetPos[3];
    dtVcopy(targetPos, ag->targetPos);
    ag->corridor.reset(ref, nearest);
    ag->boundary.reset();
    ag->partial = false;
    ag->ncorners = 0;
    dtVcopy(ag->npos, nearest);
    dtVset(ag->vel, 0, 0, 0);
    dtVset(ag->dvel, 0, 0, 0);
    dtVset(ag->nvel, 0, 0, 0);
    ag->state = DT_CROWDAGENT_STATE_WALKING;
    if (hadTarget && targetRef)
        impl->crowd->requestMoveTarget(agent, targetRef, targetPos);
    return true;
}

void NavCrowd::setObstacleState(int agent, const Vector3& position, const Vector3& velocity) {
    dtCrowdAgent* ag = impl->editable(agent);
    if (!ag->active)
        return;
    const float p[3] = {position.x, position.y, position.z};
    dtPolyRef ref = 0;
    float nearest[3] = {p[0], p[1], p[2]};
    impl->query()->findNearestPoly(p, impl->crowd->getQueryHalfExtents(), impl->filterOf(ag), &ref, nearest);
    // the obstacle stands where its transform is, on the mesh or not: only its circle matters
    ag->corridor.reset(ref, nearest);
    dtVcopy(ag->npos, p);
    impl->pinned[(size_t)agent] = {p[0], p[1], p[2]};
    const float v[3] = {velocity.x, velocity.y, velocity.z};
    dtVcopy(ag->vel, v);
    dtVcopy(ag->dvel, v);
    dtVcopy(ag->nvel, v);
    ag->state = ref ? DT_CROWDAGENT_STATE_WALKING : DT_CROWDAGENT_STATE_INVALID;
}

void NavCrowd::setVelocity(int agent, const Vector3& velocity) {
    dtCrowdAgent* ag = impl->editable(agent);
    const float v[3] = {velocity.x, velocity.y, velocity.z};
    dtVcopy(ag->vel, v);
}

float NavCrowd::getRemainingDistance(int agent, bool& reachedEnd, int maxCorners) const {
    reachedEnd = false;
    const dtCrowdAgent* ag = impl->agent(agent);
    if (!ag->active || ag->corridor.getPathCount() == 0)
        return 0.0f;
    static thread_local std::vector<float> verts;
    static thread_local std::vector<unsigned char> flags;
    static thread_local std::vector<dtPolyRef> polys;
    const int capacity = std::max(1, maxCorners);
    verts.resize((size_t)capacity * 3);
    flags.resize((size_t)capacity);
    polys.resize((size_t)capacity);
    dtPathCorridor& corridor = const_cast<dtPathCorridor&>(ag->corridor);
    const int count = corridor.findCorners(verts.data(), flags.data(), polys.data(), capacity, impl->editableQuery(), impl->filterOf(ag));
    float distance = 0.0f;
    float previous[3];
    dtVcopy(previous, ag->npos);
    for (int i = 0; i < count; i++) {
        distance += dtVdist(previous, &verts[(size_t)i * 3]);
        dtVcopy(previous, &verts[(size_t)i * 3]);
    }
    if (count > 0 && (flags[(size_t)count - 1] & DT_STRAIGHTPATH_END)) {
        reachedEnd = true;
    } else {
        // beyond the corners found: the rest as the crow flies
        distance += dtVdist(previous, ag->corridor.getTarget());
    }
    if (count == 0) {
        distance = dtVdist(ag->npos, ag->corridor.getTarget());
        reachedEnd = true;
    }
    return distance;
}

float NavCrowd::getSteeringDistance(int agent, bool& reachedEnd) const {
    const dtCrowdAgent* ag = impl->agent(agent);
    reachedEnd = false;
    if (!ag->active || ag->ncorners == 0)
        return 0.0f;
    float distance = 0.0f;
    float previous[3];
    dtVcopy(previous, ag->npos);
    for (int i = 0; i < ag->ncorners; i++) {
        distance += dtVdist(previous, &ag->cornerVerts[i * 3]);
        dtVcopy(previous, &ag->cornerVerts[i * 3]);
    }
    reachedEnd = (ag->cornerFlags[ag->ncorners - 1] & DT_STRAIGHTPATH_END) != 0;
    return distance;
}

float NavCrowd::getSteeringRemaining(int agent, bool& reachedEnd) const {
    const dtCrowdAgent* ag = impl->agent(agent);
    reachedEnd = false;
    if (!ag->active || ag->corridor.getPathCount() == 0)
        return 0.0f;
    float distance = 0.0f;
    if (ag->ncorners == 0) {
        distance = dtVdist(ag->npos, ag->corridor.getTarget());
    } else {
        distance = getSteeringDistance(agent, reachedEnd);
        if (!reachedEnd)
            distance += dtVdist(&ag->cornerVerts[(ag->ncorners - 1) * 3], ag->corridor.getTarget());
    }
    // a truncated corridor: on to the target as the crow flies (a lower bound, spec S53 R7)
    if (getPathState(agent) == NavCrowdPath::Truncated) {
        distance += dtVdist(ag->corridor.getTarget(), ag->targetPos);
        reachedEnd = false;
    }
    return distance;
}

void NavCrowd::getCorners(int agent, std::vector<Vector3>& corners, int maxCorners) const {
    corners.clear();
    const dtCrowdAgent* ag = impl->agent(agent);
    if (!ag->active || ag->corridor.getPathCount() == 0)
        return;
    std::vector<float> verts((size_t)std::max(1, maxCorners) * 3);
    std::vector<unsigned char> flags((size_t)std::max(1, maxCorners));
    std::vector<dtPolyRef> polys((size_t)std::max(1, maxCorners));
    dtPathCorridor& corridor = const_cast<dtPathCorridor&>(ag->corridor);
    const int count = corridor.findCorners(verts.data(), flags.data(), polys.data(), std::max(1, maxCorners), impl->editableQuery(),
                                           impl->filterOf(ag));
    corners.reserve((size_t)count);
    for (int i = 0; i < count; i++)
        corners.push_back(vec(&verts[(size_t)i * 3]));
}

void NavCrowd::takeOffMeshLinks(float lookahead, std::vector<NavLinkEntry>& entries) {
    if (!impl->crowd || !impl->mesh->hasOffMeshConnections())
        return;
    const int count = impl->crowd->getActiveAgents(impl->active.data(), impl->capacity);
    for (int i = 0; i < count; i++) {
        dtCrowdAgent* ag = impl->active[(size_t)i];
        const int index = (int)(ag - impl->crowd->getEditableAgent(0));
        if (ag->state != DT_CROWDAGENT_STATE_WALKING || ag->targetState == DT_CROWDAGENT_TARGET_NONE ||
            ag->targetState == DT_CROWDAGENT_TARGET_VELOCITY || impl->params[(size_t)index].obstacle)
            continue;
        // the corners dtCrowd would steer by from here, so the link is taken before its own trigger
        ag->ncorners = ag->corridor.findCorners(ag->cornerVerts, ag->cornerFlags, ag->cornerPolys, DT_CROWDAGENT_MAX_CORNERS,
                                                impl->editableQuery(), impl->filterOf(ag));
        if (ag->ncorners == 0 || !(ag->cornerFlags[ag->ncorners - 1] & DT_STRAIGHTPATH_OFFMESH_CONNECTION))
            continue;
        const float trigger = ag->params.radius * 2.25f + std::max(0.0f, lookahead);
        if (dtVdist2DSqr(ag->npos, &ag->cornerVerts[(ag->ncorners - 1) * 3]) >= trigger * trigger)
            continue;
        const dtPolyRef link = ag->cornerPolys[ag->ncorners - 1];
        dtPolyRef refs[2];
        float start[3], end[3];
        if (!ag->corridor.moveOverOffmeshConnection(link, refs, start, end, impl->editableQuery()))
            continue;
        const dtNavMesh* mesh = static_cast<const dtNavMesh*>(impl->mesh->detourNavMesh());
        const dtOffMeshConnection* con = mesh->getOffMeshConnectionByRef(link);
        NavLinkEntry entry;
        entry.agent = index;
        entry.userId = con ? con->userId : 0;
        entry.start = vec(start);
        entry.end = vec(end);
        entries.push_back(entry);
        ag->state = DT_CROWDAGENT_STATE_OFFMESH;
        ag->ncorners = 0;
        ag->nneis = 0;
        dtVset(ag->dvel, 0, 0, 0);
        dtVset(ag->nvel, 0, 0, 0);
        impl->onLink[(size_t)index] = 1;
    }
}

void NavCrowd::takeDetourTraversals(std::vector<int>& agents) {
    if (!impl->crowd)
        return;
    for (int index = 0; index < impl->capacity; index++) {
        const dtCrowdAgent* ag = impl->agent(index);
        const bool detour = ag->active && ag->state == DT_CROWDAGENT_STATE_OFFMESH && !impl->onLink[(size_t)index];
        if (detour && !impl->detourLink[(size_t)index])
            agents.push_back(index);
        impl->detourLink[(size_t)index] = detour ? 1 : 0;
    }
}

bool NavCrowd::isTraversingLink(int agent) const {
    const dtCrowdAgent* ag = impl->agent(agent);
    return ag->active && ag->state == DT_CROWDAGENT_STATE_OFFMESH;
}

void NavCrowd::setLinkPosition(int agent, const Vector3& position, const Vector3& velocity) {
    dtCrowdAgent* ag = impl->editable(agent);
    const float p[3] = {position.x, position.y, position.z};
    const float v[3] = {velocity.x, velocity.y, velocity.z};
    dtVcopy(ag->npos, p);
    dtVcopy(ag->vel, v);
}

void NavCrowd::completeOffMeshLink(int agent, const Vector3& end) {
    dtCrowdAgent* ag = impl->editable(agent);
    if (!ag->active)
        return;
    const float p[3] = {end.x, end.y, end.z};
    // the corridor already starts at the link's end (moveOverOffmeshConnection)
    dtVcopy(ag->npos, p);
    if (ag->corridor.getPathCount() > 0)
        ag->corridor.movePosition(p, impl->editableQuery(), impl->filterOf(ag));
    dtVcopy(ag->npos, ag->corridor.getPos());
    ag->state = ag->corridor.getFirstPoly() ? DT_CROWDAGENT_STATE_WALKING : DT_CROWDAGENT_STATE_INVALID;
    impl->onLink[(size_t)agent] = 0;
}

void NavCrowd::refreshFilters() {
    if (!impl->crowd)
        return;
    for (size_t i = 0; i < impl->filters.size(); i++) {
        if (impl->filters[i].used)
            impl->fillFilter((int)i);
    }
}

int NavCrowd::getFilterCount() const {
    int count = 0;
    for (const Impl::FilterSlot& slot : impl->filters)
        count += slot.used ? 1 : 0;
    return count;
}

int NavCrowd::getFilterOverflowCount() const {
    return impl->fallbackCount;
}

bool NavCrowd::isFilterFallback(int agent) const {
    return impl->crowd && agent >= 0 && agent < impl->capacity && impl->agentFallback[(size_t)agent] != 0;
}

size_t NavCrowd::getMemoryBytes() const {
    if (!impl->crowd)
        return 0;
    // agents with their corridors (256 polys) and boundaries, the animations, the grid and the
    // path queue's node pool
    return sizeof(dtCrowd) + (size_t)impl->capacity * (sizeof(dtCrowdAgent) + 256 * sizeof(dtPolyRef) + sizeof(dtCrowdAgentAnimation)) +
           (size_t)impl->capacity * 4 * sizeof(unsigned short) + 4096 * 48 + impl->params.capacity() * sizeof(NavCrowdAgentParams);
}

void NavCrowd::replanCrossing(const std::vector<uint32_t>& changedPolys, std::vector<int>& replanned) {
    replanned.clear();
    if (!impl->crowd || changedPolys.empty())
        return;
    for (int index = 0; index < impl->capacity; index++) {
        const dtCrowdAgent* ag = impl->agent(index);
        if (!ag->active || impl->params[(size_t)index].obstacle || impl->onLink[(size_t)index] || ag->state != DT_CROWDAGENT_STATE_WALKING ||
            ag->targetState != DT_CROWDAGENT_TARGET_VALID)
            continue;
        const dtPolyRef* path = ag->corridor.getPath();
        const int count = ag->corridor.getPathCount();
        for (int i = 0; i < count; i++) {
            if (std::binary_search(changedPolys.begin(), changedPolys.end(), (uint32_t)path[i])) {
                impl->crowd->requestMoveTarget(index, ag->targetRef, ag->targetPos);
                replanned.push_back(index);
                break;
            }
        }
    }
}

// ---- S55 agents

namespace {

    // dtCrowd's corridor capacity (DetourCrowd.cpp: m_maxPathResult)
    constexpr int CROWD_CORRIDOR_POLYS = 256;
    // setPath: the agent's polygon among the path's first ones (spec S55 R20)
    constexpr int CORRIDOR_START_SEARCH = 16;

    // the area a polygon reports: the index of its flag bit (links report their area; flags 0,
    // a runtime Not Walkable, reports Not Walkable)
    uint8_t crowdPolyArea(const dtNavMesh* mesh, dtPolyRef ref) {
        const dtMeshTile* tile = nullptr;
        const dtPoly* poly = nullptr;
        if (!ref || dtStatusFailed(mesh->getTileAndPolyByRef(ref, &tile, &poly)) || poly->flags == 0)
            return AREA_NOT_WALKABLE;
        uint8_t area = 0;
        while (area < AREA_COUNT - 1 && !(poly->flags & (1u << area)))
            area++;
        return area;
    }

}

bool NavCrowd::setCorridor(int agent, const std::vector<uint32_t>& polys, const Vector3& target) {
    if (!impl->crowd || agent < 0 || agent >= impl->capacity || polys.empty())
        return false;
    dtCrowdAgent* ag = impl->editable(agent);
    if (!ag->active || ag->state != DT_CROWDAGENT_STATE_WALKING || impl->params[(size_t)agent].obstacle)
        return false;
    const dtPolyRef current = ag->corridor.getFirstPoly();
    size_t first = polys.size();
    for (size_t i = 0; i < polys.size() && i < (size_t)CORRIDOR_START_SEARCH; i++) {
        if (polys[i] == current) {
            first = i;
            break;
        }
    }
    if (first == polys.size())
        return false;
    const int count = (int)std::min(polys.size() - first, (size_t)CROWD_CORRIDOR_POLYS);
    const dtQueryFilter* filter = impl->filterOf(ag);
    dtPolyRef refs[CROWD_CORRIDOR_POLYS];
    for (int i = 0; i < count; i++) {
        refs[i] = (dtPolyRef)polys[first + (size_t)i];
        if (!impl->query()->isValidPolyRef(refs[i], filter))
            return false;
    }
    const float end[3] = {target.x, target.y, target.z};
    const dtPolyRef last = refs[count - 1];
    const dtPolyRef endRef = (dtPolyRef)polys.back();
    // a longer path keeps its real end as the target: the corridor ends at the point of its last
    // polygon nearest it, and dtCrowd plans on from there when fewer than 10 polygons remain
    float corridorEnd[3];
    dtVcopy(corridorEnd, end);
    if (last != endRef && dtStatusFailed(impl->query()->closestPointOnPoly(last, end, corridorEnd, nullptr)))
        return false;
    ag->corridor.setCorridor(corridorEnd, refs, count);
    ag->boundary.reset();
    ag->partial = false;
    ag->ncorners = 0;
    ag->targetRef = endRef;
    dtVcopy(ag->targetPos, end);
    ag->targetPathqRef = DT_PATHQ_INVALID;
    ag->targetReplan = false;
    ag->targetReplanTime = 0.0f;
    ag->targetState = DT_CROWDAGENT_TARGET_VALID;
    if (!impl->scripted[(size_t)agent]) {
        impl->scripted[(size_t)agent] = 1;
        impl->scriptedCount++;
    }
    return true;
}

void NavCrowd::getCorridor(int agent, std::vector<uint32_t>& polys) const {
    polys.clear();
    if (!impl->crowd || agent < 0 || agent >= impl->capacity)
        return;
    const dtCrowdAgent* ag = impl->agent(agent);
    if (!ag->active)
        return;
    const dtPolyRef* path = ag->corridor.getPath();
    polys.assign(path, path + ag->corridor.getPathCount());
}

bool NavCrowd::isCorridorValid(int agent) const {
    if (!impl->crowd || agent < 0 || agent >= impl->capacity)
        return false;
    const dtCrowdAgent* ag = impl->agent(agent);
    if (!ag->active)
        return false;
    dtPathCorridor& corridor = const_cast<dtPathCorridor&>(ag->corridor);
    return corridor.isValid(corridor.getPathCount(), impl->editableQuery(), impl->filterOf(ag));
}

void NavCrowd::overrideVelocity(int agent, const Vector3& velocity, float dt) {
    if (!impl->crowd || agent < 0 || agent >= impl->capacity || !impl->agent(agent)->active)
        return;
    for (Impl::VelocityOverride& entry : impl->overrides) {
        if (entry.agent == agent) {
            entry.velocity[0] = velocity.x;
            entry.velocity[1] = velocity.y;
            entry.velocity[2] = velocity.z;
            entry.dt = dt;
            return;
        }
    }
    Impl::VelocityOverride entry;
    entry.agent = agent;
    entry.velocity[0] = velocity.x;
    entry.velocity[1] = velocity.y;
    entry.velocity[2] = velocity.z;
    entry.dt = dt;
    impl->overrides.push_back(entry);
}

void NavCrowd::requestMoveVelocity(int agent, const Vector3& velocity) {
    if (!impl->crowd || agent < 0 || agent >= impl->capacity)
        return;
    impl->unscript(agent);
    const float v[3] = {velocity.x, velocity.y, velocity.z};
    impl->crowd->requestMoveVelocity(agent, v);
}

Vector3 NavCrowd::getSteeringTarget(int agent) const {
    const dtCrowdAgent* ag = impl->agent(agent);
    if (!ag->active || ag->ncorners == 0)
        return vec(ag->npos);
    return vec(ag->cornerVerts);
}

void NavCrowd::getCorridorPath(int agent, NavPath& out, bool areaCrossings) const {
    out = NavPath();
    if (!impl->crowd || agent < 0 || agent >= impl->capacity)
        return;
    const dtCrowdAgent* ag = impl->agent(agent);
    const int count = ag->corridor.getPathCount();
    if (!ag->active || count == 0)
        return;
    const dtPolyRef* path = ag->corridor.getPath();
    out.polys.assign(path, path + count);
    out.meshRevision = impl->mesh->getRevision();
    const dtNavMesh* mesh = static_cast<const dtNavMesh*>(impl->mesh->detourNavMesh());
    const dtQueryFilter* filter = impl->filterOf(ag);
    // a corner per polygon crossing at most, and the two ends
    const int capacity = count * 2 + 2;
    std::vector<float> verts((size_t)capacity * 3);
    std::vector<unsigned char> flags((size_t)capacity);
    std::vector<dtPolyRef> refs((size_t)capacity);
    int corners = 0;
    if (dtStatusFailed(impl->query()->findStraightPath(ag->npos, ag->corridor.getTarget(), path, count, verts.data(), flags.data(), refs.data(),
                                                       &corners, capacity, areaCrossings ? DT_STRAIGHTPATH_AREA_CROSSINGS : 0)) ||
        corners == 0) {
        out.polys.clear();
        return;
    }
    out.status = PathStatus::Complete;
    out.failure = PathFailure::None;
    out.corners.reserve((size_t)corners);
    for (int i = 0; i < corners; i++) {
        out.corners.push_back(vec(&verts[(size_t)i * 3]));
        out.cornerFlags.push_back(flags[(size_t)i]);
        // a stretch has the area of the polygon it starts in; the last corner the area at the end
        out.cornerAreas.push_back(crowdPolyArea(mesh, i + 1 < corners ? refs[(size_t)i] : path[count - 1]));
        if (i > 0) {
            const float length = dtVdist(&verts[(size_t)(i - 1) * 3], &verts[(size_t)i * 3]);
            out.length += length;
            const dtMeshTile* tile = nullptr;
            const dtPoly* poly = nullptr;
            const dtPolyRef from = refs[(size_t)i - 1];
            const float cost = from && dtStatusSucceed(mesh->getTileAndPolyByRef(from, &tile, &poly)) ? filter->getAreaCost(poly->getArea()) : 1.0f;
            out.cost += length * cost;
            out.areaLengths[out.cornerAreas[(size_t)i - 1]] += length;
        }
    }
    out.start = out.corners.front();
    out.end = out.closestPoint = out.corners.back();
}

bool NavCrowd::getNextOffMeshLink(int agent, Vector3& start, Vector3& end, uint32_t& userId, int maxCorners) const {
    if (!impl->crowd || agent < 0 || agent >= impl->capacity || !impl->mesh->hasOffMeshConnections())
        return false;
    const dtCrowdAgent* ag = impl->agent(agent);
    if (!ag->active || ag->corridor.getPathCount() == 0)
        return false;
    const int capacity = std::max(1, maxCorners);
    std::vector<float> verts((size_t)capacity * 3);
    std::vector<unsigned char> flags((size_t)capacity);
    std::vector<dtPolyRef> polys((size_t)capacity);
    dtPathCorridor& corridor = const_cast<dtPathCorridor&>(ag->corridor);
    // the corners stop at the first link (its start carries the flag)
    const int count = corridor.findCorners(verts.data(), flags.data(), polys.data(), capacity, impl->editableQuery(), impl->filterOf(ag));
    if (count == 0 || !(flags[(size_t)count - 1] & DT_STRAIGHTPATH_OFFMESH_CONNECTION))
        return false;
    const dtPolyRef link = polys[(size_t)count - 1];
    const dtNavMesh* mesh = static_cast<const dtNavMesh*>(impl->mesh->detourNavMesh());
    const dtOffMeshConnection* connection = mesh->getOffMeshConnectionByRef(link);
    if (!connection)
        return false;
    // the link's polygon follows the one before it in the corridor: that fixes its direction
    const dtPolyRef* path = ag->corridor.getPath();
    dtPolyRef previous = 0;
    for (int i = 1; i < ag->corridor.getPathCount(); i++) {
        if (path[i] == link) {
            previous = path[i - 1];
            break;
        }
    }
    float a[3], b[3];
    if (dtStatusFailed(mesh->getOffMeshConnectionPolyEndPoints(previous, link, a, b)))
        return false;
    start = vec(a);
    end = vec(b);
    userId = connection->userId;
    return true;
}

#else // JASPERENGINE_NAVIGATION

namespace {
    void logUnavailable() {
        static std::atomic<bool> logged{false};
        if (!logged.exchange(true))
            Log::error("Navigation: this build has no navmesh support (JASPERENGINE_NAVIGATION is off)");
    }
    const NavCrowdAgentParams defaultParams;
}

struct NavCrowd::Impl {};

NavCrowd::NavCrowd(): impl(std::make_unique<Impl>()) {}
NavCrowd::~NavCrowd() = default;
bool NavCrowd::init(const NavMesh&, int, float) { logUnavailable(); return false; }
void NavCrowd::release() {}
bool NavCrowd::isValid() const { return false; }
int NavCrowd::getCapacity() const { return 0; }
int NavCrowd::getAgentCount() const { return 0; }
const NavMesh* NavCrowd::getMesh() const { return nullptr; }
int NavCrowd::addAgent(const Vector3&, const NavCrowdAgentParams&) { logUnavailable(); return -1; }
void NavCrowd::removeAgent(int) {}
void NavCrowd::setParams(int, const NavCrowdAgentParams&) {}
const NavCrowdAgentParams& NavCrowd::getParams(int) const { return defaultParams; }
bool NavCrowd::requestMoveTarget(int, const Vector3&) { return false; }
void NavCrowd::resetMoveTarget(int) {}
NavCrowdTarget NavCrowd::getTargetState(int) const { return NavCrowdTarget::Failed; }
bool NavCrowd::isPathPartial(int) const { return false; }
NavCrowdPath NavCrowd::getPathState(int) const { return NavCrowdPath::Partial; }
int NavCrowd::getCorridorSize(int) const { return 0; }
Vector3 NavCrowd::getTargetPosition(int) const { return Vector3(); }
void NavCrowd::update(float) {}
Vector3 NavCrowd::getPosition(int) const { return Vector3(); }
Vector3 NavCrowd::getVelocity(int) const { return Vector3(); }
Vector3 NavCrowd::getDesiredVelocity(int) const { return Vector3(); }
bool NavCrowd::isOnMesh(int) const { return false; }
uint8_t NavCrowd::getArea(int) const { return AREA_WALKABLE; }
void NavCrowd::movePosition(int, const Vector3&) {}
bool NavCrowd::teleport(int, const Vector3&) { return false; }
void NavCrowd::setObstacleState(int, const Vector3&, const Vector3&) {}
void NavCrowd::setVelocity(int, const Vector3&) {}
float NavCrowd::getRemainingDistance(int, bool& reachedEnd, int) const { reachedEnd = false; return 0.0f; }
float NavCrowd::getSteeringDistance(int, bool& reachedEnd) const { reachedEnd = false; return 0.0f; }
float NavCrowd::getSteeringRemaining(int, bool& reachedEnd) const { reachedEnd = false; return 0.0f; }
void NavCrowd::getCorners(int, std::vector<Vector3>& corners, int) const { corners.clear(); }
void NavCrowd::takeOffMeshLinks(float, std::vector<NavLinkEntry>&) {}
void NavCrowd::takeDetourTraversals(std::vector<int>&) {}
bool NavCrowd::isTraversingLink(int) const { return false; }
void NavCrowd::setLinkPosition(int, const Vector3&, const Vector3&) {}
void NavCrowd::completeOffMeshLink(int, const Vector3&) {}
void NavCrowd::refreshFilters() {}
int NavCrowd::getFilterCount() const { return 0; }
int NavCrowd::getFilterOverflowCount() const { return 0; }
bool NavCrowd::isFilterFallback(int) const { return false; }
size_t NavCrowd::getMemoryBytes() const { return 0; }
void NavCrowd::replanCrossing(const std::vector<uint32_t>&, std::vector<int>& replanned) { replanned.clear(); }

bool NavCrowd::setCorridor(int, const std::vector<uint32_t>&, const Vector3&) { return false; }
void NavCrowd::getCorridor(int, std::vector<uint32_t>& polys) const { polys.clear(); }
bool NavCrowd::isCorridorValid(int) const { return false; }
void NavCrowd::overrideVelocity(int, const Vector3&, float) {}
void NavCrowd::requestMoveVelocity(int, const Vector3&) {}
Vector3 NavCrowd::getSteeringTarget(int) const { return Vector3(); }
void NavCrowd::getCorridorPath(int, NavPath& out, bool) const { out = NavPath(); }
bool NavCrowd::getNextOffMeshLink(int, Vector3&, Vector3&, uint32_t&, int) const { return false; }

#endif // JASPERENGINE_NAVIGATION
