// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#ifndef WATER_LAYERING_H
#define WATER_LAYERING_H

#include "ecs/Entity.h"
#include "math/AABB.h"
#include "math/Vector2.h"
#include "math/Vector3.h"

#include <algorithm>
#include <vector>

namespace jasperengine{

    // A water surface a camera pass draws (library:water or @role water, RenderSystem), as the
    // transparent queue layers the other draws around it: its rest level, the band its waves
    // leave it by, and its area in XZ (widened by how far the waves move it sideways)
    struct WaterLayer{
        Entity entity = NULL_ENTITY;
        float level = 0.0f;
        float band = 0.0f;
        Vector2 minXZ;
        Vector2 maxXZ;
    };

    // The rank of a draw's world box among the pass's waters, seen from the eye: +1 for every
    // water whose surface lies between it and the eye over the water's area (it is under the
    // surface the camera looks down through, or above the one it looks up through), -1 for every
    // water it lies on the eye's side of over its area. A box crossing a water's band, outside
    // its area, null or not finite counts 0 for that water; a water does not count itself.
    // Higher ranks draw first (RenderSystem's transparent queue), so what a water covers is drawn
    // before it, and what is in front of it after. Clamped to [-8, 8].
    inline int waterRank(const AABB& box, const Vector3& eye, const std::vector<WaterLayer>& waters, Entity self){
        if (waters.empty() || box.isNull() || !box.isFinite())
            return 0;
        const Vector3& low = box.getMinimum();
        const Vector3& high = box.getMaximum();
        int rank = 0;
        for (const WaterLayer& water : waters){
            if (water.entity == self)
                continue;
            if (high.x < water.minXZ.x || low.x > water.maxXZ.x || high.z < water.minXZ.y || low.z > water.maxXZ.y)
                continue;
            const bool below = high.y < water.level - water.band;
            const bool above = low.y > water.level + water.band;
            if (!below && !above)
                continue;
            const bool eyeAbove = eye.y >= water.level;
            // beyond the surface from the eye: covered by it
            if (eyeAbove == below)
                rank++;
            else
                rank--;
        }
        return std::clamp(rank, -8, 8);
    }

}

#endif //WATER_LAYERING_H
