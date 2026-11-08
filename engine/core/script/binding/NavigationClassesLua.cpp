// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// Lua's navigation (docs/specs/S19-navigation.md R38, docs/navigation.md): the scene's
// NavigationSystem queries, NavMeshQueryFilter, NavMeshAgent with its events, NavMeshSurface
// (runtime bakes), NavGrid and GridCell, and their enums.

#include "LuaBinding.h"

#include "lua.hpp"

#include "LuaBridge.h"
#include "LuaBridgeAddon.h"

#include "Scene.h"
#include "navigation/NavTypes.h"
#include "object/navigation/NavGrid.h"
#include "object/navigation/NavMeshAgent.h"
#include "object/navigation/NavMeshSurface.h"
#include "subsystem/NavigationSystem.h"

#include <string>

using namespace jasperengine;
// the API generator (tools/lua_api) reads the bound names as written: unqualified
using nav::AreaMask;
using nav::AvoidanceQuality;
using nav::GridCell;
using nav::GridLayout;
using nav::PathStatus;
using nav::QueryFilter;

static_assert((int)AreaMask::All == nav::ALL_AREAS && (int)AreaMask::Walkable == (1 << nav::AREA_WALKABLE) &&
              (int)AreaMask::NotWalkable == (1 << nav::AREA_NOT_WALKABLE) && (int)AreaMask::Jump == (1 << nav::AREA_JUMP),
              "NavAreas' masks are the areas' bits");

namespace luabridge
{
    template<> struct Stack<nav::PathStatus> : EnumWrapper<nav::PathStatus>{};
    template<> struct Stack<nav::AreaMask> : EnumWrapper<nav::AreaMask>{};
    template<> struct Stack<nav::AvoidanceQuality> : EnumWrapper<nav::AvoidanceQuality>{};
    template<> struct Stack<nav::GridLayout> : EnumWrapper<nav::GridLayout>{};
    template<> struct Stack<NavAgentDriver> : EnumWrapper<NavAgentDriver>{};
    template<> struct Stack<NavCollectObjects> : EnumWrapper<NavCollectObjects>{};
    template<> struct Stack<NavUseGeometry> : EnumWrapper<NavUseGeometry>{};
}

namespace {

    // NavMeshQueryFilter{agentType = "Humanoid" or an id, areaMask = 0xFFFF}
    nav::QueryFilter filterFrom(const luabridge::LuaRef& options) {
        nav::QueryFilter filter;
        if (!options.isTable())
            return filter;
        const luabridge::LuaRef agentType = options["agentType"];
        if (agentType.isString()) {
            if (const nav::AgentTypeSettings* type = nav::getSettings().findAgentType(agentType.unsafe_cast<std::string>()))
                filter.agentType = type->id;
        } else if (agentType.isNumber()) {
            filter.agentType = agentType.unsafe_cast<int>();
        }
        const luabridge::LuaRef areaMask = options["areaMask"];
        if (areaMask.isNumber())
            filter.areaMask = (uint16_t)(areaMask.unsafe_cast<lua_Integer>() & 0xFFFF);
        return filter;
    }

    // a filter, a bare area mask, or nothing (the default agent type, every area)
    nav::QueryFilter filterArg(lua_State* L, int index) {
        if (lua_gettop(L) < index || lua_isnil(L, index))
            return nav::QueryFilter();
        if (lua_type(L, index) == LUA_TNUMBER) {
            nav::QueryFilter filter;
            filter.areaMask = (uint16_t)(lua_tointeger(L, index) & 0xFFFF);
            return filter;
        }
        if (lua_istable(L, index))
            return filterFrom(luabridge::LuaRef::fromStack(L, index));
        auto filter = luabridge::get<nav::QueryFilter>(L, index);
        if (!filter)
            luaL_argerror(L, index, "a NavMeshQueryFilter or an area mask");
        return *filter;
    }

    luabridge::LuaRef hitTable(lua_State* L, const nav::NavHit& hit) {
        luabridge::LuaRef table = luabridge::newTable(L);
        table["hit"] = hit.hit;
        table["position"] = hit.position;
        table["normal"] = hit.normal;
        table["distance"] = hit.distance;
        table["area"] = (int)hit.area;
        table["mask"] = (int)(1u << hit.area);
        return table;
    }

