// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "TitleLiveCompose.h"

#include "titleart/TitleArtIO.h"
#include "titleart/effects/TfxCommon.h"
#include "titleart/render/TitleArtRenderer.h"

#include <algorithm>
#include <cstdlib>
#include <map>
#include <set>

using namespace jasperengine;
using namespace jasperengine::titleart;

namespace {

    bool hasLiveParam(const LiveVariant& live, const std::string& param) {
        if (live.kind == LiveKind::None) return false;
        for (const char* p : live.params)
            if (param == p) return true;
        return false;
    }

    // the first segment of a param path ("center.x" -> "center")
    std::string paramOf(const std::string& rest) {
        return rest.substr(0, rest.find('.'));
    }

    double msOf(const FxCost& cost, FxTier tier) {
        return cost.fre * tfx::msPerFre(tier) + cost.instances * tfx::msPerInstance(tier);
    }

    // a definition's values as the renderer reads them (defaults filled); a ParamValues reads
    // this Json, so it stays alive for the estimate
    Json normalizedValues(const std::vector<ParamDef>& params, const Json& stored) {
        Json values = stored.is_object() ? stored : Json::object();
        NormalizeContext normalize;
        normalizeParams(params, values, "params", normalize);
        return values;
    }

    double estimateMs(FxCost (*estimate)(const ParamValues&, const FxSize&, FxTier), const Json& values, const GlobalLightBinding* light,
                      const TitleArtDoc& doc, const FxSize& size, FxTier tier) {
        const Json& global = doc.root.value("global_light", Json::object());
        const ParamValues params(values, light, global.value("angle", 120.0), global.value("altitude", 30.0));
        return estimate ? msOf(estimate(params, size, tier), tier) : tfx::msPerFre(tier);
    }

    // §6.4's tier sizes: the desktop tier at the canvas size, the mobile tier on 1024 x 512
    FxSize tierSize(const TitleArtDoc& doc, FxTier tier) {
        const int cw = std::max(1, doc.canvasWidth()), ch = std::max(1, doc.canvasHeight());
        if (tier == FxTier::Desktop) return FxSize{cw, ch, 1.0f};
        const float scale = std::min(1024.0f / (float)cw, 512.0f / (float)ch);
        return FxSize{std::max(1, (int)((float)cw * scale)), std::max(1, (int)((float)ch * scale)), scale};
    }

    // the earliest changed effect of each layer stack, and the changed generator layers
    struct Changes {
        std::map<std::string, int> stacks;
        std::set<std::string> generators;
    };
    Changes changesOf(const std::vector<TitleLiveTarget>& changed) {
        Changes out;
        for (const TitleLiveTarget& t : changed) {
            if (t.effect < 0) {
                out.generators.insert(t.layerId);
                continue;
            }
            auto it = out.stacks.find(t.layerId);
            if (it == out.stacks.end() || t.effect < it->second) out.stacks[t.layerId] = t.effect;
        }
        return out;
    }

    const Json& effectsOf(const Json& layer) {
        static const Json none = Json::array();
        auto it = layer.find("effects");
        return it != layer.end() && it->is_array() ? *it : none;
    }

}

bool titleart::titleLiveTargetOf(const TitleArtDoc& doc, const std::string& layerId, const std::string& path, TitleLiveTarget& out) {
    const Json* layer = layerId.empty() ? nullptr : findLayer(doc.root, layerId);
    if (!layer) return false;
    const ExpandedFieldPath expanded = expandFieldShorthand(layer, path);
    if (!expanded.ok) return false;
    const std::string& p = expanded.path;
    out = TitleLiveTarget();
    out.layerId = layerId;
    static const std::string generatorParams = "generator.params.";
    if (p.rfind(generatorParams, 0) == 0) {
        if (layerTypeOf(*layer) != LayerType::Generator) return false;
        const GeneratorDef* def = findGenerator(layer->value("generator", Json::object()).value("type", std::string()));
        out.param = paramOf(p.substr(generatorParams.size()));
        if (!def || !hasLiveParam(def->live, out.param)) return false;
        out.generatorDef = def;
        return true;
    }
    // a layer's own stack: effects[<n>].params.<param>
    static const std::string effects = "effects[";
    if (p.rfind(effects, 0) != 0) return false;
    const size_t close = p.find("].params.", effects.size());
    if (close == std::string::npos) return false;
    const std::string digits = p.substr(effects.size(), close - effects.size());
    if (digits.empty() || digits.find_first_not_of("0123456789") != std::string::npos) return false;
    const int index = std::atoi(digits.c_str());
    const Json& list = effectsOf(*layer);
    if (index < 0 || index >= (int)list.size()) return false;
    const EffectDef* def = findEffect(list[(size_t)index].value("type", std::string()));
    out.effect = index;
    out.param = paramOf(p.substr(close + 9));
    if (!def || !hasLiveParam(def->live, out.param)) return false;
    out.effectDef = def;
    return true;
}

