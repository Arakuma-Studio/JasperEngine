// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// Destroying entities costs the same whatever the scene's size: a component removed from the
// middle of its array leaves a hole the next access by index closes (ComponentArray), so a
// run of destroys (one by one, a Model taking its nodes along, a scene's teardown, clear())
// is O(1) an entity, where it shifted every array each time and a crowd of 150,000 entities
// took hours. The arrays still read as if every removal had shifted them: indexes, order,
// size, the hierarchy queries and revision, callbacks and what they see.

#include <gtest/gtest.h>

#include "Scene.h"
#include "component/ActionComponent.h"
#include "component/BoneComponent.h"
#include "component/MeshComponent.h"
#include "component/ModelComponent.h"
#include "component/Transform.h"
#include "ecs/ComponentArray.h"
#include "manager/ObjectManager.h"
#include "object/Model.h"
#include "registry/EntityCloner.h"
#include "component/AnimationComponent.h"
#include "component/ButtonComponent.h"
#include "component/PanelComponent.h"
#include "util/ProjectUtils.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <random>
#include <string>
#include <unordered_set>
#include <vector>

using namespace jasperengine;

namespace {

    using Clock = std::chrono::steady_clock;

    double secondsSince(Clock::time_point start) {
        return std::chrono::duration<double>(Clock::now() - start).count();
    }

    // A model copy as a glTF hero loads: the Model (Transform, Mesh, Model), an armature below
    // it with a chain of 8 bones, and 2 mesh nodes; 12 entities, the 11 nodes in nodesIdMapping
    constexpr int ENTITIES_PER_COPY = 12;

    void addCopy(Scene& scene, std::vector<std::unique_ptr<Model>>& models) {
        models.emplace_back(new Model(&scene));
        const Entity root = models.back()->getEntity();
        ModelComponent& model = scene.getComponent<ModelComponent>(root);
        int node = 0;
        auto child = [&](Entity parent) {
            const Entity entity = scene.createEntity();
            scene.addComponent<Transform>(entity, Transform());
            scene.addEntityChild(parent, entity, false);
            model.nodesIdMapping[node++] = entity;
            return entity;
        };
        Entity bone = child(root);
        for (int b = 0; b < 8; b++) {
            bone = child(bone);
            scene.addComponent<BoneComponent>(bone, BoneComponent());
        }
        for (int m = 0; m < 2; m++) {
            const Entity mesh = child(root);
            scene.addComponent<MeshComponent>(mesh, MeshComponent());
        }
    }

    struct Crowd {
        std::unique_ptr<Scene> scene{new Scene()};
        std::vector<std::unique_ptr<Model>> models;

        explicit Crowd(int copies) {
            models.reserve(copies);
            for (int i = 0; i < copies; i++) addCopy(*scene, models);
        }
        size_t entities() const { return models.size() * ENTITIES_PER_COPY; }
    };

    enum class Teardown { DestroyEach, DestroyEachBackwards, DestroyEachReadingByIndex, ModelHandles, ModelHandlesNewestFirst, DestroyImmediate, DeleteScene, Clear };

    const char* name(Teardown teardown) {
        switch (teardown) {
            case Teardown::DestroyEach: return "destroyEntity one by one";
            case Teardown::DestroyEachBackwards: return "destroyEntity, last first";
            case Teardown::DestroyEachReadingByIndex: return "destroyEntity + index reads";
            case Teardown::ModelHandles: return "Model handles deleted";
            case Teardown::ModelHandlesNewestFirst: return "Model handles, newest first";
            case Teardown::DestroyImmediate: return "destroyImmediate each";
            case Teardown::DeleteScene: return "scene deleted";
            case Teardown::Clear: return "clear()";
        }
        return "";
    }

