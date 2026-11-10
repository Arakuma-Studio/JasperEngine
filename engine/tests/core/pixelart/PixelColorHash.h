// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#pragma once

// The determinism probe of the colour maths (S43 AC5): FNV-1a over the bits of 10^5 OKLab
// conversions, their way back and the fixed cube root. Included by PixelColorTest.cpp (against
// the engine, optimised) and by PixelColorO0.cpp (the same sources built at -O0).

#include <cstdint>
#include <cstring>

namespace jasperengine::pixelart {

    inline uint64_t pixelColorHash() {
        uint64_t hash = 1469598103934665603ull;
        auto mix = [&hash](const void* data, size_t size) {
            const unsigned char* bytes = static_cast<const unsigned char*>(data);
            for (size_t i = 0; i < size; i++) {
                hash ^= bytes[i];
                hash *= 1099511628211ull;
            }
        };
        for (uint32_t k = 0; k < 100000; k++) {
            const uint32_t rgb = (k * 2654435761u) & 0xFFFFFFu;
            const Rgba8 color = unpackRgb(rgb);
            const Lab lab = toOklab(color);
            mix(&lab.l, sizeof(float));
            mix(&lab.a, sizeof(float));
            mix(&lab.b, sizeof(float));
            const Rgba8 back = fromOklab(lab);
            mix(&back, sizeof(back));
            const double root = cbrtFixed((double)(k + 1) * 0.731);
            mix(&root, sizeof(root));
        }
        return hash;
    }

}
