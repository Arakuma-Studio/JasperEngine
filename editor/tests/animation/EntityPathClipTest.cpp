// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// EntityPath and the clip binding paths are ONE grammar (animation clips spec 2.2, 9.R39): the
// editor's scene path of a GameObject is its root's path + "/" + the clip path below the root
// (EntityPath::relative = anim::relativePath), both directions resolve to the same entity, for
// Mixamo names with ':', duplicate siblings (#N), unicode, spaces and '#' that is not an index,
// also after the child order changed. Names ending in "#<digits>" or containing '/' are
// unaddressable the same way everywhere (relative fails, descendantPaths skips, a reason).

#include <gtest/gtest.h>
#include "component/Transform.h"

#include "animation/ClipBinding.h"
#include "registry/EntityRegistry.h"
#include "util/EntityPath.h"

#include <algorithm>
#include <string>
#include <vector>

using namespace jasperengine;
using editor::EntityPath;

namespace {

    Entity add(EntityRegistry& registry, const std::string& name, Entity parent = NULL_ENTITY){
        Entity entity = registry.createEntity();
        registry.setEntityName(entity, name);
        registry.addComponent<Transform>(entity, {});
        if (parent != NULL_ENTITY) registry.addEntityChild(parent, entity, false);
        return entity;
    }

    // Level
    //   Hero
    //     mixamorig:Hips
    //       mixamorig:Spine
    //       mixamorig:LeftUpLeg
    //     Arm                (Arm)
    //       Hand
    //     Leg
    //     Arm                (Arm#1)
    //       Hand
    //     Arm                (Arm#2)
    //     Épaule 肩
    //       Left Hand
    //     Take #b
    //     Bone#1             (unaddressable)
    //       Tip
    //     a/b                (unaddressable)
    struct Rig{
        EntityRegistry registry;
        Entity level, hero, hips, spine, upLeg, arm0, hand0, leg, arm1, hand1, arm2, unicode, leftHand, take, boneIndex, tip, slash;
        Rig(){
            level = add(registry, "Level");
            hero = add(registry, "Hero", level);
            hips = add(registry, "mixamorig:Hips", hero);
            spine = add(registry, "mixamorig:Spine", hips);
            upLeg = add(registry, "mixamorig:LeftUpLeg", hips);
            arm0 = add(registry, "Arm", hero);
            hand0 = add(registry, "Hand", arm0);
            leg = add(registry, "Leg", hero);
            arm1 = add(registry, "Arm", hero);
            hand1 = add(registry, "Hand", arm1);
            arm2 = add(registry, "Arm", hero);
            unicode = add(registry, "\xC3\x89paule \xE8\x82\xA9", hero);
            leftHand = add(registry, "Left Hand", unicode);
            take = add(registry, "Take #b", hero);
            boneIndex = add(registry, "Bone#1", hero);
            tip = add(registry, "Tip", boneIndex);
            slash = add(registry, "a/b", hero);
        }
        std::vector<Entity> addressable() const{
            return {hero, hips, spine, upLeg, arm0, hand0, leg, arm1, hand1, arm2, unicode, leftHand, take};
        }
    };

    // the scene path and the clip path agree for every addressable entity under `root`
    void expectParity(EntityRegistry& registry, Entity root, const std::vector<Entity>& entities){
        const std::string rootPath = EntityPath::of(&registry, root);
        const std::vector<Entity> all = registry.getEntityList();
        for (Entity entity : entities){
            std::string relative;
            ASSERT_TRUE(EntityPath::relative(&registry, root, entity, relative)) << EntityPath::of(&registry, entity);
            std::string engine;
            ASSERT_TRUE(anim::relativePath(&registry, root, entity, engine));
            EXPECT_EQ(relative, engine);
            const std::string scenePath = EntityPath::of(&registry, entity);
            EXPECT_EQ(scenePath, entity == root ? rootPath : rootPath + "/" + relative);
            EXPECT_EQ(EntityPath::findRelative(&registry, root, relative), entity) << relative;
            EXPECT_EQ(anim::resolvePath(&registry, root, relative), entity) << relative;
            EXPECT_EQ(EntityPath::find(&registry, all, scenePath), entity) << scenePath;
        }
    }

}

