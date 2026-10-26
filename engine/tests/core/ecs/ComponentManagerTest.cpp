// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "ecs/ComponentManager.h"
#include "ecs/Signature.h"

#include "LogCapture.h"

#include <gtest/gtest.h>

#include <stdexcept>

using namespace jasperengine;
using namespace jasperengine::test;

namespace {
    struct Position { float x = 0; };
    struct Health { int hp = 100; };
    struct Unregistered { int unused = 0; };
}

TEST(ComponentManager, IdsAreSequentialInRegistrationOrder){
    ComponentManager manager;
    manager.registerComponent<Health>();
    manager.registerComponent<Position>();
    EXPECT_EQ(manager.getComponentId<Health>(), 0);
    EXPECT_EQ(manager.getComponentId<Position>(), 1);
    EXPECT_TRUE(manager.isRegistered<Position>());
    EXPECT_FALSE(manager.isRegistered<Unregistered>());
}

TEST(ComponentManager, RegisteringTwiceIsAnErrorAndKeepsTheId){
    ComponentManager manager;
    manager.registerComponent<Position>();
    LogCapture log;
    manager.registerComponent<Position>();
    EXPECT_EQ(log.count(D_LOG_ERROR, "more than once"), 1u);
    EXPECT_EQ(manager.getComponentId<Position>(), 0);
}

TEST(ComponentManager, UnregisteredTypesThrow){
    ComponentManager manager;
    LogCapture log;
    EXPECT_THROW(manager.getComponentId<Unregistered>(), std::out_of_range);
    EXPECT_THROW(manager.getComponentArray<Unregistered>(), std::out_of_range);
    EXPECT_EQ(log.count(D_LOG_ERROR, "not registered"), 2u);
}

TEST(ComponentManager, AddFindAndRemoveComponents){
    ComponentManager manager;
    manager.registerComponent<Health>();
    manager.addComponent<Health>(5, Health{40});
    EXPECT_EQ(manager.getComponent<Health>(5).hp, 40);
    EXPECT_EQ(manager.findComponentFromIndex<Health>(0)->hp, 40);
    manager.removeComponent<Health>(5);
    EXPECT_EQ(manager.findComponent<Health>(5), nullptr);
}

TEST(ComponentManager, EntityDestroyedReachesEveryArray){
    ComponentManager manager;
    manager.registerComponent<Health>();
    manager.registerComponent<Position>();
    manager.addComponent<Health>(1);
    manager.addComponent<Position>(1);
    manager.addComponent<Position>(2);
    manager.entityDestroyed(1);
    EXPECT_EQ(manager.findComponent<Health>(1), nullptr);
    EXPECT_EQ(manager.findComponent<Position>(1), nullptr);
    EXPECT_NE(manager.findComponent<Position>(2), nullptr);
    manager.entitiesDestroyed({2});
    EXPECT_EQ(manager.getComponentArray<Position>()->size(), 0u);
}

TEST(ComponentManager, TypeIndicesAreStableAcrossManagers){
    ComponentManager first;
    first.registerComponent<Health>();
    ComponentManager second;
    second.registerComponent<Position>();
    second.registerComponent<Health>();
    // the dense type index is process-wide; the ids are per manager
    EXPECT_EQ(componentTypeIndex<Health>(), componentTypeIndex<Health>());
    EXPECT_NE(componentTypeIndex<Health>(), componentTypeIndex<Position>());
    EXPECT_EQ(first.getComponentId<Health>(), 0);
    EXPECT_EQ(second.getComponentId<Health>(), 1);
}

// A signature has one bit per component type: 256 since S07 (it was a std::bitset<64>); a
// position past the end is ignored, never undefined behaviour
TEST(Signature, HoldsTwoHundredFiftySixComponentTypes){
    Signature signature;
    EXPECT_EQ(signature.size(), 256u);
    signature.set(255);
    EXPECT_TRUE(signature.test(255));
    signature.set(256);
    EXPECT_FALSE(signature.test(256));
    EXPECT_EQ(signature.count(), 1u);
}
