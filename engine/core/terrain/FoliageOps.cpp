// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "terrain/FoliageOps.h"

#include "terrain/FoliageScatter.h"
#include "terrain/TerrainDeterministic.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <set>

using namespace jasperengine;
using namespace jasperengine::terrain;

namespace {

    // the runner applies an op on a job thread, and two Terrains may run theirs at once
    thread_local FoliageOpHost* foliageCurrentHost = nullptr;

    OpParam numberParam(double value) {
        OpParam param;
        param.kind = OpParam::Kind::Number;
        param.number = value;
        return param;
    }

    OpParam textParam(const std::string& value) {
        OpParam param;
        param.kind = OpParam::Kind::String;
        param.text = value;
        return param;
    }

    OpParam objectParam() {
        OpParam param;
        param.kind = OpParam::Kind::Object;
        return param;
    }

    OpParam listParam() {
        OpParam param;
        param.kind = OpParam::Kind::List;
        return param;
    }

    std::string hexHash(uint64_t hash) {
        static const char digits[] = "0123456789abcdef";
        std::string text(16, '0');
        for (int i = 15; i >= 0; i--) {
            text[static_cast<size_t>(i)] = digits[hash & 0xF];
            hash >>= 4;
        }
        return text;
    }

    bool refuse(OpError& error, const std::string& field, const std::string& message) {
        error.field = field;
        error.message = message;
        return false;
    }

    // T1's op parser (edit_terrain, Lua's applyOps) leaves a registered op's seed among its keys
    uint32_t scatterSeed(const Op& op) {
        const OpParam* seed = op.params.find("seed");
        return seed ? static_cast<uint32_t>(seed->number) : op.seed;
    }

    bool closedKeys(const OpParam& params, std::initializer_list<const char*> allowed, const char* where, OpError& error) {
        for (const auto& [key, value] : params.fields) {
            (void)value;
            bool known = false;
            for (const char* name : allowed) known = known || key == name;
            if (!known) {
                std::string list;
                for (const char* name : allowed) {
                    if (!list.empty()) list += ", ";
                    list += name;
                }
                return refuse(error, std::string(where) + key, "unknown key; valid: " + list);
            }
        }
        return true;
    }

    bool readRange(const OpParam* param, std::optional<FilterRange>& out, const std::string& field, OpError& error) {
        if (!param) return true;
        if (param->kind != OpParam::Kind::List || param->items.size() != 2 ||
            param->items[0].kind != OpParam::Kind::Number || param->items[1].kind != OpParam::Kind::Number) {
            return refuse(error, field, "expects [min, max]");
        }
        FilterRange range;
        range.min = static_cast<float>(param->items[0].number);
        range.max = static_cast<float>(param->items[1].number);
        if (!(range.min <= range.max)) return refuse(error, field, "min is past max");
        out = range;
        return true;
    }

    bool readFloatPair(const OpParam* param, float& low, float& high, const std::string& field, OpError& error) {
        if (!param) return true;
        std::optional<FilterRange> range;
        if (!readRange(param, range, field, error)) return false;
        low = range->min;
        high = range->max;
        return true;
    }

    int findPrototype(const FoliageOpHost& host, const OpParam& value) {
        if (value.kind == OpParam::Kind::Number) {
            const int index = static_cast<int>(value.number);
            if (index >= 0 && index < static_cast<int>(host.prototypes.size())) return index;
            return -1;
        }
        if (value.kind == OpParam::Kind::String) {
            for (size_t i = 0; i < host.prototypes.size(); i++) {
                if (host.prototypes[i].name == value.text) return static_cast<int>(i);
            }
        }
        return -1;
    }

    std::string prototypeNames(const FoliageOpHost& host) {
        if (host.prototypes.empty()) return "the Terrain has no tree prototypes; add one with set_properties treePrototypes";
        std::string names = "valid:";
        for (const FoliageOpHost::Prototype& prototype : host.prototypes) names += " " + prototype.name;
        return names;
    }

    // Masks evaluated for a rule set stay alive with the parse
    struct ParsedRules {
        std::vector<float> noiseValues;
        int noiseWidth = 0, noiseHeight = 0;
        std::vector<float> densityValues;
    };

