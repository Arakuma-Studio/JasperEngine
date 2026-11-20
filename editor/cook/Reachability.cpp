// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "Reachability.h"

#include "asset/AssetIndex.h"
#include "util/FileUtils.h"

#include <algorithm>
#include <string_view>
#include <unordered_map>
#include <unordered_set>

using namespace jasperengine;
using namespace jasperengine::editor;

std::string jasperengine::editor::reachAssetKey(const std::string& projectRelative, const std::string& assetsPrefix){
    if (assetsPrefix.empty() || projectRelative.compare(0, assetsPrefix.size(), assetsPrefix) != 0) return projectRelative;
    return projectRelative.substr(assetsPrefix.size());
}

std::vector<ReachEdge> jasperengine::editor::reachEdgesOf(const AssetIndex& index, const AssetDatabase& database, const std::string& assetsPrefix){
    std::vector<ReachEdge> edges;
    std::unordered_map<Guid, std::string> referrers;
    index.forEachEdge([&](const DepEdge& edge){
        auto from = referrers.find(edge.referrer);
        if (from == referrers.end()) from = referrers.emplace(edge.referrer, reachAssetKey(index.referrerPath(edge.referrer), assetsPrefix)).first;
        std::string target = edge.targetPath;
        if (!edge.target.isNull()){
            const std::filesystem::path resolved = database.getPath(edge.target);
            if (!resolved.empty()) target = FileUtils::pathToGenericUtf8(resolved);
        }
        if (!target.empty()) edges.push_back({from->second, reachAssetKey(target, assetsPrefix), edge.field, depEdgeKindName(edge.kind), edge.entityPath});
    });
    return edges;
}

std::map<std::string, BuildReportReach> jasperengine::editor::computeReachability(const std::vector<ReachEdge>& edges,
                                                                                  const std::vector<std::string>& roots){
    // each referrer's edges, in field locator then target order (R3): grouped by a hash, each group sorted
    std::unordered_map<std::string_view, std::vector<size_t>> runs;
    runs.reserve(edges.size());
    for (size_t i = 0; i < edges.size(); i++) runs[edges[i].from].push_back(i);
    for (auto& run : runs)
        std::sort(run.second.begin(), run.second.end(), [&edges](size_t a, size_t b){
            const ReachEdge& x = edges[a];
            const ReachEdge& y = edges[b];
            return x.field != y.field ? x.field < y.field : x.to < y.to;
        });

    std::map<std::string, BuildReportReach> reached;
    std::unordered_set<std::string_view> seen;
    std::vector<std::string_view> queue;
    queue.reserve(roots.size() + edges.size());
    for (const std::string& root : roots){
        if (root.empty() || !seen.insert(root).second) continue;
        reached[root] = {std::string(), std::string(), "root", std::string()};
        queue.push_back(root);
    }
    for (size_t head = 0; head < queue.size(); head++){
        const auto run = runs.find(queue[head]);
        if (run == runs.end()) continue;
        for (size_t index : run->second){
            const ReachEdge& edge = edges[index];
            if (edge.to.empty() || !seen.insert(edge.to).second) continue;
            reached[edge.to] = {edge.from, edge.field, edge.kind, edge.object};
            queue.push_back(edge.to);
        }
    }
    return reached;
}
