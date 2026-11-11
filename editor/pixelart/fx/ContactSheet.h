// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// Contact sheets of captured effect frames (docs/specs/S48-pixel-fx.md R27; pixelfx
// tools/lib/fxlib.mjs contactSheet and src/core/scene.js backdrops): the frames in a grid at one
// integer scale, a band of rows per backdrop, each frame labelled "#i t=0.25s" under it, 1 px
// separators. Each band is a PoseContactSheet (editor/pose/PoseImage.h, the sheet S91's particle
// filmstrips and the pose sheets draw): the backdrop's name in its title strip, the same label
// strips and font. Pure CPU and integer maths, so the same frames give the same PNG bytes.

#pragma once

#include "pixelart/PixelImage.h"
#include "pixelart/Rgba8.h"

#include <string>
#include <vector>

namespace jasperengine::editor::pixelart::fx {

    // pixelfx's backdrops plus a checker and the scene's own clear colour
    enum class Backdrop { Checker, Dark, Light, Sky, Ground, Black, Scene };

    const char* backdropName(Backdrop backdrop);
    bool parseBackdrop(const std::string& name, Backdrop& out);
    // every name, for errors
    std::string backdropNames();

    // A frameWidth x frameHeight backdrop in art pixels. groundY: the first ground row of "ground"
    // (a stone floor from there down); sceneColor: "scene"
    PixelImage backdropImage(Backdrop backdrop, int width, int height, int groundY, jasperengine::pixelart::Rgba8 sceneColor);

    struct SheetSettings {
        std::vector<Backdrop> backdrops{Backdrop::Checker};
        int scale = 0;              // 0: the largest that fits maxWidth (1..16)
        int columns = 0;            // 0: up to 8, fewer when the sheet would pass maxWidth at scale 1
        bool labels = true;
        int maxWidth = 1024;
        int groundY = -1;           // art pixels; < 0: the frame's bottom quarter
        jasperengine::pixelart::Rgba8 sceneColor{21, 19, 29, 255};
        // the frames are the whole view (scene mode): one band, no backdrop under them
        bool opaqueFrames = false;
    };

    struct SheetLayout {
        static constexpr int kLabelHeight = 14;   // PoseSheetLayout::LABEL_HEIGHT
        static constexpr int kHeaderHeight = 14;  // a band's title strip
        int frameWidth = 0, frameHeight = 0;
        int scale = 1;
        int columns = 1, rows = 1;                // rows per band
        int bands = 1;
        int cellWidth = 0, cellHeight = 0;        // a frame and its label strip
        int bandHeight = 0;                       // a band's PoseSheetLayout height
        int width = 0, height = 0;                // bands stacked with a 1 px separator

        // the top-left corner of frame i's image in band b
        IntPoint frameOrigin(int band, int index) const;
    };

    SheetLayout layoutSheet(int frameWidth, int frameHeight, int frameCount, const SheetSettings& settings);

    // N evenly spaced times: a loop's period without its end (t = duration is t = 0 again), a
    // one-shot from 0 to its end; one frame is t = 0. Rounded to the microsecond.
    std::vector<double> sheetTimes(int frames, double duration, bool loop);

    // "t=0.25s": up to 3 decimals, trailing zeros cut
    std::string timeLabel(double seconds);

    // The sheet. frames: premultiplied on transparent black (a capture's read-back) unless
    // settings.opaqueFrames; times label them; all frames share one size
    PixelImage composeSheet(const std::vector<const PixelImage*>& frames, const std::vector<double>& times, const SheetSettings& settings,
                            SheetLayout* layout = nullptr);

}
