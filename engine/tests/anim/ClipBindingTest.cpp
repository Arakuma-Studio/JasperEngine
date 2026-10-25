// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// ClipBinding (spec 2.2, 9.R9/R39): the path grammar (names joined by '/', "#N" = the N-th later
// same-named sibling, "" = the root), resolution in Transform child order, relative paths and
// descendant listings, and EntityRegistry::hierarchyRevision on every mutation path R9 lists
// (and not on a parentless spawn).

#include <gtest/gtest.h>

#include "animation/ClipBinding.h"
#include "registry/EntityRegistry.h"

using namespace jasperengine;

namespace {

    Entity add(EntityRegistry& registry, const std::string& name, Entity parent = NULL_ENTITY){
        Entity entity = registry.createEntity();
        registry.setEntityName(entity, name);
        registry.addComponent<Transform>(entity, {});
        if (parent != NULL_ENTITY) registry.addEntityChild(parent, entity, false);
        return entity;
    }

    // Root
    //   Arm
    //     Hand
    //   Arm        (Arm#1)
    //     Hand
    //   Leg
    struct Rig{
        EntityRegistry registry;
        Entity root, arm, hand, arm1, hand1, leg;
        Rig(){
            root = add(registry, "Root");
            arm = add(registry, "Arm", root);
            hand = add(registry, "Hand", arm);
            arm1 = add(registry, "Arm", root);
            hand1 = add(registry, "Hand", arm1);
            leg = add(registry, "Leg", root);
        }
    };

}

TEST(ClipBinding, ParseAndFormatPaths){
    std::vector<anim::PathSegment> segments;
    ASSERT_TRUE(anim::parsePath("", segments));
    EXPECT_TRUE(segments.empty());
    ASSERT_TRUE(anim::parsePath("Arm#2/Hand", segments));
    ASSERT_EQ(segments.size(), 2u);
    EXPECT_EQ(segments[0].name, "Arm");
    EXPECT_EQ(segments[0].index, 2);
    EXPECT_EQ(segments[1].name, "Hand");
    EXPECT_EQ(segments[1].index, 0);
    EXPECT_EQ(anim::formatPath(segments), "Arm#2/Hand");

    for (const char* canonical : {"", "A", "A/B/C", "Arm#1", "Take #b/Bone", "mixamorig:Hips/mixamorig:Spine#3"}){
        ASSERT_TRUE(anim::parsePath(canonical, segments)) << canonical;
        EXPECT_EQ(anim::formatPath(segments), canonical);
    }
    // "#0" is read but never written
    ASSERT_TRUE(anim::parsePath("Arm#0", segments));
    EXPECT_EQ(anim::formatPath(segments), "Arm");

    for (const char* bad : {"/", "A//B", "A/", "/A", "Arm#", "Arm#-1", "#3", "A#12345678901"}){
        EXPECT_FALSE(anim::parsePath(bad, segments)) << bad;
        EXPECT_TRUE(segments.empty());
    }

    EXPECT_EQ(anim::formatSegment("Arm", 0), "Arm");
    EXPECT_EQ(anim::formatSegment("Arm", 3), "Arm#3");
    EXPECT_TRUE(anim::isAddressableName("Arm"));
    EXPECT_TRUE(anim::isAddressableName("Take #b"));
    EXPECT_FALSE(anim::isAddressableName(""));
    EXPECT_FALSE(anim::isAddressableName("a/b"));
    EXPECT_FALSE(anim::isAddressableName("Bone#1"));
    EXPECT_FALSE(anim::isAddressableName("Bone#"));
    EXPECT_FALSE(anim::isAddressableName("Bone#-2"));
}

TEST(ClipBinding, ResolveWalksChildrenInOrder){
    Rig rig;
    EntityRegistry* r = &rig.registry;
    EXPECT_EQ(anim::resolvePath(r, rig.root, ""), rig.root);
    EXPECT_EQ(anim::resolvePath(r, rig.root, "Leg"), rig.leg);
    EXPECT_EQ(anim::resolvePath(r, rig.root, "Arm/Hand"), rig.hand);
    EXPECT_EQ(anim::resolvePath(r, rig.root, "Arm#1/Hand"), rig.hand1);
    EXPECT_EQ(anim::resolvePath(r, rig.root, "Arm#2"), NULL_ENTITY);
    EXPECT_EQ(anim::resolvePath(r, rig.arm, "Hand"), rig.hand);

    anim::PathResolution info;
    EXPECT_EQ(anim::resolvePath(r, rig.root, "Arm/Hand", &info), rig.hand);
    EXPECT_TRUE(info.duplicateSibling) << "Arm without #N is shared";
    EXPECT_EQ(info.resolvedSegments, 2u);
    EXPECT_EQ(anim::resolvePath(r, rig.root, "Arm#1/Hand", &info), rig.hand1);
    EXPECT_FALSE(info.duplicateSibling);
    EXPECT_EQ(anim::resolvePath(r, rig.root, "Leg/Foot", &info), NULL_ENTITY);
    EXPECT_EQ(info.resolvedSegments, 1u);
    EXPECT_EQ(info.lastMatched, rig.leg);
    EXPECT_EQ(anim::resolvePath(r, rig.root, "Arm//Hand", &info), NULL_ENTITY);
    EXPECT_EQ(info.lastMatched, rig.root);
    EXPECT_EQ(anim::resolvePath(r, NULL_ENTITY, "Arm"), NULL_ENTITY);

    std::vector<Entity> children = anim::getChildren(r, rig.root);
    ASSERT_EQ(children.size(), 3u);
    EXPECT_EQ(children[0], rig.arm);
    EXPECT_EQ(children[1], rig.arm1);
    EXPECT_EQ(children[2], rig.leg);
    // a sibling moved first changes what "Arm" means
    r->moveChildToIndex(rig.arm1, r->getComponentArray<Transform>()->getIndex(rig.arm));
    EXPECT_EQ(anim::resolvePath(r, rig.root, "Arm"), rig.arm1);
    EXPECT_EQ(anim::resolvePath(r, rig.root, "Arm#1/Hand"), rig.hand);
}