    bool parseRules(const FoliageOpHost& host, const TerrainMapStore& store, const OpParam* rules, FoliageRuleSet& out, ParsedRules& parsed, OpError& error) {
        if (!rules) return true;
        if (rules->kind != OpParam::Kind::Object) return refuse(error, "rules", "expects an object");
        if (!closedKeys(*rules, {"slope", "height", "layer", "noise_mask", "density_map", "exclude", "min_spacing"}, "rules.", error)) return false;

        if (!readRange(rules->find("slope"), out.slope, "rules.slope", error)) return false;
        if (!readRange(rules->find("height"), out.height, "rules.height", error)) return false;

        if (const OpParam* layer = rules->find("layer")) {
            if (layer->kind != OpParam::Kind::List) return refuse(error, "rules.layer", "expects [{index, min, max}]");
            for (const OpParam& item : layer->items) {
                const OpParam* index = item.find("index");
                if (!index || index->kind != OpParam::Kind::Number) return refuse(error, "rules.layer", "each entry needs index");
                FoliageRuleLayer entry;
                entry.index = static_cast<int>(index->number);
                if (entry.index < 0) return refuse(error, "rules.layer", "index must be 0 or more");
                if (const OpParam* min = item.find("min")) entry.min = static_cast<float>(min->number);
                if (const OpParam* max = item.find("max")) entry.max = static_cast<float>(max->number);
                out.layers.push_back(entry);
            }
        }

        if (const OpParam* noise = rules->find("noise_mask")) {
            const OpParam* graph = noise->find("graph");
            if (!graph || graph->kind != OpParam::Kind::String) return refuse(error, "rules.noise_mask.graph", "names a Noise Graph asset");
            if (!host.noiseMask) {
                return refuse(error, "rules.noise_mask", "Noise Graphs are not available in this build: T2's runtime has not landed. Use density_map as the mask");
            }
            const OpParam* output = noise->find("output");
            std::string message;
            if (!host.noiseMask(graph->text, output ? output->text : std::string(), parsed.noiseValues, parsed.noiseWidth, parsed.noiseHeight, message)) {
                return refuse(error, "rules.noise_mask", message);
            }
            FoliageRuleMask mask;
            mask.values = parsed.noiseValues;
            mask.width = parsed.noiseWidth;
            mask.height = parsed.noiseHeight;
            if (const OpParam* threshold = noise->find("threshold")) mask.threshold = static_cast<float>(threshold->number);
            out.noiseMask = mask;
        }

        if (const OpParam* densityMap = rules->find("density_map")) {
            const OpParam* layer = densityMap->find("layer");
            if (!layer || layer->kind != OpParam::Kind::Number) return refuse(error, "rules.density_map.layer", "names a detail layer's index");
            const MapId map{MapKind::Density, static_cast<int>(layer->number)};
            const MapFormat format = store.format(map);
            if (!format.exists) return refuse(error, "rules.density_map.layer", "detail layer " + std::to_string(map.index) + " has no density map");
            parsed.densityValues.resize(static_cast<size_t>(format.bounds.area()));
            if (!store.read(map, format.bounds, parsed.densityValues)) return refuse(error, "rules.density_map.layer", "could not read its density map");
            FoliageRuleMask mask;
            mask.values = parsed.densityValues;
            mask.width = format.bounds.w;
            mask.height = format.bounds.h;
            out.densityMask = mask;
        }

        if (const OpParam* exclude = rules->find("exclude")) {
            if (!closedKeys(*exclude, {"holes", "splines", "spline_margin"}, "rules.exclude.", error)) return false;
            if (const OpParam* holes = exclude->find("holes")) out.excludeHoles = holes->boolean;
            float margin = 0.0f;
            if (const OpParam* marginParam = exclude->find("spline_margin")) margin = static_cast<float>(marginParam->number);
            if (const OpParam* splines = exclude->find("splines")) {
                for (const OpParam& spline : splines->items) {
                    const auto it = host.splines.find(spline.text);
                    if (it == host.splines.end()) {
                        return refuse(error, "rules.exclude.splines", "no spline named \"" + spline.text + "\" in the scene");
                    }
                    for (const std::vector<Vector2>& line : it->second) {
                        FoliageSplineZone zone;
                        zone.points = line;
                        zone.margin = margin;
                        out.excludeSplines.push_back(std::move(zone));
                    }
                }
            }
        }

        if (const OpParam* spacing = rules->find("min_spacing")) {
            out.minSpacing = static_cast<float>(spacing->number);
            if (out.minSpacing < 0.0f) return refuse(error, "rules.min_spacing", "must be 0 or more");
        }
        return true;
    }

    // One scatter_by_rules, parsed: the same for validation and for the run
    struct ParsedScatter {
        FoliageTreeScatter trees;
        FoliageDensityScatter density;
        bool detailTarget = false;
        FoliageScatterMode mode = FoliageScatterMode::Replace;
        ParsedRules masks;
    };

