// Exact EntityRegistry lookup methods are injected by run-entity-lookup.py.
#include <algorithm>
#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <new>
#include <random>
#include "ecs/EntityManager.h"
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
static int errors = 0;
void Log::error(const char*, ...) { ++errors; }
class TagManager {
public:
    static bool isTagDefined(const std::string& tag) {
        return tag == "Untagged" || tag == "enemy" || (tag.size() == 48 && tag.find_first_not_of('t') == std::string::npos);
    }
};
}
using namespace jasperengine;
#define REGISTRY(NAME) class NAME { \
public: EntityManager entityManager; \
    bool isParentOf(Entity parent, Entity entity) const { return entity % 5 == parent; } \
    __attribute__((noinline)) Entity findEntity(const std::string&) const; \
    __attribute__((noinline)) Entity findEntity(const std::string&, Entity) const; \
    __attribute__((noinline)) Entity findEntityByTag(const std::string&) const; \
    __attribute__((noinline)) std::vector<Entity> findEntitiesByTag(const std::string&) const; \
};
REGISTRY(BaselineRegistry)
REGISTRY(WorkingRegistry)
// PRODUCTION_METHODS

static void correctness() {
    BaselineRegistry before;
    WorkingRegistry after;
    std::mt19937 random(164);
    auto compare = [&] {
        for (const auto& name : {std::string(), std::string("duplicate"), std::string("missing"), std::string(48, 'n')}) {
            assert(before.findEntity(name) == after.findEntity(name));
            for (Entity parent = 0; parent < 6; ++parent)
                assert(before.findEntity(name, parent) == after.findEntity(name, parent));
        }
        for (const auto& tag : {std::string(), std::string("Untagged"), std::string("enemy"), std::string(48, 't')}) {
            const int errorsBefore = errors;
            const Entity expected = before.findEntityByTag(tag);
            const int beforeErrors = errors - errorsBefore;
            const int errorsAfter = errors;
            assert(expected == after.findEntityByTag(tag));
            assert(errors - errorsAfter == beforeErrors);
            const int listErrorsBefore = errors;
            const auto expectedList = before.findEntitiesByTag(tag);
            const int beforeListErrors = errors - listErrorsBefore;
            const int listErrorsAfter = errors;
            assert(expectedList == after.findEntitiesByTag(tag));
            assert(errors - listErrorsAfter == beforeListErrors);
        }
    };
    compare();
    for (int i = 0; i < 300; ++i) {
        Entity entity = 1 + random() % 200;
        before.entityManager.recreateEntity(entity);
        after.entityManager.recreateEntity(entity);
        auto name = i % 3 ? std::string("duplicate") : std::string(48, 'n');
        auto tag = i % 2 ? std::string("enemy") : std::string(48, 't');
        before.entityManager.setName(entity, name); after.entityManager.setName(entity, name);
        before.entityManager.setTag(entity, tag); after.entityManager.setTag(entity, tag);
        compare();
        if (i % 3 == 0) {
            before.entityManager.destroy(entity); after.entityManager.destroy(entity);
            compare();
        }
    }
}
static double cpuNs() {
    timespec value{}; clock_gettime(CLOCK_PROCESS_CPUTIME_ID, &value);
    return value.tv_sec * 1e9 + value.tv_nsec;
}
volatile size_t sink;
template<class Registry>
static std::pair<double, double> measure(Registry& registry, const std::string& query, int mode, size_t repeats) {
    allocations = 0; countAllocations = true;
    size_t sum = 0;
    const double start = cpuNs();
    for (size_t i = 0; i < repeats; ++i) {
        if (mode == 0) sum += registry.findEntity(query);
        if (mode == 1) sum += registry.findEntity(query, 4);
        if (mode == 2) sum += registry.findEntityByTag(query);
        if (mode == 3) sum += registry.findEntitiesByTag(query).size();
    }
    const double elapsed = cpuNs() - start;
    countAllocations = false; sink = sum;
    return {elapsed / repeats, double(allocations) / repeats};
}
int main() {
    correctness();
#ifdef CORE_CHECK_ONLY
    return 0;
#endif
    for (size_t nameLength : {size_t(8), size_t(48)}) {
        for (size_t count : {size_t(100), size_t(1000), size_t(10000)}) {
            BaselineRegistry before;
            WorkingRegistry after;
            for (size_t i = 0; i < count; ++i) {
                const Entity e = Entity(i + 1);
                auto name = std::string(nameLength, 'n') + std::to_string(i);
                before.entityManager.recreateEntity(e); after.entityManager.recreateEntity(e);
                before.entityManager.setName(e, name); after.entityManager.setName(e, name);
                if (nameLength == 48) {
                    before.entityManager.setTag(e, std::string(48, 't')); after.entityManager.setTag(e, std::string(48, 't'));
                }
            }
            const size_t repeats = std::max(size_t(30), size_t(100000) / count);
            for (int mode = 0; mode < 6; ++mode) {
                const std::string query = mode < 2 ? std::string(nameLength, 'n') + std::to_string(count - 1)
                    : mode == 4 ? std::string(nameLength, 'n') + "0"
                    : mode == 5 ? "missing" : nameLength == 48 ? std::string(48, 't') : "Untagged";
                const int operation = mode < 4 ? mode : 0;
                for (int sample = 0; sample < 7; ++sample) {
                    std::pair<double, double> a, b;
                    if (sample % 2) { b = measure(after, query, operation, repeats); a = measure(before, query, operation, repeats); }
                    else { a = measure(before, query, operation, repeats); b = measure(after, query, operation, repeats); }
                    std::printf("%zu,%zu,%d,%d,%.2f,%.2f,%.2f,%.2f\n", nameLength, count, mode, sample, a.first, b.first, a.second, b.second);
                }
            }
        }
    }
}
