// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#ifndef POSTCOLORLUT_H
#define POSTCOLORLUT_H

#include "SceneSettings.h"
#include "math/Vector3.h"
#include "util/CubeLut.h"
#include <cstdint>
#include <string>
#include <vector>

namespace jasperengine{

    // The colour LUTs of the post-processing (shader library phase 4), baked on the CPU from
    // PostProcessingSettings whenever they change. A LUT is a strip of N*N x N RGBA8 texels:
    // blue picks one of N slices laid out left to right, red runs along x inside a slice,
    // green along y (row 0 first in memory), as includes/post_color.glsl and uber.frag sample
    // them. Every texel holds a display-encoded colour.
    //
    // Two domains:
    //  - shaper (the in-shader tail, T0): a texel's coordinate s is sqrt(x / (x + k)) of the
    //    linear scene colour x (0..inf), so the LUT holds encode(grade(curve(exposure * x))).
    //    k is 1, or with tone mapping NONE the one putting its clip point on a texel
    //    (RenderSystem::getPostColorShaper): linear filtering across a texel pair that
    //    straddles the clip would leave whites short of 255.
    //  - display (the uber pass): a texel's coordinate is a display-encoded colour, the LUT
    //    holds the grade of it.
    //
    // The pipeline (one order everywhere): exposure (stops), a scene-stage user LUT (mixed by
    // its contribution), tone mapping curve (linear display 0..1), white balance (temperature,
    // tint: an LMS diagonal), display encoding (pow 1/2.2, the engine's), contrast around 0.5,
    // lift / gain then gamma per channel, saturation (Rec. 709 luma), monochrome (grayscale or
    // sepia, mixed by its intensity), a display-stage user LUT (mixed by its contribution),
    // clamp 0..1. The user LUT (lutTexture: a .cube, or a PNG strip converted to a CubeLut) sits
    // at one of the two stages (stageOf, CubeLut.h); only grading on uses it.
    class JASPERENGINE_API PostColorLut{
    public:
        // a LUT strip image (N*N x N, Unity's layout: green from the bottom row up), as RGB8
        // rows from the top; empty when none. stripToCubeLut makes the CubeLut grade samples
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
        // the grade of a linear display colour, display-encoded out; lut is the settings'
        // lutTexture, loaded (null: none), which applies here when it is a display-stage LUT
        static Vector3 grade(const PostProcessingSettings& settings, const CubeLut* lut, const Vector3& linearDisplay);

        // the stage the settings' user LUT applies at; false when none applies (grading off,
        // no lutTexture, contribution 0, or no loaded LUT)
        static bool getLutStage(const PostProcessingSettings& settings, const CubeLut* lut, LutStage& stage);

        // whether the settings grade at all (white balance ... a display-stage LUT, or
        // monochrome); without the loaded LUT a set lutTexture counts as a display LUT
        static bool hasGrade(const PostProcessingSettings& settings);
        static bool hasGrade(const PostProcessingSettings& settings, const CubeLut* lut);
        // whether they change the curve (exposure, a tone mapping curve or a scene-stage LUT);
        // without the loaded LUT only exposure and the curve count
        static bool hasCurve(const PostProcessingSettings& settings);
        static bool hasCurve(const PostProcessingSettings& settings, const CubeLut* lut);

        // bakes a strip of side n for that stage into rgba (n * n * n * 4 bytes); shaper is the
        // shaper domain's k (ALL and CURVE). A scene-stage lut goes into ALL and CURVE, a
        // display-stage one into ALL and GRADE
        static void bake(const PostProcessingSettings& settings, const CubeLut* lut, Stage stage, int n, std::vector<uint8_t>& rgba,
                         float shaper = 1.0f);
        // what a texel of that bake (ALL or CURVE) holds at shaper code s (0..1 per channel), unrounded
        static Vector3 evaluate(const PostProcessingSettings& settings, const CubeLut* lut, Stage stage, const Vector3& code,
                                float shaper = 1.0f);
        // the shaper code whose evaluate is nearest `display` (0..1 per channel), per channel by
        // bisection over a few sweeps (exact where the curve and grade are monotone per channel);
        // the SSR composite's linear store of the clear colour
        static Vector3 invert(const PostProcessingSettings& settings, const CubeLut* lut, Stage stage, const Vector3& display,
                              float shaper = 1.0f);

        // loads a LUT strip image (height N, width N * N); false with a logged error otherwise,
        // also returned in error
        static bool loadStrip(const std::string& path, Strip& strip, std::string* error = nullptr);
        // a strip as the 3D CubeLut of side N it holds (fromStrip, sRGB display colour); an
        // empty LUT for an empty or malformed strip
        static void stripToCubeLut(const Strip& strip, CubeLut& lut);
    };

}

#endif //POSTCOLORLUT_H
