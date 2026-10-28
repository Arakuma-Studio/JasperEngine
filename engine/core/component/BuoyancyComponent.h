// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#ifndef BUOYANCY_COMPONENT_H
#define BUOYANCY_COMPONENT_H

#include "math/Vector3.h"

#include <vector>

namespace jasperengine{

    // where a Buoyancy samples the water
    enum class BuoyancyColumns{
        AUTO_2x2,           // 2 x 2 columns on the bottom face of the colliders' bounds
        AUTO_3x3,           // 3 x 3
        CUSTOM              // customPoints: the columns' bottoms in local space
    };

    // Floats the dynamic Rigidbody of its GameObject in Water Volumes by Archimedes
    // (docs/specs/S22-gameplay.md R32): each column of its colliders' height carries its share of
    // their volume, pushed up by water density x |gravity| x the submerged part at the centre of
    // that part, and slowed by the volume's drags against the water's own motion. A box floats
    // as deep as its density is of the water's. Without a dynamic Rigidbody it does nothing.
    struct BuoyancyComponent{
        // component checkbox: its system skips a disabled component
        bool enabled = true;

        BuoyancyColumns columns = BuoyancyColumns::AUTO_2x2;
        std::vector<Vector3> customPoints;
        float volumeScale = 1.0f;           // times the colliders' volume
        float dragScale = 1.0f;             // times the volume's drags
    };

}

#endif //BUOYANCY_COMPONENT_H
