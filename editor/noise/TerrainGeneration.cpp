// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "TerrainGeneration.h"

#include "EditorHost.h"
#include "Project.h"
#include "command/CommandHandle.h"
#include "command/CommandHistory.h"
#include "command/type/TerrainMapDeltaCmd.h"
#include "component/TerrainComponent.h"
#include "graph/GraphRegistry.h"
#include "noise/NoiseKernels.h"
#include "object/TerrainEdit.h"
#include "terrain/EditorTerrainStore.h"
#include "terrain/OverlayMapStore.h"
#include "terrain/TerrainOpsUndo.h"
#include "terrain/TerrainLayers.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>

using namespace jasperengine;
using namespace jasperengine::editor;
using terraincore::MapId;
using terraincore::MapKind;
using terraincore::TexelRect;

namespace {

    constexpr MapId kHeightMap{MapKind::Height, 0};

    struct OutputPlan {
        int index = -1;         // CompiledGraph's
        noise::OutputKind kind = noise::OutputKind::Value;
        int target = 0;         // layer or foliage layer
        std::vector<float> values;
    };

    bool hasError(const std::vector<GenerationIssue>& issues) {
        return std::any_of(issues.begin(), issues.end(), [](const GenerationIssue& issue) { return !issue.warning; });
    }

    void addIssue(GenerationResult& result, const char* code, std::string message, std::string node = {}, bool warning = false) {
        GenerationIssue issue;
        issue.code = code;
        issue.message = std::move(message);
        issue.nodeId = std::move(node);
        issue.warning = warning;
        (warning ? result.warnings : result.issues).push_back(issue);
        if (!warning && result.error.empty()) {
            result.error = issue.message;
            result.errorCode = issue.code;
        }
    }

    float percentile(std::vector<float>& sorted, float p) {
        return sorted.empty() ? 0.0f : sorted[static_cast<size_t>(p * static_cast<float>(sorted.size() - 1))];
    }

    double nowMs() {
        return static_cast<double>(std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now().time_since_epoch()).count()) / 1000.0;
    }

    // Bilinear over texel centres: outputs live on the height lattice, and a blend or density map may
    // have another resolution
    void resample(const std::vector<float>& from, int fromW, int fromH, std::vector<float>& to, int toW, int toH) {
        to.resize(static_cast<size_t>(toW) * static_cast<size_t>(toH));
        if (fromW == toW && fromH == toH) {
            to = from;
            return;
        }
        for (int y = 0; y < toH; y++) {
            const float fy = (static_cast<float>(y) + 0.5f) * static_cast<float>(fromH) / static_cast<float>(toH) - 0.5f;
            const int y0 = std::clamp(static_cast<int>(std::floor(fy)), 0, fromH - 1);
            const int y1 = std::min(y0 + 1, fromH - 1);
            const float ty = std::clamp(fy - static_cast<float>(y0), 0.0f, 1.0f);
            for (int x = 0; x < toW; x++) {
                const float fx = (static_cast<float>(x) + 0.5f) * static_cast<float>(fromW) / static_cast<float>(toW) - 0.5f;
                const int x0 = std::clamp(static_cast<int>(std::floor(fx)), 0, fromW - 1);
                const int x1 = std::min(x0 + 1, fromW - 1);
                const float tx = std::clamp(fx - static_cast<float>(x0), 0.0f, 1.0f);
                const float a = from[static_cast<size_t>(y0) * fromW + x0];
                const float b = from[static_cast<size_t>(y0) * fromW + x1];
                const float c = from[static_cast<size_t>(y1) * fromW + x0];
                const float d = from[static_cast<size_t>(y1) * fromW + x1];
                const float lower = a + (b - a) * tx;
                const float upper = c + (d - c) * tx;
                to[static_cast<size_t>(y) * toW + x] = lower + (upper - lower) * ty;
            }
        }
    }

    // R31: 1 inside, falling to 0 at the region's border, over feather texels
    float featherWeight(int x, int y, int width, int depth, float featherTexels) {
        if (featherTexels <= 0.0f) return 1.0f;
        const float edge = static_cast<float>(std::min({x, y, width - 1 - x, depth - 1 - y})) + 0.5f;
        const float t = std::clamp(edge / featherTexels, 0.0f, 1.0f);
        return t * t * (3.0f - 2.0f * t);
    }

    TexelRect scaleRect(const TexelRect& rect, int fromW, int fromH, int toW, int toH) {
        const int left = static_cast<int>(std::floor(static_cast<double>(rect.x) * toW / fromW));
        const int top = static_cast<int>(std::floor(static_cast<double>(rect.y) * toH / fromH));
        const int right = static_cast<int>(std::ceil(static_cast<double>(rect.right()) * toW / fromW));
        const int bottom = static_cast<int>(std::ceil(static_cast<double>(rect.bottom()) * toH / fromH));
        return TexelRect{left, top, right - left, bottom - top};
    }

}

