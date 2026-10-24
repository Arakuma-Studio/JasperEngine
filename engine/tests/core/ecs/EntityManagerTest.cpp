// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "ecs/EntityManager.h"

#include "LogCapture.h"

#include <gtest/gtest.h>

#include <vector>

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
