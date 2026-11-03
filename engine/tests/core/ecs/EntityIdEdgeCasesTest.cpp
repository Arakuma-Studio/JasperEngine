// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// EntityManager's paged id -> slot table (S07) at its edges: ids at and around page
// boundaries, ids far past any real scene's (a corrupt or hand-edited file), neighbours of a
// destroyed id, the entity list against isCreated, and every operation against a reference
// map. Upstream's perf-canvasgroup tests of its signature pages, on S07's manager (NULL_ENTITY,
// id 0, is never created; the manager is not copyable or movable).

#include "ecs/EntityManager.h"

#include <gtest/gtest.h>

#include <iterator>
#include <map>
#include <random>
#include <type_traits>
#include <utility>
#include <vector>

using namespace jasperengine;

namespace {
    // past where a scene's ids reach: the ids around it cross many SparseIndex pages
    constexpr Entity kFarLimit = Entity(1) << 24;

    Signature bits(std::initializer_list<int> positions){
        Signature signature;
        for (int position : positions) signature.set(position);
        return signature;
    }
}

TEST(EntityIdEdgeCases, CreateSetDestroy){
    EntityManager manager;
    const Entity system = manager.createSystemEntity();
    const Entity user = manager.createUserEntity();
    EXPECT_EQ(system, EntityManager::firstSystemEntity());
    EXPECT_EQ(user, EntityManager::firstUserEntity());
    EXPECT_TRUE(manager.isCreated(system));
    EXPECT_TRUE(manager.isCreated(user));
    EXPECT_FALSE(manager.isCreated(user + 1));
    EXPECT_EQ(manager.getSignature(user), Signature()) << "a new entity has no components";

    manager.setSignature(user, bits({0, 70, 127}));
    EXPECT_EQ(manager.getSignature(user), bits({0, 70, 127}));
    EXPECT_EQ(manager.getSignature(system), Signature()) << "a neighbour's signature is its own";

    manager.destroy(user);
    EXPECT_FALSE(manager.isCreated(user));
    EXPECT_EQ(manager.getSignature(user), Signature()) << "unknown: empty (and an error logged)";
    // an id destroyed and made again (recreateEntity, as undo does) starts empty
    EXPECT_TRUE(manager.recreateEntity(user));
    EXPECT_FALSE(manager.recreateEntity(user));
    EXPECT_TRUE(manager.isCreated(user));
    EXPECT_EQ(manager.getSignature(user), Signature());
}

TEST(EntityIdEdgeCases, PagesComeAndGoWithTheirEntities){
    EntityManager manager;
    // more than a few pages of ids, each with its own signature
    std::vector<Entity> entities;
    for (int i = 0; i < 2000; i++){
        entities.push_back(manager.createUserEntity());
        manager.setSignature(entities.back(), bits({i % 128}));
    }
    for (size_t i = 0; i < entities.size(); i++){
        ASSERT_EQ(manager.getSignature(entities[i]), bits({(int)(i % 128)})) << i;
    }
    // every entity gone: their pages too, and the ids read as never created
    for (Entity entity : entities) manager.destroy(entity);
    for (Entity entity : entities) ASSERT_FALSE(manager.isCreated(entity)) << entity;
    // the ids go on increasing; a destroyed id is not reused by createUserEntity
    const Entity next = manager.createUserEntity();
    EXPECT_EQ(next, entities.back() + 1);
    EXPECT_EQ(manager.getSignature(next), Signature());
    EXPECT_FALSE(manager.isCreated(entities.back()));
}

TEST(EntityIdEdgeCases, SetterOfAnUnknownEntityCreatesIt){
    // setSignature and setName of an unknown id create it (logging an error), as before
    EntityManager manager;
    manager.setSignature(5000, bits({3}));
    EXPECT_TRUE(manager.isCreated(5000));
    EXPECT_EQ(manager.getSignature(5000), bits({3}));
    manager.setName(6000, "named");
    EXPECT_TRUE(manager.isCreated(6000));
    EXPECT_EQ(manager.getSignature(6000), Signature());
    // and createUserEntity skips them
    manager.setLastUserEntity(4999);
    EXPECT_EQ(manager.createUserEntity(), 5001u);
}

