// SPDX-License-Identifier: MIT

#include "PlacementSettings.h"

#include <cstring>

using namespace jasperengine;
using namespace jasperengine::editor;

PlacementTuning editor::placementFeelTuning(PlacementFeel feel) {
    // spec section 6, "Feel presets"; the sleep and teleport fields keep PlacementTuning's defaults
    PlacementTuning tuning;
    tuning.maxAngularSpeed = 10.0f;
    switch (feel) {
        case PlacementFeel::Precise:
            tuning.maxSpeed = 4.0f;
            tuning.contactSpeed = 0.3f;
            tuning.substeps = 2;
            tuning.velocityIterations = 10;
            tuning.positionIterations = 4;
            break;
        case PlacementFeel::Balanced:
            tuning.maxSpeed = 10.0f;
            tuning.contactSpeed = 1.0f;
            tuning.substeps = 1;
            tuning.velocityIterations = 8;
            tuning.positionIterations = 2;
            break;
        case PlacementFeel::Fast:
            tuning.maxSpeed = 25.0f;
            tuning.contactSpeed = 3.0f;
            tuning.substeps = 1;
            tuning.velocityIterations = 4;
            tuning.positionIterations = 1;
            break;
    }
    return tuning;
}

PlacementTuning editor::resolvePlacementTuning(const PhysicsPlacementSettings& settings) {
    PlacementTuning tuning = placementFeelTuning(settings.feel);
    // a negative advanced value means "from the feel preset"
    if (settings.maxSpeed >= 0.0f) tuning.maxSpeed = settings.maxSpeed;
    if (settings.contactSpeed >= 0.0f) tuning.contactSpeed = settings.contactSpeed;
    if (settings.maxAngularSpeed >= 0.0f) tuning.maxAngularSpeed = settings.maxAngularSpeed;
    if (settings.substeps >= 0) tuning.substeps = settings.substeps;
    if (settings.velocityIterations >= 0) tuning.velocityIterations = settings.velocityIterations;
    if (settings.positionIterations >= 0) tuning.positionIterations = settings.positionIterations;
    tuning.teleportDistance = settings.teleportDistance;
    tuning.sleepLinear = settings.sleepLinear;
    tuning.sleepAngularDeg = settings.sleepAngularDeg;
    tuning.sleepSeconds = settings.sleepSeconds;
    return tuning;
}

PlacementWorldSettings editor::makePlacementWorldSettings(const PhysicsPlacementSettings& settings, ConvexDecompositionCache* cache) {
    PlacementWorldSettings world;
    world.tuning = resolvePlacementTuning(settings);
    world.colliders = settings.colliders;
    world.decomposition = settings.decomposition;
    world.decompositionCache = cache;
    const DecompositionParams params = DecompositionParams::fromQuality(settings.quality);
    world.decompositionMaxHulls = params.maxHulls;
    world.decompositionResolution = params.resolution;
    world.decompositionMaxVerticesPerHull = params.maxVerticesPerHull;
    world.maxMoverSize = settings.maxMoverSize;
    world.ignoreLayerMask = settings.ignoreLayerMask;
    world.pushNeighbours = settings.pushNeighbours;
    return world;
}

const char* editor::placementFeelName(PlacementFeel feel) {
    switch (feel) {
        case PlacementFeel::Precise: return "precise";
        case PlacementFeel::Balanced: return "balanced";
        case PlacementFeel::Fast: return "fast";
    }
    return "balanced";
}

bool editor::parsePlacementFeel(const char* name, PlacementFeel& out) {
    if (!name) return false;
    for (PlacementFeel feel : {PlacementFeel::Precise, PlacementFeel::Balanced, PlacementFeel::Fast}) {
        if (std::strcmp(name, placementFeelName(feel)) == 0) {
            out = feel;
            return true;
        }
    }
    return false;
}

const char* editor::placementColliderPolicyName(PlacementColliderPolicy policy) {
    switch (policy) {
        case PlacementColliderPolicy::AUTO: return "auto";
        case PlacementColliderPolicy::AUTHORED: return "authored";
        case PlacementColliderPolicy::GENERATED: return "generated";
        case PlacementColliderPolicy::BOTH: return "both";
    }
    return "auto";
}

bool editor::parsePlacementColliderPolicy(const char* name, PlacementColliderPolicy& out) {
    if (!name) return false;
    for (PlacementColliderPolicy policy : {PlacementColliderPolicy::AUTO, PlacementColliderPolicy::AUTHORED,
                                           PlacementColliderPolicy::GENERATED, PlacementColliderPolicy::BOTH}) {
        if (std::strcmp(name, placementColliderPolicyName(policy)) == 0) {
            out = policy;
            return true;
        }
    }
    return false;
}

const char* editor::placementDecompositionName(PlacementDecomposition mode) {
    switch (mode) {
        case PlacementDecomposition::NEVER: return "never";
        case PlacementDecomposition::CACHED: return "cached";
        case PlacementDecomposition::ALWAYS: return "always";
    }
    return "cached";
}

bool editor::parsePlacementDecomposition(const char* name, PlacementDecomposition& out) {
    if (!name) return false;
    for (PlacementDecomposition mode : {PlacementDecomposition::NEVER, PlacementDecomposition::CACHED, PlacementDecomposition::ALWAYS}) {
        if (std::strcmp(name, placementDecompositionName(mode)) == 0) {
            out = mode;
            return true;
        }
    }
    return false;
}