struct TerrainGeneration::Prepared {
    GenerationRequest request;
    std::shared_ptr<const noise::CompiledGraph> compiled;
    std::vector<OutputPlan> outputs;
    TexelRect rect;                 // on the height map's lattice
    int mapWidth = 0, mapDepth = 0; // what that lattice is
    bool wholeTerrain = true;
    bool replaces = false;
    float maxHeight = 100.0f;
    double unitsPerTexel = 1.0;
    double metresPerTexel = 1.0;
    float featherTexels = 0.0f;
    bool readsHeight = false;
    std::vector<float> heightSnapshot;
    int snapshotWidth = 0, snapshotDepth = 0;
    uint64_t snapshotHash = 0;
    std::vector<float> beforeHeights;   // feather
};

std::string TerrainGeneration::undoNameOf(const GenerationRequest& request) {
    return "Generate Terrain (" + (request.displayName.empty() ? std::string("Noise Graph") : request.displayName) + ", seed " + std::to_string(request.seed) + ")";
}

std::string TerrainGeneration::displayNameOf(const graph::Graph& graph, const std::string& path) {
    auto preset = graph.metadata.find("preset");
    if (preset != graph.metadata.end() && preset->is_object() && preset->contains("name")) return (*preset)["name"].get<std::string>();
    const size_t slash = path.find_last_of("/\\");
    const std::string file = slash == std::string::npos ? path : path.substr(slash + 1);
    const size_t dot = file.rfind('.');
    return dot == std::string::npos ? file : file.substr(0, dot);
}

