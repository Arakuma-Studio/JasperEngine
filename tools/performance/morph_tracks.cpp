// The runner injects the unmodified production method and helper templates.
// This fixture isolates sampling/blending from Scene, graphics and audio startup.
#include <cassert>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <new>
#include <unordered_map>
#include "component/KeyframeTracksComponent.h"
#include "component/MorphTracksComponent.h"
#include "math/Interpolation.h"
#include "Log.h"

static size_t allocations = 0;
static bool countAllocations = false;
void* operator new(std::size_t size) {
    if (countAllocations) ++allocations;
    if (void* p = std::malloc(size ? size : 1)) return p;
    throw std::bad_alloc();
}
void operator delete(void* p) noexcept { std::free(p); }
void operator delete(void* p, std::size_t) noexcept { std::free(p); }
namespace jasperengine {
static int loggedErrors = 0;
void Log::error(const char*, ...) { ++loggedErrors; }
class ActionSystem {
public:
    struct MorphBlendAccum {
        float sums[MAX_MORPHTARGETS] = {0};
        float weights[MAX_MORPHTARGETS] = {0};
    };
    std::unordered_map<Entity, MorphBlendAccum> morphBlend;
    void morphTracksUpdate(KeyframeTracksComponent&, MorphTracksComponent&, Entity, float);
};
// PRODUCTION_METHODS
}
using namespace jasperengine;

static void correctness() {
    for (int cubic = 0; cubic < 2; ++cubic) {
        for (int index = 0; index < 3; ++index) {
            ActionSystem system;
            KeyframeTracksComponent key;
            key.times = {0, 1, 2}; key.index = index; key.interpolation = 0.25f;
            MorphTracksComponent track;
            track.values = {{0, 1}, {2, 3}, {4, 5}};
            if (cubic) {
                track.inTangents = {{0.5f, 0.5f}, {0.5f, 0.5f}, {0.5f, 0.5f}};
                track.outTangents = track.inTangents;
            }
            const auto original = track.values;
            system.morphTracksUpdate(key, track, 1, 0.5f);
            system.morphTracksUpdate(key, track, 1, 0.25f);
            const auto& blend = system.morphBlend.at(1);
            for (int channel = 0; channel < 2; ++channel) {
                float prev = track.values[index ? index - 1 : 0][channel];
                float next = track.values[index][channel];
                float value = cubic && index ? Interpolation::cubicSpline(0.25f, 1.f, prev, 0.5f, next, 0.5f)
                                             : prev + 0.25f * (next - prev);
                assert(blend.sums[channel] == value * 0.75f);
                assert(blend.weights[channel] == 0.75f);
            }
            assert(track.values == original);
            system.morphTracksUpdate(key, track, 2, 0.f);
            assert(system.morphBlend.count(2) == 0);
            key.index = 3;
            system.morphTracksUpdate(key, track, 2, 1.f);
            assert(system.morphBlend.count(2) == 0);
            key.index = 1; track.values[1].push_back(0);
            system.morphTracksUpdate(key, track, 2, 1.f);
            assert(system.morphBlend.count(2) == 0);
        }
    }
    assert(loggedErrors == 6);
}

int main() {
    correctness();
#ifdef CORE_CHECK_ONLY
    return 0;
#endif
    for (size_t channels : {size_t(1), size_t(8), size_t(64)}) {
        for (int cubic = 0; cubic < 2; ++cubic) {
            ActionSystem system;
            KeyframeTracksComponent key;
            key.times = {0, 1}; key.index = 1; key.interpolation = 0.25f;
            MorphTracksComponent track;
            track.values = {std::vector<float>(channels, 0.25f), std::vector<float>(channels, 0.5f)};
            if (cubic) { track.inTangents = track.values; track.outTangents = track.values; }
            system.morphTracksUpdate(key, track, 1, 1.f); // prewarm the target map
            const size_t repeats = 1000000;
            for (int sample = 0; sample < 7; ++sample) {
                allocations = 0; countAllocations = true;
                auto cpuStart = std::clock();
                auto start = std::chrono::steady_clock::now();
                for (size_t i = 0; i < repeats; ++i) system.morphTracksUpdate(key, track, 1, 1.f);
                auto stop = std::chrono::steady_clock::now();
                auto cpuStop = std::clock();
                countAllocations = false;
                assert(std::isfinite(system.morphBlend.at(1).sums[0]));
                std::printf("%s,%zu,%d,%.3f,%.1f,%.3f\n", cubic ? "cubic" : "linear", channels, sample,
                    double(cpuStop - cpuStart) * 1e9 / CLOCKS_PER_SEC / repeats, double(allocations) / repeats,
                    std::chrono::duration<double, std::nano>(stop - start).count() / repeats);
            }
        }
    }
}
