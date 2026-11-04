// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "AudioMeter.h"

#include <algorithm>
#include <cmath>
#include <limits>

using namespace jasperengine;

static const double METER_PI = 3.14159265358979323846;
// 10 log10(2): the AES17 offset, so a full-scale sine reads 0 dBFS
static const double AES17_OFFSET_DB = 3.0103;

// BS.1770-4: 10 ms sub-blocks; momentary 400 ms, integrated blocks 400 ms every 100 ms, short-term
// 3 s every 100 ms (in sub-blocks)
static const int64_t SUB_BLOCKS_PER_SECOND = 100;
static const int64_t MOMENTARY_SUB_BLOCKS = 40;
static const int64_t HOP_SUB_BLOCKS = 10;
static const int64_t SHORT_TERM_SUB_BLOCKS = 300;
static const double RELATIVE_GATE_LU = -10.0;
static const double LRA_RELATIVE_GATE_LU = -20.0;
// File mode pads this much silence before and after the signal for the momentary windows
static const double FILE_PADDING_SECONDS = 0.4;

// 4th-order Butterworth as two biquads
static const double BUTTERWORTH_Q[2] = {0.5411961001461970, 1.3065629648763766};
// a band's low-pass is left out from this share of the rate, and the band is empty when its
// lower edge reaches it
static const double BAND_EDGE_LIMIT = 0.45;
static const double SUB_LOW_HZ = 20.0;
static const double SUB_HIGH_HZ = 60.0;

static const char* BAND_LABELS[AudioMeter::BANDS] = {"31.5", "63", "125", "250", "500", "1k", "2k", "4k", "8k", "16k"};

static double floorDb(double db){
    return (std::isnan(db) || db < AudioMeter::SILENCE_DB) ? AudioMeter::SILENCE_DB : db;
}

static double amplitudeDb(double amplitude){
    return amplitude > 0.0 ? floorDb(20.0 * std::log10(amplitude)) : AudioMeter::SILENCE_DB;
}

static double meanSquareDb(double meanSquare){
    return meanSquare > 0.0 ? floorDb(10.0 * std::log10(meanSquare) + AES17_OFFSET_DB) : AudioMeter::SILENCE_DB;
}

// BS.1770: L = -0.691 + 10 log10(sum of the channel-weighted mean squares); -inf for silence
static double energyLoudness(double energy){
    return energy > 0.0 ? -0.691 + 10.0 * std::log10(energy) : -std::numeric_limits<double>::infinity();
}

// floor(a / b) for b > 0
static int64_t floorDivide(int64_t a, int64_t b){
    int64_t q = a / b;
    return (a % b != 0 && a < 0) ? q - 1 : q;
}

// RBJ cookbook sections, prewarped by the bilinear transform
static void lowPassSection(double* c, double frequency, double q, double rate){
    double w = 2.0 * METER_PI * frequency / rate;
    double cw = std::cos(w);
    double alpha = std::sin(w) / (2.0 * q);
    double a0 = 1.0 + alpha;
    c[0] = (1.0 - cw) / 2.0 / a0;
    c[1] = (1.0 - cw) / a0;
    c[2] = (1.0 - cw) / 2.0 / a0;
    c[3] = -2.0 * cw / a0;
    c[4] = (1.0 - alpha) / a0;
}

static void highPassSection(double* c, double frequency, double q, double rate){
    double w = 2.0 * METER_PI * frequency / rate;
    double cw = std::cos(w);
    double alpha = std::sin(w) / (2.0 * q);
    double a0 = 1.0 + alpha;
    c[0] = (1.0 + cw) / 2.0 / a0;
    c[1] = -(1.0 + cw) / a0;
    c[2] = (1.0 + cw) / 2.0 / a0;
    c[3] = -2.0 * cw / a0;
    c[4] = (1.0 - alpha) / a0;
}

void AudioBiquad::set(const double c[5]){
    b0 = c[0]; b1 = c[1]; b2 = c[2]; a1 = c[3]; a2 = c[4];
}

double AudioBiquad::process(double x){
    double y = b0 * x + z1;
    z1 = b1 * x - a1 * y + z2;
    z2 = b2 * x - a2 * y;
    return y;
}

