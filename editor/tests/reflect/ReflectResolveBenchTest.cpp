// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// Path resolution cost (docs/specs/S08-reflection.md R18, AC14): resolving a flat and an indexed
// path allocates nothing (counted by this binary's operator new) and its median time is logged
// ("[bench] resolve"); JASPER_BENCH_ASSERT=1 (the benchmark server, a release build) also holds
// it to 150 ns.

#include "reflect/Reflect.h"

#include "component/LightComponent.h"
#include "component/MeshComponent.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <new>
#include <vector>

namespace {
    // the calling thread's allocations through operator new (the test binary's own code)
    thread_local unsigned long long reflectBenchAllocations = 0;
}

void* operator new(std::size_t size) {
    reflectBenchAllocations++;
    if (void* memory = std::malloc(size ? size : 1)) return memory;
    throw std::bad_alloc();
}

void* operator new[](std::size_t size) {
    reflectBenchAllocations++;
    if (void* memory = std::malloc(size ? size : 1)) return memory;
    throw std::bad_alloc();
}

void* operator new(std::size_t size, const std::nothrow_t&) noexcept {
    reflectBenchAllocations++;
    return std::malloc(size ? size : 1);
}

void* operator new[](std::size_t size, const std::nothrow_t&) noexcept {
    reflectBenchAllocations++;
    return std::malloc(size ? size : 1);
}

void operator delete(void* memory) noexcept { std::free(memory); }
void operator delete[](void* memory) noexcept { std::free(memory); }
void operator delete(void* memory, std::size_t) noexcept { std::free(memory); }
void operator delete[](void* memory, std::size_t) noexcept { std::free(memory); }
void operator delete(void* memory, const std::nothrow_t&) noexcept { std::free(memory); }
void operator delete[](void* memory, const std::nothrow_t&) noexcept { std::free(memory); }

using namespace jasperengine;
using namespace jasperengine::editor;

// (a named namespace, tests included: a unity build merges the test sources)
namespace reflect_resolve_bench_test {

    // median ns per resolve of `path`, over batches of 1000
    double medianNanoseconds(const reflect::ComponentDesc& desc, void* comp, std::string_view path, unsigned long long& allocations) {
        std::vector<double> batches;
        void* sink = nullptr;
        const unsigned long long before = reflectBenchAllocations;
        for (int batch = 0; batch < 51; batch++) {
            const auto start = std::chrono::steady_clock::now();
            for (int i = 0; i < 1000; i++) {
                const reflect::FieldRef ref = reflect::resolve(desc, comp, path);
                sink = ref.ref;
            }
            const auto end = std::chrono::steady_clock::now();
            batches.push_back(std::chrono::duration<double, std::nano>(end - start).count() / 1000.0);
        }
        allocations = reflectBenchAllocations - before;
        EXPECT_NE(sink, nullptr);
        std::sort(batches.begin(), batches.end());
        return batches[batches.size() / 2];
    }

    TEST(ReflectResolve, FlatAndIndexedPathsDoNotAllocate) {
        // the name indexes are built before the measure (once, at editor start)
        reflect::buildNameIndexes();
        const bool assertTime = std::getenv("JASPER_BENCH_ASSERT") != nullptr;

        LightComponent light;
        unsigned long long allocations = 0;
        const double flat = medianNanoseconds(*reflect::find(ComponentType::LightComponent), &light, "shadowNormalBias", allocations);
        std::printf("[bench] resolve light.shadowNormalBias: %.1f ns median\n", flat);
        EXPECT_EQ(allocations, 0u);

        MeshComponent mesh;
        const double indexed = medianNanoseconds(*reflect::find(ComponentType::MeshComponent), &mesh, "submeshes[3].material.baseColorFactor",
                                                 allocations);
        std::printf("[bench] resolve mesh.submeshes[3].material.baseColorFactor: %.1f ns median\n", indexed);
        EXPECT_EQ(allocations, 0u);

        if (assertTime) {
            EXPECT_LE(flat, 150.0);
            EXPECT_LE(indexed, 150.0);
        }
    }

}
