// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// Lua's Voxel Volume (docs/specs/T6-voxel-volumes.md R73): the handle, its shapes and the values
// its queries return. Under the module's #if: a project using these names needs the voxel module.

#include "LuaBinding.h"

#include "lua.hpp"

#include "LuaBridge.h"
#include "LuaBridgeAddon.h"

#include "object/GameObject.h"
#include "object/voxel/VoxelVolume.h"

using namespace jasperengine;

#if JASPERENGINE_VOXEL

namespace {

    // {blend = 0.5, material = 2, ...}: every field optional, a stated value, not a rate (D15)
    VoxelEditOptions readOptions(lua_State* L, int index) {
        VoxelEditOptions options;
        if (!lua_istable(L, index)) return options;
        const auto number = [&](const char* name, float fallback) {
            lua_getfield(L, index, name);
            const float value = lua_isnumber(L, -1) ? static_cast<float>(lua_tonumber(L, -1)) : fallback;
            lua_pop(L, 1);
            return value;
        };
        options.blend = number("blend", options.blend);
        options.material = static_cast<int>(number("material", static_cast<float>(options.material)));
        options.weight = number("weight", options.weight);
        options.falloff = number("falloff", options.falloff);
        options.amount = number("amount", options.amount);
        options.iterations = static_cast<int>(number("iterations", static_cast<float>(options.iterations)));
        return options;
    }

}

void LuaBinding::registerVoxelClasses(lua_State *L){
    luabridge::getGlobalNamespace(L)
        .beginClass<VoxelShape>("VoxelShape")
        .addStaticFunction("sphere", &VoxelShape::sphere)
        .addStaticFunction("box", &VoxelShape::box)
        .addStaticFunction("capsule", &VoxelShape::capsule)
        .addStaticFunction("mesh", &VoxelShape::mesh)
        .addStaticFunction("stamp", &VoxelShape::stamp)
        .endClass();

    luabridge::getGlobalNamespace(L)
        .beginClass<VoxelEditReport>("VoxelEditReport")
        .addProperty("ok", &VoxelEditReport::ok)
        .addProperty("error", &VoxelEditReport::error)
        .addProperty("bounds", &VoxelEditReport::bounds)
        .addProperty("voxelsChanged", &VoxelEditReport::voxelsChanged)
        .addProperty("surfaceVoxelsChanged", &VoxelEditReport::surfaceVoxelsChanged)
        .addProperty("blocksTouched", &VoxelEditReport::blocksTouched)
        .addProperty("memoryDelta", &VoxelEditReport::memoryDelta)
        .addProperty("clampedVoxels", &VoxelEditReport::clampedVoxels)
        .addProperty("removedVolume", &VoxelEditReport::removedVolume)
        .addProperty("addedVolume", &VoxelEditReport::addedVolume)
        .endClass();

    luabridge::getGlobalNamespace(L)
        .beginClass<VoxelRayHit>("VoxelRayHit")
        .addProperty("hit", &VoxelRayHit::hit)
        .addProperty("point", &VoxelRayHit::point)
        .addProperty("normal", &VoxelRayHit::normal)
        .addProperty("distance", &VoxelRayHit::distance)
        .addProperty("slot", &VoxelRayHit::slot)
        .endClass();

    luabridge::getGlobalNamespace(L)
        .beginClass<VoxelMaterialHit>("VoxelMaterialHit")
        .addProperty("slot", &VoxelMaterialHit::slot)
        .addProperty("second", &VoxelMaterialHit::second)
        .addProperty("weight", &VoxelMaterialHit::weight)
        .endClass();

    luabridge::getGlobalNamespace(L)
        .beginClass<VoxelVolumeStats>("VoxelVolumeStats")
        .addProperty("blocks", &VoxelVolumeStats::blocks)
        .addProperty("uniformBlocks", &VoxelVolumeStats::uniformBlocks)
        .addProperty("nonUniformBlocks", &VoxelVolumeStats::nonUniformBlocks)
        .addProperty("modifiedBlocks", &VoxelVolumeStats::modifiedBlocks)
        .addProperty("memoryBytes", &VoxelVolumeStats::memoryBytes)
        .addProperty("meshBytes", &VoxelVolumeStats::meshBytes)
        .addProperty("triangles", &VoxelVolumeStats::triangles)
        .addProperty("chunks", &VoxelVolumeStats::chunks)
        .addProperty("collisionBodies", &VoxelVolumeStats::collisionBodies)
        .addProperty("jobsPending", &VoxelVolumeStats::jobsPending)
        .endClass();

    luabridge::getGlobalNamespace(L)
        .deriveClass<VoxelVolume, EntityHandle>("VoxelVolume")
        .addConstructor<void (Scene*, Entity)>()
        .addStaticFunction("fromGameObject", &VoxelVolume::fromGameObject)
        .addProperty("valid", &VoxelVolume::isValid)
        .addFunction("dig", +[](VoxelVolume* self, const VoxelShape& shape, lua_State* L) { return self->dig(shape, readOptions(L, 3)); })
        .addFunction("fill", +[](VoxelVolume* self, const VoxelShape& shape, lua_State* L) { return self->fill(shape, readOptions(L, 3)); })
        .addFunction("smooth", +[](VoxelVolume* self, const VoxelShape& shape, lua_State* L) { return self->smooth(shape, readOptions(L, 3)); })
        .addFunction("paint", +[](VoxelVolume* self, const VoxelShape& shape, lua_State* L) { return self->paint(shape, readOptions(L, 3)); })
        .addFunction("raycast", &VoxelVolume::raycast)
        .addFunction("getValueAt", &VoxelVolume::getValueAt)
        .addFunction("getMaterialAt", &VoxelVolume::getMaterialAt)
        .addFunction("ensureCollision", &VoxelVolume::ensureCollision)
        .addFunction("isCollisionReady", &VoxelVolume::isCollisionReady)
        .addFunction("getStats", &VoxelVolume::getStats)
        .addFunction("saveChanges", &VoxelVolume::saveChanges)
        .addFunction("loadChanges", &VoxelVolume::loadChanges)
        .addFunction("hasSavedChanges", &VoxelVolume::hasSavedChanges)
        .addFunction("revertChanges", &VoxelVolume::revertChanges)
        .addProperty("lastError", &VoxelVolume::getLastError)
        .addProperty("onEdited",
            [](VoxelVolume* self, lua_State*) { return &self->getOnEdited(); },
            [](VoxelVolume* self, lua_State* L) { self->getOnEdited() = L; })
        .addProperty("onCollisionUpdated",
            [](VoxelVolume* self, lua_State*) { return &self->getOnCollisionUpdated(); },
            [](VoxelVolume* self, lua_State* L) { self->getOnCollisionUpdated() = L; })
        .addProperty("onBodyRescued",
            [](VoxelVolume* self, lua_State*) { return &self->getOnBodyRescued(); },
            [](VoxelVolume* self, lua_State* L) { self->getOnBodyRescued() = L; })
        .endClass();
}

#endif
