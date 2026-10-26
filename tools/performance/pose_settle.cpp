// Differential checks and paired CPU timing of the actual production method.
#include "pose/PoseTypes.h"
#include "pose/PoseTween.h"

#include <array>
#include <cassert>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <ctime>
#include <limits>
#include <random>
#include <string>

using namespace jasperengine::pose;
namespace before { float run(const PoseAnimationSettings&, const Transition&); }
namespace after { float run(const PoseAnimationSettings&, const Transition&); }

static volatile float sink;

static void check(const PoseAnimationSettings& settings, const Transition& transition) {
    const float a = before::run(settings, transition);
    const float b = after::run(settings, transition);
    assert(a == b || (std::isnan(a) && std::isnan(b)));
}

static void correctness() {
    for (const auto& preset : feelPresets()) {
        PoseAnimationSettings settings;
        applyFeelPreset(preset, settings);
        check(settings, settings.transition);
        settings.looseness = 0;
        check(settings, settings.transition);
        settings.weights = {0.3f, 0.7f, 0.5f, 1.f};
        settings.looseness = 1;
        check(settings, settings.transition);
    }
    std::mt19937 random(314159);
    std::uniform_real_distribution<float> real(-0.5f, 1.5f);
    for (int trial = 0; trial < 512; ++trial) {
        PoseAnimationSettings settings;
        settings.looseness = real(random);
        settings.overlap = real(random);
        for (auto& weight : settings.weights) weight = real(random);
        Transition transition;
        transition.duration = real(random);
        transition.overshoot = real(random);
        transition.anticipation = real(random);
        transition.curve = static_cast<TransitionCurve>(trial % 4);
        check(settings, transition);
    }
    // Public settings can contain non-finite values; cache lookup must not change those results.
    for (float special : {0.f, -0.f, std::numeric_limits<float>::infinity(),
                          -std::numeric_limits<float>::infinity(), std::numeric_limits<float>::quiet_NaN()}) {
        PoseAnimationSettings settings;
        settings.looseness = special;
        settings.weights[0] = special;
        check(settings, settings.transition);
        settings.transition.anticipation = special;
        settings.transition.overshoot = special;
        check(settings, settings.transition);
    }
    std::puts("behavior: exact settlement matches for all presets, 512 random settings, non-finite fields");
}

using Run = float (*)(const PoseAnimationSettings&, const Transition&);
static std::array<double, 2> timeCalls(Run run, const PoseAnimationSettings& settings, int repeats) {
    const auto cpu = std::clock();
    const auto wall = std::chrono::steady_clock::now();
    for (int n = 0; n < repeats; ++n) sink = run(settings, settings.transition);
    const auto wallEnd = std::chrono::steady_clock::now();
    const auto cpuEnd = std::clock();
    return {double(cpuEnd - cpu) * 1e6 / CLOCKS_PER_SEC / repeats,
            std::chrono::duration<double, std::micro>(wallEnd - wall).count() / repeats};
}

int main(int argc, char**) {
    correctness();
    if (argc > 1) return 0;
    std::puts("case,sample,before_cpu_us,after_cpu_us,before_wall_us,after_wall_us");
    for (const auto& preset : feelPresets()) {
        for (int zeroLooseness = 0; zeroLooseness < 2; ++zeroLooseness) {
            PoseAnimationSettings settings;
            applyFeelPreset(preset, settings);
            if (zeroLooseness) settings.looseness = 0;
            for (int sample = 0; sample < 7; ++sample) {
                std::array<double, 2> a, b;
                const int repeats = settings.transition.curve == TransitionCurve::Spring ? 20 : 100000;
                if (sample % 2) { b = timeCalls(after::run, settings, repeats); a = timeCalls(before::run, settings, repeats); }
                else { a = timeCalls(before::run, settings, repeats); b = timeCalls(after::run, settings, repeats); }
                std::printf("%s%s,%d,%.4f,%.4f,%.4f,%.4f\n", preset.name,
                            zeroLooseness ? "-zero-looseness" : "", sample, a[0], b[0], a[1], b[1]);
            }
        }
    }
}
