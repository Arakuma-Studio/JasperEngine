// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "SoundSynth.h"

#include "SoundDesign.h"

#include "AudioAnalysis.h"
#include "SoundTypes.h"
#include "WavWriter.h"
#include "audio/AudioImport.h"
#include "audio/AudioMeter.h"

#include "soloud_sfxr.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <memory>

using namespace jasperengine;
using namespace jasperengine::editor;

namespace {

using Json = SoundSynth::Json;
using Param = SoundSynth::Param;
using Kind = SoundSynth::Param::Kind;

const double PI = 3.14159265358979323846;

// the two biquads of a 4th-order Butterworth
const double BUTTERWORTH_Q1 = 0.5411961001461970;
const double BUTTERWORTH_Q2 = 1.3065629648763766;

// the pitch of thump falls from freq_hz + drop_hz with this time constant
const double THUMP_PITCH_TIME = 0.050;
const double THUMP_CLICK_SECONDS = 0.005;
// every render, sfxr included, fades in (raised cosine) over at most this long: 24 frames at 48 kHz,
// 22 at 44.1 kHz
const double FADE_IN_SECONDS = 0.0005;
// A render whose attack is in its first frames (click starts at its loudest and decays within a
// few ms) gets the longest fade-in, of at least SoundTypes::FADE_IN_MIN_FRAMES, that takes at most
// FADE_IN_MAX_LOSS_DB of the energy of its first ATTACK_WINDOW_SECONDS (the momentary window that
// holds the attack; AudioAnalysis::safeFadeInFrames, which the analysis' notes propose too): a
// 0.5 ms ramp took up to 2.2 dB of a click. A sound whose energy is spread out keeps the whole
// 0.5 ms. When even 3 frames take more, the render keeps every frame and starts 2 frames early
// instead, on the lead-in 0, half its first frame: the same 2-frame ramp, without taking the
// frames the attack is made of.
// and fades out (raised cosine) over 10% of its length, at least 1 frame, so its first and last
// samples are exactly 0: no render clicks where it starts or stops. A recipe's fade-out is at most
// this long (Phase 1's); an sfxr render, whose own envelope ends it already, fades out over at most
// the click-safe length of its type's kind (SoundTypes::clickSafeFadeOutMs: 5 ms for a transient,
// 12 ms for a tonal sound): a 50 ms ramp took up to 0.34 dB of a sustained sfxr tail, and over the
// step noise of an explosion's quiet end it rounded stretches to digital silence between steps
const double FADE_MAX_SECONDS = 0.050;
// below this the render is silence: no gain can make a sound of it
const double SILENT_PEAK_DBFS = -90.0;

// RBJ biquads (transposed direct form II)
struct Biquad {
    double b0 = 1.0, b1 = 0.0, b2 = 0.0, a1 = 0.0, a2 = 0.0;
    double z1 = 0.0, z2 = 0.0;

    void set(double c0, double c1, double c2, double d0, double d1, double d2) {
        b0 = c0 / d0;
        b1 = c1 / d0;
        b2 = c2 / d0;
        a1 = d1 / d0;
        a2 = d2 / d0;
    }

    void highPass(double freq, double q, double rate) {
        const double w = 2.0 * PI * freq / rate, c = std::cos(w), alpha = std::sin(w) / (2.0 * q);
        set((1.0 + c) / 2.0, -(1.0 + c), (1.0 + c) / 2.0, 1.0 + alpha, -2.0 * c, 1.0 - alpha);
    }

    // constant 0 dB peak gain
    void bandPass(double freq, double q, double rate) {
        const double w = 2.0 * PI * freq / rate, c = std::cos(w), alpha = std::sin(w) / (2.0 * q);
        set(alpha, 0.0, -alpha, 1.0 + alpha, -2.0 * c, 1.0 - alpha);
    }

