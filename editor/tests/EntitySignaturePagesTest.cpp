// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// EntityManager answers isCreated and getSignature from pages indexed by id (a mirror of its
// metadata map, which a tree search per entity made the Canvas Group pass and the systems'
// loops pay for): the mirror must agree with the map through every way an id comes and goes.

#include <gtest/gtest.h>

#include "ecs/EntityManager.h"

#include <vector>

using namespace jasperengine;

namespace {
    Signature bits(std::initializer_list<int> positions){
        Signature signature;
        for (int position : positions) signature.set(position);
        return signature;
    }
}

TEST(EntitySignaturePages, CreateSetDestroy){
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

TEST(EntitySignaturePages, PagesComeAndGoWithTheirEntities){
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

TEST(EntitySignaturePages, SetterOfAnUnknownEntityCreatesIt){
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

TEST(EntitySignaturePages, HugeIdsUseTheMapOnly){
    // ids past the paged range (a corrupt or hand-edited scene) work, without a page table
    // reaching them
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
