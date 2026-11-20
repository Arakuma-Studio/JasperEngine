// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "Reachability.h"

#include "asset/AssetIndex.h"
#include "util/FileUtils.h"

#include <algorithm>
#include <numeric>
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
    // indexes sorted by referrer, field, target: each referrer's edges are one run, in R3's order
    std::vector<size_t> order(edges.size());
    std::iota(order.begin(), order.end(), size_t(0));
    std::sort(order.begin(), order.end(), [&edges](size_t a, size_t b){
        const ReachEdge& x = edges[a];
        const ReachEdge& y = edges[b];
        if (x.from != y.from) return x.from < y.from;
        if (x.field != y.field) return x.field < y.field;
        return x.to < y.to;
    });
    std::unordered_map<std::string_view, std::pair<size_t, size_t>> runs;
    runs.reserve(edges.size());
    for (size_t i = 0; i < order.size();){
        size_t end = i + 1;
        while (end < order.size() && edges[order[end]].from == edges[order[i]].from) end++;
        runs[edges[order[i]].from] = {i, end};
        i = end;
    }

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
        for (size_t i = run->second.first; i < run->second.second; i++){
            const ReachEdge& edge = edges[order[i]];
            if (edge.to.empty() || !seen.insert(edge.to).second) continue;
            reached[edge.to] = {edge.from, edge.field, edge.kind, edge.object};
            queue.push_back(edge.to);
        }
    }
    return reached;
}
