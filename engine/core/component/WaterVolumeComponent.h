// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#ifndef WATER_VOLUME_COMPONENT_H
#define WATER_VOLUME_COMPONENT_H

#include "math/Vector3.h"

namespace jasperengine{

    // where a Water Volume's waves come from
    enum class WaterVolumeWaves{
        FROM_MATERIAL,      // the entity's library:water material (WAVES and its wave values)
        NONE                // a flat surface
    };

    // A body of water for the physics (docs/specs/S22-gameplay.md R30): what floats in it with a
    // Buoyancy and what Water.sample reads. Its surface is the GameObject's world height at
    // surfaceOffset plus the waves of its library:water material, the same waves the shader draws
    // (the material is the only place their values live); below it the water fills a local box of
    // size (XZ the extent, Y the depth under the surface), or everything with infinite (an ocean).
    // Where volumes overlap the highest surface wins.
    struct WaterVolumeComponent{
        // component checkbox: its system skips a disabled component
        bool enabled = true;

        Vector3 size = Vector3(10.0f, 5.0f, 10.0f);
        float surfaceOffset = 0.0f;
        bool infinite = false;
        float density = 1000.0f;            // kg/m3 (fresh water)
        Vector3 flow = Vector3(0.0f, 0.0f, 0.0f);   // world m/s: what floats drifts with it
        float linearDrag = 1.0f;            // 1/s, against the motion through the water
        float angularDrag = 0.5f;           // 1/s, against the turning in it
        WaterVolumeWaves waves = WaterVolumeWaves::FROM_MATERIAL;
    };

}

#endif //WATER_VOLUME_COMPONENT_H
