// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#ifndef VOXEL_VOLUME_COMPONENT_H
#define VOXEL_VOLUME_COMPONENT_H

// T6 R1: a bounded box of smooth voxel terrain. The fields stay in every build, as navigation's do;
// a player without the voxel module whose content needs one exits 5 (S34 R3). Dimensions and voxel
// size belong to the asset.

#include "asset/AssetRef.h"
#include "component/TerrainSurfaceLayer.h"
#include "ecs/Entity.h"
#include "math/AABB.h"
#include "math/Vector3.h"
#include "util/FunctionSubscribe.h"

#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace jasperengine{

    class GameObject;

    // R43: the same struct and field names as Terrain Layers, so the panels look alike
    struct VoxelMaterialSlot{
        std::string name;
        TerrainSurfaceLayer layer;
        // the Terrain Layer it follows live, -1 for its own
        int linkedTerrainLayer = -1;
    };

    enum class VoxelQuality{ Auto, High, Medium, Low };
    enum class VoxelNavigationUpdate{ Off, Automatic };
    enum class VoxelTerrainCut{ Off, Automatic };

    // what volume.onEdited receives (R31), in metres and world space
    struct VoxelEditEvent{
        AABB bounds;
        Vector3 center;
        float removedVolume = 0.0f;
        float addedVolume = 0.0f;
        std::vector<float> removedByMaterial;
        std::vector<Vector3> debrisPoints;
    };

    struct VoxelVolumeComponent{
        bool enabled = true;

        // the .voxelvolume asset
        AssetRef volumeData;
        std::vector<VoxelMaterialSlot> materialSlots;

        VoxelQuality quality = VoxelQuality::Auto;
        // 0 = from quality, else 1-4
        int lodLevels = 0;
        // blocks per LOD level around the focus: 4, 6 or 8
        int lodRadius = 4;
        // 0 = none
        float maxDrawDistance = 0.0f;

        bool castShadows = true;
        bool collision = true;
        // 0 = from quality, else blocks
        int collisionRadius = 0;
        bool rescueBodies = true;
        VoxelNavigationUpdate navigationUpdate = VoxelNavigationUpdate::Automatic;

        Entity terrain = NULL_ENTITY;
        VoxelTerrainCut terrainCut = VoxelTerrainCut::Off;
        // negative: 0.02 * voxel size, the defaults of R1 (voxel size is the asset's)
        float overlapDepth = -1.0f;
        float cutTolerance = -1.0f;

        // NULL_ENTITY: the active camera
        Entity focus = NULL_ENTITY;
        // keep the generator in the cooked file and evaluate it in the player (R78)
        bool runtimeGeneration = false;

        // R31: runtime only
        int debrisPointCount = 16;
        FunctionSubscribe<void(VoxelEditEvent)> onEdited;
        FunctionSubscribe<void(AABB)> onCollisionUpdated;
        FunctionSubscribe<void(GameObject)> onBodyRescued;
        // what Stop clears (ComponentEvents.h)
        template<typename F> void forEachEvent(F&& f) { f(onEdited); f(onCollisionUpdated); f(onBodyRescued); }
    };

}

#endif
