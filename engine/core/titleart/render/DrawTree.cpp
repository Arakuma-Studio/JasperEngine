// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT
//
// Ported from Pachislop src/engine/drawTree.ts by SafEight, used with the author's permission.

#include "DrawTree.h"

#include <algorithm>
#include <set>

using namespace jasperengine;
using namespace jasperengine::titleart;

namespace {

    struct Builder {
        const DrawTreeOptions& options;
        std::set<std::string> keep;         // isolation or content layers: their ancestors and themselves
        std::set<std::string> wanted;       // the layers drawn whole (isolated or listed)
        bool filtered = false;

        bool selfVisible(const Json& layer) const {
            if (!layer.value("visible", true)) return false;
            if (!(layer.value("opacity", 1.0) > 0.0)) return false;
            if (!options.includeGuides && layer.value("guide", false)) return false;
            return true;
        }

        // ids on the way to a wanted layer, true when `layer` holds one
        bool markPaths(const Json& layer) {
            const std::string id = layer.value("id", std::string());
            bool holds = wanted.count(id) > 0;
            for (const Json& child : layerChildren(layer)) holds = markPaths(child) || holds;
            if (holds) keep.insert(id);
            return holds;
        }

        // the drawn node of one layer; false when it draws nothing. An isolated layer and its
        // ancestors draw even when hidden (inspecting a layer alone, a layer mask's source).
        bool build(const Json& layer, int depth, bool insideWanted, DrawNode& out) const {
            const std::string id = layer.value("id", std::string());
            const bool isWanted = insideWanted || wanted.count(id) > 0;
            if (filtered && !isWanted && !keep.count(id)) return false;
            const bool forced = !options.isolateLayer.empty() && !insideWanted && keep.count(id) > 0;
            if (!forced && !selfVisible(layer)) return false;
            const LayerType type = layerTypeOf(layer);
            if (type == LayerType::Unknown) return false;
            out.layer = &layer;
            out.layerId = id;
            out.type = type;
            out.depth = depth;
            if (type == LayerType::Group) {
                out.kind = DrawNodeKind::Group;
                out.children = buildSiblings(layerChildren(layer), depth + 1, isWanted);
                return !out.children.empty();
            }
            out.kind = type == LayerType::Adjustment ? DrawNodeKind::Adjust : DrawNodeKind::Leaf;
            return true;
        }

        std::vector<DrawNode> buildSiblings(const Json& layers, int depth, bool insideWanted) const {
            std::vector<DrawNode> nodes;
            // the clip base of clip-on siblings above: none (clip ignored), hidden (they hide
            // too) or the index of its node
            constexpr long NO_BASE = -2, HIDDEN_BASE = -1;
            long base = NO_BASE;
            for (const Json& layer : layers) {
                const bool clip = layer.value("clip", false);
                if (clip && base != NO_BASE) {
                    DrawNode node;
                    if (base != HIDDEN_BASE && build(layer, depth, insideWanted, node)) nodes[(size_t)base].clipped.push_back(std::move(node));
                    continue;
                }
                DrawNode node;
                const bool drawn = build(layer, depth, insideWanted, node);
                if (drawn) nodes.push_back(std::move(node));
                if (!clip) {
                    base = layerTypeOf(layer) == LayerType::Adjustment ? NO_BASE : (drawn ? (long)nodes.size() - 1 : HIDDEN_BASE);
                }
            }
            return nodes;
        }
    };

}

std::vector<DrawNode> titleart::buildDrawTree(const TitleArtDoc& doc, const DrawTreeOptions& options) {
    Builder builder{options, {}, {}, false};
    if (!options.isolateLayer.empty()) {
        builder.wanted.insert(options.isolateLayer);
        builder.filtered = true;
    } else if (options.content == OutputContent::Layers) {
        builder.wanted.insert(options.contentLayers.begin(), options.contentLayers.end());
        builder.filtered = true;
    }
    if (builder.filtered) {
        for (const Json& layer : doc.layers()) builder.markPaths(layer);
    }
    return builder.buildSiblings(doc.layers(), 0, false);
}