std::vector<std::string> titleart::titleLiveInputsOf(const TitleArtDoc& doc, const std::vector<TitleLiveTarget>& changed) {
    std::vector<std::string> out;
    const Changes changes = changesOf(changed);
    for (const auto& [layerId, effect] : changes.stacks) {
        out.push_back(titleLiveContentKey(layerId));
        // the first enabled effect reads the content itself (and its alpha-keyed results with it)
        const Json* layer = findLayer(doc.root, layerId);
        bool enabledBefore = false;
        if (layer) {
            const Json& list = effectsOf(*layer);
            for (int i = 0; i < effect && i < (int)list.size(); i++) enabledBefore = enabledBefore || list[(size_t)i].value("enabled", true);
        }
        if (enabledBefore) out.push_back(titleLiveStackKey(layerId, effect));
    }
    for (const std::string& id : changes.generators) {
        const Json* layer = findLayer(doc.root, id);
        if (!layer || layer->value("follow", std::string()).empty()) continue;
        const GeneratorDef* def = findGenerator(layer->value("generator", Json::object()).value("type", std::string()));
        if (!def || def->follow == FollowMode::None) continue;
        out.push_back(titleLiveFollowKey(id));
        if (def->inputs.content) out.push_back(titleLiveFollowContentKey(id));
    }
    return out;
}

std::vector<std::string> titleart::titleLiveKeeps(const TitleArtDoc& doc) {
    std::vector<TitleLiveTarget> live;
    if (!doc.root.is_object()) return {};
    for (const Json& item : doc.root.value("exposed", Json::array())) {
        if (!item.is_object()) continue;
        std::vector<TitleLiveTarget> targets;
        bool all = true;
        for (const Json& target : item.value("targets", Json::array())) {
            TitleLiveTarget t;
            if (!target.is_object() || !titleLiveTargetOf(doc, target.value("layer", std::string()), target.value("field", std::string()), t)) {
                all = false;
                break;
            }
            targets.push_back(std::move(t));
        }
        if (all) live.insert(live.end(), targets.begin(), targets.end());
    }
    return titleLiveInputsOf(doc, live);
}

void titleart::touchTitleLive(TitleLiveTouched& touched, const std::vector<TitleLiveTarget>& changed) {
    for (const TitleLiveTarget& t : changed) {
        auto it = touched.find(t.layerId);
        if (it == touched.end() || t.effect < it->second) touched[t.layerId] = t.effect;
    }
}

bool titleart::titleLiveInputValid(const TitleArtDoc& doc, const std::string& name, const TitleLiveTouched& touched) {
    if (touched.empty()) return true;
    if (!doc.root.is_object()) return false;
    std::map<std::string, std::string> parents;
    forEachLayer(doc.root, [&](const Json& layer, const Json* parent, int) {
        parents[layer.value("id", std::string())] = parent ? parent->value("id", std::string()) : std::string();
        return true;
    });
    // a touched layer inside `id` (its descendants, not itself)
    auto touchedInside = [&](const std::string& id) {
        for (const auto& entry : touched) {
            auto it = parents.find(entry.first);
            for (std::string p = it != parents.end() ? it->second : std::string(); !p.empty();) {
                if (p == id) return true;
                auto up = parents.find(p);
                p = up != parents.end() ? up->second : std::string();
            }
        }
        return false;
    };
    const size_t colon = name.find(':');
    if (colon == std::string::npos) return false;
    const std::string kind = name.substr(0, colon);
    std::string id = name.substr(colon + 1);
    if (kind == "fx") {
        const size_t at = id.find('@');
        if (at == std::string::npos) return false;
        const int effect = std::atoi(id.c_str() + at + 1);
        id.resize(at);
        auto it = touched.find(id);
        return !touchedInside(id) && (it == touched.end() || it->second >= effect);
    }
    if (kind == "content") return !touchedInside(id);
    if (kind == "follow" || kind == "follow_content") {
        const Json* generator = findLayer(doc.root, id);
        const std::string followed = generator ? generator->value("follow", std::string()) : std::string();
        if (followed.empty()) return false;
        // the styled cut shows the followed layer's effects, the content cut only its insides
        return !touchedInside(followed) && (kind == "follow_content" || !touched.count(followed));
    }
    return false;
}