    double process(double x) {
        const double y = b0 * x + z1;
        z1 = b1 * x - a1 * y + z2;
        z2 = b2 * x - a2 * y;
        return y;
    }
};

double clampFrequency(double freq, double rate) {
    return std::clamp(freq, 20.0, 0.45 * rate);
}

size_t framesOf(double milliseconds, double rate) {
    return std::max<size_t>(1, (size_t)std::llround(milliseconds * rate / 1000.0));
}

// The ramps are the weights of AudioClipSource's click-safe fades (clipFadeWeight, phase 2.5 spec
// 2.5.1): frame 0 of the fade-in is 0, and so is the last frame of the fade-out. A render shorter
// than both gets both (they multiply).

// The fade-in's frames, or 0 for the lead-in (AudioAnalysis::safeFadeInFrames)
size_t fadeInFrames(const std::vector<float>& samples, double rate) {
    const size_t longest = std::min(samples.size(), std::max<size_t>(2, (size_t)std::llround(FADE_IN_SECONDS * rate)));
    std::vector<double> squares(std::min(samples.size(), (size_t)std::llround(SoundTypes::ATTACK_WINDOW_SECONDS * rate)));
    for (size_t k = 0; k < squares.size(); k++) squares[k] = (double)samples[k] * (double)samples[k];
    return AudioAnalysis::safeFadeInFrames(squares, rate, longest);
}

void fadeIn(std::vector<float>& samples, size_t fade) {
    for (size_t k = 0; k < fade; k++) {
        samples[k] *= clipFadeWeight((uint32_t)k, (uint32_t)fade, true);
    }
}

// the 2 frames of a 2-frame ramp up to the first frame, before it
void leadIn(std::vector<float>& samples) {
    const float first = samples.front();
    samples.insert(samples.begin(), {first * clipFadeWeight(0, 2, true), first * clipFadeWeight(1, 2, true)});
}

void fadeOut(std::vector<float>& samples, double rate, double maxSeconds) {
    if (samples.empty()) return;
    const size_t share = std::max<size_t>(1, (size_t)std::llround(0.1 * samples.size()));
    const size_t longest = std::max<size_t>(1, (size_t)std::llround(maxSeconds * rate));
    const size_t fade = std::min({samples.size(), share, longest});
    const size_t start = samples.size() - fade;
    for (size_t j = 0; j < fade; j++) {
        samples[start + j] *= clipFadeWeight((uint32_t)j, (uint32_t)fade, false);
    }
}

// a band-limited sawtooth (polyBLEP), t the phase in [0, 1), dt the phase step
double polyBlep(double t, double dt) {
    if (t < dt) {
        t /= dt;
        return t + t - t * t - 1.0;
    }
    if (t > 1.0 - dt) {
        t = (t - 1.0) / dt;
        return t * t + t + t + 1.0;
    }
    return 0.0;
}

double paramOf(const Json& params, const char* name, double fallback) {
    if (params.is_object() && params.contains(name) && params[name].is_number()) return params[name].get<double>();
    return fallback;
}

// -- recipes (48 kHz mono); `jitter` scales their frequencies

void click(const Json& p, SoundSynth::Random& random, double jitter, double rate, std::vector<float>& out) {
    const double freq = clampFrequency(paramOf(p, "freq_hz", 3000.0) * jitter, rate);
    const double decay = paramOf(p, "decay_ms", 1.5) / 1000.0;
    const size_t frames = framesOf(paramOf(p, "length_ms", 30.0), rate);
    Biquad first, second;
    first.highPass(freq, BUTTERWORTH_Q1, rate);
    second.highPass(freq, BUTTERWORTH_Q2, rate);
    out.resize(frames);
    for (size_t i = 0; i < frames; i++) {
        const double t = (double)i / rate;
        out[i] = (float)(second.process(first.process(random.bipolar())) * std::exp(-t / decay));
    }
}

void pluck(const Json& p, SoundSynth::Random& random, double jitter, double rate, std::vector<float>& out) {
    const double freq = clampFrequency(paramOf(p, "freq_hz", 440.0) * jitter, rate);
    const double damping = paramOf(p, "damping", 0.996);
    const double brightness = paramOf(p, "brightness", 0.5);
    const size_t frames = framesOf(paramOf(p, "length_ms", 600.0), rate);
    const double period = rate / freq;
    // the loop's two-point average delays by half a sample
    const double delay = std::max(1.0, period - 0.5);

    // excitation: one period of noise through a one-pole low-pass (brightness 1: unfiltered), DC removed
    const size_t burst = std::min(frames, (size_t)std::max(1.0, std::round(period)));
    std::vector<double> excitation(burst);
    const double coefficient = 0.05 + 0.95 * brightness;
    double state = 0.0, mean = 0.0;
    for (double& x : excitation) {
        state += coefficient * (random.bipolar() - state);
        x = state;
        mean += state;
    }
    mean /= (double)burst;
    for (double& x : excitation) x -= mean;

    std::vector<double> y(frames, 0.0);
    auto past = [&](size_t i, double d) {
        // y[i - d] with linear interpolation, 0 before the start
        const double position = (double)i - d;
        if (position < 0.0) return 0.0;
        const size_t index = (size_t)position;
        const double frac = position - (double)index;
        const double a = y[index];
        const double b = index + 1 < i ? y[index + 1] : 0.0;
        return a + (b - a) * frac;
    };
    for (size_t i = 0; i < frames; i++) {
        const double feedback = damping * 0.5 * (past(i, delay) + past(i, delay + 1.0));
        y[i] = (i < burst ? excitation[i] : 0.0) + feedback;
    }
    out.resize(frames);
    for (size_t i = 0; i < frames; i++) out[i] = (float)y[i];
}

void bell(const Json& p, SoundSynth::Random& random, double jitter, double rate, std::vector<float>& out) {
    static const double RATIOS[] = {1.0, 2.0, 2.76, 5.40, 8.93};
    const double freq = paramOf(p, "freq_hz", 880.0) * jitter;
    const double brightness = paramOf(p, "brightness", 0.5);
    const size_t frames = framesOf(paramOf(p, "length_ms", 1500.0), rate);
    std::vector<double> sum(frames, 0.0);
    for (double ratio : RATIOS) {
        // a slight per-seed detune of each partial
        const double f = freq * ratio * (1.0 + 0.002 * random.bipolar());
        if (f >= 0.45 * rate) continue;
        const double amplitude = std::pow(ratio, -2.0 * (1.0 - brightness));
        const double tau = 1.2 / std::sqrt(ratio);
        const double step = 2.0 * PI * f / rate;
        for (size_t i = 0; i < frames; i++) {
            const double t = (double)i / rate;
            sum[i] += amplitude * std::exp(-t / tau) * std::sin(step * (double)i);
        }
    }
    out.resize(frames);
    for (size_t i = 0; i < frames; i++) out[i] = (float)sum[i];
}

void whoosh(const Json& p, SoundSynth::Random& random, double jitter, double rate, std::vector<float>& out) {
    const double low = clampFrequency(paramOf(p, "low_hz", 400.0) * jitter, rate);
    const double high = clampFrequency(paramOf(p, "high_hz", 2500.0) * jitter, rate);
    const double q = paramOf(p, "q", 1.0);
    const size_t frames = framesOf(paramOf(p, "length_ms", 500.0), rate);
    Biquad filter;
    out.resize(frames);
    for (size_t i = 0; i < frames; i++) {
        const double phase = PI * (double)i / (double)frames;
        if (i % 16 == 0) {
            // low -> high -> low, exponentially
            filter.bandPass(clampFrequency(low * std::pow(high / low, std::sin(phase)), rate), q, rate);
        }
        const double envelope = std::sin(phase) * std::sin(phase);
        out[i] = (float)(filter.process(random.bipolar()) * envelope);
    }
}

void riser(const Json& p, SoundSynth::Random& random, double jitter, double rate, std::vector<float>& out) {
    const double start = paramOf(p, "start_hz", 200.0) * jitter;
    const double end = paramOf(p, "end_hz", 2000.0) * jitter;
    const double noise = paramOf(p, "noise", 0.4);
    const size_t frames = framesOf(paramOf(p, "length_ms", 2000.0), rate);
    const double peak = 0.95 * (double)frames;
    double phase = 0.0;
    out.resize(frames);
    for (size_t i = 0; i < frames; i++) {
        const double share = std::min(1.0, (double)i / peak);
        const double freq = clampFrequency(start * std::pow(end / start, share), rate);
        const double dt = freq / rate;
        const double saw = 2.0 * phase - 1.0 - polyBlep(phase, dt);
        phase += dt;
        if (phase >= 1.0) phase -= 1.0;
        const double envelope = (double)i < peak ? (double)i / peak : std::max(0.0, 1.0 - ((double)i - peak) / ((double)frames - peak));
        out[i] = (float)((saw + noise * random.bipolar()) * envelope);
    }
}

void thump(const Json& p, SoundSynth::Random& random, double jitter, double rate, std::vector<float>& out) {
    const double freq = paramOf(p, "freq_hz", 55.0) * jitter;
    const double drop = paramOf(p, "drop_hz", 70.0) * jitter;
    const double decay = paramOf(p, "decay_ms", 150.0) / 1000.0;
    const double clickLevel = paramOf(p, "click", 0.3);
    const size_t frames = framesOf(paramOf(p, "length_ms", 400.0), rate);
    double phase = 0.0;
    out.resize(frames);
    for (size_t i = 0; i < frames; i++) {
        const double t = (double)i / rate;
        const double f = clampFrequency(freq + drop * std::exp(-t / THUMP_PITCH_TIME), rate);
        double x = std::sin(phase) * std::exp(-t / decay);
        phase += 2.0 * PI * f / rate;
        if (t < THUMP_CLICK_SECONDS) x += clickLevel * random.bipolar() * (1.0 - t / THUMP_CLICK_SECONDS);
        out[i] = (float)x;
    }
}

// -- sfxr fields

struct SfxrField {
    Param param;
    float SoLoud::SfxrParams::*number = nullptr;
    int SoLoud::SfxrParams::*integer = nullptr;
    bool SoLoud::SfxrParams::*boolean = nullptr;
};

const std::vector<SfxrField>& sfxrFields() {
    using P = SoLoud::SfxrParams;
    auto number = [](const char* name, double min, double max, double fallback, float P::*field) {
        SfxrField f;
        f.param = {name, min, max, fallback, Kind::Number};
        f.number = field;
        return f;
    };
    static const std::vector<SfxrField> fields = [&]() {
        std::vector<SfxrField> list;
        SfxrField wave;
        wave.param = {"wave_type", 0.0, 3.0, 0.0, Kind::Integer};
        wave.integer = &P::wave_type;
        list.push_back(wave);
        list.push_back(number("p_base_freq", 0, 1, 0.3, &P::p_base_freq));
        list.push_back(number("p_freq_limit", 0, 1, 0.0, &P::p_freq_limit));
        list.push_back(number("p_freq_ramp", -1, 1, 0.0, &P::p_freq_ramp));
        list.push_back(number("p_freq_dramp", -1, 1, 0.0, &P::p_freq_dramp));
        list.push_back(number("p_duty", 0, 1, 0.0, &P::p_duty));
        list.push_back(number("p_duty_ramp", -1, 1, 0.0, &P::p_duty_ramp));
        list.push_back(number("p_vib_strength", 0, 1, 0.0, &P::p_vib_strength));
        list.push_back(number("p_vib_speed", 0, 1, 0.0, &P::p_vib_speed));
        list.push_back(number("p_vib_delay", 0, 1, 0.0, &P::p_vib_delay));
        list.push_back(number("p_env_attack", 0, 1, 0.0, &P::p_env_attack));
        list.push_back(number("p_env_sustain", 0, 1, 0.3, &P::p_env_sustain));
        list.push_back(number("p_env_decay", 0, 1, 0.4, &P::p_env_decay));
        list.push_back(number("p_env_punch", 0, 1, 0.0, &P::p_env_punch));
        SfxrField filter;
        filter.param = {"filter_on", 0.0, 1.0, 0.0, Kind::Boolean};
        filter.boolean = &P::filter_on;
        list.push_back(filter);
        list.push_back(number("p_lpf_resonance", 0, 1, 0.0, &P::p_lpf_resonance));
        list.push_back(number("p_lpf_freq", 0, 1, 1.0, &P::p_lpf_freq));
        list.push_back(number("p_lpf_ramp", -1, 1, 0.0, &P::p_lpf_ramp));
        list.push_back(number("p_hpf_freq", 0, 1, 0.0, &P::p_hpf_freq));
        list.push_back(number("p_hpf_ramp", -1, 1, 0.0, &P::p_hpf_ramp));
        list.push_back(number("p_pha_offset", -1, 1, 0.0, &P::p_pha_offset));
        list.push_back(number("p_pha_ramp", -1, 1, 0.0, &P::p_pha_ramp));
        list.push_back(number("p_repeat_speed", 0, 1, 0.0, &P::p_repeat_speed));
        list.push_back(number("p_arp_speed", 0, 1, 0.0, &P::p_arp_speed));
        list.push_back(number("p_arp_mod", -1, 1, 0.0, &P::p_arp_mod));
        list.push_back(number("master_vol", 0, 1, 0.05, &P::master_vol));
        list.push_back(number("sound_vol", 0, 1, 0.5, &P::sound_vol));
        return list;
    }();
    return fields;
}

std::string formatValue(double value) {
    char text[32];
    std::snprintf(text, sizeof(text), "%g", value);
    return text;
}

std::string paramList(const std::vector<Param>& params) {
    std::string out;
    for (const Param& p : params) {
        if (!out.empty()) out += ", ";
        out += p.name;
        if (p.kind == Kind::Boolean) out += " (true/false)";
        else out += " (" + formatValue(p.min) + " to " + formatValue(p.max) + ")";
    }
    return out;
}

bool renderSfxr(const SoundSynth::Preset& preset, uint32_t seed, const Json& params, std::vector<float>& samples,
                std::string& error) {
    SoLoud::Sfxr sfxr;
    if (sfxr.loadPreset(preset.sfxrPreset, (int)seed) != SoLoud::SO_NO_ERROR) {
        error = std::string("sfxr refused preset ") + preset.name;
        return false;
    }
    if (params.is_object()) {
        for (const SfxrField& field : sfxrFields()) {
            if (!params.contains(field.param.name)) continue;
            const Json& value = params[field.param.name];
            if (field.number) sfxr.mParams.*field.number = value.get<float>();
            else if (field.integer) sfxr.mParams.*field.integer = (int)std::llround(value.get<double>());
            else if (field.boolean) sfxr.mParams.*field.boolean = value.is_boolean() ? value.get<bool>() : value.get<double>() != 0.0;
        }
    }
    std::unique_ptr<SoLoud::AudioSourceInstance> instance(sfxr.createInstance());
    const size_t cap = (size_t)(SoundSynth::MAX_SECONDS * SoundSynth::SFXR_RATE);
    const unsigned int chunk = 512;
    float buffer[chunk];
    samples.clear();
    while (samples.size() < cap && !instance->hasEnded()) {
        const unsigned int want = (unsigned int)std::min<size_t>(chunk, cap - samples.size());
        const unsigned int got = instance->getAudio(buffer, want, chunk);
        samples.insert(samples.end(), buffer, buffer + std::min(got, want));
        if (got < want) break;
    }
    if (samples.empty()) {
        error = std::string("sfxr rendered nothing for ") + preset.name + " with these params";
        return false;
    }
    return true;
}

} // namespace

