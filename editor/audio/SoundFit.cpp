// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "SoundFit.h"

#include "SoundDesign.h"
#include "SoundTypes.h"

#include "soloud_fft.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

using namespace jasperengine;
using namespace jasperengine::editor;

namespace {

const double PI = 3.14159265358979323846;
const double FLOOR_DB = -60.0;
const double ENVELOPE_FRAME_SECONDS = 0.005;
const double ENVELOPE_RMS_SECONDS = 0.001;
const double BAND_EDGE = std::pow(10.0, 1.0 / 20.0);
// the longest band a file can hold: above this share of its rate, a band is out of it
const double RATE_SHARE = 0.45;
const double COMPARE_LENGTH = 1.25;
const int HALTON_BASES[5] = {2, 3, 5, 7, 11};
const double FIRST_STEP = 0.25;
const double LAST_STEP = 1.0 / 64.0;
const double COMPARE_ROUNDING = 1e-9;

double halton(int index, int base) {
    double out = 0.0;
    double f = 1.0;
    for (int i = index; i > 0; i /= base) {
        f /= base;
        out += f * (i % base);
    }
    return out;
}

double dbOfPower(double power) {
    return power > 0.0 ? 10.0 * std::log10(power) : -300.0;
}

double attackOf(const std::vector<float>& x, double rate) {
    // the 1 ms RMS, centred; 10% to 90% before its maximum (analyze_audio's attack_ms on a mono signal)
    const long half = std::max(0L, (long)std::lround(ENVELOPE_RMS_SECONDS * rate) / 2);
    const long n = (long)x.size();
    std::vector<double> envelope((size_t)n);
    double sum = 0.0;
    long count = 0;
    for (long k = 0; k < std::min(n, half + 1); k++) {
        sum += (double)x[(size_t)k] * x[(size_t)k];
        count++;
    }
    double largest = 0.0;
    long at = 0;
    for (long i = 0; i < n; i++) {
        const double e = std::sqrt(std::max(0.0, sum) / (double)std::max(1L, count));
        envelope[(size_t)i] = e;
        if (e > largest) {
            largest = e;
            at = i;
        }
        const long enter = i + half + 1;
        const long leave = i - half;
        if (enter < n) {
            sum += (double)x[(size_t)enter] * x[(size_t)enter];
            count++;
        }
        if (leave >= 0) {
            sum -= (double)x[(size_t)leave] * x[(size_t)leave];
            count--;
        }
    }
    if (largest <= 0.0) return 0.0;
    long low = -1, high = -1;
    for (long i = 0; i <= at; i++) {
        if (low < 0 && envelope[(size_t)i] >= 0.1 * largest) low = i;
        if (high < 0 && envelope[(size_t)i] >= 0.9 * largest) high = i;
    }
    return low >= 0 && high >= low ? (double)(high - low) / rate * 1000.0 : 0.0;
}

struct Dimension {
    std::string name;
    double min;
    double max;
    bool logarithmic;
};

std::vector<Dimension> dimensionsOf(const SoundSynth::Preset& preset) {
    std::vector<Dimension> out;
    const bool layered = SoundDesign::isLayered(preset.name);
    for (const SoundSynth::Param& param : SoundSynth::paramsOf(preset)) {
        if (layered && std::string(param.name) == "force") continue;
        out.push_back({param.name, param.min, param.max, param.min > 0.0 && param.max / param.min >= 10.0});
    }
    return out;
}

double toUnit(const Dimension& d, double value) {
    const double t = d.logarithmic ? std::log(value / d.min) / std::log(d.max / d.min) : (value - d.min) / (d.max - d.min);
    return std::clamp(t, 0.0, 1.0);
}

double fromUnit(const Dimension& d, double t) {
    return d.logarithmic ? d.min * std::pow(d.max / d.min, t) : d.min + (d.max - d.min) * t;
}

double rounded(double distance) {
    return std::round(distance / COMPARE_ROUNDING) * COMPARE_ROUNDING;
}

std::string hzWords(double hz) {
    char text[32];
    if (hz >= 1000.0) std::snprintf(text, sizeof(text), "%.3g kHz", hz / 1000.0);
    else std::snprintf(text, sizeof(text), "%.0f Hz", hz);
    return text;
}

} // namespace

