// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// Why each asset of a build ships (docs/specs/build-size-tools.md R3): a breadth-first walk of the
// dependency index's edges from the roots (project.yaml, then every shipped script), recording for
// each asset the first referrer that reaches it. The walk is deterministic: roots in the order
// given, a referrer's edges in field locator then target order (R5).

#pragma once

#include "cook/BuildReport.h"

#include <map>
#include <string>
#include <vector>

namespace jasperengine::editor {

    class AssetIndex;
    class AssetDatabase;

    // An edge with both ends as asset keys (the build report's: relative to the assets root)
    struct ReachEdge {
        std::string from, to, field, kind;
        std::string object;   // the GameObject holding the field, "" outside GameObjects
    };

    // asset -> how it was first reached; a root maps to {"", "", "root"}
    // a project-relative path as the report names it: relative to the assets root when under it
    // (`assetsPrefix`: the root relative to the project with a trailing '/', "" for the project itself)
    std::string reachAssetKey(const std::string& projectRelative, const std::string& assetsPrefix);
    // the index's edges, the overlay applied (main thread: forEachEdge), as asset keys
    std::vector<ReachEdge> reachEdgesOf(const AssetIndex& index, const AssetDatabase& database, const std::string& assetsPrefix);

    std::map<std::string, BuildReportReach> computeReachability(const std::vector<ReachEdge>& edges, const std::vector<std::string>& roots);

}
