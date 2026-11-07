// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#ifndef WATER_LAYERING_H
#define WATER_LAYERING_H

#include "ecs/Entity.h"
#include "math/AABB.h"
#include "math/Vector2.h"
#include "math/Vector3.h"
#include "util/WaterWaves.h"

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
        // its distance in the transparent queue
        float distance = 0.0f;
    };

    // Whether a water's world bounds are level enough to layer by and to mirror about one height:
    // at most 1% of its larger side plus 5 cm tall (a sloped river is not)
    inline bool isFlatWater(const AABB& bounds){
        if (bounds.isNull() || !bounds.isFinite())
            return false;
        const Vector3 size = bounds.getSize();
        return size.y <= 0.01f * std::max(size.x, size.z) + 0.05f;
    }

    // The layer a water mesh makes in the transparent queue (its world bounds, its waves, its
    // distance): false for one that is not flat, which sorts by its distance alone
    inline bool makeWaterLayer(Entity entity, const AABB& bounds, const WaterWaveParams& waves, float distance, WaterLayer& layer){
        if (!isFlatWater(bounds))
            return false;
        const float reach = waves.getHorizontalReach();
        layer.entity = entity;
        layer.level = bounds.getMaximum().y;
        layer.band = waves.getBand();
        layer.minXZ = Vector2(bounds.getMinimum().x - reach, bounds.getMinimum().z - reach);
        layer.maxXZ = Vector2(bounds.getMaximum().x + reach, bounds.getMaximum().z + reach);
        layer.distance = distance;
        return true;
    }

    // How one water layers a box: +1 beyond its surface from the eye over its area (covered by
    // it), -1 on the eye's side, 0 crossing its band or outside its area
    inline int waterLayerTerm(const AABB& box, const Vector3& eye, const WaterLayer& water){
        const Vector3& low = box.getMinimum();
        const Vector3& high = box.getMaximum();
        if (high.x < water.minXZ.x || low.x > water.maxXZ.x || high.z < water.minXZ.y || low.z > water.maxXZ.y)
            return 0;
        const bool below = high.y < water.level - water.band;
        const bool above = low.y > water.level + water.band;
        if (!below && !above)
            return 0;
        const bool eyeAbove = eye.y >= water.level;
        return (eyeAbove == below) ? 1 : -1;
    }

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
        int rank = 0;
        for (const WaterLayer& water : waters){
            if (water.entity != self)
                rank += waterLayerTerm(box, eye, water);
        }
        return std::clamp(rank, -8, 8);
    }

    // The distance each water's group sorts at: the farthest of the waters whose areas overlap,
    // directly or through others (a pool over a lake, the lake's other pools)
    inline std::vector<float> waterGroupDistances(const std::vector<WaterLayer>& waters){
        std::vector<size_t> group(waters.size());
        for (size_t i = 0; i < waters.size(); i++)
            group[i] = i;
        auto root = [&](size_t i){
            while (group[i] != i)
                i = group[i] = group[group[i]];
            return i;
        };
        for (size_t i = 0; i < waters.size(); i++){
            for (size_t j = i + 1; j < waters.size(); j++){
                const WaterLayer& a = waters[i];
                const WaterLayer& b = waters[j];
                if (a.maxXZ.x < b.minXZ.x || b.maxXZ.x < a.minXZ.x || a.maxXZ.y < b.minXZ.y || b.maxXZ.y < a.minXZ.y)
                    continue;
                group[root(i)] = root(j);
            }
        }
        std::vector<float> farthest(waters.size(), 0.0f);
        for (size_t i = 0; i < waters.size(); i++)
            farthest[root(i)] = std::max(farthest[root(i)], waters[i].distance);
        std::vector<float> distances(waters.size());
        for (size_t i = 0; i < waters.size(); i++)
            distances[i] = farthest[root(i)];
        return distances;
    }

    // Where a draw sorts among the pass's waters: its rank (waterRank), and the distance it sorts
    // at: its own, unless the waters layer it (or it is one of them), then the farthest group
    // distance (waterGroupDistances) of those waters, so a water and what it layers sort
    // together where the farthest of them is, and the rest by their own distances
    struct WaterPlace{
        int rank = 0;
        float sortDistance = 0.0f;
    };

    inline WaterPlace waterPlace(const AABB& box, const Vector3& eye, const std::vector<WaterLayer>& waters,
                                 const std::vector<float>& groupDistances, Entity self, float distance){
        WaterPlace place;
        place.sortDistance = distance;
        if (waters.empty())
            return place;
        const bool boxUsable = !box.isNull() && box.isFinite();
        int rank = 0;
        bool layered = false;
        float group = 0.0f;
        for (size_t i = 0; i < waters.size(); i++){
            const WaterLayer& water = waters[i];
            int term = 0;
            if (water.entity != self && boxUsable)
                term = waterLayerTerm(box, eye, water);
            if (term == 0 && water.entity != self)
                continue;
            rank += term;
            group = layered ? std::max(group, groupDistances[i]) : groupDistances[i];
            layered = true;
        }
        place.rank = std::clamp(rank, -8, 8);
        if (layered)
            place.sortDistance = group;
        return place;
    }

    // The transparent queue's order between two draws of one sort priority (RenderSystem's
    // comparator): the farthest sort distance first, then the higher rank, then the farthest
    // draw; 0 when they tie (the submission order decides)
    inline int waterSortOrder(float sortDistanceA, int rankA, float distanceA, float sortDistanceB, int rankB, float distanceB){
        if (sortDistanceA != sortDistanceB)
            return sortDistanceA > sortDistanceB ? -1 : 1;
        if (rankA != rankB)
            return rankA > rankB ? -1 : 1;
        if (distanceA != distanceB)
            return distanceA > distanceB ? -1 : 1;
        return 0;
    }
}

#endif //WATER_LAYERING_H
