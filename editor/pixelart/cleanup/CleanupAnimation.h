// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT
//
// Ported from ritmo (packages/tools/src/pixel/core/animation.ts):
// MIT License, Copyright (c) 2026 Sandro Maglione.

// The whole fix for an animation (docs/specs/S46-pixel-art-cleanup.md R19-R22): the sheet's
// background is cleared, every frame is read on one cell size and fixed with one recipe (so one
// palette), and the frames are placed on one canvas per animation with their anchors on one spot.
// Animated sources (GIF, APNG, a multi-frame .aseprite, a Sprite's frames) skip the slicing:
// runFrames takes their frames as they are, with their own delays. A cache keeps the slow steps
// (clearing, each frame's own grid) between runs, so the window re-runs on every slider move.

#pragma once

#include "pixelart/cleanup/CleanupPalette.h"
#include "pixelart/cleanup/CleanupPipeline.h"
#include "pixelart/cleanup/CleanupSheet.h"
#include "pixelart/cleanup/CleanupTypes.h"

#include <map>
#include <optional>
#include <string>
#include <vector>

namespace jasperengine::editor::pixelart::cleanup {

    struct AnimationCache {
        // the cleared sheet, for one picture (size and hash) and background setting
        bool hasCleared = false;
        uint64_t clearedImage = 0;
        std::string clearedKey;
        RgbaImage cleared;
        std::vector<Rgba8> colors;
        // by box ("x,y,w,h"), so moving one frame's box reads only that frame again
        struct FrameEntry {
            RgbaImage crop;
            std::optional<Detection> detection;
            PipelineCache pipeline;
        };
        std::map<std::string, FrameEntry> frames;
    };

    // One frame, fixed
    struct FixedFrame {
        int index = 0;
        FrameBox box;
        RgbaImage crop;                       // the frame cut from the cleared sheet: what its grid is read from
        GridSpec spec;                        // the shared cell size at the frame's own offset
        std::optional<Detection> detection;   // what detection found in this frame alone
        Outcome outcome;
        PixelArt art;                         // the fixed frame on the shared palette
        std::optional<Point> anchor;          // in its own pixels, before nudging
        int delayMs = 0;                      // an animated source's own delay (0: the animation's fps)
    };

    struct ClipFrame {
        PixelArt art;
        Point anchor;
        int index = 0;                        // the sheet frame it is
    };

    // One animation, its included frames on one canvas
    struct ClipOutcome {
        int animation = 0;
        std::string name;
        bool include = true;
        std::vector<ClipFrame> placed;
        int width = 0, height = 0;
    };

    struct AnimationOutcome {
        std::vector<Rgba8> backgroundFound;   // the colours cleared from the sheet
        RgbaImage cleared;
        double cellWidth = 0, cellHeight = 0; // the cell every frame is read at
        std::optional<double> confidence;     // how sure the shared cell is (nullopt for a given grid)
        std::vector<FixedFrame> frames;       // every frame, included or not
        std::vector<ClipOutcome> clips;       // every animation, exported or not
        std::vector<LostColor> lost;          // across the included frames of the exported animations (frame set)
        std::optional<RecipePalette> palette; // the palette every frame snapped to (an auto one proposed once for all)
        Sheet sheet;                          // the sheet that ran
        int partialAlpha = 0;                 // source pixels with 0 < alpha < 255
    };

    struct ClearedSheet {
        RgbaImage cleared;
        std::vector<Rgba8> colors;
    };
    // The sheet's background cleared when the recipe removes backgrounds (cached)
    ClearedSheet clearedSheet(const RgbaImage& image, const Recipe& recipe, AnimationCache& cache);
    // The frames found in a sheet: every one included, none nudged
    std::vector<SheetFrame> detectFrames(const RgbaImage& image, const Recipe& recipe, AnimationCache& cache);
    // Two or more frames, alike (looksAnimated on the cleared sheet)
    bool isAnimationSheet(const RgbaImage& image, const Recipe& recipe, const std::vector<SheetFrame>& frames, AnimationCache& cache);

    struct SharedCell {
        double size = 0;
        double confidence = 0;
    };
    // The cell the frames share: the confidence-weighted median of every frame's two axes
    std::optional<SharedCell> sharedCell(const std::vector<std::optional<Detection>>& detections);

    // Runs the recipe on every frame of a sheet (no hand edits, shrinking or cropping per frame)
    AnimationOutcome runSheet(const RgbaImage& image, const Recipe& recipe, const Sheet& sheet, AnimationCache& cache);

    // R22: an animated source's frames (one size), as one animation named `name` with their own
    // delays (ms; 0 or missing: the sheet's fps). The sheet's anchor and padding come from `sheet`
    // when it is given, else the defaults; its frames and animations are made here.
    AnimationOutcome runFrames(const std::vector<RgbaImage>& frames, const std::vector<int>& delaysMs, const std::string& name,
                               const Recipe& recipe, const std::optional<Sheet>& sheet, AnimationCache& cache);

    // Every opaque cell colour of every included frame of the exported animations
    std::vector<Rgba8> sheetColors(const AnimationOutcome& outcome);
    // The animations that are exported, with at least one frame
    std::vector<const ClipOutcome*> exportedClips(const AnimationOutcome& outcome);

    // One exported animation as the writers take it (CleanupSheetOutput)
    struct AtlasClip {
        std::string name;
        std::vector<PixelArt> frames;         // one size, one palette with the others
        std::vector<int> durationsMs;         // per frame
        Point anchor;
        double fps = DEFAULT_FPS;
        Loop loop = Loop::Forward;
    };
    std::vector<AtlasClip> atlasClips(const AnimationOutcome& outcome);

}