void AudioBiquad::settle(){
    // -600 dB, far under the meter's floor. Both at once: flushing one disturbs the other, which
    // keeps a limit cycle going at this level.
    const double smallest = 1e-30;
    if (std::fabs(z1) < smallest && std::fabs(z2) < smallest) {
        z1 = 0.0;
        z2 = 0.0;
    }
}

void AudioKWeighting::init(unsigned int sampleRate){
    double shelfCoefficients[5];
    double highCoefficients[5];
    AudioMeter::kWeighting(sampleRate, shelfCoefficients, highCoefficients);
    shelf = AudioBiquad();
    shelf.set(shelfCoefficients);
    highPass = AudioBiquad();
    highPass.set(highCoefficients);
}

double AudioKWeighting::process(double x){
    return highPass.process(shelf.process(x));
}

void AudioKWeighting::settle(){
    shelf.settle();
    highPass.settle();
}

double AudioOctaveBank::Band::process(double x){
    if (empty) {
        return 0.0;
    }
    for (int i = 0; i < count; i++) {
        x = stages[i].process(x);
    }
    return x;
}

void AudioOctaveBank::Band::settle(){
    for (int i = 0; i < count; i++) {
        stages[i].settle();
    }
}

void AudioOctaveBank::initBand(Band& band, double low, double high, unsigned int sampleRate){
    band = Band();
    const double rate = (double)sampleRate;
    if (low >= BAND_EDGE_LIMIT * rate) {
        band.empty = true;
        return;
    }

    double c[5];
    for (double q : BUTTERWORTH_Q) {
        highPassSection(c, low, q, rate);
        band.stages[band.count++].set(c);
    }
    if (high < BAND_EDGE_LIMIT * rate) {
        for (double q : BUTTERWORTH_Q) {
            lowPassSection(c, high, q, rate);
            band.stages[band.count++].set(c);
        }
    }
}

void AudioOctaveBank::init(unsigned int sampleRate){
    for (int b = 0; b < BANDS; b++) {
        double centre = AudioMeter::bandCentre(b);
        initBand(bands[b], centre / std::sqrt(2.0), centre * std::sqrt(2.0), sampleRate);
    }
    initBand(sub, SUB_LOW_HZ, SUB_HIGH_HZ, sampleRate);
}

void AudioOctaveBank::process(double x, double y[BANDS + 1]){
    for (int b = 0; b < BANDS; b++) {
        y[b] = bands[b].process(x);
    }
    y[BANDS] = sub.process(x);
}

void AudioOctaveBank::settle(){
    for (Band& band : bands) {
        band.settle();
    }
    sub.settle();
}

void AudioMeter::kWeighting(unsigned int sampleRate, double shelf[5], double highPass[5]){
    const double rate = (double)sampleRate;

    double f0 = 1681.974450955533;
    double gain = 3.999843853973347;
    double q = 0.7071752369554196;
    double k = std::tan(METER_PI * f0 / rate);
    double vh = std::pow(10.0, gain / 20.0);
    double vb = std::pow(vh, 0.4996667741545416);
    double a0 = 1.0 + k / q + k * k;
    shelf[0] = (vh + vb * k / q + k * k) / a0;
    shelf[1] = 2.0 * (k * k - vh) / a0;
    shelf[2] = (vh - vb * k / q + k * k) / a0;
    shelf[3] = 2.0 * (k * k - 1.0) / a0;
    shelf[4] = (1.0 - k / q + k * k) / a0;

    f0 = 38.13547087602444;
    q = 0.5003270373238773;
    k = std::tan(METER_PI * f0 / rate);
    a0 = 1.0 + k / q + k * k;
    highPass[0] = 1.0;
    highPass[1] = -2.0;
    highPass[2] = 1.0;
    highPass[3] = 2.0 * (k * k - 1.0) / a0;
    highPass[4] = (1.0 - k / q + k * k) / a0;
}

double AudioMeter::bandCentre(int band){
    return 1000.0 * std::pow(2.0, band - 5);
}

const char* AudioMeter::bandLabel(int band){
    return (band >= 0 && band < BANDS) ? BAND_LABELS[band] : "";
}

double AudioMeter::channelWeight(unsigned int channels, unsigned int c){
    const double surround = 1.41;
    if (channels == 5 && c >= 3 && c < 5) {
        return surround;
    }
    if (channels == 6 || channels == 8) {
        if (c == 3) {
            return 0.0;
        }
        if (c == channels - 2 || c == channels - 1) {
            return surround;
        }
    }
    return 1.0;
}

