// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#ifndef AUDIOMETER_H
#define AUDIOMETER_H

#include "Export.h"
#include "TruePeak.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

namespace jasperengine{

    // a biquad section, transposed direct form II, a0 = 1
    struct JASPERENGINE_API AudioBiquad{
        double b0 = 1.0, b1 = 0.0, b2 = 0.0, a1 = 0.0, a2 = 0.0;
        double z1 = 0.0, z2 = 0.0;

        void set(const double coefficients[5]);
        double process(double x);
        // In digital silence the state rings down into denormals and can stay there in a limit cycle,
        // which costs many times the CPU on some processors. This lets go of a state that small; every
        // 10 ms is often enough, long before it gets there, and leaves process() without a branch.
        void settle();
    };

    // One channel's BS.1770-4 K-weighting (shelf, then high-pass)
    struct JASPERENGINE_API AudioKWeighting{
        AudioBiquad shelf;
        AudioBiquad highPass;

        void init(unsigned int sampleRate);
        double process(double x);
        // AudioBiquad::settle
        void settle();
    };

    // One channel's octave band bank: 10 bands centred on AudioMeter::bandCentre (4th-order
    // Butterworth band-passes) and the sub band (20-60 Hz)
    class JASPERENGINE_API AudioOctaveBank{
    public:
        static constexpr int BANDS = 10;

        void init(unsigned int sampleRate);
        // y[0..BANDS-1] the bands, y[BANDS] the sub band
        void process(double x, double y[BANDS + 1]);
        // AudioBiquad::settle
        void settle();

    private:
        // up to two high-pass and two low-pass sections (4th-order Butterworth each)
        struct Band{
            AudioBiquad stages[4];
            int count = 0;
            // the band lies above the Nyquist limit: no output
            bool empty = false;

            double process(double x);
            void settle();
        };

        Band bands[BANDS];
        Band sub;

        static void initBand(Band& band, double low, double high, unsigned int sampleRate);
    };

    // What AudioMeter measured. A loudness with no value (nothing above the -70 LUFS gate, a signal
    // shorter than its window) is empty (JSON null); levels of digital silence are floored at
    // SILENCE_DB.
    struct LoudnessResult{
        static constexpr double SILENCE_DB = -120.0;

        // frames of the signal (no padding)
        uint64_t frames = 0;
        unsigned int channels = 0;
        unsigned int sampleRate = 0;

        // LUFS
        std::optional<double> integrated;
        std::optional<double> momentaryMax;
        std::optional<double> shortTermMax;
        // LU
        std::optional<double> lra;
        // the integrated loudness' 400 ms blocks (every 100 ms) that pass both gates, and the
        // relative gate (LUFS; empty without a block over the absolute gate)
        uint64_t gatedBlocks = 0;
        std::optional<double> relativeGate;

        // dBFS of the largest |sample|, dBTP of the 4x oversampled one
        double samplePeakDb = SILENCE_DB;
        double truePeakDb = SILENCE_DB;
        // AES17 (a full-scale sine reads 0 dBFS), all channels, and the mean square behind it
        double rmsDb = SILENCE_DB;
        double meanSquare = 0.0;
        // the largest absolute per-channel mean
        double dcOffset = 0.0;
        // samples (all channels) at or above AudioMeter::CLIP_LEVEL
        uint64_t clipped = 0;
        // samples (all channels) that were NaN or infinite: metered as 0
        uint64_t nonFinite = 0;

        // the octave bands (AudioMeter::bandCentre): AES17 dBFS of each band's mean square over all
        // channels, and those mean squares. The sub band (20-60 Hz): its mean square and its share
        // of the signal's, 0..1. All at the floor when AudioMeterOptions::bands is off.
        std::array<double, AudioOctaveBank::BANDS> bandsDb = {SILENCE_DB, SILENCE_DB, SILENCE_DB, SILENCE_DB, SILENCE_DB,
                                                              SILENCE_DB, SILENCE_DB, SILENCE_DB, SILENCE_DB, SILENCE_DB};
        std::array<double, AudioOctaveBank::BANDS> bandsMeanSquare = {};
        double subMeanSquare = 0.0;
        double subShare = 0.0;
    };

    enum class AudioMeterMode{
        // a live stream (a capture): silence before the start, no padding after the end
        Stream,
        // a whole file: 400 ms of silence before and after it for the momentary windows, 12 samples
        // for the true-peak filter (finish() adds the trailing ones)
        File
    };

    struct AudioMeterOptions{
        AudioMeterMode mode = AudioMeterMode::Stream;
        // the octave bands and the sub band
        bool bands = true;
    };

