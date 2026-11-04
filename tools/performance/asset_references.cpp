// Exact-production AssetIndex query fixture; see run-asset-references.py.
#include "asset/DepEdge.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <iostream>
#include <new>
#include <random>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

using jasperengine::Guid;
using jasperengine::editor::DepEdge;
using jasperengine::editor::DepEdgeKind;
using jasperengine::editor::isTextEdge;

#ifdef COUNT_ALLOCATIONS
static bool countAllocations = false;
static size_t allocationCount = 0;
[[gnu::noinline]] void* operator new(std::size_t bytes) {
    if (countAllocations) ++allocationCount;
    if (void* value = std::malloc(bytes ? bytes : 1)) return value;
    throw std::bad_alloc();
}
[[gnu::noinline]] void* operator new[](std::size_t bytes) { return ::operator new(bytes); }
[[gnu::noinline]] void operator delete(void* value) noexcept { std::free(value); }
[[gnu::noinline]] void operator delete[](void* value) noexcept { std::free(value); }
[[gnu::noinline]] void operator delete(void* value, std::size_t) noexcept { std::free(value); }
[[gnu::noinline]] void operator delete[](void* value, std::size_t) noexcept { std::free(value); }
#endif

struct AssetDatabase {
    struct SubAsset { Guid guid; };
    struct AssetInfo { std::vector<SubAsset> subAssets; std::vector<Guid> formerGuids; };
    std::unordered_map<Guid, AssetInfo> entries;
    bool getInfo(const Guid& guid, AssetInfo& info) const {
        const auto found = entries.find(guid);
        if (found == entries.end()) return false;
        info = found->second;
        return true;
    }
};

struct IndexFixture {
    struct Referrer { std::string path; std::vector<DepEdge> edges; };
    AssetDatabase* database = nullptr;
    std::unordered_map<Guid, Referrer> referrers;
    std::unordered_map<Guid, std::vector<Guid>> byTarget;
    std::unordered_map<std::string, std::vector<Guid>> byTargetPath;
};
#define INDEX_DECL(Name) struct Name : IndexFixture { \
    void rebuildReverse(); \
    void collectReferences(const Guid&, const std::string&, const std::unordered_map<Guid, std::vector<DepEdge>>&, std::vector<DepEdge>&, size_t) const; \
};
INDEX_DECL(BeforeIndex)
INDEX_DECL(AfterIndex)
// PRODUCTION_METHODS

static void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

static bool sameEdge(const DepEdge& a, const DepEdge& b) {
    return a == b && a.entity == b.entity && a.entityPath == b.entityPath &&
        a.component == b.component && a.line == b.line && a.expected == b.expected;
}

static DepEdge makeEdge(Guid referrer, Guid target, const std::string& path, DepEdgeKind kind) {
    DepEdge edge;
    edge.referrer = referrer;
    edge.target = target;
    edge.targetPath = path;
    edge.kind = kind;
    edge.field = "material/baseColorTexture";
    edge.entity = referrer.lo;
    edge.line = 7;
    return edge;
}

static void compare(BeforeIndex& before, AfterIndex& after, Guid target, const std::string& path,
                    const std::unordered_map<Guid, std::vector<DepEdge>>& memory, size_t limit,
                    const std::vector<DepEdge>& initial = {}) {
    auto a = initial, b = initial;
    before.collectReferences(target, path, memory, a, limit);
    after.collectReferences(target, path, memory, b, limit);
    require(a.size() == b.size(), "query edge count differs");
    for (size_t i = 0; i < a.size(); ++i) require(sameEdge(a[i], b[i]), "query edge or order differs");
}

static size_t verify() {
    AssetDatabase database;
    database.entries[Guid(1, 1)] = {{{Guid(2, 2)}, {Guid(2, 3)}}, {Guid(3, 3)}};
    std::mt19937 random(82379);
    size_t cases = 0;
    for (size_t round = 0; round < 80; ++round) {
        BeforeIndex before;
        before.database = &database;
        const std::vector<std::string> paths = {"", "a", "a/", "a/file", "a/folder/file", "a-other/file", "/", "assets/very-long-folder-name/texture.png", std::string("a/x\0y", 5), "assets/texture.png/"};
        for (size_t i = 0; i < 30; ++i) {
            Guid guid(9, i + 1);
            auto& referrer = before.referrers[guid];
            referrer.path = paths[random() % paths.size()] + std::to_string(random() % 5);
            for (size_t j = 0, n = random() % 8; j < n; ++j) {
                const std::vector<Guid> targets = {Guid(), Guid(1, 1), Guid(2, 2), Guid(3, 3), Guid(4, 4)};
                referrer.edges.push_back(makeEdge(guid, targets[random() % targets.size()], paths[random() % paths.size()],
                    random() % 2 ? DepEdgeKind::UIImage : DepEdgeKind::MaterialTexture));
            }
        }
        AfterIndex after;
        static_cast<IndexFixture&>(after) = before;
        before.rebuildReverse();
        after.rebuildReverse();
        std::unordered_map<Guid, std::vector<DepEdge>> memory;
        for (size_t i = 0; i < 8; ++i) {
            Guid guid(9, random() % 35 + 1);
            if (random() % 2) memory[guid] = {};
            else memory[guid] = {makeEdge(guid, Guid(1, 1), paths[random() % paths.size()], DepEdgeKind::UIImage)};
        }
        for (Guid target : {Guid(), Guid(1, 1), Guid(2, 2), Guid(5, 5)}) {
            for (const auto& path : paths) {
                for (size_t limit : {size_t(0), size_t(1), size_t(7), size_t(1000)}) {
                    compare(before, after, target, path, memory, limit);
                    ++cases;
                }
            }
        }
        const auto initial = std::vector<DepEdge>{makeEdge(Guid(7, 7), Guid(8, 8), "initial", DepEdgeKind::Other)};
        compare(before, after, Guid(1, 1), "a", memory, 1, initial);
        ++cases;
    }
    return cases;
}

