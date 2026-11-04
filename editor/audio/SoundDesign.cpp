// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "SoundDesign.h"

#include "SoundSynth.h"
#include "SoundTypes.h"
#include "audio/AudioImport.h"

#include <algorithm>
#include <cmath>
#include <map>

using namespace jasperengine;
using namespace jasperengine::editor;

namespace {

const double PI = 3.14159265358979323846;
const double RATE = SoundDesign::SAMPLE_RATE;
const double MAX_HZ = 0.45 * RATE;
// amplitude falls 60 dB over t60
const double T60_LN = 6.908;
const double DEFAULT_ATTACK_MS = 0.2;
const double FAMILY_ROOM_MS = 12.0;

enum class FilterKind { Lowpass, Highpass, Bandpass, Peaking };

// direct form I, RBJ coefficients in double
struct Filter {
    double b0 = 1.0, b1 = 0.0, b2 = 0.0, a1 = 0.0, a2 = 0.0;
    double x1 = 0.0, x2 = 0.0, y1 = 0.0, y2 = 0.0;

    void set(FilterKind kind, double hz, double q, double db = 0.0) {
        const double w = 2.0 * PI * std::min(hz, MAX_HZ) / RATE;
        const double cosw = std::cos(w);
        const double alpha = std::sin(w) / (2.0 * q);
        double a0 = 1.0 + alpha;
        switch (kind) {
            case FilterKind::Lowpass:
                b0 = (1.0 - cosw) / 2.0; b1 = 1.0 - cosw; b2 = b0;
                a1 = -2.0 * cosw; a2 = 1.0 - alpha;
                break;
            case FilterKind::Highpass:
                b0 = (1.0 + cosw) / 2.0; b1 = -(1.0 + cosw); b2 = b0;
                a1 = -2.0 * cosw; a2 = 1.0 - alpha;
                break;
            case FilterKind::Bandpass:
                b0 = alpha; b1 = 0.0; b2 = -alpha;
                a1 = -2.0 * cosw; a2 = 1.0 - alpha;
                break;
            case FilterKind::Peaking: {
                const double A = std::pow(10.0, db / 40.0);
                b0 = 1.0 + alpha * A; b1 = -2.0 * cosw; b2 = 1.0 - alpha * A;
                a0 = 1.0 + alpha / A; a1 = -2.0 * cosw; a2 = 1.0 - alpha / A;
                break;
            }
        }
        b0 /= a0; b1 /= a0; b2 /= a0; a1 /= a0; a2 /= a0;
    }