double SoundFit::bandCentre(int band) {
    return 1000.0 * std::pow(10.0, (band - 13) / 10.0);
}

std::array<double, SoundFit::BANDS> SoundFit::bandLevels(const Measure& measure, unsigned int otherRate) {
    std::array<double, BANDS> levels;
    const double top = RATE_SHARE * std::min(measure.rate, otherRate);
    double total = 0.0;
    for (int b = 0; b < BANDS; b++) {
        if (bandCentre(b) * BAND_EDGE <= top) total += measure.bandPower[(size_t)b];
    }
    for (int b = 0; b < BANDS; b++) {
        const bool held = bandCentre(b) * BAND_EDGE <= top;
        levels[(size_t)b] = held && total > 0.0 ? std::max(FLOOR_DB, dbOfPower(measure.bandPower[(size_t)b] / total)) : FLOOR_DB;
    }
    return levels;
}

SoundFit::Measure SoundFit::measure(const AudioAnalysis::Signal& signal) {
    Measure out;
    out.rate = signal.sampleRate;
    const std::vector<float> mono = signal.mono();
    float peak = 0.0f;
    for (float x : mono) peak = std::max(peak, std::fabs(x));
    if (peak <= 0.0f || signal.sampleRate == 0) return out;

    const float threshold = peak * (float)std::pow(10.0, FLOOR_DB / 20.0);
    size_t head = 0;
    while (head < mono.size() && std::fabs(mono[head]) < threshold) head++;
    size_t tail = mono.size();
    while (tail > head && std::fabs(mono[tail - 1]) < threshold) tail--;
    if (tail <= head) return out;
    out.silent = false;
    const double rate = signal.sampleRate;
    out.activeSeconds = (double)(tail - head) / rate;
    const std::vector<float> active(mono.begin() + (long)head, mono.begin() + (long)tail);
    out.attackMs = attackOf(active, rate);

    // -- band balance: Hann frames of the active region, zero-padded past its end
    static const std::vector<float> window = []() {
        std::vector<float> w(AudioAnalysis::FFT_SIZE);
        for (int i = 0; i < AudioAnalysis::FFT_SIZE; i++) w[(size_t)i] = (float)(0.5 - 0.5 * std::cos(2.0 * PI * i / AudioAnalysis::FFT_SIZE));
        return w;
    }();
    const int bins = AudioAnalysis::FFT_SIZE / 2;
    std::vector<float> buffer((size_t)(2 * AudioAnalysis::FFT_SIZE));
    std::vector<double> spectrum((size_t)bins + 1, 0.0);
    for (size_t start = 0; start < active.size(); start += AudioAnalysis::FFT_HOP) {
        for (int i = 0; i < AudioAnalysis::FFT_SIZE; i++) {
            const size_t at = start + (size_t)i;
            buffer[(size_t)(2 * i)] = at < active.size() ? active[at] * window[(size_t)i] : 0.0f;
            buffer[(size_t)(2 * i + 1)] = 0.0f;
        }
        SoLoud::FFT::fft(buffer.data(), 2 * AudioAnalysis::FFT_SIZE);
        for (int k = 0; k <= bins; k++) {
            const double re = buffer[(size_t)(2 * k)];
            const double im = buffer[(size_t)(2 * k + 1)];
            spectrum[(size_t)k] += re * re + im * im;
        }
    }
    const double binHz = rate / AudioAnalysis::FFT_SIZE;
    for (int b = 0; b < BANDS; b++) {
        const double low = bandCentre(b) / BAND_EDGE;
        const double high = bandCentre(b) * BAND_EDGE;
        for (int k = 1; k <= bins; k++) {
            const double hz = k * binHz;
            if (hz >= low && hz < high) out.bandPower[(size_t)b] += spectrum[(size_t)k];
        }
    }

    // -- envelope: from the head to the signal's end, so a tail under the floor reads as it is
    const size_t frame = std::max<size_t>(1, (size_t)std::lround(ENVELOPE_FRAME_SECONDS * rate));
    double loudest = 0.0;
    std::vector<double> power;
    for (size_t start = head; start < mono.size(); start += frame) {
        const size_t end = std::min(mono.size(), start + frame);
        double sum = 0.0;
        for (size_t i = start; i < end; i++) sum += (double)mono[i] * mono[i];
        power.push_back(sum / (double)frame);
        loudest = std::max(loudest, power.back());
    }
    out.envelopeDb.reserve(power.size());
    for (double p : power) out.envelopeDb.push_back(loudest > 0.0 ? std::max(FLOOR_DB, dbOfPower(p / loudest)) : FLOOR_DB);
    return out;
}