#ifndef CHECK_ONLY
static volatile size_t resultCount = 0;
#ifdef COUNT_ALLOCATIONS
template<class Index> static size_t countQuery(const Index& index, Guid target, const std::string& path) {
    const std::unordered_map<Guid, std::vector<DepEdge>> memory;
    std::vector<DepEdge> out;
    allocationCount = 0;
    countAllocations = true;
    index.collectReferences(target, path, memory, out, 1000000);
    countAllocations = false;
    resultCount = out.size();
    return allocationCount;
}
#else
static double cpuNs() {
    timespec stamp{};
    clock_gettime(CLOCK_PROCESS_CPUTIME_ID, &stamp);
    return stamp.tv_sec * 1e9 + stamp.tv_nsec;
}
template<class Index> static double measure(const Index& index, Guid target, const std::string& path, size_t repeats) {
    const std::unordered_map<Guid, std::vector<DepEdge>> memory;
    const double start = cpuNs();
    for (size_t i = 0; i < repeats; ++i) {
        std::vector<DepEdge> out;
        index.collectReferences(target, path, memory, out, 1000000);
        resultCount = out.size();
    }
    return (cpuNs() - start) / repeats;
}

template<class Index> static size_t calibrate(const Index& index, Guid target, const std::string& path) {
    size_t repeats = 1;
    while (measure(index, target, path, repeats) * repeats < 20000000.0) repeats *= 2;
    return repeats;
}
#endif

static void benchmark(const std::string& name, size_t count, bool sharedTarget, size_t pathLength, bool checkCuts) {
    BeforeIndex before;
    const Guid target(1, 1);
    const std::string queryPath = "assets/textures/very-long-shared-texture-name.png";
    for (size_t i = 0; i < count; ++i) {
        const Guid guid(9, i + 1);
        auto& referrer = before.referrers[guid];
        referrer.path = "assets/" + std::string(pathLength, 'p') + "/material" + std::to_string(count - i) + ".material";
        const std::string path = sharedTarget ? queryPath : "assets/other-textures/texture" + std::to_string(i) + ".png";
        referrer.edges.push_back(makeEdge(guid, sharedTarget ? target : Guid(3, i + 1), path, DepEdgeKind::MaterialTexture));
    }
    AfterIndex after;
    static_cast<IndexFixture&>(after) = before;
    before.rebuildReverse();
    after.rebuildReverse();
    compare(before, after, target, queryPath, {}, 1000000);
#ifdef COUNT_ALLOCATIONS
    const size_t allocationsBefore = countQuery(before, target, queryPath);
    const size_t allocationsAfter = countQuery(after, target, queryPath);
    for (size_t round = 0; round < 9; ++round) {
        const size_t a = countQuery(before, target, queryPath);
        const size_t b = countQuery(after, target, queryPath);
        require(a == allocationsBefore && b == allocationsAfter, "allocation counts must be deterministic");
        std::cout << name << ',' << round << ',' << a << ',' << b << '\n';
        if (checkCuts && count >= 1024 && !sharedTarget) {
            require(b <= 4, "allocation regression: a missing-path query must allocate at most four times, independent of index size");
        }
    }
#else
    (void)checkCuts;
    const size_t beforeRepeats = calibrate(before, target, queryPath);
    const size_t afterRepeats = calibrate(after, target, queryPath);
    for (size_t round = 0; round < 9; ++round) {
        double a = 0, b = 0;
        if (round % 2) {
            b = measure(after, target, queryPath, afterRepeats);
            a = measure(before, target, queryPath, beforeRepeats);
        } else {
            a = measure(before, target, queryPath, beforeRepeats);
            b = measure(after, target, queryPath, afterRepeats);
        }
        std::cout << name << ',' << round << ',' << a << ',' << b << '\n';
    }
#endif
}
#endif

int main(int argc, char** argv) {
    try {
        const bool checkCuts = argc > 1 && std::string(argv[1]) == "--check-cuts";
#ifndef COUNT_ALLOCATIONS
        require(!checkCuts, "--check-cuts requires allocation instrumentation");
#endif
        std::cerr << "PASS: " << verify() << " differential queries, 0 skipped, 0 disabled\n";
#ifndef CHECK_ONLY
        if (argc > 1 && std::string(argv[1]) == "--check-only") return 0;
        benchmark("one-referrer", 1, true, 0, checkCuts);
        benchmark("fan-in-32", 32, true, 40, checkCuts);
        benchmark("fan-in-1024", 1024, true, 40, checkCuts);
        benchmark("fan-in-10000", 10000, true, 40, checkCuts);
        benchmark("absent-path-1024", 1024, false, 40, checkCuts);
        benchmark("absent-path-10000", 10000, false, 40, checkCuts);
#endif
    } catch (const std::exception& error) {
        std::cerr << "FAIL: " << error.what() << '\n';
        return 1;
    }
}
