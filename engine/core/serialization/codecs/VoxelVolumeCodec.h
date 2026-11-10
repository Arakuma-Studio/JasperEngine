// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#pragma once

// What a Voxel Volume's saved values must hold (T6 R1), kept wherever they are read past the
// Inspector: a scene's decode (VoxelReflect.cpp) and a cooked scene's (afterVoxelVolume).

#include "component/VoxelVolumeComponent.h"

#include <algorithm>

namespace jasperengine {

    inline void sanitizeVoxelVolume(VoxelVolumeComponent& volume) {
        // R3: a sample's slots are 4 bits each
        constexpr size_t maxSlots = 16;
        if (volume.materialSlots.size() > maxSlots) volume.materialSlots.resize(maxSlots);
        volume.lodLevels = std::clamp(volume.lodLevels, 0, 4);
        volume.lodRadius = volume.lodRadius <= 4 ? 4 : volume.lodRadius <= 6 ? 6 : 8;
        volume.collisionRadius = std::max(volume.collisionRadius, 0);
        volume.maxDrawDistance = std::max(volume.maxDrawDistance, 0.0f);
    }

}