    NavigationSystem* navSelf(lua_State* L) {
        auto self = luabridge::get<NavigationSystem*>(L, 1);
        if (!self || !*self)
            luaL_argerror(L, 1, "a NavigationSystem (call it with ':')");
        return *self;
    }

    Vector3 vectorArg(lua_State* L, int index) {
        auto value = luabridge::get<Vector3>(L, index);
        if (!value)
            luaL_argerror(L, index, "a Vector3");
        return *value;
    }

    int pushHit(lua_State* L, bool result, const nav::NavHit& hit) {
        lua_pushboolean(L, result ? 1 : 0);
        hitTable(L, hit).push(L);
        return 2;
    }

    luabridge::LuaRef pathTable(lua_State* L, const nav::NavPath& path) {
        luabridge::LuaRef table = luabridge::newTable(L);
        table["status"] = (int)path.status;
        luabridge::LuaRef corners = luabridge::newTable(L);
        for (size_t i = 0; i < path.corners.size(); i++)
            corners[(int)i + 1] = path.corners[i];
        table["corners"] = corners;
        table["length"] = path.length;
        return table;
    }

    luabridge::LuaRef gridPathTable(lua_State* L, const NavGrid& grid, const nav::GridPath& path) {
        luabridge::LuaRef table = luabridge::newTable(L);
        table["status"] = (int)path.status;
        luabridge::LuaRef cells = luabridge::newTable(L);
        for (size_t i = 0; i < path.cells.size(); i++)
            cells[(int)i + 1] = path.cells[i];
        table["cells"] = cells;
        luabridge::LuaRef points = luabridge::newTable(L);
        const std::vector<Vector3> world = grid.getPathPoints(path);
        for (size_t i = 0; i < world.size(); i++)
            points[(int)i + 1] = world[i];
        table["points"] = points;
        table["cost"] = path.cost;
        return table;
    }

    nav::GridQuery gridQueryFrom(lua_State* L, int index) {
        nav::GridQuery query;
        if (lua_gettop(L) < index || !lua_istable(L, index))
            return query;
        const luabridge::LuaRef options = luabridge::LuaRef::fromStack(L, index);
        if (options["allowPartial"].isBool())
            query.allowPartial = options["allowPartial"].unsafe_cast<bool>();
        if (options["areaMask"].isNumber())
            query.areaMask = (uint16_t)(options["areaMask"].unsafe_cast<lua_Integer>() & 0xFFFF);
        if (options["cornerCutting"].isBool())
            query.cornerCutting = options["cornerCutting"].unsafe_cast<bool>();
        return query;
    }

    // currentOffMeshLinkData: {valid, startPos, endPos, link}
    luabridge::LuaRef linkData(const NavMeshAgent& agent, lua_State* L) {
        luabridge::LuaRef table = luabridge::newTable(L);
        Vector3 start, end;
        Entity link = NULL_ENTITY;
        const bool valid = agent.getCurrentOffMeshLink(start, end, link);
        table["valid"] = valid;
        table["activated"] = valid;
        table["startPos"] = start;
        table["endPos"] = end;
        table["link"] = link;
        return table;
    }

}