SoundFit::Quality SoundFit::compare(const Measure& reference, const Measure& candidate) {
    Quality out;
    if (candidate.silent || reference.silent) {
        out.distance = 1e9;
        out.grade = "rough";
        return out;
    }
    const auto lr = bandLevels(reference, candidate.rate);
    const auto lc = bandLevels(candidate, reference.rate);
    double sum = 0.0;
    int count = 0;
    for (int b = 0; b < BANDS; b++) {
        if (std::max(lr[(size_t)b], lc[(size_t)b]) >= SoundTypes::FIT_BAND_FLOOR_DB) {
            const double d = lc[(size_t)b] - lr[(size_t)b];
            sum += d * d;
            count++;
        }
    }
    out.bandDb = count > 0 ? std::sqrt(sum / count) : 0.0;

    const size_t frames = (size_t)std::ceil(COMPARE_LENGTH * reference.activeSeconds / ENVELOPE_FRAME_SECONDS);
    sum = 0.0;
    count = 0;
    for (size_t k = 0; k < frames; k++) {
        const double er = k < reference.envelopeDb.size() ? reference.envelopeDb[k] : FLOOR_DB;
        const double ec = k < candidate.envelopeDb.size() ? candidate.envelopeDb[k] : FLOOR_DB;
        if (std::max(er, ec) >= SoundTypes::FIT_BAND_FLOOR_DB) {
            sum += (ec - er) * (ec - er);
            count++;
        }
    }
    out.envelopeDb = count > 0 ? std::sqrt(sum / count) : 0.0;
    out.onsetMs = std::fabs(candidate.attackMs - reference.attackMs);
    out.lengthRatio = reference.activeSeconds > 0.0 ? candidate.activeSeconds / reference.activeSeconds : 1.0;
    out.distance = out.bandDb / SoundTypes::FIT_BAND_SCALE_DB + out.envelopeDb / SoundTypes::FIT_ENVELOPE_SCALE_DB +
                   SoundTypes::FIT_LENGTH_SCALE * std::fabs(std::log2(std::max(out.lengthRatio, 1e-6))) +
                   std::fabs(std::log2((candidate.attackMs + 1.0) / (reference.attackMs + 1.0)));
    out.grade = gradeOf(out.distance);
    return out;
}

std::string SoundFit::gradeOf(double distance) {
    if (distance <= SoundTypes::FIT_CLOSE) return "close";
    if (distance <= SoundTypes::FIT_SIMILAR) return "similar";
    return "rough";
}

std::string SoundFit::bandWords(const Measure& reference, const Measure& candidate) {
    const auto lr = bandLevels(reference, candidate.rate);
    const auto lc = bandLevels(candidate, reference.rate);
    int first = -1, second = -1;
    auto gap = [&](int b) { return std::fabs(lc[(size_t)b] - lr[(size_t)b]); };
    for (int b = 0; b < BANDS; b++) {
        if (std::max(lr[(size_t)b], lc[(size_t)b]) < SoundTypes::FIT_BAND_FLOOR_DB) continue;
        if (first < 0 || gap(b) > gap(first)) {
            second = first;
            first = b;
        } else if (second < 0 || gap(b) > gap(second)) {
            second = b;
        }
    }
    std::string out;
    for (int b : {first, second}) {
        if (b < 0) continue;
        char text[48];
        std::snprintf(text, sizeof(text), "%+.1f dB at ", lc[(size_t)b] - lr[(size_t)b]);
        out += (out.empty() ? "" : ", ") + std::string(text) + hzWords(bandCentre(b));
    }
    return out;
}

