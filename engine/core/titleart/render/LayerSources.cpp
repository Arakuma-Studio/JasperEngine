// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT
//
// Ported from Pachislop src/engine/layerSource.ts (FrameSource: a layer drawn alone once per
// frame for the layers that read it, its cycle guard) by SafEight, used with the author's
// permission.

#include "TitlePlanner.h"

using namespace jasperengine;
using namespace jasperengine::titleart;

TitlePx TitlePlanner::layerSource(const std::string& layerId, const char* cycleProblem, const std::string& reader) {
    auto known = sources.find(layerId);
    if (known != sources.end()) return known->second;
    if (layerId.empty() || !findLayer(document->root, layerId)) return {};
    if (activeSources.count(layerId) || layerId == reader) {
        // a layer that is being drawn (or reads itself) yields none: drawn as fully shown
        TitleProblem p;
        p.code = cycleProblem;
        p.layer = reader;
        p.message = "the layer " + layerId + " reads itself through its sources: shown whole";
        problem(p);
        return {};
    }
    activeSources.insert(layerId);
    activeSources.insert(reader);
    // the layer alone, its ancestors shown (their transforms place it), guides drawn
    DrawTreeOptions options;
    options.includeGuides = true;
    options.isolateLayer = layerId;
    const std::vector<DrawNode>& nodes = keepTree(buildDrawTree(*document, options));
    TitlePx acc = clearedLike(outRegion, layerId);
    acc = compose(nodes, acc, nullptr);
    activeSources.erase(layerId);
    activeSources.erase(reader);
    // kept for every reader of the render (released with the job's state)
    sources[layerId] = acc;
    return acc;
}

TitlePx TitlePlanner::layerContentSource(const std::string& layerId, const char* cycleProblem, const std::string& reader) {
    const std::string key = "content:" + layerId;
    auto known = sources.find(key);
    if (known != sources.end()) return known->second;
    if (layerId.empty() || !findLayer(document->root, layerId)) return {};
    if (activeSources.count(layerId) || layerId == reader) {
        TitleProblem p;
        p.code = cycleProblem;
        p.layer = reader;
        p.message = "the layer " + layerId + " reads itself through its sources: shown whole";
        problem(p);
        return {};
    }
    activeSources.insert(layerId);
    activeSources.insert(reader);
    DrawTreeOptions options;
    options.includeGuides = true;
    options.isolateLayer = layerId;
    const std::vector<DrawNode>& nodes = keepTree(buildDrawTree(*document, options));
    TitlePx acc = clearedLike(outRegion, layerId);
    // the isolated layer skips its own effect stack (unit() reads effectsOffLayer, never caches it)
    const std::string outer = effectsOffLayer;
    effectsOffLayer = layerId;
    acc = compose(nodes, acc, nullptr);
    effectsOffLayer = outer;
    activeSources.erase(layerId);
    activeSources.erase(reader);
    sources[key] = acc;
    return acc;
}