    bool parseScatter(const FoliageOpHost& host, const TerrainMapStore& store, const Op& op, ParsedScatter& parsed, OpError& error) {
        const OpParam& params = op.params;
        if (!closedKeys(params, {"target", "density", "count", "mode", "rules", "jitter", "seed"}, "", error)) return false;
        if (const OpParam* seed = params.find("seed")) {
            if (seed->kind != OpParam::Kind::Number || !(seed->number >= 0.0) || seed->number != std::floor(seed->number) || seed->number > 4294967295.0) {
                return refuse(error, "seed", "must be a whole number from 0");
            }
        }

        const OpParam* target = params.find("target");
        if (!target || target->kind != OpParam::Kind::Object) {
            return refuse(error, "target", "expects {\"trees\": [{prototype, weight}]} or {\"detail_layer\": index}");
        }

        if (const OpParam* mode = params.find("mode")) {
            if (mode->text == "replace") parsed.mode = FoliageScatterMode::Replace;
            else if (mode->text == "add") parsed.mode = FoliageScatterMode::Add;
            else if (mode->text == "remove") parsed.mode = FoliageScatterMode::Remove;
            else return refuse(error, "mode", "valid: replace, add, remove");
        }

        FoliageRuleSet rules;
        if (!parseRules(host, store, params.find("rules"), rules, parsed.masks, error)) return false;

        float density = 0.0f;
        if (const OpParam* value = params.find("density")) {
            if (value->kind != OpParam::Kind::Number || !(value->number >= 0.0)) return refuse(error, "density", "must be 0 or more");
            density = static_cast<float>(value->number);
        }

        if (const OpParam* layer = target->find("detail_layer")) {
            parsed.detailTarget = true;
            if (layer->kind != OpParam::Kind::Number) return refuse(error, "target.detail_layer", "expects a layer index");
            const int index = static_cast<int>(layer->number);
            if (index < 0 || index >= static_cast<int>(host.layers.size())) {
                return refuse(error, "target.detail_layer", "the Terrain has " + std::to_string(host.layers.size()) + " detail layer(s); scatter_by_rules fills a layer, it does not create one");
            }
            if (!host.layers[index].hasMap && !store.format(MapId{MapKind::Density, index}).exists) {
                return refuse(error, "target.detail_layer", "detail layer " + std::to_string(index) + " has no density map yet");
            }
            parsed.density.region = op.shape;
            parsed.density.layerIndex = index;
            parsed.density.density = density;
            parsed.density.layerDensity = host.layers[index].density;
            parsed.density.mode = parsed.mode;
            parsed.density.rules = std::move(rules);
            return true;
        }

        const OpParam* trees = target->find("trees");
        if (!trees || trees->kind != OpParam::Kind::List || trees->items.empty()) {
            return refuse(error, "target", "expects {\"trees\": [{prototype, weight}]} or {\"detail_layer\": index}");
        }
        if (!host.hasTrees()) return refuse(error, "target.trees", "the Terrain's tree data is not loaded");
        for (size_t i = 0; i < trees->items.size(); i++) {
            const OpParam& item = trees->items[i];
            const OpParam* name = item.find("prototype");
            const int index = name ? findPrototype(host, *name) : -1;
            if (index < 0) return refuse(error, "target.trees[" + std::to_string(i) + "].prototype", prototypeNames(host));
            FoliageTreeTarget entry;
            entry.prototypeId = host.prototypes[index].id;
            if (const OpParam* weight = item.find("weight")) {
                entry.weight = static_cast<float>(weight->number);
                if (!(entry.weight >= 0.0f)) return refuse(error, "target.trees[" + std::to_string(i) + "].weight", "must be 0 or more");
            }
            parsed.trees.targets.push_back(entry);
        }

        parsed.trees.region = op.shape;
        parsed.trees.seed = scatterSeed(op);
        parsed.trees.density = density;
        parsed.trees.rules = std::move(rules);

        if (const OpParam* count = params.find("count")) {
            // Mass Place's Number of Trees: the density that attempts this many over the region
            if (count->kind != OpParam::Kind::Number || !(count->number >= 0.0)) return refuse(error, "count", "must be 0 or more");
            const ShapeEvaluator evaluator(op.shape);
            LocalBounds bounds;
            const float half = store.format(MapId{MapKind::Height, 0}).exists ? std::abs(store.worldOfTexel(MapId{MapKind::Height, 0}, store.format(MapId{MapKind::Height, 0}).bounds.x, 0).x) : 0.0f;
            if (!evaluator.bounds(bounds)) {
                bounds.min = Vector2(-half, -half);
                bounds.max = Vector2(half, half);
            }
            const float area = std::max(bounds.max.x - bounds.min.x, 0.0f) * std::max(bounds.max.y - bounds.min.y, 0.0f);
            if (area > 0.0f) parsed.trees.density = static_cast<float>(count->number) / area;
        }

        if (const OpParam* jitter = params.find("jitter")) {
            if (!closedKeys(*jitter, {"scale", "height_ratio", "yaw", "align_to_normal"}, "jitter.", error)) return false;
            if (!readFloatPair(jitter->find("scale"), parsed.trees.jitter.minScale, parsed.trees.jitter.maxScale, "jitter.scale", error)) return false;
            if (!readFloatPair(jitter->find("height_ratio"), parsed.trees.jitter.minHeightRatio, parsed.trees.jitter.maxHeightRatio, "jitter.height_ratio", error)) return false;
            if (const OpParam* yaw = jitter->find("yaw")) parsed.trees.jitter.yaw = static_cast<float>(yaw->number);
            if (const OpParam* align = jitter->find("align_to_normal")) parsed.trees.jitter.alignToNormal = static_cast<float>(align->number);
        }
        if (!(parsed.trees.jitter.minScale > 0.0f)) return refuse(error, "jitter.scale", "must be above 0");
        return true;
    }