// ======================================================================== Random

SoundSynth::Random::Random(uint32_t seed)
    : state(seed == 0 ? ZERO_SEED : seed) {
}

uint32_t SoundSynth::Random::next() {
    uint32_t x = state;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    state = x;
    return x;
}

double SoundSynth::Random::uniform() {
    return (double)next() / 4294967296.0;
}

double SoundSynth::Random::bipolar() {
    return 2.0 * uniform() - 1.0;
}

// ======================================================================== presets

const std::vector<SoundSynth::Preset>& SoundSynth::presets() {
    // the same six for every layered preset; force has no default (absent: the seed draws it)
    static const std::vector<Param> LAYERED_PARAMS = {
        {"pitch_st", -12, 12, 0}, {"length", 0.25, 4, 1}, {"brightness", -1, 1, 0}, {"room", -1, 1, 0}, {"drive", -1, 1, 0}, {"force", -1, 1, 0}};
    static const std::vector<Preset> list = {
        {"coin", "pickup", SoLoud::Sfxr::COIN, {}},
        {"laser", "shoot", SoLoud::Sfxr::LASER, {}},
        {"explosion", "explosion", SoLoud::Sfxr::EXPLOSION, {}},
        {"powerup", "pickup", SoLoud::Sfxr::POWERUP, {}},
        {"hurt", "hurt", SoLoud::Sfxr::HURT, {}},
        {"jump", "jump", SoLoud::Sfxr::JUMP, {}},
        {"blip", "ui", SoLoud::Sfxr::BLIP, {}},
        {"click", "click", -1, {
            {"freq_hz", 500, 12000, 3000}, {"decay_ms", 0.2, 20, 1.5}, {"length_ms", 5, 200, 30}}},
        {"pluck", "ui", -1, {
            {"freq_hz", 40, 2000, 440}, {"damping", 0.9, 0.9999, 0.996}, {"brightness", 0, 1, 0.5}, {"length_ms", 50, 4000, 600}}},
        {"bell", "chime", -1, {
            {"freq_hz", 100, 4000, 880}, {"brightness", 0, 1, 0.5}, {"length_ms", 100, 5000, 1500}}},
        {"whoosh", "whoosh", -1, {
            {"low_hz", 100, 2000, 400}, {"high_hz", 500, 8000, 2500}, {"q", 0.3, 4, 1}, {"length_ms", 100, 3000, 500}}},
        {"riser", "riser", -1, {
            {"start_hz", 50, 2000, 200}, {"end_hz", 200, 8000, 2000}, {"noise", 0, 1, 0.4}, {"length_ms", 300, 8000, 2000}}},
        {"thump", "thump", -1, {
            {"freq_hz", 30, 150, 55}, {"drop_hz", 0, 200, 70}, {"decay_ms", 20, 1000, 150}, {"click", 0, 1, 0.3},
            {"length_ms", 100, 2000, 400}}},
        {"ui_click", "click", -1, LAYERED_PARAMS},
        {"swoosh", "whoosh", -1, LAYERED_PARAMS},
        {"impact", "impact", -1, LAYERED_PARAMS},
        {"pop", "pop", -1, LAYERED_PARAMS},
    };
    return list;
}