    // seconds an entity to tear down a crowd of `copies` model copies
    double teardownSecondsPerEntity(int copies, Teardown teardown) {
        const Clock::time_point made = Clock::now();
        Crowd crowd(copies);
        const double making = secondsSince(made);
        const size_t entities = crowd.entities();
        // the last entity of the scene, made after the crowd: the holes are never at the end
        const Entity last = crowd.scene->createEntity();
        crowd.scene->addComponent<Transform>(last, Transform());
        const Clock::time_point start = Clock::now();
        switch (teardown) {
            case Teardown::DestroyEach:
                // ascending ids, the order the scene made them in (the front of every array first)
                for (Entity entity : crowd.scene->getEntityList()) {
                    if (entity != last) crowd.scene->destroyEntity(entity);
                }
                break;
            case Teardown::DestroyEachBackwards: {
                // the roots last first (each one's children are holes after it), then the rest
                std::vector<Entity> roots;
                for (const auto& model : crowd.models) roots.push_back(model->getEntity());
                for (auto it = roots.rbegin(); it != roots.rend(); ++it) crowd.scene->destroyEntity(*it);
                std::vector<Entity> list = crowd.scene->getEntityList();
                for (auto it = list.rbegin(); it != list.rend(); ++it) {
                    if (*it != last) crowd.scene->destroyEntity(*it);
                }
                break;
            }
            case Teardown::DestroyEachReadingByIndex: {
                // reads by index between the destroys (a script or a system looking up indexes):
                // each read closes the holes before it (S07 R2), so this one is O(n) an entity, the
                // cost of an eager erase (S07 D1's worst case); timed by Profile only
                auto transforms = crowd.scene->getComponentArray<Transform>();
                auto meshes = crowd.scene->getComponentArray<MeshComponent>();
                size_t sum = 0;
                for (Entity entity : crowd.scene->getEntityList()) {
                    if (entity == last) continue;
                    crowd.scene->destroyEntity(entity);
                    sum += transforms->getIndex(last) + transforms->size() + meshes->size();
                    if (transforms->size() > 1) sum += transforms->getEntity(transforms->size() / 2);
                }
                EXPECT_GT(sum, 0u);
                break;
            }
            case Teardown::ModelHandles:
                // each Model takes its nodes along, then its own entity (in the order the standard
                // library's clear() destroys them: libc++ goes from the back)
                crowd.models.clear();
                break;
            case Teardown::ModelHandlesNewestFirst:
                // the last copy first: each root's next slots are the holes its successors left
                while (!crowd.models.empty()) crowd.models.pop_back();
                break;
            case Teardown::DestroyImmediate:
                // GameObject.destroyImmediate: each copy's hierarchy collected and destroyed
                for (const auto& model : crowd.models) ObjectManager::destroyImmediate(crowd.scene.get(), model->getEntity());
                break;
            case Teardown::DeleteScene:
                crowd.scene.reset();
                break;
            case Teardown::Clear:
                crowd.scene->clear();
                break;
        }
        const double seconds = secondsSince(start);
        if (crowd.scene && teardown != Teardown::Clear) {
            EXPECT_EQ(crowd.scene->getEntityList(), std::vector<Entity>{last}) << name(teardown);
            EXPECT_EQ(crowd.scene->getComponentArray<Transform>()->size(), 1u);
            EXPECT_EQ(crowd.scene->getComponentArray<Transform>()->getEntity(0), last);
            EXPECT_EQ(crowd.scene->getComponentArray<MeshComponent>()->size(), 0u);
        }
        if (teardown == Teardown::Clear) {
            EXPECT_TRUE(crowd.scene->getEntityList().empty());
            EXPECT_EQ(crowd.scene->getComponentArray<Transform>()->size(), 0u);
        }
        std::printf("[destroy] %-26s %7zu entities: %8.3f s (%.3f us an entity; made in %.2f s)\n", name(teardown), entities, seconds,
                    seconds * 1e6 / entities, making);
        return seconds / entities;
    }

    // the best of `runs`: what noise (another process on the machine) adds is never less
    double bestSecondsPerEntity(int copies, Teardown teardown, int runs) {
        double best = 1e30;
        for (int i = 0; i < runs; i++) best = std::min(best, teardownSecondsPerEntity(copies, teardown));
        return best;
    }

}

