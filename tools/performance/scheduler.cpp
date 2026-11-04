// The real SystemScheduler.cpp and headers, with a serial scene/jobs adapter.
#include "SystemScheduler.h"
#include "Scene.h"

#include <cassert>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <new>
#include <stdexcept>
#include <string>

using namespace jasperengine;

static size_t allocations = 0;
static bool countAllocations = false;
void* operator new(std::size_t size) {
    if (countAllocations) ++allocations;
    if (void* ptr = std::malloc(size ? size : 1)) return ptr;
    throw std::bad_alloc();
}
void operator delete(void* ptr) noexcept { std::free(ptr); }
void operator delete(void* ptr, std::size_t) noexcept { std::free(ptr); }

namespace jasperengine {
void Log::error(const char*, ...) { std::abort(); }
void EntityCommandBuffer::defer(std::function<void(Scene&)> fn) {
    push(Op::Defer, {}).payload.emplace<std::function<void(Scene&)>>(std::move(fn));
}
void EntityCommandBuffer::playback(EntityRegistry& registry) {
    auto playing = std::move(commands);
    commands.clear();
    for (auto& command : playing) {
        assert(command.op == Op::Defer);
        command.payload.get<std::function<void(Scene&)>>()(static_cast<Scene&>(registry));
    }
}
}

static SystemDescriptor descriptor(const std::string& name, bool exclusive = true) {
    SystemDescriptor result;
    result.name = name;
    if (exclusive) result.resources = SystemResource::Exclusive;
    return result;
}

static void frame(SystemScheduler& scheduler, Scene& scene) {
    scheduler.runPhase(scene, SystemPhase::Update, 0.016);
    scheduler.runPhase(scene, SystemPhase::Draw, 0.0);
}

static void correctness() {
    Scene scene;
    SystemScheduler scheduler;
    std::vector<int> calls;
    bool throwNow = true;
    auto first = descriptor("First");
    first.dependsOn = {"Second"};
    scheduler.add(first, [&](SystemContext& ctx) {
        calls.push_back(1);
        assert(ctx.dt == 0.016);
        ctx.cmd.defer([&](Scene&) { calls.push_back(10); });
    });
    scheduler.add(descriptor("Second", false), [&](SystemContext& ctx) {
        calls.push_back(2);
        ctx.cmd.defer([&](Scene&) { calls.push_back(20); });
    });
    scheduler.add(descriptor("Throw"), [&](SystemContext&) {
        if (throwNow) throw std::runtime_error("expected");
        calls.push_back(3);
    });
    try {
        frame(scheduler, scene);
        assert(false);
    } catch (const std::runtime_error&) {}
    assert((calls == std::vector<int>{2, 20, 1, 10}));
    scheduler.runPhase(scene, SystemPhase::Draw, 0.0);
    const auto& failedTrace = scheduler.getLastTrace();
    assert(failedTrace.size() == 2);
    assert(*failedTrace[0].system == "Second" && *failedTrace[1].system == "First");
    throwNow = false;
    calls.clear();
    frame(scheduler, scene);
    assert((calls == std::vector<int>{2, 20, 1, 10, 3}));
    assert(scheduler.getLastTrace().size() == 3);
    assert(scheduler.setEnabled("First", false));
    calls.clear();
    frame(scheduler, scene);
    assert((calls == std::vector<int>{2, 20, 3}));
    assert(scheduler.getLastTrace().size() == 2);
}

static size_t allocationCount(size_t waves) {
    Scene scene;
    SystemScheduler scheduler;
    for (size_t i = 0; i < waves; i++)
        scheduler.add(descriptor("Empty" + std::to_string(i)), [](SystemContext&) {});
    for (int i = 0; i < 4; i++) frame(scheduler, scene);
    allocations = 0;
    countAllocations = true;
    frame(scheduler, scene);
    countAllocations = false;
    return allocations;
}

int main(int argc, char** argv) {
    correctness();
    const size_t one = allocationCount(1), twenty = allocationCount(20);
    if (argc > 1 && std::string(argv[1]) == "--check-allocations") {
        assert(one == twenty);
        return 0;
    }
#ifdef CORE_CHECK_ONLY
    return 0;
#endif
    std::printf("allocations,%zu,%zu\n", one, twenty);
    for (bool exclusive : {false, true}) {
        Scene scene;
        SystemScheduler scheduler;
        for (int i = 0; i < 20; i++) {
            auto d = descriptor("Empty" + std::to_string(i), exclusive);
            // Writes force singleton waves even without the Exclusive declaration.
            d.writes.set(7);
            scheduler.add(std::move(d), [](SystemContext&) {});
        }
        for (int i = 0; i < 100; i++) frame(scheduler, scene);
        size_t repeats = 256;
        double cpuNs, wallNs;
        do {
            const auto start = std::chrono::steady_clock::now();
            const auto cpu = std::clock();
            for (size_t i = 0; i < repeats; i++) frame(scheduler, scene);
            cpuNs = double(std::clock() - cpu) * 1e9 / CLOCKS_PER_SEC;
            wallNs = std::chrono::duration<double, std::nano>(std::chrono::steady_clock::now() - start).count();
            if (cpuNs < 50000000.0) repeats *= 2;
        } while (cpuNs < 50000000.0);
        allocations = 0;
        countAllocations = true;
        frame(scheduler, scene);
        countAllocations = false;
        std::printf("%s,%.3f,%.3f,%zu,%zu,%.0f\n", exclusive ? "exclusive" : "declared",
                    cpuNs / repeats, wallNs / repeats, allocations, repeats, cpuNs);
    }
}