const SoundSynth::Preset* SoundSynth::find(const std::string& name) {
    for (const Preset& preset : presets()) {
        if (name == preset.name) return &preset;
    }
    return nullptr;
}

std::vector<std::string> SoundSynth::names() {
    std::vector<std::string> out;
    for (const Preset& preset : presets()) out.push_back(preset.name);
    out.push_back(RANDOM);
    return out;
}

const std::vector<SoundSynth::Param>& SoundSynth::sfxrParams() {
    static const std::vector<Param> params = []() {
        std::vector<Param> list;
        for (const SfxrField& field : sfxrFields()) list.push_back(field.param);
        return list;
    }();
    return params;
}

const std::vector<SoundSynth::Param>& SoundSynth::paramsOf(const Preset& preset) {
    return preset.isSfxr() ? sfxrParams() : preset.params;
}

const SoundSynth::Preset& SoundSynth::pick(uint32_t seed) {
    Random random(seed);
    return presets()[random.next() % RANDOM_PRESETS];
}

bool SoundSynth::checkParams(const Preset& preset, const Json& params, std::string& error) {
    if (params.is_null()) return true;
    const std::vector<Param>& valid = paramsOf(preset);
    const std::string kind = preset.isSfxr() ? "sfxr field" : "parameter";
    if (!params.is_object()) {
        error = "params is an object of " + kind + "s: " + paramList(valid) + ".";
        return false;
    }
    for (auto it = params.begin(); it != params.end(); ++it) {
        const Param* param = nullptr;
        for (const Param& p : valid) {
            if (it.key() == p.name) param = &p;
        }
        if (!param) {
            error = "unknown " + kind + " \"" + it.key() + "\" for " + preset.name + "; valid: " + paramList(valid) + ".";
            return false;
        }
        const Json& value = it.value();
        if (param->kind == Kind::Boolean) {
            if (!value.is_boolean()) {
                error = std::string(param->name) + " is true or false.";
                return false;
            }
            continue;
        }
        if (!value.is_number() || !std::isfinite(value.get<double>())) {
            error = std::string(param->name) + " is a number from " + formatValue(param->min) + " to " + formatValue(param->max) + ".";
            return false;
        }
        const double v = value.get<double>();
        if (v < param->min || v > param->max) {
            error = std::string(param->name) + " " + formatValue(v) + " is out of range: " + formatValue(param->min) + " to " +
                    formatValue(param->max) + " (valid: " + paramList(valid) + ").";
            return false;
        }
        if (param->kind == Kind::Integer && v != std::floor(v)) {
            error = std::string(param->name) + " is a whole number from " + formatValue(param->min) + " to " + formatValue(param->max) + ".";
            return false;
        }
    }
    return true;
}