// What one destroyed entity costs with 100,000 in the scene is about what it costs with 10,000
// (it was 10 times more and more: the arrays shifted each time); a ratio bound, robust to noise.
// Not with a read by index after each destroy: S07's arrays close their holes at the first index
// observation (docs/specs/S07-ecs.md R2, D1: remove, read, remove, read costs what an eager erase
// did, and is never worse), where upstream's answered reads over the holes with a rank tree
TEST(EntityDestroyScaling, PerEntityCostDoesNotGrowWithTheScene) {
    const int small = 10000 / ENTITIES_PER_COPY + 1;
    const int large = 100000 / ENTITIES_PER_COPY + 1;
    for (Teardown teardown : {Teardown::DestroyEach, Teardown::DestroyEachBackwards, Teardown::ModelHandles,
                              Teardown::ModelHandlesNewestFirst, Teardown::DestroyImmediate, Teardown::DeleteScene, Teardown::Clear}) {
        const double smallCost = bestSecondsPerEntity(small, teardown, 3);
        const double largeCost = bestSecondsPerEntity(large, teardown, 2);
        // the cache misses of 10 times the memory grow it a little; quadratic would be 10 times
        EXPECT_LT(largeCost, smallCost * 3.5 + 0.2e-6) << name(teardown) << ": " << smallCost * 1e6 << " us an entity with 10,000, "
                                                     << largeCost * 1e6 << " us with 100,000";
    }
}

// JASPER_DESTROY_PROFILE=<copies> times the teardowns of one crowd of that size (for profiling;
// opt-in, DISABLED_ as the unit gate allows no skip: --gtest_also_run_disabled_tests)
TEST(EntityDestroyScaling, DISABLED_Profile) {
    const char* copies = std::getenv("JASPER_DESTROY_PROFILE");
    if (!copies) {
        GTEST_SKIP() << "set JASPER_DESTROY_PROFILE=<model copies>";
    }
    for (Teardown teardown : {Teardown::DestroyEach, Teardown::DestroyEachBackwards, Teardown::DestroyEachReadingByIndex,
                              Teardown::ModelHandles, Teardown::ModelHandlesNewestFirst, Teardown::DestroyImmediate,
                              Teardown::DeleteScene, Teardown::Clear}) {
        teardownSecondsPerEntity(std::atoi(copies), teardown);
    }
}

// ---- the arrays read as if every removal had shifted them

namespace {

    // A ComponentArray and the plain vector it must read as
    struct PackedMirror {
        ComponentArray<int> array;
        std::vector<Entity> order;
        std::vector<int> values;

        void insert(Entity entity, int value) {
            array.insert(entity, value);
            order.push_back(entity);
            values.push_back(value);
        }
        void remove(Entity entity) {
            array.remove(entity);
            const size_t index = std::find(order.begin(), order.end(), entity) - order.begin();
            order.erase(order.begin() + index);
            values.erase(values.begin() + index);
        }
        void moveTo(Entity entity, size_t index) {
            array.moveEntityToIndex(entity, index);
            const size_t from = std::find(order.begin(), order.end(), entity) - order.begin();
            const int value = values[from];
            order.erase(order.begin() + from);
            values.erase(values.begin() + from);
            order.insert(order.begin() + index, entity);
            values.insert(values.begin() + index, value);
        }
        // compared by entity first (no compaction), then by index
        void expectSame(const std::string& when) {
            for (size_t i = 0; i < order.size(); i++) {
                ASSERT_TRUE(array.hasEntity(order[i])) << when;
                ASSERT_EQ(*array.findComponent(order[i]), values[i]) << when;
            }
            std::vector<Entity> walked;
            array.forEach([&walked](Entity entity, int&) { walked.push_back(entity); return true; });
            ASSERT_EQ(walked, order) << when << ": forEach";
            if (!order.empty()) {
                std::vector<Entity> after;
                array.forEachAfter(order.front(), [&after](Entity entity, int&) { after.push_back(entity); return true; });
                ASSERT_EQ(after, std::vector<Entity>(order.begin() + 1, order.end())) << when << ": forEachAfter";
            }
            ASSERT_EQ(array.size(), order.size()) << when;
            for (size_t i = 0; i < order.size(); i++) {
                ASSERT_EQ(array.getEntity(i), order[i]) << when << " at " << i;
                ASSERT_EQ(array.getComponentFromIndex(i), values[i]) << when << " at " << i;
                ASSERT_EQ(array.getIndex(order[i]), i) << when;
            }
        }
    };

}

