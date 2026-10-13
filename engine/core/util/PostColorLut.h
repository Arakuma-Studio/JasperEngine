// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#ifndef POSTCOLORLUT_H
#define POSTCOLORLUT_H

#include "SceneSettings.h"
#include "math/Vector3.h"
#include <cstdint>
#include <vector>

namespace jasperengine{

    // The colour LUTs of the post-processing (shader library phase 4), baked on the CPU from
    // PostProcessingSettings whenever they change. A LUT is a strip of N*N x N RGBA8 texels:
    // blue picks one of N slices laid out left to right, red runs along x inside a slice,
    // green along y (row 0 first in memory), as includes/post_color.glsl and uber.frag sample
    // them. Every texel holds a display-encoded colour.
    //
    // Two domains:
    //  - shaper (the in-shader tail, T0): a texel's coordinate s is sqrt(x / (x + 1)) of the
    //    linear scene colour x (0..inf), so the LUT holds encode(grade(curve(exposure * x))).
    //  - display (the uber pass): a texel's coordinate is a display-encoded colour, the LUT
    //    holds the grade of it.
    //
    // The pipeline (one order everywhere): exposure (stops), tone mapping curve (linear
    // display 0..1), white balance (temperature, tint: an LMS diagonal), display encoding
    // (pow 1/2.2, the engine's), contrast around 0.5, lift / gain then gamma per channel,
    // saturation (Rec. 709 luma), monochrome (grayscale or sepia, mixed by its intensity), the
    // user's LUT strip (mixed by its contribution), clamp 0..1.
    class JASPERENGINE_API PostColorLut{
    public:
        // the user's LUT strip (an image N*N x N, Unity's layout: green from the bottom row up),
        // as RGB8 rows from the top; empty when none
        struct Strip{
            int size = 0;          // N
            int width = 0;         // N * N
            std::vector<uint8_t> rgb; // width * N * 3, row 0 the image's top
        };

        enum class Stage{
            ALL,            // exposure, curve, grade (the shaper domain): T0 alone
            CURVE,          // exposure and curve only (the shaper domain): T0 with the uber pass
            GRADE           // the grade only (the display domain): the uber pass
        };

        // the linear display colour of the curve (0..1) of a linear scene colour
        static Vector3 toneMap(TonemapMode mode, const Vector3& color);
        // the grade of a linear display colour, display-encoded out
        static Vector3 grade(const PostProcessingSettings& settings, const Strip* strip, const Vector3& linearDisplay);

        // whether the settings grade at all (white balance ... user LUT, or monochrome)
        static bool hasGrade(const PostProcessingSettings& settings);
        // whether they change the curve (exposure or a tone mapping curve)
        static bool hasCurve(const PostProcessingSettings& settings);

        // bakes a strip of side n for that stage into rgba (n * n * n * 4 bytes)
        static void bake(const PostProcessingSettings& settings, const Strip* strip, Stage stage, int n, std::vector<uint8_t>& rgba);

        // loads a LUT strip image (height N, width N * N); false with a logged error otherwise
        static bool loadStrip(const std::string& path, Strip& strip);
    };

}

#endif //POSTCOLORLUT_H