    bool validateScatter(const TerrainMapStore& store, const Op& op, OpError& error) {
        if (!foliageCurrentHost) return refuse(error, "op", "foliage ops need a Terrain's tree data: run them through edit_terrain");
        ParsedScatter parsed;
        return parseScatter(*foliageCurrentHost, store, op, parsed, error);
    }

    bool hostStop(void* user) {
        const FoliageOpHost* host = static_cast<const FoliageOpHost*>(user);
        return host->shouldStop && host->shouldStop();
    }

    void reportRejections(const FoliageRejections& rejected, OpEntry& entry) {
        OpParam counts = objectParam();
        counts.fields.emplace_back("slope", numberParam(static_cast<double>(rejected.slope)));
        counts.fields.emplace_back("height", numberParam(static_cast<double>(rejected.height)));
        counts.fields.emplace_back("layer", numberParam(static_cast<double>(rejected.layer)));
        counts.fields.emplace_back("noise", numberParam(static_cast<double>(rejected.noise)));
        counts.fields.emplace_back("hole", numberParam(static_cast<double>(rejected.hole)));
        counts.fields.emplace_back("spline", numberParam(static_cast<double>(rejected.spline)));
        counts.fields.emplace_back("spacing", numberParam(static_cast<double>(rejected.spacing)));
        entry.fields.emplace_back("rejected", std::move(counts));
    }

    // The bounding box of what changed, in local units, and in the store's height texels when it can
    void reportTreeRects(std::span<const TreeRecord> removed, std::span<const TreeRecord> added, float size, OpEntry& entry) {
        float minX = 1e30f, minZ = 1e30f, maxX = -1e30f, maxZ = -1e30f;
        const auto grow = [&](const TreeRecord& record) {
            const float x = foliageTreeLocalX(record, size);
            const float z = foliageTreeLocalZ(record, size);
            minX = std::min(minX, x);
            minZ = std::min(minZ, z);
            maxX = std::max(maxX, x);
            maxZ = std::max(maxZ, z);
        };
        for (const TreeRecord& record : removed) grow(record);
        for (const TreeRecord& record : added) grow(record);
        OpParam rects = listParam();
        if (maxX >= minX) {
            OpParam rect = objectParam();
            OpParam min = listParam();
            min.items.push_back(numberParam(minX));
            min.items.push_back(numberParam(minZ));
            OpParam max = listParam();
            max.items.push_back(numberParam(maxX));
            max.items.push_back(numberParam(maxZ));
            rect.fields.emplace_back("min", std::move(min));
            rect.fields.emplace_back("max", std::move(max));
            rects.items.push_back(std::move(rect));
        }
        entry.fields.emplace_back("changed_rects", std::move(rects));
    }

    float storeTerrainSize(const TerrainMapStore& store) {
        const MapId map{MapKind::Height, 0};
        const MapFormat format = store.format(map);
        if (!format.exists || format.bounds.w < 2) return 0.0f;
        const Vector2 first = store.worldOfTexel(map, format.bounds.x, format.bounds.y);
        const Vector2 second = store.worldOfTexel(map, format.bounds.x + 1, format.bounds.y);
        return (second.x - first.x) * static_cast<float>(format.bounds.w);
    }

