// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT
//
// The Title Designer's side of S83's catalogue (docs/specs/S83-title-effects-generators.md R39,
// R41, D10, D12; S82 §6.6): cost badges of effects, generator layers and stacks from S83's static
// model (FxCost), amber "Baked on mobile" when a Realtime or Glyph Atlas renderer of an open scene
// shows the title, and the Preview Live toggle that plays an effect's live (L) params on the
// canvas as overrides of the render, never as document edits.

#include "TitleDesignerWindow.h"

#include "Project.h"
#include "component/TitleArtRendererComponent.h"
#include "titleart/fx/FxCost.h"

#include <algorithm>
#include <cmath>

using namespace jasperengine;
using namespace jasperengine::editor;
using namespace jasperengine::editor::titledesigner;
namespace ta = jasperengine::titleart;

namespace {

    std::string str(const ta::Json& object, const char* key) {
        auto it = object.find(key);
        return it != object.end() && it->is_string() ? it->get<std::string>() : std::string();
    }

    std::string ms(double value) {
        char text[32];
        std::snprintf(text, sizeof(text), value < 10.0 ? "%.2f ms" : "%.1f ms", value);
        return text;
    }

    ta::CostClass maxClass(ta::CostClass a, ta::CostClass b) {
        return (uint8_t)a >= (uint8_t)b ? a : b;
    }

}

const TitleDesignerWindow::CostCache& TitleDesignerWindow::costs() {
    const ta::TitleArtDoc& shown = doc.shown();
    if (costCache.shown == &shown && costCache.change == doc.changeCount()) return costCache;
    documentPasses++;
    costCache.shown = &shown;
    costCache.change = doc.changeCount();
    costCache.layers.clear();
    const TitleFxCost desktop = estimateTitleFxCost(shown, ta::FxTier::Desktop);
    const TitleFxCost mobile = estimateTitleFxCost(shown, ta::FxTier::Mobile);
    // the rows of each layer in order: its enabled effects ("fx:"), its shared distance field, its generator
    auto collect = [&](const TitleFxCost& cost, bool isMobile) {
        for (const TitleFxCostRow& row : cost.rows) {
            CostCache::Layer& layer = costCache.layers[row.layer];
            CostCache::Row* target = nullptr;
            if (row.path.rfind("fx:", 0) == 0) {
                const size_t n = isMobile ? layer.mobileEffects++ : layer.desktopEffects++;
                if (layer.effects.size() <= n) layer.effects.resize(n + 1);
                target = &layer.effects[n];
            } else if (row.path == "gen") {
                target = &layer.generator;
            }
            if (isMobile) {
                layer.totalMobile = maxClass(layer.totalMobile, row.costClass);
                layer.mobileMs += row.mobileMs;
            } else {
                layer.total = maxClass(layer.total, row.costClass);
                layer.desktopMs += row.desktopMs;
            }
            if (!target) continue;
            target->valid = true;
            if (isMobile) target->mobile = row.costClass;
            else target->desktop = row.costClass;
            target->desktopMs = row.desktopMs;
            target->mobileMs = row.mobileMs;
        }
    };
    collect(desktop, false);
    collect(mobile, true);
    return costCache;
}

bool TitleDesignerWindow::documentRealtime() {
    // the open scenes' Title Art Renderers that show this title in Realtime or Glyph Atlas, looked
    // at again every 2 s while badges are drawn (never on idle frames without them)
    if (!project || !doc.isOpen()) return false;
    const double t = now();
    if (realtimeCheckedAt >= 0.0 && t - realtimeCheckedAt < 2.0 && realtimeCheckedPath == doc.document()->projectPath()) return realtimeUse;
    realtimeCheckedAt = t;
    realtimeCheckedPath = doc.document()->projectPath();
    realtimeUse = false;
    const std::string mine = project->normalizeToAssetsRelative(doc.document()->absolutePath()).generic_string();
    for (SceneProject& scene : project->getScenes()) {
        if (!scene.scene || !scene.scene->isComponentRegistered<TitleArtRendererComponent>()) continue;
        for (Entity entity : scene.entities) {
            const TitleArtRendererComponent* title = scene.scene->findComponent<TitleArtRendererComponent>(entity);
            if (!title || title->renderMode == TitleArtRenderMode::BAKED || title->title.path.empty()) continue;
            const std::string theirs = project->normalizeToAssetsRelative(project->getProjectPath() / title->title.path).generic_string();
            if (theirs == mine) {
                realtimeUse = true;
                return true;
            }
        }
    }
    return false;
}