TEST(ComponentArrayHoles, RemovalsFromTheMiddleReadAsShiftedOnes) {
    PackedMirror m;
    for (Entity e = 1; e <= 10; e++) m.insert(e, int(e) * 10);
    m.remove(3);
    m.remove(4);
    m.remove(9);
    m.expectSame("after three removals");
    m.remove(1);
    m.insert(11, 110);
    m.remove(6);
    m.expectSame("removals around an insert");
}

TEST(ComponentArrayHoles, RemovingTheLastPopsItAndTheHolesBeforeIt) {
    PackedMirror m;
    for (Entity e = 1; e <= 6; e++) m.insert(e, int(e));
    m.remove(4);
    m.remove(5);
    m.remove(6); // 4 and 5 go with it
    m.insert(7, 7);
    m.remove(2);
    m.remove(7);
    m.remove(3);
    m.expectSame("popped");
    m.remove(1);
    m.expectSame("empty");
}

TEST(ComponentArrayHoles, ForEachAfterSkipsRunsOfHolesAndStopsWhenAsked) {
    ComponentArray<int> array;
    for (Entity e = 1; e <= 1000; e++) array.insert(e, int(e));
    // the ones after 10 removed last first: a growing run of holes after the one before each
    for (Entity e = 90; e > 10; e--) {
        array.remove(e);
        Entity next = NULL_ENTITY;
        array.forEachAfter(e - 1, [&next](Entity entity, int&) { next = entity; return false; });
        ASSERT_EQ(next, 91u) << "after " << e - 1;
    }
    std::vector<Entity> after;
    array.forEachAfter(10, [&after](Entity entity, int&) { after.push_back(entity); return after.size() < 3; });
    EXPECT_EQ(after, (std::vector<Entity>{91, 92, 93}));
    after.clear();
    array.forEachAfter(1000, [&after](Entity entity, int&) { after.push_back(entity); return true; });
    EXPECT_TRUE(after.empty());
    array.forEachAfter(50, [&after](Entity entity, int&) { after.push_back(entity); return true; });
    EXPECT_TRUE(after.empty()) << "a removed entity has nothing after it";
    EXPECT_EQ(array.size(), 920u);
    EXPECT_EQ(array.getEntity(10), 91u);
}

// Holes stay through inserts and reads by entity and close at the first read by index (S07 R2;
// an insert never moves a component, R6, so it does not close them as upstream's array did when
// half of it was holes: the scene's compaction points bound the growth, compactComponents)
TEST(ComponentArrayHoles, InsertIntoAnArrayHalfHolesClosesThem) {
    ComponentArray<int> array;
    for (Entity e = 1; e <= 200; e++) array.insert(e, int(e));
    array.remove(150);
    array.insert(201, 201); // one hole of 201 slots: left open
    EXPECT_TRUE(array.hasHoles());
    for (Entity e = 1; e <= 100; e++) array.remove(e);
    int* const first = array.findComponent(101);
    array.insert(202, 202); // 101 holes of 202 slots: still open, nothing moved
    EXPECT_TRUE(array.hasHoles());
    EXPECT_EQ(array.findComponent(101), first) << "an insert moves nothing";
    EXPECT_EQ(array.size(), 101u);
    EXPECT_EQ(array.getIndex(200), 98u); // the read by index closes them
    EXPECT_FALSE(array.hasHoles());
    EXPECT_EQ(*array.findComponent(200), 200);
    EXPECT_EQ(array.getIndex(200), 98u);
    EXPECT_EQ(array.getIndex(202), 100u);
    EXPECT_EQ(array.size(), 101u);
}

