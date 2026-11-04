// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// Lua's NavMesh Obstacle, Link and Modifier handles and the script volumes of
// docs/specs/S55-navigation-gameplay-api.md (R12, R16, R17, R30-R33): Unity's scripting names over
// the components' fields (docs/navigation.md "Script names"), registered from
// registerNavigationClasses.

#include "LuaBinding.h"

#include "lua.hpp"

#include "LuaBridge.h"
#include "LuaBridgeAddon.h"

#include "Scene.h"
#include "navigation/NavTypes.h"
#include "object/navigation/NavAreaOverride.h"
#include "object/navigation/NavMeshLink.h"
#include "object/navigation/NavMeshModifier.h"
#include "object/navigation/NavMeshObstacle.h"
#include "subsystem/NavigationSystem.h"

#include <string>

using namespace jasperengine;

namespace luabridge
{
    template<> struct Stack<NavObstacleShape> : EnumWrapper<NavObstacleShape>{};
}

namespace {

    NavigationSystem* areaVolumeSelf(lua_State* L) {
        auto self = luabridge::get<NavigationSystem*>(L, 1);
        if (!self || !*self)
            luaL_argerror(L, 1, "a NavigationSystem (call it with ':')");
        return *self;
    }

    Vector3 areaVolumeVector(lua_State* L, int index) {
        auto value = luabridge::get<Vector3>(L, index);
        if (!value)
            luaL_argerror(L, index, "a Vector3");
        return *value;
    }

}

