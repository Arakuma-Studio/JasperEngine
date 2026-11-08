// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "TitleAnimCost.h"

#include "titleart/TitleArtRenderHost.h"
#include "titleart/anim/TitleAnimRealtime.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

using namespace jasperengine;
using namespace jasperengine::editor;
namespace ta = jasperengine::titleanim;
namespace tt = jasperengine::titleart;

namespace {

    std::string mib(uint64_t bytes){
        char buf[32];
        std::snprintf(buf, sizeof(buf), "%.1f MiB", (double)bytes / (1024.0 * 1024.0));
        return buf;
    }

}

double editor::titleTierBudgetMs(const std::string& tier){
    return tier == "desktop" ? 4.0 : 2.0;
}

double editor::titleTierFactor(const std::string& tier){
    if (tier == "mobile") return 6.0;
    if (tier == "web") return 4.0;
    return 1.0;
}

double editor::titleRenderEstimateUs(const tt::AssetResolver* assets, const tt::TitleArtDoc& doc, float pxScale, const tt::TitleRectI& region){
    tt::RenderRequest request;
    request.assets = assets;
    request.doc = std::make_shared<const tt::TitleArtDoc>(doc);
    request.pxScale = std::max(0.05f, pxScale);
    request.region = region;
    std::string error;
    std::unique_ptr<tt::TitleRenderJob> job = TitleArtRenderHost::editor().begin(nullptr, request, error);
    return job ? (double)job->estimatedTotalUs() : 0.0;
}

tt::Json TitleAnimCost::toJson() const{
    tt::Json out = {{"animation", animation}, {"mode", tt::renderModeName(mode)},
                    {"tracks", {{"plays", plays}, {"falls_back", fallsBack}, {"not_played", notPlayed}}}};
    if (!notes.empty()) out["notes"] = notes;
    if (mode == TitleArtRenderMode::BAKED){
        tt::Json bytesJson = tt::Json::object();
        for (const char* f : TITLE_FLIPBOOK_FAMILIES) bytesJson[f] = bytes.of(f);
        out["flipbook"] = flipbook ? tt::Json{{"frames", frames}, {"unique", unique}, {"pages", pages}, {"frame_size", tt::Json::array({frameWidth, frameHeight})},
                                              {"bytes", bytesJson}, {"baked", flipbookBaked}}
                                   : tt::Json(nullptr);
    }else if (mode == TitleArtRenderMode::REALTIME){
        out["layer_splits"] = layerSplits;
        if (rerender || prerender){
            tt::Json tiers = tt::Json::object();
            for (int i = 0; i < 3; i++) tiers[TITLE_ANIM_TIERS[i]] = {{"ms_per_sample", std::round(rerenderMs[i] * 100.0) / 100.0},
                                                                      {"fps_reached", std::round(reachedFps[i] * 10.0) / 10.0}};
            out["rerender"] = {{"policy", prerender ? "prerender" : "live"}, {"fps", rerenderFps}, {"tiers", tiers}};
            if (prerender) out["rerender"]["prerender_bytes"] = prerenderBytes;
        }
    }else{
        out["cells"] = cells;
        out["quads"] = quads;
    }
    return out;
}

std::string TitleAnimCost::summary() const{
    if (mode == TitleArtRenderMode::BAKED){
        if (!flipbook) return "Baked: title live only (+0 draws, no memory)";
        return "Baked: " + std::to_string(frames) + " frames (" + std::to_string(unique) + " unique" + (flipbookBaked ? "" : ", estimated") + "), " +
               mib(bytes.mobile) + " on Mobile";
    }
    if (mode == TitleArtRenderMode::REALTIME){
        if (!rerender && !prerender) return layerSplits.empty() ? "Realtime: live (no re-render)" : "Realtime: " + std::to_string(layerSplits.size()) + " layer split(s)";
        char buf[160];
        std::snprintf(buf, sizeof(buf), "Realtime: %.0f re-renders per second asked, %.1f reached on Mobile (%.1f ms each)", rerenderFps, reachedFps[1],
                      rerenderMs[1]);
        return buf;
    }
    return "Glyph Atlas: " + std::to_string(cells) + " cells, " + std::to_string(quads) + " quads a frame";
}

