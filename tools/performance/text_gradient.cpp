// The runner injects the baseline production evaluator and its numeric helpers.
#ifdef NDEBUG
#undef NDEBUG
#endif
#include "util/TextEffects.h"
#ifdef TRACK_ALLOCATIONS
#include "support/AllocationCounter.h"
#endif

#include <algorithm>
#include <array>
#include <bit>
#include <cassert>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <ctime>
#include <limits>
#include <random>
#include <vector>

using namespace jasperengine;

// BASELINE_EVALUATOR

static bool same(float a, float b){
    return (std::isnan(a) && std::isnan(b)) || std::bit_cast<uint32_t>(a) == std::bit_cast<uint32_t>(b);
}

static void check(const std::vector<TextEffectGradientStop>& gradient, float t){
    const auto actual = TextEffects::evaluateGradient(gradient, t);
    const auto expected = baselineGradient(gradient, t);
    assert(same(actual.x, expected.x) && same(actual.y, expected.y) &&
           same(actual.z, expected.z) && same(actual.w, expected.w));
}

static void checks(){
    std::mt19937 random(0x53ed);
    std::uniform_real_distribution<float> value(-2.0f, 2.0f);
    unsigned cases = 0;
    for (unsigned count = 0; count <= 32; count++){
        for (unsigned iteration = 0; iteration < 100; iteration++){
            std::vector<TextEffectGradientStop> stops;
            for (unsigned i = 0; i < count; i++){
                const Vector4 color(value(random), value(random), value(random), value(random));
                stops.push_back({color, iteration % 2 ? static_cast<float>(random() % 5) / 4.0f : value(random)});
            }
            for (unsigned order = 0; order < 2; order++){
                if (order) std::stable_sort(stops.begin(), stops.end(), [](const auto& a, const auto& b){ return a.position < b.position; });
                for (float t : {-1.0f, -0.0f, 0.0f, 0.25f, 0.5f, 0.75f, 1.0f, 2.0f, value(random),
                               std::numeric_limits<float>::quiet_NaN(), std::numeric_limits<float>::infinity()}){
                    check(stops, t);
                    cases++;
                }
            }
        }
    }
    const std::array special = {0.0f, -0.0f, 1.0f, -1.0f, std::numeric_limits<float>::infinity(),
        -std::numeric_limits<float>::infinity(), std::numeric_limits<float>::quiet_NaN()};
    for (float a : special) for (float b : special){
        const std::vector<TextEffectGradientStop> stops = {
            {Vector4(a, -a, b, -b), a}, {Vector4(b, -b, a, -a), b}, {Vector4(1, 1, 1, 1), 1.0f}};
        for (float t : special){ check(stops, t); cases++; }
    }
    std::fprintf(stderr, "%u differential gradient samples passed\n", cases);
}

static volatile float consumed;

int main(){
    checks();
#if defined(CHECK_ONLY) && !defined(TRACK_ALLOCATIONS)
    return 0;
#endif
    for (unsigned count : {0u, 1u, 2u, 8u, 32u}) for (bool ordered : {true, false}){
        std::vector<TextEffectGradientStop> stops;
        for (unsigned i = 0; i < count; i++){
            const float t = count > 1 ? static_cast<float>(i) / static_cast<float>(count - 1) : 0.0f;
            stops.push_back({Vector4(t, 1.0f - t, 0.5f, 1.0f), t});
        }
        if (!ordered) std::reverse(stops.begin(), stops.end());
#ifdef TRACK_ALLOCATIONS
        testsupport::ThreadAllocationCount allocations;
        const auto result = TextEffects::evaluateGradient(stops, 0.375f);
        consumed = result.x;
        std::printf("%s,%u,%llu\n", ordered ? "ordered" : "reversed", count,
                    static_cast<unsigned long long>(allocations.count()));
#else
        unsigned repeats = 200000;
        double seconds;
        while (true){
            const auto start = std::clock();
            for (unsigned i = 0; i < repeats; i++){
                consumed = TextEffects::evaluateGradient(stops, static_cast<float>(i % 1024) / 1023.0f).x;
            }
            seconds = static_cast<double>(std::clock() - start) / CLOCKS_PER_SEC;
            if (seconds >= 0.020 || repeats >= (1u << 26)) break;
            repeats *= 2;
        }
        std::printf("%s,%u,%.3f,%u\n", ordered ? "ordered" : "reversed", count, 1e9 * seconds / repeats, repeats);
#endif
    }
}
