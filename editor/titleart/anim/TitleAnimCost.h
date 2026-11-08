// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// Contract (docs/specs/S85-title-animation.md R18, R20, R37, R38, R44, §6.7.2)
// - Purpose: one answer to "what does this animation cost" (estimateAnimCost), what the
//   Animation panel's badge, get_title, check_title (anim_frame_budget, anim_flipbook_memory)
//   and export_title's dry run show, per Render Mode:
//   * per class: the tracks that play, fall back, or are not played;
//   * Baked: flipbook frames (total and unique), pages and GPU bytes per cook family (R38's
//     block formats), from the baked flipbook when it is fresh, else estimated from the still's
//     frame at the flipbook's scale (every frame unique, the upper bound);
//   * Realtime: layer splits, the re-render cost per sample (S81 §6.3's model: the planned job's
//     estimate x the platform factor: desktop 1, mobile 6, web 4) and the re-render rate it
//     reaches under R18's share of S81's frame budget (desktop 4 ms, mobile and web 2), and the
//     prerender pages' bytes (4 B/px);
//   * Glyph Atlas: the cells and the compose quads a frame draws.
// - Thread-safety: main thread.

#pragma once

#include "TitleFlipbookBaker.h"
#include "component/TitleArtRendererComponent.h"
#include "titleanim/TitleAnimTypes.h"
#include "titleart/TitleArtDoc.h"
#include "titleart/TitleArtTypes.h"
#include "titleart/anim/TitleAnimPlan.h"
#include "util/TitleArtBakeInfo.h"

#include <string>
#include <vector>

namespace jasperengine::titleart {
    class AssetResolver;
}

namespace jasperengine::editor {

    constexpr const char* TITLE_ANIM_TIERS[] = {"desktop", "mobile", "web"};

    struct TitleAnimCostInputs {
        // the still's frame (the bake sidecar's; empty: the canvas) and the main output's scale
        TitleArtBakeInfo still;
        float outputScale = 1.0f;
        // the baked animation data beside the document (its Baked figures when fresh)
        const titleanim::TitleAnimData* baked = nullptr;
        // the static cost of one full render in us (desktop reference), 0 when unknown
        double renderUs = 0.0;
        // the titles animating at once (R18's share), and the budgets (MiB) of §6.9
        int animatingTitles = 1;
        TitleFlipbookBudget budget;
    };

    struct TitleAnimCost {
        std::string animation;
        TitleArtRenderMode mode = TitleArtRenderMode::BAKED;
        int plays = 0, fallsBack = 0, notPlayed = 0;
        std::vector<std::string> notes;
        // Baked
        bool flipbook = false;
        bool flipbookBaked = false;     // the figures are the baked flipbook's
        int frames = 0, unique = 0, pages = 0;
        TitleFlipbookBytes bytes;
        int frameWidth = 0, frameHeight = 0;
        // Realtime
        std::vector<std::string> layerSplits;
        bool rerender = false, prerender = false;
        double rerenderFps = 0.0;                   // asked
        double rerenderMs[3] = {0.0, 0.0, 0.0};     // per sample, desktop / mobile / web
        double reachedFps[3] = {0.0, 0.0, 0.0};
        uint64_t prerenderBytes = 0;
        // Glyph Atlas
        int cells = 0, quads = 0;
        titleart::Json toJson() const;
        // "Baked: 25 frames (18 unique), 1.9 MiB on Mobile"
        std::string summary() const;
    };

    TitleAnimCost estimateAnimCost(const titleart::TitleArtDoc& doc, const titleanim::CompiledTitleAnimation& animation, const titleart::AnimationPlan& plan,
                                   const TitleAnimCostInputs& inputs);
    // the static cost in us of one full render of the document at `pxScale` over `region` (the
    // planned job's estimate, nothing runs); 0 when it cannot be planned
    double titleRenderEstimateUs(const titleart::AssetResolver* assets, const titleart::TitleArtDoc& doc, float pxScale, const titleart::TitleRectI& region);
    // the frame budget per tier (ms, S81 R15) and the platform factor (S81 §6.3)
    double titleTierBudgetMs(const std::string& tier);
    double titleTierFactor(const std::string& tier);

}