TEST(ComponentArrayHoles, MovesSortsAndBatchesSeeThePackedArray) {
    PackedMirror m;
    for (Entity e = 1; e <= 12; e++) m.insert(e, int(e));
    m.remove(2);
    m.remove(5);
    m.moveTo(12, 1);
    m.expectSame("moved after holes");

    m.remove(7);
    m.array.sortByEntityOrder({12, 11, 10, 9, 8, 6, 4, 3, 1});
    m.order = {12, 11, 10, 9, 8, 6, 4, 3, 1};
    m.values.clear();
    for (Entity e : m.order) m.values.push_back(int(e));
    m.expectSame("sorted after a hole");

    m.array.entitiesDestroyed(std::unordered_set<Entity>{11, 8, 1, 99});
    for (Entity e : {11u, 8u, 1u}) {
        const size_t index = std::find(m.order.begin(), m.order.end(), e) - m.order.begin();
        m.order.erase(m.order.begin() + index);
        m.values.erase(m.values.begin() + index);
    }
    m.expectSame("several at once");

    // sorted by another array that has holes of its own
    ComponentArray<int> other;
    for (Entity e : {100u, 3u, 101u, 4u, 6u, 9u, 10u, 12u}) other.insert(e, 0);
    other.remove(100);
    other.remove(101);
    m.array.sortByComponent(&other);
    m.order = {3, 4, 6, 9, 10, 12};
    m.values = {3, 4, 6, 9, 10, 12};
    m.expectSame("sorted by an array with holes");
}

TEST(ComponentArrayHoles, RandomOperationsMatchAPlainVector) {
    std::mt19937 random(1234);
    PackedMirror m;
    Entity next = 1;
    for (int step = 0; step < 4000; step++) {
        const int op = random() % 10;
        if (op < 4 || m.order.empty()) {
            m.insert(next, int(next) * 3);
            next++;
        } else if (op < 8) {
            m.remove(m.order[random() % m.order.size()]);
        } else if (op == 8) {
            m.moveTo(m.order[random() % m.order.size()], random() % m.order.size());
        } else {
            m.expectSame("step " + std::to_string(step));
            if (::testing::Test::HasFatalFailure()) return;
        }
    }
    m.expectSame("end");
}

// Many short random runs (inserts, removals one by one and in batches, moves, reads by index
// and by entity, compactions): the indexes always read as the packed array, and a pointer taken
// by entity once the array is settled stays on its component through every read (by index,
// walk). S07 closes holes at the first read by index (R2), so the pointers are taken after one
// (upstream's array answered reads over its holes and moved nothing even then)
TEST(ComponentArrayHoles, ReadsNeverMoveAComponent) {
    std::mt19937 random(4321);
    for (int round = 0; round < 1500; round++) {
        PackedMirror m;
        Entity next = 1;
        const int ops = 50 + int(random() % 400);
        for (int k = 0; k < ops; k++) {
            const int op = int(random() % 12);
            if (op <= 3 || m.order.empty()) {
                m.insert(next, int(next) * 7);
                next++;
            } else if (op <= 6) {
                if (random() % 4 == 0) {
                    std::unordered_set<Entity> doomed;
                    const size_t count = 1 + random() % std::max<size_t>(1, m.order.size() / 2);
                    for (size_t q = 0; q < count; q++) doomed.insert(m.order[random() % m.order.size()]);
                    m.array.entitiesDestroyed(doomed);
                    for (size_t i = 0; i < m.order.size();) {
                        if (doomed.count(m.order[i])) {
                            m.order.erase(m.order.begin() + i);
                            m.values.erase(m.values.begin() + i);
                        } else {
                            i++;
                        }
                    }
                } else {
                    m.remove(m.order[random() % m.order.size()]);
                }
            } else if (op <= 9) {
                // pointers by entity, then every read by index: none of them moved
                ASSERT_EQ(m.array.getIndex(m.order.front()), 0u); // settles (S07 R2)
                std::vector<int*> pointers;
                for (Entity e : m.order) pointers.push_back(m.array.findComponent(e));
                ASSERT_EQ(m.array.size(), m.order.size());
                for (size_t i = 0; i < m.order.size(); i++) {
                    ASSERT_EQ(m.array.getEntity(i), m.order[i]);
                    ASSERT_EQ(m.array.getIndex(m.order[i]), i);
                    ASSERT_EQ(&m.array.getComponentFromIndex(i), pointers[i]) << "round " << round << " op " << k;
                    ASSERT_EQ(m.array.findComponentFromIndex(i), pointers[i]);
                }
                EXPECT_EQ(m.array.findComponentFromIndex(m.order.size()), nullptr);
                if (!m.order.empty()) {
                    const size_t at = random() % m.order.size();
                    size_t seen = 0;
                    m.array.forEachAfter(m.order[at], [&](Entity entity, int& value) {
                        EXPECT_EQ(entity, m.order[at + 1 + seen]);
                        EXPECT_EQ(&value, pointers[at + 1 + seen]);
                        seen++;
                        return true;
                    });
                    EXPECT_EQ(seen, m.order.size() - at - 1);
                    // a move to where it is reorders nothing, and moves nothing
                    m.array.moveEntityToIndex(m.order[at], at);
                    EXPECT_EQ(m.array.findComponent(m.order[at]), pointers[at]);
                }
                for (size_t i = 0; i < m.order.size(); i++) ASSERT_EQ(*pointers[i], m.values[i]);
            } else if (op == 10) {
                m.moveTo(m.order[random() % m.order.size()], random() % m.order.size());
            } else {
                m.array.compact();
                EXPECT_FALSE(m.array.hasHoles());
            }
        }
        m.expectSame("round " + std::to_string(round));
        if (::testing::Test::HasFatalFailure()) return;
    }
}