std::shared_ptr<TerrainGeneration::Prepared> TerrainGeneration::prepare(const terraincore::TerrainMapStore& store, const GenerationRequest& request, GenerationResult& result) {
    auto prepared = std::make_shared<Prepared>();
    prepared->request = request;
    result.dryRun = request.dryRun;
    result.undoName = undoNameOf(request);

    const terraincore::MapFormat heightFormat = store.format(kHeightMap);
    const bool hasHeight = heightFormat.exists && !heightFormat.bounds.empty();
    const int existingWidth = hasHeight ? heightFormat.bounds.w : 0;
    const int existingDepth = hasHeight ? heightFormat.bounds.h : 0;
    const bool whole = !request.region.has_value();

    // R31: resolution
    int width = request.resolution > 0 ? request.resolution : existingWidth;
    int depth = request.resolution > 0 ? request.resolution : existingDepth;
    if (width <= 0) {
        width = depth = 513;
    }
    // A preview's cost is bounded, so it can run where the user's wait is not behind Heavy work
    if (request.interactive && request.dryRun) {
        width = std::min(width, kInteractiveResolution);
        depth = std::min(depth, kInteractiveResolution);
    }
    if (width > 4096 || depth > 4096 || width < 2 || depth < 2) {
        addIssue(result, "bad-resolution", "resolution must be 2..4096, not " + std::to_string(width));
        return nullptr;
    }
    const bool otherSize = hasHeight && (width != existingWidth || depth != existingDepth);
    if (otherSize && !whole && !request.dryRun) {
        addIssue(result, "partial-resolution", "A partial run cannot change the map's resolution (" + std::to_string(existingWidth) +
                 "): drop region or resolution.");
        return nullptr;
    }
    prepared->replaces = !request.dryRun && (otherSize || !hasHeight);
    prepared->mapWidth = width;
    prepared->mapDepth = depth;
    prepared->wholeTerrain = whole;
    prepared->maxHeight = store.frame().maxHeight > 0.0f ? store.frame().maxHeight : 100.0f;

    const float size = request.terrainSize > 0.0f ? request.terrainSize : 1000.0f;
    prepared->metresPerTexel = static_cast<double>(size) / static_cast<double>(width);
    std::string space = "terrain";
    auto meta = request.graph.metadata.find("noise");
    if (meta != request.graph.metadata.end() && meta->is_object()) space = meta->value("space", space);
    prepared->unitsPerTexel = space == "world" ? prepared->metresPerTexel : 1.0 / static_cast<double>(width);

    // R32, R26: the graph, at this resolution
    noise::CompileOptions options;
    options.resolution = width;
    options.variables = request.variables;
    options.maxHeight = prepared->maxHeight;
    noise::CompileResult compiled = noise::compile(request.graph, options);
    for (const graph::Issue& issue : compiled.issues) {
        addIssue(result, issue.code.c_str(), issue.message, issue.nodeId, issue.severity != graph::Severity::Error);
    }
    result.deadNodes = compiled.deadNodes;
    result.foldedNodes = compiled.foldedNodes;
    result.ops = compiled.opCount;
    if (!compiled.graph) {
        if (result.error.empty()) result.error = "The graph did not compile.";
        return nullptr;
    }
    prepared->compiled = compiled.graph;

    // R31: the region, snapped outward to texels and reported
    TexelRect rect{0, 0, width, depth};
    if (request.region) {
        const GenerationRegion& region = *request.region;
        if (!(region.maxX > region.minX) || !(region.maxZ > region.minZ)) {
            addIssue(result, "bad-region", "region's max must be past its min.");
            return nullptr;
        }
        const float half = size * 0.5f;
        const auto texelOf = [&](float local, int count) { return (static_cast<double>(local) + half) / size * count; };
        const int left = static_cast<int>(std::floor(texelOf(region.minX, width)));
        const int top = static_cast<int>(std::floor(texelOf(region.minZ, depth)));
        const int right = static_cast<int>(std::ceil(texelOf(region.maxX, width)));
        const int bottom = static_cast<int>(std::ceil(texelOf(region.maxZ, depth)));
        rect = terraincore::intersect(TexelRect{left, top, right - left, bottom - top}, TexelRect{0, 0, width, depth});
        if (rect.empty()) {
            addIssue(result, "bad-region", "region is outside the terrain (" + std::to_string(-half) + " to " + std::to_string(half) + " metres).");
            return nullptr;
        }
    }
    prepared->rect = rect;
    result.rect = rect;
    prepared->featherTexels = request.feather > 0.0f ? static_cast<float>(request.feather / prepared->metresPerTexel) : 0.0f;

    // R30, R32: what the outputs write must exist, or say optional
    bool anyTarget = false;
    bool usesErosion = false;
    for (const graph::Node& node : request.graph.nodes) {
        usesErosion = usesErosion || node.type == "hydraulic_erosion" || node.type == "thermal_erosion";
    }
    const std::vector<noise::OutputInfo>& outputs = prepared->compiled->outputs();
    for (size_t i = 0; i < outputs.size(); i++) {
        const noise::OutputInfo& info = outputs[i];
        OutputPlan plan;
        plan.index = static_cast<int>(i);
        plan.kind = info.kind;
        plan.target = info.index;
        if (info.kind == noise::OutputKind::Height) {
            if (!request.applyTo.height) continue;
        } else if (info.kind == noise::OutputKind::Layer) {
            if (!request.applyTo.layers) continue;
            if (request.layerCount >= 0 && info.index >= request.layerCount) {
                addIssue(result, "layer-missing", "The terrain has no layer " + std::to_string(info.index) + ".", info.nodeId, info.optional);
                continue;
            }
        } else if (info.kind == noise::OutputKind::Density) {
            if (!request.applyTo.density) continue;
            const MapId map{MapKind::Density, info.index};
            std::string why;
            if (!store.format(map).exists && !store.canCreateMap(map, &why)) {
                addIssue(result, "foliage-layer-missing", "The terrain has no foliage layer " + std::to_string(info.index) + ".", info.nodeId, info.optional);
                continue;
            }
        } else {
            // A Value output has no map; the hash still reports it
        }
        anyTarget = anyTarget || info.kind != noise::OutputKind::Value;
        prepared->outputs.push_back(std::move(plan));
    }
    if (hasError(result.issues)) return nullptr;
    if (!anyTarget) {
        addIssue(result, "no-output", "Nothing to write: the graph has no output for what apply_to asks.");
        return nullptr;
    }
    // R27
    if (usesErosion && !whole) {
        addIssue(result, "erosion-region-seam", "Erosion on a region is not guaranteed to match a whole-map run at its border.", {}, true);
    }

    // R28: snapshot what the graph reads
    prepared->readsHeight = prepared->compiled->readsTerrainHeight();
    if (prepared->readsHeight) {
        if (!hasHeight) {
            addIssue(result, "no-height-map", "The graph reads Terrain Height, and the terrain has no height map: create one first.");
            return nullptr;
        }
        prepared->snapshotWidth = existingWidth;
        prepared->snapshotDepth = existingDepth;
        prepared->heightSnapshot.resize(static_cast<size_t>(existingWidth) * static_cast<size_t>(existingDepth));
        if (!store.read(kHeightMap, heightFormat.bounds, prepared->heightSnapshot)) {
            addIssue(result, "no-height-map", "Could not read the terrain's height map.");
            return nullptr;
        }
        prepared->snapshotHash = noise::hashFloats(prepared->heightSnapshot.data(), prepared->heightSnapshot.size());
    }
    if (prepared->featherTexels > 0.0f && hasHeight && !otherSize) {
        prepared->beforeHeights.resize(static_cast<size_t>(rect.area()));
        store.read(kHeightMap, rect, prepared->beforeHeights);
    }
    result.ok = true;
    return prepared;
}