TEST(EntityIdEdgeCases, HugeIdsUseTheMapOnly){
    // ids far past a scene's (a corrupt or hand-edited scene) work
    EntityManager manager;
    const Entity huge = 4000000000u;
    EXPECT_TRUE(manager.recreateEntity(huge));
    EXPECT_TRUE(manager.isCreated(huge));
    manager.setSignature(huge, bits({1, 2}));
    EXPECT_EQ(manager.getSignature(huge), bits({1, 2}));
    manager.setLastUserEntity(huge);
    const Entity after = manager.createUserEntity();
    EXPECT_EQ(after, huge + 1);
    EXPECT_TRUE(manager.isCreated(after));
    manager.destroy(huge);
    EXPECT_FALSE(manager.isCreated(huge));
    EXPECT_TRUE(manager.isCreated(after));
    EXPECT_EQ(manager.getEntityList(), std::vector<Entity>{after});
}

TEST(EntityIdEdgeCases, BoundaryIds){
    EntityManager manager;
    const Entity limit = kFarLimit;
    for (Entity entity : {Entity(1), limit - 1, limit}){
        EXPECT_FALSE(manager.isCreated(entity)) << entity;
        EXPECT_TRUE(manager.recreateEntity(entity)) << entity;
        EXPECT_TRUE(manager.isCreated(entity)) << entity;
        manager.setSignature(entity, bits({(int)(entity % 128)}));
    }
    for (Entity entity : {Entity(1), limit - 1, limit}){
        EXPECT_EQ(manager.getSignature(entity), bits({(int)(entity % 128)})) << entity;
    }
    EXPECT_FALSE(manager.isCreated(limit - 2));
    EXPECT_FALSE(manager.isCreated(limit + 1));
    for (Entity entity : {Entity(1), limit - 1, limit}){
        manager.destroy(entity);
        EXPECT_FALSE(manager.isCreated(entity)) << entity;
    }
    EXPECT_TRUE(manager.getEntityList().empty());
}

TEST(EntityIdEdgeCases, DestroyingOneKeepsItsPageNeighbours){
    EntityManager manager;
    std::vector<Entity> entities;
    for (int i = 0; i < 200; i++){
        entities.push_back(manager.createUserEntity());
        manager.setSignature(entities.back(), bits({i % 128, 127 - i % 64}));
    }
    // one in the middle of a page, and the first and last of a page
    const Entity middle = entities[100];
    const Entity pageFirst = entities[0] + (64 - entities[0] % 64);
    const Entity pageLast = pageFirst + 63;
    for (Entity gone : {middle, pageFirst, pageLast}) manager.destroy(gone);
    for (size_t i = 0; i < entities.size(); i++){
        const Entity entity = entities[i];
        const bool gone = entity == middle || entity == pageFirst || entity == pageLast;
        ASSERT_EQ(manager.isCreated(entity), !gone) << entity;
        ASSERT_EQ(manager.getSignature(entity), gone ? Signature() : bits({(int)(i % 128), 127 - (int)(i % 64)})) << entity;
    }
    // made again on its live page: empty, the neighbours unchanged
    EXPECT_TRUE(manager.recreateEntity(middle));
    EXPECT_EQ(manager.getSignature(middle), Signature());
    EXPECT_EQ(manager.getSignature(middle - 1), bits({99 % 128, 127 - 99 % 64}));
    EXPECT_EQ(manager.getSignature(middle + 1), bits({101 % 128, 127 - 101 % 64}));
}

TEST(EntityIdEdgeCases, EntityListAgreesWithIsCreated){
    EntityManager manager;
    std::mt19937 rng(7);
    for (int i = 0; i < 3000; i++){
        const Entity entity = manager.createUserEntity();
        if (rng() % 3 == 0) manager.destroy(entity);
    }
    manager.recreateEntity(kFarLimit + 5);
    manager.createSystemEntity();
    const std::vector<Entity> list = manager.getEntityList();
    std::vector<bool> listed(6000, false);
    for (Entity entity : list){
        ASSERT_TRUE(manager.isCreated(entity)) << entity;
        if (entity < listed.size()) listed[entity] = true;
    }
    for (Entity entity = 0; entity < listed.size(); entity++){
        ASSERT_EQ(manager.isCreated(entity), (bool)listed[entity]) << entity;
    }
}

