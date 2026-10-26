// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "ecs/EntityManager.h"

#include "LogCapture.h"

#include <gtest/gtest.h>

#include <vector>
#include "reference/MapEntityManager.h"
#include <algorithm>
#include <random>
#include <string>

using namespace jasperengine;
using namespace jasperengine::test;

TEST(EntityManager, UserEntitiesStartAfterTheSystemRange){
    EntityManager manager;
    EXPECT_EQ(EntityManager::firstSystemEntity(), 1u);
    EXPECT_EQ(EntityManager::lastSystemEntity(), 1000u);
    EXPECT_EQ(manager.createUserEntity(), 1001u);
    EXPECT_EQ(manager.createUserEntity(), 1002u);
    EXPECT_EQ(manager.getLastUserEntity(), 1002u);
}

TEST(EntityManager, UserIdsAreNotReusedAfterDestroy){
    EntityManager manager;
    const Entity first = manager.createUserEntity();
    const Entity second = manager.createUserEntity();
    manager.destroy(first);
    manager.destroy(second);
    EXPECT_EQ(manager.createUserEntity(), second + 1);
    EXPECT_FALSE(manager.isCreated(first));
}

TEST(EntityManager, SystemEntitiesTakeTheFirstFreeSlot){
    EntityManager manager;
    EXPECT_EQ(manager.createSystemEntity(), 1u);
    EXPECT_EQ(manager.createSystemEntity(), 2u);
    EXPECT_EQ(manager.createSystemEntity(), 3u);
    manager.destroy(2);
    EXPECT_EQ(manager.createSystemEntity(), 2u);
    EXPECT_EQ(manager.createSystemEntity(), 4u);
}

TEST(EntityManager, SystemRangeRunsOut){
    EntityManager manager;
    for (int i = 0; i < 1000; i++)
        ASSERT_NE(manager.createSystemEntity(), Entity(NULL_ENTITY));
    LogCapture log;
    EXPECT_EQ(manager.createSystemEntity(), Entity(NULL_ENTITY));
    EXPECT_EQ(log.count(D_LOG_ERROR, "No free system entity"), 1u);
}

TEST(EntityManager, UserEntitiesSkipRecreatedIds){
    EntityManager manager;
    EXPECT_TRUE(manager.recreateEntity(1001));
    EXPECT_FALSE(manager.recreateEntity(1001));
    EXPECT_EQ(manager.createUserEntity(), 1002u);
}

TEST(EntityManager, SetLastUserEntityMovesTheCounter){
    EntityManager manager;
    manager.setLastUserEntity(5000);
    EXPECT_EQ(manager.createUserEntity(), 5001u);
    manager.setLastUserEntity(10);
    EXPECT_EQ(manager.getLastUserEntity(), 1000u);
}

TEST(EntityManager, EntityListIsSortedById){
    EntityManager manager;
    manager.createUserEntity();
    manager.createSystemEntity();
    manager.createUserEntity();
    EXPECT_EQ(manager.getEntityList(), (std::vector<Entity>{1, 1001, 1002}));
    Entity first = NULL_ENTITY;
    EXPECT_TRUE(manager.getFirstEntity(first));
    EXPECT_EQ(first, 1u);
}

TEST(EntityManager, FirstEntityOfAnEmptyManager){
    EntityManager manager;
    Entity first = 77;
    EXPECT_FALSE(manager.getFirstEntity(first));
    EXPECT_EQ(first, 77u);
}

TEST(EntityManager, SignaturesAreStoredPerEntity){
    EntityManager manager;
    const Entity e = manager.createUserEntity();
    Signature signature;
    signature.set(3);
    manager.setSignature(e, signature);
    EXPECT_TRUE(manager.getSignature(e).test(3));
    EXPECT_EQ(manager.getSignature(e).count(), 1u);
}

TEST(EntityManager, SignatureOfAMissingEntityIsEmptyAndAnError){
    EntityManager manager;
    LogCapture log;
    EXPECT_TRUE(manager.getSignature(4242).none());
    EXPECT_EQ(log.count(D_LOG_ERROR, "does not exist"), 1u);
}