TEST(EntityPathClip, RelativePathsAreTheClipGrammar){
    Rig rig;
    std::string path;
    ASSERT_TRUE(EntityPath::relative(&rig.registry, rig.hero, rig.hero, path));
    EXPECT_EQ(path, "");
    EXPECT_EQ(EntityPath::findRelative(&rig.registry, rig.hero, ""), rig.hero);

    ASSERT_TRUE(EntityPath::relative(&rig.registry, rig.hero, rig.spine, path));
    EXPECT_EQ(path, "mixamorig:Hips/mixamorig:Spine");
    ASSERT_TRUE(EntityPath::relative(&rig.registry, rig.hero, rig.arm1, path));
    EXPECT_EQ(path, "Arm#1");
    ASSERT_TRUE(EntityPath::relative(&rig.registry, rig.hero, rig.hand1, path));
    EXPECT_EQ(path, "Arm#1/Hand");
    ASSERT_TRUE(EntityPath::relative(&rig.registry, rig.hero, rig.arm2, path));
    EXPECT_EQ(path, "Arm#2");
    ASSERT_TRUE(EntityPath::relative(&rig.registry, rig.hero, rig.leftHand, path));
    EXPECT_EQ(path, "\xC3\x89paule \xE8\x82\xA9/Left Hand");
    ASSERT_TRUE(EntityPath::relative(&rig.registry, rig.hero, rig.take, path));
    EXPECT_EQ(path, "Take #b");

    expectParity(rig.registry, rig.hero, rig.addressable());
    // a root deeper than the scene root, and the scene root itself
    expectParity(rig.registry, rig.hips, {rig.hips, rig.spine, rig.upLeg});
    std::vector<Entity> fromLevel = rig.addressable();
    fromLevel.push_back(rig.level);
    expectParity(rig.registry, rig.level, fromLevel);

    // not under the root
    EXPECT_FALSE(EntityPath::relative(&rig.registry, rig.arm0, rig.leg, path));
    EXPECT_EQ(EntityPath::findRelative(&rig.registry, rig.hero, "Arm#3"), NULL_ENTITY);
    EXPECT_EQ(EntityPath::findRelative(&rig.registry, rig.hero, "Arm#"), NULL_ENTITY);
    EXPECT_EQ(EntityPath::findRelative(&rig.registry, rig.hero, "Nope/Hand"), NULL_ENTITY);
}

TEST(EntityPathClip, ParityAfterTheChildOrderChanges){
    Rig rig;
    auto armPaths = [&](){
        std::vector<std::string> paths;
        for (Entity arm : {rig.arm0, rig.arm1, rig.arm2}){
            std::string path;
            EXPECT_TRUE(EntityPath::relative(&rig.registry, rig.hero, arm, path));
            paths.push_back(path);
        }
        return paths;
    };
    const std::vector<std::string> before = armPaths();
    EXPECT_EQ(before, (std::vector<std::string>{"Arm", "Arm#1", "Arm#2"}));
    // the Arms change places: their "#N" change, in both grammars alike
    // (the registry's "bottom" is the first child, "top" the last)
    rig.registry.moveChildToBottom(rig.arm2);
    rig.registry.moveChildToTop(rig.arm0);
    const std::vector<std::string> after = armPaths();
    EXPECT_EQ(after, (std::vector<std::string>{"Arm#2", "Arm#1", "Arm"}));
    expectParity(rig.registry, rig.hero, rig.addressable());
    rig.registry.moveChildDown(rig.arm2);
    expectParity(rig.registry, rig.hero, rig.addressable());
    std::string path;

    // a reparent keeps both grammars equal too
    rig.registry.addEntityChild(rig.arm1, rig.leg, false);
    ASSERT_TRUE(EntityPath::relative(&rig.registry, rig.hero, rig.leg, path));
    expectParity(rig.registry, rig.hero, rig.addressable());
}

TEST(EntityPathClip, UnaddressableNamesAreRejectedTheSameWay){
    Rig rig;
    std::string path;
    for (Entity entity : {rig.boneIndex, rig.tip, rig.slash}){
        EXPECT_FALSE(EntityPath::relative(&rig.registry, rig.hero, entity, path)) << rig.registry.getEntityName(entity);
        EXPECT_FALSE(anim::relativePath(&rig.registry, rig.hero, entity, path));
    }
    EXPECT_FALSE(EntityPath::isAddressableName("Bone#1"));
    EXPECT_FALSE(EntityPath::isAddressableName("a/b"));
    EXPECT_FALSE(EntityPath::isAddressableName(""));
    EXPECT_TRUE(EntityPath::isAddressableName("Take #b"));
    EXPECT_TRUE(EntityPath::isAddressableName("mixamorig:Hips"));
    EXPECT_TRUE(EntityPath::unaddressableReason("mixamorig:Hips").empty());
    EXPECT_NE(EntityPath::unaddressableReason("Bone#1").find("sibling index"), std::string::npos);
    EXPECT_NE(EntityPath::unaddressableReason("a/b").find("'/'"), std::string::npos);
    EXPECT_FALSE(EntityPath::unaddressableReason("").empty());

    // "Bone#1" as a path means the second "Bone", never the GameObject named "Bone#1"
    EXPECT_EQ(EntityPath::findRelative(&rig.registry, rig.hero, "Bone#1"), NULL_ENTITY);
    const Entity bone = add(rig.registry, "Bone", rig.hero);
    const Entity bone1 = add(rig.registry, "Bone", rig.hero);
    EXPECT_EQ(EntityPath::findRelative(&rig.registry, rig.hero, "Bone"), bone);
    EXPECT_EQ(EntityPath::findRelative(&rig.registry, rig.hero, "Bone#1"), bone1);

    // the Add Property tree's listing skips them and what is under them, nothing else
    std::vector<std::string> listed;
    for (const auto& entry : anim::descendantPaths(&rig.registry, rig.hero)) listed.push_back(entry.first);
    for (const char* skipped : {"Bone#1", "Bone#1/Tip", "a/b"}){
        EXPECT_EQ(std::count(listed.begin(), listed.end(), skipped), 1 * (std::string(skipped) == "Bone#1"))
            << "only the second Bone is listed as Bone#1: " << skipped;
    }
    for (Entity entity : rig.addressable()){
        std::string relative;
        ASSERT_TRUE(EntityPath::relative(&rig.registry, rig.hero, entity, relative));
        EXPECT_EQ(std::count(listed.begin(), listed.end(), relative), 1) << relative;
    }
}
