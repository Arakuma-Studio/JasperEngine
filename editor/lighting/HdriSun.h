// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// The sun of an HDR sky (spec S99 R28, R29), found on the CPU: Extract Sun aligns the scene's main
// Directional Light to it, and Remove Sun Disc (a texture import setting) clamps its disc in the
// cook so image-based lighting does not count the sun twice. Editor-only.
//
// The sky is binned into a downsampled equirect (EQUIRECT_WIDTH x EQUIRECT_HEIGHT, each bin the
// solid-angle weighted mean of the texels in it, so a sun smaller than a bin is not missed). Its
// brightest 3x3 block of bins is the candidate; a sky whose block is under MIN_PEAK_RATIO times the
// sky's mean luminance has no distinct sun. The sun's direction is the luminance-weighted
// centroid (luminance above the local background) of the source texels within CENTROID_DEGREES of
// the block, refined once around itself. Its disc is the texels within MAX_DISC_DEGREES brighter
// than the background by a share of the peak; its light is the disc's radiance above the ring
// around it times the texels' solid angle: the irradiance it gives a surface facing it, which is
// the engine intensity of a directional light (Lit draws I / pi for both).

#pragma once

#include "math/Vector3.h"

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace jasperengine::editor::hdrisun {

    constexpr int EQUIRECT_WIDTH = 256;
    constexpr int EQUIRECT_HEIGHT = 128;
    constexpr float MIN_PEAK_RATIO = 20.0f;
    constexpr float CENTROID_DEGREES = 10.0f;
    constexpr float MAX_DISC_DEGREES = 5.0f;

    enum class SkyLayout : uint8_t {
        CubeFace,   // one face of a cube (the engine's convention: 0 +X, 1 -X, 2 +Y, 3 -Y, 4 +Z, 5 -Z)
        Equirect    // longitude across (atan2(x, z), -180 at the left), latitude down (+90 at the top)
    };

    // A view of RGBA float texels (four floats a texel, linear), rows top first
    struct SkyImage {
        float* rgba = nullptr;
        int width = 0;
        int height = 0;
        int stride = 0;     // floats from one row to the next (0: width * 4)
        SkyLayout layout = SkyLayout::CubeFace;
        int face = 4;       // a lone face of an unknown cube is any face: angles within it are the same
    };

    struct SunResult {
        Vector3 direction;              // towards the sun, in the sky image's space (normalised)
        Vector3 irradiance;             // linear: the disc above its ring, radiance x solid angle
        Vector3 ringColor;              // linear: what Remove Sun Disc gives the disc's texels
        float peakRatio = 0.0f;         // the brightest 3x3 block against the mean luminance
        float discDegrees = 0.0f;       // the disc's angular radius
    };

    // false with why when the sky has no distinct sun (or no texels)
    bool findSun(const std::vector<SkyImage>& images, SunResult& out, std::string& error);
    // the texels within the sun's disc set to its ring colour (alpha kept); how many
    size_t removeSunDisc(const std::vector<SkyImage>& images, const SunResult& sun);

    // An image as views: a 4:3 or 3:4 cross (cube) as its six faces, else a square image as a lone
    // face or a 2:1 one as an equirect. False for any other shape.
    bool viewsOf(float* rgba, int width, int height, bool cube, std::vector<SkyImage>& out);
    // Remove Sun Disc on one decoded RGBA float image in place (the cook): false with why when its
    // shape is none of viewsOf's or it has no distinct sun (the image is left as it was)
    bool removeSunDiscInPlace(float* rgba, int width, int height, bool cube, std::string& why);

}
