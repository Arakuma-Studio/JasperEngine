// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "FxGeneratorChecks.h"

#include "../TitleArtChecks.h"
#include "titleart/TitleEffectRegistry.h"

#include <functional>

using namespace jasperengine;
using namespace jasperengine::editor;
using TJson = titleart::Json;

std::vector<TitleFollowProblem> editor::titleFollowProblems(const titleart::TitleArtDoc& doc) {
    std::vector<TitleFollowProblem> out;
    titleart::forEachLayer(doc.root, [&](const TJson& layer, const TJson*, int) {
        if (titleart::layerTypeOf(layer) != titleart::LayerType::Generator) return true;
        const titleart::GeneratorDef* def = titleart::findGenerator(layer.value("generator", TJson::object()).value("type", std::string()));
        if (!def || def->follow == titleart::FollowMode::None) return true;
        const std::string follow = layer.value("follow", std::string());
        const bool dangling = !follow.empty() && !titleart::findLayer(doc.root, follow);
        if (dangling || (follow.empty() && def->follow == titleart::FollowMode::Required)) out.push_back({layer.value("id", std::string()), follow});
        return true;
    });
    return out;
}

namespace {

    // the nearest layer a generator can follow: the closest sibling below it that is not a
    // generator (Pachislop's companion sits right above its layer), else any such layer
    std::string nearestFollowable(const TJson& root, const std::string& generatorId) {
        std::string below, any;
        std::function<void(const TJson&)> walk = [&](const TJson& list) {
            if (!list.is_array()) return;
            std::string lastCandidate;
            for (const TJson& layer : list) {
                const std::string id = layer.value("id", std::string());
                if (id == generatorId && !lastCandidate.empty() && below.empty()) below = lastCandidate;
                const titleart::LayerType type = titleart::layerTypeOf(layer);
                if (type != titleart::LayerType::Generator && type != titleart::LayerType::Adjustment && type != titleart::LayerType::Group) {
                    lastCandidate = id;
                    if (any.empty()) any = id;
                }
                if (layer.contains("children")) walk(layer["children"]);
            }
        };
        walk(root.value("layers", TJson::array()));
        return !below.empty() ? below : any;
    }

    void followMissing(TitleCheckContext& context, std::vector<TitleCheckResult>& out) {
        TitleCheckResult r;
        r.id = "fx_follow_missing";
        r.kind = TitleCheckKind::Document;
        r.method = "every following generator names a layer of the document";
        const std::vector<TitleFollowProblem> problems = titleFollowProblems(context.doc);
        if (problems.empty()) {
            r.message = "every following generator has its layer";
            out.push_back(std::move(r));
            return;
        }
        r.status = TitleCheckStatus::Fail;
        TJson list = TJson::array();
        TJson ops = TJson::array();
        for (const TitleFollowProblem& p : problems) {
            list.push_back(TJson{{"layer", p.layerId}, {"follow", p.follow}});
            const std::string nearest = nearestFollowable(context.doc.root, p.layerId);
            if (!nearest.empty()) ops.push_back(TJson{{"op", "set"}, {"layer", p.layerId}, {"values", TJson{{"follow", nearest}}}});
            else ops.push_back(TJson{{"op", "remove_layer"}, {"layer", p.layerId}});
        }
        r.measured = TJson{{"generators", list}};
        r.message = std::to_string(problems.size()) + " generator layer(s) follow a deleted layer or none: they draw nothing";
        r.fix = TJson{{"ops", ops}};
        out.push_back(std::move(r));
    }

}

void editor::registerTitleGeneratorChecks() {
    CheckDef def;
    def.id = "fx_follow_missing";
    def.kind = TitleCheckKind::Document;
    def.owner = "S83";
    def.description = "Every following generator names an existing layer";
    def.run = followMissing;
    TitleArtChecks::registerCheck(std::move(def));
}