TitleAnimCost editor::estimateAnimCost(const tt::TitleArtDoc& doc, const ta::CompiledTitleAnimation& animation, const tt::AnimationPlan& plan,
                                       const TitleAnimCostInputs& inputs){
    TitleAnimCost cost;
    cost.animation = animation.name;
    cost.mode = plan.mode;
    cost.notes = plan.notes;
    for (const tt::TrackPlan& t : plan.tracks){
        if (t.play == tt::TrackPlay::Plays) cost.plays++;
        else if (t.play == tt::TrackPlay::FallsBack) cost.fallsBack++;
        else if (t.code != "muted") cost.notPlayed++;
    }
    const tt::TitleRectI still = inputs.still.frame.empty() ? tt::TitleRectI{0, 0, doc.canvasWidth(), doc.canvasHeight()}
                                                            : tt::TitleRectI{inputs.still.frame.x, inputs.still.frame.y, inputs.still.frame.width, inputs.still.frame.height};
    if (plan.mode == TitleArtRenderMode::BAKED){
        cost.flipbook = plan.flipbook;
        if (!cost.flipbook) return cost;
        const ta::TitleAnimDataEntry* baked = inputs.baked ? inputs.baked->find(animation.name) : nullptr;
        if (baked && !baked->compiled.flipbook.empty()){
            cost.flipbookBaked = true;
            cost.frames = baked->compiled.bakedCost.frames;
            cost.unique = baked->compiled.bakedCost.unique;
            cost.pages = baked->compiled.bakedCost.pages;
            cost.bytes.desktop = baked->compiled.bakedCost.bytesDesktop;
            cost.bytes.mobile = baked->compiled.bakedCost.bytesMobile;
            cost.bytes.androidEtc2 = baked->compiled.bakedCost.bytesAndroidEtc2;
            cost.bytes.web = baked->compiled.bakedCost.bytesWeb;
            cost.frameWidth = baked->compiled.flipbook.pixelWidth;
            cost.frameHeight = baked->compiled.flipbook.pixelHeight;
            return cost;
        }
        // the upper bound: every frame unique, the still's frame at the flipbook's scale
        cost.frames = (int)titleFlipbookFrameTimes(animation).size();
        cost.unique = cost.frames;
        const double scale = std::max(0.05f, inputs.outputScale) * std::max(0.25f, std::min(1.0f, animation.flipbookSettings.scale));
        cost.frameWidth = std::max(1, (int)std::ceil(still.width * scale)) + 2;
        cost.frameHeight = std::max(1, (int)std::ceil(still.height * scale)) + 2;
        const int cellW = (cost.frameWidth + 3) / 4 * 4 + 4, cellH = (cost.frameHeight + 3) / 4 * 4 + 4;
        const int maxPage = animation.flipbookSettings.maxPage;
        const int cols = std::max(1, (maxPage + 4) / cellW), rows = std::max(1, (maxPage + 4) / cellH);
        const int perPage = cols * rows;
        cost.pages = (cost.unique + perPage - 1) / perPage;
        for (int p = 0; p < cost.pages; p++){
            const int count = std::min(perPage, cost.unique - p * perPage);
            const int usedCols = std::min(cols, count), usedRows = (count + cols - 1) / cols;
            cost.bytes.add((usedCols * cellW - 4 + 3) / 4 * 4, (usedRows * cellH - 4 + 3) / 4 * 4);
        }
        return cost;
    }
    if (plan.mode == TitleArtRenderMode::REALTIME){
        cost.layerSplits = plan.layerSplits;
        cost.rerender = plan.rerender;
        cost.prerender = plan.prerender;
        cost.rerenderFps = animation.rerenderFps;
        if (cost.rerender || cost.prerender){
            const double share = std::max(0.25, 1.0 / std::max(1, inputs.animatingTitles));
            for (int i = 0; i < 3; i++){
                const std::string tier = TITLE_ANIM_TIERS[i];
                cost.rerenderMs[i] = inputs.renderUs / 1000.0 * titleTierFactor(tier);
                // the budget share a frame at 60 fps: re-renders a second it allows
                const double perFrame = titleTierBudgetMs(tier) * share;
                const double reached = cost.rerenderMs[i] > 0.0 ? 60.0 * perFrame / cost.rerenderMs[i] : (double)animation.rerenderFps;
                cost.reachedFps[i] = std::min((double)animation.rerenderFps, reached);
            }
            if (cost.prerender){
                const double scale = std::max(0.05f, inputs.outputScale) * std::max(0.25f, std::min(1.0f, animation.flipbookSettings.scale));
                const uint64_t frame = (uint64_t)std::ceil(still.width * scale) * (uint64_t)std::ceil(still.height * scale);
                cost.prerenderBytes = frame * 4ull * (uint64_t)titleFlipbookFrameTimes(animation).size();
            }
        }
        return cost;
    }
    // Glyph Atlas: one back and one front quad per glyph with ink of the glyph layer
    for (const std::string& layer : plan.glyphLayers){
        const tt::Json* l = tt::findLayer(doc.root, layer);
        if (!l) continue;
        const tt::GlyphUnits units = tt::glyphUnitsOf(*l, ta::StaggerUnit::Glyph, nullptr);
        cost.cells += (int)units.count * 2;
        cost.quads += (int)units.count * 2;
    }
    return cost;
}