    void applyScatter(OpContext& context, const Op& op, OpEntry& entry) {
        FoliageOpHost* host = foliageCurrentHost;
        TerrainMapStore& store = context.store();
        ParsedScatter parsed;
        OpError error;
        if (!host || !parseScatter(*host, store, op, parsed, error)) {
            context.fail(error.message);
            return;
        }

        if (parsed.detailTarget) {
            const FoliageDensityScatterResult result = scatterDensityByRules(store, parsed.density, hostStop, host);
            if (result.mapMissing) {
                context.fail("detail layer " + std::to_string(parsed.density.layerIndex) + " has no density map");
                return;
            }
            if (!result.values.empty() && !result.rect.empty()) {
                const WriteResult wrote = store.write(MapId{MapKind::Density, parsed.density.layerIndex}, result.rect, result.values);
                if (!wrote.ok) {
                    context.fail(wrote.error);
                    return;
                }
            }
            entry.changedTexels = result.texelsSet;
            reportRejections(result.rejected, entry);
            entry.fields.emplace_back("instances_added", numberParam(0));
            entry.fields.emplace_back("instances_removed", numberParam(0));
            return;
        }

        const float size = storeTerrainSize(store);
        const TreeStore& current = host->current();
        const ShapeEvaluator shape(op.shape);

        // what spacing keeps clear of, and what a replace or remove takes away
        std::vector<Vector2> kept;
        std::vector<uint32_t> removed;
        std::vector<TreeRecord> removedRecords;
        std::set<uint16_t> targeted;
        for (const FoliageTreeTarget& target : parsed.trees.targets) targeted.insert(target.prototypeId);

        FoliageRuleSurface ruleSurface(store);
        ruleSurface.prepareLayers(parsed.trees.rules);
        for (const TreeRecord& record : current.records()) {
            const float x = foliageTreeLocalX(record, size);
            const float z = foliageTreeLocalZ(record, size);
            const bool inRegion = shape.sample(x, z).inside;
            const bool sameKind = targeted.count(record.prototypeId) != 0;
            if (inRegion && sameKind && parsed.mode == FoliageScatterMode::Replace) {
                removed.push_back(record.id);
                removedRecords.push_back(record);
                continue;
            }
            if (inRegion && sameKind && parsed.mode == FoliageScatterMode::Remove) {
                const float share = 0.0f;
                if (ruleSurface.passes(parsed.trees.rules, x, z, nullptr, nullptr, nullptr, &share)) {
                    removed.push_back(record.id);
                    removedRecords.push_back(record);
                    continue;
                }
            }
            kept.emplace_back(x, z);
        }

        FoliageTreeScatterResult scattered;
        if (parsed.mode == FoliageScatterMode::Remove) {
            scattered.hash = foliageTreesHash(removedRecords);
        } else {
            scattered = scatterTreesByRules(store, parsed.trees, kept, hostStop, host);
            if (scattered.stopped) {
                context.fail("cancelled");
                return;
            }
        }

        // a dry run too: a later op in the list must see what a real run would leave
        host->working().apply(removed, scattered.trees, {}, nullptr);

        reportRejections(scattered.rejected, entry);
        reportTreeRects(removedRecords, scattered.trees, size, entry);
        entry.fields.emplace_back("instances_added", numberParam(static_cast<double>(scattered.trees.size())));
        entry.fields.emplace_back("instances_removed", numberParam(static_cast<double>(removed.size())));
        entry.fields.emplace_back("hash", textParam(hexHash(scattered.hash)));
        entry.seed = scatterSeed(op);
    }

    bool validateClear(const TerrainMapStore& store, const Op& op, OpError& error) {
        if (!foliageCurrentHost) return refuse(error, "op", "foliage ops need a Terrain's tree data: run them through edit_terrain");
        if (!closedKeys(op.params, {"target", "spline", "margin"}, "", error)) return false;
        const OpParam* target = op.params.find("target");
        if (!target) return refuse(error, "target", "expects \"trees\", \"details\", \"all\", {\"trees\": [...]} or {\"detail_layer\": index}");
        if (target->kind == OpParam::Kind::String && target->text != "trees" && target->text != "details" && target->text != "all") {
            return refuse(error, "target", "valid: trees, details, all");
        }
        if (const OpParam* layer = target->find("detail_layer")) {
            if (!store.format(MapId{MapKind::Density, static_cast<int>(layer->number)}).exists) {
                return refuse(error, "target.detail_layer", "that layer has no density map");
            }
        }
        if (const OpParam* trees = target->find("trees")) {
            for (size_t i = 0; i < trees->items.size(); i++) {
                const OpParam* name = trees->items[i].find("prototype");
                if (findPrototype(*foliageCurrentHost, name ? *name : trees->items[i]) < 0) {
                    return refuse(error, "target.trees[" + std::to_string(i) + "]", prototypeNames(*foliageCurrentHost));
                }
            }
        }
        if (const OpParam* spline = op.params.find("spline")) {
            if (foliageCurrentHost->splines.find(spline->text) == foliageCurrentHost->splines.end()) {
                return refuse(error, "spline", "no spline named \"" + spline->text + "\" in the scene");
            }
        }
        return true;
    }