    double process(double x) {
        const double y = b0 * x + b1 * x1 + b2 * x2 - a1 * y1 - a2 * y2;
        x2 = x1; x1 = x; y2 = y1; y1 = y;
        return y;
    }
};

enum class LayerKind { Noise, Modal, Tone, Sweep };

struct Layer {
    LayerKind kind = LayerKind::Noise;
    double gainDb = 0.0;
    double delay = 0.0;
    double attack = DEFAULT_ATTACK_MS;
    double t60 = 0.0;
    // noise
    FilterKind filter = FilterKind::Bandpass;
    double hz = 0.0;
    double q = 0.707;
    // modal
    std::vector<double> ratios;
    std::vector<double> gainsDb;
    double t60Exponent = 1.0;
    // tone: hz is from, toHz the end; sweep: hz is low, toHz high
    double toHz = 0.0;
    double glide = 0.0;
    double length = 0.0;
};

struct Chain {
    double highpassHz = 0.0;
    double lowpassHz = 0.0;
    double dipHz = 0.0;
    double dipQ = 1.0;
    double dipDb = 0.0;
    double drive = 0.0;
    double roomMs = 0.0;
    double roomMix = 0.0;
    double gateDb = 0.0;  // 0: none
};

struct Family {
    const char* name;
    std::vector<Layer> layers;
    Chain chain;
};

Layer noise(FilterKind filter, double hz, double q, double t60, double gainDb = 0.0, double delay = 0.0, double attack = DEFAULT_ATTACK_MS) {
    Layer l;
    l.kind = LayerKind::Noise;
    l.filter = filter;
    l.hz = hz;
    l.q = q;
    l.t60 = t60;
    l.gainDb = gainDb;
    l.delay = delay;
    l.attack = attack;
    return l;
}

Layer modal(double hz, std::vector<double> ratios, std::vector<double> gains, double t60, double exponent, double gainDb = 0.0) {
    Layer l;
    l.kind = LayerKind::Modal;
    l.hz = hz;
    l.ratios = std::move(ratios);
    l.gainsDb = std::move(gains);
    l.t60 = t60;
    l.t60Exponent = exponent;
    l.gainDb = gainDb;
    return l;
}

Layer tone(double from, double to, double glide, double t60, double gainDb = 0.0, double attack = DEFAULT_ATTACK_MS) {
    Layer l;
    l.kind = LayerKind::Tone;
    l.hz = from;
    l.toHz = to;
    l.glide = glide;
    l.t60 = t60;
    l.gainDb = gainDb;
    l.attack = attack;
    return l;
}

Layer sweep(double low, double high, double q, double length) {
    Layer l;
    l.kind = LayerKind::Sweep;
    l.hz = low;
    l.toHz = high;
    l.q = q;
    l.length = length;
    return l;
}

Chain chainOf(double highpass, double lowpass = 0.0, double dipHz = 0.0, double dipQ = 1.0, double dipDb = 0.0, double drive = 0.0, double roomMs = 0.0,
              double roomMix = 0.0, double gateDb = 0.0) {
    Chain c;
    c.highpassHz = highpass;
    c.lowpassHz = lowpass;
    c.dipHz = dipHz;
    c.dipQ = dipQ;
    c.dipDb = dipDb;
    c.drive = drive;
    c.roomMs = roomMs;
    c.roomMix = roomMix;
    c.gateDb = gateDb;
    return c;
}

const std::map<std::string, std::vector<Family>>& designs() {
    using F = FilterKind;
    static const std::map<std::string, std::vector<Family>> table = {
        {"ui_click", {
            {"tink", {noise(F::Highpass, 5000, 0.7, 6), modal(3400, {1, 2.76}, {0, -8}, 40, 1, -6)}, chainOf(500, 0, 3500, 2, -3, 0, 0, 0, -50)},
            {"snap", {noise(F::Bandpass, 2200, 1.2, 10), tone(1600, 1100, 6, 25, -10)}, chainOf(300, 0, 0, 1, 0, 0.3, 0, 0, -50)},
            {"mouse", {noise(F::Bandpass, 1300, 2, 8), noise(F::Bandpass, 1800, 2, 6, -6, 7)}, chainOf(250, 7000, 0, 1, 0, 0, 0, 0, -50)},
            {"glass", {modal(5200, {1, 2.32, 4.25}, {0, -4, -10}, 120, 0.5), noise(F::Highpass, 7000, 0.7, 3, -10)}, chainOf(800, 0, 0, 1, 0, 0, 8, 0.08, -55)},
            {"soft", {noise(F::Lowpass, 2500, 0.7, 18, 0, 0, 0.5), tone(700, 650, 10, 40, -8)}, chainOf(150, 6000, 0, 1, 0, 0, 0, 0, -50)},
        }},
        {"swoosh", {
            {"fast", {sweep(500, 3800, 0.9, 260), noise(F::Highpass, 5000, 0.7, 160, -14, 40, 120)}, chainOf(120, 0, 6000, 1.5, -3)},
            {"big", {sweep(180, 2400, 0.7, 900), tone(90, 70, 600, 900, -12, 300)}, chainOf(40, 0, 0, 1, 0, 0, 35, 0.15)},
            {"thin", {sweep(1800, 7500, 2.0, 420)}, chainOf(800, 0, 4500, 2, -4)},
        }},
        {"impact", {
            {"wood", {modal(210, {1, 2.57, 4.12, 6.3}, {0, -3, -8, -14}, 160, 1), noise(F::Bandpass, 1600, 1, 25, -4), tone(75, 48, 30, 120, -6)},
             chainOf(40, 0, 2500, 1.5, -2, 0, 10, 0.06, -55)},
            {"metal", {modal(420, {1, 2.76, 5.40, 8.93}, {0, -2, -6, -10}, 900, 0.6), noise(F::Highpass, 3000, 0.7, 12, -6)},
             chainOf(80, 0, 3200, 3, -4, 0, 0, 0, -60)},
            {"stone", {noise(F::Lowpass, 3200, 0.7, 70), modal(150, {1, 2.3}, {0, -6}, 80, 1, -4), tone(70, 45, 25, 90, -6)},
             chainOf(35, 0, 0, 1, 0, 0.2, 0, 0, -55)},
            {"soft", {noise(F::Lowpass, 1200, 0.7, 60, 0, 0, 1), tone(95, 62, 40, 160, -3)}, chainOf(35, 5000, 0, 1, 0, 0, 0, 0, -55)},
            {"heavy", {tone(62, 34, 60, 550), noise(F::Lowpass, 900, 0.7, 220, -6), noise(F::Bandpass, 2500, 1, 30, -12)},
             chainOf(25, 0, 0, 1, 0, 0.5, 50, 0.12, -60)},
        }},
        {"pop", {
            {"bubble", {tone(280, 950, 25, 45, 0, 0.5)}, chainOf(150, 0, 0, 1, 0, 0, 0, 0, -50)},
            {"cork", {noise(F::Bandpass, 900, 3, 12), tone(520, 330, 15, 35, -4)}, chainOf(200, 0, 0, 1, 0, 0, 0, 0, -50)},
            {"soft", {tone(600, 420, 30, 60, 0, 1), noise(F::Lowpass, 2000, 0.7, 10, -14)}, chainOf(120, 4000, 0, 1, 0, 0, 0, 0, -50)},
        }},
    };
    return table;
}

double dbToGain(double db) {
    return std::pow(10.0, db / 20.0);
}

size_t framesOfMs(double ms) {
    return (size_t)std::llround(ms * RATE / 1000.0);
}

double numberParam(const SoundDesign::Json& params, const char* key, double fallback) {
    if (!params.is_object()) return fallback;
    const auto it = params.find(key);
    return it != params.end() && it->is_number() ? it->get<double>() : fallback;
}

// the envelope at frame i of a layer that starts at 0: a raised-cosine attack, then its decay
double envelopeAt(size_t i, double attackFrames, double t60Frames) {
    const double t = (double)i;
    if (t < attackFrames) return 0.5 * (1.0 - std::cos(PI * t / attackFrames));
    return std::exp(-T60_LN * (t - attackFrames) / std::max(1.0, t60Frames));
}

void addLayer(const Layer& layer, SoundSynth::Random& random, std::vector<double>& out) {
    const size_t start = framesOfMs(layer.delay);
    const double gain = dbToGain(layer.gainDb);
    const double attackFrames = std::max(1.0, layer.attack * RATE / 1000.0);
    switch (layer.kind) {
        case LayerKind::Noise: {
            Filter filter;
            filter.set(layer.filter, layer.hz, layer.q);
            const double t60Frames = layer.t60 * RATE / 1000.0;
            const size_t frames = (size_t)std::ceil(t60Frames + attackFrames);
            for (size_t i = 0; i < frames && start + i < out.size(); i++) {
                out[start + i] += gain * filter.process(random.bipolar()) * envelopeAt(i, attackFrames, t60Frames);
            }
            break;
        }
        case LayerKind::Modal: {
            for (size_t k = 0; k < layer.ratios.size(); k++) {
                const double hz = layer.hz * layer.ratios[k];
                if (hz > MAX_HZ) continue;
                const double partial = gain * dbToGain(layer.gainsDb[k]);
                const double t60Frames = layer.t60 / std::pow(layer.ratios[k], layer.t60Exponent) * RATE / 1000.0;
                const size_t frames = (size_t)std::ceil(t60Frames + attackFrames);
                const double step = 2.0 * PI * hz / RATE;
                for (size_t i = 0; i < frames && start + i < out.size(); i++) {
                    out[start + i] += partial * std::sin(step * (double)i) * envelopeAt(i, attackFrames, t60Frames);
                }
            }
            break;
        }
        case LayerKind::Tone: {
            const double t60Frames = layer.t60 * RATE / 1000.0;
            const double glideFrames = std::max(1.0, layer.glide * RATE / 1000.0);
            const size_t frames = (size_t)std::ceil(t60Frames + attackFrames);
            double phase = 0.0;
            for (size_t i = 0; i < frames && start + i < out.size(); i++) {
                const double t = std::min(1.0, (double)i / glideFrames);
                const double hz = layer.hz * std::pow(layer.toHz / layer.hz, t);
                if (hz <= MAX_HZ) out[start + i] += gain * std::sin(phase) * envelopeAt(i, attackFrames, t60Frames);
                phase += 2.0 * PI * hz / RATE;
            }
            break;
        }
        case LayerKind::Sweep: {
            const size_t frames = framesOfMs(layer.length);
            Filter filter;
            for (size_t i = 0; i < frames && start + i < out.size(); i++) {
                const double t = (double)i / (double)std::max<size_t>(1, frames);
                filter.set(FilterKind::Bandpass, layer.hz * std::pow(layer.toHz / layer.hz, std::sin(PI * t)), layer.q);
                const double envelope = std::sin(PI * t);
                out[start + i] += gain * filter.process(random.bipolar()) * envelope * envelope;
            }
            break;
        }
    }
}

void applyRoom(std::vector<double>& x, double ms, double mix) {
    // four feedback combs, then two all-passes
    const double delays[4] = {ms, ms * 1.13, ms * 1.27, ms * 1.41};
    std::vector<double> wet(x.size(), 0.0);
    for (double delayMs : delays) {
        const size_t delay = std::max<size_t>(1, framesOfMs(delayMs));
        const double feedback = std::pow(10.0, -3.0 * delayMs / (4.0 * ms));
        std::vector<double> line(delay, 0.0);
        size_t at = 0;
        for (size_t i = 0; i < x.size(); i++) {
            const double out = line[at];
            line[at] = x[i] + out * feedback;
            wet[i] += out * 0.25;
            at = (at + 1) % delay;
        }
    }
    for (double delayMs : {5.0, 1.7}) {
        const size_t delay = framesOfMs(delayMs);
        const double gain = 0.7;
        std::vector<double> line(delay, 0.0);
        size_t at = 0;
        for (size_t i = 0; i < wet.size(); i++) {
            const double delayed = line[at];
            const double in = wet[i] + delayed * gain;
            line[at] = in;
            wet[i] = delayed - in * gain;
            at = (at + 1) % delay;
        }
    }
    for (size_t i = 0; i < x.size(); i++) x[i] = x[i] * (1.0 - mix) + wet[i] * mix;
}

// the last 5 ms window at or above gateDb re the loudest ends the sound, with a 10 ms fade
void applyGate(std::vector<double>& x, double gateDb) {
    const size_t window = framesOfMs(5.0);
    if (x.size() < window) return;
    const size_t windows = x.size() / window;
    std::vector<double> energy(windows, 0.0);
    double loudest = 0.0;
    for (size_t w = 0; w < windows; w++) {
        for (size_t i = 0; i < window; i++) energy[w] += x[w * window + i] * x[w * window + i];
        loudest = std::max(loudest, energy[w]);
    }
    if (loudest <= 0.0) return;
    const double threshold = loudest * std::pow(10.0, gateDb / 10.0);
    size_t last = 0;
    for (size_t w = 0; w < windows; w++) {
        if (energy[w] >= threshold) last = w;
    }
    const size_t end = std::min(x.size(), (last + 1) * window);
    x.resize(end);
    const uint32_t fade = (uint32_t)std::min(end, framesOfMs(10.0));
    for (uint32_t k = 0; k < fade; k++) x[end - fade + k] *= clipFadeWeight(k, fade, false);
}

} // namespace