bool TerrainGeneration::evaluate(Prepared& prepared, jobs::CancelToken* cancel, bool parallel, GenerationResult& result) {
    const double started = nowMs();
    noise::GridDomain domain;
    domain.startX = prepared.rect.x;
    domain.startZ = prepared.rect.y;
    domain.width = prepared.rect.w;
    domain.depth = prepared.rect.h;
    domain.unitsPerTexel = prepared.unitsPerTexel;

    noise::EvalReport report;
    noise::EvalContext context;
    context.seed = prepared.request.seed;
    context.maxHeight = prepared.maxHeight;
    context.metresPerTexel = prepared.metresPerTexel;
    context.cancel = cancel;
    context.parallel = parallel;
    context.report = &report;
    context.variables = prepared.request.variables;
    context.uvPerUnit = 1.0 / (prepared.unitsPerTexel * static_cast<double>(prepared.mapWidth));
    if (prepared.readsHeight) {
        context.terrainHeight = prepared.heightSnapshot.data();
        // prepare read it whole, in its own lattice; a run at another resolution reads the nearest edge
        context.terrainHeightWidth = prepared.snapshotWidth;
        context.terrainHeightDepth = prepared.snapshotDepth;
    }

    std::vector<int> indices;
    std::vector<float*> targets;
    const size_t texels = static_cast<size_t>(prepared.rect.area());
    for (OutputPlan& plan : prepared.outputs) {
        plan.values.resize(texels);
        indices.push_back(plan.index);
        targets.push_back(plan.values.data());
    }
    const bool finished = prepared.compiled->evaluateGridOutputs(domain, context, indices, targets);
    result.evaluateMs = nowMs() - started;
    result.erosionMs = report.erosionMs;
    result.dropletSteps = report.dropletSteps;
    result.peakBytes = report.peakBytes + texels * sizeof(float) * prepared.outputs.size();
    if (!finished) {
        result.ok = false;
        result.cancelled = true;
        return false;
    }

    const std::vector<noise::OutputInfo>& infos = prepared.compiled->outputs();
    const OutputPlan* height = nullptr;
    for (const OutputPlan& plan : prepared.outputs) {
        GenerationOutputStats stats;
        stats.name = infos[static_cast<size_t>(plan.index)].name;
        stats.hash = noise::hashFloats(plan.values.data(), plan.values.size());
        double sum = 0.0;
        for (float value : plan.values) sum += std::clamp(value, 0.0f, 1.0f);
        stats.coverage = texels > 0 ? static_cast<float>(sum / static_cast<double>(texels)) : 0.0f;
        result.outputs.push_back(std::move(stats));
        if (plan.kind == noise::OutputKind::Height) height = &plan;
    }

    // R63's statistics, on what would be stored
    result.width = prepared.rect.w;
    result.depth = prepared.rect.h;
    if (height) {
        std::vector<float> sorted = height->values;
        double sum = 0.0;
        for (float value : height->values) {
            sum += value;
            if (value < 0.0f) result.stats.clippedLow++;
            else if (value > 1.0f) result.stats.clippedHigh++;
        }
        std::sort(sorted.begin(), sorted.end());
        result.stats.heightMin = sorted.front();
        result.stats.heightMax = sorted.back();
        result.stats.heightMean = static_cast<float>(sum / static_cast<double>(texels));
        result.stats.p5 = percentile(sorted, 0.05f);
        result.stats.p50 = percentile(sorted, 0.5f);
        result.stats.p95 = percentile(sorted, 0.95f);
        result.stats.slopeHistogram.assign(18, 0);
        const int w = prepared.rect.w, d = prepared.rect.h;
        const float scale = prepared.maxHeight * 0.5f / static_cast<float>(prepared.metresPerTexel);
        for (int z = 1; z + 1 < d; z++) {
            for (int x = 1; x + 1 < w; x++) {
                const size_t i = static_cast<size_t>(z) * w + x;
                const float gx = (height->values[i + 1] - height->values[i - 1]) * scale;
                const float gz = (height->values[i + w] - height->values[i - w]) * scale;
                // Report only: libm is fine here, no bit of it reaches a map
                const float degrees = std::atan(std::sqrt(gx * gx + gz * gz)) * 57.29578f;
                result.stats.slopeHistogram[static_cast<size_t>(std::min(17, static_cast<int>(degrees / 5.0f)))]++;
            }
        }
        result.heights = height->values;
    }
    return true;
}

