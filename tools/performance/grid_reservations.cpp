// Production declarations and methods are injected without rewriting their logic.
#include <algorithm>
#include <atomic>
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#ifdef GRID_RESERVATION_ALLOCATIONS
#include <new>
#endif
#include <random>
#include <unordered_map>
#include <vector>

#ifdef GRID_RESERVATION_ALLOCATIONS
static size_t allocationCount = 0;
static bool countAllocations = false;
void* operator new(std::size_t size){
    if (countAllocations) allocationCount++;
    if (void* value = std::malloc(size ? size : 1)) return value;
    throw std::bad_alloc();
}
void operator delete(void* value) noexcept { std::free(value); }
void operator delete(void* value, std::size_t) noexcept { std::free(value); }
#endif

// PRODUCTION_CLASSES

static void correctness(){
    BeforeReservations before;
    AfterReservations after;
    std::mt19937 random(310284);
    before.reset(64);
    after.reset(64);
    size_t queries = 0;
    for (int step = 0; step < 30000; step++){
        const uint32_t agent = step % 17 == 0 ? UINT32_MAX : random() % 34;
        const uint32_t tick = random() % 100;
        if (step % 203 == 0){
            const int cells = (int)(random() % 67) - 2;
            before.reset(cells);
            after.reset(cells);
        }else if (random() % 4 == 0){
            before.release(agent);
            after.release(agent);
        }else{
            std::vector<int> cells(random() % 12);
            for (int& cell : cells) cell = (int)(random() % 70) - 3;
            before.reserve(agent, cells, tick);
            after.reserve(agent, cells, tick);
        }
        assert(before.getCellCount() == after.getCellCount());
        for (int cell = -1; cell <= before.getCellCount(); cell++){
            for (uint32_t at : {0u, tick, 99u, UINT32_MAX}){
                assert(before.holder(cell, at) == after.holder(cell, at));
                assert(before.isReservedByOther(cell, agent, at) == after.isReservedByOther(cell, agent, at));
                queries += 2;
            }
        }
    }
    std::fprintf(stderr, "Differential checks passed: 30000 mutations, %zu holder/expiry queries.\n", queries);
}

#ifdef GRID_RESERVATION_ALLOCATIONS
template<class Reservations>
static double allocationsPerUpdate(size_t count){
    Reservations reservations;
    reservations.reset((int)count * 2);
    std::vector<std::vector<int>> paths(count);
    for (size_t i = 0; i < count; i++){
        paths[i] = {(int)i * 2, (int)i * 2 + 1};
        reservations.reserve((uint32_t)i + 1, paths[i], 10);
    }
    allocationCount = 0;
    countAllocations = true;
    for (size_t i = 0; i < count; i++) reservations.reserve((uint32_t)i + 1, paths[i], 11);
    countAllocations = false;
    const size_t allocations = allocationCount;
    assert(reservations.holder((int)count * 2 - 1, 11) == count);
    return double(allocations) / double(count);
}
#elif !defined(CHECK_ONLY)
static double cpuSeconds(){
    timespec now;
    const int status = clock_gettime(CLOCK_PROCESS_CPUTIME_ID, &now);
    assert(status == 0);
    return double(now.tv_sec) + double(now.tv_nsec) * 1e-9;
}

template<class Reservations>
static double measure(size_t count){
    Reservations reservations;
    reservations.reset((int)count * 2);
    std::vector<std::vector<int>> paths(count);
    for (size_t i = 0; i < count; i++){
        paths[i] = {(int)i * 2, (int)i * 2 + 1};
        reservations.reserve((uint32_t)i + 1, paths[i], 10);
    }
    uint32_t tick = 11;
    size_t updates = 0;
    const size_t batches = std::max(size_t(1), size_t(1024) / count);
    const double start = cpuSeconds();
    double elapsed;
    do{
        for (size_t batch = 0; batch < batches; batch++){
            for (size_t i = 0; i < count; i++) reservations.reserve((uint32_t)i + 1, paths[i], tick);
            // Make every batch's stores observable to the compiler without a hardware fence.
            std::atomic_signal_fence(std::memory_order_seq_cst);
            updates += count;
            tick++;
        }
        elapsed = cpuSeconds() - start;
    }while (elapsed < 0.025);
    assert(reservations.holder((int)count * 2 - 1, tick - 1) == count);
    return elapsed * 1e9 / double(updates);
}
#endif

int main(int argc, char** argv){
    correctness();
    if (argc > 1 && std::strcmp(argv[1], "--check") == 0) return 0;
#ifdef GRID_RESERVATION_ALLOCATIONS
    for (size_t count : {size_t(1), size_t(8), size_t(128), size_t(1024), size_t(8192)}){
        const double before = allocationsPerUpdate<BeforeReservations>(count);
        const double after = allocationsPerUpdate<AfterReservations>(count);
        std::printf("%zu,%.6f,%.6f\n", count, before, after);
    }
#elif !defined(CHECK_ONLY)
    for (size_t count : {size_t(1), size_t(8), size_t(128), size_t(1024), size_t(8192)}){
        for (int sample = 0; sample < 11; sample++){
            double before, after;
            if (sample % 2 == 0){
                before = measure<BeforeReservations>(count);
                after = measure<AfterReservations>(count);
            }else{
                after = measure<AfterReservations>(count);
                before = measure<BeforeReservations>(count);
            }
            std::printf("%zu,%d,%.3f,%.3f\n", count, sample, before, after);
        }
    }
#endif
}