bool SoundSynth::render(const Preset& preset, uint32_t seed, const Json& params, std::vector<float>& samples,
                        unsigned int& sampleRate, std::string& error, const std::string& family) {
    if (preset.isSfxr()) {
        sampleRate = SFXR_RATE;
        if (!renderSfxr(preset, seed, params, samples, error)) return false;
    } else if (SoundDesign::isLayered(preset.name)) {
        sampleRate = SoundDesign::SAMPLE_RATE;
        if (!SoundDesign::render(preset.name, family, seed, params, samples, error)) return false;
    } else {
        sampleRate = RECIPE_RATE;
        Random random(seed);
        // the first draw sets the pitch: the seed moves a recipe up to +/-3%, so variations differ
        const double jitter = 1.0 + PITCH_JITTER * random.bipolar();
        const std::string name = preset.name;
        const double rate = RECIPE_RATE;
        if (name == "click") click(params, random, jitter, rate, samples);
        else if (name == "pluck") pluck(params, random, jitter, rate, samples);
        else if (name == "bell") bell(params, random, jitter, rate, samples);
        else if (name == "whoosh") whoosh(params, random, jitter, rate, samples);
        else if (name == "riser") riser(params, random, jitter, rate, samples);
        else if (name == "thump") thump(params, random, jitter, rate, samples);
        else {
            error = "no recipe for " + name;
            return false;
        }
    }
    // every render starts and ends at exactly 0, whatever its recipe or sfxr does there; the
    // fade-out is over the render's own frames (a lead-in comes after it), so a recipe's is Phase 1's
    const size_t fade = fadeInFrames(samples, sampleRate);
    fadeIn(samples, fade);
    double fadeOutSeconds = FADE_MAX_SECONDS;
    if (preset.isSfxr()) {
        const SoundTypes::SoundType* type = SoundTypes::find(preset.type);
        fadeOutSeconds = SoundTypes::clickSafeFadeOutMs(type ? type->kind : SoundTypes::TONAL) / 1000.0;
    }
    fadeOut(samples, sampleRate, fadeOutSeconds);
    if (fade == 0) leadIn(samples);
    return true;
}

