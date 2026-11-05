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

void AudioBiquad::processRun(double* samples, size_t count){
    // the state in locals: nothing the compiler has to write back on each sample
    const double cb0 = b0, cb1 = b1, cb2 = b2, ca1 = a1, ca2 = a2;
    double s1 = z1, s2 = z2;
    for (size_t i = 0; i < count; i++) {
        const double x = samples[i];
        const double y = cb0 * x + s1;
        s1 = cb1 * x - ca1 * y + s2;
        s2 = cb2 * x - ca2 * y;
        samples[i] = y;
    }
    z1 = s1;
    z2 = s2;
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

void AudioKWeighting::processRun(double* samples, size_t count){
    shelf.processRun(samples, count);
    highPass.processRun(samples, count);
}

// the same biquad, run over two channels in step
static void processBiquadPair(AudioBiquad& a, AudioBiquad& b, double* x0, double* x1, size_t count){
    const double cb0 = a.b0, cb1 = a.b1, cb2 = a.b2, ca1 = a.a1, ca2 = a.a2;
    double s1[2] = {a.z1, b.z1};
    double s2[2] = {a.z2, b.z2};
    for (size_t i = 0; i < count; i++) {
        const double x[2] = {x0[i], x1[i]};
        double y[2];
        for (int c = 0; c < 2; c++) {
            y[c] = cb0 * x[c] + s1[c];
            s1[c] = cb1 * x[c] - ca1 * y[c] + s2[c];
            s2[c] = cb2 * x[c] - ca2 * y[c];
        }
        x0[i] = y[0];
        x1[i] = y[1];
    }
    a.z1 = s1[0];
    b.z1 = s1[1];
    a.z2 = s2[0];
    b.z2 = s2[1];
}

void AudioKWeighting::processRuns(AudioKWeighting& other, double* samples, double* otherSamples, size_t count){
    processBiquadPair(shelf, other.shelf, samples, otherSamples, count);
    processBiquadPair(highPass, other.highPass, samples, otherSamples, count);
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

// -- AudioSpectrumBands ----------------------------------------------------------------------------

void AudioSpectrumBands::init(unsigned int rate, unsigned int channelCount){
    sampleRate = rate;
    channels = channelCount;
    // about 3 Hz a bin, whatever the rate: the 31.5 Hz band is 22 Hz wide
    size = 1024;
    while (size < rate / 3) {
        size *= 2;
    }
    overlap = size / 8;
    input.assign(size * channels, 0.0f);
    // The first window's taper holds silence. A taper's square and the next window's add up to 1.
    fill = overlap;
    pending = 0;
    windows = 0;
    window.assign(size, 1.0f);
    for (size_t i = 0; i < overlap; i++) {
        const float taper = (float)std::sin(METER_PI * ((double)i + 0.5) / (double)(2 * overlap));
        window[i] = taper;
        window[size - 1 - i] = taper;
    }
    twiddleRe.resize(size / 2);
    twiddleIm.resize(size / 2);
    for (size_t i = 0; i < size / 2; i++) {
        const double angle = -2.0 * METER_PI * (double)i / (double)size;
        twiddleRe[i] = (float)std::cos(angle);
        twiddleIm[i] = (float)std::sin(angle);
    }
    re.resize(size);
    im.resize(size);
    workRe.resize(size);
    workIm.resize(size);
    power.assign(size, 0.0);
}

void AudioSpectrumBands::transformWindows(){
    windows++;
    // two channels in one complex transform: |Z(k)|^2 + |Z(n - k)|^2 is twice their powers at k, and
    // the bands read a frequency from both bins, so nothing has to separate them
    for (unsigned int c = 0; c < channels; c += 2) {
        const float* first = input.data() + (size_t)c * size;
        const float* second = c + 1 < channels ? first + size : nullptr;
        for (size_t i = 0; i < size; i++) {
            re[i] = first[i] * window[i];
            im[i] = second ? second[i] * window[i] : 0.0f;
        }

        // Stockham's radix 2, no bit reversal: the twiddle only depends on the outer loop, so that the
        // inner one vectorizes in the stages where it is long
        float* xr = re.data();
        float* xi = im.data();
        float* yr = workRe.data();
        float* yi = workIm.data();
        size_t stride = 1;
        for (size_t n = size; n > 1; n /= 2) {
            const size_t half = n / 2;
            for (size_t p = 0; p < half; p++) {
                const float wr = twiddleRe[p * stride];
                const float wi = twiddleIm[p * stride];
                const float* ar = xr + stride * p;
                const float* ai = xi + stride * p;
                const float* br = xr + stride * (p + half);
                const float* bi = xi + stride * (p + half);
                float* cr = yr + stride * 2 * p;
                float* ci = yi + stride * 2 * p;
                float* dr = yr + stride * (2 * p + 1);
                float* di = yi + stride * (2 * p + 1);
                for (size_t q = 0; q < stride; q++) {
                    const float sumR = ar[q] + br[q];
                    const float sumI = ai[q] + bi[q];
                    const float diffR = ar[q] - br[q];
                    const float diffI = ai[q] - bi[q];
                    cr[q] = sumR;
                    ci[q] = sumI;
                    dr[q] = diffR * wr - diffI * wi;
                    di[q] = diffR * wi + diffI * wr;
                }
            }
            std::swap(xr, yr);
            std::swap(xi, yi);
            stride *= 2;
        }
        for (size_t k = 0; k < size; k++) {
            power[k] += (double)xr[k] * xr[k] + (double)xi[k] * xi[k];
        }
    }
}

void AudioSpectrumBands::process(const float* planar, size_t stride, size_t frames){
    size_t position = 0;
    while (position < frames) {
        const size_t count = std::min(frames - position, size - fill);
        for (unsigned int c = 0; c < channels; c++) {
            std::copy(planar + c * stride + position, planar + c * stride + position + count, input.data() + c * size + fill);
        }
        fill += count;
        position += count;
        pending += count;
        if (fill == size) {
            transformWindows();
            advanceWindow();
            pending = 0;
        }
    }
}

void AudioSpectrumBands::advanceWindow(){
    for (unsigned int c = 0; c < channels; c++) {
        float* line = input.data() + c * size;
        std::copy(line + size - overlap, line + size, line);
    }
    fill = overlap;
}

void AudioSpectrumBands::finish(){
    // silence up to the end of this window, and the next one for what reached the taper: every sample
    // goes through all the windows it is in
    const int remaining = pending > 0 ? 2 : 1;
    for (int i = 0; i < remaining; i++) {
        for (unsigned int c = 0; c < channels; c++) {
            std::fill(input.data() + c * size + fill, input.data() + (size_t)(c + 1) * size, 0.0f);
        }
        transformWindows();
        advanceWindow();
    }
    pending = 0;
}

uint64_t AudioSpectrumBands::windowedFrames() const{
    return windows * (uint64_t)(size - overlap);
}

static double eighthPower(double x){
    const double square = x * x;
    const double fourth = square * square;
    return fourth * fourth;
}

void AudioSpectrumBands::energy(double out[BANDS + 1]) const{
    const double rate = (double)sampleRate;
    const auto warped = [rate](double hz) { return std::tan(METER_PI * hz / rate); };
    const auto bandEnergy = [&](double low, double high) {
        // as AudioOctaveBank::initBand: empty from the limit, no low-pass from it. 4th-order Butterworth
        // edges through the bilinear transform, the magnitudes of its biquads.
        if (low >= BAND_EDGE_LIMIT * rate) {
            return 0.0;
        }
        const double lowEdge = warped(low);
        const bool hasHigh = high < BAND_EDGE_LIMIT * rate;
        const double highEdge = hasHigh ? warped(high) : 0.0;
        double sum = 0.0;
        for (size_t k = 1; k < size; k++) {
            const size_t bin = std::min(k, size - k);
            if (bin * 2 == size) {
                // the Nyquist bin: the high-pass passes it, a low-pass closes it
                sum += hasHigh ? 0.0 : power[k];
                continue;
            }
            const double at = warped(rate * (double)bin / (double)size);
            double attenuation = 1.0 + eighthPower(lowEdge / at);
            if (hasHigh) {
                attenuation *= 1.0 + eighthPower(at / highEdge);
            }
            sum += power[k] / attenuation;
        }
        return sum / (double)size;
    };
    for (int b = 0; b < BANDS; b++) {
        const double centre = AudioMeter::bandCentre(b);
        out[b] = bandEnergy(centre / std::sqrt(2.0), centre * std::sqrt(2.0));
    }
    out[BANDS] = bandEnergy(SUB_LOW_HZ, SUB_HIGH_HZ);
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
    }
    if (options.bands) {
        spectrum.init(sampleRate, channels);
    }
    truePeakFilter = TruePeak(channels);
    run.assign((size_t)channels * RUN_FRAMES, 0.0f);
    weightedRun.assign((size_t)channels * RUN_FRAMES, 0.0);

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

size_t AudioMeter::runFramesNow(size_t frames) const{
    return std::min<size_t>(std::min<size_t>(frames, RUN_FRAMES), (size_t)(subBlockEnd - processed));
}

void AudioMeter::processRun(size_t frames, bool padding){
    for (unsigned int c = 0; c < channels; c++) {
        Channel& channel = state[c];
        const float* samples = run.data() + (size_t)c * RUN_FRAMES;
        double* weighted = weightedRun.data() + (size_t)c * RUN_FRAMES;
        for (size_t i = 0; i < frames; i++) {
            weighted[i] = samples[i];
        }

        if (!padding) {
            double sum[4] = {};
            double squares[4] = {};
            size_t i = 0;
            // in four lanes, which vectorize; one running sum waits for its last result on every sample
            for (; i + 4 <= frames; i += 4) {
                for (int lane = 0; lane < 4; lane++) {
                    sum[lane] += weighted[i + lane];
                    squares[lane] += weighted[i + lane] * weighted[i + lane];
                }
            }
            for (; i < frames; i++) {
                sum[0] += weighted[i];
                squares[0] += weighted[i] * weighted[i];
            }
            uint32_t over = 0;
            for (size_t k = 0; k < frames; k++) {
                over += std::fabs(samples[k]) >= (float)CLIP_LEVEL ? 1u : 0u;
            }
            clipped += over;
            channel.sum += sum[0] + sum[1] + sum[2] + sum[3];
            channel.energy += squares[0] + squares[1] + squares[2] + squares[3];
            float largest = (float)peak;
            for (size_t k = 0; k < frames; k++) {
                if (std::fabs(samples[k]) > largest) {
                    largest = std::fabs(samples[k]);
                }
            }
            peak = largest;
            truePeak = std::max(truePeak, std::max((double)largest, (double)truePeakFilter.processChannel(c, samples, frames, (float)truePeak)));
        }
    }

    // K-weighting: in pairs of channels, a channel that does not count (an LFE) left out
    unsigned int previous = channels;
    for (unsigned int c = 0; c < channels; c++) {
        if (state[c].weight == 0.0) continue;
        if (previous == channels) {
            previous = c;
            continue;
        }
        state[previous].kWeighting.processRuns(state[c].kWeighting, weightedRun.data() + (size_t)previous * RUN_FRAMES,
                                               weightedRun.data() + (size_t)c * RUN_FRAMES, frames);
        previous = channels;
    }
    if (previous != channels) {
        state[previous].kWeighting.processRun(weightedRun.data() + (size_t)previous * RUN_FRAMES, frames);
    }

    double energy = 0.0;
    for (unsigned int c = 0; c < channels; c++) {
        if (state[c].weight == 0.0) continue;
        const double* weighted = weightedRun.data() + (size_t)c * RUN_FRAMES;
        double squares = 0.0;
        for (size_t i = 0; i < frames; i++) {
            squares += weighted[i] * weighted[i];
        }
        energy += state[c].weight * squares;
    }

    subBlockSum += energy;
    processed += frames;
    if (!padding) {
        signalFrames += frames;
        if (options.bands) {
            spectrum.process(run.data(), RUN_FRAMES, frames);
        }
    }
    while (processed >= subBlockEnd) {
        energyBefore.push_back(energyBefore.back() + subBlockSum);
        subBlockSum = 0.0;
        subBlockEnd = (uint64_t)boundary(completeSubBlocks() + 1);
        for (Channel& channel : state) {
            channel.kWeighting.settle();
        }
    }
}

void AudioMeter::process(const float* interleaved, size_t frames){
    if (!valid || finished || !interleaved) {
        return;
    }
    while (frames > 0) {
        const size_t count = runFramesNow(frames);
        for (unsigned int c = 0; c < channels; c++) {
            float* out = run.data() + (size_t)c * RUN_FRAMES;
            for (size_t i = 0; i < count; i++) {
                const float x = interleaved[i * channels + c];
                if (std::isfinite(x)) {
                    out[i] = x;
                } else {
                    out[i] = 0.0f;
                    nonFinite++;
                }
            }
        }
        processRun(count, false);
        interleaved += count * channels;
        frames -= count;
    }
}

void AudioMeter::processPlanar(const float* const* planar, size_t frames){
    if (!valid || finished || !planar) {
        return;
    }
    size_t position = 0;
    while (position < frames) {
        const size_t count = runFramesNow(frames - position);
        for (unsigned int c = 0; c < channels; c++) {
            float* out = run.data() + (size_t)c * RUN_FRAMES;
            const float* in = planar[c] + position;
            for (size_t i = 0; i < count; i++) {
                if (std::isfinite(in[i])) {
                    out[i] = in[i];
                } else {
                    out[i] = 0.0f;
                    nonFinite++;
                }
            }
        }
        processRun(count, false);
        position += count;
    }
}

void AudioMeter::finish(){
    if (!valid || finished) {
        return;
    }
    finished = true;
    if (options.bands) {
        spectrum.finish();
    }
    if (options.mode != AudioMeterMode::File) {
        return;
    }

    // the true-peak filter's tail, then the momentary windows' trailing silence
    for (unsigned int c = 0; c < channels; c++) {
        for (unsigned int i = 0; i < TruePeak::TAPS; i++) {
            truePeak = std::max(truePeak, (double)truePeakFilter.processSample(c, 0.0f));
        }
    }
    std::fill(run.begin(), run.end(), 0.0f);
    uint64_t padding = (uint64_t)std::ceil(FILE_PADDING_SECONDS * sampleRate);
    while (padding > 0) {
        const size_t count = runFramesNow((size_t)padding);
        processRun(count, true);
        padding -= count;
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
        for (const Channel& channel : state) {
            energy += channel.energy;
            r.dcOffset = std::max(r.dcOffset, std::fabs(channel.sum / (double)signalFrames));
        }
        r.meanSquare = energy / samples;
        r.rmsDb = meanSquareDb(r.meanSquare);
        // before finish() the energy is that of the windows done so far
        const double windowed = finished ? samples : (double)spectrum.windowedFrames() * channels;
        if (options.bands && windowed > 0.0) {
            double bandEnergy[BANDS + 1];
            spectrum.energy(bandEnergy);
            for (int b = 0; b < BANDS; b++) {
                r.bandsMeanSquare[b] = bandEnergy[b] / windowed;
                r.bandsDb[b] = meanSquareDb(r.bandsMeanSquare[b]);
            }
            r.subMeanSquare = bandEnergy[BANDS] / windowed;
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

std::optional<double> AudioMeter::momentaryNow() const{
    const int64_t end = signalSubBlocks();
    return end > 0 ? std::optional<double>(windowLoudness(end, MOMENTARY_SUB_BLOCKS)) : std::nullopt;
}

std::optional<double> AudioMeter::shortTermNow() const{
    const int64_t end = signalSubBlocks();
    return end >= SHORT_TERM_SUB_BLOCKS ? std::optional<double>(windowLoudness(end, SHORT_TERM_SUB_BLOCKS)) : std::nullopt;
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