bool TitleDesignerWindow::effectCost(const std::string& layerId, const std::string& part, int index, CostBadge& out) {
    if (!doc.isOpen()) return false;
    const ta::TitleArtDoc& shown = doc.shown();
    const ta::Json* layer = layerById(shown, layerId);
    if (!layer) return false;
    const CostCache& cache = costs();
    auto found = cache.layers.find(layerId);
    const bool realtime = documentRealtime();
    auto badgeOf = [&](ta::CostClass desktop, ta::CostClass mobile, double desktopMs, double mobileMs, bool modeled) {
        out.cls = ta::costClassName(desktop);
        out.tooltip = std::string("Cost ") + ta::costClassName(desktop) + (modeled ? " \xC2\xB7 desktop " + ms(desktopMs) + " \xC2\xB7 mobile " +
                                                                                       ta::costClassName(mobile) + " " + ms(mobileMs) + " (predicted)"
                                                                                 : std::string(" (at its defaults)"));
        if (mobile == ta::CostClass::C4) {
            out.tooltip += "\nBake only on mobile: a Realtime title keeps this as a baked split there";
            out.amber = realtime;
        }
        return true;
    };
    if (ta::layerTypeOf(*layer) == ta::LayerType::Generator) {
        const ta::GeneratorDef* def = ta::findGenerator(layer->value("generator", ta::Json::object()).value("type", std::string()));
        if (found != cache.layers.end() && found->second.generator.valid) {
            const CostCache::Row& row = found->second.generator;
            return badgeOf(row.desktop, row.mobile, row.desktopMs, row.mobileMs, true);
        }
        return def ? badgeOf(def->costClass, def->costClass, 0.0, 0.0, false) : false;
    }
    if (index < 0) {
        if (found == cache.layers.end()) return false;
        return badgeOf(found->second.total, found->second.totalMobile, found->second.desktopMs, found->second.mobileMs, true);
    }
    // the effect entry: the model rows are the layer stack's enabled effects in order; part
    // stacks and disabled effects show their definition's class at its defaults
    const ta::Json* list = nullptr;
    if (part.empty()) {
        auto it = layer->find("effects");
        if (it != layer->end() && it->is_array()) list = &*it;
    } else {
        auto parts = layer->find("part_effects");
        if (parts != layer->end() && parts->is_object() && parts->contains(part)) list = &(*parts)[part];
    }
    if (!list || index >= (int)list->size()) return false;
    const ta::Json& effect = (*list)[(size_t)index];
    const ta::EffectDef* def = ta::findEffect(str(effect, "type"));
    if (!def) return false;
    if (part.empty() && effect.value("enabled", true) && found != cache.layers.end()) {
        int enabledBefore = 0;
        for (int i = 0; i < index; i++) {
            const ta::Json& e = (*list)[(size_t)i];
            if (e.value("enabled", true) && ta::findEffect(str(e, "type"))) enabledBefore++;
        }
        if (enabledBefore < (int)found->second.effects.size() && found->second.effects[(size_t)enabledBefore].valid) {
            const CostCache::Row& row = found->second.effects[(size_t)enabledBefore];
            return badgeOf(row.desktop, row.mobile, row.desktopMs, row.mobileMs, true);
        }
    }
    return badgeOf(def->costClass, def->costClass, 0.0, 0.0, false);
}

bool TitleDesignerWindow::livePreviewOn(const std::string& layer, const std::string& effectId) const {
    return livePreviews.count({layer, effectId}) > 0;
}

void TitleDesignerWindow::setLivePreview(const std::string& layer, const std::string& effectId, bool on) {
    if (on) livePreviews.insert({layer, effectId});
    else livePreviews.erase({layer, effectId});
    if (livePreviews.empty()) liveStart = -1.0;
    else if (liveStart < 0.0) liveStart = now();
}

std::vector<ta::FieldOverride> TitleDesignerWindow::liveOverrides(double* phaseKey) {
    std::vector<ta::FieldOverride> out;
    if (livePreviews.empty() || !doc.isOpen()) return out;
    const ta::TitleArtDoc& shown = doc.shown();
    // a 2 s cycle in 12 steps: each step is one canvas render (renders cache by the key)
    const double t = std::floor((now() - liveStart) * 6.0) / 6.0;
    if (phaseKey) *phaseKey = t;
    const double wave = 0.5 + 0.5 * std::cos(t * 3.14159265358979 / 1.0);
    for (auto it = livePreviews.begin(); it != livePreviews.end();) {
        const ta::Json* layer = layerById(shown, it->first);
        const NodeRef node{NodeKind::Effect, it->first, {}, it->second};
        const ta::Json* effect = layer ? effectOf(*layer, node) : nullptr;
        const ta::EffectDef* def = effect ? ta::findEffect(str(*effect, "type")) : nullptr;
        if (!def || def->live.kind == ta::LiveKind::None) {
            // the effect went away: its toggle with it
            it = livePreviews.erase(it);
            continue;
        }
        const std::string base = effectPathOf(*layer, node) + ".params.";
        const ta::Json params = effect->value("params", ta::Json::object());
        for (const char* key : def->live.params) {
            const ta::ParamDef* param = nullptr;
            for (const ta::ParamDef& p : def->params) {
                if (std::string(p.key) == key) param = &p;
            }
            if (!param) continue;
            const ta::Json current = params.contains(key) ? params[key] : param->defaultValue;
            ta::Json value;
            if (param->kind == ta::ParamKind::Number && current.is_number()) {
                const double v = current.get<double>();
                if (param->unit == ta::ParamUnit::Deg) {
                    value = v + 360.0 * std::fmod(t / 4.0, 1.0);
                } else {
                    // between the low end of its range (or 0) and the value set
                    const double lo = param->max > param->min ? std::max(param->min, std::min(0.0, v)) : std::min(0.0, v);
                    value = lo + (v - lo) * wave;
                }
            } else if (param->kind == ta::ParamKind::Vec2 && current.is_array() && current.size() == 2) {
                // offsets scroll
                value = ta::Json::array({current[0].get<double>() + 40.0 * t, current[1].get<double>()});
            } else {
                continue;
            }
            out.push_back({it->first, base + key, value});
        }
        ++it;
    }
    if (livePreviews.empty()) liveStart = -1.0;
    return out;
}
