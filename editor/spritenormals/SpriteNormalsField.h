// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#pragma once

// The scalar image operations of the generator (internal: SpriteNormals.cpp and the tests).
// Float-exact ports of upstream field.rs / distance.rs / components.rs / color.rs: see the
// parity rules of the Phase 2 spec (no FMA contraction, evaluation order, float literals).

#include <cstddef>
#include <cstdint>
#include <vector>

namespace jasperengine::editor::spritenormals::detail {

    // How a field reads outside itself
    enum class Border { Zero, Wrap, Clamp };

    struct Field {
        int width = 0;
        int height = 0;
        std::vector<float> data;    // row-major
        static Field zero(int width, int height);
    };

    // sRGB 8-bit -> linear, 256 entries (upstream color.rs)
    const float* srgbToLinearTable();
    // Oklab L of an sRGB colour (upstream lightness), and the full Oklab (L, a, b)
    float oklabL(uint8_t r, uint8_t g, uint8_t b);
    void oklab(uint8_t r, uint8_t g, uint8_t b, float& L, float& a, float& bb);

    // Exact Euclidean distance (pixels, centre to centre) from each mask pixel to the nearest
    // non-mask pixel; 0 on non-mask pixels; +INF where the field has none. Border::Zero: nothing
    // beyond the field (upstream); Wrap: toroidal; Clamp: outside counts as mask (Extend).
    Field edt(const std::vector<uint8_t>& mask, int width, int height, Border border = Border::Zero);

    // Separable Gaussian (upstream: sigma < 0.05 is the identity, radius ceil(3 sigma))
    Field gaussian(const Field& input, float sigma, Border border = Border::Zero);
    // gaussian(input * weight) / max(gaussian(weight), 1e-8)
    Field maskedGaussian(const Field& input, const Field& weight, float sigma, Border border = Border::Zero);
    // Normalized Sobel correlation at (x, y): +x slopes up to the right, +y downward. Border::Zero
    // requires (x, y) at least one pixel inside the field.
    void sobel(const Field& input, int x, int y, float& dx, float& dy, Border border = Border::Zero);

    // 8-connected components of `mask` (upstream order)
    struct Component {
        std::vector<int> pixels;    // indices y * width + x, BFS order
        int minX = 0, minY = 0, maxX = 0, maxY = 0;
    };
    std::vector<Component> components(const std::vector<uint8_t>& mask, int width, int height);

    // (128,128,255)-encoding of slopes (upstream slopes_to_normal + byte rounding)
    void encodeNormal(float dx, float dy, float strength, bool directx, uint8_t out[3]);
}