    void applyClear(OpContext& context, const Op& op, OpEntry& entry) {
        FoliageOpHost* host = foliageCurrentHost;
        TerrainMapStore& store = context.store();
        const OpParam* target = op.params.find("target");
        const float size = storeTerrainSize(store);
        const ShapeEvaluator shape(op.shape);

        std::vector<std::vector<Vector2>> lines;
        float margin = 0.0f;
        if (const OpParam* spline = op.params.find("spline")) {
            lines = host->splines[spline->text];
            if (const OpParam* marginParam = op.params.find("margin")) margin = static_cast<float>(marginParam->number);
        }
        const auto inside = [&](float x, float z) {
            if (!shape.sample(x, z).inside) return false;
            if (lines.empty()) return true;
            for (const std::vector<Vector2>& line : lines) {
                if (foliageDistanceToPolyline(line, x, z) <= margin) return true;
            }
            return false;
        };

        const bool all = target->kind == OpParam::Kind::String && target->text == "all";
        const bool clearDetails = all || (target->kind == OpParam::Kind::String && target->text == "details") || target->find("detail_layer");
        const bool clearTrees = all || (target->kind == OpParam::Kind::String && target->text == "trees") || target->find("trees");

        int64_t texels = 0;
        if (clearDetails) {
            for (int index = 0; index < static_cast<int>(host->layers.size()); index++) {
                if (const OpParam* layer = target->find("detail_layer")) {
                    if (static_cast<int>(layer->number) != index) continue;
                }
                const MapId map{MapKind::Density, index};
                const MapFormat format = store.format(map);
                if (!format.exists) continue;
                const TexelRect rect = shapeTexels(store, map, format, shape);
                if (rect.empty()) continue;
                std::vector<float> values(static_cast<size_t>(rect.area()));
                if (!store.read(map, rect, values)) continue;
                const MapLattice lattice = latticeOf(store, map, format);
                for (int y = 0; y < rect.h; y++) {
                    const float z = lattice.z(rect.y + y);
                    for (int x = 0; x < rect.w; x++) {
                        float& value = values[static_cast<size_t>(y) * rect.w + x];
                        if (value > 0.0f && inside(lattice.x(rect.x + x), z)) {
                            value = 0.0f;
                            texels++;
                        }
                    }
                }
                const WriteResult wrote = store.write(map, rect, values);
                if (!wrote.ok) {
                    context.fail(wrote.error);
                    return;
                }
            }
        }
        entry.changedTexels = texels;

        int64_t treesRemoved = 0;
        std::vector<TreeRecord> removedRecords;
        if (clearTrees && host->hasTrees()) {
            std::set<int> kinds;
            if (const OpParam* trees = target->find("trees")) {
                for (const OpParam& item : trees->items) {
                    const OpParam* name = item.find("prototype");
                    kinds.insert(host->prototypes[findPrototype(*host, name ? *name : item)].id);
                }
            }
            std::vector<uint32_t> removed;
            for (const TreeRecord& record : host->current().records()) {
                if (!kinds.empty() && kinds.count(record.prototypeId) == 0) continue;
                if (inside(foliageTreeLocalX(record, size), foliageTreeLocalZ(record, size))) {
                    removed.push_back(record.id);
                    removedRecords.push_back(record);
                }
            }
            if (!removed.empty()) host->working().apply(removed, {}, {}, nullptr);
            treesRemoved = static_cast<int64_t>(removed.size());
        }
        reportTreeRects(removedRecords, {}, size, entry);
        entry.fields.emplace_back("instances_added", numberParam(0));
        entry.fields.emplace_back("instances_removed", numberParam(static_cast<double>(treesRemoved)));
    }

    bool validatePlace(const TerrainMapStore& store, const Op& op, OpError& error) {
        if (!foliageCurrentHost) return refuse(error, "op", "foliage ops need a Terrain's tree data: run them through edit_terrain");
        if (!closedKeys(op.params, {"prototype", "points", "align_to_normal", "height_ratio", "color", "y_offset"}, "", error)) return false;
        const FoliageOpHost& host = *foliageCurrentHost;
        if (!host.hasTrees()) return refuse(error, "prototype", "the Terrain's tree data is not loaded");
        const OpParam* prototype = op.params.find("prototype");
        if (!prototype || findPrototype(host, *prototype) < 0) return refuse(error, "prototype", prototypeNames(host));
        const OpParam* points = op.params.find("points");
        if (!points || points->kind != OpParam::Kind::List || points->items.empty()) return refuse(error, "points", "expects [[x, z, yaw?, scale?], ...]");
        const float size = storeTerrainSize(store);
        for (size_t i = 0; i < points->items.size(); i++) {
            const OpParam& point = points->items[i];
            if (point.kind != OpParam::Kind::List || point.items.size() < 2 || point.items.size() > 4) {
                return refuse(error, "points[" + std::to_string(i) + "]", "expects [x, z, yaw?, scale?]");
            }
            const double half = size * 0.5;
            if (std::abs(point.items[0].number) > half || std::abs(point.items[1].number) > half) {
                return refuse(error, "points[" + std::to_string(i) + "]", "outside the terrain: local x and z are within " + std::to_string(half));
            }
        }
        return true;
    }