TEST(EntityManager, NamesAndFindByNameTakeTheLowestId){
    EntityManager manager;
    const Entity a = manager.createUserEntity();
    const Entity b = manager.createUserEntity();
    manager.setName(a, "Box");
    manager.setName(b, "Box");
    EXPECT_EQ(manager.getName(b), "Box");
    EXPECT_EQ(manager.findByName("Box"), a);
    EXPECT_EQ(manager.findByName("Box", [a](Entity e){ return e != a; }), b);
    EXPECT_EQ(manager.findByName("Missing"), Entity(NULL_ENTITY));
}

TEST(EntityManager, TagsDefaultToUntagged){
    EntityManager manager;
    const Entity e = manager.createUserEntity();
    EXPECT_EQ(manager.getTag(e), "Untagged");
    manager.setTag(e, "Player");
    EXPECT_EQ(manager.getTag(e), "Player");
    manager.setTag(e, "");
    EXPECT_EQ(manager.getTag(e), "Untagged");
    EXPECT_EQ(manager.getTag(999999), "Untagged");
}

TEST(EntityManager, FindByTag){
    EntityManager manager;
    const Entity a = manager.createUserEntity();
    const Entity b = manager.createUserEntity();
    const Entity c = manager.createUserEntity();
    manager.setTag(a, "Enemy");
    manager.setTag(c, "Enemy");
    EXPECT_EQ(manager.findByTag("Enemy"), a);
    EXPECT_EQ(manager.findAllByTag("Enemy"), (std::vector<Entity>{a, c}));
    EXPECT_EQ(manager.findAllByTag("Untagged"), (std::vector<Entity>{b}));
}

TEST(EntityManager, LayersAreClampedToZeroToThirtyOne){
    EntityManager manager;
    const Entity e = manager.createUserEntity();
    EXPECT_EQ(manager.getLayer(e), 0);
    manager.setLayer(e, 5);
    EXPECT_EQ(manager.getLayer(e), 5);
    LogCapture log;
    manager.setLayer(e, 40);
    EXPECT_EQ(manager.getLayer(e), 31);
    manager.setLayer(e, -2);
    EXPECT_EQ(manager.getLayer(e), 0);
    EXPECT_EQ(log.count(D_LOG_WARN, "out of range"), 2u);
}

TEST(EntityManager, ActiveFlagCountsInactiveEntities){
    EntityManager manager;
    const Entity e = manager.createUserEntity();
    EXPECT_TRUE(manager.isActive(e));
    EXPECT_FALSE(manager.hasInactive());
    EXPECT_TRUE(manager.setActive(e, false));
    EXPECT_FALSE(manager.setActive(e, false));
    EXPECT_FALSE(manager.isActive(e));
    EXPECT_TRUE(manager.hasInactive());
    EXPECT_TRUE(manager.setActive(e, true));
    EXPECT_FALSE(manager.hasInactive());
}

TEST(EntityManager, DestroyingAnInactiveEntityForgetsIt){
    EntityManager manager;
    const Entity e = manager.createUserEntity();
    manager.setActive(e, false);
    manager.destroy(e);
    EXPECT_FALSE(manager.hasInactive());
    EXPECT_TRUE(manager.isActive(e));
}

TEST(EntityManager, DestroyingAMissingEntityDoesNothing){
    EntityManager manager;
    manager.createUserEntity();
    manager.destroy(5);
    EXPECT_EQ(manager.getEntityList().size(), 1u);
}

// ---------------------------------------------------------------------------- S07: dense metadata behind a paged id table
// EntityManager: dense metadata behind a paged id table must answer exactly as the std::map
// manager it replaced (reference/MapEntityManager.h): ascending entity lists, lowest-id first
// matches, the lowest free system id, monotonic user ids, recreateEntity and setLastUserEntity.

namespace {