// A component found by entity stays where it is through later removals of others and reads by
// entity; the first read by index closes the holes (S07 R2, upstream's array kept it in place
// until a compaction), and after it every read leaves it where it is
TEST(ComponentArrayHoles, ComponentFoundByEntityStaysThroughRemovalsOfOthersAndReads) {
    ComponentArray<int> array;
    for (Entity e = 1; e <= 5; e++) array.insert(e, int(e));
    int* fifth = array.findComponent(5);
    array.remove(1);
    array.remove(3);
    EXPECT_EQ(array.findComponent(5), fifth);
    EXPECT_EQ(*fifth, 5);
    EXPECT_EQ(array.size(), 3u);
    EXPECT_TRUE(array.hasHoles());
    EXPECT_EQ(array.getIndex(5), 2u);
    EXPECT_FALSE(array.hasHoles());
    fifth = array.findComponent(5);
    EXPECT_EQ(*fifth, 5);
    EXPECT_EQ(&array.getComponentFromIndex(2), fifth);
    EXPECT_EQ(array.getEntity(0), 2u);
    EXPECT_EQ(array.findComponent(5), fifth) << "reads by index of a settled array moved it";
    array.compact();
    EXPECT_EQ(*array.findComponent(5), 5);
    EXPECT_EQ(array.getIndex(5), 2u);
}

// ---- the registry's hierarchy bookkeeping, with holes around

namespace {

    Entity transformEntity(Scene& scene, Entity parent = NULL_ENTITY) {
        const Entity entity = scene.createEntity();
        scene.addComponent<Transform>(entity, Transform());
        if (parent != NULL_ENTITY) scene.addEntityChild(parent, entity, false);
        return entity;
    }

    bool revisionMovesWhenDestroying(Scene& scene, Entity entity) {
        const uint64_t before = scene.hierarchyRevision();
        scene.destroyEntity(entity);
        return scene.hierarchyRevision() != before;
    }

}

TEST(EntityDestroyHierarchy, RootWithAChildLeftAfterHolesIsStillInTheHierarchy) {
    Scene scene;
    const Entity parent = transformEntity(scene);
    const Entity first = transformEntity(scene, parent);
    const Entity second = transformEntity(scene, parent);
    const Entity other = transformEntity(scene);
    EXPECT_TRUE(revisionMovesWhenDestroying(scene, first));
    // `second` follows the hole `first` left: still the parent's child
    EXPECT_TRUE(revisionMovesWhenDestroying(scene, parent));
    std::vector<Entity> renamed, destroyed;
    bool overflow = false;
    scene.takeHierarchyLog(renamed, destroyed, overflow);
    EXPECT_EQ(destroyed, (std::vector<Entity>{first, parent}));
    (void)second;
    (void)other;
}