void LuaBinding::registerNavigationClasses(lua_State *L){
#ifndef DISABLE_LUA_BINDINGS

    luabridge::getGlobalNamespace(L)
        .beginNamespace("NavMeshPathStatus")
        .addVariable("Complete", PathStatus::Complete)
        .addVariable("Partial", PathStatus::Partial)
        .addVariable("Invalid", PathStatus::Invalid)
        .endNamespace();

    luabridge::getGlobalNamespace(L)
        .beginNamespace("ObstacleAvoidanceType")
        .addVariable("NONE", AvoidanceQuality::None)
        .addVariable("LOW", AvoidanceQuality::Low)
        .addVariable("MEDIUM", AvoidanceQuality::Medium)
        .addVariable("GOOD", AvoidanceQuality::Good)
        .addVariable("HIGH", AvoidanceQuality::High)
        .endNamespace();

    luabridge::getGlobalNamespace(L)
        .beginNamespace("NavAgentDriver")
        .addVariable("TRANSFORM", NavAgentDriver::TRANSFORM)
        .addVariable("RIGIDBODY", NavAgentDriver::RIGIDBODY)
        .addVariable("CHARACTER_CONTROLLER", NavAgentDriver::CHARACTER_CONTROLLER)
        .addVariable("NONE", NavAgentDriver::NONE)
        .endNamespace();

    luabridge::getGlobalNamespace(L)
        .beginNamespace("NavCollectObjects")
        .addVariable("ALL", NavCollectObjects::ALL)
        .addVariable("VOLUME", NavCollectObjects::VOLUME)
        .addVariable("CHILDREN", NavCollectObjects::CHILDREN)
        .endNamespace();

    luabridge::getGlobalNamespace(L)
        .beginNamespace("NavUseGeometry")
        .addVariable("RENDER_MESHES", NavUseGeometry::RENDER_MESHES)
        .addVariable("PHYSICS_COLLIDERS", NavUseGeometry::PHYSICS_COLLIDERS)
        .addVariable("BOTH", NavUseGeometry::BOTH)
        .endNamespace();

    luabridge::getGlobalNamespace(L)
        .beginNamespace("GridLayout")
        .addVariable("SQUARE", GridLayout::Square)
        .addVariable("HEX", GridLayout::Hex)
        .endNamespace();

    // area masks and the project's area table (Unity's NavMesh.AllAreas, GetAreaFromName)
    luabridge::getGlobalNamespace(L)
        .beginNamespace("NavAreas")
        // masks: a bit per area (static_assert below keeps them the areas' bits)
        .addVariable("All", AreaMask::All)
        .addVariable("Walkable", AreaMask::Walkable)
        .addVariable("NotWalkable", AreaMask::NotWalkable)
        .addVariable("Jump", AreaMask::Jump)
        .addFunction("getAreaFromName", +[](const std::string& name) { return nav::getSettings().findArea(name); })
        .addFunction("getAreaCost", +[](int area) {
            return area >= 0 && area < nav::AREA_COUNT ? nav::getSettings().areas[(size_t)area].cost : 0.0f;
        })
        .addFunction("maskOf", +[](int area) { return area >= 0 && area < nav::AREA_COUNT ? (int)(1u << area) : 0; })
        .endNamespace();

    luabridge::getGlobalNamespace(L)
        .beginClass<nav::QueryFilter>("NavMeshQueryFilter")
        .addConstructor(
            [](void* memory) { return new (memory) nav::QueryFilter(); },
            [](void* memory, const luabridge::LuaRef& options) { return new (memory) nav::QueryFilter(filterFrom(options)); })
        .addProperty("agentType", &QueryFilter::agentType)
        .addProperty("areaMask", +[](const nav::QueryFilter* self) { return (int)self->areaMask; },
                                 +[](nav::QueryFilter* self, int mask) { self->areaMask = (uint16_t)(mask & 0xFFFF); })
        .endClass();

    luabridge::getGlobalNamespace(L)
        .beginClass<nav::GridCell>("GridCell")
        .addConstructor(
            [](void* memory) { return new (memory) nav::GridCell(); },
            [](void* memory, int x, int y) { return new (memory) nav::GridCell{x, y}; })
        .addProperty("x", &GridCell::x)
        .addProperty("y", &GridCell::y)
        .endClass();

    luabridge::getGlobalNamespace(L)
        .beginClass<FunctionSubscribe<void(NavMeshAgent)>>("FunctionSubscribe_V_NavMeshAgent")
        .addFunction("__call", &FunctionSubscribe<void(NavMeshAgent)>::call)
        .addFunction("call", &FunctionSubscribe<void(NavMeshAgent)>::call)
        .addFunction("add", (bool (FunctionSubscribe<void(NavMeshAgent)>::*)(const std::string&, lua_State*))&FunctionSubscribe<void(NavMeshAgent)>::add)
        .addFunction("remove", &FunctionSubscribe<void(NavMeshAgent)>::remove)
        .endClass();

    luabridge::getGlobalNamespace(L)
        .beginClass<FunctionSubscribe<void(NavMeshSurface)>>("FunctionSubscribe_V_NavMeshSurface")
        .addFunction("__call", &FunctionSubscribe<void(NavMeshSurface)>::call)
        .addFunction("call", &FunctionSubscribe<void(NavMeshSurface)>::call)
        .addFunction("add", (bool (FunctionSubscribe<void(NavMeshSurface)>::*)(const std::string&, lua_State*))&FunctionSubscribe<void(NavMeshSurface)>::add)
        .addFunction("remove", &FunctionSubscribe<void(NavMeshSurface)>::remove)
        .endClass();

    luabridge::getGlobalNamespace(L)
        .beginClass<NavigationSystem>("NavigationSystem")
        .addFunction("calculatePath", +[](NavigationSystem* self, const Vector3& from, const Vector3& to, lua_State* L) {
            return pathTable(L, self->calculatePath(from, to, filterArg(L, 4)));
        })
        // (found, hit): two results, pushed by hand (LuaBridge returns a std::tuple as one table)
        .addFunction("samplePosition", +[](lua_State* L) -> int {
            NavigationSystem* self = navSelf(L);
            nav::NavHit hit;
            const bool found = self->samplePosition(vectorArg(L, 2), (float)luaL_checknumber(L, 3), hit, filterArg(L, 4));
            return pushHit(L, found, hit);
        })
        .addFunction("raycast", +[](lua_State* L) -> int {
            NavigationSystem* self = navSelf(L);
            nav::NavHit hit;
            const bool blocked = self->raycast(vectorArg(L, 2), vectorArg(L, 3), hit, filterArg(L, 4));
            return pushHit(L, blocked, hit);
        })
        .addFunction("findClosestEdge", +[](lua_State* L) -> int {
            NavigationSystem* self = navSelf(L);
            nav::NavHit hit;
            const bool found = self->findClosestEdge(vectorArg(L, 2), hit, filterArg(L, 3));
            return pushHit(L, found, hit);
        })
        .addFunction("isReachable", +[](NavigationSystem* self, const Vector3& from, const Vector3& to, lua_State* L) {
            return self->isReachable(from, to, filterArg(L, 4));
        })
        .addFunction("isBaking", &NavigationSystem::isAnyBaking)
        .addProperty("agentCount", &NavigationSystem::getAgentCount)
        .addProperty("crowdCount", &NavigationSystem::getCrowdCount)
        .endClass();

    luabridge::getGlobalNamespace(L)
        .beginClass<Scene>("Scene")
        .addFunction("getNavigationSystem", +[](Scene* self) { return self->getSystem<NavigationSystem>().get(); })
        .endClass();

    luabridge::getGlobalNamespace(L)
        .deriveClass<NavMeshAgent, EntityHandle>("NavMeshAgent")
        .addConstructor<void (Scene*, Entity)>()
        .addProperty("valid", &NavMeshAgent::isValid)
        .addFunction("setDestination", &NavMeshAgent::setDestination)
        .addProperty("destination", &NavMeshAgent::getDestination, +[](NavMeshAgent* self, const Vector3& target) { self->setDestination(target); })
        .addFunction("resetPath", &NavMeshAgent::resetPath)
        .addFunction("warp", &NavMeshAgent::warp)
        .addFunction("move", &NavMeshAgent::move)
        .addFunction("completeOffMeshLink", &NavMeshAgent::completeOffMeshLink)
        .addFunction("getPathCorners", +[](const NavMeshAgent* self, lua_State* L) {
            luabridge::LuaRef corners = luabridge::newTable(L);
            const std::vector<Vector3> points = self->getPathCorners();
            for (size_t i = 0; i < points.size(); i++)
                corners[(int)i + 1] = points[i];
            return corners;
        })
        .addProperty("enabled", &NavMeshAgent::isEnabled, &NavMeshAgent::setEnabled)
        .addProperty("agentTypeID", &NavMeshAgent::getAgentTypeID, &NavMeshAgent::setAgentTypeID)
        .addProperty("speed", &NavMeshAgent::getSpeed, &NavMeshAgent::setSpeed)
        .addProperty("angularSpeed", &NavMeshAgent::getAngularSpeed, &NavMeshAgent::setAngularSpeed)
        .addProperty("acceleration", &NavMeshAgent::getAcceleration, &NavMeshAgent::setAcceleration)
        .addProperty("stoppingDistance", &NavMeshAgent::getStoppingDistance, &NavMeshAgent::setStoppingDistance)
        .addProperty("autoBraking", &NavMeshAgent::isAutoBraking, &NavMeshAgent::setAutoBraking)
        .addProperty("radius", &NavMeshAgent::getRadius, &NavMeshAgent::setRadius)
        .addProperty("height", &NavMeshAgent::getHeight, &NavMeshAgent::setHeight)
        .addProperty("baseOffset", &NavMeshAgent::getBaseOffset, &NavMeshAgent::setBaseOffset)
        .addProperty("isStopped", &NavMeshAgent::isStopped, &NavMeshAgent::setStopped)
        .addProperty("avoidancePriority", &NavMeshAgent::getAvoidancePriority, &NavMeshAgent::setAvoidancePriority)
        .addProperty("obstacleAvoidanceType", &NavMeshAgent::getObstacleAvoidanceType, &NavMeshAgent::setObstacleAvoidanceType)
        .addProperty("areaMask", &NavMeshAgent::getAreaMask, &NavMeshAgent::setAreaMask)
        .addProperty("autoRepath", &NavMeshAgent::isAutoRepath, &NavMeshAgent::setAutoRepath)
        .addProperty("autoTraverseOffMeshLink", &NavMeshAgent::isAutoTraverseOffMeshLink, &NavMeshAgent::setAutoTraverseOffMeshLink)
        .addProperty("updatePosition", &NavMeshAgent::isUpdatePosition, &NavMeshAgent::setUpdatePosition)
        .addProperty("updateRotation", &NavMeshAgent::isUpdateRotation, &NavMeshAgent::setUpdateRotation)
        .addProperty("driver", &NavMeshAgent::getDriver, &NavMeshAgent::setDriver)
        .addProperty("surface", &NavMeshAgent::getSurface, &NavMeshAgent::setSurface)
        .addProperty("remainingDistance", &NavMeshAgent::getRemainingDistance)
        .addProperty("pathPending", &NavMeshAgent::isPathPending)
        .addProperty("hasPath", &NavMeshAgent::hasPath)
        .addProperty("pathStatus", &NavMeshAgent::getPathStatus)
        .addProperty("velocity", &NavMeshAgent::getVelocity)
        .addProperty("desiredVelocity", &NavMeshAgent::getDesiredVelocity)
        .addProperty("nextPosition", &NavMeshAgent::getNextPosition)
        .addProperty("isOnNavMesh", &NavMeshAgent::isOnNavMesh)
        .addProperty("isOnOffMeshLink", &NavMeshAgent::isOnOffMeshLink)
        .addProperty("currentOffMeshLinkData", +[](const NavMeshAgent* self, lua_State* L) { return linkData(*self, L); })
        .addProperty("onDestinationReached",
            [](NavMeshAgent* self, lua_State*) { return &self->getOnDestinationReached(); },
            [](NavMeshAgent* self, lua_State* L) { self->getOnDestinationReached() = L; })
        .addProperty("onPathFailed",
            [](NavMeshAgent* self, lua_State*) { return &self->getOnPathFailed(); },
            [](NavMeshAgent* self, lua_State* L) { self->getOnPathFailed() = L; })
        .addProperty("onOffMeshLinkStart",
            [](NavMeshAgent* self, lua_State*) { return &self->getOnOffMeshLinkStart(); },
            [](NavMeshAgent* self, lua_State* L) { self->getOnOffMeshLinkStart() = L; })
        .addProperty("onOffMeshLinkEnd",
            [](NavMeshAgent* self, lua_State*) { return &self->getOnOffMeshLinkEnd(); },
            [](NavMeshAgent* self, lua_State* L) { self->getOnOffMeshLinkEnd() = L; })
        .endClass();

    luabridge::getGlobalNamespace(L)
        .deriveClass<NavMeshSurface, EntityHandle>("NavMeshSurface")
        .addConstructor<void (Scene*, Entity)>()
        .addProperty("valid", &NavMeshSurface::isValid)
        .addFunction("bakeAsync", &NavMeshSurface::bakeAsync)
        .addFunction("updateTiles", &NavMeshSurface::updateTiles)
        .addFunction("clear", &NavMeshSurface::clear)
        .addProperty("isBaking", &NavMeshSurface::isBaking)
        .addProperty("bakeProgress", &NavMeshSurface::getBakeProgress)
        .addProperty("lastBakeError", &NavMeshSurface::getLastBakeError)
        .addProperty("tileCount", &NavMeshSurface::getTileCount)
        .addProperty("polyCount", &NavMeshSurface::getPolyCount)
        .addProperty("hasNavMesh", &NavMeshSurface::hasNavMesh)
        .addProperty("enabled", &NavMeshSurface::isEnabled, &NavMeshSurface::setEnabled)
        .addProperty("agentTypeID", &NavMeshSurface::getAgentTypeID, &NavMeshSurface::setAgentTypeID)
        .addProperty("collectObjects", &NavMeshSurface::getCollectObjects, &NavMeshSurface::setCollectObjects)
        .addProperty("useGeometry", &NavMeshSurface::getUseGeometry, &NavMeshSurface::setUseGeometry)
        .addProperty("layerMask", &NavMeshSurface::getLayerMask, &NavMeshSurface::setLayerMask)
        .addProperty("defaultArea", &NavMeshSurface::getDefaultArea, &NavMeshSurface::setDefaultArea)
        .addProperty("center", &NavMeshSurface::getCenter, &NavMeshSurface::setCenter)
        .addProperty("size", &NavMeshSurface::getSize, &NavMeshSurface::setSize)
        .addProperty("runtimeCarving", &NavMeshSurface::isRuntimeCarving, &NavMeshSurface::setRuntimeCarving)
        .addProperty("followTerrainWorldFocus", &NavMeshSurface::isFollowTerrainWorldFocus, &NavMeshSurface::setFollowTerrainWorldFocus)
        .addProperty("followRadius", &NavMeshSurface::getFollowRadius, &NavMeshSurface::setFollowRadius)
        .addProperty("onBakeCompleted",
            [](NavMeshSurface* self, lua_State*) { return &self->getOnBakeCompleted(); },
            [](NavMeshSurface* self, lua_State* L) { self->getOnBakeCompleted() = L; })
        .endClass();

    luabridge::getGlobalNamespace(L)
        .deriveClass<NavGrid, EntityHandle>("NavGrid")
        .addConstructor<void (Scene*, Entity)>()
        .addProperty("valid", &NavGrid::isValid)
        .addFunction("findPath", +[](const NavGrid* self, const nav::GridCell& from, const nav::GridCell& to, lua_State* L) {
            return gridPathTable(L, *self, self->findPath(from, to, gridQueryFrom(L, 4)));
        })
        .addFunction("worldToCell", &NavGrid::worldToCell)
        .addFunction("cellToWorld", &NavGrid::cellToWorld)
        .addFunction("setCellArea", &NavGrid::setCellArea)
        .addFunction("getCellArea", &NavGrid::getCellArea)
        .addFunction("fillRect", &NavGrid::fillRect)
        .addFunction("stampObstacle", &NavGrid::stampObstacle)
        .addFunction("isReachable", &NavGrid::isReachable)
        .addFunction("contains", &NavGrid::contains)
        .addFunction("rebuild", &NavGrid::rebuild)
        .addProperty("enabled", &NavGrid::isEnabled, &NavGrid::setEnabled)
        .addProperty("width", &NavGrid::getWidth)
        .addProperty("height", &NavGrid::getHeight)
        .addProperty("cellSize", &NavGrid::getCellSize)
        .addProperty("layout", &NavGrid::getLayout)
        .endClass();

#endif //DISABLE_LUA_BINDINGS
}