bool TerrainGeneration::stillMatches(const Prepared& prepared, const terraincore::TerrainMapStore& store) {
    if (!prepared.readsHeight) return true;
    const terraincore::MapFormat format = store.format(kHeightMap);
    if (!format.exists || static_cast<size_t>(format.bounds.area()) != prepared.heightSnapshot.size()) return false;
    std::vector<float> now(prepared.heightSnapshot.size());
    if (!store.read(kHeightMap, format.bounds, now)) return false;
    return noise::hashFloats(now.data(), now.size()) == prepared.snapshotHash;
}

std::vector<int> TerrainGeneration::layersOf(const Prepared& prepared) {
    std::vector<int> layers;
    for (const OutputPlan& plan : prepared.outputs) {
        if (plan.kind == noise::OutputKind::Layer) layers.push_back(plan.target);
    }
    return layers;
}

std::vector<int> TerrainGeneration::densitiesOf(const Prepared& prepared) {
    std::vector<int> layers;
    for (const OutputPlan& plan : prepared.outputs) {
        if (plan.kind == noise::OutputKind::Density) layers.push_back(plan.target);
    }
    return layers;
}

TexelRect TerrainGeneration::heightRectOf(const Prepared& prepared) {
    return prepared.rect;
}

bool TerrainGeneration::replacesHeightMap(const Prepared& prepared) {
    return prepared.replaces;
}

