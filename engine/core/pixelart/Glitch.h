// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#ifndef PIXELART_GLITCH_H
#define PIXELART_GLITCH_H

// The uber pass's Glitch (docs/specs/S48-pixel-fx.md R18, R19): which corruption a frame shows.
// The schedule is a pure function of the corruption frame floor(time * fps) and the seed, so a
// pinned shader time gives one image; the shader (uber.frag, UBER_GLITCH) only applies it. The
// scripted burst (PixelFX.glitch) raises the main scene's intensity for some unscaled seconds
// without touching its saved settings.

#include "Export.h"

#include <array>
#include <cstdint>

namespace jasperengine {
    struct PostProcessingSettings;
}

namespace jasperengine::pixelart {

    struct GlitchTear {
        int row = 0;        // first source row, from the top
        int height = 1;     // rows
        int shift = 0;      // source texels, + right
    };

    struct GlitchFrame {
        static constexpr int kMaxTears = 8;
        uint32_t frame = 0;     // floor(time * fps)
        float strength = 0.0f;  // 0: calm (at most the lone row slip), else the burst's strength
        int split = 0;          // red left and blue right by this many source texels (signed)
        bool interlace = false; // every odd source row dark
        int tearCount = 0;
        std::array<GlitchTear, kMaxTears> tears{};
    };

    // the corruption of the frame at `time` for a source of `sourceRows` rows; scriptIntensity
    // (0..1) is a running scripted burst's, which makes every frame a burst at least that strong
    JASPERENGINE_API GlitchFrame glitchFrame(const PostProcessingSettings& settings, double time, int sourceRows, float scriptIntensity = 0.0f);

    // the frame's tears packed for the shader: one float each, row + 8192 * (height - 1) +
    // 65536 * (shift + 64), exact in a float (23 bits)
    JASPERENGINE_API float packGlitchTear(const GlitchTear& tear);

    class JASPERENGINE_API Glitch {
    public:
        // a burst of `intensity` (0..1) for `seconds` of unscaled time on the main scene; it shows
        // only where the scene's post-processing has Glitch enabled (the variant has UBER_GLITCH).
        // A second call keeps the longer remaining time and the higher intensity
        static void burst(float seconds, float intensity);
        static void stop();
        static float getBurstIntensity();   // 0 when none runs
        static float getBurstRemaining();
        static void update(double unscaledDt);
    };

}

#endif // PIXELART_GLITCH_H
