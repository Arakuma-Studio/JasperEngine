// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#pragma once

// The Materials tab's operations (docs/specs/T6-voxel-volumes.md R62), each one undo step. A slot's
// index is what every sample stores, so slots never move: removing one that is not the last leaves an
// empty slot there, and Add fills the first empty one.

#include "ecs/Entity.h"
#include "voxel/VoxelStore.h"

#include <cstdint>
#include <string>

namespace jasperengine {
    struct VoxelMaterialSlot;
}

namespace jasperengine::editor {

    class Project;

    // A bit per slot some sample near or inside a surface uses: air far from any surface shows no
    // material, so it does not hold a slot in use. Reads the blocks without keeping them.
    uint16_t voxelSlotsInUse(const voxel::Store& store);

    bool isVoxelSlotEmpty(const VoxelMaterialSlot& slot);

    // Returns the slot's index, -1 with `error` when all 16 are taken
    int addVoxelMaterialSlot(Project* project, uint32_t sceneId, Entity owner, const std::string& name, std::string* error);

    // Refused while the slot is in use, saying so: Replace With is the way
    bool removeVoxelMaterialSlot(Project* project, uint32_t sceneId, Entity owner, int index, std::string* error);

    // Every sample's use of `index` becomes `replaceWith`, and the slot goes, as one undo step
    bool replaceVoxelMaterialSlot(Project* project, uint32_t sceneId, Entity owner, int index, int replaceWith, std::string* error);

    // The slot follows that Terrain Layer live (R43); -1 unlinks
    bool linkVoxelMaterialSlot(Project* project, uint32_t sceneId, Entity owner, int index, int terrainLayer, std::string* error);

    // Any other change of a slot's fields, one undo step (a slider's drag merges)
    bool setVoxelMaterialSlot(Project* project, uint32_t sceneId, Entity owner, int index, const VoxelMaterialSlot& value, bool merge, std::string* error);

}
