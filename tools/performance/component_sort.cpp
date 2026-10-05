// Compile against the real ComponentArray header; see run_core.py.
#include <cassert>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <new>
#include <random>
#include <string>
#ifndef COMPONENT_ARRAY_HEADER
#define COMPONENT_ARRAY_HEADER "ecs/ComponentArray.h"
#endif
#include COMPONENT_ARRAY_HEADER

static size_t allocations = 0;
static bool countAllocations = false;
void* operator new(std::size_t size) {
    if (countAllocations) ++allocations;
    if (void* p = std::malloc(size ? size : 1)) return p;
    throw std::bad_alloc();
}
void operator delete(void* p) noexcept { std::free(p); }
void operator delete(void* p, std::size_t) noexcept { std::free(p); }
namespace jasperengine { void Log::error(const char*, ...) { std::abort(); } }

using namespace jasperengine;
struct Payload { Entity owner; std::string text; };

static void correctness() {
    std::mt19937 rng(42);
    for (size_t n : {size_t(0), size_t(1), size_t(2), size_t(35), size_t(300)}) {
        for (int trial = 0; trial < 20; ++trial) {
            ComponentArray<int> order;
            ComponentArray<Payload> values;
            std::vector<Entity> entities(n);
            std::iota(entities.begin(), entities.end(), 1);
            for (Entity e : entities) if (e % 4) order.insert(e, int(e));
            if (trial) std::shuffle(entities.begin(), entities.end(), rng);
            for (Entity e : entities) values.insert(e, Payload{e, std::to_string(e)});
            std::vector<size_t> permutation(n);
            std::iota(permutation.begin(), permutation.end(), 0);
            auto key = [&](Entity e) { return order.hasEntity(e) ? order.getIndex(e) : size_t(-1); };
            auto less = [&](size_t a, size_t b) { return key(entities[a]) < key(entities[b]); };
            // Match the existing tie ordering exactly, including its sorted fast path.
            if (!std::is_sorted(permutation.begin(), permutation.end(), less))
                std::sort(permutation.begin(), permutation.end(), less);
            values.sortByComponent(&order);
            for (size_t i = 0; i < n; ++i) {
                Entity e = entities[permutation[i]];
                assert(values.getEntity(i) == e);
                assert(values.getIndex(e) == i);
                assert(values.getComponent(e).owner == e);
                assert(values.getComponent(e).text == std::to_string(e));
            }
            values.sortByComponent(&order);
            for (size_t i = 0; i < n; ++i) assert(values.getEntity(i) == entities[permutation[i]]);
        }
    }
}

int main() {
    correctness();
#ifdef CORE_CHECK_ONLY
    return 0;
#endif
    for (size_t n : {size_t(1000), size_t(10000), size_t(100000)}) {
        ComponentArray<int> order, values;
        for (size_t i = 0; i < n; ++i) { order.insert(i + 1, i); values.insert(i + 1, i); }
        const size_t repeats = 10000000 / n;
        for (int sample = 0; sample < 7; ++sample) {
            allocations = 0;
            countAllocations = true;
            auto cpuStart = std::clock();
            auto start = std::chrono::steady_clock::now();
            for (size_t i = 0; i < repeats; ++i) values.sortByComponent(&order);
            auto stop = std::chrono::steady_clock::now();
            auto cpuStop = std::clock();
            countAllocations = false;
            double ns = std::chrono::duration<double, std::nano>(stop - start).count() / repeats;
            std::printf("sorted,%zu,%d,%.3f,%.1f,%.3f\n", n, sample,
                double(cpuStop - cpuStart) * 1e9 / CLOCKS_PER_SEC / repeats, double(allocations) / repeats, ns);
        }
        // Alternate opposing reference orders: every timed call must reverse
        // the entire array. Aggregate calls so clock resolution/cache noise
        // cannot dominate the smaller fixtures.
        ComponentArray<int> reverseOrder;
        for (size_t i = n; i > 0; --i) reverseOrder.insert(i, i - 1);
        const size_t reverseRepeats = 1000000 / n;
        for (int sample = 0; sample < 7; ++sample) {
            allocations = 0;
            countAllocations = true;
            auto cpuStart = std::clock();
            auto start = std::chrono::steady_clock::now();
            for (size_t i = 0; i < reverseRepeats; ++i) {
                values.sortByComponent(&reverseOrder);
                values.sortByComponent(&order);
            }
            auto stop = std::chrono::steady_clock::now();
            auto cpuStop = std::clock();
            countAllocations = false;
            for (size_t i = 0; i < n; ++i) assert(values.getEntity(i) == i + 1);
            std::printf("reversed,%zu,%d,%.3f,%.1f,%.3f\n", n, sample,
                double(cpuStop - cpuStart) * 1e9 / CLOCKS_PER_SEC / (2 * reverseRepeats),
                double(allocations) / (2 * reverseRepeats),
                std::chrono::duration<double, std::nano>(stop - start).count() / (2 * reverseRepeats));
        }
    }
}