bool TerrainGeneration::apply(Prepared& prepared, terraincore::TerrainMapStore& store, GenerationResult& result) {
    const double started = nowMs();
    const TexelRect rect = prepared.rect;
    std::unique_ptr<terraincore::OverlayMapStore> dryOverlay;
    terraincore::TerrainMapStore* target = &store;
    // A dry run goes through the same route onto an overlay it then drops, so changed tells the truth
    // and nothing is written (R63)
    if (prepared.request.dryRun) {
        dryOverlay = std::make_unique<terraincore::OverlayMapStore>(store);
        target = dryOverlay.get();
    }
    const bool sameLattice = target->format(kHeightMap).exists && target->format(kHeightMap).bounds.w == prepared.mapWidth &&
                             target->format(kHeightMap).bounds.h == prepared.mapDepth;

    terraincore::Batch batch(*target);
    std::vector<int> layerIndices;
    std::vector<const OutputPlan*> layerPlans;
    for (const OutputPlan& plan : prepared.outputs) {
        if (plan.kind == noise::OutputKind::Height) {
            // A dry run at another resolution has no map of that shape to write through: stats only
            if (!sameLattice) continue;
            std::vector<float> values = plan.values;
            if (prepared.featherTexels > 0.0f && !prepared.beforeHeights.empty()) {
                for (int y = 0; y < rect.h; y++) {
                    for (int x = 0; x < rect.w; x++) {
                        const size_t i = static_cast<size_t>(y) * rect.w + x;
                        const float weight = featherWeight(x, y, rect.w, rect.h, prepared.featherTexels);
                        values[i] = prepared.beforeHeights[i] + (values[i] - prepared.beforeHeights[i]) * weight;
                    }
                }
            }
            // Batch keeps a failure: commit reports it and writes nothing
            batch.setHeights(rect, values);
        } else if (plan.kind == noise::OutputKind::Layer) {
            layerIndices.push_back(plan.target);
            layerPlans.push_back(&plan);
        } else if (plan.kind == noise::OutputKind::Density) {
            const MapId map{MapKind::Density, plan.target};
            terraincore::MapFormat format = target->format(map);
            int mapW = prepared.mapWidth, mapH = prepared.mapDepth;
            if (format.exists) {
                mapW = format.bounds.w;
                mapH = format.bounds.h;
            }
            const TexelRect densityRect = scaleRect(rect, prepared.mapWidth, prepared.mapDepth, mapW, mapH);
            std::vector<float> values;
            resample(plan.values, rect.w, rect.h, values, densityRect.w, densityRect.h);
            if (prepared.featherTexels > 0.0f && format.exists) {
                std::vector<float> before(values.size());
                if (target->read(map, densityRect, before)) {
                    const float feather = prepared.featherTexels * static_cast<float>(mapW) / static_cast<float>(prepared.mapWidth);
                    for (int y = 0; y < densityRect.h; y++) {
                        for (int x = 0; x < densityRect.w; x++) {
                            const size_t i = static_cast<size_t>(y) * densityRect.w + x;
                            values[i] = before[i] + (values[i] - before[i]) * featherWeight(x, y, densityRect.w, densityRect.h, feather);
                        }
                    }
                }
            }
            batch.setDensity(densityRect, plan.target, values);
        }
    }
    if (!layerPlans.empty()) {
        // Every blend map shares one lattice: the first's, or the height map's when none exists yet
        int mapW = prepared.mapWidth, mapH = prepared.mapDepth;
        const terraincore::MapFormat blend = target->format(MapId{MapKind::Blend, 0});
        if (blend.exists) {
            mapW = blend.bounds.w;
            mapH = blend.bounds.h;
        }
        const TexelRect blendRect = scaleRect(rect, prepared.mapWidth, prepared.mapDepth, mapW, mapH);
        const size_t texels = static_cast<size_t>(blendRect.area());
        std::vector<float> interleaved(texels * layerPlans.size());
        std::vector<float> resampled;
        for (size_t l = 0; l < layerPlans.size(); l++) {
            resample(layerPlans[l]->values, rect.w, rect.h, resampled, blendRect.w, blendRect.h);
            for (size_t i = 0; i < texels; i++) interleaved[i * layerPlans.size() + l] = resampled[i];
        }
        if (prepared.featherTexels > 0.0f && blend.exists) {
            // Over the old weights of the listed layers, from the snapshot of the blend maps they live in
            const float feather = prepared.featherTexels * static_cast<float>(mapW) / static_cast<float>(prepared.mapWidth);
            for (size_t l = 0; l < layerPlans.size(); l++) {
                const terraincore::LayerChannel where = terraincore::terrainLayerChannel(layerIndices[l]);
                const MapId map{MapKind::Blend, where.map};
                const terraincore::MapFormat format = target->format(map);
                if (!format.exists) continue;
                std::vector<float> stored(texels * static_cast<size_t>(format.channels));
                if (!target->read(map, blendRect, stored)) continue;
                for (int y = 0; y < blendRect.h; y++) {
                    for (int x = 0; x < blendRect.w; x++) {
                        const size_t i = static_cast<size_t>(y) * blendRect.w + x;
                        float old = stored[i * static_cast<size_t>(format.channels) + static_cast<size_t>(where.channel)];
                        if (where.inverted) old = 1.0f - old;
                        float& value = interleaved[i * layerPlans.size() + l];
                        value = old + (value - old) * featherWeight(x, y, blendRect.w, blendRect.h, feather);
                    }
                }
            }
        }
        batch.setLayerWeights(blendRect, layerIndices, interleaved);
    }

    terraincore::TerrainOpResult committed = batch.commit();
    if (dryOverlay) dryOverlay->discard();
    result.applyMs = nowMs() - started;
    for (const terraincore::OpWarning& warning : committed.warnings) {
        // clipped_low and clipped_high are counts here, not warnings (R31)
        if (warning.code != "clipped_low" && warning.code != "clipped_high") addIssue(result, warning.code.c_str(), warning.message, {}, true);
    }
    if (!committed.ok) {
        result.ok = false;
        addIssue(result, "apply-failed", committed.error.empty() ? "The write failed." : committed.error);
        return false;
    }
    result.changed = committed.changed;
    result.ok = true;
    return true;
}

namespace {

    struct RunState : TerrainGeneration::Run {
        std::atomic<bool> done{false};
        std::atomic<float> done01{0.0f};
        jobs::CancelToken token;
        void cancel() override { token.cancel(); }
        bool finished() const override { return done.load(std::memory_order_acquire); }
        float progress() const override { return done01.load(std::memory_order_relaxed); }
    };

