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

void TruePeak::reset(){
    std::fill(histories.begin(), histories.end(), History());
}