AudioMeter::AudioMeter(unsigned int sampleRate, unsigned int channels, const AudioMeterOptions& options):
    sampleRate(sampleRate), channels(channels), options(options), truePeakFilter(1){

    valid = channels >= CHANNELS_MIN && channels <= CHANNELS_MAX &&
            sampleRate >= SAMPLE_RATE_MIN && sampleRate <= SAMPLE_RATE_MAX;
    if (!valid) {
        return;
    }

    state.resize(channels);
    for (unsigned int c = 0; c < channels; c++) {
        Channel& channel = state[c];
        channel.kWeighting.init(sampleRate);
        channel.weight = channelWeight(channels, c);
        if (options.bands) {
            channel.bands.init(sampleRate);
        }
    }
    truePeakFilter = TruePeak(channels);

    energyBefore.push_back(0.0);
    subBlockEnd = (uint64_t)boundary(1);
}

bool AudioMeter::isValid() const{
    return valid;
}

unsigned int AudioMeter::getSampleRate() const{
    return sampleRate;
}

unsigned int AudioMeter::getChannels() const{
    return channels;
}

bool AudioMeter::isFinished() const{
    return finished;
}

int64_t AudioMeter::boundary(int64_t k) const{
    // floor(k * rate / 100): sub-block lengths differ by a frame at rates like 22050
    return floorDivide(k * (int64_t)sampleRate, SUB_BLOCKS_PER_SECOND);
}

int64_t AudioMeter::completeSubBlocks() const{
    return (int64_t)energyBefore.size() - 1;
}

int64_t AudioMeter::signalSubBlocks() const{
    const int64_t complete = completeSubBlocks();
    int64_t last = floorDivide((int64_t)signalFrames * SUB_BLOCKS_PER_SECOND, sampleRate);
    while (last > 0 && boundary(last) > (int64_t)signalFrames) {
        last--;
    }
    while (boundary(last + 1) <= (int64_t)signalFrames) {
        last++;
    }
    return std::min(last, complete);
}

double AudioMeter::finite(float sample){
    if (std::isfinite(sample)) {
        return sample;
    }
    nonFinite++;
    return 0.0;
}

void AudioMeter::frame(const double* x, bool padding){
    double energy = 0.0;
    for (unsigned int c = 0; c < channels; c++) {
        Channel& channel = state[c];
        double weighted = channel.kWeighting.process(x[c]);
        energy += channel.weight * weighted * weighted;

        if (padding) {
            continue;
        }

        double magnitude = std::fabs(x[c]);
        peak = std::max(peak, magnitude);
        if (magnitude >= CLIP_LEVEL) {
            clipped++;
        }
        channel.sum += x[c];
        channel.energy += x[c] * x[c];
        truePeak = std::max(truePeak, (double)truePeakFilter.processSample(c, (float)x[c]));

        if (options.bands) {
            double y[BANDS + 1];
            channel.bands.process(x[c], y);
            for (int b = 0; b < BANDS; b++) {
                channel.bandEnergy[b] += y[b] * y[b];
            }
            channel.subEnergy += y[BANDS] * y[BANDS];
        }
    }

    subBlockSum += energy;
    processed++;
    if (!padding) {
        signalFrames++;
    }
    while (processed >= subBlockEnd) {
        energyBefore.push_back(energyBefore.back() + subBlockSum);
        subBlockSum = 0.0;
        subBlockEnd = (uint64_t)boundary(completeSubBlocks() + 1);
        for (Channel& channel : state) {
            channel.kWeighting.settle();
            if (options.bands) {
                channel.bands.settle();
            }
        }
    }
}

void AudioMeter::process(const float* interleaved, size_t frames){
    if (!valid || finished || !interleaved) {
        return;
    }
    double x[CHANNELS_MAX];
    for (size_t i = 0; i < frames; i++) {
        for (unsigned int c = 0; c < channels; c++) {
            x[c] = finite(interleaved[i * channels + c]);
        }
        frame(x, false);
    }
}

void AudioMeter::processPlanar(const float* const* planar, size_t frames){
    if (!valid || finished || !planar) {
        return;
    }
    double x[CHANNELS_MAX];
    for (size_t i = 0; i < frames; i++) {
        for (unsigned int c = 0; c < channels; c++) {
            x[c] = finite(planar[c][i]);
        }
        frame(x, false);
    }
}