    // Loudness, peaks and levels of float audio: ITU-R BS.1770-4 loudness (K-weighting, gating, true
    // peak by the Annex 2 filter), EBU Tech 3341 momentary and short-term maxima, EBU Tech 3342
    // loudness range, AES17 RMS and an octave band bank. Plain C++, no SoLoud. Keeps 8 bytes for
    // every 10 ms of signal; result() walks them, so it costs in proportion to the signal so far.
    class JASPERENGINE_API AudioMeter{
    public:
        static constexpr int BANDS = AudioOctaveBank::BANDS;
        // not MAX_CHANNELS, which soloud.h defines as a macro
        static constexpr unsigned int CHANNELS_MIN = 1;
        static constexpr unsigned int CHANNELS_MAX = 8;
        static constexpr unsigned int SAMPLE_RATE_MIN = 8000;
        static constexpr unsigned int SAMPLE_RATE_MAX = 192000;
        static constexpr double SILENCE_DB = LoudnessResult::SILENCE_DB;
        // |x| at or above this is a clipped sample (1 - 2^-15)
        static constexpr double CLIP_LEVEL = 1.0 - 1.0 / 32768.0;
        // the step of the curves, and the hop of the integrated blocks and short-term windows, s
        static constexpr double CURVE_STEP = 0.1;
        // BS.1770's absolute gate, LUFS
        static constexpr double ABSOLUTE_GATE_LUFS = -70.0;

        AudioMeter(unsigned int sampleRate, unsigned int channels, const AudioMeterOptions& options = AudioMeterOptions());

        // rate and channels in range; process() does nothing otherwise
        bool isValid() const;
        unsigned int getSampleRate() const;
        unsigned int getChannels() const;

        // frames of interleaved samples (channels per frame)
        void process(const float* interleaved, size_t frames);
        // planar[c] holds channel c's frames
        void processPlanar(const float* const* planar, size_t frames);
        // File mode: adds the trailing silence; later process() calls do nothing. Stream mode:
        // nothing to add. result() may be read before and after it.
        void finish();
        bool isFinished() const;

        LoudnessResult result() const;

        // Point i is the window that ends at (i + 1) * CURVE_STEP s. A short-term point before 3 s,
        // which has no value, is NaN.
        std::vector<float> momentaryCurve() const;
        std::vector<float> shortTermCurve() const;

        // a whole signal in File mode
        static LoudnessResult measure(const float* interleaved, size_t frames, unsigned int sampleRate,
                                      unsigned int channels, bool bands = true);
        static LoudnessResult measurePlanar(const float* const* planar, size_t frames, unsigned int sampleRate,
                                            unsigned int channels, bool bands = true);

        // the exact centre of band b, 1000 * 2^(b - 5) Hz, and its nominal label ("31.5" .. "16k")
        static double bandCentre(int band);
        static const char* bandLabel(int band);

        // BS.1770-4 K-weighting at a rate, derived as libebur128 does: {b0, b1, b2, a1, a2} of the
        // shelf (stage 1) and the high-pass (stage 2), a0 = 1
        static void kWeighting(unsigned int sampleRate, double shelf[5], double highPass[5]);

        // BS.1770-4's weight of channel c of a layout of `channels`, in WAV channel order: the LFE
        // of 5.1 and 7.1 is 0, side surrounds (5.0: L R C Ls Rs; 5.1: L R C LFE Ls Rs; 7.1:
        // L R C LFE Lb Rb Ls Rs) are 1.41, everything else 1
        static double channelWeight(unsigned int channels, unsigned int c);

    private:
        struct Channel{
            AudioKWeighting kWeighting;
            double weight = 1.0;

            AudioOctaveBank bands;
            double bandEnergy[BANDS] = {};
            double subEnergy = 0.0;

            double sum = 0.0;
            double energy = 0.0;
        };

        unsigned int sampleRate;
        unsigned int channels;
        AudioMeterOptions options;
        bool valid;
        bool finished = false;

        std::vector<Channel> state;
        TruePeak truePeakFilter;

        // frames through the K-weighting (padding included), frames of the signal
        uint64_t processed = 0;
        uint64_t signalFrames = 0;

        // [k]: the channel-weighted K-weighted energy of the sub-blocks before 10 ms sub-block k, so
        // that any window's sum is a subtraction. The sub-block in progress and the frame where it
        // ends.
        std::vector<double> energyBefore;
        double subBlockSum = 0.0;
        uint64_t subBlockEnd = 0;

        double peak = 0.0;
        double truePeak = 0.0;
        uint64_t clipped = 0;
        uint64_t nonFinite = 0;

        // the first frame of 10 ms sub-block k (k may be negative: silence before the start)
        int64_t boundary(int64_t k) const;
        int64_t completeSubBlocks() const;
        // the last sub-block boundary inside the signal
        int64_t signalSubBlocks() const;
        // mean energy of the window of `length` sub-blocks that ends at sub-block `end`; sub-blocks
        // before the start are silence
        double windowEnergy(int64_t end, int64_t length) const;
        double windowLoudness(int64_t end, int64_t length) const;
        void frame(const double* x, bool padding);
        // a sample as metered: a NaN or an infinity would poison the filters and the sums for good
        double finite(float sample);
    };

}

#endif //AUDIOMETER_H