bool SoundDesign::isLayered(const std::string& preset) {
    return designs().count(preset) > 0;
}

std::vector<std::string> SoundDesign::families(const std::string& preset) {
    std::vector<std::string> out;
    const auto it = designs().find(preset);
    if (it != designs().end()) {
        for (const Family& family : it->second) out.push_back(family.name);
    }
    return out;
}

bool SoundDesign::render(const std::string& preset, const std::string& familyName, uint32_t seed, const Json& params, std::vector<float>& samples, std::string& error) {
    const auto design = designs().find(preset);
    if (design == designs().end()) {
        error = "family applies to ui_click, swoosh, impact and pop";
        return false;
    }
    const Family* family = familyName.empty() ? &design->second.front() : nullptr;
    std::string list;
    for (const Family& f : design->second) {
        if (f.name == familyName) family = &f;
        list += (list.empty() ? "" : ", ") + std::string(f.name);
    }
    if (!family) {
        error = "unknown family '" + familyName + "' for " + preset + ": " + list;
        return false;
    }

    // the draws, in this order, before any sample: what a seed varies
    SoundSynth::Random random(seed);
    const double drawnForce = random.bipolar();
    const double timing = random.bipolar();
    const double pitch = random.bipolar();
    const double force = numberParam(params, "force", drawnForce);

    const double pitchSt = numberParam(params, "pitch_st", 0.0);
    const double lengthScale = numberParam(params, "length", 1.0);
    const double brightness = numberParam(params, "brightness", 0.0);
    const double roomParam = numberParam(params, "room", 0.0);
    const double driveParam = numberParam(params, "drive", 0.0);

    const double pitchFactor = std::pow(2.0, (SoundTypes::DESIGN_PITCH_ST * pitch + pitchSt) / 12.0);
    const double forceFactor = std::pow(2.0, SoundTypes::DESIGN_FORCE_OCTAVES * force);
    const double brightFactor = std::pow(2.0, brightness);
    const double timeFactor = (1.0 + SoundTypes::DESIGN_TIMING * timing) * lengthScale;

    std::vector<Layer> layers = family->layers;
    double latest = 0.0;
    for (Layer& l : layers) {
        l.t60 *= timeFactor;
        l.glide *= timeFactor;
        l.length *= timeFactor;
        l.delay = std::max(0.0, l.delay + SoundTypes::DESIGN_DELAY_MS * timing) * lengthScale;
        l.hz *= pitchFactor;
        l.toHz *= pitchFactor;
        if (l.kind == LayerKind::Noise) {
            l.hz *= forceFactor * brightFactor;
            l.gainDb += SoundTypes::DESIGN_FORCE_DB * force + 3.0 * brightness;
        }
        if (l.kind == LayerKind::Sweep) {
            l.hz *= brightFactor;
            l.toHz *= brightFactor;
        }
        latest = std::max(latest, l.delay + (l.kind == LayerKind::Sweep ? l.length : l.t60));
    }
    Chain chain = family->chain;
    chain.highpassHz *= pitchFactor * brightFactor;
    chain.lowpassHz *= pitchFactor * brightFactor;
    chain.dipHz *= pitchFactor * brightFactor * forceFactor;
    chain.drive = std::clamp(chain.drive + 0.5 * driveParam, 0.0, 1.0);
    chain.roomMix = std::clamp(chain.roomMix + 0.25 * roomParam, 0.0, 0.5);
    if (chain.roomMix > 0.0 && chain.roomMs <= 0.0) chain.roomMs = FAMILY_ROOM_MS;
    chain.roomMs *= timeFactor;

    const double totalMs = latest + (chain.roomMix > 0.0 ? 4.0 * chain.roomMs : 0.0);
    std::vector<double> sum((size_t)std::min(std::ceil(totalMs * RATE / 1000.0) + 1.0, SoundSynth::MAX_SECONDS * RATE), 0.0);
    for (const Layer& layer : layers) addLayer(layer, random, sum);

    auto filterAll = [&sum](FilterKind kind, double hz, double q, double db) {
        Filter filter;
        filter.set(kind, hz, q, db);
        for (double& x : sum) x = filter.process(x);
    };
    if (chain.highpassHz > 0.0) filterAll(FilterKind::Highpass, chain.highpassHz, 0.707, 0.0);
    if (chain.lowpassHz > 0.0) filterAll(FilterKind::Lowpass, chain.lowpassHz, 0.707, 0.0);
    if (chain.dipHz > 0.0) filterAll(FilterKind::Peaking, chain.dipHz, chain.dipQ, chain.dipDb);
    if (chain.drive > 0.0) {
        double peak = 0.0;
        for (double x : sum) peak = std::max(peak, std::fabs(x));
        if (peak > 0.0) {
            const double g = 1.0 + 3.0 * chain.drive;
            for (double& x : sum) x = std::tanh(g * x / peak) / std::tanh(g);
        }
    }
    if (chain.roomMix > 0.0) applyRoom(sum, chain.roomMs, chain.roomMix);
    if (chain.gateDb < 0.0) applyGate(sum, chain.gateDb);

    samples.assign(sum.begin(), sum.end());
    return true;
}
