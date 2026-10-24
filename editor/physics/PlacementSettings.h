// SPDX-License-Identifier: MIT
//
// Physics Placement settings (design spec docs/superpowers/specs/2026-10-23-physics-placement-design.md
// sections 6 and 7.3). Edited by the Physics Placement panel, persisted per user and project in
// .jasperengine/user/workspace.yaml (Stream::encode/decodePhysicsPlacementSettings), read by the
// interactive PlacementSession, and used as the defaults of place_with_physics / place-physics.

#pragma once

#include "math/Vector3.h"
#include "physics/ConvexDecomposition.h"
#include "physics/PlacementWorld.h"

#include <cstdint>

namespace jasperengine::editor {

    enum class PlacementFeel {
        Precise,
        Balanced,
        Fast
    };

    enum class PlacementRotatePivot {
        GroupCenter,   // the selection rotates rigidly about its centre
        EachObject     // every object turns about its own centre
    };

    struct PhysicsPlacementSettings {
        PlacementFeel feel = PlacementFeel::Balanced;
        bool settleOnRelease = true;          // after a Move/Rotate/Gather/Snap release, fall until resting, then commit
        bool keepFormation = true;            // a multi-selection keeps its relative layout while moving
        bool pushNeighbours = false;
        Vector3 gravity = Vector3(0.0f, -9.81f, 0.0f);
        bool gravityTowardSurface = false;    // Drop: gravity points at the surface under the cursor
        PlacementColliderPolicy colliders = PlacementColliderPolicy::AUTO;
        PlacementDecomposition decomposition = PlacementDecomposition::CACHED;
        DecompositionQuality quality = DecompositionQuality::MEDIUM;
        bool alignToSurface = true;           // Snap: the object's up axis follows the surface normal
        float snapOffset = 0.0f;              // Snap: extra gap above the surface, m
        PlacementRotatePivot rotatePivot = PlacementRotatePivot::GroupCenter;
        float regionMargin = 2.0f;            // context region = movers' swept bounds + max(margin, mover size)
        uint32_t ignoreLayerMask = 0;         // bit i = layer i ignored
        float maxMoverSize = 25.0f;

        // Advanced; a negative value means "from the feel preset"
        float maxSpeed = -1.0f;
        float contactSpeed = -1.0f;
        float maxAngularSpeed = -1.0f;        // rad/s
        int substeps = -1;
        int velocityIterations = -1;
        int positionIterations = -1;
        float teleportDistance = 5.0f;
        float sleepLinear = 0.02f;
        float sleepAngularDeg = 2.0f;
        float sleepSeconds = 0.25f;
        float maxSettleSeconds = 4.0f;        // interactive settle limit (MCP default is 10)
        float killYDepth = 50.0f;             // bodies this far below the context region freeze

        // Scene-view gizmos (Gizmos menu)
        bool showDistanceIndicators = false;
        bool showProxies = false;
    };

    // The preset values of a feel (spec section 6 table), before advanced overrides.
    PlacementTuning placementFeelTuning(PlacementFeel feel);
    // The feel preset with every non-negative advanced override applied.
    PlacementTuning resolvePlacementTuning(const PhysicsPlacementSettings& settings);
    // World settings for a session: tuning, colliders, decomposition (params from quality), layers,
    // size limit, pushNeighbours. killY is set later from the context region.
    PlacementWorldSettings makePlacementWorldSettings(const PhysicsPlacementSettings& settings,
                                                      ConvexDecompositionCache* cache);

    const char* placementFeelName(PlacementFeel feel);            // "precise", "balanced", "fast"
    bool parsePlacementFeel(const char* name, PlacementFeel& out);
    const char* placementColliderPolicyName(PlacementColliderPolicy policy);   // "auto", "authored", "generated", "both"
    bool parsePlacementColliderPolicy(const char* name, PlacementColliderPolicy& out);
    const char* placementDecompositionName(PlacementDecomposition mode);       // "never", "cached", "always"
    bool parsePlacementDecomposition(const char* name, PlacementDecomposition& out);

}