std::string titleart::titleLiveSuffixReason(const TitleArtDoc& doc, const std::vector<TitleLiveTarget>& changed) {
    for (const auto& [layerId, start] : changesOf(changed).stacks) {
        const Json* layer = findLayer(doc.root, layerId);
        if (!layer) continue;
        const Json& list = effectsOf(*layer);
        const EffectDef* first = start < (int)list.size() ? findEffect(list[(size_t)start].value("type", std::string())) : nullptr;
        for (size_t j = (size_t)start + 1; j < list.size(); j++) {
            if (!list[j].value("enabled", true)) continue;
            const EffectDef* def = findEffect(list[j].value("type", std::string()));
            if (def && def->live.kind != LiveKind::None) continue;
            return std::string(def ? def->label : "an unknown effect") + " after " + (first ? first->label : "the live effect") + " on layer " +
                   layer->value("name", layerId) + " has no live variant: the change re-renders (S83 D10)";
        }
    }
    return std::string();
}

double titleart::titleLiveCostMs(const TitleArtDoc& doc, const std::vector<TitleLiveTarget>& changed, FxTier tier) {
    const FxSize size = tierSize(doc, tier);
    double ms = 0.0;
    const Changes changes = changesOf(changed);
    for (const auto& [layerId, start] : changes.stacks) {
        const Json* layer = findLayer(doc.root, layerId);
        if (!layer) continue;
        const Json& list = effectsOf(*layer);
        for (size_t j = (size_t)std::max(0, start); j < list.size(); j++) {
            const Json& fx = list[j];
            if (!fx.value("enabled", true)) continue;
            const EffectDef* def = findEffect(fx.value("type", std::string()));
            if (!def) continue;
            // a scale_split composes its kept split in one full-resolution pass (FxCost's readout)
            if (def->live.kind == LiveKind::ScaleSplit) {
                ms += tfx::msPerFre(tier);
                continue;
            }
            const Json values = normalizedValues(def->params, fx.value("params", Json::object()));
            ms += estimateMs(def->estimate, values, &def->globalLight, doc, size, tier);
        }
    }
    for (const std::string& id : changes.generators) {
        const Json* layer = findLayer(doc.root, id);
        if (!layer) continue;
        const Json& gen = layer->value("generator", Json::object());
        const GeneratorDef* def = findGenerator(gen.value("type", std::string()));
        if (!def) continue;
        const Json values = normalizedValues(def->params, gen.value("params", Json::object()));
        ms += estimateMs(def->estimate, values, def->globalLight.angleKey ? &def->globalLight : nullptr, doc, size, tier);
    }
    return ms;
}

TitleLiveDecision titleart::decideTitleLive(const TitleArtDoc& doc, const std::vector<FieldOverride>& from, const std::vector<FieldOverride>& to,
                                            FxTier tier, double budgetMs) {
    TitleLiveDecision out;
    // the values each state writes, by field
    auto byField = [](const std::vector<FieldOverride>& list) {
        std::map<std::pair<std::string, std::string>, const Json*> map;
        for (const FieldOverride& o : list) map[{o.layerId, o.path}] = &o.value;
        return map;
    };
    const auto before = byField(from), after = byField(to);
    std::set<std::pair<std::string, std::string>> fields;
    for (const auto& [field, value] : before) {
        auto it = after.find(field);
        if (it == after.end() || *it->second != *value) fields.insert(field);
    }
    for (const auto& [field, value] : after)
        if (!before.count(field)) fields.insert(field);
    if (fields.empty()) return out;
    for (const auto& [layerId, path] : fields) {
        TitleLiveTarget t;
        // anything but a live param re-renders, with no reason to give
        if (!titleLiveTargetOf(doc, layerId, path, t)) return out;
        out.changed.push_back(std::move(t));
    }
    out.reason = titleLiveSuffixReason(doc, out.changed);
    if (!out.reason.empty()) return out;
    TitleArtDoc shown = doc;
    applyFieldOverrides(shown, to);
    out.costMs = titleLiveCostMs(shown, out.changed, tier);
    if (out.costMs > budgetMs) {
        char text[160];
        std::snprintf(text, sizeof(text), "the live compose is predicted at %.2f ms, over the title's live budget of %.2f ms: the change re-renders",
                      out.costMs, budgetMs);
        out.reason = text;
        return out;
    }
    out.inputs = titleLiveInputsOf(doc, out.changed);
    out.live = true;
    return out;
}

double titleart::titleLiveBudgetMs(DevicePlatformGroup group) {
    return group == DevicePlatformGroup::Desktop ? 1.0 : 0.5;
}

FxTier titleart::titleFxTierFor(DevicePlatformGroup group, std::optional<DeviceTier> tier) {
    if (group != DevicePlatformGroup::Desktop) return FxTier::Mobile;
    return tier && *tier == DeviceTier::Low ? FxTier::Mobile : FxTier::Desktop;
}