    void applyPlace(OpContext& context, const Op& op, OpEntry& entry) {
        FoliageOpHost* host = foliageCurrentHost;
        TerrainMapStore& store = context.store();
        const FoliageRuleSurface surface(store);
        const float size = surface.size();
        const int prototypeIndex = findPrototype(*host, *op.params.find("prototype"));
        const FoliageOpHost::Prototype& prototype = host->prototypes[prototypeIndex];

        float align = 0.0f;
        if (const OpParam* value = op.params.find("align_to_normal")) align = static_cast<float>(value->number);
        float ratio = 1.0f;
        if (const OpParam* value = op.params.find("height_ratio")) ratio = static_cast<float>(value->number);
        float yOffset = 0.0f;
        if (const OpParam* value = op.params.find("y_offset")) yOffset = static_cast<float>(value->number);

        std::vector<TreeRecord> added;
        added.reserve(op.params.find("points")->items.size());
        for (const OpParam& point : op.params.find("points")->items) {
            const float x = static_cast<float>(point.items[0].number);
            const float z = static_cast<float>(point.items[1].number);
            const float yawDegrees = point.items.size() > 2 ? static_cast<float>(point.items[2].number) : 0.0f;
            const float scale = point.items.size() > 3 ? static_cast<float>(point.items[3].number) : 1.0f;

            const float height = surface.height(x, z);
            float up[3] = {0.0f, 1.0f, 0.0f};
            surface.normal(x, z, height, up);

            TreeRecord record{};
            foliageSetTreeSurface(record, size, surface.maxHeight(), x, z, height);
            record.yOffset = yOffset;
            record.scale = scale;
            float rotation[4];
            foliageOrientation(yawDegrees / 360.0f, 0.0f, up, rotation);
            foliageQuantizeRotation(rotation, record.rotation);
            record.prototypeId = prototype.id;
            record.heightRatio = static_cast<uint8_t>(std::clamp(std::floor(ratio * 128.0f + 0.5f), 1.0f, 255.0f));
            record.flags = TREE_SNAP_TO_TERRAIN | treeAlignFlags(align);
            if (surface.hole(x, z)) record.flags |= TREE_HIDDEN_BY_HOLE;
            std::memset(record.color, 255, sizeof(record.color));
            added.push_back(record);
        }

        TreeDelta delta;
        host->working().apply({}, added, {}, &delta);

        OpParam ids = listParam();
        for (const TreeRecord& record : delta.added) ids.items.push_back(numberParam(record.id));
        entry.fields.emplace_back("ids", std::move(ids));
        reportTreeRects({}, delta.added, size, entry);
        entry.fields.emplace_back("instances_added", numberParam(static_cast<double>(delta.added.size())));
        entry.fields.emplace_back("instances_removed", numberParam(0));
    }

    bool validateRemove(const TerrainMapStore&, const Op& op, OpError& error) {
        if (!foliageCurrentHost) return refuse(error, "op", "foliage ops need a Terrain's tree data: run them through edit_terrain");
        if (!closedKeys(op.params, {"ids", "prototype"}, "", error)) return false;
        const FoliageOpHost& host = *foliageCurrentHost;
        if (!host.hasTrees()) return refuse(error, "ids", "the Terrain's tree data is not loaded");
        if (const OpParam* prototype = op.params.find("prototype")) {
            if (findPrototype(host, *prototype) < 0) return refuse(error, "prototype", prototypeNames(host));
        }
        if (const OpParam* ids = op.params.find("ids")) {
            if (ids->kind != OpParam::Kind::List) return refuse(error, "ids", "expects a list of tree ids");
            for (size_t i = 0; i < ids->items.size(); i++) {
                if (!host.current().find(static_cast<uint32_t>(ids->items[i].number))) {
                    return refuse(error, "ids[" + std::to_string(i) + "]", "no tree has this id; sample_terrain lists counts, place_trees returns ids");
                }
            }
        }
        return true;
    }

    void applyRemove(OpContext& context, const Op& op, OpEntry& entry) {
        FoliageOpHost* host = foliageCurrentHost;
        TerrainMapStore& store = context.store();
        const float size = storeTerrainSize(store);
        const ShapeEvaluator shape(op.shape);

        std::set<uint32_t> ids;
        if (const OpParam* list = op.params.find("ids")) {
            for (const OpParam& id : list->items) ids.insert(static_cast<uint32_t>(id.number));
        }
        int prototype = -1;
        if (const OpParam* name = op.params.find("prototype")) prototype = host->prototypes[findPrototype(*host, *name)].id;

        std::vector<uint32_t> removed;
        std::vector<TreeRecord> records;
        for (const TreeRecord& record : host->current().records()) {
            if (!ids.empty() && ids.count(record.id) == 0) continue;
            if (prototype >= 0 && record.prototypeId != prototype) continue;
            if (!shape.sample(foliageTreeLocalX(record, size), foliageTreeLocalZ(record, size)).inside) continue;
            removed.push_back(record.id);
            records.push_back(record);
        }
        if (!removed.empty()) host->working().apply(removed, {}, {}, nullptr);
        reportTreeRects(records, {}, size, entry);
        entry.fields.emplace_back("instances_added", numberParam(0));
        entry.fields.emplace_back("instances_removed", numberParam(static_cast<double>(removed.size())));
    }

    bool validateConvert(const TerrainMapStore&, const Op& op, OpError& error) {
        if (!foliageCurrentHost) return refuse(error, "op", "foliage ops need a Terrain's tree data: run them through edit_terrain");
        if (!closedKeys(op.params, {"host"}, "", error)) return false;
        const OpParam* host = op.params.find("host");
        if (!host) return refuse(error, "host", "names the instanced child to convert");
        const std::string key = host->kind == OpParam::Kind::Number ? std::to_string(static_cast<int64_t>(host->number)) : host->text;
        if (foliageCurrentHost->placedHosts.find(key) == foliageCurrentHost->placedHosts.end()) {
            std::string list;
            for (const auto& entry : foliageCurrentHost->placedHosts) list += " " + entry.first;
            return refuse(error, "host", list.empty() ? "the Terrain has no instanced host" : "valid:" + list);
        }
        return true;
    }

