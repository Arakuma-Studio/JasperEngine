// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// Contract (docs/specs/S85-title-animation.md R15, R16, R39, R46, R53, §6.6)
// - Purpose: a title's animation rendered at a time, the editor's one way to see a pose (the
//   Animation panel's canvas, render_title's frames and filmstrips, the flipbook baker, the
//   pixel checks). `as` says what a game would show: Exact (every track at its own time), Baked
//   (re-render values held per flipbook frame, the frame's scale), Realtime (re-render values at
//   rerender_fps, the plan's fallbacks) and Glyph Atlas (unplayed tracks left out).
// - Pipeline: the pose's document values are field overrides of one S80 render (the renderer's
//   caches make an unchanged pose free); a `cells` text layer with glyph tracks is drawn as its
//   back plate, its glyph cells (S80 glyphCell renders: rims behind every face) with each glyph's
//   transform, opacity, tint and flash, then its front plate (R15); with `companion` the
//   title-level values follow as the companion draws them: S81's Live Effects (sweep, tint,
//   glints, glow) and the title's transform, opacity and flash (the ANIM feature) about the bake
//   frame's pivot.
// - Pixels: CPU composition in linear premultiplied float; out straight-alpha sRGB RGBA8.
// - Thread-safety: main thread (the render host's).

#pragma once

#include "component/TitleArtRendererComponent.h"
#include "texsynth/SynthImage.h"
#include "titleanim/TitleAnimPlayer.h"
#include "titleart/TitleArtDoc.h"
#include "titleart/render/TitleArtRenderer.h"
#include "titleart/TitleArtTypes.h"
#include "titleart/anim/TitleAnimPlan.h"
#include "util/TitleArtBakeInfo.h"

#include <map>
#include <memory>
#include <string>
#include <vector>

namespace jasperengine::editor {

    class Project;

    enum class TitleAnimRenderAs : uint8_t { Exact, Baked, Realtime, GlyphAtlas };
    const char* titleAnimRenderAsName(TitleAnimRenderAs as);        // "exact", "baked", "realtime", "glyph_atlas"
    bool titleAnimRenderAsFromName(const std::string& name, TitleAnimRenderAs& out);

    struct TitleAnimFrameRequest {
        std::string animation;
        double time = 0.0;              // seconds since the start (wrapped as the animation plays)
        TitleAnimRenderAs as = TitleAnimRenderAs::Exact;
        float pxScale = 1.0f;           // output pixels per canvas pixel (Baked multiplies its flipbook scale)
        bool companion = true;          // the title-level values (transform, opacity, flash, Live Effects)
        bool liveEffects = true;        // S81's Live Effects with the component's defaults (no instance)
        // the canvas region to render (image space); empty: the whole canvas
        titleart::TitleRectI region;
        std::vector<titleart::FieldOverride> overrides;     // what-if values under the pose (render_title's try)
        // the glyph-cell compose of the animation's cells text layers even when no glyph moves
        // (anim_glyph_merge's rest against the merged still)
        bool forceCells = false;
        uint32_t testFlags = 0;
    };

    struct TitleAnimFrame {
        texsynth::Image image;          // RGBA8 straight-alpha sRGB, rows top first
        titleart::TitleRectI frame;     // canvas pixels it covers
        titleart::TitleRectI alphaBounds;
        double coveragePct = 0.0;       // of the image with alpha >= 1/255
        double localTime = 0.0;         // the animation's local time sampled
        std::vector<std::string> notPlayed;     // tracks the `as` mode leaves out ("layer/field: reason")
        std::vector<titleart::TitleProblem> problems;
        bool cells = false;             // a glyph-cell compose drew it
        uint64_t poseHash = 0;          // what was rendered (equal frames: equal hashes)
    };

    struct TitleRenderedImage;
    class TitleAnimRenderer {
    public:
        // the document's animations compiled with the project's fonts (glyph units from the layout)
        TitleAnimRenderer(Project* project, std::shared_ptr<const titleart::TitleArtDoc> doc);
        ~TitleAnimRenderer();

        const titleanim::TitleAnimLibrary& library() const{ return *lib; }
        const std::shared_ptr<titleanim::TitleAnimLibrary>& libraryPtr() const{ return lib; }
        const std::vector<titleart::TitleProblem>& compileProblems() const{ return problems; }
        // the plan of a mode (made once per mode)
        const titleart::TitleAnimPlan& plan(TitleArtRenderMode mode);
        // the still's frame the companion shows (the bake sidecar's when there is one, else the canvas)
        void setStillFrame(const TitleArtBakeInfo& bake);
        const TitleArtBakeInfo& stillFrame() const{ return still; }

        bool render(const TitleAnimFrameRequest& request, TitleAnimFrame& out, std::string& error);
        // R39: steps the pose's base render within `budgetMs`; false while it is not ready (keep
        // showing the previous image), with `error` set when it failed
        bool advancePose(const TitleAnimFrameRequest& request, double budgetMs, std::string& error);
        // the title-level pose of an animation at a time (what the companion draws)
        titleanim::TitlePoseSummary summaryAt(const std::string& animation, double time) const;

    private:
        bool baseRender(const titleart::RenderRequest& request, TitleRenderedImage& image, std::string& error);
        struct PoseSetup;
        bool poseSetup(const TitleAnimFrameRequest& request, PoseSetup& setup, TitleAnimFrame& out, std::string& error);
        Project* project = nullptr;
        std::shared_ptr<const titleart::TitleArtDoc> doc;
        std::shared_ptr<titleanim::TitleAnimLibrary> lib;
        std::vector<titleart::TitleProblem> problems;
        std::map<int, titleart::TitleAnimPlan> plans;
        TitleArtBakeInfo still;
        struct Cache;
        std::unique_ptr<Cache> cache;
    };

    // S87 R42's frame source: export_title's sheet, atlas and sequence formats render through it
    void registerTitleAnimExportFrames();

    // the Live Effect values a pose's title tracks give, over the component's defaults (S81 R33-R37)
    struct TitleLiveEffectValues {
        bool sweep = false, glow = false, tint = false, glints = false;
        float sweepPosition = 0.0f, sweepWidth = 0.15f, sweepAngle = 25.0f, sweepSoftness = 0.6f;
        float sweepColor[4] = {1.0f, 1.0f, 1.0f, 0.8f};     // linear
        float glowPulse = 0.0f;
        float glowColor[4] = {1.0f, 0.85f, 0.4f, 1.0f};
        float tintColor[4] = {1.0f, 1.0f, 1.0f, 1.0f};
        float tintAmount = 0.0f;
        float glintAmount = 1.0f, glintSize = 0.06f, glintRate = 0.5f, glintSeed = 0.0f, liveTime = 0.0f;
        float glintColor[4] = {1.0f, 1.0f, 1.0f, 1.0f};
    };
    TitleLiveEffectValues liveEffectsOf(const titleanim::TitlePoseSummary& summary);
    // title_fx's live layers on a straight sRGB RGBA8 image of the frame, in place (glow: the
    // frame's glow output as straight sRGB RGBA8 of the same size, or null)
    void applyLiveEffects(texsynth::Image& frame, const TitleLiveEffectValues& values, const std::vector<TitleArtAnchor>& anchors,
                          const texsynth::Image* glow);

}
