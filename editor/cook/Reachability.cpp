// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "Reachability.h"

#include "asset/AssetIndex.h"
#include "util/FileUtils.h"

#include <algorithm>
#include <cstdint>
#include <string_view>
#include <unordered_map>

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
    // every asset named once, as a number: the walk then runs on integers and flat arrays
    std::unordered_map<std::string_view, uint32_t> ids;
    ids.reserve(edges.size() + roots.size());
    std::vector<std::string_view> names;
    names.reserve(edges.size() + roots.size());
    auto idOf = [&](std::string_view name){
        const auto [it, inserted] = ids.try_emplace(name, (uint32_t)names.size());
        if (inserted) names.push_back(name);
        return it->second;
    };
    std::vector<uint32_t> from(edges.size()), to(edges.size());
    for (size_t i = 0; i < edges.size(); i++){
        from[i] = idOf(edges[i].from);
        to[i] = edges[i].to.empty() ? UINT32_MAX : idOf(edges[i].to);
    }
    for (const std::string& root : roots)
        if (!root.empty()) idOf(root);

    // each referrer's edges side by side (offsets into one array), in field locator then target order (R3)
    std::vector<uint32_t> first(names.size() + 1, 0);
    for (uint32_t f : from) first[f + 1]++;
    for (size_t i = 1; i < first.size(); i++) first[i] += first[i - 1];
    std::vector<uint32_t> order(edges.size());
    {
        std::vector<uint32_t> fill(first.begin(), first.end() - 1);
        for (size_t i = 0; i < edges.size(); i++) order[fill[from[i]]++] = (uint32_t)i;
    }
    for (size_t node = 0; node + 1 < first.size(); node++)
        std::sort(order.begin() + first[node], order.begin() + first[node + 1], [&edges](uint32_t a, uint32_t b){
            const ReachEdge& x = edges[a];
            const ReachEdge& y = edges[b];
            return x.field != y.field ? x.field < y.field : x.to < y.to;
        });

    // breadth first from the roots in the order given; each asset keeps the edge that first reached it
    constexpr uint32_t kUnseen = UINT32_MAX, kRoot = UINT32_MAX - 1;
    std::vector<uint32_t> via(names.size(), kUnseen);
    std::vector<uint32_t> queue;
    queue.reserve(names.size());
    for (const std::string& root : roots){
        if (root.empty()) continue;
        const uint32_t id = ids.at(root);
        if (via[id] != kUnseen) continue;
        via[id] = kRoot;
        queue.push_back(id);
    }
    for (size_t head = 0; head < queue.size(); head++){
        const uint32_t node = queue[head];
        for (uint32_t k = first[node]; k < first[node + 1]; k++){
            const uint32_t edge = order[k];
            const uint32_t target = to[edge];
            if (target == UINT32_MAX || via[target] != kUnseen) continue;
            via[target] = edge;
            queue.push_back(target);
        }
    }

    // the result in name order, so every insertion goes at the end
    std::sort(queue.begin(), queue.end(), [&names](uint32_t a, uint32_t b){ return names[a] < names[b]; });
    std::map<std::string, BuildReportReach> reached;
    for (uint32_t node : queue){
        if (via[node] == kRoot){
            reached.emplace_hint(reached.end(), std::string(names[node]), BuildReportReach{std::string(), std::string(), "root", std::string()});
        } else {
            const ReachEdge& edge = edges[via[node]];
            reached.emplace_hint(reached.end(), std::string(names[node]), BuildReportReach{edge.from, edge.field, edge.kind, edge.object});
        }
    }
    return reached;
}
