// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT
//
// `visible` is each GameObject's own flag; only an explicit change (GameObject::setVisible)
// cascades to the children. Copies (Instantiate), scene moves (DontDestroyOnLoad,
// moveGameObjectToScene) and the reparenting of a removed Transform's children keep every
// child's own value: runtime-core spec 6.4. The transform pass (propagateTransforms, what
// RenderSystem runs every frame) applies a pending cascade.

#include <gtest/gtest.h>

#include "Scene.h"
#include "component/Transform.h"
#include "manager/ObjectManager.h"
#include "object/GameObject.h"
#include "subsystem/TransformPropagation.h"

#include <vector>

using namespace jasperengine;

namespace {

    struct Family {
        Entity parent;
        Entity child;
        Entity grandchild;
    };

    // one frame's transform pass, then the flags RenderSystem clears after it
    void settle(Scene& scene) {
        propagateTransforms(scene, [](Transform&, Entity) {});
        auto transforms = scene.getComponentArray<Transform>();
        for (size_t i = 0; i < transforms->size(); i++) {
            transforms->getComponentFromIndex(i).needUpdateChildVisibility = false;
            transforms->getComponentFromIndex(i).needUpdate = false;
        }
    }

    Entity transformEntity(Scene& scene, const char* name) {
        const Entity entity = scene.createEntity();
        scene.addComponent<Transform>(entity, {});
        scene.setEntityName(entity, name);
        return entity;
    }

    // parent > child > grandchild, the visibility pass already run (the scene as loaded)
    Family family(Scene& scene, bool parentVisible, bool childVisible) {
        Family f;
        f.parent = transformEntity(scene, "Parent");
        f.child = transformEntity(scene, "Child");
        f.grandchild = transformEntity(scene, "Grandchild");
        scene.addEntityChild(f.parent, f.child, false);
        scene.addEntityChild(f.child, f.grandchild, false);
        scene.getComponent<Transform>(f.parent).visible = parentVisible;
        scene.getComponent<Transform>(f.child).visible = childVisible;
        settle(scene);
        return f;
    }

    bool visible(Scene& scene, Entity entity) {
        return scene.getComponent<Transform>(entity).visible;
    }

    Entity childOf(Scene& scene, Entity parent) {
        auto transforms = scene.getComponentArray<Transform>();
        for (size_t i = 0; i < transforms->size(); i++) {
            if (transforms->getComponentFromIndex(i).parent == parent) return transforms->getEntity(i);
        }
        return NULL_ENTITY;
    }

}

TEST(HiddenChildVisibilityTest, CloneKeepsAChildHiddenOnItsOwn) {
    Scene scene;
    const Family source = family(scene, true, false);

    const Entity copy = ObjectManager::instantiate(&scene, source.parent);
    ASSERT_NE(copy, NULL_ENTITY);
    settle(scene);
    const Entity copiedChild = childOf(scene, copy);
    ASSERT_NE(copiedChild, NULL_ENTITY);
    EXPECT_TRUE(visible(scene, copy));
    EXPECT_FALSE(visible(scene, copiedChild)) << "the copy's child hidden on its own stays hidden";
    EXPECT_TRUE(visible(scene, childOf(scene, copiedChild))) << "the grandchild keeps its own flag";
    EXPECT_FALSE(visible(scene, source.child)) << "the source is untouched";
}

TEST(HiddenChildVisibilityTest, CloneOfAHiddenParentKeepsItsVisibleChild) {
    Scene scene;
    // a parent saved hidden with visible children (the editor stores each flag on its own)
    const Family source = family(scene, false, true);

    const Entity copy = ObjectManager::instantiate(&scene, source.parent);
    ASSERT_NE(copy, NULL_ENTITY);
    settle(scene);
    EXPECT_FALSE(visible(scene, copy));
    EXPECT_TRUE(visible(scene, childOf(scene, copy))) << "the copy looks like its source";
}

TEST(HiddenChildVisibilityTest, MoveToSceneKeepsChildFlags) {
    Scene from;
    Scene to;
    const Family source = family(from, true, false);

    const std::vector<Entity> moved = ObjectManager::moveToScene(&from, source.parent, &to);
    ASSERT_EQ(moved.size(), 3u);
    settle(to);
    EXPECT_TRUE(visible(to, moved[0]));
    EXPECT_FALSE(visible(to, moved[1])) << "a child hidden on its own stays hidden after DontDestroyOnLoad";
    EXPECT_TRUE(visible(to, moved[2]));
}

TEST(HiddenChildVisibilityTest, RemovingAMiddleTransformKeepsGrandchildFlags) {
    Scene scene;
    const Family f = family(scene, true, true);
    const Entity greatGrandchild = transformEntity(scene, "GreatGrandchild");
    scene.addEntityChild(f.grandchild, greatGrandchild, false);
    scene.getComponent<Transform>(greatGrandchild).visible = false;
    settle(scene);

    // the child's Transform goes: the grandchild becomes the parent's child
    scene.removeComponent<Transform>(f.child);
    settle(scene);
    EXPECT_EQ(scene.getComponent<Transform>(f.grandchild).parent, f.parent);
    EXPECT_TRUE(visible(scene, f.grandchild));
    EXPECT_FALSE(visible(scene, greatGrandchild)) << "the reparenting is no visibility change";
}

TEST(HiddenChildVisibilityTest, SetVisibleStillCascades) {
    Scene scene;
    const Family f = family(scene, true, false);

    GameObject parent(&scene, f.parent);
    parent.setVisible(false);
    settle(scene);
    EXPECT_FALSE(visible(scene, f.child));
    EXPECT_FALSE(visible(scene, f.grandchild));

    parent.setVisible(true);
    settle(scene);
    EXPECT_TRUE(visible(scene, f.child)) << "showing a hidden parent shows its children (the documented cascade)";
    EXPECT_TRUE(visible(scene, f.grandchild));
}