bool SoundFit::canFit(const SoundSynth::Preset* preset, std::string& error) {
    if (preset && !preset->isSfxr()) return true;
    error = "like fits a recipe or a layered preset: click, pluck, bell, whoosh, riser, thump, ui_click, swoosh, impact, pop.";
    return false;
}

bool SoundFit::fit(const Measure& reference, const SoundSynth::Preset& preset, const std::string& family, uint32_t seed, Result& result,
                   std::string& error, const std::function<void(int, double)>& progress, const std::atomic<bool>* cancel) {
    if (!canFit(&preset, error)) return false;
    const std::vector<Dimension> dimensions = dimensionsOf(preset);
    std::vector<double> defaults;
    {
        size_t d = 0;
        for (const SoundSynth::Param& param : SoundSynth::paramsOf(preset)) {
            if (d < dimensions.size() && dimensions[d].name == param.name) defaults.push_back(toUnit(dimensions[d++], param.defaultValue));
        }
    }

    int renders = 0;
    double best = INFINITY;
    std::vector<double> bestPoint = defaults;
    std::string bestFamily = family;
    bool cancelled = false;

    // A render at a point: true when it is the best so far. A render that fails ends the fit.
    auto tryPoint = [&](const std::vector<double>& point, const std::string& withFamily) {
        Json params = Json::object();
        for (size_t d = 0; d < dimensions.size(); d++) params[dimensions[d].name] = fromUnit(dimensions[d], point[d]);
        AudioAnalysis::Signal signal;
        unsigned int rate = 0;
        std::vector<float> samples;
        if (!SoundSynth::render(preset, seed, params, samples, rate, error, withFamily)) return false;
        signal.channels = 1;
        signal.sampleRate = rate;
        signal.frames = samples.size();
        signal.samples = std::move(samples);
        const Measure measured = measure(signal);
        const Quality quality = compare(reference, measured);
        if (renders == 0) result.startDistance = quality.distance;
        renders++;
        if (rounded(quality.distance) < rounded(best)) {
            best = quality.distance;
            bestPoint = point;
            bestFamily = withFamily;
            result.params = params;
            result.family = withFamily;
            result.quality = quality;
            result.measure = measured;
        }
        if (progress) progress(renders, best);
        cancelled = cancel && cancel->load();
        return true;
    };

    // -- stage A: each family at the defaults, then the Halton points
    std::vector<std::string> families;
    if (SoundDesign::isLayered(preset.name)) {
        if (family.empty()) families = SoundDesign::families(preset.name);
        else families = {family};
    } else {
        families = {std::string()};
    }
    for (const std::string& name : families) {
        if (renders >= SoundTypes::FIT_STAGE_A_RENDERS || cancelled) break;
        if (!tryPoint(defaults, name)) return false;
    }
    const std::string stageFamily = bestFamily;
    for (int index = 1; renders < SoundTypes::FIT_STAGE_A_RENDERS && !cancelled; index++) {
        std::vector<double> point(dimensions.size());
        for (size_t d = 0; d < dimensions.size(); d++) point[d] = halton(index, HALTON_BASES[d]);
        if (!tryPoint(point, stageFamily)) return false;
    }

    // -- stage B: a compass search from the best point
    double step = FIRST_STEP;
    while (renders < SoundTypes::FIT_RENDERS && step >= LAST_STEP && !cancelled) {
        bool improved = false;
        for (size_t d = 0; d < dimensions.size() && renders < SoundTypes::FIT_RENDERS && !cancelled; d++) {
            for (double sign : {1.0, -1.0}) {
                if (renders >= SoundTypes::FIT_RENDERS || cancelled) break;
                std::vector<double> point = bestPoint;
                point[d] = std::clamp(point[d] + sign * step, 0.0, 1.0);
                if (point[d] == bestPoint[d]) continue;
                const double before = best;
                if (!tryPoint(point, bestFamily)) return false;
                if (rounded(best) < rounded(before)) {
                    improved = true;
                    break;
                }
            }
        }
        if (!improved) step /= 2.0;
    }
    if (cancelled) {
        error = "cancelled";
        return false;
    }
    result.renders = renders;
    return true;
}