    std::function<void()>& jobFault() {
        static std::function<void()> fault;
        return fault;
    }

    SceneProject* sceneOf(Project* project, uint32_t sceneId) {
        SceneProject* scene = project ? project->getScene(sceneId) : nullptr;
        return scene && scene->scene ? scene : nullptr;
    }

    // The maps a run writes that the terrain lacks, with the size each is made at: the height map when the run
    // replaces it, the blend maps its layers need, the density maps of its foliage layers
    std::vector<std::pair<MapId, terraincore::MapFormat>> mapsToMake(const TerrainGeneration::Prepared& prepared, const terraincore::TerrainMapStore& store,
                                                                 int densityResolution) {
        std::vector<std::pair<MapId, terraincore::MapFormat>> out;
        const auto sized = [](int width, int depth) {
            terraincore::MapFormat format;
            format.bounds = TexelRect{0, 0, width, depth};
            return format;
        };
        const bool writesHeight = std::any_of(prepared.outputs.begin(), prepared.outputs.end(), [](const OutputPlan& plan) { return plan.kind == noise::OutputKind::Height; });
        if (prepared.replaces && writesHeight) out.emplace_back(kHeightMap, sized(prepared.mapWidth, prepared.mapDepth));
        const std::vector<int> layers = TerrainGeneration::layersOf(prepared);
        if (!layers.empty()) {
            const int needed = terraincore::terrainBlendMapCount(*std::max_element(layers.begin(), layers.end()) + 1);
            const terraincore::MapFormat first = store.format(MapId{MapKind::Blend, 0});
            const int side = first.exists ? first.bounds.w : prepared.mapWidth;
            for (int map = 0; map < needed; map++) {
                if (!store.format(MapId{MapKind::Blend, map}).exists) out.emplace_back(MapId{MapKind::Blend, map}, sized(side, side));
            }
        }
        for (int density : TerrainGeneration::densitiesOf(prepared)) {
            if (!store.format(MapId{MapKind::Density, density}).exists) out.emplace_back(MapId{MapKind::Density, density}, sized(densityResolution, densityResolution));
        }
        return out;
    }

}

void TerrainGeneration::setJobFaultForTests(std::function<void()> fault) {
    jobFault() = std::move(fault);
}

