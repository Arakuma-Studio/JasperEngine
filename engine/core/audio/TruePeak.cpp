// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "TruePeak.h"

#include <algorithm>
#include <cmath>

using namespace jasperengine;

// BS.1770-4 Annex 2, by tap and then phase (the Recommendation lists it by phase): the four phases
// of a tap side by side make the inner loop of processSample one a compiler vectorizes without
// reordering a floating-point sum. Every value is exact in a float (16 fractional bits).
static const float TRUE_PEAK_FIR[TruePeak::TAPS][TruePeak::PHASES] = {
    {  0.0017089843750f,  -0.0291748046875f,  -0.0189208984375f,  -0.0083007812500f},
    {  0.0109863281250f,   0.0292968750000f,   0.0330810546875f,   0.0148925781250f},
    { -0.0196533203125f,  -0.0517578125000f,  -0.0582275390625f,  -0.0266113281250f},
    {  0.0332031250000f,   0.0891113281250f,   0.1015625000000f,   0.0476074218750f},
    { -0.0594482421875f,  -0.1665039062500f,  -0.2003173828125f,  -0.1022949218750f},
    {  0.1373291015625f,   0.4650878906250f,   0.7797851562500f,   0.9721679687500f},
    {  0.9721679687500f,   0.7797851562500f,   0.4650878906250f,   0.1373291015625f},
    { -0.1022949218750f,  -0.2003173828125f,  -0.1665039062500f,  -0.0594482421875f},
    {  0.0476074218750f,   0.1015625000000f,   0.0891113281250f,   0.0332031250000f},
    { -0.0266113281250f,  -0.0582275390625f,  -0.0517578125000f,  -0.0196533203125f},
    {  0.0148925781250f,   0.0330810546875f,   0.0292968750000f,   0.0109863281250f},
    { -0.0083007812500f,  -0.0189208984375f,  -0.0291748046875f,   0.0017089843750f}
};

// the largest sum of a phase's absolute coefficients, rounded up
static const float TRUE_PEAK_MAX_GAIN = 2.03f;

TruePeak::TruePeak(unsigned int channels): histories(std::max(channels, 1u)){
}

unsigned int TruePeak::getChannels() const{
    return (unsigned int)histories.size();
}

float TruePeak::process(const float* interleaved, size_t frames){
    if (!interleaved) {
        return 0.0f;
    }
    const unsigned int channels = getChannels();
    float largest = 0.0f;
    for (size_t i = 0; i < frames; i++) {
        for (unsigned int c = 0; c < channels; c++) {
            largest = std::max(largest, processSample(c, interleaved[i * channels + c]));
        }
    }
    return largest;
}

float TruePeak::processSample(unsigned int channel, float sample){
    if (!std::isfinite(sample)) {
        sample = 0.0f;
    }

    History& history = histories[channel];
    history.newest = history.newest == 0 ? TAPS - 1 : history.newest - 1;
    history.samples[history.newest] = sample;
    history.samples[history.newest + TAPS] = sample;
    const float* recent = history.samples + history.newest;

    float y[PHASES] = {};
    for (unsigned int tap = 0; tap < TAPS; tap++) {
        for (unsigned int phase = 0; phase < PHASES; phase++) {
            y[phase] += TRUE_PEAK_FIR[tap][phase] * recent[tap];
        }
    }

    float largest = std::fabs(sample);
    for (unsigned int phase = 0; phase < PHASES; phase++) {
        largest = std::max(largest, std::fabs(y[phase]));
    }
    return largest;
}

// Whether any |x| is over a limit, as a loop every compiler vectorizes: a floating-point maximum is not one
// (its order and NaNs matter), and a new maximum is rare.
static bool anyOver(const float* values, size_t count, float limit){
    int any = 0;
    for (size_t i = 0; i < count; i++) {
        any |= std::fabs(values[i]) > limit ? 1 : 0;
    }
    return any != 0;
}

static float largestOf(const float* values, size_t count, float start){
    float largest = start;
    for (size_t i = 0; i < count; i++) {
        largest = std::max(largest, std::fabs(values[i]));
    }
    return largest;
}

float TruePeak::processChannel(unsigned int channel, const float* samples, size_t count, float above){
    static constexpr size_t RUN = 256;
    History& history = histories[channel];
    // oldest first: the history, then the run, so that the filter walks a plain array
    float line[TAPS + RUN];
    float y[RUN];
    for (unsigned int tap = 0; tap < TAPS; tap++) {
        line[TAPS - 1 - tap] = history.samples[history.newest + tap];
    }

    float largest = above;
    while (count > 0) {
        const size_t run = count < RUN ? count : RUN;
        for (size_t i = 0; i < run; i++) {
            line[TAPS + i] = std::isfinite(samples[i]) ? samples[i] : 0.0f;
        }
        if (anyOver(line + TAPS, run, largest)) {
            largest = largestOf(line + TAPS, run, largest);
        }
        if (anyOver(line, TAPS + run, largest / TRUE_PEAK_MAX_GAIN)) {
            // one phase at a time over the run, one tap at a time: the same sums as processSample's,
            // in the order that vectorizes
            for (unsigned int phase = 0; phase < PHASES; phase++) {
                std::fill(y, y + run, 0.0f);
                for (unsigned int tap = 0; tap < TAPS; tap++) {
                    const float coefficient = TRUE_PEAK_FIR[tap][phase];
                    const float* in = line + TAPS - tap;
                    for (size_t i = 0; i < run; i++) {
                        y[i] += coefficient * in[i];
                    }
                }
                if (anyOver(y, run, largest)) {
                    largest = largestOf(y, run, largest);
                }
            }
        }
        for (unsigned int tap = 0; tap < TAPS; tap++) {
            line[tap] = line[run + tap];
        }
        samples += run;
        count -= run;
    }

    history.newest = 0;
    for (unsigned int tap = 0; tap < TAPS; tap++) {
        const float x = line[TAPS - 1 - tap];
        history.samples[tap] = x;
        history.samples[tap + TAPS] = x;
    }
    return largest > above ? largest : 0.0f;
}

void TruePeak::reset(){
    std::fill(histories.begin(), histories.end(), History());
}