// every operation against a reference std::map (seeded)
TEST(EntityIdEdgeCases, RandomizedAgainstAReferenceMap){
    std::mt19937 rng(12345);
    const Entity limit = kFarLimit;
    const std::vector<Entity> special = {1, 2, 62, 63, 64, 65, 127, 128, 999, 1000, 1001, 1023, 1024, 1025, 65535, 65536,
                                         limit - 65, limit - 64, limit - 63, limit - 2, limit - 1, limit, limit + 1, limit + 63,
                                         limit + 64, 0xFFFFFFFEu, 0xFFFFFFFFu};
    int failures = 0;
    auto verify = [&](const EntityManager& manager, const std::map<Entity, Signature>& reference, const std::vector<Entity>& probes){
        for (const auto& [entity, signature] : reference){
            if (!manager.isCreated(entity) || manager.getSignature(entity) != signature){
                ADD_FAILURE() << "live " << entity;
                failures++;
            }
        }
        for (Entity entity : probes){
            const bool live = reference.count(entity) > 0;
            // a dead id's empty signature is CreateSetDestroy's (each read logs an error)
            if (manager.isCreated(entity) != live){
                ADD_FAILURE() << "probe " << entity << " expected created " << live;
                failures++;
            }
        }
        if (manager.getEntityList().size() != reference.size()){
            ADD_FAILURE() << "list " << manager.getEntityList().size() << " vs " << reference.size();
            failures++;
        }
    };
    for (int round = 0; round < 8 && failures == 0; round++){
        EntityManager manager;
        std::map<Entity, Signature> reference;
        std::vector<Entity> probes = special;
        auto pick = [&]() -> Entity {
            const int kind = rng() % 4;
            if (kind == 0) return special[rng() % special.size()];
            if (kind == 1 && !reference.empty()){
                auto it = reference.begin();
                std::advance(it, rng() % reference.size());
                return it->first;
            }
            if (kind == 2) return 1000 + rng() % 3000;
            return (Entity)(1 + rng() % 1999); // NULL_ENTITY is never created
        };
        for (int op = 0; op < 5000 && failures == 0; op++){
            const int kind = rng() % 9;
            if (kind == 0){
                const Entity entity = manager.createUserEntity();
                if (entity != NULL_ENTITY){
                    EXPECT_FALSE(reference.count(entity)) << "user id reused " << entity;
                    reference[entity];
                    probes.push_back(entity);
                }
            }else if (kind == 1){
                const Entity entity = manager.createSystemEntity();
                if (entity != NULL_ENTITY){
                    EXPECT_FALSE(reference.count(entity)) << "system id reused " << entity;
                    reference[entity];
                }
            }else if (kind == 2){
                const Entity entity = pick();
                EXPECT_EQ(manager.recreateEntity(entity), !reference.count(entity)) << entity;
                reference[entity];
                probes.push_back(entity);
            }else if (kind == 3 || kind == 4){
                const Entity entity = pick();
                manager.destroy(entity);
                reference.erase(entity);
                probes.push_back(entity);
            }else if (kind == 5){
                // an unknown id now and then (the setters create it, logging an error)
                const Entity entity = pick();
                if (!reference.count(entity) && rng() % 50 != 0) continue;
                const Signature signature = bits({(int)(rng() % 128), (int)(rng() % 128)});
                manager.setSignature(entity, signature);
                reference[entity] = signature;
                probes.push_back(entity);
            }else if (kind == 6){
                const Entity entity = pick();
                if (!reference.count(entity) && rng() % 50 != 0) continue;
                manager.setName(entity, "n");
                reference[entity];
                probes.push_back(entity);
            }else if (kind == 7){
                const Entity entity = special[rng() % special.size()];
                if (entity != 0xFFFFFFFFu) manager.setLastUserEntity(entity);
            }
            if (op % 499 == 0) verify(manager, reference, probes);
        }
        verify(manager, reference, probes);
        // everything destroyed: every id reads as never created
        const std::map<Entity, Signature> all = reference;
        for (const auto& item : all){
            manager.destroy(item.first);
            reference.erase(item.first);
        }
        verify(manager, reference, probes);
    }
    static_assert(!std::is_copy_constructible_v<EntityManager>, "EntityManager is not copyable");
    static_assert(!std::is_move_constructible_v<EntityManager>, "nor movable (it owns its pages)");
}