void AudioMeter::finish(){
    if (!valid || finished) {
        return;
    }
    finished = true;
    if (options.mode != AudioMeterMode::File) {
        return;
    }

    // the true-peak filter's tail, then the momentary windows' trailing silence
    for (unsigned int c = 0; c < channels; c++) {
        for (unsigned int i = 0; i < TruePeak::TAPS; i++) {
            truePeak = std::max(truePeak, (double)truePeakFilter.processSample(c, 0.0f));
        }
    }
    const double zeros[CHANNELS_MAX] = {};
    const uint64_t padding = (uint64_t)std::ceil(FILE_PADDING_SECONDS * sampleRate);
    for (uint64_t i = 0; i < padding; i++) {
        frame(zeros, true);
    }
}

double AudioMeter::windowEnergy(int64_t end, int64_t length) const{
    const double energy = energyBefore[(size_t)end] - energyBefore[(size_t)std::max<int64_t>(0, end - length)];
    const int64_t frames = boundary(end) - boundary(end - length);
    return frames > 0 ? energy / (double)frames : 0.0;
}

double AudioMeter::windowLoudness(int64_t end, int64_t length) const{
    return std::max(energyLoudness(windowEnergy(end, length)), SILENCE_DB);
}

LoudnessResult AudioMeter::result() const{
    LoudnessResult r;
    r.frames = signalFrames;
    r.channels = channels;
    r.sampleRate = sampleRate;
    r.samplePeakDb = amplitudeDb(peak);
    r.truePeakDb = amplitudeDb(truePeak);
    r.clipped = clipped;
    r.nonFinite = nonFinite;
    if (!valid) {
        return r;
    }

    // levels
    if (signalFrames > 0) {
        const double samples = (double)signalFrames * channels;
        double energy = 0.0;
        double subEnergy = 0.0;
        double bandEnergy[BANDS] = {};
        for (const Channel& channel : state) {
            energy += channel.energy;
            subEnergy += channel.subEnergy;
            for (int b = 0; b < BANDS; b++) {
                bandEnergy[b] += channel.bandEnergy[b];
            }
            r.dcOffset = std::max(r.dcOffset, std::fabs(channel.sum / (double)signalFrames));
        }
        r.meanSquare = energy / samples;
        r.rmsDb = meanSquareDb(r.meanSquare);
        if (options.bands) {
            for (int b = 0; b < BANDS; b++) {
                r.bandsMeanSquare[b] = bandEnergy[b] / samples;
                r.bandsDb[b] = meanSquareDb(r.bandsMeanSquare[b]);
            }
            r.subMeanSquare = subEnergy / samples;
            r.subShare = r.meanSquare > 0.0 ? std::min(1.0, r.subMeanSquare / r.meanSquare) : 0.0;
        }
    }

    const int64_t complete = completeSubBlocks();
    const int64_t signalEnd = signalSubBlocks();

    // Momentary: windows every 10 ms. A stream's last one ends by its last sample; a file's start
    // from -390 ms to 10 ms before its end, inside the padding.
    int64_t momentaryEnd = complete;
    if (options.mode == AudioMeterMode::File && finished) {
        const int64_t lastStart = floorDivide((int64_t)signalFrames * SUB_BLOCKS_PER_SECOND, sampleRate) - 1;
        momentaryEnd = std::min(complete, lastStart + MOMENTARY_SUB_BLOCKS);
    }
    for (int64_t end = 1; end <= momentaryEnd; end++) {
        const double value = windowLoudness(end, MOMENTARY_SUB_BLOCKS);
        r.momentaryMax = r.momentaryMax ? std::max(*r.momentaryMax, value) : value;
    }

    // integrated: 400 ms blocks every 100 ms inside the signal, gated at -70 LUFS and then 10 LU
    // under the mean of the blocks that passed
    double gatedSum = 0.0;
    size_t gatedCount = 0;
    for (int64_t end = MOMENTARY_SUB_BLOCKS; end <= signalEnd; end += HOP_SUB_BLOCKS) {
        const double energy = windowEnergy(end, MOMENTARY_SUB_BLOCKS);
        if (energyLoudness(energy) > ABSOLUTE_GATE_LUFS) {
            gatedSum += energy;
            gatedCount++;
        }
    }
    if (gatedCount > 0) {
        const double relative = energyLoudness(gatedSum / (double)gatedCount) + RELATIVE_GATE_LU;
        r.relativeGate = relative;
        double sum = 0.0;
        for (int64_t end = MOMENTARY_SUB_BLOCKS; end <= signalEnd; end += HOP_SUB_BLOCKS) {
            const double energy = windowEnergy(end, MOMENTARY_SUB_BLOCKS);
            const double lufs = energyLoudness(energy);
            if (lufs > ABSOLUTE_GATE_LUFS && lufs > relative) {
                sum += energy;
                r.gatedBlocks++;
            }
        }
        if (r.gatedBlocks > 0) {
            r.integrated = energyLoudness(sum / (double)r.gatedBlocks);
        }
    }

    // short-term: 3 s windows every 100 ms inside the signal; LRA from them (EBU Tech 3342)
    if (signalEnd >= SHORT_TERM_SUB_BLOCKS) {
        double sum = 0.0;
        size_t count = 0;
        for (int64_t end = SHORT_TERM_SUB_BLOCKS; end <= signalEnd; end += HOP_SUB_BLOCKS) {
            const double energy = windowEnergy(end, SHORT_TERM_SUB_BLOCKS);
            const double value = windowLoudness(end, SHORT_TERM_SUB_BLOCKS);
            r.shortTermMax = r.shortTermMax ? std::max(*r.shortTermMax, value) : value;
            if (energyLoudness(energy) > ABSOLUTE_GATE_LUFS) {
                sum += energy;
                count++;
            }
        }
        std::vector<double> gated;
        if (count > 0) {
            const double relative = energyLoudness(sum / (double)count) + LRA_RELATIVE_GATE_LU;
            for (int64_t end = SHORT_TERM_SUB_BLOCKS; end <= signalEnd; end += HOP_SUB_BLOCKS) {
                const double lufs = energyLoudness(windowEnergy(end, SHORT_TERM_SUB_BLOCKS));
                if (lufs > ABSOLUTE_GATE_LUFS && lufs > relative) {
                    gated.push_back(lufs);
                }
            }
        }
        if (gated.size() < 2) {
            r.lra = 0.0;
        }else{
            std::sort(gated.begin(), gated.end());
            const size_t low = (size_t)((double)(gated.size() - 1) * 0.10 + 0.5);
            const size_t high = (size_t)((double)(gated.size() - 1) * 0.95 + 0.5);
            r.lra = gated[high] - gated[low];
        }
    }

    return r;
}

