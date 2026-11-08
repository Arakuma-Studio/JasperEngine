// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#ifndef WATER_EXCLUDER_COMPONENT_H
#define WATER_EXCLUDER_COMPONENT_H

#include "math/Vector3.h"

namespace jasperengine{

    enum class WaterExcluderShape{
        BOX,
        SPHERE,
        CONVEX_HULL     // the convex hull of the GameObject's mesh (or its model's mesh nodes)
    };

    // Keeps water out (Unity HDRP's Water Excluder): a boat, a cabin, a dry tunnel. Inside it no
    // water surface is drawn (shaders with @require waterexclusion: library:water), a camera sees
    // no underwater fade, Water.isUnderwater is false and nothing floats but the Rigidbody that
    // owns it (the nearest Rigidbody on its GameObject or above). WaterInteractionSystem keeps
    // its hull and per-frame state; disabled, inactive or hidden it keeps nothing out.
    struct WaterExcluderComponent{
        bool enabled = true;

        WaterExcluderShape shape = WaterExcluderShape::CONVEX_HULL;
        Vector3 center = Vector3(0.0f, 0.0f, 0.0f);     // local, Box and Sphere
        Vector3 size = Vector3(1.0f, 1.0f, 1.0f);       // local; a Sphere is the ellipsoid in this box
    };

}

#endif //WATER_EXCLUDER_COMPONENT_H