    void expectSame(const EntityManager& manager, const reference::MapEntityManager& model){
        ASSERT_EQ(manager.getEntityList(), model.getEntityList());
        ASSERT_EQ(manager.getLastUserEntity(), model.getLastUserEntity());
        for (const std::string& name : {std::string("a"), std::string("b"), std::string(""), std::string("missing")}){
            ASSERT_EQ(manager.findByName(name), model.findByName(name)) << name;
        }
        for (const std::string& tag : {std::string("Untagged"), std::string("Enemy"), std::string("Player"), std::string("Nope")}){
            ASSERT_EQ(manager.findByTag(tag), model.findByTag(tag)) << tag;
            ASSERT_EQ(manager.findAllByTag(tag), model.findAllByTag(tag)) << tag;
        }
        for (Entity entity : model.getEntityList()){
            ASSERT_EQ(manager.getName(entity), model.getName(entity));
            ASSERT_EQ(manager.getTag(entity), model.getTag(entity));
            ASSERT_EQ(manager.getLayer(entity), model.getLayer(entity));
            ASSERT_EQ(manager.isActive(entity), model.isActive(entity));
            ASSERT_EQ(manager.getSignature(entity).to_string().substr(256 - 64), model.getSignature(entity).to_string());
        }
        ASSERT_EQ(manager.hasInactive(), model.hasInactive());
        Entity first = 0, modelFirst = 0;
        ASSERT_EQ(manager.getFirstEntity(first), model.getFirstEntity(modelFirst));
        ASSERT_EQ(first, modelFirst);
    }

}

TEST(EntityManagerTest, DifferentialAgainstMapModel){
    const char* names[] = {"a", "b", ""};
    const char* tags[] = {"Untagged", "Enemy", "Player", ""};
    for (unsigned seed = 1; seed <= 50; seed++){
        std::mt19937 random(seed);
        EntityManager manager;
        reference::MapEntityManager model;
        std::vector<Entity> live;
        for (int step = 0; step < 1500; step++){
            const unsigned op = random() % 100;
            if (op < 25){
                const Entity a = manager.createUserEntity();
                const Entity b = model.createUserEntity();
                ASSERT_EQ(a, b);
                live.push_back(a);
            }else if (op < 40){
                const Entity a = manager.createSystemEntity();
                const Entity b = model.createSystemEntity();
                ASSERT_EQ(a, b);
                if (a != NULL_ENTITY) live.push_back(a);
            }else if (op < 55){
                if (!live.empty()){
                    const size_t i = random() % live.size();
                    manager.destroy(live[i]);
                    model.destroy(live[i]);
                    live.erase(live.begin() + (ptrdiff_t)i);
                }
            }else if (op < 62){
                const Entity entity = 1 + random() % 2000;
                ASSERT_EQ(manager.recreateEntity(entity), model.recreateEntity(entity));
                if (std::find(live.begin(), live.end(), entity) == live.end()) live.push_back(entity);
            }else if (op < 65){
                const Entity entity = random() % 3000;
                manager.setLastUserEntity(entity);
                model.setLastUserEntity(entity);
            }else if (!live.empty()){
                const Entity entity = live[random() % live.size()];
                if (op < 75){
                    const char* name = names[random() % 3];
                    manager.setName(entity, name);
                    model.setName(entity, name);
                }else if (op < 83){
                    const char* tag = tags[random() % 4];
                    manager.setTag(entity, tag);
                    model.setTag(entity, tag);
                }else if (op < 88){
                    const int layer = (int)(random() % 40) - 4;
                    manager.setLayer(entity, layer);
                    model.setLayer(entity, layer);
                }else if (op < 94){
                    const bool active = random() % 2;
                    ASSERT_EQ(manager.setActive(entity, active), model.setActive(entity, active));
                }else{
                    const size_t bit = random() % 64;
                    Signature signature = manager.getSignature(entity);
                    signature.set(bit);
                    manager.setSignature(entity, signature);
                    reference::Signature legacy = model.getSignature(entity);
                    legacy.set(bit);
                    model.setSignature(entity, legacy);
                }
            }
            if (step % 50 == 0){
                expectSame(manager, model);
                if (::testing::Test::HasFatalFailure()) FAIL() << "seed " << seed << " step " << step;
            }
        }
        expectSame(manager, model);
        if (::testing::Test::HasFatalFailure()) FAIL() << "seed " << seed;
    }
}

TEST(EntityManagerTest, AscendingEntityList){
    EntityManager manager;
    for (Entity entity : {5000u, 1003u, 7u, 70000u, 1u}){
        manager.recreateEntity(entity);
    }
    EXPECT_EQ(manager.getEntityList(), (std::vector<Entity>{1, 7, 1003, 5000, 70000}));
    std::vector<Entity> visited;
    manager.forEachEntity([&visited](Entity entity){ visited.push_back(entity); });
    EXPECT_EQ(visited, manager.getEntityList());
}

