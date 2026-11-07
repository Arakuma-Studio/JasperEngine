// (c) JasperDev and contributors
// SPDX-License-Identifier: MIT

// The engine's own SoLoud filters (engine-internal: the public headers do not include SoLoud)
#ifndef AudioFilters_h
#define AudioFilters_h

#include <atomic>
#include <cstddef>
#include <vector>

#include "soloud.h"
#include "soloud_filter.h"

namespace jasperengine {

    // A peak limiter, channels linked. With lookahead 0 and truePeak 0 it is lookahead-free: the gain
    // drops at once to keep every sample under the threshold and recovers with the release time.
    // Otherwise the signal is delayed by the lookahead, and the gain the (true-peak) detector asks
    // for goes through a running minimum and a moving average over that delay, so it has arrived
    // when the peak leaves: a Master limiter holds a dBTP ceiling (S38 R4, R5).
    class LimiterFilter : public SoLoud::Filter {
    public:
        // BYPASS is the mixer's: a limiter with a delay stays in its slot while bypassed, so that a
        // switch crossfades instead of dropping or repeating the delay's audio
        enum Attribute { WET = 0, THRESHOLD, RELEASE, LOOKAHEAD, TRUE_PEAK, BYPASS, ATTRIBUTE_COUNT };
        float threshold = -1.0f;   // dB
        float release = 0.1f;      // s
        float lookahead = 0.0f;    // ms
        bool truePeak = false;
        bool bypass = false;
        // what the instance sizes its delay for up front; another rate reallocates on the audio thread
        float sampleRate = 44100.0f;

        // the largest gain reduction of the last 100 ms, dB, 0 or more (the audio thread writes it)
        std::atomic<float> gainReductionDb{0.0f};

        float getGainReductionDb() const;
        // frames of delay at a rate: 72 for 1.5 ms at 48 kHz
        static unsigned int lookaheadFrames(float lookaheadMs, float sampleRate);

        int getParamCount() override;
        const char* getParamName(unsigned int index) override;
        unsigned int getParamType(unsigned int index) override;
        float getParamMax(unsigned int index) override;
        float getParamMin(unsigned int index) override;
        SoLoud::FilterInstance* createInstance() override;
    };

    // The frames a group's meter passes on for a live loudness measurement: a lock-free ring, one
    // producer (the audio thread) and one consumer (the main thread). It holds the first two channels,
    // what a mixer bus has, interleaved; a block that does not fit is dropped.
    class LoudnessTapRing {
    public:
        static constexpr unsigned int CHANNELS = 2;

        explicit LoudnessTapRing(size_t frames);
        void push(const float* planar, unsigned int samples, unsigned int bufferSize, unsigned int channels);
        size_t pop(float* interleaved, size_t maxFrames);
        // on the main thread, with the producer stopped or dropping
        void clear();

    private:
        std::vector<float> data;
        size_t capacity;
        std::atomic<size_t> head{0};   // frames ever written
        std::atomic<size_t> tail{0};   // frames ever read
    };

    // The level meter of a mixer group: the last slot of its bus (after its effects, before its
    // fader). The audio thread keeps the peaks of the last few blocks; the main thread reads their
    // maximum (getPeak), which falls to 0 about 90 ms after the group went silent.
    class MeterFilter : public SoLoud::Filter {
    public:
        static constexpr int blockCount = 8;
        std::atomic<float> blocks[blockCount];
        std::atomic<unsigned int> next{0};
        // the Master group's, while the editor measures loudness (AudioMixer::setLoudnessTap)
        std::atomic<LoudnessTapRing*> tap{nullptr};

        MeterFilter();
        float getPeak() const;
        void reset();

        // A capture's view of each group's blocks (samples of `channels` planes, `stride` apart).
        // Set only on the deterministic driver: its mix is on the main thread, so it needs no lock.
        using CaptureHook = void (*)(const MeterFilter& meter, const float* planar, unsigned int samples, unsigned int stride, unsigned int channels);
        static CaptureHook captureHook;

        SoLoud::FilterInstance* createInstance() override;
    };

}

#endif /* AudioFilters_h */