TEST(EntityDestroyHierarchy, RootWhoseChildrenWentFirstIsALoneEntity) {
    Scene scene;
    const Entity parent = transformEntity(scene);
    const Entity first = transformEntity(scene, parent);
    const Entity second = transformEntity(scene, parent);
    const Entity other = transformEntity(scene);
    EXPECT_TRUE(revisionMovesWhenDestroying(scene, second));
    EXPECT_TRUE(revisionMovesWhenDestroying(scene, first));
    // the holes after it are skipped: `other` is next, not its child
    EXPECT_FALSE(revisionMovesWhenDestroying(scene, parent));
    EXPECT_FALSE(revisionMovesWhenDestroying(scene, other));
}

TEST(EntityDestroyHierarchy, RemainingSiblingsKeepTheirOrderAndQueriesTheirAnswers) {
    Scene scene;
    const Entity root = transformEntity(scene);
    std::vector<Entity> children;
    for (int i = 0; i < 6; i++) children.push_back(transformEntity(scene, root));
    const Entity grandchild = transformEntity(scene, children[4]);
    const Entity mesh = children[5];
    scene.addComponent<MeshComponent>(mesh, MeshComponent());
    const Entity tail = transformEntity(scene);
    scene.addComponent<MeshComponent>(tail, MeshComponent());

    scene.destroyEntity(children[1]);
    scene.destroyEntity(children[3]);
    // before any access by index: by entity and the parent walks
    EXPECT_TRUE(scene.isParentOf(root, grandchild));
    EXPECT_EQ(scene.findOldestParent(grandchild), root);
    EXPECT_EQ(scene.findBranchLastIndex(root), 5u);

    auto transforms = scene.getComponentArray<Transform>();
    const std::vector<Entity> expected{root, children[0], children[2], children[4], grandchild, children[5], tail};
    ASSERT_EQ(transforms->size(), expected.size());
    for (size_t i = 0; i < expected.size(); i++) EXPECT_EQ(transforms->getEntity(i), expected[i]) << i;

    // a move after more destroys keeps the meshes in the Transforms' order
    scene.destroyEntity(children[0]);
    scene.moveChildToTop(children[2]);
    const std::vector<Entity> moved{root, children[4], grandchild, children[5], children[2], tail};
    ASSERT_EQ(transforms->size(), moved.size());
    for (size_t i = 0; i < moved.size(); i++) EXPECT_EQ(transforms->getEntity(i), moved[i]) << i;
    auto meshes = scene.getComponentArray<MeshComponent>();
    ASSERT_EQ(meshes->size(), 2u);
    EXPECT_EQ(meshes->getEntity(0), mesh);
    EXPECT_EQ(meshes->getEntity(1), tail);
}

// The removal callbacks still come entity by entity, in the order they are destroyed, and
// see the scene as before (the destroyed entity's components there, the earlier ones gone)
TEST(EntityDestroyHierarchy, CallbacksComeInDestroyOrderAndSeeTheEarlierOnesGone) {
    class Recording : public EntityRegistry {
    public:
        std::vector<Entity> order;
        std::vector<size_t> transformsSeen;
        ~Recording() override = default;

    protected:
        void onEntityDestroyed(Entity entity, Signature) override {
            order.push_back(entity);
            EXPECT_NE(findComponent<Transform>(entity), nullptr);
            transformsSeen.push_back(getComponentArray<Transform>()->size());
        }
    };
    Recording registry;
    std::vector<Entity> entities;
    for (int i = 0; i < 5; i++) {
        entities.push_back(registry.createEntity());
        registry.addComponent<Transform>(entities.back(), Transform());
    }
    registry.destroyEntity(entities[1]);
    registry.destroyEntity(entities[3]);
    registry.destroyEntity(entities[0]);
    EXPECT_EQ(registry.order, (std::vector<Entity>{entities[1], entities[3], entities[0]}));
    EXPECT_EQ(registry.transformsSeen, (std::vector<size_t>{5, 4, 3}));
}

