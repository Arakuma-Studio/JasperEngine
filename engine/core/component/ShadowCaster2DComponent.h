// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#ifndef SHADOWCASTER2D_COMPONENT_H
#define SHADOWCASTER2D_COMPONENT_H

#include "math/Vector2.h"
#include "Engine.h"
#include <vector>

namespace jasperengine{

    enum class ShadowCaster2DShape{
        AUTO_QUAD, // segments derived from the sibling mesh's local AABB (xy)
        POLYGON    // explicit local-space points
    };

    struct JASPERENGINE_API ShadowCaster2DComponent{
        ShadowCaster2DShape shape = ShadowCaster2DShape::AUTO_QUAD;

        std::vector<Vector2> points; // local-space vertices, used when shape == POLYGON
        bool closed = true;          // closed loop (last connects to first) vs open chain
        bool enabled = true;
    };

}

#endif //SHADOWCASTER2D_COMPONENT_H
