// Alternate baseline/current production headers in one process, reversing run
// order on every sample, to distinguish a regression from host load drift.
#include <cassert>
#include <ctime>
#include <cstdio>
#include <cstdlib>
#include "ecs/ComponentArray.h"
#include BASELINE_COMPONENT_HEADER
namespace jasperengine { void Log::error(const char*, ...) { std::abort(); } }

template<class Array> struct Fixture {
    Array values, forward, backward;
    explicit Fixture(size_t count) {
        for (size_t i = 1; i <= count; ++i) {
            values.insert(i, int(i));
            forward.insert(i, int(i));
        }
        for (size_t i = count; i > 0; --i) backward.insert(i, int(i));
    }
    double reverse(size_t repeats) {
        auto start = std::clock();
        for (size_t i = 0; i < repeats; ++i) {
            values.sortByComponent(&backward);
            values.sortByComponent(&forward);
        }
        auto stop = std::clock();
        for (size_t i = 0; i < values.size(); ++i) assert(values.getEntity(i) == i + 1);
        return double(stop - start) * 1e9 / CLOCKS_PER_SEC / (2 * repeats);
    }
};

int main() {
    for (size_t count : {size_t(1000), size_t(10000), size_t(100000)}) {
        Fixture<baseline::ComponentArray<int>> before(count);
        Fixture<jasperengine::ComponentArray<int>> after(count);
        const size_t repeats = 1000000 / count;
        for (int sample = 0; sample < 14; ++sample) {
            double baselineTime, currentTime;
            if (sample % 2) {
                currentTime = after.reverse(repeats);
                baselineTime = before.reverse(repeats);
            } else {
                baselineTime = before.reverse(repeats);
                currentTime = after.reverse(repeats);
            }
            std::printf("%zu,%d,%.2f,%.2f,%.4f\n", count, sample,
                baselineTime, currentTime, currentTime / baselineTime);
        }
    }
}
