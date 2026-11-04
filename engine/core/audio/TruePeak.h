// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#ifndef TRUEPEAK_H
#define TRUEPEAK_H

#include "Export.h"

#include <cstddef>
#include <vector>

namespace jasperengine{

    // The BS.1770-4 Annex 2 true-peak detector: 4x oversampling with its 48-tap FIR (4 phases of 12
    // taps), streaming, one filter per channel. A value counts both the interpolated points and
    // the original samples, whose exact level the filter's phases do not reproduce. A NaN or an
    // infinity is read as 0, so it cannot poison a filter. Allocates only in the constructor: the
    // limiter runs it on the audio thread. Plain C++, no SoLoud, and apart from AudioMeter so that a
    // release player links this without the meter.
    class JASPERENGINE_API TruePeak{
    public:
        static constexpr unsigned int PHASES = 4;
        static constexpr unsigned int TAPS = 12;
        // The interpolated points of a step lie between the inputs DELAY and DELAY - 1 steps before
        // it: a peak shows up that much after the sample that made it.
        static constexpr unsigned int DELAY = 6;

        explicit TruePeak(unsigned int channels);

        unsigned int getChannels() const;

        // The largest absolute value of these frames of interleaved samples, over all channels.
        float process(const float* interleaved, size_t frames);
        // Feeds one sample of one channel, for a caller that needs it step by step; the largest
        // absolute value of this step.
        float processSample(unsigned int channel, float sample);
        void reset();

    private:
        struct History{
            // written twice, TAPS apart, so that the last TAPS samples are always contiguous
            float samples[2 * TAPS] = {};
            unsigned int newest = 0;
        };

        std::vector<History> histories;
    };

}

#endif //TRUEPEAK_H