bool SoundSynth::generate(const Preset& preset, uint32_t seed, const Json& params, double targetLufs, Output& out,
                          std::string& error, const std::string& family) {
    std::vector<float> samples;
    unsigned int rate = 0;
    if (!render(preset, seed, params, samples, rate, error, family)) return false;

    const LoudnessResult raw = AudioMeter::measure(samples.data(), samples.size(), rate, 1, false);
    if (!raw.momentaryMax || raw.samplePeakDb < SILENT_PEAK_DBFS || *raw.momentaryMax <= AudioMeter::SILENCE_DB) {
        error = std::string(preset.name) + " renders silence with these params: nothing to bring to a level.";
        return false;
    }

    double gain = targetLufs - *raw.momentaryMax;
    bool limited = false;
    if (raw.truePeakDb + gain > TRUE_PEAK_CEILING_DBTP) {
        gain = TRUE_PEAK_CEILING_DBTP - raw.truePeakDb;
        limited = true;
    }
    const float scale = (float)std::pow(10.0, gain / 20.0);
    for (float& x : samples) x *= scale;

    out.preset = &preset;
    out.seed = seed;
    out.targetLufs = targetLufs;
    out.gainDb = gain;
    out.limitedByTruePeak = limited;
    out.sampleRate = rate;
    out.frames = samples.size();
    out.wav = WavWriter::pcm16(samples, 1, rate);
    return true;
}