std::shared_ptr<TerrainGeneration::Run> TerrainGeneration::start(Project* project, uint32_t sceneId, Entity entity, GenerationRequest request, Done done) {
    GenerationResult refused;
    SceneProject* sceneProject = sceneOf(project, sceneId);
    TerrainComponent* component = sceneProject ? sceneProject->scene->findComponent<TerrainComponent>(entity) : nullptr;
    if (!component) {
        addIssue(refused, "no-terrain", "The GameObject has no Terrain.");
        done(refused);
        return nullptr;
    }
    if (sceneProject->playState != ScenePlayState::STOPPED) {
        addIssue(refused, "playing", "Scene " + std::to_string(sceneId) + " is playing: stop play mode before generating.");
        done(refused);
        return nullptr;
    }
    request.terrainSize = component->terrainSize;
    request.layerCount = static_cast<int>(component->surfaceLayers.size());
    if (component->heightMap.empty() && request.resolution <= 0) {
        request.resolution = project->getTerrainEditorSettings().heightMapResolution;
    }

    TerrainComponentStore store(sceneProject->scene, entity, *component);
    auto result = std::make_shared<GenerationResult>();
    std::shared_ptr<Prepared> prepared = prepare(store, request, *result);
    if (!prepared) {
        result->ok = false;
        done(*result);
        return nullptr;
    }

    auto run = std::make_shared<RunState>();
    run->done01 = 0.05f;

    jobs::SubmitOptions options;
    if (prepared->request.interactive && prepared->request.dryRun) {
        // A Preview a person waits on: ahead of the Heavy Background work (shader compiles, cooks) that holds
        // the gate's only slot on a small machine. Bounded by kInteractiveResolution.
        options.lane = jobs::Lane::Normal;
        options.name = "preview-terrain";
    } else {
        options.lane = jobs::Lane::Background;
        options.gate = jobs::GateClass::Heavy;
        options.name = "generate-terrain";
    }
    auto finish = [run, done](const GenerationResult& final) {
        run->done01 = 1.0f;
        run->done.store(true, std::memory_order_release);
        done(final);
    };
    jobs::submit([run, prepared, result, project, sceneId, entity, finish, fault = jobFault()]() {
        // done always runs, once: a job that threw would otherwise leave its caller's progress bar up forever
        std::string failure;
        bool finished = false;
        try {
            if (fault) fault();
            finished = evaluate(*prepared, &run->token, true, *result);
        } catch (const std::exception& e) {
            failure = e.what();
        } catch (...) {
            failure = "unknown error";
        }
        run->done01 = 0.9f;
        // The editor's own queue, not jobs::postToMain: that one drains only in a frame that draws a scene, and
        // an idle editor draws none, so a Preview's result would wait for the next unrelated redraw
        getEditorHost().enqueueMainThreadTask([run, prepared, result, project, sceneId, entity, finish, finished, failure]() {
            // the same on the main thread: an exception here would skip done and keep the run up forever
            const auto onMain = [&]() {
                if (!failure.empty()) {
                    result->ok = false;
                    addIssue(*result, "evaluate-failed", "The generation failed: " + failure);
                    finish(*result);
                    return;
                }
                if (!finished || run->token.isCancelled()) {
                    result->ok = false;
                    result->cancelled = true;
                    finish(*result);
                    return;
                }
                SceneProject* scene = sceneOf(project, sceneId);
                TerrainComponent* terrainNow = scene ? scene->scene->findComponent<TerrainComponent>(entity) : nullptr;
                if (!terrainNow) {
                    result->ok = false;
                    addIssue(*result, "no-terrain", "The terrain is gone.");
                    finish(*result);
                    return;
                }
                TerrainComponentStore storeNow(scene->scene, entity, *terrainNow);
                if (!stillMatches(*prepared, storeNow)) {
                    result->ok = false;
                    addIssue(*result, "terrain-changed", "The terrain changed while generating: nothing was written. Run it again.");
                    finish(*result);
                    return;
                }
                if (prepared->request.dryRun) {
                    apply(*prepared, storeNow, *result);
                    finish(*result);
                    return;
                }

                // One undo step (R29), made as T1's edit_terrain makes one: the maps the run needs and the terrain
                // lacks, made blank and each kept by a structural step holding its final pixels, then one tile
                // delta over the maps that were there, all grouped
                CommandHistory* history = CommandHandle::get(sceneId);
                const size_t mark = history->mark();
                const auto fail = [&](const std::string& message) {
                    while (history->position() > mark && history->canUndo() && history->undo()) {}
                    result->ok = false;
                    addIssue(*result, "apply-failed", message);
                    finish(*result);
                };
                std::vector<TerrainMadeMap> made;
                std::vector<MapId> madeIds;
                for (const auto& [map, format] : mapsToMake(*prepared, storeNow, project->getTerrainEditorSettings().densityMapResolution)) {
                    made.emplace_back();
                    if (!makeTerrainBlankMap(project, scene, entity, map, format, made.back())) {
                        made.pop_back();
                        fail(std::string("The ") + (map.kind == MapKind::Height ? "height" : map.kind == MapKind::Blend ? "blend" : "density") +
                             " map could not be made.");
                        return;
                    }
                    madeIds.push_back(map);
                }
                EditorTerrainStore writer(project, sceneId, scene->scene, entity, scene->scene->getComponent<TerrainComponent>(entity));
                terraincore::OverlayMapStore overlay(writer);
                if (!apply(*prepared, overlay, *result)) {
                    fail(result->error.empty() ? "The write failed: nothing changed." : result->error);
                    return;
                }
                std::string error;
                for (const TerrainMadeMap& map : made) {
                    if (!keepTerrainMadeMap(project, scene, entity, map, overlay, result->undoName, error)) {
                        fail(error);
                        return;
                    }
                }
                auto delta = std::make_unique<TerrainMapDeltaCmd>(project, sceneId, entity, entity, overlay, false, result->undoName, madeIds);
                if (!delta->empty()) {
                    Command* command = delta.release();
                    command->setNoMerge();
                    if (!history->addCommand(command)) {
                        fail("The write failed: nothing changed.");
                        return;
                    }
                }
                overlay.discard();
                history->groupSince(mark, result->undoName);
                finish(*result);
            };
            try {
                onMain();
            } catch (const std::exception& e) {
                if (run->finished()) return;
                result->ok = false;
                addIssue(*result, "apply-failed", std::string("The generation failed: ") + e.what());
                finish(*result);
            }
        });
    }, options);
    return run;
}