TEST(EntityManagerTest, LowestIdFirstMatch){
    EntityManager manager;
    manager.recreateEntity(9000);
    manager.recreateEntity(1200);
    manager.recreateEntity(30);
    for (Entity entity : {9000u, 1200u, 30u}){
        manager.setName(entity, "Twin");
        manager.setTag(entity, "Enemy");
    }
    EXPECT_EQ(manager.findByName("Twin"), (Entity)30);
    EXPECT_EQ(manager.findByName("Twin", [](Entity e){ return e > 100; }), (Entity)1200);
    EXPECT_EQ(manager.findByTag("Enemy"), (Entity)30);
    EXPECT_EQ(manager.findAllByTag("Enemy"), (std::vector<Entity>{30, 1200, 9000}));
    EXPECT_EQ(manager.findByTag("Untagged"), NULL_ENTITY);
    EXPECT_EQ(manager.getTag(30), "Enemy");
    manager.setTag(30, "");
    EXPECT_EQ(manager.getTag(30), "Untagged");
}

TEST(EntityManagerTest, LowestFreeSystemIdAfterRandomFrees){
    EntityManager manager;
    for (Entity expected = 1; expected <= 1000; expected++){
        EXPECT_EQ(manager.createSystemEntity(), expected);
    }
    EXPECT_EQ(manager.createSystemEntity(), NULL_ENTITY);

    std::mt19937 random(7);
    std::vector<Entity> freed;
    for (int i = 0; i < 100; i++){
        const Entity entity = 1 + random() % 1000;
        if (manager.isCreated(entity)){
            manager.destroy(entity);
            freed.push_back(entity);
        }
    }
    std::sort(freed.begin(), freed.end());
    for (Entity expected : freed){
        EXPECT_EQ(manager.createSystemEntity(), expected);
    }
    EXPECT_EQ(manager.createSystemEntity(), NULL_ENTITY);
}

TEST(EntityManagerTest, MonotonicUserIds){
    EntityManager manager;
    const Entity a = manager.createUserEntity();
    const Entity b = manager.createUserEntity();
    EXPECT_EQ(a, (Entity)1001);
    EXPECT_EQ(b, (Entity)1002);
    manager.destroy(b);
    EXPECT_EQ(manager.createUserEntity(), (Entity)1003); // never reused
    manager.recreateEntity(1004);
    EXPECT_EQ(manager.createUserEntity(), (Entity)1005); // skips a taken id
}

TEST(EntityManagerTest, RecreateAndSetLastUserEntity){
    EntityManager manager;
    EXPECT_TRUE(manager.recreateEntity(1500));
    EXPECT_FALSE(manager.recreateEntity(1500));
    EXPECT_EQ(manager.getLastUserEntity(), (Entity)1000); // recreate is not bound by the counter
    manager.setLastUserEntity(1500);
    EXPECT_EQ(manager.createUserEntity(), (Entity)1501);
    manager.setLastUserEntity(10);
    EXPECT_EQ(manager.getLastUserEntity(), (Entity)1000);
    EXPECT_EQ(manager.createUserEntity(), (Entity)1001);
}

TEST(EntityManagerTest, SerialChangesWhenAnIdIsReused){
    EntityManager manager;
    const Entity entity = manager.createSystemEntity();
    const uint32_t serial = manager.getSerial(entity);
    EXPECT_NE(serial, 0u);
    manager.destroy(entity);
    EXPECT_EQ(manager.getSerial(entity), 0u);
    EXPECT_EQ(manager.createSystemEntity(), entity);
    EXPECT_NE(manager.getSerial(entity), serial);
}

TEST(EntityManagerTest, MetadataBytesPerEntity){
    EntityManager manager;
    for (int i = 0; i < 100000; i++){
        manager.createUserEntity();
    }
    // R27: at most 96 bytes per entity, names' heap buffers left out
    EXPECT_LE((double)manager.bytes() / 100000.0, 96.0);
}
