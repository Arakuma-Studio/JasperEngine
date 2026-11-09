// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "terrain/TerrainPlacement.h"
#include "terrain/TerrainDeterministic.h"

#include <algorithm>
#include <cmath>

using namespace jasperengine;
using namespace jasperengine::terraincore;

PlacementPlan jasperengine::terraincore::planPlacement(std::span<const PlacementStamp> stamps, const PlacementParams& params, std::span<const Vector2> existing,
                                                   Vector2 boundsMin, Vector2 boundsMax, PlacementAccept accept, void* acceptUser) {
    PlacementPlan plan;
    Pcg32 random(params.seed);
    const float spacingSquared = params.spacing * params.spacing;
    const float scaleRange = std::max(0.0f, params.maxScale - params.minScale);

    auto tooClose = [&](float x, float z) {
        auto within = [&](float otherX, float otherZ) {
            const float dx = otherX - x;
            const float dz = otherZ - z;
            return (dx * dx + dz * dz) < spacingSquared;
        };
        for (const Vector2& point : existing) {
            if (within(point.x, point.y)) return true;
        }
        for (const PlacedObject& object : plan.objects) {
            if (within(object.x, object.z)) return true;
        }
        return false;
    };

    for (const PlacementStamp& stamp : stamps) {
        plan.attempts++;
        // Every attempt draws the same numbers, accepted or not, so a refusal does not move the
        // stamps after it
        const float a = random.nextFloat();
        const float b = random.nextFloat();
        const float yawDraw = random.nextFloat();
        const float scaleDraw = random.nextFloat();

        float offsetX = 0.0f;
        float offsetZ = 0.0f;
        if (stamp.square) {
            offsetX = (a * 2.0f - 1.0f) * stamp.radius;
            offsetZ = (b * 2.0f - 1.0f) * stamp.radius;
        } else {
            const float angle = a * DET_TWO_PI;
            const float radius = stamp.radius * std::sqrt(b);
            offsetX = detCos(angle) * radius;
            offsetZ = detSin(angle) * radius;
        }
        const float x = std::clamp(stamp.x + offsetX, boundsMin.x, boundsMax.x);
        const float z = std::clamp(stamp.z + offsetZ, boundsMin.y, boundsMax.y);

        if (accept && !accept(acceptUser, x, z)) {
            plan.rejectedByRegion++;
            continue;
        }
        if (tooClose(x, z)) {
            plan.rejectedBySpacing++;
            continue;
        }
        plan.objects.push_back(PlacedObject{x, z, yawDraw * DET_TWO_PI * params.rotationJitter, params.minScale + scaleDraw * scaleRange});
    }
    return plan;
}
