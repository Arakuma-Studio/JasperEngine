// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#ifndef NAV_WALKABILITY_PROBE_H
#define NAV_WALKABILITY_PROBE_H

// "Why is this point not walkable?" (docs/specs/S75-navigation-diagnostics.md R11, R12, D5): a
// point's location on a surface and, off the mesh, the reasons in the order they apply. The
// geometric reasons read the surface's collected inputs (with their sources); erosion, step
// height and region pruning are decided by rebuilding only the tile under the point with that
// one Recast limit relaxed. Editor only: it needs the collector's sources.

#include "ecs/Entity.h"
#include "math/Vector3.h"
#include "navigation/NavBakeInput.h"
#include "navigation/NavTypes.h"

#include <memory>
#include <string>
#include <vector>

namespace jasperengine {
    class Scene;
    class NavigationSystem;
}

namespace jasperengine::editor {

    struct NavProbeReason {
        // not_baked, stale_tile, outside_volume, outside_bounds, no_geometry, excluded,
        // not_walkable_area, filtered, too_steep, low_clearance, near_wall, step_too_high,
        // small_region, small_island
        std::string code;
        // one sentence naming the GameObject, the value and the limit
        std::string message;
        // the GameObject it is about (excluded, not_walkable_area), NULL_ENTITY: none
        Entity entity = NULL_ENTITY;
        // the measured value against the limit (degrees, metres, m^2), 0 when none
        float value = 0, limit = 0;
        // excluded: why the GameObject was left out ("layer", "inactive", "moving body", "agent",
        // "obstacle", "exclude modifier"); filtered: the area's name
        std::string detail;
    };

    struct NavProbeResult {
        nav::NavLocation location;
        std::vector<NavProbeReason> reasons;
    };

    struct NavWalkabilityProbe {
        // the reasons for a world point on `surface`; input: the surface's inputs collected with
        // recordSources (NavigationSystem::inspectInputs), collected now when null
        static NavProbeResult explain(Scene* scene, NavigationSystem& system, Entity surface, const Vector3& world,
                                      const nav::QueryFilter& filter, std::shared_ptr<const nav::NavBakeInput> input = nullptr);
    };

}

#endif // NAV_WALKABILITY_PROBE_H