void LuaBinding::registerNavigationComponentClasses(lua_State *L){
#ifndef DISABLE_LUA_BINDINGS

    luabridge::getGlobalNamespace(L)
        .beginNamespace("NavMeshObstacleShape")
        .addVariable("CAPSULE", NavObstacleShape::CAPSULE)
        .addVariable("BOX", NavObstacleShape::BOX)
        .endNamespace();

    luabridge::getGlobalNamespace(L)
        .beginClass<NavAreaOverride>("NavAreaOverride")
        .addProperty("valid", &NavAreaOverride::isValid)
        .addProperty("handle", &NavAreaOverride::getHandle)
        .addProperty("area", &NavAreaOverride::getArea, &NavAreaOverride::setArea)
        .addProperty("center", &NavAreaOverride::getCenter, &NavAreaOverride::setCenter)
        .addProperty("size", &NavAreaOverride::getSize, &NavAreaOverride::setSize)
        .addProperty("rotation", &NavAreaOverride::getRotation, &NavAreaOverride::setRotation)
        .addProperty("appliedPolygons", &NavAreaOverride::getAppliedPolygons)
        .addFunction("remove", &NavAreaOverride::remove)
        .endClass();

    // setAreaInVolume(center, size, area[, {rotation = q, agentType = "Humanoid" or an id}]):
    // a NavAreaOverride, nil (with a warning) for an area outside 0-15
    luabridge::getGlobalNamespace(L)
        .beginClass<NavigationSystem>("NavigationSystem")
        .addFunction("setAreaInVolume", +[](lua_State* L) -> int {
            NavigationSystem* self = areaVolumeSelf(L);
            const Vector3 center = areaVolumeVector(L, 2);
            const Vector3 size = areaVolumeVector(L, 3);
            const lua_Integer area = luaL_checkinteger(L, 4);
            Quaternion rotation;
            int agentType = -1;
            if (lua_istable(L, 5)) {
                const luabridge::LuaRef options = luabridge::LuaRef::fromStack(L, 5);
                const luabridge::LuaRef turn = options["rotation"];
                if (!turn.isNil()) {
                    auto value = turn.cast<Quaternion>();
                    if (!value)
                        luaL_argerror(L, 5, "rotation: a Quaternion");
                    rotation = *value;
                }
                const luabridge::LuaRef type = options["agentType"];
                if (type.isString()) {
                    const nav::AgentTypeSettings* found = nav::getSettings().findAgentType(type.unsafe_cast<std::string>());
                    if (!found)
                        luaL_argerror(L, 5, "agentType: no agent type of that name");
                    agentType = found->id;
                } else if (type.isNumber()) {
                    agentType = type.unsafe_cast<int>();
                }
            }
            // outside 0-15 the system refuses it (and says so)
            const uint8_t index = area >= 0 && area < nav::AREA_COUNT ? (uint8_t)area : (uint8_t)nav::AREA_COUNT;
            const uint32_t handle = self->setAreaInVolume(center, size, rotation, index, agentType);
            if (!handle) {
                lua_pushnil(L);
                return 1;
            }
            if (!luabridge::push(L, NavAreaOverride(self->getScene(), handle)))
                lua_pushnil(L);
            return 1;
        })
        .endClass();

    luabridge::getGlobalNamespace(L)
        .deriveClass<NavMeshObstacle, EntityHandle>("NavMeshObstacle")
        .addConstructor<void (Scene*, Entity)>()
        .addProperty("valid", &NavMeshObstacle::isValid)
        .addProperty("enabled", &NavMeshObstacle::isEnabled, &NavMeshObstacle::setEnabled)
        .addProperty("shape", &NavMeshObstacle::getShape, &NavMeshObstacle::setShape)
        .addProperty("center", &NavMeshObstacle::getCenter, &NavMeshObstacle::setCenter)
        .addProperty("size", &NavMeshObstacle::getSize, &NavMeshObstacle::setSize)
        .addProperty("radius", &NavMeshObstacle::getRadius, &NavMeshObstacle::setRadius)
        .addProperty("height", &NavMeshObstacle::getHeight, &NavMeshObstacle::setHeight)
        .addProperty("carving", &NavMeshObstacle::isCarving, &NavMeshObstacle::setCarving)
        .addProperty("carveOnlyStationary", &NavMeshObstacle::isCarveOnlyStationary, &NavMeshObstacle::setCarveOnlyStationary)
        .addProperty("carvingMoveThreshold", &NavMeshObstacle::getCarvingMoveThreshold, &NavMeshObstacle::setCarvingMoveThreshold)
        .addProperty("carvingTimeToStationary", &NavMeshObstacle::getCarvingTimeToStationary, &NavMeshObstacle::setCarvingTimeToStationary)
        .addProperty("velocity", &NavMeshObstacle::getVelocity)
        .endClass();

    luabridge::getGlobalNamespace(L)
        .deriveClass<NavMeshLink, EntityHandle>("NavMeshLink")
        .addConstructor<void (Scene*, Entity)>()
        .addProperty("valid", &NavMeshLink::isValid)
        .addProperty("enabled", &NavMeshLink::isEnabled, &NavMeshLink::setEnabled)
        .addProperty("agentTypeID", &NavMeshLink::getAgentTypeID, &NavMeshLink::setAgentTypeID)
        .addProperty("startPoint", &NavMeshLink::getStartPoint, &NavMeshLink::setStartPoint)
        .addProperty("endPoint", &NavMeshLink::getEndPoint, &NavMeshLink::setEndPoint)
        .addProperty("width", &NavMeshLink::getWidth, &NavMeshLink::setWidth)
        .addProperty("costModifier", &NavMeshLink::getCostModifier, &NavMeshLink::setCostModifier)
        .addProperty("bidirectional", &NavMeshLink::isBidirectional, &NavMeshLink::setBidirectional)
        .addProperty("area", &NavMeshLink::getArea, &NavMeshLink::setArea)
        .addProperty("autoUpdate", &NavMeshLink::isAutoUpdate, &NavMeshLink::setAutoUpdate)
        .addProperty("activated", &NavMeshLink::isActivated, &NavMeshLink::setActivated)
        .addProperty("occupied", &NavMeshLink::isOccupied)
        .addFunction("updateLink", &NavMeshLink::updateLink)
        .endClass();

    luabridge::getGlobalNamespace(L)
        .deriveClass<NavMeshModifier, EntityHandle>("NavMeshModifier")
        .addConstructor<void (Scene*, Entity)>()
        .addProperty("valid", &NavMeshModifier::isValid)
        .addProperty("enabled", &NavMeshModifier::isEnabled, &NavMeshModifier::setEnabled)
        .addProperty("area", &NavMeshModifier::getArea, &NavMeshModifier::setArea)
        .addProperty("center", &NavMeshModifier::getCenter, &NavMeshModifier::setCenter)
        .addProperty("size", &NavMeshModifier::getSize, &NavMeshModifier::setSize)
        .addProperty("useVolume", &NavMeshModifier::isUseVolume)
        .addProperty("applyAtRuntime", &NavMeshModifier::isApplyAtRuntime)
        .addProperty("overrideArea", &NavMeshModifier::isOverrideArea)
        .addProperty("applyToChildren", &NavMeshModifier::isApplyToChildren)
        .addProperty("appliedPolygons", &NavMeshModifier::getAppliedPolygons)
        .endClass();

#endif //DISABLE_LUA_BINDINGS
}