TEST(ClipBinding, RelativePathsAndDescendants){
    Rig rig;
    EntityRegistry* r = &rig.registry;
    std::string path;
    ASSERT_TRUE(anim::relativePath(r, rig.root, rig.root, path));
    EXPECT_EQ(path, "");
    ASSERT_TRUE(anim::relativePath(r, rig.root, rig.hand1, path));
    EXPECT_EQ(path, "Arm#1/Hand");
    ASSERT_TRUE(anim::relativePath(r, rig.arm1, rig.hand1, path));
    EXPECT_EQ(path, "Hand");
    EXPECT_FALSE(anim::relativePath(r, rig.arm, rig.hand1, path)) << "not under the root";
    EXPECT_FALSE(anim::relativePath(r, rig.hand, rig.root, path));

    EXPECT_TRUE(anim::hasDuplicateSiblingName(r, rig.arm));
    EXPECT_TRUE(anim::hasDuplicateSiblingName(r, rig.arm1));
    EXPECT_FALSE(anim::hasDuplicateSiblingName(r, rig.leg));
    EXPECT_FALSE(anim::hasDuplicateSiblingName(r, rig.hand));

    // every descendant, depth first in child order; resolving each path gives it back
    std::vector<std::pair<std::string, Entity>> all = anim::descendantPaths(r, rig.root);
    std::vector<std::pair<std::string, Entity>> expected = {
        {"", rig.root}, {"Arm", rig.arm}, {"Arm/Hand", rig.hand}, {"Arm#1", rig.arm1}, {"Arm#1/Hand", rig.hand1}, {"Leg", rig.leg}};
    EXPECT_EQ(all, expected);
    for (const auto& entry : all){
        EXPECT_EQ(anim::resolvePath(r, rig.root, entry.first), entry.second) << entry.first;
        ASSERT_TRUE(anim::relativePath(r, rig.root, entry.second, path));
        EXPECT_EQ(path, entry.first);
    }

    // a name that can't be addressed hides its branch
    r->setEntityName(rig.arm1, "Arm#7");
    EXPECT_FALSE(anim::relativePath(r, rig.root, rig.hand1, path));
    all = anim::descendantPaths(r, rig.root);
    expected = {{"", rig.root}, {"Arm", rig.arm}, {"Arm/Hand", rig.hand}, {"Leg", rig.leg}};
    EXPECT_EQ(all, expected);
}

TEST(ClipBinding, HierarchyRevisionFollowsEveryMutation){
    Rig rig;
    EntityRegistry& r = rig.registry;
    uint64_t revision = r.hierarchyRevision();
    auto bumped = [&](const char* what){
        uint64_t now = r.hierarchyRevision();
        EXPECT_GT(now, revision) << what;
        revision = now;
    };
    auto unchanged = [&](const char* what){
        EXPECT_EQ(r.hierarchyRevision(), revision) << what;
    };

    // a parentless spawn and its destruction leave it (one bullet a frame costs nothing)
    Entity bullet = r.createEntity();
    r.addComponent<Transform>(bullet, {});
    unchanged("parentless create");
    r.destroyEntity(bullet);
    unchanged("parentless destroy");

    r.setEntityName(rig.leg, "Leg");
    unchanged("same name");
    r.setEntityName(rig.leg, "Shin");
    bumped("setEntityName");

    r.addEntityChild(rig.arm, rig.leg, false);
    bumped("addEntityChild");
    r.addEntityChild(rig.arm, rig.leg, false);
    unchanged("addEntityChild to the same parent");
    r.addEntityChild(NULL_ENTITY, rig.leg, false);
    bumped("addEntityChild to the top");

    r.moveChildToIndex(rig.arm1, r.getComponentArray<Transform>()->getIndex(rig.arm));
    bumped("moveChildToIndex");
    r.moveChildUp(rig.arm1);
    bumped("moveChildUp");
    r.sortComponentsByTransform(r.getSignature(rig.arm));
    bumped("sortComponentsByTransform");

    // a Transform that arrives with its parent set (decode, clone)
    Entity decoded = r.createEntity();
    Transform child;
    child.parent = rig.root;
    r.addComponent<Transform>(decoded, child);
    bumped("addComponent<Transform> with a parent");
    Entity moved = r.createEntity();
    Transform movedChild;
    movedChild.parent = rig.root;
    r.addComponent<Transform>(moved, std::move(movedChild));
    bumped("addComponent<Transform>(T&&) with a parent");
    r.removeComponent<Transform>(moved);
    bumped("removeComponent<Transform> with a parent");

    // a parentless Transform with children
    Entity lone = r.createEntity();
    r.addComponent<Transform>(lone, {});
    unchanged("parentless Transform");
    r.removeComponent<Transform>(lone);
    unchanged("removing a lone Transform");
    r.removeComponent<Transform>(rig.arm1);
    bumped("removeComponent<Transform> of a parent (children reparented)");

    r.destroyEntity(rig.hand);
    bumped("destroyEntity with a parent");
    r.destroyEntity(rig.root);
    bumped("destroyEntity with children");

    r.destroyEntities({rig.arm, decoded});
    bumped("destroyEntities");

    Entity gone = r.createEntity();
    r.destroyEntity(gone);
    unchanged("destroying an entity without a Transform");
    r.recreateEntity(gone);
    bumped("recreateEntity");

    r.clear();
    bumped("clear");
}