    void applyConvert(OpContext& context, const Op& op, OpEntry& entry) {
        FoliageOpHost* host = foliageCurrentHost;
        TerrainMapStore& store = context.store();
        const FoliageRuleSurface surface(store);
        const float size = surface.size();
        const OpParam* param = op.params.find("host");
        const std::string key = param->kind == OpParam::Kind::Number ? std::to_string(static_cast<int64_t>(param->number)) : param->text;
        const FoliageOpHost::PlacedHost& placed = host->placedHosts[key];

        std::vector<TreeRecord> added;
        added.reserve(placed.instances.size());
        for (const FoliageOpHost::PlacedInstance& instance : placed.instances) {
            const float x = instance.position[0];
            const float z = instance.position[2];
            const float height = surface.height(x, z);
            TreeRecord record{};
            foliageSetTreeSurface(record, size, surface.maxHeight(), x, z, height);
            record.yOffset = instance.position[1] - height;
            record.scale = std::max(instance.scale[0], instance.scale[2]);
            foliageQuantizeRotation(instance.rotation, record.rotation);
            record.prototypeId = placed.prototypeId;
            const float ratio = record.scale > 0.0f ? instance.scale[1] / record.scale : 1.0f;
            record.heightRatio = static_cast<uint8_t>(std::clamp(std::floor(ratio * 128.0f + 0.5f), 1.0f, 255.0f));
            record.flags = TREE_SNAP_TO_TERRAIN;
            std::memcpy(record.color, instance.color, sizeof(record.color));
            added.push_back(record);
        }
        TreeDelta delta;
        host->working().apply({}, added, {}, &delta);
        host->convertedHosts.push_back(key);
        reportTreeRects({}, delta.added, size, entry);
        entry.fields.emplace_back("instances_added", numberParam(static_cast<double>(delta.added.size())));
        entry.fields.emplace_back("instances_removed", numberParam(0));
    }

}

FoliageOpHost::FoliageOpHost(std::shared_ptr<const TreeStore> base): base(std::move(base)) {}

const TreeStore& FoliageOpHost::current() const {
    static const TreeStore empty;
    if (workingCopy) return *workingCopy;
    return base ? *base : empty;
}

TreeStore& FoliageOpHost::working() {
    if (!workingCopy) {
        workingCopy = base ? std::make_unique<TreeStore>(*base) : std::make_unique<TreeStore>();
    }
    return *workingCopy;
}

TreeDelta FoliageOpHost::delta() const {
    TreeDelta result;
    const TreeStore& before = base ? *base : current();
    const TreeStore& after = current();
    result.nextIdBefore = before.nextId();
    result.nextIdAfter = after.nextId();
    const std::vector<TreeRecord>& a = before.records();
    const std::vector<TreeRecord>& b = after.records();
    size_t i = 0, j = 0;
    while (i < a.size() || j < b.size()) {
        if (j >= b.size() || (i < a.size() && a[i].id < b[j].id)) {
            result.removed.push_back(a[i++]);
        } else if (i >= a.size() || b[j].id < a[i].id) {
            result.added.push_back(b[j++]);
        } else {
            if (std::memcmp(&a[i], &b[j], sizeof(TreeRecord)) != 0) {
                result.removed.push_back(a[i]);
                result.added.push_back(b[j]);
            }
            i++;
            j++;
        }
    }
    return result;
}

FoliageOpScope::FoliageOpScope(FoliageOpHost& host): previous(foliageCurrentHost) {
    foliageCurrentHost = &host;
}

FoliageOpScope::~FoliageOpScope() {
    foliageCurrentHost = previous;
}

void jasperengine::terrain::registerFoliageOps() {
    if (registeredOpExists("scatter_by_rules")) return;
    registerOp("scatter_by_rules", validateScatter, applyScatter);
    registerOp("clear_foliage", validateClear, applyClear);
    registerOp("place_trees", validatePlace, applyPlace);
    registerOp("remove_trees", validateRemove, applyRemove);
    registerOp("convert_placed_to_trees", validateConvert, applyConvert);
}

void jasperengine::terrain::unregisterFoliageOps() {
    unregisterOp("scatter_by_rules");
    unregisterOp("clear_foliage");
    unregisterOp("place_trees");
    unregisterOp("remove_trees");
    unregisterOp("convert_placed_to_trees");
}

bool jasperengine::terrain::foliageOpParams(const FoliageOpHost& host, const TerrainMapStore& store, const Op& op, FoliageTreeScatter& trees, FoliageDensityScatter& density,
                                            bool& detailTarget, FoliageScatterMode& mode, OpError& error) {
    ParsedScatter parsed;
    if (!parseScatter(host, store, op, parsed, error)) return false;
    trees = parsed.trees;
    density = parsed.density;
    detailTarget = parsed.detailTarget;
    mode = parsed.mode;
    return true;
}
