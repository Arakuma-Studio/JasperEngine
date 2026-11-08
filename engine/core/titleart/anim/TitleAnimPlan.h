// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// Contract (docs/specs/S85-title-animation.md R2-R12, R14, R15, R19, §6.2, §6.3)
// - Purpose: an animation of the document compiled for playing (compileAnimation: fields
//   resolved, values numeric, rests, ranges, stagger units from the text layout, classes), and
//   the plan of what each Render Mode does with each track (buildAnimPlan: plays, falls back or
//   is not played, with the reason). Pure: the editor's panel, get_title, check_title, the export
//   and the runtime all read the same plan.
// - Classes (R11) are data on fields (D5): S80's FxAnim per schema member and S83's per param;
//   S85 adds its title and glyph virtual fields; `text` is Glyph Atlas live; an L param plays live
//   only when its effect has a live variant naming it and every later effect of the stack is
//   scale_split (S83 D10), else it re-renders with that reason.
// - Thread-safety: pure functions (the test flag is set before other threads plan).

#pragma once

#include "Export.h"
#include "component/TitleArtRendererComponent.h"
#include "titleanim/TitleAnimPlayer.h"
#include "titleanim/TitleAnimTypes.h"
#include "titleart/TitleArtDoc.h"

#include <memory>
#include <string>
#include <vector>

namespace jasperengine::titleart {

    class TitleTextLayout;

    // One animation compiled against the document. `layout` gives glyph tracks their units
    // (grapheme clusters with ink, words, lines); without one, units come from the text's code
    // points. Problems (anim_orphan, not_animatable, key_clamped, ease_dropped,
    // reseed_not_seed, relative_stepped) go to `problems`.
    JASPERENGINE_API titleanim::CompiledTitleAnimation compileAnimation(const TitleArtDoc& doc, const titleanim::TitleAnimation& animation,
                                                                        TitleTextLayout* layout, std::vector<TitleProblem>* problems,
                                                                        const std::string& path = {});
    // every animation of the document
    JASPERENGINE_API std::shared_ptr<titleanim::TitleAnimLibrary> compileAnimations(const TitleArtDoc& doc, TitleTextLayout* layout,
                                                                                    std::vector<TitleProblem>* problems);

    // the units of a glyph track's stagger: per cluster of the layout (logical order) the unit it
    // belongs to (-1: whitespace or no ink) and the number of units
    struct JASPERENGINE_API GlyphUnits {
        std::vector<int32_t> clusterUnit;
        size_t count = 0;
    };
    JASPERENGINE_API GlyphUnits glyphUnitsOf(const Json& textLayer, titleanim::StaggerUnit unit, TitleTextLayout* layout);

    // ---- the plan (R12) ----

    enum class TrackPlay : uint8_t { Plays, FallsBack, NotPlayed };
    JASPERENGINE_API const char* trackPlayName(TrackPlay play);     // "plays", "falls_back", "not_played"

    struct JASPERENGINE_API TrackPlan {
        size_t track = 0;                   // into the animation's tracks
        std::string layer, field;
        titleanim::TrackClass cls = titleanim::TrackClass::Rerender;    // R11's class
        TrackPlay play = TrackPlay::Plays;
        // how: "companion", "flipbook", "layer_split", "glyph_cells", "cells_swap", "rerender",
        // "prerender", "plate_rerender", "none"
        std::string how;
        std::string reason;                 // why it falls back or is not played ("" plays)
        std::string code;                   // the problem code of a fallback ("adjustment_above", ...)
    };

    struct JASPERENGINE_API AnimationPlan {
        std::string name;
        TitleArtRenderMode mode = TitleArtRenderMode::BAKED;
        std::vector<TrackPlan> tracks;
        bool flipbook = false;              // Baked: frames needed (a track that is not title live)
        bool rerender = false;              // Realtime / Glyph Atlas: re-render requests at rerender_fps
        bool prerender = false;
        std::vector<std::string> layerSplits;   // Realtime: the layers composed live
        std::vector<std::string> glyphLayers;   // the text layers animated by cells
        std::vector<std::string> notes;     // overlay_needs_title_live, glow_pulse_over_frames ...
    };

    struct JASPERENGINE_API TitleAnimPlan {
        TitleArtRenderMode mode = TitleArtRenderMode::BAKED;
        std::vector<AnimationPlan> animations;
        const AnimationPlan* find(const std::string& name) const;

        // tests: AC8's red control
        struct TestFlags {
            bool ignoreAdjustments = false;
        };
        static TestFlags& testFlags();
    };

    JASPERENGINE_API TitleAnimPlan buildAnimPlan(const TitleArtDoc& doc, const std::vector<titleanim::CompiledTitleAnimation>& animations,
                                                 TitleArtRenderMode mode);

    // R14: why a layer cannot be a layer split ("" when it can): the rule that fails
    JASPERENGINE_API std::string layerSplitBlocker(const TitleArtDoc& doc, const std::string& layerId, std::string* code = nullptr);
    // S83 D10: why a live (L) parameter re-renders instead ("" when it plays live)
    JASPERENGINE_API std::string liveParamBlocker(const Json& layer, const std::string& longPath);

    JASPERENGINE_API const char* renderModeName(TitleArtRenderMode mode);      // "baked", "realtime", "glyph_atlas"
    JASPERENGINE_API bool renderModeFromName(const std::string& name, TitleArtRenderMode& out);

}