// GameObject.destroyImmediate collects a hierarchy without compacting the arrays, and finds
// the same entities with holes before and inside the branch as without
TEST(EntityDestroyHierarchy, CollectHierarchyWalksPastHoles) {
    Scene scene;
    const Entity before = transformEntity(scene);
    const Entity root = transformEntity(scene);
    const Entity a = transformEntity(scene, root);
    const Entity gone = transformEntity(scene, root);
    const Entity b = transformEntity(scene, root);
    const Entity c = transformEntity(scene, b);
    const Entity after = transformEntity(scene);
    const Entity action = scene.createEntity();
    scene.addComponent<ActionComponent>(action, ActionComponent());
    scene.getComponent<ActionComponent>(action).target = c;
    const Entity goneAction = scene.createEntity();
    scene.addComponent<ActionComponent>(goneAction, ActionComponent());

    scene.destroyEntity(before);
    scene.destroyEntity(gone);
    scene.destroyEntity(goneAction);
    EXPECT_EQ(EntityCloner::collectHierarchy(&scene, root), (std::vector<Entity>{root, a, b, c, action}));
    (void)after;
}

// ---- the editor's delete of many: locked parents indexed once give getLockedEntityParent's answers

TEST(EntityDestroyEditor, LockedParentIndexAnswersAsTheScan) {
    Scene scene;
    auto entity = [&scene](Entity parent = NULL_ENTITY) {
        return transformEntity(scene, parent);
    };
    auto plain = [&scene]() { return scene.createEntity(); };

    // two models; the second also names the first one's mesh node (the first model answers)
    const Entity modelA = entity();
    scene.addComponent<ModelComponent>(modelA, ModelComponent());
    const Entity skeleton = entity(modelA);
    const Entity bone = entity(skeleton);
    const Entity meshNode = entity(modelA);
    const Entity looseNode = entity(); // a node with no parent: the model answers
    const Entity animation = plain();
    scene.addComponent<AnimationComponent>(animation, AnimationComponent());
    const Entity track = plain();
    ActionFrame frame;
    frame.action = track;
    scene.getComponent<AnimationComponent>(animation).actions.push_back(frame);
    {
        ModelComponent& model = scene.getComponent<ModelComponent>(modelA);
        model.skeleton = skeleton;
        model.animations = {animation};
        model.bonesIdMapping[0] = bone;
        model.meshNodesMapping[1] = meshNode;
        model.nodesIdMapping[2] = looseNode;
    }
    const Entity modelB = entity();
    scene.addComponent<ModelComponent>(modelB, ModelComponent());
    const Entity nodeB = entity(modelB);
    {
        ModelComponent& model = scene.getComponent<ModelComponent>(modelB);
        model.nodesIdMapping[0] = nodeB;
        model.nodesIdMapping[1] = meshNode;
        model.animations = {track};
    }

    // UI parts: a button's label, a panel's header parts (one of them not below it)
    const Entity button = entity();
    scene.addComponent<ButtonComponent>(button, ButtonComponent());
    const Entity label = entity(button);
    scene.getComponent<ButtonComponent>(button).label = label;
    const Entity panel = entity();
    scene.addComponent<PanelComponent>(panel, PanelComponent());
    const Entity header = entity(panel);
    const Entity headerRoot = entity();
    scene.getComponent<PanelComponent>(panel).headercontainer = header;
    scene.getComponent<PanelComponent>(panel).headertext = headerRoot;
    const Entity unrelated = entity(panel);
    (void)unrelated;

    const editor::LockedParentIndex index(&scene);
    size_t locked = 0;
    for (Entity e : scene.getEntityList()) {
        const Entity expected = editor::ProjectUtils::getLockedEntityParent(&scene, e);
        EXPECT_EQ(index.get(e), expected) << "entity " << e;
        locked += expected != NULL_ENTITY ? 1 : 0;
    }
    EXPECT_EQ(index.get(NULL_ENTITY), NULL_ENTITY);
    EXPECT_EQ(index.get(meshNode), modelA);
    EXPECT_EQ(index.get(looseNode), modelA);
    EXPECT_EQ(index.get(bone), skeleton);
    EXPECT_EQ(index.get(track), animation);
    EXPECT_EQ(index.get(label), button);
    EXPECT_EQ(index.get(header), panel);
    EXPECT_EQ(index.get(headerRoot), NULL_ENTITY) << "a panel part at the top is not locked (as the scan)";
    EXPECT_GE(locked, 8u);
}
