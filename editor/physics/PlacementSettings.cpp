// SPDX-License-Identifier: MIT
// STUB (contract only): replaced by the PlacementSession implementation.

#include "PlacementSettings.h"

#include <cstring>

using namespace jasperengine;
using namespace jasperengine::editor;

PlacementTuning editor::placementFeelTuning(PlacementFeel) { return PlacementTuning(); }
PlacementTuning editor::resolvePlacementTuning(const PhysicsPlacementSettings& s) { return placementFeelTuning(s.feel); }
PlacementWorldSettings editor::makePlacementWorldSettings(const PhysicsPlacementSettings& s, ConvexDecompositionCache* cache) {
    PlacementWorldSettings w;
    w.tuning = resolvePlacementTuning(s);
    w.decompositionCache = cache;
    return w;
}
const char* editor::placementFeelName(PlacementFeel f) {
    switch (f) { case PlacementFeel::Precise: return "precise"; case PlacementFeel::Fast: return "fast"; default: return "balanced"; }
}
bool editor::parsePlacementFeel(const char* n, PlacementFeel& out) {
    if (!n) return false;
    if (!std::strcmp(n, "precise")) { out = PlacementFeel::Precise; return true; }
    if (!std::strcmp(n, "balanced")) { out = PlacementFeel::Balanced; return true; }
    if (!std::strcmp(n, "fast")) { out = PlacementFeel::Fast; return true; }
    return false;
}
const char* editor::placementColliderPolicyName(PlacementColliderPolicy p) {
    switch (p) { case PlacementColliderPolicy::AUTHORED: return "authored"; case PlacementColliderPolicy::GENERATED: return "generated";
                 case PlacementColliderPolicy::BOTH: return "both"; default: return "auto"; }
}
bool editor::parsePlacementColliderPolicy(const char* n, PlacementColliderPolicy& out) {
    if (!n) return false;
    if (!std::strcmp(n, "auto")) { out = PlacementColliderPolicy::AUTO; return true; }
    if (!std::strcmp(n, "authored")) { out = PlacementColliderPolicy::AUTHORED; return true; }
    if (!std::strcmp(n, "generated")) { out = PlacementColliderPolicy::GENERATED; return true; }
    if (!std::strcmp(n, "both")) { out = PlacementColliderPolicy::BOTH; return true; }
    return false;
}
const char* editor::placementDecompositionName(PlacementDecomposition d) {
    switch (d) { case PlacementDecomposition::NEVER: return "never"; case PlacementDecomposition::ALWAYS: return "always"; default: return "cached"; }
}
bool editor::parsePlacementDecomposition(const char* n, PlacementDecomposition& out) {
    if (!n) return false;
    if (!std::strcmp(n, "never")) { out = PlacementDecomposition::NEVER; return true; }
    if (!std::strcmp(n, "cached")) { out = PlacementDecomposition::CACHED; return true; }
    if (!std::strcmp(n, "always")) { out = PlacementDecomposition::ALWAYS; return true; }
    return false;
}
