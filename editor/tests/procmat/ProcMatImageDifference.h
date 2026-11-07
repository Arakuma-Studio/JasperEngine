// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#ifndef PROCMAT_IMAGEDIFFERENCE_H
#define PROCMAT_IMAGEDIFFERENCE_H

// How two tiers' images of one output differ, in AC8's terms (S90): the tests that hold the GPU tiers
// to the CPU tier's ProceduralCpuEvaluator measure with it.

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <vector>

namespace jasperengine::editor::procmat::test {

    // AC8's numbers for one output, the worst channel's, in 8-bit steps
    struct ImageDifference {
        double mean = 0.0;
        int p99 = 0;
        int maximum = 0;

        bool withinAc8() const { return mean <= 2.0 && p99 <= 4; }
    };

    inline ImageDifference differenceOf(const std::vector<uint8_t>& a, const std::vector<uint8_t>& b) {
        ImageDifference result;
        const size_t pixels = a.size() / 4;
        for (int channel = 0; channel < 4; channel++) {
            int histogram[256] = {};
            for (size_t i = (size_t)channel; i < a.size() && i < b.size(); i += 4) histogram[std::abs((int)a[i] - (int)b[i])]++;
            size_t sum = 0, seen = 0;
            int p99 = -1, maximum = 0;
            for (int d = 0; d < 256; d++) {
                sum += (size_t)histogram[d] * d;
                seen += histogram[d];
                if (histogram[d]) maximum = d;
                if (p99 < 0 && seen * 100 >= pixels * 99) p99 = d;
            }
            result.mean = std::max(result.mean, (double)sum / (double)pixels);
            result.p99 = std::max(result.p99, p99);
            result.maximum = std::max(result.maximum, maximum);
        }
        return result;
    }

}

#endif