std::vector<float> AudioMeter::momentaryCurve() const{
    std::vector<float> curve;
    const int64_t signalEnd = signalSubBlocks();
    for (int64_t end = HOP_SUB_BLOCKS; end <= signalEnd; end += HOP_SUB_BLOCKS) {
        curve.push_back((float)windowLoudness(end, MOMENTARY_SUB_BLOCKS));
    }
    return curve;
}

std::vector<float> AudioMeter::shortTermCurve() const{
    std::vector<float> curve;
    const int64_t signalEnd = signalSubBlocks();
    for (int64_t end = HOP_SUB_BLOCKS; end <= signalEnd; end += HOP_SUB_BLOCKS) {
        curve.push_back(end >= SHORT_TERM_SUB_BLOCKS ? (float)windowLoudness(end, SHORT_TERM_SUB_BLOCKS)
                                                     : std::numeric_limits<float>::quiet_NaN());
    }
    return curve;
}

LoudnessResult AudioMeter::measure(const float* interleaved, size_t frames, unsigned int sampleRate,
                                   unsigned int channels, bool bands){
    AudioMeter meter(sampleRate, channels, AudioMeterOptions{AudioMeterMode::File, bands});
    meter.process(interleaved, frames);
    meter.finish();
    return meter.result();
}

LoudnessResult AudioMeter::measurePlanar(const float* const* planar, size_t frames, unsigned int sampleRate,
                                         unsigned int channels, bool bands){
    AudioMeter meter(sampleRate, channels, AudioMeterOptions{AudioMeterMode::File, bands});
    meter.processPlanar(planar, frames);
    meter.finish();
    return meter.result();
}
