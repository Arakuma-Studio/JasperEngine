// (c) JasperDev and contributors
// SPDX-License-Identifier: MIT

// The engine's own SoLoud filters (engine-internal: the public headers do not include SoLoud)
#ifndef AudioFilters_h
#define AudioFilters_h

#include <atomic>

#include "soloud.h"
#include "soloud_filter.h"

namespace jasperengine {

    // A lookahead-free peak limiter: the gain drops at once to keep every sample under the
    // threshold and recovers with the release time (per-sample smoothing, channels linked)
    class LimiterFilter : public SoLoud::Filter {
    public:
        enum Attribute { WET = 0, THRESHOLD, RELEASE };
        float threshold = -1.0f;   // dB
        float release = 0.1f;      // s

        int getParamCount() override;
        const char* getParamName(unsigned int index) override;
        unsigned int getParamType(unsigned int index) override;
        float getParamMax(unsigned int index) override;
        float getParamMin(unsigned int index) override;
        SoLoud::FilterInstance* createInstance() override;
    };

    // The level meter of a mixer group: the last slot of its bus (after its effects, before its
    // fader). The audio thread keeps the peaks of the last few blocks; the main thread reads their
    // maximum (getPeak), which falls to 0 about 90 ms after the group went silent.
    class MeterFilter : public SoLoud::Filter {
    public:
        static constexpr int blockCount = 8;
        std::atomic<float> blocks[blockCount];
        std::atomic<unsigned int> next{0};

        MeterFilter();
        float getPeak() const;
        void reset();

        SoLoud::FilterInstance* createInstance() override;
    };

}

#endif /* AudioFilters_h */
